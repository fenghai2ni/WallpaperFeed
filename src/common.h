#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <cstdint>

#include "resource.h"

// ── 基础工具 ────────────────────────────────────────────────────────────
std::wstring GetExePath();
std::wstring GetExeDir();
std::wstring PathJoin(const std::wstring& dir, const std::wstring& name);
std::wstring Utf8ToWide(const std::string& s);
std::string  WideToUtf8(const std::wstring& s);
bool  FileExists(const std::wstring& path);
bool  EnsureDir(const std::wstring& dir);
bool  WriteAllBytes(const std::wstring& path, const void* data, size_t size);
std::wstring FormatBytes(uint64_t bytes);
uint64_t DirSize(const std::wstring& dir);   // 递归统计目录占用字节数

// ── 日志 ────────────────────────────────────────────────────────────────
void LogInit(const std::wstring& dir);
void LogMsg(const wchar_t* fmt, ...);
std::wstring GetLogPath();
