#include "wf.h"
#include "net.h"
#include "config.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>

namespace {

const wchar_t* kBase = L"https://wallpaperfeed.com";

size_t FindOrNpos(const std::string& s, const char* needle, size_t from) {
    size_t p = s.find(needle, from);
    return p;
}

// 从 [begin, end) 里取 attr="value" 的 value
std::string AttrValue(const std::string& s, size_t begin, size_t end, const char* attr) {
    size_t p = s.find(attr, begin);
    if (p == std::string::npos || p >= end) return std::string();
    size_t q = s.find('"', p);
    if (q == std::string::npos || q >= end) return std::string();
    size_t r = s.find('"', q + 1);
    if (r == std::string::npos || r > end) return std::string();
    return s.substr(q + 1, r - q - 1);
}

void AppendUtf8(std::string& out, int code) {
    if (code < 0x80) {
        out.push_back((char)code);
    } else if (code < 0x800) {
        out.push_back((char)(0xC0 | (code >> 6)));
        out.push_back((char)(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out.push_back((char)(0xE0 | (code >> 12)));
        out.push_back((char)(0x80 | ((code >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (code & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (code >> 18)));
        out.push_back((char)(0x80 | ((code >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((code >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (code & 0x3F)));
    }
}

std::string HtmlUnescape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '&') {
            out.push_back(in[i]);
            continue;
        }
        if (in.compare(i, 5, "&amp;") == 0) { out += '&'; i += 4; }
        else if (in.compare(i, 4, "&lt;") == 0) { out += '<'; i += 3; }
        else if (in.compare(i, 4, "&gt;") == 0) { out += '>'; i += 3; }
        else if (in.compare(i, 6, "&quot;") == 0) { out += '"'; i += 5; }
        else if (in.compare(i, 6, "&nbsp;") == 0) { out += ' '; i += 5; }
        else if (in.compare(i, 6, "&#039;") == 0) { out += '\''; i += 5; }
        else if (in.compare(i, 5, "&#39;") == 0) { out += '\''; i += 4; }
        else if (i + 1 < in.size() && in[i + 1] == '#') {
            size_t semi = in.find(';', i);
            if (semi != std::string::npos && semi - i <= 9) {
                std::string num = in.substr(i + 2, semi - i - 2);
                int code = 0;
                if (!num.empty() && (num[0] == 'x' || num[0] == 'X')) {
                    code = (int)strtol(num.c_str() + 1, nullptr, 16);
                } else {
                    code = (int)strtol(num.c_str(), nullptr, 10);
                }
                if (code > 0 && code <= 0x10FFFF) AppendUtf8(out, code);
                i = semi;
                continue;
            }
            out.push_back(in[i]);
        }
        else out.push_back(in[i]);
    }
    return out;
}

std::string CleanTitle(const std::string& alt) {
    std::string t = alt;
    size_t p = t.find(" - free ");
    if (p != std::string::npos) t = t.substr(0, p);
    return HtmlUnescape(t);
}

bool IsImageFile(const std::wstring& path, uint64_t* sizeOut) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    unsigned char head[4] = {};
    DWORD read = 0;
    ReadFile(f, head, 4, &read, nullptr);
    CloseHandle(f);

    if (sizeOut) *sizeOut = (uint64_t)sz.QuadPart;
    if (sz.QuadPart < 4096) return false;
    // JPEG / PNG / BMP / WEBP
    if (read >= 3 && head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF) return true;
    if (read >= 4 && head[0] == 0x89 && head[1] == 'P' && head[2] == 'N' && head[3] == 'G') return true;
    if (read >= 2 && head[0] == 'B' && head[1] == 'M') return true;
    if (read >= 4 && memcmp(head, "RIFF", 4) == 0) return true;
    return false;
}

} // namespace

std::wstring WallpaperFeed::DetailReferer(const std::string& href, int id) {
    if (!href.empty()) return std::wstring(kBase) + Utf8ToWide(href);
    wchar_t buf[128];
    swprintf_s(buf, L"https://wallpaperfeed.com/wallpaper/%d", id);
    return buf;
}

bool WallpaperFeed::GetListing(const std::wstring& url, std::vector<WfItem>& items,
                               std::wstring& err) {
    items.clear();
    net::Response res;
    if (!net::GetText(url, res, err)) {
        LogMsg(L"列表请求失败: %s (%s)", url.c_str(), err.c_str());
        return false;
    }

    const std::string& body = res.body;
    if (body.size() < 1000) {
        err = L"返回内容为空";
        return false;
    }

    std::set<int> seen;
    size_t pos = 0;
    const std::string marker = "data-wallpaper-id=\"";
    while (true) {
        size_t p = body.find(marker, pos);
        if (p == std::string::npos) break;
        pos = p + marker.size();

        // id
        size_t numStart = p + marker.size();
        size_t numEnd = numStart;
        while (numEnd < body.size() && isdigit((unsigned char)body[numEnd])) ++numEnd;
        if (numEnd == numStart) continue;
        int id = atoi(body.substr(numStart, numEnd - numStart).c_str());
        if (id <= 0 || seen.count(id)) continue;

        // 卡片块边界
        size_t blockStart = body.rfind("<figure", p);
        if (blockStart == std::string::npos) blockStart = p;
        size_t blockEnd = body.find("</figure>", p);
        if (blockEnd == std::string::npos) blockEnd = std::min(body.size(), p + 4096);

        // img 标签
        size_t imgStart = body.find("<img", blockStart);
        if (imgStart == std::string::npos || imgStart > blockEnd) continue;
        size_t imgEnd = body.find('>', imgStart);
        if (imgEnd == std::string::npos || imgEnd > blockEnd) imgEnd = blockEnd;

        int w = atoi(AttrValue(body, imgStart, imgEnd, "width=\"").c_str());
        int h = atoi(AttrValue(body, imgStart, imgEnd, "height=\"").c_str());
        std::string alt = AttrValue(body, imgStart, imgEnd, "alt=\"");
        std::string thumb = AttrValue(body, imgStart, imgEnd, "src=\"");
        if (thumb.find("/uploads/") != 0) thumb.clear();

        std::string href = AttrValue(body, blockStart, blockEnd, "href=\"/wallpaper/");
        if (!href.empty()) {
            if (href.rfind("/wallpaper/", 0) != 0) href = "/wallpaper/" + href;
        }
        else {
            size_t hp = body.find("\"/wallpaper/", blockStart);
            if (hp != std::string::npos && hp < blockEnd) {
                size_t he = body.find('"', hp + 1);
                if (he != std::string::npos) href = body.substr(hp + 1, he - hp - 1);
            }
        }

        WfItem it;
        it.id = id;
        it.w = w;
        it.h = h;
        it.title = CleanTitle(alt);
        it.href = href;
        it.thumbUrl = thumb;
        items.push_back(it);
        seen.insert(id);

        if (items.size() >= 200) break;
    }

    if (items.empty()) {
        err = L"未解析到壁纸条目（页面结构可能已变化）";
        LogMsg(L"解析失败: %s", url.c_str());
        return false;
    }

    LogMsg(L"列表 %s 解析到 %d 张", url.c_str(), (int)items.size());
    return true;
}

bool WallpaperFeed::Download(int id, const std::wstring& referer, int width, int height,
                            const std::wstring& outFile, std::wstring& err) {
    if (width < 320) width = 320;
    if (height < 320) height = 320;

    char form[256];
    sprintf_s(form, "wallpaper_id=%d&mode=rescale&width=%d&height=%d", id, width, height);

    std::wstring part = outFile + L".part";
    DeleteFileW(part.c_str());

    net::Response res;
    std::string body(form);
    if (!net::FetchToFile(std::wstring(kBase) + L"/api/download.php", referer, &body, part, res, err)) {
        LogMsg(L"下载失败 id=%d (%dx%d): %s", id, width, height, err.c_str());
        DeleteFileW(part.c_str());
        return false;
    }

    uint64_t size = 0;
    if (!IsImageFile(part, &size)) {
        err = L"返回数据不是有效图片";
        LogMsg(L"下载内容异常 id=%d type=%s", id, Utf8ToWide(res.contentType).c_str());
        DeleteFileW(part.c_str());
        return false;
    }

    if (!MoveFileExW(part.c_str(), outFile.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        err = L"无法保存壁纸文件";
        DeleteFileW(part.c_str());
        return false;
    }

    LogMsg(L"下载完成 id=%d %dx%d %s", id, width, height, FormatBytes(size).c_str());
    return true;
}
