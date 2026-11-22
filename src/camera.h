// camera.h
//   The helmet-cam physics. Fed the game's raw view matrix once per frame, it returns an
//   eye-space correction matrix C such that  C * RawView == DynamicView.
//
//   Because C is applied in eye space, the same C is correct for every object drawn that
//   frame:  C * (RawView * Model) == DynamicView * Model.  That's what lets the hooks apply
//   it to every modelview load without knowing which object is which.
#pragma once

namespace mxcam {
namespace camera {

// Last frame's internals, for the frame dump and the trace.
struct Telemetry {
    double dtMs              = 0;  // the step the filters took (smoothed frame interval)
    double rawIntervalMs     = 0;  // the last measured swap-to-swap interval
    double lagDeg            = 0;  // smoothed view vs target
    double rawRollDeg        = 0;  // bike (or game camera) roll relative to the horizon
    double rollCorrectionDeg = 0;  // final view roll minus the game camera's roll
    double pitchExcessDeg    = 0;  // bike pitch away from its resting pitch (wheelie > 0)
    double viewPitchDeg      = 0;  // final view pitch (+ = up)
    double lookYawDeg        = 0;  // looking left / right / back: the game camera vs the bike, plus the d-pad's head turn
    double airYawDeg         = 0;  // air looseness: turn applied to the view in the air
    double apexDeg           = 0;  // look to apex: the head's turn into the corner (+ = right)
    double airborne          = 0;  // 0..1
    bool   bikeTracked       = false;
    bool   bikeFresh         = false;  // the bike's own world pose was used (not carried via the camera)
    bool   customActive      = false;
    double customWeight      = 0;
    bool   cut               = false;  // the raw camera snapped this frame (look key, camera switch)
    bool   reset             = false;  // every filter started over this frame
    double verticalAccelG    = 0;
    double fallG             = 0;      // vertical acceleration used for airborne detection
    double whip              = 0;      // 0..1 how far whip lock holds the view
    double bikeRestDeg       = 0;      // the bike's resting pitch, learnt
    double cameraRestDeg     = 0;      // the game camera's resting pitch, learnt
    double leanAlong         = 0;      // lean limiter: the game camera along the bike from its usual place (units, + = forward)
    double leanPitchDeg      = 0;      // ...and pitched on the bike from its usual angle (+ = up)
    double leanShift         = 0;      // ...how far the eye was moved back along the bike for it (units)
    double neckOffset        = 0;  // spring displacement along WorldUp (negative = compressed)
    double verticalOffset    = 0;  // total eye height change (smoothing + neck)
    // Attitudes in degrees: yaw (heading about WorldUp), pitch, roll.
    double rawYaw = 0, rawPitch = 0, rawRoll = 0;     // game camera
    double bikeYaw = 0, bikePitch = 0, bikeRoll = 0;  // bike (0 when not tracked)
    double outYaw = 0, outPitch = 0, outRoll = 0;     // our view
    double rawPos[3] = {};                            // game camera position
    double eyeOffset[3] = {};                         // our eye minus the game camera's
};

// Forget all filter history; the next Update() starts from the raw camera again.
void Reset();

// Where the rider is asked to look (the controller's d-pad; the game has no look keys). The
// head turns there over ~0.2 s, on top of everything else. Set before each frame is drawn.
enum class Look { Ahead, Left, Right, Back };
void SetLook(Look look);

// Called once per presented frame (at the buffer swap). Measures the frame interval; all
// filters step by a smoothed version of it, so timing jitter can't make the view judder.
void FrameTick();

// Time the content of the frame being drawn belongs to (the previous swap), in seconds, and
// the smoothed frame interval.
double ContentTime();
double FrameInterval();

// The game's clock as of the last Update(), in seconds: whole physics steps (kGameTick) counted
// from how far things moved, where that can be told; the time between frames otherwise. Stamp
// motion samples with it; it drifts from ContentTime(), so don't compare the two.
double GameTime();

// MX Simulator steps its physics 128 times a second, and every frame shows a whole number of
// those steps: in a trace at ~124 fps, one frame in 35 showed two, whatever the frame times
// said (they wobble by +-1.5 ms; a 52 ms hitch still showed one step). A frame is never
// counted as showing none: see FinishFrame.
constexpr double kGameTick = 1.0 / 128.0;
// Below this much travel per step (units) the steps can't be counted from the travel.
constexpr double kMinStepTravel = 0.03;

// Counts the frames Update() has stamped on the game's clock. The bike tracker, which sees the
// bike only after Update(), counts the steps of the frame it was on from the bike's own travel
// (rigid, unlike the sprung game camera's) and, when the step count differs from Update()'s,
// sets it with SetLastStep(): it moves the game's clock and the camera's last motion sample,
// and returns the corrected GameTime().
unsigned MotionFrame();
double SetLastStep(double seconds);
double LastStep();  // how far the last frame moved the game's clock (s), for the trace

// view: the game's raw world->eye matrix (column-major). Returns false - leaving the frame
// untouched - if it isn't a rigid camera transform (e.g. the wrong load was picked).
// Advances every filter by one frame.
bool Update(const double view[16], double correction[16]);

// Works out, without advancing anything, the rotation-only correction this frame will get if
// `view` carries this frame's raw camera rotation (the sky's rotation-only view is drawn before
// the camera position is known). Returns false - out untouched - if view isn't a rotation.
bool PreviewRotation(const double view[16], double out[16]);

// Angle in degrees between view's rotation and the last raw camera rotation (0 before the
// first frame, -1 if view isn't a rotation). The caller uses it to tell the sky (within a few
// degrees) from other rotation-only draws before calling PreviewRotation().
double AngleToLastCameraDeg(const double view[16]);

// Rotation-only correction of the last Update().
bool RotationCorrection(double out[16]);

// With the first person mod off, in place of Update(): only the look keys (SetLook) turn the
// game's own camera, about WorldUp, the eye left where the game put it. correction receives the
// eye-space rotation (it has no translation: it is the rotation-only correction too). False -
// the frame left untouched - while the head looks straight ahead, or if view isn't a camera.
bool LookOnlyUpdate(const double view[16], double correction[16]);
// The same for the sky, drawn before the camera, without advancing anything; and the angle to
// the last frame's game camera, as PreviewRotation() / AngleToLastCameraDeg() for Update().
bool LookOnlyPreview(const double view[16], double out[16]);
double LookOnlyAngleToLastDeg(const double view[16]);

// How far the look keys turn the view this frame (rad, + = right), as of the last Update() or
// LookOnlyUpdate(): their share of the turn about WorldUp (look to apex not included).
double LookTurn();

// Third person: moves the eye of correction (C, eye space: C * view is what's drawn, already turned
// by LookTurn() about WorldUp) round the vertical through rider (a point in view's eye space) by
// the same turn, so the camera swings round your rider instead of turning where it stands: looking
// back it's out in front of you, facing you, and you stay where you were on screen. The rotation
// (and so the sky's) is unchanged.
void OrbitLook(const double view[16], const double rider[3], double correction[16]);

const Telemetry& LastTelemetry();

}  // namespace camera
}  // namespace mxcam
