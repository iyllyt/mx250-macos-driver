/* SPDX-License-Identifier: MIT
 * Stub for CUDA CUBIN ELF loader on macOS (Vulkan SPIR-V uses NAK directly)
 */
#include "nv_cubin.h"

bool
nv_cubin_module_init(struct nv_cubin_module *module, const void *data, size_t size)
{
   (void)module; (void)data; (void)size;
   return false;
}

void
nv_cubin_module_fini(struct nv_cubin_module *module)
{
   (void)module;
}

const struct nv_cubin_function *
nv_cubin_module_find_function(const struct nv_cubin_module *module, const char *name)
{
   (void)module; (void)name;
   return NULL;
}

const struct nv_cubin_function_param_info *
nv_cubin_function_get_param_info(const struct nv_cubin_function *function, uint32_t index)
{
   (void)function; (void)index;
   return NULL;
}
