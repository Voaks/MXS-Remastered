// gamelog.h
//   The game's own log (lastlog.txt in its personal folder), read as it grows: what it says the game did
//   that nothing it draws shows. Only the game's file is read.
//     "Resetting"        a track loaded or loaded again, a rejoin, a new race: the game deallocates
//                        and loads the bikes again (on the same track, not the track itself)
//     "Loading terrain"  a track's ground loaded
#pragma once

#include <string>

namespace mxcam {
namespace gamelog {

// Once a frame (whatever is on): twice a second, the whole lines the game added since - what was there
// when the mod first looked is left alone - handed on (Handle).
void Poll(double now);

// New text of the game's log: to the lap timer (laptimer::NoteGameLog), and on a reset or a track
// loading, the tire marks start over (postfx::MarksNewTrack: reloading the same track kept its ruts).
void Handle(const std::string& text);

}  // namespace gamelog
}  // namespace mxcam
