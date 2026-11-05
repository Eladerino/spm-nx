// Host harness for lib/gx/vcache_core.hpp: the vcache eligibility scan and bounded
// store, compiled verbatim from the header the Switch build uses. Not part of the
// gtest suite (which needs the fetched Dawn/absl stack); this one builds with any
// host C++17 compiler:
//
//   g++ -std=c++17 -Wall -Wextra -I lib/gx tests/vcache_core_test.cpp -o /tmp/vcache_core_test
//
// Exit code 0 and "vcache_core_test: all checks passed" on success.
#include "vcache_core.hpp"

#include <cstdio>
#include <cstring>
#include <string>

using namespace aurora::gx::vcache;

static int g_failures = 0;
#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
      ++g_failures;                                                                 \
    }                                                                               \
  } while (0)

// Vertex sizes per format for the fake state the scan tests run under.
static uint32_t FakeVtxSize(uint8_t fmt, void* ctx) {
  const auto* sizes = static_cast<const uint32_t*>(ctx);
  return sizes[fmt & 7];
}

static ScanResult Scan(const uint8_t* data, uint32_t size, bool be, const uint32_t* sizes) {
  ScanEnv env{};
  env.vtxSizeForFmt = &FakeVtxSize;
  env.ctx = const_cast<uint32_t*>(sizes);
  return ScanList(data, size, be, env);
}

static void TestScan() {
  // fmt0: 12 bytes/vertex, fmt1: 8 bytes/vertex, the rest 0.
  const uint32_t sizes[8] = {12, 8, 0, 0, 0, 0, 0, 0};

  { // NOPs around one big-endian TRIANGLES draw of 3 vertices in fmt0.
    const uint8_t list[] = {0x00, 0x00, 0x90, 0x00, 0x03, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
                            13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
                            30, 31, 32, 33, 34, 35, 36};
    const auto r = Scan(list, sizeof(list), true, sizes);
    CHECK(r.reject == ScanReject::None);
    CHECK(r.draws.size() == 1);
    CHECK(r.draws[0].opcode == 0x90 && r.draws[0].fmt == 0 && r.draws[0].vtxCount == 3);
    CHECK(r.draws[0].vtxSize == 12 && r.draws[0].payloadOffset == 5);
  }
  { // Little-endian count field, two draws, second in fmt1.
    const uint8_t list[] = {0x98, 0x04, 0x00, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                            16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
                            33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48,
                            0x91, 0x02, 0x00, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
    const auto r = Scan(list, sizeof(list), false, sizes);
    CHECK(r.reject == ScanReject::None);
    CHECK(r.draws.size() == 2);
    CHECK(r.draws[0].vtxCount == 4 && r.draws[0].vtxSize == 12);
    CHECK(r.draws[1].fmt == 1 && r.draws[1].vtxCount == 2 && r.draws[1].vtxSize == 8);
    CHECK(r.draws[1].payloadOffset == 54);
  }
  { // A BP write anywhere rejects the whole list as state-changing.
    const uint8_t list[] = {0x90, 0x00, 0x03, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                            16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
                            33, 34, 35, 36, 0x61, 0x00, 0x00, 0x00, 0x00};
    CHECK(Scan(list, sizeof(list), true, sizes).reject == ScanReject::StateOpcode);
  }
  { // CALL_DL and INVL_VC are state opcodes too, not draws to cache around.
    const uint8_t call[] = {0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    CHECK(Scan(call, sizeof(call), true, sizes).reject == ScanReject::StateOpcode);
    const uint8_t invl[] = {0x48, 0x00, 0x00};
    CHECK(Scan(invl, sizeof(invl), true, sizes).reject == ScanReject::StateOpcode);
    const uint8_t indx[] = {0x20, 0x00, 0x00, 0x00, 0x00, 0x00};
    CHECK(Scan(indx, sizeof(indx), true, sizes).reject == ScanReject::StateOpcode);
  }
  { // Unknown opcodes: a stray byte, and a masked draw opcode that is not a primitive.
    const uint8_t stray[] = {0x07, 0x00};
    CHECK(Scan(stray, sizeof(stray), true, sizes).reject == ScanReject::UnknownOpcode);
    const uint8_t badDraw[] = {0x88, 0x00, 0x00};
    CHECK(Scan(badDraw, sizeof(badDraw), true, sizes).reject == ScanReject::UnknownOpcode);
  }
  { // Overruns: payload past the end, and a count field cut off.
    const uint8_t shortPayload[] = {0x90, 0x00, 0x03, 1, 2, 3};
    CHECK(Scan(shortPayload, sizeof(shortPayload), true, sizes).reject == ScanReject::Overrun);
    const uint8_t shortCount[] = {0x90, 0x00};
    CHECK(Scan(shortCount, sizeof(shortCount), true, sizes).reject == ScanReject::Overrun);
  }
  { // NOPs only, and the empty list.
    const uint8_t nops[] = {0x00, 0x00, 0x00};
    CHECK(Scan(nops, sizeof(nops), true, sizes).reject == ScanReject::NoDraws);
    CHECK(Scan(nops, 0, true, sizes).reject == ScanReject::NoDraws);
  }
  { // A zero-count draw is legal (the real path skips it) and does not stop the walk.
    // The second draw uses fmt2, whose fake vertex size is 0, so its payload is empty.
    const uint8_t list[] = {0x90, 0x00, 0x00, 0xBA, 0x00, 0x01};
    const auto r = Scan(list, sizeof(list), true, sizes);
    CHECK(r.reject == ScanReject::None);
    CHECK(r.draws.size() == 2 && r.draws[1].opcode == 0xBA && r.draws[1].vtxCount == 1);
  }
  { // Every primitive opcode is accepted.
    const uint8_t prims[] = {0x80, 0x90, 0x98, 0xA0, 0xA8, 0xB0, 0xB8};
    for (const uint8_t p : prims) {
      const uint8_t list[] = {p, 0x00, 0x00};
      CHECK(Scan(list, sizeof(list), true, sizes).reject == ScanReject::None);
    }
  }
}

struct TestEntry {
  int id = 0;
};

static Key MakeKey(uint64_t list, uint64_t digest) {
  Key k;
  k.listHash = list;
  k.stateDigest = digest;
  k.nbytes = 100;
  k.bigEndian = true;
  return k;
}

static void TestStore() {
  Store<TestEntry> s;
  CHECK(s.Size() == 0 && s.Bytes() == 0);

  CHECK(s.Insert(MakeKey(1, 10), TestEntry{1}, 100));
  CHECK(s.Insert(MakeKey(1, 11), TestEntry{2}, 200)); // same content, another state
  CHECK(s.Insert(MakeKey(2, 10), TestEntry{3}, 300));
  CHECK(s.Size() == 3 && s.Bytes() == 600);
  CHECK(s.HasContent(ContentKey(MakeKey(1, 999))));
  CHECK(!s.HasContent(ContentKey(MakeKey(3, 10))));

  TestEntry* e = s.Find(MakeKey(1, 11));
  CHECK(e != nullptr && e->id == 2);
  CHECK(s.Find(MakeKey(1, 12)) == nullptr); // digest miss
  CHECK(s.Peek(MakeKey(2, 10)) != nullptr && s.Peek(MakeKey(2, 10))->id == 3);

  // Replacing a key swaps its byte accounting.
  CHECK(s.Insert(MakeKey(2, 10), TestEntry{4}, 50));
  CHECK(s.Size() == 3 && s.Bytes() == 350);
  CHECK(s.Find(MakeKey(2, 10))->id == 4);

  // Erase removes the content index entry once the last variant goes.
  CHECK(s.Erase(MakeKey(1, 10)));
  CHECK(s.HasContent(ContentKey(MakeKey(1, 10))));
  CHECK(s.Erase(MakeKey(1, 11)));
  CHECK(!s.HasContent(ContentKey(MakeKey(1, 10))));
  CHECK(!s.Erase(MakeKey(1, 11)));

  { // Byte-budget eviction: two 10 MiB entries cannot coexist in a 16 MiB store.
    Store<TestEntry> big;
    CHECK(big.Insert(MakeKey(1, 1), TestEntry{1}, 10ull * 1024 * 1024));
    CHECK(big.Insert(MakeKey(2, 1), TestEntry{2}, 10ull * 1024 * 1024));
    CHECK(big.Size() == 1 && big.Bytes() == 10ull * 1024 * 1024);
    CHECK(big.Evictions() == 1);
    CHECK(big.Find(MakeKey(1, 1)) == nullptr); // the older one went
    CHECK(big.Find(MakeKey(2, 1)) != nullptr);
    // An entry bigger than the whole budget is refused, evicting nothing.
    CHECK(!big.Insert(MakeKey(3, 1), TestEntry{3}, 17ull * 1024 * 1024));
    CHECK(big.Size() == 1 && big.Evictions() == 1);
  }
  { // LRU order: touching the older entry protects it from eviction.
    Store<TestEntry> lru;
    for (uint64_t i = 0; i < Store<TestEntry>::kMaxEntries; ++i) {
      CHECK(lru.Insert(MakeKey(i, 1), TestEntry{static_cast<int>(i)}, 1));
    }
    CHECK(lru.Size() == Store<TestEntry>::kMaxEntries);
    CHECK(lru.Find(MakeKey(0, 1)) != nullptr); // key 0 now most-recent
    CHECK(lru.Insert(MakeKey(999999, 1), TestEntry{-1}, 1));
    CHECK(lru.Size() == Store<TestEntry>::kMaxEntries);
    CHECK(lru.Find(MakeKey(0, 1)) != nullptr);  // survived
    CHECK(lru.Find(MakeKey(1, 1)) == nullptr);  // least recent went instead
  }
  { // Backoff: the streak threshold backs a content key off for the call window.
    Store<TestEntry> b;
    const ContentKey ck(MakeKey(7, 1));
    uint64_t now = 1000;
    for (uint32_t i = 0; i < Store<TestEntry>::kBackoffStreak - 1; ++i) {
      b.NoteDigestMiss(ck, now);
      CHECK(!b.BackedOff(ck, now));
    }
    b.NoteDigestMiss(ck, now);
    CHECK(b.BackedOff(ck, now));
    CHECK(b.BackedOff(ck, now + Store<TestEntry>::kBackoffCalls - 1));
    CHECK(!b.BackedOff(ck, now + Store<TestEntry>::kBackoffCalls));
    // A served hit clears the streak, so the count restarts.
    now += Store<TestEntry>::kBackoffCalls;
    for (uint32_t i = 0; i < Store<TestEntry>::kBackoffStreak - 1; ++i) b.NoteDigestMiss(ck, now);
    b.NoteContentServed(ck);
    b.NoteDigestMiss(ck, now);
    CHECK(!b.BackedOff(ck, now));
  }
}

int main() {
  TestScan();
  TestStore();
  if (g_failures != 0) {
    std::fprintf(stderr, "vcache_core_test: %d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("vcache_core_test: all checks passed\n");
  return 0;
}
