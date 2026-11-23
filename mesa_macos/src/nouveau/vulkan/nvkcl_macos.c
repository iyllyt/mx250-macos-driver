/* SPDX-License-Identifier: MIT
 * Direct NIR Builder Implementation of nvk_copy_queries & nvk_copy_indirect
 */
#include "nvkcl.h"
#include "nvk_private.h"
#include "cl/nvk_copy_indirect.h"
#include "nv_push.h"
#include "clc1b5.h"

static void
emit_write_query(nir_builder *b, nir_def *dst_base, nir_def *idx_u32,
                 nir_def *flags, nir_def *val_u64)
{
   nir_def *is_64 = nir_ine_imm(b, nir_iand_imm(b, flags, VK_QUERY_RESULT_64_BIT), 0);
   nir_push_if(b, is_64);
   {
      nir_def *off = nir_imul_imm(b, nir_u2u64(b, idx_u32), 8);
      nir_store_global(b, val_u64, nir_iadd(b, dst_base, off));
   }
   nir_push_else(b, NULL);
   {
      nir_def *off = nir_imul_imm(b, nir_u2u64(b, idx_u32), 4);
      nir_store_global(b, nir_u2u32(b, val_u64), nir_iadd(b, dst_base, off));
   }
   nir_pop_if(b, NULL);
}

void
nvk_copy_queries(nir_builder *b,
                 nir_def *pool_addr,
                 nir_def *available_stride,
                 nir_def *reports_start,
                 nir_def *report_count,
                 nir_def *query_stride,
                 nir_def *first_query,
                 nir_def *query_count,
                 nir_def *dst_addr,
                 nir_def *dst_stride,
                 nir_def *flags)
{
   nir_def *gid = nir_channel(b, nir_load_global_invocation_id(b, 32), 0);
   nir_push_if(b, nir_ult(b, gid, query_count));
   {
      nir_def *query = nir_iadd(b, first_query, gid);
      nir_def *avail_off = nir_imul(b, nir_u2u64(b, query), nir_u2u64(b, available_stride));
      nir_def *avail_u32 = nir_load_global(b, 1, 32, nir_iadd(b, pool_addr, avail_off));
      nir_def *avail_b   = nir_ine_imm(b, avail_u32, 0);
      nir_def *partial_b = nir_ine_imm(b, nir_iand_imm(b, flags, VK_QUERY_RESULT_PARTIAL_BIT), 0);
      nir_def *do_write  = nir_ior(b, avail_b, partial_b);

      nir_def *rep_off = nir_iadd(b, nir_u2u64(b, reports_start),
                                  nir_imul(b, nir_u2u64(b, query), nir_u2u64(b, query_stride)));
      nir_def *rep_base = nir_iadd(b, pool_addr, rep_off);
      nir_def *dst_base = nir_iadd(b, dst_addr, nir_imul(b, dst_stride, nir_u2u64(b, gid)));

      nir_push_if(b, do_write);
      {
         nir_variable *r_var = nir_local_variable_create(b->impl, glsl_uint_type(), "r");
         nir_store_var(b, r_var, nir_imm_int(b, 0), 0x1);
         nir_push_loop(b);
         {
            nir_def *r = nir_load_var(b, r_var);
            nir_push_if(b, nir_uge(b, r, report_count));
            {
               nir_jump(b, nir_jump_break);
            }
            nir_pop_if(b, NULL);

            nir_def *val_addr = nir_iadd(b, rep_base, nir_imul_imm(b, nir_u2u64(b, r), 16));
            nir_def *val_u64  = nir_load_global(b, 1, 64, val_addr);
            emit_write_query(b, dst_base, r, flags, val_u64);
            nir_store_var(b, r_var, nir_iadd_imm(b, r, 1), 0x1);
         }
         nir_pop_loop(b, NULL);
      }
      nir_pop_if(b, NULL);

      nir_def *with_avail =
         nir_ine_imm(b, nir_iand_imm(b, flags, VK_QUERY_RESULT_WITH_AVAILABILITY_BIT), 0);
      nir_push_if(b, with_avail);
      {
         nir_def *av64 = nir_bcsel(b, avail_b, nir_imm_int64(b, 1), nir_imm_int64(b, 0));
         emit_write_query(b, dst_base, report_count, flags, av64);
      }
      nir_pop_if(b, NULL);
   }
   nir_pop_if(b, NULL);
}

void
nvk_copy_indirect(nir_builder *b,
                  nir_def *in_addr,
                  nir_def *in_stride,
                  nir_def *out_addr,
                  nir_def *count)
{
   nir_def *gid = nir_channel(b, nir_load_global_invocation_id(b, 32), 0);
   nir_push_if(b, nir_ult(b, gid, count));
   {
      nir_def *cmd_ptr  = nir_iadd(b, in_addr, nir_imul(b, in_stride, nir_u2u64(b, gid)));
      nir_def *src_addr = nir_load_global(b, 1, 64, cmd_ptr);
      nir_def *dst_addr = nir_load_global(b, 1, 64, nir_iadd_imm(b, cmd_ptr, 8));
      nir_def *size_64  = nir_load_global(b, 1, 64, nir_iadd_imm(b, cmd_ptr, 16));
      nir_def *size_32  = nir_u2u32(b, size_64);

      nir_def *out_ptr =
         nir_iadd(b, out_addr, nir_imul_imm(b, nir_u2u64(b, gid), NVK_COPY_INDIRECT_CMD_BYTES));

      /* Subchannel 4 = SUBC_NV90B5:
       * [0] Mthd 0x0400 (OFFSET_IN_UPPER), count 4
       * [1..4] src_hi, src_lo, dst_hi, dst_lo
       * [5] Mthd 0x0418 (LINE_LENGTH_IN), count 1
       * [6] size_32
       * [7] Immd Mthd 0x0300 (LAUNCH_DMA) = PIPELINED(1) | FLUSH_ENABLE(4) = 0x5
       */
      uint32_t hdr_offsets = (2U << 29) | (4U << 16) | (SUBC_NV90B5 << 13) | (0x0400U >> 2);
      uint32_t hdr_len     = (2U << 29) | (1U << 16) | (SUBC_NV90B5 << 13) | (0x0418U >> 2);
      uint32_t hdr_launch  = (4U << 29) | (0x0005U << 16) | (SUBC_NV90B5 << 13) | (0x0300U >> 2);

      nir_store_global(b, nir_imm_int(b, hdr_offsets), out_ptr);
      nir_store_global(b, nir_unpack_64_2x32_split_y(b, src_addr), nir_iadd_imm(b, out_ptr, 4));
      nir_store_global(b, nir_unpack_64_2x32_split_x(b, src_addr), nir_iadd_imm(b, out_ptr, 8));
      nir_store_global(b, nir_unpack_64_2x32_split_y(b, dst_addr), nir_iadd_imm(b, out_ptr, 12));
      nir_store_global(b, nir_unpack_64_2x32_split_x(b, dst_addr), nir_iadd_imm(b, out_ptr, 16));
      nir_store_global(b, nir_imm_int(b, hdr_len),                 nir_iadd_imm(b, out_ptr, 20));
      nir_store_global(b, size_32,                                 nir_iadd_imm(b, out_ptr, 24));
      nir_store_global(b, nir_imm_int(b, hdr_launch),              nir_iadd_imm(b, out_ptr, 28));
   }
   nir_pop_if(b, NULL);
}
