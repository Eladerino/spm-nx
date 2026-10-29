#pragma once

#include "gx.hpp"
// MaxInterpolatedFrames / FrameInterpolationDrawIdentity, and the interpolation API that build_uniform feeds.
#include "frame_interpolation.hpp"

namespace aurora::gx {
struct UniformRanges {
  gfx::Range current;
  std::array<gfx::Range, MaxInterpolatedFrames> interpolated;
};

ShaderInfo build_shader_info(const ShaderConfig& config) noexcept;
Light prepare_shader_light(Light light) noexcept;
UniformRanges build_uniform(const ShaderInfo& info, uint32_t vtxStart, const BindGroupRanges& ranges,
                            const FrameInterpolationDrawIdentity& drawIdentity, bool perspective,
                            uint16_t usedPnMtxMask = 1) noexcept;
u8 color_channel(GXChannelID id) noexcept;

// NSMBW debugging: the runtime setters for the mtxlog and uvlog switches (shader_info.cpp).
// Each flag still initialises from its env var (NSMBW_MTX_LOG / NSMBW_UV_LOG) on first use.
void nsmbw_set_mtx_log(bool on);
void nsmbw_set_uv_log(bool on);
}; // namespace aurora::gx
