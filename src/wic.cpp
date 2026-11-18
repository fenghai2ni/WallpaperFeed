#include "wic.h"

#include <wincodec.h>
#include <objbase.h>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace wic {

bool DecodeToBGRA(const std::wstring& path, std::vector<uint8_t>& out,
                  int& width, int& height, std::wstring& err) {
    out.clear();
    width = height = 0;

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool needUninit = SUCCEEDED(hrCo);

    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    bool ok = false;

    do {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory)))) {
            err = L"WIC 工厂创建失败";
            break;
        }
        if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnDemand, &dec))) {
            err = L"无法解码该图片格式";
            break;
        }
        if (FAILED(dec->GetFrame(0, &frame))) {
            err = L"读取图像帧失败";
            break;
        }

        UINT w = 0, h = 0;
        if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0) {
            err = L"图像尺寸无效";
            break;
        }
        if (w > 8192 || h > 8192) {
            err = L"图像尺寸超出限制";
            break;
        }

        if (FAILED(factory->CreateFormatConverter(&conv))) {
            err = L"创建格式转换器失败";
            break;
        }
        if (FAILED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                                    WICBitmapDitherTypeNone, nullptr, 0.0,
                                    WICBitmapPaletteTypeCustom))) {
            err = L"转换到 BGRA 失败";
            break;
        }

        size_t stride = (size_t)w * 4;
        out.resize(stride * h);
        if (FAILED(conv->CopyPixels(nullptr, (UINT)stride, (UINT)out.size(), out.data()))) {
            out.clear();
            err = L"读取像素失败";
            break;
        }

        width = (int)w;
        height = (int)h;
        ok = true;
    } while (false);

    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (factory) factory->Release();
    if (needUninit) CoUninitialize();
    return ok;
}

} // namespace wic
