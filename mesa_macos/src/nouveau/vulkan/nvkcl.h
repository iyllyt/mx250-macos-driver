/* SPDX-License-Identifier: MIT
 * Direct NIR Builder Implementation of nvkcl Kernels (Bypassing LLVM mesa_clc)
 */
#ifndef NVKCL_MACOS_H
#define NVKCL_MACOS_H

#include "nir_builder.h"

#ifdef __cplusplus
extern "C" {
#endif

void nvk_copy_queries(nir_builder *b,
                      nir_def *pool_addr,
                      nir_def *available_stride,
                      nir_def *reports_start,
                      nir_def *report_count,
                      nir_def *query_stride,
                      nir_def *first_query,
                      nir_def *query_count,
                      nir_def *dst_addr,
                      nir_def *dst_stride,
                      nir_def *flags);

void nvk_copy_indirect(nir_builder *b,
                       nir_def *in_addr,
                       nir_def *in_stride,
                       nir_def *out_addr,
                       nir_def *count);

#ifdef __cplusplus
}
#endif

#endif /* NVKCL_MACOS_H */
