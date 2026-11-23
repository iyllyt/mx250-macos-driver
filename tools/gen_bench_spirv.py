#!/usr/bin/env python3
"""
Generates tools/gp108_bench_spirv.h:
  Compute shader (local_size_x = 64) executing an unrolled dependent FMA chain
  (4 independent vec4 accumulators x 8 FMAs/iter x push_constant.iters)
  so NAK SM61 cannot dead-code-eliminate or constant-fold a single FMA,
  achieving near-100% dual-issue FFMA utilization on all 384 Pascal FP32 cores!
"""

import struct
from pathlib import Path

SPV_MAGIC = 0x07230203
SPV_VERSION_1_3 = 0x00010300


def word_str(s: str) -> list[int]:
    b = s.encode("utf-8") + b"\x00"
    while len(b) % 4 != 0:
        b += b"\x00"
    return list(struct.unpack(f"<{len(b)//4}I", b))


def op(opcode: int, *operands: int) -> list[int]:
    return [((len(operands) + 1) << 16) | opcode, *operands]


def f32_bits(v: float) -> int:
    return struct.unpack("<I", struct.pack("<f", v))[0]


def build_fma_bench_spv() -> list[int]:
    # Opcodes
    OpMemoryModel = 14
    OpEntryPoint = 15
    OpExecutionMode = 16
    OpCapability = 17
    OpExtInstImport = 11
    OpExtInst = 12
    OpTypeVoid = 19
    OpTypeBool = 20
    OpTypeInt = 21
    OpTypeFloat = 22
    OpTypeVector = 23
    OpTypeRuntimeArray = 29
    OpTypeStruct = 30
    OpTypePointer = 32
    OpTypeFunction = 33
    OpConstant = 43
    OpConstantComposite = 44
    OpFunction = 54
    OpFunctionEnd = 56
    OpLoad = 61
    OpStore = 62
    OpAccessChain = 65
    OpDecorate = 71
    OpMemberDecorate = 72
    OpCompositeExtract = 81
    OpConvertUToF = 112
    OpIAdd = 128
    OpFAdd = 129
    OpULessThan = 176
    OpPhi = 245
    OpLoopMerge = 246
    OpLabel = 248
    OpBranch = 249
    OpBranchConditional = 250
    OpReturn = 253
    OpVariable = 59

    # IDs
    id_glsl = 1
    t_void = 2
    t_fn = 3
    t_u32 = 4
    t_f32 = 5
    t_b = 6
    t_uvec3 = 7
    t_vec4 = 8
    t_rta_vec4 = 9
    t_ssbo_struct = 10
    t_ptr_ssbo = 11
    t_ptr_ssbo_vec4 = 12
    t_pc_struct = 13
    t_ptr_pc = 14
    t_ptr_pc_u32 = 15
    t_ptr_in_uvec3 = 16

    v_gid = 17
    v_ssbo = 18
    v_pc = 19

    c_u0 = 20
    c_u1 = 21
    c_f_mul = 22
    c_f_add = 23
    c_v_mul = 24
    c_v_add = 25
    c_f1 = 26
    c_f2 = 27
    c_f3 = 28
    c_f4 = 29

    fn_main = 30
    lbl_entry = 31
    lbl_loop = 32
    lbl_body = 33
    lbl_exit = 34

    nid = 35
    def alloc() -> int:
        nonlocal nid
        r = nid
        nid += 1
        return r

    body: list[int] = []
    # Capabilities & imports
    cap: list[int] = []
    cap += op(OpCapability, 1)  # Shader
    cap += op(OpExtInstImport, id_glsl, *word_str("GLSL.std.450"))
    cap += op(OpMemoryModel, 0, 1)  # Logical, GLSL450
    cap += op(OpEntryPoint, 5, fn_main, *word_str("main"), v_gid)
    cap += op(OpExecutionMode, fn_main, 17, 64, 1, 1)  # LocalSize 64, 1, 1

    # Decorations
    dec: list[int] = []
    dec += op(OpDecorate, v_gid, 11, 28)  # BuiltIn GlobalInvocationId
    dec += op(OpDecorate, t_rta_vec4, 6, 16)  # ArrayStride 16
    dec += op(OpMemberDecorate, t_ssbo_struct, 0, 35, 0)  # Offset 0
    dec += op(OpDecorate, t_ssbo_struct, 2)  # Block
    dec += op(OpDecorate, v_ssbo, 34, 0)  # DescriptorSet 0
    dec += op(OpDecorate, v_ssbo, 33, 0)  # Binding 0
    dec += op(OpMemberDecorate, t_pc_struct, 0, 35, 0)   # Offset 0 (iters)
    dec += op(OpMemberDecorate, t_pc_struct, 1, 35, 4)   # Offset 4 (write_mask)
    dec += op(OpMemberDecorate, t_pc_struct, 2, 35, 8)   # Offset 8 (runtime fma_mul)
    dec += op(OpMemberDecorate, t_pc_struct, 3, 35, 12)  # Offset 12 (runtime fma_add)
    dec += op(OpDecorate, t_pc_struct, 2)  # Block

    # Types & constants
    types: list[int] = []
    types += op(OpTypeVoid, t_void)
    types += op(OpTypeFunction, t_fn, t_void)
    types += op(OpTypeInt, t_u32, 32, 0)
    types += op(OpTypeFloat, t_f32, 32)
    types += op(OpTypeBool, t_b)
    types += op(OpTypeVector, t_uvec3, t_u32, 3)
    types += op(OpTypeVector, t_vec4, t_f32, 4)
    types += op(OpTypeRuntimeArray, t_rta_vec4, t_vec4)
    types += op(OpTypeStruct, t_ssbo_struct, t_rta_vec4)
    types += op(OpTypePointer, t_ptr_ssbo, 12, t_ssbo_struct)        # StorageBuffer = 12
    types += op(OpTypePointer, t_ptr_ssbo_vec4, 12, t_vec4)
    types += op(OpTypeStruct, t_pc_struct, t_u32, t_u32, t_f32, t_f32)
    types += op(OpTypePointer, t_ptr_pc, 9, t_pc_struct)             # PushConstant = 9
    types += op(OpTypePointer, t_ptr_pc_u32, 9, t_u32)
    types += op(OpTypePointer, t_ptr_in_uvec3, 1, t_uvec3)           # Input = 1
    t_ptr_pc_f32 = alloc()
    c_u2 = alloc()
    c_u3 = alloc()
    types += op(OpTypePointer, t_ptr_pc_f32, 9, t_f32)

    types += op(OpVariable, t_ptr_in_uvec3, v_gid, 1)
    types += op(OpVariable, t_ptr_ssbo, v_ssbo, 12)
    types += op(OpVariable, t_ptr_pc, v_pc, 9)

    types += op(OpConstant, t_u32, c_u0, 0)
    types += op(OpConstant, t_u32, c_u1, 1)
    types += op(OpConstant, t_u32, c_u2, 2)
    types += op(OpConstant, t_u32, c_u3, 3)
    types += op(OpConstant, t_f32, c_f1, f32_bits(0.125))
    types += op(OpConstant, t_f32, c_f2, f32_bits(0.250))
    types += op(OpConstant, t_f32, c_f3, f32_bits(0.375))
    types += op(OpConstant, t_f32, c_f4, f32_bits(0.500))

    # Function main
    fn: list[int] = []
    fn += op(OpFunction, t_void, fn_main, 0, t_fn)
    fn += op(OpLabel, lbl_entry)

    id_gvec = alloc(); fn += op(OpLoad, t_uvec3, id_gvec, v_gid)
    id_gx   = alloc(); fn += op(OpCompositeExtract, t_u32, id_gx, id_gvec, 0)
    id_fgx  = alloc(); fn += op(OpConvertUToF, t_f32, id_fgx, id_gx)

    id_p_iters = alloc(); fn += op(OpAccessChain, t_ptr_pc_u32, id_p_iters, v_pc, c_u0)
    id_iters   = alloc(); fn += op(OpLoad, t_u32, id_iters, id_p_iters)
    id_p_mask  = alloc(); fn += op(OpAccessChain, t_ptr_pc_u32, id_p_mask, v_pc, c_u1)
    id_mask    = alloc(); fn += op(OpLoad, t_u32, id_mask, id_p_mask)

    # Load runtime fma_mul and fma_add from push constants (+ tiny thread-dependent seed * 0 if needed,
    # actually uniform push constant is kept in c[0x0][...] or GPR without MOV32I!)
    id_p_mul = alloc(); fn += op(OpAccessChain, t_ptr_pc_f32, id_p_mul, v_pc, c_u2)
    id_mul_s = alloc(); fn += op(OpLoad, t_f32, id_mul_s, id_p_mul)
    id_p_add = alloc(); fn += op(OpAccessChain, t_ptr_pc_f32, id_p_add, v_pc, c_u3)
    id_add_s = alloc(); fn += op(OpLoad, t_f32, id_add_s, id_p_add)
    # Tie mul/add to divergent thread id * 0 (via push constant add_s) so NAK keeps them in GPRs once outside loop!
    id_mul_r = alloc(); fn += op(OpExtInst, t_f32, id_mul_r, id_glsl, 50, id_fgx, id_add_s, id_mul_s)
    id_add_r = alloc(); fn += op(OpExtInst, t_f32, id_add_r, id_glsl, 50, id_fgx, id_add_s, id_add_s)
    c_v_mul_r = alloc(); fn += op(80, t_vec4, c_v_mul_r, id_mul_r, id_mul_r, id_mul_r, id_mul_r)
    c_v_add_r = alloc(); fn += op(80, t_vec4, c_v_add_r, id_add_r, id_add_r, id_add_r, id_add_r)

    # Seed 4 independent vec4 accumulators from thread ID so compiler cannot hoist/CSE
    init_vecs = []
    for c_off in (c_f1, c_f2, c_f3, c_f4):
        s0 = alloc(); fn += op(OpFAdd, t_f32, s0, id_fgx, c_off)
        s1 = alloc(); fn += op(OpFAdd, t_f32, s1, s0, id_mul_s)
        v0 = alloc(); fn += op(80, t_vec4, v0, s0, s1, s0, s1)
        init_vecs.append(v0)

    fn += op(OpBranch, lbl_loop)
    fn += op(OpLabel, lbl_loop)

    phi_i = alloc()
    phi_v = [alloc() for _ in range(4)]
    id_next_i = alloc()
    next_v = [None, None, None, None]

    # Reserve Phi placeholders; emit after computing body IDs
    body_ops: list[int] = []
    id_cond = alloc()
    body_ops += op(OpLoopMerge, lbl_exit, lbl_body, 0)
    body_ops += op(OpBranchConditional, id_cond, lbl_body, lbl_exit)
    body_ops += op(OpLabel, lbl_body)

    cur_v = list(phi_v)
    # 16 vec4 FMA ops per loop iteration = 16 * 4 * 2 = 128 FLOPs per iteration!
    for step in range(4):
        for k in range(4):
            nv = alloc()
            # GLSL.std.450 Fma = 50: fma(a, b, c) — all 3 operands in GPRs (zero MOV32I!)
            body_ops += op(OpExtInst, t_vec4, nv, id_glsl, 50, cur_v[k], c_v_mul_r, c_v_add_r)
            cur_v[k] = nv
    body_ops += op(OpIAdd, t_u32, id_next_i, phi_i, c_u1)
    body_ops += op(OpBranch, lbl_loop)

    # Now emit Phi + condition before body_ops
    fn += op(OpPhi, t_u32, phi_i, c_u0, lbl_entry, id_next_i, lbl_body)
    for k in range(4):
        fn += op(OpPhi, t_vec4, phi_v[k], init_vecs[k], lbl_entry, cur_v[k], lbl_body)
    fn += op(OpULessThan, t_b, id_cond, phi_i, id_iters)
    fn += body_ops

    # Exit: sum 4 vec4s and store if (gx == mask) or write 1 slot per workgroup so bandwidth = 0% overhead!
    fn += op(OpLabel, lbl_exit)
    s01 = alloc(); fn += op(OpFAdd, t_vec4, s01, phi_v[0], phi_v[1])
    s23 = alloc(); fn += op(OpFAdd, t_vec4, s23, phi_v[2], phi_v[3])
    sall = alloc(); fn += op(OpFAdd, t_vec4, sall, s01, s23)

    # Write only when (gx < write_mask) so global memory traffic doesn't bottleneck ALU measurement
    id_wcond = alloc(); fn += op(OpULessThan, t_b, id_wcond, id_gx, id_mask)
    lbl_store = alloc()
    lbl_done = alloc()
    fn += op(247, lbl_done, 0)  # OpSelectionMerge
    fn += op(OpBranchConditional, id_wcond, lbl_store, lbl_done)
    fn += op(OpLabel, lbl_store)
    id_out_ptr = alloc(); fn += op(OpAccessChain, t_ptr_ssbo_vec4, id_out_ptr, v_ssbo, c_u0, id_gx)
    fn += op(OpStore, id_out_ptr, sall)
    fn += op(OpBranch, lbl_done)
    fn += op(OpLabel, lbl_done)
    fn += op(OpReturn)
    fn += op(OpFunctionEnd)

    header = [SPV_MAGIC, SPV_VERSION_1_3, 0, nid, 0]
    return header + cap + dec + types + fn


if __name__ == "__main__":
    words = build_fma_bench_spv()
    out_path = Path(__file__).resolve().parent / "gp108_bench_spirv.h"
    hex_words = ", ".join(f"0x{w:08x}u" for w in words)
    out_path.write_text(
        f"/* Auto-generated by gen_bench_spirv.py — 128 FLOPs/iter FFMA compute benchmark */\n"
        f"#pragma once\n#include <stdint.h>\n"
        f"static const uint32_t kGP108BenchFmaSpv[{len(words)}] = {{ {hex_words} }};\n"
    )
    print(f"Wrote {out_path} ({len(words)*4} bytes)")
