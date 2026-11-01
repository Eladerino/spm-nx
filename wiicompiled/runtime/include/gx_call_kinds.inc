// How each GX override behaves under the GX worker's record ring (gx_worker.h), by guest address.
//
// This is a .inc, not a header: gx_worker.h includes it once, inside namespace GxWorker right after
// declaring KindOf<>. An address
// listed nowhere here runs Inline: pending records first, then the call itself, as before. That is
// the safe default, so an address only moves out of it once its body has been read for what it
// touches. The [calls] lines (perftimer) say how often each one runs.
//
// Queue: void, scalar arguments only, and the body touches only GX state (g_hleGxState, aurora,
// host-side copies such as g_viewportState) and its own diagnostics. A body that reads the guest
// context does so only for log lines, which then name the replay's caller in Deferred mode.
//
// SelfDefer: as Queue, except one argument points at a small struct (a colour, a matrix, a light).
// Its body reads the struct and hands it to GxWorker::DeferOrRun, so the record carries a copy.
//
// NoSync: as Inline but without running pending records first, for a call that touches nothing a
// record changes. The three getters answer from a game-side copy their setters keep at the call
// while records are pending; the texture object builders (InitTexObj and its LOD, filter, wrap
// mode, TLUT and CI forms) only fill in a host texture object and its guest struct, which nothing
// reads until LoadTexObj (Inline).
//
// Not here on purpose, so Inline: the other getters (Get*), waits (DrawDone, Flush, PixModeSync,
// SetDrawSync), InitTlutObj, LoadTexObj and LoadTlut (they read a host object keyed by the guest
// address), display list recording, copies, FIFO setup, the indexed matrix loads (they read the matrix array
// through array state a pending SetArray may still change), setters that write __GXData
// (SetScissorBoxOffset, SetZScaleOffset; SetScissor writes its part at the call and defers the
// rest), the Poke* calls, list setters (SetVtxDescv, SetVtxAttrFmtv), and the EGG and NSMBW
// helpers.

#define GX_WORKER_KIND(address, kind) \
    template <> struct KindOf<address> { static constexpr Kind kKind = Kind::kind; }

// TEV (gx_tev.cpp)
GX_WORKER_KIND(0x802904B4u, Queue);      // GXSetNumTevStages
GX_WORKER_KIND(0x8028FE58u, Queue);      // GXSetTevOp
GX_WORKER_KIND(0x80290358u, Queue);      // GXSetTevOrder
GX_WORKER_KIND(0x8028FEECu, Queue);      // GXSetTevColorIn
GX_WORKER_KIND(0x8028FF2Cu, Queue);      // GXSetTevAlphaIn
GX_WORKER_KIND(0x8028FF6Cu, Queue);      // GXSetTevColorOp
GX_WORKER_KIND(0x8028FFC4u, Queue);      // GXSetTevAlphaOp
GX_WORKER_KIND(0x8029013Cu, Queue);      // GXSetTevKColorSel
GX_WORKER_KIND(0x8029018Cu, Queue);      // GXSetTevKAlphaSel
GX_WORKER_KIND(0x80290218u, Queue);      // GXSetTevSwapModeTable
GX_WORKER_KIND(0x802901DCu, Queue);      // GXSetTevSwapMode
GX_WORKER_KIND(0x8029001Cu, SelfDefer);  // GXSetTevColor (GXColor*)
GX_WORKER_KIND(0x8029007Cu, SelfDefer);  // GXSetTevColorS10 (GXColorS10*)
GX_WORKER_KIND(0x802900E0u, SelfDefer);  // GXSetTevKColor (GXColor*)

// Pixel (gx_pixel.cpp)
GX_WORKER_KIND(0x80290828u, Queue);      // GXSetBlendMode
GX_WORKER_KIND(0x80290878u, Queue);      // GXSetColorUpdate
GX_WORKER_KIND(0x802908A4u, Queue);      // GXSetAlphaUpdate
GX_WORKER_KIND(0x802908D0u, Queue);      // GXSetZMode
GX_WORKER_KIND(0x80290904u, Queue);      // GXSetZCompLoc
GX_WORKER_KIND(0x80290934u, Queue);      // GXSetPixelFmt
GX_WORKER_KIND(0x802909DCu, Queue);      // GXSetDither
GX_WORKER_KIND(0x80290A08u, Queue);      // GXSetDstAlpha
GX_WORKER_KIND(0x80290294u, Queue);      // GXSetAlphaCompare
GX_WORKER_KIND(0x802902CCu, Queue);      // GXSetZTexture
GX_WORKER_KIND(0x8028D67Cu, SelfDefer);  // GXSetCullMode (keeps the game-side mode)
GX_WORKER_KIND(0x8028D6A4u, NoSync);     // GXGetCullMode
GX_WORKER_KIND(0x8028D6C0u, Queue);      // GXSetCoPlanar
GX_WORKER_KIND(0x80291B38u, Queue);      // GXSetClipMode
GX_WORKER_KIND(0x802904D8u, SelfDefer);  // GXSetFog (GXColor*)

// Vertex (gx_vertex.cpp)
GX_WORKER_KIND(0x8028BEC8u, Queue);      // GXClearVtxDesc
GX_WORKER_KIND(0x8028B638u, Queue);      // GXSetVtxDesc
GX_WORKER_KIND(0x8028BEFCu, Queue);      // GXSetVtxAttrFmt
GX_WORKER_KIND(0x8028C5C0u, Queue);      // GXSetArray
GX_WORKER_KIND(0x8028C838u, Queue);      // GXSetNumTexGens
GX_WORKER_KIND(0x8028C610u, Queue);      // GXSetTexCoordGen2
GX_WORKER_KIND(0x8028D640u, Queue);      // GXEnableTexOffsets
GX_WORKER_KIND(0x8028D5D8u, Queue);      // GXSetLineWidth
GX_WORKER_KIND(0x8028D60Cu, Queue);      // GXSetPointSize
GX_WORKER_KIND(0x8028D3B4u, Queue);      // GXBegin
GX_WORKER_KIND(0x802A35B0u, Queue);      // GXEnd (out-of-line copy 1)
GX_WORKER_KIND(0x802A7C90u, Queue);      // GXEnd (out-of-line copy 2)

// Lighting (gx_lighting.cpp)
GX_WORKER_KIND(0x8028E778u, Queue);      // GXSetNumChans
GX_WORKER_KIND(0x8028E79Cu, Queue);      // GXSetChanCtrl
GX_WORKER_KIND(0x8028E5C8u, SelfDefer);  // GXSetChanAmbColor (GXColor*)
GX_WORKER_KIND(0x8028E6A0u, SelfDefer);  // GXSetChanMatColor (GXColor*)
GX_WORKER_KIND(0x8028E54Cu, SelfDefer);  // GXLoadLightObjImm (GXLightObj*)

// Indirect (gx_indirect.cpp)
GX_WORKER_KIND(0x8028FD44u, Queue);      // GXSetNumIndStages
GX_WORKER_KIND(0x8028FC78u, Queue);      // GXSetIndTexOrder
GX_WORKER_KIND(0x8028FB74u, Queue);      // GXSetIndTexCoordScale
GX_WORKER_KIND(0x8028FD64u, Queue);      // GXSetTevDirect
GX_WORKER_KIND(0x8028FDACu, Queue);      // GXSetTevIndWarp
GX_WORKER_KIND(0x8028F9B8u, Queue);      // GXSetTevIndirect
GX_WORKER_KIND(0x8028FA20u, SelfDefer);  // GXSetIndTexMtx (f32[2][3])

// Transform (gx_transform.cpp)
GX_WORKER_KIND(0x80291830u, Queue);      // GXSetCurrentMtx
GX_WORKER_KIND(0x802919D0u, SelfDefer);  // GXSetViewport (keeps the game-side viewport)
GX_WORKER_KIND(0x80291994u, SelfDefer);  // GXSetViewportJitter (likewise)
GX_WORKER_KIND(0x802919FCu, NoSync);     // GXGetViewportv
GX_WORKER_KIND(0x80291A4Cu, SelfDefer);  // GXSetScissor (writes __GXData at the call)
GX_WORKER_KIND(0x80291788u, SelfDefer);  // GXLoadPosMtxImm (f32[3][4])
GX_WORKER_KIND(0x802917D8u, SelfDefer);  // GXLoadNrmMtxImm (f32[3][4])
GX_WORKER_KIND(0x80291850u, SelfDefer);  // GXLoadTexMtxImm (f32[3][4] or [2][4])
GX_WORKER_KIND(0x80291698u, SelfDefer);  // GXSetProjection (f32[4][4])
GX_WORKER_KIND(0x802916FCu, SelfDefer);  // GXSetProjectionv (f32[7])
GX_WORKER_KIND(0x80291748u, NoSync);     // GXGetProjectionv

// Texture (gx_texture.cpp)
GX_WORKER_KIND(0x8028EA24u, NoSync);     // GXInitTexObj
GX_WORKER_KIND(0x8028EC30u, NoSync);     // GXInitTexObjCI
GX_WORKER_KIND(0x8028EC78u, NoSync);     // GXInitTexObjLOD
GX_WORKER_KIND(0x8028ED8Cu, NoSync);     // GXInitTexObjWrapMode
GX_WORKER_KIND(0x8028EDA0u, NoSync);     // GXInitTexObjTlut
GX_WORKER_KIND(0x8028EDA8u, NoSync);     // GXInitTexObjFilter

// Display lists (gx_dl.cpp): the list's bytes are read at replay, as the GPU reads them late on
// hardware and the decode thread already does.
GX_WORKER_KIND(0x80291458u, Queue);      // GXCallDisplayList

#undef GX_WORKER_KIND
