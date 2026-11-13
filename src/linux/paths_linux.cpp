// paths_linux.cpp (Linux build only): paths.h the Linux way.
//   The game folder: MX_HOME (the game's own launcher, mx, sets it, and so does
//   mxs-remastered.sh), else the folder mxbin runs from. The mod's files go there when it can be
//   written to, else to $XDG_DATA_HOME/mxs-remastered/ (~/.local/share/mxs-remastered/); once
//   settings are there they stay there. The game's personal folder (its own settings file,
//   "config", which the dab indicator and the brake squeal read the game's controls from):
//   ~/.mxsimulator/ when there is one, else the game folder.
#include "../paths.h"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "win32_compat.h"

namespace mxcam {
namespace paths {
namespace {

constexpr wchar_t kIniName[] = L"MXS_REMASTERED.ini";

wchar_t g_game[MAX_PATH] = L"";
wchar_t g_data[MAX_PATH] = L"";
wchar_t g_ini[MAX_PATH] = L"";
wchar_t g_personal[MAX_PATH] = L"";
char    g_note[2 * MAX_PATH + 160] = "";

bool IsDirectory(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool IsFile(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// A folder as the shared code keeps them: wide, with a trailing separator.
void SetFolder(wchar_t (&out)[MAX_PATH], std::string folder)
{
    if (!folder.empty() && folder.back() != '/')
        folder += '/';
    wcsncpy_s(out, mxcam_wide(folder.c_str()).c_str(), _TRUNCATE);
}

std::string Narrow(const wchar_t* folder) { return mxcam_path(folder); }

bool MakeFolders(const std::string& path)
{
    std::string partial;
    for (size_t i = 0; i < path.size(); ++i) {
        partial += path[i];
        if ((path[i] == '/' && i > 0) || i + 1 == path.size())
            if (!IsDirectory(partial) && mkdir(partial.c_str(), 0755) != 0 && !IsDirectory(partial))
                return false;
    }
    return true;
}

}  // namespace

bool CanWriteFolder(const wchar_t* folder)
{
    if (!folder || !folder[0])
        return false;
    return access(Narrow(folder).c_str(), W_OK | X_OK) == 0;
}

void Init()
{
    // The game folder.
    std::string game;
    if (const char* home = getenv("MX_HOME"); home && home[0] && IsDirectory(home)) {
        game = home;
    } else {
        char exe[PATH_MAX] = "";
        const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0) {
            exe[n] = '\0';
            game = exe;
            const size_t slash = game.rfind('/');
            game = slash == std::string::npos ? std::string() : game.substr(0, slash);
        }
    }
    if (!game.empty())
        SetFolder(g_game, game);

    // The game's personal folder.
    const char* home = getenv("HOME");
    if (home && IsDirectory(std::string(home) + "/.mxsimulator"))
        SetFolder(g_personal, std::string(home) + "/.mxsimulator");
    else if (g_game[0])
        wcscpy_s(g_personal, g_game);

    // The mod's own files.
    std::string user;
    if (const char* xdg = getenv("XDG_DATA_HOME"); xdg && xdg[0] == '/')
        user = std::string(xdg) + "/mxs-remastered";
    else if (home && home[0])
        user = std::string(home) + "/.local/share/mxs-remastered";
    const std::string userIni = user.empty() ? std::string() : user + "/MXS_REMASTERED.ini";
    const std::string gameIni = g_game[0] ? Narrow(g_game) + "MXS_REMASTERED.ini" : std::string();
    const bool gameWritable = g_game[0] && CanWriteFolder(g_game) && (!IsFile(gameIni) || access(gameIni.c_str(), W_OK) == 0);

    if (!userIni.empty() && IsFile(userIni)) {
        SetFolder(g_data, user);
        snprintf(g_note, sizeof g_note, "files: settings and logs in %s (moved there earlier)", user.c_str());
    } else if (gameWritable || user.empty()) {
        wcscpy_s(g_data, g_game);
        snprintf(g_note, sizeof g_note, "files: settings and logs in the game folder %s", Narrow(g_game).c_str());
    } else if (MakeFolders(user)) {
        SetFolder(g_data, user);
        bool copied = false;
        if (IsFile(gameIni)) {
            const std::wstring from = mxcam_wide(gameIni.c_str()), to = mxcam_wide(userIni.c_str());
            copied = CopyFileW(from.c_str(), to.c_str(), TRUE) != FALSE;
        }
        snprintf(g_note, sizeof g_note, "files: the game folder %s can't be written to; settings and logs in %s%s",
                 Narrow(g_game).c_str(), user.c_str(), copied ? " (copied the game folder's settings there)" : "");
    } else {
        wcscpy_s(g_data, g_game);
        snprintf(g_note, sizeof g_note, "files: neither the game folder %s nor %s can be written to", Narrow(g_game).c_str(),
                 user.c_str());
    }
    swprintf_s(g_ini, L"%s%s", g_data, kIniName);
    const size_t used = strlen(g_note);
    snprintf(g_note + used, sizeof g_note - used, "; the game's own settings read from %sconfig",
             g_personal[0] ? Narrow(g_personal).c_str() : "(nowhere)");
}

const wchar_t* GameFolder() { return g_game; }
const wchar_t* DataFolder() { return g_data; }
const wchar_t* IniPath() { return g_ini; }
const wchar_t* PersonalFolder() { return g_personal; }
const char* Note() { return g_note; }

}  // namespace paths
}  // namespace mxcam
