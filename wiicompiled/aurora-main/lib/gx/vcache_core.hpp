// The decoded display-list vertex cache ("vcache"), lab piece: pure logic.
//
// dlprobe's hardware verdict (stderr-49, he2_03 2D, ~210 active frames): 515,790 display
// list calls, 99.4% of submitted list bytes exact repeats of a list seen before, 0 changed,
// and decoding those lists costs ~11.7 ms/frame of dlprocess time. This cache is the
// follow-up the probe was measuring for: remember what a list decoded into and replay that
// instead of decoding again. The pattern follows mkdd-nx's vertex cache (digdat0, MIT;
// see the family deep dive): whole-list capture keyed by content, a state signature beside
// the content hash, bounded storage with eviction, a churn backoff, and a verify mode.
//
// This header is the part that does not depend on Aurora state: the eligibility scan (a
// list may only be cached if it is nothing but NOPs and draws, so decoding it cannot
// change the state the key was taken under) and the bounded store (LRU + byte budget +
// digest-miss backoff). command_processor.cpp holds the state-coupled half: the state
// digest, the recording/replay against the staging arenas, and the counters. Keeping the
// scan and the store here lets a host harness compile them verbatim
// (tests/vcache_core_test.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aurora::gx::vcache {

// Why a list may not be cached, as decided by the scan. The glue adds its own reject
// reasons on top (staging split mid-list, a draw merged into a foreign draw, and so on);
// these are the ones decidable from the bytes alone.
enum class ScanReject : uint8_t {
  None = 0,
  StateOpcode,   // BP/CP/XF/INDX/CALL_DL/INVL_VC/AURORA: decoding writes decode state
  UnknownOpcode, // not a NOP and not a known draw command
  Overrun,       // a draw's count field or vertex payload runs past the end of the list
  NoDraws,       // NOPs only: nothing worth caching
};

struct ScanDraw {
  uint8_t opcode = 0;        // the raw draw command byte
  uint8_t fmt = 0;           // opcode & 0x07 (GX_VAT_MASK)
  uint16_t vtxCount = 0;
  uint32_t vtxSize = 0;      // bytes per vertex under the caller's current desc/fmts
  uint32_t payloadOffset = 0; // offset of the vertex payload within the list
};

struct ScanEnv {
  // Bytes per vertex for a vertex format under the caller's current vertex descriptor
  // and attribute formats. Must be a pure function of that state: the scan runs before
  // decoding and may not disturb anything (command_processor's calculate_last_vtx_size
  // writes lastVtxFmt/lastVtxSize/stateDirty, so the glue passes a side-effect-free
  // twin of it).
  uint32_t (*vtxSizeForFmt)(uint8_t fmt, void* ctx) = nullptr;
  void* ctx = nullptr;
};

struct ScanResult {
  ScanReject reject = ScanReject::NoDraws;
  std::vector<ScanDraw> draws;
};

// Walk a display list exactly the way the command processor would: NOPs and draw
// commands only, every draw's vertex payload fully inside the list, the walk ending
// exactly at the end of the buffer. Anything else rejects the list. The walk is a
// pre-check, not a decoder: if its vtxSize ever disagreed with the real decoder's, the
// safe direction is a reject here, and a wrongly accepted list still decodes through
// the real path (the recording is only kept if the real decode's own output checks
// pass), so a disagreement can cost a cache slot, never a wrong picture.
inline ScanResult ScanList(const uint8_t* data, uint32_t size, bool bigEndian, const ScanEnv& env) {
  ScanResult out;
  uint32_t pos = 0;
  while (pos < size) {
    const uint8_t op = data[pos++];
    if (op == 0x00) { // GX_NOP
      continue;
    }
    if (op >= 0x80 && op <= 0xBF) { // draw command range
      switch (op & 0xF8) {          // GX_OPCODE_MASK
      case 0x80:                    // GX_QUADS
      case 0x90:                    // GX_TRIANGLES
      case 0x98:                    // GX_TRIANGLESTRIP
      case 0xA0:                    // GX_TRIANGLEFAN
      case 0xA8:                    // GX_LINES
      case 0xB0:                    // GX_LINESTRIP
      case 0xB8:                    // GX_POINTS
        break;
      default:
        out.reject = ScanReject::UnknownOpcode;
        return out;
      }
      if (pos + 2 > size) {
        out.reject = ScanReject::Overrun;
        return out;
      }
      const uint16_t count = bigEndian ? static_cast<uint16_t>((data[pos] << 8) | data[pos + 1])
                                       : static_cast<uint16_t>(data[pos] | (data[pos + 1] << 8));
      pos += 2;
      const uint8_t fmt = static_cast<uint8_t>(op & 0x07); // GX_VAT_MASK
      const uint32_t vtxSize = env.vtxSizeForFmt(fmt, env.ctx);
      const uint64_t payload = static_cast<uint64_t>(count) * vtxSize;
      if (payload > size - pos) {
        out.reject = ScanReject::Overrun;
        return out;
      }
      out.draws.push_back(ScanDraw{op, fmt, count, vtxSize, pos});
      pos += static_cast<uint32_t>(payload);
      continue;
    }
    // Everything else either writes decode state or is not a command the scan can
    // account for. The classification only feeds the counters.
    switch (op & 0xF8) {
    case 0x08: // GX_LOAD_CP_REG
    case 0x10: // GX_LOAD_XF_REG
    case 0x20: // GX_LOAD_INDX_A
    case 0x28: // GX_LOAD_INDX_B
    case 0x30: // GX_LOAD_INDX_C
    case 0x38: // GX_LOAD_INDX_D
    case 0x40: // GX_CMD_CALL_DL
    case 0x48: // GX_CMD_INVL_VC
    case 0x50: // GX_LOAD_AURORA
    case 0x60: // GX_LOAD_BP_REG (0x61)
      out.reject = ScanReject::StateOpcode;
      return out;
    default:
      out.reject = ScanReject::UnknownOpcode;
      return out;
    }
  }
  if (out.draws.empty()) {
    out.reject = ScanReject::NoDraws;
    return out;
  }
  out.reject = ScanReject::None;
  return out;
}

// The full cache key: what the list is (content hash + length + byte order) and the
// state it decoded under (the glue's digest of everything the decode path reads).
// Address-independent on purpose: the game copies lists around, and keying on content
// means changed bytes are a different key, never a stale hit.
struct Key {
  uint64_t listHash = 0;
  uint64_t stateDigest = 0;
  uint32_t nbytes = 0;
  bool bigEndian = false;
  bool operator==(const Key& rhs) const noexcept {
    return listHash == rhs.listHash && stateDigest == rhs.stateDigest && nbytes == rhs.nbytes &&
           bigEndian == rhs.bigEndian;
  }
};

// The content half of the key: identifies a list regardless of the state it arrived
// under. Variants of one list under different states share a ContentKey.
struct ContentKey {
  uint64_t listHash = 0;
  uint32_t nbytes = 0;
  bool bigEndian = false;
  bool operator==(const ContentKey& rhs) const noexcept {
    return listHash == rhs.listHash && nbytes == rhs.nbytes && bigEndian == rhs.bigEndian;
  }
  ContentKey(const Key& key) noexcept : listHash(key.listHash), nbytes(key.nbytes), bigEndian(key.bigEndian) {}
};

inline uint64_t MixHash(uint64_t h, uint64_t v) noexcept {
  // splitmix64 finalizer step: cheap and well distributed for map bucketing.
  v += 0x9E3779B97F4A7C15ull;
  v = (v ^ (v >> 30)) * 0xBF58476D1CE4E5B9ull;
  v = (v ^ (v >> 27)) * 0x94D049BB133111EBull;
  v = v ^ (v >> 31);
  return h ^ (v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2));
}

struct KeyHash {
  size_t operator()(const Key& k) const noexcept {
    uint64_t h = MixHash(0, k.listHash);
    h = MixHash(h, k.stateDigest);
    h = MixHash(h, (static_cast<uint64_t>(k.nbytes) << 1) | (k.bigEndian ? 1u : 0u));
    return static_cast<size_t>(h);
  }
};
struct ContentKeyHash {
  size_t operator()(const ContentKey& k) const noexcept {
    uint64_t h = MixHash(0, k.listHash);
    h = MixHash(h, (static_cast<uint64_t>(k.nbytes) << 1) | (k.bigEndian ? 1u : 0u));
    return static_cast<size_t>(h);
  }
};

// The bounded store. LRU by last use across all entries; a byte budget over the stored
// segment bytes; and a per-content backoff: a list whose state churns (digest miss after
// digest miss) stops being looked up for a while, because the digest computation is the
// one cost a miss cannot avoid. Not thread-safe: the glue keeps one per processing
// thread, like the runtime's display-list scan cache.
template <typename Entry>
class Store {
public:
  static constexpr size_t kMaxEntries = 2048;
  static constexpr uint64_t kMaxBytes = 16ull * 1024 * 1024;
  static constexpr uint32_t kBackoffStreak = 16;      // consecutive digest misses
  static constexpr uint64_t kBackoffCalls = 65536;    // lookups skipped once backed off
  static constexpr size_t kMaxBackoffKeys = 4096;     // beyond this the table is dropped

  // The entry stored under exactly this key, or nullptr. A hit touches the LRU.
  Entry* Find(const Key& key) {
    const auto it = m_map.find(key);
    if (it == m_map.end()) return nullptr;
    m_lru.splice(m_lru.begin(), m_lru, it->second);
    return &it->second->entry;
  }
  const Entry* Peek(const Key& key) const {
    const auto it = m_map.find(key);
    return it == m_map.end() ? nullptr : &it->second->entry;
  }
  // Whether any state variant of this content is stored, without touching the LRU.
  bool HasContent(const ContentKey& content) const { return m_content.find(content) != m_content.end(); }

  // Insert (or replace) an entry of `bytes` stored bytes, evicting least-recently-used
  // entries until it fits both budgets. Refuses an entry that alone exceeds the byte
  // budget. Returns false only for that refusal.
  bool Insert(Key key, Entry entry, uint64_t bytes) {
    if (bytes > kMaxBytes) return false;
    if (const auto it = m_map.find(key); it != m_map.end()) {
      m_bytes -= it->second->bytes;
      m_lru.erase(it->second);
      m_map.erase(it);
      RemoveContentIndex(key);
    }
    while (!m_lru.empty() && (m_lru.size() >= kMaxEntries || m_bytes + bytes > kMaxBytes)) {
      EvictBack();
    }
    m_lru.push_front(Node{key, std::move(entry), bytes});
    m_map.emplace(key, m_lru.begin());
    m_content[ContentKey(key)].push_back(key);
    m_bytes += bytes;
    return true;
  }

  bool Erase(const Key& key) {
    const auto it = m_map.find(key);
    if (it == m_map.end()) return false;
    m_bytes -= it->second->bytes;
    m_lru.erase(it->second);
    m_map.erase(it);
    RemoveContentIndex(key);
    return true;
  }

  // One more consecutive digest miss for this content. Reaching the streak backs the
  // content off for kBackoffCalls further lookups and restarts the streak.
  void NoteDigestMiss(const ContentKey& content, uint64_t now) {
    if (m_backoff.size() >= kMaxBackoffKeys && m_backoff.find(content) == m_backoff.end()) {
      m_backoff.clear(); // pathological churn everywhere: forget, never grow unbounded
    }
    Backoff& b = m_backoff[content];
    if (now < b.skipUntil) return; // already backed off; lookups should not reach here
    if (++b.streak >= kBackoffStreak) {
      b.streak = 0;
      b.skipUntil = now + kBackoffCalls;
    }
  }
  bool BackedOff(const ContentKey& content, uint64_t now) const {
    const auto it = m_backoff.find(content);
    return it != m_backoff.end() && now < it->second.skipUntil;
  }
  // A hit (or a fresh recording) clears any miss streak for the content.
  void NoteContentServed(const ContentKey& content) {
    if (const auto it = m_backoff.find(content); it != m_backoff.end()) it->second.streak = 0;
  }

  size_t Size() const noexcept { return m_lru.size(); }
  uint64_t Bytes() const noexcept { return m_bytes; }
  uint64_t Evictions() const noexcept { return m_evictions; }

private:
  struct Node {
    Key key;
    Entry entry;
    uint64_t bytes;
  };
  struct Backoff {
    uint32_t streak = 0;
    uint64_t skipUntil = 0;
  };
  void EvictBack() {
    const Node& back = m_lru.back();
    m_bytes -= back.bytes;
    m_map.erase(back.key);
    RemoveContentIndex(back.key);
    m_lru.pop_back();
    ++m_evictions;
  }
  void RemoveContentIndex(const Key& key) {
    const auto it = m_content.find(ContentKey(key));
    if (it == m_content.end()) return;
    auto& keys = it->second;
    for (size_t i = 0; i < keys.size(); ++i) {
      if (keys[i] == key) {
        keys[i] = keys.back();
        keys.pop_back();
        break;
      }
    }
    if (keys.empty()) m_content.erase(it);
  }

  std::list<Node> m_lru; // front = most recently used
  std::unordered_map<Key, typename std::list<Node>::iterator, KeyHash> m_map;
  std::unordered_map<ContentKey, std::vector<Key>, ContentKeyHash> m_content;
  std::unordered_map<ContentKey, Backoff, ContentKeyHash> m_backoff;
  uint64_t m_bytes = 0;
  uint64_t m_evictions = 0;
};

} // namespace aurora::gx::vcache
