#include "common.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>

static std::wstring g_logPath;
static CRITICAL_SECTION g_logCs;
static bool g_logCsInit = false;

std::wstring GetExePath() {
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    return std::wstring(buf);
}

std::wstring GetExeDir() {
    std::wstring p = GetExePath();
    size_t pos = p.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return p;
    return p.substr(0, pos);
}

std::wstring PathJoin(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    wchar_t last = dir[dir.size() - 1];
    if (last == L'\\' || last == L'/') return dir + name;
    return dir + L"\\" + name;
}

bool FileExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool EnsureDir(const std::wstring& dir) {
    if (dir.empty()) return false;
    DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) {
        return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    // 逐级创建，兼容已存在中间目录的情况
    size_t pos = 0;
    std::wstring cur;
    while ((pos = dir.find_first_of(L"\\/", pos + 1)) != std::wstring::npos) {
        cur = dir.substr(0, pos);
        if (cur.size() > 1) CreateDirectoryW(cur.c_str(), nullptr);
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    return GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool WriteAllBytes(const std::wstring& path, const void* data, size_t size) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    if (size > 0) {
        const BYTE* p = static_cast<const BYTE*>(data);
        size_t left = size;
        while (left > 0) {
            DWORD chunk = (DWORD)(left > (1u << 20) ? (1u << 20) : left);
            DWORD written = 0;
            if (!WriteFile(f, p, chunk, &written, nullptr) || written == 0) {
                ok = false;
                break;
            }
            p += written;
            left -= written;
        }
    }
    CloseHandle(f);
    return ok;
}

std::wstring FormatBytes(uint64_t bytes) {
    wchar_t buf[64];
    if (bytes >= 1024ull * 1024 * 1024) {
        swprintf_s(buf, L"%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ull * 1024) {
        swprintf_s(buf, L"%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024) {
        swprintf_s(buf, L"%.0f KB", (double)bytes / 1024.0);
    } else {
        swprintf_s(buf, L"%llu B", (unsigned long long)bytes);
    }
    return std::wstring(buf);
}

uint64_t DirSize(const std::wstring& dir) {
    uint64_t total = 0;
    WIN32_FIND_DATAW fd{};
    std::wstring pattern = PathJoin(dir, L"*");
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring p = PathJoin(dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            total += DirSize(p);
        } else {
            total += (uint64_t)fd.nFileSizeLow + ((uint64_t)fd.nFileSizeHigh << 32);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out((size_t)need, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], need);
    return out;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int need = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out((size_t)need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], need, nullptr, nullptr);
    return out;
}

// ── 日志 ────────────────────────────────────────────────────────────────
void LogInit(const std::wstring& dir) {
    g_logPath = PathJoin(dir, L"WallpaperFeed.log");
    if (!g_logCsInit) {
        InitializeCriticalSection(&g_logCs);
        g_logCsInit = true;
    }
}

std::wstring GetLogPath() { return g_logPath; }

void LogMsg(const wchar_t* fmt, ...) {
    if (g_logPath.empty()) return;
    if (!g_logCsInit) {
        InitializeCriticalSection(&g_logCs);
        g_logCsInit = true;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);

    wchar_t msg[2048] = {};
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, _TRUNCATE, fmt, ap);
    va_end(ap);

    std::string line;
    char head[64];
    sprintf_s(head, "[%04d-%02d-%02d %02d:%02d:%02d] ", st.wYear, st.wMonth, st.wDay,
              st.wHour, st.wMinute, st.wSecond);
    line = std::string(head) + WideToUtf8(msg) + "\r\n";

    EnterCriticalSection(&g_logCs);

    // 简单滚动：超过 2MB 时截断重写
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(g_logPath.c_str(), GetFileExInfoStandard, &fad)) {
        uint64_t size = (uint64_t)fad.nFileSizeHigh << 32 | fad.nFileSizeLow;
        if (size > 2ull * 1024ull * 1024ull) {
            DeleteFileW(g_logPath.c_str());
        }
    }

    HANDLE f = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(f, line.c_str(), (DWORD)line.size(), &written, nullptr);
        CloseHandle(f);
    }

    LeaveCriticalSection(&g_logCs);
}
