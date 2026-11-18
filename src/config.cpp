#include "config.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <map>

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static bool GetBool(const std::map<std::string, std::string>& kv, const char* key, bool def) {
    auto it = kv.find(key);
    if (it == kv.end()) return def;
    const std::string& v = it->second;
    if (v == "1" || v == "true" || v == "yes") return true;
    if (v == "0" || v == "false" || v == "no") return false;
    return def;
}

static int GetInt(const std::map<std::string, std::string>& kv, const char* key, int def) {
    auto it = kv.find(key);
    if (it == kv.end() || it->second.empty()) return def;
    try { return std::stoi(it->second); } catch (...) { return def; }
}

static std::wstring GetStr(const std::map<std::string, std::string>& kv, const char* key,
                           const std::wstring& def) {
    auto it = kv.find(key);
    if (it == kv.end()) return def;
    return Utf8ToWide(it->second);
}

void Config::Load(const std::wstring& path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) return;

    std::map<std::string, std::string> kv;
    std::string line;
    while (std::getline(in, line)) {
        std::string t = Trim(line);
        if (t.empty() || t[0] == '#' || t[0] == '[') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        kv[Trim(t.substr(0, eq))] = Trim(t.substr(eq + 1));
    }

    mode = GetStr(kv, "mode", mode);
    intervalMinutes = GetInt(kv, "interval_minutes", intervalMinutes);
    autostart = GetBool(kv, "autostart", autostart);
    showTips = GetBool(kv, "show_tips", showTips);
    source = GetStr(kv, "source", source);
    categorySlug = GetStr(kv, "category", categorySlug);
    tagName = GetStr(kv, "tag", tagName);
    customUrl = GetStr(kv, "custom_url", customUrl);
    resolution = GetStr(kv, "resolution", resolution);
    keepFiles = GetInt(kv, "keep_files", keepFiles);
    jpegQuality = GetInt(kv, "jpeg_quality", jpegQuality);
    requestCapWidth = GetInt(kv, "request_cap_width", requestCapWidth);
    autoClean = GetBool(kv, "auto_clean", autoClean);
    keepFavorites = GetBool(kv, "keep_favorites", keepFavorites);
    keepDays = GetInt(kv, "keep_days", keepDays);
    maxCacheMb = GetInt(kv, "max_cache_mb", maxCacheMb);
    firstRun = GetBool(kv, "first_run", firstRun);
    lastMonitorCount = GetInt(kv, "last_monitor_count", lastMonitorCount);

    if (keepFiles < 5) keepFiles = 5;
    if (jpegQuality < 60) jpegQuality = 60;
    if (jpegQuality > 100) jpegQuality = 100;
    if (intervalMinutes < 0) intervalMinutes = 0;
    if (requestCapWidth < 1280) requestCapWidth = 1280;
}

void Config::Save(const std::wstring& path) const {
    std::ostringstream out;
    out << "# WallpaperFeed 配置文件（UTF-8）\r\n";
    out << "# mode: different = 每屏不同 | span = 跨屏合一 | clone = 复制同一张\r\n";
    out << "mode=" << WideToUtf8(mode) << "\r\n";
    out << "# 定时切换间隔（分钟），0 = 关闭\r\n";
    out << "interval_minutes=" << intervalMinutes << "\r\n";
    out << "autostart=" << (autostart ? 1 : 0) << "\r\n";
    out << "show_tips=" << (showTips ? 1 : 0) << "\r\n";
    out << "# source: trending | new | popular | category | tag | custom\r\n";
    out << "source=" << WideToUtf8(source) << "\r\n";
    out << "category=" << WideToUtf8(categorySlug) << "\r\n";
    out << "tag=" << WideToUtf8(tagName) << "\r\n";
    out << "custom_url=" << WideToUtf8(customUrl) << "\r\n";
    out << "# 屏幕尺寸规格：空 = 不限，或 1920x1080 / 2560x1440 / 4k / 5k / 8k / ultrawide / dual-monitor / macbook / phone\r\n";
    out << "resolution=" << WideToUtf8(resolution) << "\r\n";
    out << "keep_files=" << keepFiles << "\r\n";
    out << "jpeg_quality=" << jpegQuality << "\r\n";
    out << "request_cap_width=" << requestCapWidth << "\r\n";
    out << "# 存储管理\r\n";
    out << "auto_clean=" << (autoClean ? 1 : 0) << "\r\n";
    out << "keep_favorites=" << (keepFavorites ? 1 : 0) << "\r\n";
    out << "keep_days=" << keepDays << "\r\n";
    out << "max_cache_mb=" << maxCacheMb << "\r\n";
    out << "first_run=" << (firstRun ? 1 : 0) << "\r\n";
    out << "last_monitor_count=" << lastMonitorCount << "\r\n";

    std::ofstream f(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!f) return;
    std::string text = out.str();
    f.write(text.data(), (std::streamsize)text.size());
}

std::wstring Config::SourceUrl() const {
    const std::wstring base = L"https://wallpaperfeed.com";
    // 选了屏幕尺寸规格时，优先用站点的 /theme/<slug> 图库（站点不支持分类和尺寸叠加）
    if (!resolution.empty()) return base + L"/theme/" + resolution;
    if (source == L"new") return base + L"/new";
    if (source == L"popular") return base + L"/popular";
    if (source == L"category") return base + L"/category/" + categorySlug;
    if (source == L"tag") return base + L"/tag/" + tagName;
    if (source == L"custom") {
        if (customUrl.empty()) return base + L"/trending";
        return customUrl;
    }
    return base + L"/trending";
}

std::wstring Config::SourceLabel() const {
    if (!resolution.empty()) {
        std::wstring rl = ResolutionLabel();
        return L"规格 / " + (rl.empty() ? resolution : rl);
    }
    if (source == L"new") return L"最新上传";
    if (source == L"popular") return L"最高下载";
    if (source == L"category") {
        for (const auto& c : GetCategories()) {
            if (categorySlug == c.slug) return std::wstring(L"分类 / ") + c.label;
        }
        return L"分类 / " + categorySlug;
    }
    if (source == L"tag") return L"标签 / " + tagName;
    if (source == L"custom") return L"自定义地址";
    return L"本周趋势";
}

// ── 最近使用记录 ────────────────────────────────────────────────────────
void RecentIds::Load(const std::wstring& path) {
    path_ = path;
    ids_.clear();
    std::ifstream in(path.c_str());
    if (!in) return;
    int v = 0;
    while (in >> v) ids_.push_back(v);
}

void RecentIds::Save() const {
    if (path_.empty()) return;
    std::ofstream f(path_.c_str(), std::ios::trunc);
    if (!f) return;
    for (size_t i = 0; i < ids_.size(); ++i) {
        if (i) f << "\n";
        f << ids_[i];
    }
}

bool RecentIds::Contains(int id) const {
    return std::find(ids_.begin(), ids_.end(), id) != ids_.end();
}

void RecentIds::Add(int id, size_t maxKeep) {
    ids_.erase(std::remove(ids_.begin(), ids_.end(), id), ids_.end());
    ids_.insert(ids_.begin(), id);
    if (ids_.size() > maxKeep) ids_.resize(maxKeep);
}

const std::vector<CategoryDef>& GetCategories() {
    static const std::vector<CategoryDef> cats = {
        {L"landscapes",       L"自然风景"},
        {L"nature",           L"自然环境"},
        {L"anime",            L"动漫"},
        {L"abstract",         L"抽象"},
        {L"people-portraits", L"人物肖像"},
        {L"animals",          L"动物"},
        {L"architecture",     L"建筑"},
        {L"art-design",       L"艺术设计"},
        {L"cars-vehicles",    L"汽车载具"},
        {L"cartoons-comics",  L"卡通漫画"},
        {L"city-urban",       L"城市"},
        {L"cyberpunk",        L"赛博朋克"},
        {L"dark",             L"暗色"},
        {L"fantasy",          L"奇幻"},
        {L"food",             L"美食"},
        {L"gaming",           L"游戏"},
        {L"kids-cute",        L"可爱儿童"},
        {L"minimalist",       L"极简"},
        {L"movies-tv",        L"影视"},
        {L"music",            L"音乐"},
        {L"photography",      L"摄影"},
        {L"retro",            L"复古"},
        {L"sci-fi",           L"科幻"},
        {L"space",            L"太空"},
        {L"sports",           L"运动"},
        {L"technology",       L"科技"},
        {L"weapons",          L"武器"},
    };
    return cats;
}

const std::vector<ResolutionDef>& GetResolutions() {
    static const std::vector<ResolutionDef> res = {
        {L"1920x1080",   L"1080P",      1920, 1080},
        {L"2560x1440",   L"2K",         2560, 1440},
        {L"4k",          L"4K",         3840, 2160},
        {L"5k",          L"5K",         5120, 2880},
        {L"8k",          L"8K",         7680, 4320},
        {L"ultrawide",   L"带鱼屏",      3440, 1440},
        {L"dual-monitor",L"双屏拼接",    3840, 1080},
        {L"macbook",     L"MacBook",    2880, 1800},
        {L"phone",       L"手机竖屏",     1080, 1920},
    };
    return res;
}

std::wstring Config::ResolutionLabel() const {
    for (const auto& r : GetResolutions()) {
        if (resolution == r.slug) return r.label;
    }
    return resolution;
}

bool Config::ResolutionSize(int& w, int& h) const {
    for (const auto& r : GetResolutions()) {
        if (resolution == r.slug) { w = r.w; h = r.h; return true; }
    }
    return false;
}
