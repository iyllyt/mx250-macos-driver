/* SPDX-License-Identifier: MIT
 * Clean Standard-C Bridge from Mesa NVK (nvkmd_macos) -> libnvkm_gp108.a
 * Avoids any Linux kernel macro collisions with Mesa headers.
 */
#ifndef GP108_NVK_BRIDGE_H
#define GP108_NVK_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gp108_nvk_bo;
struct gp108_nvk_ctx;

struct gp108_nvk_hw_info {
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subvendor_id;
    uint16_t subdevice_id;
    uint16_t chipset;
    uint8_t  revision_id;
    uint8_t  pci_bus;
    uint8_t  pci_dev;
    uint8_t  pci_func;

    uint8_t  gpc_count;
    uint8_t  tpc_count;

    uint64_t vram_total_B;
    uint64_t vram_usable_B;
    uint64_t bar1_size_B;

    uint16_t cls_copy;
    uint16_t cls_eng2d;
    uint16_t cls_eng3d;
    uint16_t cls_m2mf;
    uint16_t cls_compute;

    char     chip_name[16];
    char     dev_name[64];
};

struct gp108_nvk_push {
    uint64_t addr;
    uint32_t size_B;
    bool     no_prefetch;
};

/* Engine flags matching enum nvkmd_engines */
#define GP108_NVK_ENGINE_COPY    (1U << 0)
#define GP108_NVK_ENGINE_2D      (1U << 1)
#define GP108_NVK_ENGINE_3D      (1U << 2)
#define GP108_NVK_ENGINE_M2MF    (1U << 3)
#define GP108_NVK_ENGINE_COMPUTE (1U << 4)
#define GP108_NVK_ENGINE_BIND    (1U << 5)

/* 1. Singleton Hardware Lifecycle & Queries */
int      gp108_nvk_retain(struct gp108_nvk_hw_info *out_info);
void     gp108_nvk_release(void);
uint64_t gp108_nvk_get_timestamp(void);
uint64_t gp108_nvk_get_vram_used(void);

/* 2. Memory Object (BO) Allocation & Direct CPU Mapping */
int      gp108_nvk_bo_alloc(uint64_t size_B, uint8_t pte_kind,
                            bool in_vram, bool mappable,
                            struct gp108_nvk_bo **out_bo);
void     gp108_nvk_bo_free(struct gp108_nvk_bo *bo);
void    *gp108_nvk_bo_map(struct gp108_nvk_bo *bo);
uint32_t gp108_nvk_bo_handle_id(struct gp108_nvk_bo *bo);
uint64_t gp108_nvk_bo_size(struct gp108_nvk_bo *bo);

/* 3. GPU Virtual Address Space (GP100 RAW VMM) */
int      gp108_nvk_vm_sparse(uint64_t addr, uint64_t size_B, bool enable);
int      gp108_nvk_vm_get_ptes(uint64_t addr, uint64_t size_B);
int      gp108_nvk_vm_put_ptes(uint64_t addr, uint64_t size_B);
int      gp108_nvk_vm_map(uint64_t addr, uint64_t range_B, uint8_t pte_kind,
                          struct gp108_nvk_bo *bo, uint64_t bo_offset_B);
int      gp108_nvk_vm_unmap(uint64_t addr, uint64_t range_B, bool is_sparse);

/* 4. Hardware GPFIFO Execution Context & GPU Fence Completion */
int      gp108_nvk_ctx_create(uint32_t engine_mask, struct gp108_nvk_ctx **out_ctx);
void     gp108_nvk_ctx_destroy(struct gp108_nvk_ctx *ctx);
int      gp108_nvk_ctx_submit(struct gp108_nvk_ctx *ctx,
                              const struct gp108_nvk_push *pushes,
                              uint32_t push_count,
                              uint64_t *out_fence_seq);
uint64_t gp108_nvk_ctx_completed_fence(struct gp108_nvk_ctx *ctx);
int      gp108_nvk_ctx_wait_fence(struct gp108_nvk_ctx *ctx,
                                  uint64_t target_seq,
                                  uint64_t timeout_ns);

#ifdef __cplusplus
}
#endif

#endif /* GP108_NVK_BRIDGE_H */
