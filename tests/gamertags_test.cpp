// gamertags_test.cpp - the gamer tags' matching, on recordings of real races (a .mxdemo's path, the track's
// width in game units): the file handed over in 4 KB pieces as the game writes it, the riders drawn where
// the recording has them - as the game draws them (guessed ahead from the last state received), those near
// enough the camera and in its sight, in a shuffled order - and each one's tag checked against who it is.
//   cl /EHsc /O2 /std:c++17 /DNOMINMAX tests\gamertags_test.cpp src\gamertags.cpp
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "../src/gamertags.h"

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
const wchar_t* PersonalFolder() { return L""; }
const wchar_t* DataFolder() { return L""; }
}  // namespace paths
}  // namespace mxcam
using namespace mxcam;

struct State {
    unsigned recorded, tick;
    double   x, z;
};

int main(int argc, char** argv)
{
    if (argc < 3)
        return printf("gamertags_test <file.mxdemo> <track width> [drawn late, s] [from, s] [frames a second]\n"), 2;
    FILE* f = fopen(argv[1], "rb");
    if (!f)
        return printf("can't open %s\n", argv[1]), 2;
    std::string b;
    {
        static char chunk[1 << 16];
        size_t n;
        while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
            b.append(chunk, n);
    }
    fclose(f);
    const double width = atof(argv[2]), extra = argc > 3 ? atof(argv[3]) : 0.0, from = argc > 4 ? atof(argv[4]) : 0.0;
    const int skinKind = argc > 6 ? atoi(argv[6]) : 1;
    const bool hold = argc > 7 && atoi(argv[7]) == 1;
    // 2: drawn as it was "drawn late" seconds ago (the game's buffer), give or take up to [8] s a rider, from
    // the states it had by then (as the game draws the others: about a second late)
    const bool buffered = argc > 7 && atoi(argv[7]) == 2;
    const double spread = argc > 8 ? atof(argv[8]) : 0.0;  // drawn where the last state received put it (not carried on)  // 1: a skin each, 0: one for all, 2: half of them the same one
    const double frameTime = 1.0 / (argc > 5 ? atof(argv[5]) : 144.0);
    // The records: where each ends (what the file held at each tick), and everyone's states.
    int count = 0, version = 0;
    sscanf(b.c_str(), "MX%d %d", &version, &count);
    size_t at = b.find('\n', b.find('\n') + 1) + 1;
    std::vector<std::string> names;
    for (int i = 0; i < count; ++i) {
        for (int k = 0; k < 4; ++k) {
            const size_t e = b.find('\n', at);
            if (k == 2)
                names.push_back(b.substr(at, e - at));
            at = e + 1;
        }
    }
    std::vector<std::vector<State>> all(static_cast<size_t>(count));
    std::vector<std::pair<unsigned, size_t>> ends;
    unsigned previous = 0;
    const auto Word = [&](size_t o) {
        unsigned v;
        memcpy(&v, b.data() + o, 4);
        return v;
    };
    while (at + 9 <= b.size()) {
        const unsigned length = Word(at) ^ previous, tick = Word(at + 4);
        if (at + 9 + length > b.size())
            break;
        if (b[at + 8] == 1) {
            for (unsigned k = 0; k + 44 <= length; k += 44) {
                const unsigned char* p = reinterpret_cast<const unsigned char*>(b.data()) + at + 9 + k;
                unsigned t;
                memcpy(&t, p + 1, 4);
                const auto V = [&](int o) { return (p[o] | p[o + 1] << 8 | p[o + 2] << 16) / 16777216.0 * width; };
                if (p[0] < count)
                    all[p[0]].push_back({ tick, t, V(5), V(11) });
            }
        }
        at += 9 + length;
        previous = length;
        ends.push_back({ tick, at });
    }
    const unsigned last = ends.back().first;
    int own = -1;
    for (int s = 0; s < count && own < 0; ++s)
        if (!all[s].empty() && all[s][0].tick == 0 && all[s][0].recorded == 0)
            own = s;
    printf("%s: %d riders, %.0f s, yours %s, %.0f KB\n", argv[1], count, last / 128.0, own >= 0 ? names[own].c_str() : "none", b.size() / 1024.0);

    gamertags::TestStart(width);
    std::mt19937 random(7);
    std::normal_distribution<double> noise(0.0, 0.35);
    std::vector<double> sway(static_cast<size_t>(count), 0.0);
    size_t fed = 0, endAt = 0;
    double matchedAt = -1, polledAt = -1;
    long long right = 0, wrong = 0, none = 0, seenNone = 0, ownTagged = 0;
    std::map<int, double> since;  // rider -> in sight since
    std::map<int, bool> got;
    std::vector<double> waits;
    std::map<std::pair<int, int>, int> wrongs;
    std::map<int, unsigned> lastTrack;  // (each rider's track: how often it was lost)
    long long trackBreaks = 0;
    std::map<int, std::array<float, 2>> lastAt;
    double frameSpent = 0, matchSpent = 0, matchMost = 0;
    long long frames = 0, matches = 0;
    const double start = 1000.0;
    for (double clock = from; clock * 128.0 < last; clock += frameTime) {
        const double now = start + clock, tick = clock * 128.0;
        // Everything recorded by now, but for what's still in the game's 4 KB buffer.
        while (endAt < ends.size() && ends[endAt].first <= tick)
            ++endAt;
        const size_t written = endAt ? (ends[endAt - 1].second / 4096) * 4096 : 0;
        if (now - polledAt >= 0.045) {
            polledAt = now;
            if (written > fed) {
                gamertags::TestFeed(b.data() + fed, written - fed, now);
                fed = written;
            }
        }
        // The riders as drawn: the last state received, carried on at its speed.
        struct Drawn {
            int   who;
            float p[3];
        };
        std::vector<Drawn> drawn;
        const auto Where = [&](int s, double& x, double& z) {
            const std::vector<State>& v = all[static_cast<size_t>(s)];
            if (buffered) {
                // (As a real race had them: most the same, one in seven 0.4 s less, two in seven up to [8] s more.)
                const double own = s % 7 == 3 ? -0.4 : s % 7 == 5 ? spread : s % 7 == 6 ? 0.5 * spread : 0.0;
                const double at = tick - (extra + own) * 128.0;
                const auto b = std::lower_bound(v.begin(), v.end(), at, [](const State& st, double t) { return st.tick < t; });
                if (b == v.end() || b == v.begin() || b->recorded > tick || b->tick - (b - 1)->tick > 64)
                    return false;
                const State &q = *b, &p = *(b - 1);
                const double f = (at - p.tick) / (double(q.tick) - p.tick);
                x = p.x + (q.x - p.x) * f;
                z = p.z + (q.z - p.z) * f;
                return true;
            }
            const auto it = std::upper_bound(v.begin(), v.end(), tick, [](double t, const State& st) { return t < st.recorded; });
            if (it - v.begin() < 2)
                return false;
            const State &q = *(it - 1), &p = *(it - 2);
            if (tick - q.tick > 5 * 128.0 || q.tick <= p.tick)
                return false;
            const double ahead = hold ? 0.0 : std::min(tick - extra * 128.0 - q.tick, 64.0), span = q.tick - p.tick;
            x = q.x + (q.x - p.x) / span * ahead;
            z = q.z + (q.z - p.z) / span * ahead;
            return true;
        };
        double cx = 0, cz = 0;
        int camera = own;
        if (camera < 0)  // (watching: the first one riding)
            for (int s = 0; s < count && camera < 0; ++s) {
                double x, z;
                if (Where(s, x, z))
                    camera = s;
            }
        if (camera < 0 || !Where(camera, cx, cz))
            continue;
        const double heading = clock * 0.35;  // (looking round and round: riders come into sight and go)
        for (int s = 0; s < count; ++s) {
            double x, z;
            if (!Where(s, x, z))
                continue;
            const double dx = x - cx, dz = z - cz, d = std::sqrt(dx * dx + dz * dz);
            if (s != camera && (d > 400.0 || (d > 1.0 && (dx * std::cos(heading) + dz * std::sin(heading)) / d < 0.6)))
                continue;
            sway[s] += (noise(random) - sway[s]) * 0.02;  // (the hips against the bike: a body leans)
            drawn.push_back({ s, { float(x + sway[s] + noise(random) * 0.03), 10.0f, float(z + sway[s] * 0.5) } });
        }
        std::shuffle(drawn.begin(), drawn.end(), random);
        std::vector<std::array<float, 3>> points;
        for (const Drawn& d : drawn)
            points.push_back({ d.p[0], d.p[1], d.p[2] });
        std::vector<unsigned> skinIds;
        int ownAt = -1;
        for (size_t i = 0; i < drawn.size(); ++i) {
            const int who = drawn[i].who;
            skinIds.push_back(skinKind == 0 ? 5u : skinKind == 2 && who % 2 ? 7u : 100u + static_cast<unsigned>(who));
            if (who == own)
                ownAt = static_cast<int>(i);
        }
        const auto t0 = std::chrono::steady_clock::now();
        gamertags::Frame(now, true, reinterpret_cast<const float(*)[3]>(points.data()), skinIds.data(), static_cast<int>(points.size()), ownAt);
        frameSpent += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        ++frames;
        if (now - matchedAt >= 0.15) {
            matchedAt = now;
            const auto m0 = std::chrono::steady_clock::now();
            gamertags::TestMatch(now);
            const double spent = std::chrono::duration<double>(std::chrono::steady_clock::now() - m0).count();
            matchSpent += spent;
            matchMost = std::max(matchMost, spent);
            ++matches;
        }
        if (clock < from + 3.0)
            continue;
        std::map<int, bool> inSight;
        for (size_t i = 0; i < drawn.size(); ++i) {
            const int who = drawn[i].who, slot = gamertags::TestSlotOf(static_cast<int>(i));
            const unsigned id = gamertags::TestTrackOf(static_cast<int>(i));
            if (lastTrack[who] && lastTrack[who] != id) {
                ++trackBreaks;
                static int shown = 0;
                if (getenv("TAGS_DEBUG") && shown++ < 12) {
                    printf("    lost: rider %d at %.1f %.1f, t %.3f (track %u -> %u); nearest others:", who, drawn[i].p[0], drawn[i].p[2], now, lastTrack[who], id);
                    for (size_t j = 0; j < drawn.size(); ++j)
                        if (j != i && std::hypot(drawn[j].p[0] - drawn[i].p[0], drawn[j].p[2] - drawn[i].p[2]) < 12)
                            printf(" %d at %.1f %.1f;", drawn[j].who, drawn[j].p[0], drawn[j].p[2]);
                    printf(" last frame here %.1f %.1f\n", lastAt[who][0], lastAt[who][1]);
                }
            }
            lastAt[who][0] = drawn[i].p[0];
            lastAt[who][1] = drawn[i].p[2];
            lastTrack[who] = id;
            inSight[who] = true;
            if (!since.count(who)) {
                since[who] = now;
                got[who] = false;
            }
            if (slot < 0 || (slot == own && who != own)) {  // (taken for you: nothing's drawn)
                ++none;
                if (now - since[who] > 1.0)
                    ++seenNone;
                continue;
            }
            if (slot == who) {
                ++right;
            } else {
                ++wrong;
                ++wrongs[{ who, slot }];
            }
            if (!got[who]) {
                got[who] = true;
                waits.push_back(now - since[who]);
            }
            if (who == own && gamertags::Of(static_cast<int>(i)).name[0])
                ++ownTagged;
        }
        for (auto it = since.begin(); it != since.end();) {
            if (!inSight.count(it->first)) {
                got.erase(it->first);
                it = since.erase(it);
            } else {
                ++it;
            }
        }
    }
    std::sort(waits.begin(), waits.end());
    const double total = std::max(1.0, double(right + wrong + none));
    printf("  rider-frames %.0f: right %.2f%%  wrong %.3f%%  no tag %.2f%% (in sight over 1 s without one: %.2f%%)\n", total, 100 * right / total,
           100 * wrong / total, 100 * none / total, 100 * seenNone / total);
    printf("  riders lost from frame to frame and found as new ones: %lld times\n", trackBreaks);
    printf("  time to a tag: middle %.2f s, 95%% within %.2f s; your own tagged: %lld\n", waits.empty() ? -1 : waits[waits.size() / 2],
           waits.empty() ? -1 : waits[waits.size() * 95 / 100], ownTagged);
    printf("  cost: game thread %.2f us a frame; matching thread %.0f us each (most %.0f), %.3f%% of one core\n", 1e6 * frameSpent / std::max(1ll, frames),
           1e6 * matchSpent / std::max(1ll, matches), 1e6 * matchMost, 100 * matchSpent / (last / 128.0 - from));
    std::vector<std::pair<int, std::pair<int, int>>> worst;
    for (auto& w : wrongs)
        worst.push_back({ w.second, w.first });
    std::sort(worst.rbegin(), worst.rend());
    for (size_t k = 0; k < worst.size() && k < 6; ++k)
        printf("    wrong %d frames: %s shown as %s\n", worst[k].first, names[worst[k].second.first].c_str(), names[worst[k].second.second].c_str());
    const bool ok = right > 0 && 100 * wrong / total < (skinKind == 1 ? 0.4 : 1.5) && 100 * right / total > (skinKind == 1 ? 95 : 80) && ownTagged == 0;
    printf("  [%s]\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
