#pragma once

// NSMBW_PROFILE=<start s>:<length s>: sample the game thread and log where its time goes, by
// translated guest function (guest_profiler.cpp; Windows capture). On Switch the same
// attribution and hot list are driven by the "profile" dk_diag switch instead:
// SetSwitchProfileEnabled below.
namespace GuestProfiler {
// From the thread that presents frames, at each present: the first call starts the sampler.
void OnFramePresented();
#if defined(__SWITCH__)
// Switch: start the game-thread sampler (true), or stop it and log the hot list (false).
// Called by switch/deko/device.cpp's diag_set and diag_init for the "profile" switch.
void SetSwitchProfileEnabled(bool enabled);
#endif
} // namespace GuestProfiler
