// gx_lighting.cpp - Lighting and Channel Control
#include "gx_internal.h"
#include "runtime_log.h"
extern "C" unsigned g_spmAudit[8];  // aurora command_processor (SPM diagnostics)
extern "C" int g_spmSweepDump;
bool SpmAuroraDlRecording();  // gx_init.cpp: material lists are recorded at map load

#include <cstdio>
#include <cstdlib>

namespace {

// GXLightObj is an opaque 64-byte guest object. The Revolution SDK leaves
// three words at the front unused, then stores the exact 13 XF light words
// loaded by GXLoadLightObjImm. Aurora's host-side GXLightObj implementation is
// compact (it starts at color), so a guest pointer must never be passed to the
// host implementation directly.
constexpr uint32_t kLightObjSize = 0x40;
constexpr uint32_t kColorOffset = 0x0c;
constexpr uint32_t kAttnAOffset = 0x10;
constexpr uint32_t kAttnKOffset = 0x1c;
constexpr uint32_t kPositionOffset = 0x28;
constexpr uint32_t kDirectionOffset = 0x34;

bool ValidateGuestLightObj(uint32_t addr, const char* operation) {
    if (addr != 0 && Memory::Contains(addr, kLightObjSize)) {
        return true;
    }
    RT_LOGF(RT_TAG_GX, "%s: invalid GXLightObj @0x%08X\n", operation, addr);
    std::fflush(stderr);
    return false;
}

// `obj` is the guest object's kLightObjSize bytes: in guest memory, or the GX worker's copy of them.
float ReadLightFloat(const uint8_t* obj, uint32_t offset) {
    return BigEndian::ReadFloat32(obj + offset);
}

void InitializeHostLightFromGuest(GXLightObj& host, const uint8_t* obj) {
    GXInitLightColor(&host, DecodeGxColor(BigEndian::Read32(obj + kColorOffset)));
    GXInitLightAttn(
        &host,
        ReadLightFloat(obj, kAttnAOffset + 0),
        ReadLightFloat(obj, kAttnAOffset + 4),
        ReadLightFloat(obj, kAttnAOffset + 8),
        ReadLightFloat(obj, kAttnKOffset + 0),
        ReadLightFloat(obj, kAttnKOffset + 4),
        ReadLightFloat(obj, kAttnKOffset + 8));
    GXInitLightPos(
        &host,
        ReadLightFloat(obj, kPositionOffset + 0),
        ReadLightFloat(obj, kPositionOffset + 4),
        ReadLightFloat(obj, kPositionOffset + 8));

    // GXInitLightDir negates its arguments before storing them. The guest
    // object already contains those stored XF direction components.
    GXInitLightDir(
        &host,
        -ReadLightFloat(obj, kDirectionOffset + 0),
        -ReadLightFloat(obj, kDirectionOffset + 4),
        -ReadLightFloat(obj, kDirectionOffset + 8));
}

void LoadLightObjFrom(const uint8_t* obj, uint32_t lid) {
    GXLightObj host{};
    InitializeHostLightFromGuest(host, obj);
    GXLoadLightObjImm(&host, static_cast<GXLightID>(lid));
}

// The GX part of the channel colour setters, on the colour's guest bytes (or the GX worker's copy).
void SetChanAmbColorFrom(const uint8_t* color, uint32_t c) {
    EnsureAuroraFrameActive();
    GXSetChanAmbColor((GXChannelID)c, DecodeGxColor(BigEndian::Read32(color)));
}

void SetChanMatColorFrom(const uint8_t* color, uint32_t c) {
    EnsureAuroraFrameActive();
    GXSetChanMatColor((GXChannelID)c, DecodeGxColor(BigEndian::Read32(color)));
}

} // namespace

// ============================================================================
// Light Object Load
// ============================================================================

extern "C" void GX__LoadLightObjImm_80170320(uint32_t la, uint32_t lid) {
    if (!ValidateGuestLightObj(la, "GXLoadLightObjImm")) return;
    GxWorker::DeferOrRun<&LoadLightObjFrom>(Memory::GetPointer(la, kLightObjSize), kLightObjSize, lid);
}
PPC_NATIVE_OVERRIDE_VOID(8028E54C, GX__LoadLightObjImm_80170320, (uint32_t la, uint32_t lid), (la, lid));

// ============================================================================
// Channel Control
// ============================================================================

extern "C" void GX__SetChanAmbColor_8017039c(uint32_t c, uint32_t cp) {
    { static int n = 0; if (n++ < 60) RT_LOGF(RT_TAG_GX, "[spmchan] amb ch %u %08X\n", c, Memory::Read32(cp)); }
    GxWorker::DeferOrRun<&SetChanAmbColorFrom>(Memory::GetPointer(cp, 4), 4, c);
}
PPC_NATIVE_OVERRIDE_VOID(8028e5c8, GX__SetChanAmbColor_8017039c, (uint32_t c, uint32_t cp), (c, cp));

extern "C" void GX__SetChanMatColor_80170474(uint32_t c, uint32_t cp) {
    { static int n = 0; if (n++ < 60) RT_LOGF(RT_TAG_GX, "[spmchan] mat ch %u %08X\n", c, Memory::Read32(cp)); }
    GxWorker::DeferOrRun<&SetChanMatColorFrom>(Memory::GetPointer(cp, 4), 4, c);
}
PPC_NATIVE_OVERRIDE_VOID(8028E6A0, GX__SetChanMatColor_80170474, (uint32_t c, uint32_t cp), (c, cp));

extern "C" void GX__SetNumChans_8017054c(uint32_t n) { GXSetNumChans((u8)n); }
PPC_NATIVE_OVERRIDE_VOID(8028e778, GX__SetNumChans_8017054c, (uint32_t n), (n));

extern "C" void GX__SetChanCtrl_80170570(uint32_t ch, uint32_t en, uint32_t as, uint32_t ms, uint32_t lm, uint32_t df, uint32_t af) {
    static const bool noLight = std::getenv("SPM_NOLIGHT") != nullptr;  // diagnostic: draw unlit to test lighting as the cause of black map geometry
    if (noLight) en = 0;
    { const CpuContext* x = TryGetCpuContext();
      // mapSetMaterial (lr 0x8008A6AC): name the material being set up, with the renderer's draw counter for matching
      if (x && x->lr == 0x8008A6ACu && ch == 0) {
          static int m = 0;
          static const bool matLog = std::getenv("SPM_DL_HEX") != nullptr;  // diagnostic: names the material lists as they are recorded
          if (matLog && SpmAuroraDlRecording() && m++ < 3000) {
              std::string names;
              auto readStr = [](uint32_t s) {
                  std::string str;
                  if (!Memory::Contains(s, 32)) return str;
                  for (int k = 0; k < 31; ++k) { char c = static_cast<char>(Memory::Read8(s + k)); if (!c) break; if (c < 0x20 || c > 0x7e) { str.clear(); break; } str += c; }
                  return str.size() >= 3 ? str : std::string();
              };
              for (int r = 14; r <= 31; ++r) {
                  const uint32_t p = x->gpr[r];
                  if (!Memory::Contains(p, 16)) continue;
                  std::string s = readStr(p);  // inline name
                  if (s.empty()) { uint32_t q = 0; if (Memory::TryRead32(p, q)) s = readStr(q); }  // name pointer at +0
                  if (s.empty()) { uint32_t q = 0; if (Memory::TryRead32(p, q) && Memory::Contains(q, 8)) { uint32_t q2 = 0; if (Memory::TryRead32(q, q2)) s = readStr(q2); } }  // struct -> name ptr
                  if (!s.empty()) names += " r" + std::to_string(r) + "=" + s;
              }
              RT_LOGF(RT_TAG_GX, "[spmmat] draw %u%s\n", g_spmAudit[0], names.c_str());
          }
      } }
    GXSetChanCtrl((GXChannelID)ch, en!=0, (GXColorSrc)as, (GXColorSrc)ms, lm, (GXDiffuseFn)df, (GXAttnFn)af);
}
PPC_NATIVE_OVERRIDE_VOID(8028E79C, GX__SetChanCtrl_80170570, (uint32_t ch, uint32_t en, uint32_t as, uint32_t ms, uint32_t lm, uint32_t df, uint32_t af), (ch, en, as, ms, lm, df, af));
