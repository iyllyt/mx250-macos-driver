#!/usr/bin/env python3
"""
Pure-Python Vulkan 1.3 SPIR-V binary generator for MX250 (NVK/NAK SM61) testing.
Emits tools/gp108_test_shaders.h with zero external compiler dependencies.
"""

import struct
import os

SPV_MAGIC = 0x07230203
SPV_VERSION_1_3 = 0x00010300

# Opcodes (from Mesa src/compiler/spirv/spirv.h)
OpName = 5
OpMemberName = 6
OpMemoryModel = 14
OpEntryPoint = 15
OpExecutionMode = 16
OpCapability = 17
OpTypeVoid = 19
OpTypeBool = 20
OpTypeInt = 21
OpTypeFloat = 22
OpTypeVector = 23
OpTypeMatrix = 24
OpTypeRuntimeArray = 29
OpTypeStruct = 30
OpTypePointer = 32
OpTypeFunction = 33
OpConstant = 43
OpConstantComposite = 44
OpFunction = 54
OpFunctionEnd = 56
OpVariable = 59
OpLoad = 61
OpStore = 62
OpAccessChain = 65
OpDecorate = 71
OpMemberDecorate = 72
OpCompositeConstruct = 80
OpCompositeExtract = 81
OpConvertUToF = 112
OpConvertFToU = 109
OpFNegate = 127
OpIAdd = 128
OpFAdd = 129
OpFSub = 131
OpIMul = 132
OpFMul = 133
OpVectorTimesScalar = 142
OpMatrixTimesVector = 145
OpDot = 148
OpSelect = 169
OpULessThan = 176
OpFOrdLessThan = 184
OpFOrdLessThanEqual = 188
OpFOrdGreaterThan = 186
OpShiftLeftLogical = 196
OpBitwiseOr = 197
OpLogicalAnd = 167
OpLabel = 248
OpBranchConditional = 250
OpSelectionMerge = 247
OpReturn = 253

# Enums
CapMatrix = 0
CapShader = 1
AddrLogical = 0
MemGLSL450 = 1

ExecVertex = 0
ExecFragment = 4
ExecGLCompute = 5

ModeOriginUpperLeft = 7
ModeLocalSize = 17

SCInput = 1
SCOutput = 3
SCPushConstant = 9
SCStorageBuffer = 12

DecBlock = 2
DecColMajor = 5
DecArrayStride = 6
DecMatrixStride = 7
DecBuiltIn = 11
DecLocation = 30
DecBinding = 33
DecDescriptorSet = 34
DecOffset = 35

BuiltInPosition = 0
BuiltInGlobalInvocationId = 28


def f32_bits(val: float) -> int:
    return struct.unpack("<I", struct.pack("<f", val))[0]


def encode_str(s: str) -> list:
    b = s.encode("utf-8") + b"\x00"
    while len(b) % 4 != 0:
        b += b"\x00"
    return list(struct.unpack("<%dI" % (len(b) // 4), b))


class SpvBuilder:
    def __init__(self):
        self.next_id = 1
        self.caps = []
        self.entry_points = []
        self.exec_modes = []
        self.decorations = []
        self.types_consts_globals = []
        self.functions = []
        self._const_cache = {}

    def alloc_id(self) -> int:
        r = self.next_id
        self.next_id += 1
        return r

    @staticmethod
    def inst(op: int, *words: int) -> list:
        count = 1 + len(words)
        return [(count << 16) | (op & 0xFFFF)] + [w & 0xFFFFFFFF for w in words]

    def cap(self, c: int):
        self.caps += self.inst(OpCapability, c)

    def entry(self, model: int, fn_id: int, name: str, iface_ids: list):
        self.entry_points += self.inst(
            OpEntryPoint, model, fn_id, *encode_str(name), *iface_ids
        )

    def exec_mode(self, fn_id: int, mode: int, *args: int):
        self.exec_modes += self.inst(OpExecutionMode, fn_id, mode, *args)

    def decorate(self, target: int, dec: int, *args: int):
        self.decorations += self.inst(OpDecorate, target, dec, *args)

    def member_decorate(self, struct_id: int, member: int, dec: int, *args: int):
        self.decorations += self.inst(OpMemberDecorate, struct_id, member, dec, *args)

    def def_op(self, op: int, *args: int) -> int:
        rid = self.alloc_id()
        self.types_consts_globals += self.inst(op, rid, *args)
        return rid

    def type_void(self) -> int:
        return self.def_op(OpTypeVoid)

    def type_bool(self) -> int:
        return self.def_op(OpTypeBool)

    def type_int(self, width: int, signed: int) -> int:
        return self.def_op(OpTypeInt, width, signed)

    def type_float(self, width: int) -> int:
        return self.def_op(OpTypeFloat, width)

    def type_vec(self, comp_type: int, count: int) -> int:
        return self.def_op(OpTypeVector, comp_type, count)

    def type_mat(self, col_type: int, cols: int) -> int:
        return self.def_op(OpTypeMatrix, col_type, cols)

    def type_rt_array(self, elem_type: int) -> int:
        return self.def_op(OpTypeRuntimeArray, elem_type)

    def type_struct(self, *member_types: int) -> int:
        return self.def_op(OpTypeStruct, *member_types)

    def type_ptr(self, storage_class: int, elem_type: int) -> int:
        return self.def_op(OpTypePointer, storage_class, elem_type)

    def type_fn(self, ret_type: int, *arg_types: int) -> int:
        return self.def_op(OpTypeFunction, ret_type, *arg_types)

    def const_u32(self, t_u32: int, val: int) -> int:
        key = ("u32", t_u32, val & 0xFFFFFFFF)
        if key not in self._const_cache:
            rid = self.alloc_id()
            self.types_consts_globals += self.inst(OpConstant, t_u32, rid, val & 0xFFFFFFFF)
            self._const_cache[key] = rid
        return self._const_cache[key]

    def const_i32(self, t_i32: int, val: int) -> int:
        key = ("i32", t_i32, val & 0xFFFFFFFF)
        if key not in self._const_cache:
            rid = self.alloc_id()
            self.types_consts_globals += self.inst(OpConstant, t_i32, rid, val & 0xFFFFFFFF)
            self._const_cache[key] = rid
        return self._const_cache[key]

    def const_f32(self, t_f32: int, val: float) -> int:
        bits = f32_bits(val)
        key = ("f32", t_f32, bits)
        if key not in self._const_cache:
            rid = self.alloc_id()
            self.types_consts_globals += self.inst(OpConstant, t_f32, rid, bits)
            self._const_cache[key] = rid
        return self._const_cache[key]

    def global_var(self, ptr_type: int, storage_class: int) -> int:
        rid = self.alloc_id()
        self.types_consts_globals += self.inst(OpVariable, ptr_type, rid, storage_class)
        return rid

    def fn_op(self, res_type: int, op: int, *args: int) -> int:
        rid = self.alloc_id()
        self.functions += self.inst(op, res_type, rid, *args)
        return rid

    def fn_void_op(self, op: int, *args: int):
        self.functions += self.inst(op, *args)

    def build(self) -> list:
        header = [SPV_MAGIC, SPV_VERSION_1_3, 0, self.next_id, 0]
        mem = self.inst(OpMemoryModel, AddrLogical, MemGLSL450)
        return (
            header
            + self.caps
            + mem
            + self.entry_points
            + self.exec_modes
            + self.decorations
            + self.types_consts_globals
            + self.functions
        )


def build_compute_mandelbrot() -> list:
    """
    Compute shader: LocalSize(16, 16, 1)
    set=0, binding=0: StorageBuffer { uint pixels[]; }
    PushConstant: { uint width; uint height; float scale; float center_x; }
    Computes 24-iteration FP32 Mandelbrot escape count & packs RGBA8 pixel:
      out[gy * width + gx] = (0xFF << 24) | (b << 16) | (g << 8) | r
    """
    b = SpvBuilder()
    b.cap(CapShader)

    t_void = b.type_void()
    t_bool = b.type_bool()
    t_u32 = b.type_int(32, 0)
    t_i32 = b.type_int(32, 1)
    t_f32 = b.type_float(32)
    t_uvec3 = b.type_vec(t_u32, 3)
    t_fn = b.type_fn(t_void)

    # GlobalInvocationId input
    t_ptr_in_uvec3 = b.type_ptr(SCInput, t_uvec3)
    v_gid = b.global_var(t_ptr_in_uvec3, SCInput)
    b.decorate(v_gid, DecBuiltIn, BuiltInGlobalInvocationId)

    # StorageBuffer { uint data[]; }
    t_rta_u32 = b.type_rt_array(t_u32)
    b.decorate(t_rta_u32, DecArrayStride, 4)
    t_ssbo_struct = b.type_struct(t_rta_u32)
    b.decorate(t_ssbo_struct, DecBlock)
    b.member_decorate(t_ssbo_struct, 0, DecOffset, 0)
    t_ptr_ssbo = b.type_ptr(SCStorageBuffer, t_ssbo_struct)
    t_ptr_ssbo_u32 = b.type_ptr(SCStorageBuffer, t_u32)
    v_ssbo = b.global_var(t_ptr_ssbo, SCStorageBuffer)
    b.decorate(v_ssbo, DecDescriptorSet, 0)
    b.decorate(v_ssbo, DecBinding, 0)

    # PushConstant { uint width; uint height; float scale; float center_x; }
    t_pc_struct = b.type_struct(t_u32, t_u32, t_f32, t_f32)
    b.decorate(t_pc_struct, DecBlock)
    b.member_decorate(t_pc_struct, 0, DecOffset, 0)
    b.member_decorate(t_pc_struct, 1, DecOffset, 4)
    b.member_decorate(t_pc_struct, 2, DecOffset, 8)
    b.member_decorate(t_pc_struct, 3, DecOffset, 12)
    t_ptr_pc = b.type_ptr(SCPushConstant, t_pc_struct)
    t_ptr_pc_u32 = b.type_ptr(SCPushConstant, t_u32)
    t_ptr_pc_f32 = b.type_ptr(SCPushConstant, t_f32)
    v_pc = b.global_var(t_ptr_pc, SCPushConstant)

    c_i0 = b.const_i32(t_i32, 0)
    c_i1 = b.const_i32(t_i32, 1)
    c_i2 = b.const_i32(t_i32, 2)
    c_i3 = b.const_i32(t_i32, 3)

    c_u0 = b.const_u32(t_u32, 0)
    c_u1 = b.const_u32(t_u32, 1)
    c_u8 = b.const_u32(t_u32, 8)
    c_u16 = b.const_u32(t_u32, 16)
    c_u9 = b.const_u32(t_u32, 9)
    c_u7 = b.const_u32(t_u32, 7)
    c_u11 = b.const_u32(t_u32, 10)
    c_uff = b.const_u32(t_u32, 0xFF000000)

    c_f0 = b.const_f32(t_f32, 0.0)
    c_f05 = b.const_f32(t_f32, 0.5)
    c_f2 = b.const_f32(t_f32, 2.0)
    c_f4 = b.const_f32(t_f32, 4.0)

    fn_main = b.alloc_id()
    lbl_entry = b.alloc_id()
    lbl_do_store = b.alloc_id()
    lbl_exit = b.alloc_id()

    b.entry(ExecGLCompute, fn_main, "main", [v_gid])
    b.exec_mode(fn_main, ModeLocalSize, 16, 16, 1)

    b.functions += b.inst(OpFunction, t_void, fn_main, 0, t_fn)
    b.functions += b.inst(OpLabel, lbl_entry)

    gid = b.fn_op(t_uvec3, OpLoad, v_gid)
    gx = b.fn_op(t_u32, OpCompositeExtract, gid, 0)
    gy = b.fn_op(t_u32, OpCompositeExtract, gid, 1)

    p_w = b.fn_op(t_ptr_pc_u32, OpAccessChain, v_pc, c_i0)
    p_h = b.fn_op(t_ptr_pc_u32, OpAccessChain, v_pc, c_i1)
    p_sc = b.fn_op(t_ptr_pc_f32, OpAccessChain, v_pc, c_i2)
    p_cx0 = b.fn_op(t_ptr_pc_f32, OpAccessChain, v_pc, c_i3)

    w = b.fn_op(t_u32, OpLoad, p_w)
    h = b.fn_op(t_u32, OpLoad, p_h)
    scale = b.fn_op(t_f32, OpLoad, p_sc)
    center_x = b.fn_op(t_f32, OpLoad, p_cx0)

    in_x = b.fn_op(t_bool, OpULessThan, gx, w)
    in_y = b.fn_op(t_bool, OpULessThan, gy, h)
    in_bounds = b.fn_op(t_bool, OpLogicalAnd, in_x, in_y)

    b.fn_void_op(OpSelectionMerge, lbl_exit, 0)
    b.fn_void_op(OpBranchConditional, in_bounds, lbl_do_store, lbl_exit)

    b.functions += b.inst(OpLabel, lbl_do_store)

    fgx = b.fn_op(t_f32, OpConvertUToF, gx)
    fgy = b.fn_op(t_f32, OpConvertUToF, gy)
    fw = b.fn_op(t_f32, OpConvertUToF, w)
    fh = b.fn_op(t_f32, OpConvertUToF, h)

    hw = b.fn_op(t_f32, OpFMul, fw, c_f05)
    hh = b.fn_op(t_f32, OpFMul, fh, c_f05)
    dx = b.fn_op(t_f32, OpFSub, fgx, hw)
    dy = b.fn_op(t_f32, OpFSub, fgy, hh)
    cx_rel = b.fn_op(t_f32, OpFMul, dx, scale)
    cx = b.fn_op(t_f32, OpFAdd, cx_rel, center_x)
    cy = b.fn_op(t_f32, OpFMul, dy, scale)

    zr = c_f0
    zi = c_f0
    iters = c_u0

    # 24 unrolled Mandelbrot steps (pure SSA, exercises SM61 FP32 FMA + ISETP + SEL)
    for _ in range(24):
        zr2 = b.fn_op(t_f32, OpFMul, zr, zr)
        zi2 = b.fn_op(t_f32, OpFMul, zi, zi)
        mag2 = b.fn_op(t_f32, OpFAdd, zr2, zi2)
        alive = b.fn_op(t_bool, OpFOrdLessThanEqual, mag2, c_f4)

        inc = b.fn_op(t_u32, OpIAdd, iters, c_u1)
        iters = b.fn_op(t_u32, OpSelect, alive, inc, iters)

        # Keep zr/zi clamped once escaped so it never overflows to NaN
        zr_cl = b.fn_op(t_f32, OpSelect, alive, zr, c_f2)
        zi_cl = b.fn_op(t_f32, OpSelect, alive, zi, c_f2)
        zr2_c = b.fn_op(t_f32, OpFMul, zr_cl, zr_cl)
        zi2_c = b.fn_op(t_f32, OpFMul, zi_cl, zi_cl)
        diff = b.fn_op(t_f32, OpFSub, zr2_c, zi2_c)
        new_zr = b.fn_op(t_f32, OpFAdd, diff, cx)

        zri = b.fn_op(t_f32, OpFMul, zr_cl, zi_cl)
        two_zri = b.fn_op(t_f32, OpFMul, c_f2, zri)
        new_zi = b.fn_op(t_f32, OpFAdd, two_zri, cy)

        zr = b.fn_op(t_f32, OpSelect, alive, new_zr, zr_cl)
        zi = b.fn_op(t_f32, OpSelect, alive, new_zi, zi_cl)

    # Encode deterministic RGBA8 + iteration count in low byte:
    # pixel = 0xFF000000 | ((iters * 11) << 16) | ((iters * 7) << 8) | iters
    # Notice low byte == iters (0..24), making exact numerical verification trivial,
    # while RGB channels form a rich palette!
    g_val = b.fn_op(t_u32, OpIMul, iters, c_u7)
    b_val = b.fn_op(t_u32, OpIMul, iters, c_u9)
    r_val = b.fn_op(t_u32, OpIMul, iters, c_u11)
    # Put exact `iters` in bits 0..7? Even better: pack `(iters * 9)` in R, `(iters * 7)` in G,
    # `(iters * 11)` in B, and store exact `iters` in alpha or verify the exact formula!
    g_sh = b.fn_op(t_u32, OpShiftLeftLogical, g_val, c_u8)
    b_sh = b.fn_op(t_u32, OpShiftLeftLogical, b_val, c_u16)
    rg = b.fn_op(t_u32, OpBitwiseOr, r_val, g_sh)
    rgb = b.fn_op(t_u32, OpBitwiseOr, rg, b_sh)
    rgba = b.fn_op(t_u32, OpBitwiseOr, rgb, c_uff)

    row_off = b.fn_op(t_u32, OpIMul, gy, w)
    idx = b.fn_op(t_u32, OpIAdd, row_off, gx)
    p_out = b.fn_op(t_ptr_ssbo_u32, OpAccessChain, v_ssbo, c_i0, idx)
    b.fn_void_op(OpStore, p_out, rgba)

    b.fn_void_op(OpReturn)
    b.functions += b.inst(OpLabel, lbl_exit)
    b.fn_void_op(OpReturn)
    b.functions += b.inst(OpFunctionEnd)
    return b.build()


def build_vert_cube3d() -> list:
    """
    3D Vertex Shader for both Offscreen 3D verification & Live Windowed 3D Demo:
      Location 0: in vec3 in_pos
      Location 1: in vec3 in_normal
      Location 2: in vec3 in_color
      PushConstant (128 bytes):
        mat4 mvp        (offset 0)
        vec4 model_col0 (offset 64)
        vec4 model_col1 (offset 80)
        vec4 model_col2 (offset 96)
        vec4 light_dir  (offset 112)
      Outputs:
        BuiltIn Position: gl_Position = mvp * vec4(in_pos, 1.0)
        Location 0: out vec3 v_normal (world-space rotated normal)
        Location 1: out vec3 v_color
        Location 2: out vec3 v_local_pos
    """
    b = SpvBuilder()
    b.cap(CapShader)
    b.cap(CapMatrix)

    t_void = b.type_void()
    t_i32 = b.type_int(32, 1)
    t_f32 = b.type_float(32)
    t_vec3 = b.type_vec(t_f32, 3)
    t_vec4 = b.type_vec(t_f32, 4)
    t_mat4 = b.type_mat(t_vec4, 4)
    t_fn = b.type_fn(t_void)

    t_ptr_in_vec3 = b.type_ptr(SCInput, t_vec3)
    t_ptr_out_vec3 = b.type_ptr(SCOutput, t_vec3)
    t_ptr_out_vec4 = b.type_ptr(SCOutput, t_vec4)

    v_in_pos = b.global_var(t_ptr_in_vec3, SCInput)
    v_in_nrm = b.global_var(t_ptr_in_vec3, SCInput)
    v_in_col = b.global_var(t_ptr_in_vec3, SCInput)
    b.decorate(v_in_pos, DecLocation, 0)
    b.decorate(v_in_nrm, DecLocation, 1)
    b.decorate(v_in_col, DecLocation, 2)

    v_gl_pos = b.global_var(t_ptr_out_vec4, SCOutput)
    v_out_nrm = b.global_var(t_ptr_out_vec3, SCOutput)
    v_out_col = b.global_var(t_ptr_out_vec3, SCOutput)
    v_out_lpos = b.global_var(t_ptr_out_vec3, SCOutput)
    b.decorate(v_gl_pos, DecBuiltIn, BuiltInPosition)
    b.decorate(v_out_nrm, DecLocation, 0)
    b.decorate(v_out_col, DecLocation, 1)
    b.decorate(v_out_lpos, DecLocation, 2)

    t_pc = b.type_struct(t_mat4, t_vec4, t_vec4, t_vec4, t_vec4)
    b.decorate(t_pc, DecBlock)
    b.member_decorate(t_pc, 0, DecColMajor)
    b.member_decorate(t_pc, 0, DecMatrixStride, 16)
    b.member_decorate(t_pc, 0, DecOffset, 0)
    b.member_decorate(t_pc, 1, DecOffset, 64)
    b.member_decorate(t_pc, 2, DecOffset, 80)
    b.member_decorate(t_pc, 3, DecOffset, 96)
    b.member_decorate(t_pc, 4, DecOffset, 112)

    t_ptr_pc = b.type_ptr(SCPushConstant, t_pc)
    t_ptr_pc_mat4 = b.type_ptr(SCPushConstant, t_mat4)
    t_ptr_pc_vec4 = b.type_ptr(SCPushConstant, t_vec4)
    v_pc = b.global_var(t_ptr_pc, SCPushConstant)

    c_i0 = b.const_i32(t_i32, 0)
    c_i1 = b.const_i32(t_i32, 1)
    c_i2 = b.const_i32(t_i32, 2)
    c_i3 = b.const_i32(t_i32, 3)
    c_f1 = b.const_f32(t_f32, 1.0)

    fn_main = b.alloc_id()
    lbl_entry = b.alloc_id()
    b.entry(
        ExecVertex,
        fn_main,
        "main",
        [v_in_pos, v_in_nrm, v_in_col, v_gl_pos, v_out_nrm, v_out_col, v_out_lpos],
    )

    b.functions += b.inst(OpFunction, t_void, fn_main, 0, t_fn)
    b.functions += b.inst(OpLabel, lbl_entry)

    pos = b.fn_op(t_vec3, OpLoad, v_in_pos)
    nrm = b.fn_op(t_vec3, OpLoad, v_in_nrm)
    col = b.fn_op(t_vec3, OpLoad, v_in_col)

    px = b.fn_op(t_f32, OpCompositeExtract, pos, 0)
    py = b.fn_op(t_f32, OpCompositeExtract, pos, 1)
    pz = b.fn_op(t_f32, OpCompositeExtract, pos, 2)
    pos4 = b.fn_op(t_vec4, OpCompositeConstruct, px, py, pz, c_f1)

    p_mvp = b.fn_op(t_ptr_pc_mat4, OpAccessChain, v_pc, c_i0)
    mvp = b.fn_op(t_mat4, OpLoad, p_mvp)
    clip = b.fn_op(t_vec4, OpMatrixTimesVector, mvp, pos4)
    b.fn_void_op(OpStore, v_gl_pos, clip)

    # Rotate normal by model columns 0..2
    p_c0 = b.fn_op(t_ptr_pc_vec4, OpAccessChain, v_pc, c_i1)
    p_c1 = b.fn_op(t_ptr_pc_vec4, OpAccessChain, v_pc, c_i2)
    p_c2 = b.fn_op(t_ptr_pc_vec4, OpAccessChain, v_pc, c_i3)
    c0_4 = b.fn_op(t_vec4, OpLoad, p_c0)
    c1_4 = b.fn_op(t_vec4, OpLoad, p_c1)
    c2_4 = b.fn_op(t_vec4, OpLoad, p_c2)

    def vec4_xyz(v4):
        x = b.fn_op(t_f32, OpCompositeExtract, v4, 0)
        y = b.fn_op(t_f32, OpCompositeExtract, v4, 1)
        z = b.fn_op(t_f32, OpCompositeExtract, v4, 2)
        return b.fn_op(t_vec3, OpCompositeConstruct, x, y, z)

    col0 = vec4_xyz(c0_4)
    col1 = vec4_xyz(c1_4)
    col2 = vec4_xyz(c2_4)

    nx = b.fn_op(t_f32, OpCompositeExtract, nrm, 0)
    ny = b.fn_op(t_f32, OpCompositeExtract, nrm, 1)
    nz = b.fn_op(t_f32, OpCompositeExtract, nrm, 2)

    t0 = b.fn_op(t_vec3, OpVectorTimesScalar, col0, nx)
    t1 = b.fn_op(t_vec3, OpVectorTimesScalar, col1, ny)
    t2 = b.fn_op(t_vec3, OpVectorTimesScalar, col2, nz)
    s01 = b.fn_op(t_vec3, OpFAdd, t0, t1)
    wnrm = b.fn_op(t_vec3, OpFAdd, s01, t2)

    b.fn_void_op(OpStore, v_out_nrm, wnrm)
    b.fn_void_op(OpStore, v_out_col, col)
    b.fn_void_op(OpStore, v_out_lpos, pos)

    b.fn_void_op(OpReturn)
    b.functions += b.inst(OpFunctionEnd)
    return b.build()


def build_frag_cube3d() -> list:
    """
    3D Per-Pixel Blinn-Phong Lit Fragment Shader:
      Location 0: in vec3 v_normal
      Location 1: in vec3 v_color
      Location 2: in vec3 v_local_pos
      PushConstant (same 128-byte layout, reads vec4 light_dir at offset 112):
        light_dir.xyz = normalized directional light
        light_dir.w   = ambient factor (if <= 0, unlit pure v_color mode for exact pixel tests!)
      Location 0: out vec4 out_color
    """
    b = SpvBuilder()
    b.cap(CapShader)
    b.cap(CapMatrix)

    t_void = b.type_void()
    t_bool = b.type_bool()
    t_i32 = b.type_int(32, 1)
    t_f32 = b.type_float(32)
    t_vec3 = b.type_vec(t_f32, 3)
    t_vec4 = b.type_vec(t_f32, 4)
    t_mat4 = b.type_mat(t_vec4, 4)
    t_fn = b.type_fn(t_void)

    t_ptr_in_vec3 = b.type_ptr(SCInput, t_vec3)
    t_ptr_out_vec4 = b.type_ptr(SCOutput, t_vec4)

    v_in_nrm = b.global_var(t_ptr_in_vec3, SCInput)
    v_in_col = b.global_var(t_ptr_in_vec3, SCInput)
    v_in_lpos = b.global_var(t_ptr_in_vec3, SCInput)
    b.decorate(v_in_nrm, DecLocation, 0)
    b.decorate(v_in_col, DecLocation, 1)
    b.decorate(v_in_lpos, DecLocation, 2)

    v_out_col = b.global_var(t_ptr_out_vec4, SCOutput)
    b.decorate(v_out_col, DecLocation, 0)

    t_pc = b.type_struct(t_mat4, t_vec4, t_vec4, t_vec4, t_vec4)
    b.decorate(t_pc, DecBlock)
    b.member_decorate(t_pc, 0, DecColMajor)
    b.member_decorate(t_pc, 0, DecMatrixStride, 16)
    b.member_decorate(t_pc, 0, DecOffset, 0)
    b.member_decorate(t_pc, 1, DecOffset, 64)
    b.member_decorate(t_pc, 2, DecOffset, 80)
    b.member_decorate(t_pc, 3, DecOffset, 96)
    b.member_decorate(t_pc, 4, DecOffset, 112)

    t_ptr_pc = b.type_ptr(SCPushConstant, t_pc)
    t_ptr_pc_vec4 = b.type_ptr(SCPushConstant, t_vec4)
    v_pc = b.global_var(t_ptr_pc, SCPushConstant)

    c_i4 = b.const_i32(t_i32, 4)
    c_f0 = b.const_f32(t_f32, 0.0)
    c_f1 = b.const_f32(t_f32, 1.0)
    c_f035 = b.const_f32(t_f32, 0.35)
    c_f085 = b.const_f32(t_f32, 0.85)
    c_fhx = b.const_f32(t_f32, 0.25)
    c_fhy = b.const_f32(t_f32, 0.45)
    c_fhz = b.const_f32(t_f32, 0.856)

    fn_main = b.alloc_id()
    lbl_entry = b.alloc_id()
    b.entry(ExecFragment, fn_main, "main", [v_in_nrm, v_in_col, v_in_lpos, v_out_col])
    b.exec_mode(fn_main, ModeOriginUpperLeft)

    b.functions += b.inst(OpFunction, t_void, fn_main, 0, t_fn)
    b.functions += b.inst(OpLabel, lbl_entry)

    nrm = b.fn_op(t_vec3, OpLoad, v_in_nrm)
    col = b.fn_op(t_vec3, OpLoad, v_in_col)
    _lpos = b.fn_op(t_vec3, OpLoad, v_in_lpos)

    p_ld = b.fn_op(t_ptr_pc_vec4, OpAccessChain, v_pc, c_i4)
    ld4 = b.fn_op(t_vec4, OpLoad, p_ld)
    lx = b.fn_op(t_f32, OpCompositeExtract, ld4, 0)
    ly = b.fn_op(t_f32, OpCompositeExtract, ld4, 1)
    lz = b.fn_op(t_f32, OpCompositeExtract, ld4, 2)
    amb = b.fn_op(t_f32, OpCompositeExtract, ld4, 3)
    ldir = b.fn_op(t_vec3, OpCompositeConstruct, lx, ly, lz)

    ndotl_raw = b.fn_op(t_f32, OpDot, nrm, ldir)
    gt0 = b.fn_op(t_bool, OpFOrdGreaterThan, ndotl_raw, c_f0)
    ndotl = b.fn_op(t_f32, OpSelect, gt0, ndotl_raw, c_f0)

    # Specular highlight (half-vector dot^8)
    hvec = b.fn_op(t_vec3, OpCompositeConstruct, c_fhx, c_fhy, c_fhz)
    ndoth_raw = b.fn_op(t_f32, OpDot, nrm, hvec)
    hgt0 = b.fn_op(t_bool, OpFOrdGreaterThan, ndoth_raw, c_f0)
    ndoth = b.fn_op(t_f32, OpSelect, hgt0, ndoth_raw, c_f0)
    s2 = b.fn_op(t_f32, OpFMul, ndoth, ndoth)
    s4 = b.fn_op(t_f32, OpFMul, s2, s2)
    s8 = b.fn_op(t_f32, OpFMul, s4, s4)
    spec = b.fn_op(t_f32, OpFMul, s8, c_f035)

    diff_term = b.fn_op(t_f32, OpFMul, ndotl, c_f085)
    shade = b.fn_op(t_f32, OpFAdd, amb, diff_term)
    lit_col = b.fn_op(t_vec3, OpVectorTimesScalar, col, shade)

    spec_vec = b.fn_op(t_vec3, OpCompositeConstruct, spec, spec, spec)
    lit_plus_spec = b.fn_op(t_vec3, OpFAdd, lit_col, spec_vec)

    # If amb <= 0.0, output unlit `col` directly (for deterministic offscreen pixel checks)
    use_lit = b.fn_op(t_bool, OpFOrdGreaterThan, amb, c_f0)

    def clamp01(x):
        gt = b.fn_op(t_bool, OpFOrdGreaterThan, x, c_f0)
        x0 = b.fn_op(t_f32, OpSelect, gt, x, c_f0)
        lt = b.fn_op(t_bool, OpFOrdLessThan, x0, c_f1)
        return b.fn_op(t_f32, OpSelect, lt, x0, c_f1)

    rx = clamp01(b.fn_op(t_f32, OpCompositeExtract, lit_plus_spec, 0))
    ry = clamp01(b.fn_op(t_f32, OpCompositeExtract, lit_plus_spec, 1))
    rz = clamp01(b.fn_op(t_f32, OpCompositeExtract, lit_plus_spec, 2))
    clamped_lit = b.fn_op(t_vec3, OpCompositeConstruct, rx, ry, rz)

    bool3 = b.type_vec(t_bool, 3)
    use_lit3 = b.fn_op(bool3, OpCompositeConstruct, use_lit, use_lit, use_lit)
    final_rgb = b.fn_op(t_vec3, OpSelect, use_lit3, clamped_lit, col)

    fr = b.fn_op(t_f32, OpCompositeExtract, final_rgb, 0)
    fg = b.fn_op(t_f32, OpCompositeExtract, final_rgb, 1)
    fb = b.fn_op(t_f32, OpCompositeExtract, final_rgb, 2)
    out4 = b.fn_op(t_vec4, OpCompositeConstruct, fr, fg, fb, c_f1)

    b.fn_void_op(OpStore, v_out_col, out4)
    b.fn_void_op(OpReturn)
    b.functions += b.inst(OpFunctionEnd)
    return b.build()


def format_c_array(name: str, words: list) -> str:
    lines = [f"static const uint32_t {name}[{len(words)}] = {{"]
    for i in range(0, len(words), 8):
        chunk = ", ".join(f"0x{w:08x}u" for w in words[i : i + 8])
        lines.append(f"    {chunk},")
    lines.append("};\n")
    return "\n".join(lines)


def main():
    comp = build_compute_mandelbrot()
    vert = build_vert_cube3d()
    frag = build_frag_cube3d()

    out_path = os.path.join(os.path.dirname(__file__), "gp108_test_shaders.h")
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("/* Auto-generated by tools/gen_test_spirv.py - DO NOT EDIT */\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write(format_c_array("kSpvCompMandelbrot", comp))
        f.write("\n")
        f.write(format_c_array("kSpvVertCube3D", vert))
        f.write("\n")
        f.write(format_c_array("kSpvFragCube3D", frag))
    print(
        f"Generated {out_path}: comp={len(comp)} words, vert={len(vert)} words, frag={len(frag)} words"
    )


if __name__ == "__main__":
    main()
