// gamertags.h
//   Gamer tags (Config::gamerTags): the riders' names over their heads. Nothing read from the game but a
//   file: the replay it records as it plays (demos/default.mxdemo in its personal folder, with its own
//   "Record Demo" setting on) starts with every rider's name and number, and goes on with where each one
//   is. Who a drawn rider is: the one who has been everywhere that rider was seen over the last second
//   or so. The file is read and the riders told apart on a thread of its own, at the lowest priority; the
//   game's thread only follows the riders it draws from frame to frame (a few microseconds).
#pragma once

#include <cstddef>

namespace mxcam {
namespace gamertags {

// The riders drawn this frame (each one's hips, in the world, game units), once a frame while the tags
// are on; on false lets the file go. now: seconds, any steady clock. skins: the texture each is drawn with
// (0 not known), own: which of them is yours (-1 not known): both only help.
void Frame(double now, bool on, const float (*riders)[3], const unsigned* skins, int count, int own);

// F11: MXS_REMASTERED_tags.txt - each rider drawn and who in the recording was near it (written by the
// thread, a moment later; only while the tags are on).
void Report();

// The loaded track's width (game units; 0 unknown): the file's positions are shares of it.
void SetWorldSize(double size);

// The rider drawn as riders[i] in the last Frame: its name ("" not known, or it's you), and how far its
// tag has faded in (0..1). lift: the caller's own, kept with the rider.
struct Tag {
    const char* name;
    float       fade;
    float*      lift;
};
Tag Of(int i);

// For tests: the recorded file's bytes as they're written, and the matching, without the thread.
void TestStart(double worldSize);
void TestFeed(const char* bytes, size_t count, double now);
void TestMatch(double now);
int  TestSlotOf(int i);  // the rider riders[i] is taken for (its place in the file's list), -1 none
int  TestOwnSlot();
unsigned TestTrackOf(int i);  // which rider followed from frame to frame it is (0 none)

}  // namespace gamertags
}  // namespace mxcam
