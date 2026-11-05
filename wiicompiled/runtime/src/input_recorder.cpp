// Grade 1 KPAD input recorder / replayer: see include/input_recorder.h for
// the design and the stream format. The format layer below is platform-free
// (the host fixtures link this file on its own); the Switch glue after it
// owns the diag handshakes, the UserData/Logs/ file and the stderr lines.

#include "input_recorder.h"

#include "wii_remote_input.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "the SPM-PADREC stream is little-endian; this host is not"
#endif

namespace InputRecorder {
namespace {

void PutU32(uint8_t* out, uint32_t v) { std::memcpy(out, &v, 4); }
void PutF32(uint8_t* out, float v) { std::memcpy(out, &v, 4); }
uint32_t GetU32(const uint8_t* in) {
    uint32_t v;
    std::memcpy(&v, in, 4);
    return v;
}
float GetF32(const uint8_t* in) {
    float v;
    std::memcpy(&v, in, 4);
    return v;
}
uint32_t FloatBits(float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    return bits;
}

// One header field: single-line (the header is four \n-terminated lines)
// and bounded, so a stray string can never smuggle extra lines into the
// block or bloat it.
std::string HeaderField(const char* text) {
    std::string out;
    if (!text) return out;
    for (const char* p = text; *p && out.size() < 120; ++p)
        out += (*p == '\n' || *p == '\r') ? ' ' : *p;
    return out;
}

}  // namespace

bool SamplesEqual(const PadSample& a, const PadSample& b) {
    if (a.hold != b.hold || a.clHold != b.clHold || a.pointerValid != b.pointerValid) return false;
    for (int i = 0; i < 3; ++i)
        if (FloatBits(a.acc[i]) != FloatBits(b.acc[i])) return false;
    for (int i = 0; i < 2; ++i)
        if (FloatBits(a.pointer[i]) != FloatBits(b.pointer[i])) return false;
    return true;
}

void EncodeRecord(const Record& record, uint8_t out[kRecordSize]) {
    std::memset(out, 0, kRecordSize);
    PutU32(out + 0, record.readSeq);
    PutU32(out + 4, record.retrace);
    PutU32(out + 8, record.presents);
    PutU32(out + 12, record.sample.hold);
    PutU32(out + 16, record.sample.clHold);
    for (int i = 0; i < 3; ++i) PutF32(out + 20 + 4 * i, record.sample.acc[i]);
    for (int i = 0; i < 2; ++i) PutF32(out + 32 + 4 * i, record.sample.pointer[i]);
    out[40] = record.sample.pointerValid ? 1 : 0;
}

bool DecodeRecord(const uint8_t* in, Record& out) {
    if (!in) return false;
    out.readSeq = GetU32(in + 0);
    out.retrace = GetU32(in + 4);
    out.presents = GetU32(in + 8);
    out.sample.hold = GetU32(in + 12);
    out.sample.clHold = GetU32(in + 16);
    for (int i = 0; i < 3; ++i) out.sample.acc[i] = GetF32(in + 20 + 4 * i);
    for (int i = 0; i < 2; ++i) out.sample.pointer[i] = GetF32(in + 32 + 4 * i);
    out.sample.pointerValid = in[40] != 0;
    return true;
}

void Recorder::Start(const char* version, const char* title, const char* note) {
    header_ = "SPM-PADREC 1\nversion=" + HeaderField(version) + "\ntitle=" + HeaderField(title) +
              "\nnote=" + HeaderField(note) + "\n";
    if (ring_.size() != kRingBytes) ring_.assign(kRingBytes, 0);  // the 8 MB preallocation, once
    prev_ = PadSample{};
    recording_ = true;
    havePrev_ = false;
    reads_ = 0;
    records_ = 0;
    lastStoredSeq_ = 0;
    firstRetrace_ = lastRetrace_ = 0;
    firstPresents_ = lastPresents_ = 0;
}

int Recorder::Observe(const PadSample& sample, uint32_t retrace, uint32_t presents) {
    if (!recording_) return 0;
    const uint32_t seq = reads_++;
    if (seq == 0) {
        firstRetrace_ = retrace;
        firstPresents_ = presents;
    }
    lastRetrace_ = retrace;
    lastPresents_ = presents;
    const bool changed = !havePrev_ || !SamplesEqual(sample, prev_);
    const bool heartbeat = havePrev_ && (seq - lastStoredSeq_ >= kHeartbeatReads);
    prev_ = sample;
    havePrev_ = true;
    if (!changed && !heartbeat) return 0;
    if (records_ >= kRingRecords) return 2;  // full and not yet flushed: refuse, stay loud
    Record record;
    record.readSeq = seq;
    record.retrace = retrace;
    record.presents = presents;
    record.sample = sample;
    EncodeRecord(record, ring_.data() + size_t(records_) * kRecordSize);
    ++records_;
    lastStoredSeq_ = seq;
    return records_ >= kRingRecords ? 2 : 1;
}

bool Player::Load(const uint8_t* data, size_t size) {
    Unload();
    if (!data) return false;
    // The header is the first four \n-terminated lines; records follow
    // immediately after the fourth newline.
    size_t pos = 0;
    std::string lines[4];
    for (int i = 0; i < 4; ++i) {
        const void* nl = std::memchr(data + pos, '\n', size - pos);
        if (!nl) return false;
        const size_t end = size_t(static_cast<const uint8_t*>(nl) - data);
        lines[i].assign(reinterpret_cast<const char*>(data + pos), end - pos);
        if (!lines[i].empty() && lines[i].back() == '\r') lines[i].pop_back();
        pos = end + 1;
    }
    if (lines[0] != "SPM-PADREC 1") return false;
    if (lines[1].rfind("version=", 0) != 0 || lines[2].rfind("title=", 0) != 0 ||
        lines[3].rfind("note=", 0) != 0)
        return false;
    const size_t recordBytes = size - pos;
    if (recordBytes % kRecordSize != 0) return false;
    std::vector<Record> records;
    records.reserve(recordBytes / kRecordSize);
    for (size_t off = pos; off < size; off += kRecordSize) {
        Record record;
        DecodeRecord(data + off, record);
        if (!records.empty() && record.readSeq <= records.back().readSeq) return false;
        records.push_back(record);
    }
    version_ = lines[1].substr(8);
    title_ = lines[2].substr(6);
    note_ = lines[3].substr(5);
    records_ = std::move(records);
    loaded_ = true;
    return true;
}

void Player::Unload() {
    version_.clear();
    title_.clear();
    note_.clear();
    records_.clear();
    loaded_ = false;
    next_ = 0;
    served_ = 0;
    current_ = PadSample{};
    haveCurrent_ = false;
}

bool Player::Next(PadSample& out) {
    if (!loaded_) return false;
    const uint32_t read = served_++;
    while (next_ < records_.size() && records_[next_].readSeq <= read) {
        current_ = records_[next_].sample;
        haveCurrent_ = true;
        ++next_;
    }
    if (next_ < records_.size()) {
        // Before the first record's read (only possible in a hand-made
        // stream; Recorder always stores read 0): neutral until it lands.
        out = haveCurrent_ ? current_ : PadSample{};
        return true;
    }
    if (records_.empty()) {
        out = PadSample{};
        return true;
    }
    // Past the last record: hold the final sample for kHeartbeatReads
    // reads, then idle (neutral) forever.
    out = (read - records_.back().readSeq <= kHeartbeatReads) ? current_ : PadSample{};
    return true;
}

// ---- Switch glue ----------------------------------------------------------
// The diag switches live in switch/deko (dk.h's padrec / padreplay); the
// state they drive lives here. All file work happens under g_mutex, on the
// thread that flips the switch, stops the recording or exits the process --
// never on a read in steady state (RecordSample below only appends to RAM
// and, on the read that fills the ring, safety-flushes: the one mid-run
// write the scope allows, on exactly the read that would otherwise lose
// the run).
namespace {

Recorder g_recorder;
Player g_player;
std::mutex g_mutex;
std::atomic<bool> g_recordOn{false};
std::atomic<bool> g_replayOn{false};
bool g_exitHookRegistered = false;
uint32_t g_segment = 0;  // 0 = pad-record.bin itself; later segments are ring-fill continuations

// CMake passes SPM_BAKED_VERSION as a bare token (SpmSwitch.cmake's single
// version source, the same one the self-updater glue stringizes).
#ifdef SPM_BAKED_VERSION
#define SPM_PADREC_STR_(x) #x
#define SPM_PADREC_STR(x) SPM_PADREC_STR_(x)
const char* BakedVersion() { return SPM_PADREC_STR(SPM_BAKED_VERSION); }
#else
const char* BakedVersion() { return ""; }
#endif

std::string LogsDir() {
    if (const char* data = std::getenv("WIICOMPILED_DATA_DIR"); data && *data)
        return std::string(data) + "/Logs";
#if defined(__SWITCH__)
    return "/switch/spm-nx/UserData/Logs";
#else
    return "UserData/Logs";
#endif
}

std::string SegmentPath() {
    if (g_segment == 0) return LogsDir() + "/pad-record.bin";
    return LogsDir() + "/pad-record.seg" + std::to_string(g_segment) + ".bin";
}

// Writes the buffered segment. Segment 0 first rotates any previous
// pad-record.bin to pad-record.prev.bin -- one previous recording kept,
// the shape stderr.log's handling suggests (spm_switch_init.cpp replaces
// the old log at startup; a recorder that overwrote the only copy of the
// last run at flush time would lose it the same way). FAT rename does not
// replace, so the old .prev is removed first (debug_server.cpp's put).
bool FlushLocked() {
    const std::string path = SegmentPath();
    if (g_segment == 0) {
        const std::string prev = LogsDir() + "/pad-record.prev.bin";
        std::remove(prev.c_str());
        std::rename(path.c_str(), prev.c_str());  // fine when absent: the first recording
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "[padrec] cannot write %s\n", path.c_str());
        return false;
    }
    const std::string& header = g_recorder.Header();
    std::fwrite(header.data(), 1, header.size(), f);
    if (g_recorder.RecordBytes() > 0)
        std::fwrite(g_recorder.RecordData(), 1, g_recorder.RecordBytes(), f);
    std::fclose(f);
    return true;
}

void StartSegmentLocked() {
    g_recorder.Start(BakedVersion(), "unknown",
                     g_segment == 0 ? "" : "continuation: the previous segment's ring filled");
}

void StopRecordingLocked() {
    if (!g_recorder.Recording()) return;
    const bool wrote = FlushLocked();
    std::fprintf(stderr, "[padrec] stopped: %u reads, %u records, retraces %u..%u, presents %u..%u%s\n",
                 g_recorder.Reads(), g_recorder.Records(), g_recorder.FirstRetrace(),
                 g_recorder.LastRetrace(), g_recorder.FirstPresents(), g_recorder.LastPresents(),
                 wrote ? "" : " (write failed)");
    std::fflush(stderr);
    g_recorder.Stop();
    g_recordOn.store(false);
}

void FlushAtExit() {
    std::lock_guard<std::mutex> lock(g_mutex);
    StopRecordingLocked();
}

void RegisterExitHookLocked() {
    if (g_exitHookRegistered) return;
    g_exitHookRegistered = true;
    std::atexit(&FlushAtExit);
}

}  // namespace

bool ReplayActive() { return g_replayOn.load(); }
bool RecordActive() { return g_recordOn.load(); }

bool ReplayNext(WiiRemoteInput::KpadSample& out) {
    PadSample sample;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_replayOn.load() || !g_player.Next(sample)) return false;
    }
    out = WiiRemoteInput::KpadSample{};
    out.hold = sample.hold;
    out.clHold = sample.clHold;
    for (int i = 0; i < 3; ++i) out.acc[i] = sample.acc[i];
    for (int i = 0; i < 2; ++i) out.pointer[i] = sample.pointer[i];
    out.pointerValid = sample.pointerValid;
    return true;
}

void RecordSample(const WiiRemoteInput::KpadSample& sample, uint32_t retrace, uint32_t presents) {
    if (!g_recordOn.load()) return;
    PadSample pad;
    pad.hold = sample.hold;
    pad.clHold = sample.clHold;
    for (int i = 0; i < 3; ++i) pad.acc[i] = sample.acc[i];
    for (int i = 0; i < 2; ++i) pad.pointer[i] = sample.pointer[i];
    pad.pointerValid = sample.pointerValid;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_recorder.Observe(pad, retrace, presents) == 2) {
        // The ring filled: safety-flush this segment and keep recording
        // into the next one (its header note says why it exists).
        if (FlushLocked()) {
            std::fprintf(stderr, "[padrec] ring full: flushed %s, continuing in segment %u\n",
                         SegmentPath().c_str(), g_segment + 1);
            std::fflush(stderr);
        }
        ++g_segment;
        StartSegmentLocked();
    }
}

}  // namespace InputRecorder

extern "C" void spm_input_record_set(bool enabled) {
    using namespace InputRecorder;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (enabled) {
        if (g_recorder.Recording()) return;
        RegisterExitHookLocked();
        g_segment = 0;
        StartSegmentLocked();
        g_recordOn.store(true);
        std::fprintf(stderr, "[padrec] recording to %s\n", SegmentPath().c_str());
        std::fflush(stderr);
    } else {
        StopRecordingLocked();
    }
}

extern "C" void spm_input_replay_set(const char* filename) {
    using namespace InputRecorder;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!filename) {
        g_replayOn.store(false);
        g_player.Unload();
        return;
    }
    const std::string path = LogsDir() + "/" + (*filename ? filename : "pad-record.bin");
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "[padreplay] cannot open %s; live input for this run\n", path.c_str());
        std::fflush(stderr);
        g_replayOn.store(false);
        g_player.Unload();
        return;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> bytes;
    bool read = size >= 0;
    if (read && size > 0) {
        bytes.resize(size_t(size));
        read = std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
    }
    std::fclose(f);
    if (!read || !g_player.Load(bytes.data(), bytes.size())) {
        // Exactly one line, live input for the whole run, never a crash.
        std::fprintf(stderr, "[padreplay] %s is not a valid SPM-PADREC stream; live input for this run\n",
                     path.c_str());
        std::fflush(stderr);
        g_replayOn.store(false);
        g_player.Unload();
        return;
    }
    g_replayOn.store(true);
    std::fprintf(stderr, "[padreplay] replaying %s: %zu records\n", path.c_str(), g_player.RecordCount());
    std::fflush(stderr);
}
