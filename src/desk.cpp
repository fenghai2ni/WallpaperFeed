#include "desk.h"

#include <shobjidl.h>
#include <objbase.h>
#include <vector>
#include <algorithm>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kRunValue = L"WallpaperFeed";

struct WallpaperApi {
    IDesktopWallpaper* p = nullptr;
    WallpaperApi() {
        CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL,
                         IID_IDesktopWallpaper, (void**)&p);
    }
    ~WallpaperApi() {
        if (p) p->Release();
    }
    bool ok() const { return p != nullptr; }
};

} // namespace

namespace desk {

bool EnumScreens(std::vector<ScreenInfo>& screens, bool& perMonitorCapable, std::wstring& err) {
    screens.clear();
    perMonitorCapable = false;

    WallpaperApi api;
    if (api.ok()) {
        UINT count = 0;
        if (SUCCEEDED(api.p->GetMonitorDevicePathCount(&count)) && count > 0) {
            bool allRects = true;
            for (UINT i = 0; i < count; ++i) {
                LPWSTR monPath = nullptr;
                if (FAILED(api.p->GetMonitorDevicePathAt(i, &monPath)) || !monPath) {
                    allRects = false;
                    break;
                }
                ScreenInfo si;
                si.id = monPath;
                CoTaskMemFree(monPath);

                RECT r{};
                if (SUCCEEDED(api.p->GetMonitorRECT(si.id.c_str(), &r)) &&
                    (r.right - r.left) > 0 && (r.bottom - r.top) > 0) {
                    si.rect = r;
                    si.width = r.right - r.left;
                    si.height = r.bottom - r.top;
                } else {
                    allRects = false;
                    break;
                }
                screens.push_back(si);
            }
            if (allRects && !screens.empty()) {
                perMonitorCapable = true;
                LogMsg(L"IDesktopWallpaper 枚举到 %d 台显示器", (int)screens.size());
                return true;
            }
            screens.clear();
        }
    }

    // 回退：GDI 枚举（无法逐屏设置）
    struct CollectCtx {
        std::vector<ScreenInfo>* out;
    };

    static auto collect = [](HMONITOR, HDC, LPRECT lprc, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<CollectCtx*>(lp);
        ScreenInfo si;
        si.rect = *lprc;
        si.width = lprc->right - lprc->left;
        si.height = lprc->bottom - lprc->top;
        c->out->push_back(si);
        return TRUE;
    };

    CollectCtx ctx{&screens};
    EnumDisplayMonitors(nullptr, nullptr, collect, (LPARAM)&ctx);

    if (screens.empty()) {
        err = L"未检测到任何显示器";
        return false;
    }
    LogMsg(L"回退 GDI 枚举到 %d 台显示器（不支持逐屏设置）", (int)screens.size());
    return true;
}

bool ApplyPerScreen(const std::vector<ScreenInfo>& screens,
                    const std::vector<std::wstring>& files, std::wstring& err) {
    if (screens.empty() || files.empty()) {
        err = L"参数为空";
        return false;
    }

    // 只有一台屏幕（含系统处于「复制这些显示器」的镜像模式）时，直接设整桌面
    if (screens.size() == 1) return ApplySingle(files[0], err);

    // 有 id 时逐屏设置
    bool hasId = true;
    for (const auto& s : screens) {
        if (s.id.empty()) { hasId = false; break; }
    }

    WallpaperApi api;
    if (hasId && api.ok()) {
        api.p->SetPosition(DWPOS_FILL);
        for (size_t i = 0; i < screens.size(); ++i) {
            const std::wstring& f = files[i < files.size() ? i : 0];
            HRESULT hr = api.p->SetWallpaper(screens[i].id.c_str(), f.c_str());
            if (FAILED(hr)) {
                wchar_t msg[160];
                swprintf_s(msg, L"设置第 %d 台显示器壁纸失败 (0x%08X)", (int)(i + 1), (unsigned)hr);
                LogMsg(L"%s", msg);
                err = msg;
                // 尝试整体回退
                return ApplySingle(files[0], err) ? true : false;
            }
        }
        LogMsg(L"已为 %d 台显示器分别设置壁纸", (int)screens.size());
        return true;
    }

    return ApplySingle(files[0], err);
}

bool ApplyOne(const std::vector<ScreenInfo>& screens, int index,
              const std::wstring& file, std::wstring& err) {
    // 索引越界或拿不到显示器 id（镜像模式／回退 GDI 枚举）时，直接铺满整个桌面
    if (index < 0 || index >= (int)screens.size() || screens[index].id.empty()) {
        LogMsg(L"显示器 %d 不可单独设置，改为设置整个桌面", index + 1);
        return ApplySingle(file, err);
    }
    WallpaperApi api;
    if (api.ok()) {
        api.p->SetPosition(DWPOS_FILL);
        HRESULT hr = api.p->SetWallpaper(screens[index].id.c_str(), file.c_str());
        if (SUCCEEDED(hr)) {
            LogMsg(L"已设置第 %d 台显示器壁纸", index + 1);
            return true;
        }
        LogMsg(L"SetWallpaper(单屏) 失败 0x%08X", (unsigned)hr);
    }
    err = L"不支持逐屏设置，已改为设置整个桌面";
    return ApplySingle(file, err);
}

bool ApplySingle(const std::wstring& file, std::wstring& err) {
    // 通过 IDesktopWallpaper 设置全部
    WallpaperApi api;
    if (api.ok()) {
        api.p->SetPosition(DWPOS_FILL);
        if (SUCCEEDED(api.p->SetWallpaper(nullptr, file.c_str()))) {
            LogMsg(L"已设置整桌面壁纸: %s", file.c_str());
            return true;
        }
    }
    if (!SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (PVOID)file.c_str(),
                               SPIF_UPDATEINIFILE | SPIF_SENDWININICHANGE)) {
        err = L"系统拒绝设置壁纸";
        LogMsg(L"SystemParametersInfo 设置壁纸失败: %lu", GetLastError());
        return false;
    }
    LogMsg(L"已通过 SPI 设置整桌面壁纸");
    return true;
}

} // namespace desk

namespace autostart {

bool IsEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t value[1024] = {};
    DWORD size = sizeof(value);
    DWORD type = 0;
    bool exists = RegQueryValueExW(key, kRunValue, nullptr, &type, (LPBYTE)value, &size) == ERROR_SUCCESS;
    RegCloseKey(key);
    return exists;
}

bool SetEnabled(bool enabled) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        LogMsg(L"打开开机启动注册表项失败");
        return false;
    }

    bool ok = true;
    if (enabled) {
        std::wstring exe = GetExePath();
        std::wstring cmd = L"\"" + exe + L"\" --autostart";
        ok = RegSetValueExW(key, kRunValue, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                            (DWORD)((cmd.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
        LogMsg(L"已开启开机启动: %s", cmd.c_str());
    } else {
        if (RegDeleteValueW(key, kRunValue) != ERROR_SUCCESS &&
            GetLastError() != ERROR_FILE_NOT_FOUND) {
            ok = false;
        }
        LogMsg(L"已关闭开机启动");
    }
    RegCloseKey(key);
    return ok;
}

} // namespace autostart
