/* SPDX-License-Identifier: MIT
 * Userspace ELF64 Resource Manager Loader & Host API for GP108 (MX250) on macOS
 */
#ifndef GP108_RM_HOST_H
#define GP108_RM_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NVRM 1
#include "nvtypes.h"
#include "nvstatus.h"
#include "nv.h"
#include "os-interface.h"

#ifdef __cplusplus
extern "C" {
#endif

struct gp108_rm_exports {
    NvBool      (*rm_init_rm)(nvidia_stack_t *);
    void        (*rm_shutdown_rm)(nvidia_stack_t *);
    NvBool      (*rm_init_private_state)(nvidia_stack_t *, nv_state_t *);
    void        (*rm_free_private_state)(nvidia_stack_t *, nv_state_t *);
    NvBool      (*rm_init_adapter)(nvidia_stack_t *, nv_state_t *);
    void        (*rm_disable_adapter)(nvidia_stack_t *, nv_state_t *);
    void        (*rm_shutdown_adapter)(nvidia_stack_t *, nv_state_t *);
    NV_STATUS   (*rm_is_supported_device)(nvidia_stack_t *, nv_state_t *);
    NvBool      (*rm_is_supported_pci_device)(NvU8, NvU8, NvU16, NvU16, NvU16, NvU16, NvBool);
    const char *(*rm_get_device_name)(NvU16, NvU16, NvU16);
    void        (*rm_get_vbios_version)(nvidia_stack_t *, nv_state_t *, char *);
    char       *(*rm_get_gpu_uuid)(nvidia_stack_t *, nv_state_t *);
    NV_STATUS   (*rm_ioctl)(nvidia_stack_t *, nv_state_t *, nv_file_private_t *, NvU32, void *, NvU32);
    NvBool      (*rm_isr)(nvidia_stack_t *, nv_state_t *, NvU32 *);
    void        (*rm_isr_bh)(nvidia_stack_t *, nv_state_t *);
    NV_STATUS   (*rm_gpu_copy_mmu_faults)(nvidia_stack_t *, nv_state_t *, NvU32 *);
    NV_STATUS   (*rm_run_rc_callback)(nvidia_stack_t *, nv_state_t *);
    void        (*rm_execute_work_item)(nvidia_stack_t *, void *);
    NvBool      (*rm_init_event_locks)(nvidia_stack_t *, nv_state_t *);
    void        (*rm_destroy_event_locks)(nvidia_stack_t *, nv_state_t *);
    void        (*rm_cleanup_file_private)(nvidia_stack_t *, nv_state_t *, nv_file_private_t *);
    NV_STATUS   (*rm_read_registry_dword)(nvidia_stack_t *, nv_state_t *, const char *, NvU32 *);
    NV_STATUS   (*rm_write_registry_dword)(nvidia_stack_t *, nv_state_t *, const char *, NvU32);
    NV_STATUS   (*rm_write_registry_string)(nvidia_stack_t *, nv_state_t *, const char *, const char *, NvU32);
    NV_STATUS   (*rm_write_registry_binary)(nvidia_stack_t *, nv_state_t *, const char *, NvU8 *, NvU32);
    NV_STATUS   (*rm_perform_version_check)(nvidia_stack_t *, void *, NvU32);
};

struct gp108_ext_sym {
    const char *name;
    void       *addr;
    bool        is_data;
    size_t      data_size;
};

/* Provided by macos_nv_os.c */
const struct gp108_ext_sym *gp108_os_lookup_symbol(const char *name);

/* Allocate a generous 64 KiB RM alt-stack compatible with nvidia_stack_t */
nvidia_stack_t *gp108_alloc_rm_stack(void);
void            gp108_free_rm_stack(nvidia_stack_t *sp);

/* Load ELF64 nv-kernel.o into low <2 GiB VA space, apply R_X86_64_{64,PC32,32S}
 * relocations, install ring-3 CR4 trap handler, and populate out_exports.
 */
int gp108_elf64_load_rm(const char *elf_path, struct gp108_rm_exports *out_exports);

/* High-level OS + RM bootstrap on GP108Bridge.kext */
int  gp108_rm_bootstrap(const char *elf_path);
void gp108_rm_teardown(void);

/* Direct in-process RM ioctl wrapper (nr = NV_ESC_*, e.g. NV_ESC_RM_ALLOC) */
int  gp108_rm_call_ioctl(nv_file_private_t *nvfp, bool is_ctl, uint32_t nr, void *params, uint32_t size);

/* Retrieve last mmap context recorded on fd (0=ctl, 1=dev) and resolve CPU VA */
int  gp108_rm_get_last_mmap(int fd, uint64_t *out_phys, uint64_t *out_size,
                            int *out_bar_idx, uint64_t *out_bar_off, void **out_cpu_va);

/* Direct BAR0 / BAR1 / BAR3 access */
void *gp108_rm_bar_va(int bar_idx, uint64_t *out_size, uint64_t *out_phys);

/* Access active RM exports and nv_state_t */
extern struct gp108_rm_exports g_rm;
nv_state_t *gp108_rm_get_gpu_state(void);
nv_state_t *gp108_rm_get_ctl_state(void);

#ifdef __cplusplus
}
#endif

#endif /* GP108_RM_HOST_H */
