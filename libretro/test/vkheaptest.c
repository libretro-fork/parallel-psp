/* Common/GPU/Vulkan/VulkanHeap.c against a stub driver that counts calls
 * and hands out fake handles: the arithmetic that decides where memory goes
 * and whether a frame asks the driver for any. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Common/GPU/Vulkan/VulkanHeap.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s (line %d)\n", m, __LINE__); failures++; } } while (0)

/* The stub device. Memory types:
 *   0 device-local
 *   1 device-local | host-visible | host-coherent (a small BAR window)
 *   2 host-visible | host-coherent
 *   3 host-visible | host-cached (not coherent) */
static VkPhysicalDeviceMemoryProperties props;

#define MAX_MEM 4096
static struct { int live; unsigned type; VkDeviceSize size; unsigned char *ptr; int dedicated; } mem[MAX_MEM];
static int driver_allocs, driver_frees, maps, unmaps;
static VkDeviceSize refuse_type1_after = (VkDeviceSize)-1;
static VkDeviceSize type1_taken;
static int refuse_dedicated;
static int fail_map_type = -1;
static VkMappedMemoryRange last_invalidate;
static int invalidates;

/* Objects: requirements per handle. */
#define MAX_OBJ 65536
static struct { VkMemoryRequirements req; int prefers, requires; int bound; VkDeviceMemory mem; VkDeviceSize off; } obj[MAX_OBJ];
static unsigned obj_count;
static VkMemoryRequirements next_req;
static int next_prefers, next_requires;

static unsigned mem_index(VkDeviceMemory m) { return (unsigned)((size_t)m - 1); }

static VkResult VKAPI_CALL stub_allocate(VkDevice d, const VkMemoryAllocateInfo *ai,
      const VkAllocationCallbacks *cb, VkDeviceMemory *out)
{
   unsigned i;
   int dedicated = 0;
   const VkBaseInStructure *n = (const VkBaseInStructure *)ai->pNext;
   (void)d; (void)cb;
   for (; n; n = n->pNext)
      if (n->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO)
         dedicated = 1;
   if (dedicated && refuse_dedicated)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   if (ai->memoryTypeIndex == 1)
   {
      if (type1_taken + ai->allocationSize > refuse_type1_after)
         return VK_ERROR_OUT_OF_DEVICE_MEMORY;
      type1_taken += ai->allocationSize;
   }
   for (i = 0; i < MAX_MEM; i++)
      if (!mem[i].live)
         break;
   if (i == MAX_MEM || ai->allocationSize == 0)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   mem[i].live = 1;
   mem[i].type = ai->memoryTypeIndex;
   mem[i].size = ai->allocationSize;
   mem[i].ptr = NULL;
   mem[i].dedicated = dedicated;
   driver_allocs++;
   *out = (VkDeviceMemory)(size_t)(i + 1);
   return VK_SUCCESS;
}

static void VKAPI_CALL stub_free(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *cb)
{
   unsigned i = mem_index(m);
   (void)d; (void)cb;
   if (m == VK_NULL_HANDLE)
      return;
   CHECK(i < MAX_MEM && mem[i].live, "free of memory that is not live");
   CHECK(!mem[i].ptr, "free of memory still mapped");
   if (mem[i].type == 1)
      type1_taken -= mem[i].size;
   mem[i].live = 0;
   driver_frees++;
}

static VkResult VKAPI_CALL stub_map(VkDevice d, VkDeviceMemory m, VkDeviceSize off,
      VkDeviceSize size, VkMemoryMapFlags f, void **out)
{
   unsigned i = mem_index(m);
   (void)d; (void)f;
   CHECK(off == 0 && size == VK_WHOLE_SIZE, "map of part of a block");
   CHECK(props.memoryTypes[mem[i].type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, "map of device memory");
   if ((int)mem[i].type == fail_map_type)
      return VK_ERROR_MEMORY_MAP_FAILED;
   mem[i].ptr = (unsigned char *)malloc((size_t)mem[i].size);
   maps++;
   *out = mem[i].ptr;
   return mem[i].ptr ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}

static void VKAPI_CALL stub_unmap(VkDevice d, VkDeviceMemory m)
{
   unsigned i = mem_index(m);
   (void)d;
   free(mem[i].ptr);
   mem[i].ptr = NULL;
   unmaps++;
}

static VkResult VKAPI_CALL stub_invalidate(VkDevice d, uint32_t n, const VkMappedMemoryRange *r)
{
   (void)d;
   CHECK(n == 1, "one range");
   last_invalidate = r[0];
   invalidates++;
   return VK_SUCCESS;
}

static VkResult new_obj(uint64_t *out)
{
   if (obj_count + 1 >= MAX_OBJ)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   obj_count++;
   obj[obj_count].req = next_req;
   obj[obj_count].prefers = next_prefers;
   obj[obj_count].requires = next_requires;
   obj[obj_count].bound = 0;
   *out = obj_count;
   return VK_SUCCESS;
}

static VkResult VKAPI_CALL stub_create_buffer(VkDevice d, const VkBufferCreateInfo *ci,
      const VkAllocationCallbacks *cb, VkBuffer *b)
{
   uint64_t h;
   VkResult r;
   (void)d; (void)cb; (void)ci;
   r = new_obj(&h);
   *b = (VkBuffer)(size_t)h;
   return r;
}
static void VKAPI_CALL stub_destroy_buffer(VkDevice d, VkBuffer b, const VkAllocationCallbacks *cb) { (void)d; (void)b; (void)cb; }
static VkResult VKAPI_CALL stub_create_image(VkDevice d, const VkImageCreateInfo *ci,
      const VkAllocationCallbacks *cb, VkImage *im)
{
   uint64_t h;
   VkResult r;
   (void)d; (void)cb; (void)ci;
   r = new_obj(&h);
   *im = (VkImage)(size_t)h;
   return r;
}
static void VKAPI_CALL stub_destroy_image(VkDevice d, VkImage im, const VkAllocationCallbacks *cb) { (void)d; (void)im; (void)cb; }
static void VKAPI_CALL stub_buffer_req(VkDevice d, VkBuffer b, VkMemoryRequirements *r) { (void)d; *r = obj[(size_t)b].req; }
static void VKAPI_CALL stub_image_req(VkDevice d, VkImage im, VkMemoryRequirements *r) { (void)d; *r = obj[(size_t)im].req; }
static void fill_req2(size_t h, VkMemoryRequirements2 *r)
{
   VkBaseOutStructure *n = (VkBaseOutStructure *)r->pNext;
   r->memoryRequirements = obj[h].req;
   for (; n; n = n->pNext)
      if (n->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS)
      {
         ((VkMemoryDedicatedRequirements *)n)->prefersDedicatedAllocation = obj[h].prefers ? VK_TRUE : VK_FALSE;
         ((VkMemoryDedicatedRequirements *)n)->requiresDedicatedAllocation = obj[h].requires ? VK_TRUE : VK_FALSE;
      }
}
static void VKAPI_CALL stub_buffer_req2(VkDevice d, const VkBufferMemoryRequirementsInfo2 *i, VkMemoryRequirements2 *r) { (void)d; fill_req2((size_t)i->buffer, r); }
static void VKAPI_CALL stub_image_req2(VkDevice d, const VkImageMemoryRequirementsInfo2 *i, VkMemoryRequirements2 *r) { (void)d; fill_req2((size_t)i->image, r); }
static VkResult VKAPI_CALL stub_bind_buffer(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize off)
{
   size_t h = (size_t)b;
   unsigned i = mem_index(m);
   (void)d;
   CHECK(!obj[h].bound, "bound twice");
   CHECK(off % obj[h].req.alignment == 0, "buffer misaligned");
   CHECK(off + obj[h].req.size <= mem[i].size, "buffer past its memory");
   CHECK(obj[h].req.memoryTypeBits & (1u << mem[i].type), "buffer in a type it cannot use");
   obj[h].bound = 1;
   return VK_SUCCESS;
}
static VkResult VKAPI_CALL stub_bind_image(VkDevice d, VkImage im, VkDeviceMemory m, VkDeviceSize off)
{
   size_t h = (size_t)im;
   unsigned i = mem_index(m);
   (void)d;
   CHECK(!obj[h].bound, "bound twice");
   CHECK(off % obj[h].req.alignment == 0, "image misaligned");
   CHECK(off + obj[h].req.size <= mem[i].size, "image past its memory");
   CHECK(obj[h].req.memoryTypeBits & (1u << mem[i].type), "image in a type it cannot use");
   obj[h].bound = 1;
   return VK_SUCCESS;
}

static vk_heap_fns_t fns;

static void setup(vk_heap_t *heap, int with_dedicated, VkDeviceSize block)
{
   memset(&props, 0, sizeof(props));
   props.memoryHeapCount = 3;
   props.memoryHeaps[0].size = (VkDeviceSize)8 << 30;
   props.memoryHeaps[1].size = (VkDeviceSize)256 << 20;
   props.memoryHeaps[2].size = (VkDeviceSize)16 << 30;
   props.memoryTypeCount = 4;
   props.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
   props.memoryTypes[0].heapIndex = 0;
   props.memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   props.memoryTypes[1].heapIndex = 1;
   props.memoryTypes[2].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   props.memoryTypes[2].heapIndex = 2;
   props.memoryTypes[3].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
   props.memoryTypes[3].heapIndex = 2;
   memset(&fns, 0, sizeof(fns));
   fns.allocate_memory = stub_allocate;
   fns.free_memory = stub_free;
   fns.map_memory = stub_map;
   fns.unmap_memory = stub_unmap;
   fns.invalidate_ranges = stub_invalidate;
   fns.create_buffer = stub_create_buffer;
   fns.destroy_buffer = stub_destroy_buffer;
   fns.create_image = stub_create_image;
   fns.destroy_image = stub_destroy_image;
   fns.buffer_requirements = stub_buffer_req;
   fns.image_requirements = stub_image_req;
   if (with_dedicated)
   {
      fns.buffer_requirements2 = stub_buffer_req2;
      fns.image_requirements2 = stub_image_req2;
   }
   fns.bind_buffer = stub_bind_buffer;
   fns.bind_image = stub_bind_image;
   refuse_type1_after = (VkDeviceSize)-1;
   refuse_dedicated = 0;
   fail_map_type = -1;
   next_prefers = next_requires = 0;
   CHECK(vk_heap_init(heap, (VkDevice)(size_t)1, &props, 64, &fns, block), "init");
}

static void teardown(vk_heap_t *heap)
{
   unsigned i;
   vk_heap_shutdown(heap);
   for (i = 0; i < MAX_MEM; i++)
      CHECK(!mem[i].live, "memory left after shutdown");
}

static void req(VkDeviceSize size, VkDeviceSize align, uint32_t bits)
{
   next_req.size = size;
   next_req.alignment = align;
   next_req.memoryTypeBits = bits;
}

/* Random images and buffers made and destroyed against a model: nothing
 * overlaps, everything is aligned and in a type it may use, the accounting
 * adds up, mapped pointers are the block's plus the offset, and buffers and
 * optimal images never share memory. */
#define LIVE 600
static struct { int used; int image; vk_heap_alloc_t a; VkDeviceSize req_size; } live[LIVE];

static unsigned rnd_state = 12345;
static unsigned rnd(void) { rnd_state = rnd_state * 1103515245u + 12345u; return (rnd_state >> 8) & 0xFFFFFF; }

static void check_model(vk_heap_t *heap)
{
   int i, j;
   VkDeviceSize used = 0;
   for (i = 0; i < LIVE; i++)
   {
      if (!live[i].used)
         continue;
      used += live[i].a.size;
      CHECK(live[i].a.size >= live[i].req_size, "allocation smaller than asked");
      for (j = i + 1; j < LIVE; j++)
      {
         if (!live[j].used || live[j].a.memory != live[i].a.memory)
            continue;
         CHECK(live[i].image == live[j].image, "a buffer and an optimal image share memory");
         CHECK(live[i].a.offset + live[i].a.size <= live[j].a.offset
               || live[j].a.offset + live[j].a.size <= live[i].a.offset, "allocations overlap");
      }
   }
   CHECK(used == heap->bytes_used, "bytes_used is the sum of what is live");
}

static void test_random(void)
{
   vk_heap_t heap;
   int round;
   int i;
   unsigned b;
   int steady_allocs;
   VkBuffer buf;
   VkImage img;

   setup(&heap, 1, (VkDeviceSize)8 << 20);
   memset(live, 0, sizeof(live));
   for (round = 0; round < 20000; round++)
   {
      i = (int)(rnd() % LIVE);
      if (live[i].used)
      {
         if (live[i].image)
            vk_heap_destroy_image(&heap, (VkImage)(size_t)1, &live[i].a);
         else
            vk_heap_destroy_buffer(&heap, (VkBuffer)(size_t)1, &live[i].a);
         CHECK(live[i].a.memory == VK_NULL_HANDLE, "free zeroes the allocation");
         live[i].used = 0;
      }
      else
      {
         VkImageCreateInfo ici;
         VkBufferCreateInfo bci;
         VkResult r;
         const VkDeviceSize size = 1 + rnd() % ((rnd() & 7) ? 65536u : (3u << 20));
         const VkDeviceSize align = (VkDeviceSize)1 << (rnd() % 13);
         memset(&ici, 0, sizeof(ici));
         memset(&bci, 0, sizeof(bci));
         ici.tiling = VK_IMAGE_TILING_OPTIMAL;
         live[i].image = (int)(rnd() & 1);
         req(size, align, live[i].image ? 0x1u : 0xFu);
         next_prefers = (rnd() % 50) == 0;
         if (live[i].image)
            r = vk_heap_create_image(&heap, &ici, VK_HEAP_DEVICE, &img, &live[i].a);
         else
            r = vk_heap_create_buffer(&heap, &bci, (rnd() & 1) ? VK_HEAP_UPLOAD : VK_HEAP_DEVICE, &buf, &live[i].a);
         next_prefers = 0;
         CHECK(r == VK_SUCCESS, "create");
         live[i].used = r == VK_SUCCESS;
         live[i].req_size = size;
         if (live[i].used && live[i].a.mapped)
         {
            unsigned m = mem_index(live[i].a.memory);
            CHECK((unsigned char *)live[i].a.mapped == mem[m].ptr + live[i].a.offset, "mapped is block + offset");
            /* The whole allocation is writable; the stub maps each block
             * with an allocation of exactly its size, so ASan sees overruns. */
            memset(live[i].a.mapped, (int)(i & 0xFF), (size_t)size);
         }
      }
      if ((round & 255) == 0)
         check_model(&heap);
   }
   check_model(&heap);

   /* All freed: at most one spare block of each kind, each one whole span. */
   for (i = 0; i < LIVE; i++)
      if (live[i].used)
      {
         vk_heap_free(&heap, &live[i].a);
         live[i].used = 0;
      }
   CHECK(heap.bytes_used == 0, "nothing used once all is freed");
   for (b = 0; b < heap.block_count; b++)
   {
      unsigned o;
      const vk_heap_block_t *blk = &heap.blocks[b];
      if (!blk->memory)
         continue;
      CHECK(!blk->dedicated, "dedicated memory released with its allocation");
      CHECK(blk->span_count == 1 && blk->spans[0].offset == 0 && blk->spans[0].size == blk->size, "an empty block is one span");
      for (o = b + 1; o < heap.block_count; o++)
         CHECK(!heap.blocks[o].memory || heap.blocks[o].type != blk->type || heap.blocks[o].linear != blk->linear,
               "two spare blocks of one kind");
   }

   /* The same work again, with only what fits in a block: no driver calls. */
   for (i = 0; i < 64; i++)
   {
      VkBufferCreateInfo bci;
      memset(&bci, 0, sizeof(bci));
      req(4096, 256, 0xF);
      vk_heap_create_buffer(&heap, &bci, VK_HEAP_UPLOAD, &buf, &live[i].a);
      live[i].used = 1;
   }
   for (i = 0; i < 64; i++)
      vk_heap_free(&heap, &live[i].a);
   steady_allocs = driver_allocs;
   for (round = 0; round < 10; round++)
   {
      for (i = 0; i < 64; i++)
      {
         VkBufferCreateInfo bci;
         memset(&bci, 0, sizeof(bci));
         req(4096, 256, 0xF);
         vk_heap_create_buffer(&heap, &bci, VK_HEAP_UPLOAD, &buf, &live[i].a);
      }
      for (i = 0; i < 64; i++)
         vk_heap_free(&heap, &live[i].a);
   }
   CHECK(driver_allocs == steady_allocs, "steady state asks the driver for nothing");
   teardown(&heap);
}

static void test_types(void)
{
   vk_heap_t heap;
   vk_heap_alloc_t a, c[8];
   VkMemoryRequirements r;
   int i;

   setup(&heap, 0, (VkDeviceSize)16 << 20);
   r.size = 4096; r.alignment = 256; r.memoryTypeBits = 0xF;
   CHECK(vk_heap_alloc(&heap, &r, VK_HEAP_DEVICE, 0, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, &a), "device");
   CHECK(a.type == 0 && !a.mapped, "device memory is device-local only, unmapped");
   vk_heap_free(&heap, &a);
   CHECK(vk_heap_alloc(&heap, &r, VK_HEAP_UPLOAD, 1, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, &a), "upload");
   CHECK(a.type == 1 && a.mapped, "upload prefers the device-local window, mapped");
   CHECK(vk_heap_is_coherent(&heap, &a), "upload is coherent");
   vk_heap_free(&heap, &a);
   CHECK(vk_heap_alloc(&heap, &r, VK_HEAP_READBACK, 1, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, &a), "readback");
   CHECK(a.type == 3 && a.mapped && !vk_heap_is_coherent(&heap, &a), "readback prefers cached memory");

   /* Invalidate whole atoms, inside the block. */
   CHECK(vk_heap_invalidate(&heap, &a, 10, 100) == VK_SUCCESS && invalidates == 1, "invalidate");
   CHECK(last_invalidate.offset % 64 == 0 && last_invalidate.offset <= a.offset + 10, "invalidate starts on an atom at or before");
   CHECK(last_invalidate.size == VK_WHOLE_SIZE || (last_invalidate.size % 64 == 0
         && last_invalidate.offset + last_invalidate.size >= a.offset + 110), "invalidate ends on an atom at or after");
   vk_heap_free(&heap, &a);

   /* The window runs out: upload goes on in plain host memory. */
   refuse_type1_after = 0;
   r.size = 1 << 20;
   for (i = 0; i < 8; i++)
   {
      CHECK(vk_heap_alloc(&heap, &r, VK_HEAP_UPLOAD, 1, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, &c[i]), "upload, window full");
      CHECK(c[i].type == 2 || c[i].type == 1, "a coherent host type");
   }
   for (i = 0; i < 8; i++)
      vk_heap_free(&heap, &c[i]);

   /* A type that will not map is passed over for CPU memory. */
   heap.block_size = (VkDeviceSize)16 << 20;
   teardown(&heap);
   setup(&heap, 0, (VkDeviceSize)16 << 20);
   fail_map_type = 1;
   r.size = 4096;
   CHECK(vk_heap_alloc(&heap, &r, VK_HEAP_UPLOAD, 1, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, &a), "upload, window unmappable");
   CHECK(a.type == 2 && a.mapped, "goes to memory that maps");
   vk_heap_free(&heap, &a);

   /* Nothing will do. */
   r.memoryTypeBits = 0x1;
   CHECK(!vk_heap_alloc(&heap, &r, VK_HEAP_UPLOAD, 1, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, &a), "no host type allowed");
   CHECK(heap.last_error != VK_SUCCESS, "says why");
   teardown(&heap);
}

static void test_dedicated(void)
{
   vk_heap_t heap;
   vk_heap_alloc_t a;
   VkImageCreateInfo ici;
   VkImage im;
   VkResult r;
   int before;

   memset(&ici, 0, sizeof(ici));
   ici.tiling = VK_IMAGE_TILING_OPTIMAL;
   setup(&heap, 1, (VkDeviceSize)16 << 20);

   /* Preferred: a memory of its own, named, released with it. */
   req(1 << 20, 4096, 0x1);
   next_prefers = 1;
   before = driver_frees;
   r = vk_heap_create_image(&heap, &ici, VK_HEAP_DEVICE, &im, &a);
   CHECK(r == VK_SUCCESS && mem[mem_index(a.memory)].dedicated && a.offset == 0, "preferred dedicated");
   vk_heap_destroy_image(&heap, im, &a);
   CHECK(driver_frees == before + 1, "dedicated memory goes with its image");

   /* Preferred but refused: a shared block. */
   refuse_dedicated = 1;
   r = vk_heap_create_image(&heap, &ici, VK_HEAP_DEVICE, &im, &a);
   CHECK(r == VK_SUCCESS && !mem[mem_index(a.memory)].dedicated, "preference falls back to a shared block");
   vk_heap_destroy_image(&heap, im, &a);

   /* Required and refused: fails, nothing left behind. */
   next_prefers = 0;
   next_requires = 1;
   r = vk_heap_create_image(&heap, &ici, VK_HEAP_DEVICE, &im, &a);
   CHECK(r != VK_SUCCESS && im == VK_NULL_HANDLE && a.memory == VK_NULL_HANDLE, "requirement does not fall back");
   refuse_dedicated = 0;
   next_requires = 0;

   /* Over half a block: alone, without naming the image where the device cannot. */
   teardown(&heap);
   setup(&heap, 0, (VkDeviceSize)16 << 20);
   req((VkDeviceSize)12 << 20, 4096, 0x1);
   r = vk_heap_create_image(&heap, &ici, VK_HEAP_DEVICE, &im, &a);
   CHECK(r == VK_SUCCESS && !mem[mem_index(a.memory)].dedicated && a.offset == 0
         && mem[mem_index(a.memory)].size == ((VkDeviceSize)12 << 20), "large: a memory of its own, not named");
   vk_heap_destroy_image(&heap, im, &a);
   teardown(&heap);
}

int main(void)
{
   test_types();
   test_dedicated();
   test_random();
   if (maps != unmaps)
   {
      printf("FAIL: %d maps, %d unmaps\n", maps, unmaps);
      failures++;
   }
   if (failures)
   {
      printf("vkheaptest: %d failures\n", failures);
      return 1;
   }
   printf("vkheaptest: %d driver allocations, all checks passed\n", driver_allocs);
   return 0;
}
