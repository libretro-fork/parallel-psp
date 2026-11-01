/* See VulkanHeap.h. */

#include <stdlib.h>
#include <string.h>

#include "Common/GPU/Vulkan/VulkanHeap.h"

#define VK_HEAP_MIN_SPANS  16
#define VK_HEAP_MIN_BLOCKS 16

/* alignment is a power of two; Vulkan says so. */
#define VK_HEAP_ALIGN_UP(v, a) (((v) + ((a) - 1)) & ~((a) - 1))

/* What each usage requires, prefers and would rather not have. */
static void vk_heap_usage_flags(enum vk_heap_usage usage, VkMemoryPropertyFlags *required,
      VkMemoryPropertyFlags *preferred, VkMemoryPropertyFlags *avoided)
{
   switch (usage)
   {
      case VK_HEAP_UPLOAD:
         *required  = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
         *preferred = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
         *avoided   = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
         break;
      case VK_HEAP_READBACK:
         *required  = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
         *preferred = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
         *avoided   = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
         break;
      default:
         *required  = 0;
         *preferred = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
         *avoided   = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
         break;
   }
}

/* The memory types an allocation may come from, best first: with the
 * preferred flags and without the avoided ones, then with both, then the
 * rest likewise - each group in the driver's own order, its preference. */
static unsigned vk_heap_types(const vk_heap_t *heap, uint32_t type_bits,
      enum vk_heap_usage usage, unsigned char *types)
{
   VkMemoryPropertyFlags required, preferred, avoided;
   unsigned count = 0;
   unsigned rank;
   unsigned i;

   vk_heap_usage_flags(usage, &required, &preferred, &avoided);
   for (rank = 0; rank < 4; rank++)
      for (i = 0; i < heap->props.memoryTypeCount && i < VK_MAX_MEMORY_TYPES; i++)
      {
         const VkMemoryPropertyFlags flags = heap->props.memoryTypes[i].propertyFlags;
         unsigned r;

         if (!(type_bits & (1u << i)) || (flags & required) != required)
            continue;
         r = ((flags & preferred) == preferred ? 0u : 2u) + ((flags & avoided) ? 1u : 0u);
         if (r == rank)
            types[count++] = (unsigned char)i;
      }
   return count;
}

/* A block of a memory type: the heap's block size, but no more than an
 * eighth of the driver heap that type is in, and no less than is needed. */
static VkDeviceSize vk_heap_block_size(const vk_heap_t *heap, unsigned type, VkDeviceSize need)
{
   const unsigned heap_index = heap->props.memoryTypes[type].heapIndex;
   VkDeviceSize size         = heap->block_size;

   if (heap_index < heap->props.memoryHeapCount)
   {
      const VkDeviceSize eighth = heap->props.memoryHeaps[heap_index].size / 8u;

      if (eighth && eighth < size)
         size = eighth;
   }
   if (size < need)
      size = need;
   return VK_HEAP_ALIGN_UP(size, (VkDeviceSize)(64u * 1024u));
}

/* Makes room for one more span at index at. */
static int vk_heap_span_insert(vk_heap_block_t *b, unsigned at, VkDeviceSize offset, VkDeviceSize size)
{
   if (b->span_count == b->span_capacity)
   {
      const unsigned  cap   = b->span_capacity ? b->span_capacity * 2u : VK_HEAP_MIN_SPANS;
      vk_heap_span_t *grown = (vk_heap_span_t*)realloc(b->spans, cap * sizeof(vk_heap_span_t));

      if (!grown)
         return 0;
      b->spans         = grown;
      b->span_capacity = cap;
   }
   if (at < b->span_count)
      memmove(&b->spans[at + 1], &b->spans[at], (b->span_count - at) * sizeof(vk_heap_span_t));
   b->spans[at].offset = offset;
   b->spans[at].size   = size;
   b->span_count++;
   return 1;
}

static void vk_heap_span_remove(vk_heap_block_t *b, unsigned at)
{
   b->span_count--;
   if (at < b->span_count)
      memmove(&b->spans[at], &b->spans[at + 1], (b->span_count - at) * sizeof(vk_heap_span_t));
}

static void vk_heap_block_release(vk_heap_t *heap, vk_heap_block_t *b)
{
   if (b->mapped)
      heap->fns.unmap_memory(heap->device, b->memory);
   heap->fns.free_memory(heap->device, b->memory, NULL);
   free(b->spans);
   heap->bytes_reserved -= b->size;
   /* A hole, not a gap closed up: live allocations carry their block's index. */
   memset(b, 0, sizeof(*b));
}

/* A block from the driver, mapped if host-visible. Returns its index, or
 * -1 with heap->last_error saying why. */
static int vk_heap_add_block(vk_heap_t *heap, unsigned type, int linear, VkDeviceSize size,
      int need_map, int dedicated, VkBuffer for_buffer, VkImage for_image)
{
   VkMemoryAllocateInfo mai;
   VkMemoryDedicatedAllocateInfo dai;
   vk_heap_block_t *b;
   VkDeviceMemory memory = VK_NULL_HANDLE;
   void *mapped          = NULL;
   VkResult result;
   unsigned slot;

   /* A slot a release left empty, if there is one. */
   for (slot = 0; slot < heap->block_count; slot++)
      if (!heap->blocks[slot].memory)
         break;
   if (slot == heap->block_capacity)
   {
      const unsigned   cap   = heap->block_capacity ? heap->block_capacity * 2u : VK_HEAP_MIN_BLOCKS;
      vk_heap_block_t *grown = (vk_heap_block_t*)realloc(heap->blocks, cap * sizeof(vk_heap_block_t));

      if (!grown)
      {
         heap->last_error = VK_ERROR_OUT_OF_HOST_MEMORY;
         return -1;
      }
      memset(grown + heap->block_capacity, 0, (cap - heap->block_capacity) * sizeof(vk_heap_block_t));
      heap->blocks         = grown;
      heap->block_capacity = cap;
   }

   memset(&mai, 0, sizeof(mai));
   mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   mai.allocationSize  = size;
   mai.memoryTypeIndex = type;
   /* Named only where the device has dedicated allocations. */
   if (dedicated && heap->fns.image_requirements2 && heap->fns.buffer_requirements2
         && (for_buffer != VK_NULL_HANDLE || for_image != VK_NULL_HANDLE))
   {
      memset(&dai, 0, sizeof(dai));
      dai.sType  = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
      dai.buffer = for_buffer;
      dai.image  = for_image;
      mai.pNext  = &dai;
   }
   result = heap->fns.allocate_memory(heap->device, &mai, NULL, &memory);
   if (result != VK_SUCCESS)
   {
      heap->last_error = result;
      return -1;
   }

   /* Mapped once, for the life of the block, when it is for the CPU. */
   if (need_map)
   {
      result = heap->fns.map_memory(heap->device, memory, 0, VK_WHOLE_SIZE, 0, &mapped);
      if (result != VK_SUCCESS)
      {
         heap->fns.free_memory(heap->device, memory, NULL);
         heap->last_error = result;
         return -1;
      }
   }

   b = &heap->blocks[slot];
   memset(b, 0, sizeof(*b));
   if (!vk_heap_span_insert(b, 0, 0, size))
   {
      if (mapped)
         heap->fns.unmap_memory(heap->device, memory);
      heap->fns.free_memory(heap->device, memory, NULL);
      heap->last_error = VK_ERROR_OUT_OF_HOST_MEMORY;
      return -1;
   }
   b->memory    = memory;
   b->mapped    = mapped;
   b->size      = size;
   b->max_free  = size;
   b->type      = type;
   b->linear    = linear ? 1u : 0u;
   b->dedicated = dedicated ? 1u : 0u;

   if (slot == heap->block_count)
      heap->block_count++;
   heap->bytes_reserved += size;
   return (int)slot;
}

/* The lowest span of the block that fits, at the alignment asked for. */
static int vk_heap_block_alloc(vk_heap_t *heap, vk_heap_block_t *b, unsigned index,
      VkDeviceSize size, VkDeviceSize align, vk_heap_alloc_t *out)
{
   VkDeviceSize largest = 0;
   unsigned i;

   if (size > b->max_free)
      return 0;

   for (i = 0; i < b->span_count; i++)
   {
      const VkDeviceSize start     = b->spans[i].offset;
      const VkDeviceSize span_size = b->spans[i].size;
      const VkDeviceSize aligned   = VK_HEAP_ALIGN_UP(start, align);
      const VkDeviceSize head      = aligned - start;
      VkDeviceSize tail;

      if (span_size > largest)
         largest = span_size;
      if (head > span_size || size > span_size - head)
         continue;
      tail = span_size - head - size;

      if (head && tail)
      {
         /* The head stays; the tail goes in after it. */
         if (!vk_heap_span_insert(b, i + 1, aligned + size, tail))
            return 0;
         b->spans[i].size = head;
      }
      else if (head)
         b->spans[i].size = head;
      else if (tail)
      {
         b->spans[i].offset = aligned + size;
         b->spans[i].size   = tail;
      }
      else
         vk_heap_span_remove(b, i);

      b->used          += size;
      heap->bytes_used += size;

      out->memory = b->memory;
      out->offset = aligned;
      out->size   = size;
      out->mapped = b->mapped ? (void*)((unsigned char*)b->mapped + aligned) : NULL;
      out->block  = index;
      out->type   = b->type;
      return 1;
   }

   /* Every span was looked at and none fits: the bound is exact now. */
   b->max_free = largest;
   return 0;
}

static int vk_heap_alloc_existing(vk_heap_t *heap, const VkMemoryRequirements *req,
      unsigned type, unsigned linear, int need_map, vk_heap_alloc_t *out)
{
   const VkDeviceSize align = req->alignment ? req->alignment : 1;
   unsigned i;

   for (i = 0; i < heap->block_count; i++)
   {
      vk_heap_block_t *b = &heap->blocks[i];

      if (!b->memory || b->dedicated || b->type != type || b->linear != linear)
         continue;
      if (need_map && !b->mapped)
         continue;
      if (vk_heap_block_alloc(heap, b, i, req->size, align, out))
         return 1;
   }
   return 0;
}

/* The only place that asks the driver for memory. */
static int vk_heap_alloc_new(vk_heap_t *heap, const VkMemoryRequirements *req,
      unsigned type, unsigned linear, int need_map, int dedicated,
      VkBuffer for_buffer, VkImage for_image, vk_heap_alloc_t *out)
{
   const VkDeviceSize align = req->alignment ? req->alignment : 1;
   const VkDeviceSize size  = dedicated ? req->size : vk_heap_block_size(heap, type, req->size);
   const int slot = vk_heap_add_block(heap, type, (int)linear, size, need_map,
         dedicated, for_buffer, for_image);

   if (slot < 0)
      return 0;
   return vk_heap_block_alloc(heap, &heap->blocks[slot], (unsigned)slot, req->size, align, out);
}

/* Empty shared blocks go back to the driver. Returns how many. */
static unsigned vk_heap_trim(vk_heap_t *heap)
{
   unsigned freed = 0;
   unsigned i;

   for (i = 0; i < heap->block_count; i++)
   {
      vk_heap_block_t *b = &heap->blocks[i];

      if (!b->memory || b->used != 0)
         continue;
      vk_heap_block_release(heap, b);
      freed++;
   }
   return freed;
}

int vk_heap_init(vk_heap_t *heap, VkDevice device,
      const VkPhysicalDeviceMemoryProperties *props, VkDeviceSize atom,
      const vk_heap_fns_t *fns, VkDeviceSize block_size)
{
   if (!heap || !props || !fns || !fns->allocate_memory || !fns->free_memory
         || !fns->map_memory || !fns->unmap_memory || !fns->invalidate_ranges
         || !fns->create_buffer || !fns->destroy_buffer || !fns->create_image
         || !fns->destroy_image || !fns->buffer_requirements || !fns->image_requirements
         || !fns->bind_buffer || !fns->bind_image)
      return 0;
   memset(heap, 0, sizeof(*heap));
   heap->device     = device;
   heap->props      = *props;
   heap->fns        = *fns;
   heap->atom       = atom ? atom : 1;
   heap->block_size = block_size ? block_size : (VkDeviceSize)(64u * 1024u * 1024u);
   return 1;
}

void vk_heap_shutdown(vk_heap_t *heap)
{
   unsigned i;

   for (i = 0; i < heap->block_count; i++)
      if (heap->blocks[i].memory)
         vk_heap_block_release(heap, &heap->blocks[i]);
   free(heap->blocks);
   heap->blocks         = NULL;
   heap->block_count    = 0;
   heap->block_capacity = 0;
   heap->bytes_reserved = 0;
   heap->bytes_used     = 0;
}

int vk_heap_alloc(vk_heap_t *heap, const VkMemoryRequirements *req,
      enum vk_heap_usage usage, int linear, int dedicated,
      VkBuffer for_buffer, VkImage for_image, vk_heap_alloc_t *out)
{
   const unsigned kind = linear ? 1u : 0u;
   /* Upload and readback memory is used through its address. */
   const int need_map  = usage != VK_HEAP_DEVICE;
   unsigned char types[VK_MAX_MEMORY_TYPES];
   unsigned count = vk_heap_types(heap, req->memoryTypeBits, usage, types);
   unsigned i;

   memset(out, 0, sizeof(*out));
   if (!count)
   {
      heap->last_error = VK_ERROR_FEATURE_NOT_PRESENT;
      return 0;
   }
   /* Half a block or more is better off alone than splitting a block. */
   if (req->size > heap->block_size / 2u && !dedicated)
      dedicated = VK_HEAP_DEDICATED_PREFERRED;

   if (dedicated)
   {
      for (i = 0; i < count; i++)
         if (vk_heap_alloc_new(heap, req, types[i], kind, need_map, 1, for_buffer, for_image, out))
            return 1;
      /* Only a preference: a shared block will do. */
      if (dedicated == VK_HEAP_DEDICATED_REQUIRED)
         return 0;
   }

   /* Every type that will do, best first: its blocks, then a new block of
    * it, before a worse type is touched. A driver can be out of one type
    * and have another. */
   for (i = 0; i < count; i++)
      if (vk_heap_alloc_existing(heap, req, types[i], kind, need_map, out)
            || vk_heap_alloc_new(heap, req, types[i], kind, need_map, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, out))
         return 1;

   /* No room and no new block: the empty ones of other kinds go back first. */
   if (vk_heap_trim(heap))
      for (i = 0; i < count; i++)
         if (vk_heap_alloc_new(heap, req, types[i], kind, need_map, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, out))
            return 1;
   return 0;
}

void vk_heap_free(vk_heap_t *heap, vk_heap_alloc_t *alloc)
{
   vk_heap_block_t *b;
   VkDeviceSize offset;
   VkDeviceSize size;
   unsigned lo;
   unsigned hi;
   unsigned i;
   int joins_prev;
   int joins_next;

   if (!alloc || alloc->memory == VK_NULL_HANDLE)
      return;
   if (alloc->block >= heap->block_count || heap->blocks[alloc->block].memory != alloc->memory)
   {
      memset(alloc, 0, sizeof(*alloc));
      return;
   }
   b      = &heap->blocks[alloc->block];
   offset = alloc->offset;
   size   = alloc->size;
   memset(alloc, 0, sizeof(*alloc));
   b->used          -= size;
   heap->bytes_used -= size;

   if (b->dedicated)
   {
      vk_heap_block_release(heap, b);
      return;
   }

   /* The first free span that starts after this one. */
   lo = 0;
   hi = b->span_count;
   while (lo < hi)
   {
      const unsigned mid = lo + (hi - lo) / 2u;

      if (b->spans[mid].offset < offset)
         lo = mid + 1;
      else
         hi = mid;
   }

   /* Joined to whichever neighbours it touches: sorted, never adjacent. */
   joins_prev = lo > 0 && b->spans[lo - 1].offset + b->spans[lo - 1].size == offset;
   joins_next = lo < b->span_count && offset + size == b->spans[lo].offset;

   if (joins_prev && joins_next)
   {
      b->spans[lo - 1].size += size + b->spans[lo].size;
      size = b->spans[lo - 1].size;
      vk_heap_span_remove(b, lo);
   }
   else if (joins_prev)
   {
      b->spans[lo - 1].size += size;
      size = b->spans[lo - 1].size;
   }
   else if (joins_next)
   {
      b->spans[lo].offset = offset;
      b->spans[lo].size  += size;
      size = b->spans[lo].size;
   }
   else if (!vk_heap_span_insert(b, lo, offset, size))
   {
      /* No host memory for the span: it is lost until the block goes,
       * which beats writing it where there is no room. */
      return;
   }

   if (size > b->max_free)
      b->max_free = size;

   /* One empty block of each kind stays for the next allocation; a second
    * goes back to the driver. */
   if (b->used == 0)
      for (i = 0; i < heap->block_count; i++)
      {
         const vk_heap_block_t *o = &heap->blocks[i];

         if (o != b && o->memory && !o->dedicated && o->used == 0
               && o->type == b->type && o->linear == b->linear)
         {
            vk_heap_block_release(heap, b);
            break;
         }
      }
}

/* Requirements, and whether the driver wants the object alone. */
static void vk_heap_requirements(vk_heap_t *heap, VkBuffer buffer, VkImage image,
      VkMemoryRequirements *req, int *dedicated)
{
   *dedicated = 0;
   if ((buffer != VK_NULL_HANDLE && heap->fns.buffer_requirements2)
         || (image != VK_NULL_HANDLE && heap->fns.image_requirements2))
   {
      VkMemoryDedicatedRequirements dr;
      VkMemoryRequirements2 r2;

      memset(&dr, 0, sizeof(dr));
      dr.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS;
      memset(&r2, 0, sizeof(r2));
      r2.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
      r2.pNext = &dr;
      if (buffer != VK_NULL_HANDLE)
      {
         VkBufferMemoryRequirementsInfo2 info;

         memset(&info, 0, sizeof(info));
         info.sType  = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2;
         info.buffer = buffer;
         heap->fns.buffer_requirements2(heap->device, &info, &r2);
      }
      else
      {
         VkImageMemoryRequirementsInfo2 info;

         memset(&info, 0, sizeof(info));
         info.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
         info.image = image;
         heap->fns.image_requirements2(heap->device, &info, &r2);
      }
      *req       = r2.memoryRequirements;
      *dedicated = dr.requiresDedicatedAllocation ? VK_HEAP_DEDICATED_REQUIRED
         : dr.prefersDedicatedAllocation ? VK_HEAP_DEDICATED_PREFERRED : 0;
      return;
   }
   if (buffer != VK_NULL_HANDLE)
      heap->fns.buffer_requirements(heap->device, buffer, req);
   else
      heap->fns.image_requirements(heap->device, image, req);
}

VkResult vk_heap_create_buffer(vk_heap_t *heap, const VkBufferCreateInfo *info,
      enum vk_heap_usage usage, VkBuffer *buffer, vk_heap_alloc_t *out)
{
   VkMemoryRequirements req;
   VkResult result;
   int dedicated;

   memset(out, 0, sizeof(*out));
   *buffer = VK_NULL_HANDLE;
   result  = heap->fns.create_buffer(heap->device, info, NULL, buffer);
   if (result != VK_SUCCESS)
   {
      *buffer = VK_NULL_HANDLE;
      return result;
   }
   vk_heap_requirements(heap, *buffer, VK_NULL_HANDLE, &req, &dedicated);
   if (!vk_heap_alloc(heap, &req, usage, 1, dedicated, *buffer, VK_NULL_HANDLE, out))
      result = heap->last_error;
   else
      result = heap->fns.bind_buffer(heap->device, *buffer, out->memory, out->offset);
   if (result != VK_SUCCESS)
   {
      vk_heap_destroy_buffer(heap, *buffer, out);
      *buffer = VK_NULL_HANDLE;
   }
   return result;
}

VkResult vk_heap_create_image(vk_heap_t *heap, const VkImageCreateInfo *info,
      enum vk_heap_usage usage, VkImage *image, vk_heap_alloc_t *out)
{
   VkMemoryRequirements req;
   VkResult result;
   int dedicated;

   memset(out, 0, sizeof(*out));
   *image = VK_NULL_HANDLE;
   result = heap->fns.create_image(heap->device, info, NULL, image);
   if (result != VK_SUCCESS)
   {
      *image = VK_NULL_HANDLE;
      return result;
   }
   vk_heap_requirements(heap, VK_NULL_HANDLE, *image, &req, &dedicated);
   if (!vk_heap_alloc(heap, &req, usage, info->tiling == VK_IMAGE_TILING_LINEAR,
            dedicated, VK_NULL_HANDLE, *image, out))
      result = heap->last_error;
   else
      result = heap->fns.bind_image(heap->device, *image, out->memory, out->offset);
   if (result != VK_SUCCESS)
   {
      vk_heap_destroy_image(heap, *image, out);
      *image = VK_NULL_HANDLE;
   }
   return result;
}

void vk_heap_destroy_buffer(vk_heap_t *heap, VkBuffer buffer, vk_heap_alloc_t *alloc)
{
   if (buffer != VK_NULL_HANDLE)
      heap->fns.destroy_buffer(heap->device, buffer, NULL);
   vk_heap_free(heap, alloc);
}

void vk_heap_destroy_image(vk_heap_t *heap, VkImage image, vk_heap_alloc_t *alloc)
{
   if (image != VK_NULL_HANDLE)
      heap->fns.destroy_image(heap->device, image, NULL);
   vk_heap_free(heap, alloc);
}

int vk_heap_is_coherent(const vk_heap_t *heap, const vk_heap_alloc_t *alloc)
{
   return alloc->type < heap->props.memoryTypeCount
      && (heap->props.memoryTypes[alloc->type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
}

VkResult vk_heap_invalidate(vk_heap_t *heap, const vk_heap_alloc_t *alloc,
      VkDeviceSize offset, VkDeviceSize size)
{
   VkMappedMemoryRange range;
   const vk_heap_block_t *b;
   VkDeviceSize start;
   VkDeviceSize end;

   if (alloc->memory == VK_NULL_HANDLE || vk_heap_is_coherent(heap, alloc))
      return VK_SUCCESS;
   if (alloc->block >= heap->block_count)
      return VK_ERROR_INITIALIZATION_FAILED;
   b = &heap->blocks[alloc->block];
   if (offset > alloc->size)
      offset = alloc->size;
   if (size > alloc->size - offset)
      size = alloc->size - offset;
   /* Whole atoms, within the block. */
   start = alloc->offset + offset;
   end   = start + size;
   start = start & ~(heap->atom - 1);
   end   = VK_HEAP_ALIGN_UP(end, heap->atom);
   memset(&range, 0, sizeof(range));
   range.sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
   range.memory = alloc->memory;
   range.offset = start;
   range.size   = end >= b->size ? VK_WHOLE_SIZE : end - start;
   return heap->fns.invalidate_ranges(heap->device, 1, &range);
}
