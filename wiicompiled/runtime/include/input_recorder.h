#pragma once

// Grade 1 KPAD input recorder / replayer (scope: spm-perf-scope/
// SCOPE-INPUT-RECORDER-2026-11-05.md). The guest is handed its input by
// kpad::ReadSample() (hle/input/kpad.cpp); this module records the chan-0
// samples that function returns, and can later substitute a recorded stream
// for live sampling, read for read: the Nth chan-0 read gets the Nth
// recorded answer. Nothing here promises the guest then walks the same
// path (guest-visible wall time still differs run to run); the retrace and
// present witnesses in every record are what lets a comparison say where
// two runs diverged.
//
// Two layers, deliberately separate:
//   - the format layer (this header's Recorder / Player / record codec) is
//     plain C++ with no platform calls, so the host fixtures
//     (tests/input_recorder_tests.cpp) exercise the real code;
//   - the Switch glue at the bottom (the InputRecorder namespace and the
//     extern "C" diag handshakes) owns the diag switches, the file under
//     UserData/Logs/ and the stderr lines. It never writes the SD card
//     mid-run: samples accumulate in a preallocated 8 MB RAM ring and are
//     flushed once at stop/exit, plus a safety flush if the ring fills.
//
// Stream format (binary, little-endian):
//   header (text block, four \n-terminated lines):
//     SPM-PADREC 1
//     version=<baked version>
//     title=<title id>
//     note=<free text>
//   record (44 bytes, emitted when the sample differs from the previous
//   read, plus a heartbeat at least every 60 reads):
//     u32 read_seq   u32 retrace   u32 presents
//     u32 hold       u32 cl_hold
//     f32 acc[3]     f32 pointer[2]   u8 pointer_valid  (+3 pad)
// The scope estimates "~36 bytes/record"; its own field list encodes to
// 44 bytes, which is what kRecordSize says and what the fixtures assert.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace WiiRemoteInput {
struct KpadSample;
}

namespace InputRecorder {

// The recorded slice of a KpadSample: exactly the fields the stream format
// carries, in format order after the three witness stamps.
struct PadSample {
    uint32_t hold = 0;
    uint32_t clHold = 0;
    float acc[3] = {};
    float pointer[2] = {};
    bool pointerValid = false;
};

// Bitwise equality (floats compared by their bits, so -0.0f differs from
// 0.0f): change detection and the fixtures both want "the same sample the
// guest was handed", not "numerically close".
bool SamplesEqual(const PadSample& a, const PadSample& b);

struct Record {
    uint32_t readSeq = 0;
    uint32_t retrace = 0;
    uint32_t presents = 0;
    PadSample sample;
};

constexpr size_t kRecordSize = 44;
constexpr uint32_t kHeartbeatReads = 60;  // a heartbeat at least every 60 reads
constexpr size_t kRingBytes = 8u << 20;   // preallocated RAM ring
constexpr size_t kRingRecords = kRingBytes / kRecordSize;

void EncodeRecord(const Record& record, uint8_t out[kRecordSize]);
bool DecodeRecord(const uint8_t* in, Record& out);  // `in` holds kRecordSize bytes

// Change-only-plus-heartbeat recorder over the RAM ring. The ring is
// allocated by Start() and never grows; Observe() does no allocation, no
// formatting and no I/O in steady state.
class Recorder {
public:
    void Start(const char* version, const char* title, const char* note);
    void Stop() { recording_ = false; }
    bool Recording() const { return recording_; }

    // Feeds one read. Returns 0 when nothing needs doing, 1 when a record
    // was stored, and 2 when a record was stored and the ring is now full
    // (the caller safety-flushes and restarts the recorder as the next
    // segment; records offered while the ring stays full are refused and
    // Observe keeps returning 2 without storing).
    int Observe(const PadSample& sample, uint32_t retrace, uint32_t presents);

    uint32_t Reads() const { return reads_; }
    uint32_t Records() const { return records_; }
    uint32_t FirstRetrace() const { return firstRetrace_; }
    uint32_t LastRetrace() const { return lastRetrace_; }
    uint32_t FirstPresents() const { return firstPresents_; }
    uint32_t LastPresents() const { return lastPresents_; }

    const std::string& Header() const { return header_; }
    const uint8_t* RecordData() const { return ring_.data(); }
    size_t RecordBytes() const { return size_t(records_) * kRecordSize; }

private:
    std::string header_;
    std::vector<uint8_t> ring_;
    PadSample prev_{};
    bool recording_ = false;
    bool havePrev_ = false;
    uint32_t reads_ = 0;
    uint32_t records_ = 0;
    uint32_t lastStoredSeq_ = 0;
    uint32_t firstRetrace_ = 0, lastRetrace_ = 0;
    uint32_t firstPresents_ = 0, lastPresents_ = 0;
};

// A loaded stream, served one read at a time. Load() validates the header
// and the record block; anything malformed is refused whole, so a corrupt
// file can never produce a half-live run.
class Player {
public:
    bool Load(const uint8_t* data, size_t size);
    void Unload();
    bool Loaded() const { return loaded_; }

    const std::string& Version() const { return version_; }
    const std::string& Title() const { return title_; }
    const std::string& Note() const { return note_; }
    size_t RecordCount() const { return records_.size(); }

    // The sample for the next read. Sparse records reconstruct by holding
    // the latest record whose read_seq has been reached. After the last
    // record, the final sample is held for kHeartbeatReads further reads,
    // then neutral input forever; live input never resumes mid-run.
    // Returns false only when nothing is loaded.
    bool Next(PadSample& out);

private:
    std::string version_, title_, note_;
    std::vector<Record> records_;
    bool loaded_ = false;
    size_t next_ = 0;         // first record not yet applied
    uint32_t served_ = 0;     // reads served so far (the next read's index)
    PadSample current_{};
    bool haveCurrent_ = false;
};

// ---- Switch glue (input_recorder.cpp) ------------------------------------
// Fast paths for kpad.cpp's ReadSample()/KPAD__Read_HLE: one atomic load
// each while the corresponding switch is off.
bool ReplayActive();
bool RecordActive();
// The next replayed sample for chan 0, converted to a KpadSample. False
// when replay is not (or no longer) armed.
bool ReplayNext(WiiRemoteInput::KpadSample& out);
// Offers one chan-0 sample ReadSample returned, with its witnesses.
void RecordSample(const WiiRemoteInput::KpadSample& sample, uint32_t retrace, uint32_t presents);

}  // namespace InputRecorder

// The diag handshakes (switch/deko/device.cpp forwards the "padrec" and
// "padreplay" switches through these, from diag_init and diag_set, the same
// way as spm_wii_suppress_set / spm_pose_log_set).
extern "C" void spm_input_record_set(bool enabled);
// Arms replay from a file under UserData/Logs/ (nullptr disarms; an empty
// string names the default, pad-record.bin), or disarms with nullptr.
extern "C" void spm_input_replay_set(const char* filename);
