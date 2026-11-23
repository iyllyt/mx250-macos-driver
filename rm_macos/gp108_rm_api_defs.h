/* SPDX-License-Identifier: MIT
 * Self-contained NVIDIA RM Escape, Memory, VASPACE, and Channel ABI Definitions
 * Verified byte-for-byte against NVIDIA 470.256.02 / 515.43.04 SDK headers.
 */
#ifndef GP108_RM_API_DEFS_H
#define GP108_RM_API_DEFS_H

#include "nvtypes.h"
#include "nvlimits.h"
#include "nvmisc.h"
#include "sdk_inc/nv_escape.h"

/* Object Classes */
#define NV01_ROOT_CLIENT_CLASS               0x00000041U
#define NV01_CONTEXT_DMA_CLASS               0x00000002U
#define NV01_DEVICE_0_CLASS                  0x00000080U
#define NV20_SUBDEVICE_0_CLASS               0x00002080U
#define NV01_MEMORY_SYSTEM_CLASS             0x0000003eU
#define NV01_MEMORY_LOCAL_USER_CLASS         0x00000040U
#define NV01_MEMORY_SYSTEM_OS_DESC_CLASS     0x00000071U
#define NV50_MEMORY_VIRTUAL_CLASS            0x000050a0U
#define FERMI_VASPACE_A_CLASS                0x000090f1U
#define KEPLER_CHANNEL_GROUP_A_CLASS         0x0000a06cU
#define PASCAL_CHANNEL_GPFIFO_A_CLASS        0x0000c06fU
#define PASCAL_B_CLASS                       0x0000c197U
#define PASCAL_COMPUTE_B_CLASS               0x0000c1c0U
#define PASCAL_DMA_COPY_B_CLASS              0x0000c1b5U
#define FERMI_TWOD_A_CLASS                   0x0000902dU
#define KEPLER_INLINE_TO_MEMORY_B_CLASS      0x0000a140U

/* Control Command IDs */
#define NV0000_CTRL_GPU_INVALID_ID           0xffffffffU
#define NV0000_CTRL_CMD_GPU_GET_ID_INFO      0x00000202U
#define NV0000_CTRL_CMD_GPU_GET_PROBED_IDS   0x00000214U
#define NV0000_CTRL_CMD_GPU_ATTACH_IDS       0x00000215U
#define NV0000_CTRL_CMD_GPU_DETACH_IDS       0x00000216U

#define NV2080_ENGINE_TYPE_GRAPHICS          0x00000001U
#define NV2080_CTRL_CMD_PERF_BOOST_ID        0x2080200aU
#define NV2080_CTRL_CMD_PERF_GET_CUR_PSTATE  0x20802068U

#define NVA06F_CTRL_CMD_GPFIFO_SCHEDULE      0xa06f0103U
#define NVA06F_CTRL_CMD_BIND                 0xa06f0104U

#define NVOS_MAX_SUBDEVICES                  8

/* NVOS02 (NV_ESC_RM_ALLOC_MEMORY) flags */
#define NVOS02_FLAGS_PHYSICALITY                     7:4
#define NVOS02_FLAGS_PHYSICALITY_CONTIGUOUS          0x00000000U
#define NVOS02_FLAGS_PHYSICALITY_NONCONTIGUOUS       0x00000001U
#define NVOS02_FLAGS_LOCATION                        11:8
#define NVOS02_FLAGS_LOCATION_PCI                    0x00000000U
#define NVOS02_FLAGS_COHERENCY                       15:12
#define NVOS02_FLAGS_COHERENCY_UNCACHED              0x00000000U
#define NVOS02_FLAGS_COHERENCY_CACHED                0x00000001U
#define NVOS02_FLAGS_COHERENCY_WRITE_COMBINE         0x00000002U
#define NVOS02_FLAGS_GPU_CACHEABLE                   18:18
#define NVOS02_FLAGS_GPU_CACHEABLE_NO                0x00000000U
#define NVOS02_FLAGS_GPU_CACHEABLE_YES               0x00000001U
#define NVOS02_FLAGS_MAPPING                         31:30
#define NVOS02_FLAGS_MAPPING_DEFAULT                 0x00000000U
#define NVOS02_FLAGS_MAPPING_NO_MAP                  0x00000001U

/* NVOS03 (NV01_CONTEXT_DMA) flags */
#define NVOS03_FLAGS_MAPPING                         20:20
#define NVOS03_FLAGS_MAPPING_NONE                    0x00000000U
#define NVOS03_FLAGS_MAPPING_KERNEL                  0x00000001U
#define NVOS03_FLAGS_HASH_TABLE                      29:29
#define NVOS03_FLAGS_HASH_TABLE_ENABLE               0x00000000U
#define NVOS03_FLAGS_HASH_TABLE_DISABLE              0x00000001U

/* NVOS32 (NV_MEMORY_ALLOCATION_PARAMS) types, flags & attributes */
#define NVOS32_TYPE_IMAGE                            0U
#define NVOS32_TYPE_DEPTH                            1U
#define NVOS32_TYPE_STENCIL                          11U

#define NVOS32_ALLOC_FLAGS_FIXED_ADDRESS_ALLOCATE    0x00000010U
#define NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE           0x00000100U
#define NVOS32_ALLOC_FLAGS_PERSISTENT_VIDMEM         0x00010000U
#define NVOS32_ALLOC_FLAGS_VIRTUAL                   0x00080000U
#define NVOS32_ALLOC_FLAGS_FORCE_INTERNAL_INDEX      0x00100000U

#define NVOS32_ATTR_DEPTH                            2:0
#define NVOS32_ATTR_DEPTH_8                          0x00000001U
#define NVOS32_ATTR_DEPTH_16                         0x00000002U
#define NVOS32_ATTR_DEPTH_32                         0x00000004U
#define NVOS32_ATTR_DEPTH_64                         0x00000005U
#define NVOS32_ATTR_COMPR                            13:12
#define NVOS32_ATTR_COMPR_NONE                       0x00000000U
#define NVOS32_ATTR_COMPR_ANY                        0x00000001U
#define NVOS32_ATTR_COMPR_REQUIRED                   0x00000002U
#define NVOS32_ATTR_FORMAT                           17:16
#define NVOS32_ATTR_FORMAT_PITCH                     0x00000000U
#define NVOS32_ATTR_FORMAT_BLOCK_LINEAR              0x00000002U
#define NVOS32_ATTR_Z_TYPE                           18:18
#define NVOS32_ATTR_Z_TYPE_FIXED                     0x00000000U
#define NVOS32_ATTR_Z_TYPE_FLOAT                     0x00000001U
#define NVOS32_ATTR_ZS_PACKING                       21:19
#define NVOS32_ATTR_ZS_PACKING_S8                    0x00000000U
#define NVOS32_ATTR_ZS_PACKING_Z24S8                 0x00000000U
#define NVOS32_ATTR_ZS_PACKING_S8Z24                 0x00000001U
#define NVOS32_ATTR_ZS_PACKING_Z32                   0x00000002U
#define NVOS32_ATTR_ZS_PACKING_Z32_X24S8             0x00000005U
#define NVOS32_ATTR_ZS_PACKING_Z16                   0x00000007U
#define NVOS32_ATTR_PAGE_SIZE                        24:23
#define NVOS32_ATTR_PAGE_SIZE_DEFAULT                0x00000000U
#define NVOS32_ATTR_PAGE_SIZE_4KB                    0x00000001U
#define NVOS32_ATTR_PAGE_SIZE_BIG                    0x00000002U
#define NVOS32_ATTR_LOCATION                         26:25
#define NVOS32_ATTR_LOCATION_VIDMEM                  0x00000000U
#define NVOS32_ATTR_LOCATION_PCI                     0x00000001U
#define NVOS32_ATTR_PHYSICALITY                      28:27
#define NVOS32_ATTR_PHYSICALITY_DEFAULT              0x00000000U
#define NVOS32_ATTR_PHYSICALITY_NONCONTIGUOUS        0x00000001U
#define NVOS32_ATTR_PHYSICALITY_CONTIGUOUS           0x00000002U
#define NVOS32_ATTR_PHYSICALITY_ALLOW_NONCONTIGUOUS  0x00000003U
#define NVOS32_ATTR_COHERENCY                        31:29
#define NVOS32_ATTR_COHERENCY_UNCACHED               0x00000000U
#define NVOS32_ATTR_COHERENCY_CACHED                 0x00000001U
#define NVOS32_ATTR_COHERENCY_WRITE_COMBINE          0x00000002U

#define NVOS32_ATTR2_ZBC                             1:0
#define NVOS32_ATTR2_ZBC_DEFAULT                     0x00000000U
#define NVOS32_ATTR2_ZBC_PREFER_ZBC                  0x00000002U
#define NVOS32_ATTR2_GPU_CACHEABLE                   3:2
#define NVOS32_ATTR2_GPU_CACHEABLE_DEFAULT           0x00000000U
#define NVOS32_ATTR2_GPU_CACHEABLE_YES               0x00000001U
#define NVOS32_ATTR2_GPU_CACHEABLE_NO                0x00000002U

/* NVOS46 (NV_ESC_RM_MAP_MEMORY_DMA) flags */
#define NVOS46_FLAGS_ACCESS                          1:0
#define NVOS46_FLAGS_ACCESS_READ_WRITE               0x00000000U
#define NVOS46_FLAGS_PAGE_KIND                       3:3
#define NVOS46_FLAGS_PAGE_KIND_PHYSICAL              0x00000000U
#define NVOS46_FLAGS_PAGE_KIND_VIRTUAL               0x00000001U
#define NVOS46_FLAGS_CACHE_SNOOP                     4:4
#define NVOS46_FLAGS_CACHE_SNOOP_DISABLE             0x00000000U
#define NVOS46_FLAGS_CACHE_SNOOP_ENABLE              0x00000001U
#define NVOS46_FLAGS_DMA_OFFSET_FIXED                15:15
#define NVOS46_FLAGS_DMA_OFFSET_FIXED_FALSE          0x00000000U
#define NVOS46_FLAGS_DMA_OFFSET_FIXED_TRUE           0x00000001U

#define NV_VASPACE_ALLOCATION_FLAGS_RETRY_PTE_ALLOC_IN_SYS 0x00000002U
#define NV_VASPACE_ALLOCATION_FLAGS_ALLOW_ZERO_ADDRESS     0x00000100U

/* Escape Parameter Structs */
typedef struct {
    NvHandle hRoot;
    NvHandle hObjectParent;
    NvHandle hObjectOld;
    NvV32    status;
} NVOS00_PARAMETERS;

typedef struct {
    NvHandle hRoot;
    NvHandle hObjectParent;
    NvHandle hObjectNew;
    NvV32    hClass;
    NvV32    flags;
    NvP64    pMemory NV_ALIGN_BYTES(8);
    NvU64    limit   NV_ALIGN_BYTES(8);
    NvV32    status;
} NVOS02_PARAMETERS;

typedef struct {
    NVOS02_PARAMETERS params;
    int               fd;
} nv_ioctl_nvos02_with_fd_t;

typedef struct {
    NvHandle hRoot;
    NvHandle hObjectParent;
    NvHandle hObjectNew;
    NvV32    hClass;
    NvP64    pAllocParms NV_ALIGN_BYTES(8);
    NvV32    status;
} NVOS21_PARAMETERS;

typedef struct {
    NvHandle hClient;
    NvHandle hDevice;
    NvHandle hMemory;
    NvU64    offset         NV_ALIGN_BYTES(8);
    NvU64    length         NV_ALIGN_BYTES(8);
    NvP64    pLinearAddress NV_ALIGN_BYTES(8);
    NvU32    status;
    NvU32    flags;
} NVOS33_PARAMETERS;

typedef struct {
    NVOS33_PARAMETERS params;
    int               fd;
} nv_ioctl_nvos33_with_fd_t;

typedef struct {
    NvHandle hClient;
    NvHandle hDevice;
    NvHandle hMemory;
    NvP64    pLinearAddress NV_ALIGN_BYTES(8);
    NvU32    status;
    NvU32    flags;
} NVOS34_PARAMETERS;

typedef struct {
    NvHandle hClient;
    NvHandle hDevice;
    NvHandle hDma;
    NvHandle hMemory;
    NvU64    offset    NV_ALIGN_BYTES(8);
    NvU64    length    NV_ALIGN_BYTES(8);
    NvV32    flags;
    NvU64    dmaOffset NV_ALIGN_BYTES(8);
    NvV32    status;
} NVOS46_PARAMETERS;

typedef struct {
    NvHandle hClient;
    NvHandle hDevice;
    NvHandle hDma;
    NvHandle hMemory;
    NvV32    flags;
    NvU64    dmaOffset NV_ALIGN_BYTES(8);
    NvV32    status;
} NVOS47_PARAMETERS;

typedef struct {
    NvHandle hClient;
    NvHandle hObject;
    NvV32    cmd;
    NvU32    flags;
    NvP64    params NV_ALIGN_BYTES(8);
    NvU32    paramsSize;
    NvV32    status;
} NVOS54_PARAMETERS;

/* Allocation Parameter Structs */
typedef struct {
    NvHandle hSubDevice;
    NvV32    flags;
    NvHandle hMemory;
    NvU64    offset NV_ALIGN_BYTES(8);
    NvU64    limit  NV_ALIGN_BYTES(8);
} NV_CONTEXT_DMA_ALLOCATION_PARAMS;

typedef struct {
    NvU32    deviceId;
    NvHandle hClientShare;
    NvHandle hTargetClient;
    NvHandle hTargetDevice;
    NvV32    flags;
    NvU64    vaSpaceSize     NV_ALIGN_BYTES(8);
    NvU64    vaStartInternal NV_ALIGN_BYTES(8);
    NvU64    vaLimitInternal NV_ALIGN_BYTES(8);
    NvV32    vaMode;
} NV0080_ALLOC_PARAMETERS;

typedef struct {
    NvU32 subDeviceId;
} NV2080_ALLOC_PARAMETERS;

typedef struct {
    NvU32 index;
    NvV32 flags;
    NvU64 vaSize          NV_ALIGN_BYTES(8);
    NvU64 vaStartInternal NV_ALIGN_BYTES(8);
    NvU64 vaLimitInternal NV_ALIGN_BYTES(8);
    NvU32 bigPageSize;
    NvU64 vaBase          NV_ALIGN_BYTES(8);
} NV_VASPACE_ALLOCATION_PARAMETERS;

typedef struct {
    NvU32    owner;
    NvU32    type;
    NvU32    flags;
    NvU32    width;
    NvU32    height;
    NvS32    pitch;
    NvU32    attr;
    NvU32    attr2;
    NvU32    format;
    NvU32    comprCovg;
    NvU32    zcullCovg;
    NvU64    rangeLo   NV_ALIGN_BYTES(8);
    NvU64    rangeHi   NV_ALIGN_BYTES(8);
    NvU64    size      NV_ALIGN_BYTES(8);
    NvU64    alignment NV_ALIGN_BYTES(8);
    NvU64    offset    NV_ALIGN_BYTES(8);
    NvU64    limit     NV_ALIGN_BYTES(8);
    NvP64    address   NV_ALIGN_BYTES(8);
    NvU32    ctagOffset;
    NvHandle hVASpace;
    NvU32    internalflags;
    NvU32    tag;
} NV_MEMORY_ALLOCATION_PARAMS;

typedef struct {
    NvU64 base     NV_ALIGN_BYTES(8);
    NvU64 size     NV_ALIGN_BYTES(8);
    NvU32 addressSpace;
    NvU32 cacheAttrib;
} NV_MEMORY_DESC_PARAMS;

typedef struct {
    NvHandle              hObjectError;
    NvHandle              hObjectBuffer;
    NvU64                 gpFifoOffset NV_ALIGN_BYTES(8);
    NvU32                 gpFifoEntries;
    NvU32                 flags;
    NvHandle              hContextShare;
    NvHandle              hVASpace;
    NvHandle              hUserdMemory[NVOS_MAX_SUBDEVICES];
    NvU64                 userdOffset[NVOS_MAX_SUBDEVICES] NV_ALIGN_BYTES(8);
    NvU32                 engineType;
    NvU32                 cid;
    NvU32                 subDeviceId;
    NvHandle              hObjectEccError;
    NV_MEMORY_DESC_PARAMS instanceMem;
    NV_MEMORY_DESC_PARAMS userdMem;
    NV_MEMORY_DESC_PARAMS ramfcMem;
    NV_MEMORY_DESC_PARAMS mthdbufMem;
    NvHandle              hPhysChannelGroup;
    NvU32                 internalFlags;
    NV_MEMORY_DESC_PARAMS errorNotifierMem;
    NV_MEMORY_DESC_PARAMS eccErrorNotifierMem;
    NvU32                 ProcessID;
    NvU32                 SubProcessID;
} NV_CHANNELGPFIFO_ALLOCATION_PARAMETERS;

typedef struct {
    NvBool bEnable;
    NvBool bSkipSubmit;
} NVA06F_CTRL_GPFIFO_SCHEDULE_PARAMS;

typedef struct {
    NvU32 engineType;
} NVA06F_CTRL_BIND_PARAMS;

typedef struct {
    NvU32 gpuIds[NV_MAX_DEVICES];
    NvU32 failedId;
} NV0000_CTRL_GPU_ATTACH_IDS_PARAMS;

typedef struct {
    NvU32 gpuId;
    NvU32 gpuFlags;
    NvU32 deviceInstance;
    NvU32 subDeviceInstance;
    NvP64 szName NV_ALIGN_BYTES(8);
    NvU32 sliStatus;
    NvU32 boardId;
    NvU32 gpuInstance;
    NvS32 numaId;
} NV0000_CTRL_GPU_GET_ID_INFO_PARAMS;

#endif /* GP108_RM_API_DEFS_H */
