#pragma once

#include "../internal.hpp"

#include <cstring>

namespace aurora::gx::fifo {

namespace detail {
extern uint8_t* sBufferData;
extern uint32_t sBufferSize;
extern uint32_t sBufferCapacity;
extern bool sInDisplayList;
extern uint8_t* sDlBuffer;
extern uint32_t sDlSize;
extern uint32_t sDlWritePos;
extern bool sAsyncDecode;
} // namespace detail

void init();

// Out-of-line slow path: grows internal buffer then appends data
void write_data_grow(const void* data, uint32_t length);

inline void write_data(const void* data, const uint32_t length) {
  if (!detail::sInDisplayList)
    LIKELY {
      if (detail::sBufferSize + length <= detail::sBufferCapacity)
        LIKELY {
          std::memcpy(detail::sBufferData + detail::sBufferSize, data, length);
          detail::sBufferSize += length;
          return;
        }
      write_data_grow(data, length);
    }
  else if (detail::sDlWritePos + length <= detail::sDlSize) {
    std::memcpy(detail::sDlBuffer + detail::sDlWritePos, data, length);
    detail::sDlWritePos += length;
  }
}

inline void write_u8(const uint8_t val) {
  if (!detail::sInDisplayList)
    LIKELY {
      if (detail::sBufferSize < detail::sBufferCapacity)
        LIKELY {
          detail::sBufferData[detail::sBufferSize++] = val;
          return;
        }
      write_data_grow(&val, 1);
    }
  else if (detail::sDlWritePos < detail::sDlSize) {
    detail::sDlBuffer[detail::sDlWritePos++] = val;
  }
}

inline void write_u16(const uint16_t val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

inline void write_u32(const uint32_t val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

inline void write_u64(const uint64_t val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

inline void write_f32(const float val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

// Display list recording
void begin_display_list(uint8_t* buf, uint32_t size);
uint32_t end_display_list();
bool in_display_list();

// Drain the internal FIFO buffer through the command processor. With the decode thread on, this
// waits until that thread has decoded everything recorded so far, so callers may read and change
// live renderer state afterwards exactly as before.
void drain();

// Internal buffer inspection
const uint8_t* get_buffer_data();
uint32_t get_buffer_size();
void clear_buffer();

// Decode a display list in staging-sized pieces (GXCallDisplayList, and the decode thread).
void process_display_list(const uint8_t* data, uint32_t nbytes, bool bigEndian);

// The decode thread (Switch Diagnostics "cpthread"): GX commands are still recorded on the game
// thread, but process() runs on a thread of its own, the way the GameCube's command processor
// consumed the FIFO behind the CPU. The recorded bytes are handed over in batches; display lists
// and raw draws are copied into the batch, since their buffers are only borrowed for the call.
// Everything that reads or changes decoder state on the game thread calls sync() (or drain())
// first. Off, every entry point below runs the code it always ran.
//
// The request is read at the end of a frame (apply_decode_thread_request, from aurora's
// end_frame), when everything recorded has been decoded, so a frame never mixes the two modes.
void set_decode_thread_requested(bool on) noexcept;
bool decode_thread_requested() noexcept;
void apply_decode_thread_request();
// Game thread only. True from the frame the decode thread took over.
inline bool decode_thread_active() noexcept { return detail::sAsyncDecode; }
// Waits until the decode thread has decoded what sync mode would have by now: everything up to
// the drain point (see insert_at_drain_point), which leaves decoder state as sync mode has it
// here. Does nothing while the decode thread is off. drain() decodes everything recorded.
void sync();
// The old `if (get_buffer_size() != 0) drain()` of the copy paths: with the decode thread on the
// buffer can be empty while batches are still being decoded, so this drains either way.
void drain_if_pending();
// GXEnd's drain: drain() when off; with the decode thread on, the batch is handed over once it
// is large enough.
void flush();
// GXCallDisplayList with the decode thread on: the list's bytes go into the batch.
void queue_display_list(const uint8_t* data, uint32_t nbytes, bool bigEndian);
// submit_raw_draw with the decode thread on: the vertices go into the batch.
void queue_raw_draw(uint8_t prim, uint8_t fmt, const uint8_t* vertices, uint16_t vtxCount, uint32_t vertexBytes);
// Sync mode applies a few things to decoder state on the spot, ahead of the commands recorded
// since the last drain, which it decodes only at the next one. With the decode thread on, these
// go into the recorded FIFO at that same point (where drain() would have stopped), so the decode
// thread applies them in the order sync mode did. Never into a display list being recorded, as
// the direct writes never were.
void insert_at_drain_point(const void* data, uint32_t length);
// Builds a command for insert_at_drain_point in the recorded FIFO's big-endian layout.
struct DirectCommand {
  uint8_t bytes[32];
  uint32_t size = 0;
  void put8(uint8_t v) noexcept { bytes[size++] = v; }
  void put16(uint16_t v) noexcept {
    put8(static_cast<uint8_t>(v >> 8));
    put8(static_cast<uint8_t>(v));
  }
  void put32(uint32_t v) noexcept {
    put16(static_cast<uint16_t>(v >> 16));
    put16(static_cast<uint16_t>(v));
  }
  void put64(uint64_t v) noexcept {
    put32(static_cast<uint32_t>(v >> 32));
    put32(static_cast<uint32_t>(v));
  }
};
inline void insert_at_drain_point(const DirectCommand& cmd) { insert_at_drain_point(cmd.bytes, cmd.size); }
// GXApplyBPReg with the decode thread on: the BP write, at the drain point.
void queue_bp_write(uint32_t value);
// Stops the decode thread (aurora's shutdown).
void stop_decode_thread() noexcept;

} // namespace aurora::gx::fifo
