#include "app.h"
#include "imaging.h"
#include "net.h"
#include "library.h"
#include "ui.h"

#include <shlobj.h>
#include <shellapi.h>
#include <random>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {

const wchar_t* kClassName = L"WallpaperFeedTrayWnd";
const wchar_t* kAppName = L"WallpaperFeed 壁纸";
const wchar_t* kVersion = L"1.5.2";

const UINT WM_APP_TRAY = WM_APP + 1;
const UINT WM_APP_CHANGED = WM_APP + 2;
const UINT WM_APP_OPENMENU = WM_APP + 3;

const UINT_PTR TIMER_TICK = 1;

enum MenuId {
    ID_M_OPEN_UI = 1000,
    ID_M_NEXT = 1001,

    ID_M_MODE_DIFF = 2001,
    ID_M_MODE_SPAN = 2002,
    ID_M_MODE_CLONE = 2003,

    ID_M_INT_OFF = 2010,
    ID_M_INT_BASE = 2011,

    ID_M_SRC_TRENDING = 2020,
    ID_M_SRC_NEW = 2021,
    ID_M_SRC_POPULAR = 2022,

    ID_M_CAT_BASE = 2100,
    ID_M_RES_BASE = 2200,      // +0 = 不限，+1.. = GetResolutions() 顺序
    ID_M_FAV_BASE = 2300,      // +0.. = 收藏当前桌面第 N 屏的壁纸

    ID_M_AUTOSTART = 3001,
    ID_M_OPEN_CACHE = 3002,
    ID_M_OPEN_CONFIG = 3003,
    ID_M_ABOUT = 3004,
    ID_M_EXIT = 3005,
};

const int kIntervals[] = {15, 30, 60, 120, 240, 480, 720, 1440};
const wchar_t* kIntervalLabels[] = {L"15 分钟", L"30 分钟", L"1 小时", L"2 小时",
                                    L"4 小时", L"8 小时", L"12 小时", L"24 小时"};

std::wstring PickDataDir() {
    std::wstring dir = GetExeDir();
    std::wstring probe = PathJoin(dir, L".write_test");
    if (WriteAllBytes(probe, "x", 1)) {
        DeleteFileW(probe.c_str());
        return dir;
    }
    wchar_t appdata[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
        std::wstring fallback = PathJoin(appdata, L"WallpaperFeed");
        EnsureDir(fallback);
        return fallback;
    }
    return dir;
}

// ── 本地图片池（站点列表接口挂掉时靠它继续换图）─────────────────────────
std::string ReadFileUtf8(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::string();
    std::string out;
    LARGE_INTEGER sz{};
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < 16 * 1024 * 1024) {
        out.resize((size_t)sz.QuadPart);
        DWORD rd = 0;
        if (ReadFile(h, &out[0], (DWORD)out.size(), &rd, nullptr)) out.resize(rd);
        else out.clear();
    }
    CloseHandle(h);
    return out;
}

std::wstring FindCachedSrc(const std::wstring& dir, int id) {
    wchar_t pat[64];
    swprintf_s(pat, L"wf_%d_*.jpg", id);
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(PathJoin(dir, pat).c_str(), &fd);
    std::wstring out;
    if (h != INVALID_HANDLE_VALUE) {
        out = PathJoin(dir, std::wstring(fd.cFileName));
        FindClose(h);
    }
    return out;
}

void SavePoolFile(const std::wstring& path, const std::vector<WfItem>& items) {
    std::string out;
    for (const auto& it : items) {
        std::string title = it.title;
        for (size_t i = 0; i < title.size(); ++i) {
            if (title[i] == '\t' || title[i] == '\r' || title[i] == '\n') title[i] = ' ';
        }
        char head[64];
        sprintf_s(head, "%d\t%d\t%d\t", it.id, it.w, it.h);
        out += head;
        out += it.thumbUrl; out += '\t';
        out += it.href;     out += '\t';
        out += title;       out += '\n';
    }
    WriteAllBytes(path, out.data(), out.size());
}

bool LoadPoolFile(const std::wstring& path, std::vector<WfItem>& items) {
    std::string text = ReadFileUtf8(path);
    if (text.empty()) return false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.empty()) continue;
        std::vector<std::string> f;
        size_t s = 0;
        while (true) {
            size_t t = line.find('\t', s);
            if (t == std::string::npos) { f.push_back(line.substr(s)); break; }
            f.push_back(line.substr(s, t - s));
            s = t + 1;
        }
        if (f.size() < 6) continue;
        WfItem it;
        it.id = atoi(f[0].c_str());
        it.w = atoi(f[1].c_str());
        it.h = atoi(f[2].c_str());
        it.thumbUrl = f[3];
        it.href = f[4];
        it.title = f[5];
        if (it.id > 0) items.push_back(it);
    }
    return !items.empty();
}

LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    App* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return self->HandleMessage(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void CleanupDir(const std::wstring& dir, int keep,
                const std::vector<std::wstring>& protect) {
    std::wstring pattern = PathJoin(dir, L"*.jpg");
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    std::vector<std::pair<uint64_t, std::wstring>> files;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        uint64_t t = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) |
                     fd.ftLastWriteTime.dwLowDateTime;
        files.emplace_back(t, PathJoin(dir, fd.cFileName));
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(files.begin(), files.end(),
              [](const std::pair<uint64_t, std::wstring>& a,
                 const std::pair<uint64_t, std::wstring>& b) { return a.first > b.first; });

    int counted = 0;
    for (const auto& f : files) {
        bool isProtected = false;
        for (const auto& p : protect) {
            if (_wcsicmp(p.c_str(), f.second.c_str()) == 0) { isProtected = true; break; }
        }
        if (isProtected) continue;
        if (counted < keep) { ++counted; continue; }
        DeleteFileW(f.second.c_str());
    }
}

struct StoreFile {
    std::wstring path;
    uint64_t size = 0;
    uint64_t mtime = 0;   // FILETIME（100ns）
};

void CollectStoreFiles(const std::wstring& dir, std::vector<StoreFile>& out) {
    WIN32_FIND_DATAW fd{};
    std::wstring pattern = PathJoin(dir, L"*");
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring p = PathJoin(dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            CollectStoreFiles(p, out);
        } else {
            uint64_t sz = (uint64_t)fd.nFileSizeLow + ((uint64_t)fd.nFileSizeHigh << 32);
            uint64_t t = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) |
                         fd.ftLastWriteTime.dwLowDateTime;
            out.push_back({p, sz, t});
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

bool IsFullPathEqual(const std::vector<std::wstring>& list, const std::wstring& path) {
    for (const auto& p : list) {
        if (_wcsicmp(p.c_str(), path.c_str()) == 0) return true;
    }
    return false;
}

} // namespace
App::App(HINSTANCE hInst) : hInst_(hInst) {}

App::~App() {
    shuttingDown_ = true;
    if (worker_.joinable()) worker_.join();
    if (ui_) {
        delete ui_;
        ui_ = nullptr;
    }
    if (lib_) lib_->Shutdown();
    gfx::Shutdown();
}

void App::SaveConfig() {
    cfg_.Save(configPath_);
}

void App::NotifySettingsChanged() {
    ScheduleNext(true);
    UpdateTrayTip();
}

void App::NotifyManualApply() {
    ScheduleNext(true);
    UpdateTrayTip();
}

void App::OpenMainWindow() {
    if (ui_) ui_->Show();
}

bool App::Init(bool launchedAtStartup) {
    dataDir_ = PickDataDir();
    LogInit(dataDir_);

    cacheDir_ = PathJoin(dataDir_, L"wallpapers");
    srcDir_ = PathJoin(cacheDir_, L"src");
    curDir_ = PathJoin(cacheDir_, L"cur");
    EnsureDir(cacheDir_);
    EnsureDir(srcDir_);
    EnsureDir(curDir_);

    configPath_ = PathJoin(dataDir_, L"config.ini");
    cfg_.Load(configPath_);
    cfg_.autostart = autostart::IsEnabled();

    recent_.Load(PathJoin(dataDir_, L"recent.txt"));

    cacheStat_ = std::make_shared<CacheStat>();
    cacheStat_->dirs[0] = cacheDir_;
    cacheStat_->dirs[1] = PathJoin(dataDir_, L"thumbs");

    prefetchFlag_ = std::make_shared<std::atomic<bool>>(false);

    // 启动时先把上次落盘的图片池读进来，站点不通也能直接换图
    {
        std::vector<WfItem> saved;
        if (LoadPoolFile(PathJoin(dataDir_, L"pool.txt"), saved) && !saved.empty()) {
            std::lock_guard<std::mutex> lk(cacheMutex_);
            cachedItems_.swap(saved);
            LogMsg(L"载入本地图片池 %d 张", (int)cachedItems_.size());
        }
    }

    InitializeCriticalSection(&resultCs_);

    if (!gfx::Startup()) {
        LogMsg(L"GDI+ 初始化失败");
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = StaticWndProc;
    wc.hInstance = hInst_;
    wc.lpszClassName = kClassName;
    RegisterClassW(&wc);

    hwnd_ = CreateWindowExW(0, kClassName, kAppName, WS_POPUP, 0, 0, 0, 0,
                            nullptr, nullptr, hInst_, this);
    if (!hwnd_) {
        LogMsg(L"创建消息窗口失败");
        return false;
    }

    icon_ = LoadIconW(hInst_, MAKEINTRESOURCEW(IDI_APP));
    if (!icon_) icon_ = LoadIconW(nullptr, IDI_APPLICATION);

    AddTrayIcon();

    SetTimer(hwnd_, TIMER_TICK, 30000, nullptr);

    // 先统计显示器数量，让菜单/气泡一开始就有信息
    {
        std::vector<ScreenInfo> screens;
        bool perMon = false;
        std::wstring e;
        if (desk::EnumScreens(screens, perMon, e)) {
            lastScreenCount_ = (int)screens.size();
        }
    }

    uint64_t now = GetTickCount64();
    nextChangeTick_ = now + (launchedAtStartup ? 20000ull : 8000ull);

    // 配置里的开机启动状态与注册表不一致时以注册表为准
    if (!cfg_.autostart) cfg_.autostart = autostart::IsEnabled();

    cfg_.Save(configPath_);

    LogMsg(L"启动完成：数据目录 %s，模式 %s，间隔 %d 分钟", dataDir_.c_str(),
           cfg_.mode.c_str(), cfg_.intervalMinutes);

    UpdateTrayTip();

    // 壁纸库与主界面
    lib_ = std::make_unique<Library>();
    lib_->Init(dataDir_, hwnd_);

    ui_ = new MainWindow();
    if (!ui_->Create(hInst_, this, lib_.get(), &cfg_, dataDir_, configPath_,
                     cacheDir_, srcDir_, curDir_)) {
        LogMsg(L"主界面创建失败");
        delete ui_;
        ui_ = nullptr;
    }

    if (!launchedAtStartup) {
        ShowTip(L"WallpaperFeed 已启动", L"双击托盘图标打开主界面，右键打开菜单。");
    }

    CheckStorage(true);
    return true;
}

void App::Run() {
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

LRESULT App::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_APP_TRAY: {
        UINT evt = LOWORD(lp);
        if (evt == WM_LBUTTONDBLCLK) {
            if (ui_) ui_->Show();
        } else if (evt == WM_LBUTTONUP || evt == NIN_SELECT) {
            StartChange(true);
        } else if (evt == WM_RBUTTONUP || evt == WM_CONTEXTMENU) {
            POINT pt{};
            GetCursorPos(&pt);
            ShowMenu(pt);
        }
        return 0;
    }
    case Library::WM_LIB_UPDATED:
        if (ui_) ui_->OnLibraryUpdated();
        return 0;
    case Library::WM_THUMB_READY:
        if (ui_) ui_->OnThumbReady();
        return 0;
    case Library::WM_FAV_READY:
        if (ui_) ui_->OnThumbReady();
        return 0;
    case WM_APP_CHANGED: {
        busy_ = false;
        bool ok = false;
        std::wstring text;
        EnterCriticalSection(&resultCs_);
        ok = resultOk_;
        text = resultMsg_;
        LeaveCriticalSection(&resultCs_);

        ScheduleNext(ok);
        UpdateTrayTip();

        if (cfg_.showTips) {
            ShowTip(ok ? L"壁纸已更换" : L"更换失败", text, ok ? NIIF_INFO : NIIF_WARNING);
        }
        LogMsg(L"切换结果: %s - %s", ok ? L"成功" : L"失败", text.c_str());
        if (ok) PrefetchNext();
        return 0;
    }
    case WM_APP_OPENMENU: {
        POINT pt{};
        GetCursorPos(&pt);
        ShowMenu(pt);
        return 0;
    }
    case WM_APP + 4: {
        if (ui_) ui_->Show();
        return 0;
    }
    case WM_TIMER: {
        if (wp == TIMER_TICK) {
            if (!busy_ && GetTickCount64() >= nextChangeTick_) {
                StartChange(false);
            }
        }
        return 0;
    }
    case WM_COMMAND: {
        OnCommand(LOWORD(wp));
        return 0;
    }
    case WM_DESTROY: {
        PostQuitMessage(0);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// ── 托盘 ────────────────────────────────────────────────────────────────

void App::AddTrayIcon() {
    nid_ = NOTIFYICONDATAW{};
    nid_.cbSize = sizeof(NOTIFYICONDATAW);
    nid_.hWnd = hwnd_;
    nid_.uID = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = WM_APP_TRAY;
    nid_.hIcon = icon_;
    wcsncpy_s(nid_.szTip, L"WallpaperFeed 壁纸轮换", _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &nid_);
}

void App::RemoveTrayIcon() {
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    Shell_NotifyIconW(NIM_DELETE, &nid_);
}

void App::UpdateTrayTip() {
    std::wstring tip = L"WallpaperFeed · " + CurrentModeLabel();
    if (cfg_.intervalMinutes > 0) {
        wchar_t buf[64];
        swprintf_s(buf, L" · %d 分钟", cfg_.intervalMinutes);
        tip += buf;
    } else {
        tip += L" · 不定时";
    }
    if (lastScreenCount_ > 0) {
        wchar_t buf[48];
        swprintf_s(buf, L" · %d 屏", lastScreenCount_);
        tip += buf;
    }

    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    wcsncpy_s(nid_.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

void App::ShowTip(const std::wstring& title, const std::wstring& text, DWORD flags) {
    NOTIFYICONDATAW nid = nid_;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = flags;
    nid.uTimeout = 5000;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// ── 菜单 ────────────────────────────────────────────────────────────────

std::wstring App::CurrentModeLabel() const {
    if (cfg_.mode == L"span") return L"跨屏合一";
    if (cfg_.mode == L"clone") return L"复制同一张";
    return L"每屏不同";
}

void App::ShowMenu(POINT pt) {
    HMENU menu = CreatePopupMenu();
    HMENU modeMenu = CreatePopupMenu();
    HMENU intMenu = CreatePopupMenu();
    HMENU srcMenu = CreatePopupMenu();
    HMENU catMenu = CreatePopupMenu();
    HMENU resMenu = CreatePopupMenu();

    AppendMenuW(menu, MF_STRING, ID_M_OPEN_UI, L"打开主界面");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // 头部状态
    std::wstring header = L"来源：" + cfg_.SourceLabel();
    AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, header.c_str());

    if (lastScreenCount_ > 0) {
        wchar_t buf[64];
        swprintf_s(buf, L"显示器：%d 台", lastScreenCount_);
        AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, buf);
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_M_NEXT, busy_ ? L"下一张壁纸（正在切换…）" : L"下一张壁纸");

    // 收藏当前桌面壁纸：按实际每屏记录决定菜单项（跨屏合一/复制时只有一张，就一项）
    {
        std::vector<WfItem> cur;
        {
            std::lock_guard<std::mutex> lk(curMu_);
            cur = curItems_;
        }
        std::vector<int> slots;      // 有记录的屏索引
        bool allSame = true;
        int firstId = 0;
        for (size_t i = 0; i < cur.size(); ++i) {
            if (cur[i].id <= 0) continue;
            slots.push_back((int)i);
            if (firstId == 0) firstId = cur[i].id;
            else if (cur[i].id != firstId) allSame = false;
        }
        if (slots.empty()) {
            AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED,
                        0, L"收藏当前壁纸（本次运行还没换过壁纸）");
        } else if (slots.size() == 1 || allSame) {
            const WfItem& it = cur[slots[0]];
            bool faved = lib_ && lib_->IsFavorite(it.id);
            std::wstring t = Utf8ToWide(it.title);
            if (t.size() > 20) t = t.substr(0, 20) + L"…";
            wchar_t buf[256];
            if (slots.size() > 1) {
                swprintf_s(buf, L"收藏当前壁纸（%d 屏同一张）：%ls", (int)slots.size(), t.c_str());
            } else if (cfg_.mode == L"span" && lastScreenCount_ > 1) {
                swprintf_s(buf, L"收藏当前壁纸（跨屏合一整张）：%ls", t.c_str());
            } else {
                swprintf_s(buf, L"收藏当前壁纸：%ls", t.c_str());
            }
            AppendMenuW(menu, MF_STRING | (faved ? MF_CHECKED : 0),
                        ID_M_FAV_BASE + (UINT)slots[0], buf);
        } else {
            for (int slot : slots) {
                bool faved = lib_ && lib_->IsFavorite(cur[slot].id);
                std::wstring t = Utf8ToWide(cur[slot].title);
                if (t.size() > 16) t = t.substr(0, 16) + L"…";
                wchar_t buf[256];
                swprintf_s(buf, L"收藏桌面 %d 壁纸：%ls", slot + 1, t.c_str());
                AppendMenuW(menu, MF_STRING | (faved ? MF_CHECKED : 0),
                            ID_M_FAV_BASE + (UINT)slot, buf);
            }
        }
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // 模式
    UINT fDiff = MF_STRING | (cfg_.mode == L"different" ? MF_CHECKED : 0);
    UINT fSpan = MF_STRING | (cfg_.mode == L"span" ? MF_CHECKED : 0);
    UINT fClone = MF_STRING | (cfg_.mode == L"clone" ? MF_CHECKED : 0);
    AppendMenuW(modeMenu, fDiff, ID_M_MODE_DIFF, L"每屏不同壁纸");
    AppendMenuW(modeMenu, fSpan, ID_M_MODE_SPAN, L"跨屏合一（一张图铺满）");
    AppendMenuW(modeMenu, fClone, ID_M_MODE_CLONE, L"复制到所有屏");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)modeMenu, L"显示模式");

    // 定时
    int cur = cfg_.intervalMinutes;
    AppendMenuW(intMenu, MF_STRING | (cur == 0 ? MF_CHECKED : 0),
                ID_M_INT_OFF, L"关闭定时切换");
    AppendMenuW(intMenu, MF_SEPARATOR, 0, nullptr);
    for (int i = 0; i < (int)(sizeof(kIntervals) / sizeof(kIntervals[0])); ++i) {
        UINT flags = MF_STRING | (cur == kIntervals[i] ? MF_CHECKED : 0);
        AppendMenuW(intMenu, flags, ID_M_INT_BASE + i, kIntervalLabels[i]);
    }
    wchar_t intTitle[64];
    if (cur > 0) swprintf_s(intTitle, L"定时切换（%d 分钟）", cur);
    else wcscpy_s(intTitle, L"定时切换（已关闭）");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)intMenu, intTitle);

    // 来源
    bool noRes = cfg_.resolution.empty();
    AppendMenuW(srcMenu, MF_STRING | ((noRes && cfg_.source == L"trending") ? MF_CHECKED : 0),
                ID_M_SRC_TRENDING, L"本周趋势");
    AppendMenuW(srcMenu, MF_STRING | ((noRes && cfg_.source == L"new") ? MF_CHECKED : 0),
                ID_M_SRC_NEW, L"最新上传");
    AppendMenuW(srcMenu, MF_STRING | ((noRes && cfg_.source == L"popular") ? MF_CHECKED : 0),
                ID_M_SRC_POPULAR, L"最高下载");

    const auto& cats = GetCategories();
    for (size_t i = 0; i < cats.size(); ++i) {
        UINT flags = MF_STRING |
                     ((noRes && cfg_.source == L"category" && cfg_.categorySlug == cats[i].slug)
                          ? MF_CHECKED : 0);
        AppendMenuW(catMenu, flags, ID_M_CAT_BASE + (UINT)i, cats[i].label);
    }
    AppendMenuW(srcMenu, MF_POPUP, (UINT_PTR)catMenu, L"按分类挑选");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)srcMenu, L"壁纸来源");

    // 图片规格（站点按屏幕尺寸分类 /theme/<slug>）
    AppendMenuW(resMenu, MF_STRING | (cfg_.resolution.empty() ? MF_CHECKED : 0),
                ID_M_RES_BASE, L"不限（按内容分类）");
    AppendMenuW(resMenu, MF_SEPARATOR, 0, nullptr);
    const auto& resList = GetResolutions();
    for (size_t i = 0; i < resList.size(); ++i) {
        wchar_t buf[96];
        swprintf_s(buf, L"%ls  %dx%d", resList[i].label, resList[i].w, resList[i].h);
        UINT flags = MF_STRING | ((cfg_.resolution == resList[i].slug) ? MF_CHECKED : 0);
        AppendMenuW(resMenu, flags, ID_M_RES_BASE + 1 + (UINT)i, buf);
    }
    wchar_t resTitle[96];
    if (cfg_.resolution.empty()) wcscpy_s(resTitle, L"图片规格（不限）");
    else swprintf_s(resTitle, L"图片规格（%ls）", cfg_.ResolutionLabel().c_str());
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)resMenu, resTitle);

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (cfg_.autostart ? MF_CHECKED : 0), ID_M_AUTOSTART, L"开机自动启动");
    AppendMenuW(menu, MF_STRING, ID_M_OPEN_CACHE, L"打开壁纸缓存目录");
    AppendMenuW(menu, MF_STRING, ID_M_OPEN_CONFIG, L"打开配置文件");
    AppendMenuW(menu, MF_STRING, ID_M_ABOUT, L"关于");
    AppendMenuW(menu, MF_STRING, ID_M_EXIT, L"退出");

    SetForegroundWindow(hwnd_);
    TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                     pt.x, pt.y, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);

    DestroyMenu(menu);
}

void App::OnCommand(UINT id) {
    if (id == ID_M_OPEN_UI) {
        if (ui_) ui_->Show();
        return;
    }
    if (id == ID_M_NEXT) {
        StartChange(true);
        return;
    }
    if (id >= ID_M_FAV_BASE && id < ID_M_FAV_BASE + 8) {
        int idx = (int)(id - ID_M_FAV_BASE);
        WfItem item;
        bool ok = false;
        {
            std::lock_guard<std::mutex> lk(curMu_);
            if (idx >= 0 && idx < (int)curItems_.size() && curItems_[idx].id > 0) {
                item = curItems_[idx];
                ok = true;
            }
        }
        if (ok) FavoriteCurrent(item);
        return;
    }
    if (id >= ID_M_MODE_DIFF && id <= ID_M_MODE_CLONE) {
        if (id == ID_M_MODE_DIFF) cfg_.mode = L"different";
        else if (id == ID_M_MODE_SPAN) cfg_.mode = L"span";
        else cfg_.mode = L"clone";
        cfg_.Save(configPath_);
        UpdateTrayTip();
        StartChange(true);
        return;
    }
    if (id == ID_M_INT_OFF) {
        cfg_.intervalMinutes = 0;
        cfg_.Save(configPath_);
        nextChangeTick_ = UINT64_MAX;
        UpdateTrayTip();
        ShowTip(L"已关闭定时切换", L"可使用托盘菜单手动换图。");
        return;
    }
    if (id >= ID_M_INT_BASE && id < ID_M_INT_BASE + (UINT)(sizeof(kIntervals) / sizeof(kIntervals[0]))) {
        int idx = (int)(id - ID_M_INT_BASE);
        cfg_.intervalMinutes = kIntervals[idx];
        cfg_.Save(configPath_);
        ScheduleNext(true);
        UpdateTrayTip();
        wchar_t buf[128];
        swprintf_s(buf, L"每 %d 分钟自动更换壁纸", cfg_.intervalMinutes);
        ShowTip(L"定时切换已更新", buf);
        return;
    }
    if (id >= ID_M_SRC_TRENDING && id <= ID_M_SRC_POPULAR) {
        if (id == ID_M_SRC_TRENDING) cfg_.source = L"trending";
        else if (id == ID_M_SRC_NEW) cfg_.source = L"new";
        else cfg_.source = L"popular";
        cfg_.resolution.clear();
        cfg_.Save(configPath_);
        UpdateTrayTip();
        {
            std::lock_guard<std::mutex> lk(cacheMutex_);
            cachedUrl_.clear();
        }
        StartChange(true);
        return;
    }
    if (id >= ID_M_RES_BASE && id < ID_M_RES_BASE + 32) {
        const auto& res = GetResolutions();
        size_t idx = id - ID_M_RES_BASE;   // 0 = 不限
        cfg_.resolution = (idx >= 1 && idx <= res.size()) ? res[idx - 1].slug : L"";
        cfg_.Save(configPath_);
        UpdateTrayTip();
        {
            std::lock_guard<std::mutex> lk(cacheMutex_);
            cachedUrl_.clear();
        }
        if (ui_) ui_->OnResolutionChanged();
        StartChange(true);
        if (cfg_.resolution.empty()) {
            ShowTip(L"图片规格：不限", L"按当前壁纸来源更换。");
        } else {
            wchar_t buf[128];
            swprintf_s(buf, L"按 %ls 规格下载并更换（%d×%d）",
                       cfg_.ResolutionLabel().c_str(), res[idx - 1].w, res[idx - 1].h);
            ShowTip(L"图片规格已切换", buf);
        }
        return;
    }
    if (id >= ID_M_CAT_BASE && id < ID_M_RES_BASE) {
        const auto& cats = GetCategories();
        size_t idx = id - ID_M_CAT_BASE;
        if (idx < cats.size()) {
            cfg_.source = L"category";
            cfg_.categorySlug = cats[idx].slug;
            cfg_.resolution.clear();   // 站点不支持分类 + 尺寸叠加
            cfg_.Save(configPath_);
            UpdateTrayTip();
            {
                std::lock_guard<std::mutex> lk(cacheMutex_);
                cachedUrl_.clear();
            }
            StartChange(true);
            return;
        }
    }
    if (id == ID_M_AUTOSTART) {
        cfg_.autostart = !cfg_.autostart;
        autostart::SetEnabled(cfg_.autostart);
        cfg_.Save(configPath_);
        ShowTip(cfg_.autostart ? L"已开启开机启动" : L"已关闭开机启动",
                cfg_.autostart ? L"登录 Windows 后会自动在后台运行。" : L"下次开机不再自动运行。");
        return;
    }
    if (id == ID_M_OPEN_CACHE) {
        ShellExecuteW(nullptr, L"open", cacheDir_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (id == ID_M_OPEN_CONFIG) {
        if (FileExists(configPath_)) {
            ShellExecuteW(nullptr, L"open", configPath_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else {
            ShowTip(L"配置文件不存在", configPath_);
        }
        return;
    }
    if (id == ID_M_ABOUT) {
        wchar_t buf[1024];
        swprintf_s(buf,
                   L"WallpaperFeed 壁纸工具 v%ls\n\n"
                   L"壁纸源：https://wallpaperfeed.com\n"
                   L"当前来源：%ls\n"
                   L"显示模式：%ls\n"
                   L"定时切换：%d 分钟（0 表示关闭）\n"
                   L"显示器：%d 台\n"
                   L"缓存目录：%ls\n\n"
                   L"左键单击图标＝立即换一张，右键＝打开菜单。",
                   kVersion, cfg_.SourceLabel().c_str(), CurrentModeLabel().c_str(),
                   cfg_.intervalMinutes, lastScreenCount_, cacheDir_.c_str());
        MessageBoxW(hwnd_, buf, L"关于 WallpaperFeed", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (id == ID_M_EXIT) {
        shuttingDown_ = true;
        RemoveTrayIcon();
        DestroyWindow(hwnd_);
        return;
    }
}

// ── 切换流程 ────────────────────────────────────────────────────────────

void App::StartChange(bool manual) {
    if (busy_) {
        if (manual) ShowTip(L"正在切换", L"上一张还在处理，请稍候。");
        return;
    }
    if (shuttingDown_) return;

    if (worker_.joinable()) worker_.join();

    busy_ = true;
    Config snapshot = cfg_;

    worker_ = std::thread([this, snapshot]() {
        // 换壁纸全程用「低于正常」优先级，下载/解码/JPEG 编码不抢前台程序的 CPU
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::wstring msg;
        bool ok = PerformChange(snapshot, msg);
        CoUninitialize();

        EnterCriticalSection(&resultCs_);
        resultOk_ = ok;
        resultMsg_ = msg;
        LeaveCriticalSection(&resultCs_);

        PostMessageW(hwnd_, WM_APP_CHANGED, 0, 0);
    });
}

int App::RequestWidth(const WfItem& it, int needW, int needH, int capWidth, int& reqH) {
    int ow = it.w > 0 ? it.w : 3840;
    int oh = it.h > 0 ? it.h : 2160;

    double cover = std::max((double)needW / ow, (double)needH / oh);
    if (cover <= 0.0) cover = 1.0;

    double s = std::min(1.0, cover * 1.15);
    if ((double)ow * s > (double)capWidth) s = (double)capWidth / ow;
    if (s > 1.0) s = 1.0;
    if (s < cover) s = std::min(1.0, cover);
    if (s <= 0.0) s = 1.0;

    int w = (int)std::lround(ow * s);
    int h = (int)std::lround(oh * s);
    if (w < 640) {
        double k = 640.0 / (double)w;
        w = 640;
        h = (int)std::lround(h * k);
    }
    if (h < 360) {
        double k = 360.0 / (double)h;
        h = 360;
        w = (int)std::lround(w * k);
    }
    reqH = h;
    return w;
}

bool App::GetItems(const Config& cfg, std::vector<WfItem>& items, std::wstring& err) {
    std::wstring url = cfg.SourceUrl();

    {
        std::lock_guard<std::mutex> lk(cacheMutex_);
        if (!cachedItems_.empty() && cachedUrl_ == url &&
            GetTickCount64() - cachedTick_ < 15ull * 60ull * 1000ull) {
            items = cachedItems_;
            return true;
        }
    }

    std::vector<WfItem> fresh;
    if (!WallpaperFeed::GetListing(url, fresh, err)) {
        std::lock_guard<std::mutex> lk(cacheMutex_);
        if (!cachedItems_.empty() && cachedUrl_ == url) {
            items = cachedItems_;
            err.clear();
            LogMsg(L"列表获取失败，使用缓存");
            return true;
        }
        // 站点列表接口挂了也不能停摆：改用上次成功保存到本地的图片池
        if (cachedItems_.empty()) {
            std::vector<WfItem> saved;
            if (LoadPoolFile(PathJoin(dataDir_, L"pool.txt"), saved)) cachedItems_.swap(saved);
        }
        if (!cachedItems_.empty()) {
            items = cachedItems_;
            err.clear();
            LogMsg(L"列表获取失败，改用本地图片池（%d 张）", (int)items.size());
            return true;
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(cacheMutex_);
        cachedItems_ = fresh;
        cachedUrl_ = url;
        cachedTick_ = GetTickCount64();
    }
    // 存一份到磁盘，下次站点抖动时不用干等
    SavePoolFile(PathJoin(dataDir_, L"pool.txt"), fresh);
    items = fresh;
    return true;
}

void App::BuildCandidates(const std::vector<WfItem>& pool, size_t want,
                          std::vector<WfItem>& out) {
    out.clear();
    if (pool.empty()) return;

    std::vector<WfItem> shuffled = pool;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::shuffle(shuffled.begin(), shuffled.end(), gen);

    // 先取没出现过的
    for (const auto& it : shuffled) {
        if (out.size() >= want) break;
        if (!recent_.Contains(it.id)) out.push_back(it);
    }
    // 不够的话从全部里补
    if (out.size() < want) {
        for (const auto& it : shuffled) {
            if (out.size() >= want) break;
            bool dup = false;
            for (const auto& o : out) {
                if (o.id == it.id) { dup = true; break; }
            }
            if (!dup) out.push_back(it);
        }
    }
}

bool App::PerformChange(const Config& cfg, std::wstring& resultMsg) {
    std::vector<ScreenInfo> screens;
    bool perMon = false;
    std::wstring err;
    if (!desk::EnumScreens(screens, perMon, err)) {
        resultMsg = L"检测显示器失败：" + err;
        return false;
    }
    lastScreenCount_ = (int)screens.size();

    RECT bbox = screens[0].rect;
    for (const auto& s : screens) {
        bbox.left = std::min<LONG>(bbox.left, s.rect.left);
        bbox.top = std::min<LONG>(bbox.top, s.rect.top);
        bbox.right = std::max<LONG>(bbox.right, s.rect.right);
        bbox.bottom = std::max<LONG>(bbox.bottom, s.rect.bottom);
    }
    int bboxW = (int)(bbox.right - bbox.left);
    int bboxH = (int)(bbox.bottom - bbox.top);

    bool spanMode = (cfg.mode == L"span");
    bool cloneMode = (cfg.mode == L"clone");
    size_t needCount = spanMode ? 1 : (cloneMode ? 1 : screens.size());
    if (needCount == 0) needCount = 1;

    int targetW = spanMode ? bboxW : 0;
    int targetH = spanMode ? bboxH : 0;
    if (!spanMode) {
        for (const auto& s : screens) {
            targetW = std::max(targetW, s.width);
            targetH = std::max(targetH, s.height);
        }
    }

    // 用户指定了图片规格：取「屏幕尺寸」和「规格尺寸」里更大的那个，
    // 保证既不浪费也不缩小（例如 4K 规格在 1080P 屏上仍按 3840×2160 取图）
    int capW = cfg.requestCapWidth;
    int specW = 0, specH = 0;
    if (cfg.ResolutionSize(specW, specH)) {
        targetW = std::max(targetW, specW);
        targetH = std::max(targetH, specH);
        if (capW < specW) capW = specW;
    }

    std::vector<WfItem> pool;
    if (!GetItems(cfg, pool, err)) {
        resultMsg = L"获取壁纸列表失败：" + err;
        return false;
    }

    std::vector<WfItem> candidates;
    BuildCandidates(pool, needCount * 3 + 8, candidates);
    if (candidates.empty()) {
        resultMsg = L"没有可用的壁纸";
        return false;
    }

    // 记下这次的取图尺寸，空闲时按同样的尺寸预取下一张
    pfTargetW_.store(targetW);
    pfTargetH_.store(targetH);
    pfCapW_.store(capW);

    // 已经在本地（含上一次预取好的）候选排到前面，命中就直接跳过网络下载
    {
        std::vector<WfItem> hot, cold;
        hot.reserve(candidates.size());
        cold.reserve(candidates.size());
        for (const auto& it : candidates) {
            int reqH = 0;
            int reqW = RequestWidth(it, targetW, targetH, capW, reqH);
            wchar_t name[160];
            swprintf_s(name, L"wf_%d_%dx%d.jpg", it.id, reqW, reqH);
            // 精确尺寸的本地文件优先；退一步，同 id 的其它尺寸本地文件也能直接用（重新裁切即可）
            if (FileExists(PathJoin(srcDir_, name)) || !FindCachedSrc(srcDir_, it.id).empty())
                hot.push_back(it);
            else cold.push_back(it);
        }
        hot.insert(hot.end(), cold.begin(), cold.end());
        candidates.swap(hot);
    }

    // 并发下载候选：多张图同时取，谁先下完先用；单张失败立刻换下一张，不再串行干等慢图
    std::vector<WfItem> chosen;
    std::vector<std::wstring> srcFiles;
    {
        std::atomic<size_t> nextIdx{0};
        std::mutex mu;
        const size_t need = needCount;
        int nThreads = (int)std::min<size_t>(3, std::max<size_t>(1,
                        std::min<size_t>(candidates.size(), needCount * 2)));
        auto worker = [&]() {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            while (true) {
                {
                    std::lock_guard<std::mutex> lk(mu);
                    if (chosen.size() >= need) return;
                }
                size_t i = nextIdx.fetch_add(1);
                if (i >= candidates.size()) return;
                WfItem it = candidates[i];

                int reqH = 0;
                int reqW = RequestWidth(it, targetW, targetH, capW, reqH);
                wchar_t name[160];
                swprintf_s(name, L"wf_%d_%dx%d.jpg", it.id, reqW, reqH);
                std::wstring srcFile = PathJoin(srcDir_, name);

                std::wstring useFile = srcFile;
                bool have = FileExists(srcFile);
                if (!have) {
                    // 同 id 的其它尺寸已经下过，直接拿来用，不再等网络
                    std::wstring alt = FindCachedSrc(srcDir_, it.id);
                    if (!alt.empty()) { have = true; useFile = alt; }
                }
                if (!have) {
                    std::wstring referer = WallpaperFeed::DetailReferer(it.href, it.id);
                    std::wstring derr;
                    have = WallpaperFeed::Download(it.id, referer, reqW, reqH, srcFile, derr);
                    if (!have) LogMsg(L"候选 %d 下载失败: %s", it.id, derr.c_str());
                    else useFile = srcFile;
                }
                if (!have) continue;

                std::lock_guard<std::mutex> lk(mu);
                if (chosen.size() >= need) return;
                chosen.push_back(it);
                srcFiles.push_back(useFile);
            }
        };
        std::vector<std::thread> ths;
        for (int t = 0; t < nThreads; ++t) ths.emplace_back(worker);
        for (auto& t : ths) t.join();
    }

    if (chosen.size() < needCount) {
        resultMsg = L"下载壁纸失败（已尝试多张候选）";
        return false;
    }

    // 渲染
    std::vector<std::wstring> outFiles(screens.size());
    if (spanMode) {
        std::vector<RECT> slices;
        for (const auto& s : screens) {
            RECT r{};
            r.left = s.rect.left - bbox.left;
            r.top = s.rect.top - bbox.top;
            r.right = r.left + s.width;
            r.bottom = r.top + s.height;
            slices.push_back(r);
        }
        for (size_t i = 0; i < screens.size(); ++i) {
            wchar_t name[180];
            swprintf_s(name, L"span_m%u_%d_%dx%d.jpg", (unsigned)i, chosen[0].id,
                       screens[i].width, screens[i].height);
            outFiles[i] = PathJoin(curDir_, name);
        }
        if (!gfx::RenderCoverCrop(srcFiles[0], bboxW, bboxH, slices, outFiles,
                                  cfg.jpegQuality, err)) {
            resultMsg = L"生成跨屏壁纸失败：" + err;
            return false;
        }
    } else {
        // 同一张源图可能要被裁成多屏（复制模式），按来源分组后一次解码、多屏输出
        std::map<size_t, std::vector<size_t>> bySrc;
        for (size_t i = 0; i < screens.size(); ++i) {
            size_t srcIdx = cloneMode ? 0 : (i < srcFiles.size() ? i : 0);
            bySrc[srcIdx].push_back(i);
        }
        for (const auto& kv : bySrc) {
            size_t srcIdx = kv.first;
            std::vector<std::pair<int, int>> targets;
            std::vector<std::wstring> dsts;
            for (size_t i : kv.second) {
                wchar_t name[180];
                swprintf_s(name, L"%s_m%u_%d_%dx%d.jpg", cloneMode ? L"clone" : L"diff",
                           (unsigned)i, chosen[srcIdx].id, screens[i].width, screens[i].height);
                outFiles[i] = PathJoin(curDir_, name);
                targets.push_back({screens[i].width, screens[i].height});
                dsts.push_back(outFiles[i]);
            }
            if (!gfx::RenderScreens(srcFiles[srcIdx], targets, dsts, cfg.jpegQuality, err)) {
                resultMsg = L"生成壁纸失败：" + err;
                return false;
            }
        }
    }

    // 应用
    if (!desk::ApplyPerScreen(screens, outFiles, err)) {
        resultMsg = L"设置壁纸失败：" + err;
        return false;
    }
    LogMsg(L"壁纸已应用（模式 %s，%d 屏）", cfg.mode.c_str(), (int)screens.size());

    {
        std::lock_guard<std::mutex> lk(cacheMutex_);
        currentFiles_ = outFiles;
    }

    // 记录「当前桌面上每屏对应哪张壁纸」，托盘菜单据此生成收藏项
    {
        std::vector<WfItem> perScreen;
        for (size_t i = 0; i < screens.size(); ++i) {
            size_t si = (spanMode || cloneMode) ? 0 : (i < chosen.size() ? i : 0);
            perScreen.push_back(chosen[si]);
        }
        std::lock_guard<std::mutex> lk(curMu_);
        curItems_.swap(perScreen);
        curMode_ = cfg.mode;
    }

    // 记录使用过的 id
    for (const auto& it : chosen) {
        recent_.Add(it.id);
    }
    recent_.Save();

    for (const auto& it : chosen) {
        std::string t = it.title;
        if (!t.empty()) {
            resultMsg = L"已更换：" + Utf8ToWide(t);
            break;
        }
    }
    if (resultMsg.empty()) {
        wchar_t buf[128];
        swprintf_s(buf, L"已更换 %d 张壁纸", (int)chosen.size());
        resultMsg = buf;
    }

    CleanupCache();
    CheckStorage(false);
    return true;
}

// 预取：趁空闲（屏幕上已经有壁纸、没有切换任务在跑）先把后面 2 张候选图下到 src\。
// 站点列表/下载接口经常要十几秒甚至更久，提前下好之后，到点换壁纸就只剩解码+编码，几乎瞬间完成。
void App::PrefetchNext() {
    if (shuttingDown_ || busy_ || !prefetchFlag_) return;
    if (prefetchFlag_->exchange(true)) return;

    Config snapshot = cfg_;
    std::wstring srcDir = srcDir_;
    int tw = pfTargetW_.load();
    int th = pfTargetH_.load();
    int cap = pfCapW_.load();
    if (tw <= 0 || th <= 0) {
        prefetchFlag_->store(false);
        return;
    }

    auto flag = prefetchFlag_;
    std::thread([this, snapshot, srcDir, tw, th, cap, flag]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        std::wstring err;
        std::vector<WfItem> pool;
        if (GetItems(snapshot, pool, err) && !pool.empty()) {
            std::vector<WfItem> cands = pool;
            std::random_device rdev;
            std::mt19937 gen(rdev());
            std::shuffle(cands.begin(), cands.end(), gen);
            if (cands.size() > 12) cands.resize(12);
            int done = 0;
            int tried = 0;
            for (const auto& it : cands) {
                if (done >= 2 || shuttingDown_ || busy_) break;
                int reqH = 0;
                int reqW = RequestWidth(it, tw, th, cap, reqH);
                wchar_t name[160];
                swprintf_s(name, L"wf_%d_%dx%d.jpg", it.id, reqW, reqH);
                std::wstring srcFile = PathJoin(srcDir, name);
                if (FileExists(srcFile)) continue;
                ++tried;
                std::wstring referer = WallpaperFeed::DetailReferer(it.href, it.id);
                std::wstring derr;
                if (WallpaperFeed::Download(it.id, referer, reqW, reqH, srcFile, derr)) {
                    ++done;
                    LogMsg(L"预取壁纸完成 id=%d %dx%d", it.id, reqW, reqH);
                } else {
                    LogMsg(L"预取壁纸失败 id=%d: %s", it.id, derr.c_str());
                }
            }
            LogMsg(L"预取结束：新增 %d 张，尝试 %d 张", done, tried);
        }
        flag->store(false);
    }).detach();
}

// 托盘菜单「收藏当前壁纸」：把正在桌面上的那张图加进收藏（写 favorites.txt + 后台下载高清备份）
void App::FavoriteCurrent(const WfItem& item) {
    if (!lib_) return;
    std::wstring title = Utf8ToWide(item.title);
    if (title.empty()) title = L"壁纸 #" + std::to_wstring(item.id);

    if (!lib_->AddFavorite(item)) {
        ShowTip(L"已经在收藏里了", title + L" 之前就已经收藏过。");
        return;
    }
    if (ui_) ui_->OnLibraryUpdated();
    ShowTip(L"已收藏当前壁纸", title + L"\n可在主界面「我的收藏」里查看，高清备份正在下载。");
}

// kind: 0 = 单屏，1 = 跨屏合一，2 = 复制到所有屏
void App::RecordUiApplied(const WfItem& item, int kind, int monitor, int screenCount) {
    if (item.id <= 0) return;
    int n = screenCount > 0 ? screenCount : (lastScreenCount_ > 0 ? lastScreenCount_ : 1);
    std::lock_guard<std::mutex> lk(curMu_);
    if ((int)curItems_.size() != n) curItems_.assign(n, WfItem());
    if (kind == 0) {
        if (monitor < 0) monitor = 0;
        if (monitor >= n) monitor = n - 1;
        curItems_[monitor] = item;
    } else {
        for (auto& c : curItems_) c = item;
    }
    curMode_ = (kind == 1) ? L"span" : (kind == 2 ? L"clone" : L"different");
}

void App::ScheduleNext(bool ok) {
    uint64_t now = GetTickCount64();
    lastChangeTick_ = now;

    if (!ok) {
        nextChangeTick_ = now + 180000ull; // 失败 3 分钟后重试
        return;
    }
    if (cfg_.intervalMinutes > 0) {
        nextChangeTick_ = now + (uint64_t)cfg_.intervalMinutes * 60000ull;
    } else {
        nextChangeTick_ = UINT64_MAX;
    }
}

void App::CleanupCache() {
    std::vector<std::wstring> protect;
    {
        std::lock_guard<std::mutex> lk(cacheMutex_);
        protect = currentFiles_;
    }
    int keep = cfg_.keepFiles > 0 ? cfg_.keepFiles : 40;
    CleanupDir(curDir_, keep, protect);
    CleanupDir(srcDir_, keep * 2, {});
}

uint64_t App::CacheBytes() const {
    if (!cacheStat_) {
        return DirSize(cacheDir_) + DirSize(PathJoin(dataDir_, L"thumbs"));
    }
    const uint64_t now = GetTickCount64();
    const uint64_t last = cacheStat_->tick.load();
    // 首次或超过 5 秒没有更新，就让后台线程重新统计，界面只读上一次的结果
    if ((last == 0 || now - last > 5000) && !cacheStat_->running.exchange(true)) {
        auto st = cacheStat_;
        std::thread([st]() {
            st->bytes = DirSize(st->dirs[0]) + DirSize(st->dirs[1]);
            st->tick = GetTickCount64();
            st->running = false;
        }).detach();
    }
    if (last == 0) {
        // 首次调用还没算完，先同步算一次（只在启动时发生）
        uint64_t v = DirSize(cacheStat_->dirs[0]) + DirSize(cacheStat_->dirs[1]);
        cacheStat_->bytes = v;
        cacheStat_->tick = GetTickCount64();
        return v;
    }
    return cacheStat_->bytes.load();
}

bool App::CheckStorage(bool force) {
    uint64_t now = GetTickCount64();
    if (!force && now - lastStorageCheck_ < 10ull * 60ull * 1000ull) return false;
    lastStorageCheck_ = now;

    std::wstring thumbsDir = PathJoin(dataDir_, L"thumbs");
    std::vector<StoreFile> all;
    CollectStoreFiles(cacheDir_, all);
    CollectStoreFiles(thumbsDir, all);

    uint64_t total = 0;
    for (const auto& f : all) total += f.size;

    std::vector<std::wstring> protect;
    {
        std::lock_guard<std::mutex> lk(cacheMutex_);
        protect = currentFiles_;
    }

    bool cleaned = false;
    const uint64_t limit = (uint64_t)cfg_.maxCacheMb * 1024ull * 1024ull;
    const bool inFav = true;

    // 1) 清理超过保留天数的文件（缩略图体积小，不按天数删）
    if (cfg_.autoClean && cfg_.keepDays > 0) {
        FILETIME nowFt{};
        GetSystemTimeAsFileTime(&nowFt);
        uint64_t nowT = ((uint64_t)nowFt.dwHighDateTime << 32) | nowFt.dwLowDateTime;
        uint64_t cutoff = nowT - (uint64_t)cfg_.keepDays * 24ull * 3600ull * 10000000ull;
        for (auto& f : all) {
            if (f.size == 0 || f.mtime >= cutoff) continue;
            if (f.path.find(thumbsDir) == 0) continue;
            if (inFav && cfg_.keepFavorites && f.path.find(L"\\fav\\") != std::wstring::npos) continue;
            if (IsFullPathEqual(protect, f.path)) continue;
            if (DeleteFileW(f.path.c_str())) {
                total -= f.size;
                f.size = 0;
                cleaned = true;
            }
        }
    }

    // 2) 超过体积上限时，按时间从旧到新删
    if (cfg_.autoClean && limit > 0 && total > limit) {
        std::vector<StoreFile*> cand;
        for (auto& f : all) {
            if (f.size == 0) continue;
            if (cfg_.keepFavorites && f.path.find(L"\\fav\\") != std::wstring::npos) continue;
            if (IsFullPathEqual(protect, f.path)) continue;
            cand.push_back(&f);
        }
        std::sort(cand.begin(), cand.end(),
                  [](const StoreFile* a, const StoreFile* b) { return a->mtime < b->mtime; });
        for (StoreFile* f : cand) {
            if (total <= limit * 9 / 10) break;
            if (DeleteFileW(f->path.c_str())) {
                total -= f->size;
                cleaned = true;
            }
        }
    }

    if (cleaned) LogMsg(L"存储清理完成，当前占用 %s", FormatBytes(total).c_str());

    // 3) 超限提醒 / 清理提示（低频，避免打扰）
    if (limit > 0 && total > limit) {
        if (now - lastStorageWarn_ > 60ull * 60ull * 1000ull) {
            lastStorageWarn_ = now;
            ShowTip(L"存储空间提醒",
                    L"壁纸数据已超过上限 " + FormatBytes(limit) + L"，已自动清理较早的文件，当前占用 " +
                        FormatBytes(total),
                    NIIF_WARNING);
        }
    } else if (cleaned && now - lastStorageWarn_ > 60ull * 60ull * 1000ull) {
        lastStorageWarn_ = now;
        ShowTip(L"本地存储", L"已自动清理旧壁纸，当前占用 " + FormatBytes(total), NIIF_INFO);
    }
    return cleaned;
}
