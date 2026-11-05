// The Switch backend for the built-in updater (wiicompiled/runtime/include/updater.h),
// over the shared cross-port self-updater core (switch/source/self_updater.{h,cpp},
// design: ~/workspace/spm-updater-2026-11-05/DESIGN.md). It replaces the old inert stub:
// Current() now reports the baked version, so the Options > Game update rows appear, and
// the row's taps drive the core. Everything is menu-triggered (Brandon's locked rule):
// Start() only stores the core's config -- no check, no thread, no sockets at boot.
// Sockets, the download and the staged apply all happen inside the core, on its worker,
// only after a tap. Merge-never-replace is the core's rule table below: UserData/ and
// game/ are Deny, everything else in the release zip Overwrites, NRO last (core-side).

#include "updater.h"

#include "aurora_events.h"  // ExitForAuroraWindowClose(): the clean shutdown route (§4)
#include "self_updater.h"

#include <switch.h>  // envHasNextLoad / envSetNextLoad (libnx runtime/env.h)

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#ifndef SPM_BAKED_VERSION
#define SPM_BAKED_VERSION ""
#endif
// CMake passes SPM_BAKED_VERSION as a bare token (0.1.2-beta); stringize it in two steps
// so the macro expands before it is quoted.
#define SPM_UPD_STR_(x) #x
#define SPM_UPD_STR(x) SPM_UPD_STR_(x)

namespace Updater {
namespace {

// The runtime root, in the exact form spm_switch_init.cpp exports it as
// WIICOMPILED_RUNTIME_DIR (kRoot, no "sdmc:" prefix: fsdevMountSdmc makes the SD card
// the default device). Read once at Start(); the strings live in statics because the
// core keeps the Config's pointers.
const std::string& InstallRoot() {
    static const std::string root = [] {
        const char* env = std::getenv("WIICOMPILED_RUNTIME_DIR");
        return std::string(env != nullptr && env[0] != '\0' ? env : "/switch/spm-nx");
    }();
    return root;
}

const std::string& CaCertPath() {
    static const std::string path = InstallRoot() + "/cacert.pem";
    return path;
}

// The chain-load target, in hbmenu's convention (DESIGN.md §4: full sdmc: path; the
// path form is desk-flagged there and proven by the hardware probe, not by argument).
std::string NextLoadPath() { return "sdmc:" + InstallRoot() + "/spm-nx.nro"; }

// Brandon's locked rule, no exceptions (DESIGN.md §3): the updater may never write
// UserData/ (saves, settings, caches, logs) or game/ (the player's own dump).
const selfupd::FileRule kFileRules[] = {
    {"UserData/", selfupd::RuleMode::Deny},
    {"game/", selfupd::RuleMode::Deny},
};

bool g_chainLoadFired = false;  // the chain-load + shutdown happens at most once

// Post-apply observation, polled from GetState()/Install() on the menu's own cadence
// (the builder polls GetState() several times a frame; this is one atomic snapshot
// read plus, once, the shutdown). Never blocks, never runs on the core's worker.
void ObserveApplied() {
    if (g_chainLoadFired || !selfupd::applied()) return;
    if (!envHasNextLoad()) {
        // Launched outside hbloader: the apply still completed; there is no next-load
        // buffer to chain through, so the player restarts the game by hand. The row
        // keeps reading "restart to apply" (GetState stays ReadyToApply), which is
        // exactly that instruction. Mark handled so this does not re-check per frame.
        g_chainLoadFired = true;
        std::fprintf(stderr, "[updater] update applied; no next-load buffer, restart the game manually\n");
        return;
    }
    const std::string path = NextLoadPath();
    // argv is the path itself as the SINGLE token: the runtime throws on argc != 1
    // (DESIGN.md §4). Nothing else, ever.
    const Result rc = envSetNextLoad(path.c_str(), path.c_str());
    if (R_FAILED(rc)) {
        g_chainLoadFired = true;  // do not retry per frame; the row stays ReadyToApply
        std::fprintf(stderr, "[updater] envSetNextLoad failed (rc 0x%x); restart the game manually\n", rc);
        return;
    }
    g_chainLoadFired = true;
    std::fprintf(stderr, "[updater] update applied; chain-loading %s\n", path.c_str());
    // The process's normal clean shutdown, the same route the guest-OS exit paths use:
    // it ends the process through libnx's exit, which runs userAppExit() (debug server
    // stop, guest window unmap, socket exit) before hbloader loads the target above.
    ExitForAuroraWindowClose();
}

} // namespace

void Start() {
    // init() only: stores the config in the core. No check, no thread, no sockets --
    // Brandon's menu-triggered-only rule overrides the desktop updater's boot check,
    // so the existing call site in InitializeRuntimeSettings() stays and stays inert.
    static std::once_flag once;
    std::call_once(once, [] {
        selfupd::Config cfg{};
        cfg.repo = "Eladerino/spm-nx";
        cfg.app_id = "spm-nx";
        cfg.user_agent = "spm-nx-updater/1.0 (+https://github.com/Eladerino/spm-nx)";
        cfg.endpoint = selfupd::Endpoint::List;
        cfg.asset_match = selfupd::AssetMatch::PrefixZip;
        cfg.asset_template = nullptr;
        cfg.baked_version = SPM_UPD_STR(SPM_BAKED_VERSION);
        cfg.compare = selfupd::compare_semver_prerelease;
        cfg.channel = nullptr;
        cfg.install_root = InstallRoot().c_str();
        cfg.zip_prefix = "switch/spm-nx/";
        cfg.nro_name = "spm-nx.nro";
        cfg.file_rules = kFileRules;
        cfg.file_rule_count = sizeof(kFileRules) / sizeof(kFileRules[0]);
        cfg.default_mode = selfupd::RuleMode::Overwrite;
        cfg.cacert_path = CaCertPath().c_str();
        cfg.auto_download = true;  // a check that finds a release chains into its download
        selfupd::init(cfg);
    });
}

State GetState() {
    const selfupd::Status st = selfupd::status();
    switch (st.state) {
    case selfupd::State::Checking: return State::Checking;
    case selfupd::State::UpToDate: return State::UpToDate;
    case selfupd::State::Available: return State::Available;
    case selfupd::State::Downloading: return State::Downloading;
    case selfupd::State::Verifying: return State::Downloading;  // header has no Verifying
    case selfupd::State::Applying: return State::Downloading;   // brief busy state, same fold
    case selfupd::State::Failed: return State::Failed;
    case selfupd::State::ReadyToApply:
        // The core reports a completed apply as ReadyToApply + applied(); observing it
        // here is what fires the chain-load (§4), once. Before completion this is the
        // staged-and-validated state the builder renders as "restart to apply".
        ObserveApplied();
        return State::ReadyToApply;
    case selfupd::State::Idle:
    default: return State::Off;
    }
}

std::string Current() { return SPM_UPD_STR(SPM_BAKED_VERSION); }

std::string Latest() { return selfupd::status().latest; }

void Install() {
    // The row tap, dispatched on the CORE state (the header's folded states would lose
    // the Idle/ReadyToApply distinction this dispatch needs).
    switch (selfupd::status().state) {
    case selfupd::State::Idle:
    case selfupd::State::UpToDate:
    case selfupd::State::Failed:
        selfupd::request_check();  // auto_download chains a hit into the download
        break;
    case selfupd::State::Available:
        selfupd::request_download();
        break;
    case selfupd::State::ReadyToApply:
        if (!selfupd::applied()) {
            selfupd::request_apply();  // completion is observed via ObserveApplied()
        } else {
            ObserveApplied();  // already applied (e.g. no next-load buffer): stay honest
        }
        break;
    case selfupd::State::Checking:
    case selfupd::State::Downloading:
    case selfupd::State::Verifying:
    case selfupd::State::Applying:
        break;  // work already in flight; the tap changes nothing
    }
}

// The desktop toggle persists an auto-check-at-boot preference; on Switch that would
// mean checking at boot, which the menu-triggered-only rule forbids, and an inert
// toggle would be a lying control -- so the row is suppressed in the Switch builder
// (#if !defined(__SWITCH__) in settings_overlay.cpp) and the backend reports off.
bool CheckEnabled() { return false; }
void SetCheckEnabled(bool) {}

} // namespace Updater
