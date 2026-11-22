#include "library.h"
#include "config.h"
#include "net.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <random>
#include <set>

#pragma comment(lib, "winhttp.lib")

namespace {

const wchar_t* kBase = L"https://wallpaperfeed.com";

bool IsWebpOrImage(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    unsigned char head[12] = {};
    DWORD read = 0;
    ReadFile(f, head, 12, &read, nullptr);
    CloseHandle(f);
    if (sz.QuadPart < 2000) return false;
    if (read >= 3 && head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF) return true;
    if (read >= 4 && head[0] == 0x89 && head[1] == 'P' && head[2] == 'N' && head[3] == 'G') return true;
    if (read >= 12 && memcmp(head, "RIFF", 4) == 0 && memcmp(head + 8, "WEBP", 4) == 0) return true;
    return false;
}

const char* kFallbackTags[] = {
    "anime", "nature", "space", "car", "minimal", "dark", "cyberpunk", "city",
    "fantasy", "technology", "art", "landscape", "animal", "flower", "ocean",
    "mountain", "sunset", "night", "rain", "winter", "neon", "gaming", "portrait", "abstract"
};

} // namespace

Library::~Library() { Shutdown(); }

void Library::Init(const std::wstring& dataDir, HWND notifyWnd) {
    dataDir_ = dataDir;
    notify_ = notifyWnd;
    thumbDir_ = PathJoin(dataDir, L"thumbs");
    EnsureDir(thumbDir_);
    favDir_ = PathJoin(PathJoin(dataDir, L"wallpapers"), L"fav");
    EnsureDir(favDir_);
    favPath_ = PathJoin(dataDir, L"favorites.txt");

    LoadFavoritesInternal();
    PruneFavFiles();

    listThread_ = std::thread(&Library::ListWorkerMain, this);
    for (int i = 0; i < 4; ++i) {
        thumbThreads_.push_back(std::thread(&Library::ThumbWorkerMain, this));
    }
    favThread_ = std::thread(&Library::FavWorkerMain, this);
    EnsureFavFiles();
    LogMsg(L"壁纸库已启动，缩略图线程 4 个，收藏下载目录 %s", favDir_.c_str());
}

void Library::Shutdown() {
    if (stopping_.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lk(jobMu_);
        stopFlag_ = true;
    }
    jobCv_.notify_all();
    {
        std::lock_guard<std::mutex> lk(favMu_);
        favStop_ = true;
    }
    favCv_.notify_all();
    if (listThread_.joinable()) listThread_.join();
    for (auto& t : thumbThreads_) {
        if (t.joinable()) t.join();
    }
    thumbThreads_.clear();
    if (favThread_.joinable()) favThread_.join();
}

std::wstring Library::ThumbPath(int id) const {
    wchar_t name[64];
    swprintf_s(name, L"thumb_%d.webp", id);
    return PathJoin(thumbDir_, name);
}

ItemPtr Library::MakeItem(const WfItem& src) {
    ItemPtr it = std::make_shared<Item>();
    it->id = src.id;
    it->w = src.w > 0 ? src.w : 1920;
    it->h = src.h > 0 ? src.h : 1080;
    it->title = src.title;
    it->href = src.href;
    it->thumbUrl = src.thumbUrl;
    it->thumbFile = ThumbPath(src.id);
    it->favFile = FavPath(src.id);
    if (it->thumbUrl.empty()) it->thumbState = 3;
    else if (FileExists(it->thumbFile)) it->thumbState = 2;
    else it->thumbState = 0;
    it->favState = FileExists(it->favFile) ? 2 : 0;
    return it;
}

void Library::SetMode(LibraryMode mode, const std::wstring& categorySlug,
                      const std::wstring& resolutionSlug) {
    std::vector<ItemPtr> favItems;
    {
        std::lock_guard<std::mutex> lk(mu_);
        mode_ = mode;
        if (!categorySlug.empty()) categorySlug_ = categorySlug;
        resSlug_ = resolutionSlug;
        resPage_ = 1;
        items_.clear();
        seen_.clear();
        // 内容源队列跨页面保留，切回「发现」时可立即加载，不用重新抓取
        if (mode != LibraryMode::Discover) {
            srcIdx_ = 0;
        }
        emptyStreak_ = 0;
        if (mode == LibraryMode::Category) {
            // 分类页支持 ?page=N（每页 32 张且互不重复），按 category / tag 交替往后翻
            catPage_ = 1;
            catTurn_ = 0;
            catDead_ = false;
            tagDead_ = false;
        }
        ++generation_;
        noMore_ = false;
        loading_ = false;
        if (mode == LibraryMode::Favorites) {
            for (int id : favOrder_) {
                auto f = favMap_.find(id);
                if (f != favMap_.end()) {
                    items_.push_back(f->second);
                    favItems.push_back(f->second);
                }
            }
            noMore_ = true;
        }
    }
    for (const auto& it : favItems) {
        EnqueueThumb(it);
        EnqueueFav(it);
    }
    if (mode != LibraryMode::Favorites) RequestMore();
    PostUpdate();
}

void Library::RequestMore() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (mode_ == LibraryMode::Favorites) return;
        if (noMore_.load()) return;
        loading_ = true;
    }
    {
        std::lock_guard<std::mutex> lk(jobMu_);
        listRequest_ = true;
    }
    jobCv_.notify_all();
}

std::vector<ItemPtr> Library::Snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    return items_;
}

size_t Library::Count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return items_.size();
}

size_t Library::FavoriteCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return favOrder_.size();
}

bool Library::IsFavorite(int id) const {
    std::lock_guard<std::mutex> lk(mu_);
    return favMap_.find(id) != favMap_.end();
}

ItemPtr Library::FindFavorite(int id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto f = favMap_.find(id);
    return f == favMap_.end() ? nullptr : f->second;
}

void Library::PostUpdate() {
    if (notify_) PostMessageW(notify_, WM_LIB_UPDATED, 0, 0);
}

// ── 列表加载 ────────────────────────────────────────────────────────────

void Library::ListWorkerMain() {
    // 后台抓列表用最低优先级，避免和前台程序、换壁纸线程抢 CPU
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    while (true) {
        std::unique_lock<std::mutex> lk(jobMu_);
        // 列表线程和缩略图线程共用一个 condition_variable，notify_one 可能唤醒错误的
        // 线程而把列表请求饿死；这里用带超时的等待兜底，任何通知丢失都能自愈。
        jobCv_.wait_for(lk, std::chrono::milliseconds(120),
                        [this] { return stopFlag_ || listRequest_; });
        if (stopFlag_) break;
        if (!listRequest_) continue;
        listRequest_ = false;
        lk.unlock();
        DoLoadMore();
    }
    CoUninitialize();
}

std::wstring Library::NextSourceUrl() {
    std::lock_guard<std::mutex> lk(mu_);
    // 选了屏幕尺寸规格：整站只按 /theme/<slug> 翻页（站点不支持分类 + 尺寸叠加）
    if (!resSlug_.empty()) {
        std::wstring url = std::wstring(kBase) + L"/theme/" + resSlug_;
        if (resPage_ > 1) url += L"?page=" + std::to_wstring(resPage_);
        return url;
    }
    if (mode_ == LibraryMode::Category) {
        // 只读不推进：页码在真正取到一页之后才往前走，失败不会把源耗光
        if (catDead_ && tagDead_) return std::wstring();
        std::wstring url = std::wstring(kBase) + (catTurn_ == 0 ? L"/category/" : L"/tag/") + categorySlug_;
        if (catPage_ > 1) url += L"?page=" + std::to_wstring(catPage_);
        return url;
    }
    if (sources_.empty()) return std::wstring();
    if (srcIdx_ >= sources_.size()) srcIdx_ = 0;
    return sources_[srcIdx_++];
}

void Library::AdvanceCategoryPage() {
    std::lock_guard<std::mutex> lk(mu_);
    if (catDead_) {                 // 只剩 tag，直接在 tag 上翻页
        catTurn_ = 1;
        ++catPage_;
        return;
    }
    if (tagDead_) {                 // 只剩 category，直接在 category 上翻页
        catTurn_ = 0;
        ++catPage_;
        return;
    }
    if (catTurn_ == 0) {
        catTurn_ = 1;
    } else {
        catTurn_ = 0;
        ++catPage_;
    }
}

void Library::RemoveItemById(int id) {
    std::lock_guard<std::mutex> lk(mu_);
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [id](const ItemPtr& p) { return p && p->id == id; }),
                 items_.end());
}

void Library::FetchTags() {
    net::Response res;
    std::wstring err;
    if (!net::GetText(std::wstring(kBase) + L"/tags", res, err)) {
        LogMsg(L"标签页抓取失败: %s", err.c_str());
        return;
    }
    std::set<std::string> found;
    const std::string& body = res.body;
    size_t p = 0;
    while ((p = body.find("/tag/", p)) != std::string::npos) {
        p += 5;
        size_t e = p;
        while (e < body.size()) {
            char c = body[e];
            if (!(isalnum((unsigned char)c) || c == '-' || c == '_')) break;
            ++e;
        }
        std::string t = body.substr(p, e - p);
        if (t.size() >= 2 && t.size() <= 40) found.insert(t);
        if (e == p) ++p; else p = e;
    }

    std::vector<std::wstring> out;
    for (const auto& t : found) out.push_back(Utf8ToWide(t));
    if (out.size() > 60) out.resize(60);

    std::lock_guard<std::mutex> lk(mu_);
    tags_.swap(out);
    LogMsg(L"标签页解析到 %d 个标签", (int)tags_.size());
}

void Library::EnsureSources() {
    std::vector<std::wstring> tags;
    {
        std::lock_guard<std::mutex> lk(mu_);
        tags = tags_;
    }
    if (tags.empty()) {
        FetchTags();
        std::lock_guard<std::mutex> lk(mu_);
        tags = tags_;
    }
    if (tags.empty()) {
        for (const char* t : kFallbackTags) tags.push_back(Utf8ToWide(t));
        std::lock_guard<std::mutex> lk(mu_);
        tags_ = tags;
    }

    std::vector<std::wstring> urls;
    urls.push_back(std::wstring(kBase) + L"/trending");
    urls.push_back(std::wstring(kBase) + L"/new");
    urls.push_back(std::wstring(kBase) + L"/popular");
    for (const auto& c : GetCategories()) {
        urls.push_back(std::wstring(kBase) + L"/category/" + c.slug);
    }
    for (const auto& t : tags) {
        urls.push_back(std::wstring(kBase) + L"/tag/" + t);
    }

    std::mt19937 gen((unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
    std::shuffle(urls.begin(), urls.end(), gen);

    std::lock_guard<std::mutex> lk(mu_);
    sources_.swap(urls);
    srcIdx_ = 0;
    LogMsg(L"内容源队列已建立，共 %d 个列表页", (int)sources_.size());
}

void Library::DoLoadMore() {
    LibraryMode mode;
    int gen = 0;
    bool needSources = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        mode = mode_;
        gen = generation_;
        needSources = sources_.empty();
    }
    if (mode == LibraryMode::Favorites) {
        loading_ = false;
        return;
    }

    if (needSources && mode == LibraryMode::Discover) {
        EnsureSources();
    }

    int attempts = 0;   // 空结果次数
    int fails = 0;      // 网络失败次数
    while (!stopping_.load()) {
        std::wstring url = NextSourceUrl();
        if (url.empty()) {
            // 没有可用源（队列正在重建），稍后重试即可，不判定为到底
            LogMsg(L"暂无可用内容源，稍后重试");
            break;
        }

        std::vector<WfItem> raw;
        std::wstring err;
        bool themeMode = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            themeMode = !resSlug_.empty();
        }
        if (!WallpaperFeed::GetListing(url, raw, err)) {
            bool gone404 = err.find(L"404") != std::wstring::npos;
            if (themeMode) {
                if (gone404) {
                    // 尺寸图库翻到最后一页时站点直接返回 404
                    std::lock_guard<std::mutex> lk(mu_);
                    noMore_ = true;
                    LogMsg(L"尺寸图库已翻到底 %s", url.c_str());
                    continue;
                }
                ++fails;
                LogMsg(L"列表请求失败(%d/4) %s : %s", fails, url.c_str(), err.c_str());
                if (fails >= 4) break;
                Sleep(250);
                continue;
            }
            // 404 = 这一路已经翻到底，只是标记，不算网络失败；其它错误才计数
            if (mode == LibraryMode::Category && gone404) {
                std::lock_guard<std::mutex> lk(mu_);
                if (catTurn_ == 0) catDead_ = true; else tagDead_ = true;
                if (catDead_ && tagDead_) noMore_ = true;
                LogMsg(L"分类源已翻到底 %s", url.c_str());
            } else {
                ++fails;
                LogMsg(L"列表请求失败(%d/4) %s : %s", fails, url.c_str(), err.c_str());
            }
            // 跳过这一路，继续翻下一页
            if (mode == LibraryMode::Category) AdvanceCategoryPage();
            if (fails >= 4) break;
            if (!gone404) Sleep(250);
            continue;
        }

        // 这一页已经取到，翻到下一个源（category?page=N → tag?page=N → category?page=N+1）
        if (themeMode) {
            std::lock_guard<std::mutex> lk(mu_);
            ++resPage_;
        } else if (mode == LibraryMode::Category) {
            AdvanceCategoryPage();
        }

        std::vector<ItemPtr> added;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (generation_ != gen) {
                loading_ = false;
                return;
            }
            for (const auto& r : raw) {
                if (seen_.count(r.id)) continue;
                seen_.insert(r.id);
                ItemPtr it = MakeItem(r);
                it->favorite = favMap_.find(r.id) != favMap_.end();
                items_.push_back(it);
                added.push_back(it);
            }
            if (added.empty()) ++emptyStreak_;
            else emptyStreak_ = 0;
        }

        for (const auto& it : added) EnqueueThumb(it);

        if (!added.empty()) {
            loading_ = false;
            LogMsg(L"列表新增 %d 张，累计 %d 张", (int)added.size(), (int)Count());
            PostUpdate();
            return;
        }

        ++attempts;
        if (attempts >= 6) break;
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!resSlug_.empty()) {
            if (resPage_ > 40 || fails >= 4 || emptyStreak_ >= 8) noMore_ = true;
        } else if (mode == LibraryMode::Category) {
            // 分类是无限翻页：只有两路都翻到底、或连续取不到内容、或页码超过站点上限，才算真的到底
            if ((catDead_ && tagDead_) || fails >= 4 || catPage_ > 40 || emptyStreak_ >= 8) noMore_ = true;
        } else if (emptyStreak_ >= 4) {
            // 只有连续多个源都没有新内容，才认为真的到底；网络失败不算
            noMore_ = true;
        }
    }
    loading_ = false;
    PostUpdate();
}

// ── 缩略图下载 ──────────────────────────────────────────────────────────

void Library::EnqueueThumb(const ItemPtr& it) {
    if (!it || stopping_.load()) return;
    if (it->thumbState.load() != 0) return;
    if (FileExists(it->thumbFile)) {
        it->thumbState = 2;
        return;
    }
    if (it->thumbUrl.empty()) {
        it->thumbState = 3;
        return;
    }
    it->thumbState = 1;
    {
        std::lock_guard<std::mutex> lk(jobMu_);
        thumbQueue_.push_back(it);
    }
    jobCv_.notify_all();
}

void Library::ThumbWorkerMain() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    while (true) {
        ItemPtr it;
        {
            std::unique_lock<std::mutex> lk(jobMu_);
            jobCv_.wait(lk, [this] { return stopFlag_ || !thumbQueue_.empty(); });
            if (stopFlag_) break;
            if (thumbQueue_.empty()) continue;
            it = thumbQueue_.front();
            thumbQueue_.pop_front();
        }
        if (!it) continue;

        if (FileExists(it->thumbFile)) {
            it->thumbState = 2;
            if (notify_) PostMessageW(notify_, WM_THUMB_READY, 0, 0);
            continue;
        }
        if (it->thumbUrl.empty()) {
            it->thumbState = 3;
            continue;
        }

        std::wstring url = std::wstring(kBase) + Utf8ToWide(it->thumbUrl);
        std::wstring part = it->thumbFile + L".part";

        bool ok = false;
        {
            DeleteFileW(part.c_str());
            net::Response res;
            std::wstring err;
            bool got = net::FetchToFile(url, std::wstring(kBase) + L"/", nullptr, part, res, err);
            if (got && res.status >= 200 && res.status < 300 && IsWebpOrImage(part)) {
                if (MoveFileExW(part.c_str(), it->thumbFile.c_str(), MOVEFILE_REPLACE_EXISTING)) ok = true;
            }
            if (!ok) DeleteFileW(part.c_str());
        }

        if (ok) {
            it->thumbState = 2;
            if (notify_) PostMessageW(notify_, WM_THUMB_READY, 0, 0);
            continue;
        }

        // 失败：最多再试 3 次，仍拿不到缩略图就把这张从列表里摘掉
        int tries = it->thumbTries.fetch_add(1) + 1;
        if (tries < 3 && !stopping_.load()) {
            LogMsg(L"缩略图下载失败(%d/3)，稍后重试 id=%d", tries, it->id);
            it->thumbState = 0;
            {
                std::lock_guard<std::mutex> lk(jobMu_);
                thumbQueue_.push_back(it);
            }
            jobCv_.notify_all();
            Sleep(700);
        } else {
            it->thumbState = 4;
            LogMsg(L"缩略图下载失败 3 次，从列表移除 id=%d %s", it->id, url.c_str());
            RemoveItemById(it->id);
            if (notify_) {
                PostMessageW(notify_, WM_THUMB_READY, 0, 0);
                PostMessageW(notify_, WM_LIB_UPDATED, 0, 0);
            }
        }
    }
    CoUninitialize();
}

// ── 收藏壁纸本地备份 ────────────────────────────────────────────────────

std::wstring Library::FavPath(int id) const {
    wchar_t name[64];
    swprintf_s(name, L"fav_%d.jpg", id);
    return PathJoin(favDir_, name);
}

void Library::PruneFavFiles() {
    std::set<int> keep;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& kv : favMap_) keep.insert(kv.first);
    }
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(PathJoin(favDir_, L"fav_*.jpg").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name.rfind(L"fav_", 0) != 0) continue;
        int id = (int)wcstol(name.c_str() + 4, nullptr, 10);
        if (id <= 0 || keep.count(id)) continue;
        std::wstring p = PathJoin(favDir_, name);
        if (DeleteFileW(p.c_str())) LogMsg(L"清理未收藏的本地图片 %s", p.c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void Library::EnqueueFav(const ItemPtr& it) {
    if (!it || stopping_.load()) return;
    if (it->favFile.empty()) it->favFile = FavPath(it->id);
    if (FileExists(it->favFile)) {
        it->favState = 2;
        return;
    }
    int expect = 0;
    if (!it->favState.compare_exchange_strong(expect, 1)) return;
    {
        std::lock_guard<std::mutex> lk(favMu_);
        favQueue_.push_back(it);
    }
    favCv_.notify_one();
}

void Library::FavWorkerMain() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    while (true) {
        ItemPtr it;
        {
            std::unique_lock<std::mutex> lk(favMu_);
            favCv_.wait(lk, [this] { return favStop_ || !favQueue_.empty(); });
            if (favStop_) break;
            it = favQueue_.front();
            favQueue_.pop_front();
        }
        if (!it) continue;
        if (FileExists(it->favFile)) {
            it->favState = 2;
            if (notify_) PostMessageW(notify_, WM_FAV_READY, 0, 0);
            continue;
        }

        std::wstring err;
        bool ok = false;
        for (int attempt = 0; attempt < 2 && !ok; ++attempt) {
            if (attempt > 0) Sleep(400);
            ok = WallpaperFeed::Download(it->id,
                                         WallpaperFeed::DetailReferer(it->href, it->id),
                                         1920, 1080, it->favFile, err);
        }
        it->favState = ok ? 2 : 3;
        if (ok) LogMsg(L"收藏壁纸已保存到本地 id=%d", it->id);
        else LogMsg(L"收藏壁纸保存失败 id=%d: %s", it->id, err.c_str());
        if (notify_) PostMessageW(notify_, WM_FAV_READY, 0, 0);
    }
    CoUninitialize();
}

void Library::EnsureFavFiles() {
    std::vector<ItemPtr> list;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (int id : favOrder_) {
            auto f = favMap_.find(id);
            if (f != favMap_.end() && !FileExists(f->second->favFile)) list.push_back(f->second);
        }
    }
    for (const auto& it : list) EnqueueFav(it);
    if (!list.empty()) LogMsg(L"收藏壁纸待下载到本地 %d 张", (int)list.size());
}

size_t Library::FavFileCount() const {
    size_t n = 0;
    WIN32_FIND_DATAW fd{};
    std::wstring pattern = PathJoin(favDir_, L"fav_*.jpg");
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ++n;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

uint64_t Library::FavDirSize() const { return DirSize(favDir_); }

// ── 收藏 ────────────────────────────────────────────────────────────────

void Library::LoadFavoritesInternal() {
    std::string text;
    HANDLE f = CreateFileW(favPath_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    if (sz.QuadPart > 0 && sz.QuadPart < 4 * 1024 * 1024) {
        text.resize((size_t)sz.QuadPart);
        DWORD read = 0;
        ReadFile(f, text.data(), (DWORD)text.size(), &read, nullptr);
        text.resize(read);
    }
    CloseHandle(f);

    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        size_t e = text.find('\n', start);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(start, e - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
        start = e + 1;
    }

    std::lock_guard<std::mutex> lk(mu_);
    favMap_.clear();
    favOrder_.clear();

    for (const auto& line : lines) {
        std::vector<std::string> parts;
        size_t p = 0;
        while (true) {
            size_t tab = line.find('\t', p);
            if (tab == std::string::npos) {
                parts.push_back(line.substr(p));
                break;
            }
            parts.push_back(line.substr(p, tab - p));
            p = tab + 1;
        }
        if (parts.size() < 6) continue;

        ItemPtr it = std::make_shared<Item>();
        it->id = atoi(parts[0].c_str());
        it->w = atoi(parts[1].c_str());
        it->h = atoi(parts[2].c_str());
        it->thumbUrl = parts[3];
        it->href = parts[4];
        it->title = parts[5];
        if (it->id <= 0) continue;
        if (it->w <= 0) it->w = 1920;
        if (it->h <= 0) it->h = 1080;
        it->thumbFile = ThumbPath(it->id);
        it->favFile = FavPath(it->id);
        it->favorite = true;
        it->thumbState = FileExists(it->thumbFile) ? 2 : (it->thumbUrl.empty() ? 3 : 0);
        it->favState = FileExists(it->favFile) ? 2 : 0;

        favMap_[it->id] = it;
        favOrder_.push_back(it->id);
    }
    LogMsg(L"收藏夹载入 %d 张", (int)favOrder_.size());
}

void Library::ReloadFavorites() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        favMap_.clear();
        favOrder_.clear();
    }
    LoadFavoritesInternal();
    PruneFavFiles();

    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& it : items_) {
        it->favorite = favMap_.find(it->id) != favMap_.end();
    }
}

void Library::ToggleFavorite(const ItemPtr& it) {
    if (!it) return;
    bool now = !it->favorite.load();
    it->favorite = now;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (now) {
            favMap_[it->id] = it;
            favOrder_.push_back(it->id);
        } else {
            favMap_.erase(it->id);
            favOrder_.erase(std::remove(favOrder_.begin(), favOrder_.end(), it->id),
                            favOrder_.end());
        }
    }
    SaveFavorites();

    if (now) {
        EnqueueFav(it);
    } else {
        // 取消收藏时把本地那份高清备份也删掉，避免白白占空间
        std::wstring f = it->favFile.empty() ? FavPath(it->id) : it->favFile;
        bool existed = FileExists(f);
        LogMsg(L"取消收藏 id=%d 本地文件=%s 存在=%d", it->id, f.c_str(), existed ? 1 : 0);
        if (existed) {
            if (DeleteFileW(f.c_str())) {
                LogMsg(L"已删除收藏本地文件 %s", f.c_str());
            } else {
                LogMsg(L"删除收藏文件失败(%lu) %s", GetLastError(), f.c_str());
            }
        }
        it->favState = 0;
    }

    if (mode_ == LibraryMode::Favorites) {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<ItemPtr> list;
        for (int id : favOrder_) {
            auto f = favMap_.find(id);
            if (f != favMap_.end()) list.push_back(f->second);
        }
        items_.swap(list);
        noMore_ = true;
    }
    PostUpdate();
}

// 收藏一张「当前正在桌面上的壁纸」：它可能不在当前列表里，所以单独造一条记录
bool Library::AddFavorite(const WfItem& src) {
    if (src.id <= 0) return false;
    if (IsFavorite(src.id)) return false;

    ItemPtr it = MakeItem(src);
    it->favorite = true;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (favMap_.count(it->id)) return false;
        favMap_[it->id] = it;
        favOrder_.push_back(it->id);
    }
    SaveFavorites();
    EnqueueFav(it);
    LogMsg(L"收藏当前壁纸 id=%d", it->id);

    if (mode_ == LibraryMode::Favorites) {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<ItemPtr> list;
        for (int id : favOrder_) {
            auto f = favMap_.find(id);
            if (f != favMap_.end()) list.push_back(f->second);
        }
        items_.swap(list);
        noMore_ = true;
    }
    PostUpdate();
    return true;
}

void Library::SaveFavorites() {
    std::string text;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (int id : favOrder_) {
            auto f = favMap_.find(id);
            if (f == favMap_.end()) continue;
            const auto& it = f->second;
            text += std::to_string(it->id);
            text += '\t';
            text += std::to_string(it->w);
            text += '\t';
            text += std::to_string(it->h);
            text += '\t';
            text += it->thumbUrl;
            text += '\t';
            text += it->href;
            text += '\t';
            text += it->title;
            text += '\n';
        }
    }
    if (!WriteAllBytes(favPath_, text.data(), text.size())) {
        LogMsg(L"收藏文件写入失败: %s", favPath_.c_str());
    }
}
