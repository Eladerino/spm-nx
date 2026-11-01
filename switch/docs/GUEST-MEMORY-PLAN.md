# Horizon guest memory: plan

> **Status: implemented.** Phase 2 (the flat window) is the default on Switch and phase 1
> remains as a build option; the port has since merged upstream v1.0.6, and phase 2 survived
> that merge. Read the "plan" below as the design record it was written as.

Based on upstream NewSuperPaperBros at `46343ab`:
`wiicompiled/runtime/include/guest_flat_memory.h`, `src/guest_flat_memory.cpp`
(Windows + Linux), `src/guest_flat_memory_macos.cpp`, `src/memory.cpp`.

## How the runtime does guest memory today

- **Flat 4 GiB window at a fixed base.** A guest access compiles to
  `*(T*)(MKW_FLAT_GUEST_BASE + addr)` plus a byte swap. The base is a
  `constexpr` picked per architecture in `guest_flat_memory.h`. Generic
  AArch64 gets `0x10'0000'0000` (64 GiB). The translator never emits the
  number itself (it only shows up in runtime headers), so a
  Horizon-specific base is a header change and has no effect
  on recompiled code.
- **Two views of one backing store.** Each backing store (MEM1, MEM2, plus
  "Owned" stores such as the locked cache at `0xE0000000`) is mapped twice:
  1. a **host view** that is always read/write, used by HLE, DVD and GX code
     through `HostPointer()`;
  2. one **guest view per region** inside the flat window. Mirrors such as
     `0x00000000`, `0x80000000` and `0xC0000000` are separate guest views of
     the same MEM1 store, at offset `base & 0x01FFFFFF`.
- **Page faults as a control mechanism (Windows/Linux only).** The guest view
  uses page protection to trap MMIO (`0xCC000000`, `PROT_NONE`), deferred EFB
  reads, writes to executable pages, and wild pointers (a 64 KiB block gets
  committed on demand). A SIGSEGV or vectored handler calls
  `HandleAccessViolation()` and then resumes.
- **macOS skips all of that.** `guest_flat_memory_macos.cpp` is 80 lines: it
  reserves the window, builds the two aliased views, and stubs out every
  fault feature (`HandleAccessViolation` returns false, and the
  Protect/Register functions do nothing). So the runtime already ships on a
  platform without fault-driven interception. **That file is the template
  for Horizon.**
- **Escape hatch.** `GuestFlat::RequiresCheckedAccess()` sends translated
  accesses through the slow, checked `Memory::Read*/Write*` path. It exists
  for AArch64 kernels with pages larger than 4 KiB. Horizon uses 4 KiB pages,
  so we don't need it for that, but forcing it on gives a bring-up mode that
  never touches the flat window, with one catch: the `FlatWriteRam*` stores
  (the translator emits them where it proved the address is plain RAM, about
  45% of all stores) always wrote through the flat base, even in checked
  mode. On `__SWITCH__` they now honor checked mode too
  (`runtime/include/memory_access.h`). Without that patch phase 1 would
  crash on the first one.

## What this changes in my earlier plan

The masking question is settled: **aliasing is required.** The runtime
expects both a host view and guest views of the same bytes, so the "just
`aligned_alloc`" option only works in checked-access mode. The new plan has
two phases.

### Phase 1: bring-up (checked access, no SVCs). Kept as a build option

Configure with `-DSPM_SWITCH_CHECKED_GUEST_MEMORY=ON` to get it back.

- One heap block per backing store, with `HostPointer()` returning into it
  and every fault feature stubbed like macOS.
- `RequiresCheckedAccess()` is a compile-time `true`, so every guest load
  and store pays the checked path's page-table lookup.
- It needs none of the SVCs below, and a bad guest access is reported with
  the guest's CPU state instead of a data abort. That makes it the mode to
  reach for when a memory bug needs chasing.

### Phase 2: flat fast path (real aliasing). Implemented, the default

`switch/runtime/guest_flat_memory_horizon.cpp`. The PC test
(`switch/tests/guest_flat_memory_horizon_tests.cpp`) runs the same layout
code with mmap of a shared file in place of the SVCs. It checks that every
mirror sees every other one's stores through the window, and that the window
unmaps and maps again.

The sequence (the flatmem-spike test proved it on hardware, 2026-10-31):

1. `envIsSyscallHinted` for svcs 0x73, 0x74, 0x75, 0x77 and 0x78. If one is
   missing, the error names it.
2. `virtmemFindCodeMemory` finds 4 GiB plus 2 MiB of free code-region
   space, and `virtmemAddReservation` reserves it. The window starts at the
   first 2 MiB boundary inside it.
3. Per store: a 2 MiB-aligned heap block, zeroed, then
   `svcMapProcessCodeMemory` maps it at the window address of its primary
   view (the first region that covers the whole store), and
   `svcSetProcessMemoryPermission` makes that view read/write.
4. Each other region of the store (a mirror) is
   `svcMapProcessMemory(window + base, own process, primary view + offset, size)`.
5. At exit, `userAppExit()` undoes it all in reverse:
   `svcUnmapProcessMemory` for each mirror, then
   `svcUnmapProcessCodeMemory`, then the reservation. hbloader runs the next
   NRO in the same process, and heap pages still mapped as code memory would
   break it.

Why the detour through code memory: `svcMapProcessMemory` only accepts
source pages whose kernel memory state allows "map process", and plain heap
(`Normal`) doesn't. Remapping the heap as code memory gives it that state.

Why 2 MiB alignment: each view then sits at the same offset in a 2 MiB
block as the memory under it. That lets the kernel use 2 MiB blocks instead
of 4 KiB pages wherever the heap's physical memory is contiguous, which
means fewer TLB misses across 155 MiB of guest RAM.

How the open items came out on hardware:

1. **Several guest views of one source:** fine. The spike mapped two
   mirrors per store, and every view saw every other one's writes.
2. **Fixed base:** dropped. The window lands wherever free code-region space
   is (`0x4162368000` in the spike), so the base is a runtime value,
   `mkw_flat_guest_base`. The C++ side declares it `const` (it's defined
   non-const in `switch/runtime/horizon_flat_base.c`), so the compiler loads
   it once per translated function instead of after every guest store. A
   flat read is then `ldr w, [base, w, uxtw]` plus `rev`.
3. **Process handle:** `envGetOwnProcessHandle()` gave a real handle.

Spike timings, checked path against flat, per access:

| Test | Checked | Flat |
|---|---|---|
| Pointer chase, 16 KiB | 6.62 ns | 2.55 ns |
| Pointer chase, 1 MiB | 20.14 ns | 13.69 ns |
| Loads | 2.17 ns | 1.36 ns |
| Stores | 3.06 ns | 1.53 ns |

## Questions from the port review

- **MEM2 "fixed physical range":** that doesn't matter here. The runtime
  never models physical addresses beyond the masks above. MEM2 is just a
  128 MiB store (the NDEV size the runtime uses) with views at `0x10000000`, `0x90000000` and `0xD0000000`.
- **Hollywood registers (`0xCC000000`/`0xCD000000`):** no backing needed.
  Every device is HLE'd at the function level. Linux commits that window as
  `PROT_NONE` only so a missed hook produces a readable error. On Horizon we
  leave it unmapped, so a stray access becomes a data abort that the libnx
  crash handler reports. The checked path in Phase 1 throws its own "MMIO
  read blocked" error before that point.
- **Known gap inherited from macOS:** with no page traps, deferred EFB-copy
  reads aren't intercepted. If SPM reads back an EFB copy on the CPU before
  it's materialized, it sees stale bytes. Nothing in this build calls
  `Memory::RegisterDeferredRead` (checked 2026-10-31), so today there's
  nothing to miss. If that changes, route those specific reads through the
  checked path instead of trying to add fault handling.
- **Wild guest pointers:** Windows and Linux commit a 64 KiB block on demand
  and count it; macOS reads zeros. The Switch build has always treated
  them as fatal (the checked path throws "no mapped region"), and with the
  flat window they're a data abort that the crash report labels with the
  guest address.

## Rotate builtin

Moot. The build compiles WiiCompiled with clang (WiiCompiled requires it),
and clang has `__builtin_rotateleft32`. The GCC shim from the prep folder is
not part of the fork. If upstream ever wants GCC support, the one-line fix in
`PpcRotl32Inline()` is `return std::rotl(value, static_cast<int>(shift));`.
