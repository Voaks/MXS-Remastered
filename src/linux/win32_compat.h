// win32_compat.h (Linux build only)
//   The handful of Windows types and functions the shared code uses, done the Linux way, so the
//   camera, the effects, the settings and the menu compile unchanged on both systems. Only what
//   the mod calls is here, and only as much of each as it relies on. linux/include/windows.h
//   brings it in where the Windows build includes <windows.h>.
//
//   Paths: the shared code builds them Windows-style (L"folder\\file"). Every function here that
//   takes a path turns it into UTF-8 with '/' separators.
#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <cctype>
#include <pthread.h>
#include <strings.h>
#include <string>

// ---- Calling conventions and basic types -------------------------------------------------
#define WINAPI
#define APIENTRY
#define CALLBACK
#define WINGDIAPI
#define __stdcall
#define __cdecl

typedef int                BOOL;
typedef unsigned char      BYTE;
typedef unsigned short     WORD;
typedef unsigned long      DWORD;  // (as Windows code prints it: %lu)
typedef long               LONG;
typedef unsigned long      ULONG;
typedef short              SHORT;
typedef unsigned short     USHORT;
typedef int                INT;
typedef unsigned int       UINT;
typedef char               CHAR;
typedef wchar_t            WCHAR;
typedef float              FLOAT;
typedef int64_t            LONGLONG;
typedef uint64_t           ULONGLONG;
typedef intptr_t           LONG_PTR;
typedef uintptr_t          ULONG_PTR;
typedef intptr_t           INT_PTR;
typedef uintptr_t          UINT_PTR;
typedef uintptr_t          DWORD_PTR;
typedef size_t             SIZE_T;
typedef const wchar_t*     LPCWSTR;
typedef wchar_t*           LPWSTR;
typedef const char*        LPCSTR;
typedef char*              LPSTR;
typedef void*              LPVOID;
typedef const void*        LPCVOID;
typedef DWORD*             LPDWORD;
typedef long               HRESULT;
typedef uintptr_t          WPARAM;
typedef intptr_t           LPARAM;
typedef intptr_t           LRESULT;
typedef void*              HANDLE;
typedef void*              HMODULE;
typedef void*              HINSTANCE;
typedef void*              HGLRC;
typedef void*              HCURSOR;
typedef void (*PROC)();
typedef void (*FARPROC)();

// The game's window and its "device context": on Linux both are the X11 window the game draws
// into (linux/input_linux.cpp keeps the display it's on).
struct LinuxWindow;
typedef LinuxWindow* HWND;
typedef LinuxWindow* HDC;
typedef LRESULT (*WNDPROC)(HWND, UINT, WPARAM, LPARAM);

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#define MAX_PATH 4096
#define S_OK 0L
#define SUCCEEDED(hr) ((hr) >= 0)
#define FAILED(hr) ((hr) < 0)

template <class T, size_t N>
constexpr size_t mxcam_countof(T (&)[N]) { return N; }
#define _countof(a) mxcam_countof(a)
#define _TRUNCATE (static_cast<size_t>(-1))

struct RECT { LONG left, top, right, bottom; };
struct POINT { LONG x, y; };
union LARGE_INTEGER {
    struct { DWORD LowPart; LONG HighPart; } u;
    LONGLONG QuadPart;
};
struct SYSTEMTIME { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; };
struct FILETIME { DWORD dwLowDateTime, dwHighDateTime; };
struct WIN32_FILE_ATTRIBUTE_DATA {
    DWORD    dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD    nFileSizeHigh, nFileSizeLow;
};
enum GET_FILEEX_INFO_LEVELS { GetFileExInfoStandard };

// ---- Errors ------------------------------------------------------------------------------
#define ERROR_SUCCESS          0
#define ERROR_FILE_NOT_FOUND   2
#define ERROR_PATH_NOT_FOUND   3
#define ERROR_ACCESS_DENIED    5
#define ERROR_SHARING_VIOLATION 32
#define ERROR_ALREADY_EXISTS   183
DWORD GetLastError();
void SetLastError(DWORD error);

// ---- Time, processes, environment ---------------------------------------------------------
BOOL QueryPerformanceCounter(LARGE_INTEGER* count);
BOOL QueryPerformanceFrequency(LARGE_INTEGER* frequency);
ULONGLONG GetTickCount64();
DWORD GetTickCount();
void GetLocalTime(SYSTEMTIME* time);
void Sleep(DWORD milliseconds);
DWORD GetCurrentProcessId();
DWORD GetCurrentThreadId();
DWORD GetEnvironmentVariableA(LPCSTR name, LPSTR buffer, DWORD size);
DWORD GetEnvironmentVariableW(LPCWSTR name, LPWSTR buffer, DWORD size);
BOOL SetEnvironmentVariableA(LPCSTR name, LPCSTR value);

// ---- Locks (a slim reader/writer lock) ----------------------------------------------------
struct SRWLOCK { pthread_rwlock_t lock; };
#define SRWLOCK_INIT { PTHREAD_RWLOCK_INITIALIZER }
inline void InitializeSRWLock(SRWLOCK* l) { pthread_rwlock_init(&l->lock, nullptr); }
inline void AcquireSRWLockShared(SRWLOCK* l) { pthread_rwlock_rdlock(&l->lock); }
inline void ReleaseSRWLockShared(SRWLOCK* l) { pthread_rwlock_unlock(&l->lock); }
inline void AcquireSRWLockExclusive(SRWLOCK* l) { pthread_rwlock_wrlock(&l->lock); }
inline void ReleaseSRWLockExclusive(SRWLOCK* l) { pthread_rwlock_unlock(&l->lock); }

// ---- Strings ------------------------------------------------------------------------------
#define CP_ACP  0
#define CP_UTF8 65001
int MultiByteToWideChar(UINT codePage, DWORD flags, LPCSTR text, int length, LPWSTR out, int outSize);
int WideCharToMultiByte(UINT codePage, DWORD flags, LPCWSTR text, int length, LPSTR out, int outSize,
                        LPCSTR defaultChar, BOOL* usedDefault);
std::string mxcam_utf8(const wchar_t* text);         // wide to UTF-8
std::wstring mxcam_wide(const char* text);           // UTF-8 to wide
std::string mxcam_path(const wchar_t* path);         // a Windows-style wide path as a Linux one

inline int _stricmp(const char* a, const char* b) { return strcasecmp(a, b); }
inline int _strnicmp(const char* a, const char* b, size_t n) { return strncasecmp(a, b, n); }
inline int _wcsicmp(const wchar_t* a, const wchar_t* b) { return wcscasecmp(a, b); }
inline int _wcsnicmp(const wchar_t* a, const wchar_t* b, size_t n) { return wcsncasecmp(a, b, n); }
inline int _wtoi(const wchar_t* s) { return static_cast<int>(wcstol(s, nullptr, 10)); }
inline double _wtof(const wchar_t* s) { return wcstod(s, nullptr); }

// The "secure" CRT functions: always terminated, cut short when they don't fit.
inline int strcpy_s(char* dest, size_t size, const char* src)
{
    if (!dest || size == 0) return 22;
    snprintf(dest, size, "%s", src ? src : "");
    return 0;
}
template <size_t N> int strcpy_s(char (&dest)[N], const char* src) { return strcpy_s(dest, N, src); }
inline int strncpy_s(char* dest, size_t size, const char* src, size_t count)
{
    if (!dest || size == 0) return 22;
    size_t n = src ? strnlen(src, count == _TRUNCATE ? size - 1 : count) : 0;
    if (n >= size) n = size - 1;
    memcpy(dest, src, n);
    dest[n] = '\0';
    return 0;
}
template <size_t N> int strncpy_s(char (&dest)[N], const char* src, size_t count) { return strncpy_s(dest, N, src, count); }
inline int strcat_s(char* dest, size_t size, const char* src)
{
    const size_t have = strnlen(dest, size);
    return have < size ? strcpy_s(dest + have, size - have, src) : 22;
}
template <size_t N> int strcat_s(char (&dest)[N], const char* src) { return strcat_s(dest, N, src); }
inline int wcscpy_s(wchar_t* dest, size_t size, const wchar_t* src)
{
    if (!dest || size == 0) return 22;
    size_t n = src ? wcsnlen(src, size - 1) : 0;
    wmemcpy(dest, src, n);
    dest[n] = L'\0';
    return 0;
}
template <size_t N> int wcscpy_s(wchar_t (&dest)[N], const wchar_t* src) { return wcscpy_s(dest, N, src); }
inline int wcsncpy_s(wchar_t* dest, size_t size, const wchar_t* src, size_t count)
{
    if (!dest || size == 0) return 22;
    size_t n = src ? wcsnlen(src, count == _TRUNCATE ? size - 1 : count) : 0;
    if (n >= size) n = size - 1;
    wmemcpy(dest, src, n);
    dest[n] = L'\0';
    return 0;
}
template <size_t N> int wcsncpy_s(wchar_t (&dest)[N], const wchar_t* src, size_t count) { return wcsncpy_s(dest, N, src, count); }
inline int wcscat_s(wchar_t* dest, size_t size, const wchar_t* src)
{
    const size_t have = wcsnlen(dest, size);
    return have < size ? wcscpy_s(dest + have, size - have, src) : 22;
}
template <size_t N> int wcscat_s(wchar_t (&dest)[N], const wchar_t* src) { return wcscat_s(dest, N, src); }

// Wide printf: Windows reads %s in a wide format as a wide string; so does this (each %s, %c is
// given the l it needs before glibc sees the format).
int mxcam_vswprintf(wchar_t* buffer, size_t size, const wchar_t* format, va_list args);
inline int swprintf_s(wchar_t* buffer, size_t size, const wchar_t* format, ...)
{
    va_list args;
    va_start(args, format);
    const int n = mxcam_vswprintf(buffer, size, format, args);
    va_end(args);
    return n;
}
template <size_t N> int swprintf_s(wchar_t (&buffer)[N], const wchar_t* format, ...)
{
    va_list args;
    va_start(args, format);
    const int n = mxcam_vswprintf(buffer, N, format, args);
    va_end(args);
    return n;
}
inline int _snwprintf_s(wchar_t* buffer, size_t size, size_t, const wchar_t* format, ...)
{
    va_list args;
    va_start(args, format);
    const int n = mxcam_vswprintf(buffer, size, format, args);
    va_end(args);
    return n;
}
#define sprintf_s snprintf
#define _snprintf_s(buffer, size, count, ...) snprintf(buffer, size, __VA_ARGS__)
#define vsprintf_s vsnprintf
#define sscanf_s sscanf
#define swscanf_s mxcam_swscanf
int mxcam_swscanf(const wchar_t* text, const wchar_t* format, ...);

// ---- Files --------------------------------------------------------------------------------
#define INVALID_HANDLE_VALUE (reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1)))
#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define FILE_ATTRIBUTE_READONLY  0x01
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_NORMAL    0x80
#define FILE_ATTRIBUTE_TEMPORARY 0x100
#define GENERIC_READ   0x80000000u
#define GENERIC_WRITE  0x40000000u
#define FILE_SHARE_READ   1
#define FILE_SHARE_WRITE  2
#define FILE_SHARE_DELETE 4
#define CREATE_NEW        1
#define CREATE_ALWAYS     2
#define OPEN_EXISTING     3
#define OPEN_ALWAYS       4
#define TRUNCATE_EXISTING 5
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000u
#define FILE_FLAG_WRITE_THROUGH   0x80000000u
#define MOVEFILE_REPLACE_EXISTING 1
#define MOVEFILE_COPY_ALLOWED     2
#define MOVEFILE_WRITE_THROUGH    8
#define FILE_BEGIN   0
#define FILE_CURRENT 1
#define FILE_END     2

HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, void* security, DWORD disposition, DWORD flags,
                   HANDLE templateFile);
BOOL WriteFile(HANDLE file, LPCVOID data, DWORD size, LPDWORD written, void* overlapped);
BOOL ReadFile(HANDLE file, LPVOID data, DWORD size, LPDWORD read, void* overlapped);
BOOL FlushFileBuffers(HANDLE file);
BOOL CloseHandle(HANDLE handle);
DWORD GetFileSize(HANDLE file, LPDWORD high);
DWORD GetFileAttributesW(LPCWSTR path);
BOOL GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID info);
BOOL CreateDirectoryW(LPCWSTR path, void* security);
BOOL DeleteFileW(LPCWSTR path);
BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags);
inline BOOL MoveFileW(LPCWSTR from, LPCWSTR to) { return MoveFileExW(from, to, MOVEFILE_COPY_ALLOWED); }
inline LONG CompareFileTime(const FILETIME* a, const FILETIME* b)
{
    const uint64_t x = (static_cast<uint64_t>(a->dwHighDateTime) << 32) | a->dwLowDateTime;
    const uint64_t y = (static_cast<uint64_t>(b->dwHighDateTime) << 32) | b->dwLowDateTime;
    return x < y ? -1 : x > y ? 1 : 0;
}
BOOL CopyFileW(LPCWSTR from, LPCWSTR to, BOOL failIfExists);
int _wfopen_s(FILE** file, const wchar_t* path, const wchar_t* mode);
FILE* _wfopen(const wchar_t* path, const wchar_t* mode);
FILE* _wfsopen(const wchar_t* path, const wchar_t* mode, int share);
#define _SH_DENYNO 0x40
#define _SH_DENYWR 0x20

// ---- Settings files (Windows .ini) --------------------------------------------------------
// As Windows does them: [Section] then Key=Value lines; sections and keys found ignoring case;
// a missing file or key gives the default; writing keeps the rest of the file (its comments and
// other keys) as it was; a null value deletes the key.
DWORD GetPrivateProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR defaultValue, LPWSTR out, DWORD size, LPCWSTR path);
UINT GetPrivateProfileIntW(LPCWSTR section, LPCWSTR key, int defaultValue, LPCWSTR path);
BOOL WritePrivateProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR value, LPCWSTR path);

// ---- Keyboard and mouse (linux/input_linux.cpp) -------------------------------------------
// The Windows virtual-key codes the mod's keybinds are saved as, mapped to X11 keys there.
#define VK_LBUTTON  0x01
#define VK_RBUTTON  0x02
#define VK_MBUTTON  0x04
#define VK_BACK     0x08
#define VK_TAB      0x09
#define VK_RETURN   0x0D
#define VK_SHIFT    0x10
#define VK_CONTROL  0x11
#define VK_MENU     0x12
#define VK_PAUSE    0x13
#define VK_CAPITAL  0x14
#define VK_ESCAPE   0x1B
#define VK_SPACE    0x20
#define VK_PRIOR    0x21
#define VK_NEXT     0x22
#define VK_END      0x23
#define VK_HOME     0x24
#define VK_LEFT     0x25
#define VK_UP       0x26
#define VK_RIGHT    0x27
#define VK_DOWN     0x28
#define VK_INSERT   0x2D
#define VK_DELETE   0x2E
#define VK_LWIN     0x5B
#define VK_RWIN     0x5C
#define VK_NUMPAD0  0x60
#define VK_MULTIPLY 0x6A
#define VK_ADD      0x6B
#define VK_SEPARATOR 0x6C
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL  0x6E
#define VK_DIVIDE   0x6F
#define VK_F1       0x70
#define VK_F2       0x71
#define VK_F12      0x7B
#define VK_F24      0x87
#define VK_NUMLOCK  0x90
#define VK_SCROLL   0x91
#define VK_LSHIFT   0xA0
#define VK_RSHIFT   0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU    0xA4
#define VK_RMENU    0xA5
#define VK_OEM_1      0xBA
#define VK_OEM_PLUS   0xBB
#define VK_OEM_COMMA  0xBC
#define VK_OEM_MINUS  0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2      0xBF
#define VK_OEM_3      0xC0
#define VK_OEM_4      0xDB
#define VK_OEM_5      0xDC
#define VK_OEM_6      0xDD
#define VK_OEM_7      0xDE
SHORT GetAsyncKeyState(int vk);
SHORT GetKeyState(int vk);
SHORT VkKeyScanA(char ch);
UINT MapVirtualKeyW(UINT code, UINT mapType);
int GetKeyNameTextW(LONG lParam, LPWSTR out, int size);
#define MAPVK_VK_TO_VSC 0
HWND GetForegroundWindow();
DWORD GetWindowThreadProcessId(HWND window, LPDWORD process);
BOOL GetClientRect(HWND window, RECT* rect);
BOOL GetCursorPos(POINT* point);
BOOL ScreenToClient(HWND window, POINT* point);
BOOL IsWindow(HWND window);

UINT MapVirtualKeyA(UINT code, UINT mapType);
int GetKeyNameTextA(LONG lParam, LPSTR out, int size);
// A key as the character it types (VK, shift and caps lock from state[]), by the X keyboard
// layout. Returns how many characters (0 or 1).
int ToUnicode(UINT vk, UINT scanCode, const BYTE* state, LPWSTR out, int size, UINT flags);
#define WHEEL_DELTA 120
HCURSOR SetCursor(HCURSOR cursor);  // (the X cursor is the game's own business: does nothing)

// ---- OpenGL contexts and windows (the GLX equivalents) ------------------------------------
HGLRC wglGetCurrentContext();
HDC wglGetCurrentDC();
HWND WindowFromDC(HDC dc);

// ---- Modules ------------------------------------------------------------------------------
HMODULE GetModuleHandleW(LPCWSTR name);
DWORD GetModuleFileNameW(HMODULE module, LPWSTR out, DWORD size);
FARPROC GetProcAddress(HMODULE module, LPCSTR name);
