// LinkListImpl ctor / deleting-dtor indirect-call overrides.
//
// nw4r::ut::detail::LinkListImpl is a header-only intrusive list (count at
// +0, sentinel node at +4), so every translation unit that uses it got its
// own copy of the ctor and the deleting dtor at a different guest address,
// with identical bodies. The sound driver's object manager constructs and
// destroys 128-element arrays of these (12 bytes each) for its bucket pass,
// several passes per frame, and other users build small member arrays the
// same way. The bodies are tiny (the ctor is five stores), so almost all of
// the per-call cost is the indirect-dispatch boundary around them, not the
// work. Serving the calls natively removes that boundary.
//
// These addresses are reached only through guest function pointers
// (__construct_array / __destroy_arr), never by a direct call, and they sit
// in the generated indirect dispatch table. They are therefore registered
// with REGISTER_INDIRECT_CALL_OVERRIDE, not as natives: a native winner
// would fail ValidateGeneratedIndirectDispatchLocked at boot (see the
// AXFXReverbHiExpCallback note in hle/audio/ax_effects.cpp). The generated
// table and registry stay as generated and no retranslation is needed.
//
// Semantics are transcribed from the generated bodies:
//  - Ctor: zero the count and both sentinel links, then self-link the
//    sentinel ([this+4] = [this+8] = this+4). Leaves gpr[0] = 0 and
//    gpr[4] = this+4; gpr[3] passes through. No null check, matching the
//    generated body.
//  - Deleting dtor: a null this returns at once. Otherwise it performs the
//    real dtor's walk (func_802E1854, always called with flag 0 by these
//    wrappers): unlink every node, decrement the count once per node and
//    zero each removed node's links; the nodes themselves are not freed.
//    Then, iff the incoming flag in gpr[4] is signed-greater than 0, it
//    frees this through guest operator delete (func_801A61FC). gpr[3]
//    holds this on every exit path, and gpr[4] is 0 on the non-delete
//    path (null this leaves gpr[4] untouched, as generated). The
//    generated prologue/epilogue stack-frame writes and lr
//    juggling are not reproduced: that scratch is dead by the ABI.
// The four deleting dtors of static LinkListImpl objects have no ctor
// copy and run only at exit; they are deliberately not covered here.

#include "abi_bridge.h"
#include "memory.h"
#include "ppc_runtime.h"

#include <cstdint>

// Guest operator delete (__dl__FPv), still linked as a translated body.
extern "C" void func_801A61FC(CpuContext* ctx);

namespace LinkListNativeDetail {

void Construct(CpuContext* ctx)
{
    const uint32_t self = ctx->gpr[3];
    MemoryInline::FlatWrite32(self + 4, 0);
    MemoryInline::FlatWrite32(self + 8, 0);
    MemoryInline::FlatWrite32(self, 0);
    MemoryInline::FlatWrite32(self + 4, self + 4);
    MemoryInline::FlatWrite32(self + 8, self + 4);
    ctx->gpr[0] = 0;
    ctx->gpr[4] = self + 4;
}

void DeletingDestruct(CpuContext* ctx)
{
    const uint32_t self = ctx->gpr[3];
    const int32_t flag = static_cast<int32_t>(ctx->gpr[4]);
    if (self == 0) {
        ctx->gpr[3] = 0;
        return;
    }

    const uint32_t sentinel = self + 4;
    uint32_t node = MemoryInline::FlatRead32(self + 4);
    while (node != sentinel) {
        const uint32_t next = MemoryInline::FlatRead32(node);
        const uint32_t prev = MemoryInline::FlatRead32(node + 4);
        MemoryInline::FlatWrite32(next + 4, prev);
        MemoryInline::FlatWrite32(prev, next);
        MemoryInline::FlatWrite32(self, MemoryInline::FlatRead32(self) - 1);
        MemoryInline::FlatWrite32(node, 0);
        MemoryInline::FlatWrite32(node + 4, 0);
        node = next;
    }

    if (flag > 0) {
        // The generated wrapper reaches operator delete with gpr[4] = 0,
        // left there by the real-dtor call above.
        ctx->gpr[3] = self;
        ctx->gpr[4] = 0;
        func_801A61FC(ctx);
        ctx->gpr[3] = self;
        return;
    }
    ctx->gpr[4] = 0;
    ctx->gpr[3] = self;
}

} // namespace LinkListNativeDetail

void LinkListCtor_80303D40(CpuContext* ctx) { LinkListNativeDetail::Construct(ctx); }
void LinkListDeletingDtor_80303D60(CpuContext* ctx) { LinkListNativeDetail::DeletingDestruct(ctx); }
void LinkListCtor_80301164(CpuContext* ctx) { LinkListNativeDetail::Construct(ctx); }
void LinkListDeletingDtor_80301184(CpuContext* ctx) { LinkListNativeDetail::DeletingDestruct(ctx); }
void LinkListCtor_803011DC(CpuContext* ctx) { LinkListNativeDetail::Construct(ctx); }
void LinkListDeletingDtor_803011FC(CpuContext* ctx) { LinkListNativeDetail::DeletingDestruct(ctx); }
void LinkListCtor_80301254(CpuContext* ctx) { LinkListNativeDetail::Construct(ctx); }
void LinkListDeletingDtor_80301274(CpuContext* ctx) { LinkListNativeDetail::DeletingDestruct(ctx); }
void LinkListCtor_802F1BDC(CpuContext* ctx) { LinkListNativeDetail::Construct(ctx); }
void LinkListDeletingDtor_802F1BFC(CpuContext* ctx) { LinkListNativeDetail::DeletingDestruct(ctx); }
void LinkListCtor_80308174(CpuContext* ctx) { LinkListNativeDetail::Construct(ctx); }
void LinkListDeletingDtor_8030811C(CpuContext* ctx) { LinkListNativeDetail::DeletingDestruct(ctx); }

REGISTER_INDIRECT_CALL_OVERRIDE(0x80303D40u, LinkListCtor_80303D40);
REGISTER_INDIRECT_CALL_OVERRIDE(0x80303D60u, LinkListDeletingDtor_80303D60);
REGISTER_INDIRECT_CALL_OVERRIDE(0x80301164u, LinkListCtor_80301164);
REGISTER_INDIRECT_CALL_OVERRIDE(0x80301184u, LinkListDeletingDtor_80301184);
REGISTER_INDIRECT_CALL_OVERRIDE(0x803011DCu, LinkListCtor_803011DC);
REGISTER_INDIRECT_CALL_OVERRIDE(0x803011FCu, LinkListDeletingDtor_803011FC);
REGISTER_INDIRECT_CALL_OVERRIDE(0x80301254u, LinkListCtor_80301254);
REGISTER_INDIRECT_CALL_OVERRIDE(0x80301274u, LinkListDeletingDtor_80301274);
REGISTER_INDIRECT_CALL_OVERRIDE(0x802F1BDCu, LinkListCtor_802F1BDC);
REGISTER_INDIRECT_CALL_OVERRIDE(0x802F1BFCu, LinkListDeletingDtor_802F1BFC);
// Pair 6: the dtor sits below the ctor in guest address order; each is
// registered at its own address.
REGISTER_INDIRECT_CALL_OVERRIDE(0x80308174u, LinkListCtor_80308174);
REGISTER_INDIRECT_CALL_OVERRIDE(0x8030811Cu, LinkListDeletingDtor_8030811C);
