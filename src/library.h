#pragma once
#include "common.h"
#include "wf.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

// 一张壁纸的界面数据
struct Item {
    int id = 0;
    int w = 0, h = 0;                 // 原图尺寸
    std::string title;                // UTF-8
    std::string href;                 // /wallpaper/<id>-<slug>
    std::string thumbUrl;             // 站点缩略图相对地址
    std::wstring thumbFile;           // 本地缩略图缓存路径
    std::atomic<int> thumbState{0};   // 0 待下载 / 1 下载中 / 2 就绪 / 3 空地址 / 4 放弃
    std::atomic<int> thumbTries{0};   // 缩略图下载失败次数，满 3 次就从列表移除
    std::wstring favFile;             // 收藏图本地文件（高清）
    std::atomic<int> favState{0};     // 0 未下载 / 1 下载中 / 2 就绪 / 3 失败
    std::atomic<bool> favorite{false};

    Item() {}
};

using ItemPtr = std::shared_ptr<Item>;

enum class LibraryMode { Discover, Category, Favorites };

class Library {
public:
    static const UINT WM_LIB_UPDATED = WM_APP + 10;
    static const UINT WM_THUMB_READY = WM_APP + 11;
    static const UINT WM_FAV_READY = WM_APP + 12;

    ~Library();

    void Init(const std::wstring& dataDir, HWND notifyWnd);
    void Shutdown();

    void SetMode(LibraryMode mode, const std::wstring& categorySlug = L"",
                 const std::wstring& resolutionSlug = L"");
    LibraryMode Mode() const { return mode_; }
    std::wstring CategorySlug() const { return categorySlug_; }

    void RequestMore();
    bool Loading() const { return loading_.load(); }
    bool NoMore() const { return noMore_.load(); }

    std::vector<ItemPtr> Snapshot() const;
    size_t Count() const;

    void ReloadFavorites();
    void ToggleFavorite(const ItemPtr& it);
    bool IsFavorite(int id) const;
    size_t FavoriteCount() const;

    void EnqueueThumb(const ItemPtr& it);
    std::wstring ThumbPath(int id) const;

    void EnqueueFav(const ItemPtr& it);
    void PruneFavFiles();
    std::wstring FavPath(int id) const;
    std::wstring FavDir() const { return favDir_; }
    size_t FavFileCount() const;
    uint64_t FavDirSize() const;

    std::wstring DataDir() const { return dataDir_; }

private:
    void ListWorkerMain();
    void ThumbWorkerMain();
    void FavWorkerMain();
    void EnsureFavFiles();
    void DoLoadMore();
    std::wstring NextSourceUrl();
    void AdvanceCategoryPage();
    void RemoveItemById(int id);
    void EnsureSources();
    void FetchTags();
    ItemPtr MakeItem(const WfItem& item);
    void SaveFavorites();
    void LoadFavoritesInternal();
    ItemPtr FindFavorite(int id) const;
    void PostUpdate();

    HWND notify_ = nullptr;
    std::wstring dataDir_;
    std::wstring thumbDir_;
    std::wstring favDir_;
    std::wstring favPath_;

    mutable std::mutex mu_;
    std::vector<ItemPtr> items_;
    std::vector<int> favOrder_;
    std::map<int, ItemPtr> favMap_;
    std::set<int> seen_;
    std::vector<std::wstring> sources_;
    size_t srcIdx_ = 0;
    std::vector<std::wstring> catSources_;
    size_t catIdx_ = 0;
    int catPage_ = 1;    // 分类翻页：category/tag 共用的页码
    int catTurn_ = 0;    // 0 取 /category/<slug>?page=N，1 取 /tag/<slug>?page=N
    bool catDead_ = false;  // /category 已经翻到底（404）
    bool tagDead_ = false;  // /tag 已经翻到底（404）
    int emptyStreak_ = 0;
    int generation_ = 0;
    LibraryMode mode_ = LibraryMode::Discover;
    std::wstring categorySlug_ = L"landscapes";
    std::wstring resSlug_;          // 屏幕尺寸规格，非空时走 /theme/<slug>
    int resPage_ = 1;
    std::vector<std::wstring> tags_;

    std::atomic<bool> loading_{false};
    std::atomic<bool> noMore_{false};
    std::atomic<bool> stopping_{false};

    std::thread listThread_;
    std::vector<std::thread> thumbThreads_;
    std::thread favThread_;

    std::mutex jobMu_;
    std::condition_variable jobCv_;
    std::deque<ItemPtr> thumbQueue_;

    std::mutex favMu_;
    std::condition_variable favCv_;
    std::deque<ItemPtr> favQueue_;
    bool favStop_ = false;
    bool stopFlag_ = false;
    bool listRequest_ = false;
    bool hasListRequest_ = false;
};
