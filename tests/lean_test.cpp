// lean_test.cpp
//   The lean limiter (Config::leanLimiter) through the camera module (camera.cpp) and the bike tracker:
//   riding straight and level, then the rider leans forward - the game camera (on the rider's head)
//   moves 0.4 units forward along the bike and pitches 6 deg down on it, over 0.3 s, and stays there.
//   At 0 the view goes with it, as it always has; at 100% the eye stays at its usual place on the bike
//   and the view's pitch at its usual angle; at 50%, half. Game camera and custom camera.
//   Runs in real time (the camera times frames with the clock), ~30 s.
//
//   Build: cl /EHsc /std:c++17 tests\lean_test.cpp src\camera.cpp src\bike.cpp src\config.cpp src\log.cpp user32.lib

#include <windows.h>
#include <cmath>
#include <cstdio>

#include "../src/bike.h"
#include "../src/camera.h"
#include "../src/config.h"

namespace mxcam {
bool IsDumping() { return false; }  // the frame dump lives in hooks.cpp, not linked here
namespace gamepad {                 // (the controller binds' names, for the ini: not needed here)
const char* ControlKey(int) { return ""; }
int ParseControl(const char*) { return 0; }
}  // namespace gamepad
}

using namespace mxcam;

namespace {

constexpr double kPi = 3.14159265358979, kDeg = kPi / 180.0;
constexpr double kSpeed = 15.0;
constexpr double kStart = 3.0;    // s of riding level first (the tracker finds the bike, the lean's usual place)
constexpr double kEnd = 4.4;
constexpr double kLean = 0.4;     // units forward along the bike
constexpr double kLeanPitch = -6.0;  // deg on the bike

int g_failures = 0;
void Check(bool ok, const char* what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    g_failures += ok ? 0 : 1;
}

struct V { double x, y, z; };
V operator+(V a, V b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
V operator*(V a, double s) { return { a.x * s, a.y * s, a.z * s }; }
V Cross(V a, V b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
double Dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V Normalize(V a) { return a * (1.0 / std::sqrt(Dot(a, a))); }

// The view matrix of a camera at eye with the given yaw (0 = +x), pitch and no roll.
void View(V eye, double yaw, double pitch, double m[16])
{
    const V heading{ std::cos(yaw), 0, std::sin(yaw) }, worldUp{ 0, 1, 0 };
    const V forward = heading * std::cos(pitch) + worldUp * std::sin(pitch);
    const V right = Normalize(Cross(heading, worldUp));
    const V up = Cross(right, forward);
    const V back = forward * -1.0;
    const V axes[3] = { right, up, back };
    for (int r = 0; r < 3; ++r) {
        m[0 * 4 + r] = axes[r].x;
        m[1 * 4 + r] = axes[r].y;
        m[2 * 4 + r] = axes[r].z;
        m[3 * 4 + r] = -Dot(axes[r], eye);
        m[r * 4 + 3] = 0;
    }
    m[15] = 1;
}

// One frame: the camera's view, then the bike drawn at bike, level, facing yaw (its model's -Z forward).
const camera::Telemetry& Draw(V bike, double bikeYaw, V eye, double camPitch)
{
    double view[16], axes[16];
    View(eye, bikeYaw, camPitch, view);
    View({ 0, 0, 0 }, bikeYaw, 0.0, axes);
    double model[16] = {};
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            model[c * 4 + r] = axes[r * 4 + c];
    model[12] = bike.x; model[13] = bike.y; model[14] = bike.z; model[15] = 1;
    float viewModel[16];
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            double sum = 0;
            for (int k = 0; k < 4; ++k)
                sum += view[k * 4 + r] * model[c * 4 + k];
            viewModel[c * 4 + r] = float(sum);
        }
    camera::FrameTick();
    bike::BeginFrame(view);
    double correction[16];
    camera::Update(view, correction);
    bike::Observe(0, viewModel);
    bike::EndFrame(camera::GameTime(), camera::FrameInterval());
    return camera::LastTelemetry();
}

double Ease(double x, double over)
{
    const double u = std::fmin(std::fmax(x / over, 0.0), 1.0);
    return u * u * (3 - 2 * u);
}

struct Result {
    double shift = 0;   // the eye moved along the bike by us, held (units; - = back)
    double pitch = 0;   // the view's pitch change from level riding, held (deg)
    bool tracked = true;
};

Result Ride(bool custom, double limiter)
{
    Config& cfg = EditConfig();
    cfg.customCamera = custom;
    cfg.steadiness = 0.6;
    cfg.pitchLock = 0.5;
    cfg.leanLimiter = limiter;
    camera::Reset();
    bike::Reset();
    Result result;
    double lastClock = 0, levelPitch = 0, shiftSum = 0, pitchSum = 0, n = 0;
    const double yaw = 0.3;
    const V fwd{ std::cos(yaw), 0, std::sin(yaw) };
    V pos{ 0, 0, 0 };
    LARGE_INTEGER frequency, start, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    for (;;) {
        QueryPerformanceCounter(&now);
        const double t = double(now.QuadPart - start.QuadPart) / frequency.QuadPart;
        if (t > kEnd)
            break;
        const double dt = t - lastClock;
        lastClock = t;
        const double lean = t > kStart ? Ease(t - kStart, 0.3) : 0.0;
        pos = pos + fwd * (kSpeed * dt);
        const V eye = pos + V{ 0, 3.0, 0 } + fwd * (kLean * lean);
        const camera::Telemetry& tel = Draw(pos, yaw, eye, (-8.0 + kLeanPitch * lean) * kDeg);
        if (t > kStart - 0.5 && t < kStart)
            levelPitch = tel.outPitch;
        if (t > kStart - 0.3)
            result.tracked &= tel.bikeTracked;
        if (t > kStart + 0.6 && t < kStart + 1.2) {  // held
            shiftSum += tel.eyeOffset[0] * fwd.x + tel.eyeOffset[2] * fwd.z;
            pitchSum += tel.outPitch - levelPitch;
            n += 1;
        }
        Sleep(7);
    }
    result.shift = n > 0 ? shiftSum / n : 0;
    result.pitch = n > 0 ? pitchSum / n : 0;
    return result;
}

}  // namespace

int main()
{
    printf("Leaning forward 0.4 units along the bike and 6 deg down on it (pitch lock 50%%, steadiness 60%%)\n");
    for (int custom = 0; custom < 2; ++custom) {
        printf("\n%s camera\n", custom ? "CUSTOM" : "GAME");
        const double settings[3] = { 0.0, 0.5, 1.0 };
        Result r[3];
        for (int i = 0; i < 3; ++i) {
            r[i] = Ride(custom != 0, settings[i]);
            printf("  lean limiter %3.0f%%: the eye moved %+.3f units along the bike by us, the view pitched %+.2f deg%s\n",
                   settings[i] * 100, r[i].shift, r[i].pitch, r[i].tracked ? "" : " (the tracker lost the bike)");
        }
        Check(r[0].tracked && r[1].tracked && r[2].tracked, "the bike is tracked through the lean");
        Check(std::fabs(r[0].shift) < 0.005, "off: the eye isn't moved (as it always was)");
        Check(r[2].shift < -0.7 * kLean && r[2].shift > -1.05 * kLean, "100%: the eye kept at its usual place (most of the lean taken back)");
        Check(std::fabs(r[1].shift - 0.5 * r[2].shift) < 0.05, "50%: half of that");
        if (!custom) {
            Check(r[0].pitch < -2.0, "off: the view pitches down with the lean (game camera, half of it through pitch lock)");
            Check(std::fabs(r[2].pitch) < 0.4 * std::fabs(r[0].pitch), "100%: the view hardly pitches");
            Check(std::fabs(r[1].pitch - 0.5 * (r[0].pitch + r[2].pitch)) < 0.6, "50%: in between");
        } else {
            Check(std::fabs(r[2].pitch - r[0].pitch) < 0.3, "custom camera: its pitch is the bike's, the limiter leaves it");
        }
    }
    printf("\n%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
