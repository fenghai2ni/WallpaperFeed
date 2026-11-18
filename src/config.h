#pragma once
#include "common.h"
#include <map>

struct Config {
    // 显示模式：different = 每屏不同，span = 跨屏合一，clone = 复制到所有屏
    std::wstring mode = L"different";
    int  intervalMinutes = 60;      // 0 表示不定时切换
    bool autostart = false;         // 开机自动启动
    bool showTips = true;           // 切换后托盘气泡提示

    // 壁纸源：trending / new / popular / category / tag / custom
    std::wstring source = L"trending";
    std::wstring categorySlug = L"landscapes";
    std::wstring tagName = L"nature";
    std::wstring customUrl = L"";

    int  keepFiles = 40;            // 本地缓存保留数量
    int  jpegQuality = 92;
    int  requestCapWidth = 5120;    // 单次下载请求的最大宽度

    // 存储管理
    bool autoClean = true;          // 自动清理旧文件
    bool keepFavorites = true;      // 清理时保留收藏的壁纸
    int  keepDays = 30;             // 只保留最近 N 天的壁纸，0 = 不限
    int  maxCacheMb = 1024;         // 最大占用（MB），0 = 不限

    bool firstRun = true;
    int  lastMonitorCount = 2;

    void Load(const std::wstring& path);
    void Save(const std::wstring& path) const;

    std::wstring SourceUrl() const;     // 由 source 派生出的列表页 URL
    std::wstring SourceLabel() const;   // 菜单中显示的名称
};

// 最近用过的壁纸 id，避免连续重复
class RecentIds {
public:
    void Load(const std::wstring& path);
    void Save() const;
    bool Contains(int id) const;
    void Add(int id, size_t maxKeep = 200);
private:
    std::wstring path_;
    std::vector<int> ids_;
};

struct CategoryDef {
    const wchar_t* slug;
    const wchar_t* label;
};
const std::vector<CategoryDef>& GetCategories();
