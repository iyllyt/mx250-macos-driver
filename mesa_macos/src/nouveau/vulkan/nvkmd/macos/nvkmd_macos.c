/* SPDX-License-Identifier: MIT
 * macOS Userspace NVKMD Backend -> libnvkm_gp108.a -> GP108Bridge.kext
 */
#include "nvkmd_macos.h"

#include "util/cache_ops.h"
#include "util/os_misc.h"
#include "util/os_time.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "vk_log.h"
#include "vk_sync.h"

#include "../../../../../../../nvkm_macos/gp108_nvk_bridge.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NVKMD_MACOS_HEAP_START        0x00000010000ULL /* 64 KiB (GP100 VMM start) */
#define NVKMD_MACOS_HEAP_END          (1ULL << 38)     /* 256 GiB */
#define NVKMD_MACOS_REPLAY_HEAP_START NVKMD_MACOS_HEAP_END
#define NVKMD_MACOS_REPLAY_HEAP_END   (1ULL << 39)     /* 512 GiB */

#define NVKMD_MACOS_MAX_SYNCS         256U
#define NVKMD_MACOS_RING_SLOTS        128U

/* ========================================================================= *
 * 1. Userspace Vulkan Timeline & Binary Sync Implementation                 *
 * ========================================================================= */
struct nvkmd_macos_timeline {
   struct vk_sync  base;
   pthread_mutex_t mtx;
   pthread_cond_t  cnd;
   uint64_t        signaled_val;
};

static VkResult
macos_timeline_init(struct vk_device *device, struct vk_sync *sync,
                    uint64_t initial_value)
{
   (void)device;
   struct nvkmd_macos_timeline *tl =
      container_of(sync, struct nvkmd_macos_timeline, base);
   pthread_mutex_init(&tl->mtx, NULL);
   pthread_cond_init(&tl->cnd, NULL);
   tl->signaled_val = initial_value;
   return VK_SUCCESS;
}

static void
macos_timeline_finish(struct vk_device *device, struct vk_sync *sync)
{
   (void)device;
   struct nvkmd_macos_timeline *tl =
      container_of(sync, struct nvkmd_macos_timeline, base);
   pthread_cond_destroy(&tl->cnd);
   pthread_mutex_destroy(&tl->mtx);
}

static void
macos_timeline_signal_raw(struct nvkmd_macos_timeline *tl, uint64_t value)
{
   pthread_mutex_lock(&tl->mtx);
   if (value > tl->signaled_val) {
      tl->signaled_val = value;
      pthread_cond_broadcast(&tl->cnd);
   }
   pthread_mutex_unlock(&tl->mtx);
}

static VkResult
macos_timeline_signal(struct vk_device *device, struct vk_sync *sync,
                      uint64_t value)
{
   (void)device;
   struct nvkmd_macos_timeline *tl =
      container_of(sync, struct nvkmd_macos_timeline, base);
   macos_timeline_signal_raw(tl, value);
   return VK_SUCCESS;
}

static VkResult
macos_timeline_get_value(struct vk_device *device, struct vk_sync *sync,
                         uint64_t *value)
{
   (void)device;
   struct nvkmd_macos_timeline *tl =
      container_of(sync, struct nvkmd_macos_timeline, base);
   pthread_mutex_lock(&tl->mtx);
   *value = tl->signaled_val;
   pthread_mutex_unlock(&tl->mtx);
   return VK_SUCCESS;
}

static VkResult
macos_timeline_wait_raw(struct nvkmd_macos_timeline *tl,
                        uint64_t wait_value,
                        uint64_t abs_timeout_ns)
{
   if (wait_value == 0)
      return VK_SUCCESS;

   /* Fast lock-free optimistic check + brief spin */
   uint64_t spin_end = os_time_get_nano() + 100000ULL; /* 100 us */
   while (os_time_get_nano() < spin_end) {
      pthread_mutex_lock(&tl->mtx);
      bool done = (tl->signaled_val >= wait_value);
      pthread_mutex_unlock(&tl->mtx);
      if (done)
         return VK_SUCCESS;
      if (abs_timeout_ns != OS_TIMEOUT_INFINITE && os_time_get_nano() >= abs_timeout_ns)
         return VK_TIMEOUT;
#if defined(__x86_64__)
      __asm__ volatile("pause");
#endif
   }

   pthread_mutex_lock(&tl->mtx);
   while (tl->signaled_val < wait_value) {
      if (abs_timeout_ns == 0) {
         pthread_mutex_unlock(&tl->mtx);
         return VK_TIMEOUT;
      }
      if (abs_timeout_ns == OS_TIMEOUT_INFINITE) {
         pthread_cond_wait(&tl->cnd, &tl->mtx);
      } else {
         uint64_t now_mono = os_time_get_nano();
         if (now_mono >= abs_timeout_ns) {
            pthread_mutex_unlock(&tl->mtx);
            return VK_TIMEOUT;
         }
         uint64_t rel_ns = abs_timeout_ns - now_mono;
         struct timespec ts;
         clock_gettime(CLOCK_REALTIME, &ts);
         ts.tv_sec  += (time_t)(rel_ns / 1000000000ULL);
         ts.tv_nsec += (long)(rel_ns % 1000000000ULL);
         if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
         }
         int rc = pthread_cond_timedwait(&tl->cnd, &tl->mtx, &ts);
         if (rc != 0 && tl->signaled_val < wait_value &&
             os_time_get_nano() >= abs_timeout_ns) {
            pthread_mutex_unlock(&tl->mtx);
            return VK_TIMEOUT;
         }
      }
   }
   pthread_mutex_unlock(&tl->mtx);
   return VK_SUCCESS;
}

static VkResult
macos_timeline_wait(struct vk_device *device, struct vk_sync *sync,
                    uint64_t wait_value, enum vk_sync_wait_flags wait_flags,
                    uint64_t abs_timeout_ns)
{
   (void)device;
   if (wait_flags & VK_SYNC_WAIT_PENDING)
      return VK_SUCCESS;
   struct nvkmd_macos_timeline *tl =
      container_of(sync, struct nvkmd_macos_timeline, base);
   return macos_timeline_wait_raw(tl, wait_value, abs_timeout_ns);
}

static const struct vk_sync_type nvkmd_macos_timeline_sync_type = {
   .size     = sizeof(struct nvkmd_macos_timeline),
   .features = VK_SYNC_FEATURE_TIMELINE |
               VK_SYNC_FEATURE_GPU_WAIT |
               VK_SYNC_FEATURE_CPU_WAIT |
               VK_SYNC_FEATURE_CPU_SIGNAL |
               VK_SYNC_FEATURE_WAIT_PENDING |
               VK_SYNC_FEATURE_WAIT_BEFORE_SIGNAL,
   .init      = macos_timeline_init,
   .finish    = macos_timeline_finish,
   .signal    = macos_timeline_signal,
   .get_value = macos_timeline_get_value,
   .wait      = macos_timeline_wait,
};

static void
unwrap_vk_sync(struct vk_sync *sync, uint64_t val,
               struct nvkmd_macos_timeline **out_tl, uint64_t *out_val)
{
   struct vk_sync_binary *bin = vk_sync_as_binary(sync);
   if (bin) {
      *out_tl  = container_of(&bin->timeline, struct nvkmd_macos_timeline, base);
      *out_val = bin->next_point;
   } else {
      *out_tl  = container_of(sync, struct nvkmd_macos_timeline, base);
      *out_val = val;
   }
}

/* ========================================================================= *
 * 2. Subclassed NVKMD Structs                                               *
 * ========================================================================= */
struct nvkmd_macos_pdev {
   struct nvkmd_pdev          base;
   struct vk_sync_binary_type sync_binary_type;
   const struct vk_sync_type *sync_types[3];
};
NVKMD_DECL_SUBCLASS(pdev, macos);

struct nvkmd_macos_dev {
   struct nvkmd_dev     base;
   simple_mtx_t         heap_mutex;
   struct util_vma_heap heap;
   struct util_vma_heap replay_heap;
};
NVKMD_DECL_SUBCLASS(dev, macos);

struct nvkmd_macos_mem {
   struct nvkmd_mem     base;
   struct gp108_nvk_bo *bo;
};
NVKMD_DECL_SUBCLASS(mem, macos);

struct nvkmd_macos_sparse_span {
   struct list_head link;
   uint64_t         offset_B;
   uint64_t         size_B;
};

struct nvkmd_macos_va {
   struct nvkmd_va  base;
   bool             ptes_allocated;
   simple_mtx_t     span_mtx;
   struct list_head sparse_spans;
};
NVKMD_DECL_SUBCLASS(va, macos);

struct nvkmd_macos_sync_item {
   struct nvkmd_macos_timeline *tl;
   uint64_t                     val;
};

struct nvkmd_macos_fence_job {
   uint64_t                      gpu_seq;
   uint32_t                      sig_count;
   struct nvkmd_macos_sync_item *sigs;
};

struct nvkmd_macos_exec_ctx {
   struct nvkmd_ctx             base;
   struct gp108_nvk_ctx        *hw_ctx;

   uint32_t                     wait_count;
   struct nvkmd_macos_sync_item waits[NVKMD_MACOS_MAX_SYNCS];

   uint32_t                     sig_count;
   struct nvkmd_macos_sync_item sigs[NVKMD_MACOS_MAX_SYNCS];

   struct util_dynarray         pushes; /* struct gp108_nvk_push */

   pthread_t                    worker_tid;
   pthread_mutex_t              worker_mtx;
   pthread_cond_t               worker_cnd;
   bool                         worker_stop;
   uint32_t                     job_head;
   uint32_t                     job_tail;
   uint32_t                     job_count;
   struct nvkmd_macos_fence_job jobs[NVKMD_MACOS_RING_SLOTS];
};
NVKMD_DECL_SUBCLASS(ctx, macos_exec);

struct nvkmd_macos_bind_ctx {
   struct nvkmd_ctx             base;
   uint32_t                     wait_count;
   struct nvkmd_macos_sync_item waits[NVKMD_MACOS_MAX_SYNCS];
};
NVKMD_DECL_SUBCLASS(ctx, macos_bind);

/* ========================================================================= *
 * 3. GPU Virtual Address Space (nvkmd_va)                                   *
 * ========================================================================= */
static VkResult
alloc_heap_addr(struct nvkmd_macos_dev *dev,
                struct vk_object_base *log_obj,
                enum nvkmd_va_flags flags,
                uint64_t size_B, uint64_t align_B,
                uint64_t fixed_addr, uint64_t *addr_out)
{
   simple_mtx_lock(&dev->heap_mutex);
   VkResult res = VK_SUCCESS;

   if (flags & NVKMD_VA_ALLOC_FIXED) {
      if (fixed_addr < NVKMD_MACOS_REPLAY_HEAP_START ||
          fixed_addr >= NVKMD_MACOS_REPLAY_HEAP_END ||
          (fixed_addr & (align_B - 1)) != 0 ||
          !util_vma_heap_alloc_addr(&dev->replay_heap, fixed_addr, size_B)) {
         res = vk_error(log_obj, VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS);
      } else {
         *addr_out = fixed_addr;
      }
   } else if (flags & NVKMD_VA_REPLAY) {
      *addr_out = util_vma_heap_alloc(&dev->replay_heap, size_B, align_B);
      if (*addr_out == 0)
         res = vk_error(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   } else {
      *addr_out = util_vma_heap_alloc(&dev->heap, size_B, align_B);
      if (*addr_out == 0)
         res = vk_error(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }

   simple_mtx_unlock(&dev->heap_mutex);
   return res;
}

static void
free_heap_addr(struct nvkmd_macos_dev *dev,
               enum nvkmd_va_flags flags,
               uint64_t addr, uint64_t size_B)
{
   simple_mtx_lock(&dev->heap_mutex);
   if (flags & NVKMD_VA_REPLAY)
      util_vma_heap_free(&dev->replay_heap, addr, size_B);
   else
      util_vma_heap_free(&dev->heap, addr, size_B);
   simple_mtx_unlock(&dev->heap_mutex);
}

static void
va_unmap_sparse_range_locked(struct nvkmd_macos_va *va,
                             uint64_t off_B, uint64_t len_B)
{
   uint64_t u_end = off_B + len_B;
   list_for_each_entry_safe(struct nvkmd_macos_sparse_span, sp, &va->sparse_spans, link) {
      uint64_t s_start = sp->offset_B;
      uint64_t s_end   = sp->offset_B + sp->size_B;
      if (s_end <= off_B || s_start >= u_end)
         continue;

      /* Unmap & put PTEs for existing span, then re-insert any non-overlapping tails */
      gp108_nvk_vm_unmap(va->base.addr + s_start, sp->size_B, true);
      gp108_nvk_vm_put_ptes(va->base.addr + s_start, sp->size_B);
      list_del(&sp->link);
      free(sp);
   }
}

static void
nvkmd_macos_va_free(struct nvkmd_va *_va)
{
   struct nvkmd_macos_dev *dev = nvkmd_macos_dev(_va->dev);
   struct nvkmd_macos_va *va   = nvkmd_macos_va(_va);
   const bool is_sparse = (va->base.flags & NVKMD_VA_SPARSE) != 0;

   if (is_sparse) {
      simple_mtx_lock(&va->span_mtx);
      va_unmap_sparse_range_locked(va, 0, va->base.size_B);
      simple_mtx_unlock(&va->span_mtx);
      gp108_nvk_vm_sparse(va->base.addr, va->base.size_B, false);
   } else if (va->ptes_allocated) {
      gp108_nvk_vm_unmap(va->base.addr, va->base.size_B, false);
      gp108_nvk_vm_put_ptes(va->base.addr, va->base.size_B);
   }

   free_heap_addr(dev, va->base.flags, va->base.addr, va->base.size_B);
   simple_mtx_destroy(&va->span_mtx);
   FREE(va);
}

static VkResult
nvkmd_macos_va_bind_mem(struct nvkmd_va *_va,
                        struct vk_object_base *log_obj,
                        uint64_t va_offset_B,
                        struct nvkmd_mem *_mem,
                        uint64_t mem_offset_B,
                        uint64_t range_B)
{
   struct nvkmd_macos_va  *va  = nvkmd_macos_va(_va);
   struct nvkmd_macos_mem *mem = nvkmd_macos_mem(_mem);
   const bool is_sparse = (va->base.flags & NVKMD_VA_SPARSE) != 0;
   const uint64_t gpu_addr = va->base.addr + va_offset_B;
   int err;

   if (is_sparse) {
      simple_mtx_lock(&va->span_mtx);
      va_unmap_sparse_range_locked(va, va_offset_B, range_B);
      err = gp108_nvk_vm_get_ptes(gpu_addr, range_B);
      if (err == 0) {
         err = gp108_nvk_vm_map(gpu_addr, range_B, va->base.pte_kind,
                                mem->bo, mem_offset_B);
         if (err == 0) {
            struct nvkmd_macos_sparse_span *sp = calloc(1, sizeof(*sp));
            if (sp) {
               sp->offset_B = va_offset_B;
               sp->size_B   = range_B;
               list_addtail(&sp->link, &va->sparse_spans);
            }
         } else {
            gp108_nvk_vm_put_ptes(gpu_addr, range_B);
         }
      }
      simple_mtx_unlock(&va->span_mtx);
   } else {
      gp108_nvk_vm_unmap(gpu_addr, range_B, false);
      err = gp108_nvk_vm_map(gpu_addr, range_B, va->base.pte_kind,
                             mem->bo, mem_offset_B);
   }

   if (err != 0)
      return vk_errorf(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "gp108_nvk_vm_map failed: %d", err);
   return VK_SUCCESS;
}

static VkResult
nvkmd_macos_va_unbind(struct nvkmd_va *_va,
                      struct vk_object_base *log_obj,
                      uint64_t va_offset_B,
                      uint64_t range_B)
{
   (void)log_obj;
   struct nvkmd_macos_va *va = nvkmd_macos_va(_va);
   const bool is_sparse = (va->base.flags & NVKMD_VA_SPARSE) != 0;

   if (is_sparse) {
      simple_mtx_lock(&va->span_mtx);
      va_unmap_sparse_range_locked(va, va_offset_B, range_B);
      simple_mtx_unlock(&va->span_mtx);
   } else {
      gp108_nvk_vm_unmap(va->base.addr + va_offset_B, range_B, false);
   }
   return VK_SUCCESS;
}

const struct nvkmd_va_ops nvkmd_macos_va_ops = {
   .free     = nvkmd_macos_va_free,
   .bind_mem = nvkmd_macos_va_bind_mem,
   .unbind   = nvkmd_macos_va_unbind,
};

static VkResult
nvkmd_macos_alloc_va(struct nvkmd_dev *_dev,
                     struct vk_object_base *log_obj,
                     enum nvkmd_va_flags flags, uint8_t pte_kind,
                     uint64_t size_B, uint64_t align_B,
                     uint64_t fixed_addr, struct nvkmd_va **va_out)
{
   struct nvkmd_macos_dev *dev = nvkmd_macos_dev(_dev);
   struct nvkmd_macos_va *va = CALLOC_STRUCT(nvkmd_macos_va);
   if (!va)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   align_B = MAX2(align_B, _dev->pdev->bind_align_B);
   size_B  = align64(size_B, align_B);

   VkResult result = alloc_heap_addr(dev, log_obj, flags, size_B, align_B,
                                     fixed_addr, &va->base.addr);
   if (result != VK_SUCCESS) {
      FREE(va);
      return result;
   }

   simple_mtx_init(&va->span_mtx, mtx_plain);
   list_inithead(&va->sparse_spans);

   if (flags & NVKMD_VA_SPARSE) {
      if (gp108_nvk_vm_sparse(va->base.addr, size_B, true) != 0) {
         free_heap_addr(dev, flags, va->base.addr, size_B);
         simple_mtx_destroy(&va->span_mtx);
         FREE(va);
         return vk_error(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      }
   } else {
      if (gp108_nvk_vm_get_ptes(va->base.addr, size_B) != 0) {
         free_heap_addr(dev, flags, va->base.addr, size_B);
         simple_mtx_destroy(&va->span_mtx);
         FREE(va);
         return vk_error(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      }
      va->ptes_allocated = true;
   }

   va->base.ops      = &nvkmd_macos_va_ops;
   va->base.dev      = &dev->base;
   va->base.flags    = flags;
   va->base.pte_kind = pte_kind;
   va->base.size_B   = size_B;
   *va_out = &va->base;
   return VK_SUCCESS;
}

/* ========================================================================= *
 * 4. Memory Allocation & Mapping (nvkmd_mem)                                *
 * ========================================================================= */
static void
nvkmd_macos_mem_free(struct nvkmd_mem *_mem)
{
   struct nvkmd_macos_mem *mem = nvkmd_macos_mem(_mem);
   if (mem->base.va)
      nvkmd_va_free(mem->base.va);
   gp108_nvk_bo_free(mem->bo);
   FREE(mem);
}

static VkResult
nvkmd_macos_mem_map(struct nvkmd_mem *_mem,
                    struct vk_object_base *log_obj,
                    enum nvkmd_mem_map_flags map_flags,
                    void *fixed_addr,
                    void **map_out)
{
   (void)map_flags;
   if (fixed_addr != NULL)
      return vk_error(log_obj, VK_ERROR_MEMORY_MAP_FAILED);

   struct nvkmd_macos_mem *mem = nvkmd_macos_mem(_mem);
   void *ptr = gp108_nvk_bo_map(mem->bo);
   if (!ptr)
      return vk_error(log_obj, VK_ERROR_MEMORY_MAP_FAILED);

   *map_out = ptr;
   return VK_SUCCESS;
}

static void
nvkmd_macos_mem_unmap(struct nvkmd_mem *_mem,
                      enum nvkmd_mem_map_flags flags,
                      void *map)
{
   /* Persistent kernel/BAR1 mapping lives with gp108_nvk_bo */
   (void)_mem; (void)flags; (void)map;
}

static VkResult
nvkmd_macos_mem_overmap(struct nvkmd_mem *_mem,
                        struct vk_object_base *log_obj,
                        enum nvkmd_mem_map_flags flags,
                        void *map)
{
   (void)_mem; (void)flags; (void)map;
   return vk_error(log_obj, VK_ERROR_MEMORY_MAP_FAILED);
}

static VkResult
nvkmd_macos_mem_export_dma_buf(struct nvkmd_mem *_mem,
                               struct vk_object_base *log_obj,
                               int *fd_out)
{
   (void)_mem; (void)fd_out;
   return vk_error(log_obj, VK_ERROR_INVALID_EXTERNAL_HANDLE);
}

static uint32_t
nvkmd_macos_mem_log_handle(struct nvkmd_mem *_mem)
{
   struct nvkmd_macos_mem *mem = nvkmd_macos_mem(_mem);
   return gp108_nvk_bo_handle_id(mem->bo);
}

const struct nvkmd_mem_ops nvkmd_macos_mem_ops = {
   .free           = nvkmd_macos_mem_free,
   .map            = nvkmd_macos_mem_map,
   .unmap          = nvkmd_macos_mem_unmap,
   .overmap        = nvkmd_macos_mem_overmap,
   .export_dma_buf = nvkmd_macos_mem_export_dma_buf,
   .log_handle     = nvkmd_macos_mem_log_handle,
};

static VkResult
nvkmd_macos_alloc_tiled_mem(struct nvkmd_dev *_dev,
                            struct vk_object_base *log_obj,
                            uint64_t size_B, uint64_t align_B,
                            uint8_t pte_kind, uint16_t tile_mode,
                            enum nvkmd_mem_flags flags,
                            struct nvkmd_mem **mem_out)
{
   (void)tile_mode;
   struct nvkmd_macos_dev *dev = nvkmd_macos_dev(_dev);

   align_B = MAX2(align_B, dev->base.pdev->bind_align_B);
   size_B  = align64(size_B, align_B);

   bool in_vram = (flags & (NVKMD_MEM_VRAM | NVKMD_MEM_LOCAL)) != 0;
   if (dev->base.pdev->debug_flags & NVK_DEBUG_FORCE_GART)
      in_vram = false;
   bool mappable = (flags & NVKMD_MEM_CAN_MAP) != 0;
   flags |= NVKMD_MEM_COHERENT;

   struct gp108_nvk_bo *bo = NULL;
   int err = gp108_nvk_bo_alloc(size_B, pte_kind, in_vram, mappable, &bo);
   if (err != 0 || !bo)
      return vk_error(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   struct nvkmd_macos_mem *mem = CALLOC_STRUCT(nvkmd_macos_mem);
   if (!mem) {
      gp108_nvk_bo_free(bo);
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   nvkmd_mem_init(&dev->base, &mem->base, &nvkmd_macos_mem_ops,
                  flags, size_B, align_B);
   mem->bo = bo;

   enum nvkmd_va_flags va_flags = in_vram ? 0 : NVKMD_VA_GART;
   VkResult result = nvkmd_dev_alloc_va(&dev->base, log_obj,
                                        va_flags, pte_kind,
                                        size_B, align_B, 0, &mem->base.va);
   if (result != VK_SUCCESS) {
      gp108_nvk_bo_free(bo);
      FREE(mem);
      return result;
   }

   result = nvkmd_va_bind_mem(mem->base.va, log_obj, 0, &mem->base, 0, size_B);
   if (result != VK_SUCCESS) {
      nvkmd_va_free(mem->base.va);
      gp108_nvk_bo_free(bo);
      FREE(mem);
      return result;
   }

   *mem_out = &mem->base;
   return VK_SUCCESS;
}

static VkResult
nvkmd_macos_alloc_mem(struct nvkmd_dev *dev,
                      struct vk_object_base *log_obj,
                      uint64_t size_B, uint64_t align_B,
                      enum nvkmd_mem_flags flags,
                      struct nvkmd_mem **mem_out)
{
   return nvkmd_macos_alloc_tiled_mem(dev, log_obj, size_B, align_B,
                                      0, 0, flags, mem_out);
}

static VkResult
nvkmd_macos_import_dma_buf(struct nvkmd_dev *dev,
                           struct vk_object_base *log_obj,
                           int fd, struct nvkmd_mem **mem_out)
{
   (void)dev; (void)fd; (void)mem_out;
   return vk_error(log_obj, VK_ERROR_INVALID_EXTERNAL_HANDLE);
}

/* ========================================================================= *
 * 5. Execution & Bind Contexts (nvkmd_ctx)                                  *
 * ========================================================================= */
static void *
macos_exec_worker_main(void *arg)
{
   struct nvkmd_macos_exec_ctx *ctx = arg;

   for (;;) {
      pthread_mutex_lock(&ctx->worker_mtx);
      while (ctx->job_count == 0 && !ctx->worker_stop)
         pthread_cond_wait(&ctx->worker_cnd, &ctx->worker_mtx);

      if (ctx->job_count == 0 && ctx->worker_stop) {
         pthread_mutex_unlock(&ctx->worker_mtx);
         break;
      }

      struct nvkmd_macos_fence_job job = ctx->jobs[ctx->job_head];
      ctx->job_head = (ctx->job_head + 1U) % NVKMD_MACOS_RING_SLOTS;
      ctx->job_count--;
      pthread_cond_signal(&ctx->worker_cnd);
      pthread_mutex_unlock(&ctx->worker_mtx);

      gp108_nvk_ctx_wait_fence(ctx->hw_ctx, job.gpu_seq, UINT64_MAX);
      for (uint32_t i = 0; i < job.sig_count; i++)
         macos_timeline_signal_raw(job.sigs[i].tl, job.sigs[i].val);
      free(job.sigs);
   }
   return NULL;
}

static VkResult
nvkmd_macos_exec_ctx_flush(struct nvkmd_ctx *_ctx,
                           struct vk_object_base *log_obj)
{
   struct nvkmd_macos_exec_ctx *ctx = nvkmd_macos_exec_ctx(_ctx);

   /* 1. Wait on any pending input syncs */
   for (uint32_t i = 0; i < ctx->wait_count; i++) {
      VkResult wr = macos_timeline_wait_raw(ctx->waits[i].tl,
                                            ctx->waits[i].val,
                                            OS_TIMEOUT_INFINITE);
      if (wr != VK_SUCCESS)
         return wr;
   }
   ctx->wait_count = 0;

   /* 2. Submit indirect pushbuffers to MX250 GPFIFO */
   uint32_t push_count =
      util_dynarray_num_elements(&ctx->pushes, struct gp108_nvk_push);
   uint64_t gpu_seq = 0;

   if (push_count > 0) {
      const struct gp108_nvk_push *p = util_dynarray_begin(&ctx->pushes);
      int err = gp108_nvk_ctx_submit(ctx->hw_ctx, p, push_count, &gpu_seq);
      util_dynarray_clear(&ctx->pushes);
      if (err != 0)
         return vk_errorf(log_obj, VK_ERROR_DEVICE_LOST,
                          "gp108_nvk_ctx_submit failed: %d", err);
   }

   /* 3. Dispatch output signals either immediately (no GPU work) or via worker */
   if (ctx->sig_count > 0) {
      if (gpu_seq == 0) {
         for (uint32_t i = 0; i < ctx->sig_count; i++)
            macos_timeline_signal_raw(ctx->sigs[i].tl, ctx->sigs[i].val);
      } else {
         struct nvkmd_macos_sync_item *copy =
            malloc(ctx->sig_count * sizeof(*copy));
         if (!copy) {
            gp108_nvk_ctx_wait_fence(ctx->hw_ctx, gpu_seq, UINT64_MAX);
            for (uint32_t i = 0; i < ctx->sig_count; i++)
               macos_timeline_signal_raw(ctx->sigs[i].tl, ctx->sigs[i].val);
         } else {
            memcpy(copy, ctx->sigs, ctx->sig_count * sizeof(*copy));
            pthread_mutex_lock(&ctx->worker_mtx);
            while (ctx->job_count == NVKMD_MACOS_RING_SLOTS)
               pthread_cond_wait(&ctx->worker_cnd, &ctx->worker_mtx);
            ctx->jobs[ctx->job_tail] = (struct nvkmd_macos_fence_job) {
               .gpu_seq   = gpu_seq,
               .sig_count = ctx->sig_count,
               .sigs      = copy,
            };
            ctx->job_tail = (ctx->job_tail + 1U) % NVKMD_MACOS_RING_SLOTS;
            ctx->job_count++;
            pthread_cond_signal(&ctx->worker_cnd);
            pthread_mutex_unlock(&ctx->worker_mtx);
         }
      }
      ctx->sig_count = 0;
   }

   return VK_SUCCESS;
}

static VkResult
nvkmd_macos_exec_ctx_wait(struct nvkmd_ctx *_ctx,
                          struct vk_object_base *log_obj,
                          uint32_t wait_count,
                          const struct vk_sync_wait *waits)
{
   struct nvkmd_macos_exec_ctx *ctx = nvkmd_macos_exec_ctx(_ctx);
   for (uint32_t i = 0; i < wait_count; i++) {
      if (ctx->wait_count >= NVKMD_MACOS_MAX_SYNCS) {
         VkResult r = nvkmd_macos_exec_ctx_flush(_ctx, log_obj);
         if (r != VK_SUCCESS)
            return r;
      }
      unwrap_vk_sync(waits[i].sync, waits[i].wait_value,
                     &ctx->waits[ctx->wait_count].tl,
                     &ctx->waits[ctx->wait_count].val);
      ctx->wait_count++;
   }
   return VK_SUCCESS;
}

static VkResult
nvkmd_macos_exec_ctx_exec(struct nvkmd_ctx *_ctx,
                          struct vk_object_base *log_obj,
                          uint32_t exec_count,
                          const struct nvkmd_ctx_exec *execs)
{
   (void)log_obj;
   struct nvkmd_macos_exec_ctx *ctx = nvkmd_macos_exec_ctx(_ctx);
   for (uint32_t i = 0; i < exec_count; i++) {
      struct gp108_nvk_push p = {
         .addr        = execs[i].addr,
         .size_B      = execs[i].size_B,
         .no_prefetch = execs[i].no_prefetch,
      };
      util_dynarray_append(&ctx->pushes, p);
   }
   return VK_SUCCESS;
}

static VkResult
nvkmd_macos_exec_ctx_signal(struct nvkmd_ctx *_ctx,
                            struct vk_object_base *log_obj,
                            uint32_t signal_count,
                            const struct vk_sync_signal *signals)
{
   struct nvkmd_macos_exec_ctx *ctx = nvkmd_macos_exec_ctx(_ctx);
   for (uint32_t i = 0; i < signal_count; i++) {
      if (ctx->sig_count >= NVKMD_MACOS_MAX_SYNCS) {
         VkResult r = nvkmd_macos_exec_ctx_flush(_ctx, log_obj);
         if (r != VK_SUCCESS)
            return r;
      }
      unwrap_vk_sync(signals[i].sync, signals[i].signal_value,
                     &ctx->sigs[ctx->sig_count].tl,
                     &ctx->sigs[ctx->sig_count].val);
      ctx->sig_count++;
   }
   return nvkmd_macos_exec_ctx_flush(_ctx, log_obj);
}

static VkResult
nvkmd_macos_exec_ctx_sync(struct nvkmd_ctx *_ctx,
                          struct vk_object_base *log_obj)
{
   struct nvkmd_macos_exec_ctx *ctx = nvkmd_macos_exec_ctx(_ctx);
   VkResult r = nvkmd_macos_exec_ctx_flush(_ctx, log_obj);
   if (r != VK_SUCCESS)
      return r;

   uint64_t target = 0;
   gp108_nvk_ctx_submit(ctx->hw_ctx, NULL, 0, &target);
   if (gp108_nvk_ctx_wait_fence(ctx->hw_ctx, target, 5000000000ULL) != 0)
      return vk_error(log_obj, VK_ERROR_DEVICE_LOST);
   return VK_SUCCESS;
}

static void
nvkmd_macos_exec_ctx_destroy(struct nvkmd_ctx *_ctx)
{
   struct nvkmd_macos_exec_ctx *ctx = nvkmd_macos_exec_ctx(_ctx);

   pthread_mutex_lock(&ctx->worker_mtx);
   ctx->worker_stop = true;
   pthread_cond_signal(&ctx->worker_cnd);
   pthread_mutex_unlock(&ctx->worker_mtx);
   pthread_join(ctx->worker_tid, NULL);

   pthread_cond_destroy(&ctx->worker_cnd);
   pthread_mutex_destroy(&ctx->worker_mtx);
   util_dynarray_fini(&ctx->pushes);
   gp108_nvk_ctx_destroy(ctx->hw_ctx);
   FREE(ctx);
}

const struct nvkmd_ctx_ops nvkmd_macos_exec_ctx_ops = {
   .destroy = nvkmd_macos_exec_ctx_destroy,
   .wait    = nvkmd_macos_exec_ctx_wait,
   .exec    = nvkmd_macos_exec_ctx_exec,
   .signal  = nvkmd_macos_exec_ctx_signal,
   .flush   = nvkmd_macos_exec_ctx_flush,
   .sync    = nvkmd_macos_exec_ctx_sync,
};

/* Bind context (VM_BIND queue) */
static void
nvkmd_macos_bind_ctx_destroy(struct nvkmd_ctx *_ctx)
{
   struct nvkmd_macos_bind_ctx *ctx = nvkmd_macos_bind_ctx(_ctx);
   FREE(ctx);
}

static VkResult
nvkmd_macos_bind_ctx_flush(struct nvkmd_ctx *_ctx,
                           struct vk_object_base *log_obj)
{
   (void)log_obj;
   struct nvkmd_macos_bind_ctx *ctx = nvkmd_macos_bind_ctx(_ctx);
   for (uint32_t i = 0; i < ctx->wait_count; i++) {
      VkResult wr = macos_timeline_wait_raw(ctx->waits[i].tl,
                                            ctx->waits[i].val,
                                            OS_TIMEOUT_INFINITE);
      if (wr != VK_SUCCESS)
         return wr;
   }
   ctx->wait_count = 0;
   return VK_SUCCESS;
}

static VkResult
nvkmd_macos_bind_ctx_wait(struct nvkmd_ctx *_ctx,
                          struct vk_object_base *log_obj,
                          uint32_t wait_count,
                          const struct vk_sync_wait *waits)
{
   struct nvkmd_macos_bind_ctx *ctx = nvkmd_macos_bind_ctx(_ctx);
   for (uint32_t i = 0; i < wait_count; i++) {
      if (ctx->wait_count >= NVKMD_MACOS_MAX_SYNCS) {
         VkResult r = nvkmd_macos_bind_ctx_flush(_ctx, log_obj);
         if (r != VK_SUCCESS)
            return r;
      }
      unwrap_vk_sync(waits[i].sync, waits[i].wait_value,
                     &ctx->waits[ctx->wait_count].tl,
                     &ctx->waits[ctx->wait_count].val);
      ctx->wait_count++;
   }
   return VK_SUCCESS;
}

static VkResult
nvkmd_macos_bind_ctx_bind(struct nvkmd_ctx *_ctx,
                          struct vk_object_base *log_obj,
                          uint32_t bind_count,
                          const struct nvkmd_ctx_bind *binds)
{
   VkResult r = nvkmd_macos_bind_ctx_flush(_ctx, log_obj);
   if (r != VK_SUCCESS)
      return r;

   for (uint32_t i = 0; i < bind_count; i++) {
      if (binds[i].op == NVKMD_BIND_OP_BIND) {
         r = nvkmd_macos_va_bind_mem(binds[i].va, log_obj,
                                     binds[i].va_offset_B,
                                     binds[i].mem, binds[i].mem_offset_B,
                                     binds[i].range_B);
      } else {
         r = nvkmd_macos_va_unbind(binds[i].va, log_obj,
                                   binds[i].va_offset_B,
                                   binds[i].range_B);
      }
      if (r != VK_SUCCESS)
         return r;
   }
   return VK_SUCCESS;
}

static VkResult
nvkmd_macos_bind_ctx_signal(struct nvkmd_ctx *_ctx,
                            struct vk_object_base *log_obj,
                            uint32_t signal_count,
                            const struct vk_sync_signal *signals)
{
   VkResult r = nvkmd_macos_bind_ctx_flush(_ctx, log_obj);
   if (r != VK_SUCCESS)
      return r;

   for (uint32_t i = 0; i < signal_count; i++) {
      struct nvkmd_macos_timeline *tl = NULL;
      uint64_t val = 0;
      unwrap_vk_sync(signals[i].sync, signals[i].signal_value, &tl, &val);
      macos_timeline_signal_raw(tl, val);
   }
   return VK_SUCCESS;
}

const struct nvkmd_ctx_ops nvkmd_macos_bind_ctx_ops = {
   .destroy = nvkmd_macos_bind_ctx_destroy,
   .wait    = nvkmd_macos_bind_ctx_wait,
   .bind    = nvkmd_macos_bind_ctx_bind,
   .signal  = nvkmd_macos_bind_ctx_signal,
   .flush   = nvkmd_macos_bind_ctx_flush,
   .sync    = nvkmd_macos_bind_ctx_flush,
};

static VkResult
nvkmd_macos_create_ctx(struct nvkmd_dev *_dev,
                       struct vk_object_base *log_obj,
                       enum nvkmd_engines engines,
                       struct nvkmd_ctx **ctx_out)
{
   if (engines == NVKMD_ENGINE_BIND) {
      struct nvkmd_macos_bind_ctx *bctx = CALLOC_STRUCT(nvkmd_macos_bind_ctx);
      if (!bctx)
         return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
      bctx->base.ops = &nvkmd_macos_bind_ctx_ops;
      bctx->base.dev = _dev;
      *ctx_out = &bctx->base;
      return VK_SUCCESS;
   }

   struct nvkmd_macos_exec_ctx *ctx = CALLOC_STRUCT(nvkmd_macos_exec_ctx);
   if (!ctx)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   if (gp108_nvk_ctx_create((uint32_t)engines, &ctx->hw_ctx) != 0) {
      FREE(ctx);
      return vk_error(log_obj, VK_ERROR_INITIALIZATION_FAILED);
   }

   ctx->base.ops = &nvkmd_macos_exec_ctx_ops;
   ctx->base.dev = _dev;
   util_dynarray_init(&ctx->pushes, NULL);
   pthread_mutex_init(&ctx->worker_mtx, NULL);
   pthread_cond_init(&ctx->worker_cnd, NULL);
   pthread_create(&ctx->worker_tid, NULL, macos_exec_worker_main, ctx);

   *ctx_out = &ctx->base;
   return VK_SUCCESS;
}

/* ========================================================================= *
 * 6. Logical & Physical Device Creation (nvkmd_dev & nvkmd_pdev)            *
 * ========================================================================= */
static void
nvkmd_macos_dev_destroy(struct nvkmd_dev *_dev)
{
   struct nvkmd_macos_dev *dev = nvkmd_macos_dev(_dev);
   util_vma_heap_finish(&dev->heap);
   util_vma_heap_finish(&dev->replay_heap);
   simple_mtx_destroy(&dev->heap_mutex);
   gp108_nvk_release();
   FREE(dev);
}

static uint64_t
nvkmd_macos_dev_get_gpu_timestamp(struct nvkmd_dev *_dev)
{
   (void)_dev;
   return gp108_nvk_get_timestamp();
}

const struct nvkmd_dev_ops nvkmd_macos_dev_ops = {
   .destroy           = nvkmd_macos_dev_destroy,
   .get_gpu_timestamp = nvkmd_macos_dev_get_gpu_timestamp,
   .get_drm_fd        = NULL,
   .alloc_mem         = nvkmd_macos_alloc_mem,
   .alloc_tiled_mem   = nvkmd_macos_alloc_tiled_mem,
   .import_dma_buf    = nvkmd_macos_import_dma_buf,
   .alloc_va          = nvkmd_macos_alloc_va,
   .create_ctx        = nvkmd_macos_create_ctx,
};

static VkResult
nvkmd_macos_create_dev(struct nvkmd_pdev *_pdev,
                       struct vk_object_base *log_obj,
                       struct nvkmd_dev **dev_out)
{
   if (gp108_nvk_retain(NULL) != 0)
      return vk_error(log_obj, VK_ERROR_INITIALIZATION_FAILED);

   struct nvkmd_macos_dev *dev = CALLOC_STRUCT(nvkmd_macos_dev);
   if (!dev) {
      gp108_nvk_release();
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   dev->base.ops      = &nvkmd_macos_dev_ops;
   dev->base.pdev     = _pdev;
   dev->base.va_start = NVKMD_MACOS_HEAP_START;
   dev->base.va_end   = NVKMD_MACOS_REPLAY_HEAP_END;

   list_inithead(&dev->base.mems);
   simple_mtx_init(&dev->base.mems_mutex, mtx_plain);
   simple_mtx_init(&dev->heap_mutex, mtx_plain);

   util_vma_heap_init(&dev->heap, NVKMD_MACOS_HEAP_START,
                      NVKMD_MACOS_HEAP_END - NVKMD_MACOS_HEAP_START);
   util_vma_heap_init(&dev->replay_heap, NVKMD_MACOS_REPLAY_HEAP_START,
                      NVKMD_MACOS_REPLAY_HEAP_END - NVKMD_MACOS_REPLAY_HEAP_START);

   *dev_out = &dev->base;
   return VK_SUCCESS;
}

static void
nvkmd_macos_pdev_destroy(struct nvkmd_pdev *_pdev)
{
   struct nvkmd_macos_pdev *pdev = nvkmd_macos_pdev(_pdev);
   gp108_nvk_release();
   FREE(pdev);
}

static uint64_t
nvkmd_macos_pdev_get_vram_used(struct nvkmd_pdev *_pdev)
{
   (void)_pdev;
   return gp108_nvk_get_vram_used();
}

const struct nvkmd_pdev_ops nvkmd_macos_pdev_ops = {
   .destroy            = nvkmd_macos_pdev_destroy,
   .get_vram_used      = nvkmd_macos_pdev_get_vram_used,
   .get_drm_primary_fd = NULL,
   .create_dev         = nvkmd_macos_create_dev,
};

VkResult
nvkmd_macos_try_create_pdev(struct vk_object_base *log_obj,
                            enum nvk_debug debug_flags,
                            struct nvkmd_pdev **pdev_out)
{
   struct gp108_nvk_hw_info hw = {};
   if (gp108_nvk_retain(&hw) != 0)
      return VK_ERROR_INCOMPATIBLE_DRIVER;

   struct nvkmd_macos_pdev *pdev = CALLOC_STRUCT(nvkmd_macos_pdev);
   if (!pdev) {
      gp108_nvk_release();
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   pdev->base.ops         = &nvkmd_macos_pdev_ops;
   pdev->base.debug_flags = debug_flags;

   struct nv_device_info *info = &pdev->base.dev_info;
   memset(info, 0, sizeof(*info));
   info->type       = NV_DEVICE_TYPE_DIS;
   info->device_id  = hw.device_id;
   info->chipset    = hw.chipset;
   strncpy(info->device_name, hw.dev_name, sizeof(info->device_name) - 1);
   strncpy(info->chipset_name, hw.chip_name, sizeof(info->chipset_name) - 1);

   info->pci.domain      = 0;
   info->pci.bus         = hw.pci_bus;
   info->pci.dev         = hw.pci_dev;
   info->pci.func        = hw.pci_func;
   info->pci.revision_id = hw.revision_id;

   info->sm                = 61; /* GP108 = Pascal SM 6.1 */
   info->gpc_count         = hw.gpc_count;
   info->tpc_count         = hw.tpc_count;
   info->mp_per_tpc        = 1;
   info->max_warps_per_mp  = 64;
   info->max_blocks_per_mp = 32;

   info->has_transfer_queue = false;
   info->has_video          = false;
   info->nc_atom_size_B     = util_cache_granularity();

   info->cls_copy    = hw.cls_copy;
   info->cls_eng2d   = hw.cls_eng2d;
   info->cls_eng3d   = hw.cls_eng3d;
   info->cls_m2mf    = hw.cls_m2mf;
   info->cls_compute = hw.cls_compute;
   info->cls_gpfifo  = 0xc06f; /* PASCAL_CHANNEL_GPFIFO_A */
   info->cls_vdec    = 0;

   info->vram_size_B = hw.vram_usable_B;
   info->bar_size_B  = hw.bar1_size_B;

   info->max_smem_per_wg_kB   = 48;
   info->sm_smem_sizes_kB[0]  = 96;
   info->sm_smem_size_count   = 1;
   info->has_zcull_info       = false;

   pdev->base.kmd_info = (struct nvkmd_info) {
      .has_dma_buf       = false,
      .has_get_vram_used = true,
      .has_alloc_tiled   = true,
      .has_map_fixed     = false,
      .has_overmap       = false,
      .has_compression   = false,
   };

   pdev->base.bind_align_B = 4096U;

   pdev->sync_binary_type = vk_sync_binary_get_type(&nvkmd_macos_timeline_sync_type);
   pdev->sync_types[0]    = &nvkmd_macos_timeline_sync_type;
   pdev->sync_types[1]    = &pdev->sync_binary_type.sync;
   pdev->sync_types[2]    = NULL;
   pdev->base.sync_types  = pdev->sync_types;

   *pdev_out = &pdev->base;
   return VK_SUCCESS;
}
