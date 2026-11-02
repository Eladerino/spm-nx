// Instrumented-PGO capture build (SpmSwitch.cmake SPM_PGO_INSTRUMENT; test build spec E).
// Ported from KartPadNX's switch/src/pgo_instr_switch.c (same author family, GPL): it stands in
// for compiler-rt's profile runtime, which is not built for aarch64-none-elf, and writes what
// that runtime's __llvm_profile_write_buffer writes (clang 19 raw profile, no value profiles:
// -disable-vp; no binary ids) for everything run since boot to
// /switch/spm-nx/UserData/pgo/spm-<boot time>.profraw, replaced every 5 minutes and once more
// from userAppExit. Each file holds its whole session, so merge only the last file of each
// session (llvm-profdata 19). This build exists to produce that file: its frame rates are
// never quoted.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <switch.h>

#include <profile/InstrProfData.inc>
enum ValueKind {
#define VALUE_PROF_KIND(Enumerator, Value, Descr) Enumerator = Value,
#include <profile/InstrProfData.inc>
};
typedef void* IntPtrT;
typedef struct __attribute__((aligned(INSTR_PROF_DATA_ALIGNMENT))) {
#define INSTR_PROF_DATA(Type, LLVMType, Name, Initializer) Type Name;
#include <profile/InstrProfData.inc>
} ProfData;
typedef struct {
#define INSTR_PROF_RAW_HEADER(Type, Name, Initializer) Type Name;
#include <profile/InstrProfData.inc>
} ProfHeader;

// Each instrumented object references this symbol to pull a profile runtime in.
int __llvm_profile_runtime;
extern const uint64_t __llvm_profile_raw_version __attribute__((visibility("hidden")));
#define PROF_SECTION(name)                                                    \
  extern char __start_##name[] __attribute__((weak, visibility("hidden"))); \
  extern char __stop_##name[] __attribute__((weak, visibility("hidden")));
PROF_SECTION(__llvm_prf_data)
PROF_SECTION(__llvm_prf_cnts)
PROF_SECTION(__llvm_prf_bits)
PROF_SECTION(__llvm_prf_names)

static uint64_t padding(uint64_t n) { return 7 & (8 - n % 8); }

// In 1 MiB pieces, so the game's own SD reads are not held behind one long write. Each piece is
// copied into pages of its own first: the FS write maps its buffer's whole pages into the server
// and the kernel makes them read-only meanwhile, so writing the live counters faulted the next
// thread that counted into them (two Data Aborts on KartPadNX hardware, 2026-10-31).
static int put(FILE* f, const void* p, uint64_t n) {
  static char piece[1u << 20] __attribute__((aligned(0x1000)));
  for (const char* c = p; n > 0;) {
    const size_t part = n < sizeof(piece) ? (size_t)n : sizeof(piece);
    if (p != NULL) {
      memcpy(piece, c, part);
      c += part;
    } else {
      memset(piece, 0, part);
    }
    if (fwrite(piece, 1, part, f) != part) return 0;
    n -= part;
  }
  return 1;
}

static int write_profile(FILE* f) {
  const char *data = __start___llvm_prf_data, *cnts = __start___llvm_prf_cnts;
  const char *bits = __start___llvm_prf_bits, *names = __start___llvm_prf_names;
  const uint64_t cntsSize = __stop___llvm_prf_cnts - cnts, bitsSize = __stop___llvm_prf_bits - bits;
  const uint64_t namesSize = __stop___llvm_prf_names - names;
  ProfHeader h = {0};
  h.Magic = INSTR_PROF_RAW_MAGIC_64;
  h.Version = __llvm_profile_raw_version;
  h.NumData = (__stop___llvm_prf_data - data + sizeof(ProfData) - 1) / sizeof(ProfData);
  h.NumCounters = cntsSize / sizeof(uint64_t);
  h.PaddingBytesAfterCounters = padding(cntsSize);
  h.NumBitmapBytes = bitsSize;
  h.PaddingBytesAfterBitmapBytes = padding(bitsSize);
  h.NamesSize = namesSize;
  h.CountersDelta = (uintptr_t)cnts - (uintptr_t)data;
  h.BitmapDelta = (uintptr_t)bits - (uintptr_t)data;
  h.NamesDelta = (uintptr_t)names;
  h.ValueKindLast = IPVK_Last;
  return put(f, &h, sizeof(h)) && put(f, data, h.NumData * sizeof(ProfData)) && put(f, cnts, cntsSize) &&
         put(f, NULL, h.PaddingBytesAfterCounters) && put(f, bits, bitsSize) &&
         put(f, NULL, h.PaddingBytesAfterBitmapBytes) && put(f, names, namesSize) &&
         put(f, NULL, padding(namesSize));
}

static long long g_session;

static void dump(const char* why) {
  char path[96], tmp[104];
  snprintf(path, sizeof(path), "/switch/spm-nx/UserData/pgo/spm-%lld.profraw", g_session);
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  const uint64_t t0 = armGetSystemTick();
  FILE* f = fopen(tmp, "wb");
  if (f != NULL) setvbuf(f, NULL, _IONBF, 0);  // every write straight from put's own pages, not a shared heap buffer
  int ok = f != NULL && write_profile(f);
  if (f != NULL) ok &= fclose(f) == 0;
  if (ok) {
    remove(path);
    ok = rename(tmp, path) == 0;
  }
  fprintf(stderr, "[pgo] %s: %s %s in %llu ms\n", why, ok ? "wrote" : "FAILED to write", path,
          (unsigned long long)(armTicksToNs(armGetSystemTick() - t0) / 1000000));
}

// The periodic dump is the primary flush: crash and hard-exit paths may skip userAppExit, so a
// session never depends on a clean exit for its last five minutes.
static void pgo_main(void* arg) {
  (void)arg;
  for (;;) {
    svcSleepThread(300000000000ll);  // 5 minutes
    dump("5 minutes");
  }
}

// spm_switch_init.cpp's userAppExit, before the guest memory release: the session's final,
// most complete file.
void spm_pgo_instr_dump_now(void) { dump("exit"); }

// spm_switch_init.cpp's userAppInit, once the SD layout and stderr exist. This line is also
// the build tag: a capture NRO announces itself in every log it writes.
void spm_pgo_instr_start(void) {
  static Thread thread;
  g_session = (long long)time(NULL);
  mkdir("/switch/spm-nx/UserData/pgo", 0777);
  if (R_FAILED(threadCreate(&thread, pgo_main, NULL, NULL, 64 * 1024, 0x3B, 0)) || R_FAILED(threadStart(&thread))) {
    fprintf(stderr, "[pgo] dump thread start failed\n");
    return;
  }
  fprintf(stderr, "[pgo] [build] pgo=instrumented capture build: %llu KiB of counters, %llu functions\n",
          (unsigned long long)((__stop___llvm_prf_cnts - __start___llvm_prf_cnts) >> 10),
          (unsigned long long)((__stop___llvm_prf_data - __start___llvm_prf_data) / sizeof(ProfData)));
}
