/* SPDX-License-Identifier: MIT
 * macOS Userspace NVKMD Backend -> libnvkm_gp108.a -> GP108Bridge.kext
 */
#ifndef NVKMD_MACOS_H
#define NVKMD_MACOS_H 1

#include "nvkmd/nvkmd.h"
#include "vk_sync_binary.h"
#include "util/u_dynarray.h"
#include "util/vma.h"

#ifdef __cplusplus
extern "C" {
#endif

VkResult nvkmd_macos_try_create_pdev(struct vk_object_base *log_obj,
                                     enum nvk_debug debug_flags,
                                     struct nvkmd_pdev **pdev_out);

void gp108_vk_set_next_alloc_iosurface_id(uint32_t iosurface_id);
bool gp108_vk_was_last_iosurface_imported(void);

#ifdef __cplusplus
}
#endif

#endif /* NVKMD_MACOS_H */
