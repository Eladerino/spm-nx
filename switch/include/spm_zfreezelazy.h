// zfreezelazy: lazy materialization of the CP zfreeze coplanar reference plane
// (adopted for v0.5.0). While it is on, a zfreeze-off draw records its coplanar
// reference inputs instead of transforming them, and aurora_zfl_materialize()
// runs the deferred math just before a read point consumes the plane
// (aurora/command_processor.cpp). update_zfreeze_reference recomputes the
// coplanar depth plane on every zfreeze-off draw, but the plane is read only
// at frozen-draw pipeline-config builds (gx.cpp), frozen-draw uniform bakes
// (shader_info.cpp) and vcache list boundaries (the state digest and the end
// capture in command_processor.cpp). Measured on the counter build, the reads
// were zero in the priced 3D scenes: the eager transform was dead work there.
// The state lives in command_processor.cpp; the switch is forwarded from
// dk_diag by the deko3d diag layer (aurora_zfl_set_lazy).
#pragma once

#include <cstdint>

#if defined(__SWITCH__)
extern "C" void aurora_zfl_set_lazy(bool on);  // dk_diag zfreezelazy (default on)
extern "C" void aurora_zfl_materialize();      // run pending lazy records before consuming the plane
extern "C" void aurora_zfl_clear();            // drop pending records (a restore/reset superseded them)
extern "C" void aurora_zfl_snapshot(uint64_t* records, uint64_t* resFailures, uint64_t* materializations,
                                    uint64_t* matSolves, uint64_t* collapses, uint64_t* collapseSolves,
                                    uint64_t* writes, uint64_t* ringHighWater);
#else
// Hosts: the mechanism is inert.
inline void aurora_zfl_set_lazy(bool) {}
inline void aurora_zfl_materialize() {}
inline void aurora_zfl_clear() {}
inline void aurora_zfl_snapshot(uint64_t* records, uint64_t* resFailures, uint64_t* materializations,
                                uint64_t* matSolves, uint64_t* collapses, uint64_t* collapseSolves,
                                uint64_t* writes, uint64_t* ringHighWater) {
    *records = *resFailures = *materializations = *matSolves = *collapses = *collapseSolves = *writes =
        *ringHighWater = 0;
}
#endif
