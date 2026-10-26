// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of
// the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
// The texture, sampler and format parts are adapted from SwitchWaker (https://github.com/centollOS/SwitchWaker)
// at 92307ff: switch/deko/aurora/dk_objects.hpp and dk_pipeline.cpp.
#pragma once
// The deko3d shadows of Aurora's WebGPU objects (switch/README.md, "Graphics"). Dawn's Null device stays
// Aurora's object model: every object is still created, validated and counted by Dawn, which draws
// nothing. The linker's --wrap (switch/cmake/SpmSwitch.cmake) puts the __wrap_ functions of
// wgpu_objects.cpp and wgpu_encode.cpp in front of Dawn's for every caller in the NRO; each calls
// Dawn's own (__real_) where Dawn needs to see the call, and keeps a shadow in step:
//   texture        -> a DkImage in the image heap
//   texture view   -> a DkImageView of it, and an image descriptor slot once sampled
//   sampler        -> a DkSampler (its descriptor slot from descriptors.cpp's cache by key)
//   buffer         -> a memory block of its own; GetMappedRange hands Aurora that memory
//   bind group     -> its entries' shadows, with each dynamic offset's index
//   shader module  -> its WGSL
//   pipeline       -> the compiled stages (shaders.cpp) and the fixed-function state in deko3d's terms
//   encoders       -> recorded commands, replayed into deko3d at the queue submit (wgpu_encode.cpp)
// Lifetimes: a handle's shadow is held by shared_ptr. The map entry counts Aurora's references
// (create 1, AddRef +1, Release -1) and goes at 0; whatever still uses the shadow holds its own
// shared_ptr (a view its texture, a bind group its entries, recorded commands everything they use,
// and a submit everything until the GPU has finished it), so memory and descriptor slots are given
// back only once nothing, the GPU included, can reach them.
#include <deko3d.h>
#include <webgpu/webgpu.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "dk.h"

namespace swdk::wg {

struct Object {
    virtual ~Object() = default;
    // the last encoder that took a reference (wgpu_encode.cpp: one reference per encoder)
    std::atomic<uint64_t> encoderMark{0};
    // a submit used the object; buffers note the serial (their maps wait for it)
    virtual void used_by(uint64_t /*serial*/) {}
};
using ObjectRef = std::shared_ptr<Object>;

// The deko3d format of a WebGPU texture format (DkImageFormat_None when unsupported), with its block
// geometry (compressed formats: blocks of blockW x blockH)
struct FormatInfo {
    DkImageFormat format = DkImageFormat_None;
    uint8_t blockW = 1, blockH = 1, blockBytes = 4;
    bool depth = false, stencil = false, compressed = false, integer = false, canRender = false, can2d = false;
};
FormatInfo format_info(WGPUTextureFormat f);

struct Tex : Object {
    WGPUTextureFormat wformat = WGPUTextureFormat_Undefined;
    FormatInfo fmt;
    uint32_t width = 1, height = 1, layers = 1, mips = 1;
    bool is3d = false, valid = false;
    DkImage image{};
    ImageAlloc mem;
    std::string label;
    ~Tex() override;
    uint32_t level_width(uint32_t level) const { return width >> level ? width >> level : 1; }
    uint32_t level_height(uint32_t level) const { return height >> level ? height >> level : 1; }
};

struct View : Object {
    std::shared_ptr<Tex> tex;
    DkImageView view{};  // pImage: &tex->image
    uint32_t baseMip = 0, mipCount = 1, baseLayer = 0, layerCount = 1;
    uint32_t slot = 0;  // the image descriptor, written on first use (under queue_mutex())
    ~View() override;
    bool valid() const { return tex && tex->valid; }
};

struct Samp : Object {
    DkSampler sampler{};
    uint64_t key = 0;
};

struct Buf : Object {
    Block block;
    uint64_t size = 0;
    uint64_t usage = 0;
    std::string label;
    std::atomic<uint64_t> lastSerial{0};  // the newest submit that used it
    ~Buf() override;
    void used_by(uint64_t serial) override { lastSerial.store(serial, std::memory_order_release); }
};

struct BindGroupLayout : Object {
    std::vector<uint32_t> dynamicBindings;  // the bindings with a dynamic offset, in increasing order
};

struct BindGroup : Object {
    struct Entry {
        uint32_t binding = 0;
        std::shared_ptr<Buf> buf;
        uint64_t offset = 0, size = 0;  // size: WGPU_WHOLE_SIZE resolved
        std::shared_ptr<View> view;
        std::shared_ptr<Samp> samp;
        int dynIndex = -1;  // its dynamic offset among SetBindGroup's
    };
    std::vector<Entry> entries;  // by binding
    const Entry* find(uint32_t binding) const;
};

struct ShaderModule : Object {
    std::string wgsl;
};

struct Pipe : Object {
    bool compute = false;
    bool valid = false;
    const CompiledStage* vs = nullptr;
    const CompiledStage* fs = nullptr;
    const CompiledStage* cs = nullptr;
    DkPrimitive prim = DkPrimitive_Triangles;
    bool strip = false;
    DkRasterizerState rs{};
    DkColorState color{};
    DkColorWriteState colorWrite{};
    DkBlendState blend[8]{};
    uint32_t blendCount = 0;
    DkDepthStencilState ds{};
    float depthBias[3] = {};  // constant, clamp, slope
    bool stencil = false;
    uint8_t stencilReadMask = 0xFF, stencilWriteMask = 0xFF;
    DkVtxAttribState attribs[32]{};
    uint32_t attribCount = 0;
    DkVtxBufferState buffers[16]{};
    uint32_t bufferCount = 0;
    std::string label;
};

// Handle -> shadow, with Aurora's reference count. Any thread.
template <class T> class Registry {
public:
    void add(const void* handle, std::shared_ptr<T> obj) {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        m_map[handle] = Rec{std::move(obj), 1};  // (a stale entry: Dawn reused the address)
    }
    std::shared_ptr<T> get(const void* handle) {
        if (!handle) return nullptr;
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        auto it = m_map.find(handle);
        return it == m_map.end() ? nullptr : it->second.obj;
    }
    void add_ref(const void* handle) {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        auto it = m_map.find(handle);
        if (it != m_map.end()) it->second.refs++;
    }
    // Before Dawn's release: Dawn may free the object and hand its address to the next one.
    // The shadow is destroyed outside the lock (destructors take other locks).
    void release(const void* handle) {
        std::shared_ptr<T> last;
        {
            std::unique_lock<std::shared_mutex> lock(m_mutex);
            auto it = m_map.find(handle);
            if (it == m_map.end()) return;
            if (--it->second.refs != 0) return;
            last = std::move(it->second.obj);
            m_map.erase(it);
        }
    }
    size_t size() {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        return m_map.size();
    }
    template <class F> void for_each(F&& f) {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        for (auto& [h, rec] : m_map) f(*rec.obj);
    }

private:
    struct Rec {
        std::shared_ptr<T> obj;
        uint32_t refs = 0;
    };
    std::shared_mutex m_mutex;
    std::unordered_map<const void*, Rec> m_map;
};

extern Registry<Tex> g_textures;
extern Registry<View> g_views;
extern Registry<Samp> g_samplers;
extern Registry<Buf> g_buffers;
extern Registry<BindGroupLayout> g_bindGroupLayouts;
extern Registry<BindGroup> g_bindGroups;
extern Registry<ShaderModule> g_shaderModules;
extern Registry<Pipe> g_renderPipelines;
extern Registry<Pipe> g_computePipelines;

// Under queue_mutex(): the view's image descriptor slot, written on first use (kEmptyImage if none is
// free or the view has no image)
uint32_t view_slot(View* v);
// Under queue_mutex(): a combined handle for a view and a sampler (null sampler: nearest, clamped)
DkResHandle texture_handle(View* v, Samp* s);
// A view of one level (and layer) of a texture: transfers
DkImageView level_view(const Tex& t, uint32_t level);

// Under queue_mutex(): a zeroed buffer for bindings that have nothing behind them
DkGpuAddr null_buffer();
constexpr uint32_t kNullBufferSize = 0x10000;

std::string to_string(WGPUStringView s);
// log a message the first `limit` times its call site is reached
#define SWDK_LOG_LIMITED(limit, ...)              \
    do {                                          \
        static std::atomic<int> logged_{0};       \
        if (logged_.fetch_add(1) < (limit)) swdk::dklog(__VA_ARGS__); \
    } while (0)

}  // namespace swdk::wg
