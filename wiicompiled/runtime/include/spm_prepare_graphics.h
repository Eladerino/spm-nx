#pragma once
// Prepare graphics: the player's own console builds the game's shaders once, ahead of play.
//
// Nothing game-derived ships with the port (PROVENANCE.md, "Pipeline cache seed: removed"), so
// a new install meets every effect for the first time while playing, and each first meeting
// stutters while its shaders compile. This walks every room in the Debug warp list
// (spm_warp_rooms.inc) and waits in each until Aurora has nothing left to compile. What gets
// built lands in the caches that already persist on the SD card (UserData/Cache/pipeline_cache.db
// and the Switch's dksh_cache.bin), so the next start prewarms all of it.
//
// The save in play is only a vehicle: the save folder is copied before the first warp and put
// back afterwards, and the game goes back to the title screen at the end, dropping whatever the
// warps changed in memory. Progress is kept in UserData/PrepareGraphics/, so a sweep that is
// stopped (hold B) or cut short carries on from the next room.
#include <cstdint>
#include <string>

struct CpuContext;

namespace SpmPrepareGraphics {

// Game thread, once per presented frame (SpmMapSweep::Tick).
void Tick(CpuContext* cpu, uint32_t presentedFrames);

// Menu: begin now when a save is loaded and Mario is in a map, otherwise as soon as he is.
void Start();
// Ends a running sweep at the next check (it keeps its place), or stops waiting for a save.
void Stop();
// The stop button's state, from the input filter while a sweep runs: held long enough, Stop().
void NoteStopButton(bool held);

struct Status {
    bool running = false;   // warping; the game's input is blocked
    bool waiting = false;   // asked for, starts once a save is loaded
    bool complete = false;  // every room done once (it can be run again)
    int place = 0, total = 0;
    int minutesLeft = -1;   // -1 until the pace is known
    float stopHold = 0.0f;  // 0..1 of the hold that stops it
    std::string room;
    std::string message;    // the last end message, shown for a while after the sweep ends
};
Status GetStatus();

// The one-time offer when a player first reaches a map without a finished sweep.
bool ShouldOffer();
void MarkOffered();

}  // namespace SpmPrepareGraphics
