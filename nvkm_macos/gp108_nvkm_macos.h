/* SPDX-License-Identifier: MIT
 * Public macOS Userspace Bootstrap & Bridge Interface for GP108 NVKM/NVIF
 */
#ifndef GP108_NVKM_MACOS_H
#define GP108_NVKM_MACOS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../include/GP108BridgeShared.h"

#ifdef __cplusplus
extern "C" {
#endif

struct pci_dev;
struct nvkm_device;
struct nvif_client;
struct nvif_device;

/* Open GP108Bridge.kext UserClient, map BAR0/BAR1/BAR3 & MSI event queue,
 * and synthesize the Linux struct pci_dev for MX250 (10de:1d13).
 */
int gp108_bridge_open(struct pci_dev **out_pdev, struct GP108DeviceInfo *out_info);
void gp108_bridge_close(void);

/* Refresh live GP108DeviceInfo from GP108Bridge.kext */
int gp108_bridge_get_info(struct GP108DeviceInfo *out_info);

/* Direct access to mapped BAR virtual addresses in userspace */
volatile void *gp108_bridge_bar_virt(uint32_t bar_slot, uint64_t *out_size);

/* Pin an arbitrary page-aligned userspace buffer or IOSurface baseAddress
 * for zero-copy 40-bit PCIe DMA and return its physical page array (caller frees out_phys_pages).
 */
int gp108_bridge_pin_usermem(void *user_va, size_t size_bytes,
                             uint32_t *out_pin_slot, uint64_t **out_phys_pages,
                             size_t *out_num_pages);
int gp108_bridge_unpin_usermem(uint32_t pin_slot);

/* Allocate a dedicated kernel-shared DMA buffer slot directly from GP108Bridge
 * (used for large zero-copy BOs or pushbuffers) and map it into userspace.
 */
int gp108_bridge_alloc_dma_pages(size_t size_bytes, uint32_t cache_mode, uint32_t mask_bits,
                                 uint32_t *out_dma_slot, void **out_user_va,
                                 uint64_t **out_phys_pages, size_t *out_num_pages);
int gp108_bridge_free_dma_pages(uint32_t dma_slot, void *user_va, size_t size_bytes);

/* High-level one-call initialization of NVKM + NVIF client + NVIF device for MX250 */
int gp108_nvkm_bootstrap(const char *cfg, const char *dbg,
                         struct nvkm_device **out_nvkm_dev,
                         struct nvif_client *out_client,
                         struct nvif_device *out_device);
void gp108_nvkm_shutdown(struct nvkm_device **inout_nvkm_dev,
                         struct nvif_client *client,
                         struct nvif_device *device);

#ifdef __cplusplus
}
#endif

#endif /* GP108_NVKM_MACOS_H */
