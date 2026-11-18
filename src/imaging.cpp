#include "imaging.h"

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

namespace {

ULONG_PTR g_token = 0;
bool g_inited = false;

int GetEncoderClsid(const WCHAR* format, CLSID* out) {
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    std::vector<BYTE> buf(size);
    ImageCodecInfo* info = reinterpret_cast<ImageCodecInfo*>(buf.data());
    GetImageEncoders(num, size, info);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(info[i].MimeType, format) == 0) {
            *out = info[i].Clsid;
            return (int)i;
        }
    }
    return -1;
}

bool SaveJpeg(Bitmap& bmp, const std::wstring& path, int quality) {
    CLSID clsid{};
    if (GetEncoderClsid(L"image/jpeg", &clsid) < 0) return false;

    if (quality < 50) quality = 50;
    if (quality > 100) quality = 100;
    long q = quality;

    EncoderParameters params;
    params.Count = 1;
    params.Parameter[0].Guid = EncoderQuality;
    params.Parameter[0].Type = EncoderParameterValueTypeLong;
    params.Parameter[0].NumberOfValues = 1;
    params.Parameter[0].Value = &q;

    Status st = bmp.Save(path.c_str(), &clsid, &params);
    return st == Ok;
}

} // namespace

namespace gfx {

bool Startup() {
    if (g_inited) return true;
    GdiplusStartupInput input;
    input.GdiplusVersion = 1;
    input.DebugEventCallback = nullptr;
    input.SuppressBackgroundThread = FALSE;
    input.SuppressExternalCodecs = TRUE;
    if (GdiplusStartup(&g_token, &input, nullptr) != Ok) return false;
    g_inited = true;
    return true;
}

void Shutdown() {
    if (g_inited) {
        GdiplusShutdown(g_token);
        g_token = 0;
        g_inited = false;
    }
}

bool GetImageSize(const std::wstring& path, int& w, int& h) {
    Bitmap bmp(path.c_str(), false);
    if (bmp.GetLastStatus() != Ok) return false;
    w = (int)bmp.GetWidth();
    h = (int)bmp.GetHeight();
    return true;
}

bool RenderCoverCrop(const std::wstring& srcPath,
                     int canvasW, int canvasH,
                     const std::vector<RECT>& slices,
                     const std::vector<std::wstring>& dsts,
                     int jpegQuality,
                     std::wstring& err) {
    if (slices.size() != dsts.size() || slices.empty()) {
        err = L"切片参数不匹配";
        return false;
    }
    if (canvasW <= 0 || canvasH <= 0) {
        err = L"画布尺寸无效";
        return false;
    }

    Bitmap src(srcPath.c_str(), false);
    if (src.GetLastStatus() != Ok) {
        err = L"图片解码失败";
        return false;
    }

    const int sw = (int)src.GetWidth();
    const int sh = (int)src.GetHeight();
    if (sw <= 0 || sh <= 0) {
        err = L"图片尺寸无效";
        return false;
    }

    // cover：等比缩放到刚好覆盖画布，超出部分居中裁掉
    double scale = std::max((double)canvasW / sw, (double)canvasH / sh);
    double drawW = sw * scale;
    double drawH = sh * scale;
    double offX = (canvasW - drawW) / 2.0;
    double offY = (canvasH - drawH) / 2.0;

    for (size_t i = 0; i < slices.size(); ++i) {
        const RECT& r = slices[i];
        int dw = r.right - r.left;
        int dh = r.bottom - r.top;
        if (dw <= 0 || dh <= 0) {
            err = L"切片尺寸无效";
            return false;
        }

        // 计算该切片在源图上的取样区域
        double sx = (r.left - offX) / scale;
        double sy = (r.top - offY) / scale;
        double srw = dw / scale;
        double srh = dh / scale;

        // 越界保护
        if (sx < 0) { srw += sx; sx = 0; }
        if (sy < 0) { srh += sy; sy = 0; }
        if (sx + srw > sw) srw = sw - sx;
        if (sy + srh > sh) srh = sh - sy;
        if (srw <= 0.5) srw = 0.5;
        if (srh <= 0.5) srh = 0.5;

        Bitmap dst(dw, dh, PixelFormat24bppRGB);
        if (dst.GetLastStatus() != Ok) {
            err = L"无法创建输出位图";
            return false;
        }
        {
            Graphics g(&dst);
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
            g.SetSmoothingMode(SmoothingModeHighQuality);
            g.SetCompositingQuality(CompositingQualityHighQuality);
            g.DrawImage(&src,
                        RectF(0.0f, 0.0f, (REAL)dw, (REAL)dh),
                        (REAL)sx, (REAL)sy, (REAL)srw, (REAL)srh,
                        UnitPixel);
        }
        if (!SaveJpeg(dst, dsts[i], jpegQuality)) {
            err = L"保存 JPEG 失败";
            return false;
        }
    }
    return true;
}

} // namespace gfx
