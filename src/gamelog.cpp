// gamelog.cpp - see gamelog.h.
#include "gamelog.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "laptimer.h"
#include "log.h"
#include "paths.h"
#include "postfx.h"

namespace mxcam {
namespace gamelog {
namespace {

struct Watch {
    bool               started = false;
    unsigned long long size = 0;      // read up to here
    double             checkedAt = -1e9;
};
Watch g_watch;

}  // namespace

void Handle(const std::string& text)
{
    laptimer::NoteGameLog(text);
    if (text.find("] Resetting") != std::string::npos || text.find("] Loading terrain") != std::string::npos) {
        Log("game log: the game reset or loaded a track: the tire marks start over");
        postfx::MarksNewTrack();
    }
}

void Poll(double now)
{
    if (now - g_watch.checkedAt < 0.5)
        return;
    g_watch.checkedAt = now;
    const wchar_t* folder = paths::PersonalFolder();
    if (!folder || !folder[0])
        return;
    const std::filesystem::path path = std::filesystem::path(folder) / L"lastlog.txt";
    std::error_code error;
    const unsigned long long size = std::filesystem::file_size(path, error);
    if (error)
        return;
    if (!g_watch.started) {  // (what it says already was before now)
        g_watch.started = true;
        g_watch.size = size;
        return;
    }
    if (size < g_watch.size)
        g_watch.size = 0;  // (a new log: the game started again)
    if (size == g_watch.size)
        return;
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return;
    const unsigned long long from = std::max(g_watch.size, size > (1u << 20) ? size - (1u << 20) : 0ull);
    file.seekg(static_cast<std::streamoff>(from));
    std::string text(static_cast<size_t>(size - from), '\0');
    file.read(&text[0], static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<size_t>(std::max<std::streamsize>(file.gcount(), 0)));
    // (Up to its last whole line: the rest is read next time.)
    const size_t end = text.rfind('\n');
    if (end == std::string::npos)
        return;
    g_watch.size = from + end + 1;
    Handle(text.substr(0, end + 1));
}

}  // namespace gamelog
}  // namespace mxcam
