// 测试系统 WIC 是否能解码 WebP
#include <windows.h>
#include <wincodec.h>
#include <stdio.h>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (argc < 2) { wprintf(L"usage: test_wic <file>\n"); return 1; }

    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { wprintf(L"factory failed 0x%08X\n", hr); return 1; }

    IWICBitmapDecoder* dec = nullptr;
    hr = factory->CreateDecoderFromFilename(argv[1], nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnDemand, &dec);
    if (FAILED(hr)) {
        wprintf(L"decoder failed 0x%08X (0x88982F50 = no codec)\n", hr);
        return 1;
    }

    IWICBitmapFrameDecode* frame = nullptr;
    hr = dec->GetFrame(0, &frame);
    if (FAILED(hr)) { wprintf(L"frame failed 0x%08X\n", hr); return 1; }

    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    wprintf(L"decoded size = %u x %u\n", w, h);

    IWICFormatConverter* conv = nullptr;
    hr = factory->CreateFormatConverter(&conv);
    if (FAILED(hr)) { wprintf(L"converter failed 0x%08X\n", hr); return 1; }
    hr = conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                          nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) { wprintf(L"convert failed 0x%08X\n", hr); return 1; }

    wprintf(L"WIC webp decode OK\n");
    conv->Release();
    frame->Release();
    dec->Release();
    factory->Release();
    CoUninitialize();
    return 0;
}
