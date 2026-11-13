// entry_linux.cpp (Linux build only)
//   What DllMain and InitializeProxy (proxy.cpp) do on Windows: when the library is loaded into
//   the game (LD_PRELOAD, by mxs-remastered.sh), the folders are worked out, the log opened, the
//   settings read, and each hooked function's real one found in the libraries after this one.
//
//   LD_PRELOAD is passed on to whatever the game starts: this library takes itself out of it at
//   once, and does nothing at all in any process but the game (mxbin).
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <unistd.h>

#include "../config.h"
#include "../hooks.h"
#include "../log.h"
#include "../paths.h"
#include "../sound_packs.h"
#include "win32_compat.h"

namespace mxcam {

void ResolveRealFunctions();  // hooks.cpp

HMODULE RealOpenGLModule()
{
    static HMODULE gl = dlopen("libGL.so.1", RTLD_NOW | RTLD_NOLOAD);
    return gl;
}

namespace {

// LD_PRELOAD without this library (it names it by the path it was loaded from).
void LeavePreload()
{
    const char* preload = getenv("LD_PRELOAD");
    if (!preload)
        return;
    Dl_info self{};
    dladdr(reinterpret_cast<void*>(&LeavePreload), &self);
    const std::string me = self.dli_fname ? self.dli_fname : "";
    std::string kept, item;
    const std::string all = preload;
    for (size_t i = 0; i <= all.size(); ++i) {
        if (i == all.size() || all[i] == ':' || all[i] == ' ') {
            const bool ours = !item.empty() && (item == me || item.find("libmxs-remastered") != std::string::npos);
            if (!item.empty() && !ours)
                kept += (kept.empty() ? "" : ":") + item;
            item.clear();
        } else {
            item += all[i];
        }
    }
    if (kept.empty())
        unsetenv("LD_PRELOAD");
    else
        setenv("LD_PRELOAD", kept.c_str(), 1);
}

bool IsTheGame()
{
    char exe[PATH_MAX] = "";
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0)
        return false;
    exe[n] = '\0';
    const char* name = strrchr(exe, '/');
    name = name ? name + 1 : exe;
    return strcmp(name, "mxbin") == 0 || getenv("MXS_REMASTERED_ANY_PROGRAM") != nullptr;  // (the latter: tests)
}

__attribute__((constructor)) void Load()
{
    LeavePreload();
    if (!IsTheGame())
        return;
    paths::Init();
    if (paths::DataFolder()[0]) {
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%sMXS_REMASTERED.log", paths::DataFolder());
        LogOpen(path);
        Log("MXS Remastered loaded (Linux x86-64 build)");
        Log("%s", paths::Note());
        LoadConfig(paths::IniPath());
        soundpacks::Startup();
        Config& c = EditConfig();
        c.soundPack125 = soundpacks::Installed(0);
        c.soundPack250 = soundpacks::Installed(1);
        c.soundPack250f = soundpacks::Installed(2);
        c.soundPack450f = soundpacks::Installed(3);
        c.soundPack450fClear = soundpacks::Installed(4);
    }
    ResolveRealFunctions();
    size_t count = 0;
    const HookEntry* hooks = GetHookTable(count);
    int found = 0;
    std::string missing;
    for (size_t i = 0; i < count; ++i) {
        if (*hooks[i].realSlot)
            ++found;
        else
            missing += std::string(missing.empty() ? "" : ", ") + hooks[i].name;
    }
    Log("forwarding to the system's OpenGL: %d of %d hooked functions found%s%s", found, static_cast<int>(count),
        missing.empty() ? "" : "; not there (fine unless the game uses them): ", missing.c_str());
}

__attribute__((destructor)) void Unload()
{
    LogClose();
}

}  // namespace
}  // namespace mxcam
