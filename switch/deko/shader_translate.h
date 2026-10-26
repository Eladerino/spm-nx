#pragma once
// WGSL -> GLSL for uam (deko3d's shader compiler). From SwitchWaker (MIT) at 92307ff:
// switch/deko/shader_translate.h, with one binding scheme for every shader (spm-nx draws whatever
// WebGPU pipeline Aurora creates, not only GX's) and compute shaders.
//
//   WGSL --Tint's GLSL writer--> GLSL 4.60 --post_pass--> GLSL that uam accepts --uam--> DKSH
//
// Tint options: desktop GLSL 4.60, depth_zero_to_one (the device clips z to [0, w]),
// disable_position_y_negation (dawn-switch-tint-position-y-up.patch: deko3d's clip space with an
// upper-left origin is WebGPU's), real uniform blocks, robustness off, names kept, and explicit
// bindings everywhere because uam has no linking: every binding is the deko3d slot itself.
//
// Slots, per stage: uniform buffers, storage buffers and (texture, sampler) pairs are each numbered
// from 0 in (group, binding) order (pairs by the texture's, then the sampler's). The result says
// which WGSL binding (and for a pair, which sampler) every slot came from, and the renderer binds
// from the WebGPU bind groups by that table.
//
// The post-pass gives every combined sampler its binding (Tint leaves them without one) and drops
// "#extension GL_EXT_blend_func_extended" (dual-source output with layout(index = 1) is core in
// desktop GLSL; uam refuses the extension name).
#include <cstdint>
#include <string>
#include <vector>

namespace swdk {

// uam::Stage's numbering for the stages used here (and deko3d's DkStage).
enum class ShaderStage : uint8_t { Vertex = 0, Fragment = 4, Compute = 5 };

// Changes whenever this file's output for the same input changes: part of the cache file's id.
constexpr uint32_t kTranslateRevision = 2;

// a texture used without a sampler (textureLoad): the renderer binds its nearest sampler
constexpr uint32_t kNoSamplerGroup = 1000;

struct SlotBinding {
    enum class Kind : uint8_t { Uniform, Storage, Sampler };
    Kind kind = Kind::Uniform;
    uint8_t slot = 0;
    // WGSL binding point (for a pair: the texture's)
    uint8_t group = 0;
    uint8_t binding = 0;
    // a pair's sampler (kNoSamplerGroup: none)
    uint16_t samplerGroup = 0;
    uint8_t samplerBinding = 0;
};

struct TranslateResult {
    bool ok = false;
    std::string glsl;   // after the post-pass
    std::string error;  // Tint's or the post-pass's reason when !ok
    std::vector<SlotBinding> bindings;
    uint32_t workgroup[3] = {1, 1, 1};  // compute
};

// once per process, before the first translate (tint::Initialize)
void translate_init();

// One entry point of a WGSL module (empty name: the module's only entry point of that stage).
// Never throws; failures come back in error. Not reentrant: callers serialize.
TranslateResult wgsl_to_glsl(const std::string& wgsl, const std::string& entryPoint, ShaderStage stage);

struct SamplerSlot {
    std::string name;
    uint32_t slot = 0;
};
bool post_pass(std::string* glsl, const std::vector<SamplerSlot>& samplerSlots, std::string* error);

}  // namespace swdk
