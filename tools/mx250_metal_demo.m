/* SPDX-License-Identifier: MIT
 * tools/mx250_metal_demo.m -> build/mx250-metal-demo
 *
 * 100% Pure Native Apple Metal.framework Compute + 3D CAMetalLayer App
 * (Zero Vulkan headers — runs unmodified on Intel Metal OR on MX250 via mx250-metal-run!)
 */
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <math.h>
#include <stdio.h>
#include <time.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static NSString *const kMSLSource = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"\n"
"struct ComputeParams { float mul; float add; uint iters; uint pad; };\n"
"kernel void bench_ffma(device float4 *buf [[buffer(0)]],\n"
"                       constant ComputeParams &p [[buffer(1)]],\n"
"                       uint gid [[thread_position_in_grid]]) {\n"
"    float4 a = float4(float(gid & 255u) * 0.001f + 1.0f);\n"
"    float4 b = a + 0.25f, c = a + 0.50f, d = a + 0.75f;\n"
"    float4 m = float4(p.mul), k = float4(p.add);\n"
"    for (uint i = 0; i < p.iters; i++) {\n"
"        a = fma(a, m, k); b = fma(b, m, k);\n"
"        c = fma(c, m, k); d = fma(d, m, k);\n"
"        a = fma(a, m, k); b = fma(b, m, k);\n"
"        c = fma(c, m, k); d = fma(d, m, k);\n"
"    }\n"
"    buf[gid] = a + b + c + d;\n"
"}\n"
"\n"
"struct Vtx { packed_float3 pos; packed_float3 nrm; packed_float3 col; };\n"
"struct UBO { float4x4 mvp; float4 light; };\n"
"struct VOut { float4 pos [[position]]; float4 col; };\n"
"\n"
"vertex VOut vs_cube(device const Vtx *v [[buffer(0)]],\n"
"                    constant UBO &ubo [[buffer(1)]],\n"
"                    uint vid [[vertex_id]]) {\n"
"    VOut o;\n"
"    o.pos = ubo.mvp * float4(float3(v[vid].pos), 1.0f);\n"
"    float ndl = max(dot(normalize(float3(v[vid].nrm)), normalize(ubo.light.xyz)), 0.18f);\n"
"    o.col = float4(float3(v[vid].col) * ndl, 1.0f);\n"
"    return o;\n"
"}\n"
"\n"
"fragment float4 fs_cube(VOut in [[stage_in]]) {\n"
"    return in.col;\n"
"}\n";

typedef struct { float pos[3]; float nrm[3]; float col[3]; } CubeVtx;
static const CubeVtx kCube[36] = {
    {{-1,-1, 1},{0,0,1},{0.46f,0.88f,0.18f}}, {{ 1,-1, 1},{0,0,1},{0.46f,0.88f,0.18f}}, {{ 1, 1, 1},{0,0,1},{0.46f,0.88f,0.18f}},
    {{-1,-1, 1},{0,0,1},{0.46f,0.88f,0.18f}}, {{ 1, 1, 1},{0,0,1},{0.46f,0.88f,0.18f}}, {{-1, 1, 1},{0,0,1},{0.46f,0.88f,0.18f}},
    {{ 1,-1,-1},{0,0,-1},{0.18f,0.82f,0.92f}}, {{-1,-1,-1},{0,0,-1},{0.18f,0.82f,0.92f}}, {{-1, 1,-1},{0,0,-1},{0.18f,0.82f,0.92f}},
    {{ 1,-1,-1},{0,0,-1},{0.18f,0.82f,0.92f}}, {{-1, 1,-1},{0,0,-1},{0.18f,0.82f,0.92f}}, {{ 1, 1,-1},{0,0,-1},{0.18f,0.82f,0.92f}},
    {{ 1,-1, 1},{1,0,0},{0.96f,0.32f,0.28f}}, {{ 1,-1,-1},{1,0,0},{0.96f,0.32f,0.28f}}, {{ 1, 1,-1},{1,0,0},{0.96f,0.32f,0.28f}},
    {{ 1,-1, 1},{1,0,0},{0.96f,0.32f,0.28f}}, {{ 1, 1,-1},{1,0,0},{0.96f,0.32f,0.28f}}, {{ 1, 1, 1},{1,0,0},{0.96f,0.32f,0.28f}},
    {{-1,-1,-1},{-1,0,0},{0.98f,0.76f,0.18f}}, {{-1,-1, 1},{-1,0,0},{0.98f,0.76f,0.18f}}, {{-1, 1, 1},{-1,0,0},{0.98f,0.76f,0.18f}},
    {{-1,-1,-1},{-1,0,0},{0.98f,0.76f,0.18f}}, {{-1, 1, 1},{-1,0,0},{0.98f,0.76f,0.18f}}, {{-1, 1,-1},{-1,0,0},{0.98f,0.76f,0.18f}},
    {{-1, 1, 1},{0,1,0},{0.28f,0.54f,0.98f}}, {{ 1, 1, 1},{0,1,0},{0.28f,0.54f,0.98f}}, {{ 1, 1,-1},{0,1,0},{0.28f,0.54f,0.98f}},
    {{-1, 1, 1},{0,1,0},{0.28f,0.54f,0.98f}}, {{ 1, 1,-1},{0,1,0},{0.28f,0.54f,0.98f}}, {{-1, 1,-1},{0,1,0},{0.28f,0.54f,0.98f}},
    {{-1,-1,-1},{0,-1,0},{0.84f,0.32f,0.94f}}, {{ 1,-1,-1},{0,-1,0},{0.84f,0.32f,0.94f}}, {{ 1,-1, 1},{0,-1,0},{0.84f,0.32f,0.94f}},
    {{-1,-1,-1},{0,-1,0},{0.84f,0.32f,0.94f}}, {{ 1,-1, 1},{0,-1,0},{0.84f,0.32f,0.94f}}, {{-1,-1, 1},{0,-1,0},{0.84f,0.32f,0.94f}},
};

static void make_mvp(float out[16], float aspect, float t, float tx, float ty, float tz, float sc) {
    float f = 1.0f / tanf(45.0f * 0.5f * 3.14159265f / 180.0f);
    float zn = 0.1f, zf = 50.0f;
    float p[16] = {0};
    p[0] = f / aspect; p[5] = f; p[10] = zf / (zn - zf); p[11] = -1.0f; p[14] = (zn * zf) / (zn - zf);
    float cy = cosf(t), sy = sinf(t), cx = cosf(t * 0.7f), sx = sinf(t * 0.7f);
    float m[16] = {
        cy * sc, sx * sy * sc, -cx * sy * sc, 0,
        0,       cx * sc,       sx * sc,      0,
        sy * sc,-sx * cy * sc,  cx * cy * sc, 0,
        tx,      ty,            tz,           1
    };
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            out[c*4+r] = p[0*4+r]*m[c*4+0] + p[1*4+r]*m[c*4+1] + p[2*4+r]*m[c*4+2] + p[3*4+r]*m[c*4+3];
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        double win_secs = (argc > 1) ? atof(argv[1]) : 3.0;
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { fprintf(stderr, "MTLCreateSystemDefaultDevice returned nil!\n"); return 1; }
        printf("=== Native Apple Metal.framework App ===\n");
        printf("    Active MTLDevice : %s (registryID=0x%llx)\n", [[dev name] UTF8String], dev.registryID);

        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kMSLSource options:nil error:&err];
        if (!lib) { fprintf(stderr, "MSL compile error: %s\n", [[err localizedDescription] UTF8String]); return 1; }

        /* 1. Metal Compute FP32 Throughput Benchmark */
        id<MTLComputePipelineState> cpso = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"bench_ffma"] error:&err];
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        const uint32_t nThreads = 262144;
        id<MTLBuffer> cbuf = [dev newBufferWithLength:nThreads * 16 options:MTLResourceStorageModePrivate];
        id<MTLBuffer> rbuf = [dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
        struct { float mul; float add; uint32_t iters; uint32_t pad; } cp = { 0.999999f, 0.000001f, 8192u, 0u };

        /* Warmup */
        {
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:cpso];
            [ce setBuffer:cbuf offset:0 atIndex:0];
            [ce setBytes:&cp length:sizeof(cp) atIndex:1];
            [ce dispatchThreadgroups:MTLSizeMake(nThreads / 256, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
            [ce endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
        }

        double t0 = now_sec();
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:cpso];
        [ce setBuffer:cbuf offset:0 atIndex:0];
        [ce setBytes:&cp length:sizeof(cp) atIndex:1];
        [ce dispatchThreadgroups:MTLSizeMake(nThreads / 256, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        [ce endEncoding];
        id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
        [be copyFromBuffer:cbuf sourceOffset:0 toBuffer:rbuf destinationOffset:0 size:64];
        [be endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        double dt = now_sec() - t0;
        /* 8 float4 FMAs/iter = 64 FLOPs/iter * 8192 iters * 262144 threads = 137.438953472 GFLOP */
        double gflops = ((double)nThreads * (double)cp.iters * 64.0) / (dt * 1e9);
        const float *out_f = (const float *)[rbuf contents];
        printf("    Metal Compute    : %.2f GFLOPS (%.2f ms, verify out[0]=%.4f)\n", gflops, dt * 1e3, out_f[0]);

        if (win_secs <= 0.0) return 0;

        /* 2. Live 3D CAMetalLayer Windowed Rendering */
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        const uint32_t W = 960, H = 640;
        NSWindow *win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, W, H)
                                                    styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable)
                                                      backing:NSBackingStoreBuffered defer:NO];
        [win center];
        NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, W, H)];
        view.wantsLayer = YES;
        CAMetalLayer *mlayer = [CAMetalLayer layer];
        mlayer.device = dev;
        mlayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        mlayer.contentsScale = 1.0;
        mlayer.bounds = view.bounds;
        mlayer.drawableSize = CGSizeMake(W, H);
        view.layer = mlayer;
        [win setContentView:view];
        [win makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];

        MTLRenderPipelineDescriptor *rpd = [[MTLRenderPipelineDescriptor alloc] init];
        rpd.vertexFunction = [lib newFunctionWithName:@"vs_cube"];
        rpd.fragmentFunction = [lib newFunctionWithName:@"fs_cube"];
        rpd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        rpd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        id<MTLRenderPipelineState> rpso = [dev newRenderPipelineStateWithDescriptor:rpd error:&err];

        MTLDepthStencilDescriptor *dsd = [[MTLDepthStencilDescriptor alloc] init];
        dsd.depthCompareFunction = MTLCompareFunctionLess;
        dsd.depthWriteEnabled = YES;
        id<MTLDepthStencilState> dss = [dev newDepthStencilStateWithDescriptor:dsd];

        MTLTextureDescriptor *dtd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:W height:H mipmapped:NO];
        dtd.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> depthTex = [dev newTextureWithDescriptor:dtd];
        id<MTLBuffer> vbuf = [dev newBufferWithBytes:kCube length:sizeof(kCube) options:MTLResourceStorageModeShared];

        double start = now_sec(), lastTitle = start;
        uint64_t frames = 0, winFrames = 0;
        while (now_sec() - start < win_secs) {
            NSEvent *ev;
            while ((ev = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast] inMode:NSDefaultRunLoopMode dequeue:YES]))
                [NSApp sendEvent:ev];

            id<CAMetalDrawable> drw = [mlayer nextDrawable];
            if (!drw) continue;

            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = drw.texture;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0.05, 0.07, 0.11, 1.0);
            pass.depthAttachment.texture = depthTex;
            pass.depthAttachment.loadAction = MTLLoadActionClear;
            pass.depthAttachment.storeAction = MTLStoreActionDontCare;
            pass.depthAttachment.clearDepth = 1.0;

            id<MTLCommandBuffer> rcb = [queue commandBuffer];
            id<MTLRenderCommandEncoder> re = [rcb renderCommandEncoderWithDescriptor:pass];
            [re setRenderPipelineState:rpso];
            [re setDepthStencilState:dss];
            [re setVertexBuffer:vbuf offset:0 atIndex:0];

            float t = (float)(now_sec() - start);
            struct { float mvp[16]; float light[4]; } u = { .light = { 0.45f, 0.75f, 0.65f, 0.0f } };
            make_mvp(u.mvp, (float)W / (float)H, t * 1.4f,  0.0f, 0.0f, -4.5f, 1.15f);
            [re setVertexBytes:&u length:sizeof(u) atIndex:1];
            [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:36];

            make_mvp(u.mvp, (float)W / (float)H, -t * 2.1f, -2.3f, 0.4f, -5.2f, 0.55f);
            [re setVertexBytes:&u length:sizeof(u) atIndex:1];
            [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:36];

            make_mvp(u.mvp, (float)W / (float)H, t * 2.4f,  2.3f, -0.4f, -5.2f, 0.55f);
            [re setVertexBytes:&u length:sizeof(u) atIndex:1];
            [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:36];

            [re endEncoding];
            [rcb presentDrawable:drw];
            [rcb commit];

            frames++; winFrames++;
            double now = now_sec();
            if (now - lastTitle >= 0.25) {
                double fps = (double)winFrames / (now - lastTitle);
                [win setTitle:[NSString stringWithFormat:@"%@ | Native Metal App -> Zero-Copy IOSurface | %.1f FPS", [dev name], fps]];
                winFrames = 0; lastTitle = now;
            }
        }
        double total = now_sec() - start;
        printf("    Metal 3D Window  : %llu frames in %.2f s -> %.2f FPS (%.2f ms/frame)\n",
               (unsigned long long)frames, total, (double)frames / total, 1000.0 * total / (double)frames);
    }
    return 0;
}
