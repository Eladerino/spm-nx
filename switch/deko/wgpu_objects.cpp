// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of
// the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
// The format table, texture layouts, views, samplers and pipeline state conversions are adapted from
// SwitchWaker (https://github.com/centollOS/SwitchWaker) at 92307ff: switch/deko/aurora/dk_objects.cpp
// and dk_pipeline.cpp.
// The shadows of Aurora's WebGPU objects (wgpu_objects.hpp): the __wrap_ functions that create them,
// count Aurora's references, and give Aurora the buffers' memory when it maps one.
#include "wgpu_objects.hpp"

#include <algorithm>
#include <cstring>

extern "C" {
WGPUTexture __real_wgpuDeviceCreateTexture(WGPUDevice device, WGPUTextureDescriptor const* descriptor);
void __real_wgpuTextureAddRef(WGPUTexture texture);
void __real_wgpuTextureRelease(WGPUTexture texture);
WGPUTextureView __real_wgpuTextureCreateView(WGPUTexture texture, WGPUTextureViewDescriptor const* descriptor);
void __real_wgpuTextureViewAddRef(WGPUTextureView view);
void __real_wgpuTextureViewRelease(WGPUTextureView view);
WGPUSampler __real_wgpuDeviceCreateSampler(WGPUDevice device, WGPUSamplerDescriptor const* descriptor);
void __real_wgpuSamplerAddRef(WGPUSampler sampler);
void __real_wgpuSamplerRelease(WGPUSampler sampler);
WGPUBuffer __real_wgpuDeviceCreateBuffer(WGPUDevice device, WGPUBufferDescriptor const* descriptor);
void __real_wgpuBufferAddRef(WGPUBuffer buffer);
void __real_wgpuBufferRelease(WGPUBuffer buffer);
WGPUFuture __real_wgpuBufferMapAsync(WGPUBuffer buffer, WGPUMapMode mode, size_t offset, size_t size,
                                     WGPUBufferMapCallbackInfo callbackInfo);
void* __real_wgpuBufferGetMappedRange(WGPUBuffer buffer, size_t offset, size_t size);
void const* __real_wgpuBufferGetConstMappedRange(WGPUBuffer buffer, size_t offset, size_t size);
WGPUBindGroupLayout __real_wgpuDeviceCreateBindGroupLayout(WGPUDevice device,
                                                           WGPUBindGroupLayoutDescriptor const* descriptor);
void __real_wgpuBindGroupLayoutAddRef(WGPUBindGroupLayout layout);
void __real_wgpuBindGroupLayoutRelease(WGPUBindGroupLayout layout);
WGPUBindGroup __real_wgpuDeviceCreateBindGroup(WGPUDevice device, WGPUBindGroupDescriptor const* descriptor);
void __real_wgpuBindGroupAddRef(WGPUBindGroup group);
void __real_wgpuBindGroupRelease(WGPUBindGroup group);
WGPUShaderModule __real_wgpuDeviceCreateShaderModule(WGPUDevice device, WGPUShaderModuleDescriptor const* descriptor);
void __real_wgpuShaderModuleAddRef(WGPUShaderModule module);
void __real_wgpuShaderModuleRelease(WGPUShaderModule module);
WGPURenderPipeline __real_wgpuDeviceCreateRenderPipeline(WGPUDevice device,
                                                         WGPURenderPipelineDescriptor const* descriptor);
void __real_wgpuRenderPipelineAddRef(WGPURenderPipeline pipeline);
void __real_wgpuRenderPipelineRelease(WGPURenderPipeline pipeline);
WGPUComputePipeline __real_wgpuDeviceCreateComputePipeline(WGPUDevice device,
                                                           WGPUComputePipelineDescriptor const* descriptor);
void __real_wgpuComputePipelineAddRef(WGPUComputePipeline pipeline);
void __real_wgpuComputePipelineRelease(WGPUComputePipeline pipeline);
}

namespace swdk::wg {

Registry<Tex> g_textures;
Registry<View> g_views;
Registry<Samp> g_samplers;
Registry<Buf> g_buffers;
Registry<BindGroupLayout> g_bindGroupLayouts;
Registry<BindGroup> g_bindGroups;
Registry<ShaderModule> g_shaderModules;
Registry<Pipe> g_renderPipelines;
Registry<Pipe> g_computePipelines;

namespace {

uint64_t fnv_bytes(const void* p, size_t n, uint64_t h = 0xCBF29CE484222325ull) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

DkImageSwizzle swizzle_of(WGPUComponentSwizzle s, DkImageSwizzle identity) {
    switch (s) {
    case WGPUComponentSwizzle_Zero: return DkImageSwizzle_Zero;
    case WGPUComponentSwizzle_One: return DkImageSwizzle_One;
    case WGPUComponentSwizzle_R: return DkImageSwizzle_Red;
    case WGPUComponentSwizzle_G: return DkImageSwizzle_Green;
    case WGPUComponentSwizzle_B: return DkImageSwizzle_Blue;
    case WGPUComponentSwizzle_A: return DkImageSwizzle_Alpha;
    default: return identity;
    }
}

DkWrapMode wrap_of(WGPUAddressMode m) {
    switch (m) {
    case WGPUAddressMode_Repeat: return DkWrapMode_Repeat;
    case WGPUAddressMode_MirrorRepeat: return DkWrapMode_MirroredRepeat;
    default: return DkWrapMode_ClampToEdge;  // ClampToEdge, Undefined (WebGPU's default)
    }
}

// WebGPU's and deko3d's comparison functions have the same numbers (Never 1 ... Always 8)
DkCompareOp compare_of(WGPUCompareFunction f) {
    if (f < WGPUCompareFunction_Never || f > WGPUCompareFunction_Always) return DkCompareOp_Always;
    return DkCompareOp(int(f));
}

// WebGPU: Keep 1, Zero 2, Replace 3, Invert 4, IncrementClamp 5, DecrementClamp 6, IncrementWrap 7,
// DecrementWrap 8; deko3d puts Invert after the clamped ones
DkStencilOp stencil_op_of(WGPUStencilOperation o) {
    switch (o) {
    case WGPUStencilOperation_Zero: return DkStencilOp_Zero;
    case WGPUStencilOperation_Replace: return DkStencilOp_Replace;
    case WGPUStencilOperation_Invert: return DkStencilOp_Invert;
    case WGPUStencilOperation_IncrementClamp: return DkStencilOp_Incr;
    case WGPUStencilOperation_DecrementClamp: return DkStencilOp_Decr;
    case WGPUStencilOperation_IncrementWrap: return DkStencilOp_IncrWrap;
    case WGPUStencilOperation_DecrementWrap: return DkStencilOp_DecrWrap;
    default: return DkStencilOp_Keep;
    }
}

DkBlendFactor blend_factor_of(WGPUBlendFactor f, bool alpha) {
    switch (f) {
    case WGPUBlendFactor_Zero: return DkBlendFactor_Zero;
    case WGPUBlendFactor_One: return DkBlendFactor_One;
    case WGPUBlendFactor_Src: return DkBlendFactor_SrcColor;
    case WGPUBlendFactor_OneMinusSrc: return DkBlendFactor_InvSrcColor;
    case WGPUBlendFactor_SrcAlpha: return DkBlendFactor_SrcAlpha;
    case WGPUBlendFactor_OneMinusSrcAlpha: return DkBlendFactor_InvSrcAlpha;
    case WGPUBlendFactor_Dst: return DkBlendFactor_DstColor;
    case WGPUBlendFactor_OneMinusDst: return DkBlendFactor_InvDstColor;
    case WGPUBlendFactor_DstAlpha: return DkBlendFactor_DstAlpha;
    case WGPUBlendFactor_OneMinusDstAlpha: return DkBlendFactor_InvDstAlpha;
    case WGPUBlendFactor_SrcAlphaSaturated: return DkBlendFactor_SrcAlphaSaturate;
    // WebGPU's constant is the blend constant's rgb for the colour, its a for the alpha
    case WGPUBlendFactor_Constant: return alpha ? DkBlendFactor_ConstAlpha : DkBlendFactor_ConstColor;
    case WGPUBlendFactor_OneMinusConstant: return alpha ? DkBlendFactor_InvConstAlpha : DkBlendFactor_InvConstColor;
    case WGPUBlendFactor_Src1: return DkBlendFactor_Src1Color;
    case WGPUBlendFactor_OneMinusSrc1: return DkBlendFactor_InvSrc1Color;
    case WGPUBlendFactor_Src1Alpha: return DkBlendFactor_Src1Alpha;
    case WGPUBlendFactor_OneMinusSrc1Alpha: return DkBlendFactor_InvSrc1Alpha;
    default: return DkBlendFactor_One;
    }
}

DkBlendOp blend_op_of(WGPUBlendOperation o) {
    switch (o) {
    case WGPUBlendOperation_Subtract: return DkBlendOp_Sub;
    case WGPUBlendOperation_ReverseSubtract: return DkBlendOp_RevSub;
    case WGPUBlendOperation_Min: return DkBlendOp_Min;
    case WGPUBlendOperation_Max: return DkBlendOp_Max;
    default: return DkBlendOp_Add;
    }
}

// A WebGPU vertex format as deko3d's (size, type); false when deko3d has none
bool vertex_format_of(WGPUVertexFormat f, DkVtxAttribSize& size, DkVtxAttribType& type, bool& bgra) {
    bgra = false;
    switch (f) {
    case WGPUVertexFormat_Uint8: size = DkVtxAttribSize_1x8; type = DkVtxAttribType_Uint; return true;
    case WGPUVertexFormat_Uint8x2: size = DkVtxAttribSize_2x8; type = DkVtxAttribType_Uint; return true;
    case WGPUVertexFormat_Uint8x4: size = DkVtxAttribSize_4x8; type = DkVtxAttribType_Uint; return true;
    case WGPUVertexFormat_Sint8: size = DkVtxAttribSize_1x8; type = DkVtxAttribType_Sint; return true;
    case WGPUVertexFormat_Sint8x2: size = DkVtxAttribSize_2x8; type = DkVtxAttribType_Sint; return true;
    case WGPUVertexFormat_Sint8x4: size = DkVtxAttribSize_4x8; type = DkVtxAttribType_Sint; return true;
    case WGPUVertexFormat_Unorm8: size = DkVtxAttribSize_1x8; type = DkVtxAttribType_Unorm; return true;
    case WGPUVertexFormat_Unorm8x2: size = DkVtxAttribSize_2x8; type = DkVtxAttribType_Unorm; return true;
    case WGPUVertexFormat_Unorm8x4: size = DkVtxAttribSize_4x8; type = DkVtxAttribType_Unorm; return true;
    case WGPUVertexFormat_Unorm8x4BGRA:
        size = DkVtxAttribSize_4x8;
        type = DkVtxAttribType_Unorm;
        bgra = true;
        return true;
    case WGPUVertexFormat_Snorm8: size = DkVtxAttribSize_1x8; type = DkVtxAttribType_Snorm; return true;
    case WGPUVertexFormat_Snorm8x2: size = DkVtxAttribSize_2x8; type = DkVtxAttribType_Snorm; return true;
    case WGPUVertexFormat_Snorm8x4: size = DkVtxAttribSize_4x8; type = DkVtxAttribType_Snorm; return true;
    case WGPUVertexFormat_Uint16: size = DkVtxAttribSize_1x16; type = DkVtxAttribType_Uint; return true;
    case WGPUVertexFormat_Uint16x2: size = DkVtxAttribSize_2x16; type = DkVtxAttribType_Uint; return true;
    case WGPUVertexFormat_Uint16x4: size = DkVtxAttribSize_4x16; type = DkVtxAttribType_Uint; return true;
    case WGPUVertexFormat_Sint16: size = DkVtxAttribSize_1x16; type = DkVtxAttribType_Sint; return true;
    case WGPUVertexFormat_Sint16x2: size = DkVtxAttribSize_2x16; type = DkVtxAttribType_Sint; return true;
    case WGPUVertexFormat_Sint16x4: size = DkVtxAttribSize_4x16; type = DkVtxAttribType_Sint; return true;
    case WGPUVertexFormat_Unorm16: size = DkVtxAttribSize_1x16; type = DkVtxAttribType_Unorm; return true;
    case WGPUVertexFormat_Unorm16x2: size = DkVtxAttribSize_2x16; type = DkVtxAttribType_Unorm; return true;
    case WGPUVertexFormat_Unorm16x4: size = DkVtxAttribSize_4x16; type = DkVtxAttribType_Unorm; return true;
    case WGPUVertexFormat_Snorm16: size = DkVtxAttribSize_1x16; type = DkVtxAttribType_Snorm; return true;
    case WGPUVertexFormat_Snorm16x2: size = DkVtxAttribSize_2x16; type = DkVtxAttribType_Snorm; return true;
    case WGPUVertexFormat_Snorm16x4: size = DkVtxAttribSize_4x16; type = DkVtxAttribType_Snorm; return true;
    case WGPUVertexFormat_Float16: size = DkVtxAttribSize_1x16; type = DkVtxAttribType_Float; return true;
    case WGPUVertexFormat_Float16x2: size = DkVtxAttribSize_2x16; type = DkVtxAttribType_Float; return true;
    case WGPUVertexFormat_Float16x4: size = DkVtxAttribSize_4x16; type = DkVtxAttribType_Float; return true;
    case WGPUVertexFormat_Float32: size = DkVtxAttribSize_1x32; type = DkVtxAttribType_Float; return true;
    case WGPUVertexFormat_Float32x2: size = DkVtxAttribSize_2x32; type = DkVtxAttribType_Float; return true;
    case WGPUVertexFormat_Float32x3: size = DkVtxAttribSize_3x32; type = DkVtxAttribType_Float; return true;
    case WGPUVertexFormat_Float32x4: size = DkVtxAttribSize_4x32; type = DkVtxAttribType_Float; return true;
    case WGPUVertexFormat_Uint32: size = DkVtxAttribSize_1x32; type = DkVtxAttribType_Uint; return true;
    case WGPUVertexFormat_Uint32x2: size = DkVtxAttribSize_2x32; type = DkVtxAttribType_Uint; return true;
    case WGPUVertexFormat_Uint32x3: size = DkVtxAttribSize_3x32; type = DkVtxAttribType_Uint; return true;
    case WGPUVertexFormat_Uint32x4: size = DkVtxAttribSize_4x32; type = DkVtxAttribType_Uint; return true;
    case WGPUVertexFormat_Sint32: size = DkVtxAttribSize_1x32; type = DkVtxAttribType_Sint; return true;
    case WGPUVertexFormat_Sint32x2: size = DkVtxAttribSize_2x32; type = DkVtxAttribType_Sint; return true;
    case WGPUVertexFormat_Sint32x3: size = DkVtxAttribSize_3x32; type = DkVtxAttribType_Sint; return true;
    case WGPUVertexFormat_Sint32x4: size = DkVtxAttribSize_4x32; type = DkVtxAttribType_Sint; return true;
    case WGPUVertexFormat_Unorm10_10_10_2:
        size = DkVtxAttribSize_10_10_10_2;
        type = DkVtxAttribType_Unorm;
        return true;
    default: return false;
    }
}

std::shared_ptr<Tex> make_texture(const WGPUTextureDescriptor& d) {
    auto t = std::make_shared<Tex>();
    t->wformat = d.format;
    t->width = std::max<uint32_t>(d.size.width, 1);
    t->height = std::max<uint32_t>(d.size.height, 1);
    t->layers = std::max<uint32_t>(d.size.depthOrArrayLayers, 1);
    t->mips = std::max<uint32_t>(d.mipLevelCount, 1);
    t->is3d = d.dimension == WGPUTextureDimension_3D;
    t->label = to_string(d.label);
    t->fmt = format_info(d.format);
    const FormatInfo& f = t->fmt;
    if (f.format == DkImageFormat_None) {
        SWDK_LOG_LIMITED(16, "textures: WebGPU format 0x%X (%s, %ux%u) has no deko3d format: not drawn",
                         unsigned(d.format), t->label.c_str(), t->width, t->height);
        return t;
    }
    if (d.sampleCount > 1) {
        SWDK_LOG_LIMITED(4, "textures: %s asks for %u samples; drawn single-sampled", t->label.c_str(),
                         unsigned(d.sampleCount));
    }
    DkImageLayoutMaker m;
    dkImageLayoutMakerDefaults(&m, R.device);
    m.type = t->is3d ? DkImageType_3D : t->layers > 1 ? DkImageType_2DArray : DkImageType_2D;
    m.format = f.format;
    m.dimensions[0] = t->width;
    m.dimensions[1] = t->height;
    m.dimensions[2] = m.type == DkImageType_2D ? 0 : t->layers;
    m.mipLevels = t->mips;
    if ((d.usage & WGPUTextureUsage_RenderAttachment) && f.canRender) m.flags |= DkImageFlags_UsageRender;
    if (d.usage & WGPUTextureUsage_StorageBinding) m.flags |= DkImageFlags_UsageLoadStore;
    // the 2D engine: the present's scaled blit out of Aurora's picture
    if (f.can2d) m.flags |= DkImageFlags_Usage2DEngine;
    image_tile_size_fix(m, f.compressed ? (t->height + f.blockH - 1) / f.blockH : t->height);
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &m);
    t->mem = image_alloc(uint32_t(dkImageLayoutGetSize(&layout)), dkImageLayoutGetAlignment(&layout));
    dkImageInitialize(&t->image, &layout, t->mem.block, t->mem.offset);
    t->valid = true;
    return t;
}

DkSampler sampler_of(const WGPUSamplerDescriptor& sd) {
    DkSampler k;
    memset(&k, 0, sizeof k);  // (the key hashes its bytes, padding included)
    dkSamplerDefaults(&k);
    k.wrapMode[0] = wrap_of(sd.addressModeU);
    k.wrapMode[1] = wrap_of(sd.addressModeV);
    k.wrapMode[2] = wrap_of(sd.addressModeW);
    k.magFilter = sd.magFilter == WGPUFilterMode_Linear ? DkFilter_Linear : DkFilter_Nearest;
    k.minFilter = sd.minFilter == WGPUFilterMode_Linear ? DkFilter_Linear : DkFilter_Nearest;
    k.mipFilter = sd.mipmapFilter == WGPUMipmapFilterMode_Linear ? DkMipFilter_Linear : DkMipFilter_Nearest;
    // deko3d raises the max to the min when min > max where GL's state tracker swaps them: keep them ordered
    k.lodClampMin = std::min(sd.lodMinClamp, sd.lodMaxClamp);
    k.lodClampMax = std::max(sd.lodMinClamp, sd.lodMaxClamp);
    k.maxAnisotropy = float(std::max<uint16_t>(sd.maxAnisotropy, 1));
    if (sd.compare != WGPUCompareFunction_Undefined) {
        k.compareEnable = true;
        k.compareOp = compare_of(sd.compare);
    }
    return k;
}

const CompiledStage* stage_of(WGPUShaderModule module, WGPUStringView entry, ShaderStage stage, const char* label) {
    auto m = g_shaderModules.get(module);
    if (!m || m->wgsl.empty()) {
        SWDK_LOG_LIMITED(16, "pipelines: %s: its shader module has no WGSL (not drawn)", label);
        return nullptr;
    }
    return compile_stage(m->wgsl, to_string(entry), stage);
}

std::shared_ptr<Pipe> make_render_pipeline(const WGPURenderPipelineDescriptor& d) {
    auto p = std::make_shared<Pipe>();
    p->label = to_string(d.label);
    const char* label = p->label.empty() ? "(no label)" : p->label.c_str();
    dkRasterizerStateDefaults(&p->rs);
    dkColorStateDefaults(&p->color);
    dkColorWriteStateDefaults(&p->colorWrite);
    dkDepthStencilStateDefaults(&p->ds);
    for (auto& b : p->blend) dkBlendStateDefaults(&b);

    if (d.vertex.constantCount || (d.fragment && d.fragment->constantCount))
        SWDK_LOG_LIMITED(8, "pipelines: %s overrides shader constants, which are not applied", label);
    p->vs = stage_of(d.vertex.module, d.vertex.entryPoint, ShaderStage::Vertex, label);
    if (d.fragment) p->fs = stage_of(d.fragment->module, d.fragment->entryPoint, ShaderStage::Fragment, label);
    p->valid = p->vs && (!d.fragment || p->fs);

    // vertex input: attributes by shader location, holes as fixed zeros
    for (size_t i = 0; i < d.vertex.bufferCount && i < 16; i++) {
        const WGPUVertexBufferLayout& b = d.vertex.buffers[i];
        p->buffers[i].stride = uint32_t(b.arrayStride);
        p->buffers[i].divisor = b.stepMode == WGPUVertexStepMode_Instance ? 1 : 0;
        p->bufferCount = uint32_t(i + 1);
        for (size_t a = 0; a < b.attributeCount; a++) {
            const WGPUVertexAttribute& at = b.attributes[a];
            if (at.shaderLocation >= 32) continue;
            DkVtxAttribSize size;
            DkVtxAttribType type;
            bool bgra;
            if (!vertex_format_of(at.format, size, type, bgra)) {
                SWDK_LOG_LIMITED(8, "pipelines: %s: vertex format 0x%X has no deko3d format", label,
                                 unsigned(at.format));
                continue;
            }
            DkVtxAttribState& s = p->attribs[at.shaderLocation];
            s.bufferId = uint32_t(i);
            s.isFixed = 0;
            s.offset = uint32_t(at.offset);
            s.size = size;
            s.type = type;
            s.isBgra = bgra;
            p->attribCount = std::max(p->attribCount, at.shaderLocation + 1);
        }
    }
    for (uint32_t i = 0; i < p->attribCount; i++) {
        if (p->attribs[i].size == 0 && p->attribs[i].type == 0) {
            p->attribs[i].isFixed = 1;
            p->attribs[i].size = DkVtxAttribSize_1x32;
            p->attribs[i].type = DkVtxAttribType_Float;
        }
    }

    switch (d.primitive.topology) {
    case WGPUPrimitiveTopology_PointList: p->prim = DkPrimitive_Points; break;
    case WGPUPrimitiveTopology_LineList: p->prim = DkPrimitive_Lines; break;
    case WGPUPrimitiveTopology_LineStrip:
        p->prim = DkPrimitive_LineStrip;
        p->strip = true;
        break;
    case WGPUPrimitiveTopology_TriangleStrip:
        p->prim = DkPrimitive_TriangleStrip;
        p->strip = true;
        break;
    default: p->prim = DkPrimitive_Triangles; break;
    }
    static const bool flipFront = env_flag("SPM_DK_FLIP_FRONT", false);
    const bool ccw = d.primitive.frontFace != WGPUFrontFace_CW;
    p->rs.frontFace = ccw != flipFront ? DkFrontFace_CCW : DkFrontFace_CW;
    p->rs.cullMode = d.primitive.cullMode == WGPUCullMode_Front  ? DkFace_Front
                     : d.primitive.cullMode == WGPUCullMode_Back ? DkFace_Back
                                                                 : DkFace_None;
    p->rs.depthClampEnable = d.primitive.unclippedDepth ? 1 : 0;

    if (const WGPUDepthStencilState* ds = d.depthStencil) {
        p->ds.depthTestEnable = 1;
        p->ds.depthWriteEnable = ds->depthWriteEnabled == WGPUOptionalBool_True;
        p->ds.depthCompareOp = compare_of(ds->depthCompare);
        auto face_used = [](const WGPUStencilFaceState& f) {
            return (f.compare != WGPUCompareFunction_Undefined && f.compare != WGPUCompareFunction_Always) ||
                   stencil_op_of(f.failOp) != DkStencilOp_Keep || stencil_op_of(f.depthFailOp) != DkStencilOp_Keep ||
                   stencil_op_of(f.passOp) != DkStencilOp_Keep;
        };
        p->stencil = format_info(ds->format).stencil && (face_used(ds->stencilFront) || face_used(ds->stencilBack));
        p->ds.stencilTestEnable = p->stencil;
        p->ds.stencilFrontFailOp = stencil_op_of(ds->stencilFront.failOp);
        p->ds.stencilFrontPassOp = stencil_op_of(ds->stencilFront.passOp);
        p->ds.stencilFrontDepthFailOp = stencil_op_of(ds->stencilFront.depthFailOp);
        p->ds.stencilFrontCompareOp = compare_of(ds->stencilFront.compare);
        p->ds.stencilBackFailOp = stencil_op_of(ds->stencilBack.failOp);
        p->ds.stencilBackPassOp = stencil_op_of(ds->stencilBack.passOp);
        p->ds.stencilBackDepthFailOp = stencil_op_of(ds->stencilBack.depthFailOp);
        p->ds.stencilBackCompareOp = compare_of(ds->stencilBack.compare);
        p->stencilReadMask = uint8_t(ds->stencilReadMask);
        p->stencilWriteMask = uint8_t(ds->stencilWriteMask);
        p->depthBias[0] = float(ds->depthBias);
        p->depthBias[1] = ds->depthBiasClamp;
        p->depthBias[2] = ds->depthBiasSlopeScale;
        if (ds->depthBias != 0 || ds->depthBiasSlopeScale != 0.0f) p->rs.depthBiasEnableMask = DkPolygonFlag_All;
    } else {
        p->ds.depthTestEnable = 0;
        p->ds.depthWriteEnable = 0;
        p->ds.stencilTestEnable = 0;
    }

    // colour targets: none written unless the fragment state says so
    for (uint32_t i = 0; i < 8; i++) dkColorWriteStateSetMask(&p->colorWrite, i, 0);
    if (d.fragment) {
        for (size_t i = 0; i < d.fragment->targetCount && i < 8; i++) {
            const WGPUColorTargetState& t = d.fragment->targets[i];
            if (t.format == WGPUTextureFormat_Undefined) continue;
            // WebGPU's write mask bits are deko3d's (R 1, G 2, B 4, A 8)
            dkColorWriteStateSetMask(&p->colorWrite, uint32_t(i), uint32_t(t.writeMask) & 0xF);
            if (t.blend && !format_info(t.format).integer) {
                dkColorStateSetBlendEnable(&p->color, uint32_t(i), true);
                // separate alpha factors: switch/deko3d's patch 1 programs FuncAlphaDst from them
                dkBlendStateSetOps(&p->blend[i], blend_op_of(t.blend->color.operation),
                                   blend_op_of(t.blend->alpha.operation));
                dkBlendStateSetFactors(&p->blend[i], blend_factor_of(t.blend->color.srcFactor, false),
                                       blend_factor_of(t.blend->color.dstFactor, false),
                                       blend_factor_of(t.blend->alpha.srcFactor, true),
                                       blend_factor_of(t.blend->alpha.dstFactor, true));
            }
            p->blendCount = uint32_t(i + 1);
        }
    }
    if (!p->valid) dklog("pipelines: render pipeline %s has no usable shaders: its draws are skipped", label);
    return p;
}

}  // namespace

std::string to_string(WGPUStringView s) {
    if (!s.data) return {};
    if (s.length == WGPU_STRLEN) return std::string(s.data);
    return std::string(s.data, s.length);
}

Tex::~Tex() {
    if (valid) image_free(mem);
}

View::~View() {
    if (slot) image_slot_release(slot);
}

Buf::~Buf() { block_destroy(block); }

const BindGroup::Entry* BindGroup::find(uint32_t binding) const {
    for (const Entry& e : entries) {
        if (e.binding == binding) return &e;
    }
    return nullptr;
}

FormatInfo format_info(WGPUTextureFormat f) {
    FormatInfo i;
    auto plain = [&](DkImageFormat fmt, uint8_t bytes, bool integer = false) {
        i.format = fmt;
        i.blockBytes = bytes;
        i.integer = integer;
        i.canRender = true;
        i.can2d = !integer;
    };
    auto depth = [&](DkImageFormat fmt, uint8_t bytes, bool stencil) {
        i.format = fmt;
        i.blockBytes = bytes;
        i.depth = true;
        i.stencil = stencil;
        i.canRender = true;
    };
    auto block = [&](DkImageFormat fmt, uint8_t w, uint8_t h, uint8_t bytes) {
        i.format = fmt;
        i.blockW = w;
        i.blockH = h;
        i.blockBytes = bytes;
        i.compressed = true;
    };
    switch (f) {
    case WGPUTextureFormat_R8Unorm: plain(DkImageFormat_R8_Unorm, 1); break;
    case WGPUTextureFormat_R8Snorm: plain(DkImageFormat_R8_Snorm, 1); break;
    case WGPUTextureFormat_R8Uint: plain(DkImageFormat_R8_Uint, 1, true); break;
    case WGPUTextureFormat_R8Sint: plain(DkImageFormat_R8_Sint, 1, true); break;
    case WGPUTextureFormat_R16Unorm: plain(DkImageFormat_R16_Unorm, 2); break;
    case WGPUTextureFormat_R16Snorm: plain(DkImageFormat_R16_Snorm, 2); break;
    case WGPUTextureFormat_R16Uint: plain(DkImageFormat_R16_Uint, 2, true); break;
    case WGPUTextureFormat_R16Sint: plain(DkImageFormat_R16_Sint, 2, true); break;
    case WGPUTextureFormat_R16Float: plain(DkImageFormat_R16_Float, 2); break;
    case WGPUTextureFormat_RG8Unorm: plain(DkImageFormat_RG8_Unorm, 2); break;
    case WGPUTextureFormat_RG8Snorm: plain(DkImageFormat_RG8_Snorm, 2); break;
    case WGPUTextureFormat_RG8Uint: plain(DkImageFormat_RG8_Uint, 2, true); break;
    case WGPUTextureFormat_RG8Sint: plain(DkImageFormat_RG8_Sint, 2, true); break;
    case WGPUTextureFormat_R32Float: plain(DkImageFormat_R32_Float, 4); break;
    case WGPUTextureFormat_R32Uint: plain(DkImageFormat_R32_Uint, 4, true); break;
    case WGPUTextureFormat_R32Sint: plain(DkImageFormat_R32_Sint, 4, true); break;
    case WGPUTextureFormat_RG16Unorm: plain(DkImageFormat_RG16_Unorm, 4); break;
    case WGPUTextureFormat_RG16Snorm: plain(DkImageFormat_RG16_Snorm, 4); break;
    case WGPUTextureFormat_RG16Uint: plain(DkImageFormat_RG16_Uint, 4, true); break;
    case WGPUTextureFormat_RG16Sint: plain(DkImageFormat_RG16_Sint, 4, true); break;
    case WGPUTextureFormat_RG16Float: plain(DkImageFormat_RG16_Float, 4); break;
    case WGPUTextureFormat_RGBA8Unorm: plain(DkImageFormat_RGBA8_Unorm, 4); break;
    case WGPUTextureFormat_RGBA8UnormSrgb: plain(DkImageFormat_RGBA8_Unorm_sRGB, 4); break;
    case WGPUTextureFormat_RGBA8Snorm: plain(DkImageFormat_RGBA8_Snorm, 4); break;
    case WGPUTextureFormat_RGBA8Uint: plain(DkImageFormat_RGBA8_Uint, 4, true); break;
    case WGPUTextureFormat_RGBA8Sint: plain(DkImageFormat_RGBA8_Sint, 4, true); break;
    case WGPUTextureFormat_BGRA8Unorm: plain(DkImageFormat_BGRA8_Unorm, 4); break;
    case WGPUTextureFormat_BGRA8UnormSrgb: plain(DkImageFormat_BGRA8_Unorm_sRGB, 4); break;
    case WGPUTextureFormat_RGB10A2Uint: plain(DkImageFormat_RGB10A2_Uint, 4, true); break;
    case WGPUTextureFormat_RGB10A2Unorm: plain(DkImageFormat_RGB10A2_Unorm, 4); break;
    case WGPUTextureFormat_RG11B10Ufloat: plain(DkImageFormat_RG11B10_Float, 4); break;
    case WGPUTextureFormat_RGB9E5Ufloat:
        plain(DkImageFormat_E5BGR9_Float, 4);
        i.canRender = false;
        break;
    case WGPUTextureFormat_RG32Float: plain(DkImageFormat_RG32_Float, 8); break;
    case WGPUTextureFormat_RG32Uint: plain(DkImageFormat_RG32_Uint, 8, true); break;
    case WGPUTextureFormat_RG32Sint: plain(DkImageFormat_RG32_Sint, 8, true); break;
    case WGPUTextureFormat_RGBA16Unorm: plain(DkImageFormat_RGBA16_Unorm, 8); break;
    case WGPUTextureFormat_RGBA16Snorm: plain(DkImageFormat_RGBA16_Snorm, 8); break;
    case WGPUTextureFormat_RGBA16Uint: plain(DkImageFormat_RGBA16_Uint, 8, true); break;
    case WGPUTextureFormat_RGBA16Sint: plain(DkImageFormat_RGBA16_Sint, 8, true); break;
    case WGPUTextureFormat_RGBA16Float: plain(DkImageFormat_RGBA16_Float, 8); break;
    case WGPUTextureFormat_RGBA32Float: plain(DkImageFormat_RGBA32_Float, 16); break;
    case WGPUTextureFormat_RGBA32Uint: plain(DkImageFormat_RGBA32_Uint, 16, true); break;
    case WGPUTextureFormat_RGBA32Sint: plain(DkImageFormat_RGBA32_Sint, 16, true); break;
    case WGPUTextureFormat_Stencil8: depth(DkImageFormat_S8, 1, true); break;
    case WGPUTextureFormat_Depth16Unorm: depth(DkImageFormat_Z16, 2, false); break;
    case WGPUTextureFormat_Depth24Plus: depth(DkImageFormat_Z24X8, 4, false); break;
    case WGPUTextureFormat_Depth24PlusStencil8: depth(DkImageFormat_Z24S8, 4, true); break;
    case WGPUTextureFormat_Depth32Float: depth(DkImageFormat_ZF32, 4, false); break;
    case WGPUTextureFormat_Depth32FloatStencil8: depth(DkImageFormat_ZF32_X24S8, 8, true); break;
    case WGPUTextureFormat_BC1RGBAUnorm: block(DkImageFormat_RGBA_BC1, 4, 4, 8); break;
    case WGPUTextureFormat_BC1RGBAUnormSrgb: block(DkImageFormat_RGBA_BC1_sRGB, 4, 4, 8); break;
    case WGPUTextureFormat_BC2RGBAUnorm: block(DkImageFormat_RGBA_BC2, 4, 4, 16); break;
    case WGPUTextureFormat_BC2RGBAUnormSrgb: block(DkImageFormat_RGBA_BC2_sRGB, 4, 4, 16); break;
    case WGPUTextureFormat_BC3RGBAUnorm: block(DkImageFormat_RGBA_BC3, 4, 4, 16); break;
    case WGPUTextureFormat_BC3RGBAUnormSrgb: block(DkImageFormat_RGBA_BC3_sRGB, 4, 4, 16); break;
    case WGPUTextureFormat_BC4RUnorm: block(DkImageFormat_R_BC4_Unorm, 4, 4, 8); break;
    case WGPUTextureFormat_BC4RSnorm: block(DkImageFormat_R_BC4_Snorm, 4, 4, 8); break;
    case WGPUTextureFormat_BC5RGUnorm: block(DkImageFormat_RG_BC5_Unorm, 4, 4, 16); break;
    case WGPUTextureFormat_BC5RGSnorm: block(DkImageFormat_RG_BC5_Snorm, 4, 4, 16); break;
    case WGPUTextureFormat_BC6HRGBUfloat: block(DkImageFormat_RGBA_BC6H_UF16_Float, 4, 4, 16); break;
    case WGPUTextureFormat_BC6HRGBFloat: block(DkImageFormat_RGBA_BC6H_SF16_Float, 4, 4, 16); break;
    case WGPUTextureFormat_BC7RGBAUnorm: block(DkImageFormat_RGBA_BC7_Unorm, 4, 4, 16); break;
    case WGPUTextureFormat_BC7RGBAUnormSrgb: block(DkImageFormat_RGBA_BC7_Unorm_sRGB, 4, 4, 16); break;
    default: break;
    }
    return i;
}

uint32_t view_slot(View* v) {
    if (!v || !v->valid()) return kEmptyImage;
    if (v->slot == 0) {
        const uint32_t slot = image_slot_alloc();
        if (slot == 0) return kEmptyImage;
        write_image_view_descriptor(slot, v->view);
        v->slot = slot;
    }
    return v->slot;
}

DkResHandle texture_handle(View* v, Samp* s) {
    const uint32_t image = view_slot(v);
    const uint32_t sampler = s ? sampler_slot(s->sampler, s->key) : uint32_t(kSamplerNearestClamp);
    return dkMakeTextureHandle(image, sampler);
}

DkImageView level_view(const Tex& t, uint32_t level) {
    DkImageView v;
    dkImageViewDefaults(&v, &t.image);
    v.mipLevelOffset = uint8_t(level);
    v.mipLevelCount = 1;
    return v;
}

DkGpuAddr null_buffer() {
    static Block b;
    if (!b) {
        b = block_create(kNullBufferSize, "the null buffer");
        memset(b.cpu, 0, kNullBufferSize);
    }
    return b.gpu;
}

}  // namespace swdk::wg

using namespace swdk;
using namespace swdk::wg;

extern "C" {

// ---- textures and views
WGPUTexture __wrap_wgpuDeviceCreateTexture(WGPUDevice device, WGPUTextureDescriptor const* descriptor) {
    WGPUTexture t = __real_wgpuDeviceCreateTexture(device, descriptor);
    if (t && descriptor && R.device) g_textures.add(t, make_texture(*descriptor));
    return t;
}

void __wrap_wgpuTextureAddRef(WGPUTexture texture) {
    g_textures.add_ref(texture);
    __real_wgpuTextureAddRef(texture);
}

void __wrap_wgpuTextureRelease(WGPUTexture texture) {
    g_textures.release(texture);
    __real_wgpuTextureRelease(texture);
}

WGPUTextureView __wrap_wgpuTextureCreateView(WGPUTexture texture, WGPUTextureViewDescriptor const* d) {
    WGPUTextureView v = __real_wgpuTextureCreateView(texture, d);
    auto t = g_textures.get(texture);
    if (!v || !t) return v;
    auto view = std::make_shared<View>();
    view->tex = t;
    dkImageViewDefaults(&view->view, &t->image);
    const uint32_t base = d ? std::min(d->baseMipLevel, t->mips - 1) : 0;
    uint32_t count = d && d->mipLevelCount != WGPU_MIP_LEVEL_COUNT_UNDEFINED ? d->mipLevelCount : t->mips - base;
    count = std::max(1u, std::min(count, t->mips - base));
    view->baseMip = base;
    view->mipCount = count;
    view->view.mipLevelOffset = uint8_t(base);
    view->view.mipLevelCount = uint8_t(count);
    if (d && d->format != WGPUTextureFormat_Undefined && d->format != t->wformat) {
        const FormatInfo f = format_info(d->format);
        if (f.format != DkImageFormat_None) view->view.format = f.format;
    }
    view->layerCount = t->layers;
    if (d && t->layers > 1 && !t->is3d) {
        view->baseLayer = std::min(d->baseArrayLayer, t->layers - 1);
        view->layerCount = d->arrayLayerCount != WGPU_ARRAY_LAYER_COUNT_UNDEFINED ? d->arrayLayerCount
                                                                                   : t->layers - view->baseLayer;
        view->view.layerOffset = uint16_t(view->baseLayer);
        view->view.layerCount = uint16_t(view->layerCount);
        if (d->dimension == WGPUTextureViewDimension_2D) view->view.type = DkImageType_2D;
    }
    if (d && d->aspect == WGPUTextureAspect_StencilOnly) view->view.dsSource = DkDsSource_Stencil;
    for (const WGPUChainedStruct* c = d ? d->nextInChain : nullptr; c; c = c->next) {
        if (c->sType == WGPUSType_TextureComponentSwizzleDescriptor) {
            const auto* s = reinterpret_cast<const WGPUTextureComponentSwizzleDescriptor*>(c);
            view->view.swizzle[0] = swizzle_of(s->swizzle.r, DkImageSwizzle_Red);
            view->view.swizzle[1] = swizzle_of(s->swizzle.g, DkImageSwizzle_Green);
            view->view.swizzle[2] = swizzle_of(s->swizzle.b, DkImageSwizzle_Blue);
            view->view.swizzle[3] = swizzle_of(s->swizzle.a, DkImageSwizzle_Alpha);
        }
    }
    g_views.add(v, std::move(view));
    return v;
}

void __wrap_wgpuTextureViewAddRef(WGPUTextureView view) {
    g_views.add_ref(view);
    __real_wgpuTextureViewAddRef(view);
}

void __wrap_wgpuTextureViewRelease(WGPUTextureView view) {
    g_views.release(view);
    __real_wgpuTextureViewRelease(view);
}

// ---- samplers
WGPUSampler __wrap_wgpuDeviceCreateSampler(WGPUDevice device, WGPUSamplerDescriptor const* d) {
    WGPUSampler s = __real_wgpuDeviceCreateSampler(device, d);
    if (!s || !R.device) return s;
    auto samp = std::make_shared<Samp>();
    WGPUSamplerDescriptor def = WGPU_SAMPLER_DESCRIPTOR_INIT;
    samp->sampler = sampler_of(d ? *d : def);
    samp->key = fnv_bytes(&samp->sampler, sizeof samp->sampler);
    g_samplers.add(s, std::move(samp));
    return s;
}

void __wrap_wgpuSamplerAddRef(WGPUSampler sampler) {
    g_samplers.add_ref(sampler);
    __real_wgpuSamplerAddRef(sampler);
}

void __wrap_wgpuSamplerRelease(WGPUSampler sampler) {
    g_samplers.release(sampler);
    __real_wgpuSamplerRelease(sampler);
}

// ---- buffers: a memory block each; Aurora maps that memory, never Dawn's
WGPUBuffer __wrap_wgpuDeviceCreateBuffer(WGPUDevice device, WGPUBufferDescriptor const* d) {
    WGPUBuffer b = __real_wgpuDeviceCreateBuffer(device, d);
    if (!b || !d || !R.device) return b;
    if (d->size > 0xFFFF0000ull) {
        SWDK_LOG_LIMITED(8, "buffers: %s is %llu bytes, more than a memory block takes: not drawn",
                         to_string(d->label).c_str(), (unsigned long long)d->size);
        return b;
    }
    auto buf = std::make_shared<Buf>();
    buf->size = d->size;
    buf->usage = d->usage;
    buf->label = to_string(d->label);
    buf->block = block_create(uint32_t(std::max<uint64_t>(d->size, 4)), "a WebGPU buffer");
    if (d->size >= (8u << 20))
        dklog("buffers: %s, %llu MiB", to_string(d->label).c_str(), (unsigned long long)(d->size >> 20));
    g_buffers.add(b, std::move(buf));
    return b;
}

void __wrap_wgpuBufferAddRef(WGPUBuffer buffer) {
    g_buffers.add_ref(buffer);
    __real_wgpuBufferAddRef(buffer);
}

void __wrap_wgpuBufferRelease(WGPUBuffer buffer) {
    g_buffers.release(buffer);
    __real_wgpuBufferRelease(buffer);
}

// The GPU has to be done with the buffer before Aurora writes or reads its memory: Dawn's Null queue
// finishes everything at once, so the map waits for the newest submit that used the buffer first
// (the map's callback then comes from Dawn as before).
}  // extern "C"

namespace swdk::wg {
namespace {
std::atomic<uint64_t> g_mapWaits{0}, g_mapWaitNs{0}, g_mapWaitMaxNs{0};
}  // namespace

MapWaitStats map_wait_stats_take() {
    MapWaitStats s;
    s.count = g_mapWaits.exchange(0, std::memory_order_relaxed);
    s.totalNs = g_mapWaitNs.exchange(0, std::memory_order_relaxed);
    s.maxNs = g_mapWaitMaxNs.exchange(0, std::memory_order_relaxed);
    return s;
}
}  // namespace swdk::wg

extern "C" {

WGPUFuture __wrap_wgpuBufferMapAsync(WGPUBuffer buffer, WGPUMapMode mode, size_t offset, size_t size,
                                     WGPUBufferMapCallbackInfo callbackInfo) {
    if (auto b = g_buffers.get(buffer)) {
        const uint64_t serial = b->lastSerial.load(std::memory_order_acquire);
        if (serial > completed_serial()) {
            const uint64_t t0 = now_ns();
            wait_serial(serial);
            const uint64_t ns = now_ns() - t0;
            g_mapWaits.fetch_add(1, std::memory_order_relaxed);
            g_mapWaitNs.fetch_add(ns, std::memory_order_relaxed);
            uint64_t prev = g_mapWaitMaxNs.load(std::memory_order_relaxed);
            while (ns > prev && !g_mapWaitMaxNs.compare_exchange_weak(prev, ns, std::memory_order_relaxed)) {
            }
            const uint64_t ms = ns / 1000000;
            if (ms >= 4) SWDK_LOG_LIMITED(16, "buffers: a map waited %llu ms for the GPU", (unsigned long long)ms);
        }
    }
    return __real_wgpuBufferMapAsync(buffer, mode, offset, size, callbackInfo);
}

void* __wrap_wgpuBufferGetMappedRange(WGPUBuffer buffer, size_t offset, size_t size) {
    if (auto b = g_buffers.get(buffer)) {
        if (offset > b->size) return nullptr;
        return b->block.cpu + offset;
    }
    return __real_wgpuBufferGetMappedRange(buffer, offset, size);
}

void const* __wrap_wgpuBufferGetConstMappedRange(WGPUBuffer buffer, size_t offset, size_t size) {
    if (auto b = g_buffers.get(buffer)) {
        if (offset > b->size) return nullptr;
        return b->block.cpu + offset;
    }
    return __real_wgpuBufferGetConstMappedRange(buffer, offset, size);
}

// ---- bind groups and their layouts
WGPUBindGroupLayout __wrap_wgpuDeviceCreateBindGroupLayout(WGPUDevice device,
                                                           WGPUBindGroupLayoutDescriptor const* d) {
    WGPUBindGroupLayout l = __real_wgpuDeviceCreateBindGroupLayout(device, d);
    if (!l || !d) return l;
    auto layout = std::make_shared<BindGroupLayout>();
    for (size_t i = 0; i < d->entryCount; i++) {
        if (d->entries[i].buffer.hasDynamicOffset) layout->dynamicBindings.push_back(d->entries[i].binding);
    }
    std::sort(layout->dynamicBindings.begin(), layout->dynamicBindings.end());
    g_bindGroupLayouts.add(l, std::move(layout));
    return l;
}

void __wrap_wgpuBindGroupLayoutAddRef(WGPUBindGroupLayout layout) {
    g_bindGroupLayouts.add_ref(layout);
    __real_wgpuBindGroupLayoutAddRef(layout);
}

void __wrap_wgpuBindGroupLayoutRelease(WGPUBindGroupLayout layout) {
    g_bindGroupLayouts.release(layout);
    __real_wgpuBindGroupLayoutRelease(layout);
}

WGPUBindGroup __wrap_wgpuDeviceCreateBindGroup(WGPUDevice device, WGPUBindGroupDescriptor const* d) {
    WGPUBindGroup g = __real_wgpuDeviceCreateBindGroup(device, d);
    if (!g || !d || !R.device) return g;
    auto group = std::make_shared<BindGroup>();
    auto layout = g_bindGroupLayouts.get(d->layout);
    for (size_t i = 0; i < d->entryCount; i++) {
        const WGPUBindGroupEntry& e = d->entries[i];
        BindGroup::Entry entry;
        entry.binding = e.binding;
        if (e.buffer) {
            entry.buf = g_buffers.get(e.buffer);
            entry.offset = e.offset;
            if (entry.buf) {
                const uint64_t rest = entry.buf->size > e.offset ? entry.buf->size - e.offset : 0;
                entry.size = e.size == WGPU_WHOLE_SIZE ? rest : std::min(e.size, rest);
            }
        }
        entry.view = g_views.get(e.textureView);
        entry.samp = g_samplers.get(e.sampler);
        if (layout) {
            const auto& dyn = layout->dynamicBindings;
            auto it = std::find(dyn.begin(), dyn.end(), e.binding);
            if (it != dyn.end()) entry.dynIndex = int(it - dyn.begin());
        }
        group->entries.push_back(std::move(entry));
    }
    std::sort(group->entries.begin(), group->entries.end(),
              [](const BindGroup::Entry& a, const BindGroup::Entry& b) { return a.binding < b.binding; });
    g_bindGroups.add(g, std::move(group));
    return g;
}

void __wrap_wgpuBindGroupAddRef(WGPUBindGroup group) {
    g_bindGroups.add_ref(group);
    __real_wgpuBindGroupAddRef(group);
}

void __wrap_wgpuBindGroupRelease(WGPUBindGroup group) {
    g_bindGroups.release(group);
    __real_wgpuBindGroupRelease(group);
}

// ---- shader modules: the WGSL, compiled per entry point when a pipeline uses it
WGPUShaderModule __wrap_wgpuDeviceCreateShaderModule(WGPUDevice device, WGPUShaderModuleDescriptor const* d) {
    WGPUShaderModule m = __real_wgpuDeviceCreateShaderModule(device, d);
    if (!m || !d) return m;
    auto module = std::make_shared<ShaderModule>();
    for (const WGPUChainedStruct* c = d->nextInChain; c; c = c->next) {
        if (c->sType == WGPUSType_ShaderSourceWGSL) {
            module->wgsl = to_string(reinterpret_cast<const WGPUShaderSourceWGSL*>(c)->code);
        }
    }
    if (module->wgsl.empty())
        SWDK_LOG_LIMITED(8, "shaders: module %s is not WGSL: its pipelines are not drawn", to_string(d->label).c_str());
    g_shaderModules.add(m, std::move(module));
    return m;
}

void __wrap_wgpuShaderModuleAddRef(WGPUShaderModule module) {
    g_shaderModules.add_ref(module);
    __real_wgpuShaderModuleAddRef(module);
}

void __wrap_wgpuShaderModuleRelease(WGPUShaderModule module) {
    g_shaderModules.release(module);
    __real_wgpuShaderModuleRelease(module);
}

// ---- pipelines: compiled when created (the creating thread waits for uam; cached on the SD card)
WGPURenderPipeline __wrap_wgpuDeviceCreateRenderPipeline(WGPUDevice device, WGPURenderPipelineDescriptor const* d) {
    WGPURenderPipeline p = __real_wgpuDeviceCreateRenderPipeline(device, d);
    if (!p || !d || !R.device) return p;
    g_renderPipelines.add(p, make_render_pipeline(*d));
    return p;
}

void __wrap_wgpuRenderPipelineAddRef(WGPURenderPipeline pipeline) {
    g_renderPipelines.add_ref(pipeline);
    __real_wgpuRenderPipelineAddRef(pipeline);
}

void __wrap_wgpuRenderPipelineRelease(WGPURenderPipeline pipeline) {
    g_renderPipelines.release(pipeline);
    __real_wgpuRenderPipelineRelease(pipeline);
}

WGPUComputePipeline __wrap_wgpuDeviceCreateComputePipeline(WGPUDevice device,
                                                           WGPUComputePipelineDescriptor const* d) {
    WGPUComputePipeline p = __real_wgpuDeviceCreateComputePipeline(device, d);
    if (!p || !d || !R.device) return p;
    auto pipe = std::make_shared<Pipe>();
    pipe->compute = true;
    pipe->label = to_string(d->label);
    pipe->cs = stage_of(d->compute.module, d->compute.entryPoint, ShaderStage::Compute, pipe->label.c_str());
    pipe->valid = pipe->cs != nullptr;
    if (!pipe->valid) dklog("pipelines: compute pipeline %s has no usable shader: its dispatches are skipped",
                            pipe->label.c_str());
    g_computePipelines.add(p, std::move(pipe));
    return p;
}

void __wrap_wgpuComputePipelineAddRef(WGPUComputePipeline pipeline) {
    g_computePipelines.add_ref(pipeline);
    __real_wgpuComputePipelineAddRef(pipeline);
}

void __wrap_wgpuComputePipelineRelease(WGPUComputePipeline pipeline) {
    g_computePipelines.release(pipeline);
    __real_wgpuComputePipelineRelease(pipeline);
}

}  // extern "C"
