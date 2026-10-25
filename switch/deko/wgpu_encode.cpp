// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of
// the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Aurora's WebGPU commands, drawn by deko3d (wgpu_objects.hpp, switch/README.md "Graphics").
//
// Recording: every command encoder gets an op list. Pass and copy commands are recorded there (with a
// reference on each object they use) instead of going to Dawn; Dawn still sees the encoder, the
// passes' begin and end and the finish, so its own bookkeeping stays valid. Finish turns the op list
// into the command buffer's.
//
// The queue submit replays the op lists into the one deko3d command buffer (dk.h, under queue_mutex())
// and submits it with a fence: one serial per submit. Everything the ops used is kept alive until the
// GPU has passed that serial, and every buffer notes it, so a map waits for the GPU first
// (wgpu_objects.cpp).
//
// Queue writes go through upload space with a copy at the point of the call, as WebGPU orders them.
// The present (aurora_switch_dk_present, from Aurora's surfaceless present) scales Aurora's finished
// picture into a swapchain image with the 2D engine and shows it.
//
// Ordering on the GPU:
//   - copies run on the copy and 2D engines; a full barrier comes before the first of a run of copies
//     and after its last (with the texture and shader caches invalidated, for sampled images and
//     uniform buffers), whatever comes next;
//   - a render pass ends with a fragments barrier and an invalidated texture cache (the next pass
//     samples what this one drew);
//   - a compute pass ends with a full barrier.
#include "wgpu_objects.hpp"

#include "dk_aurora.h"

#include <algorithm>
#include <cstring>

extern "C" {
WGPUCommandEncoder __real_wgpuDeviceCreateCommandEncoder(WGPUDevice device,
                                                         WGPUCommandEncoderDescriptor const* descriptor);
void __real_wgpuCommandEncoderAddRef(WGPUCommandEncoder encoder);
void __real_wgpuCommandEncoderRelease(WGPUCommandEncoder encoder);
WGPURenderPassEncoder __real_wgpuCommandEncoderBeginRenderPass(WGPUCommandEncoder encoder,
                                                               WGPURenderPassDescriptor const* descriptor);
WGPUComputePassEncoder __real_wgpuCommandEncoderBeginComputePass(WGPUCommandEncoder encoder,
                                                                 WGPUComputePassDescriptor const* descriptor);
WGPUCommandBuffer __real_wgpuCommandEncoderFinish(WGPUCommandEncoder encoder,
                                                  WGPUCommandBufferDescriptor const* descriptor);
void __real_wgpuRenderPassEncoderEnd(WGPURenderPassEncoder pass);
void __real_wgpuRenderPassEncoderAddRef(WGPURenderPassEncoder pass);
void __real_wgpuRenderPassEncoderRelease(WGPURenderPassEncoder pass);
void __real_wgpuComputePassEncoderEnd(WGPUComputePassEncoder pass);
void __real_wgpuComputePassEncoderAddRef(WGPUComputePassEncoder pass);
void __real_wgpuComputePassEncoderRelease(WGPUComputePassEncoder pass);
void __real_wgpuCommandBufferAddRef(WGPUCommandBuffer buffer);
void __real_wgpuCommandBufferRelease(WGPUCommandBuffer buffer);
void __real_wgpuQueueSubmit(WGPUQueue queue, size_t commandCount, WGPUCommandBuffer const* commands);
}

namespace swdk::wg {
namespace {

// ---- recorded commands ---------------------------------------------------------------------------
enum class OpType : uint8_t {
    BeginRender,
    EndRender,
    BeginCompute,
    EndCompute,
    SetRenderPipeline,
    SetComputePipeline,
    SetBindGroup,
    SetVertexBuffer,
    SetIndexBuffer,
    SetViewport,
    SetScissor,
    SetBlendConstant,
    SetStencilReference,
    Draw,
    DrawIndexed,
    Dispatch,
    Copy,
};

struct Op {
    OpType type;
    uint8_t index = 0;  // a bind group or vertex buffer slot, an index format
    uint32_t n = 0;     // dynamic offsets (count), a pass or copy (index)
    void* obj = nullptr;
    union {
        uint32_t u[6];
        float f[6];
        uint64_t q[3];
    };
};

struct PassDesc {
    uint32_t colorCount = 0;
    View* color[8] = {};
    View* resolve[8] = {};
    bool clear[8] = {};
    float clearValue[8][4] = {};
    View* depth = nullptr;
    bool clearDepth = false, clearStencil = false;
    float depthValue = 1.0f;
    uint8_t stencilValue = 0;
};

struct CopyDesc {
    enum Kind : uint8_t { BufferToBuffer, BufferToTexture, TextureToBuffer, TextureToTexture } kind;
    Buf* srcBuf = nullptr;
    Buf* dstBuf = nullptr;
    uint64_t srcOffset = 0, dstOffset = 0, size = 0;
    uint32_t bytesPerRow = 0, rowsPerImage = 0;  // the buffer side's layout
    Tex* srcTex = nullptr;
    Tex* dstTex = nullptr;
    uint32_t srcMip = 0, dstMip = 0;
    WGPUOrigin3D srcOrigin{}, dstOrigin{};
    WGPUExtent3D extent{};
};

struct Encoder {
    uint64_t id = 0;
    std::vector<Op> ops;
    std::vector<uint32_t> words;  // dynamic offsets
    std::vector<PassDesc> passes;
    std::vector<CopyDesc> copies;
    std::vector<ObjectRef> refs;  // every object the ops use, once
    bool finished = false;
    bool submitted = false;

    template <class T> T* use(const std::shared_ptr<T>& obj) {
        if (!obj) return nullptr;
        if (obj->encoderMark.exchange(id, std::memory_order_relaxed) != id) refs.push_back(obj);
        return obj.get();
    }
    Op& add(OpType type) {
        ops.emplace_back();
        Op& op = ops.back();
        op.type = type;
        memset(op.u, 0, sizeof op.u);
        return op;
    }
};

std::atomic<uint64_t> g_nextEncoderId{1};
Registry<Encoder> g_encoders;
Registry<Encoder> g_commandBuffers;

// Passes: the encoder they record into. Pass commands are the hot path (every draw), so each thread
// keeps its last lookup; any pass begin or release makes every thread look again.
std::shared_mutex g_passMutex;
struct PassRec {
    std::shared_ptr<Encoder> enc;
    uint32_t refs = 1;
};
std::unordered_map<const void*, PassRec> g_passes;
std::atomic<uint64_t> g_passEpoch{1};
struct PassCache {
    const void* handle = nullptr;
    Encoder* enc = nullptr;
    uint64_t epoch = 0;
};
thread_local PassCache t_pass;

void pass_add(const void* pass, std::shared_ptr<Encoder> enc) {
    std::unique_lock<std::shared_mutex> lock(g_passMutex);
    g_passes[pass] = PassRec{std::move(enc), 1};
    g_passEpoch.fetch_add(1, std::memory_order_acq_rel);
}

Encoder* pass_encoder(const void* pass) {
    const uint64_t epoch = g_passEpoch.load(std::memory_order_acquire);
    if (t_pass.handle == pass && t_pass.epoch == epoch) return t_pass.enc;
    std::shared_lock<std::shared_mutex> lock(g_passMutex);
    auto it = g_passes.find(pass);
    Encoder* enc = it == g_passes.end() ? nullptr : it->second.enc.get();
    t_pass = {pass, enc, epoch};
    return enc;
}

void pass_add_ref(const void* pass) {
    std::unique_lock<std::shared_mutex> lock(g_passMutex);
    auto it = g_passes.find(pass);
    if (it != g_passes.end()) it->second.refs++;
}

void pass_release(const void* pass) {
    std::shared_ptr<Encoder> last;
    std::unique_lock<std::shared_mutex> lock(g_passMutex);
    auto it = g_passes.find(pass);
    if (it == g_passes.end() || --it->second.refs != 0) return;
    last = std::move(it->second.enc);
    g_passes.erase(it);
    g_passEpoch.fetch_add(1, std::memory_order_acq_rel);
    lock.unlock();
}

// ---- GPU ordering (under queue_mutex()) -------------------------------------------------------------
bool g_copyRun = false;  // the last GPU work recorded was a copy

void copy_begin() {
    if (!g_copyRun) dkCmdBufBarrier(R.cmd, DkBarrier_Full, DkInvalidateFlags_Image);
    g_copyRun = true;
}

void work_begin() {
    if (g_copyRun) dkCmdBufBarrier(R.cmd, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Shader);
    g_copyRun = false;
}

struct Stats {
    uint64_t submits = 0, passes = 0, draws = 0, skippedDraws = 0, dispatches = 0, copies = 0, writes = 0,
             writeBytes = 0;
};
Stats g_stats;  // under queue_mutex()

// ---- replay (under queue_mutex()) -------------------------------------------------------------------
struct VertexBinding {
    Buf* buf = nullptr;
    uint64_t offset = 0, size = 0;
};

class Replay {
public:
    void run(Encoder& e) {
        m_enc = &e;
        if (code_take_written()) dkCmdBufBarrier(R.cmd, DkBarrier_None, DkInvalidateFlags_Shader);
        for (const Op& op : e.ops) step(op);
    }

private:
    Encoder* m_enc = nullptr;
    bool m_render = false, m_computePass = false, m_skipPass = false;
    Pipe* m_pipe = nullptr;
    bool m_pipeDirty = true, m_bindDirty = true, m_vbDirty = true, m_ibDirty = true, m_stencilDirty = true;
    BindGroup* m_groups[4] = {};
    uint32_t m_offsets[4][8] = {};
    VertexBinding m_vb[16];
    Buf* m_ib = nullptr;
    uint64_t m_ibOffset = 0;
    DkIdxFormat m_ibFormat = DkIdxFormat_Uint16;
    uint8_t m_stencilRef = 0;
    uint32_t m_width = 0, m_height = 0;

    void reset_pass_state() {
        m_pipe = nullptr;
        m_pipeDirty = m_bindDirty = m_vbDirty = m_ibDirty = m_stencilDirty = true;
        for (auto& g : m_groups) g = nullptr;
        for (auto& v : m_vb) v = {};
        m_ib = nullptr;
        m_stencilRef = 0;
    }

    void step(const Op& op) {
        switch (op.type) {
        case OpType::BeginRender: begin_render(m_enc->passes[op.n]); break;
        case OpType::EndRender: end_render(m_enc->passes[op.n]); break;
        case OpType::BeginCompute:
            work_begin();
            reset_pass_state();
            m_computePass = true;
            break;
        case OpType::EndCompute:
            m_computePass = false;
            dkCmdBufBarrier(R.cmd, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Shader);
            break;
        case OpType::SetRenderPipeline:
        case OpType::SetComputePipeline:
            if (m_pipe != op.obj) m_pipeDirty = true;
            m_pipe = static_cast<Pipe*>(op.obj);
            break;
        case OpType::SetBindGroup:
            if (op.index < 4) {
                m_groups[op.index] = static_cast<BindGroup*>(op.obj);
                for (uint32_t i = 0; i < 8; i++) m_offsets[op.index][i] = i < op.n ? m_enc->words[op.u[0] + i] : 0;
                m_bindDirty = true;
            }
            break;
        case OpType::SetVertexBuffer:
            if (op.index < 16) {
                m_vb[op.index] = {static_cast<Buf*>(op.obj), op.q[0], op.q[1]};
                m_vbDirty = true;
            }
            break;
        case OpType::SetIndexBuffer:
            m_ib = static_cast<Buf*>(op.obj);
            m_ibOffset = op.q[0];
            m_ibFormat = DkIdxFormat(op.index);
            m_ibDirty = true;
            break;
        case OpType::SetViewport: {
            if (!m_render || m_skipPass) break;
            const DkViewport v = {op.f[0], op.f[1], op.f[2], op.f[3], op.f[4], op.f[5]};
            dkCmdBufSetViewports(R.cmd, 0, &v, 1);
            break;
        }
        case OpType::SetScissor: {
            if (!m_render || m_skipPass) break;
            const DkScissor s = {op.u[0], op.u[1], op.u[2], op.u[3]};
            dkCmdBufSetScissors(R.cmd, 0, &s, 1);
            break;
        }
        case OpType::SetBlendConstant:
            if (m_render && !m_skipPass) dkCmdBufSetBlendConst(R.cmd, op.f[0], op.f[1], op.f[2], op.f[3]);
            break;
        case OpType::SetStencilReference:
            m_stencilRef = uint8_t(op.u[0]);
            m_stencilDirty = true;
            break;
        case OpType::Draw:
            if (!draw_ready()) break;
            dkCmdBufDraw(R.cmd, m_pipe->prim, op.u[0], op.u[1], op.u[2], op.u[3]);
            g_stats.draws++;
            break;
        case OpType::DrawIndexed:
            if (!draw_ready()) break;
            if (!m_ib) {
                g_stats.skippedDraws++;
                break;
            }
            if (m_ibDirty) {
                dkCmdBufBindIdxBuffer(R.cmd, m_ibFormat, m_ib->block.gpu + m_ibOffset);
                if (m_pipe->strip)
                    dkCmdBufSetPrimitiveRestart(R.cmd, true, m_ibFormat == DkIdxFormat_Uint32 ? 0xFFFFFFFFu : 0xFFFFu);
                m_ibDirty = false;
            }
            dkCmdBufDrawIndexed(R.cmd, m_pipe->prim, op.u[0], op.u[1], op.u[2], int32_t(op.u[3]), op.u[4]);
            g_stats.draws++;
            break;
        case OpType::Dispatch: dispatch(op.u[0], op.u[1], op.u[2]); break;
        case OpType::Copy: copy(m_enc->copies[op.n]); break;
        }
    }

    void begin_render(const PassDesc& p) {
        work_begin();
        reset_pass_state();
        m_render = true;
        m_skipPass = false;
        g_stats.passes++;
        DkImageView colors[8];
        const DkImageView* colorPtrs[8];
        uint32_t w = 0, h = 0;
        auto note_size = [&](const View* v) {
            const uint32_t vw = v->tex->level_width(v->baseMip), vh = v->tex->level_height(v->baseMip);
            w = w ? std::min(w, vw) : vw;
            h = h ? std::min(h, vh) : vh;
        };
        for (uint32_t i = 0; i < p.colorCount; i++) {
            View* v = p.color[i];
            if (!v || !v->valid() || !v->tex->fmt.canRender) {
                m_skipPass = true;
                continue;
            }
            colors[i] = v->view;
            colors[i].mipLevelCount = 1;
            colorPtrs[i] = &colors[i];
            note_size(v);
        }
        DkImageView depth;
        const bool hasDepth = p.depth != nullptr;
        if (hasDepth) {
            if (!p.depth->valid()) {
                m_skipPass = true;
            } else {
                depth = p.depth->view;
                depth.mipLevelCount = 1;
                note_size(p.depth);
            }
        }
        if (m_skipPass || (p.colorCount == 0 && !hasDepth)) {
            m_skipPass = true;
            SWDK_LOG_LIMITED(16, "replay: a render pass has a target without a deko3d image: its draws are skipped");
            return;
        }
        dkCmdBufBindRenderTargets(R.cmd, colorPtrs, p.colorCount, hasDepth ? &depth : nullptr);
        m_width = w;
        m_height = h;
        const DkViewport v = {0.0f, 0.0f, float(w), float(h), 0.0f, 1.0f};
        dkCmdBufSetViewports(R.cmd, 0, &v, 1);
        const DkScissor s = {0, 0, w, h};
        dkCmdBufSetScissors(R.cmd, 0, &s, 1);  // (clears are scissored)
        dkCmdBufSetBlendConst(R.cmd, 0.0f, 0.0f, 0.0f, 0.0f);
        for (uint32_t i = 0; i < p.colorCount; i++) {
            if (!p.clear[i]) continue;
            const float* c = p.clearValue[i];
            if (p.color[i]->tex->fmt.integer) {
                dkCmdBufClearColorUint(R.cmd, i, DkColorMask_RGBA, uint32_t(c[0]), uint32_t(c[1]), uint32_t(c[2]),
                                       uint32_t(c[3]));
            } else {
                dkCmdBufClearColorFloat(R.cmd, i, DkColorMask_RGBA, c[0], c[1], c[2], c[3]);
            }
        }
        if (hasDepth && (p.clearDepth || p.clearStencil)) {
            dkCmdBufClearDepthStencil(R.cmd, p.clearDepth, p.depthValue, p.clearStencil ? 0xFF : 0, p.stencilValue);
        }
    }

    void end_render(const PassDesc& p) {
        m_render = false;
        if (m_skipPass) return;
        bool resolves = false;
        for (uint32_t i = 0; i < p.colorCount; i++) resolves |= p.resolve[i] && p.resolve[i]->valid();
        if (resolves) {
            // every texture is single-sampled here (wgpu_objects.cpp), so a resolve is a copy
            dkCmdBufBarrier(R.cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
            for (uint32_t i = 0; i < p.colorCount; i++) {
                View* src = p.color[i];
                View* dst = p.resolve[i];
                if (!dst || !dst->valid()) continue;
                const uint32_t w = std::min(src->tex->level_width(src->baseMip), dst->tex->level_width(dst->baseMip));
                const uint32_t h =
                    std::min(src->tex->level_height(src->baseMip), dst->tex->level_height(dst->baseMip));
                const DkImageView sv = level_view(*src->tex, src->baseMip), dv = level_view(*dst->tex, dst->baseMip);
                const DkImageRect r = {0, 0, 0, w, h, 1};
                dkCmdBufCopyImage(R.cmd, &sv, &r, &dv, &r, 0);
            }
            g_copyRun = true;
        } else {
            dkCmdBufBarrier(R.cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
        }
    }

    bool draw_ready() {
        if (!m_render || m_skipPass || !m_pipe || m_pipe->compute || !m_pipe->valid) {
            g_stats.skippedDraws++;
            return false;
        }
        if (m_pipeDirty) {
            const DkShader* shaders[2];
            uint32_t n = 0;
            shaders[n++] = &m_pipe->vs->shader;
            if (m_pipe->fs) shaders[n++] = &m_pipe->fs->shader;
            // stages of the mask without a shader are unbound
            dkCmdBufBindShaders(R.cmd, DkStageFlag_GraphicsMask, shaders, n);
            dkCmdBufBindRasterizerState(R.cmd, &m_pipe->rs);
            dkCmdBufBindColorState(R.cmd, &m_pipe->color);
            dkCmdBufBindColorWriteState(R.cmd, &m_pipe->colorWrite);
            if (m_pipe->blendCount) dkCmdBufBindBlendStates(R.cmd, 0, m_pipe->blend, m_pipe->blendCount);
            dkCmdBufBindDepthStencilState(R.cmd, &m_pipe->ds);
            if (m_pipe->rs.depthBiasEnableMask)
                dkCmdBufSetDepthBias(R.cmd, m_pipe->depthBias[0], m_pipe->depthBias[1], m_pipe->depthBias[2]);
            dkCmdBufBindVtxAttribState(R.cmd, m_pipe->attribs, m_pipe->attribCount);
            dkCmdBufBindVtxBufferState(R.cmd, m_pipe->buffers, m_pipe->bufferCount);
            if (!m_pipe->strip) dkCmdBufSetPrimitiveRestart(R.cmd, false, 0);
            m_pipeDirty = false;
            m_bindDirty = m_vbDirty = m_ibDirty = m_stencilDirty = true;
        }
        if (m_bindDirty) {
            bind_stage(m_pipe->vs, DkStage_Vertex);
            if (m_pipe->fs) bind_stage(m_pipe->fs, DkStage_Fragment);
            m_bindDirty = false;
        }
        if (m_vbDirty && m_pipe->bufferCount) {
            DkBufExtents ext[16];
            for (uint32_t i = 0; i < m_pipe->bufferCount; i++) {
                const VertexBinding& b = m_vb[i];
                if (b.buf) {
                    const uint64_t rest = b.buf->size > b.offset ? b.buf->size - b.offset : 0;
                    ext[i] = {b.buf->block.gpu + b.offset, uint32_t(std::min(b.size, rest))};
                } else {
                    ext[i] = {null_buffer(), kNullBufferSize};
                }
            }
            dkCmdBufBindVtxBuffers(R.cmd, 0, ext, m_pipe->bufferCount);
            m_vbDirty = false;
        }
        if (m_stencilDirty && m_pipe->stencil) {
            dkCmdBufSetStencil(R.cmd, DkFace_FrontAndBack, m_pipe->stencilWriteMask, m_stencilRef,
                               m_pipe->stencilReadMask);
            m_stencilDirty = false;
        }
        return true;
    }

    const BindGroup::Entry* entry(uint32_t group, uint32_t binding) const {
        if (group >= 4 || !m_groups[group]) return nullptr;
        return m_groups[group]->find(binding);
    }

    // the stage's uniform buffers, storage buffers and texture/sampler pairs from the bound groups, by
    // the table its translation made (shader_translate.h)
    void bind_stage(const CompiledStage* cs, DkStage stage) {
        DkBufExtents ubo[16], ssbo[16];
        DkResHandle tex[32];
        uint32_t nUbo = 0, nSsbo = 0, nTex = 0;
        for (const SlotBinding& b : cs->bindings) {
            const BindGroup::Entry* e = entry(b.group, b.binding);
            switch (b.kind) {
            case SlotBinding::Kind::Uniform:
            case SlotBinding::Kind::Storage: {
                DkBufExtents x = {null_buffer(), kNullBufferSize};
                if (e && e->buf) {
                    const uint64_t dyn = e->dynIndex >= 0 && e->dynIndex < 8 ? m_offsets[b.group][e->dynIndex] : 0;
                    x.addr = e->buf->block.gpu + e->offset + dyn;
                    x.size = uint32_t(e->size);
                }
                if (b.kind == SlotBinding::Kind::Uniform) {
                    x.size = std::min<uint32_t>(x.size, DK_UNIFORM_BUF_MAX_SIZE);
                    ubo[b.slot] = x;
                    nUbo = std::max<uint32_t>(nUbo, b.slot + 1);
                } else {
                    ssbo[b.slot] = x;
                    nSsbo = std::max<uint32_t>(nSsbo, b.slot + 1);
                }
                break;
            }
            case SlotBinding::Kind::Sampler: {
                View* v = e ? e->view.get() : nullptr;
                Samp* s = nullptr;
                if (b.samplerGroup != kNoSamplerGroup) {
                    const BindGroup::Entry* se = entry(b.samplerGroup, b.samplerBinding);
                    s = se ? se->samp.get() : nullptr;
                }
                tex[b.slot] = texture_handle(v, s);
                nTex = std::max<uint32_t>(nTex, b.slot + 1);
                break;
            }
            }
        }
        // (slots are dense per kind: every slot below the count has a binding)
        if (nUbo) dkCmdBufBindUniformBuffers(R.cmd, stage, 0, ubo, nUbo);
        if (nSsbo) dkCmdBufBindStorageBuffers(R.cmd, stage, 0, ssbo, nSsbo);
        if (nTex) dkCmdBufBindTextures(R.cmd, stage, 0, tex, nTex);
    }

    void dispatch(uint32_t x, uint32_t y, uint32_t z) {
        if (!m_computePass || !m_pipe || !m_pipe->compute || !m_pipe->valid) {
            SWDK_LOG_LIMITED(8, "replay: a dispatch without a usable compute pipeline is skipped");
            return;
        }
        if (m_pipeDirty) {
            const DkShader* shader = &m_pipe->cs->shader;
            dkCmdBufBindShaders(R.cmd, DkStageFlag_Compute, &shader, 1);
            m_pipeDirty = false;
            m_bindDirty = true;
        }
        if (m_bindDirty) {
            bind_stage(m_pipe->cs, DkStage_Compute);
            m_bindDirty = false;
        }
        dkCmdBufDispatchCompute(R.cmd, x, y, z);
        g_stats.dispatches++;
    }

    void copy(const CopyDesc& c) {
        copy_begin();
        g_stats.copies++;
        switch (c.kind) {
        case CopyDesc::BufferToBuffer:
            if (!c.srcBuf || !c.dstBuf || c.size == 0) return;
            dkCmdBufCopyBuffer(R.cmd, c.srcBuf->block.gpu + c.srcOffset, c.dstBuf->block.gpu + c.dstOffset,
                               uint32_t(c.size));
            break;
        case CopyDesc::BufferToTexture:
        case CopyDesc::TextureToBuffer: {
            const bool up = c.kind == CopyDesc::BufferToTexture;
            Buf* buf = up ? c.srcBuf : c.dstBuf;
            Tex* tex = up ? c.dstTex : c.srcTex;
            const uint32_t mip = up ? c.dstMip : c.srcMip;
            const WGPUOrigin3D& o = up ? c.dstOrigin : c.srcOrigin;
            if (!buf || !tex || !tex->valid || mip >= tex->mips) return;
            const uint32_t w = std::min(c.extent.width, tex->level_width(mip) - std::min(o.x, tex->level_width(mip)));
            const uint32_t h =
                std::min(c.extent.height, tex->level_height(mip) - std::min(o.y, tex->level_height(mip)));
            if (w == 0 || h == 0) return;
            const uint32_t rows = c.rowsPerImage ? c.rowsPerImage : (h + tex->fmt.blockH - 1) / tex->fmt.blockH;
            const DkCopyBuf b = {buf->block.gpu + (up ? c.srcOffset : c.dstOffset), c.bytesPerRow,
                                 c.bytesPerRow * rows};
            const DkImageView v = level_view(*tex, mip);
            const DkImageRect r = {o.x, o.y, o.z, w, h, std::max<uint32_t>(c.extent.depthOrArrayLayers, 1)};
            if (up) {
                dkCmdBufCopyBufferToImage(R.cmd, &b, &v, &r, 0);
            } else {
                dkCmdBufCopyImageToBuffer(R.cmd, &v, &r, &b, 0);
            }
            break;
        }
        case CopyDesc::TextureToTexture: {
            Tex* s = c.srcTex;
            Tex* d = c.dstTex;
            if (!s || !d || !s->valid || !d->valid || c.srcMip >= s->mips || c.dstMip >= d->mips) return;
            uint32_t sx = c.srcOrigin.x, sy = c.srcOrigin.y, dx = c.dstOrigin.x, dy = c.dstOrigin.y;
            const uint32_t sw = s->level_width(c.srcMip), sh = s->level_height(c.srcMip);
            const uint32_t dw = d->level_width(c.dstMip), dh = d->level_height(c.dstMip);
            if (sx >= sw || sy >= sh || dx >= dw || dy >= dh) return;
            uint32_t w = std::min({c.extent.width, sw - sx, dw - dx});
            uint32_t h = std::min({c.extent.height, sh - sy, dh - dy});
            if (s->fmt.compressed) {
                // dkCmdBufCopyImage takes compressed rectangles in blocks (SwitchWakerHD)
                w = (w + s->fmt.blockW - 1) / s->fmt.blockW;
                h = (h + s->fmt.blockH - 1) / s->fmt.blockH;
                sx /= s->fmt.blockW;
                sy /= s->fmt.blockH;
                dx /= d->fmt.blockW;
                dy /= d->fmt.blockH;
            }
            const DkImageView sv = level_view(*s, c.srcMip), dv = level_view(*d, c.dstMip);
            const uint32_t layers = std::max<uint32_t>(c.extent.depthOrArrayLayers, 1);
            const DkImageRect sr = {sx, sy, c.srcOrigin.z, w, h, layers}, dr = {dx, dy, c.dstOrigin.z, w, h, layers};
            dkCmdBufCopyImage(R.cmd, &sv, &sr, &dv, &dr, 0);
            break;
        }
        }
    }
};

DkIdxFormat index_format_of(WGPUIndexFormat f) {
    return f == WGPUIndexFormat_Uint32 ? DkIdxFormat_Uint32 : DkIdxFormat_Uint16;
}

void record_bind_group(Encoder* e, OpType type, uint32_t index, WGPUBindGroup group, size_t count,
                       const uint32_t* offsets) {
    if (!e) return;
    Op& op = e->add(type);
    op.index = uint8_t(index);
    op.obj = e->use(g_bindGroups.get(group));
    op.n = uint32_t(std::min<size_t>(count, 8));
    op.u[0] = uint32_t(e->words.size());
    for (uint32_t i = 0; i < op.n; i++) e->words.push_back(offsets[i]);
}

}  // namespace
}  // namespace swdk::wg

using namespace swdk;
using namespace swdk::wg;

extern "C" {

// ---- encoders
WGPUCommandEncoder __wrap_wgpuDeviceCreateCommandEncoder(WGPUDevice device, WGPUCommandEncoderDescriptor const* d) {
    WGPUCommandEncoder enc = __real_wgpuDeviceCreateCommandEncoder(device, d);
    if (enc && R.device) {
        auto e = std::make_shared<Encoder>();
        e->id = g_nextEncoderId.fetch_add(1, std::memory_order_relaxed);
        g_encoders.add(enc, std::move(e));
    }
    return enc;
}

void __wrap_wgpuCommandEncoderAddRef(WGPUCommandEncoder encoder) {
    g_encoders.add_ref(encoder);
    __real_wgpuCommandEncoderAddRef(encoder);
}

void __wrap_wgpuCommandEncoderRelease(WGPUCommandEncoder encoder) {
    g_encoders.release(encoder);
    __real_wgpuCommandEncoderRelease(encoder);
}

WGPURenderPassEncoder __wrap_wgpuCommandEncoderBeginRenderPass(WGPUCommandEncoder encoder,
                                                               WGPURenderPassDescriptor const* d) {
    WGPURenderPassEncoder pass = __real_wgpuCommandEncoderBeginRenderPass(encoder, d);
    auto e = g_encoders.get(encoder);
    if (!pass || !e || !d) return pass;
    PassDesc p;
    p.colorCount = uint32_t(std::min<size_t>(d->colorAttachmentCount, 8));
    for (uint32_t i = 0; i < p.colorCount; i++) {
        const WGPURenderPassColorAttachment& a = d->colorAttachments[i];
        p.color[i] = e->use(g_views.get(a.view));
        p.resolve[i] = e->use(g_views.get(a.resolveTarget));
        p.clear[i] = a.loadOp == WGPULoadOp_Clear;
        p.clearValue[i][0] = float(a.clearValue.r);
        p.clearValue[i][1] = float(a.clearValue.g);
        p.clearValue[i][2] = float(a.clearValue.b);
        p.clearValue[i][3] = float(a.clearValue.a);
    }
    if (const WGPURenderPassDepthStencilAttachment* ds = d->depthStencilAttachment) {
        p.depth = e->use(g_views.get(ds->view));
        p.clearDepth = ds->depthLoadOp == WGPULoadOp_Clear;
        p.depthValue = ds->depthClearValue;
        p.clearStencil = ds->stencilLoadOp == WGPULoadOp_Clear;
        p.stencilValue = uint8_t(ds->stencilClearValue);
    }
    Op& op = e->add(OpType::BeginRender);
    op.n = uint32_t(e->passes.size());
    e->passes.push_back(p);
    pass_add(pass, e);
    return pass;
}

WGPUComputePassEncoder __wrap_wgpuCommandEncoderBeginComputePass(WGPUCommandEncoder encoder,
                                                                 WGPUComputePassDescriptor const* d) {
    WGPUComputePassEncoder pass = __real_wgpuCommandEncoderBeginComputePass(encoder, d);
    auto e = g_encoders.get(encoder);
    if (!pass || !e) return pass;
    e->add(OpType::BeginCompute);
    pass_add(pass, e);
    return pass;
}

void __wrap_wgpuCommandEncoderCopyBufferToBuffer(WGPUCommandEncoder encoder, WGPUBuffer source,
                                                 uint64_t sourceOffset, WGPUBuffer destination,
                                                 uint64_t destinationOffset, uint64_t size) {
    auto e = g_encoders.get(encoder);
    if (!e) return;
    CopyDesc c;
    c.kind = CopyDesc::BufferToBuffer;
    c.srcBuf = e->use(g_buffers.get(source));
    c.dstBuf = e->use(g_buffers.get(destination));
    c.srcOffset = sourceOffset;
    c.dstOffset = destinationOffset;
    c.size = size;
    if (size == WGPU_WHOLE_SIZE && c.srcBuf) c.size = c.srcBuf->size > sourceOffset ? c.srcBuf->size - sourceOffset : 0;
    e->add(OpType::Copy).n = uint32_t(e->copies.size());
    e->copies.push_back(c);
}

void __wrap_wgpuCommandEncoderCopyBufferToTexture(WGPUCommandEncoder encoder, WGPUTexelCopyBufferInfo const* source,
                                                  WGPUTexelCopyTextureInfo const* destination,
                                                  WGPUExtent3D const* copySize) {
    auto e = g_encoders.get(encoder);
    if (!e || !source || !destination || !copySize) return;
    CopyDesc c;
    c.kind = CopyDesc::BufferToTexture;
    c.srcBuf = e->use(g_buffers.get(source->buffer));
    c.srcOffset = source->layout.offset;
    c.bytesPerRow = source->layout.bytesPerRow;
    c.rowsPerImage = source->layout.rowsPerImage == WGPU_COPY_STRIDE_UNDEFINED ? 0 : source->layout.rowsPerImage;
    c.dstTex = e->use(g_textures.get(destination->texture));
    c.dstMip = destination->mipLevel;
    c.dstOrigin = destination->origin;
    c.extent = *copySize;
    e->add(OpType::Copy).n = uint32_t(e->copies.size());
    e->copies.push_back(c);
}

void __wrap_wgpuCommandEncoderCopyTextureToBuffer(WGPUCommandEncoder encoder, WGPUTexelCopyTextureInfo const* source,
                                                  WGPUTexelCopyBufferInfo const* destination,
                                                  WGPUExtent3D const* copySize) {
    auto e = g_encoders.get(encoder);
    if (!e || !source || !destination || !copySize) return;
    CopyDesc c;
    c.kind = CopyDesc::TextureToBuffer;
    c.srcTex = e->use(g_textures.get(source->texture));
    c.srcMip = source->mipLevel;
    c.srcOrigin = source->origin;
    c.dstBuf = e->use(g_buffers.get(destination->buffer));
    c.dstOffset = destination->layout.offset;
    c.bytesPerRow = destination->layout.bytesPerRow;
    c.rowsPerImage =
        destination->layout.rowsPerImage == WGPU_COPY_STRIDE_UNDEFINED ? 0 : destination->layout.rowsPerImage;
    c.extent = *copySize;
    e->add(OpType::Copy).n = uint32_t(e->copies.size());
    e->copies.push_back(c);
}

void __wrap_wgpuCommandEncoderCopyTextureToTexture(WGPUCommandEncoder encoder,
                                                   WGPUTexelCopyTextureInfo const* source,
                                                   WGPUTexelCopyTextureInfo const* destination,
                                                   WGPUExtent3D const* copySize) {
    auto e = g_encoders.get(encoder);
    if (!e || !source || !destination || !copySize) return;
    CopyDesc c;
    c.kind = CopyDesc::TextureToTexture;
    c.srcTex = e->use(g_textures.get(source->texture));
    c.srcMip = source->mipLevel;
    c.srcOrigin = source->origin;
    c.dstTex = e->use(g_textures.get(destination->texture));
    c.dstMip = destination->mipLevel;
    c.dstOrigin = destination->origin;
    c.extent = *copySize;
    e->add(OpType::Copy).n = uint32_t(e->copies.size());
    e->copies.push_back(c);
}

WGPUCommandBuffer __wrap_wgpuCommandEncoderFinish(WGPUCommandEncoder encoder, WGPUCommandBufferDescriptor const* d) {
    WGPUCommandBuffer cb = __real_wgpuCommandEncoderFinish(encoder, d);
    auto e = g_encoders.get(encoder);
    if (cb && e) {
        e->finished = true;
        g_commandBuffers.add(cb, std::move(e));
    }
    return cb;
}

void __wrap_wgpuCommandBufferAddRef(WGPUCommandBuffer buffer) {
    g_commandBuffers.add_ref(buffer);
    __real_wgpuCommandBufferAddRef(buffer);
}

void __wrap_wgpuCommandBufferRelease(WGPUCommandBuffer buffer) {
    g_commandBuffers.release(buffer);
    __real_wgpuCommandBufferRelease(buffer);
}

// ---- render passes: recorded only (Dawn sees the begin and the end)
void __wrap_wgpuRenderPassEncoderSetPipeline(WGPURenderPassEncoder pass, WGPURenderPipeline pipeline) {
    Encoder* e = pass_encoder(pass);
    if (!e) return;
    e->add(OpType::SetRenderPipeline).obj = e->use(g_renderPipelines.get(pipeline));
}

void __wrap_wgpuRenderPassEncoderSetBindGroup(WGPURenderPassEncoder pass, uint32_t groupIndex, WGPUBindGroup group,
                                              size_t dynamicOffsetCount, uint32_t const* dynamicOffsets) {
    record_bind_group(pass_encoder(pass), OpType::SetBindGroup, groupIndex, group, dynamicOffsetCount, dynamicOffsets);
}

void __wrap_wgpuRenderPassEncoderSetVertexBuffer(WGPURenderPassEncoder pass, uint32_t slot, WGPUBuffer buffer,
                                                 uint64_t offset, uint64_t size) {
    Encoder* e = pass_encoder(pass);
    if (!e) return;
    Op& op = e->add(OpType::SetVertexBuffer);
    op.index = uint8_t(slot);
    op.obj = e->use(g_buffers.get(buffer));
    op.q[0] = offset;
    op.q[1] = size;
}

void __wrap_wgpuRenderPassEncoderSetIndexBuffer(WGPURenderPassEncoder pass, WGPUBuffer buffer, WGPUIndexFormat format,
                                                uint64_t offset, uint64_t size) {
    Encoder* e = pass_encoder(pass);
    if (!e) return;
    Op& op = e->add(OpType::SetIndexBuffer);
    op.index = uint8_t(index_format_of(format));
    op.obj = e->use(g_buffers.get(buffer));
    op.q[0] = offset;
    op.q[1] = size;
}

void __wrap_wgpuRenderPassEncoderSetViewport(WGPURenderPassEncoder pass, float x, float y, float width, float height,
                                             float minDepth, float maxDepth) {
    Encoder* e = pass_encoder(pass);
    if (!e) return;
    Op& op = e->add(OpType::SetViewport);
    op.f[0] = x;
    op.f[1] = y;
    op.f[2] = width;
    op.f[3] = height;
    op.f[4] = minDepth;
    op.f[5] = maxDepth;
}

void __wrap_wgpuRenderPassEncoderSetScissorRect(WGPURenderPassEncoder pass, uint32_t x, uint32_t y, uint32_t width,
                                                uint32_t height) {
    Encoder* e = pass_encoder(pass);
    if (!e) return;
    Op& op = e->add(OpType::SetScissor);
    op.u[0] = x;
    op.u[1] = y;
    op.u[2] = width;
    op.u[3] = height;
}

void __wrap_wgpuRenderPassEncoderSetBlendConstant(WGPURenderPassEncoder pass, WGPUColor const* color) {
    Encoder* e = pass_encoder(pass);
    if (!e || !color) return;
    Op& op = e->add(OpType::SetBlendConstant);
    op.f[0] = float(color->r);
    op.f[1] = float(color->g);
    op.f[2] = float(color->b);
    op.f[3] = float(color->a);
}

void __wrap_wgpuRenderPassEncoderSetStencilReference(WGPURenderPassEncoder pass, uint32_t reference) {
    Encoder* e = pass_encoder(pass);
    if (!e) return;
    e->add(OpType::SetStencilReference).u[0] = reference;
}

void __wrap_wgpuRenderPassEncoderDraw(WGPURenderPassEncoder pass, uint32_t vertexCount, uint32_t instanceCount,
                                      uint32_t firstVertex, uint32_t firstInstance) {
    Encoder* e = pass_encoder(pass);
    if (!e || vertexCount == 0 || instanceCount == 0) return;
    Op& op = e->add(OpType::Draw);
    op.u[0] = vertexCount;
    op.u[1] = instanceCount;
    op.u[2] = firstVertex;
    op.u[3] = firstInstance;
}

void __wrap_wgpuRenderPassEncoderDrawIndexed(WGPURenderPassEncoder pass, uint32_t indexCount, uint32_t instanceCount,
                                             uint32_t firstIndex, int32_t baseVertex, uint32_t firstInstance) {
    Encoder* e = pass_encoder(pass);
    if (!e || indexCount == 0 || instanceCount == 0) return;
    Op& op = e->add(OpType::DrawIndexed);
    op.u[0] = indexCount;
    op.u[1] = instanceCount;
    op.u[2] = firstIndex;
    op.u[3] = uint32_t(baseVertex);
    op.u[4] = firstInstance;
}

void __wrap_wgpuRenderPassEncoderEnd(WGPURenderPassEncoder pass) {
    if (Encoder* e = pass_encoder(pass)) {
        Op& op = e->add(OpType::EndRender);
        // the pass this end closes: the newest one begun
        op.n = e->passes.empty() ? 0 : uint32_t(e->passes.size() - 1);
    }
    __real_wgpuRenderPassEncoderEnd(pass);
}

void __wrap_wgpuRenderPassEncoderAddRef(WGPURenderPassEncoder pass) {
    pass_add_ref(pass);
    __real_wgpuRenderPassEncoderAddRef(pass);
}

void __wrap_wgpuRenderPassEncoderRelease(WGPURenderPassEncoder pass) {
    pass_release(pass);
    __real_wgpuRenderPassEncoderRelease(pass);
}

// ---- compute passes
void __wrap_wgpuComputePassEncoderSetPipeline(WGPUComputePassEncoder pass, WGPUComputePipeline pipeline) {
    Encoder* e = pass_encoder(pass);
    if (!e) return;
    e->add(OpType::SetComputePipeline).obj = e->use(g_computePipelines.get(pipeline));
}

void __wrap_wgpuComputePassEncoderSetBindGroup(WGPUComputePassEncoder pass, uint32_t groupIndex, WGPUBindGroup group,
                                               size_t dynamicOffsetCount, uint32_t const* dynamicOffsets) {
    record_bind_group(pass_encoder(pass), OpType::SetBindGroup, groupIndex, group, dynamicOffsetCount, dynamicOffsets);
}

void __wrap_wgpuComputePassEncoderDispatchWorkgroups(WGPUComputePassEncoder pass, uint32_t x, uint32_t y,
                                                     uint32_t z) {
    Encoder* e = pass_encoder(pass);
    if (!e || x == 0 || y == 0 || z == 0) return;
    Op& op = e->add(OpType::Dispatch);
    op.u[0] = x;
    op.u[1] = y;
    op.u[2] = z;
}

void __wrap_wgpuComputePassEncoderEnd(WGPUComputePassEncoder pass) {
    if (Encoder* e = pass_encoder(pass)) e->add(OpType::EndCompute);
    __real_wgpuComputePassEncoderEnd(pass);
}

void __wrap_wgpuComputePassEncoderAddRef(WGPUComputePassEncoder pass) {
    pass_add_ref(pass);
    __real_wgpuComputePassEncoderAddRef(pass);
}

void __wrap_wgpuComputePassEncoderRelease(WGPUComputePassEncoder pass) {
    pass_release(pass);
    __real_wgpuComputePassEncoderRelease(pass);
}

// ---- the queue
void __wrap_wgpuQueueSubmit(WGPUQueue queue, size_t commandCount, WGPUCommandBuffer const* commands) {
    std::vector<std::shared_ptr<Encoder>> encoders;
    encoders.reserve(commandCount);
    for (size_t i = 0; i < commandCount; i++) {
        if (auto e = g_commandBuffers.get(commands[i])) encoders.push_back(std::move(e));
    }
    if (!encoders.empty()) {
        std::lock_guard<std::mutex> lock(queue_mutex());
        const uint64_t serial = recording_serial();
        for (auto& e : encoders) {
            if (e->submitted) continue;  // (Dawn refuses a second submit)
            e->submitted = true;
            Replay replay;
            replay.run(*e);
            std::vector<std::shared_ptr<void>> keep;
            keep.reserve(e->refs.size());
            for (auto& r : e->refs) {
                r->used_by(serial);
                keep.push_back(std::move(r));
            }
            e->refs.clear();
            keep_until_done(std::move(keep));
            // the op list is spent: its memory goes now, not with the command buffer
            std::vector<Op>().swap(e->ops);
            std::vector<CopyDesc>().swap(e->copies);
            std::vector<PassDesc>().swap(e->passes);
            std::vector<uint32_t>().swap(e->words);
        }
        g_stats.submits++;
        flush();
        if (g_stats.submits == 1 || g_stats.submits % 3600 == 0) {
            dklog("replay: %llu submits, %llu passes, %llu draws (%llu skipped), %llu dispatches, %llu copies, "
                  "%llu queue writes (%llu KiB)",
                  (unsigned long long)g_stats.submits, (unsigned long long)g_stats.passes,
                  (unsigned long long)g_stats.draws, (unsigned long long)g_stats.skippedDraws,
                  (unsigned long long)g_stats.dispatches, (unsigned long long)g_stats.copies,
                  (unsigned long long)g_stats.writes, (unsigned long long)(g_stats.writeBytes >> 10));
        }
    }
    __real_wgpuQueueSubmit(queue, commandCount, commands);
}

// A queue write lands before every submit that follows it. When the GPU is done with the buffer and no
// recorded copy into it is pending, the CPU writes the memory itself; otherwise a copy from upload space.
void __wrap_wgpuQueueWriteBuffer(WGPUQueue queue, WGPUBuffer buffer, uint64_t bufferOffset, void const* data,
                                 size_t size) {
    (void)queue;
    auto b = g_buffers.get(buffer);
    if (!b || size == 0 || bufferOffset >= b->size) return;
    size = size_t(std::min<uint64_t>(size, b->size - bufferOffset));
    std::lock_guard<std::mutex> lock(queue_mutex());
    g_stats.writes++;
    g_stats.writeBytes += size;
    if (b->lastSerial.load(std::memory_order_acquire) <= completed_serial()) {
        memcpy(b->block.cpu + bufferOffset, data, size);
        return;
    }
    copy_begin();
    const auto* src = static_cast<const uint8_t*>(data);
    constexpr size_t kPiece = 4u << 20;
    for (size_t at = 0; at < size; at += kPiece) {
        const size_t n = std::min(kPiece, size - at);
        const UploadAlloc u = upload_alloc(uint32_t(n), 16);
        memcpy(u.cpu, src + at, n);
        dkCmdBufCopyBuffer(R.cmd, u.gpu, b->block.gpu + bufferOffset + at, uint32_t(n));
    }
    b->used_by(recording_serial());
    keep_until_done(std::static_pointer_cast<void>(b));
}

void __wrap_wgpuQueueWriteTexture(WGPUQueue queue, WGPUTexelCopyTextureInfo const* destination, void const* data,
                                  size_t dataSize, WGPUTexelCopyBufferLayout const* dataLayout,
                                  WGPUExtent3D const* writeSize) {
    (void)queue;
    if (!destination || !dataLayout || !writeSize) return;
    auto t = g_textures.get(destination->texture);
    if (!t || !t->valid) return;
    const uint32_t mip = destination->mipLevel;
    if (mip >= t->mips) return;
    const FormatInfo& f = t->fmt;
    const WGPUOrigin3D& o = destination->origin;
    const uint32_t lw = t->level_width(mip), lh = t->level_height(mip);
    if (o.x >= lw || o.y >= lh) return;
    const uint32_t w = std::min(writeSize->width, lw - o.x), h = std::min(writeSize->height, lh - o.y);
    const uint32_t layers = std::max<uint32_t>(writeSize->depthOrArrayLayers, 1);
    if (w == 0 || h == 0) return;
    const uint32_t blocksW = (w + f.blockW - 1) / f.blockW, blocksH = (h + f.blockH - 1) / f.blockH;
    const uint32_t rowBytes = blocksW * f.blockBytes;
    const uint32_t srcPitch = dataLayout->bytesPerRow == WGPU_COPY_STRIDE_UNDEFINED ? rowBytes : dataLayout->bytesPerRow;
    const uint32_t srcRows =
        dataLayout->rowsPerImage == WGPU_COPY_STRIDE_UNDEFINED || dataLayout->rowsPerImage == 0 ? blocksH
                                                                                                 : dataLayout->rowsPerImage;
    const uint64_t need = dataLayout->offset + uint64_t(srcPitch) * srcRows * (layers - 1) +
                          uint64_t(srcPitch) * (blocksH - 1) + rowBytes;
    if (need > dataSize) {
        SWDK_LOG_LIMITED(8, "textures: a texture write of %zu bytes needs %llu: skipped", dataSize,
                         (unsigned long long)need);
        return;
    }
    // rows repacked at a 256-byte pitch in upload space
    const uint32_t pitch = align_up(rowBytes, 256);
    const uint64_t total = uint64_t(pitch) * blocksH * layers;
    if (total > 0xFFFF0000ull) return;
    std::lock_guard<std::mutex> lock(queue_mutex());
    g_stats.writes++;
    g_stats.writeBytes += total;
    const UploadAlloc u = upload_alloc(uint32_t(total), 256);
    const auto* src = static_cast<const uint8_t*>(data) + dataLayout->offset;
    for (uint32_t z = 0; z < layers; z++) {
        for (uint32_t y = 0; y < blocksH; y++) {
            memcpy(u.cpu + (uint64_t(z) * blocksH + y) * pitch, src + (uint64_t(z) * srcRows + y) * srcPitch, rowBytes);
        }
    }
    copy_begin();
    const DkCopyBuf b = {u.gpu, pitch, pitch * blocksH};
    const DkImageView v = level_view(*t, mip);
    const DkImageRect r = {o.x, o.y, o.z, w, h, layers};
    dkCmdBufCopyBufferToImage(R.cmd, &b, &v, &r, 0);
    keep_until_done(std::static_pointer_cast<void>(t));
}

// ---- the present: Aurora's picture, scaled to fit, into a swapchain image
bool aurora_switch_dk_present(void* texture) {
    auto t = g_textures.get(texture);
    if (!R.device || !t || !t->valid || !t->fmt.can2d) {
        SWDK_LOG_LIMITED(8, "present: the picture has no deko3d image: nothing shown");
        return false;
    }
    std::lock_guard<std::mutex> lock(queue_mutex());
    const int slot = swapchain_acquire();
    const DkImage& target = swapchain_image(slot);
    DkImageView dst;
    dkImageViewDefaults(&dst, &target);
    // aspect fit
    const uint32_t sw = t->width, sh = t->height, dw = R.width, dh = R.height;
    uint32_t fw = dw, fh = uint32_t(uint64_t(dw) * sh / sw);
    if (fh > dh) {
        fh = dh;
        fw = uint32_t(uint64_t(dh) * sw / sh);
    }
    fw = std::max(fw, 1u);
    fh = std::max(fh, 1u);
    const uint32_t fx = (dw - fw) / 2, fy = (dh - fh) / 2;
    // the picture's draws done (whatever ran last)
    dkCmdBufBarrier(R.cmd, DkBarrier_Full, DkInvalidateFlags_Image);
    if (fw != dw || fh != dh) {
        const DkImageView* targets[1] = {&dst};
        dkCmdBufBindRenderTargets(R.cmd, targets, 1, nullptr);
        const DkScissor s = {0, 0, dw, dh};
        dkCmdBufSetScissors(R.cmd, 0, &s, 1);
        dkCmdBufClearColorFloat(R.cmd, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);
        dkCmdBufBarrier(R.cmd, DkBarrier_Fragments, 0);
    }
    const DkImageView src = level_view(*t, 0);
    const DkImageRect sr = {0, 0, 0, sw, sh, 1};
    const DkImageRect dr = {fx, fy, 0, fw, fh, 1};
    dkCmdBufBlitImage(R.cmd, &src, &sr, &dst, &dr, DkBlitFlag_FilterLinear, 0);
    g_copyRun = true;  // the next draw into Aurora's picture waits for the blit
    keep_until_done(std::static_pointer_cast<void>(t));
    flush();
    swapchain_present(slot);
    if (R.presents == 1 || R.presents % 3600 == 0) {
        const MemoryStats m = memory_stats();
        const ShaderStats s = shader_stats();
        dklog("present %llu: %ux%u picture; images %llu MiB in %llu chunks, buffers %llu MiB in %llu blocks, %llu "
              "submits; shaders %llu from the cache file, %llu compiled (%.0f ms, longest %.0f ms), %llu failed; "
              "objects: %zu textures, %zu views, %zu buffers, %zu pipelines",
              (unsigned long long)R.presents, sw, sh, (unsigned long long)(m.imageBytes >> 20),
              (unsigned long long)m.imageChunks, (unsigned long long)(m.blockBytes >> 20),
              (unsigned long long)m.blocks, (unsigned long long)m.submits, (unsigned long long)s.fromFile,
              (unsigned long long)s.compiled, double(s.compileNs) / 1e6, double(s.compileMaxNs) / 1e6,
              (unsigned long long)s.failed, g_textures.size(), g_views.size(), g_buffers.size(),
              g_renderPipelines.size());
        log_flush();
    }
    return true;
}

}  // extern "C"
