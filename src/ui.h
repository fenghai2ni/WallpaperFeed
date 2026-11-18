#pragma once
#include "common.h"
#include "config.h"
#include "desk.h"
#include "library.h"

#include <gdiplus.h>
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class App;

// 主界面窗口（自绘，纯 Win32 + GDI+）
class MainWindow {
public:
    static const UINT WM_UI_APPLY_DONE = WM_APP + 20;
    static const UINT WM_UI_PREVIEW_READY = WM_APP + 21;

    MainWindow();
    ~MainWindow();

    bool Create(HINSTANCE inst, App* app, Library* lib, Config* cfg,
                const std::wstring& dataDir, const std::wstring& configPath,
                const std::wstring& cacheDir, const std::wstring& srcDir,
                const std::wstring& curDir);

    void Show();
    void Hide();
    void Toggle();
    bool IsVisible() const;
    HWND Handle() const { return hwnd_; }

    void OnResolutionChanged();   // 托盘里改了图片规格后同步当前列表

    void OnLibraryUpdated();
    void OnThumbReady();

    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    enum class Page { Discover, Category, Favorites, Settings };
    enum class ApplyKind { Monitor, Span, Clone, ModeDefault };

    struct CardLayout {
        ItemPtr item;
        RECT rc{};
        int imgH = 0;
    };
    struct Hit {
        RECT rc;
        int id;
        int clipTop = 0;   // >0 表示这个热区属于可滚动内容区，y 小于该值时不算命中
    };
    struct CachedImage {
        std::unique_ptr<std::vector<uint8_t>> buf;
        std::unique_ptr<Gdiplus::Bitmap> bmp;
        uint64_t lastUse = 0;
    };

    // ── 尺寸/字体 ──
    int S(int v) const { return (int)(v * dpi_ + 0.5); }
    void UpdateDpi();
    void InitFonts();

    // ── 布局与绘制 ──
    void Relayout();
    void Paint(HDC hdc);
    void DrawSidebar(Gdiplus::Graphics& g, int W, int H);
    void DrawHeader(Gdiplus::Graphics& g, int W, int H);
    void DrawCategoryRow(Gdiplus::Graphics& g, int W, int H);
    void DrawGrid(Gdiplus::Graphics& g, int W, int H, int top);
    void DrawCard(Gdiplus::Graphics& g, int index, bool hover);
    void DrawSettings(Gdiplus::Graphics& g, int W, int H);
    void DrawToast(Gdiplus::Graphics& g, int W, int H);
    void DrawScrollbar(Gdiplus::Graphics& g, int W, int H, int top);
    void DrawPreview(Gdiplus::Graphics& g, int W, int H);

    int GridTop(int W) const;
    int ContentLeft() const { return S(196) + S(26); }
    int ContentRight(int W) const { return W - S(26); }
    bool InGridArea(POINT pt) const;

    Gdiplus::Bitmap* GetImage(const ItemPtr& item);
    void EvictImages();
    void ReleaseMemory();

    // ── 交互 ──
    void SwitchPage(Page p);
    void SwitchCategory(int index);
    void OnClick(POINT pt);
    void OnCardAction(int index, int sub);
    void OnSettingClick(int id);
    void ToggleFavorite(const ItemPtr& item);
    void ScrollBy(int delta);
    void Toast(const std::wstring& text, int ms = 2600);
    void RefreshScreens();
    void RefreshCache();
    int HitTest(POINT pt) const;

    // ── 应用与预览 ──
    void ApplyItem(const ItemPtr& item, ApplyKind kind, int monitor);
    void OpenPreview(int index);
    void ClosePreview();
    void FinishPreview();
    bool ApplyOnBackground(const ItemPtr& item, ApplyKind kind, int monitor,
                           const Config& cfg, std::wstring& msg);

    // ── 窗口过程 ──
    static LRESULT CALLBACK WndProcStatic(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    App* app_ = nullptr;
    Library* lib_ = nullptr;
    Config* cfg_ = nullptr;

    std::wstring dataDir_, configPath_, cacheDir_, srcDir_, curDir_;

    Page page_ = Page::Discover;
    std::vector<ItemPtr> items_;
    std::vector<CardLayout> cards_;
    std::vector<ScreenInfo> screens_;
    int screenCount_ = 1;

    int scrollY_ = 0;
    int contentH_ = 0;
    int maxScroll_ = 0;

    int hoverCard_ = -1;
    int hoverHit_ = 0;
    POINT mouse_{};
    bool mouseIn_ = false;
    bool tracking_ = false;

    std::wstring catSlug_ = L"landscapes";
    std::wstring catLabel_ = L"自然风景";

    std::vector<Hit> hits_;

    std::map<int, CachedImage> images_;
    uint64_t useCounter_ = 0;
    int imgBudget_ = 0;

    std::unique_ptr<Gdiplus::FontFamily> famUI_, famSym_;
    std::unique_ptr<Gdiplus::Font> fPage_, fSection_, fCard_, fSmall_, fNav_, fTitle_, fSym_;

    double dpi_ = 1.0;
    bool loadedOnce_ = false;

    std::wstring toast_;
    uint64_t toastUntil_ = 0;

    uint64_t cacheBytes_ = 0;
    int tickCount_ = 0;

    bool scrolling_ = false;
    int scrollDragOffset_ = 0;

    // 预览
    bool previewOpen_ = false;
    bool previewLoading_ = false;
    ItemPtr previewItem_;
    std::unique_ptr<std::vector<uint8_t>> previewBuf_;
    std::unique_ptr<Gdiplus::Bitmap> previewBmp_;
    std::thread previewThread_;

    // 应用
    bool applying_ = false;
    std::thread applyThread_;
    std::wstring applyResult_;
    bool applyOk_ = false;

    std::atomic<bool> shuttingDown_{false};
};
