// laptimer.h
//   The lap timer (Config::lapTimer): your laps timed by the mod itself, from the track's own timing
//   gates (each track's timing_gates file: its start gate, its checkpoint lines and the order a lap
//   takes them in), as your bike crosses them. Nothing read from the game but the files and what it
//   draws. Which track it is: the one whose checkpoints you cross in their order (or whose start gate
//   you stand in) - every track in the game folder and the game's personal folder, loose or in .saf
//   archives, looked through once in the background.
#pragma once

#include <string>

namespace mxcam {
namespace laptimer {

// Each frame (menu open or not), with your bike's position in the world (game units) if it's known,
// and the time (seconds, any steady clock).
void Update(double now, bool have, const double position[3]);

// The game loaded another track: whatever was timed is the old one's.
void NewTrack();

// The game's pause key pressed (its own, from its settings): the game paused, or carried on. While it's
// paused the lap's clock stops, and carries on from there. (If your bike goes on moving after a press
// - pausing did nothing: an online race - it wasn't paused.)
void PauseKey(double now);

// What the game added to its own log (lastlog.txt; gamelog.h reads it as it grows): its "Resetting" - a
// rejoin, a new race, on the same track (it loads the bikes again, not the track: nothing drawn says
// so) - starts the laps and bests over; its "Loading terrain" (another track) is NewTrack.
void NoteGameLog(const std::string& text);

// The loaded track's heightmap spacing (the game's u_spacing; 0 unknown): only tracks whose own
// (terrain.hf) is the same are looked for. Stadium tracks sit in the same few hundred metres: their
// checkpoints lined up in order by chance and the timer jumped from track to track.
void SetTerrain(double spacing);

// What the panel shows.
struct Display {
    bool        track = false;     // a track's gates are known (else only placeholders)
    std::string trackName;
    int         lap = 0;           // the lap being timed (1 the first), 0 none
    double      current = -1.0;    // this lap so far (seconds; or, for a moment after it ends, its time), -1 none
    double      predicted = -1.0;  // where this lap is heading: your best plus the gap at the last checkpoint, -1 none
    int         split = 0;         // checkpoints passed this lap
    bool        haveGap = false;
    double      gap = 0.0;         // seconds behind (+) or ahead (-) of your best lap at the last checkpoint
    bool        finished = false;  // showing a lap just finished
    bool        valid = true;      // no timing gate missed (running: one is - the Gate shown amber; finished:
                                   // the lap ended over the finish line with one missed - an invalid lap time)
    bool        paused = false;    // the game paused: the clock stopped
    double      best = -1.0;       // your best lap of this kind (seconds), -1 none
    int         gates = 0;         // timing gates in this lap
};
Display Current(double now);

// For tests: the tracks from these timing_gates texts only (none read from disk); spacings: their
// heightmaps' (terrain.hf), or null.
void UseTracksForTest(const char* const* names, const char* const* texts, int count, const double* spacings = nullptr);

}  // namespace laptimer
}  // namespace mxcam
