#pragma once
#include "common.h"
#include <vector>

// 用 Windows Imaging Component 解码图片（JPEG/PNG/WebP 等系统已注册的格式）
namespace wic {

// 解码到 BGRA（straight alpha）缓冲区，stride = width*4
bool DecodeToBGRA(const std::wstring& path, std::vector<uint8_t>& out,
                  int& width, int& height, std::wstring& err);

} // namespace wic
