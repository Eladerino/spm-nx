// Host tests for the Grade 1 input recorder's stream format and ring
// (include/input_recorder.h, src/input_recorder.cpp): the scope's Phase 0
// desk fixtures (spm-perf-scope/SCOPE-INPUT-RECORDER-2026-11-05.md §7).
// A synthesised sample sequence -- pointer/acc changes and a 200-read
// constant stretch -- is encoded through the real Recorder, decoded
// through the real Player, and must reconstruct bit-identically, with
// heartbeats present in the constant stretch at <=60-read gaps and
// change-only records elsewhere. The ring-full signal and the replay
// edge behaviour (hold the final sample for 60 reads, then idle forever)
// are checked against the same real code. Proves the plumbing only.

#include "input_recorder.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
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

void TestRecordCodec() {
    using namespace InputRecorder;
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

void TestStreamRoundTrip() {
    using namespace InputRecorder;
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

    // Header text: four lines, first is the magic.
    const std::string& header = rec.Header();
    Check(header.rfind("SPM-PADREC 1\nversion=0.1.2-beta\ntitle=0100000000000000\nnote=fixture\n", 0) == 0,
          "header block is the four specced lines");

    // Whole-stream round trip through the Player.
    std::vector<uint8_t> stream(header.begin(), header.end());
    stream.insert(stream.end(), rec.RecordData(), rec.RecordData() + rec.RecordBytes());
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

void TestCorruptStreams() {
    using namespace InputRecorder;
    Recorder rec;
    rec.Start("v", "t", "n");
    rec.Observe(MakeSample(1), 0, 0);
    rec.Observe(MakeSample(2), 1, 1);
    std::vector<uint8_t> stream(rec.Header().begin(), rec.Header().end());
    stream.insert(stream.end(), rec.RecordData(), rec.RecordData() + rec.RecordBytes());

    Player player;
    Check(!player.Load(nullptr, 0), "empty stream refused");
    Check(!player.Load(stream.data(), stream.size() - 1), "truncated record refused");
    std::vector<uint8_t> badMagic = stream;
    badMagic[0] = 'X';
    Check(!player.Load(badMagic.data(), badMagic.size()), "bad magic refused");
    std::vector<uint8_t> badOrder = stream;
    badOrder[11] = '2';  // "SPM-PADREC 2"
    Check(!player.Load(badOrder.data(), badOrder.size()), "unknown format version refused");
    // Non-monotone read_seq: rewrite the second record's seq to the first's.
    std::vector<uint8_t> dupSeq = stream;
    Record dup;
    DecodeRecord(dupSeq.data() + rec.Header().size() + kRecordSize, dup);
    dup.readSeq = 0;
    EncodeRecord(dup, dupSeq.data() + rec.Header().size() + kRecordSize);
    Check(!player.Load(dupSeq.data(), dupSeq.size()), "non-increasing read_seq refused");
    Check(!player.Loaded(), "a refused load leaves the player unloaded");
    PadSample out;
    Check(!player.Next(out), "an unloaded player serves nothing");
}

void TestRingFull() {
    using namespace InputRecorder;
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
    using namespace InputRecorder;
    Recorder rec;
    rec.Start("v\nversion=forged", "t", "line1\nline2");
    const std::string& header = rec.Header();
    uint32_t newlines = 0;
    for (char c : header)
        if (c == '\n') ++newlines;
    Check(newlines == 4, "header stays four lines even when fields carry newlines");
    std::vector<uint8_t> stream(header.begin(), header.end());
    stream.insert(stream.end(), rec.RecordData(), rec.RecordData() + rec.RecordBytes());
    Player player;
    Check(player.Load(stream.data(), stream.size()), "sanitised header still parses");
    Check(player.Version().find('\n') == std::string::npos, "parsed version carries no newline");
}

}  // namespace

int main() {
    TestRecordCodec();
    TestStreamRoundTrip();
    TestCorruptStreams();
    TestRingFull();
    TestHeaderSanitising();
    std::printf("input_recorder_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
