// The Switch stand-in for the built-in updater (wiicompiled/runtime/src/updater.cpp,
// which spm_switch_runtime_sources leaves out of the Horizon build): the real one
// checks GitHub in the background and hands over to a shell script that overwrites
// the program folder, and neither exists on Horizon. This one reports no packaged
// version, so the Options > Game update rows never appear (the overlay draws them
// only when Updater::Current() is non-empty), and every other entry point is inert.

#include "updater.h"

namespace Updater {
void Start() {}
State GetState() { return State::Off; }
std::string Current() { return {}; }
std::string Latest() { return {}; }
void Install() {}
bool CheckEnabled() { return false; }
void SetCheckEnabled(bool) {}
} // namespace Updater
