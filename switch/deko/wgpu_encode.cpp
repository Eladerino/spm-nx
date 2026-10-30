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
#include <cstdio>
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

// CPU time of the queue's work since the last pacing line (any thread): the wait for queue_mutex,
// our replay and flush, and Dawn's own submit after it
struct CpuTimes {
    std::atomic<uint64_t> lockWaitNs{0}, replayNs{0}, dawnSubmitNs{0};
};
CpuTimes g_cpu;

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
            note_sampled_textures();
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
            note_sampled_textures();
            g_stats.draws++;
            break;
        case OpType::Dispatch:
            dispatch(op.u[0], op.u[1], op.u[2]);
            note_sampled_textures();
            break;
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
            // diag efbclear: the EFB's passes (the only ones with a depth target) clear magenta, so a
            // magenta screen proves the EFB reaches the picture and a black one proves it does not
            static const float kMagenta[4] = {1.0f, 0.0f, 1.0f, 1.0f};
            if (g_diag.efbClear && hasDepth) c = kMagenta;
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
            if (g_diag.cullOff) {
                DkRasterizerState rs = m_pipe->rs;
                rs.cullMode = DkFace_None;
                dkCmdBufBindRasterizerState(R.cmd, &rs);
            } else {
                dkCmdBufBindRasterizerState(R.cmd, &m_pipe->rs);
            }
            dkCmdBufBindColorState(R.cmd, &m_pipe->color);
            dkCmdBufBindColorWriteState(R.cmd, &m_pipe->colorWrite);
            if (m_pipe->blendCount) dkCmdBufBindBlendStates(R.cmd, 0, m_pipe->blend, m_pipe->blendCount);
            if (g_diag.depthOff) {
                DkDepthStencilState ds = m_pipe->ds;
                ds.depthTestEnable = 0;
                ds.depthWriteEnable = 0;
                ds.stencilTestEnable = 0;
                dkCmdBufBindDepthStencilState(R.cmd, &ds);
            } else {
                dkCmdBufBindDepthStencilState(R.cmd, &m_pipe->ds);
            }
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

    // diag texdump: count this draw or dispatch against every texture it samples, so the dump
    // at a present can pick the textures the game is actually using (the crowd's sprite sheets,
    // say) instead of guessing by size. Only runs while the switch is on.
    void note_sampled_textures() {
        if (!g_diag.texDump || !m_pipe) return;
        const CompiledStage* stages[3] = {m_pipe->vs, m_pipe->fs, m_pipe->cs};
        for (const CompiledStage* cs : stages) {
            if (!cs) continue;
            for (const SlotBinding& b : cs->bindings) {
                if (b.kind != SlotBinding::Kind::Sampler) continue;
                const BindGroup::Entry* e = entry(b.group, b.binding);
                if (e && e->view && e->view->tex) e->view->tex->sampledBinds++;
            }
        }
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

namespace {

// Diagnostic readback (dk.h, picdump): copies the picture into a CPU-readable block, waits for
// the GPU, and logs what is actually in it: how many sampled pixels are lit, the brightest
// channel, each quadrant's average brightness, the centre pixel. Runs at a handful of present
// calls only; each one stalls the present for one GPU round trip. Under queue_mutex().
// diag bufdump: with the GPU idle, every live buffer's size, usage, label and how many of its first
// 4 MiB of 32-bit words are non-zero (staging should be full, and so should the vertex, uniform,
// index and storage buffers it is copied into; zeros there mean the copies never landed)
void buf_dump_maybe(uint64_t presentCall) {
    if (presentCall != 600 && presentCall != 1800 && presentCall != 3600) return;
    dkQueueWaitIdle(R.queue);
    dklog("bufdump at present %llu:", (unsigned long long)presentCall);
    g_buffers.for_each([](const Buf& b) {
        const uint64_t n = std::min<uint64_t>(b.size, 4u << 20) / 4;
        const uint32_t* w = reinterpret_cast<const uint32_t*>(b.block.cpu);
        uint64_t nonzero = 0, firstNonzero = UINT64_MAX;
        for (uint64_t i = 0; i < n; i++) {
            if (w[i]) {
                nonzero++;
                if (firstNonzero == UINT64_MAX) firstNonzero = i * 4;
            }
        }
        dklog("  buffer %s: %llu bytes, usage 0x%llX, last submit %llu, %llu of %llu words non-zero (first at %lld), "
              "words 0-3: %08X %08X %08X %08X",
              b.label.empty() ? "(no label)" : b.label.c_str(), (unsigned long long)b.size,
              (unsigned long long)b.usage, (unsigned long long)b.lastSerial.load(), (unsigned long long)nonzero,
              (unsigned long long)n, firstNonzero == UINT64_MAX ? -1ll : (long long)firstNonzero,
              n > 0 ? w[0] : 0, n > 1 ? w[1] : 0, n > 2 ? w[2] : 0, n > 3 ? w[3] : 0);
    });
    log_flush();
}

void pic_dump_maybe(const Tex& t, uint64_t presentCall) {
    if (presentCall != 90 && presentCall != 1500 && presentCall != 5400 && presentCall != 10800) return;
    if (t.fmt.compressed || t.fmt.depth || t.fmt.blockBytes != 4) {
        dklog("diag picdump @%llu: skipped, the picture format is not 4-byte pixels",
              (unsigned long long)presentCall);
        log_flush();
        return;
    }
    const uint32_t w = t.width, h = t.height, pitch = w * 4;
    static Block s_block;
    if (s_block && s_block.size < pitch * h) {
        block_destroy(s_block);
        s_block = Block{};
    }
    if (!s_block) s_block = block_create(pitch * h, "diag picdump");
    if (!s_block) {
        dklog("diag picdump: the readback block could not be created");
        return;
    }
    const DkImageView src = level_view(t, 0);
    const DkImageRect r = {0, 0, 0, w, h, 1};
    const DkCopyBuf b = {s_block.gpu, pitch, pitch * h};
    dkCmdBufCopyImageToBuffer(R.cmd, &src, &r, &b, 0);
    const uint64_t serial = recording_serial();
    flush();
    if (!wait_serial(serial)) {
        dklog("diag picdump: the GPU did not finish the copy in time");
        log_flush();
        return;
    }
    uint64_t lit = 0, samples = 0;
    uint32_t maxv = 0;
    uint64_t quadLum[4] = {0, 0, 0, 0}, quadN[4] = {0, 0, 0, 0};
    for (uint32_t y = 0; y < h; y += 8) {
        for (uint32_t x = 0; x < w; x += 8) {
            const uint8_t* p = s_block.cpu + uint64_t(y) * pitch + uint64_t(x) * 4;
            if (p[0] > 8 || p[1] > 8 || p[2] > 8) lit++;
            maxv = std::max({maxv, uint32_t(p[0]), uint32_t(p[1]), uint32_t(p[2])});
            const int q = (y >= h / 2 ? 2 : 0) + (x >= w / 2 ? 1 : 0);
            quadLum[q] += (uint32_t(p[0]) + p[1] + p[2]) / 3;
            quadN[q]++;
            samples++;
        }
    }
    const uint8_t* c = s_block.cpu + uint64_t(h / 2) * pitch + uint64_t(w / 2) * 4;
    dklog("diag picdump @%llu: %ux%u picture, %llu of %llu sampled pixels lit, brightest channel %u, quadrant "
          "brightness TL %llu TR %llu BL %llu BR %llu, centre pixel (%u,%u,%u,%u)",
          (unsigned long long)presentCall, w, h, (unsigned long long)lit, (unsigned long long)samples, maxv,
          (unsigned long long)(quadN[0] ? quadLum[0] / quadN[0] : 0),
          (unsigned long long)(quadN[1] ? quadLum[1] / quadN[1] : 0),
          (unsigned long long)(quadN[2] ? quadLum[2] / quadN[2] : 0),
          (unsigned long long)(quadN[3] ? quadLum[3] / quadN[3] : 0), c[0], c[1], c[2], c[3]);
    log_flush();
}

// diag texdump: at a few presents, the textures the game sampled most since the last dump
// (Tex::sampledBinds, counted in the replay while the switch is on) are copied out and saved
// under UserData/Logs: a .ppm for plain 4-byte pixel formats, which opens anywhere, and the
// raw bytes for anything else, with one manifest line per level in the log. Every mip level
// is dumped, not just level 0: level 0 checks out clean on all 18 textures captured,
// while the on-screen corruption hits small, distant sprites, which is exactly the content
// the GPU samples from the deeper levels. A saved level can then be compared with what the
// screen showed, which separates an upload that was already wrong (the corruption is in the
// file) from sampling that goes wrong (the file is clean). Under queue_mutex(); each level
// stalls for one GPU round trip, like picdump.
void tex_dump_maybe(uint64_t presentCall) {
    if (presentCall != 1800 && presentCall != 3600 && presentCall != 5400) return;
    uint32_t counts[1024];
    size_t nCounts = 0;
    g_textures.for_each([&](const Tex& t) {
        if (t.valid && !t.fmt.depth && t.sampledBinds && nCounts < 1024) counts[nCounts++] = t.sampledBinds;
    });
    if (!nCounts) {
        dklog("diag texdump @%llu: nothing was sampled since the last dump", (unsigned long long)presentCall);
        log_flush();
        return;
    }
    std::sort(counts, counts + nCounts, [](uint32_t a, uint32_t b) { return a > b; });
    const uint32_t threshold = counts[std::min<size_t>(nCounts, 6) - 1];
    dklog("diag texdump @%llu: %zu sampled textures, dumping the ones with at least %u sampled draws",
          (unsigned long long)presentCall, nCounts, threshold);
    static Block s_block;
    uint32_t dumped = 0;
    g_textures.for_each([&](const Tex& t) {
        if (dumped >= 6 || !t.valid || t.fmt.depth || t.sampledBinds < threshold) return;
        for (uint32_t level = 0; level < t.mips; level++) {
            const uint32_t w = t.level_width(level), h = t.level_height(level);
            const uint32_t pitch = (w + t.fmt.blockW - 1) / t.fmt.blockW * t.fmt.blockBytes;
            const uint32_t rows = (h + t.fmt.blockH - 1) / t.fmt.blockH;
            const uint32_t total = pitch * rows;
            if (!total || total > (16u << 20)) {
                dklog("  texdump: skipped mip %u of a %ux%u texture (format %u), %u bytes is too large to dump",
                      level, t.width, t.height, unsigned(t.wformat), total);
                continue;
            }
            if (s_block && s_block.size < total) {
                block_destroy(s_block);
                s_block = Block{};
            }
            if (!s_block) s_block = block_create(total, "diag texdump");
            if (!s_block) return;
            const DkImageView src = level_view(t, level);
            const DkImageRect r = {0, 0, 0, w, h, 1};
            const DkCopyBuf b = {s_block.gpu, pitch, total};
            dkCmdBufCopyImageToBuffer(R.cmd, &src, &r, &b, 0);
            const uint64_t serial = recording_serial();
            flush();
            if (!wait_serial(serial)) {
                dklog("  texdump: the GPU did not finish a copy in time");
                continue;
            }
            const bool pixels = !t.fmt.compressed && t.fmt.blockBytes == 4;
            char path[128];
            std::snprintf(path, sizeof(path), "/switch/spm-nx/UserData/Logs/texdump_p%llu_%u_%ux%u_f%u_m%u.%s",
                          (unsigned long long)presentCall, dumped, t.width, t.height, unsigned(t.wformat),
                          level, pixels ? "ppm" : "bin");
            FILE* f = std::fopen(path, "wb");
            if (!f) {
                dklog("  texdump: could not open %s", path);
                continue;
            }
            if (pixels) {
                std::fprintf(f, "P6\n%u %u\n255\n", w, h);
                std::vector<uint8_t> row(w * 3);
                for (uint32_t y = 0; y < h; y++) {
                    const uint8_t* srcRow = s_block.cpu + uint64_t(y) * pitch;
                    for (uint32_t x = 0; x < w; x++) {
                        row[x * 3 + 0] = srcRow[x * 4 + 0];
                        row[x * 3 + 1] = srcRow[x * 4 + 1];
                        row[x * 3 + 2] = srcRow[x * 4 + 2];
                    }
                    std::fwrite(row.data(), 1, row.size(), f);
                }
            } else {
                std::fwrite(s_block.cpu, 1, total, f);
            }
            std::fclose(f);
            if (pixels) {
                uint64_t bands[16] = {}, bandN[16] = {};
                for (uint32_t y = 0; y < h; y += 4) {
                    for (uint32_t x = 0; x < w; x += 4) {
                        const uint8_t* p = s_block.cpu + uint64_t(y) * pitch + uint64_t(x) * 4;
                        const uint32_t band = std::min<uint32_t>(x * 16 / w, 15);
                        bands[band] += (uint32_t(p[0]) + p[1] + p[2]) / 3;
                        bandN[band]++;
                    }
                }
                uint32_t avg[16];
                for (int i = 0; i < 16; i++) avg[i] = bandN[i] ? uint32_t(bands[i] / bandN[i]) : 0;
                dklog("  texdump %u mip %u/%u: %ux%u (level %ux%u) format %u, %u sampled draws, saved %s; "
                      "column brightness %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u",
                      dumped, level + 1, t.mips, t.width, t.height, w, h, unsigned(t.wformat), t.sampledBinds,
                      path, avg[0], avg[1], avg[2], avg[3], avg[4], avg[5], avg[6], avg[7], avg[8], avg[9],
                      avg[10], avg[11], avg[12], avg[13], avg[14], avg[15]);
            } else {
                uint64_t nonzero = 0;
                for (uint32_t i = 0; i < total; i++) nonzero += s_block.cpu[i] != 0;
                dklog("  texdump %u mip %u/%u: %ux%u (level %ux%u) format %u, %u sampled draws, saved %s; "
                      "%llu of %u bytes non-zero",
                      dumped, level + 1, t.mips, t.width, t.height, w, h, unsigned(t.wformat), t.sampledBinds,
                      path, (unsigned long long)nonzero, total);
            }
        }
        dumped++;
    });
    g_textures.for_each([](Tex& t) { t.sampledBinds = 0; });
    log_flush();
}
}  // namespace

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
        const uint64_t l0 = now_ns();
        std::lock_guard<std::mutex> lock(queue_mutex());
        const uint64_t r0 = now_ns();
        g_cpu.lockWaitNs.fetch_add(r0 - l0, std::memory_order_relaxed);
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
        g_cpu.replayNs.fetch_add(now_ns() - r0, std::memory_order_relaxed);
        if (g_stats.submits == 1 || g_stats.submits % 3600 == 0) {
            dklog("replay: %llu submits, %llu passes, %llu draws (%llu skipped), %llu dispatches, %llu copies, "
                  "%llu queue writes (%llu KiB)",
                  (unsigned long long)g_stats.submits, (unsigned long long)g_stats.passes,
                  (unsigned long long)g_stats.draws, (unsigned long long)g_stats.skippedDraws,
                  (unsigned long long)g_stats.dispatches, (unsigned long long)g_stats.copies,
                  (unsigned long long)g_stats.writes, (unsigned long long)(g_stats.writeBytes >> 10));
        }
    }
    const uint64_t d0 = now_ns();
    __real_wgpuQueueSubmit(queue, commandCount, commands);
    g_cpu.dawnSubmitNs.fetch_add(now_ns() - d0, std::memory_order_relaxed);
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
    // (the wait for a free image, up to a vsync, outside the lock: the game's submits go on meanwhile)
    DkFence imageFence{};
    const uint64_t a0 = now_ns();
    const int slot = swapchain_acquire(&imageFence);
    const uint64_t acquireNs = now_ns() - a0;
    std::unique_lock<std::mutex> lock(queue_mutex());
    swapchain_wait(&imageFence);
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
    static uint64_t s_presentCalls = 0;
    const uint64_t presentCall = ++s_presentCalls;
    if (g_diag.picDump) pic_dump_maybe(*t, presentCall);
    if (g_diag.magenta || fw != dw || fh != dh) {
        const DkImageView* targets[1] = {&dst};
        dkCmdBufBindRenderTargets(R.cmd, targets, 1, nullptr);
        const DkScissor s = {0, 0, dw, dh};
        dkCmdBufSetScissors(R.cmd, 0, &s, 1);
        // diag magenta: a clear colour the game never uses, so any of it left on screen after
        // the blit marks exactly the pixels the blit did not cover
        dkCmdBufClearColorFloat(R.cmd, 0, DkColorMask_RGBA, g_diag.magenta ? 1.0f : 0.0f, 0.0f,
                                g_diag.magenta ? 1.0f : 0.0f, 1.0f);
        dkCmdBufBarrier(R.cmd, DkBarrier_Fragments, 0);
    }
    const DkImageView src = level_view(*t, 0);
    const DkImageRect sr = {0, 0, 0, sw, sh, 1};
    const DkImageRect dr = {fx, fy, 0, fw, fh, 1};
    dkCmdBufBlitImage(R.cmd, &src, &sr, &dst, &dr, DkBlitFlag_FilterLinear, 0);
    g_copyRun = true;  // the next draw into Aurora's picture waits for the blit
    keep_until_done(std::static_pointer_cast<void>(t));
    flush();
    if (g_diag.bufDump) buf_dump_maybe(presentCall);
    if (g_diag.texDump) tex_dump_maybe(presentCall);
    swapchain_present(slot);
    // frame pacing over the last 600 presents: the gaps between presents, the acquire waits and the
    // buffer maps' waits for the GPU (wgpu_objects.cpp)
    static uint64_t s_lastPresentNs = 0, s_gapMaxNs = 0, s_gapSlow = 0, s_acquireNs = 0, s_acquireMaxNs = 0;
    static uint64_t s_windowStartNs = 0, s_windowDraws = 0;
    const uint64_t nowNs = now_ns();
    if (s_lastPresentNs) {
        const uint64_t gap = nowNs - s_lastPresentNs;
        s_gapMaxNs = std::max(s_gapMaxNs, gap);
        if (gap > 20'000'000) s_gapSlow++;  // a frame missed (60 Hz: 16.7 ms)
    }
    s_lastPresentNs = nowNs;
    s_acquireNs += acquireNs;
    s_acquireMaxNs = std::max(s_acquireMaxNs, acquireNs);
    if (!s_windowStartNs) s_windowStartNs = nowNs;
    if (R.presents % 600 == 0) {
        const MapWaitStats mw = map_wait_stats_take();
        // per present, over the window: the average gap, and the queue's CPU time
        const double perFrame = 600e6;
        const double windowMs = double(nowNs - s_windowStartNs) / 1e6;
        dklog("cpu per frame, presents %llu-%llu: %.1f ms between presents; replay %.2f ms, queue lock wait %.2f "
              "ms, Dawn submit %.2f ms; %llu draws",
              (unsigned long long)(R.presents - 599), (unsigned long long)R.presents, windowMs / 600.0,
              double(g_cpu.replayNs.exchange(0)) / perFrame, double(g_cpu.lockWaitNs.exchange(0)) / perFrame,
              double(g_cpu.dawnSubmitNs.exchange(0)) / perFrame,
              (unsigned long long)((g_stats.draws - s_windowDraws) / 600));
        s_windowDraws = g_stats.draws;
        s_windowStartNs = nowNs;
        dklog("pacing, presents %llu-%llu: %llu over 20 ms (longest %.1f ms); acquire %.1f ms avg, %.1f max; "
              "%llu map waits, %.1f ms total, %.1f max",
              (unsigned long long)(R.presents - 599), (unsigned long long)R.presents, (unsigned long long)s_gapSlow,
              double(s_gapMaxNs) / 1e6, double(s_acquireNs) / 600e6, double(s_acquireMaxNs) / 1e6,
              (unsigned long long)mw.count, double(mw.totalNs) / 1e6, double(mw.maxNs) / 1e6);
        s_gapMaxNs = s_gapSlow = s_acquireNs = s_acquireMaxNs = 0;
    }
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
    // a dk_cmd.txt command, if one is waiting (device.cpp): looked for every 30th present, twice a
    // second, and outside the queue lock, because the look is a file open on the SD card
    lock.unlock();
    if (R.presents % 30 == 0) diag_cmd_poll();
    return true;
}

}  // extern "C"
