// laptimer.cpp
//   See laptimer.h. A track's timing_gates file (a track's folder, or an entry in a .saf archive):
//
//     startinggate:
//     [x y z] angle ...              one per gate slot
//     checkpoints:
//     width [x y z] [dx dy dz]       a line across the track: its middle, and the way through it (the
//                                    track's direction there, width long: the line is at right angles)
//     firstlap:
//     0 1 2 ... 36                   the checkpoints of the first lap, from the gate, in order
//     normallap:
//     36 37 ... 35 36                a lap after it, round to the finish line: on some tracks from it
//                                    too ("36 ... 36"), on most from the checkpoint after it ("44 ... 43",
//                                    the finish line 43, where the first lap ends): a lap starts at the
//                                    finish line either way
//
//   A lap is timed from crossing the first of its checkpoints (the first lap: from leaving the start
//   gate) to crossing its last; each checkpoint on the way is a split. Your best lap (of each kind,
//   first and normal, this session, only laps with no checkpoint missed) is what the live gap is
//   measured against: at each checkpoint, this lap's time there less the best lap's.
#include "laptimer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "log.h"
#include "paths.h"

namespace mxcam {
namespace laptimer {
namespace {

struct Gate {
    double cx = 0, cz = 0, dx = 0, dz = 0;  // its middle, and the way through it (as long as it's wide), x and z
};

struct Track {
    std::string        name;
    std::vector<double> slots;           // start gate slots, x z pairs
    std::vector<Gate>  gates;
    std::vector<int>   first, normal;    // the laps' checkpoints in order
    double             spacing = 0;      // its heightmap's (terrain.hf; 0 unknown)
    // Telling which track this is: the last of its checkpoints crossed, how many in a row in order, and
    // when one last followed another.
    int                last = -1;
    int                streak = 0;
    double             inOrderAt = -1e9;
};

int Numbers(const std::string& line, double* out, int max);

// terrain.hf: "<size as a power of two> <spacing> <lowest> <highest>". Its spacing (0: none).
double HeightmapSpacing(const std::string& text)
{
    double v[2] = {};
    return Numbers(text, v, 2) == 2 && v[1] > 0 ? v[1] : 0.0;
}

// ---- Reading the tracks ------------------------------------------------------------------------

// The first numbers in a line (brackets skipped), up to max; how many were read.
int Numbers(const std::string& line, double* out, int max)
{
    int n = 0;
    const char* p = line.c_str();
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t' || *p == '[' || *p == ']')
            ++p;
        if (!*p)
            break;
        char* end = nullptr;
        const double v = std::strtod(p, &end);
        if (end == p)
            break;  // (the rest: a model's file name, not a number)
        out[n++] = v;
        p = end;
    }
    return n;
}

bool ParseGates(const std::string& name, const std::string& text, Track& out)
{
    out = Track{};
    out.name = name;
    std::istringstream in(text);
    std::string line;
    enum { None, Slots, Checkpoints, First, Normal } section = None;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        size_t start = 0;
        while (start < line.size() && static_cast<unsigned char>(line[start]) <= 0x20)
            ++start;  // (spaces, and control bytes: one archive's file starts with two stray ones)
        line.erase(0, start);
        if (line.empty())
            continue;
        if (line.rfind("startinggate:", 0) == 0) { section = Slots; continue; }
        if (line.rfind("checkpoints:", 0) == 0) { section = Checkpoints; continue; }
        if (line.rfind("firstlap:", 0) == 0) { section = First; continue; }
        if (line.rfind("normallap:", 0) == 0) { section = Normal; continue; }
        if (section == Slots) {
            double v[3];
            if (line[0] == '[' && Numbers(line, v, 3) == 3) {  // "[x y z] angle ...": the first three
                out.slots.push_back(v[0]);
                out.slots.push_back(v[2]);
            }
        } else if (section == Checkpoints) {
            double v[7];
            if (Numbers(line, v, 7) == 7) {  // "width [x y z] [dx dy dz]"
                Gate g;
                g.cx = v[1];
                g.cz = v[3];
                g.dx = v[4];
                g.dz = v[6];
                out.gates.push_back(g);
            }
        } else if (section == First || section == Normal) {
            std::istringstream numbers(line);
            int n = 0;
            while (numbers >> n)
                (section == First ? out.first : out.normal).push_back(n);
        }
    }
    const int count = static_cast<int>(out.gates.size());
    for (std::vector<int>* seq : { &out.first, &out.normal }) {
        for (int n : *seq)
            if (n < 0 || n >= count)
                return false;
        // (A checkpoint twice in a row - one track's lap has "54 54" - is crossed once.)
        seq->erase(std::unique(seq->begin(), seq->end()), seq->end());
    }
    // A lap from the finish line (the last of the lap, where the first lap ends too), whichever way the
    // file lists it. (Not a straight run's - Straight Rhythm's laps, 3 to 20, never pass 20 to start.)
    if (!out.normal.empty() && !out.first.empty() && out.first.back() == out.normal.back() && out.normal.front() != out.normal.back())
        out.normal.insert(out.normal.begin(), out.normal.back());
    return count >= 2 && out.normal.size() >= 3;
}

std::string ReadWhole(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

// A .saf archive: lines "<size> <path>" up to a line "-", then the files' bytes one after another in
// that order. Each track's timing_gates in it, with its terrain.hf (its own folder's, or the one above:
// a track's other layouts sit in folders inside it).
void ReadArchive(const std::filesystem::path& path, std::vector<Track>& tracks)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return;
    std::string header;
    char buffer[65536];
    size_t end = std::string::npos;
    while (end == std::string::npos && header.size() < (16u << 20)) {
        file.read(buffer, sizeof buffer);
        const std::streamsize got = file.gcount();
        if (got <= 0)
            break;
        header.append(buffer, static_cast<size_t>(got));
        // (The list ends with a line of its own: "-".)
        const size_t at = header.find("\n-\n");
        if (at != std::string::npos)
            end = at + 3;
        else if (header.rfind("-\n", 0) == 0)
            end = 2;
    }
    if (end == std::string::npos)
        return;
    unsigned long long offset = end;
    size_t pos = 0;
    std::vector<std::pair<std::string, double>> spacings;  // folder -> its heightmap's spacing
    const size_t firstNew = tracks.size();
    const auto ReadEntry = [&](unsigned long long at, unsigned long long size, std::string& text) {
        std::ifstream data(path, std::ios::binary);
        data.seekg(static_cast<std::streamoff>(at));
        text.assign(static_cast<size_t>(size), '\0');
        data.read(&text[0], static_cast<std::streamsize>(size));
        return data.gcount() == static_cast<std::streamsize>(size);
    };
    while (pos < end - 2) {
        const size_t nl = header.find('\n', pos);
        if (nl == std::string::npos || nl >= end)
            break;
        const std::string line = header.substr(pos, nl - pos);
        pos = nl + 1;
        char* after = nullptr;
        const unsigned long long size = std::strtoull(line.c_str(), &after, 10);
        if (!after || *after != ' ')
            break;
        const std::string name = after + 1;
        const auto Ends = [&name](const std::string& suffix) {
            return name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
        };
        std::string text;
        if (Ends("/timing_gates") && size < (4u << 20) && ReadEntry(offset, size, text)) {
            Track t;
            if (ParseGates(name.substr(0, name.size() - 13), text, t))
                tracks.push_back(std::move(t));
        } else if (Ends("/terrain.hf") && size < 4096 && ReadEntry(offset, size, text)) {
            spacings.push_back({ name.substr(0, name.size() - 11), HeightmapSpacing(text) });
        }
        offset += size;
    }
    for (size_t i = firstNew; i < tracks.size(); ++i) {
        std::string folder = tracks[i].name;
        while (!folder.empty() && tracks[i].spacing == 0) {
            for (const auto& s : spacings)
                if (s.first == folder)
                    tracks[i].spacing = s.second;
            const size_t slash = folder.rfind('/');
            folder = slash == std::string::npos ? std::string() : folder.substr(0, slash);
        }
    }
}

void ReadFolder(const wchar_t* folder, std::vector<Track>& tracks)
{
    if (!folder || !folder[0])
        return;
    std::error_code error;
    const std::filesystem::path root(folder);
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        std::error_code e2;
        if (entry.is_directory(e2)) {
            const std::filesystem::path gates = entry.path() / "timing_gates";
            if (std::filesystem::is_regular_file(gates, e2)) {
                Track t;
                if (ParseGates(entry.path().filename().string(), ReadWhole(gates), t)) {
                    t.spacing = HeightmapSpacing(ReadWhole(entry.path() / "terrain.hf"));
                    tracks.push_back(std::move(t));
                }
            }
        } else if (entry.is_regular_file(e2)) {
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == ".saf")
                ReadArchive(entry.path(), tracks);
        }
    }
}

// ---- State ---------------------------------------------------------------------------------------

std::mutex              g_loadedMutex;
std::vector<Track>      g_loaded;           // handed over by the loading thread
std::atomic<bool>       g_loadDone{ false };
bool                    g_loadStarted = false;
bool                    g_taken = false;
std::vector<Track>      g_tracks;           // the render thread's own
bool                    g_testTracks = false;
double                  g_terrainSpacing = 0;  // the loaded track's heightmap spacing (SetTerrain; 0 unknown)

// Whether a track could be the one loaded (its heightmap's spacing the same, where both are known).
bool Candidate(const Track& t)
{
    return g_terrainSpacing <= 0 || t.spacing <= 0 || std::fabs(t.spacing - g_terrainSpacing) < 1e-3;
}

struct BestLap {
    bool                have = false;
    double              time = 0;
    std::vector<double> splits;  // at each place in its lap's order (0 at its start)
};

struct Timing {
    int                 track = -1;   // into g_tracks: the track known
    // The lap running.
    bool                running = false;
    bool                firstLap = false;
    int                 pos = 0;      // its place in the lap's order: the last checkpoint crossed
    double              start = 0;
    std::vector<double> splits;
    bool                valid = true;
    int                 lap = 0;
    BestLap             best[2];      // [0] first laps, [1] the others
    // A checkpoint further on crossed (2 to 4 ahead): taken (the ones before it missed) once the one
    // after it is crossed too - alone it's a stray line (a stadium's lanes run side by side).
    int                 pending = -1;
    // Back after a while gone (a track loading) somewhere else: which track it is is told again - the
    // one known only if it's that one again (its gates, or its start gate), with your bests kept; else
    // the other one, fresh. (Two tracks whose heightmaps' numbers match load with no sign the game
    // gives, and the last track's laps and gates went on being timed on the new one.)
    bool                recheck = false;
    double              nearest = 1e9;  // closest you came to the next gate's middle (game units), for the log
    double              pendingAt = 0;
    // The start gate: standing in a slot (since when), to time the first lap from leaving it.
    int                 slot = -1;
    double              slotX = 0, slotZ = 0;
    // A lap just finished: shown for a moment.
    double              shownUntil = -1;
    double              shownTime = 0, shownGap = 0;
    bool                shownHaveGap = false, shownValid = true;
    int                 shownLap = 0, shownSplit = 0;
    // The last position.
    bool                haveLast = false;
    double              lastX = 0, lastZ = 0, lastT = 0;
    // The game paused (its pause key): since when, and your bike moving meanwhile (frames on end, since when).
    bool                paused = false;
    double              pausedAt = 0;
    int                 movingFrames = 0;
    double              stillAt = 0;   // the last frame it stood still
};
Timing g_t;

void StartLoading()
{
    if (g_loadStarted || g_testTracks)
        return;
    g_loadStarted = true;
    std::thread([] {
        std::vector<Track> tracks;
        ReadFolder(paths::GameFolder(), tracks);
        ReadFolder(paths::PersonalFolder(), tracks);
        Log("lap timer: %zu tracks with timing gates found", tracks.size());
        {
            std::lock_guard<std::mutex> lock(g_loadedMutex);
            g_loaded = std::move(tracks);
        }
        g_loadDone = true;
    }).detach();
}

constexpr double kGateMargin = 10.0;  // game units past each end of a timing gate's line

// Where the segment a->b crosses gate g the way through it (its fraction along a->b, 0..1), or -1.
// (The file's vector is the way through, not the line: the first checkpoints of a track point the
// way the riders leave the start gate. Taken as the line, no lap was ever seen.)
double Crossing(double ax, double az, double bx, double bz, const Gate& g, double margin = kGateMargin)
{
    const double rx = bx - ax, rz = bz - az;
    // (Only clearly backwards left out: through a turn a rider can cross a line well off its way.)
    const double lr = std::sqrt(rx * rx + rz * rz), lg = std::sqrt(g.dx * g.dx + g.dz * g.dz);
    if (lr <= 0 || lg <= 0 || rx * g.dx + rz * g.dz < -0.5 * lr * lg)
        return -1;
    // Which side of the line each end is on (along the way through; a point right on it counts as past
    // it): crossed when that changes - once, however a position lands on the line. (Taken as the step
    // crossing the line between its two ends, a position landing right on it counted it twice, or -
    // the step's ends just either side by a rounding - never.)
    const double da = ((ax - g.cx) * g.dx + (az - g.cz) * g.dz) / lg;
    const double db = ((bx - g.cx) * g.dx + (bz - g.cz) * g.dz) / lg;
    if ((da < 0) == (db < 0))
        return -1;
    const double s = da / (da - db);  // along a->b
    // Where on the line: within its middle +- its width, and 10 units (3 m) more each side. (The file's
    // width is how far the line reaches each side of its middle, not end to end: a start straight's
    // first gate reaches out to the start gate's end slots - Raceway Park 62 for slots -45..58, OMC 97
    // for -76..76. Taken as end to end, gates you rode through were missed - 25.7 and 37.2 from the
    // middle of gates 31.1 and 44.7 wide - and a gate further on was seldom crossed within it: the next
    // missed was seldom shown.)
    const double cx = ax + rx * s - g.cx, cz = az + rz * s - g.cz;
    const double across = std::fabs((cx * -g.dz + cz * g.dx) / lg);
    if (across > lg + margin)
        return -1;
    return std::max(s, 1e-9);
}

// Whether gate g comes right after gate last in one of the track's laps.
bool Follows(const Track& t, int last, int g)
{
    for (const std::vector<int>* seq : { &t.first, &t.normal })
        for (size_t i = 0; i + 1 < seq->size(); ++i)
            if ((*seq)[i] == last && (*seq)[i + 1] == g)
                return true;
    return false;
}

void ResetLap()
{
    g_t.running = false;
    g_t.splits.clear();
    g_t.pos = 0;
    g_t.valid = true;
    g_t.pending = -1;
}

void ResetTiming(int track)
{
    const bool same = track == g_t.track;
    Timing fresh;
    if (same) {
        fresh.best[0] = g_t.best[0];
        fresh.best[1] = g_t.best[1];
    }
    fresh.track = track;
    fresh.haveLast = g_t.haveLast;
    fresh.lastX = g_t.lastX;
    fresh.lastZ = g_t.lastZ;
    fresh.lastT = g_t.lastT;
    g_t = fresh;
}

void StartLap(bool first, double at)
{
    const Track& t = g_tracks[static_cast<size_t>(g_t.track)];
    g_t.running = true;
    g_t.firstLap = first;
    g_t.start = at;
    g_t.valid = true;
    g_t.splits.assign((first ? t.first : t.normal).size(), -1.0);
    // (The first lap from the gate: none of its checkpoints crossed yet. The others: from the finish
    // line, their first checkpoint.)
    g_t.pos = first ? -1 : 0;
    if (!first)
        g_t.splits[0] = 0.0;
    g_t.pending = -1;
    g_t.lap += 1;
}

// The lap ends at gate (its finish line): shown for a moment - if valid, your best when it's quicker; if
// not (a gate missed), its time only, marked invalid. The next lap from here if a lap starts where this
// one ended (round a loop); else (a straight run, one track's laps from checkpoint 3 to 20) from
// crossing its start again.
void EndLap(int gate, double at, bool valid)
{
    const Track& t = g_tracks[static_cast<size_t>(g_t.track)];
    const double time = at - g_t.start;
    BestLap& best = g_t.best[g_t.firstLap ? 0 : 1];
    g_t.shownUntil = at + 4.0;
    g_t.shownTime = time;
    g_t.shownHaveGap = valid && best.have;
    g_t.shownGap = g_t.shownHaveGap ? time - best.time : 0.0;
    g_t.shownValid = valid;
    g_t.shownLap = g_t.lap;
    g_t.shownSplit = g_t.firstLap ? g_t.pos + 1 : g_t.pos;  // (checkpoints passed: as Current counts them)
    if (valid && (!best.have || time < best.time)) {
        best.have = true;
        best.time = time;
        best.splits = g_t.splits;
    }
    const long long cs = static_cast<long long>(std::floor(time * 100.0 + 1e-6));  // (hundredths, cut: as the panel shows it)
    Log("lap timer: lap %d on %s: %lld:%02lld.%02lld%s", g_t.lap, t.name.c_str(), cs / 6000, (cs / 100) % 60, cs % 100,
        valid ? "" : " (invalid: a timing gate missed)");
    if (gate == t.normal.front())
        StartLap(false, at);
    else
        g_t.running = false;
}

void Passed(int gate, double at, bool exact)
{
    const Track& t = g_tracks[static_cast<size_t>(g_t.track)];
    if (!g_t.running) {
        if (gate == t.normal.front()) {
            g_t.lap = std::max(g_t.lap, 0);
            StartLap(false, at);
        }
        return;
    }
    const std::vector<int>& seq = g_t.firstLap ? t.first : t.normal;
    const int count = static_cast<int>(seq.size());
    // Only the next gate in order counts, as the game counts a lap: a gate missed isn't skipped. The
    // clock runs on; the lap goes on once the missed one is crossed (gone back for, or next time round
    // - crossing the finish line without it is no lap, and the clock keeps counting into the next).
    if (g_t.pos + 1 >= count || seq[static_cast<size_t>(g_t.pos + 1)] != gate) {
        // A gate just after it (2 to 4 on): the next one was missed (shown till it's crossed). (Not one
        // further: a first lap from the gate can pass the finish line before its first gate.)
        // (Only through the gate itself, not the margin past its ends: riding up to the next gate, a
        // later one's line was clipped, and the next showed missed for a moment.)
        for (int i = g_t.pos + 2; exact && i < count && i <= g_t.pos + 4; ++i) {
            if (seq[static_cast<size_t>(i)] != gate)
                continue;
            const int missed = g_t.firstLap ? g_t.pos + 2 : g_t.pos + 1;  // (as the panel counts gates)
            if (g_t.pending < 0) {
                const Gate& mg = t.gates[static_cast<size_t>(seq[static_cast<size_t>(g_t.pos + 1)])];
                Log("lap timer: lap %d missed timing gate G%d (checkpoint %d, reaching %.1f each side of its middle): shown amber till it's crossed; "
                    "the closest you came to its middle %.1f", g_t.lap, missed, seq[static_cast<size_t>(g_t.pos + 1)],
                    std::sqrt(mg.dx * mg.dx + mg.dz * mg.dz), g_t.nearest);
            }
            g_t.pending = g_t.pos + 1;
            break;
        }
        // The finish line crossed with a gate missed (the Gate shown amber): the lap ends there, timed but
        // invalid (not your best, no gap), and the next starts - no going back for the missed gate.
        // (Not before any gate of the lap: a first lap from the gate can cross the finish line first.)
        if (gate == seq.back() && g_t.pending >= 0) {
            const int missed = g_t.firstLap ? g_t.pending + 1 : g_t.pending;
            Log("lap timer: the finish line crossed with G%d missed: lap %d ends, an invalid lap time", missed, g_t.lap);
            EndLap(gate, at, false);
        }
        return;
    }
    const int found = g_t.pos + 1;
    g_t.pending = -1;  // (a missed gate crossed after all)
    g_t.nearest = 1e9;
    g_t.pos = found;
    g_t.splits[static_cast<size_t>(found)] = at - g_t.start;
    if (found + 1 < static_cast<int>(seq.size()))
        return;
    EndLap(gate, at, true);  // (every gate crossed, in order)
}

// Standing in one of a track's start gate slots (within 3 units of one: they're 4 apart).
int SlotOf(const Track& t, double x, double z)
{
    for (size_t i = 0; i + 1 < t.slots.size(); i += 2) {
        const double dx = x - t.slots[i], dz = z - t.slots[i + 1];
        if (dx * dx + dz * dz < 3.0 * 3.0)
            return static_cast<int>(i / 2);
    }
    return -1;
}

}  // namespace

void UseTracksForTest(const char* const* names, const char* const* texts, int count, const double* spacings)
{
    g_testTracks = true;
    g_tracks.clear();
    for (int i = 0; i < count; ++i) {
        Track t;
        if (ParseGates(names[i], texts[i], t)) {
            t.spacing = spacings ? spacings[i] : 0.0;
            g_tracks.push_back(std::move(t));
        }
    }
    g_taken = true;
    g_terrainSpacing = 0;
    g_t = Timing{};
}

void SetTerrain(double spacing)
{
    g_terrainSpacing = spacing > 0 ? spacing : 0.0;
}

void NewTrack()
{
    for (Track& t : g_tracks) {
        t.last = -1;
        t.streak = 0;
        t.inOrderAt = -1e9;
    }
    g_t = Timing{};
}

namespace {
// Carrying on after a pause from at: the lap's clock (and a finished lap's moment shown) moved on by the
// time paused, so it carries on where it stopped.
void Resume(double at)
{
    const double paused = std::max(0.0, at - g_t.pausedAt);
    g_t.start += paused;
    if (g_t.shownUntil > g_t.pausedAt)
        g_t.shownUntil += paused;
    g_t.paused = false;
    Log("lap timer: the game carries on after %.1f s paused: the clock carries on", paused);
}
}  // namespace

void PauseKey(double now)
{
    if (g_t.paused) {
        Resume(now);
        return;
    }
    g_t.paused = true;
    g_t.pausedAt = now;
    g_t.movingFrames = 0;
    g_t.stillAt = now;
    Log("lap timer: the game's pause key: the clock stopped");
}

// The game reset the session on the same track (rejoined, a new race): the laps and your bests start
// over; the track stays known.
void SessionReset()
{
    const int track = g_t.track;
    const bool recheck = g_t.recheck;
    g_t = Timing{};
    g_t.track = track;
    g_t.recheck = recheck;
}

void NoteGameLog(const std::string& text)
{
    // (Its lines: "[h:mm:ss.cc] Resetting" - rejoining the track, or a new race on it: the game
    // deallocates and loads the bikes again, but not the track, and nothing it draws says so;
    // "Loading terrain" - another track.)
    if (text.find("] Loading terrain") != std::string::npos) {
        Log("lap timer: the game's log says a track is loading: the laps start over");
        NewTrack();
    } else if (text.find("] Resetting") != std::string::npos) {
        Log("lap timer: the game's log says it reset (a rejoin or a new race): the laps and bests start over");
        SessionReset();
    }
}

namespace {
// The game's own log (lastlog.txt in its personal folder), read as it grows: twice a second its size,
// and what was added since.
struct GameLogWatch {
    bool               started = false;
    unsigned long long size = 0;
    double             checkedAt = -1e9;
};
GameLogWatch g_gameLog;

void PollGameLog(double now)
{
    if (g_testTracks || now - g_gameLog.checkedAt < 0.5)
        return;
    g_gameLog.checkedAt = now;
    const wchar_t* folder = paths::PersonalFolder();
    if (!folder || !folder[0])
        return;
    const std::filesystem::path path = std::filesystem::path(folder) / L"lastlog.txt";
    std::error_code error;
    const unsigned long long size = std::filesystem::file_size(path, error);
    if (error)
        return;
    if (!g_gameLog.started) {  // (what it says already was before now)
        g_gameLog.started = true;
        g_gameLog.size = size;
        return;
    }
    if (size < g_gameLog.size)
        g_gameLog.size = 0;  // (a new log: the game started again)
    if (size == g_gameLog.size)
        return;
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return;
    const unsigned long long from = std::max(g_gameLog.size, size > (1u << 20) ? size - (1u << 20) : 0ull);
    file.seekg(static_cast<std::streamoff>(from));
    std::string text(static_cast<size_t>(size - from), '\0');
    file.read(&text[0], static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<size_t>(std::max<std::streamsize>(file.gcount(), 0)));
    // (Up to its last whole line: the rest is read next time.)
    const size_t end = text.rfind('\n');
    if (end == std::string::npos)
        return;
    g_gameLog.size = from + end + 1;
    NoteGameLog(text.substr(0, end + 1));
}
}  // namespace

void Update(double now, bool have, const double position[3])
{
    StartLoading();
    PollGameLog(now);
    if (!g_taken && g_loadDone.load()) {
        std::lock_guard<std::mutex> lock(g_loadedMutex);
        g_tracks = std::move(g_loaded);
        g_taken = true;
    }
    // (Not known for a moment - your bike lost: the last place is kept, and the step from it checked when
    // it's known again; started afresh, a gate crossed meanwhile was missed.)
    if (!have || g_tracks.empty())
        return;
    const double x = position[0], z = position[2];
    if (!g_t.haveLast) {
        g_t.haveLast = true;
        g_t.lastX = x;
        g_t.lastZ = z;
        g_t.lastT = now;
        return;
    }
    const double ax = g_t.lastX, az = g_t.lastZ, at = g_t.lastT;
    const double step = std::sqrt((x - ax) * (x - ax) + (z - az) * (z - az));
    g_t.lastX = x;
    g_t.lastZ = z;
    g_t.lastT = now;
    const bool still = step < 0.02;
    // Paused, your bike moving on (4 frames on end: a frame stepped while paused is one): at once after the
    // pause key - pausing did nothing (an online race): it never was; later - the game carried on some
    // other way (its menu): from the last moment it stood still.
    if (g_t.paused) {
        if (step > 0.05) {
            if (++g_t.movingFrames >= 4) {
                if (g_t.stillAt - g_t.pausedAt < 0.25) {
                    g_t.paused = false;
                    Log("lap timer: your bike went on moving after the pause key: the game wasn't paused (the clock ran on)");
                } else {
                    Resume(g_t.stillAt);
                }
            }
        } else {
            g_t.movingFrames = 0;
            g_t.stillAt = now;
        }
    }
    // A jump (a reset, a respawn, a new session): no line crossed on the way. (The lap goes on: its
    // gates still have to be crossed in order.) After a while not known, as far as riding goes: 100
    // units (30 m) a second.
    // (Gone two and a half seconds or more - a track loading, the game paused: never riding, where you
    // are is known all the while, crashed too - the lap running is over, and which track it is is told
    // again.)
    if (now - at >= 2.5 && g_t.track >= 0 && !g_t.recheck) {
        Log("lap timer: back %.0f s later: the lap running stopped, which track it is told again", now - at);
        ResetLap();
        g_t.lap = 0;
        g_t.slot = -1;
        g_t.recheck = true;
        for (Track& t : g_tracks) {
            t.last = -1;
            t.streak = 0;
        }
    }
    if (step > 40.0 + 100.0 * (now - at))
        return;

    // Which track: standing in a start gate slot tells it at once; else the checkpoints crossed in order.
    // (The start gate: the known track's own; with none known, only if a single track that could be
    // the one loaded has a slot there - another track's slot can be anywhere on this one.)
    if (still) {
        int slotTrack = -1, slotCount = 0;
        for (size_t i = 0; i < g_tracks.size(); ++i) {
            if ((g_t.track >= 0 && !g_t.recheck && static_cast<int>(i) != g_t.track) || !Candidate(g_tracks[i]) || SlotOf(g_tracks[i], x, z) < 0)
                continue;
            slotTrack = static_cast<int>(i);
            ++slotCount;
        }
        if (slotCount == 1) {
            const int slot = SlotOf(g_tracks[static_cast<size_t>(slotTrack)], x, z);
            if (g_t.track != slotTrack) {
                Log("lap timer: in a start gate slot of %s", g_tracks[static_cast<size_t>(slotTrack)].name.c_str());
                ResetTiming(slotTrack);
            }
            g_t.recheck = false;
            // A new start: whatever lap was running is over (a race restarted).
            if (g_t.slot < 0) {
                ResetLap();
                g_t.lap = 0;
            }
            g_t.slot = slot;
            g_t.slotX = x;
            g_t.slotZ = z;
        }
    }
    struct Hit { int track, gate; double s; bool exact; };
    Hit hits[16];
    int hitCount = 0;
    for (size_t i = 0; i < g_tracks.size(); ++i) {
        Track& t = g_tracks[i];
        if (!Candidate(t))
            continue;
        for (size_t g = 0; g < t.gates.size(); ++g) {
            const double s = Crossing(ax, az, x, z, t.gates[g]);
            if (s < 0)
                continue;
            if (t.last >= 0 && Follows(t, t.last, static_cast<int>(g))) {
                ++t.streak;
                t.inOrderAt = now;
            } else {
                t.streak = 1;
            }
            t.last = static_cast<int>(g);
            if (hitCount < 16)
                hits[hitCount++] = Hit{ static_cast<int>(i), static_cast<int>(g), s, Crossing(ax, az, x, z, t.gates[g], 0.0) >= 0 };
        }
    }
    if (hitCount > 0) {
        // The track: with none known, the one with the longest run of checkpoints in order (3 or more).
        // Once one is, it's kept (till the game loads another) - unless its own have gone 20 s without
        // one following another while another's have followed 8 in a row: three of another track's
        // in order by chance kept taking the timer away, your laps and your best with it.
        const Track* known = g_t.track >= 0 && !g_t.recheck ? &g_tracks[static_cast<size_t>(g_t.track)] : nullptr;
        const int needed = !known ? 4 : (now - known->inOrderAt > 20.0 ? 8 : 1 << 30);
        int bestTrack = known ? g_t.track : -1;
        int bestStreak = known ? known->streak : 0;
        for (size_t i = 0; i < g_tracks.size(); ++i)
            if (Candidate(g_tracks[i]) && g_tracks[i].streak >= needed && g_tracks[i].streak > bestStreak) {
                bestStreak = g_tracks[i].streak;
                bestTrack = static_cast<int>(i);
            }
        if (bestTrack >= 0 && (bestTrack != g_t.track || g_t.recheck) && bestStreak >= needed) {
            if (bestTrack != g_t.track)
                Log("lap timer: riding %s (%d of its checkpoints in order)", g_tracks[static_cast<size_t>(bestTrack)].name.c_str(), bestStreak);
            else
                Log("lap timer: still %s (%d of its checkpoints in order)", g_tracks[static_cast<size_t>(bestTrack)].name.c_str(), bestStreak);
            ResetTiming(bestTrack);  // (your bests kept if it's the same one)
        }
    }
    if (g_t.track < 0) {
        // (For the log, every half minute riding without a track: where you are, and the nearest
        // checkpoint of any track.)
        static double lastNote = -1;
        static int notes = 0;
        if (notes < 8 && step > 0.5 && (lastNote < 0 || now - lastNote > 30.0)) {
            if (lastNote >= 0) {
                double best = 1e18;
                const Track* bt = nullptr;
                size_t bg = 0;
                for (const Track& t : g_tracks)
                    for (size_t g = 0; g < t.gates.size(); ++g) {
                        const double d = (t.gates[g].cx - x) * (t.gates[g].cx - x) + (t.gates[g].cz - z) * (t.gates[g].cz - z);
                        if (d < best) {
                            best = d;
                            bt = &t;
                            bg = g;
                        }
                    }
                ++notes;
                if (bt)
                    Log("lap timer: no track told yet; you're at (%.1f, %.1f), the nearest checkpoint %s's %zu, %.1f away", x, z,
                        bt->name.c_str(), bg, std::sqrt(best));
            }
            lastNote = now;
        }
        return;
    }
    // Leaving the start gate: the first lap starts (when you set off: a unit out of where you stood).
    if (g_t.slot >= 0) {
        const double dx = x - g_t.slotX, dz = z - g_t.slotZ;
        if (dx * dx + dz * dz > 1.0 * 1.0) {
            g_t.slot = -1;
            ResetLap();
            g_t.lap = 0;
            const Track& t = g_tracks[static_cast<size_t>(g_t.track)];
            if (!t.first.empty())
                StartLap(true, at);
        }
    }
    for (int h = 0; h < hitCount; ++h)
        if (hits[h].track == g_t.track && !g_t.recheck)  // (not till the track's told again)
            Passed(hits[h].gate, at + hits[h].s * (now - at), hits[h].exact);
    // (For the log: how close you come to the next gate's middle.)
    if (g_t.running) {
        const Track& t = g_tracks[static_cast<size_t>(g_t.track)];
        const std::vector<int>& seq = g_t.firstLap ? t.first : t.normal;
        if (g_t.pos + 1 < static_cast<int>(seq.size())) {
            const Gate& g = t.gates[static_cast<size_t>(seq[static_cast<size_t>(g_t.pos + 1)])];
            g_t.nearest = std::min(g_t.nearest, std::sqrt((x - g.cx) * (x - g.cx) + (z - g.cz) * (z - g.cz)));
        }
    }
}

Display Current(double now)
{
    Display d;
    if (g_t.track < 0)
        return d;
    // (Paused: everything as it stood when the game paused.)
    d.paused = g_t.paused;
    if (g_t.paused)
        now = g_t.pausedAt;
    const Track& t = g_tracks[static_cast<size_t>(g_t.track)];
    d.track = true;
    d.trackName = t.name;
    {
        const bool first = g_t.shownUntil > now ? false : g_t.running && g_t.firstLap;
        const BestLap& b = g_t.best[first ? 0 : 1];
        d.best = b.have ? b.time : -1.0;
        d.gates = static_cast<int>((first ? t.first : t.normal).size()) - (first ? 0 : 1);
    }
    if (g_t.shownUntil > now) {
        d.lap = g_t.shownLap;
        d.current = g_t.shownTime;
        d.predicted = g_t.shownTime;
        d.split = g_t.shownSplit;
        d.haveGap = g_t.shownHaveGap;
        d.gap = g_t.shownGap;
        d.finished = true;
        d.valid = g_t.shownValid;
        return d;
    }
    if (!g_t.running)
        return d;
    d.lap = g_t.lap;
    d.current = now - g_t.start;
    // (Checkpoints passed: a lap's first place is its start line - the first lap's is the gate, none.)
    d.split = g_t.firstLap ? g_t.pos + 1 : g_t.pos;
    d.valid = g_t.pending < 0;  // (a gate missed: shown till it's crossed)
    const BestLap& best = g_t.best[g_t.firstLap ? 0 : 1];
    if (best.have && g_t.pos >= 1 && static_cast<size_t>(g_t.pos) < best.splits.size() && best.splits[static_cast<size_t>(g_t.pos)] >= 0 &&
        g_t.splits[static_cast<size_t>(g_t.pos)] >= 0) {
        d.haveGap = true;
        d.gap = g_t.splits[static_cast<size_t>(g_t.pos)] - best.splits[static_cast<size_t>(g_t.pos)];
        d.predicted = best.time + d.gap;
    }
    return d;
}

}  // namespace laptimer
}  // namespace mxcam
