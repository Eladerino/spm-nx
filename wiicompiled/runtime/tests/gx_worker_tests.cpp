// Host tests for the GX worker's record ring (include/gx_worker.h, src/hle/gx/gx_worker.cpp): the
// order records replay in, that a self-deferred call replays the bytes it saw at the call, what
// each mode does, that a drain reached from inside a replay does nothing, that a full ring drains
// rather than drops, that small records coalesce, and that a Deferred failure is contained.

#include "gx_worker.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

bool g_blocked = false;
std::vector<std::string> g_log;
int g_failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

void SetMode(GxWorker::Mode mode) {
    GxWorker::RequestMode(mode);
    GxWorker::FrameBoundary();
}

void Setter(uint32_t a, uint32_t b) { g_log.push_back("set " + std::to_string(a) + " " + std::to_string(b)); }

void Thrower(uint32_t a) {
    g_log.push_back("throw " + std::to_string(a));
    throw std::runtime_error("test");
}

void Getter() {
    // An Inline call: what the inline dispatch does before running one.
    GxWorker::Drain();
    g_log.push_back("get");
}

void ColorFrom(const uint8_t* bytes, uint32_t id) {
    g_log.push_back("color " + std::to_string(id) + " " + std::to_string(bytes[0]) + "," + std::to_string(bytes[1]) +
                    "," + std::to_string(bytes[2]) + "," + std::to_string(bytes[3]));
}

void SetColor(uint8_t* guest, uint32_t id) {
    // A self-deferring body as the GX overrides have it, armed the way the dispatch arms it.
    GxWorker::SelfDeferScope armed;
    GxWorker::DeferOrRun<&ColorFrom>(guest, 4, id);
}

void NestedDrainer(uint32_t a) {
    g_log.push_back("outer " + std::to_string(a));
    GxWorker::Drain();  // a sync point reached inside a replay
    g_log.push_back("outer done " + std::to_string(a));
}

struct Run {
    uint32_t count;
    uint32_t values[1];
};

void ReplayRun(void* payload) {
    const uint32_t* words = static_cast<const uint32_t*>(payload);
    std::string line = "run";
    for (uint32_t i = 0; i < words[0]; ++i) {
        line += " " + std::to_string(words[1 + i]);
    }
    g_log.push_back(line);
}

void RecordRunValue(uint32_t value) {
    void* extra = nullptr;
    if (void* payload = GxWorker::ExtendLast(&ReplayRun, sizeof(uint32_t), &extra)) {
        std::memcpy(extra, &value, sizeof(value));
        ++static_cast<uint32_t*>(payload)[0];
        return;
    }
    void* payload = GxWorker::Reserve(&ReplayRun, 2 * sizeof(uint32_t), alignof(uint32_t));
    uint32_t words[2] = {1, value};
    std::memcpy(payload, words, sizeof(words));
    GxWorker::Commit();
}

bool Same(const std::vector<std::string>& want) {
    if (g_log == want) {
        return true;
    }
    std::fprintf(stderr, "  got:");
    for (const auto& s : g_log) std::fprintf(stderr, " [%s]", s.c_str());
    std::fprintf(stderr, "\n  want:");
    for (const auto& s : want) std::fprintf(stderr, " [%s]", s.c_str());
    std::fprintf(stderr, "\n");
    return false;
}

void TestOffRecordsNothing() {
    SetMode(GxWorker::Mode::Off);
    Check(!GxWorker::Engaged(), "off: not engaged");
    Check(!GxWorker::CanRecord(), "off: cannot record");
}

void TestDeferredOrder() {
    SetMode(GxWorker::Mode::Deferred);
    g_log.clear();
    Check(GxWorker::CanRecord(), "deferred: can record");
    Check(GxWorker::RecordCall<&Setter>(1u, 2u), "deferred: record 1");
    uint8_t guest[4] = {10, 20, 30, 40};
    SetColor(guest, 7);
    Check(GxWorker::RecordCall<&Setter>(3u, 4u), "deferred: record 2");
    Check(g_log.empty(), "deferred: nothing ran before a sync point");
    // The guest overwrites the colour before the replay: the record keeps what the call saw.
    guest[0] = 99;
    Getter();
    Check(Same({"set 1 2", "color 7 10,20,30,40", "set 3 4", "get"}), "deferred: order and captured bytes");
}

void TestImmediateRunsAtOnce() {
    SetMode(GxWorker::Mode::Immediate);
    g_log.clear();
    Check(GxWorker::RecordCall<&Setter>(5u, 6u), "immediate: record");
    Check(Same({"set 5 6"}), "immediate: ran at the call");
    uint8_t guest[4] = {1, 2, 3, 4};
    SetColor(guest, 0);
    Check(Same({"set 5 6", "color 0 1,2,3,4"}), "immediate: self-deferred ran at the call");
}

void TestImmediateThrowsThrough() {
    SetMode(GxWorker::Mode::Immediate);
    g_log.clear();
    bool caught = false;
    try {
        GxWorker::RecordCall<&Thrower>(1u);
    } catch (const std::runtime_error&) {
        caught = true;
    }
    Check(caught, "immediate: a replay's exception reaches the caller");
    Check(GxWorker::RecordCall<&Setter>(1u, 1u), "immediate: ring usable after a throw");
    Check(Same({"throw 1", "set 1 1"}), "immediate: nothing replayed twice after a throw");
}

void TestDeferredContainsThrow() {
    SetMode(GxWorker::Mode::Deferred);
    g_log.clear();
    GxWorker::RecordCall<&Setter>(1u, 0u);
    GxWorker::RecordCall<&Thrower>(2u);
    GxWorker::RecordCall<&Setter>(3u, 0u);
    GxWorker::Drain();
    Check(Same({"set 1 0", "throw 2", "set 3 0"}), "deferred: a throw is logged and the rest replay");
}

void TestNestedDrain() {
    SetMode(GxWorker::Mode::Deferred);
    g_log.clear();
    GxWorker::RecordCall<&NestedDrainer>(1u);
    GxWorker::RecordCall<&Setter>(2u, 0u);
    Check(GxWorker::CanRecord(), "nested: can record outside a replay");
    GxWorker::Drain();
    Check(Same({"outer 1", "outer done 1", "set 2 0"}), "nested: a drain inside a replay is a no-op");
}

void TestRingFullDrains() {
    SetMode(GxWorker::Mode::Deferred);
    g_log.clear();
    // 16-byte header plus 8 bytes of tuple, rounded to 32: 2 MiB holds 65536 of them.
    const uint32_t count = 70000;
    for (uint32_t i = 0; i < count; ++i) {
        if (!GxWorker::RecordCall<&Setter>(i, 0u)) {
            Check(false, "ring full: a record was refused");
            break;
        }
    }
    GxWorker::Drain();
    bool ordered = g_log.size() == count;
    for (uint32_t i = 0; ordered && i < count; ++i) {
        ordered = g_log[i] == "set " + std::to_string(i) + " 0";
    }
    Check(ordered, "ring full: every record replayed once, in order");
}

void TestExtendLast() {
    SetMode(GxWorker::Mode::Deferred);
    g_log.clear();
    RecordRunValue(1);
    RecordRunValue(2);
    RecordRunValue(3);
    GxWorker::RecordCall<&Setter>(9u, 9u);
    RecordRunValue(4);
    GxWorker::Drain();
    Check(Same({"run 1 2 3", "set 9 9", "run 4"}), "extend: consecutive small records share one");
}

void TestBlocked() {
    SetMode(GxWorker::Mode::Deferred);
    g_blocked = true;
    Check(!GxWorker::CanRecord(), "blocked: no records while a display list is built");
    g_blocked = false;
    Check(GxWorker::CanRecord(), "blocked: records again after it");
}

void TestFrameBoundaryDrains() {
    SetMode(GxWorker::Mode::Deferred);
    g_log.clear();
    GxWorker::RecordCall<&Setter>(1u, 1u);
    GxWorker::RequestMode(GxWorker::Mode::Off);
    GxWorker::FrameBoundary();
    Check(Same({"set 1 1"}), "boundary: pending records run before the mode changes");
    Check(!GxWorker::Engaged(), "boundary: off after the boundary");
}

const char* TestName(uint32_t address) { return address == 0x80001234u ? "GXGetTest" : nullptr; }

void TestDrainCausesPrint() {
    SetMode(GxWorker::Mode::Deferred);
    GxWorker::PrintPerfWindow(1, 1, &TestName);  // clears what earlier tests counted
    for (int i = 0; i < 3; ++i) {
        GxWorker::RecordCall<&Setter>(1u, 1u);
        GxWorker::Drain(0x80001234u);
    }
    GxWorker::RecordCall<&Setter>(1u, 1u);
    GxWorker::Drain(0x80005678u);
    GxWorker::Drain(0x80005678u);  // nothing pending: not counted
    std::fprintf(stderr, "expect: GXGetTest 3.0, 80005678 1.0\n");
    GxWorker::PrintPerfWindow(1, 1, &TestName);
}

} // namespace

bool GxWorkerRecordingBlocked() noexcept { return g_blocked; }

int main() {
    TestOffRecordsNothing();
    TestDeferredOrder();
    TestImmediateRunsAtOnce();
    TestImmediateThrowsThrough();
    TestDeferredContainsThrow();
    TestNestedDrain();
    TestRingFullDrains();
    TestExtendLast();
    TestBlocked();
    TestFrameBoundaryDrains();
    TestDrainCausesPrint();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failures\n", g_failures);
        return 1;
    }
    std::printf("gx_worker_tests: all passed\n");
    return 0;
}
