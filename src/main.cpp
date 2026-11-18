#include "app.h"
#include "resource.h"

#include <windows.h>
#include <objbase.h>

namespace {

void EnableDpiAwareness() {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        using SetCtxFn = BOOL(WINAPI*)(HANDLE);
        auto setCtx = reinterpret_cast<SetCtxFn>(
            GetProcAddress(u, "SetProcessDpiAwarenessContext"));
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
        if (setCtx && setCtx((HANDLE)(-4))) return;

        using SetAwareFn = HRESULT(WINAPI*)(int);
        auto setAware = reinterpret_cast<SetAwareFn>(
            GetProcAddress(u, "SetProcessDpiAwareness"));
        if (setAware && setAware(2) == S_OK) return; // PER_MONITOR_AWARE
    }
    SetProcessDPIAware();
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR lpCmdLine, int) {
    EnableDpiAwareness();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\WallpaperFeedSingleton_v1");
    bool openUi = lpCmdLine && wcsstr(lpCmdLine, L"--ui") != nullptr;
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        // 已有实例在运行
        HWND existing = FindWindowW(L"WallpaperFeedTrayWnd", nullptr);
        if (existing) PostMessageW(existing, openUi ? (WM_APP + 4) : (WM_APP + 3), 0, 0);
        CloseHandle(mutex);
        CoUninitialize();
        return 0;
    }

    bool launchedAtStartup = lpCmdLine && wcsstr(lpCmdLine, L"--autostart") != nullptr;

    int ret = 1;
    {
        App app(hInst);
        if (app.Init(launchedAtStartup)) {
            if (openUi) app.OpenMainWindow();
            app.Run();
            ret = 0;
        }
    }

    if (mutex) CloseHandle(mutex);
    CoUninitialize();
    return ret;
}
