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
// LoadTexObj is one too: its game-side part (the texture meta, the host object build, the bound
// map cache, __GXData) runs at the call, and only the bind is recorded, with a copy of the host
// texture object.
//
// NoSync: as Inline but without running pending records first, for a call that touches nothing a
// record changes. The three getters answer from a game-side copy their setters keep at the call
// while records are pending; the texture object builders (InitTexObj and its LOD, filter, wrap
// mode, TLUT and CI forms) and InitTexObjData only fill in a host texture object, its guest struct
// and its meta, which nothing reads until LoadTexObj, and that copies the object at the call.
//
// Not here on purpose, so Inline: the other getters (Get*), waits (DrawDone, Flush, PixModeSync,
// SetDrawSync), InitTlutObj and LoadTlut (they read a host object keyed by the guest address), display list recording, copies (GXCopyTex stays Inline: its body downloads the copy for the CPU to read; GXCopyDisp is the one exception, classified at the end of this file), FIFO setup, the indexed matrix loads (they read the matrix array
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
GX_WORKER_KIND(0x8028ED7Cu, NoSync);     // GXInitTexObjData (meta and guest struct only)
GX_WORKER_KIND(0x8028EF78u, SelfDefer);  // GXLoadTexObj (records the bind with a copy of the object)
GX_WORKER_KIND(0x8028EE14u, SelfDefer);  // GXLoadTexObjPreLoaded (runs the LoadTexObj body)

// Display lists (gx_dl.cpp): the list's bytes are read at replay, as the GPU reads them late on
// hardware and the decode thread already does.
GX_WORKER_KIND(0x80291458u, Queue);      // GXCallDisplayList

// GXInvalidateTexAll (gx_texture.cpp): its whole body is aurora's GXInvalidateTexAll, one
// GX_LOAD_AURORA_INVALIDATE_TEX_ALL write into the FIFO, so it qualifies for Queue (void, no
// arguments, touches only GX state, and the replay keeps it in order with the draws and loads
// around it). Held behind the invtexq experiment until hardware has shown it is identical.
GX_WORKER_KIND(0x8028F198u, QueueIfAsked);  // GXInvalidateTexAll

// KPADRead (kpad.cpp): not a GX call at all. It reads the controller (the HID sample, the
// recorder, the menu and startup-cover input filters, the input bindings) and writes the guest's
// KPADStatus; nothing in it reads or writes GX or Aurora state, so the records pending at it lose
// nothing by staying pending. With dcnosync and invtexq on it is the one drain left in a frame at
// stock (8-11 ms, the worker's end-of-frame tail), and SPM reads the pad after drawing, so letting
// it pass lets the game thread run on to the next sync point (GXCopyDisp at the latest) while the
// worker finishes. Held behind the kpadnosync experiment until hardware shows where the wait goes.
GX_WORKER_KIND(0x802DE7B4u, NoSyncIfAsked);  // KPADRead
// The rest of the per-frame input reads (wpad.cpp), under the same switch: with KPADRead alone
// relaxed, hardware showed its whole wait move to the next of these (WPADIsDpdEnabled), so they
// go together. Each only reads the controller state or answers a constant; none touches GX.
// The rarer WPAD calls that complete a guest callback (Init, SetDataFormat, ControlMotor,
// GetInfoAsync, StopSimpleSync) stay Inline.
GX_WORKER_KIND(0x802CCBB4u, NoSyncIfAsked);  // WPADGetStatus
GX_WORKER_KIND(0x802D0874u, NoSyncIfAsked);  // WPADGetDpdSensitivity
GX_WORKER_KIND(0x802D087Cu, NoSyncIfAsked);  // WPADIsDpdEnabled
GX_WORKER_KIND(0x802D97B0u, NoSyncIfAsked);  // WUDGetStatus
GX_WORKER_KIND(0x802CE3CCu, NoSyncIfAsked);  // WPADGetDataFormat
GX_WORKER_KIND(0x802CE208u, NoSyncIfAsked);  // WPADProbe

// Data-cache ops (os_cache.cpp). NoSync so the dispatch layer leaves the drain to their body,
// DcRangeOp, which drains first exactly as an Inline call would, unless the dcnosync experiment
// is on: then only an op over a screen copy drains (gx_worker.h has the reasoning).
GX_WORKER_KIND(0x8026D7D4u, NoSync);     // DCInvalidateRange
GX_WORKER_KIND(0x8026D800u, NoSync);     // DCFlushRange
GX_WORKER_KIND(0x8026D830u, NoSync);     // DCStoreRange
GX_WORKER_KIND(0x8026D860u, NoSync);     // DCFlushRangeNoSync

// The display copy (gx_copy.cpp). NoSync because the body does its own recording now: frame
// overlap phase 2 splits GXCopyDisp so only its Aurora half becomes a record (the payload is
// the guest address and the clear flag; the host pointer is resolved at replay), the record
// replays in ring order while the game thread runs on, and PresentFrame's latch
// (GxWorker::WaitForFrameMark) waits for the newest frame's copy before the frame seals.
// GXDrawDone is not part of the record: Aurora's GXCopyDisp waits for the decode thread
// itself (fifo::sync, then drain_if_pending, which is the same finish drain), so the copy
// alone is complete. The dispatch drain this classification removes was the point: it ran
// the whole frame's records on the game thread at every frame end.
GX_WORKER_KIND(0x8028DFA0u, NoSync);     // GXCopyDisp

#undef GX_WORKER_KIND
