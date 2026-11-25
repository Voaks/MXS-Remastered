// gamertags.cpp - see gamertags.h.
#include "gamertags.h"

#include <windows.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "log.h"
#include "paths.h"

namespace mxcam {
namespace gamertags {
namespace {

constexpr double kTicks     = 128.0;  // the game's, a second
constexpr double kKeep      = 4.0;    // seconds of everyone's whereabouts kept
constexpr double kCompare   = 1.5;    // seconds of them compared
constexpr double kEarliest  = -2.5;   // s: how long ago the game may draw a rider as being (and ahead, kLatest)
constexpr double kLatest    = 0.4;
constexpr double kNoDelay   = 99.0;
constexpr double kRecent    = 0.5;    // (the later, the more it counts: a third as much this long before)
constexpr float  kReach     = 6.0f;   // game units: how far a rider may be from where it would be by now
constexpr float  kNewReach  = 12.0f;  // game units: and further while its speed isn't known (a network update apart at speed)
constexpr float  kStep      = 0.4f;   // game units: less isn't a move (a body swaying on its bike)
constexpr float  kOtherSkin = 4.0f;   // and how much further another skin counts as
constexpr float  kMixed     = 3.0f;   // game units: two riders this near may be taken for one another (less than a start gate is wide: 3.8)
constexpr int    kMaxRiders = 128;

// ---- The recorded file ----------------------------------------------------------------------------
// "MX06 <riders> <race ticks> <laps> <?>\n<track>\n", then each rider's bike and skins, number, name
// and an empty line, then records: their length xor the one before's, the game's tick, a kind (1: riders'
// states, 2: messages, ...) and that many bytes. A state is 44 bytes: the rider's place in the list, the
// tick it was there, and where (x, y, z: 24 bits each, shares of the track's width), then how it's turned.
struct Where {
    unsigned tick;
    float    x, z;  // shares of the track's width
};
struct Rider {
    std::string       name;
    std::deque<Where> path;  // by the tick it was there
    std::deque<Where> held;  // by the tick it came in
};

// Text the menu's font has: accents dropped, anything else left out.
std::string Plain(const std::string& raw)
{
    static const char kLatin[] =  // U+00C0..U+00FF
        "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    std::string out;
    for (size_t i = 0; i < raw.size();) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        unsigned code = c;
        size_t used = 1;
        const auto More = [&](size_t k) { return i + k < raw.size() && (static_cast<unsigned char>(raw[i + k]) & 0xC0) == 0x80; };
        if (c >= 0xC2 && c < 0xE0 && More(1)) {
            code = (c & 0x1Fu) << 6 | (static_cast<unsigned char>(raw[i + 1]) & 0x3Fu);
            used = 2;
        } else if (c >= 0xE0 && c < 0xF0 && More(1) && More(2)) {
            code = 0x10000;  // (none of these in the font)
            used = 3;
        } else if (c >= 0xF0 && More(1) && More(2) && More(3)) {
            code = 0x10000;
            used = 4;
        }
        i += used;
        if (code >= 32 && code <= 126)
            out += static_cast<char>(code);
        else if (code >= 0xC0 && code <= 0xFF)
            out += kLatin[code - 0xC0];
    }
    const size_t from = out.find_first_not_of(' ');
    if (from == std::string::npos)
        return std::string();
    out = out.substr(from, out.find_last_not_of(' ') - from + 1);
    const bool readable = std::any_of(out.begin(), out.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    });
    return readable ? out.substr(0, 40) : std::string();
}

class Recording {
public:
    void Reset()
    {
        pending_.clear();
        at_ = 0;
        riders.clear();
        track.clear();
        header_ = states_ = bad = false;
        previous_ = 0;
        tick = 0;
        own = -1;
    }
    void Feed(const char* bytes, size_t count)
    {
        if (bad)
            return;
        pending_.append(bytes, count);
        if (!header_ && !ReadHeader())
            return;
        while (pending_.size() - at_ >= 9) {
            const unsigned char* p = reinterpret_cast<const unsigned char*>(pending_.data()) + at_;
            const unsigned length = Word(p) ^ previous_, t = Word(p + 4);
            if (length > (1u << 20) || p[8] > 63) {
                bad = true;
                break;
            }
            if (pending_.size() - at_ < 9 + static_cast<size_t>(length))
                break;
            tick = t;
            if (p[8] == 1)
                States(p + 9, length, t);
            previous_ = length;
            at_ += 9 + static_cast<size_t>(length);
        }
        if (at_ > (1u << 16) || at_ == pending_.size()) {
            pending_.erase(0, at_);
            at_ = 0;
        }
    }
    bool Ready() const { return header_ && !bad; }

    std::vector<Rider> riders;
    std::string        track;
    unsigned           tick = 0;   // the last record's
    int                own = -1;   // you: the first state recorded, at the start (-1: you came to watch)
    bool               bad = false;

private:
    static unsigned Word(const unsigned char* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<unsigned>(p[3]) << 24; }

    // Whether the whole list of riders is there (and read).
    bool ReadHeader()
    {
        size_t at = 0;
        const auto Line = [&](std::string& out) {
            const size_t end = pending_.find('\n', at);
            if (end == std::string::npos)
                return false;
            out = pending_.substr(at, end - at);
            at = end + 1;
            return true;
        };
        std::string line, trackName;
        int version = 0, count = 0;
        if (!Line(line)) {
            bad = pending_.size() > 4096;
            return false;
        }
        char* end = nullptr;
        if (line.compare(0, 2, "MX") == 0)
            version = static_cast<int>(std::strtol(line.c_str() + 2, &end, 10));
        if (end && *end == ' ')
            count = static_cast<int>(std::strtol(end, nullptr, 10));
        if (version <= 0 || count <= 0 || count > kMaxRiders) {
            bad = true;
            return false;
        }
        if (!Line(trackName))
            return false;
        std::vector<Rider> all(static_cast<size_t>(count));
        for (Rider& r : all) {
            std::string skins, number, name, empty;
            if (!Line(skins) || !Line(number) || !Line(name) || !Line(empty)) {
                bad = pending_.size() > (1u << 20);
                return false;
            }
            r.name = Plain(name);
        }
        riders.swap(all);
        track = trackName;
        at_ = at;
        header_ = true;
        return true;
    }
    void States(const unsigned char* p, unsigned length, unsigned recorded)
    {
        for (unsigned k = 0; k + 44 <= length; k += 44, p += 44) {
            const unsigned slot = p[0], t = Word(p + 1);
            if (slot >= riders.size())
                continue;
            if (!states_) {
                states_ = true;
                if (recorded == 0 && t == 0)
                    own = static_cast<int>(slot);
            }
            std::deque<Where>& path = riders[slot].path;
            if (!path.empty() && t <= path.back().tick)
                continue;
            const auto Share = [](const unsigned char* q) { return static_cast<float>((q[0] | q[1] << 8 | q[2] << 16) / 16777216.0); };
            path.push_back(Where{ t, Share(p + 5), Share(p + 11) });
            while (path.size() > 2 && path.front().tick + static_cast<unsigned>(kKeep * kTicks) < t)
                path.pop_front();
            // (And when it came in: the last of a record's states for it, at the record's tick.)
            std::deque<Where>& held = riders[slot].held;
            if (!held.empty() && held.back().tick >= recorded)
                held.back() = Where{ held.back().tick, path.back().x, path.back().z };
            else
                held.push_back(Where{ recorded, path.back().x, path.back().z });
            while (held.size() > 2 && held.front().tick + static_cast<unsigned>(kKeep * kTicks) < recorded)
                held.pop_front();
        }
    }

    std::string pending_;
    size_t      at_ = 0;
    bool        header_ = false, states_ = false;
    unsigned    previous_ = 0;
};

// ---- The riders drawn, followed from frame to frame (the game's thread) ------------------------------
struct Track {
    unsigned    id = 0, skin = 0;
    float       pos[3] = {}, vel[3] = {}, anchor[3] = {};  // last drawn; speed; where it last moved to
    double      seen = -1, moved = -1;  // last drawn; last drawn somewhere else
    int         slot = -1, moves = 0;  // (moves: how often it's been seen to move, for its speed)
    std::string name;
    float       fade = 0, lift = 0;
    bool        used = false, own = false;
};
struct Seen {  // a rider drawn, for the thread
    unsigned id, skin;
    double   t;
    float    x, z;
    bool     own;    // yours, as far as it looks
    bool     mixed;  // right beside another one: which of the two it is from here on is in doubt
};
struct Named {
    unsigned    id;
    int         slot;
    std::string name;
};

struct Shared {
    std::mutex          lock;
    std::vector<Seen>   seen;   // the game's thread -> ours
    std::vector<Named>  named;  // ours -> the game's thread
    unsigned            version = 0;
    std::atomic<bool>   on{ false }, running{ false };
    std::atomic<double> worldSize{ 0.0 };
    std::atomic<double> clock{ 0.0 };  // Frame's clock less Steady's
    std::atomic<bool>   report{ false };  // F11: the report asked for
};
// (Never freed: the thread may still be going as the game closes.)
Shared& TheShared()
{
    static Shared* shared = new Shared;
    return *shared;
}

std::vector<Track> g_tracks;
std::vector<int>   g_of;  // riders[i]'s track
std::vector<Seen>  g_unsent;
unsigned           g_nextId = 1, g_versionSeen = 0;
double             g_sentAt = -1, g_lastFrame = -1;

// (For tests: the track's width, its middle where the world's is.)
double TestWorld()
{
    static const double size = [] {
        char text[32] = "";
        return GetEnvironmentVariableA("MXCAM_TEST_TAG_WORLD", text, sizeof text) > 0 ? std::atof(text) : 0.0;
    }();
    return size;
}

// Whether another rider is so near this one that the two may have been taken for one another.
bool Mixed(const Track& t)
{
    for (const Track& o : g_tracks) {
        if (&o == &t || !o.used || (o.skin != t.skin && o.skin && t.skin))
            continue;
        const float dx = o.pos[0] - t.pos[0], dy = o.pos[1] - t.pos[1], dz = o.pos[2] - t.pos[2];
        if (dx * dx + dy * dy + dz * dz < kMixed * kMixed)
            return true;
    }
    return false;
}

double Steady()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- Who's who (our thread) ----------------------------------------------------------------------
class Matcher {
public:
    // Another recording: nothing known.
    void Reset()
    {
        recording.Reset();
        Quiet();
        votes_.clear();
        bias_ = 0;
        delay_ = 0;
        delayKnown_ = false;
        lag_.clear();
        lagCount_.clear();
        told_ = false;
    }
    // Nothing written for a while (the game paused, the race over): its clock is found again when it goes on.
    void Quiet()
    {
        offsets_.clear();
        taken_.clear();
        matched = 0;
    }
    void Forget()
    {
        paths_.clear();
        doubt_.clear();
        skins_.clear();
        own_.clear();
    }
    // The file grew, and is read to its end (now: Frame's clock).
    void Grew(double now)
    {
        if (!recording.Ready())
            return;
        // What a read ends on was written before it: the least (now - tick) of late is nearest the truth.
        offsets_.push_back({ now, now - recording.tick / kTicks });
        while (now - offsets_.front().first > 10.0)
            offsets_.pop_front();
    }
    void Add(const std::vector<Seen>& seen)
    {
        for (const Seen& s : seen) {
            std::deque<Seen>& path = paths_[s.id];
            if (s.mixed) {  // (where it was before may have been another's)
                path.clear();
                doubt_.insert(s.id);
            }
            path.push_back(s);
            own_[s.id] = s.own;
            skins_[s.id] = s.skin;
        }
    }
    void Match(double now, double worldSize)
    {
        for (auto it = paths_.begin(); it != paths_.end();) {
            std::deque<Seen>& path = it->second;
            while (!path.empty() && now - path.front().t > kKeep)
                path.pop_front();
            if (path.empty()) {
                taken_.erase(it->first);
                doubt_.erase(it->first);
                own_.erase(it->first);
                skins_.erase(it->first);
                it = paths_.erase(it);
            } else {
                ++it;
            }
        }
        if (!recording.Ready() || offsets_.empty() || worldSize <= 0)
            return;
        if (lag_.size() != recording.riders.size()) {
            lag_.assign(recording.riders.size(), 0.0);
            lagCount_.assign(recording.riders.size(), 0);
        }
        double offset = offsets_.front().second;
        for (const auto& o : offsets_)
            offset = std::min(offset, o.second);

        // The game draws the other riders a while after they were where it draws them (about a second, the
        // same for most of them: a buffer against the network's ups and downs), each a little differently.
        // So each rider drawn is compared with each one recorded at whatever time fits best - nearest the
        // usual delay (theirs, or everyone's), as two riders on the same line a moment apart would fit too.
        struct Pair {
            double   cost;
            unsigned id;
            int      slot, count;
            double   shift;
        };
        std::vector<Pair> pairs;
        std::vector<double> delays;  // of the riders drawn that fit one recorded, and no other
        const auto Fit = [&](const std::deque<Seen>& path, const std::deque<Where>& ride, double shift, int& count) {
            const double latest = offset + ride.back().tick / kTicks - shift;  // (seen later than this: not recorded yet)
            double sum = 0, weight = 0;
            count = 0;
            int skip = 0;
            for (auto seen = path.rbegin(); seen != path.rend(); ++seen) {
                if (seen->t > latest)
                    continue;
                if (latest - seen->t > kCompare)
                    break;
                if (skip++ % 2)
                    continue;  // (every other one is enough)
                const double tick = (seen->t - offset + shift) * kTicks;
                const auto after = std::lower_bound(ride.begin(), ride.end(), tick, [](const Where& w, double t) { return w.tick < t; });
                if (after == ride.end() || after == ride.begin())
                    continue;
                const Where &b = *after, &a = *(after - 1);
                const double span = double(b.tick) - a.tick;
                if (span > kTicks || span <= 0)
                    continue;
                const double f = (tick - a.tick) / span;
                const double dx = (a.x + (b.x - a.x) * f) * worldSize - seen->x, dz = (a.z + (b.z - a.z) * f) * worldSize - seen->z;
                const double w = std::exp((seen->t - latest) / kRecent);
                sum += std::min(std::sqrt(dx * dx + dz * dz), 30.0) * w;
                weight += w;
                ++count;
            }
            return count >= 3 ? sum / weight : 1e9;
        };
        for (const auto& entry : paths_) {
            const std::deque<Seen>& path = entry.second;
            if (!InSight(path, now))
                continue;
            const Seen& last = path.back();
            const auto had = taken_.find(entry.first);
            const bool doubt = doubt_.count(entry.first) != 0;
            double best = 1e9, second = 1e9, bestShift = 0;
            for (size_t s = 0; s < recording.riders.size(); ++s) {
                const std::deque<Where>& ride = recording.riders[s].path;
                if (ride.size() < 2 || ride.back().tick + static_cast<unsigned>(3 * kTicks) < recording.tick)
                    continue;  // (gone)
                if (own_[entry.first] && static_cast<int>(s) != recording.own)
                    continue;  // (yours is only ever yourself: not the rider beside you, when you aren't in the file)
                bool close = false;  // (anywhere near where it's drawn, of late)
                for (auto w = ride.rbegin(); w != ride.rend() && !close; ++w)
                    close = std::hypot(w->x * worldSize - last.x, w->z * worldSize - last.z) < 50.0;
                if (!close)
                    continue;
                const double expect = lagCount_[s] >= 3 ? lag_[s] : delayKnown_ ? delay_ : kNoDelay;
                const auto Prior = [&](double shift) {
                    return expect == kNoDelay ? 0.0 : 6.0 * std::max(0.0, std::fabs(shift - expect) - 0.12);
                };
                // The best time: in tenths of a second, then in fiftieths around the best of those.
                double cost = 1e9, rated = 1e9, shift = 0;
                int count = 0;
                const auto Try = [&](double at) {
                    int n = 0;
                    const double c = Fit(path, ride, at, n);
                    if (c < 1e8 && c + Prior(at) < rated) {
                        rated = c + Prior(at);
                        cost = c;
                        shift = at;
                        count = n;
                    }
                };
                for (double at = kEarliest; at <= kLatest + 1e-9; at += 0.1)
                    Try(at);
                if (rated < 1e8)
                    for (double at = shift - 0.08; at <= shift + 0.081; at += 0.02)
                        Try(at);
                if (count < 3)
                    continue;
                if (cost < best) {
                    second = best;
                    best = cost;
                    bestShift = shift;
                } else if (cost < second) {
                    second = cost;
                }
                const bool mine = had != taken_.end() && had->second == static_cast<int>(s) && !doubt;
                if (cost < (mine ? 14.0 : 8.0))
                    pairs.push_back(Pair{ (mine ? rated * 0.7 - 1.0 : rated) - (own_[entry.first] && static_cast<int>(s) == recording.own ? 3.0 : 0.0) -
                                              (SkinOf(skins_[entry.first]) == static_cast<int>(s) ? 1.5 : 0.0),
                                          entry.first, static_cast<int>(s), count, shift });
            }
            if (best < 3.0 && second > best + 6.0)
                delays.push_back(bestShift);
        }
        // Everyone's usual delay: the middle of the clear ones'.
        if (delays.size() >= 2) {
            std::sort(delays.begin(), delays.end());
            const double middle = delays[delays.size() / 2];
            delay_ = delayKnown_ ? delay_ + 0.3 * (middle - delay_) : middle;
            delayKnown_ = true;
        }        std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.cost < b.cost; });
        std::unordered_map<unsigned, int> taken;
        std::vector<char> slotTaken(recording.riders.size(), 0);
        for (size_t i = 0; i < pairs.size(); ++i) {
            const Pair& p = pairs[i];
            if (slotTaken[static_cast<size_t>(p.slot)] || taken.count(p.id))
                continue;
            // Only when nobody left fits about as well.
            bool sure = true;
            for (size_t k = i + 1; k < pairs.size() && sure; ++k) {
                const Pair& q = pairs[k];
                if (q.cost > p.cost + 1.0)
                    break;
                if ((q.id == p.id && !slotTaken[static_cast<size_t>(q.slot)]) || (q.slot == p.slot && !taken.count(q.id)))
                    sure = false;
            }
            const auto had = taken_.find(p.id);
            if (!sure && (had == taken_.end() || had->second != p.slot))
                continue;
            taken[p.id] = p.slot;
            slotTaken[static_cast<size_t>(p.slot)] = 1;
            if (p.count >= 4)
                doubt_.erase(p.id);
            if (sure) {  // (its own delay)
                double& lag = lag_[static_cast<size_t>(p.slot)];
                int& n = lagCount_[static_cast<size_t>(p.slot)];
                lag = n ? lag + 0.3 * (p.shift - lag) : p.shift;
                n = std::min(n + 1, 100);
            }
        }
        // The skin each is drawn with, if nobody else has it: whose it is.
        for (const auto& t : taken) {
            const unsigned skin = skins_[t.first];
            if (!skin || doubt_.count(t.first))
                continue;
            SkinVotes& v = votes_[skin];
            if (v.slot == t.second) {
                v.count = std::min(v.count + 1, 40);
            } else if (--v.count <= 0) {
                v.slot = t.second;
                v.count = 1;
            }
        }
        for (auto& v : votes_)
            v.second.shown = 0;
        for (const auto& s : skins_)
            if (const auto v = votes_.find(s.second); v != votes_.end() && InSight(paths_[s.first], now))
                ++v->second.shown;
        for (auto& v : votes_)
            if (v.second.shown > 1)
                v.second.count = std::min(v.second.count, 0);  // (more than one has it)
        // Come into sight, too little of it seen yet to tell by where it was: by its skin.
        for (const auto& entry : paths_) {
            if (taken.count(entry.first) || taken_.count(entry.first) || !InSight(entry.second, now))
                continue;
            const int slot = SkinOf(skins_[entry.first]);
            if (slot >= 0 && !slotTaken[static_cast<size_t>(slot)] && entry.second.size() < 12) {
                taken[entry.first] = slot;
                slotTaken[static_cast<size_t>(slot)] = 1;
                doubt_.insert(entry.first);
            }
        }
        // (Right beside another: the name it had, till they part.)
        for (const unsigned id : doubt_) {
            const auto had = taken_.find(id);
            if (had != taken_.end() && !taken.count(id) && InSight(paths_[id], now) && !slotTaken[static_cast<size_t>(had->second)]) {
                taken[id] = had->second;
                slotTaken[static_cast<size_t>(had->second)] = 1;
            }
        }
        taken_.swap(taken);
        matched = static_cast<int>(taken_.size());
        if (!told_ && matched > 0) {
            told_ = true;
            Log("gamer tags: %d riders in the game's recording (%s), %d of those in view told apart so far (drawn %.2f s late)",
                static_cast<int>(recording.riders.size()), recording.track.c_str(), matched, -delay_);
        }
    }
    const std::unordered_map<unsigned, int>& Taken() const { return taken_; }

    // F11's report: each rider drawn, and who in the recording was nearest it - at the same time, and at
    // whatever time puts them nearest (how far off the clock or the place is).
    void Report(FILE* out, double now, double worldSize) const
    {
        double offset = offsets_.empty() ? 0.0 : offsets_.front().second;
        for (const auto& o : offsets_)
            offset = std::min(offset, o.second);
        fprintf(out, "track width %.1f, %d riders in the recording (%s), you %d, its last tick %u; clock: file tick 0 at %.3f s; riders drawn %.2f s late%s; %d told apart\n\n",
                worldSize, static_cast<int>(recording.riders.size()), recording.track.c_str(), recording.own, recording.tick,
                offset, -delay_, delayKnown_ ? "" : " (not known yet)", matched);
        const double at = offset + bias_;
        const auto Place = [&](const std::deque<Where>& line, double tick, double& x, double& z) {
            const auto after = std::lower_bound(line.begin(), line.end(), tick, [](const Where& w, double t) { return w.tick < t; });
            if (after == line.end() || after == line.begin())
                return false;
            const Where &b = *after, &a = *(after - 1);
            const double span = double(b.tick) - a.tick, f = span > 0 ? (tick - a.tick) / span : 0.0;
            x = (a.x + (b.x - a.x) * f) * worldSize;
            z = (a.z + (b.z - a.z) * f) * worldSize;
            return true;
        };
        for (const auto& entry : paths_) {
            const std::deque<Seen>& path = entry.second;
            if (path.empty() || now - path.back().t > 1.0)
                continue;
            const Seen& s = path.back();
            const auto taken = taken_.find(entry.first);
            fprintf(out, "rider drawn #%u at %.1f %.1f (%.2f s ago, %d places kept, skin %u%s%s): %s\n", entry.first, s.x, s.z, now - s.t,
                    static_cast<int>(path.size()), s.skin, s.own ? ", yours" : "", doubt_.count(entry.first) ? ", in doubt" : "",
                    taken != taken_.end() ? recording.riders[static_cast<size_t>(taken->second)].name.c_str() : "(no name)");
            // The nearest at the same time, by either clock; and at any time 2 s either way.
            struct Near { double d; int slot; double shift; int held; };
            std::vector<Near> same, any;
            for (size_t r = 0; r < recording.riders.size(); ++r) {
                for (int held = 0; held < 2; ++held) {
                    const std::deque<Where>& line = held ? recording.riders[r].held : recording.riders[r].path;
                    double x, z;
                    if (Place(line, (s.t - at) * kTicks, x, z))
                        same.push_back(Near{ std::hypot(x - s.x, z - s.z), static_cast<int>(r), 0.0, held });
                    Near best{ 1e9, static_cast<int>(r), 0.0, held };
                    for (double shift = -2.0; shift <= 2.0; shift += 0.02)
                        if (Place(line, (s.t - at + shift) * kTicks, x, z) && std::hypot(x - s.x, z - s.z) < best.d)
                            best = Near{ std::hypot(x - s.x, z - s.z), static_cast<int>(r), shift, held };
                    if (best.d < 1e8)
                        any.push_back(best);
                }
            }
            std::sort(same.begin(), same.end(), [](const Near& a, const Near& b) { return a.d < b.d; });
            std::sort(any.begin(), any.end(), [](const Near& a, const Near& b) { return a.d < b.d; });
            for (size_t k = 0; k < same.size() && k < 3; ++k)
                fprintf(out, "    now:      %6.1f away  %s (%s)\n", same[k].d, recording.riders[static_cast<size_t>(same[k].slot)].name.c_str(),
                        same[k].held ? "as it came in" : "by its own clock");
            for (size_t k = 0; k < any.size() && k < 3; ++k)
                fprintf(out, "    any time: %6.1f away  %s, %+.2f s (%s)\n", any[k].d, recording.riders[static_cast<size_t>(any[k].slot)].name.c_str(),
                        any[k].shift, any[k].held ? "as it came in" : "by its own clock");
        }
        fprintf(out, "\nthe recording's riders, last known:\n");
        for (size_t r = 0; r < recording.riders.size(); ++r) {
            const Rider& rider = recording.riders[r];
            if (rider.path.empty())
                continue;
            const Where& w = rider.path.back();
            fprintf(out, "  %2d %-34s at %.1f %.1f, tick %u (%.2f s before the file's last)\n", static_cast<int>(r), rider.name.c_str(),
                    w.x * worldSize, w.z * worldSize, w.tick, (double(recording.tick) - w.tick) / kTicks);
        }
    }

    Recording recording;
    int       matched = 0;

private:
    std::deque<std::pair<double, double>>          offsets_;  // (when, now - tick)
    std::unordered_map<unsigned, std::deque<Seen>> paths_;
    std::unordered_map<unsigned, int>              taken_;
    std::unordered_set<unsigned>                   doubt_;
    struct SkinVotes {
        int slot = -1, count = 0, shown = 0;
    };
    std::unordered_map<unsigned, SkinVotes>        votes_;  // by skin
    std::unordered_map<unsigned, unsigned>         skins_;  // by track
    std::unordered_map<unsigned, bool>             own_;
    static bool InSight(const std::deque<Seen>& path, double now) { return !path.empty() && now - path.back().t < 0.4; }
    // Whose skin it is (-1 not known, or more than one's).
    int SkinOf(unsigned skin) const
    {
        const auto v = skin ? votes_.find(skin) : votes_.end();
        return v != votes_.end() && v->second.count >= 10 && v->second.slot < static_cast<int>(recording.riders.size()) ? v->second.slot : -1;
    }
    double bias_ = 0;  // (the report's: nothing puts the clock right now - the delays do)
    double delay_ = 0;  // s: how long ago the game draws the riders as being (everyone's usual; negative)
    bool   delayKnown_ = false, told_ = false;
    std::vector<double> lag_;     // each recorded rider's own
    std::vector<int>    lagCount_;
};

Matcher& TheMatcher()
{
    static Matcher* matcher = new Matcher;
    return *matcher;
}

void Publish()
{
    const Matcher& m = TheMatcher();
    std::vector<Named> named;
    named.reserve(m.Taken().size());
    for (const auto& t : m.Taken()) {
        const Rider& r = m.recording.riders[static_cast<size_t>(t.second)];
        const bool you = t.second == m.recording.own;
        named.push_back(Named{ t.first, t.second, you ? std::string() : r.name });
    }
    Shared& s = TheShared();
    const std::lock_guard<std::mutex> hold(s.lock);
    s.named.swap(named);
    ++s.version;
}

// ---- The file, as the game writes it --------------------------------------------------------------
// Held open (opened over and over, a virus scanner may look through all of it each time), and with
// everything left allowed to the game meanwhile: writing it, starting it over, renaming it, deleting it.
class LiveFile {
public:
    LiveFile() = default;
    LiveFile(const LiveFile&) = delete;
    LiveFile& operator=(const LiveFile&) = delete;
    ~LiveFile() { Close(); }
#ifdef _WIN32
    bool Open(const std::wstring& path)
    {
        Close();
        handle_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        return IsOpen();
    }
    void Close()
    {
        if (IsOpen())
            CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
    bool IsOpen() const { return handle_ != INVALID_HANDLE_VALUE; }
    unsigned long long Size() const
    {
        LARGE_INTEGER size = {};
        return GetFileSizeEx(handle_, &size) ? static_cast<unsigned long long>(size.QuadPart) : 0;
    }
    size_t Read(unsigned long long at, char* out, size_t count) const
    {
        OVERLAPPED where = {};
        where.Offset = static_cast<DWORD>(at);
        where.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD got = 0;
        return ReadFile(handle_, out, static_cast<DWORD>(count), &got, &where) ? got : 0;
    }
    unsigned long long Identity() const
    {
        BY_HANDLE_FILE_INFORMATION info = {};
        return GetFileInformationByHandle(handle_, &info) ? (static_cast<unsigned long long>(info.nFileIndexHigh) << 32 | info.nFileIndexLow) : 0;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    bool Open(const std::wstring& path)
    {
        Close();
        std::string narrow;  // (UTF-8)
        for (wchar_t c : path) {
            const unsigned u = c == L'\\' ? '/' : static_cast<unsigned>(c);
            if (u < 0x80) {
                narrow += static_cast<char>(u);
            } else if (u < 0x800) {
                narrow += static_cast<char>(0xC0 | u >> 6);
                narrow += static_cast<char>(0x80 | (u & 0x3F));
            } else if (u < 0x10000) {
                narrow += static_cast<char>(0xE0 | u >> 12);
                narrow += static_cast<char>(0x80 | (u >> 6 & 0x3F));
                narrow += static_cast<char>(0x80 | (u & 0x3F));
            } else {
                narrow += static_cast<char>(0xF0 | u >> 18);
                narrow += static_cast<char>(0x80 | (u >> 12 & 0x3F));
                narrow += static_cast<char>(0x80 | (u >> 6 & 0x3F));
                narrow += static_cast<char>(0x80 | (u & 0x3F));
            }
        }
        fd_ = open(narrow.c_str(), O_RDONLY | O_CLOEXEC);
        return IsOpen();
    }
    void Close()
    {
        if (IsOpen())
            close(fd_);
        fd_ = -1;
    }
    bool IsOpen() const { return fd_ >= 0; }
    unsigned long long Size() const
    {
        struct stat info = {};
        return fstat(fd_, &info) == 0 ? static_cast<unsigned long long>(info.st_size) : 0;
    }
    size_t Read(unsigned long long at, char* out, size_t count) const
    {
        const ssize_t got = pread(fd_, out, count, static_cast<off_t>(at));
        return got > 0 ? static_cast<size_t>(got) : 0;
    }
    unsigned long long Identity() const
    {
        struct stat info = {};
        return fstat(fd_, &info) == 0 ? static_cast<unsigned long long>(info.st_ino) : 0;
    }

private:
    int fd_ = -1;
#endif
};

void Work()
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#endif
    Shared& s = TheShared();
    Matcher& m = TheMatcher();
    m.Reset();
    m.Forget();
    const std::wstring path = std::wstring(paths::PersonalFolder()) + L"demos\\default.mxdemo";
    LiveFile file;
    std::vector<char> buffer(1 << 18);
    unsigned long long at = 0, identity = 0;
    // caughtUp: what's read from now on is being written now. live: it is.
    bool caughtUp = false, live = false, badTold = false, quietTold = false;
    int retries = 0;
    double grewAt = -1e9, lookedAt = -1e9, matchedAt = -1e9, badAt = -1e9;
    const double startedAt = Steady();
    while (s.on) {
        const double now = Steady();
        // The file wasn't there, or the game went on in another one under its name: looked for now and then.
        if (now - lookedAt > 2.0 && (!file.IsOpen() || now - grewAt > 1.5)) {
            lookedAt = now;
            LiveFile again;
            if (!again.Open(path)) {
                file.Close();
            } else if (!file.IsOpen() || again.Identity() != identity) {
                again.Close();
                if (file.Open(path)) {
                    identity = file.Identity();
                    at = 0;
                    caughtUp = live = false;
                    retries = 0;
                    m.Reset();
                }
            }
        }
        if (file.IsOpen()) {
            const unsigned long long size = file.Size();
            if (size < at) {  // started over: a new race, another track
                at = 0;
                caughtUp = true;
                retries = 0;
                m.Reset();
            } else if (m.recording.bad && retries < 3 && now - badAt > 5.0) {
                ++retries;  // (started over and already longer than it was?)
                at = 0;
                caughtUp = false;
                m.Reset();
            }
            bool grew = false;
            while (at < size && s.on && !m.recording.bad) {
                const size_t got = file.Read(at, buffer.data(), static_cast<size_t>(std::min<unsigned long long>(buffer.size(), size - at)));
                if (!got)
                    break;
                m.recording.Feed(buffer.data(), got);
                at += got;
                grew = true;
                if (m.recording.bad) {
                    badAt = now;
                    if (!badTold) {
                        badTold = true;
                        Log("gamer tags: the game's recording isn't as expected at byte %llu (another version of the game?)", at);
                    }
                }
            }
            if (grew && caughtUp && !m.recording.bad) {
                if (!live)
                    Log("gamer tags: the game is recording: %d riders%s", static_cast<int>(m.recording.riders.size()),
                        m.recording.own >= 0 ? ", you among them" : "");
                live = true;
                grewAt = now;
                m.Grew(Steady() + s.clock);
            }
            if (at >= size)
                caughtUp = true;
        }
        if (live && (now - grewAt > 5.0 || m.recording.bad)) {
            live = false;
            m.Quiet();
        }
        if (!live && !quietTold && now - startedAt > 60.0) {
            quietTold = true;
            Log("gamer tags: the game isn't recording (its Record Demo setting off, or nobody riding): no tags for now");
        }
        std::vector<Seen> seen;
        {
            const std::lock_guard<std::mutex> hold(s.lock);
            seen.swap(s.seen);
        }
        m.Add(seen);
        if (now - matchedAt >= 0.15) {
            matchedAt = now;
            if (live)
                m.Match(Steady() + s.clock, s.worldSize);
            Publish();
        }
        if (s.report.exchange(false)) {
            const std::wstring where = std::wstring(paths::DataFolder()) + L"MXS_REMASTERED_tags.txt";
            FILE* out = nullptr;
            if (_wfopen_s(&out, where.c_str(), L"wb") == 0 && out) {
                fprintf(out, "MXS Remastered: gamer tags, as they were when F11 was pressed\n"
                             "the game's recording: %s, %llu bytes read of it, %s\n",
                        file.IsOpen() ? "open" : "not found", at, live ? "being written now" : "not being written");
                m.Report(out, Steady() + s.clock, s.worldSize);
                fclose(out);
                Log("gamer tags: F11 report written to MXS_REMASTERED_tags.txt");
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    m.Reset();
    m.Forget();
    Publish();
    s.running = false;
}

}  // namespace

void SetWorldSize(double size) { TheShared().worldSize = size; }

void Report() { TheShared().report = true; }

void Frame(double now, bool on, const float (*riders)[3], const unsigned* skins, int count, int own)
{
    Shared& s = TheShared();
    if (!on) {
        if (s.on) {
            s.on = false;
            g_tracks.clear();
            g_of.clear();
            g_unsent.clear();
        }
        return;
    }
    s.clock = now - Steady();
    const float middle = static_cast<float>(0.5 * TestWorld());
    if (middle > 0)
        s.worldSize = TestWorld();
    if (!s.on) {
        if (s.running)
            return;  // (the last thread still letting go)
        s.on = true;
        s.running = true;
        std::thread(Work).detach();
    }
    const float dt = static_cast<float>(g_lastFrame >= 0 ? std::min(std::max(now - g_lastFrame, 0.0), 0.1) : 0.0);
    g_lastFrame = now;

    // Each rider drawn is one of the last frames' - where it was, or where it would be by now at its speed
    // (the game may move a rider smoothly, or in steps as its network updates come in, held still in
    // between): the nearest pairs first, over them all (one at a time, in the order they're drawn, a rider
    // took the one in front of it on the same line).
    struct Candidate {
        float d;
        int   i, k;
    };
    static std::vector<Candidate> candidates;
    candidates.clear();
    for (Track& t : g_tracks)
        t.used = false;
    g_of.assign(static_cast<size_t>(std::max(count, 0)), -1);
    const size_t known = g_tracks.size();
    for (int i = 0; i < count; ++i) {
        const unsigned skin = skins ? skins[i] : 0;
        for (size_t k = 0; k < known; ++k) {
            const Track& t = g_tracks[k];
            const float ahead = static_cast<float>(now - t.moved);
            float held2 = 0, moved2 = 0;
            for (int c = 0; c < 3; ++c) {
                const float h = t.pos[c] - riders[i][c], m = t.anchor[c] + t.vel[c] * ahead - riders[i][c];
                held2 += h * h;
                moved2 += m * m;
            }
            const float d2 = std::min(held2, moved2);
            // (Its speed not known yet - seen only standing, or once: as far as a rider can go between two
            // network updates; and a little more besides the speed it has, for its jumps to vary.)
            const float speed = std::sqrt(t.vel[0] * t.vel[0] + t.vel[1] * t.vel[1] + t.vel[2] * t.vel[2]);
            const float reach = kReach + (t.moves < 2 ? kNewReach : 0.1f * speed);
            if (d2 < reach * reach)
                candidates.push_back(Candidate{ std::sqrt(d2) + (t.skin != skin && t.skin && skin ? kOtherSkin : 0.0f), i, static_cast<int>(k) });
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.d < b.d; });
    for (const Candidate& c : candidates) {
        if (g_of[static_cast<size_t>(c.i)] >= 0 || g_tracks[static_cast<size_t>(c.k)].used)
            continue;
        g_of[static_cast<size_t>(c.i)] = c.k;
        g_tracks[static_cast<size_t>(c.k)].used = true;
    }
    for (int i = 0; i < count; ++i) {
        int best = g_of[static_cast<size_t>(i)];
        if (best < 0) {
            Track t;
            t.id = g_nextId++;
            t.moved = now;
            std::copy(riders[i], riders[i] + 3, t.anchor);
            best = static_cast<int>(g_tracks.size());
            g_tracks.push_back(std::move(t));
        } else {
            // Its speed from one move to the next (held still between network updates: not slowing down;
            // a body swaying on its bike isn't a move).
            Track& t = g_tracks[static_cast<size_t>(best)];
            const double passed = now - t.moved;
            float step2 = 0;
            for (int c = 0; c < 3; ++c)
                step2 += (riders[i][c] - t.anchor[c]) * (riders[i][c] - t.anchor[c]);
            if (step2 > kStep * kStep && passed > 1e-4) {
                const float k = static_cast<float>(1.0 - std::exp(-passed * 8.0));
                for (int c = 0; c < 3; ++c)
                    t.vel[c] += (static_cast<float>((riders[i][c] - t.anchor[c]) / passed) - t.vel[c]) * k;
                std::copy(riders[i], riders[i] + 3, t.anchor);
                t.moved = now;
                ++t.moves;
            } else if (passed > 0.5) {
                std::fill(t.vel, t.vel + 3, 0.0f);  // (standing still)
                std::copy(riders[i], riders[i] + 3, t.anchor);
                t.moved = now;
            }
        }
        Track& t = g_tracks[static_cast<size_t>(best)];
        std::copy(riders[i], riders[i] + 3, t.pos);
        t.skin = skins ? skins[i] : 0;
        t.own = i == own;
        t.seen = now;
        t.used = true;
        g_of[static_cast<size_t>(i)] = best;
    }
    // To the thread, and what it made of them (never waited for).
    if (now - g_sentAt >= 0.04 || now < g_sentAt) {
        g_sentAt = now;
        if (g_unsent.size() > 4096)
            g_unsent.clear();
        for (const Track& t : g_tracks)
            if (t.used)
                g_unsent.push_back(Seen{ t.id, t.skin, now, t.pos[0] + middle, t.pos[2] + middle, t.own, Mixed(t) });
        if (s.lock.try_lock()) {
            if (s.seen.size() > 20000)
                s.seen.clear();
            s.seen.insert(s.seen.end(), g_unsent.begin(), g_unsent.end());
            g_unsent.clear();
            if (s.version != g_versionSeen) {
                g_versionSeen = s.version;
                for (Track& t : g_tracks)
                    t.slot = -1;
                for (const Named& n : s.named) {
                    for (Track& t : g_tracks) {
                        if (t.id != n.id)
                            continue;
                        t.slot = n.slot;
                        if (t.name != n.name)
                            t.name = n.name;
                        break;
                    }
                }
            }
            s.lock.unlock();
        }
    }
    for (Track& t : g_tracks)
        t.fade = std::min(1.0f, std::max(0.0f, t.fade + dt * (t.slot >= 0 ? 4.0f : -4.0f)));
    // Out of sight for a while: forgotten (the last one takes its place).
    for (size_t k = g_tracks.size(); k-- > 0;) {
        if (g_tracks[k].used || now - g_tracks[k].seen < 0.6)
            continue;
        const size_t last = g_tracks.size() - 1;
        if (k != last) {
            g_tracks[k] = std::move(g_tracks[last]);
            for (int& of : g_of)
                if (of == static_cast<int>(last))
                    of = static_cast<int>(k);
        }
        g_tracks.pop_back();
    }
}

Tag Of(int i)
{
    static float none = 0;
    if (i < 0 || i >= static_cast<int>(g_of.size()) || g_of[static_cast<size_t>(i)] < 0)
        return Tag{ "", 0.0f, &none };
    Track& t = g_tracks[static_cast<size_t>(g_of[static_cast<size_t>(i)])];
    if (t.own)
        return Tag{ "", 0.0f, &t.lift };  // (yours: never a name, whoever it's next to)
    return Tag{ t.name.c_str(), t.fade, &t.lift };
}

// ---- Tests ----------------------------------------------------------------------------------------
void TestStart(double worldSize)
{
    Shared& s = TheShared();
    s.on = true;
    s.running = true;
    s.worldSize = worldSize;
    TheMatcher().Reset();
    TheMatcher().Forget();
    g_tracks.clear();
    g_lastFrame = g_sentAt = -1;
}

void TestFeed(const char* bytes, size_t count, double now)
{
    TheMatcher().recording.Feed(bytes, count);
    TheMatcher().Grew(now);
}

void TestMatch(double now)
{
    std::vector<Seen> seen;
    {
        const std::lock_guard<std::mutex> hold(TheShared().lock);
        seen.swap(TheShared().seen);
    }
    TheMatcher().Add(seen);
    TheMatcher().Match(now, TheShared().worldSize);
    Publish();
}

int TestSlotOf(int i)
{
    if (i < 0 || i >= static_cast<int>(g_of.size()) || g_of[static_cast<size_t>(i)] < 0)
        return -1;
    return g_tracks[static_cast<size_t>(g_of[static_cast<size_t>(i)])].slot;
}

int TestOwnSlot() { return TheMatcher().recording.own; }

unsigned TestTrackOf(int i)
{
    if (i < 0 || i >= static_cast<int>(g_of.size()) || g_of[static_cast<size_t>(i)] < 0)
        return 0;
    return g_tracks[static_cast<size_t>(g_of[static_cast<size_t>(i)])].id;
}

}  // namespace gamertags
}  // namespace mxcam
