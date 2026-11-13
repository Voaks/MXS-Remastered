// fonts_linux.cpp (Linux build only): the menu's fonts from the system's.
//   fontconfig (fc-match, on any desktop Linux) picks the file for each face the Windows build
//   uses: "Arial" matches Liberation Sans or another metric-compatible sans-serif, "Segoe UI" the
//   system's sans-serif. Without fc-match, the usual places of DejaVu Sans and Liberation Sans are
//   tried. Each file is read once.
#include "platform_linux.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <sys/stat.h>

#include "../log.h"

namespace mxcam {
namespace platform {
namespace {

bool ReadFile(const std::string& path, std::vector<unsigned char>& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    out.clear();
    unsigned char buffer[65536];
    size_t n;
    while ((n = fread(buffer, 1, sizeof buffer, f)) > 0 && out.size() < (64u << 20))
        out.insert(out.end(), buffer, buffer + n);
    fclose(f);
    return out.size() > 12;
}

std::string Match(const char* face, int weight)
{
    // (fontconfig's weights: regular 80, semibold 180, bold 200.)
    const int fcWeight = weight >= 700 ? 200 : weight >= 600 ? 180 : 80;
    char command[256];
    snprintf(command, sizeof command, "fc-match -f '%%{file}' '%s:weight=%d:fontformat=TrueType' 2>/dev/null", face, fcWeight);
    std::string file;
    if (FILE* p = popen(command, "r")) {
        char buffer[1024];
        size_t n;
        while ((n = fread(buffer, 1, sizeof buffer, p)) > 0)
            file.append(buffer, n);
        pclose(p);
    }
    while (!file.empty() && (file.back() == '\n' || file.back() == ' '))
        file.pop_back();
    return file;
}

}  // namespace

const std::vector<unsigned char>& FontFile(const char* face, int weight)
{
    static std::map<std::string, std::vector<unsigned char>> loaded;
    const std::string key = std::string(face) + ":" + std::to_string(weight);
    const auto found = loaded.find(key);
    if (found != loaded.end())
        return found->second;
    std::vector<unsigned char>& bytes = loaded[key];

    std::string file = Match(face, weight);
    if (file.empty() || !ReadFile(file, bytes)) {
        const bool bold = weight >= 600;
        const char* const fallbacks[] = {
            bold ? "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf" : "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            bold ? "/usr/share/fonts/liberation-sans/LiberationSans-Bold.ttf" : "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
            bold ? "/usr/share/fonts/TTF/LiberationSans-Bold.ttf" : "/usr/share/fonts/TTF/LiberationSans-Regular.ttf",
            bold ? "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf" : "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            bold ? "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf" : "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
            bold ? "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf" : "/usr/share/fonts/TTF/DejaVuSans.ttf",
            bold ? "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf" : "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        };
        file.clear();
        for (const char* path : fallbacks) {
            if (ReadFile(path, bytes)) {
                file = path;
                break;
            }
        }
    }
    if (file.empty())
        bytes.clear();
    Log("overlay: font for %s %d: %s", face, weight, file.empty() ? "NONE FOUND" : file.c_str());
    return bytes;
}

}  // namespace platform
}  // namespace mxcam
