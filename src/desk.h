#pragma once
#include "common.h"
#include <vector>
#include <string>

struct ScreenInfo {
    std::wstring id;        // IDesktopWallpaper 显示器路径
    RECT rect = {0, 0, 0, 0}; // 虚拟桌面坐标（物理像素）
    int width = 0;
    int height = 0;
};

namespace desk {

// 枚举显示器：优先使用 IDesktopWallpaper（可逐屏设置）
bool EnumScreens(std::vector<ScreenInfo>& screens, bool& perMonitorCapable, std::wstring& err);

// 逐屏设置壁纸；files 数量可为 screens 数量或 1（单图用于全部）
bool ApplyPerScreen(const std::vector<ScreenInfo>& screens,
                    const std::vector<std::wstring>& files, std::wstring& err);

// 只设置其中一台显示器，其余保持不变
bool ApplyOne(const std::vector<ScreenInfo>& screens, int index,
              const std::wstring& file, std::wstring& err);

// 传统方式（整桌面单图）
bool ApplySingle(const std::wstring& file, std::wstring& err);

} // namespace desk

namespace autostart {
bool IsEnabled();
bool SetEnabled(bool enabled);
}
