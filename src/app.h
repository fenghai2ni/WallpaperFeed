#pragma once
#include "common.h"
#include "config.h"
#include "desk.h"
#include "wf.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

class MainWindow;
class Library;

class App {
public:
    explicit App(HINSTANCE hInst);
    ~App();

    bool Init(bool launchedAtStartup);
    void Run();
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    Config& ConfigRef() { return cfg_; }
    void SaveConfig();
    void NotifySettingsChanged();
    void NotifyManualApply();
    void OpenMainWindow();

    uint64_t CacheBytes() const;              // 壁纸数据总占用（含缩略图与收藏备份）
    bool CheckStorage(bool force = false);    // 按设置清理，返回是否有清理动作

private:
    // 托盘
    void AddTrayIcon();
    void RemoveTrayIcon();
    void UpdateTrayTip();
    void ShowTip(const std::wstring& title, const std::wstring& text, DWORD flags = NIIF_INFO);

    // 菜单
    void ShowMenu(POINT pt);
    void OnCommand(UINT id);

    // 切换
    void StartChange(bool manual);
    bool PerformChange(const Config& cfg, std::wstring& resultMsg);
    bool GetItems(const Config& cfg, std::vector<WfItem>& items, std::wstring& err);
    void BuildCandidates(const std::vector<WfItem>& pool, size_t want,
                         std::vector<WfItem>& out);
    void ScheduleNext(bool ok);
    void CleanupCache();

    static int  RequestWidth(const WfItem& it, int needW, int needH, int capWidth, int& reqH);
    std::wstring CurrentModeLabel() const;

    HINSTANCE hInst_ = nullptr;
    HWND hwnd_ = nullptr;
    HICON icon_ = nullptr;
    NOTIFYICONDATAW nid_{};

    Config cfg_;
    RecentIds recent_;

    std::wstring dataDir_;
    std::wstring cacheDir_;
    std::wstring srcDir_;
    std::wstring curDir_;
    std::wstring configPath_;

    CRITICAL_SECTION resultCs_{};
    bool resultOk_ = false;
    std::wstring resultMsg_;
    std::vector<std::wstring> currentFiles_;

    bool busy_ = false;
    std::thread worker_;
    std::atomic<bool> shuttingDown_{false};

    std::unique_ptr<Library> lib_;
    MainWindow* ui_ = nullptr;

    uint64_t nextChangeTick_ = 0;
    uint64_t lastChangeTick_ = 0;
    uint64_t lastStorageCheck_ = 0;
    uint64_t lastStorageWarn_ = 0;

    // 列表缓存
    std::mutex cacheMutex_;
    std::vector<WfItem> cachedItems_;
    std::wstring cachedUrl_;
    uint64_t cachedTick_ = 0;

    int lastScreenCount_ = 0;
};
