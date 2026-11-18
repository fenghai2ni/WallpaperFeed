// 生成应用图标：用 GDI+ 绘制后打包为多尺寸 PNG-ICO
#include <windows.h>
#include <gdiplus.h>
#include <cstdio>
#include <vector>
#include <cmath>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")

using namespace Gdiplus;

static int GetEncoderClsid(const WCHAR* mime, CLSID* clsid) {
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (!size) return -1;
    std::vector<BYTE> buf(size);
    auto* info = reinterpret_cast<ImageCodecInfo*>(buf.data());
    GetImageEncoders(num, size, info);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(info[i].MimeType, mime) == 0) {
            *clsid = info[i].Clsid;
            return (int)i;
        }
    }
    return -1;
}

static bool SavePng(Bitmap& bmp, const wchar_t* tempPath, std::vector<BYTE>& out) {
    CLSID png{};
    if (GetEncoderClsid(L"image/png", &png) < 0) return false;
    if (bmp.Save(tempPath, &png, nullptr) != Ok) return false;

    HANDLE f = CreateFileW(tempPath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    out.resize((size_t)sz.QuadPart);
    DWORD read = 0;
    bool ok = out.empty() ||
              (ReadFile(f, out.data(), (DWORD)out.size(), &read, nullptr) && read == out.size());
    CloseHandle(f);
    DeleteFileW(tempPath);
    return ok && !out.empty();
}

static void DrawIcon(Bitmap& bmp, int S) {
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    g.SetCompositingQuality(CompositingQualityHighQuality);
    g.Clear(Color(0, 0, 0, 0));

    REAL k = (REAL)S / 512.0f;
    REAL r = 112.0f * k;

    GraphicsPath path;
    path.AddArc(0.0f, 0.0f, r * 2, r * 2, 180.0f, 90.0f);
    path.AddArc((REAL)S - r * 2, 0.0f, r * 2, r * 2, 270.0f, 90.0f);
    path.AddArc((REAL)S - r * 2, (REAL)S - r * 2, r * 2, r * 2, 0.0f, 90.0f);
    path.AddArc(0.0f, (REAL)S - r * 2, r * 2, r * 2, 90.0f, 90.0f);
    path.CloseFigure();

    LinearGradientBrush bg(PointF(0, 0), PointF((REAL)S, (REAL)S),
                           Color(255, 11, 34, 68), Color(255, 38, 138, 210));
    g.FillPath(&bg, &path);
    g.SetClip(&path);

    SolidBrush glow(Color(70, 255, 205, 128));
    g.FillEllipse(&glow, 236.0f * k, -20.0f * k, 320.0f * k, 320.0f * k);

    SolidBrush sun(Color(255, 255, 200, 108));
    g.FillEllipse(&sun, 318.0f * k, 78.0f * k, 104.0f * k, 104.0f * k);

    PointF farPts[] = {
        PointF(0 * k, 374 * k), PointF(148 * k, 248 * k), PointF(244 * k, 332 * k),
        PointF(332 * k, 230 * k), PointF(432 * k, 328 * k), PointF(512 * k, 288 * k),
        PointF(512 * k, 512 * k), PointF(0 * k, 512 * k)
    };
    SolidBrush farBrush(Color(255, 15, 46, 88));
    g.FillPolygon(&farBrush, farPts, 8);

    PointF nearP[] = {
        PointF(0 * k, 436 * k), PointF(116 * k, 340 * k), PointF(252 * k, 450 * k),
        PointF(332 * k, 380 * k), PointF(432 * k, 460 * k), PointF(512 * k, 424 * k),
        PointF(512 * k, 512 * k), PointF(0 * k, 512 * k)
    };
    SolidBrush nearBrush(Color(255, 219, 238, 255));
    g.FillPolygon(&nearBrush, nearP, 8);

    SolidBrush base(Color(255, 10, 30, 62));
    g.FillRectangle(&base, 0.0f, 470.0f * k, 512.0f * k, 42.0f * k);

    g.ResetClip();

    Pen pen(Color(60, 255, 255, 255), 5.0f * k);
    g.DrawPath(&pen, &path);
}

int wmain(int argc, wchar_t** argv) {
    const wchar_t* outPath = (argc > 1) ? argv[1] : L"app.ico";

    GdiplusStartupInput input;
    input.GdiplusVersion = 1;
    input.DebugEventCallback = nullptr;
    input.SuppressBackgroundThread = FALSE;
    input.SuppressExternalCodecs = TRUE;
    ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &input, nullptr) != Ok) {
        wprintf(L"GdiplusStartup failed\n");
        return 1;
    }

    const int sizes[] = {16, 24, 32, 48, 64, 128, 256};
    struct Entry { int size; std::vector<BYTE> data; };
    std::vector<Entry> entries;

    {
        Bitmap master(512, 512, PixelFormat32bppARGB);
        if (master.GetLastStatus() != Ok) {
            wprintf(L"master bitmap creation failed\n");
            return 1;
        }
        DrawIcon(master, 512);

        for (int s : sizes) {
            Bitmap thumb(s, s, PixelFormat32bppARGB);
            {
                Graphics tg(&thumb);
                tg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
                tg.SetPixelOffsetMode(PixelOffsetModeHighQuality);
                tg.SetSmoothingMode(SmoothingModeAntiAlias);
                tg.SetCompositingQuality(CompositingQualityHighQuality);
                tg.Clear(Color(0, 0, 0, 0));
                tg.DrawImage(&master, 0, 0, s, s);
            }
            std::vector<BYTE> png;
            wchar_t tempName[64];
            swprintf_s(tempName, L"ico_tmp_%d.png", s);
            if (!SavePng(thumb, tempName, png)) {
                wprintf(L"PNG encode failed (size=%d)\n", s);
                return 1;
            }
            entries.push_back({s, png});
        }
    } // 所有 GDI+ 对象在此销毁，之后才能 GdiplusShutdown

    FILE* f = _wfopen(outPath, L"wb");
    if (!f) {
        wprintf(L"Cannot write %ls\n", outPath);
        GdiplusShutdown(token);
        return 1;
    }

    uint16_t reserved = 0, type = 1, count = (uint16_t)entries.size();
    fwrite(&reserved, 2, 1, f);
    fwrite(&type, 2, 1, f);
    fwrite(&count, 2, 1, f);

    uint32_t offset = 6 + 16 * (uint32_t)entries.size();
    for (auto& e : entries) {
        unsigned char dim = (e.size >= 256) ? 0 : (unsigned char)e.size;
        unsigned char zero = 0;
        uint16_t planes = 1, bpp = 32;
        uint32_t bytes = (uint32_t)e.data.size();
        fwrite(&dim, 1, 1, f);
        fwrite(&dim, 1, 1, f);
        fwrite(&zero, 1, 1, f);
        fwrite(&zero, 1, 1, f);
        fwrite(&planes, 2, 1, f);
        fwrite(&bpp, 2, 1, f);
        fwrite(&bytes, 4, 1, f);
        fwrite(&offset, 4, 1, f);
        offset += bytes;
    }
    for (auto& e : entries) {
        fwrite(e.data.data(), 1, e.data.size(), f);
    }
    fclose(f);

    GdiplusShutdown(token);
    wprintf(L"Generated %ls\n", outPath);
    return 0;
}
