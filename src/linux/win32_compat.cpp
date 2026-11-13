// win32_compat.cpp (Linux build only): the Windows stand-ins of win32_compat.h.
#include "win32_compat.h"

#include <cerrno>
#include <climits>
#include <ctime>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <vector>

#include "platform_linux.h"

// ---- Errors ------------------------------------------------------------------------------
namespace {
thread_local DWORD g_lastError = 0;

DWORD ErrorFromErrno(int e)
{
    switch (e) {
    case ENOENT:  return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES:
    case EPERM:
    case EROFS:   return ERROR_ACCESS_DENIED;
    case EEXIST:  return ERROR_ALREADY_EXISTS;
    case EBUSY:   return ERROR_SHARING_VIOLATION;
    default:      return static_cast<DWORD>(1000 + e);
    }
}

BOOL Fail()
{
    g_lastError = ErrorFromErrno(errno);
    return FALSE;
}
}  // namespace

DWORD GetLastError() { return g_lastError; }
void SetLastError(DWORD error) { g_lastError = error; }

// ---- Time, processes, environment ---------------------------------------------------------
BOOL QueryPerformanceCounter(LARGE_INTEGER* count)
{
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    count->QuadPart = static_cast<LONGLONG>(t.tv_sec) * 1000000000LL + t.tv_nsec;
    return TRUE;
}

BOOL QueryPerformanceFrequency(LARGE_INTEGER* frequency)
{
    frequency->QuadPart = 1000000000LL;
    return TRUE;
}

ULONGLONG GetTickCount64()
{
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<ULONGLONG>(t.tv_sec) * 1000ULL + static_cast<ULONGLONG>(t.tv_nsec) / 1000000ULL;
}

DWORD GetTickCount() { return static_cast<DWORD>(GetTickCount64()); }

void GetLocalTime(SYSTEMTIME* out)
{
    timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    tm local;
    localtime_r(&t.tv_sec, &local);
    out->wYear = static_cast<WORD>(local.tm_year + 1900);
    out->wMonth = static_cast<WORD>(local.tm_mon + 1);
    out->wDayOfWeek = static_cast<WORD>(local.tm_wday);
    out->wDay = static_cast<WORD>(local.tm_mday);
    out->wHour = static_cast<WORD>(local.tm_hour);
    out->wMinute = static_cast<WORD>(local.tm_min);
    out->wSecond = static_cast<WORD>(local.tm_sec);
    out->wMilliseconds = static_cast<WORD>(t.tv_nsec / 1000000);
}

void Sleep(DWORD milliseconds) { usleep(static_cast<useconds_t>(milliseconds) * 1000); }
DWORD GetCurrentProcessId() { return static_cast<DWORD>(getpid()); }
DWORD GetCurrentThreadId() { return static_cast<DWORD>(reinterpret_cast<uintptr_t>(pthread_self())); }

DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR buffer, DWORD size)
{
    const char* value = getenv(name);
    if (!value) {
        g_lastError = 203;  // ERROR_ENVVAR_NOT_FOUND
        return 0;
    }
    const DWORD length = static_cast<DWORD>(strlen(value));
    if (!buffer || size <= length)
        return length + 1;  // (as Windows: the size needed, the terminator included)
    memcpy(buffer, value, length + 1);
    return length;
}

DWORD GetEnvironmentVariableW(LPCWSTR name, LPWSTR buffer, DWORD size)
{
    const char* value = getenv(mxcam_utf8(name).c_str());
    if (!value) {
        g_lastError = 203;
        return 0;
    }
    const std::wstring wide = mxcam_wide(value);
    if (!buffer || size <= wide.size())
        return static_cast<DWORD>(wide.size() + 1);
    wmemcpy(buffer, wide.c_str(), wide.size() + 1);
    return static_cast<DWORD>(wide.size());
}

BOOL SetEnvironmentVariableA(LPCSTR name, LPCSTR value)
{
    return (value ? setenv(name, value, 1) : unsetenv(name)) == 0;
}

// ---- Strings ------------------------------------------------------------------------------
std::string mxcam_utf8(const wchar_t* text)
{
    std::string out;
    if (!text)
        return out;
    for (; *text; ++text) {
        uint32_t c = static_cast<uint32_t>(*text);
        if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF))
            c = 0xFFFD;
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::wstring mxcam_wide(const char* text)
{
    std::wstring out;
    if (!text)
        return out;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
    while (*p) {
        uint32_t c = *p++;
        int more = 0;
        if (c >= 0xF0) {
            c &= 0x07;
            more = 3;
        } else if (c >= 0xE0) {
            c &= 0x0F;
            more = 2;
        } else if (c >= 0xC0) {
            c &= 0x1F;
            more = 1;
        } else if (c >= 0x80) {
            c = 0xFFFD;  // (a stray continuation byte)
        }
        for (; more > 0 && (*p & 0xC0) == 0x80; --more)
            c = (c << 6) | (*p++ & 0x3F);
        if (more > 0)
            c = 0xFFFD;  // (cut short)
        out += static_cast<wchar_t>(c);
    }
    return out;
}

std::string mxcam_path(const wchar_t* path)
{
    std::string out = mxcam_utf8(path);
    for (char& c : out)
        if (c == '\\')
            c = '/';
    return out;
}

int MultiByteToWideChar(UINT, DWORD, LPCSTR text, int length, LPWSTR out, int outSize)
{
    const std::string in = length < 0 ? std::string(text) : std::string(text, static_cast<size_t>(length));
    std::wstring wide = mxcam_wide(in.c_str());
    if (length < 0)
        wide.push_back(L'\0');  // (counted, as Windows does for a -1 length)
    if (!out || outSize == 0)
        return static_cast<int>(wide.size());
    if (static_cast<int>(wide.size()) > outSize)
        return 0;
    wmemcpy(out, wide.data(), wide.size());
    return static_cast<int>(wide.size());
}

int WideCharToMultiByte(UINT, DWORD, LPCWSTR text, int length, LPSTR out, int outSize, LPCSTR, BOOL* usedDefault)
{
    if (usedDefault)
        *usedDefault = FALSE;
    const std::wstring in = length < 0 ? std::wstring(text) : std::wstring(text, static_cast<size_t>(length));
    std::string narrow = mxcam_utf8(in.c_str());
    if (length < 0)
        narrow.push_back('\0');
    if (!out || outSize == 0)
        return static_cast<int>(narrow.size());
    if (static_cast<int>(narrow.size()) > outSize)
        return 0;
    memcpy(out, narrow.data(), narrow.size());
    return static_cast<int>(narrow.size());
}

namespace {
// A wide format as Windows reads it, for glibc: %s / %c (no size given) are wide strings and
// characters there, %S / %C narrow ones.
std::wstring WindowsWideFormat(const wchar_t* format, bool scan)
{
    std::wstring out;
    for (const wchar_t* p = format; *p;) {
        if (*p != L'%') {
            out += *p++;
            continue;
        }
        out += *p++;
        if (*p == L'%') {
            out += *p++;
            continue;
        }
        bool sized = false;
        while (*p && !wcschr(L"diouxXeEfFgGaAcCsSpn[", *p)) {
            if (*p == L'l' || *p == L'h' || *p == L'w' || *p == L'L' || *p == L'z' || *p == L'j' || *p == L't')
                sized = true;
            out += *p++;
        }
        if (!*p)
            break;
        const wchar_t c = *p++;
        if ((c == L's' || c == L'c' || (scan && c == L'[')) && !sized)
            out += L'l';
        if (c == L'S')
            out += L's';
        else if (c == L'C')
            out += L'c';
        else
            out += c;
        if (c == L'[') {  // (a scanset: copied through its closing bracket)
            if (*p == L']')
                out += *p++;
            while (*p && *p != L']')
                out += *p++;
            if (*p)
                out += *p++;
        }
    }
    return out;
}
}  // namespace

int mxcam_vswprintf(wchar_t* buffer, size_t size, const wchar_t* format, va_list args)
{
    if (!buffer || size == 0)
        return -1;
    const std::wstring fixed = WindowsWideFormat(format, false);
    const int n = vswprintf(buffer, size, fixed.c_str(), args);
    if (n < 0)
        buffer[size - 1] = L'\0';
    return n;
}

int mxcam_swscanf(const wchar_t* text, const wchar_t* format, ...)
{
    const std::wstring fixed = WindowsWideFormat(format, true);
    va_list args;
    va_start(args, format);
    const int n = vswscanf(text, fixed.c_str(), args);
    va_end(args);
    return n;
}

// ---- Files --------------------------------------------------------------------------------
namespace {
struct FileHandle {
    int         fd = -1;
    bool        deleteOnClose = false;
    std::string path;
};
}  // namespace

HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD, void*, DWORD disposition, DWORD flags, HANDLE)
{
    const std::string p = mxcam_path(path);
    int mode = (access & GENERIC_WRITE) ? ((access & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disposition) {
    case CREATE_NEW:        mode |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS:     mode |= O_CREAT | O_TRUNC; break;
    case OPEN_ALWAYS:       mode |= O_CREAT; break;
    case TRUNCATE_EXISTING: mode |= O_TRUNC; break;
    default:                break;
    }
    const int fd = open(p.c_str(), mode | O_CLOEXEC, 0644);
    if (fd < 0) {
        Fail();
        return INVALID_HANDLE_VALUE;
    }
    FileHandle* h = new FileHandle;
    h->fd = fd;
    h->deleteOnClose = (flags & FILE_FLAG_DELETE_ON_CLOSE) != 0;
    h->path = p;
    return h;
}

BOOL WriteFile(HANDLE file, LPCVOID data, DWORD size, LPDWORD written, void*)
{
    FileHandle* h = static_cast<FileHandle*>(file);
    if (written)
        *written = 0;
    if (!h || file == INVALID_HANDLE_VALUE)
        return FALSE;
    const char* p = static_cast<const char*>(data);
    DWORD done = 0;
    while (done < size) {
        const ssize_t n = write(h->fd, p + done, size - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (written)
                *written = done;
            return Fail();
        }
        done += static_cast<DWORD>(n);
    }
    if (written)
        *written = done;
    return TRUE;
}

BOOL ReadFile(HANDLE file, LPVOID data, DWORD size, LPDWORD read, void*)
{
    FileHandle* h = static_cast<FileHandle*>(file);
    if (read)
        *read = 0;
    if (!h || file == INVALID_HANDLE_VALUE)
        return FALSE;
    ssize_t n;
    do {
        n = ::read(h->fd, data, size);
    } while (n < 0 && errno == EINTR);
    if (n < 0)
        return Fail();
    if (read)
        *read = static_cast<DWORD>(n);
    return TRUE;
}

BOOL FlushFileBuffers(HANDLE file)
{
    FileHandle* h = static_cast<FileHandle*>(file);
    return h && file != INVALID_HANDLE_VALUE && fsync(h->fd) == 0;
}

BOOL CloseHandle(HANDLE handle)
{
    FileHandle* h = static_cast<FileHandle*>(handle);
    if (!h || handle == INVALID_HANDLE_VALUE)
        return FALSE;
    close(h->fd);
    if (h->deleteOnClose)
        unlink(h->path.c_str());
    delete h;
    return TRUE;
}

DWORD GetFileSize(HANDLE file, LPDWORD high)
{
    FileHandle* h = static_cast<FileHandle*>(file);
    struct stat st;
    if (!h || file == INVALID_HANDLE_VALUE || fstat(h->fd, &st) != 0)
        return 0xFFFFFFFFu;
    if (high)
        *high = static_cast<DWORD>(static_cast<uint64_t>(st.st_size) >> 32);
    return static_cast<DWORD>(st.st_size & 0xFFFFFFFFu);
}

DWORD GetFileAttributesW(LPCWSTR path)
{
    struct stat st;
    if (stat(mxcam_path(path).c_str(), &st) != 0) {
        Fail();
        return INVALID_FILE_ATTRIBUTES;
    }
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

BOOL GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS, LPVOID info)
{
    struct stat st;
    if (stat(mxcam_path(path).c_str(), &st) != 0)
        return Fail();
    WIN32_FILE_ATTRIBUTE_DATA* out = static_cast<WIN32_FILE_ATTRIBUTE_DATA*>(info);
    memset(out, 0, sizeof *out);
    out->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    out->nFileSizeHigh = static_cast<DWORD>(static_cast<uint64_t>(st.st_size) >> 32);
    out->nFileSizeLow = static_cast<DWORD>(st.st_size & 0xFFFFFFFFu);
    // (100 ns ticks since 1601, as Windows counts them)
    const uint64_t ticks = (static_cast<uint64_t>(st.st_mtime) + 11644473600ULL) * 10000000ULL;
    out->ftLastWriteTime.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFu);
    out->ftLastWriteTime.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
    out->ftCreationTime = out->ftLastAccessTime = out->ftLastWriteTime;
    return TRUE;
}

BOOL CreateDirectoryW(LPCWSTR path, void*)
{
    return mkdir(mxcam_path(path).c_str(), 0755) == 0 ? TRUE : Fail();
}

BOOL DeleteFileW(LPCWSTR path)
{
    return unlink(mxcam_path(path).c_str()) == 0 ? TRUE : Fail();
}

namespace {
bool CopyBytes(const std::string& from, const std::string& to, bool failIfExists)
{
    const int in = open(from.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return Fail();
    struct stat st;
    fstat(in, &st);
    const int out = open(to.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | (failIfExists ? O_EXCL : O_TRUNC), st.st_mode & 0777);
    if (out < 0) {
        close(in);
        return Fail();
    }
    std::vector<char> buffer(1 << 16);
    bool ok = true;
    for (;;) {
        const ssize_t n = read(in, buffer.data(), buffer.size());
        if (n == 0)
            break;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ok = false;
            break;
        }
        ssize_t done = 0;
        while (done < n) {
            const ssize_t w = write(out, buffer.data() + done, static_cast<size_t>(n - done));
            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0) {
                ok = false;
                break;
            }
            done += w;
        }
        if (!ok)
            break;
    }
    if (ok)
        ok = fsync(out) == 0 || errno == EINVAL;
    if (!ok)
        Fail();
    close(in);
    close(out);
    if (!ok)
        unlink(to.c_str());
    return ok;
}
}  // namespace

BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags)
{
    const std::string f = mxcam_path(from), t = mxcam_path(to);
    if (!(flags & MOVEFILE_REPLACE_EXISTING)) {
        struct stat st;
        if (stat(t.c_str(), &st) == 0) {
            g_lastError = ERROR_ALREADY_EXISTS;
            return FALSE;
        }
    }
    if (rename(f.c_str(), t.c_str()) == 0)
        return TRUE;
    if (errno == EXDEV && (flags & MOVEFILE_COPY_ALLOWED) && CopyBytes(f, t, false)) {
        unlink(f.c_str());
        return TRUE;
    }
    return Fail();
}

BOOL CopyFileW(LPCWSTR from, LPCWSTR to, BOOL failIfExists)
{
    return CopyBytes(mxcam_path(from), mxcam_path(to), failIfExists != FALSE) ? TRUE : FALSE;
}

FILE* _wfopen(const wchar_t* path, const wchar_t* mode)
{
    std::string m = mxcam_utf8(mode);
    // (Windows' "t"/"b" and ccs= flags: Linux reads every file as bytes.)
    std::string plain;
    for (char c : m) {
        if (c == ',')
            break;
        if (c != 't')
            plain += c;
    }
    FILE* f = fopen(mxcam_path(path).c_str(), plain.c_str());
    if (!f)
        Fail();
    return f;
}

int _wfopen_s(FILE** file, const wchar_t* path, const wchar_t* mode)
{
    *file = _wfopen(path, mode);
    return *file ? 0 : (errno ? errno : ENOENT);
}

FILE* _wfsopen(const wchar_t* path, const wchar_t* mode, int) { return _wfopen(path, mode); }

// ---- Settings files (Windows .ini) --------------------------------------------------------
namespace {
std::string Trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
        --b;
    return s.substr(a, b - a);
}

bool ReadLines(const std::string& path, std::vector<std::string>& lines, bool& crlf)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    std::string text;
    char buffer[8192];
    size_t n;
    while ((n = fread(buffer, 1, sizeof buffer, f)) > 0)
        text.append(buffer, n);
    fclose(f);
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.erase(0, 3);
    crlf = text.find("\r\n") != std::string::npos;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find('\n', start);
        std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (end == std::string::npos) {
            if (!line.empty())
                lines.push_back(line);
            break;
        }
        lines.push_back(line);
        start = end + 1;
    }
    return true;
}

// The section a line opens ("" when it doesn't).
bool SectionOf(const std::string& line, std::string& name)
{
    const std::string t = Trim(line);
    if (t.size() < 2 || t[0] != '[')
        return false;
    const size_t close = t.find(']');
    if (close == std::string::npos)
        return false;
    name = Trim(t.substr(1, close - 1));
    return true;
}

// The key a line sets, and its value.
bool KeyOf(const std::string& line, std::string& key, std::string& value)
{
    const std::string t = Trim(line);
    if (t.empty() || t[0] == ';' || t[0] == '#' || t[0] == '[')
        return false;
    const size_t eq = t.find('=');
    if (eq == std::string::npos)
        return false;
    key = Trim(t.substr(0, eq));
    value = Trim(t.substr(eq + 1));
    return true;
}
}  // namespace

DWORD GetPrivateProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR defaultValue, LPWSTR out, DWORD size, LPCWSTR path)
{
    if (!out || size == 0)
        return 0;
    std::wstring result = defaultValue ? defaultValue : L"";
    std::vector<std::string> lines;
    bool crlf = false;
    if (section && key && ReadLines(mxcam_path(path), lines, crlf)) {
        const std::string wantSection = mxcam_utf8(section), wantKey = mxcam_utf8(key);
        bool inSection = false;
        for (const std::string& line : lines) {
            std::string name, k, v;
            if (SectionOf(line, name)) {
                inSection = strcasecmp(name.c_str(), wantSection.c_str()) == 0;
                continue;
            }
            if (inSection && KeyOf(line, k, v) && strcasecmp(k.c_str(), wantKey.c_str()) == 0) {
                // (As Windows: a value in double quotes is given without them.)
                if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\'')))
                    v = v.substr(1, v.size() - 2);
                result = mxcam_wide(v.c_str());
                break;
            }
        }
    }
    const size_t n = result.size() < size ? result.size() : size - 1;
    wmemcpy(out, result.data(), n);
    out[n] = L'\0';
    return static_cast<DWORD>(n);
}

UINT GetPrivateProfileIntW(LPCWSTR section, LPCWSTR key, int defaultValue, LPCWSTR path)
{
    wchar_t text[64];
    if (GetPrivateProfileStringW(section, key, L"", text, 64, path) == 0)
        return static_cast<UINT>(defaultValue);
    return static_cast<UINT>(wcstol(text, nullptr, 0));
}

BOOL WritePrivateProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR value, LPCWSTR path)
{
    if (!section)
        return FALSE;
    const std::string p = mxcam_path(path);
    std::vector<std::string> lines;
    bool crlf = false;
    ReadLines(p, lines, crlf);
    const std::string wantSection = mxcam_utf8(section);
    const std::string wantKey = key ? mxcam_utf8(key) : std::string();
    const std::string newLine = key && value ? wantKey + "=" + mxcam_utf8(value) : std::string();

    // The section's lines: from its heading to the next.
    int heading = -1, end = static_cast<int>(lines.size());
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        std::string name;
        if (!SectionOf(lines[i], name))
            continue;
        if (heading >= 0) {
            end = i;
            break;
        }
        if (strcasecmp(name.c_str(), wantSection.c_str()) == 0)
            heading = i;
    }
    if (heading < 0) {
        if (!key || !value)
            return TRUE;  // (nothing to delete)
        if (!lines.empty() && !Trim(lines.back()).empty())
            lines.push_back("");
        lines.push_back("[" + wantSection + "]");
        lines.push_back(newLine);
    } else if (!key) {
        lines.erase(lines.begin() + heading, lines.begin() + end);  // the whole section
    } else {
        int found = -1, last = heading;
        for (int i = heading + 1; i < end; ++i) {
            std::string k, v;
            if (KeyOf(lines[i], k, v)) {
                last = i;
                if (found < 0 && strcasecmp(k.c_str(), wantKey.c_str()) == 0)
                    found = i;
            }
        }
        if (found >= 0) {
            if (value)
                lines[found] = newLine;
            else
                lines.erase(lines.begin() + found);
        } else if (value) {
            lines.insert(lines.begin() + last + 1, newLine);  // after the section's last key
        }
    }
    std::string text;
    for (const std::string& line : lines) {
        text += line;
        text += crlf ? "\r\n" : "\n";
    }
    const std::string temp = p + ".writing";
    FILE* f = fopen(temp.c_str(), "wb");
    if (!f)
        return Fail();
    const bool written = fwrite(text.data(), 1, text.size(), f) == text.size();
    const bool closed = fclose(f) == 0;
    if (!written || !closed || rename(temp.c_str(), p.c_str()) != 0) {
        Fail();
        unlink(temp.c_str());
        return FALSE;
    }
    return TRUE;
}

// ---- Keyboard and mouse --------------------------------------------------------------------
SHORT GetAsyncKeyState(int vk)
{
    return mxcam::platform::KeyDown(vk) ? static_cast<SHORT>(0x8000) : 0;
}

SHORT GetKeyState(int vk)
{
    return GetAsyncKeyState(vk);  // (the lock keys' toggle state isn't followed: caps lock reads off)
}

SHORT VkKeyScanA(char ch)
{
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c >= 'a' && c <= 'z')
        return static_cast<SHORT>(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z')
        return static_cast<SHORT>(0x100 | c);
    if (c >= '0' && c <= '9')
        return static_cast<SHORT>(c);
    static const struct { char ch; SHORT code; } kOthers[] = {
        { ' ', VK_SPACE }, { '\r', VK_RETURN }, { '\t', VK_TAB }, { ';', VK_OEM_1 }, { '=', VK_OEM_PLUS },
        { ',', VK_OEM_COMMA }, { '-', VK_OEM_MINUS }, { '.', VK_OEM_PERIOD }, { '/', VK_OEM_2 }, { '`', VK_OEM_3 },
        { '[', VK_OEM_4 }, { '\\', VK_OEM_5 }, { ']', VK_OEM_6 }, { '\'', VK_OEM_7 },
    };
    for (const auto& o : kOthers)
        if (o.ch == ch)
            return o.code;
    return -1;
}

namespace {
const char* VirtualKeyName(int vk)
{
    static char name[16];
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        name[0] = static_cast<char>(vk);
        name[1] = '\0';
        return name;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        snprintf(name, sizeof name, "F%d", vk - VK_F1 + 1);
        return name;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD0 + 9) {
        snprintf(name, sizeof name, "Num %d", vk - VK_NUMPAD0);
        return name;
    }
    switch (vk) {
    case VK_LBUTTON:  return "Left Mouse";
    case VK_RBUTTON:  return "Right Mouse";
    case VK_MBUTTON:  return "Middle Mouse";
    case VK_BACK:     return "Backspace";
    case VK_TAB:      return "Tab";
    case VK_RETURN:   return "Enter";
    case VK_SHIFT:    return "Shift";
    case VK_CONTROL:  return "Ctrl";
    case VK_MENU:     return "Alt";
    case VK_PAUSE:    return "Pause";
    case VK_CAPITAL:  return "Caps Lock";
    case VK_ESCAPE:   return "Esc";
    case VK_SPACE:    return "Space";
    case VK_PRIOR:    return "Page Up";
    case VK_NEXT:     return "Page Down";
    case VK_END:      return "End";
    case VK_HOME:     return "Home";
    case VK_LEFT:     return "Left";
    case VK_UP:       return "Up";
    case VK_RIGHT:    return "Right";
    case VK_DOWN:     return "Down";
    case VK_INSERT:   return "Insert";
    case VK_DELETE:   return "Delete";
    case VK_MULTIPLY: return "Num *";
    case VK_ADD:      return "Num +";
    case VK_SUBTRACT: return "Num -";
    case VK_DECIMAL:  return "Num Del";
    case VK_DIVIDE:   return "Num /";
    case VK_NUMLOCK:  return "Num Lock";
    case VK_SCROLL:   return "Scroll Lock";
    case VK_LSHIFT:   return "Shift";
    case VK_RSHIFT:   return "Right Shift";
    case VK_LCONTROL: return "Ctrl";
    case VK_RCONTROL: return "Right Ctrl";
    case VK_LMENU:    return "Alt";
    case VK_RMENU:    return "Right Alt";
    case VK_OEM_1:    return ";";
    case VK_OEM_PLUS: return "=";
    case VK_OEM_COMMA: return ",";
    case VK_OEM_MINUS: return "-";
    case VK_OEM_PERIOD: return ".";
    case VK_OEM_2:    return "/";
    case VK_OEM_3:    return "`";
    case VK_OEM_4:    return "[";
    case VK_OEM_5:    return "\\";
    case VK_OEM_6:    return "]";
    case VK_OEM_7:    return "'";
    default:          return nullptr;
    }
}
}  // namespace

// (The "scan code" here is the key's own code: GetKeyNameText gets it back.)
UINT MapVirtualKeyW(UINT code, UINT) { return VirtualKeyName(static_cast<int>(code)) ? code : 0; }
UINT MapVirtualKeyA(UINT code, UINT mapType) { return MapVirtualKeyW(code, mapType); }

int GetKeyNameTextA(LONG lParam, LPSTR out, int size)
{
    const char* name = VirtualKeyName(static_cast<int>((lParam >> 16) & 0x1FF));
    if (!name || size <= 0)
        return 0;
    snprintf(out, static_cast<size_t>(size), "%s", name);
    return static_cast<int>(strlen(out));
}

int GetKeyNameTextW(LONG lParam, LPWSTR out, int size)
{
    char narrow[32];
    const int n = GetKeyNameTextA(lParam, narrow, sizeof narrow);
    if (n <= 0 || size <= 0)
        return 0;
    const std::wstring wide = mxcam_wide(narrow);
    wcsncpy_s(out, static_cast<size_t>(size), wide.c_str(), _TRUNCATE);
    return static_cast<int>(wcslen(out));
}

HCURSOR SetCursor(HCURSOR) { return nullptr; }

// ---- Windows and contexts ------------------------------------------------------------------
// There's one window that matters, the game's: every HWND / HDC here is it.
struct LinuxWindow {
    int unused;
};
namespace {
LinuxWindow g_window{};
}

HWND GetForegroundWindow() { return mxcam::platform::WindowFocused() ? &g_window : nullptr; }

DWORD GetWindowThreadProcessId(HWND window, LPDWORD process)
{
    if (process)
        *process = window ? GetCurrentProcessId() : 0;
    return 0;
}

BOOL GetClientRect(HWND window, RECT* rect)
{
    int w = 0, h = 0;
    if (!window || !mxcam::platform::WindowSize(w, h))
        return FALSE;
    rect->left = rect->top = 0;
    rect->right = w;
    rect->bottom = h;
    return TRUE;
}

// (The pointer is kept in the game window's own coordinates already: ScreenToClient leaves it.)
BOOL GetCursorPos(POINT* point)
{
    int x = 0, y = 0;
    if (!mxcam::platform::PointerInWindow(x, y))
        return FALSE;
    point->x = x;
    point->y = y;
    return TRUE;
}

BOOL ScreenToClient(HWND window, POINT*) { return window ? TRUE : FALSE; }
BOOL IsWindow(HWND window) { return window != nullptr; }

HGLRC wglGetCurrentContext()
{
    using Fn = void* (*)();
    static Fn current = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, "glXGetCurrentContext"));
    return current ? current() : nullptr;
}

HDC wglGetCurrentDC() { return mxcam::platform::GameWindow() ? &g_window : nullptr; }
HWND WindowFromDC(HDC dc) { return dc; }

// ---- Modules ------------------------------------------------------------------------------
HMODULE GetModuleHandleW(LPCWSTR name)
{
    if (!name)
        return dlopen(nullptr, RTLD_NOW | RTLD_NOLOAD);
    return dlopen(mxcam_utf8(name).c_str(), RTLD_NOW | RTLD_NOLOAD);
}

DWORD GetModuleFileNameW(HMODULE module, LPWSTR out, DWORD size)
{
    Dl_info info{};
    const void* address = module ? dlsym(module, "_init") : nullptr;
    if (!address || !dladdr(address, &info) || !info.dli_fname)
        address = reinterpret_cast<const void*>(&GetModuleFileNameW), dladdr(address, &info);
    if (!info.dli_fname || size == 0)
        return 0;
    const std::wstring wide = mxcam_wide(info.dli_fname);
    wcsncpy_s(out, size, wide.c_str(), _TRUNCATE);
    return static_cast<DWORD>(wcslen(out));
}

FARPROC GetProcAddress(HMODULE module, LPCSTR name)
{
    return reinterpret_cast<FARPROC>(dlsym(module ? module : RTLD_DEFAULT, name));
}
