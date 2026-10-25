// WGSL -> uam GLSL (shader_translate.h). From SwitchWaker (MIT) at 92307ff:
// switch/deko/shader_translate.cpp, reduced to one binding scheme, with the sampler of every pair
// recorded and compute entry points.
#include "shader_translate.h"

#include <algorithm>
#include <map>
#include <sstream>
#include <tuple>

#include "src/tint/api/tint.h"
#include "src/tint/lang/core/ir/module.h"
#include "src/tint/lang/glsl/writer/writer.h"
#include "src/tint/lang/wgsl/inspector/inspector.h"
#include "src/tint/lang/wgsl/reader/reader.h"

namespace swdk {
namespace {

using tint::BindingPoint;
using RB = tint::inspector::ResourceBinding;

std::string sampler_name(uint32_t slot) { return "spm_tex" + std::to_string(slot); }

bool is_uniform(RB::ResourceType t) { return t == RB::ResourceType::kUniformBuffer; }
bool is_storage(RB::ResourceType t) {
    return t == RB::ResourceType::kStorageBuffer || t == RB::ResourceType::kReadOnlyStorageBuffer;
}
bool is_texture(RB::ResourceType t) {
    return t == RB::ResourceType::kSampledTexture || t == RB::ResourceType::kDepthTexture ||
           t == RB::ResourceType::kMultisampledTexture || t == RB::ResourceType::kDepthMultisampledTexture;
}

tint::inspector::PipelineStage tint_stage(ShaderStage s) {
    switch (s) {
    case ShaderStage::Vertex: return tint::inspector::PipelineStage::kVertex;
    case ShaderStage::Fragment: return tint::inspector::PipelineStage::kFragment;
    default: return tint::inspector::PipelineStage::kCompute;
    }
}

// a binding point packed into one number for Tint's remapping (only names the pair below; uam sees
// the post-pass binding)
uint32_t packed(uint32_t group, uint32_t binding) { return group * 64 + binding; }

}  // namespace

void translate_init() { tint::Initialize(); }

TranslateResult wgsl_to_glsl(const std::string& wgsl, const std::string& requestedEntry, ShaderStage stage) {
    TranslateResult r;
    tint::Source::File file("shader.wgsl", wgsl);
    tint::wgsl::reader::Options readerOptions;
    readerOptions.allowed_features = tint::wgsl::AllowedFeatures::Everything();
    tint::Program program = tint::wgsl::reader::Parse(&file, readerOptions);
    if (!program.IsValid()) {
        r.error = "WGSL: " + program.Diagnostics().Str();
        return r;
    }

    tint::inspector::Inspector inspector(program);
    std::string entryPoint = requestedEntry;
    bool found = false;
    for (const auto& ep : inspector.GetEntryPoints()) {
        if (ep.stage != tint_stage(stage)) continue;
        if (!entryPoint.empty() && ep.name != entryPoint) continue;
        found = true;
        entryPoint = ep.name;
        if (ep.workgroup_size) {
            r.workgroup[0] = ep.workgroup_size->x;
            r.workgroup[1] = ep.workgroup_size->y;
            r.workgroup[2] = ep.workgroup_size->z;
        }
        break;
    }
    if (!found) {
        r.error = "no entry point '" + requestedEntry + "' of the requested stage";
        return r;
    }

    tint::glsl::writer::Options options;
    options.version = tint::glsl::writer::Version(tint::glsl::writer::Version::Standard::kDesktop, 4, 6);
    options.entry_point_name = entryPoint;
    options.disable_robustness = true;
    options.depth_zero_to_one = true;
    options.disable_position_y_negation = true;
    options.use_uniform_buffers = true;
    options.strip_all_names = false;
    options.disable_polyfill_integer_div_mod = false;
    options.placeholder_sampler_bind_point = {kNoSamplerGroup, 0};

    // Buffers: (group, binding) order, uniform and storage each from slot 0.
    std::vector<RB> resources = inspector.GetResourceBindings(entryPoint);
    std::sort(resources.begin(), resources.end(), [](const RB& a, const RB& b) {
        return std::tie(a.bind_group, a.binding) < std::tie(b.bind_group, b.binding);
    });
    uint32_t nextUbo = 0, nextSsbo = 0;
    for (const RB& res : resources) {
        const BindingPoint wgslBp{res.bind_group, res.binding};
        if (res.bind_group >= 4 || res.binding >= 64) {
            r.error = "binding @group(" + std::to_string(res.bind_group) + ") @binding(" + std::to_string(res.binding) +
                      ") out of range";
            return r;
        }
        if (is_uniform(res.resource_type) || is_storage(res.resource_type)) {
            const bool ubo = is_uniform(res.resource_type);
            const uint32_t slot = ubo ? nextUbo++ : nextSsbo++;
            if (slot >= 16) {
                r.error = "more than 16 " + std::string(ubo ? "uniform" : "storage") + " buffers";
                return r;
            }
            (ubo ? options.bindings.uniform : options.bindings.storage).emplace(wgslBp, BindingPoint{0, slot});
            SlotBinding b;
            b.kind = ubo ? SlotBinding::Kind::Uniform : SlotBinding::Kind::Storage;
            b.slot = uint8_t(slot);
            b.group = uint8_t(res.bind_group);
            b.binding = uint8_t(res.binding);
            r.bindings.push_back(b);
        } else if (is_texture(res.resource_type)) {
            options.bindings.texture.emplace(wgslBp, BindingPoint{0, packed(res.bind_group, res.binding)});
        } else if (res.resource_type == RB::ResourceType::kSampler) {
            options.bindings.sampler.emplace(wgslBp, BindingPoint{0, packed(res.bind_group, res.binding)});
        } else {
            r.error = "unsupported resource " + res.variable_name + " (storage textures are not drawn yet)";
            return r;
        }
    }

    // Combined samplers: one slot per (texture, sampler) pair the entry point uses.
    auto pairs = inspector.GetSamplerAndNonSamplerTextureUses(entryPoint, BindingPoint{kNoSamplerGroup, 0});
    std::sort(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) {
        return std::tie(a.texture_binding_point.group, a.texture_binding_point.binding, a.sampler_binding_point.group,
                        a.sampler_binding_point.binding) <
               std::tie(b.texture_binding_point.group, b.texture_binding_point.binding, b.sampler_binding_point.group,
                        b.sampler_binding_point.binding);
    });
    std::vector<SamplerSlot> samplerSlots;
    uint32_t nextSampler = 0;
    for (const auto& pair : pairs) {
        const BindingPoint tex = pair.texture_binding_point, smp = pair.sampler_binding_point;
        const uint32_t slot = nextSampler++;
        if (slot >= 32) {
            r.error = "more than 32 texture/sampler pairs";
            return r;
        }
        const bool noSampler = smp.group == kNoSamplerGroup;
        const BindingPoint texRemapped{0, packed(tex.group, tex.binding)};
        const BindingPoint smpRemapped =
            noSampler ? options.placeholder_sampler_bind_point : BindingPoint{0, packed(smp.group, smp.binding)};
        const std::string name = sampler_name(slot);
        options.sampler_texture_to_name.emplace(
            tint::glsl::writer::CombinedTextureSamplerPair{texRemapped, smpRemapped, false}, name);
        samplerSlots.push_back({name, slot});
        SlotBinding b;
        b.kind = SlotBinding::Kind::Sampler;
        b.slot = uint8_t(slot);
        b.group = uint8_t(tex.group);
        b.binding = uint8_t(tex.binding);
        b.samplerGroup = noSampler ? uint16_t(kNoSamplerGroup) : uint16_t(smp.group);
        b.samplerBinding = noSampler ? 0 : uint8_t(smp.binding);
        r.bindings.push_back(b);
    }

    auto ir = tint::wgsl::reader::ProgramToLoweredIR(program);
    if (ir != tint::Success) {
        r.error = "IR: " + ir.Failure().reason;
        return r;
    }
    auto out = tint::glsl::writer::Generate(ir.Get(), options);
    if (out != tint::Success) {
        r.error = "GLSL writer: " + out.Failure().reason;
        return r;
    }
    r.glsl = std::move(out->glsl);
    if (!post_pass(&r.glsl, samplerSlots, &r.error)) {
        r.error = "post-pass: " + r.error;
        return r;
    }
    r.ok = true;
    return r;
}

bool post_pass(std::string* glsl, const std::vector<SamplerSlot>& samplerSlots, std::string* error) {
    std::map<std::string, uint32_t> slots;
    for (const auto& s : samplerSlots) slots[s.name] = s.slot;

    std::istringstream in(*glsl);
    std::string out, line;
    out.reserve(glsl->size() + 256);
    while (std::getline(in, line)) {
        // "uniform highp sampler2D spm_tex0;" (also isampler2D, usampler2D, sampler2DShadow, ...)
        const size_t u = line.find("uniform ");
        const size_t sp = line.find("sampler");
        if (u != std::string::npos && sp != std::string::npos && line.find("layout(") == std::string::npos &&
            line.find('{') == std::string::npos && !line.empty() && line.back() == ';') {
            const size_t nameEnd = line.size() - 1;
            const size_t nameStart = line.find_last_of(' ', nameEnd) + 1;
            const std::string name = line.substr(nameStart, nameEnd - nameStart);
            auto it = slots.find(name);
            if (it == slots.end()) {
                *error = "no slot for sampler " + name;
                return false;
            }
            out += line.substr(0, u) + "layout(binding = " + std::to_string(it->second) + ") " + line.substr(u) + "\n";
            continue;
        }
        if (line.rfind("#extension GL_EXT_blend_func_extended", 0) == 0) continue;
        out += line;
        out += '\n';
    }
    *glsl = std::move(out);
    return true;
}

}  // namespace swdk
