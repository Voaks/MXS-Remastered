// laptimer_test.cpp
//   The lap timer's own logic (src/laptimer.cpp) on made-up tracks: a square loop 200 units a side,
//   ridden at a steady speed, a frame every 10 ms.
//     laptimer_test.exe              the checks
//     laptimer_test.exe <folder>     also: how many tracks the game folder's own files give
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/laptimer.cpp"

namespace mxcam {
void Log(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    printf("    log: ");
    vprintf(format, args);
    printf("\n");
    va_end(args);
}
namespace paths {
static std::wstring g_folder;
const wchar_t* GameFolder() { return g_folder.c_str(); }
const wchar_t* PersonalFolder() { return L""; }
}  // namespace paths
}  // namespace mxcam

using namespace mxcam;

static int g_failed = 0;
static void Check(bool ok, const char* what)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_failed;
}

// The square: corners (-100,-100) (100,-100) (100,100) (-100,100), ridden anticlockwise seen from
// above with x right and z up: along the bottom to +x, up the right side, back along the top, down
// the left. Checkpoints: 0 on the bottom at x -50, 1 on the right at z 0, 2 on the top at x 0, 3 on
// the left at z 0, 4 (the finish line) on the bottom at x -75. The start gate's slots behind the
// finish line, at x -95.
static const char* kSquare =
    "startinggate:\n"
    "[-95.000000 0.000000 -104.000000] 0.0\n"
    "[-95.000000 0.000000 -100.000000] 0.0\n"
    "[-95.000000 0.000000 -96.000000] 0.0\n"
    "checkpoints:\n"
    "30.0 [-50.0 0.0 -100.0] [30.0 0.0 0.0]\n"   // (each one's vector: the way through it, as the game's)
    "30.0 [100.0 0.0 0.0] [0.0 0.0 30.0]\n"
    "30.0 [0.0 0.0 100.0] [-30.0 0.0 0.0]\n"
    "30.0 [-100.0 0.0 0.0] [0.0 0.0 -30.0]\n"
    "30.0 [-75.0 0.0 -100.0] [30.0 0.0 0.0]\n"
    "firstlap:\n"
    "0 1 2 3 4\n"
    "normallap:\n"
    "4 0 0 1 2 3 4\n";  // (a checkpoint twice, as one real track has: crossed once, still a whole lap)
// Another track far away (its gates never crossed): never taken for this one.
static const char* kElsewhere =
    "startinggate:\n"
    "[5000.0 0.0 5000.0] 0.0\n"
    "checkpoints:\n"
    "30.0 [5000.0 0.0 5100.0] [0.0 0.0 30.0]\n"
    "30.0 [5100.0 0.0 5100.0] [30.0 0.0 0.0]\n"
    "30.0 [5200.0 0.0 5100.0] [0.0 0.0 30.0]\n"
    "firstlap:\n0 1 2\nnormallap:\n2 0 1 2\n";

// Another track overlapping the square's right side (its 4 checkpoints in order up it, its lap round
// them): what a stadium's other tracks did - three in order by chance - and the timer jumped to them.
static const char* kDecoy =
    "startinggate:\n"
    "[3000.0 0.0 3000.0] 0.0\n"
    "checkpoints:\n"
    "20.0 [100.0 0.0 -60.0] [0.0 0.0 20.0]\n"
    "20.0 [100.0 0.0 -20.0] [0.0 0.0 20.0]\n"
    "20.0 [100.0 0.0 20.0] [0.0 0.0 20.0]\n"
    "20.0 [100.0 0.0 60.0] [0.0 0.0 20.0]\n"
    "firstlap:\n0 1 2 3\nnormallap:\n0 1 2 3\n";

struct Rider {
    double t = 0;
    double d = 0;  // how far round the square from (-100,-100)
    double cut = -1;  // (riding straight across from the bottom to the top at this lap's distance, missing checkpoint 1)
};

static double g_offsetX = 0;  // (another square, this far along x)

static void PositionAt(double d, double out[3])
{
    double s = std::fmod(d, 800.0);
    if (s < 0)
        s += 800.0;
    out[1] = 0;
    if (s < 200) { out[0] = -100 + s; out[2] = -100; }
    else if (s < 400) { out[0] = 100; out[2] = -100 + (s - 200); }
    else if (s < 600) { out[0] = 100 - (s - 400); out[2] = 100; }
    else { out[0] = -100; out[2] = 100 - (s - 600); }
    out[0] += g_offsetX;
}

static laptimer::Display g_last;
static std::vector<laptimer::Display> g_seen;

static void Frame(Rider& r, const double pos[3])
{
    laptimer::Update(r.t, true, pos);
    g_last = laptimer::Current(r.t);
    g_seen.push_back(g_last);
}

// Ride from distance d0 to d1 at speed (units a second), a frame each 10 ms.
static void Ride(Rider& r, double d1, double speed)
{
    const double dt = 0.01;
    while (r.d < d1) {
        r.d = std::fmin(d1, r.d + speed * dt);
        r.t += dt;
        double p[3];
        PositionAt(r.d, p);
        Frame(r, p);
    }
}

int main(int argc, char** argv)
{
    printf("lap timer test\n");
    const char* names[3] = { "Square", "Elsewhere", "Decoy" };
    const char* texts[3] = { kSquare, kElsewhere, kDecoy };
    const double spacings[3] = { 1.0, 2.0, 0.5 };
    laptimer::UseTracksForTest(names, texts, 3, spacings);

    Rider r;
    // Standing in the middle slot for a second: the track is known.
    double slot[3] = { -95.0, 0.0, -100.0 };
    for (int i = 0; i < 100; ++i) {
        r.t += 0.01;
        Frame(r, slot);
    }
    Check(g_last.track && g_last.trackName == "Square", "standing in a start gate slot tells which track it is");
    Check(g_last.lap == 0 && g_last.current < 0, "no lap timed at the gate");

    // Off the gate at 20 units a second: the first lap from leaving the slot (x -95: 5 units round).
    r.d = 5.0;
    const double setOff = r.t;
    Ride(r, 5.0 + 50.0, 20.0);
    Check(g_last.lap == 1 && std::fabs(g_last.current - (r.t - setOff)) < 0.1, "leaving the gate starts lap 1");
    // Round to the finish line (x -75 on the bottom: 825 units round from (-100,-100) the second time).
    Ride(r, 825.0 - 0.5, 20.0);
    Check(g_last.lap == 1 && g_last.split == 4, "the first lap passed checkpoints 0 1 2 3 (i4) before the finish");
    Ride(r, 825.0 + 0.5, 20.0);
    // 1 unit out of the slot at the start (x -94), 819 units to the line: 40.95 s.
    Check(g_last.finished && std::fabs(g_last.current - 40.95) < 0.03, "the first lap's time: 819 units at 20 a second");
    printf("    first lap %.3f s\n", g_last.current);

    // Lap 2 at 20 a second: 800 units, 40 s.
    Ride(r, 825.0 + 800.0 + 0.5, 20.0);
    Check(g_last.finished && g_last.lap == 2 && std::fabs(g_last.current - 40.0) < 0.03, "lap 2: 40.00 s");
    Check(!g_last.haveGap, "the first normal lap has nothing to be measured against");
    printf("    lap 2 %.3f s\n", g_last.current);

    // Lap 3 at 25 a second: 32 s; at checkpoint 0 (25 units in) 1.00 s against 1.25: 0.25 ahead.
    Ride(r, 825.0 + 800.0 + 25.0 + 1.0, 25.0);
    Ride(r, 825.0 + 800.0 + 100.0, 25.0);  // (past the lap's end being shown)
    Check(g_last.lap == 3 && g_last.split == 1, "lap 3 past its first checkpoint: (i1)");
    Check(g_last.haveGap && std::fabs(g_last.gap + 0.25) < 0.03, "live gap at checkpoint 1: 0.25 s ahead of the best");
    Check(std::fabs(g_last.predicted - 39.75) < 0.03, "heading for the best less the gap: 39.75");
    printf("    lap 3 at checkpoint 1: gap %+.3f, predicted %.3f\n", g_last.gap, g_last.predicted);
    Ride(r, 825.0 + 1600.0 + 0.5, 25.0);
    Check(g_last.finished && g_last.lap == 3 && std::fabs(g_last.current - 32.0) < 0.03, "lap 3: 32.00 s");
    Check(g_last.haveGap && std::fabs(g_last.gap + 8.0) < 0.03, "its gap at the line: 8.00 s ahead of lap 2");

    // Lap 4 at 25 again, but cutting across from the bottom to the top, missing gate 2 (checkpoint 1):
    // as the game counts a lap, every gate in order - the missed one isn't skipped, the finish line
    // crossed without it is no lap, and the clock runs on into the next time round, till the missed
    // gate is crossed and the rest after it.
    {
        const double start = 825.0 + 1600.0;
        Ride(r, start + 100.0, 25.0);  // to x 25 on the bottom (past checkpoint 0)
        // Straight up to the top at x 25 (inside the square, missing the right side's checkpoint),
        // 200 units in 4 s, then on along the top to the left.
        for (int i = 1; i <= 400; ++i) {
            r.t += 0.01;
            const double p[3] = { 25.0, 0.0, -100.0 + 200.0 * i / 400.0 };
            Frame(r, p);
        }
        r.d = start + 400.0 + 75.0;  // (x 25 on the top)
        Ride(r, start + 600.0, 25.0);  // past gate 4 on the left
        Check(g_last.lap == 4 && !g_last.valid && g_last.split == 1, "a gate missed: shown as missed, the lap stays at the gate before it (G1)");
        Ride(r, start + 800.0 + 0.5, 25.0);  // over the finish line, 21 s in
        Check(!g_last.finished && g_last.lap == 4 && g_last.current > 20.9, "the finish line crossed with a gate missed: no lap, the clock runs on");
        printf("    lap 4 over the finish line with gate 2 missed: still running, %.3f s\n", g_last.current);
        // On round at 20 (40 s): gate 2 this time, and the rest, to the finish line: lap 4 done, 61 s.
        Ride(r, start + 1100.0 + 1.0, 20.0);
        Check(g_last.lap == 4 && g_last.valid && g_last.split == 2, "the missed gate crossed next time round: the lap goes on from it");
        Ride(r, start + 1600.0 + 0.5, 20.0);
        Check(g_last.finished && g_last.lap == 4 && std::fabs(g_last.current - 61.0) < 0.05, "and ends at the next finish line: 61 s, the clock never reset");
        printf("    lap 4 %.3f s\n", g_last.current);
    }
    // Lap 5 at 20: measured against lap 3 (32 s). (Lap 4 ended at 825 + 3200.)
    Ride(r, 825.0 + 3200.0 + 25.0 + 1.0, 20.0);
    Ride(r, 825.0 + 3200.0 + 100.0, 20.0);
    Check(g_last.lap == 5 && g_last.haveGap && std::fabs(g_last.gap - 0.25) < 0.03, "lap 5 is measured against lap 3, the best");
    Check(g_last.trackName == "Square" && std::fabs(g_last.best - 32.0) < 0.03,
          "riding up the decoy's checkpoints every lap (8 of them in order by now) never took the timer away, nor the best");
    Check(g_last.gates == 5, "the lap's timing gates: 5 (G1 to G5)");

    // Back in the gate (a restart): the laps start over from lap 1.
    for (int i = 0; i < 100; ++i) {
        r.t += 0.01;
        Frame(r, slot);
    }
    Check(g_last.lap == 0 || g_last.finished == false, "a restart in the gate stops the lap");
    r.d = 5.0;
    Ride(r, 5.0 + 50.0, 20.0);
    Check(g_last.lap == 1, "and leaving it again is lap 1");

    // Telling the track from its checkpoints only (no start gate): a fresh session on the square,
    // three checkpoints in order.
    // (The heightmap known: the decoy's isn't this one's, so its checkpoints - 4 in order on the way
    // up the right side, before the square's 3rd - don't count.)
    laptimer::UseTracksForTest(names, texts, 3, spacings);
    laptimer::SetTerrain(1.0);
    Rider q;
    q.d = 100.0;  // (x 0 on the bottom: past checkpoints 4 and 0; 1 is at 300 round, 2 at 500, 3 at 700)
    Ride(q, 710.0, 20.0);  // through 1, 2 and 3
    Check(!g_last.track, "three checkpoints in order aren't enough to tell the track (by chance, too often)");
    Ride(q, 825.0 + 1.0, 20.0);  // the finish line (x -75: 825 round), the 4th, then on
    Check(g_last.track && g_last.trackName == "Square", "four checkpoints in order tell it");
    Check(g_last.lap == 1, "the finish line starts lap 1 (no start gate seen)");
    // Gate 1 (the right side's, 30 wide: x 85 to 115) ridden through 3 units past its end, at x 118:
    // counted (the lines reach 10 units past a gate's width). Then gate 2 (the top's, at x 0) crossed
    // while your position isn't known for a moment: counted, from the step across the gap.
    {
        Ride(q, 1000.0, 20.0);  // (to the bottom right corner: x 100)
        const auto Leg = [&](double x0, double z0, double x1, double z1, bool known) {
            const int n = static_cast<int>(std::hypot(x1 - x0, z1 - z0) / 0.2);
            for (int i = 1; i <= n; ++i) {
                q.t += 0.01;
                const double p[3] = { x0 + (x1 - x0) * i / n, 0.0, z0 + (z1 - z0) * i / n };
                laptimer::Update(q.t, known, p);
                g_last = laptimer::Current(q.t);
            }
        };
        Leg(100, -100, 118, -90, true);
        Leg(118, -90, 118, 90, true);     // up past gate 1's end
        Check(g_last.lap == 1 && g_last.split == 2 && g_last.valid, "a gate ridden through just past its end is counted");
        Leg(118, 90, 100, 100, true);
        Leg(100, 100, 10, 100, true);
        Leg(10, 100, -10, 100, false);    // your position not known across gate 2 (a second)
        Leg(-10, 100, -11, 100, true);
        Check(g_last.lap == 1 && g_last.split == 3 && g_last.valid, "a gate crossed while your position wasn't known is counted");
    }

    // Another track loaded with no sign from the game (its heightmap's numbers the same): gone a few
    // seconds (loading), back somewhere else. The timer tells the track again: the new one, fresh -
    // not the last one's laps and gates timed on it. And the same one loaded again keeps your best.
    {
        std::string square2 = kSquare;
        const auto Shift = [](std::string text) {
            std::string out;
            size_t pos = 0;
            while (true) {
                const size_t open = text.find('[', pos);
                if (open == std::string::npos)
                    break;
                out += text.substr(pos, open + 1 - pos);
                char* end = nullptr;
                const double x = std::strtod(text.c_str() + open + 1, &end);
                const bool isPlace = text[open - 1] == ' ' || text[open - 1] == '\n';
                char buffer[32];
                // (Only the places move - the first bracket on a line; the way through stays.)
                const size_t lineStart = text.rfind('\n', open);
                const bool first = text.find('[', lineStart == std::string::npos ? 0 : lineStart + 1) == open;
                snprintf(buffer, sizeof buffer, "%.1f", first ? x + 1500.0 : x);
                (void)isPlace;
                out += buffer;
                pos = static_cast<size_t>(end - text.c_str());
            }
            return out + text.substr(pos);
        };
        square2 = Shift(square2);
        const char* n2[2] = { "Square", "Square2" };
        const char* t2[2] = { kSquare, square2.c_str() };
        const double sp2[2] = { 1.0, 1.0 };
        laptimer::UseTracksForTest(n2, t2, 2, sp2);
        laptimer::SetTerrain(1.0);
        g_offsetX = 0;
        Rider a;
        a.d = 100.0;
        Ride(a, 825.0 + 800.0 + 300.0, 25.0);  // told, lap 1 from the finish line (32 s), lap 2 begun
        Check(g_last.trackName == "Square" && g_last.lap == 2 && std::fabs(g_last.best - 32.0) < 0.05, "on the first track: a lap timed");
        // Gone 3 s (a track loading), back on the other.
        a.t += 3.0;
        g_offsetX = 1500.0;
        a.d = 100.0;
        Ride(a, 300.0, 25.0);
        Check(g_last.lap == 0, "back somewhere else after a few seconds gone: the lap running stops");
        Ride(a, 825.0 + 1.0, 25.0);  // 4 of its gates in order, and its finish line
        Check(g_last.trackName == "Square2" && g_last.best < 0 && g_last.lap == 1,
              "another track with nothing to say so: told by its own gates, timed fresh (not the last one's best)");
        Ride(a, 825.0 + 800.0 + 1.0, 25.0);  // a lap on it: 32 s
        Check(g_last.finished && g_last.lap == 1 && std::fabs(g_last.best - 32.0) < 0.05, "and its own laps timed");
        // Gone 3 s, back on the same one (loaded again): told again, its best kept.
        a.t += 3.0;
        a.d = 100.0;
        Ride(a, 825.0 + 1.0, 25.0);
        Check(g_last.trackName == "Square2" && std::fabs(g_last.best - 32.0) < 0.05 && g_last.lap == 1,
              "the same track loaded again: told again, your best kept");
        g_offsetX = 0;
    }

    if (argc > 1) {
        // The game folder's own tracks.
        std::string folder = argv[1];
        paths::g_folder.assign(folder.begin(), folder.end());
        if (!paths::g_folder.empty() && paths::g_folder.back() != L'\\')
            paths::g_folder += L'\\';
        std::vector<laptimer::Track> tracks;
        laptimer::ReadFolder(paths::GameFolder(), tracks);
        printf("    %zu tracks with timing gates in %s\n", tracks.size(), argv[1]);
        size_t fromArchives = 0;
        for (const auto& t : tracks) {
            printf("      %-40s %zu checkpoints, first lap %zu, lap %zu, %zu gate slots\n", t.name.c_str(), t.gates.size(), t.first.size(),
                   t.normal.size(), t.slots.size() / 2);
            if (t.name.rfind("sx20", 0) == 0 || t.name.rfind("mx20", 0) == 0 || t.name.rfind("gp20", 0) == 0)
                ++fromArchives;
        }
        Check(tracks.size() >= 30, "the game folder's tracks are read");
        int withSpacing = 0;
        for (const auto& t : tracks)
            withSpacing += t.spacing > 0 ? 1 : 0;
        printf("    %d of them with their heightmap's spacing (terrain.hf)\n", withSpacing);
        Check(withSpacing >= static_cast<int>(tracks.size()) - 2, "each track's heightmap spacing is read (folders and archives)");
        Check(fromArchives >= 3, "tracks inside .saf archives too");

        // What a checkpoint's vector is: the way through it (the track's direction there) - it points
        // from the checkpoint before to the one after, round each track's lap.
        double cosSum = 0;
        int cosCount = 0, against = 0;
        for (const auto& t : tracks) {
            const std::vector<int>& seq = t.normal;
            for (size_t i = 1; i + 1 < seq.size(); ++i) {
                const laptimer::Gate& prev = t.gates[static_cast<size_t>(seq[i - 1])];
                const laptimer::Gate& g = t.gates[static_cast<size_t>(seq[i])];
                const laptimer::Gate& next = t.gates[static_cast<size_t>(seq[i + 1])];
                const double ax = next.cx - prev.cx, az = next.cz - prev.cz;
                const double la = std::sqrt(ax * ax + az * az), lv = std::sqrt(g.dx * g.dx + g.dz * g.dz);
                if (la < 1e-6 || lv < 1e-6)
                    continue;
                const double c = (ax * g.dx + az * g.dz) / (la * lv);
                cosSum += c;
                ++cosCount;
                if (c < 0)
                    ++against;
            }
        }
        printf("    checkpoint vectors against the track's way round (prev -> next): average cos %.2f over %d, %d pointing back\n",
               cosCount ? cosSum / cosCount : 0.0, cosCount, against);
        Check(cosCount > 500 && cosSum / cosCount > 0.7, "each checkpoint's vector points the way round the track");

        // Each track ridden: from checkpoint to checkpoint in its lap's order, three times round, through
        // each the way a rider goes (from 6 units before its middle to 2 past it, along its way through;
        // straight on between them), 25 units a second.
        int timed = 0, notTold = 0;
        for (const auto& t : tracks) {
            if (t.normal.front() != t.normal.back())
                continue;  // (a straight run, not a loop: Straight Rhythm)
            laptimer::g_tracks.clear();
            laptimer::g_tracks.push_back(t);
            laptimer::SetTerrain(t.spacing);  // (as the game sends it: the track's own heightmap)
            laptimer::g_taken = true;
            laptimer::g_testTracks = true;
            laptimer::g_t = laptimer::Timing{};
            std::vector<std::pair<double, double>> path;
            for (int lap = 0; lap < 3; ++lap)
                for (size_t i = lap ? 1 : 0; i < t.normal.size(); ++i) {
                    const laptimer::Gate& g = t.gates[static_cast<size_t>(t.normal[i])];
                    const double lv = std::sqrt(g.dx * g.dx + g.dz * g.dz);
                    path.push_back({ g.cx - 6.0 * g.dx / lv, g.cz - 6.0 * g.dz / lv });
                    path.push_back({ g.cx + 2.0 * g.dx / lv, g.cz + 2.0 * g.dz / lv });
                }
            double now = 0;
            int laps = 0;
            for (size_t i = 0; i + 1 < path.size(); ++i) {
                const double ax = path[i].first, az = path[i].second, bx = path[i + 1].first, bz = path[i + 1].second;
                const double len = std::sqrt((bx - ax) * (bx - ax) + (bz - az) * (bz - az));
                const int steps = std::max(1, static_cast<int>(len / 25.0 / 0.01));
                for (int k = 1; k <= steps; ++k) {
                    // (Every step under the jump a respawn makes.)
                    const double f = static_cast<double>(k) / steps;
                    const double pos[3] = { ax + (bx - ax) * f, 0.0, az + (bz - az) * f };
                    now += 0.01;
                    laptimer::Update(now, true, pos);
                    const laptimer::Display d = laptimer::Current(now);
                    if (d.finished && d.valid && d.lap > laps)
                        laps = d.lap;
                }
            }
            if (laptimer::g_t.track < 0)
                ++notTold;
            if (laps >= 2)
                ++timed;
            else
                printf("      %s: %d whole laps timed (track %s)\n", t.name.c_str(), laps, laptimer::g_t.track < 0 ? "not told" : "told");
        }
        printf("    %d tracks ridden round: %d with 2 whole laps timed, %d never told\n", static_cast<int>(tracks.size()), timed, notTold);
        Check(timed >= static_cast<int>(tracks.size()) - 3, "riding round each of the game folder's tracks, whole laps are timed");
    }

    printf(g_failed ? "%d check(s) FAILED\n" : "All checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
