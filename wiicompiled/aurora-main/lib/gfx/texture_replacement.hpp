#pragma once

#include "texture.hpp"
#include <optional>

namespace aurora::gfx::texture_replacement {
void initialize() noexcept;
void shutdown() noexcept;
void register_tlut(const GXTlutObj* obj, const void* data, GXTlutFmt format, uint16_t entries) noexcept;
void load_tlut(const GXTlutObj* obj, uint32_t idx) noexcept;
// load_tlut in two halves for the GX decode thread (lib/gx/fifo.hpp): the game thread takes the
// pending registration at GXLoadTlut (an owning pointer, or null), and the decode thread installs
// it in stream order, since find_replacement reads the loaded palettes while decoding.
void* take_pending_tlut(const GXTlutObj* obj) noexcept;
void load_taken_tlut(const GXTlutObj* obj, void* taken, uint32_t idx) noexcept;
std::optional<TextureHandle> find_replacement(const GXTexObj_& obj) noexcept;
std::string build_texture_replacement_name(const GXTexObj_& obj) noexcept;
// New Super Paper Bros.: the button prompt set for the controller in use (0 xbox, 1 playstation, 2 nintendo,
// -1 the game's own Wii Remote icons). Returns true once after it changed, so cached textures can be re-resolved.
void set_button_icon_style(int style) noexcept;
bool take_button_icon_style_change() noexcept;
int button_icon_style() noexcept;
} // namespace aurora::gfx::texture_replacement
