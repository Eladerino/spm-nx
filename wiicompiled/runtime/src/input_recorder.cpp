// Grade 1 KPAD input recorder / replayer: see include/input_recorder.h for
// the design and the stream format. The format layer below is platform-free
// (the host fixtures link this file on its own); the Switch glue after it
// owns the diag handshakes, the UserData/Logs/ file and the stderr lines.

#include "input_recorder.h"

#include "wii_remote_input.h"

#include <algorithm>
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

struct ParsedStream {
    int version = 0;
    std::string versionField, titleField, noteField;
    std::vector<Record> records;
};

// Parses one whole stream (a file, or one segment of a recording). False
// means "refuse whole": bad header, a v1 body with any defect, or a v2
// body in which not one chunk validates. A v2 body that starts valid and
// goes bad keeps its good prefix -- that is the tolerance a writer killed
// mid-flush (guest panic, power loss) relies on.
bool ParseStream(const uint8_t* data, size_t size, ParsedStream& out) {
    if (!data) return false;
    // The header is the first four \n-terminated lines; the body follows
    // immediately after the fourth newline.
    size_t pos = 0;
    std::string lines[4];
    for (int i = 0; i < 4; ++i) {
        if (pos >= size) return false;
        const void* nl = std::memchr(data + pos, '\n', size - pos);
        if (!nl) return false;
        const size_t end = size_t(static_cast<const uint8_t*>(nl) - data);
        lines[i].assign(reinterpret_cast<const char*>(data + pos), end - pos);
        if (!lines[i].empty() && lines[i].back() == '\r') lines[i].pop_back();
        pos = end + 1;
    }
    if (lines[0] == "SPM-PADREC 1")
        out.version = 1;
    else if (lines[0] == "SPM-PADREC 2")
        out.version = 2;
    else
        return false;
    if (lines[1].rfind("version=", 0) != 0 || lines[2].rfind("title=", 0) != 0 ||
        lines[3].rfind("note=", 0) != 0)
        return false;
    out.versionField = lines[1].substr(8);
    out.titleField = lines[2].substr(6);
    out.noteField = lines[3].substr(5);
    out.records.clear();

    if (out.version == 1) {
        // Legacy body: unchunked records, validated whole, as v1 was.
        const size_t recordBytes = size - pos;
        if (recordBytes % kRecordSize != 0) return false;
        out.records.reserve(recordBytes / kRecordSize);
        for (size_t off = pos; off < size; off += kRecordSize) {
            Record record;
            DecodeRecord(data + off, record);
            if (!out.records.empty() && record.readSeq <= out.records.back().readSeq) return false;
            out.records.push_back(record);
        }
        return true;
    }

    // v2 body: chunks. Each chunk is decoded into a scratch run first and
    // only committed when its CRC and the read_seq run both hold, so a bad
    // chunk costs exactly itself (and everything after it, which can no
    // longer be trusted to line up), never the prefix.
    if (pos == size) return true;  // header only: a recording stopped before any flush
    bool anyChunk = false;
    while (pos < size) {
        if (size - pos < kChunkHeaderSize) break;  // torn chunk header: tail lost, prefix kept
        const uint32_t magic = GetU32(data + pos);
        const uint32_t count = GetU32(data + pos + 4);
        const uint32_t crc = GetU32(data + pos + 8);
        if (magic != kChunkMagic || count == 0 || count > kMaxChunkRecords) break;
        const size_t bodyBytes = size_t(count) * kRecordSize;
        if (size - pos - kChunkHeaderSize < bodyBytes) break;  // torn chunk body
        const uint8_t* body = data + pos + kChunkHeaderSize;
        if (PadrecCrc32(body, bodyBytes) != crc) break;
        std::vector<Record> chunk;
        chunk.reserve(count);
        bool monotone = true;
        for (uint32_t i = 0; i < count; ++i) {
            Record record;
            DecodeRecord(body + size_t(i) * kRecordSize, record);
            const uint32_t prev = chunk.empty() ? (out.records.empty() ? 0 : out.records.back().readSeq)
                                                : chunk.back().readSeq;
            if ((!chunk.empty() || !out.records.empty()) && record.readSeq <= prev) {
                monotone = false;
                break;
            }
            chunk.push_back(record);
        }
        if (!monotone) break;
        out.records.insert(out.records.end(), chunk.begin(), chunk.end());
        anyChunk = true;
        pos += kChunkHeaderSize + bodyBytes;
    }
    return anyChunk;
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

uint32_t PadrecCrc32(const uint8_t* data, size_t size) {
    // IEEE CRC-32 (the zip polynomial), table built on first use.
    static uint32_t table[256];
    static bool built = [] {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        return true;
    }();
    (void)built;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

size_t EncodeChunk(const uint8_t* recordBytes, size_t recordCount, uint8_t* out, size_t outCapacity) {
    if (!recordBytes || !out || recordCount == 0 || recordCount > kMaxChunkRecords) return 0;
    const size_t total = kChunkHeaderSize + recordCount * kRecordSize;
    if (outCapacity < total) return 0;
    PutU32(out + 0, kChunkMagic);
    PutU32(out + 4, uint32_t(recordCount));
    PutU32(out + 8, PadrecCrc32(recordBytes, recordCount * kRecordSize));
    std::memcpy(out + kChunkHeaderSize, recordBytes, recordCount * kRecordSize);
    return total;
}

void Recorder::Start(const char* version, const char* title, const char* note, uint32_t readBase) {
    header_ = "SPM-PADREC 2\nversion=" + HeaderField(version) + "\ntitle=" + HeaderField(title) +
              "\nnote=" + HeaderField(note) + "\n";
    if (ring_.size() != kRingBytes) ring_.assign(kRingBytes, 0);  // the 8 MB preallocation, once
    prev_ = PadSample{};
    recording_ = true;
    havePrev_ = false;
    readBase_ = readBase;
    reads_ = readBase;
    records_ = 0;
    lastStoredSeq_ = readBase;
    firstRetrace_ = lastRetrace_ = 0;
    firstPresents_ = lastPresents_ = 0;
}

int Recorder::Observe(const PadSample& sample, uint32_t retrace, uint32_t presents) {
    if (!recording_) return 0;
    const uint32_t seq = reads_++;
    if (seq == readBase_) {
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
    ParsedStream parsed;
    if (!ParseStream(data, size, parsed)) return false;
    version_ = std::move(parsed.versionField);
    title_ = std::move(parsed.titleField);
    note_ = std::move(parsed.noteField);
    records_ = std::move(parsed.records);
    loaded_ = true;
    return true;
}

bool Player::Append(const uint8_t* data, size_t size) {
    if (!loaded_) return Load(data, size);
    ParsedStream parsed;
    if (!ParseStream(data, size, parsed)) return false;
    if (parsed.records.empty()) return true;  // an empty segment joins as nothing
    if (!records_.empty()) {
        const uint32_t last = records_.back().readSeq;
        const uint32_t first = parsed.records.front().readSeq;
        // The next segment's first read is always stored, and the previous
        // segment stored a heartbeat within kHeartbeatReads of its end, so
        // a true continuation lands in (last, last + kHeartbeatReads + 1].
        // Anything wider is a different recording's segment, not ours.
        if (first <= last || first - last > kHeartbeatReads + 1) return false;
    }
    records_.insert(records_.end(), parsed.records.begin(), parsed.records.end());
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
        // stream; Recorder always stores read 0 of a segment): neutral
        // until it lands.
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
// state they drive lives here. All file work happens under g_mutex: at arm
// / stop / exit, on ring fill, and at the periodic safety flush -- the
// last two fire from RecordSample's bookkeeping at a KPAD read boundary,
// the same place the original design's ring-fill flush already ran, and
// never from the draw path. RecordSample in steady state only appends to
// RAM.
namespace {

Recorder g_recorder;
Player g_player;
std::mutex g_mutex;
std::atomic<bool> g_recordOn{false};
std::atomic<bool> g_replayOn{false};
bool g_exitHookRegistered = false;
uint32_t g_segment = 0;  // 0 = pad-record.bin itself; later segments are ring-fill continuations

// Periodic safety flush: whichever comes first since the last flush, this
// many KPAD reads (nominally one per guest frame: 1800 reads is ~30 s at
// 60 fps, ~60 s at 30 fps) with anything pending, or this many pending
// records. One flush writes at most kMaxChunkRecords-sized chunks of the
// pending run -- bounded by 2048 pending records (~90 KB) outside the
// ring-fill path, which can owe the whole ring and writes it in chunks.
constexpr uint32_t kPeriodicFlushReads = 1800;
constexpr uint32_t kMaxPendingRecords = 2048;

uint32_t g_readBase = 0;          // read_seq of the current segment's first read
uint32_t g_flushedRecords = 0;    // current segment's records already on disk
uint32_t g_lastFlushReads = 0;    // Reads() at the last successful flush
bool g_segmentOnDisk = false;     // the current segment's file exists with its header
std::vector<uint8_t> g_chunkScratch;

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

// Creates the current segment's file with its header, if that has not
// happened yet. Segment 0 first rotates any previous pad-record.bin to
// pad-record.prev.bin -- one previous recording kept, the shape
// stderr.log's handling suggests (spm_switch_init.cpp replaces the old log
// at startup; a recorder that overwrote the only copy of the last run at
// flush time would lose it the same way). FAT rename does not replace, so
// the old .prev is removed first (debug_server.cpp's put).
bool EnsureSegmentFileLocked() {
    if (g_segmentOnDisk) return true;
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
    const bool ok = std::fwrite(header.data(), 1, header.size(), f) == header.size();
    std::fclose(f);
    if (!ok) {
        std::fprintf(stderr, "[padrec] cannot write %s\n", path.c_str());
        return false;
    }
    g_segmentOnDisk = true;
    return true;
}

// Appends every not-yet-saved record of the current segment to its file,
// in chunks of at most kMaxChunkRecords. This is the one write path: the
// periodic safety flush, the ring-fill flush and the stop/exit flush all
// come through here.
bool FlushPendingLocked() {
    if (!EnsureSegmentFileLocked()) return false;
    const uint32_t total = g_recorder.Records();
    if (g_flushedRecords >= total) return true;
    FILE* f = std::fopen(SegmentPath().c_str(), "ab");
    if (!f) {
        std::fprintf(stderr, "[padrec] cannot write %s\n", SegmentPath().c_str());
        return false;
    }
    if (g_chunkScratch.size() < kChunkHeaderSize + kMaxChunkRecords * kRecordSize)
        g_chunkScratch.resize(kChunkHeaderSize + kMaxChunkRecords * kRecordSize);
    bool ok = true;
    while (ok && g_flushedRecords < total) {
        const uint32_t count = std::min(uint32_t(kMaxChunkRecords), total - g_flushedRecords);
        const uint8_t* bytes = g_recorder.RecordData() + size_t(g_flushedRecords) * kRecordSize;
        const size_t written = EncodeChunk(bytes, count, g_chunkScratch.data(), g_chunkScratch.size());
        ok = written > 0 && std::fwrite(g_chunkScratch.data(), 1, written, f) == written;
        if (ok) g_flushedRecords += count;
    }
    std::fclose(f);
    if (!ok) {
        std::fprintf(stderr, "[padrec] cannot write %s\n", SegmentPath().c_str());
        return false;
    }
    g_lastFlushReads = g_recorder.Reads();
    return true;
}

void StartSegmentLocked() {
    g_recorder.Start(BakedVersion(), "unknown",
                     g_segment == 0 ? "" : "continuation: the previous segment's ring filled",
                     g_readBase);
    g_flushedRecords = 0;
    g_segmentOnDisk = false;
    g_lastFlushReads = g_recorder.Reads();
}

void StopRecordingLocked() {
    if (!g_recorder.Recording()) return;
    const bool wrote = FlushPendingLocked();
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

// The safety flush without the stop: same mutex, same single write path as
// the periodic flush, for callers that learn about danger from outside
// RecordSample's bookkeeping (the scaffold's applet hook / message watcher,
// when the game loses the screen). Recording continues afterwards.
void FlushPending() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_recorder.Recording()) return;
    FlushPendingLocked();
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
        // The ring filled: flush this segment out and keep recording into
        // the next one, its read_seq run continuing where this one's ended
        // (its header note says why it exists).
        if (FlushPendingLocked()) {
            std::fprintf(stderr, "[padrec] ring full: flushed %s, continuing in segment %u\n",
                         SegmentPath().c_str(), g_segment + 1);
            std::fflush(stderr);
        }
        g_readBase = g_recorder.Reads();
        ++g_segment;
        StartSegmentLocked();
        return;
    }
    // Periodic safety flush: persist what the ring holds so a hard death
    // from here loses at most one interval of reads, not the run.
    const uint32_t pending = g_recorder.Records() - g_flushedRecords;
    if (pending > 0 && (pending >= kMaxPendingRecords ||
                        g_recorder.Reads() - g_lastFlushReads >= kPeriodicFlushReads))
        FlushPendingLocked();
}

}  // namespace InputRecorder

extern "C" void spm_input_record_set(bool enabled) {
    using namespace InputRecorder;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (enabled) {
        if (g_recorder.Recording()) return;
        RegisterExitHookLocked();
        g_segment = 0;
        g_readBase = 0;
        // Continuation segments from an earlier, longer recording must not
        // survive into this one: the replay loader joins pad-record.segN
        // onto pad-record.bin, and a stale segment would otherwise pass
        // the join check against a short new recording only by luck.
        for (uint32_t n = 1; n <= 64; ++n)
            std::remove((LogsDir() + "/pad-record.seg" + std::to_string(n) + ".bin").c_str());
        StartSegmentLocked();
        g_recordOn.store(true);
        std::fprintf(stderr, "[padrec] recording to %s\n", SegmentPath().c_str());
        std::fflush(stderr);
    } else {
        StopRecordingLocked();
    }
}

extern "C" void spm_input_flush_at_exit(void) {
    InputRecorder::FlushAtExit();
}

extern "C" void spm_input_flush_pending(void) {
    InputRecorder::FlushPending();
}

extern "C" void spm_input_replay_set(const char* filename) {
    using namespace InputRecorder;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!filename) {
        g_replayOn.store(false);
        g_player.Unload();
        return;
    }
    const bool defaultName = *filename == '\0' || std::strcmp(filename, "pad-record.bin") == 0;
    const std::string path = LogsDir() + "/" + (*filename ? filename : "pad-record.bin");
    auto readWholeFile = [](const std::string& p, std::vector<uint8_t>& bytes) {
        FILE* f = std::fopen(p.c_str(), "rb");
        if (!f) return false;
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        bool read = size >= 0;
        if (read && size > 0) {
            bytes.resize(size_t(size));
            read = std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
        }
        std::fclose(f);
        return read;
    };
    std::vector<uint8_t> bytes;
    if (!readWholeFile(path, bytes)) {
        std::fprintf(stderr, "[padreplay] cannot open %s; live input for this run\n", path.c_str());
        std::fflush(stderr);
        g_replayOn.store(false);
        g_player.Unload();
        return;
    }
    if (!g_player.Load(bytes.data(), bytes.size())) {
        // Exactly one line, live input for the whole run, never a crash.
        std::fprintf(stderr, "[padreplay] %s is not a valid SPM-PADREC stream; live input for this run\n",
                     path.c_str());
        std::fflush(stderr);
        g_replayOn.store(false);
        g_player.Unload();
        return;
    }
    if (defaultName) {
        // A recording that outgrew the ring lives in continuation
        // segments; join each that continues the read_seq run, and stop
        // at the first that is missing or does not join.
        for (uint32_t n = 1; n <= 64; ++n) {
            const std::string segPath =
                LogsDir() + "/pad-record.seg" + std::to_string(n) + ".bin";
            std::vector<uint8_t> segBytes;
            if (!readWholeFile(segPath, segBytes)) break;
            if (!g_player.Append(segBytes.data(), segBytes.size())) {
                std::fprintf(stderr, "[padreplay] %s does not continue the stream; ignoring it\n",
                             segPath.c_str());
                std::fflush(stderr);
                break;
            }
        }
    }
    g_replayOn.store(true);
    std::fprintf(stderr, "[padreplay] replaying %s: %zu records\n", path.c_str(), g_player.RecordCount());
    std::fflush(stderr);
}
