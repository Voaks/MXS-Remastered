// postfx.h
//   Shaders for MX Simulator without touching the game: the finished 3D image is copied (colour
//   and depth) right before the HUD is drawn over it, reworked by a chain of fullscreen passes of
//   our own, and written back. Everything the passes need about the scene comes from the GL calls
//   the hooks already see: the camera (the terrain's u_modelview), the projection (any perspective
//   matrix upload), the sun (u_light_position) and the moment the terrain pass ends (for a mask of
//   the ground). See postfx.cpp for the passes.
#pragma once

namespace mxcam {
namespace postfx {

// ---- What the hooks tell it ----------------------------------------------------------------

// This frame's camera: the game's view matrix and the one actually drawn with (after the helmet
// cam's correction; the same matrix when it is off). Column-major. Marks the frame as 3D.
void NoteView(const double raw[16], const double rendered[16]);

// A perspective projection the game uploaded or loaded (column-major). fromCamera: it went to
// the terrain program (preferred over any other).
void NoteProjection(const float m[16], bool fromCamera);

// The light direction the game sent to its shaders (before any change of ours), in the eye
// space of its own camera. components: 3, or 4 with w = 0 for a direction.
void NoteGameLight(const float* value, int components);

// Time of day: the light directions to upload instead of the game's (same layout as value),
// turning the game's own sun. Returns false to keep the game's.
bool OverrideLight(const float* value, int count, int components, float* out);

// Every skinned draw (riders, bikes) before it is made: its bones as uploaded (bind-pose centres,
// positions, rotations; v = R (p - c) + t) and the u_modelview it's drawn with (as the game sent
// it). A rigid draw of a skinned program comes as one bone at its origin. Your own rider is the
// one nearest the camera; the bones under it that keep turning are your bike's wheels.
void NoteSkeleton(unsigned program, const float* centres, const float* positions, const float* rotations, bool transposed,
                  int bones, const float modelview[16]);

// Every object the game draws in the 3D scene this frame (its u_modelview as sent, View * Model,
// column-major): one of them is your bike, which carries the headlight in any camera.
void NoteObject(unsigned program, const float viewModel[16]);

// The game has drawn the terrain (its program is still bound): the depth buffer now holds the
// ground and nothing else, which makes the ground mask for the dirt detail.
void TerrainPassDone();

// ---- Shadow maps: the game's own draw calls, repeated from the sun ----------------------------
// While the 3D scene is drawn, every object's draw call is drawn a second time into a depth
// texture seen from the sun (shadow map), with the same program and uniforms except its
// modelview and projection. The caller (proc_hooks.cpp) swaps those two uniforms around the
// repeat; postfx owns the sun's camera and the depth texture.

// Cheap: whether objects drawn now should cast shadows.
bool WantsShadowCasters();

// For one object: uploaded is the modelview the game's draw uses (as sent to the driver, after
// any correction), anchorEye an eye-space point the object is around (its skeleton; null: its
// origin). Gives the modelview and projection that draw it from the sun, or false when it is too
// far away to matter.
// The map is a grid of cascades (sharper near you, wider further out): cascades gets which of them
// the object can reach (bit i: cascade i); draw it into each with UseShadowCascade.
bool ShadowMatrices(const float uploaded[16], const float* anchorEye, float lightModelview[16], unsigned* cascades);
int  ShadowCascades();
// Between BeginShadowDraw and EndShadowDraw: draw into cascade i from now (its tile of the map),
// with this projection (uploaded in place of the object's own).
void UseShadowCascade(int i, float projection[16]);

// Around the repeated draw: draw into the shadow map (cleared on the first of the frame), then
// put the game's framebuffer, viewport and depth state back.
bool BeginShadowDraw();
void EndShadowDraw();

// Your bike's wheels in the shadow map. The game doesn't draw a wheel that's out of view (in first
// person the rear one is under you), so it would cast no shadow. For wheel i (0 rear, 1 front),
// asked right before the held-back casters are drawn:
//   Drawn:   the game drew it this frame; viewModel is its u_modelview as the game sent it (to
//            recognise its draw call and keep it) and program the program that drew it;
//   Missing: not drawn, but known on your bike; viewModel is the modelview to cast the kept draw
//            call with (in the eye space the frame is drawn from, like an uploaded u_modelview);
//   None:    neither. (A wheel the game hasn't drawn yet this session, like your rear one in first
//            person, has a shape of postfx's own standing in for it.)
enum class WheelCast { None, Drawn, Missing };
WheelCast OwnWheelCast(int i, unsigned* program, float viewModel[16]);

// What the parts of your bike and rider the game leaves out are placed from (proc_hooks.cpp keeps
// their draw calls while they're drawn and casts them when they aren't: in first person the game
// draws neither your helmet nor your rear wheel and swingarm). World transforms (game units,
// column-major), asked right before the held-back casters are drawn.
struct OwnAnchors {
    bool   bike = false, torso = false;  // found this frame
    double bikeWorld[16] = {};           // your bike's model
    double bikeAcross[3] = { 1, 0, 0 };  // across your bike, in its own space (the wheels' axles)
    double torsoWorld[16] = {};          // your rider's torso bone (it carries the head)
    double riderPosition[3] = {};        // the middle of your rider and bike
    double hips[3] = {};                 // your rider's hips bone (every piece of your rider is drawn with it)
    double raw[16] = {};                 // the game's view this frame, and the one drawn with
    double rendered[16] = {};
    double unitsPerMetre = 3.2808;
    bool   thirdPerson = false;          // the camera is metres away from your rider
};
bool GetOwnAnchors(OwnAnchors& out);

// For the look keys in third person (they swing the camera round your rider): true while the
// game's camera is in third person and your rider was seen last frame, with the middle of your
// rider and bike in that frame's raw camera eye space (the chase camera follows you, so it holds
// for this frame's camera too).
bool OwnRiderForLook(double inEye[3]);

// The game drew wheel i's own draw call (kept while it was yours) near your rider this frame: no
// stand-in tire goes in the shadow map beside it (0 rear, 1 front).
void NoteWheelDrawnByGame(int i);

// Something of the game's own was cast this frame in place of a stand-in shape: a part kept on your
// torso (your helmet; piece = false: no ball for your head), or a body piece of your rider's
// (piece = true: no torso shape).
void NoteRiderPartCast(bool piece);

// How many of your rider's own body pieces the game drew this frame (asked right before the held-back
// casters are drawn). In its first person view the game draws 3 (no torso, no helmet: shapes of
// postfx's own stand in for them in the shadow map); in third person, or with the camera close behind
// or beside your rider, all of them (8), and then no stand-in is cast inside the real ones.
void NoteRiderPiecesDrawn(int pieces);

// Your wheels this frame, for the trace (F11): per wheel (rear, front) found, its part seen, the part
// far from where the wheel sits, its axle on your bike (x, y, z); the tire dirt's stretch; bike found.
const char* WheelTraceText();

// Tire dirt goes only where the game drew your wheel: each frame your wheel's own draw call is
// drawn again, as the game drew it, into a depth mask of ours, and the dirt is put only on what the
// picture shows of it there (never on the ground, someone else's bike, or your leg in front of it).
// Cheap: whether the draws near the camera should be collected for it (they find your wheels'
// draw calls for the tire dirt, drawn by the bike shader itself: proc_hooks PaintTireDirt).
bool WantsWheelMask();
// WEATHER: the wet shine the bike shader gives everything it draws in the rain (proc_hooks
// PaintWet), as last worked out (the frame before). false: dry (no rain, or no shaders).
struct WetLook {
    bool  valid = false;
    float wet = 0.0f;                      // 0..1 Config::wetness
    float shine = 0.0f;                    // how strongly it reflects (Config::rainReflections)
    float sky[3] = {}, ground[3] = {};     // what it reflects above and below, in the game's colours
    float up[3] = { 0.0f, 1.0f, 0.0f };    // the world's up in the drawn view (eye space)
};
bool GetWetLook(WetLook& out);
// This frame's camera (the game's view), once the 3D scene has started.
bool FrameRawView(double raw[16]);
// The one it's drawn with (after the helmet cam's correction: an uploaded u_modelview is
// View * Model with it), and the world's up (unit).
bool FrameRenderedView(double rendered[16], double up[3]);
// Wheel i (0 rear, 1 front) drawn by the game this frame and taken for yours: every part of it that
// turns about its axle (the tire, rim and spokes, the brake disc, the sprocket: the one followed can
// be any of them, and the mask was once just the disc), each one's u_modelview as the game sent it
// (to find its draw calls among this frame's, all programs) and its axle in its own model (the point
// that stays put as it turns: the model's origin swings round). Returns how many (up to max).
constexpr int kWheelMaskParts = 6;
int WheelMaskDraws(int i, float viewModels[][16], double axleLocals[][3], int max);
// Around drawing them again into the mask (cleared on the first of the frame).
bool BeginWheelMask();
void EndWheelMask();

// Cheap: whether the game's lens flare (and its sun sprite) should be hidden: a time of day is
// set, so the game's sun is in the wrong place, and at night there's no sun to flare at all.
bool HidesGameFlare();

// "TIRES: ..." for the settings panel: whether your bike's wheels were found (empty: tire marks and
// tire dirt are off).
const char* TireStatusText();

// A new track was loaded: the game sent its heightmap a new u_spacing / u_heights_dim_inv (proc_hooks
// sees it, and terrain.cpp forgets its map of the ground at the same moment). The ground everywhere
// is another track's, so everything the marks learnt about it is forgotten: the map of the ground
// around you, the cache of your ruts (it re-presses them wherever ground is reused, so kept, they'd
// be laid where they were never made), the heights your tires recorded and the terrain read off the
// screen.
void MarksNewTrack();

// Cheap: whether draw calls should be checked for the start of the HUD.
bool WantsSceneEnd();

// For the trace (F11): the effects were already applied this frame (the HUD taken as started), and a
// 3D object is drawn now - too late for the shadow map and the effects. And a rider's piece near the
// camera not collected (the list was full).
bool SceneApplied();
void NoteDrawAfterScene(unsigned program);
void NoteRiderDrawDropped();
// For the trace: your rider's middle in the eye space the frame is drawn from (false: no rider), and
// what became of a draw of the game's right around it (your bike, your rider): 0 seen, 1 cast into
// the shadow map straight away, 2 left out as too big / overhead, 3 out of the map's reach, 4
// see-through, 5 held back (into the shadow map later, with the others).
bool OwnRiderEye(float out[3]);
void NoteNearCast(int outcome);
// The same for every draw offered to the shadow map (the trace's sh_all_* column): 0 a 3D object
// that writes depth, 1 cast straight away, 2 too big / overhead, 3 out of reach, 4 see-through, 5
// held back, 6 not a 3D object, 7 depth writes found back on (the game turned them on some other
// way than glDepthMask: taken as solid).
void NoteCast(int outcome);
// How far from the camera (m) a 3D object offered to the shadow map is (sh_seen_m_* in the trace).
void NoteCastDistance(double metres);

// Whether a point (in the eye space the frame is drawn from) is within reach of any rider: one of last
// frame's (a bike can be drawn before its rider). For the shadow map: what's with the riders - their
// bikes, their helmets - is cast as the game draws it, never held back.
bool NearAnyRiderEye(const float eye[3], double metres);

// For the trace: a held-back or kept draw repeated into the shadow map (skinned: with bones, a rider's
// piece; else rigid): 0 drawn as it was kept, 1 skipped (its vertex array was given other indices
// since), 2 drawn though its vertex array was re-pointed since it was kept (it then draws whatever
// that points at), 3 skipped (its bones weren't found).
void NoteReplay(bool skinned, int outcome);

// Your rider's hips in the eye space the frame is drawn from (false: no rider). And: pieces of your
// rider the game left out (your body, in first person) drawn into the shadow map right along with a
// piece it did draw (your arms, your legs), with the game's own state for your rider - the torso's
// shape then steps aside this frame.
bool OwnHipsEye(float out[3]);

// Whether the game's camera sat at your rider's eyes last frame (first person: the game leaves your
// helmet and body out). Only then is anything of your rider cast in their place.
bool CameraAtYourHead();

// For the trace (F11): CPU time spent casting draws into the shadow map as the game draws them
// (added up over the frame), and in watching every bike's wheels (lobby_wheels FrameEnd).
void NoteCastNowMs(double ms);
void NoteLobbyMs(double ms);

// For bug reports (F11): the tire marks map around you, saved as MXS_REMASTERED_marks.bmp next to
// the trace at the next frame.
void RequestMarksDump();
void NoteRealBodyDrawn(int pieces);

// The HUD is about to be drawn (why: for the log). Applies the effects now.
void SceneEnd(unsigned program, const char* why);

// Right before the buffer swap: applies the effects if SceneEnd didn't, then starts a new frame.
// apply = false only starts a new frame.
void FrameEnd(bool apply = true);

// One line for the settings panel.
const char* StatusText();

// Your speed (m/s), from your rider's movement (for the dab indicator). False when your rider
// hasn't been seen lately.
bool RiderSpeed(double& speed);

}  // namespace postfx
}  // namespace mxcam
