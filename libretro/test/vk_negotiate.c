/* Vulkan context negotiation test host.
 *
 * usage: vk_negotiate core content|- [key=value ...]
 *
 * Loads the core as a frontend that asks for Vulkan, serves each
 * key=value as a core option and loads the content. When the core has
 * set up its Vulkan negotiation interface ("-" loads no content), creates an instance and has
 * the core create the device, as a frontend does before context_reset:
 *
 *   VKN_OFFER=2 (default)  a frontend with negotiation v2, as RetroArch:
 *                          create_device2 when the core offers it, with
 *                          the frontend's wrapper, which adds an extension
 *                          of its own where the GPU has it; create_device
 *                          otherwise. VKN_EXPECT_WRAPPER=1 fails the run
 *                          unless the core made its device through that
 *                          wrapper.
 *   VKN_OFFER=1            a frontend that predates v2: no support query,
 *                          only a version 1 interface is accepted, and
 *                          the device comes from create_device.
 *
 * VKN_LOAD_MAY_FAIL=1 goes on to the device when the content fails to load
 * after the core has asked for Vulkan, for cores that need real content to
 * boot but negotiate before reading it.
 *
 * VK_LAYER_KHRONOS_validation is enabled when installed, and any error it
 * reports fails the run. VKN_DIR is the system/save directory (default
 * "."). Exits 0 when the device was made as described, non-zero otherwise.
 *
 * VKN_FRAMES=N then goes on as a frontend: serves the HW render interface,
 * calls context_reset and runs N frames, reading back each picture the
 * core hands over and printing a digest of them ("vk_negotiate: frames=N
 * shown=S digest=D"). The core's teardown is not part of the test. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "libretro.h"
#include "libretro_vulkan.h"

#ifdef _WIN32
#include <windows.h>
#define dlopen(path, flags) ((void*)LoadLibraryA(path))
#define dlsym(h, name) ((void*)GetProcAddress((HMODULE)(h), name))
#define VKN_LIBVULKAN "vulkan-1.dll"
static CRITICAL_SECTION queue_lock;
#define QUEUE_LOCK_INIT() InitializeCriticalSection(&queue_lock)
#define QUEUE_LOCK() EnterCriticalSection(&queue_lock)
#define QUEUE_UNLOCK() LeaveCriticalSection(&queue_lock)
static void boot_poll_sleep(void) { Sleep(1); }
#else
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#define VKN_LIBVULKAN "libvulkan.so.1"
/* The frontend's queue mutex, as RetroArch has: the core's render thread
 * and this host's readback share the queue. */
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
#define QUEUE_LOCK_INIT()
#define QUEUE_LOCK() pthread_mutex_lock(&queue_lock)
#define QUEUE_UNLOCK() pthread_mutex_unlock(&queue_lock)
static void boot_poll_sleep(void) { usleep(1000); }
#endif

static char *opt_keys[64], *opt_vals[64];
static int nopts;
static unsigned offer = 2;
static struct retro_hw_render_callback hw;
static int have_hw;
static const struct retro_hw_render_context_negotiation_interface_vulkan *neg;
static unsigned wrapper_calls;
static VkPhysicalDevice wrapper_gpu;
static int wrapper_added;
static int validation_errors;

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkCreateInstance p_vkCreateInstance;
static PFN_vkEnumerateInstanceLayerProperties p_vkEnumerateInstanceLayerProperties;
static PFN_vkEnumeratePhysicalDevices p_vkEnumeratePhysicalDevices;
static PFN_vkEnumerateDeviceExtensionProperties p_vkEnumerateDeviceExtensionProperties;
static PFN_vkCreateDevice p_vkCreateDevice;
static PFN_vkGetDeviceProcAddr p_vkGetDeviceProcAddr;

/* VKN_FRAMES: the frontend side of the HW render interface. */
static struct retro_hw_render_interface_vulkan vk_if;
static struct retro_vulkan_context vk_ctx;
static struct retro_vulkan_image vk_image;
static int vk_have_image;
static uint32_t vk_sync;
static VkCommandBuffer vk_pending[16];
static uint32_t vk_pending_count;
static VkCommandPool vk_pool;
static VkCommandBuffer vk_copy_cmd;
static VkBuffer vk_buf;
static VkDeviceMemory vk_mem;
static VkDeviceSize vk_buf_size;
static void *vk_map;
static VkFence vk_fence;
static VkPhysicalDeviceMemoryProperties vk_mem_props;
static unsigned long frames_shown;
static uint64_t frames_digest = 1469598103934665603ULL;

#define VKF(name) static PFN_##name p_##name
VKF(vkQueueSubmit); VKF(vkQueueWaitIdle); VKF(vkCreateCommandPool); VKF(vkAllocateCommandBuffers);
VKF(vkBeginCommandBuffer); VKF(vkEndCommandBuffer); VKF(vkCmdPipelineBarrier); VKF(vkCmdCopyImageToBuffer);
VKF(vkCreateBuffer); VKF(vkDestroyBuffer); VKF(vkGetBufferMemoryRequirements); VKF(vkAllocateMemory);
VKF(vkFreeMemory); VKF(vkBindBufferMemory); VKF(vkMapMemory); VKF(vkCreateFence); VKF(vkWaitForFences);
VKF(vkResetFences); VKF(vkResetCommandBuffer); VKF(vkGetPhysicalDeviceMemoryProperties);

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
   va_list ap;
   if (level < RETRO_LOG_WARN && !getenv("VKN_VERBOSE"))
      return;
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
}

static bool environ_cb(unsigned cmd, void *data)
{
   const char *dir = getenv("VKN_DIR") ? getenv("VKN_DIR") : ".";
   int i;
   switch (cmd)
   {
      case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
         ((struct retro_log_callback*)data)->log = log_cb;
         return true;
      case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
      case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
      case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
         *(const char**)data = dir;
         return true;
      case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
      case RETRO_ENVIRONMENT_SET_VARIABLES:
         return true;
      case RETRO_ENVIRONMENT_GET_CAN_DUPE:
         *(bool*)data = true;
         return true;
      case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
         *(bool*)data = false;
         return true;
      case RETRO_ENVIRONMENT_GET_VARIABLE:
      {
         struct retro_variable *var = (struct retro_variable*)data;
         for (i = 0; i < nopts; i++)
            if (!strcmp(opt_keys[i], var->key))
            {
               var->value = opt_vals[i];
               return true;
            }
         var->value = NULL;
         return false;
      }
      case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
         if (!vk_if.device)
            return false;
         *(const struct retro_hw_render_interface **)data = (const struct retro_hw_render_interface *)&vk_if;
         return true;
      case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
         *(unsigned*)data = RETRO_HW_CONTEXT_VULKAN;
         return true;
      case RETRO_ENVIRONMENT_SET_HW_RENDER:
      {
         const struct retro_hw_render_callback *cb = (const struct retro_hw_render_callback*)data;
         if (cb->context_type != RETRO_HW_CONTEXT_VULKAN)
            return false;
         hw = *cb;
         have_hw = 1;
         return true;
      }
      case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE:
      {
         const struct retro_hw_render_context_negotiation_interface *n =
            (const struct retro_hw_render_context_negotiation_interface*)data;
         if (n->interface_type != RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN)
            return false;
         neg = (const struct retro_hw_render_context_negotiation_interface_vulkan*)data;
         /* RetroArch before v2 dropped any other version */
         if (offer < 2 && n->interface_version != 1)
         {
            fprintf(stderr, "vk_negotiate: interface version %u dropped\n", n->interface_version);
            neg = NULL;
         }
         return true;
      }
      case RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT:
      {
         struct retro_hw_render_context_negotiation_interface *n =
            (struct retro_hw_render_context_negotiation_interface*)data;
         if (offer < 2)
            return false;
         n->interface_version = n->interface_type == RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN ? 2 : 0;
         return true;
      }
      default:
         return false;
   }
}

static void fnv(uint64_t *h, const void *data, size_t size)
{
   const unsigned char *p = (const unsigned char *)data;
   size_t i;
   for (i = 0; i < size; i++)
      *h = (*h ^ p[i]) * 1099511628211ULL;
}

static void vk_set_image(void *handle, const struct retro_vulkan_image *image, uint32_t num_semaphores,
      const VkSemaphore *semaphores, uint32_t src_queue_family)
{
   (void)handle; (void)num_semaphores; (void)semaphores; (void)src_queue_family;
   vk_have_image = image != NULL;
   if (image)
      vk_image = *image;
}
static uint32_t vk_get_sync_index(void *handle) { (void)handle; return vk_sync; }
static uint32_t vk_get_sync_index_mask(void *handle) { (void)handle; return 3; }
static void vk_set_command_buffers(void *handle, uint32_t num, const VkCommandBuffer *cmd)
{
   uint32_t i;
   (void)handle;
   for (i = 0; i < num && vk_pending_count < 16; i++)
      vk_pending[vk_pending_count++] = cmd[i];
}
static void vk_wait_sync_index(void *handle)
{
   (void)handle;
   QUEUE_LOCK();
   p_vkQueueWaitIdle(vk_ctx.queue);
   QUEUE_UNLOCK();
}
static void vk_lock_queue(void *handle) { (void)handle; QUEUE_LOCK(); }
static void vk_unlock_queue(void *handle) { (void)handle; QUEUE_UNLOCK(); }
static void vk_set_signal_semaphore(void *handle, VkSemaphore semaphore) { (void)handle; (void)semaphore; }

/* A host-visible buffer of at least size bytes for the readback. */
static int vk_readback_buffer(VkDeviceSize size)
{
   VkBufferCreateInfo bi;
   VkMemoryRequirements req;
   VkMemoryAllocateInfo mi;
   const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   uint32_t t;
   if (size <= vk_buf_size)
      return 1;
   if (vk_buf)
   {
      p_vkDestroyBuffer(vk_ctx.device, vk_buf, NULL);
      p_vkFreeMemory(vk_ctx.device, vk_mem, NULL);
      vk_buf = VK_NULL_HANDLE;
      vk_buf_size = 0;
   }
   memset(&bi, 0, sizeof(bi));
   bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
   bi.size = size;
   bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
   if (p_vkCreateBuffer(vk_ctx.device, &bi, NULL, &vk_buf) != VK_SUCCESS)
      return 0;
   p_vkGetBufferMemoryRequirements(vk_ctx.device, vk_buf, &req);
   for (t = 0; t < vk_mem_props.memoryTypeCount; t++)
      if ((req.memoryTypeBits >> t & 1) && (vk_mem_props.memoryTypes[t].propertyFlags & want) == want)
         break;
   memset(&mi, 0, sizeof(mi));
   mi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   mi.allocationSize = req.size;
   mi.memoryTypeIndex = t;
   if (t == vk_mem_props.memoryTypeCount || p_vkAllocateMemory(vk_ctx.device, &mi, NULL, &vk_mem) != VK_SUCCESS)
      return 0;
   p_vkBindBufferMemory(vk_ctx.device, vk_buf, vk_mem, 0);
   if (p_vkMapMemory(vk_ctx.device, vk_mem, 0, VK_WHOLE_SIZE, 0, &vk_map) != VK_SUCCESS)
      return 0;
   vk_buf_size = size;
   return 1;
}

/* The picture the core set, read back and added to the digest. */
static void vk_picture(unsigned w, unsigned h)
{
   VkCommandBufferBeginInfo bi;
   VkImageMemoryBarrier b;
   VkBufferImageCopy region;
   VkSubmitInfo si;
   unsigned y;
   QUEUE_LOCK();
   if (vk_pending_count)
   {
      memset(&si, 0, sizeof(si));
      si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      si.commandBufferCount = vk_pending_count;
      si.pCommandBuffers = vk_pending;
      p_vkQueueSubmit(vk_ctx.queue, 1, &si, VK_NULL_HANDLE);
      vk_pending_count = 0;
   }
   if (!vk_have_image || !w || !h || !vk_readback_buffer((VkDeviceSize)w * h * 4))
   {
      QUEUE_UNLOCK();
      return;
   }
   p_vkResetCommandBuffer(vk_copy_cmd, 0);
   memset(&bi, 0, sizeof(bi));
   bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
   bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
   p_vkBeginCommandBuffer(vk_copy_cmd, &bi);
   memset(&b, 0, sizeof(b));
   b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
   b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
   b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   b.oldLayout = vk_image.image_layout;
   b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   b.image = vk_image.create_info.image;
   b.subresourceRange = vk_image.create_info.subresourceRange;
   p_vkCmdPipelineBarrier(vk_copy_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
         0, NULL, 0, NULL, 1, &b);
   memset(&region, 0, sizeof(region));
   region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   region.imageSubresource.layerCount = 1;
   region.imageExtent.width = w;
   region.imageExtent.height = h;
   region.imageExtent.depth = 1;
   p_vkCmdCopyImageToBuffer(vk_copy_cmd, vk_image.create_info.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk_buf, 1, &region);
   b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
   b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   b.newLayout = vk_image.image_layout;
   p_vkCmdPipelineBarrier(vk_copy_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
         0, NULL, 0, NULL, 1, &b);
   p_vkEndCommandBuffer(vk_copy_cmd);
   memset(&si, 0, sizeof(si));
   si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
   si.commandBufferCount = 1;
   si.pCommandBuffers = &vk_copy_cmd;
   p_vkResetFences(vk_ctx.device, 1, &vk_fence);
   p_vkQueueSubmit(vk_ctx.queue, 1, &si, vk_fence);
   QUEUE_UNLOCK();
   p_vkWaitForFences(vk_ctx.device, 1, &vk_fence, VK_TRUE, ~(uint64_t)0);
   fnv(&frames_digest, &vk_image.create_info.format, sizeof(vk_image.create_info.format));
   for (y = 0; y < h; y++)
      fnv(&frames_digest, (const unsigned char *)vk_map + (size_t)y * w * 4, (size_t)w * 4);
   frames_shown++;
}

static void video_cb(const void *d, unsigned w, unsigned h, size_t p)
{
   (void)p;
   if (d == RETRO_HW_FRAME_BUFFER_VALID)
      vk_picture(w, h);
   /* The next frame renders into the next image. */
   vk_sync = (vk_sync + 1) & 1;
}
static void audio_cb(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch_cb(const int16_t *d, size_t f) { (void)d; return f; }
static void input_poll_cb(void) { }
static int16_t input_state_cb(unsigned p, unsigned d, unsigned i, unsigned id) { (void)p; (void)d; (void)i; (void)id; return 0; }

/* create_device2's wrapper, as RetroArch's: adds an extension of its own
 * where the GPU has it and the core did not ask for it, then creates the
 * device. */
static VkDevice create_device_wrapper(VkPhysicalDevice gpu, void *opaque, const VkDeviceCreateInfo *info)
{
   static const char added[] = "VK_KHR_present_id";
   VkDeviceCreateInfo local = *info;
   const char *exts[256];
   VkExtensionProperties props[512];
   uint32_t i, j, n = 512, count = info->enabledExtensionCount;
   VkDevice device = VK_NULL_HANDLE;
   (void)opaque;
   wrapper_calls++;
   wrapper_gpu = gpu;
   if (count > 255)
      return VK_NULL_HANDLE;
   if (count)
      memcpy(exts, info->ppEnabledExtensionNames, count * sizeof(*exts));
   if (p_vkEnumerateDeviceExtensionProperties(gpu, NULL, &n, props) >= 0)
      for (i = 0; i < n; i++)
         if (!strcmp(props[i].extensionName, added))
         {
            for (j = 0; j < count && strcmp(exts[j], added); j++) ;
            if (j == count)
            {
               exts[count++] = added;
               wrapper_added = 1;
            }
         }
   local.enabledExtensionCount   = count;
   local.ppEnabledExtensionNames = exts;
   if (p_vkCreateDevice(gpu, &local, NULL, &device) != VK_SUCCESS)
      return VK_NULL_HANDLE;
   return device;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
      VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT *d, void *u)
{
   (void)type; (void)u;
   if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
   {
      validation_errors++;
      fprintf(stderr, "vk_negotiate: validation: %s\n", d->pMessage);
   }
   return VK_FALSE;
}

/* Leaves without the core's teardown (not part of the test), which static
 * destructors would run on a return from main. */
static int finish(int code)
{
   fflush(stdout);
   fflush(stderr);
#ifdef _WIN32
   ExitProcess((UINT)code);
#else
   _Exit(code);
#endif
   return code;
}

static void *load_file(const char *path, size_t *size)
{
   FILE *f = fopen(path, "rb");
   void *buf;
   long n;
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   n = ftell(f);
   fseek(f, 0, SEEK_SET);
   buf = malloc(n > 0 ? (size_t)n : 1);
   if (buf && n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n)
   {
      free(buf);
      buf = NULL;
   }
   fclose(f);
   *size = n > 0 ? (size_t)n : 0;
   return buf;
}

typedef void (*set_env_t)(retro_environment_t);
typedef void (*set_video_t)(retro_video_refresh_t);
typedef void (*set_audio_t)(retro_audio_sample_t);
typedef void (*set_audio_batch_t)(retro_audio_sample_batch_t);
typedef void (*set_poll_t)(retro_input_poll_t);
typedef void (*set_state_t)(retro_input_state_t);
typedef void (*init_t)(void);
typedef bool (*load_game_t)(const struct retro_game_info*);
#define CORE_FN(type, name) type name = (type)dlsym(core, #name)

int main(int argc, char **argv)
{
   void *core, *vk;
   struct retro_game_info game;
   VkApplicationInfo app;
   VkInstanceCreateInfo ici;
   VkDebugUtilsMessengerCreateInfoEXT dci;
   VkLayerProperties layers[64];
   uint32_t nlayers = 64, ngpu = 1, i;
   const char *layer = "VK_LAYER_KHRONOS_validation";
   const char *iexts[2];
   uint32_t niexts = 0;
   VkExtensionProperties iprops[128];
   uint32_t niprops = 128;
   PFN_vkEnumerateInstanceExtensionProperties p_vkEnumerateInstanceExtensionProperties;
   int validation = 0;
   VkInstance instance;
   VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
   VkPhysicalDevice gpu;
   struct retro_vulkan_context ctx;
   static const VkPhysicalDeviceFeatures no_features;
   PFN_vkGetDeviceQueue p_vkGetDeviceQueue;
   VkQueue queue = VK_NULL_HANDLE;
   size_t size = 0;

   if (argc < 3)
   {
      fprintf(stderr, "usage: vk_negotiate core content [key=value ...]\n");
      return 2;
   }
   if (getenv("VKN_OFFER"))
      offer = (unsigned)atoi(getenv("VKN_OFFER"));
   for (i = 3; i < (uint32_t)argc && nopts < 64; i++)
   {
      char *eq = strchr(argv[i], '=');
      if (!eq)
         continue;
      *eq = '\0';
      opt_keys[nopts] = argv[i];
      opt_vals[nopts++] = eq + 1;
   }

   if (!(core = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL)))
   {
      fprintf(stderr, "vk_negotiate: cannot load %s\n", argv[1]);
      return 2;
   }
   {
      CORE_FN(set_env_t, retro_set_environment);
      CORE_FN(set_video_t, retro_set_video_refresh);
      CORE_FN(set_audio_t, retro_set_audio_sample);
      CORE_FN(set_audio_batch_t, retro_set_audio_sample_batch);
      CORE_FN(set_poll_t, retro_set_input_poll);
      CORE_FN(set_state_t, retro_set_input_state);
      CORE_FN(init_t, retro_init);
      CORE_FN(load_game_t, retro_load_game);
      retro_set_environment(environ_cb);
      retro_init();
      retro_set_video_refresh(video_cb);
      retro_set_audio_sample(audio_cb);
      retro_set_audio_sample_batch(audio_batch_cb);
      retro_set_input_poll(input_poll_cb);
      retro_set_input_state(input_state_cb);
      memset(&game, 0, sizeof(game));
      game.path = argv[2];
      if (strcmp(argv[2], "-"))
         game.data = load_file(argv[2], &size);
      game.size = size;
      if (!retro_load_game(strcmp(argv[2], "-") ? &game : NULL))
      {
         fprintf(stderr, "vk_negotiate: retro_load_game failed\n");
         if (!getenv("VKN_LOAD_MAY_FAIL") || !atoi(getenv("VKN_LOAD_MAY_FAIL")))
            return finish(2);
      }
   }
   if (!have_hw)
   {
      fprintf(stderr, "vk_negotiate: the core did not ask for Vulkan\n");
      return finish(1);
   }
   if (!neg)
   {
      fprintf(stderr, "vk_negotiate: no negotiation interface\n");
      return finish(1);
   }

   if (!(vk = dlopen(VKN_LIBVULKAN, RTLD_NOW | RTLD_LOCAL)))
   {
      fprintf(stderr, "vk_negotiate: no Vulkan loader\n");
      return finish(2);
   }
   gipa = (PFN_vkGetInstanceProcAddr)dlsym(vk, "vkGetInstanceProcAddr");
   p_vkCreateInstance = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
   p_vkEnumerateInstanceLayerProperties = (PFN_vkEnumerateInstanceLayerProperties)
      gipa(VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties");
   if (p_vkEnumerateInstanceLayerProperties(&nlayers, layers) >= 0)
      for (i = 0; i < nlayers; i++)
         if (!strcmp(layers[i].layerName, layer))
            validation = 1;
   /* VK_KHR_surface, as on RetroArch's instance: cores enable
    * VK_KHR_swapchain, which requires it */
   p_vkEnumerateInstanceExtensionProperties = (PFN_vkEnumerateInstanceExtensionProperties)
      gipa(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
   if (p_vkEnumerateInstanceExtensionProperties(NULL, &niprops, iprops) >= 0)
      for (i = 0; i < niprops; i++)
         if (!strcmp(iprops[i].extensionName, "VK_KHR_surface"))
            iexts[niexts++] = "VK_KHR_surface";
   if (validation)
      iexts[niexts++] = "VK_EXT_debug_utils";

   memset(&app, 0, sizeof(app));
   app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
   app.pApplicationName = "vk_negotiate";
   app.apiVersion = VK_API_VERSION_1_1;
   if (neg->get_application_info && neg->get_application_info())
      app = *neg->get_application_info();
   /* RetroArch raises a core's 1.0 to 1.1 where the loader has it */
   if (app.apiVersion < VK_API_VERSION_1_1)
   {
      PFN_vkEnumerateInstanceVersion p_vkEnumerateInstanceVersion = (PFN_vkEnumerateInstanceVersion)
         gipa(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
      uint32_t supported = 0;
      if (p_vkEnumerateInstanceVersion && p_vkEnumerateInstanceVersion(&supported) == VK_SUCCESS
            && supported >= VK_API_VERSION_1_1)
         app.apiVersion = VK_API_VERSION_1_1;
   }
   memset(&dci, 0, sizeof(dci));
   dci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
   dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
   dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
   dci.pfnUserCallback = debug_cb;
   memset(&ici, 0, sizeof(ici));
   ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
   ici.pApplicationInfo = &app;
   ici.enabledExtensionCount = niexts;
   ici.ppEnabledExtensionNames = niexts ? iexts : NULL;
   if (validation)
   {
      ici.pNext = &dci;
      ici.enabledLayerCount = 1;
      ici.ppEnabledLayerNames = &layer;
   }
   if (p_vkCreateInstance(&ici, NULL, &instance) != VK_SUCCESS)
   {
      fprintf(stderr, "vk_negotiate: vkCreateInstance failed\n");
      return finish(2);
   }
   if (validation)
   {
      /* the pNext one covers instance creation only */
      PFN_vkCreateDebugUtilsMessengerEXT create_messenger = (PFN_vkCreateDebugUtilsMessengerEXT)
         gipa(instance, "vkCreateDebugUtilsMessengerEXT");
      if (!create_messenger || create_messenger(instance, &dci, NULL, &messenger) != VK_SUCCESS)
      {
         fprintf(stderr, "vk_negotiate: no debug messenger\n");
         return finish(2);
      }
   }
   p_vkEnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices");
   p_vkEnumerateDeviceExtensionProperties = (PFN_vkEnumerateDeviceExtensionProperties)
      gipa(instance, "vkEnumerateDeviceExtensionProperties");
   p_vkCreateDevice = (PFN_vkCreateDevice)gipa(instance, "vkCreateDevice");
   p_vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
   if (p_vkEnumeratePhysicalDevices(instance, &ngpu, &gpu) < 0 || !ngpu)
   {
      fprintf(stderr, "vk_negotiate: no GPU\n");
      return finish(2);
   }

   memset(&ctx, 0, sizeof(ctx));
   if (offer >= 2 && neg->interface_version >= 2 && neg->create_device2)
   {
      if (!neg->create_device2(&ctx, instance, gpu, VK_NULL_HANDLE, gipa, create_device_wrapper, NULL))
      {
         fprintf(stderr, "vk_negotiate: create_device2 failed\n");
         return finish(1);
      }
      if (wrapper_calls != 1 || wrapper_gpu != gpu)
      {
         fprintf(stderr, "vk_negotiate: the core did not create its device through the frontend (%u)\n", wrapper_calls);
         return finish(1);
      }
   }
   else if (getenv("VKN_EXPECT_WRAPPER") && atoi(getenv("VKN_EXPECT_WRAPPER")))
   {
      fprintf(stderr, "vk_negotiate: the core did not offer create_device2\n");
      return finish(1);
   }
   else if (!neg->create_device
         || !neg->create_device(&ctx, instance, gpu, VK_NULL_HANDLE, gipa, NULL, 0, NULL, 0, &no_features))
   {
      fprintf(stderr, "vk_negotiate: create_device failed\n");
      return finish(1);
   }

   if (ctx.gpu != gpu || ctx.device == VK_NULL_HANDLE || ctx.queue == VK_NULL_HANDLE)
   {
      fprintf(stderr, "vk_negotiate: the core returned no usable device\n");
      return finish(1);
   }
   p_vkGetDeviceQueue = (PFN_vkGetDeviceQueue)p_vkGetDeviceProcAddr(ctx.device, "vkGetDeviceQueue");
   p_vkGetDeviceQueue(ctx.device, ctx.queue_family_index, 0, &queue);
   if (queue == VK_NULL_HANDLE)
   {
      fprintf(stderr, "vk_negotiate: the queue family is not the device's\n");
      return finish(1);
   }
   if (validation_errors)
      return finish(1);
   printf("vk_negotiate: frontend v%u, device made %s%s\n", offer >= 2 ? 2u : 1u,
         wrapper_calls ? "through the frontend (create_device2)" : "by the core (create_device)",
         wrapper_added ? ", with the frontend's extension" : "");
   if (getenv("VKN_FRAMES"))
   {
      typedef void (*run_t)(void);
      run_t run = (run_t)dlsym(core, "retro_run");
      const int frames = atoi(getenv("VKN_FRAMES"));
      VkCommandPoolCreateInfo pci;
      VkCommandBufferAllocateInfo ai;
      VkFenceCreateInfo fi;
      PFN_vkGetPhysicalDeviceMemoryProperties get_props = (PFN_vkGetPhysicalDeviceMemoryProperties)
         gipa(instance, "vkGetPhysicalDeviceMemoryProperties");
      int f;

      QUEUE_LOCK_INIT();
      vk_ctx = ctx;
#define VKD(name) p_##name = (PFN_##name)p_vkGetDeviceProcAddr(ctx.device, #name)
      VKD(vkQueueSubmit); VKD(vkQueueWaitIdle); VKD(vkCreateCommandPool); VKD(vkAllocateCommandBuffers);
      VKD(vkBeginCommandBuffer); VKD(vkEndCommandBuffer); VKD(vkCmdPipelineBarrier); VKD(vkCmdCopyImageToBuffer);
      VKD(vkCreateBuffer); VKD(vkDestroyBuffer); VKD(vkGetBufferMemoryRequirements); VKD(vkAllocateMemory);
      VKD(vkFreeMemory); VKD(vkBindBufferMemory); VKD(vkMapMemory); VKD(vkCreateFence); VKD(vkWaitForFences);
      VKD(vkResetFences); VKD(vkResetCommandBuffer);
#undef VKD
      get_props(gpu, &vk_mem_props);
      memset(&pci, 0, sizeof(pci));
      pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      pci.queueFamilyIndex = ctx.queue_family_index;
      p_vkCreateCommandPool(ctx.device, &pci, NULL, &vk_pool);
      memset(&ai, 0, sizeof(ai));
      ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      ai.commandPool = vk_pool;
      ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      ai.commandBufferCount = 1;
      p_vkAllocateCommandBuffers(ctx.device, &ai, &vk_copy_cmd);
      memset(&fi, 0, sizeof(fi));
      fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      p_vkCreateFence(ctx.device, &fi, NULL, &vk_fence);

      vk_if.interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN;
      vk_if.interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
      vk_if.handle = &vk_if;
      vk_if.instance = instance;
      vk_if.gpu = gpu;
      vk_if.device = ctx.device;
      vk_if.get_device_proc_addr = p_vkGetDeviceProcAddr;
      vk_if.get_instance_proc_addr = gipa;
      vk_if.queue = ctx.queue;
      vk_if.queue_index = ctx.queue_family_index;
      vk_if.set_image = vk_set_image;
      vk_if.get_sync_index = vk_get_sync_index;
      vk_if.get_sync_index_mask = vk_get_sync_index_mask;
      vk_if.set_command_buffers = vk_set_command_buffers;
      vk_if.wait_sync_index = vk_wait_sync_index;
      vk_if.lock_queue = vk_lock_queue;
      vk_if.unlock_queue = vk_unlock_queue;
      vk_if.set_signal_semaphore = vk_set_signal_semaphore;
      {
         /* Asked before context_reset, as a frontend does: the core sizes
          * its output from it. */
         typedef void (*av_info_t)(struct retro_system_av_info *);
         av_info_t av_info = (av_info_t)dlsym(core, "retro_get_system_av_info");
         struct retro_system_av_info av;
         memset(&av, 0, sizeof(av));
         if (av_info)
            av_info(&av);
      }
      if (hw.context_reset)
         hw.context_reset();
      /* Boot runs return without a picture until the core is up: those do
       * not count, for as long as a slow machine takes (up to a minute). */
      for (f = 0; f < 60000 && !frames_shown; f++)
      {
         run();
         if (!frames_shown)
            boot_poll_sleep();
      }
      for (f = 1; f < frames; f++)
         run();
      if (validation_errors)
         return finish(1);
      printf("vk_negotiate: frames=%d shown=%lu digest=%016llx\n", frames, frames_shown,
            (unsigned long long)frames_digest);
   }
   return finish(0);
}
