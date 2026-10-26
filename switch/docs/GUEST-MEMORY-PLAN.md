# Horizon guest memory: plan

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

### Phase 1: bring-up (checked access, no SVCs). Implemented

`switch/runtime/guest_flat_memory_horizon.cpp`, tested off-device by
`switch/tests/guest_flat_memory_horizon_tests.cpp` (real region layout, ASan
and UBSan clean, 155 MiB across 4 stores).

- `guest_flat_memory_horizon.cpp`: one heap block per backing store
  (`aligned_alloc(0x10000, size)`), `HostPointer()` returning into it,
  `g_requiresCheckedAccess = true`, and every fault feature stubbed like
  macOS.
- Add a `#elif defined(__SWITCH__)` branch in `guest_flat_memory.h`, before
  the generic `__aarch64__` one, so the header stops claiming a fixed base
  that we can't guarantee (see below). Leave `MKW_GUEST_FLAT_FIXED_PAGE_SIZE`
  undefined so `RequiresCheckedAccess()` stays a runtime check.
- The cost is a branch plus a page-table lookup on every guest load and
  store. That's slow, but it gets the game booting without depending on SVC
  memory tricks, and it isolates memory bugs from graphics bugs.

### Phase 2: flat fast path (real aliasing)

The macOS file's structure, with libnx calls in place of mmap:

| macOS/Linux | Horizon |
|---|---|
| reserve 4 GiB at fixed base | `svcQueryMemory` confirms the range is Free and outside the heap, alias and stack regions (`svcGetInfo` InfoType_*RegionAddress/Size), then `virtmemAddReservation` |
| backing fd (memfd/mkstemp) | page-aligned heap block per store |
| host view mmap | the code-memory window from the JSRF fork (`svcMapProcessCodeMemory` + permission to RW) |
| guest view mmap per region | `svcMapProcessMemory(g_base + r.base, envGetOwnProcessHandle(), window + offset, size)` |

Why the detour through code memory: `svcMapProcessMemory` only accepts
source pages whose kernel memory state allows "map process", and plain heap
(`Normal`) doesn't. Remapping the heap as code memory gives it that state.
That's the JSRF trick, so we lift its exact sequence instead of
rediscovering it.

Open items to verify on hardware:
1. **Three guest views of one source.** MEM1 is mirrored at `0x00000000`,
   `0x80000000` and `0xC0000000`. JSRF mapped one alias, so confirm the
   kernel accepts several destination maps of the same source range.
2. **Fixed base.** Horizon randomizes where heap, alias and stack land in
   the 39-bit space, so `0x10'0000'0000` may be taken on some boots. If the
   check above fails often, make the base a runtime global on `__SWITCH__`
   only. That costs one extra load per access, which is still far cheaper
   than Phase 1.
3. **Process handle.** `envGetOwnProcessHandle()` needs hbloader to pass a
   real handle, which it does under title takeover. Title takeover is
   already the plan.

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
  it's materialized, it sees stale bytes. Watch for this. If it shows up,
  route those specific reads through the checked path instead of trying to
  add fault handling.

## Rotate builtin

Moot. The build compiles WiiCompiled with clang (WiiCompiled requires it),
and clang has `__builtin_rotateleft32`. The GCC shim from the prep folder is
not part of the fork. If upstream ever wants GCC support, the one-line fix in
`PpcRotl32Inline()` is `return std::rotl(value, static_cast<int>(shift));`.
