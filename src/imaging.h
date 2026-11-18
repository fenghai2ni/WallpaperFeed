#pragma once
#include "common.h"
#include <vector>
#include <string>

namespace gfx {

bool Startup();
void Shutdown();

// 把 srcPath 按 cover 方式铺满 canvasW×canvasH，然后按 slices（画布坐标）逐块裁切保存
// slices 与 dsts 一一对应
bool RenderCoverCrop(const std::wstring& srcPath,
                     int canvasW, int canvasH,
                     const std::vector<RECT>& slices,
                     const std::vector<std::wstring>& dsts,
                     int jpegQuality,
                     std::wstring& err);

// 只读取图片尺寸（用于校验/日志）
bool GetImageSize(const std::wstring& path, int& w, int& h);

} // namespace gfx
