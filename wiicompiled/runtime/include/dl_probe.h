#pragma once

#include <cstdint>

// ---- the display-list cache eligibility probe (Switch Diagnostics "dlprobe"; state in gx_dl.cpp) ----
//
// A measurement, not a cache: while the switch is on, every display list the guest submits
// through GX__CallDisplayList is hashed (XXH3, the digest gx_dl.cpp already uses for its scan
// cache) and compared against the last hash seen at the same guest address. Each perftimer
// window prints one [dlprobe] line: how many calls, how many distinct addresses, how many
// calls were the same bytes at the same address as last time ("repeat" calls and bytes —
// exactly what a decoded-display-list cache would serve instead of re-decoding), and how many
// had changed. Nothing is cached, skipped, reordered or returned differently; the only cost
// while on is the hashing itself, and while off the submit path pays a single atomic load.
// The address -> last-hash map persists across windows (a window's first frames are not a
// warmup for it); the window counters reset at each print, and the first printed window is
// still the warmup — it holds every list's first sighting.

namespace DlProbe {

// True while the dlprobe switch is on. One relaxed atomic load; safe to ask on the submit path.
bool Enabled() noexcept;

// Records one display-list submission: the list's guest address (canonicalised inside), the
// bytes that will be decoded (for SPM's host-recorded lists these are the host bytes the
// guest stub names, at the stub's guest address), their length, and whether the projection
// in effect is orthographic (g_projectionVector[0] != 0) as opposed to perspective — the
// probe's 2D/3D split. Call only while Enabled(); Record does not check.
void Record(uint32_t listAddr, const uint8_t* bytes, uint32_t nbytes, bool orthographic);

// The [dlprobe] line for a perftimer window, printed with the [calls] lines from
// PrintNativeOverrideCounts. Prints nothing when the probe was off for the whole window and
// nothing was recorded in it.
void PrintWindow(uint64_t firstFrame, uint64_t lastFrame);

} // namespace DlProbe

// The Diagnostics forwarding entry (device.cpp's diag_init / diag_set call it for the
// "dlprobe" switch), the same shape as spm_gxskip_set / spm_rtt_log_set.
extern "C" void spm_dlprobe_set(bool enabled);
