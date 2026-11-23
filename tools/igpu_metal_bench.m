/*
 * tools/igpu_metal_bench.m
 *
 * Benchmarks Intel UHD Graphics 620 (WindowServer Metal GPU) on two workloads:
 *   1. Sudden Idle -> UI Animation Burst (first 15 frames of 16-pass blur/glass
 *      compositing at 1920x1080 — directly measures RPS / AGPM clock ramp speed!)
 *   2. Sustained Compositing Throughput (120 frames of 16-pass shader compositing)
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

static NSString *const kBenchShaderSrc =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct VSOut {\n"
     "    float4 pos [[position]];\n"
     "    float2 uv;\n"
     "};\n"
     "vertex VSOut vs_fullscreen(uint vid [[vertex_id]]) {\n"
     "    float2 p = float2((vid << 1) & 2, vid & 2);\n"
     "    VSOut o;\n"
     "    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);\n"
     "    o.uv  = p;\n"
     "    return o;\n"
     "}\n"
     "fragment float4 fs_glass_blur(VSOut in [[stage_in]],\n"
     "                              constant float4 &params [[buffer(0)]]) {\n"
     "    float2 uv = in.uv;\n"
     "    float t = params.x;\n"
     "    float3 acc = float3(0.0);\n"
     "    /* Simulate WindowServer Vibrancy / Gaussian blur + rounded-rect SDF */\n"
     "    for (int i = 0; i < 24; i++) {\n"
     "        float fi = float(i) * 0.261799 + t;\n"
     "        float2 off = float2(cos(fi), sin(fi)) * (0.004 * float((i & 3) + 1));\n"
     "        float2 p = uv + off;\n"
     "        acc += float3(0.5 + 0.5 * sin(p.x * 18.0 + t),\n"
     "                      0.5 + 0.5 * cos(p.y * 14.0 - t),\n"
     "                      0.5 + 0.5 * sin((p.x + p.y) * 12.0));\n"
     "    }\n"
     "    acc /= 24.0;\n"
     "    return float4(acc, 0.85);\n"
     "}\n";

__attribute__((visibility("default")))
int run_igpu_metal_bench(void) {
    @autoreleasepool {
        id<MTLDevice> dev = [MTLCreateSystemDefaultDevice() autorelease];
        if (!dev) {
            fprintf(stderr, "No Metal device found!\n");
            return -1;
        }
        NSError *err = nil;
        id<MTLLibrary> lib = [[dev newLibraryWithSource:kBenchShaderSrc options:nil error:&err] autorelease];
        if (!lib) {
            fprintf(stderr, "Metal shader compile error: %s\n", [[err localizedDescription] UTF8String]);
            return -1;
        }

        MTLRenderPipelineDescriptor *pd = [[[MTLRenderPipelineDescriptor alloc] init] autorelease];
        pd.vertexFunction = [[lib newFunctionWithName:@"vs_fullscreen"] autorelease];
        pd.fragmentFunction = [[lib newFunctionWithName:@"fs_glass_blur"] autorelease];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        pd.colorAttachments[0].blendingEnabled = YES;
        pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

        id<MTLRenderPipelineState> pso = [[dev newRenderPipelineStateWithDescriptor:pd error:&err] autorelease];
        if (!pso) return -1;

        MTLTextureDescriptor *td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                         width:1920
                                        height:1080
                                     mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> rt = [[dev newTextureWithDescriptor:td] autorelease];
        id<MTLCommandQueue> q = [[dev newCommandQueue] autorelease];

        /* Warm pipeline state once, then let iGPU settle to idle for 600ms */
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
            rpd.colorAttachments[0].texture = rt;
            rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
            rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rpd];
            [enc setRenderPipelineState:pso];
            float p[4] = { 0.1f, 0, 0, 0 };
            [enc setFragmentBytes:p length:sizeof(p) atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
        }
        usleep(600000);

        /* Test 1: Sudden 15-frame UI Animation Burst from Idle (12 layers/frame @ 1080p) */
        double t_burst0 = now_ms();
        for (int f = 0; f < 15; f++) {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
            rpd.colorAttachments[0].texture = rt;
            rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
            rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rpd];
            [enc setRenderPipelineState:pso];
            for (int layer = 0; layer < 12; layer++) {
                float p[4] = { (float)(f * 12 + layer) * 0.05f, 0, 0, 0 };
                [enc setFragmentBytes:p length:sizeof(p) atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            }
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
        }
        double burst_ms = now_ms() - t_burst0;
        double burst_fps = 15000.0 / burst_ms;

        /* Test 2: Sustained 90-frame Heavy Blur Compositing (12 layers/frame @ 1080p) */
        double t_sus0 = now_ms();
        for (int f = 0; f < 90; f++) {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
            rpd.colorAttachments[0].texture = rt;
            rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
            rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rpd];
            [enc setRenderPipelineState:pso];
            for (int layer = 0; layer < 12; layer++) {
                float p[4] = { (float)(f * 12 + layer) * 0.03f, 0, 0, 0 };
                [enc setFragmentBytes:p length:sizeof(p) atIndex:0];
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            }
            [enc endEncoding];
            [cb commit];
            if ((f & 1) == 1) [cb waitUntilCompleted];
        }
        id<MTLCommandBuffer> last = [q commandBuffer];
        [last commit];
        [last waitUntilCompleted];
        double sus_ms = now_ms() - t_sus0;
        double sus_fps = 90000.0 / sus_ms;

        printf("=== Intel iGPU Metal WindowServer-Blur Benchmark ===\n");
        printf("    Device             : %s (Max Working Set: %llu MiB)\n",
               [[dev name] UTF8String],
               (unsigned long long)([dev recommendedMaxWorkingSetSize] >> 20));
        printf("    Idle->Burst (15f)  : %.2f FPS (%.2f ms/frame, total %.1f ms)\n",
               burst_fps, burst_ms / 15.0, burst_ms);
        printf("    Sustained (90f)    : %.2f FPS (%.2f ms/frame, total %.1f ms)\n",
               sus_fps, sus_ms / 90.0, sus_ms);
        return 0;
    }
}
