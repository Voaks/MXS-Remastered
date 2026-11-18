// lobby_wheels.h
//   Every bike's wheels in the scene, whatever the model, for tire marks on every bike in a lobby
//   (Config::marksEveryBike): a wheel is a rigid draw whose copies keep turning about one fixed axis
//   of their own, from frame to frame, as they move (in a replay of 22 YZ450s: the two wheel meshes,
//   turning 87-100% of the frames about the same axle every time, against 1-8% about axes that kept
//   changing for every other part of the bikes). Each wheel mesh's axle and tire radius come from its
//   own vertices, so each copy's contact with the ground is known, and its path from frame to frame
//   is handed to the tire marks as segments.
#pragma once

#include <vector>

namespace mxcam {
struct DrawCall;
namespace lobby {

// A rigid 3D draw of the frame being drawn: its program, its mesh (vertex array, element buffer,
// the draw's range) and its View * Model as the game sent it. True when this mesh is known to be a
// wheel but its shape isn't known yet: measure the draw and pass its bounds to GiveBounds.
bool NoteRigidDraw(unsigned program, unsigned vao, unsigned elementBuffer, const DrawCall& draw, const float raw[16]);

// The draw's vertices' bounds in its own model's space (lo, hi); ok false if they couldn't be read.
void GiveBounds(unsigned program, unsigned vao, unsigned elementBuffer, const DrawCall& draw, bool ok,
                const float lo[3], const float hi[3]);

// Whether this draw's mesh is known to be a wheel (for the tire dirt on every bike).
bool IsWheel(unsigned program, unsigned vao, unsigned elementBuffer, const DrawCall& draw);

// Whether it's running and has found wheels already (so a mesh it doesn't know as one isn't one).
bool KnowsWheels();

// Whether this draw's mesh has been watched long enough to know it isn't a wheel (its copies seen
// frame after frame, not turning like one). A mesh only just seen, or not yet classified, isn't.
bool KnownNotWheel(unsigned program, unsigned vao, unsigned elementBuffer, const DrawCall& draw);

// Whether a mesh is a wheel or, watched only a moment so far, turns like one: in most of the frames
// it was seen, about the same axis of its own (a bike's body or a helmet never does). For the tire
// dirt: what may have mud on it.
bool SeenTurning(unsigned program, unsigned vao, unsigned elementBuffer, const DrawCall& draw);

// A wheel mesh's shape in its own model's space, once known: the middle of its bounds (on the axle),
// its axle (unit) and its tire's radius. For the tire dirt: where on the tire a point is.
bool WheelShape(unsigned program, unsigned vao, unsigned elementBuffer, const DrawCall& draw, float centre[3], float axle[3],
                float& radius);

// The frame is done: rawView is the game's view for it (null if none was found).
void FrameEnd(const double* rawView, double now);
// (For the log: a 3D draw the wheels can't be learnt from - 1: drawn with bones, 2: its geometry streamed.)
void NoteNotRigid(int why);
// How many of those lines have been logged (postfx adds its own line after each).
int LearningReports();

// A wheel's path over the ground between two frames (world, game units), for the tire marks.
struct Segment {
    double a[3], b[3];
    double radius;   // the tire's (game units)
    bool   grounded; // on the ground at both ends, by how its contact moves (not in a flight)
    unsigned id;     // which wheel (the same from frame to frame while it's followed)
};
// The segments made since the last call (moved into out).
void TakeSegments(std::vector<Segment>& out);

// This frame's wheels so far (the draws since the last FrameEnd), for a headlight on every bike:
// each copy of a wheel mesh whose shape is known, its centre and axle (unit) in the world (game
// units) and its tire's radius. rawView: the game's view this frame. (Every part that turns on an
// axle counts as a wheel - the tire, a brake disc, a sprocket: the radius tells them apart.)
struct WheelNow {
    double centre[3], axle[3];
    double radius;
    unsigned long long mesh;  // which mesh it is (the same for every copy, from frame to frame)
};
void CurrentWheels(const double rawView[16], std::vector<WheelNow>& out);

}  // namespace lobby
}  // namespace mxcam
