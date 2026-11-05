// Host fixtures for the shared self-updater core (DESIGN §7). Links the REAL
// core (switch/source/self_updater.cpp) WITHOUT SELFUPD_TRANSPORT_CURL and
// provides the transport seam itself: the API fetch returns canned releases
// JSON set by the current case, and the download copies a fixture zip's bytes
// to the target path honouring size/progress. The core's real
// check -> verify -> extract -> validate -> apply flow runs against temp dirs.
//
// Usage: self_updater_fixtures <workdir>   (workdir from make_fixtures.py)
#include "self_updater.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Transport seam (must match the declarations in self_updater.cpp exactly).
// ---------------------------------------------------------------------------
namespace selfupd {
namespace detail {
bool http_get_to_memory(const char* url, const Config& cfg, std::string& out, FailReason& reason);
bool http_download_to_file(const char* url, const char* path, long long expected_size,
                           const Config& cfg,
                           bool (*progress)(long long received, long long total, void* ctx),
                           void* progress_ctx, std::string& digest_hex_out, FailReason& reason);
} // namespace detail
} // namespace selfupd

static std::string g_fixture_api_json;
static std::string g_fixture_zip_path;

namespace selfupd {
namespace detail {

bool http_get_to_memory(const char*, const Config&, std::string& out, FailReason& reason) {
    if (g_fixture_api_json.empty()) { reason = FailReason::Http; return false; }
    out = g_fixture_api_json;
    return true;
}

bool http_download_to_file(const char*, const char* path, long long expected_size,
                           const Config&,
                           bool (*progress)(long long, long long, void*), void* progress_ctx,
                           std::string& digest_hex_out, FailReason& reason) {
    digest_hex_out.clear();  // the core then hashes the file itself (digest path exercised)
    FILE* in = std::fopen(g_fixture_zip_path.c_str(), "rb");
    if (!in) { reason = FailReason::Io; return false; }
    FILE* out = std::fopen(path, "wb");
    if (!out) { std::fclose(in); reason = FailReason::Io; return false; }
    long long total = 0;
    { struct stat st; if (::stat(g_fixture_zip_path.c_str(), &st) == 0) total = (long long)st.st_size; }
    if (expected_size > 0) total = expected_size;
    bool ok = true;
    long long sent = 0;
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, in)) > 0) {
        if (std::fwrite(buf, 1, n, out) != n) { ok = false; break; }
        sent += (long long)n;
        if (progress && !progress(sent, total, progress_ctx)) { reason = FailReason::Cancelled; ok = false; break; }
    }
    if (std::fclose(out) != 0) ok = false;
    std::fclose(in);
    if (!ok && reason == FailReason::None) reason = FailReason::Io;
    return ok;
}

} // namespace detail
} // namespace selfupd

// ---------------------------------------------------------------------------
// Test harness.
// ---------------------------------------------------------------------------
static int g_pass = 0, g_fail = 0;
static int g_sec_pass = 0, g_sec_fail = 0;
static std::string g_section;

static void section(const char* name) {
    if (!g_section.empty())
        std::printf("SECTION %s: %d/%d passed\n", g_section.c_str(), g_sec_pass, g_sec_pass + g_sec_fail);
    g_section = name;
    g_sec_pass = g_sec_fail = 0;
    std::printf("-- %s\n", name);
}
static void check(bool cond, const char* what, int line) {
    if (cond) { ++g_pass; ++g_sec_pass; }
    else {
        ++g_fail; ++g_sec_fail;
        std::printf("FAIL (line %d): %s\n", line, what);
    }
}
#define CHECK(cond) check((cond), #cond, __LINE__)

// ---------------------------------------------------------------------------
// Small filesystem helpers.
// ---------------------------------------------------------------------------
static bool exists(const std::string& p) { struct stat st; return ::stat(p.c_str(), &st) == 0; }
static bool is_dir(const std::string& p) { struct stat st; return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
static void mkdirs(const std::string& dir) {
    std::string cur;
    for (size_t i = 0; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == '/') {
            if (!cur.empty() && !is_dir(cur)) ::mkdir(cur.c_str(), 0777);
            cur += '/';
        } else cur += dir[i];
    }
}
static void write_file(const std::string& p, const std::string& data) {
    size_t slash = p.rfind('/');
    if (slash != std::string::npos) mkdirs(p.substr(0, slash));
    FILE* f = std::fopen(p.c_str(), "wb");
    if (f) { std::fwrite(data.data(), 1, data.size(), f); std::fclose(f); }
}
static std::string read_file(const std::string& p) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return {};
    std::string out;
    char buf[8192]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}
static void list_into(const std::string& dir, const std::string& prefix, std::vector<std::string>& out) {
    DIR* d = ::opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = ::readdir(d)) {
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
        std::string rel = prefix.empty() ? e->d_name : prefix + "/" + e->d_name;
        std::string full = dir + "/" + e->d_name;
        if (is_dir(full)) list_into(full, rel, out);
        else out.push_back(rel);
    }
    ::closedir(d);
}
static std::vector<std::string> list_files(const std::string& dir) {
    std::vector<std::string> out;
    list_into(dir, "", out);
    std::sort(out.begin(), out.end());
    return out;
}
static bool wait_until(const std::function<bool()>& pred, int timeout_ms = 15000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}
static bool wait_terminal() {
    using selfupd::State;
    return wait_until([] {
        State s = selfupd::status().state;
        return s == State::ReadyToApply || s == State::Failed || s == State::UpToDate ||
               s == State::Available || s == State::Idle;
    });
}

// ---------------------------------------------------------------------------
// Fixture data: manifest (zip name -> size, sha256) and JSON builders.
// ---------------------------------------------------------------------------
static std::string g_work;
static std::map<std::string, std::pair<long long, std::string>> g_manifest;

static std::string asset_json(const std::string& zip, const std::string& asset_name, bool with_digest) {
    auto it = g_manifest.find(zip);
    long long size = it != g_manifest.end() ? it->second.first : 0;
    std::string digest = it != g_manifest.end() ? it->second.second : "";
    std::string out = "{\"name\":\"" + asset_name + "\",\"size\":" + std::to_string(size) +
                      ",\"browser_download_url\":\"fixture://" + zip + "\"";
    if (with_digest) out += ",\"digest\":\"sha256:" + digest + "\"";
    out += "}";
    return out;
}
static std::string release_json(const std::string& tag, bool draft, const std::string& asset) {
    return "{\"tag_name\":\"" + tag + "\",\"draft\":" + (draft ? "true" : "false") +
           ",\"prerelease\":false,\"assets\":[" + asset + "]}";
}
static std::string list_json(const std::vector<std::string>& releases) {
    std::string out = "[";
    for (size_t i = 0; i < releases.size(); ++i) { if (i) out += ","; out += releases[i]; }
    return out + "]";
}

// Config holder: strings outlive the Config's pointers for the whole case.
struct CfgHolder {
    std::string repo, app_id, tmpl, baked, channel, root, prefix, nro, cacert;
    selfupd::Config cfg{};
    selfupd::Config build() {
        cfg.repo = repo.c_str();
        cfg.app_id = app_id.c_str();
        cfg.user_agent = "fixture-updater/1.0";
        cfg.asset_template = tmpl.empty() ? nullptr : tmpl.c_str();
        cfg.baked_version = baked.c_str();
        cfg.channel = channel.empty() ? nullptr : channel.c_str();
        cfg.install_root = root.c_str();
        cfg.zip_prefix = prefix.c_str();
        cfg.nro_name = nro.c_str();
        cfg.cacert_path = cacert.c_str();
        return cfg;
    }
};

static const selfupd::FileRule kSpmRules[] = {
    {"UserData/", selfupd::RuleMode::Deny},
    {"game/", selfupd::RuleMode::Deny},
};
static const selfupd::FileRule kJsrfRules[] = {
    {"jsrf-unleashed.nro", selfupd::RuleMode::Overwrite},
    {"READ-ME-FIRST.txt", selfupd::RuleMode::Overwrite},
    {"LICENSE", selfupd::RuleMode::Overwrite},
    {"NOTICE", selfupd::RuleMode::Overwrite},
    {"LICENSES/", selfupd::RuleMode::Overwrite},
    {"cacert.pem", selfupd::RuleMode::Overwrite},
    {"game/", selfupd::RuleMode::CreateOnly},
    {"options.txt", selfupd::RuleMode::Deny},
};

static CfgHolder spm_cfg(const std::string& case_dir, bool auto_download) {
    CfgHolder h;
    h.repo = "Eladerino/spm-nx";
    h.app_id = "spm-nx";
    h.baked = "0.1.2-beta";
    h.root = case_dir + "/install";
    h.prefix = "switch/spm-nx/";
    h.nro = "spm-nx.nro";
    h.cacert = g_work + "/cacert.pem";
    h.cfg.endpoint = selfupd::Endpoint::List;
    h.cfg.asset_match = selfupd::AssetMatch::PrefixZip;
    h.cfg.compare = &selfupd::compare_semver_prerelease;
    h.cfg.file_rules = kSpmRules;
    h.cfg.file_rule_count = sizeof kSpmRules / sizeof kSpmRules[0];
    h.cfg.default_mode = selfupd::RuleMode::Overwrite;
    h.cfg.auto_download = auto_download;
    mkdirs(h.root);
    return h;
}

static void run_check(const std::string& api_json, const std::string& zip) {
    g_fixture_api_json = api_json;
    g_fixture_zip_path = g_work + "/" + zip;
}

int main(int argc, char** argv) {
    using selfupd::FailReason;
    using selfupd::State;
    using selfupd::Verdict;
    if (argc < 2) { std::printf("usage: %s <workdir>\n", argv[0]); return 2; }
    g_work = argv[1];
    {   // manifest
        std::string text = read_file(g_work + "/manifest.txt");
        size_t pos = 0;
        while (pos < text.size()) {
            size_t nl = text.find('\n', pos);
            std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = nl == std::string::npos ? text.size() : nl + 1;
            char name[128]; long long size; char sha[128];
            if (std::sscanf(line.c_str(), "%127s %lld %127s", name, &size, sha) == 3)
                g_manifest[name] = {size, sha};
        }
    }
    CHECK(g_manifest.size() == 5);
    write_file(g_work + "/cacert.pem", "fixture CA bundle placeholder");

    // ------------------------------------------------------------------
    section("a-policy-a-semver-prerelease");
    {
        auto A = &selfupd::compare_semver_prerelease;
        CHECK(A("0.1.2-beta", "v0.1.2-beta", nullptr) == Verdict::NotNewer);
        CHECK(A("0.1.2-beta", "v0.1.2", nullptr) == Verdict::Newer);
        CHECK(A("0.1.2-beta", "v0.1.3-beta", nullptr) == Verdict::Newer);
        CHECK(A("0.1.2-beta", "v0.1.1-beta", nullptr) == Verdict::NotNewer);
        CHECK(A("0.1.2-beta", "v0.1.0-testbuild", nullptr) == Verdict::NotNewer);
        CHECK(A("0.0.1", "v0.1.2-beta", nullptr) == Verdict::Newer);
        // prerelease ordering extras
        CHECK(A("0.1.2-alpha", "v0.1.2-beta", nullptr) == Verdict::Newer);
        CHECK(A("0.1.2-beta", "v0.1.2-alpha", nullptr) == Verdict::NotNewer);
        CHECK(A("0.1.2-alpha.1", "v0.1.2-alpha.beta", nullptr) == Verdict::Newer);  // numeric < alphanumeric
        CHECK(A("0.1.2-alpha", "v0.1.2-alpha.1", nullptr) == Verdict::Newer);       // shorter prefix list lower
        CHECK(A("0.1.2-alpha.beta", "v0.1.2-beta", nullptr) == Verdict::Newer);
        CHECK(A("0.1.2", "v0.1.2-beta", nullptr) == Verdict::NotNewer);
    }

    // ------------------------------------------------------------------
    section("b-policy-b-describe-channel");
    {
        auto B = &selfupd::compare_describe_channel;
        CHECK(B("dev", "v0.4.0-switch", "-switch") == Verdict::Newer);
        CHECK(B("0.3.0-switch-13-gabcdef-dirty", "v0.4.0-switch", "-switch") == Verdict::Newer);
        CHECK(B("0.4.0-switch", "v0.4.0-switch", "-switch") == Verdict::NotNewer);
        CHECK(B("0.4.0-switch-5-gabcdef", "v0.4.0-switch", "-switch") == Verdict::NotNewer);
        CHECK(B("0.3.0-switch", "v0.4.0", "-switch") == Verdict::IgnoreCandidate);
        CHECK(B("0.3.0-switch", "v0.2.0-switch", "-switch") == Verdict::NotNewer);
        CHECK(B("dev", "v0.4.0", "-switch") == Verdict::IgnoreCandidate);
        // canonical state strings
        CHECK(std::strcmp(selfupd::state_text(State::Idle), "") == 0);
        CHECK(std::strcmp(selfupd::state_text(State::Checking), "Checking...") == 0);
        CHECK(std::strcmp(selfupd::state_text(State::UpToDate), "Up to date") == 0);
        CHECK(std::strcmp(selfupd::state_text(State::Available), "available") == 0);
        CHECK(std::strcmp(selfupd::state_text(State::Downloading), "Downloading...") == 0);
        CHECK(std::strcmp(selfupd::state_text(State::Verifying), "Verifying...") == 0);
        CHECK(std::strcmp(selfupd::state_text(State::ReadyToApply), "ready — restart to apply") == 0);
        CHECK(std::strcmp(selfupd::state_text(State::Applying), "Applying...") == 0);
        CHECK(std::strcmp(selfupd::state_text(State::Failed), "Couldn't check") == 0);
    }

    // ------------------------------------------------------------------
    section("c-zip-filter-spm-table");
    {
        CfgHolder h = spm_cfg(g_work + "/case_c", true);
        run_check(list_json({release_json("v0.1.3-beta", false,
                        asset_json("spm_good.zip", "spm-nx-v0.1.3-beta.zip", true))}),
                  "spm_good.zip");
        selfupd::init(h.build());
        selfupd::request_check();
        CHECK(wait_terminal());
        CHECK(selfupd::status().state == State::ReadyToApply);
        CHECK(std::strcmp(selfupd::status().latest, "0.1.3-beta") == 0);
        std::vector<std::string> staged = list_files(h.root + "/update-staging");
        std::vector<std::string> want = {"LICENSE", "cacert.pem", "spm-nx.nro"};
        CHECK(staged == want);
        CHECK(!exists(h.root + "/update-staging/UserData"));
        CHECK(!exists(h.root + "/update-staging/game"));
        CHECK(exists(h.root + "/update.zip"));       // kept until a successful apply
        CHECK(!exists(h.root + "/spm-nx.nro"));      // nothing on live paths yet
    }

    // ------------------------------------------------------------------
    section("c2-candidate-selection");
    {
        // Drafts skipped, prerelease flag never consulted, winner = max by
        // compare (not list order): v0.1.2 (listed first) must lose to
        // v0.1.3-beta; the drafted v0.9.9 must not win.
        CfgHolder h = spm_cfg(g_work + "/case_c2", true);
        run_check(list_json({
                        release_json("v0.1.2", false, asset_json("spm_good.zip", "spm-nx.zip", false)),
                        release_json("v0.9.9", true, asset_json("spm_good.zip", "spm-nx-v0.9.9.zip", false)),
                        release_json("v0.1.3-beta", false, asset_json("spm_good.zip", "spm-nx-v0.1.3-beta.zip", false)),
                    }),
                  "spm_good.zip");
        selfupd::init(h.build());
        selfupd::request_check();
        CHECK(wait_terminal());
        CHECK(selfupd::status().state == State::ReadyToApply);
        CHECK(std::strcmp(selfupd::status().latest, "0.1.3-beta") == 0);
    }

    // ------------------------------------------------------------------
    section("d-traversal-and-prefix-aborts");
    {
        CfgHolder h = spm_cfg(g_work + "/case_d1", true);
        run_check(list_json({release_json("v0.1.3-beta", false,
                        asset_json("traversal.zip", "spm-nx-v0.1.3-beta.zip", true))}),
                  "traversal.zip");
        selfupd::init(h.build());
        selfupd::request_check();
        CHECK(wait_terminal());
        CHECK(selfupd::status().state == State::Failed);
        CHECK(selfupd::status().reason == FailReason::Policy);
        CHECK(!exists(h.root + "/update-staging"));          // staging cleaned
        CHECK(!exists(h.root + "/evil.txt"));
        CHECK(!exists(g_work + "/case_d1/evil.txt"));
    }
    {
        CfgHolder h = spm_cfg(g_work + "/case_d2", true);
        run_check(list_json({release_json("v0.1.3-beta", false,
                        asset_json("offprefix.zip", "spm-nx-v0.1.3-beta.zip", true))}),
                  "offprefix.zip");
        selfupd::init(h.build());
        selfupd::request_check();
        CHECK(wait_terminal());
        CHECK(selfupd::status().state == State::Failed);
        CHECK(selfupd::status().reason == FailReason::Layout);
        CHECK(!exists(h.root + "/update-staging"));
    }

    // ------------------------------------------------------------------
    section("e-jsrf-table-createonly");
    {
        CfgHolder h;
        h.repo = "Eladerino/JetSetRadioFuture-Unleashed-NX";
        h.app_id = "jsrf-unleashed";
        h.tmpl = "jsrf-unleashed-{tag}.zip";
        h.baked = "0.3.0-switch";
        h.channel = "-switch";
        h.root = g_work + "/case_e/install";
        h.prefix = "switch/jsrf-unleashed/";
        h.nro = "jsrf-unleashed.nro";
        h.cacert = g_work + "/cacert.pem";
        h.cfg.endpoint = selfupd::Endpoint::Latest;
        h.cfg.asset_match = selfupd::AssetMatch::ExactTemplate;
        h.cfg.compare = &selfupd::compare_describe_channel;
        h.cfg.file_rules = kJsrfRules;
        h.cfg.file_rule_count = sizeof kJsrfRules / sizeof kJsrfRules[0];
        h.cfg.default_mode = selfupd::RuleMode::Deny;
        h.cfg.auto_download = true;
        mkdirs(h.root);
        write_file(h.root + "/game/existing.bin", "SENTINEL");
        run_check(release_json("v0.4.0-switch", false,
                      asset_json("jsrf.zip", "jsrf-unleashed-v0.4.0-switch.zip", true)),
                  "jsrf.zip");
        selfupd::init(h.build());
        selfupd::request_check();
        CHECK(wait_terminal());
        CHECK(selfupd::status().state == State::ReadyToApply);
        std::vector<std::string> staged = list_files(h.root + "/update-staging");
        std::vector<std::string> want = {"READ-ME-FIRST.txt", "game/existing.bin", "game/new.bin",
                                         "jsrf-unleashed.nro"};
        CHECK(staged == want);
        CHECK(!exists(h.root + "/update-staging/options.txt"));
        CHECK(!exists(h.root + "/update-staging/random.txt"));
        selfupd::request_apply();
        CHECK(wait_until([] { return selfupd::applied() || selfupd::status().state == State::Failed; }));
        CHECK(selfupd::applied());
        CHECK(read_file(h.root + "/game/existing.bin") == "SENTINEL");  // CreateOnly: untouched
        CHECK(read_file(h.root + "/game/new.bin") == "ZIP-NEW");        // CreateOnly: created
        CHECK(exists(h.root + "/jsrf-unleashed.nro"));
        CHECK(!exists(h.root + "/options.txt"));
    }

    // ------------------------------------------------------------------
    section("f-apply-order-and-invariant");
    {
        CfgHolder h = spm_cfg(g_work + "/case_f", true);
        // Seed target with OLD contents; zzz_asset.txt's target is a directory
        // so its swap fails mid-apply (after aaa, before the NRO).
        std::string old_nro(70000, 'O');
        old_nro.replace(0x10, 4, "NRO0");
        old_nro.replace(0x20, 4, "OLD!");
        write_file(h.root + "/spm-nx.nro", old_nro);
        write_file(h.root + "/aaa_asset.txt", "OLD-aaa");
        mkdirs(h.root + "/zzz_asset.txt");
        run_check(list_json({release_json("v0.1.3-beta", false,
                        asset_json("apply.zip", "spm-nx-v0.1.3-beta.zip", true))}),
                  "apply.zip");
        selfupd::init(h.build());
        selfupd::request_check();
        CHECK(wait_terminal());
        CHECK(selfupd::status().state == State::ReadyToApply);
        selfupd::request_apply();
        CHECK(wait_until([] { return selfupd::applied() || selfupd::status().state == State::Failed; }));
        CHECK(selfupd::status().state == State::Failed);
        CHECK(selfupd::status().reason == FailReason::Io);
        CHECK(!selfupd::applied());
        CHECK(read_file(h.root + "/aaa_asset.txt") == "NEW-aaa");       // first asset applied
        CHECK(read_file(h.root + "/spm-nx.nro").compare(0x20, 4, "OLD!") == 0);  // NRO last: still OLD
        CHECK(exists(h.root + "/update-staging"));                      // staging left in place

        // Clean run: remove the obstacle, re-check, apply.
        ::rmdir((h.root + "/zzz_asset.txt").c_str());
        selfupd::request_check();
        CHECK(wait_terminal());
        CHECK(selfupd::status().state == State::ReadyToApply);
        selfupd::request_apply();
        CHECK(wait_until([] { return selfupd::applied() || selfupd::status().state == State::Failed; }));
        CHECK(selfupd::applied());
        CHECK(read_file(h.root + "/spm-nx.nro").compare(0x20, 4, "NEW!") == 0);
        CHECK(read_file(h.root + "/aaa_asset.txt") == "NEW-aaa");
        CHECK(read_file(h.root + "/zzz_asset.txt") == "NEW-zzz");
        CHECK(!exists(h.root + "/update.zip"));
        CHECK(!exists(h.root + "/update-staging"));
    }

    // ------------------------------------------------------------------
    section("g-digest");
    {
        // Correct digest, manual download tap (auto_download off).
        CfgHolder h = spm_cfg(g_work + "/case_g1", false);
        run_check(list_json({release_json("v0.1.3-beta", false,
                        asset_json("spm_good.zip", "spm-nx-v0.1.3-beta.zip", true))}),
                  "spm_good.zip");
        selfupd::init(h.build());
        selfupd::request_check();
        CHECK(wait_terminal());
        CHECK(selfupd::status().state == State::Available);
        selfupd::request_download();
        CHECK(wait_terminal());
        CHECK(selfupd::status().state == State::ReadyToApply);
    }
    {
        // Wrong digest -> Failed, never ReadyToApply, nothing promoted.
        CfgHolder h = spm_cfg(g_work + "/case_g2", true);
        std::string bad = asset_json("spm_good.zip", "spm-nx-v0.1.3-beta.zip", true);
        size_t p = bad.find("sha256:");
        if (p != std::string::npos) bad[p + 7] = bad[p + 7] == '0' ? '1' : '0';
        run_check(list_json({release_json("v0.1.3-beta", false, bad)}), "spm_good.zip");
        selfupd::init(h.build());
        selfupd::request_check();
        CHECK(wait_terminal());
        CHECK(selfupd::status().state == State::Failed);
        CHECK(!exists(h.root + "/update.zip"));
        CHECK(!exists(h.root + "/update.zip.part"));
        CHECK(!exists(h.root + "/update-staging/spm-nx.nro"));
    }

    section("");  // flush the last section line
    std::printf("FIXTURES: %d/%d passed\n", g_pass, g_pass + g_fail);
    return g_fail == 0 ? 0 : 1;
}
