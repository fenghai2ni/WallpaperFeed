// 截图小工具：shot.exe <输出png> [窗口标题]
#include <windows.h>
#include <gdiplus.h>
#include <stdio.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

static int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    Gdiplus::ImageCodecInfo* pInfo = (Gdiplus::ImageCodecInfo*)malloc(size);
    if (!pInfo) return -1;
    Gdiplus::GetImageEncoders(num, size, pInfo);
    int found = -1;
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(pInfo[i].MimeType, format) == 0) {
            *pClsid = pInfo[i].Clsid;
            found = (int)i;
            break;
        }
    }
    free(pInfo);
    return found;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { wprintf(L"usage: shot <out.png> [window title]\n"); return 1; }

    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token, &gsi, NULL);

    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    HWND target = NULL;

    if (argc >= 6) {
        x = wcstol(argv[2], nullptr, 10);
        y = wcstol(argv[3], nullptr, 10);
        w = wcstol(argv[4], nullptr, 10);
        h = wcstol(argv[5], nullptr, 10);
        target = NULL;
    } else if (argc >= 3) {
        target = FindWindowW(NULL, argv[2]);
        if (!target) { wprintf(L"window not found: %s\n", argv[2]); return 2; }
        RECT r;
        GetWindowRect(target, &r);
        x = r.left; y = r.top;
        w = r.right - r.left;
        h = r.bottom - r.top;
    }

    HDC screen = GetDC(NULL);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);

    if (target) {
        PrintWindow(target, mem, PW_RENDERFULLCONTENT);
    } else {
        BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY);
    }

    {
        Gdiplus::Bitmap out(bmp, NULL);
        CLSID clsid;
        if (GetEncoderClsid(L"image/png", &clsid) >= 0) {
            out.Save(argv[1], &clsid, NULL);
        }
    }

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
    Gdiplus::GdiplusShutdown(token);
    wprintf(L"saved %s (%dx%d)\n", argv[1], w, h);
    return 0;
}
