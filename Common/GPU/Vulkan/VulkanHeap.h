/*
 * Vulkan device memory: large blocks taken from the driver, handed out as
 * offsets inside them. C89, for MSVC as much as for GCC and Clang.
 *
 * It replaces the Vulkan Memory Allocator. What the renderer asks of memory
 * is small - an image or a buffer in device-local, upload or readback
 * memory, the pointer when it is host-visible, and to give it back - so
 * that is all there is:
 *
 *  - Blocks come from the driver and stay; an image or buffer made while a
 *    frame is recorded is an offset in a VkDeviceMemory that exists already.
 *  - An allocation is the lowest free span of a block that fits, at the
 *    alignment asked for. A block keeps its free spans sorted by offset, so
 *    a free is a binary search and a merge with its neighbours, with nothing
 *    allocated per allocation.
 *  - A host-visible block is mapped once, when it is taken, and stays mapped;
 *    an allocation in it carries its pointer.
 *  - Buffers and linear images never share a block with optimal images,
 *    which honours bufferImageGranularity without padding anything.
 *  - What the driver requires or prefers dedicated, and anything larger
 *    than half a block, gets a VkDeviceMemory of its own.
 *  - One empty block of each kind is kept for the next allocation; further
 *    empty ones go back to the driver.
 *
 * The Vulkan entry points are passed in, not linked against, so a test can
 * run the heap against a stub driver.
 *
 * One owning thread: the heap takes no locks. The renderer allocates and
 * frees on the thread that records frames.
 */

#ifndef PPSSPP_VULKAN_HEAP_H
#define PPSSPP_VULKAN_HEAP_H

#include <stddef.h>

/* Only the types. Where the Vulkan headers are in already, as they were set up. */
#ifndef VULKAN_CORE_H_
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include "ext/vulkan/vulkan.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vk_heap_fns
{
   PFN_vkAllocateMemory               allocate_memory;
   PFN_vkFreeMemory                   free_memory;
   PFN_vkMapMemory                    map_memory;
   PFN_vkUnmapMemory                  unmap_memory;
   PFN_vkInvalidateMappedMemoryRanges invalidate_ranges;
   PFN_vkCreateBuffer                 create_buffer;
   PFN_vkDestroyBuffer                destroy_buffer;
   PFN_vkCreateImage                  create_image;
   PFN_vkDestroyImage                 destroy_image;
   PFN_vkGetBufferMemoryRequirements  buffer_requirements;
   PFN_vkGetImageMemoryRequirements   image_requirements;
   /* Optional: NULL when the device has no dedicated allocations. */
   PFN_vkGetBufferMemoryRequirements2 buffer_requirements2;
   PFN_vkGetImageMemoryRequirements2  image_requirements2;
   PFN_vkBindBufferMemory             bind_buffer;
   PFN_vkBindImageMemory              bind_image;
} vk_heap_fns_t;

/* Where something ended up. mapped already has offset added; NULL when the
 * memory is not host-visible. A zeroed one is no allocation. */
typedef struct vk_heap_alloc
{
   VkDeviceMemory memory;
   VkDeviceSize   offset;
   VkDeviceSize   size;
   void          *mapped;
   unsigned       block;
   unsigned       type;   /* memory type index */
} vk_heap_alloc_t;

/* What memory is for. */
enum vk_heap_usage
{
   VK_HEAP_DEVICE = 0,   /* device-local, never touched by the CPU */
   VK_HEAP_UPLOAD,       /* host-visible and coherent, written by the CPU */
   VK_HEAP_READBACK      /* host-visible, read by the CPU; cached where there is such */
};

typedef struct vk_heap_span
{
   VkDeviceSize offset;
   VkDeviceSize size;
} vk_heap_span_t;

typedef struct vk_heap_block
{
   VkDeviceMemory  memory;     /* VK_NULL_HANDLE: a free slot */
   void           *mapped;     /* the whole block, or NULL */
   VkDeviceSize    size;
   VkDeviceSize    used;
   /* No free span is larger. Exact after a search that found nothing, an
    * upper bound otherwise, so a full block is passed over unread. */
   VkDeviceSize    max_free;
   vk_heap_span_t *spans;      /* free, sorted by offset, never adjacent */
   unsigned        span_count;
   unsigned        span_capacity;
   unsigned        type;
   unsigned        linear;     /* buffers and linear images, or optimal images */
   unsigned        dedicated;  /* one allocation, freed with it */
} vk_heap_block_t;

typedef struct vk_heap
{
   VkDevice                         device;
   vk_heap_fns_t                    fns;
   VkPhysicalDeviceMemoryProperties props;
   VkDeviceSize                     block_size;
   VkDeviceSize                     atom;       /* nonCoherentAtomSize */
   VkDeviceSize                     bytes_reserved;
   VkDeviceSize                     bytes_used;
   vk_heap_block_t                 *blocks;
   unsigned                         block_count;
   unsigned                         block_capacity;
   VkResult                         last_error;
} vk_heap_t;

/* block_size 0 means 64 MiB; a block is never more than an eighth of the
 * driver heap it comes from. atom is VkPhysicalDeviceLimits::nonCoherentAtomSize.
 * Returns 0 when fns lacks an entry point that is not optional. */
int  vk_heap_init(vk_heap_t *heap, VkDevice device,
      const VkPhysicalDeviceMemoryProperties *props, VkDeviceSize atom,
      const vk_heap_fns_t *fns, VkDeviceSize block_size);

/* Gives every block back to the driver. Nothing may still be allocated. */
void vk_heap_shutdown(vk_heap_t *heap);

#define VK_HEAP_DEDICATED_PREFERRED 1
#define VK_HEAP_DEDICATED_REQUIRED  2

/* Memory for these requirements. linear: a buffer or an image with linear
 * tiling. dedicated, one of the above or 0: a VkDeviceMemory of its own,
 * with for_buffer or for_image, if not VK_NULL_HANDLE, named in it; a
 * preference falls back to a shared block. Returns 0 when there is no
 * memory; heap->last_error says why. */
int  vk_heap_alloc(vk_heap_t *heap, const VkMemoryRequirements *req,
      enum vk_heap_usage usage, int linear, int dedicated,
      VkBuffer for_buffer, VkImage for_image, vk_heap_alloc_t *out);

/* Zeroes *alloc. */
void vk_heap_free(vk_heap_t *heap, vk_heap_alloc_t *alloc);

/* Creates the buffer or image, finds its memory and binds it. On failure
 * nothing is left behind and the result says why. */
VkResult vk_heap_create_buffer(vk_heap_t *heap, const VkBufferCreateInfo *info,
      enum vk_heap_usage usage, VkBuffer *buffer, vk_heap_alloc_t *out);
VkResult vk_heap_create_image(vk_heap_t *heap, const VkImageCreateInfo *info,
      enum vk_heap_usage usage, VkImage *image, vk_heap_alloc_t *out);

/* Destroys the object and frees its memory; both may be null. */
void vk_heap_destroy_buffer(vk_heap_t *heap, VkBuffer buffer, vk_heap_alloc_t *alloc);
void vk_heap_destroy_image(vk_heap_t *heap, VkImage image, vk_heap_alloc_t *alloc);

/* Whether CPU reads need vk_heap_invalidate first. */
int  vk_heap_is_coherent(const vk_heap_t *heap, const vk_heap_alloc_t *alloc);

/* Makes device writes to [offset, offset + size) of the allocation visible to
 * the CPU. Nothing to do on coherent memory. */
VkResult vk_heap_invalidate(vk_heap_t *heap, const vk_heap_alloc_t *alloc,
      VkDeviceSize offset, VkDeviceSize size);

#ifdef __cplusplus
}
#endif

#endif
