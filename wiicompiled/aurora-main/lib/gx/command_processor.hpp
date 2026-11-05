#pragma once

#include "dolphin/gx/GXEnum.h"
#include "../internal.hpp"

#include <array>
#include <cstdint>

namespace aurora::gx::fifo {

// Reset the CP write-deduplication state whenever the GX shadow state is reinitialized.
void reset_cp_register_cache();

// True while a recorded display list is being replayed. The game's SDK never flushes deferred state (genMode) into a
// list, so genMode copies Aurora recorded must not switch coplanar mode off (SPM mi1_01 front grass).
extern bool g_replayingDisplayList;

// Process a buffer of GX FIFO commands
uint32_t process(const uint8_t* data, uint32_t size, bool bigEndian);

// NSMBW debugging: the runtime setters for the draw-merging mode and the vertex-array
// re-upload switch (handle_draw in command_processor.cpp). Each flag still initialises from
// its env var (NSMBW_NO_MERGE / NSMBW_ARRAY_REUPLOAD) on first use.
void nsmbw_set_no_merge(int mode);
void nsmbw_set_array_reupload(bool on);

// The decoded display-list vertex cache (vcache_core.hpp; implementation in
// command_processor.cpp). process_display_list (fifo.cpp) consults it around its
// decode loop while the vcache diag switch is on: try_replay first, then begin/end
// around the real decode to record what it produced. All three are no-ops that cost
// one flag check while the switch is off (fifo.cpp does not even call them then).
struct VCacheRecording {
  bool active = false; // the list passed every gate and the decode is being recorded
  uint64_t listHash = 0;
  uint64_t stateDigest = 0;
  uint32_t nbytes = 0;
  bool bigEndian = false;
  bool indexed = false; // a vertex attribute is indexed: array generations were sampled
  std::array<uint64_t, 4> usage0{}; // gfx::StagingSizes at begin
  uint64_t epoch0 = 0;
  uint32_t passIndex0 = 0;
  uint32_t commandCount0 = 0;
  // The pass's last GX draw before the list, for spotting a first draw that merged
  // into a draw the list does not own.
  bool hadDraw0 = false;
  uint32_t draw0VtxCount = 0;
  uint32_t draw0VertRangeSize = 0;
  uint32_t draw0IdxRangeSize = 0;
  uint64_t statDraws0 = 0;
  uint32_t audit0 = 0;
  uint32_t merged0 = 0;
  std::array<uint64_t, 26> arrayGen0{}; // per-attribute guest write generations
  uint32_t arrayGenMask = 0;
  bool arrayGenOk = false; // every indexed array is write-tracked
  uint32_t bypassStamp = 0; // the bypass-relevant diag flags as they were at begin
  bool verify = false;      // SPM_VCACHE_VERIFY: compare against the stored entry at end
};
bool vcache_try_replay(const uint8_t* data, uint32_t nbytes, bool bigEndian);
VCacheRecording vcache_begin(const uint8_t* data, uint32_t nbytes, bool bigEndian);
void vcache_end(VCacheRecording& rec, bool splitOccurred);

// Submit already-packed direct vertex bytes against the current GX state.
bool submit_raw_draw(GXPrimitive prim, GXVtxFmt fmt, const uint8_t* vertices, uint16_t vtxCount,
                     uint32_t vertexBytes);
// submit_raw_draw's decode half, after its drain: what the decode thread runs for a raw draw.
bool decode_raw_draw(GXPrimitive prim, GXVtxFmt fmt, const uint8_t* vertices, uint16_t vtxCount,
                     uint32_t vertexBytes);

} // namespace aurora::gx::fifo
