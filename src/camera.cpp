// camera.cpp
//   Per-frame pipeline. Every filter steps by the smoothed frame interval (FrameTick), so a
//   frame that happens to be measured a millisecond long or short doesn't make the view judder.
//   Orient - everything about where the view points. It only needs the raw camera rotation,
//            so it can also be *previewed* from the sky, which is drawn before the camera
//            position is known. A preview works on a copy of the filter state and changes
//            nothing; only Update() (the terrain, once per frame) advances the filters. So an
//            odd rotation-only draw can't step or reset them, and the sky always gets exactly
//            the rotation the rest of the frame gets.
//     1. decompose       - raw view matrix -> head orientation (quaternion)
//     2. cut detection   - camera switches / look-key snaps skip the smoothing for one frame
//     3. attitude        - heading / pitch / roll of the game camera and, when the tracker has
//                          found it, of the rider's bike (bike.cpp)
//     4. target view     - game camera:   the game's view with pitch lock and horizon lock
//                          custom camera: a view built from the bike alone (the game's 1st
//                                         person angle and gimbal play no part), with the
//                                         locks, our own look-down angle and the game's
//                                         look-left / right / back passed through;
//                          both: air looseness (the nose's swing filtered in the air),
//                                head position offsets
//     5. smoothing       - exponential slerp toward the target, with a lag limit; head nod
//   FinishFrame - where the eye is.
//     6. motion          - velocity and acceleration of the eye (least-squares fit, motion.h);
//                          airborne detection
//     7. neck physics    - vertical low-pass + spring-damper driven by vertical acceleration
//     8. rebuild         - new view matrix, and correction C = NewView * inverse(RawView)

#include <windows.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>

#include "bike.h"
#include "camera.h"
#include "config.h"
#include "log.h"
#include "mathlib.h"
#include "motion.h"

namespace mxcam {
namespace camera {
namespace {

// Learns the angle something rests at (the bike's pitch on flat ground, the game camera's
// look-down angle) as the peak of a histogram that forgets over ~30 s of riding. Wheelies, jump
// faces and landings are brief, so they barely move the peak.
// weight: how much a sample counts, and how far it makes the histogram forget. Riding counts in
// full; standing still or lying down only a little (kRestStillWeight): riding spreads its time
// over a range of pitches, while a bike lying still after a crash piles all of its into one bin -
// 4 s of it at 25 deg took the rest from 3.7 to 20 deg for 20-30 s of riding after (replayed from
// a trace), the view pitched by half that. A little, so a learner with nothing better (just
// started, at the gate) still finds the rest from there.
class RestEstimator {
public:
    void Add(double degrees, double dt, double weight = 1.0)
    {
        if (!seeded_) {
            value_ = degrees;
            seeded_ = true;
        }
        if (dt <= 0 || weight <= 0)
            return;
        const double decay = std::exp(-dt * weight / 30.0);
        for (double& bin : bins_)
            bin *= decay;
        const double position = Clamp(degrees, -kRange, kRange) + kRange;
        const int    i = position >= kBins - 1 ? kBins - 2 : static_cast<int>(position);
        const double f = position - i;
        bins_[i]     += dt * weight * (1.0 - f);
        bins_[i + 1] += dt * weight * f;
        riding_      += dt * weight;

        int peak = 0;
        for (int b = 1; b < kBins; ++b) {
            if (bins_[b] > bins_[peak])
                peak = b;
        }
        double offset = 0;  // parabolic refinement between the 1-degree bins
        if (peak > 0 && peak < kBins - 1) {
            const double a = bins_[peak - 1], b = bins_[peak], c = bins_[peak + 1];
            const double denominator = a - 2.0 * b + c;
            if (std::fabs(denominator) > 1e-12)
                offset = Clamp(0.5 * (a - c) / denominator, -0.5, 0.5);
        }
        const double mode = peak + offset - kRange;
        // Eased, once there's something to go on; until then (just started, nothing in the ini)
        // straight to the peak, so a first frame at the spawn doesn't linger for seconds.
        value_ += (mode - value_) * (1.0 - std::exp(-(riding_ < 1.0 ? 30.0 : 0.7) * dt));
    }
    double Degrees() const { return value_; }

    // Starts from what was learnt in an earlier session, as if seen for `seconds` of riding.
    void Seed(double degrees, double seconds)
    {
        value_ = degrees;
        seeded_ = true;
        const double position = Clamp(degrees, -kRange, kRange) + kRange;
        const int    i = position >= kBins - 1 ? kBins - 2 : static_cast<int>(position);
        const double f = position - i;
        bins_[i]     += seconds * (1.0 - f);
        bins_[i + 1] += seconds * f;
    }
    // Seconds of riding (weighted) it has learnt from in this session.
    double Riding() const { return riding_; }

private:
    static constexpr int    kBins  = 121;  // -60..+60 degrees
    static constexpr double kRange = 60.0;
    double bins_[kBins] = {};
    double value_  = 0;
    double riding_ = 0;
    bool   seeded_ = false;
};

// What counts as riding on flat ground for the rest estimators: moving faster than this (units/s,
// about 18 km/h), the bike within this far of upright and of level (a bike rests at a few degrees;
// a face, a hill or a wheelie isn't flat ground). Anything else counts kRestStillWeight. Without
// the pitch limit a session learnt the bike's rest as 36 deg while "riding", and kept it in the
// ini: the next session's view sat 17 deg low until ~100 s of standing still outweighed it.
constexpr double kRestMinSpeed    = 5.0;
constexpr double kRestMaxRollDeg  = 60.0;
constexpr double kRestMaxPitchDeg = 20.0;
constexpr double kRestStillWeight = 0.05;
// The rests are kept in the ini ([Learned] RestPitch: bike, game camera, game angle) once this
// much riding is behind them, at most this often, and start the next session counting as
// kRestSeedSeconds of riding: right from the first frame at the gate, and soon outweighed.
constexpr double kRestSaveAfter   = 20.0;  // s of riding
constexpr double kRestSaveEvery   = 60.0;  // s
constexpr double kRestSeedSeconds = 5.0;

// Landing (see FinishFrame): an impact, vertical acceleration this many g above resting...
constexpr double kImpactG  = 0.8;
// ...or the eye alone out of free fall for longer than a whip swings the head (s).
constexpr double kSwingTime = 0.5;
// In the air the direction of travel can't change. Turning faster than this (deg/s) is a
// corner, whatever the vertical acceleration says: a bike laid over in a rut drops the eye
// and bike as it leans in, which reads as free fall (85 deg of lean, 100 deg/s, seen in a
// trace). Real jumps in the same trace stayed under 26 deg/s, whips included.
constexpr double kAirTurnRateDeg = 40.0;
// Whip lock is stricter, since it takes the view off the bike: the bike's path must be nearly
// straight (a downhill corner read as 0.25 s of free fall while turning 63 deg/s, but only
// crossed kAirTurnRateDeg after the lock had snatched the view; jumps stayed under 6 deg/s).
// Measured from the eye, a whip's head swing alone can read 26 deg/s, so the air limit then.
constexpr double kWhipTurnRateDeg = 20.0;
// ...and free fall must read at least this fraction of what it reads in the air. MX Simulator's
// airtime reads about -3 g in this mod's units; a bump in a slight turn read -0.9 g for 65 ms
// and snatched the view, while every real takeoff passed -1.5 g within 10 ms.
constexpr double kWhipFallFraction = 0.5;
// ...and only for a real jump. Whoops and rollers leave the ground too, for 0.2-0.6 s, reading
// as deep as airtime; locking the view there swings it from the bike's nose to its path (they
// differ by the slide through the section) and back on landing. The size of a jump is set at
// the lip: the airtime its launch speed carries on flat ground. In traces whoops launched for
// at most 0.5 s of that, real jumps for 0.8 s or more. A jump that launches flat and drops away
// (a step-down) still gets the lock once it has really been in the air this long.
constexpr double kWhipAirtimeMin  = 0.6, kWhipAirtimeFull  = 0.8;  // s, launch airtime
constexpr double kWhipFallTimeMin = 0.8, kWhipFallTimeFull = 1.2;  // s, time in the air

// Whip lock takes the direction of travel as it was over this long before free fall is detected:
// the path's own direction, averaged, not a filtered one. A filter lags a turn, and a double
// taken while turning out of the landing of the one before (20 deg in 0.3 s) was held 18 deg
// off its path, along the direction from before the turn, and swung back on landing.
// Whip lock takes the view over as it is - where it points and how fast it is turning - and
// brings it to the direction of travel, level, at the resting look-down angle, on these springs
// (1/s). It used to aim straight at that within ~0.1 s: off a steep face (the bike 44 deg up,
// the view 7 deg up at 50% pitch lock) a 13 deg snap down just after the lip. Easing only how
// far the pitch went still stopped the view dead: up a face it tilts up at ~30 deg/s, and at
// the lip that went to -10 deg/s within 40 ms (the roll from -23 to +35 deg/s), on every jump.
// Carried on, the view's turning slows over ~0.15 s, then it eases home: 90% there after ~1.3 s
// for the pitch, ~0.8 s for the heading and the roll.
constexpr double kWhipPitchOmega = 3.0;
constexpr double kWhipLevelOmega = 5.0;
constexpr double kWhipMaxRate    = 120.0 * kDegToRad;  // rad/s carried over at most
// The head's nod follows the neck on this spring (1/s); see Orient(), section 5.
constexpr double kNodOmega = 8.0;

// Lean limiter: the game camera's usual place on the bike follows it over about 4 s (1/s) on the
// ground; a lean is counted to at most 2 units (~60 cm) along the bike and 25 deg of pitch (a crash's
// tumble isn't one).
constexpr double kLeanNeutralRate = 0.25;
constexpr double kLeanMaxShift    = 2.0;
constexpr double kLeanMaxPitch    = 25.0;
constexpr double kTravelHoldBack = 0.25;  // s
constexpr int    kTravelSamples  = 128;   // frames of history: covers it up to ~500 fps
// In the air the path seen from above is a straight line, so the held direction then settles
// onto the line from where the fall began, once it has run far enough that the head's sway
// (the eye's path) can't turn it.
constexpr double kPathRunMin = 3.0, kPathRunFull = 10.0;  // units
constexpr double kPathResponse = 4.0;                     // 1/s

// Look to apex: the head turns into a corner by the path's turn rate times this (s, at 100%),
// up to about kApexMaxDeg. A point on the path t seconds ahead lies half the heading change over
// those t off the nose, so 100% looks about 1.2 s down the path. Faded out at walking pace (a turn
// on the spot isn't a corner), in the air and while the d-pad turns the head.
// The head follows on a critically damped spring: an exponential ease sets off at full speed the
// moment the corner ends, which read as a snap back. The spring's frequency is Config::apexSpeed
// times kApexSpringScale, which settles in about the time the exponential took.
// Coming back out, the head turns against the way the bike turned. While the bike is still turning
// under it that reads as the eyes holding on the exit, so it may unwind kApexReturnScale times
// faster, up to the path's own turn rate. Past that it's the view swinging back on the straight
// (it read as a snap at 15 deg/s, from 20 deg at a corner's exit in a trace), so the rest drifts
// home at no more than Config::apexSpeed times kApexDriftScale, eased in and out. Replaying that
// trace: 4 deg/s of swing instead of 15, and home 1.5 s sooner. The limit is lifted for a turn the
// other way (an S-bend) and while the d-pad turns the head.
// Anticipation: the head aims by the turn rate Config::apexAnticipation seconds ahead, from how
// fast the rate is changing (the gap between two filters of it). It turns in as a corner tightens
// and, more to the point, unwinds as it opens up, while the bike's turn still absorbs it. A
// forecast that crosses zero is held at zero, as a corner easing off forecasts the next one the
// other way. Forecasting amplifies every wobble in the line, so the filters are slow (3 and 8/s)
// and the forecast's lead is eased at kApexLeadResponse: with quicker filters (6 and 15/s) and no
// easing, a trace at 0.5 s shook the head with ~2.8x the acceleration it had with anticipation
// off. Replayed now it's within ~15% of off, with no reversals, and on the straights the head is
// turned 2.5 deg on average instead of 3.5. Only the lead is eased (not the whole aim), so how
// fast the head comes back is otherwise unchanged.
constexpr double kApexLookAhead   = 0.6;                    // s
constexpr double kApexMaxDeg      = 45.0;
constexpr double kApexSpeedMin    = 3.0, kApexSpeedFull = 8.0;  // units/s
constexpr double kApexSpringScale = 1.6;
constexpr double kApexReturnScale = 6.0;
constexpr double kApexDriftScale  = 2.5;                    // deg/s per unit of apexSpeed
constexpr double kApexOtherWayMin = 15.0, kApexOtherWayFull = 40.0;  // deg/s
constexpr double kApexQuickResponse = 40.0;               // 1/s
constexpr double kApexTrendSlow = 3.0, kApexTrendFast = 8.0;   // 1/s
constexpr double kApexLeadResponse = 4.0;                 // 1/s
// Steadiness: the spring that follows the bike's lean and pitch runs at kSteadyFastest (1/s) just
// above 0% down to kSteadySlowest at 100%, evenly on a log scale. At the default 60% that's ~7.6/s:
// it trails a lean by about a quarter of a second, while a 5 Hz jolt reaches the view at about a
// twentieth of its size (the view's own smoothing then halves that again). The same scale goes on
// up to 200% for those who want it calmer still: ~0.6/s at 150%, ~0.16/s at 200% (the view hardly
// leans into a corner at all).
constexpr double kSteadyFastest = 40.0, kSteadySlowest = 2.5;

struct TravelSample {
    Vec3   direction;  // the direction of travel on that frame
    double rise = 0;  // upward speed (units/s)
    double dt   = 0;
};

struct State {
    bool   valid        = false;  // false: the next Update() seeds every filter from the raw camera
    bool   havePos      = false;  // false: FinishFrame() seeds the position filters
    Quat   smoothRot;             // low-passed view orientation
    Quat   prevRawRot;
    Vec3   prevPos;
    Vec3   velocity;              // world-space velocity of the eye (units/s)
    Vec3   accel;                 // world-space acceleration of the eye (units/s^2)
    Vec3   airNose;               // slowly filtered nose heading in the air: bike, game camera
    Vec3   airNoseCamera;
    Vec3   travel;                // horizontal direction of travel, filtered (whip lock)
    TravelSample travelHistory[kTravelSamples];  // on the ground since the last fall
    int    travelNext   = 0;
    int    travelCount  = 0;
    bool   wasFalling   = false;
    Vec3   takeoffPos;            // where this fall began: the bike's origin, or the eye (whip lock)
    bool   takeoffFromBike = false;
    double launchRise   = 0;      // upward speed at the lip of this fall (units/s, whip lock)
    double outOfFallTime = 0;    // in the air: how long the eye has read out of free fall
    Vec3   motionHeading;         // horizontal direction of the eye's velocity, last frame
    double turnRate     = 0;      // how fast that direction turns (rad/s, filtered)
    double pathTurnRate = 0;      // the same, signed (+ = turning right)
    double pathTurnQuick = 0;     // the same, filtered less: lags less as a corner eases off
    double trendSlow = 0, trendFast = 0;  // the same at kApexTrendSlow / Fast, for anticipation
    double pathSpeed    = 0;      // horizontal speed it was measured at (units/s)
    bool   turnFromBike = false;  // turnRate is measured from the bike's path (else the eye's)
    bool   deepFall     = false;  // this fall has read as deep as real airtime (whip lock)
    double whip         = 0;      // 0..1 how far whip lock has taken over (eased)
    bool   whipEngaged  = false;  // whip lock is taking over (last frame)
    // Where whip lock holds the view (rad), easing to the travel direction, rest pitch, level:
    // the heading as a turn from the direction of travel, the pitch and the roll, with rates.
    double whipYaw   = 0, whipYawRate   = 0;
    double whipPitch = 0, whipPitchRate = 0;
    double whipRoll  = 0, whipRollRate  = 0;
    // The view as it was last frame (the smoothed view's attitude), to tell how fast it turns.
    bool   haveLastView = false;
    Vec3   lastViewHeading;
    double lastViewPitch = 0, lastViewRoll = 0;
    double airborne     = 0;      // 0 on the ground .. 1 in the air (eased)
    bool   inAir        = false;
    double fallTime     = 0;      // seconds of continuous free fall
    double lookSide     = 0;      // 0..1 how much the rider is looking sideways / back
    double lookBack     = 0;
    double headTurn     = 0;      // d-pad look: the head's turn about WorldUp (rad, + = right, eased)
    double apexTurn     = 0;      // look to apex: the head's turn into the corner (rad, + = right, eased)
    double apexRate     = 0;      // how fast it is turning (rad/s)
    double apexLead     = 0;      // anticipation: the forecast's change in turn rate (rad/s, eased)
    double smoothHeight = 0;      // low-passed eye height along WorldUp
    // Steadiness: the lean (roll) and the pitch away from rest, followed through a slow spring,
    // [0] of the game camera, [1] of the bike.
    double steadyRoll[2]  = {}, steadyRollRate[2]  = {};
    double steadyPitch[2] = {}, steadyPitchRate[2] = {};
    // The same for the view itself, which whip lock holds level: these follow only the share
    // of the lean and pitch it lets through (the head offsets keep following the bike).
    double viewRoll[2]    = {}, viewRollRate[2]    = {};
    double viewPitch[2]   = {}, viewPitchRate[2]   = {};
    bool   steadyBike     = false;  // [1] is following the bike (false: start it where the bike is)
    double neckOffset   = 0;      // spring displacement along WorldUp (negative = compressed)
    double neckVelocity = 0;
    double nod          = 0;      // the head's nod from the neck (rad, eased: kNodOmega)
    double nodRate      = 0;
    // Lean limiter: where the game camera sits along the bike and how it's pitched on it, usually
    // (averaged over a few seconds on the ground): leaning moves it from there.
    bool   leanHave     = false;
    double leanAlong    = 0;      // world units along the bike
    double leanPitch    = 0;      // rad
};

// Everything the orientation half carries from frame to frame. A preview runs on a copy.
struct Model {
    State         state;
    RestEstimator cameraPitchRest;  // game camera pitch on flat ground (includes the game's angle)
    RestEstimator bikePitchRest;    // bike model pitch on flat ground
    RestEstimator gameAngleRest;    // game camera pitch relative to the bike: the game's 1st-person angle
    bool          restLoaded = false;  // seeded from the ini (or found nothing there)
    double        restSavedAt = -1;    // content time the rests were last written to the ini
    double        restLoggedDeg = 0;   // the bike's rest when the log last mentioned it
    Vec3          lastHeading{ 0, 0, -1 };
    double        customWeight = 0; // 0 = game camera .. 1 = custom camera, eased on switches
    double        airG = -3.0;      // what free fall reads well into a jump (g), learned; MX Simulator's to start
    bike::Pose    lastPose;         // last pose the tracker reported, used while easing out
    bool          haveLastPose = false;
};

// One frame's orientation, carried from Orient() to FinishFrame().
struct Frame {
    bool   valid    = false;
    double dt       = 0;
    bool   reset    = false;
    Vec3   rawRight, rawUp, rawBack;    // raw camera axes the orientation was computed from
    Quat   rot;                         // final view orientation
    Vec3   offsetFwd, offsetUp;         // head offset axes and amounts
    double forwardOffset = 0, heightOffset = 0;
    Vec3   leanShift;                   // lean limiter: added to the eye (world)
    Telemetry telemetry;
};

// Rate-limited log for events that can repeat every frame: the first few are logged, then a
// count every ten seconds, so a problem that keeps happening stays visible in the log.
struct EventLog {
    int    logged     = 0;
    int    suppressed = 0;
    double since      = 0;
};

Model     g_model;
Frame     g_frame;
Telemetry g_telemetry;
MotionFit g_eyeFit;
EventLog  g_cutLog, g_teleportLog, g_restLog;

// D-pad look (SetLook). The head turns at Config::lookTurnSpeed.
Look g_look = Look::Ahead;

// Frame timing (FrameTick).
double g_contentTime     = 0;      // the swap that started the frame being drawn, seconds
double g_frameInterval   = 1.0 / 60.0;
bool   g_haveInterval    = false;
double g_lastCommitTime  = -1;     // content time of the last Update()
double g_lastCommitWallTime = 0;   // when it ran, to notice pauses (loading, alt-tab)
double g_rawIntervalMs   = 0;

// The game's own clock (FinishFrame), which the motion fits are stamped with. The game steps its
// physics in fixed increments (kGameTick) and now and then shows two steps in one frame - one
// frame in 35 at 124 fps in a trace - which the swap times can't see. Stamped with the swap
// time, such a frame read as a 6 g jolt and then a false landing, worst falling fast at the end
// of a big jump: whip lock let go and grabbed again, and the neck bounced. The steps were once
// counted only where the travel was a whole multiple of what the frame's time predicted, but
// the frame times wobble too much for that (a two-step frame read as 1.73 or 2.30 steps, 13 of
// 58 missed in a trace, a false landing 13 m up). They're counted against kGameTick now.
double g_gameTime        = 0;
double g_lastMotionTime  = -1;     // content time of the last FinishFrame()
double g_lastStep        = 0;      // how far the last FinishFrame() moved the game's clock (s)
unsigned g_motionFrame   = 0;      // FinishFrame()s so far

double NowSeconds()
{
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return double(now.QuadPart) / double(frequency.QuadPart);
}

void LogEvent(EventLog& e, const char* format, ...)
{
    if (e.logged >= 10) {
        ++e.suppressed;
        return;
    }
    ++e.logged;
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    Log("%s", text);
}

void SummarizeEvents(EventLog& e, const char* what)
{
    if (e.logged < 10 || g_contentTime - e.since < 10.0)
        return;
    if (e.suppressed)
        Log("camera: %d more %s in the last %.0f s", e.suppressed, what, g_contentTime - e.since);
    e.suppressed = 0;
    e.since = g_contentTime;
}

// Blend factor of a first-order low-pass with the given rate (1/s) over dt seconds.
// Frame-rate independent: two 8 ms steps give the same result as one 16 ms step.
// A rate of 0 means "no filtering" (factor 1).
double LowPassAlpha(double ratePerSecond, double dt)
{
    return ratePerSecond > 0 ? 1.0 - std::exp(-ratePerSecond * dt) : 1.0;
}

// One step of a critically damped spring pulling x (moving at v) toward target, at angular
// frequency omega (1/s). Solved exactly for a target held over the step, so any dt is stable.
// It starts and stops moving gently and never overshoots.
void CriticalSpring(double& x, double& v, double target, double omega, double dt)
{
    const double e = x - target, decay = std::exp(-omega * dt), k = v + omega * e;
    x = target + (e + k * dt) * decay;
    v = (v - omega * k * dt) * decay;
}

// An angle in radians brought into -pi..pi.
double WrapAngle(double radians) { return std::remainder(radians, 2.0 * kPi); }

double SmoothStep(double edge0, double edge1, double x)
{
    const double t = Clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// Neck as a damped spring along WorldUp. The rider's body is accelerated by the bike; the
// head lags behind. Under a steady vertical acceleration a the neck settles at
//   offset = -CompressionPerG * a / g
// so a landing (large upward acceleration) compresses it and airtime (-1 g) extends it.
void StepNeck(State& s, double verticalAccel, double dt, const Config& cfg)
{
    const double omega = 2.0 * kPi * cfg.springHz;
    if (omega <= 0) {
        s.neckOffset = s.neckVelocity = 0;
        return;
    }
    const double stiffness = omega * omega;
    const double damping   = 2.0 * cfg.dampingRatio * omega;
    const double target    = -cfg.compressionPerG * verticalAccel / kGravity;

    // 1 ms semi-implicit Euler substeps keep a stiff spring stable at any frame rate.
    const int    steps = static_cast<int>(std::ceil(dt * 1000.0));
    const double h     = dt / steps;
    double& x = s.neckOffset;
    double& v = s.neckVelocity;
    for (int i = 0; i < steps; ++i) {
        v += (stiffness * (target - x) - damping * v) * h;
        x += v * h;
    }

    // End stops: the neck can't compress or stretch indefinitely.
    if (x < -cfg.maxCompression) { x = -cfg.maxCompression; if (v < 0) v = 0; }
    if (x >  cfg.maxExtension)   { x =  cfg.maxExtension;   if (v > 0) v = 0; }
}

// Heading (horizontal unit vector), pitch (radians, + = nose up) and roll (radians, + = right
// side up) of a forward / up pair relative to the horizon.
struct Attitude {
    Vec3   heading;
    double pitch = 0;
    double roll  = 0;
    double level = 1;  // ~cos(pitch); heading and roll are meaningless near 0
};

Attitude Measure(Vec3 forward, Vec3 up, Vec3 worldUp, Vec3 fallbackHeading)
{
    Attitude a;
    const double sinPitch = Clamp(Dot(forward, worldUp), -1.0, 1.0);
    a.pitch = std::asin(sinPitch);
    const Vec3 horizontal = forward - worldUp * sinPitch;
    a.level = Length(horizontal);
    a.heading = a.level > 0.05 ? horizontal / a.level : fallbackHeading;
    const Vec3 levelRight = Normalize(Cross(a.heading, worldUp));  // a level head's right axis
    const Vec3 levelUp    = Cross(levelRight, forward);
    const Vec3 right      = Cross(forward, up);
    a.roll = std::atan2(Dot(right, levelUp), Dot(right, levelRight));
    return a;
}

// Heading as a compass angle in degrees, for the trace.
double YawDeg(Vec3 heading, Vec3 worldUp)
{
    const Vec3 a = std::fabs(worldUp.z) < 0.9 ? Vec3{ 0, 0, -1 } : Vec3{ 0, 1, 0 };
    const Vec3 north = Normalize(a - worldUp * Dot(a, worldUp));
    const Vec3 east  = Cross(north, worldUp);
    return std::atan2(Dot(heading, east), Dot(heading, north)) * kRadToDeg;
}

// Orientation (camera-local -> world) from heading, pitch and roll; the inverse of Measure.
Quat Build(Vec3 heading, double pitch, double roll, Vec3 worldUp, double lookYaw = 0)
{
    const Vec3 levelRight = Normalize(Cross(heading, worldUp));
    Vec3 forward = heading * std::cos(pitch) + worldUp * std::sin(pitch);
    const Vec3 levelUp = Cross(levelRight, forward);
    Vec3 right = levelRight * std::cos(roll) + levelUp * std::sin(roll);
    const Vec3 up = Cross(right, forward);
    if (lookYaw != 0) {  // turn the head about its own up axis (+ = look right)
        const Vec3 f = forward * std::cos(lookYaw) + right * std::sin(lookYaw);
        right = right * std::cos(lookYaw) - forward * std::sin(lookYaw);
        forward = f;
    }
    return QuatFromBasis(right, up, -forward);
}

// Rotates a horizontal heading about WorldUp (+ = turn right).
Vec3 Turn(Vec3 heading, double radians, Vec3 worldUp)
{
    const Vec3 right = Normalize(Cross(heading, worldUp));
    return heading * std::cos(radians) + right * std::sin(radians);
}

// Roll after removing `lock` of it, capped at MaxRollCorrectionDeg and faded out near vertical.
double LockedRoll(const Attitude& a, double lock, const Config& cfg)
{
    const double fade    = Clamp((a.level - 0.1) / 0.3, 0.0, 1.0);
    const double maxCorr = cfg.maxRollCorrectionDeg * kDegToRad;
    return a.roll + Clamp(-a.roll * lock * fade, -maxCorr, maxCorr);
}

// Camera axes (unit, right-handed) and uniform scale of a view matrix; false if it isn't rigid.
bool DecomposeView(const double view[16], Vec3& right, Vec3& up, Vec3& back, double& scale)
{
    // A view matrix is [ s*R | t ] with bottom row 0,0,0,1. The rows of R are the camera's
    // right / up / back axes expressed in world space; the camera looks down -back.
    if (std::fabs(view[3]) > 1e-4 || std::fabs(view[7]) > 1e-4 || std::fabs(view[11]) > 1e-4 ||
        std::fabs(view[15] - 1.0) > 1e-4)
        return false;
    right = { view[0], view[4], view[8] };
    up    = { view[1], view[5], view[9] };
    back  = { view[2], view[6], view[10] };
    scale = Length(right);  // engines sometimes bake a uniform world scale in
    if (scale < 1e-9 || std::fabs(Length(up) / scale - 1.0) > 0.01 || std::fabs(Length(back) / scale - 1.0) > 0.01)
        return false;  // non-uniform scale: not a camera
    right = right / scale;
    up    = up / scale;
    back  = back / scale;
    if (Dot(Cross(right, up), back) < 0.9)
        return false;  // mirrored or sheared: e.g. a reflection pass
    // Re-orthonormalise to remove float drift before converting to a quaternion.
    back  = Normalize(back);
    right = Normalize(Cross(up, back));
    up    = Cross(back, right);
    return true;
}

// The step the next Update() will take: whole frames since the last one, at the smoothed
// frame interval. gap: too long since the last Update() to carry the filters over.
double StepDt(bool& gap)
{
    gap = g_lastCommitTime < 0 || NowSeconds() - g_lastCommitWallTime > 0.25;
    if (gap)
        return 0;
    const double elapsed = g_contentTime - g_lastCommitTime;
    const double frames = std::fmax(1.0, std::floor(elapsed / g_frameInterval + 0.5));
    return Clamp(frames * g_frameInterval, 0.0, 0.05);
}

bool Orient(Model& m, const double view[16], double dt, bool commit, Frame& f)
{
    const Config& cfg = GetConfig();
    const Vec3 worldUp{ cfg.worldUp[0], cfg.worldUp[1], cfg.worldUp[2] };
    State& s = m.state;
    Telemetry& tel = f.telemetry;

    // ---- 1. Decompose the raw view matrix ---------------------------------------------
    Vec3 right, up, back;
    double scale;
    if (!DecomposeView(view, right, up, back, scale))
        return false;
    const Quat rawRot = QuatFromBasis(right, up, back);
    const Vec3 rawFwd = -back;

    // ---- 2. Camera cut detection (rotation; position jumps are handled in FinishFrame) ----
    // Compared raw to raw: the corrected view may sit tens of degrees from the raw one. A cut
    // (the look keys snap the camera 90 / 180 degrees) only skips the smoothing for a frame;
    // the motion filters, airborne state and neck carry on.
    bool cut = false;
    if (s.valid) {
        const double jumpDeg = AngleBetween(s.prevRawRot, rawRot) * kRadToDeg;
        if (jumpDeg > cfg.snapAngleDeg) {
            cut = true;
            if (commit)
                LogEvent(g_cutLog, "camera: cut detected (%.1f deg turn), view snapped", jumpDeg);
        }
    }
    const bool reset = !s.valid;
    if (reset) {
        s = State{};
        s.valid = true;
        dt = 0;
    }
    s.prevRawRot = rawRot;
    const bool snap = reset || cut;

    // D-pad look: the head turns toward where it's asked to look. Back is reached over the
    // shoulder it's already turned toward (the right one from straight ahead).
    {
        const double side = cfg.lookSideDeg * kDegToRad;
        const double aim = g_look == Look::Left  ? -side
                         : g_look == Look::Right ? side
                         : g_look == Look::Back  ? (s.headTurn < 0 ? -kPi : kPi)
                         : 0.0;
        s.headTurn += (aim - s.headTurn) * (snap ? 1.0 : LowPassAlpha(cfg.lookTurnSpeed, dt));
    }

    // Look to apex: the head turns the way the bike's path is turning.
    {
        const double headFade = 1.0 - SmoothStep(5.0, 30.0, std::fabs(s.headTurn) * kRadToDeg);
        const double fade = SmoothStep(kApexSpeedMin, kApexSpeedFull, s.pathSpeed) * (1.0 - s.airborne) * headFade;
        const double maxTurn = kApexMaxDeg * kDegToRad;
        // Each filter lags a steady change in the rate by 1/its response, so their gap gives its slope.
        const double slope = (s.trendFast - s.trendSlow) / (1.0 / kApexTrendSlow - 1.0 / kApexTrendFast);
        const double lead = slope * cfg.apexAnticipation;
        s.apexLead = snap ? lead : s.apexLead + (lead - s.apexLead) * LowPassAlpha(kApexLeadResponse, dt);
        double ahead = s.pathTurnRate + s.apexLead;
        if (ahead * s.pathTurnRate < 0)
            ahead = 0;
        const double aim = maxTurn * std::tanh(cfg.apexLook * kApexLookAhead * ahead * fade / maxTurn);
        if (snap || dt <= 1e-6) {
            s.apexTurn = aim;
            s.apexRate = 0;
        } else {
            const double before = s.apexTurn;
            const double side = before > 0 ? 1.0 : -1.0;
            const bool unwinding = (aim - before) * side < 0;
            double omega = cfg.apexSpeed * kApexSpringScale;
            if (unwinding)
                omega *= kApexReturnScale;
            CriticalSpring(s.apexTurn, s.apexRate, aim, omega, dt);

            // Unwinding (rad/s, + = back toward straight ahead): free up to what the bike's turn
            // absorbs, eased into the drift past that. The turn is the lower of the two filtered
            // rates: the usual one lags as a corner eases off, the quick one is noisier.
            const double turn = s.pathTurnRate * side;
            const double absorbed = std::fmax(std::fmin(turn, s.pathTurnQuick * side), 0.0) +
                SmoothStep(kApexOtherWayMin * kDegToRad, kApexOtherWayFull * kDegToRad, -turn) * std::fabs(turn);
            const double drift = cfg.apexSpeed * kApexDriftScale * kDegToRad + (1.0 - headFade) * 2.0 * kPi;
            const auto Limit = [&](double unwind) {
                return unwind <= absorbed ? unwind : absorbed + drift * std::tanh((unwind - absorbed) / drift);
            };
            const double stepUnwind = (before - s.apexTurn) * side / dt;
            if (unwinding && stepUnwind > absorbed) {
                s.apexTurn = before - side * Limit(stepUnwind) * dt;
                s.apexRate = -side * Limit(-s.apexRate * side);
            }
        }
    }

    const bool onGround = s.airborne < 0.3;

    // ---- 3. Attitude of the game camera and of the bike -----------------------------------
    // The rests start from the last session's (see kRestSeedSeconds).
    if (commit && !m.restLoaded) {
        m.restLoaded = true;
        double v[3];
        if (ReadLearned("RestPitch", v)) {
            if (std::fabs(v[0]) <= kRestMaxPitchDeg) {
                m.bikePitchRest.Seed(v[0], kRestSeedSeconds);
                m.cameraPitchRest.Seed(v[1], kRestSeedSeconds);
                m.gameAngleRest.Seed(v[2], kRestSeedSeconds);
                Log("camera: the bike's resting pitch starts at %.1f deg (the game camera's %.1f), as learnt last time", v[0], v[1]);
            } else {
                Log("camera: ignored the resting pitch kept in the ini (the bike's %.1f deg can't be flat ground); learning it again", v[0]);
            }
        }
        m.restLoggedDeg = m.bikePitchRest.Degrees();
    }
    // Riding on flat ground: moving, upright and level (s.pathSpeed is the bike's when the
    // tracker has it).
    const bool moving = s.pathSpeed > kRestMinSpeed;
    const auto RestWeight = [&](const Attitude& a, double fromLevelRad) {
        return moving && std::fabs(a.roll) < kRestMaxRollDeg * kDegToRad &&
               std::fabs(fromLevelRad) < kRestMaxPitchDeg * kDegToRad ? 1.0 : kRestStillWeight;
    };
    const Attitude cameraAtt = Measure(rawFwd, up, worldUp, m.lastHeading);

    // The custom camera eases in when the bike is found and out when it is lost; while easing
    // out it keeps using the last known bike pose (the bike barely moves against the camera).
    const bike::Pose& pose = bike::Current();
    if (pose.tracked) {
        m.lastPose = pose;
        m.haveLastPose = true;
    }
    m.customWeight += (((cfg.customCamera && pose.tracked) ? 1.0 : 0.0) - m.customWeight) * LowPassAlpha(3.0, dt);
    if (!m.haveLastPose)
        m.customWeight = 0;

    bool     haveBike = false;
    Vec3     bikeFwd, bikeUp;
    Attitude bikeAtt;
    double   lookYaw = 0;
    if (pose.tracked && pose.fresh) {
        // The bike's own world-space axes from the last frame it was drawn. They come straight
        // from the bike, so the game camera's spring wobble against the bike can't leak in.
        bikeFwd = Normalize(Vec3{ pose.worldForward[0], pose.worldForward[1], pose.worldForward[2] });
        const Vec3 u{ pose.worldUp[0], pose.worldUp[1], pose.worldUp[2] };
        bikeUp = Normalize(u - bikeFwd * Dot(u, bikeFwd));
        haveBike = Length(Cross(bikeFwd, bikeUp)) > 0.5;
    } else if (m.haveLastPose && (pose.tracked || m.customWeight > 1e-3)) {
        // Bike not drawn last frame (a short gap, or easing out after losing it): its last known
        // pose relative to the camera, carried into world space through this frame's camera axes.
        const double* mv = m.lastPose.cameraSpace;
        const auto ToCamera = [mv](const double a[3]) {
            return Vec3{ mv[0] * a[0] + mv[4] * a[1] + mv[8] * a[2],
                         mv[1] * a[0] + mv[5] * a[1] + mv[9] * a[2],
                         mv[2] * a[0] + mv[6] * a[1] + mv[10] * a[2] };
        };
        const auto ToWorld = [&](Vec3 v) { return right * v.x + up * v.y + back * v.z; };
        bikeFwd = Normalize(ToWorld(ToCamera(m.lastPose.forward)));
        const Vec3 u = ToWorld(ToCamera(m.lastPose.up));
        bikeUp = Normalize(u - bikeFwd * Dot(u, bikeFwd));
        haveBike = Length(Cross(bikeFwd, bikeUp)) > 0.5;
    }
    if (haveBike) {
        bikeAtt = Measure(bikeFwd, bikeUp, worldUp, m.lastHeading);
        const double bikeWeight = RestWeight(bikeAtt, bikeAtt.pitch);
        if (onGround && bikeAtt.level > 0.5 && pose.tracked)
            m.bikePitchRest.Add(bikeAtt.pitch * kRadToDeg, dt, bikeWeight);

        // Where the game camera points relative to the bike: its look-down angle, and the
        // look-left / right / back keys.
        const Vec3 bikeRight = Cross(bikeFwd, bikeUp);
        const double x = Dot(rawFwd, bikeRight), y = Dot(rawFwd, bikeUp), z = Dot(rawFwd, bikeFwd);
        lookYaw = std::atan2(x, z);
        if (std::fabs(lookYaw) < 20.0 * kDegToRad && pose.tracked)
            m.gameAngleRest.Add(std::atan2(y, std::sqrt(x * x + z * z)) * kRadToDeg, dt, bikeWeight);
    } else {
        m.customWeight = 0;
    }
    // The game camera on flat ground: level as the bike is, when the tracker has it (the camera
    // looks down at the game's angle), else within kRestMaxPitchDeg of its rest so far.
    if (onGround && cameraAtt.level > 0.5) {
        const double fromLevel = haveBike && pose.tracked ? bikeAtt.pitch
                               : cameraAtt.pitch - m.cameraPitchRest.Degrees() * kDegToRad;
        m.cameraPitchRest.Add(cameraAtt.pitch * kRadToDeg, dt, RestWeight(cameraAtt, fromLevel));
    }
    if (commit && std::fabs(m.bikePitchRest.Degrees() - m.restLoggedDeg) > 3.0) {
        LogEvent(g_restLog, "camera: the bike's resting pitch is now %.1f deg (was %.1f; the bike at %.1f deg, %.1f units/s, %s)",
                 m.bikePitchRest.Degrees(), m.restLoggedDeg, bikeAtt.pitch * kRadToDeg, s.pathSpeed,
                 onGround ? "on the ground" : "in the air");
        m.restLoggedDeg = m.bikePitchRest.Degrees();
    }
    tel.bikeRestDeg   = m.bikePitchRest.Degrees();
    tel.cameraRestDeg = m.cameraPitchRest.Degrees();
    if (commit && m.bikePitchRest.Riding() > kRestSaveAfter &&
        (m.restSavedAt < 0 || g_contentTime - m.restSavedAt > kRestSaveEvery)) {
        m.restSavedAt = g_contentTime;
        const double v[3] = { m.bikePitchRest.Degrees(), m.cameraPitchRest.Degrees(), m.gameAngleRest.Degrees() };
        WriteLearned("RestPitch", v);
    }
    const double absYawDeg = std::fabs(lookYaw) * kRadToDeg;

    // Pitch lock works on the pitch *away from rest*, so riding on flat ground looks the same
    // at any setting and only wheelies, stoppies and jump faces are evened out.
    const double cameraPitchExcess = cameraAtt.pitch - m.cameraPitchRest.Degrees() * kDegToRad;
    const double bikePitchExcess   = haveBike ? bikeAtt.pitch - m.bikePitchRest.Degrees() * kDegToRad : 0.0;

    // Lean limiter (Config::leanLimiter): leaning forward / back moves the game camera against the
    // bike - along it, and pitched on it (the tracker sees 20-35 cm and 6-8 deg of it). Where the
    // camera sits along the bike and how it's pitched there are measured each frame (the camera in the
    // bike's own space, as the tracker last matched them, both from the same frame) against where it
    // usually is - an average over a few seconds on the ground, held still in the air and while looking
    // round - and the slider's share of that is taken off: the eye moved back toward its usual place
    // along the bike, the view's pitch toward its usual angle (before the steadiness springs, so its
    // jolts can't come back through). Up and down is left to the neck. At 0: nothing at all.
    double leanPitchCut = 0;  // rad off the game camera's pitch
    f.leanShift = Vec3{};
    if (cfg.leanLimiter > 0 && pose.tracked && haveBike) {
        double inv[16];
        const double* c = m.lastPose.cameraSpace;
        if (AffineInverse(c, inv)) {
            const Vec3 mf{ m.lastPose.forward[0], m.lastPose.forward[1], m.lastPose.forward[2] };
            const Vec3 mu{ m.lastPose.up[0], m.lastPose.up[1], m.lastPose.up[2] };
            const Vec3 eyeOnBike{ inv[12], inv[13], inv[14] };
            const Vec3 lookOnBike = Normalize(Vec3{ -inv[8], -inv[9], -inv[10] });
            const double toWorld = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]) / std::fmax(scale, 1e-9);
            const double along = Dot(eyeOnBike, mf) * toWorld;
            const double pitchOnBike = std::atan2(Dot(lookOnBike, mu), Dot(lookOnBike, mf));
            if (!s.leanHave || snap) {
                s.leanAlong = along;
                s.leanPitch = pitchOnBike;
                s.leanHave = true;
            } else if (onGround && absYawDeg < 20.0) {
                const double a = LowPassAlpha(kLeanNeutralRate, dt);
                s.leanAlong += (along - s.leanAlong) * a;
                s.leanPitch += (pitchOnBike - s.leanPitch) * a;
            }
            // (Not while looking round: the camera turned on the bike isn't a lean.)
            const double k = Clamp(cfg.leanLimiter, 0.0, 1.0) * (1.0 - SmoothStep(20.0, 40.0, absYawDeg));
            const double dAlong = Clamp(along - s.leanAlong, -kLeanMaxShift, kLeanMaxShift);
            const double dPitch = Clamp(pitchOnBike - s.leanPitch, -kLeanMaxPitch * kDegToRad, kLeanMaxPitch * kDegToRad);
            f.leanShift = bikeFwd * (-dAlong * k);
            leanPitchCut = dPitch * k;
            tel.leanAlong = dAlong;
            tel.leanPitchDeg = dPitch * kRadToDeg;
            tel.leanShift = -dAlong * k;
        }
    } else if (cfg.leanLimiter <= 0) {
        s.leanHave = false;  // (turned on again: its usual place found afresh)
    }
    const double leanedExcess = cameraPitchExcess - leanPitchCut;

    // Steadiness: like a gimbal, the view follows the lean and pitch through a critically damped
    // spring, so it eases into a corner's lean, a wheelie or a jump face, but the quick jolts of
    // bumps, landings and the suspension diving under braking don't reach it. The locks then take
    // their share of what's left. (Without it a lock let its remainder through at full speed: at
    // 50% pitch lock, half of every jolt.) The head offsets work from the same steadied axes, so
    // a jolt doesn't swing the eye on their lever either.
    const double steadyOmega = cfg.steadiness > 0
        ? kSteadyFastest * std::pow(kSteadySlowest / kSteadyFastest, cfg.steadiness) : 0.0;
    const auto Steady = [&](double& x, double& v, double target, bool seed) {
        if (seed || snap || steadyOmega <= 0) {
            x = target;
            v = 0;
        } else if (dt > 1e-6) {
            CriticalSpring(x, v, x + WrapAngle(target - x), steadyOmega, dt);  // the short way round
            x = WrapAngle(x);
        }
        return x;
    };
    const double cameraRestPitch = cameraAtt.pitch - cameraPitchExcess;
    Attitude gameSteady = cameraAtt;
    gameSteady.roll = Steady(s.steadyRoll[0], s.steadyRollRate[0], cameraAtt.roll, false);
    const double gameExcess = Steady(s.steadyPitch[0], s.steadyPitchRate[0], leanedExcess, false);
    Attitude bikeSteady = bikeAtt;
    double bikeExcess = bikePitchExcess;
    if (haveBike) {
        bikeSteady.roll = Steady(s.steadyRoll[1], s.steadyRollRate[1], bikeAtt.roll, !s.steadyBike);
        bikeExcess = Steady(s.steadyPitch[1], s.steadyPitchRate[1], bikePitchExcess, !s.steadyBike);
    }
    // The view's own springs follow only what whip lock (as of last frame) lets through. Through
    // a jump they used to follow the bike's nose dropping for the landing while whip lock held
    // the view level over them; letting go on landing showed that pitch, now stale - the bike's
    // nose was already up off the rear wheel - and the view dipped 3-5 deg in 0.15 s and came
    // back. Held level, they start from level on landing and ease to the bike from there.
    // Without whip lock they are the springs above.
    const double heldLevel = cfg.whipLock * s.whip;
    Attitude gameView = cameraAtt;
    gameView.roll = Steady(s.viewRoll[0], s.viewRollRate[0], cameraAtt.roll * (1.0 - heldLevel), false);
    const double gameViewExcess = Steady(s.viewPitch[0], s.viewPitchRate[0], leanedExcess * (1.0 - heldLevel), false);
    Attitude bikeView = bikeAtt;
    double bikeViewExcess = bikePitchExcess;
    if (haveBike) {
        bikeView.roll = Steady(s.viewRoll[1], s.viewRollRate[1], bikeAtt.roll * (1.0 - heldLevel), !s.steadyBike);
        bikeViewExcess = Steady(s.viewPitch[1], s.viewPitchRate[1], bikePitchExcess * (1.0 - heldLevel), !s.steadyBike);
    }
    s.steadyBike = haveBike;

    // ---- 4. Target view -------------------------------------------------------------------
    // Air looseness: in the air the view follows the nose's left / right swing only through a
    // slow filter, so most of a scrub or whip passes under the camera and the view keeps
    // pointing forward. Nothing is locked: the filter always heads for the nose, starts at the
    // first moment of free fall, and catches up smoothly after landing.
    // The view trails the nose by at most maxSwing (soft limit). A rider steering through a
    // jump out of a corner turns the bike one way and keeps it there (80 deg in 0.7 s seen in a
    // trace); unlimited, the view fell 30 deg behind and swung round on landing.
    const double airRate  = 30.0 * std::pow(1.5 / 30.0, cfg.airLooseness);  // 1/s: 30 (tight) .. 1.5 (loose)
    const double maxSwing = 15.0 * kDegToRad * cfg.airLooseness;
    const auto AirYaw = [&](Vec3 nose, Vec3& loose) {
        const bool falling = s.fallTime > 0 || s.inAir;
        if (cfg.airLooseness <= 0 || Length(loose) < 0.5 || (!falling && s.airborne < 0.01)) {
            loose = nose;
            return 0.0;
        }
        loose = Normalize(loose + (nose - loose) * LowPassAlpha(falling ? airRate : 10.0, dt));
        double w = std::atan2(Dot(nose, Normalize(Cross(loose, worldUp))), Dot(nose, loose));
        if (std::fabs(w) > 2.0 * maxSwing) {  // no wind-up past the soft limit
            w = w > 0 ? 2.0 * maxSwing : -2.0 * maxSwing;
            loose = Turn(nose, -w, worldUp);
        }
        return -maxSwing * std::tanh(w / maxSwing) * s.airborne;
    };

    // Game camera: the game's view with the locks applied. (It is fixed to the bike, so its
    // own pitch away from rest is the bike's.) A look-left / right / back isn't a swing.
    double airYaw = AirYaw(cameraAtt.heading, s.airNoseCamera) * (1.0 - SmoothStep(50.0, 75.0, absYawDeg));
    // With the custom camera chosen but the bike not found yet (just spawned, standing still), this
    // view stands in for it: at the custom camera's look-down angle, not the game's, so the angle
    // slider works from the first frame and nothing jumps when the bike is found. (It used to
    // sit at the game's 1st Person Angle, the slider doing nothing, until riding off.)
    const double gameRestPitch = cfg.customCamera ? -cfg.cameraAngleDeg * kDegToRad : cameraRestPitch;
    const double gamePitch = gameRestPitch + (1.0 - cfg.pitchLock) * gameViewExcess;
    const double gameRoll  = LockedRoll(gameView, cfg.horizonLock, cfg);
    Quat target = Build(Turn(cameraAtt.heading, airYaw, worldUp), gamePitch, gameRoll, worldUp);

    // Custom camera: built from the bike only.
    if (m.customWeight > 1e-3) {
        const double angleDown = cfg.cameraAngleDeg;
        const double pitch = (1.0 - cfg.pitchLock) * bikeViewExcess - angleDown * kDegToRad;
        const double roll  = LockedRoll(bikeView, cfg.horizonLock, cfg);
        // The game's look keys turn the camera 90 / 180 degrees; smaller offsets (the game
        // camera's own wobble, whip or gimbal) are ignored.
        const double look = lookYaw * SmoothStep(50.0, 75.0, absYawDeg);
        const double customAirYaw = AirYaw(bikeAtt.heading, s.airNose);
        const Quat custom = Build(Turn(bikeAtt.heading, customAirYaw, worldUp), pitch, roll, worldUp, look);
        target = Slerp(target, custom, m.customWeight);
        airYaw += (customAirYaw - airYaw) * m.customWeight;
    }

    // Whip lock: in the air the view holds the direction of travel, level, at the resting
    // look-down angle, whatever the bike does under it. In the air that direction can't change,
    // so it is held from takeoff - as it was over kTravelHoldBack before free fall was detected,
    // as the head is already swinging sideways with the bike by then - and settles onto the
    // path actually flown. On the ground it follows the velocity (or the view, when slow),
    // filtered. All of it from the bike when the tracker has it: the eye swings with the head.
    {
        const bool fromBike = pose.tracked && pose.haveAccel;
        const Vec3 velocity = fromBike ? Vec3{ pose.velocity[0], pose.velocity[1], pose.velocity[2] } : s.velocity;
        const Vec3 horizontal = velocity - worldUp * Dot(velocity, worldUp);
        const double speed = Length(horizontal);
        const Vec3 viewFwd = Rotate(target, { 0, 0, -1 });
        const Vec3 viewHorizontal = viewFwd - worldUp * Dot(viewFwd, worldUp);
        const Vec3 direction = speed > 3.0 ? horizontal / speed
                             : Length(viewHorizontal) > 0.05 ? Normalize(viewHorizontal) : cameraAtt.heading;
        const bool falling = s.fallTime > 0 || s.inAir;
        const double rise = Dot(velocity, worldUp);
        const Vec3 bikePos{ pose.worldPosition[0], pose.worldPosition[1], pose.worldPosition[2] };
        if (snap || Length(s.travel) < 0.5) {
            s.travel = direction;
            s.travelCount = 0;
        }
        if (!falling) {
            s.travel = Normalize(s.travel + (direction - s.travel) * LowPassAlpha(5.0, dt));
            s.travelHistory[s.travelNext] = { direction, rise, dt };
            s.travelNext = (s.travelNext + 1) % kTravelSamples;
            s.travelCount = s.travelCount < kTravelSamples ? s.travelCount + 1 : kTravelSamples;
        } else if (!s.wasFalling) {
            // Free fall is detected a little after the lip, so the launch speed is the fastest
            // rise over the same stretch the direction is taken from.
            s.launchRise = rise;
            Vec3 sum{};
            double age = 0;
            for (int i = 1; i <= s.travelCount && age < kTravelHoldBack; ++i) {
                const TravelSample& sample = s.travelHistory[(s.travelNext - i + kTravelSamples) % kTravelSamples];
                sum = sum + sample.direction * sample.dt;
                s.launchRise = std::fmax(s.launchRise, sample.rise);
                age += sample.dt;
            }
            if (Length(sum) > 1e-9)
                s.travel = Normalize(sum);
            // The history only ever holds the ground since the last fall: a fall read again just
            // after landing must not reach back to before the jump.
            s.travelCount = 0;
            s.takeoffFromBike = fromBike && pose.fresh;
            s.takeoffPos = s.takeoffFromBike ? bikePos : s.prevPos;
        } else if (!s.takeoffFromBike || pose.fresh) {
            const Vec3 run = (s.takeoffFromBike ? bikePos : s.prevPos) - s.takeoffPos;
            const Vec3 path = run - worldUp * Dot(run, worldUp);
            const double length = Length(path);
            if (length > kPathRunMin)
                s.travel = Normalize(s.travel + (path / length - s.travel) *
                                     (LowPassAlpha(kPathResponse, dt) * SmoothStep(kPathRunMin, kPathRunFull, length)));
        }
        s.wasFalling = falling;
        // Engages within ~0.1 s of the first moment of deep free fall (the airborne estimate takes
        // too long for a whip thrown at the lip), only while the bike's path runs straight, as far
        // as the jump is big enough for it, and lets go over ~0.3 s after landing.
        const double maxTurn = (s.turnFromBike ? kWhipTurnRateDeg : kAirTurnRateDeg) * kDegToRad;
        const bool engage = falling && s.deepFall && s.turnRate < maxTurn;
        const double launchAirtime = 2.0 * std::fmax(s.launchRise, 0.0) / (std::fabs(m.airG) * kGravity);
        const double jumpSize = std::fmax(SmoothStep(kWhipAirtimeMin, kWhipAirtimeFull, launchAirtime),
                                          SmoothStep(kWhipFallTimeMin, kWhipFallTimeFull, s.fallTime));
        s.whip += ((engage ? jumpSize : 0.0) - s.whip) * LowPassAlpha(engage ? 25.0 : 6.0, dt);

        // Where it holds the view: taken over from the view as it is, turning as it turns (see
        // kWhipPitchOmega), then eased to the direction of travel, the resting pitch, level.
        const double customRest = -cfg.cameraAngleDeg;
        const double cameraRest = cfg.customCamera ? customRest : m.cameraPitchRest.Degrees();  // (as gamePitch)
        const double restPitch = (cameraRest + (customRest - cameraRest) * m.customWeight) * kDegToRad;
        const Attitude seen = Measure(Rotate(s.smoothRot, { 0, 0, -1 }), Rotate(s.smoothRot, { 0, 1, 0 }),
                                      worldUp, s.travel);
        const auto TurnFrom = [&](Vec3 from, Vec3 to) {  // + = to the right, as Turn()
            return std::atan2(Dot(to, Normalize(Cross(from, worldUp))), Dot(to, from));
        };
        const auto Rate = [&](double change) { return dt > 1e-6 ? Clamp(change / dt, -kWhipMaxRate, kWhipMaxRate) : 0.0; };
        const bool haveRates = s.haveLastView && !snap;
        const double yawRate   = haveRates ? Rate(TurnFrom(s.lastViewHeading, seen.heading)) : 0.0;
        const double pitchRate = haveRates ? Rate(seen.pitch - s.lastViewPitch) : 0.0;
        const double rollRate  = haveRates ? Rate(WrapAngle(seen.roll - s.lastViewRoll)) : 0.0;
        s.lastViewHeading = seen.heading;
        s.lastViewPitch = seen.pitch;
        s.lastViewRoll = seen.roll;
        s.haveLastView = true;
        if (snap) {
            s.whipYaw = s.whipYawRate = 0;
            s.whipPitch = restPitch;
            s.whipPitchRate = 0;
            s.whipRoll = s.whipRollRate = 0;
        } else if (engage && !s.whipEngaged) {
            s.whipYaw = TurnFrom(s.travel, seen.heading);
            s.whipYawRate = yawRate;
            s.whipPitch = seen.pitch;
            s.whipPitchRate = pitchRate;
            s.whipRoll = seen.roll;
            s.whipRollRate = rollRate;
        } else {
            CriticalSpring(s.whipYaw, s.whipYawRate, 0.0, kWhipLevelOmega, dt);
            CriticalSpring(s.whipPitch, s.whipPitchRate, restPitch, kWhipPitchOmega, dt);
            CriticalSpring(s.whipRoll, s.whipRollRate, 0.0, kWhipLevelOmega, dt);
        }
        s.whipEngaged = engage;
    }
    const double whipWeight = cfg.whipLock * s.whip;
    tel.whip = whipWeight;
    if (whipWeight > 1e-3)
        target = Slerp(target, Build(Turn(s.travel, s.whipYaw, worldUp), s.whipPitch, s.whipRoll, worldUp), whipWeight);
    m.lastHeading = cameraAtt.heading;

    // Head position: along the bike (custom) or the game camera's own axes.
    const double lookAlpha = LowPassAlpha(10.0, dt);
    const double headDeg = std::fmax(absYawDeg, std::fabs(s.headTurn) * kRadToDeg);
    const double sideTarget = haveBike ? SmoothStep(50.0, 75.0, headDeg) * (1.0 - SmoothStep(110.0, 150.0, headDeg)) : 0.0;
    const double backTarget = haveBike ? SmoothStep(110.0, 150.0, headDeg) : 0.0;
    s.lookSide += (sideTarget - s.lookSide) * (snap ? 1.0 : lookAlpha);
    s.lookBack += (backTarget - s.lookBack) * (snap ? 1.0 : lookAlpha);
    const double side = s.lookSide * m.customWeight, rear = s.lookBack * m.customWeight;
    f.forwardOffset = cfg.headForward + side * cfg.lookSideForward + rear * cfg.lookBackForward;
    f.heightOffset  = cfg.headHeight + side * cfg.lookSideHeight + rear * cfg.lookBackHeight;
    f.offsetFwd = rawFwd;
    f.offsetUp  = up;
    Vec3 offsetBikeFwd = bikeFwd, offsetBikeUp = bikeUp;
    if (steadyOmega > 0) {  // steadied (see Steadiness above)
        const Quat game = Build(cameraAtt.heading, cameraRestPitch + gameExcess, gameSteady.roll, worldUp);
        f.offsetFwd = Rotate(game, { 0, 0, -1 });
        f.offsetUp  = Rotate(game, { 0, 1, 0 });
        if (haveBike) {
            const double bikeRest = bikeAtt.pitch - bikePitchExcess;
            const Quat bikeFrame = Build(bikeAtt.heading, bikeRest + bikeExcess, bikeSteady.roll, worldUp);
            offsetBikeFwd = Rotate(bikeFrame, { 0, 0, -1 });
            offsetBikeUp  = Rotate(bikeFrame, { 0, 1, 0 });
        }
    }
    if (haveBike) {
        f.offsetFwd = f.offsetFwd + (offsetBikeFwd - f.offsetFwd) * m.customWeight;
        f.offsetUp  = f.offsetUp + (offsetBikeUp - f.offsetUp) * m.customWeight;
    }

    // ---- 5. Inertial smoothing (pitch / yaw / roll) ---------------------------------------
    // Exponential slerp toward the target, filtering out high-frequency spikes. MaxLagDeg caps
    // how far behind the view can fall during sustained fast motion.
    Quat& smooth = s.smoothRot;
    if (snap)
        smooth = target;
    smooth = Slerp(smooth, target, LowPassAlpha(cfg.rotationResponse, dt));
    const double lag    = AngleBetween(smooth, target);
    const double maxLag = cfg.maxLagDeg * kDegToRad;
    if (lag > maxLag)
        smooth = Slerp(target, smooth, maxLag / lag);
    Quat rot = smooth;

    // The d-pad's and apex look's head turn, about WorldUp so the horizon stays where the rest
    // left it. After the smoothing, so the head turns at its own pace.
    if (s.headTurn + s.apexTurn != 0)
        rot = AxisAngle(worldUp, -(s.headTurn + s.apexTurn)) * rot;

    // Neck compression also nods the head forward a little (rotation about the camera's right
    // axis; a negative angle pitches down). Uses last frame's neck, so the orientation is
    // complete here - the sky, drawn before the camera position is known, gets all of it.
    // The nod follows the neck through a spring (kNodOmega): the neck goes from fully stretched
    // in the air to fully compressed within ~50 ms of a landing, and nodding with it pitched the
    // view 4.4 deg down at ~90 deg/s - with whip lock holding the view still through the air,
    // a plain snap down on every landing. Through the spring: ~16 deg/s, over ~0.3 s.
    if (cfg.nodDegPerMeter != 0) {
        const double nod = s.neckOffset * cfg.nodDegPerMeter * kDegToRad;
        if (snap || dt <= 0) {
            s.nod = nod;
            s.nodRate = 0;
        } else {
            CriticalSpring(s.nod, s.nodRate, nod, kNodOmega, dt);
        }
        const Vec3 pitchAxis = Rotate(rot, { 1, 0, 0 });
        rot = AxisAngle(pitchAxis, s.nod) * rot;
    }

    f.rot      = Normalize(rot);
    f.dt       = dt;
    f.reset    = reset;
    f.rawRight = right;
    f.rawUp    = up;
    f.rawBack  = back;
    f.valid    = true;

    const Vec3 B = Rotate(f.rot, { 0, 0, 1 });
    const Attitude finalAtt = Measure(-B, Rotate(f.rot, { 0, 1, 0 }), worldUp, cameraAtt.heading);
    tel.dtMs              = dt * 1000.0;
    tel.lagDeg            = AngleBetween(s.smoothRot, target) * kRadToDeg;
    tel.rawRollDeg        = (haveBike ? bikeAtt.roll : cameraAtt.roll) * kRadToDeg;
    tel.rollCorrectionDeg = (finalAtt.roll - cameraAtt.roll) * kRadToDeg;
    tel.pitchExcessDeg    = (m.customWeight > 0.5 ? bikePitchExcess : cameraPitchExcess) * kRadToDeg;
    tel.viewPitchDeg      = finalAtt.pitch * kRadToDeg;
    tel.lookYawDeg        = (lookYaw + s.headTurn) * kRadToDeg;
    tel.airYawDeg         = airYaw * kRadToDeg;
    tel.apexDeg           = s.apexTurn * kRadToDeg;
    tel.bikeTracked       = pose.tracked;
    tel.bikeFresh         = pose.tracked && pose.fresh;
    tel.customActive      = m.customWeight > 0.5;
    tel.customWeight      = m.customWeight;
    tel.cut               = cut;
    tel.reset             = reset;
    tel.rawYaw    = YawDeg(cameraAtt.heading, worldUp);
    tel.rawPitch  = cameraAtt.pitch * kRadToDeg;
    tel.rawRoll   = cameraAtt.roll * kRadToDeg;
    tel.bikeYaw   = haveBike ? YawDeg(bikeAtt.heading, worldUp) : 0.0;
    tel.bikePitch = haveBike ? bikeAtt.pitch * kRadToDeg : 0.0;
    tel.bikeRoll  = haveBike ? bikeAtt.roll * kRadToDeg : 0.0;
    tel.outYaw    = YawDeg(finalAtt.heading, worldUp);
    tel.outPitch  = finalAtt.pitch * kRadToDeg;
    tel.outRoll   = finalAtt.roll * kRadToDeg;
    return true;
}

void RotationCorrectionOf(const Frame& f, double out[16])
{
    // C = NewRotation * RawRotation^T; element (r, c) = dot(new axis r, raw axis c).
    const Vec3 newAxes[3] = { Rotate(f.rot, { 1, 0, 0 }), Rotate(f.rot, { 0, 1, 0 }), Rotate(f.rot, { 0, 0, 1 }) };
    const Vec3 rawAxes[3] = { f.rawRight, f.rawUp, f.rawBack };
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r)
            out[c * 4 + r] = (r < 3 && c < 3) ? Dot(newAxes[r], rawAxes[c]) : (r == c ? 1.0 : 0.0);
    }
}

// ---- Look only: the look keys with the first person mod off --------------------------------
// Just the head's turn, on the game's own camera: about WorldUp, as the full camera turns it,
// the eye where the game put it. Nothing else of the mod runs (no smoothing, levelling, neck or
// bike tracking), and looking ahead the game's view is left exactly as it is.
struct LookOnly {
    bool   valid    = false;
    Quat   prevRawRot;      // the game camera's rotation last frame (for the sky's preview)
    double headTurn = 0;    // rad, + = right, eased at Config::lookTurnSpeed
};
LookOnly g_lookOnly;
double   g_lookTurn = 0;  // the look keys' turn in the last frame's view (rad, + = right): LookTurn()

// Steps s by dt toward where the rider is asked to look. False if view isn't a camera.
bool StepLookOnly(LookOnly& s, const double view[16], double dt, Vec3& right, Vec3& up, Vec3& back)
{
    double scale;
    if (!DecomposeView(view, right, up, back, scale))
        return false;
    if (!s.valid) {  // (from straight ahead: it eases into a look already held, never jumps)
        s = LookOnly{};
        s.valid = true;
    }
    s.prevRawRot = QuatFromBasis(right, up, back);
    const Config& cfg = GetConfig();
    const double side = cfg.lookSideDeg * kDegToRad;
    const double aim = g_look == Look::Left  ? -side
                     : g_look == Look::Right ? side
                     : g_look == Look::Back  ? (s.headTurn < 0 ? -kPi : kPi)
                     : 0.0;
    s.headTurn += (aim - s.headTurn) * LowPassAlpha(cfg.lookTurnSpeed, dt);
    if (aim == 0 && std::fabs(s.headTurn) < 1e-4)  // (back ahead: the game's view, untouched)
        s.headTurn = 0;
    return true;
}

// The eye-space correction that turns the game camera (axes right, up, back) by s's head turn;
// false (out untouched) while it looks straight ahead.
bool LookOnlyCorrectionOf(const LookOnly& s, Vec3 right, Vec3 up, Vec3 back, double out[16])
{
    if (s.headTurn == 0)
        return false;
    const Config& cfg = GetConfig();
    const Vec3 worldUp{ cfg.worldUp[0], cfg.worldUp[1], cfg.worldUp[2] };
    Frame f;
    f.rot = Normalize(AxisAngle(worldUp, -s.headTurn) * QuatFromBasis(right, up, back));
    f.rawRight = right;
    f.rawUp = up;
    f.rawBack = back;
    RotationCorrectionOf(f, out);  // (a rotation about the eye: no translation)
    return true;
}

bool FinishFrame(const double view[16], double correction[16])
{
    const Config& cfg = GetConfig();
    const Vec3 worldUp{ cfg.worldUp[0], cfg.worldUp[1], cfg.worldUp[2] };
    State& s = g_model.state;
    Telemetry& tel = g_frame.telemetry;

    Vec3 right, up, back;
    double scale;
    if (!DecomposeView(view, right, up, back, scale))
        return false;
    const Vec3 t{ view[12], view[13], view[14] };
    const Vec3 rawPos = -(right * t.x + up * t.y + back * t.z) / scale;  // eye = -R^T t / s
    double dt = g_frame.dt;

    // A position jump (respawn) restarts the position filters only.
    if (s.havePos && Length(rawPos - s.prevPos) > cfg.teleportDistance) {
        LogEvent(g_teleportLog, "camera: cut detected (%.2f units), resetting the position filters", Length(rawPos - s.prevPos));
        s.havePos = false;
    }
    if (!s.havePos) {
        s.havePos      = true;
        s.velocity = s.accel = Vec3{};
        s.neckOffset = s.neckVelocity = 0;
        s.inAir = false;
        s.fallTime = 0;
        s.motionHeading = Vec3{};
        s.turnRate = s.pathTurnRate = s.pathTurnQuick = s.pathSpeed = 0;
        s.trendSlow = s.trendFast = 0;
        s.prevPos      = rawPos;
        s.smoothHeight = Dot(rawPos, worldUp);
        g_eyeFit.Reset();
        dt = 0;
    }

    // ---- 6. Motion: velocity, acceleration, airborne ------------------------------------
    // How many physics steps the game moved on this frame: at speed the camera covers a whole
    // number of steps' travel. Where it doesn't - slow, or the sprung camera bouncing on a
    // landing - the clock moves by the time that passed, and the bike tracker corrects it from
    // the bike's own travel (SetLastStep). A pause just advances it. Never by no steps: the
    // velocity it's judged by comes from samples stamped with this clock, so a clock that
    // stood still kept a stale velocity that went on reading as no steps - stuck for good
    // (seen after a crash: the neck pinned, fall_g at 159, standing still read as riding).
    // Above 128 fps a frame with no new step then just counts its time, as it always did.
    const double wallDt = g_lastMotionTime >= 0 ? std::fmax(g_contentTime - g_lastMotionTime, 0.0) : 0.0;
    g_lastMotionTime = g_contentTime;
    double step = wallDt;
    const double stepTravel = Length(s.velocity) * kGameTick;
    if (wallDt < 0.25 && stepTravel > kMinStepTravel) {
        const double ratio = Length(rawPos - s.prevPos) / stepTravel;
        const double n = std::floor(ratio + 0.5);
        if (n >= 1 && n <= 4 && std::fabs(ratio - n) < 0.25)
            step = n * kGameTick;
    }
    g_gameTime += step;
    g_lastStep = step;
    ++g_motionFrame;

    // A least-squares fit over the last ~0.1 s (AccelFilterHz), stamped with the game's clock:
    // frame-to-frame timing noise averages out instead of being differentiated twice.
    g_eyeFit.Add(g_gameTime, rawPos);
    const double window = Clamp(cfg.accelFilterHz > 0 ? 1.2 / cfg.accelFilterHz : 0.1, 0.04, 0.3);
    Vec3 velocity, accel;
    if (g_eyeFit.Estimate(window, velocity, accel)) {
        const double maxAccel = 30.0 * kGravity;  // reject glitches (e.g. a missed cut)
        if (Length(accel) > maxAccel)
            accel = Normalize(accel) * maxAccel;
        s.velocity = velocity;
        s.accel = accel;
    }
    const double verticalAccel = Dot(s.accel, worldUp);

    // How fast the direction of travel turns (only measurable when moving). From the bike when
    // the tracker has it: the eye is on the rider's head, which a whip swings from side to side
    // (up to 90 deg/s of apparent turning in a test), while the bike's own path holds straight.
    const bike::Pose& pose = bike::Current();
    const bool bikeAccel = pose.tracked && pose.haveAccel;
    {
        const Vec3 travel = bikeAccel ? Vec3{ pose.velocity[0], pose.velocity[1], pose.velocity[2] } : s.velocity;
        const Vec3 horizontal = travel - worldUp * Dot(travel, worldUp);
        const double speed = Length(horizontal);
        double rate = 0;
        if (speed > 3.0) {
            const Vec3 heading = horizontal / speed;
            if (Length(s.motionHeading) > 0.5 && dt > 1e-5)
                rate = std::atan2(Dot(heading, Cross(s.motionHeading, worldUp)), Dot(heading, s.motionHeading)) / dt;
            s.motionHeading = heading;
        } else {
            s.motionHeading = Vec3{};
        }
        s.turnRate += (std::fabs(rate) - s.turnRate) * LowPassAlpha(10.0, dt);
        s.pathTurnRate += (rate - s.pathTurnRate) * LowPassAlpha(10.0, dt);
        s.pathTurnQuick += (rate - s.pathTurnQuick) * LowPassAlpha(kApexQuickResponse, dt);
        s.trendSlow += (rate - s.trendSlow) * LowPassAlpha(kApexTrendSlow, dt);
        s.trendFast += (rate - s.trendFast) * LowPassAlpha(kApexTrendFast, dt);
        s.pathSpeed = speed;
        s.turnFromBike = bikeAccel;
    }
    const bool turning = s.turnRate > kAirTurnRateDeg * kDegToRad;

    // Airborne: in the air everything is in free fall (-1 g); on the ground it hovers around 0 g.
    // Takeoff: both the eye and (when the tracker has it) the bike must be in free fall - either
    // alone can fake it for a moment: the sprung game camera bouncing, or the bike flicking from
    // one lean to the other through a corner - for a quarter second (a real jump lasts far longer).
    // Landing: the eye alone can't be trusted. It is on the rider's head, which swings through
    // an arc as the bike is laid over in a whip or scrub, and can read near 0 g for a few tenths
    // of a second in mid-air. So it takes the bike back out of free fall, a clear impact, or
    // (untracked) the eye out of free fall for longer than a swing lasts.
    // Either way, a direction of travel that is turning means the wheels are on the ground.
    double eyeFallG = verticalAccel / kGravity;
    {
        Vec3 unusedVelocity, slowAccel;
        if (g_eyeFit.Estimate(0.15, unusedVelocity, slowAccel))
            eyeFallG = Dot(slowAccel, worldUp) / kGravity;
    }
    const double fallG = bikeAccel ? std::fmax(eyeFallG, pose.verticalAccelG) : eyeFallG;
    if (!s.inAir) {
        if (fallG < -0.7 && !turning) {
            s.fallTime += dt;
            if (s.fallTime > 0.25)
                s.inAir = true;
        } else if (fallG > -0.4 || turning) {
            s.fallTime = 0;
        }
        s.outOfFallTime = 0;
    } else {
        s.outOfFallTime = eyeFallG > -0.4 ? s.outOfFallTime + dt : 0.0;
        const bool impact = eyeFallG > kImpactG || (bikeAccel && pose.verticalAccelG > kImpactG);
        const bool landed = impact || turning ||
                            (bikeAccel ? pose.verticalAccelG > -0.4 : s.outOfFallTime > kSwingTime);
        if (landed) {
            s.inAir = false;
            s.fallTime = 0;
        } else {
            s.fallTime += dt;
        }
    }
    // How deep free fall reads well into a jump: the game's gravity in this mod's units.
    if (s.inAir && s.fallTime > 0.4)
        g_model.airG = Clamp(g_model.airG + (fallG - g_model.airG) * LowPassAlpha(2.0, dt), -6.0, -0.8);
    if (s.fallTime <= 0 && !s.inAir)
        s.deepFall = false;
    else if (fallG < kWhipFallFraction * g_model.airG)
        s.deepFall = true;
    // Eases in over a few tenths of a second, and out twice as fast on landing.
    s.airborne += ((s.inAir ? 1.0 : 0.0) - s.airborne) * LowPassAlpha(s.inAir ? 4.0 : 8.0, dt);

    // ---- 7. Vertical damping & G-force neck compression ---------------------------------
    const double rawHeight = Dot(rawPos, worldUp);
    if (dt > 1e-5) {
        // The neck is loaded by the bike's acceleration when the tracker has it: the game's head
        // animation (bobbing, swinging through a scrub) isn't a G-force and would only shake it.
        StepNeck(s, pose.tracked && pose.haveAccel ? pose.verticalAccelG * kGravity : verticalAccel, dt, cfg);
        // Low-pass the eye height itself to iron out head-bob jitter from the animation.
        s.smoothHeight += (rawHeight - s.smoothHeight) * LowPassAlpha(cfg.verticalResponse, dt);
    }
    s.smoothHeight = Clamp(s.smoothHeight, rawHeight - cfg.maxVerticalLag, rawHeight + cfg.maxVerticalLag);
    s.prevPos = rawPos;

    const double verticalOffset = (s.smoothHeight - rawHeight) + s.neckOffset;
    const Vec3 headOffset = g_frame.offsetFwd * g_frame.forwardOffset + g_frame.offsetUp * g_frame.heightOffset;
    const Vec3 eye = rawPos + headOffset + g_frame.leanShift + worldUp * verticalOffset;

    // ---- 8. Rebuild the view matrix and derive the eye-space correction -----------------
    const Vec3 R = Rotate(g_frame.rot, { 1, 0, 0 });
    const Vec3 U = Rotate(g_frame.rot, { 0, 1, 0 });
    const Vec3 B = Rotate(g_frame.rot, { 0, 0, 1 });
    const double sc = scale;
    const double newView[16] = {
        sc * R.x, sc * U.x, sc * B.x, 0,
        sc * R.y, sc * U.y, sc * B.y, 0,
        sc * R.z, sc * U.z, sc * B.z, 0,
        -sc * Dot(R, eye), -sc * Dot(U, eye), -sc * Dot(B, eye), 1,
    };
    double rawInverse[16];
    if (!AffineInverse(view, rawInverse))
        return false;
    MatMul(newView, rawInverse, correction);

    tel.airborne       = s.airborne;
    tel.verticalAccelG = verticalAccel / kGravity;
    tel.fallG          = fallG;
    tel.neckOffset     = s.neckOffset;
    tel.verticalOffset = verticalOffset;
    tel.rawPos[0] = rawPos.x;  tel.rawPos[1] = rawPos.y;  tel.rawPos[2] = rawPos.z;
    const Vec3 offset = eye - rawPos;
    tel.eyeOffset[0] = offset.x;  tel.eyeOffset[1] = offset.y;  tel.eyeOffset[2] = offset.z;
    return true;
}

}  // namespace

void Reset()
{
    g_model.state.valid = false;
    g_lookOnly.valid = false;
}

const Telemetry& LastTelemetry() { return g_telemetry; }

void FrameTick()
{
    const double now = NowSeconds();
    if (g_contentTime > 0) {
        const double interval = now - g_contentTime;
        g_rawIntervalMs = interval * 1000.0;
        // Pauses (loading, alt-tab) don't count toward the typical frame interval.
        if (interval > 0 && interval < 0.25) {
            g_frameInterval = g_haveInterval ? g_frameInterval + (interval - g_frameInterval) * 0.2 : interval;
            g_haveInterval = true;
        }
    }
    g_contentTime = now;
    SummarizeEvents(g_cutLog, "camera cuts");
    SummarizeEvents(g_teleportLog, "position cuts");
}

void SetLook(Look look) { g_look = look; }

double ContentTime() { return g_contentTime; }
double GameTime() { return g_gameTime; }
unsigned MotionFrame() { return g_motionFrame; }
double LastStep() { return g_lastStep; }

double SetLastStep(double seconds)
{
    const double delta = seconds - g_lastStep;
    if (std::fabs(delta) > 1e-9) {
        g_gameTime += delta;
        g_eyeFit.ShiftNewest(delta);
        g_lastStep = seconds;
    }
    return g_gameTime;
}
double FrameInterval() { return g_frameInterval; }

bool PreviewRotation(const double view[16], double out[16])
{
    bool gap;
    const double dt = StepDt(gap);
    Model copy = g_model;
    if (gap)
        copy.state.valid = false;
    Frame f;
    if (!Orient(copy, view, dt, false, f))
        return false;
    RotationCorrectionOf(f, out);
    return true;
}

double AngleToLastCameraDeg(const double view[16])
{
    Vec3 right, up, back;
    double scale;
    if (!DecomposeView(view, right, up, back, scale))
        return -1;
    if (!g_model.state.valid)
        return 0;
    return AngleBetween(g_model.state.prevRawRot, QuatFromBasis(right, up, back)) * kRadToDeg;
}

bool RotationCorrection(double out[16])
{
    if (!g_frame.valid)
        return false;
    RotationCorrectionOf(g_frame, out);
    return true;
}

bool Update(const double view[16], double correction[16])
{
    bool gap;
    const double dt = StepDt(gap);
    if (gap)
        g_model.state.valid = false;  // paused, alt-tabbed or loading: resync rather than integrate
    g_lookOnly.valid = false;  // (the mod is on: the look-only head starts ahead next time)
    Frame f;
    if (!Orient(g_model, view, dt, true, f))
        return false;
    g_frame = f;
    g_lookTurn = g_model.state.headTurn;
    g_lastCommitTime = g_contentTime;
    g_lastCommitWallTime = NowSeconds();
    if (!FinishFrame(view, correction))
        return false;
    g_frame.telemetry.rawIntervalMs = g_rawIntervalMs;
    g_telemetry = g_frame.telemetry;
    return true;
}

bool LookOnlyUpdate(const double view[16], double correction[16])
{
    bool gap;
    const double dt = StepDt(gap);
    if (gap)
        g_lookOnly.valid = false;  // paused, alt-tabbed or loading: back to straight ahead
    Vec3 right, up, back;
    if (!StepLookOnly(g_lookOnly, view, dt, right, up, back))
        return false;
    g_lastCommitTime = g_contentTime;
    g_lastCommitWallTime = NowSeconds();
    // The full camera starts from the raw camera again when the mod comes back on.
    g_model.state.valid = false;
    g_frame.valid = false;
    g_lookTurn = g_lookOnly.headTurn;
    return LookOnlyCorrectionOf(g_lookOnly, right, up, back, correction);
}

double LookTurn() { return g_lookTurn; }

void OrbitLook(const double view[16], const double rider[3], double correction[16])
{
    const double turn = g_lookTurn;
    if (turn == 0)
        return;
    const Config& cfg = GetConfig();
    // In the raw camera's eye space (V's rows are its axes; a uniform scale cancels out).
    const auto Rot = [](const double m[16], const double v[3], double out[3]) {  // m's 3x3 times v
        for (int r = 0; r < 3; ++r)
            out[r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2];
    };
    double up[3];
    Rot(view, cfg.worldUp, up);
    const double upLength = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    if (upLength < 1e-9)
        return;
    for (double& c : up)
        c /= upLength;
    // The drawn eye: the point C sends to the origin, -R_C^T t_C.
    double eye[3];
    for (int r = 0; r < 3; ++r)
        eye[r] = -(correction[r * 4] * correction[12] + correction[r * 4 + 1] * correction[13] +
                   correction[r * 4 + 2] * correction[14]);
    // Round the rider: the same turn as the view's (AxisAngle(WorldUp, -turn)), by Rodrigues.
    const double a = -turn, cosA = std::cos(a), sinA = std::sin(a);
    const double v[3] = { eye[0] - rider[0], eye[1] - rider[1], eye[2] - rider[2] };
    const double along = up[0] * v[0] + up[1] * v[1] + up[2] * v[2];
    const double cross[3] = { up[1] * v[2] - up[2] * v[1], up[2] * v[0] - up[0] * v[2], up[0] * v[1] - up[1] * v[0] };
    double moved[3];
    for (int r = 0; r < 3; ++r)
        moved[r] = rider[r] + v[r] * cosA + cross[r] * sinA + up[r] * along * (1.0 - cosA);
    // C's translation puts the eye there: t = -R_C * eye.
    double t[3];
    Rot(correction, moved, t);
    correction[12] = -t[0];
    correction[13] = -t[1];
    correction[14] = -t[2];
}

bool LookOnlyPreview(const double view[16], double out[16])
{
    bool gap;
    const double dt = StepDt(gap);
    LookOnly copy = g_lookOnly;
    if (gap)
        copy.valid = false;
    Vec3 right, up, back;
    if (!StepLookOnly(copy, view, dt, right, up, back))
        return false;
    if (!LookOnlyCorrectionOf(copy, right, up, back, out)) {
        for (int i = 0; i < 16; ++i)
            out[i] = (i % 5 == 0) ? 1.0 : 0.0;
    }
    return true;
}

double LookOnlyAngleToLastDeg(const double view[16])
{
    Vec3 right, up, back;
    double scale;
    if (!DecomposeView(view, right, up, back, scale))
        return -1;
    if (!g_lookOnly.valid)
        return 0;
    return AngleBetween(g_lookOnly.prevRawRot, QuatFromBasis(right, up, back)) * kRadToDeg;
}

}  // namespace camera
}  // namespace mxcam
