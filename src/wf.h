#pragma once
#include "common.h"
#include <vector>
#include <string>

struct WfItem {
    int id = 0;
    int w = 0;              // 原图宽
    int h = 0;              // 原图高
    std::string title;      // UTF-8
    std::string href;       // /wallpaper/<id>-<slug>
    std::string thumbUrl;   // 缩略图相对地址 /uploads/thumbnails/xxx.webp
};

class WallpaperFeed {
public:
    // 抓取列表页并解析出壁纸条目
    static bool GetListing(const std::wstring& url, std::vector<WfItem>& items, std::wstring& err);

    // 通过 download.php 获取指定分辨率（等比原图比例）的 JPEG
    static bool Download(int id, const std::wstring& referer, int width, int height,
                         const std::wstring& outFile, std::wstring& err);

    static std::wstring DetailReferer(const std::string& href, int id);
};
