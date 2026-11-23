/* SPDX-License-Identifier: MIT
 * IPC Protocol between gp108-rmd (Userspace NVIDIA RM Host) and Mesa NVK / Metal Clients
 */
#ifndef GP108_RMD_IPC_H
#define GP108_RMD_IPC_H

#include <stdbool.h>
#include <stdint.h>
#include "../nvkm_macos/gp108_nvk_bridge.h"

#define GP108_RMD_SOCK_PATH "/tmp/gp108_rmd.sock"
#define GP108_RMD_MAGIC     0x31303852U /* '108R' */
#define GP108_RMD_VERSION   1U

/* Layout of 128 KiB per-channel shared memory segment (passed via SCM_RIGHTS) */
#define GP108_CHAN_SHM_PUSH_OFF   0x00000U /* 64 KiB internal semaphore push ring */
#define GP108_CHAN_SHM_PUSH_LEN   0x10000U
#define GP108_CHAN_SHM_GPFIFO_OFF 0x10000U /* 32 KiB GPFIFO (4096 IB entries) */
#define GP108_CHAN_SHM_GPFIFO_LEN 0x08000U
#define GP108_CHAN_SHM_SEMA_OFF   0x18000U /* 4 KiB 64-bit fence page */
#define GP108_CHAN_SHM_SEMA_LEN   0x01000U
#define GP108_CHAN_SHM_NOTIF_OFF  0x19000U /* 4 KiB error notifier page */
#define GP108_CHAN_SHM_NOTIF_LEN  0x01000U
#define GP108_CHAN_SHM_TOTAL_SIZE 0x20000U /* 128 KiB */

enum gp108_rmd_op {
    RMD_OP_HELLO               = 1,
    RMD_OP_GET_TIMESTAMP       = 2,
    RMD_OP_BO_ALLOC            = 3,
    RMD_OP_BO_IMPORT_IOSURFACE = 4,
    RMD_OP_BO_FREE             = 5,
    RMD_OP_VM_MAP              = 6,
    RMD_OP_VM_UNMAP            = 7,
    RMD_OP_CTX_CREATE          = 8,
    RMD_OP_CTX_DESTROY         = 9,
    RMD_OP_CTX_KICK            = 10,
};

struct gp108_rmd_req {
    uint32_t magic;
    uint32_t op;
    union {
        struct {
            uint32_t client_pid;
        } hello;
        struct {
            uint64_t size_B;
            uint8_t  pte_kind;
            uint8_t  in_vram;
            uint8_t  mappable;
            uint8_t  pad;
        } bo_alloc;
        struct {
            uint32_t iosurface_id;
            uint64_t size_B;
        } bo_iosurf;
        struct {
            uint32_t bo_id;
        } bo_free;
        struct {
            uint64_t addr;
            uint64_t range_B;
            uint64_t bo_offset_B;
            uint32_t bo_id;
            uint8_t  pte_kind;
            uint8_t  pad[3];
        } vm_map;
        struct {
            uint64_t addr;
            uint64_t range_B;
        } vm_unmap;
        struct {
            uint32_t engine_mask;
        } ctx_create;
        struct {
            uint32_t ctx_id;
        } ctx_destroy;
        struct {
            uint32_t ctx_id;
            uint32_t gp_put;
        } ctx_kick;
    } u;
};

struct gp108_rmd_resp {
    int32_t  status; /* 0 = success, negative errno on error */
    uint32_t has_fd; /* 1 if an anonymous POSIX shm fd is attached via SCM_RIGHTS */
    union {
        struct {
            struct gp108_nvk_hw_info hw;
        } hello;
        struct {
            uint64_t timestamp_ns;
        } ts;
        struct {
            uint32_t bo_id;
            uint32_t is_vram;
            uint64_t actual_size_B;
        } bo;
        struct {
            uint32_t ctx_id;
            uint32_t gpfifo_entries;
            uint64_t shm_gpu_va;
        } ctx;
        struct {
            uint32_t gp_get;
        } kick;
    } u;
};

#endif /* GP108_RMD_IPC_H */
