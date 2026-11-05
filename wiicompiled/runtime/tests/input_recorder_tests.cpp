// Host tests for the Grade 1 input recorder's stream format and ring
// (include/input_recorder.h, src/input_recorder.cpp): the scope's Phase 0
// desk fixtures (spm-perf-scope/SCOPE-INPUT-RECORDER-2026-11-05.md §7),
// extended for the format v2 chunk layout and the flush redesign of
// Nov 5, 2026. A synthesised sample sequence -- pointer/acc changes and a
// 200-read constant stretch -- is encoded through the real Recorder,
// decoded through the real Player, and must reconstruct bit-identically,
// with heartbeats present in the constant stretch at <=60-read gaps and
// change-only records elsewhere. The chunk integrity rules (a bad chunk
// costs itself and the tail, never the prefix), legacy v1 loading,
// segment joining through Player::Append, the ring-full signal and the
// replay edge behaviour (hold the final sample for 60 reads, then idle
// forever) are checked against the same real code. Proves the plumbing
// only; the Switch glue's file writes are not exercised here.

#include "input_recorder.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

uint32_t FloatBits(float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    return bits;
}

bool SameSample(const InputRecorder::PadSample& a, const InputRecorder::PadSample& b) {
    return InputRecorder::SamplesEqual(a, b);
}

InputRecorder::PadSample MakeSample(uint32_t i) {
    InputRecorder::PadSample s;
    s.hold = 0x1000u + i;
    s.clHold = i * 3u;
    s.acc[0] = 0.25f * float(i % 7) - 0.75f;
    s.acc[1] = -1.0f + 0.01f * float(i);
    s.acc[2] = (i % 2) ? -0.0f : 0.5f;  // -0.0f: bitwise distinct from 0.0f
    s.pointer[0] = -1.0f + 0.02f * float(i);
    s.pointer[1] = 1.0f - 0.03f * float(i);
    s.pointerValid = (i % 3) != 0;
    return s;
}

using namespace InputRecorder;

// Assembles a v2 stream exactly the way the glue's writer does: the
// recorder's header text, then its ring bytes in chunks of at most
// chunkRecords records each.
std::vector<uint8_t> BuildV2Stream(const Recorder& rec, size_t chunkRecords) {
    std::vector<uint8_t> stream(rec.Header().begin(), rec.Header().end());
    std::vector<uint8_t> chunk(kChunkHeaderSize + kMaxChunkRecords * kRecordSize);
    for (uint32_t off = 0; off < rec.Records();) {
        const size_t count = std::min<size_t>(chunkRecords, rec.Records() - off);
        const size_t written =
            EncodeChunk(rec.RecordData() + off * kRecordSize, count, chunk.data(), chunk.size());
        Check(written == kChunkHeaderSize + count * kRecordSize, "chunk encodes at the specced size");
        stream.insert(stream.end(), chunk.begin(), chunk.begin() + written);
        off += uint32_t(count);
    }
    return stream;
}

// Records `count` ever-changing reads (hold carries the read index) into
// a fresh recorder and returns it by value along with its read total.
Recorder RecordChanging(uint32_t base, uint32_t count, const char* note) {
    Recorder rec;
    rec.Start("v", "t", note, base);
    for (uint32_t i = 0; i < count; ++i) {
        PadSample s;
        s.hold = base + i + 1;
        rec.Observe(s, base + i, base + i);
    }
    return rec;
}

void TestRecordCodec() {
    Record in;
    in.readSeq = 0xDEADBEEFu;
    in.retrace = 123456u;
    in.presents = 789u;
    in.sample = MakeSample(42);
    in.sample.acc[0] = 3.4028235e38f;   // FLT_MAX
    in.sample.acc[1] = 1e-30f;          // subnormal-adjacent small
    uint8_t bytes[kRecordSize];
    EncodeRecord(in, bytes);
    Check(bytes[41] == 0 && bytes[42] == 0 && bytes[43] == 0, "record padding bytes are zero");
    Record out;
    Check(DecodeRecord(bytes, out), "decode of an encoded record succeeds");
    Check(out.readSeq == in.readSeq && out.retrace == in.retrace && out.presents == in.presents,
          "witness fields round-trip");
    Check(out.sample.hold == in.sample.hold && out.sample.clHold == in.sample.clHold &&
              out.sample.pointerValid == in.sample.pointerValid,
          "integer/flag fields round-trip");
    bool floatsOk = true;
    for (int i = 0; i < 3; ++i) floatsOk = floatsOk && FloatBits(out.sample.acc[i]) == FloatBits(in.sample.acc[i]);
    for (int i = 0; i < 2; ++i)
        floatsOk = floatsOk && FloatBits(out.sample.pointer[i]) == FloatBits(in.sample.pointer[i]);
    Check(floatsOk, "float fields round-trip bit-identically");
    Check(!DecodeRecord(nullptr, out), "decode of null is refused");
}

void TestCrc32() {
    // The standard check value for the IEEE CRC-32.
    const char* text = "123456789";
    Check(PadrecCrc32(reinterpret_cast<const uint8_t*>(text), 9) == 0xCBF43926u,
          "crc32 matches the IEEE check value");
    Check(PadrecCrc32(nullptr, 0) == 0, "crc32 of empty input is 0");
    uint8_t byte = 0xFF;
    Check(PadrecCrc32(&byte, 1) != PadrecCrc32(&byte, 0), "crc32 is content-sensitive");
}

void TestStreamRoundTrip() {
    // The sequence: 10 changing reads, a 200-read constant stretch, then
    // 10 pointer/acc-changing reads. Witnesses are arbitrary monotone
    // counters: retrace = read/2, presents = read/3.
    std::vector<PadSample> seq;
    for (uint32_t i = 0; i < 10; ++i) seq.push_back(MakeSample(i));
    const PadSample constant = MakeSample(1000);
    for (int i = 0; i < 200; ++i) seq.push_back(constant);
    for (uint32_t i = 0; i < 10; ++i) seq.push_back(MakeSample(2000 + i));
    Check(seq.size() == 220, "synthesised sequence has 220 reads");

    Recorder rec;
    rec.Start("0.1.2-beta", "0100000000000000", "fixture");
    Check(rec.Recording(), "recorder runs after Start");
    for (uint32_t i = 0; i < seq.size(); ++i) rec.Observe(seq[i], i / 2, i / 3);
    Check(rec.Reads() == 220, "recorder counted every read");
    // Records: 10 changes + the constant stretch's first read + heartbeats
    // at +60/+120/+180 into it + 10 changes = 24. Any other count means
    // change detection or heartbeat cadence is wrong.
    Check(rec.Records() == 24, "record count: change-only plus heartbeats at 60-read cadence");
    Check(rec.FirstRetrace() == 0 && rec.LastRetrace() == 219 / 2, "retrace witnesses span the run");
    Check(rec.FirstPresents() == 0 && rec.LastPresents() == 219 / 3, "present witnesses span the run");

    // The stored record seqs, read straight out of the ring bytes.
    std::vector<uint32_t> seqs;
    for (uint32_t r = 0; r < rec.Records(); ++r) {
        Record record;
        Check(DecodeRecord(rec.RecordData() + r * kRecordSize, record), "stored record decodes");
        seqs.push_back(record.readSeq);
    }
    const uint32_t wantSeqs[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 70, 130, 190,
                                 210, 211, 212, 213, 214, 215, 216, 217, 218, 219};
    Check(seqs.size() == sizeof(wantSeqs) / sizeof(wantSeqs[0]), "stored seq list length");
    bool seqsOk = seqs.size() == sizeof(wantSeqs) / sizeof(wantSeqs[0]);
    for (size_t i = 0; seqsOk && i < seqs.size(); ++i) seqsOk = seqs[i] == wantSeqs[i];
    Check(seqsOk, "heartbeats land at exactly 60-read gaps in the constant stretch, nowhere else");

    // Header text: four lines, first is the v2 magic.
    const std::string& header = rec.Header();
    Check(header.rfind("SPM-PADREC 2\nversion=0.1.2-beta\ntitle=0100000000000000\nnote=fixture\n", 0) == 0,
          "header block is the four specced lines, format v2");

    // Whole-stream round trip through the Player, one chunk per flush.
    const std::vector<uint8_t> stream = BuildV2Stream(rec, 24);
    Player player;
    Check(player.Load(stream.data(), stream.size()), "encoded stream loads");
    Check(player.Version() == "0.1.2-beta" && player.Title() == "0100000000000000" &&
              player.Note() == "fixture",
          "header fields parse back");
    Check(player.RecordCount() == 24, "player sees every record");
    uint32_t mismatches = 0;
    for (uint32_t i = 0; i < seq.size(); ++i) {
        PadSample out;
        if (!player.Next(out) || !SameSample(out, seq[i])) ++mismatches;
    }
    Check(mismatches == 0, "220 reads reconstruct bit-identically");

    // Replay edges: the final sample holds for 60 further reads, then
    // neutral forever, and playback never reports "not armed".
    const PadSample last = seq.back();
    uint32_t edgeBad = 0;
    for (uint32_t i = 0; i < 60; ++i) {
        PadSample out;
        if (!player.Next(out) || !SameSample(out, last)) ++edgeBad;
    }
    Check(edgeBad == 0, "final sample held for exactly 60 reads after the last record");
    const PadSample neutral{};
    edgeBad = 0;
    for (uint32_t i = 0; i < 120; ++i) {
        PadSample out;
        if (!player.Next(out) || !SameSample(out, neutral)) ++edgeBad;
    }
    Check(edgeBad == 0, "neutral input forever after the hold, live input never resumes");
}

void TestChunkedLayout() {
    // 30 changing reads flushed as three chunks of 10: a mid-recording
    // periodic flush looks exactly like this on disk.
    Recorder rec = RecordChanging(0, 30, "chunks");
    Check(rec.Records() == 30, "chunk fixture stored every change");
    const std::vector<uint8_t> stream = BuildV2Stream(rec, 10);
    Player player;
    Check(player.Load(stream.data(), stream.size()), "multi-chunk stream loads");
    Check(player.RecordCount() == 30, "multi-chunk stream yields every record");

    const size_t headerSize = rec.Header().size();
    const size_t chunkBytes = kChunkHeaderSize + 10 * kRecordSize;

    // A corrupted CRC in the second chunk: the loader keeps chunk 1 and
    // drops chunks 2 and 3 -- the corrupt-tail contract a panic mid-write
    // relies on.
    std::vector<uint8_t> badCrc = stream;
    badCrc[headerSize + chunkBytes + 8] ^= 0xFF;
    Check(player.Load(badCrc.data(), badCrc.size()), "corrupt second chunk still loads the prefix");
    Check(player.RecordCount() == 10, "corrupt chunk drops itself and the tail, prefix kept");

    // A chunk body torn mid-record (process died mid-flush): same prefix.
    std::vector<uint8_t> torn = stream;
    torn.resize(headerSize + chunkBytes + kChunkHeaderSize + 5 * kRecordSize + 17);
    Check(player.Load(torn.data(), torn.size()), "torn final chunk still loads the prefix");
    Check(player.RecordCount() == 10, "torn chunk contributes nothing, prefix kept");

    // A seq regression inside chunk 2 (payload rewritten, CRC recomputed
    // to match): monotonicity is checked per chunk, so the chunk is
    // dropped whole even though its CRC is now self-consistent.
    std::vector<uint8_t> regressed = stream;
    Record dup;
    DecodeRecord(regressed.data() + headerSize + chunkBytes + kChunkHeaderSize, dup);
    dup.readSeq = 0;
    EncodeRecord(dup, regressed.data() + headerSize + chunkBytes + kChunkHeaderSize);
    const uint32_t crc = PadrecCrc32(regressed.data() + headerSize + chunkBytes + kChunkHeaderSize,
                                     10 * kRecordSize);
    std::memcpy(regressed.data() + headerSize + chunkBytes + 8, &crc, 4);
    Check(player.Load(regressed.data(), regressed.size()), "seq-regressed chunk still loads the prefix");
    Check(player.RecordCount() == 10, "seq-regressed chunk dropped whole despite a valid CRC");

    // Garbage where the first chunk should be: not one chunk validates,
    // so the stream is refused whole and replay stays disarmed.
    std::vector<uint8_t> garbage(rec.Header().begin(), rec.Header().end());
    garbage.insert(garbage.end(), 64, 0xAB);
    Check(!player.Load(garbage.data(), garbage.size()), "v2 body with no valid chunk is refused");
    Check(!player.Loaded(), "a refused v2 load leaves the player unloaded");

    // Header only (a recording stopped before its first flush): loads as
    // an empty stream.
    const std::vector<uint8_t> headerOnly(rec.Header().begin(), rec.Header().end());
    Check(player.Load(headerOnly.data(), headerOnly.size()), "header-only stream loads");
    Check(player.RecordCount() == 0, "header-only stream has no records");
    PadSample out;
    Check(player.Next(out) && SameSample(out, PadSample{}), "an empty stream serves neutral input");
}

void TestLegacyV1() {
    // A v1 stream: the v1 header plus unchunked records, as the original
    // writer produced. It still loads; it still refuses on any defect.
    Recorder rec = RecordChanging(0, 3, "legacy");
    std::string header = rec.Header();
    header.replace(11, 1, "1");  // "SPM-PADREC 2" -> "SPM-PADREC 1"
    std::vector<uint8_t> stream(header.begin(), header.end());
    stream.insert(stream.end(), rec.RecordData(), rec.RecordData() + rec.RecordBytes());

    Player player;
    Check(player.Load(stream.data(), stream.size()), "legacy v1 stream loads");
    Check(player.RecordCount() == 3, "legacy v1 stream yields its records");

    Check(!player.Load(stream.data(), stream.size() - 1), "truncated v1 record refused");
    std::vector<uint8_t> dupSeq = stream;
    Record dup;
    DecodeRecord(dupSeq.data() + header.size() + kRecordSize, dup);
    dup.readSeq = 0;
    EncodeRecord(dup, dupSeq.data() + header.size() + kRecordSize);
    Check(!player.Load(dupSeq.data(), dupSeq.size()), "v1 non-increasing read_seq refused");
}

void TestSegments() {
    // Segment 0 covers reads 0..49 (every read changes, so 50 records);
    // its continuation starts at read base 50.
    Recorder seg0 = RecordChanging(0, 50, "seg0");
    Recorder seg1 = RecordChanging(50, 50, "continuation");
    const std::vector<uint8_t> stream0 = BuildV2Stream(seg0, 4096);
    const std::vector<uint8_t> stream1 = BuildV2Stream(seg1, 4096);

    Player player;
    Check(player.Load(stream0.data(), stream0.size()), "segment 0 loads");
    Check(player.Append(stream1.data(), stream1.size()), "continuation segment joins");
    Check(player.RecordCount() == 100, "joined stream holds both segments' records");
    uint32_t mismatches = 0;
    for (uint32_t i = 0; i < 100; ++i) {
        PadSample out;
        if (!player.Next(out) || out.hold != i + 1) ++mismatches;
    }
    Check(mismatches == 0, "joined stream serves the global read sequence in order");

    // Joining the same segment twice: its run is already covered.
    Check(!player.Append(stream1.data(), stream1.size()), "re-joining a covered segment is refused");
    // A segment from a different run whose seqs leap past the join window
    // (first record more than a heartbeat gap beyond the tail) is not a
    // continuation of this stream.
    Recorder foreign = RecordChanging(500, 10, "foreign");
    const std::vector<uint8_t> foreignStream = BuildV2Stream(foreign, 4096);
    Check(!player.Append(foreignStream.data(), foreignStream.size()),
          "a segment leaping past the heartbeat window is refused");
    // An empty continuation (header only) joins as a no-op.
    Recorder empty = RecordChanging(100, 0, "continuation");
    const std::vector<uint8_t> emptyStream = BuildV2Stream(empty, 4096);
    Check(player.Append(emptyStream.data(), emptyStream.size()), "an empty continuation joins as a no-op");
    Check(player.RecordCount() == 100, "an empty continuation adds nothing");
}

void TestCorruptStreams() {
    Recorder rec = RecordChanging(0, 2, "n");
    const std::vector<uint8_t> stream = BuildV2Stream(rec, 4096);

    Player player;
    Check(!player.Load(nullptr, 0), "empty stream refused");
    std::vector<uint8_t> badMagic = stream;
    badMagic[0] = 'X';
    Check(!player.Load(badMagic.data(), badMagic.size()), "bad magic refused");
    std::vector<uint8_t> badVersion = stream;
    badVersion[11] = '3';  // "SPM-PADREC 3"
    Check(!player.Load(badVersion.data(), badVersion.size()), "unknown format version refused");
    Check(!player.Loaded(), "a refused load leaves the player unloaded");
    PadSample out;
    Check(!player.Next(out), "an unloaded player serves nothing");
}

void TestRingFull() {
    Recorder rec;
    rec.Start("v", "t", "ring");
    int fullAt = -1;
    // Every read changes (hold carries the index), so every read stores.
    for (uint32_t i = 0; i < kRingRecords + 10; ++i) {
        PadSample s;
        s.hold = i + 1;
        if (rec.Observe(s, i, i) == 2) {
            fullAt = int(i);
            break;
        }
    }
    Check(fullAt == int(kRingRecords) - 1, "ring reports full exactly when it holds kRingRecords records");
    Check(rec.Records() == kRingRecords, "ring holds exactly its capacity, no overwrite");
    PadSample s;
    s.hold = 0xFFFFu;
    Check(rec.Observe(s, 0, 0) == 2, "a full ring keeps refusing (and reporting) instead of wrapping");
    Check(rec.Records() == kRingRecords, "refused records are not stored over old ones");
    rec.Start("v", "t", "next segment");
    Check(rec.Records() == 0 && rec.Observe(s, 0, 0) == 1, "a restarted recorder stores again");
}

void TestHeaderSanitising() {
    Recorder rec;
    rec.Start("v\nversion=forged", "t", "line1\nline2");
    const std::string& header = rec.Header();
    uint32_t newlines = 0;
    for (char c : header)
        if (c == '\n') ++newlines;
    Check(newlines == 4, "header stays four lines even when fields carry newlines");
    const std::vector<uint8_t> stream = BuildV2Stream(rec, 4096);
    Player player;
    Check(player.Load(stream.data(), stream.size()), "sanitised header still parses");
    Check(player.Version().find('\n') == std::string::npos, "parsed version carries no newline");
}

}  // namespace

int main() {
    TestRecordCodec();
    TestCrc32();
    TestStreamRoundTrip();
    TestChunkedLayout();
    TestLegacyV1();
    TestSegments();
    TestCorruptStreams();
    TestRingFull();
    TestHeaderSanitising();
    std::printf("input_recorder_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
