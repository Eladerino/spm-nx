// Self-updater core: one shared kit across the ports (SPM, JSRF, CTTR, ...), sited beside
// debug_server.h/.cpp in each tree. A port supplies a Config block (repo, endpoint, asset rule,
// compare policy, baked version, install root, file-rule table, cacert path) plus thin glue
// (menu binding, restart action); the core supplies the state machine, the GitHub Releases
// client, the download/verify pipeline, staged minizip extraction with merge-never-replace
// rule enforcement, and the apply protocol (bundled assets first, the NRO last).
//
// Locked rules (design: spm-updater-2026-11-05/DESIGN.md):
//   - Menu-triggered only. No threads, no sockets, no network at boot or while idle.
//   - Merge, never replace: the file-rule table decides per path what may be written;
//     Deny paths can never reach a write call.
//   - Fails silent: any error lands in State::Failed with a log-only FailReason; the only
//     user-facing failure text is state_text(Failed).
//   - Sockets initialise on demand on the worker, tolerating a platform that already
//     initialised them (AlreadyInitialized = borrowed, never exited by the core).
//   - TLS is never weakened: VERIFYPEER 1 / VERIFYHOST 2, CA bundle from Config::cacert_path;
//     a missing bundle or a TLS-less transport fails quiet with FailReason::Tls.
//
// This header is game-agnostic: no libnx, no menu, no game headers. The only platform seams
// are the transport (libcurl, SELFUPD_TRANSPORT_CURL) and the socket guard (libnx, __SWITCH__);
// everything else compiles and runs on a host toolchain (see tests/self_updater/).
#pragma once
#include <cstddef>

namespace selfupd {

enum class State {
    Idle,          // never checked this session (or cancelled back)
    Checking,      // talking to the GitHub API
    UpToDate,      // checked; baked version is the newest candidate
    Available,     // a newer release exists; nothing downloaded yet
    Downloading,   // asset download in flight (progress valid)
    Verifying,     // size / digest / archive validation
    ReadyToApply,  // staged + validated; waiting for the apply tap
    Applying,      // file swaps in flight
    Failed,        // quiet failure; see Status::reason (log-only detail)
};

enum class FailReason { None, Offline, Http, Tls, Parse, AssetMissing,
                        Io, Layout, Policy, Cancelled };

struct Status {
    State      state;
    int        progress_permille;  // 0..1000; valid only in Downloading
    char       latest[32];         // newest candidate version ("" if none)
    FailReason reason;             // meaningful only in State::Failed
};

// Merge-never-replace rule table (DESIGN §3). Longest-prefix match wins; unmatched
// paths take Config::default_mode.
enum class RuleMode { Deny, CreateOnly, Overwrite };
struct FileRule { const char* prefix; RuleMode mode; };

enum class Endpoint { List, Latest };              // DESIGN §2
enum class AssetMatch { PrefixZip, ExactTemplate }; // DESIGN §2

enum class Verdict { Newer, NotNewer, IgnoreCandidate };
using CompareFn = Verdict (*)(const char* baked, const char* candidate_tag, const char* channel);

struct Config {
    // Identity / endpoints
    const char* repo;              // "Eladerino/spm-nx"
    const char* app_id;            // "spm-nx" — asset prefix, log tag
    const char* user_agent;        // GitHub API requires a UA
    Endpoint    endpoint;          // List | Latest          (§2)
    // Asset selection (§2)
    AssetMatch  asset_match;       // PrefixZip | ExactTemplate
    const char* asset_template;    // ExactTemplate only: "jsrf-unleashed-{tag}.zip"
    // Version (§2)
    const char* baked_version;     // from the port's single source
    CompareFn   compare;           // per-port policy (two ship with the core)
    const char* channel;           // expected channel suffix or nullptr
    // Files (§3). install_root is a stdio path with no "sdmc:" prefix
    // (e.g. "/switch/spm-nx"); all core file IO uses it verbatim.
    const char* install_root;
    const char* zip_prefix;        // "switch/spm-nx/" — stripped from entries
    const char* nro_name;          // "spm-nx.nro"
    const FileRule* file_rules;    // per-prefix modes (§3)
    size_t      file_rule_count;
    RuleMode    default_mode;      // SPM: Overwrite. JSRF: Deny (closed list).
    // TLS (§3, §5)
    const char* cacert_path;       // nullptr = NOT_SHIPPED (valid config state)
    bool        auto_download;     // chain check -> download on Available
};

void        init(const Config& cfg);   // stores config; NO threads, NO sockets
void        request_check();           // Idle/UpToDate/Failed -> Checking
void        request_download();        // Available -> Downloading
void        request_apply();           // ReadyToApply -> Applying -> applied()
void        cancel();                  // Checking/Downloading -> Idle, cleans .part
Status      status();                  // cheap atomic snapshot; safe per-frame
const char* state_text(State s);       // canonical base strings (DESIGN §1)

// True after a successful apply (the DESIGN §1 "apply completed" signal). The state
// remains ReadyToApply; the port's glue reads applied() and performs its restart action.
bool        applied();

// The two shipped compare policies (DESIGN §2). A port may supply its own CompareFn.
Verdict compare_semver_prerelease(const char* baked, const char* candidate_tag, const char* channel);
Verdict compare_describe_channel(const char* baked, const char* candidate_tag, const char* channel);

} // namespace selfupd
