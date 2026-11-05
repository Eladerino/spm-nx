// Self-updater core — see self_updater.h for the contract and DESIGN.md
// (spm-updater-2026-11-05) for the design this implements section by section.
//
// Transport seam: the ONLY platform seam besides the socket guard. The core calls
// selfupd::detail::http_get_to_memory / http_download_to_file. With
// SELFUPD_TRANSPORT_CURL defined (the Switch build) they are implemented here with
// libcurl; without it they are only declared and the linking translation unit
// provides them (the host fixtures in tests/self_updater/ do exactly that).
// http_download_to_file additionally reports the SHA-256 of the bytes it wrote in
// digest_hex_out ("" when it did not hash); when a digest must be verified and the
// seam did not hash, the core hashes the downloaded file itself — same result.
#include "self_updater.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <minizip/unzip.h>

#ifdef __SWITCH__
#include <switch.h>
#include <mbedtls/sha256.h>
#endif

#ifdef SELFUPD_TRANSPORT_CURL
#include <curl/curl.h>
#endif

namespace selfupd {
namespace detail {
// Transport seam (see the note at the top of this file).
bool http_get_to_memory(const char* url, const Config& cfg, std::string& out, FailReason& reason);
bool http_download_to_file(const char* url, const char* path, long long expected_size,
                           const Config& cfg,
                           bool (*progress)(long long received, long long total, void* ctx),
                           void* progress_ctx, std::string& digest_hex_out, FailReason& reason);
} // namespace detail
} // namespace selfupd

namespace {

// ---------------------------------------------------------------------------
// SHA-256. On the Switch the digest work uses the in-tree mbedTLS; on host
// builds a compact built-in implementation (FIPS 180-4) so the fixtures
// exercise the same digest path. One wrapper, both routes.
// ---------------------------------------------------------------------------
class Sha256 {
public:
    Sha256() { reset(); }
    void reset() {
#ifdef __SWITCH__
        mbedtls_sha256_init(&ctx_);
        mbedtls_sha256_starts(&ctx_, 0);
#else
        h_[0]=0x6a09e667u; h_[1]=0xbb67ae85u; h_[2]=0x3c6ef372u; h_[3]=0xa54ff53au;
        h_[4]=0x510e527fu; h_[5]=0x9b05688cu; h_[6]=0x1f83d9abu; h_[7]=0x5be0cd19u;
        buf_len_ = 0; total_ = 0;
#endif
    }
    void update(const unsigned char* data, size_t len) {
#ifdef __SWITCH__
        mbedtls_sha256_update(&ctx_, data, len);
#else
        total_ += len;
        while (len > 0) {
            size_t take = 64 - buf_len_; if (take > len) take = len;
            std::memcpy(buf_ + buf_len_, data, take);
            buf_len_ += take; data += take; len -= take;
            if (buf_len_ == 64) { block(buf_); buf_len_ = 0; }
        }
#endif
    }
    void finish_hex(char out[65]) {
        static const char* kHex = "0123456789abcdef";
        unsigned char digest[32];
#ifdef __SWITCH__
        mbedtls_sha256_finish(&ctx_, digest);
        mbedtls_sha256_free(&ctx_);
#else
        const unsigned long long bits = total_ * 8;
        unsigned char pad = 0x80; update_raw(&pad, 1);
        unsigned char zero = 0; while (buf_len_ != 56) update_raw(&zero, 1);
        unsigned char lenbuf[8];
        for (int i = 0; i < 8; ++i) lenbuf[i] = (unsigned char)(bits >> (56 - 8 * i));
        // Feed the length through the raw path so total_ is not disturbed further.
        for (int i = 0; i < 8; ++i) {
            buf_[buf_len_++] = lenbuf[i];
            if (buf_len_ == 64) { block(buf_); buf_len_ = 0; }
        }
        for (int i = 0; i < 8; ++i) {
            digest[4*i+0] = (unsigned char)(h_[i] >> 24);
            digest[4*i+1] = (unsigned char)(h_[i] >> 16);
            digest[4*i+2] = (unsigned char)(h_[i] >> 8);
            digest[4*i+3] = (unsigned char)(h_[i]);
        }
#endif
        for (int i = 0; i < 32; ++i) { out[2*i] = kHex[digest[i] >> 4]; out[2*i+1] = kHex[digest[i] & 15]; }
        out[64] = '\0';
    }
private:
#ifndef __SWITCH__
    void update_raw(const unsigned char* data, size_t len) {
        while (len > 0) {
            size_t take = 64 - buf_len_; if (take > len) take = len;
            std::memcpy(buf_ + buf_len_, data, take);
            buf_len_ += take; data += take; len -= take;
            if (buf_len_ == 64) { block(buf_); buf_len_ = 0; }
        }
    }
    static unsigned rotr(unsigned x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const unsigned char* p) {
        static const unsigned K[64] = {
            0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
            0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
            0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
            0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
            0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
            0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
            0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
            0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
        unsigned w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = ((unsigned)p[4*i] << 24) | ((unsigned)p[4*i+1] << 16) | ((unsigned)p[4*i+2] << 8) | (unsigned)p[4*i+3];
        for (int i = 16; i < 64; ++i) {
            unsigned s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
            unsigned s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        unsigned a=h_[0],b=h_[1],c=h_[2],d=h_[3],e=h_[4],f=h_[5],g=h_[6],h=h_[7];
        for (int i = 0; i < 64; ++i) {
            unsigned S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            unsigned ch = (e & f) ^ (~e & g);
            unsigned t1 = h + S1 + ch + K[i] + w[i];
            unsigned S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            unsigned mj = (a & b) ^ (a & c) ^ (b & c);
            unsigned t2 = S0 + mj;
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h_[0]+=a; h_[1]+=b; h_[2]+=c; h_[3]+=d; h_[4]+=e; h_[5]+=f; h_[6]+=g; h_[7]+=h;
    }
    unsigned h_[8];
    unsigned char buf_[64];
    size_t buf_len_ = 0;
    unsigned long long total_ = 0;
#else
    mbedtls_sha256_context ctx_;
#endif
};

// Hash a whole file; false on IO error.
bool hash_file_hex(const char* path, char out[65]) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    Sha256 sha;
    unsigned char buf[16384];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) sha.update(buf, n);
    bool ok = std::ferror(f) == 0;
    std::fclose(f);
    if (!ok) return false;
    sha.finish_hex(out);
    return true;
}

// ---------------------------------------------------------------------------
// Small filesystem helpers (stdio paths; install_root is used verbatim).
// ---------------------------------------------------------------------------
bool path_exists(const char* path) { struct stat st; return ::stat(path, &st) == 0; }
bool path_is_dir(const char* path) { struct stat st; return ::stat(path, &st) == 0 && S_ISDIR(st.st_mode); }
long long path_size(const char* path) { struct stat st; return ::stat(path, &st) == 0 ? (long long)st.st_size : -1; }

void remove_tree(const std::string& path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) { std::remove(path.c_str()); return; }
    DIR* d = ::opendir(path.c_str());
    if (d) {
        while (dirent* e = ::readdir(d)) {
            if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) continue;
            remove_tree(path + "/" + e->d_name);
        }
        ::closedir(d);
    }
    ::rmdir(path.c_str());
}

// mkdir -p for a directory path (absolute or relative).
bool mkdirs(const std::string& dir) {
    if (dir.empty()) return true;
    std::string cur;
    size_t i = 0;
    if (dir[0] == '/') { cur = "/"; i = 1; }
    while (i <= dir.size()) {
        size_t slash = dir.find('/', i);
        std::string comp = dir.substr(i, slash == std::string::npos ? std::string::npos : slash - i);
        if (!comp.empty()) {
            if (!cur.empty() && cur.back() != '/') cur += '/';
            cur += comp;
            if (!path_is_dir(cur.c_str())) ::mkdir(cur.c_str(), 0777);
        }
        if (slash == std::string::npos) break;
        i = slash + 1;
    }
    return path_is_dir(dir.c_str());
}

bool mkdirs_for_file(const std::string& file_path) {
    size_t slash = file_path.rfind('/');
    if (slash == std::string::npos) return true;
    return mkdirs(file_path.substr(0, slash));
}

std::string join_path(const char* root, const std::string& rel) {
    std::string out = root ? root : "";
    if (!out.empty() && out.back() != '/') out += '/';
    out += rel;
    return out;
}

// ---------------------------------------------------------------------------
// Core-wide state. The snapshot fields follow DESIGN §1: the worker writes
// latest/reason/progress first, then release-stores the state; status()
// acquire-loads the state before reading the rest. No locks, no allocation.
// ---------------------------------------------------------------------------
selfupd::Config g_cfg{};
bool g_cfg_set = false;
std::atomic<selfupd::State> g_state{selfupd::State::Idle};
std::atomic<int> g_progress{0};
std::atomic<bool> g_cancel{false};
std::atomic<bool> g_applied{false};
selfupd::FailReason g_reason = selfupd::FailReason::None;
char g_latest[32] = "";
// The single worker. Held through a leaked pointer on purpose: a joinable
// std::thread destroyed at process exit calls std::terminate, and the last
// worker before shutdown has no later request_* call to reap it. It is still
// joined at every terminal transition (reap_worker, below) — the leak is of
// the empty thread handle only, never of a running thread.
std::thread& worker_slot() {
    static std::thread* t = new std::thread();
    return *t;
}

// Chosen release/asset for the download phase (written by the check worker,
// read by a later download worker; the join between them synchronises).
std::string g_asset_url;
std::string g_asset_digest;   // "sha256:<hex>" or ""
long long g_asset_size = 0;
std::vector<std::string> g_staged;  // staged relative paths, from extraction

void set_latest(const std::string& v) {
    size_t n = v.size(); if (n > 31) n = 31;
    std::memcpy(g_latest, v.data(), n);
    g_latest[n] = '\0';
}
void publish(selfupd::State s) { g_state.store(s, std::memory_order_release); }
void publish_failed(selfupd::FailReason r) {
    g_reason = r;
    publish(selfupd::State::Failed);
}
void reap_worker() { if (worker_slot().joinable()) worker_slot().join(); }

std::string zip_path()     { return join_path(g_cfg.install_root, "update.zip"); }
std::string part_path()    { return join_path(g_cfg.install_root, "update.zip.part"); }
std::string staging_root() { return join_path(g_cfg.install_root, "update-staging"); }

// ---------------------------------------------------------------------------
// Socket guard (DESIGN §5). Owned sockets are exited by release(); borrowed
// sockets (the platform already initialised them) are never exited.
// ---------------------------------------------------------------------------
struct SocketGuard {
    enum class Ownership { None, Owned, Borrowed };
    Ownership ownership = Ownership::None;
    bool ensure(selfupd::FailReason& reason) {
#ifdef __SWITCH__
        Result rc = socketInitializeDefault();
        if (R_SUCCEEDED(rc)) { ownership = Ownership::Owned; return true; }
        // 0xf59 == MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized)
        if (rc == 0xF59) { ownership = Ownership::Borrowed; return true; }
        reason = selfupd::FailReason::Offline;
        return false;
#else
        (void)reason;
        ownership = Ownership::Owned;
        return true;
#endif
    }
    void release() {
#ifdef __SWITCH__
        if (ownership == Ownership::Owned) socketExit();
#endif
        ownership = Ownership::None;
    }
};

// ---------------------------------------------------------------------------
// Version parsing shared by the two compare policies.
// ---------------------------------------------------------------------------
std::string strip_one_v(const char* s) {
    std::string out = s ? s : "";
    if (!out.empty() && out[0] == 'v') out.erase(0, 1);
    return out;
}

// Split a leading numeric dotted core ("0.4.0" of "0.4.0-switch-5-gabc").
// Returns false when there is no leading numeric component. rest = remainder.
bool split_core(const std::string& s, long core[3], std::string& rest) {
    core[0] = core[1] = core[2] = 0;
    size_t i = 0;
    int comp = 0;
    bool any = false;
    while (i < s.size() && comp < 3) {
        size_t j = i;
        long val = 0;
        bool digits = false;
        while (j < s.size() && std::isdigit((unsigned char)s[j])) { val = val * 10 + (s[j] - '0'); ++j; digits = true; }
        if (!digits) break;
        core[comp++] = val; any = true;
        if (j < s.size() && s[j] == '.') { i = j + 1; continue; }
        i = j; break;
    }
    rest = any ? s.substr(i) : s;
    return any;
}

int cmp_core(const long a[3], const long b[3]) {
    for (int i = 0; i < 3; ++i) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

} // namespace

namespace selfupd {

// ---------------------------------------------------------------------------
// Policy A — semver-prerelease (DESIGN §2). Strip one leading v; numeric
// X.Y.Z core; on equal cores a release beats any prerelease; prerelease
// identifiers compare per semver (numeric < alphanumeric, numeric compare
// numerically, shorter prefix-list ranks lower).
// ---------------------------------------------------------------------------
Verdict compare_semver_prerelease(const char* baked, const char* candidate_tag, const char* /*channel*/) {
    auto parse = [](const char* s, long core[3], std::string& suffix, bool& valid) {
        std::string v = strip_one_v(s);
        valid = split_core(v, core, suffix);
        if (valid && !suffix.empty() && suffix[0] == '-') suffix.erase(0, 1);
        else if (valid && !suffix.empty()) valid = false;  // junk after the core
        if (valid && !suffix.empty()) {
            // suffix must be dot/hyphen separated identifiers
            for (char c : suffix)
                if (!(std::isalnum((unsigned char)c) || c == '.' || c == '-')) valid = false;
        }
    };
    long bc[3], cc[3]; std::string bsfx, csfx; bool bok, cok;
    parse(baked, bc, bsfx, bok);
    parse(candidate_tag, cc, csfx, cok);
    if (!cok) return Verdict::NotNewer;
    if (!bok) return Verdict::Newer;  // unparseable baked build self-heals onto a valid release
    int c = cmp_core(cc, bc);
    if (c > 0) return Verdict::Newer;
    if (c < 0) return Verdict::NotNewer;
    if (csfx.empty() && bsfx.empty()) return Verdict::NotNewer;
    if (csfx.empty()) return Verdict::Newer;        // release beats prerelease
    if (bsfx.empty()) return Verdict::NotNewer;
    auto tokens = [](const std::string& sfx) {
        std::vector<std::string> out; std::string cur;
        for (char ch : sfx) {
            if (ch == '.' || ch == '-') { out.push_back(cur); cur.clear(); }
            else cur += ch;
        }
        out.push_back(cur);
        return out;
    };
    auto is_num = [](const std::string& t) {
        if (t.empty()) return false;
        for (char ch : t) if (!std::isdigit((unsigned char)ch)) return false;
        return true;
    };
    std::vector<std::string> ct = tokens(csfx), bt = tokens(bsfx);
    size_t n = std::min(ct.size(), bt.size());
    for (size_t i = 0; i < n; ++i) {
        if (ct[i] == bt[i]) continue;
        bool cn = is_num(ct[i]), bn = is_num(bt[i]);
        if (cn && bn) {
            // numeric compare without overflow: strip zeros, then length, then lexical
            auto norm = [](std::string t) { size_t p = t.find_first_not_of('0'); return p == std::string::npos ? std::string("0") : t.substr(p); };
            std::string a = norm(ct[i]), b = norm(bt[i]);
            if (a.size() != b.size()) return a.size() > b.size() ? Verdict::Newer : Verdict::NotNewer;
            return a > b ? Verdict::Newer : Verdict::NotNewer;
        }
        if (cn != bn) return cn ? Verdict::NotNewer : Verdict::Newer;  // numeric < alphanumeric
        return ct[i] > bt[i] ? Verdict::Newer : Verdict::NotNewer;
    }
    if (ct.size() == bt.size()) return Verdict::NotNewer;
    return ct.size() > bt.size() ? Verdict::Newer : Verdict::NotNewer;
}

// ---------------------------------------------------------------------------
// Policy B — describe-channel (DESIGN §2). The candidate's remainder must
// begin with the configured channel or the candidate is ignored. Describe
// tails (-N-g<sha>) and -dirty are stripped (comparing is core-only) and
// never make either side newer; baked "dev" accepts any channel-valid
// candidate; newer iff candidate core > baked core.
// ---------------------------------------------------------------------------
Verdict compare_describe_channel(const char* baked, const char* candidate_tag, const char* channel) {
    std::string cand = strip_one_v(candidate_tag);
    long cc[3]; std::string crest;
    if (!split_core(cand, cc, crest)) return Verdict::IgnoreCandidate;
    if (channel && channel[0]) {
        if (crest.compare(0, std::strlen(channel), channel) != 0) return Verdict::IgnoreCandidate;
    }
    std::string bk = baked ? baked : "";
    if (bk == "dev") return Verdict::Newer;
    long bc[3]; std::string brest;
    if (!split_core(strip_one_v(bk.c_str()), bc, brest)) return Verdict::Newer;
    return cmp_core(cc, bc) > 0 ? Verdict::Newer : Verdict::NotNewer;
}

} // namespace selfupd

namespace {

// ---------------------------------------------------------------------------
// Hand-rolled string-aware JSON scanner (no external JSON library). Only the
// fields the core consumes are extracted: tag_name, draft, and per asset
// name / size / browser_download_url / digest.
// ---------------------------------------------------------------------------
struct JsonAsset { std::string name; long long size = 0; std::string url; std::string digest; };
struct JsonRelease { std::string tag; bool draft = false; std::vector<JsonAsset> assets; };

struct Scanner {
    const std::string& s;
    explicit Scanner(const std::string& str) : s(str) {}
    size_t ws(size_t i) const { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; return i; }
    // Parse the string at i (s[i]=='"'); on success out = contents, end = past the closing quote.
    bool parse_string(size_t i, std::string& out, size_t& end) const {
        if (i >= s.size() || s[i] != '"') return false;
        out.clear();
        ++i;
        while (i < s.size()) {
            char c = s[i];
            if (c == '"') { end = i + 1; return true; }
            if (c == '\\' && i + 1 < s.size()) {
                char e = s[i + 1];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'u': out += '?'; i += 4; break;  // tags/URLs are ASCII in practice
                    default: out += e; break;
                }
                i += 2; continue;
            }
            out += c; ++i;
        }
        return false;
    }
    // End index (exclusive) of the JSON value starting at i, string-aware.
    size_t value_end(size_t i) const {
        i = ws(i);
        if (i >= s.size()) return i;
        if (s[i] == '"') { std::string tmp; size_t e; return parse_string(i, tmp, e) ? e : s.size(); }
        if (s[i] == '{' || s[i] == '[') {
            char open = s[i], close = open == '{' ? '}' : ']';
            int depth = 0; bool in_str = false, esc = false;
            for (size_t j = i; j < s.size(); ++j) {
                char c = s[j];
                if (in_str) { if (esc) esc = false; else if (c == '\\') esc = true; else if (c == '"') in_str = false; continue; }
                if (c == '"') { in_str = true; continue; }
                if (c == open) ++depth;
                else if (c == close) { if (--depth == 0) return j + 1; }
            }
            return s.size();
        }
        size_t j = i;
        while (j < s.size() && s[j] != ',' && s[j] != '}' && s[j] != ']' && s[j] != ' ' && s[j] != '\n' && s[j] != '\r' && s[j] != '\t') ++j;
        return j;
    }
    // Iterate the members of the object spanning [begin,end): fn(key, value_begin, value_end).
    template <typename Fn> void object_members(size_t begin, size_t end, Fn&& fn) const {
        size_t i = ws(begin);
        if (i >= s.size() || s[i] != '{') return;
        ++i;
        while (true) {
            i = ws(i);
            if (i >= end || i >= s.size() || s[i] == '}') return;
            std::string key; size_t kend;
            if (!parse_string(i, key, kend)) return;
            i = ws(kend);
            if (i >= s.size() || s[i] != ':') return;
            size_t vbegin = ws(i + 1);
            size_t vend = value_end(vbegin);
            fn(key, vbegin, vend);
            i = ws(vend);
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            return;
        }
    }
    // Spans of the elements of the array spanning [begin,end).
    std::vector<std::pair<size_t, size_t>> array_elements(size_t begin, size_t end) const {
        std::vector<std::pair<size_t, size_t>> out;
        size_t i = ws(begin);
        if (i >= s.size() || s[i] != '[') return out;
        ++i;
        while (true) {
            i = ws(i);
            if (i >= end || i >= s.size() || s[i] == ']') break;
            size_t vend = value_end(i);
            out.push_back({i, vend});
            i = ws(vend);
            if (i < s.size() && s[i] == ',') { ++i; continue; }
            break;
        }
        return out;
    }
    bool get_string(size_t ob, size_t oe, const char* key, std::string& out) const {
        bool found = false;
        object_members(ob, oe, [&](const std::string& k, size_t vb, size_t) {
            if (k == key) { size_t e; if (parse_string(vb, out, e)) found = true; }
        });
        return found;
    }
    bool get_bool(size_t ob, size_t oe, const char* key, bool& out) const {
        bool found = false;
        object_members(ob, oe, [&](const std::string& k, size_t vb, size_t ve) {
            if (k == key) { out = s.compare(vb, ve - vb, "true") == 0; found = true; }
        });
        return found;
    }
    bool get_ll(size_t ob, size_t oe, const char* key, long long& out) const {
        bool found = false;
        object_members(ob, oe, [&](const std::string& k, size_t vb, size_t ve) {
            if (k == key) { out = std::strtoll(s.substr(vb, ve - vb).c_str(), nullptr, 10); found = true; }
        });
        return found;
    }
};

bool parse_releases(const std::string& json, bool list_endpoint, std::vector<JsonRelease>& out) {
    Scanner sc(json);
    size_t begin = sc.ws(0);
    if (begin >= json.size()) return false;
    std::vector<std::pair<size_t, size_t>> spans;
    if (list_endpoint) {
        if (json[begin] != '[') return false;
        spans = sc.array_elements(begin, sc.value_end(begin));
    } else {
        if (json[begin] != '{') return false;
        spans.push_back({begin, sc.value_end(begin)});
    }
    for (auto& span : spans) {
        JsonRelease rel;
        if (!sc.get_string(span.first, span.second, "tag_name", rel.tag)) continue;
        sc.get_bool(span.first, span.second, "draft", rel.draft);
        // assets array
        size_t ab = 0, ae = 0; bool has_assets = false;
        sc.object_members(span.first, span.second, [&](const std::string& k, size_t vb, size_t ve) {
            if (k == "assets") { ab = vb; ae = ve; has_assets = true; }
        });
        if (has_assets) {
            for (auto& as : sc.array_elements(ab, ae)) {
                JsonAsset a;
                if (!sc.get_string(as.first, as.second, "name", a.name)) continue;
                sc.get_ll(as.first, as.second, "size", a.size);
                sc.get_string(as.first, as.second, "browser_download_url", a.url);
                sc.get_string(as.first, as.second, "digest", a.digest);  // absent/null -> ""
                rel.assets.push_back(std::move(a));
            }
        }
        out.push_back(std::move(rel));
    }
    return true;
}

// Rule lookup: longest-prefix match wins; unmatched takes the default mode.
selfupd::RuleMode rule_for(const std::string& rel) {
    selfupd::RuleMode best = g_cfg.default_mode;
    size_t best_len = 0;
    bool matched = false;
    for (size_t i = 0; i < g_cfg.file_rule_count; ++i) {
        const selfupd::FileRule& r = g_cfg.file_rules[i];
        if (!r.prefix) continue;
        size_t len = std::strlen(r.prefix);
        if (rel.compare(0, len, r.prefix) == 0 && (!matched || len > best_len)) {
            best = r.mode; best_len = len; matched = true;
        }
    }
    return best;
}

} // namespace

namespace {

// ---------------------------------------------------------------------------
// Extraction (DESIGN §3): stream the zip into update-staging/, each entry in
// order: (1) prefix strip or whole-update Layout abort, (2) zip-slip defence
// or whole-update Policy abort, (3) rule table — Deny entries are never
// written and Deny directories are never created.
// ---------------------------------------------------------------------------
selfupd::FailReason extract_zip(const char* zipfile) {
    using selfupd::FailReason;
    using selfupd::RuleMode;
    std::string staging = staging_root();
    remove_tree(staging);
    if (!mkdirs(staging)) return FailReason::Io;
    unzFile uf = unzOpen(zipfile);
    if (!uf) return FailReason::Io;
    FailReason fail = FailReason::None;
    std::vector<std::string> staged;
    const std::string prefix = g_cfg.zip_prefix ? g_cfg.zip_prefix : "";
    if (unzGoToFirstFile(uf) == UNZ_OK) {
        while (true) {
            unz_file_info64 info{};
            char name_buf[1024];
            if (unzGetCurrentFileInfo64(uf, &info, name_buf, sizeof name_buf, nullptr, 0, nullptr, 0) != UNZ_OK) {
                fail = FailReason::Io; break;
            }
            std::string name = name_buf;
            bool is_dir = !name.empty() && name.back() == '/';
            // (1) prefix strip
            if (name.compare(0, prefix.size(), prefix) != 0) { fail = FailReason::Layout; break; }
            std::string rel = name.substr(prefix.size());
            if (!rel.empty()) {
                // (2) zip-slip defence
                bool slip = false;
                if (rel.front() == '/') slip = true;
                // drive-style prefix: a ':' before the first '/'
                size_t colon = rel.find(':');
                size_t slash = rel.find('/');
                if (colon != std::string::npos && (slash == std::string::npos || colon < slash)) slip = true;
                std::string comps = (is_dir && rel.back() == '/') ? rel.substr(0, rel.size() - 1) : rel;
                size_t pos = 0;
                while (!slip && pos <= comps.size()) {
                    size_t sp = comps.find('/', pos);
                    std::string comp = comps.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
                    if (comp.empty() || comp == "..") slip = true;
                    if (sp == std::string::npos) break;
                    pos = sp + 1;
                }
                if (slip) { fail = FailReason::Policy; break; }
                // (3) rule table
                RuleMode mode = rule_for(rel);
                if (mode != RuleMode::Deny) {
                    std::string dest = staging + "/" + rel;
                    if (is_dir) {
                        if (!mkdirs(dest)) { fail = FailReason::Io; break; }
                    } else {
                        if (!mkdirs_for_file(dest)) { fail = FailReason::Io; break; }
                        if (unzOpenCurrentFile(uf) != UNZ_OK) { fail = FailReason::Io; break; }
                        FILE* out = std::fopen(dest.c_str(), "wb");
                        if (!out) { unzCloseCurrentFile(uf); fail = FailReason::Io; break; }
                        bool io_ok = true;
                        char buf[32768];
                        int n;
                        while ((n = unzReadCurrentFile(uf, buf, sizeof buf)) > 0) {
                            if (std::fwrite(buf, 1, (size_t)n, out) != (size_t)n) { io_ok = false; break; }
                        }
                        if (n < 0) io_ok = false;
                        if (unzCloseCurrentFile(uf) != UNZ_OK) io_ok = false;  // CRC error lands here
                        if (std::fclose(out) != 0) io_ok = false;
                        if (!io_ok) { fail = FailReason::Io; break; }
                        staged.push_back(rel);
                    }
                }
            }
            if (unzGoToNextFile(uf) != UNZ_OK) break;  // UNZ_END_OF_LIST_OF_FILE: done
        }
    }
    unzClose(uf);
    if (fail != FailReason::None) {
        remove_tree(staging);
        return fail;
    }
    g_staged = std::move(staged);
    return FailReason::None;
}

// Layout validation (DESIGN §3): the staged NRO exists, is plausibly sized,
// and carries the NRO0 magic at offset 0x10.
bool validate_staged_nro() {
    std::string nro = staging_root() + "/" + (g_cfg.nro_name ? g_cfg.nro_name : "");
    if (path_size(nro.c_str()) < 65536) return false;
    FILE* f = std::fopen(nro.c_str(), "rb");
    if (!f) return false;
    bool ok = std::fseek(f, 0x10, SEEK_SET) == 0;
    char magic[4] = {};
    if (ok) ok = std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "NRO0", 4) == 0;
    std::fclose(f);
    return ok;
}

// ---------------------------------------------------------------------------
// Download + verify + extract + validate. Entered with the state already
// published as Downloading; leaves the state terminal (ReadyToApply / Failed /
// Idle on cancel). Socket guard is held by the caller.
// ---------------------------------------------------------------------------
bool download_progress_cb(long long received, long long total, void*) {
    if (g_cancel.load(std::memory_order_acquire)) return false;
    long long denom = total > 0 ? total : g_asset_size;
    if (denom > 0) {
        long long pm = received * 1000 / denom;
        if (pm < 0) pm = 0;
        if (pm > 1000) pm = 1000;
        g_progress.store((int)pm, std::memory_order_relaxed);
    }
    return true;
}

void fail_download(selfupd::FailReason reason) {
    std::remove(part_path().c_str());
    publish_failed(reason);
}

void run_download_phase() {
    using selfupd::FailReason;
    using selfupd::State;
    g_progress.store(0, std::memory_order_relaxed);
    publish(State::Downloading);
    std::string digest_hex;
    FailReason reason = FailReason::None;
    bool ok = selfupd::detail::http_download_to_file(
        g_asset_url.c_str(), part_path().c_str(), g_asset_size, g_cfg,
        &download_progress_cb, nullptr, digest_hex, reason);
    if (!ok) {
        if (g_cancel.load(std::memory_order_acquire)) {
            std::remove(part_path().c_str());
            publish(State::Idle);
        } else {
            fail_download(reason != FailReason::None ? reason : FailReason::Http);
        }
        return;
    }
    if (g_cancel.load(std::memory_order_acquire)) {
        std::remove(part_path().c_str());
        publish(State::Idle);
        return;
    }
    publish(State::Verifying);
    // Byte count must equal the API-reported size.
    if (path_size(part_path().c_str()) != g_asset_size) { fail_download(FailReason::Io); return; }
    // Digest verified when the release carried one ("sha256:<hex>").
    if (g_asset_digest.compare(0, 7, "sha256:") == 0) {
        std::string want = g_asset_digest.substr(7);
        std::string got = digest_hex;
        if (got.empty()) {
            char hex[65];
            if (!hash_file_hex(part_path().c_str(), hex)) { fail_download(FailReason::Io); return; }
            got = hex;
        }
        std::string want_lc = want;
        for (char& c : want_lc) c = (char)std::tolower((unsigned char)c);
        if (got != want_lc) { fail_download(FailReason::Io); return; }
    }
    // Promote .part -> update.zip (FAT rename does not replace: remove first).
    std::remove(zip_path().c_str());
    if (std::rename(part_path().c_str(), zip_path().c_str()) != 0) { fail_download(FailReason::Io); return; }
    // Extract + validate.
    FailReason ex = extract_zip(zip_path().c_str());
    if (ex != FailReason::None) { publish_failed(ex); return; }
    if (!validate_staged_nro()) {
        remove_tree(staging_root());
        publish_failed(FailReason::Layout);
        return;
    }
    publish(State::ReadyToApply);
}

// ---------------------------------------------------------------------------
// Check phase: API call, candidate selection (DESIGN §2), asset selection.
// Leaves the state terminal or, with auto_download, continues in the same
// worker run into the download phase.
// ---------------------------------------------------------------------------
void run_check_phase() {
    using selfupd::FailReason;
    using selfupd::State;
    // Clean stale leftovers from any earlier run before starting our own.
    std::remove(part_path().c_str());
    remove_tree(staging_root());
    g_staged.clear();
    publish(State::Checking);

    // TLS gate, quiet: no bundle configured or the file is missing -> Failed(Tls).
    if (!g_cfg.cacert_path || !path_exists(g_cfg.cacert_path)) { publish_failed(FailReason::Tls); return; }

    SocketGuard sockets;
    FailReason reason = FailReason::None;
    if (!sockets.ensure(reason)) { publish_failed(reason); return; }

    std::string url = "https://api.github.com/repos/";
    url += g_cfg.repo ? g_cfg.repo : "";
    bool list = g_cfg.endpoint == selfupd::Endpoint::List;
    url += list ? "/releases?per_page=10" : "/releases/latest";

    std::string json;
    bool ok = selfupd::detail::http_get_to_memory(url.c_str(), g_cfg, json, reason);
    if (!ok) {
        sockets.release();
        if (g_cancel.load(std::memory_order_acquire)) publish(State::Idle);
        else publish_failed(reason != FailReason::None ? reason : FailReason::Http);
        return;
    }
    if (g_cancel.load(std::memory_order_acquire)) { sockets.release(); publish(State::Idle); return; }

    std::vector<JsonRelease> releases;
    if (!parse_releases(json, list, releases) || !g_cfg.compare) {
        sockets.release();
        publish_failed(FailReason::Parse);
        return;
    }
    // Winner = max by compare among Newer candidates; drafts skipped; the
    // prerelease flag is never consulted and list order is never trusted.
    const JsonRelease* winner = nullptr;
    for (const JsonRelease& rel : releases) {
        if (rel.draft) continue;
        if (g_cfg.compare(g_cfg.baked_version, rel.tag.c_str(), g_cfg.channel) != selfupd::Verdict::Newer) continue;
        if (!winner || g_cfg.compare(winner->tag.c_str(), rel.tag.c_str(), g_cfg.channel) == selfupd::Verdict::Newer)
            winner = &rel;
    }
    if (!winner) { sockets.release(); publish(State::UpToDate); return; }

    // Asset selection (DESIGN §2).
    const JsonAsset* asset = nullptr;
    if (g_cfg.asset_match == selfupd::AssetMatch::PrefixZip) {
        const std::string app = g_cfg.app_id ? g_cfg.app_id : "";
        for (const JsonAsset& a : winner->assets) {
            if (a.name.compare(0, app.size(), app) != 0) continue;
            if (a.name.size() < 4 || a.name.compare(a.name.size() - 4, 4, ".zip") != 0) continue;
            if (!asset || a.size > asset->size) asset = &a;  // largest wins
        }
    } else {  // ExactTemplate: {tag} replaced by the full tag_name (v kept)
        std::string want = g_cfg.asset_template ? g_cfg.asset_template : "";
        const std::string marker = "{tag}";
        size_t p = want.find(marker);
        if (p != std::string::npos) want.replace(p, marker.size(), winner->tag);
        for (const JsonAsset& a : winner->assets) if (a.name == want) { asset = &a; break; }
    }
    if (!asset) { sockets.release(); publish_failed(FailReason::AssetMissing); return; }

    g_asset_url = asset->url;
    g_asset_size = asset->size;
    g_asset_digest = asset->digest;
    set_latest(strip_one_v(winner->tag.c_str()));

    if (g_cancel.load(std::memory_order_acquire)) { sockets.release(); publish(State::Idle); return; }
    publish(State::Available);
    if (!g_cfg.auto_download) { sockets.release(); return; }
    run_download_phase();  // same worker run, no second tap
    sockets.release();
}

void worker_check() { run_check_phase(); }

void worker_download() {
    SocketGuard sockets;
    selfupd::FailReason reason = selfupd::FailReason::None;
    if (!sockets.ensure(reason)) { publish_failed(reason); return; }
    run_download_phase();
    sockets.release();
}

// ---------------------------------------------------------------------------
// Apply (DESIGN §3): deterministic order — staged paths sorted, bundled
// assets first, the staging-root NRO last. A mid-apply failure stops, leaves
// staging in place, and (the invariant) leaves the old NRO in place.
// ---------------------------------------------------------------------------
void worker_apply() {
    using selfupd::FailReason;
    using selfupd::RuleMode;
    using selfupd::State;
    publish(State::Applying);
    std::vector<std::string> order = g_staged;
    std::sort(order.begin(), order.end());
    const std::string nro = g_cfg.nro_name ? g_cfg.nro_name : "";
    auto it = std::find(order.begin(), order.end(), nro);
    if (it != order.end()) { order.erase(it); order.push_back(nro); }
    std::string staging = staging_root();
    for (const std::string& rel : order) {
        RuleMode mode = rule_for(rel);
        if (mode == RuleMode::Deny) continue;  // cannot happen; staged set is rule-filtered
        std::string target = join_path(g_cfg.install_root, rel);
        std::string staged = staging + "/" + rel;
        bool failed = false;
        if (!mkdirs_for_file(target)) failed = true;
        else if (mode == RuleMode::CreateOnly) {
            if (!path_exists(target.c_str()) && std::rename(staged.c_str(), target.c_str()) != 0) failed = true;
        } else {  // Overwrite: remove-then-rename (FAT semantics)
            if (path_is_dir(target.c_str())) failed = true;  // never remove a directory
            else {
                if (path_exists(target.c_str())) std::remove(target.c_str());
                if (std::rename(staged.c_str(), target.c_str()) != 0) failed = true;
            }
        }
        if (failed) { publish_failed(FailReason::Io); return; }  // staging left in place
    }
    std::remove(zip_path().c_str());
    remove_tree(staging);
    g_applied.store(true, std::memory_order_release);
    publish(State::ReadyToApply);  // state stays; applied() is the completion signal
}

} // namespace

namespace selfupd {

void init(const Config& cfg) {
    reap_worker();
    g_cfg = cfg;
    g_cfg_set = true;
    g_cancel.store(false, std::memory_order_relaxed);
    g_applied.store(false, std::memory_order_relaxed);
    g_progress.store(0, std::memory_order_relaxed);
    g_reason = FailReason::None;
    set_latest("");
    publish(State::Idle);
}

void request_check() {
    State s = g_state.load(std::memory_order_acquire);
    if (!g_cfg_set) return;
    if (s != State::Idle && s != State::UpToDate && s != State::Failed) return;
    reap_worker();
    g_cancel.store(false, std::memory_order_relaxed);
    g_applied.store(false, std::memory_order_relaxed);
    g_progress.store(0, std::memory_order_relaxed);
    g_reason = FailReason::None;
    set_latest("");
    publish(State::Checking);
    worker_slot() = std::thread(&worker_check);
}

void request_download() {
    if (!g_cfg_set) return;
    if (g_state.load(std::memory_order_acquire) != State::Available) return;
    reap_worker();
    g_cancel.store(false, std::memory_order_relaxed);
    g_progress.store(0, std::memory_order_relaxed);
    publish(State::Downloading);
    worker_slot() = std::thread(&worker_download);
}

void request_apply() {
    if (!g_cfg_set) return;
    if (g_state.load(std::memory_order_acquire) != State::ReadyToApply) return;
    if (g_applied.load(std::memory_order_acquire)) return;
    reap_worker();
    publish(State::Applying);
    worker_slot() = std::thread(&worker_apply);
}

void cancel() {
    State s = g_state.load(std::memory_order_acquire);
    if (s == State::Checking || s == State::Downloading || s == State::Verifying)
        g_cancel.store(true, std::memory_order_release);
}

Status status() {
    Status st;
    st.state = g_state.load(std::memory_order_acquire);
    st.progress_permille = g_progress.load(std::memory_order_relaxed);
    st.reason = g_reason;
    std::memcpy(st.latest, g_latest, sizeof st.latest);
    return st;
}

bool applied() { return g_applied.load(std::memory_order_acquire); }

// Canonical base strings (DESIGN §1). The glue composes state-specific text
// from status().latest where the design's table shows "<latest> ..." forms:
// Available -> "<latest> available", ReadyToApply -> "<latest> ready —
// restart to apply"; the base strings below are the state-only parts.
const char* state_text(State s) {
    switch (s) {
        case State::Idle:         return "";
        case State::Checking:     return "Checking...";
        case State::UpToDate:     return "Up to date";
        case State::Available:    return "available";
        case State::Downloading:  return "Downloading...";
        case State::Verifying:    return "Verifying...";
        case State::ReadyToApply: return "ready — restart to apply";
        case State::Applying:     return "Applying...";
        case State::Failed:       return "Couldn't check";
    }
    return "";
}

} // namespace selfupd

// ---------------------------------------------------------------------------
// libcurl transport (Switch build only; SELFUPD_TRANSPORT_CURL). Honours
// DESIGN §3 exactly: one guarded curl_global_init, CAINFO from the config,
// VERIFYPEER 1 / VERIFYHOST 2 never weakened, redirects followed (max 5),
// connect timeout 10 s, API total 30 s, download low-speed abort instead of
// a total cap, UA + GitHub Accept headers, and the runtime TLS gate:
// curl_version_info() must advertise CURL_VERSION_SSL before any request.
// ---------------------------------------------------------------------------
#ifdef SELFUPD_TRANSPORT_CURL
namespace selfupd {
namespace detail {
namespace {
bool g_curl_global_done = false;

bool tls_gate(const Config& cfg, FailReason& reason) {
    if (!cfg.cacert_path) { reason = FailReason::Tls; return false; }
    FILE* f = std::fopen(cfg.cacert_path, "rb");
    if (!f) { reason = FailReason::Tls; return false; }
    std::fclose(f);
    curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    if (!info || !(info->features & CURL_VERSION_SSL)) { reason = FailReason::Tls; return false; }
    if (!g_curl_global_done) { curl_global_init(CURL_GLOBAL_DEFAULT); g_curl_global_done = true; }
    return true;
}

FailReason map_curl_error(CURLcode rc) {
    if (rc == CURLE_SSL_CONNECT_ERROR || rc == CURLE_PEER_FAILED_VERIFICATION ||
        rc == CURLE_SSL_CERTPROBLEM || rc == CURLE_SSL_CACERT)
        return FailReason::Tls;
    if (rc == CURLE_COULDNT_RESOLVE_HOST || rc == CURLE_COULDNT_CONNECT ||
        rc == CURLE_OPERATION_TIMEDOUT)
        return FailReason::Offline;
    return FailReason::Http;
}

void apply_common(CURL* h, const Config& cfg) {
    curl_easy_setopt(h, CURLOPT_CAINFO, cfg.cacert_path);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    if (cfg.user_agent) curl_easy_setopt(h, CURLOPT_USERAGENT, cfg.user_agent);
}

size_t append_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

struct DownloadCtx {
    FILE* file;
    Sha256 sha;
    bool (*progress)(long long, long long, void*);
    void* progress_ctx;
    bool io_error;
};

size_t download_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<DownloadCtx*>(userdata);
    size_t bytes = size * nmemb;
    ctx->sha.update(reinterpret_cast<const unsigned char*>(ptr), bytes);
    if (std::fwrite(ptr, 1, bytes, ctx->file) != bytes) ctx->io_error = true;
    return bytes;
}

int download_xfer_cb(void* userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<DownloadCtx*>(userdata);
    if (ctx->progress && !ctx->progress((long long)dlnow, (long long)dltotal, ctx->progress_ctx)) return 1;
    return 0;
}
} // namespace

bool http_get_to_memory(const char* url, const Config& cfg, std::string& out, FailReason& reason) {
    if (!tls_gate(cfg, reason)) return false;
    CURL* h = curl_easy_init();
    if (!h) { reason = FailReason::Http; return false; }
    apply_common(h, cfg);
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, &append_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &out);
    curl_slist* headers = curl_slist_append(nullptr, "Accept: application/vnd.github+json");
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
    CURLcode rc = curl_easy_perform(h);
    long http_code = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(h);
    if (rc != CURLE_OK) { reason = map_curl_error(rc); return false; }
    if (http_code >= 400) { reason = FailReason::Http; return false; }
    return true;
}

bool http_download_to_file(const char* url, const char* path, long long /*expected_size*/,
                           const Config& cfg,
                           bool (*progress)(long long, long long, void*), void* progress_ctx,
                           std::string& digest_hex_out, FailReason& reason) {
    digest_hex_out.clear();
    if (!tls_gate(cfg, reason)) return false;
    FILE* f = std::fopen(path, "wb");
    if (!f) { reason = FailReason::Io; return false; }
    CURL* h = curl_easy_init();
    if (!h) { std::fclose(f); reason = FailReason::Http; return false; }
    apply_common(h, cfg);
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1024L);  // < 1 KB/s ...
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 30L);      // ... sustained 30 s aborts
    DownloadCtx ctx{f, Sha256(), progress, progress_ctx, false};
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, &download_write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, &download_xfer_cb);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    CURLcode rc = curl_easy_perform(h);
    long http_code = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(h);
    bool io_error = ctx.io_error;
    if (std::fclose(f) != 0) io_error = true;
    if (rc != CURLE_OK) { reason = map_curl_error(rc); return false; }
    if (http_code >= 400) { reason = FailReason::Http; return false; }
    if (io_error) { reason = FailReason::Io; return false; }
    char hex[65];
    ctx.sha.finish_hex(hex);
    digest_hex_out = hex;
    return true;
}
} // namespace detail
} // namespace selfupd
#endif // SELFUPD_TRANSPORT_CURL
