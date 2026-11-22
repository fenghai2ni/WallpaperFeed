#include "ui.h"
#include "app.h"
#include "wic.h"
#include "imaging.h"
#include "wf.h"

#include <dwmapi.h>
#include <psapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

namespace {

// 命中 id
const int HIT_NAV_BASE = 1000;
const int HIT_MODE_BASE = 1100;
const int HIT_INTERVAL_BASE = 1200;
const int HIT_AUTO_TOGGLE = 1210;
const int HIT_AUTOSTART = 1300;
const int HIT_TIPS = 1301;
const int HIT_QUALITY_BASE = 1400;
const int HIT_OPEN_CACHE = 1500;
const int HIT_OPEN_CONFIG = 1501;
const int HIT_CLEAR_THUMBS = 1502;
const int HIT_AUTOCLEAN = 1310;
const int HIT_KEEPFAV = 1311;
const int HIT_KEEPDAYS_BASE = 1312;
const int HIT_MAXSIZE_BASE = 1320;
const int HIT_CLEAN_NOW = 1330;
const int HIT_REFRESH = 1600;
const int HIT_SCROLL_TRACK = 1610;
const int HIT_CAT_BASE = 2000;
const int HIT_RES_BASE = 2200;         // 屏幕尺寸规格（0 = 全部）
const int HIT_CARD_BASE = 5000;        // + idx*10 + sub
const int HIT_CARD_OPEN_BASE = 30000;  // + idx
const int HIT_PREVIEW_CLOSE = 40000;
const int HIT_PREVIEW_MODE = 40001;
const int HIT_PREVIEW_FAV = 40002;
const int HIT_PREVIEW_OPEN = 40003;
const int HIT_PREVIEW_MON_BASE = 40100;
const int HIT_PREVIEW_SPAN = 40500;
const int HIT_PREVIEW_CLONE = 40501;

const int kIntervals[8] = {15, 30, 60, 120, 240, 480, 720, 1440};
const wchar_t* kIntervalLabels[8] = {L"15 分钟", L"30 分钟", L"1 小时", L"2 小时",
                                     L"4 小时", L"8 小时", L"12 小时", L"24 小时"};

inline Color Clr(int a, int r, int g, int b) { return Color(a, r, g, b); }

inline Rect TR(const RECT& r) { return Rect(r.left, r.top, r.right - r.left, r.bottom - r.top); }

void AddRound(GraphicsPath& p, const Rect& r, int rad) {
    int d = rad * 2;
    if (d > r.Width) d = r.Width;
    if (d > r.Height) d = r.Height;
    if (d < 2) { p.AddRectangle(r); return; }
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    p.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    p.CloseFigure();
}

void FillRound(Graphics& g, const Rect& r, int rad, Color c) {
    if (r.Width <= 0 || r.Height <= 0) return;
    GraphicsPath p;
    AddRound(p, r, rad);
    SolidBrush b(c);
    g.FillPath(&b, &p);
}

void StrokeRound(Graphics& g, const Rect& r, int rad, Color c, REAL width = 1.0f) {
    if (r.Width <= 0 || r.Height <= 0) return;
    GraphicsPath p;
    AddRound(p, r, rad);
    Pen pen(c, width);
    g.DrawPath(&pen, &p);
}

void DrawStr(Graphics& g, const std::wstring& s, const Rect& r, Font* f, Color c,
             StringAlignment ha = StringAlignmentNear,
             StringAlignment va = StringAlignmentCenter,
             bool ellipsis = true) {
    if (s.empty() || !f || r.Width <= 0 || r.Height <= 0) return;
    StringFormat fmt;
    fmt.SetAlignment(ha);
    fmt.SetLineAlignment(va);
    fmt.SetTrimming(ellipsis ? StringTrimmingEllipsisCharacter : StringTrimmingNone);
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush b(c);
    RectF rf((REAL)r.X, (REAL)r.Y, (REAL)r.Width, (REAL)r.Height);
    g.DrawString(s.c_str(), (INT)s.size(), f, rf, &fmt, &b);
}

int TextW(Graphics& g, const std::wstring& s, Font* f) {
    if (s.empty() || !f) return 0;
    RectF out;
    StringFormat fmt;
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    g.MeasureString(s.c_str(), (INT)s.size(), f, PointF(0, 0), &fmt, &out);
    return (int)out.Width + 2;
}

void DrawCover(Graphics& g, Bitmap* bmp, const Rect& dst, int rad) {
    if (!bmp || dst.Width <= 0 || dst.Height <= 0) return;
    GraphicsPath p;
    AddRound(p, dst, rad);
    // 必须存/还原裁剪状态：直接 SetClip + ResetClip 会把外层（列表滚动区）的裁剪一起清掉，
    // 卡片就会画到标题栏上去
    GraphicsState st = g.Save();
    g.SetClip(&p, CombineModeIntersect);
    int bw = (int)bmp->GetWidth();
    int bh = (int)bmp->GetHeight();
    if (bw > 0 && bh > 0) {
        double s = std::max((double)dst.Width / bw, (double)dst.Height / bh);
        double sw = dst.Width / s;
        double sh = dst.Height / s;
        double sx = (bw - sw) / 2.0;
        double sy = (bh - sh) / 2.0;
        g.DrawImage(bmp, RectF((REAL)dst.X, (REAL)dst.Y, (REAL)dst.Width, (REAL)dst.Height),
                    (REAL)sx, (REAL)sy, (REAL)sw, (REAL)sh, UnitPixel);
    }
    g.Restore(st);
}

void HeartPath(GraphicsPath& p, const Rect& r) {
    REAL x = (REAL)r.X, y = (REAL)r.Y, w = (REAL)r.Width, h = (REAL)r.Height;
    REAL cx = x + w * 0.5f;
    p.AddBezier(cx, y + h * 0.24f, x + w * 0.34f, y - h * 0.06f, x, y + h * 0.14f, x, y + h * 0.44f);
    p.AddBezier(x, y + h * 0.44f, x, y + h * 0.72f, cx, y + h * 0.86f, cx, y + h);
    p.AddBezier(cx, y + h, x + w, y + h * 0.86f, x + w, y + h * 0.72f, x + w, y + h * 0.44f);
    p.AddBezier(x + w, y + h * 0.44f, x + w, y + h * 0.14f, cx + w * 0.16f, y - h * 0.06f, cx, y + h * 0.24f);
}

void DrawHeart(Graphics& g, const Rect& r, bool filled, Color col) {
    GraphicsPath p;
    HeartPath(p, r);
    if (filled) {
        SolidBrush b(col);
        g.FillPath(&b, &p);
    }
    Pen pen(col, 1.6f);
    g.DrawPath(&pen, &p);
}

void IconMonitor(Graphics& g, const Rect& box, int number, Font* numFont, Color col) {
    REAL cx = box.X + box.Width / 2.0f;
    REAL cy = box.Y + box.Height / 2.0f;
    REAL w = box.Width * 0.56f;
    REAL h = box.Height * 0.40f;
    REAL x = cx - w / 2.0f;
    REAL y = cy - h / 2.0f - box.Height * 0.06f;
    Pen p(col, 1.3f);
    g.DrawRectangle(&p, x, y, w, h);
    g.DrawLine(&p, cx, y + h, cx, y + h + box.Height * 0.07f);
    g.DrawLine(&p, cx - w * 0.22f, y + h + box.Height * 0.09f,
               cx + w * 0.22f, y + h + box.Height * 0.09f);
    if (number > 0 && numFont) {
        wchar_t t[8];
        swprintf_s(t, L"%d", number);
        DrawStr(g, t, Rect((INT)x, (INT)y, (INT)w, (INT)h), numFont, col, StringAlignmentCenter);
    }
}

void IconSpan(Graphics& g, const Rect& box, Color col) {
    REAL cx = box.X + box.Width / 2.0f;
    REAL cy = box.Y + box.Height / 2.0f;
    REAL w = box.Width * 0.29f;
    REAL h = box.Height * 0.32f;
    Pen p(col, 1.3f);
    g.DrawRectangle(&p, cx - w - 2.0f, cy - h / 2.0f - 2.0f, w, h);
    g.DrawRectangle(&p, cx + 2.0f, cy - h / 2.0f - 2.0f, w, h);
    g.DrawLine(&p, cx - w - 2.0f, cy + h / 2.0f + 3.0f, cx + w + 2.0f, cy + h / 2.0f + 3.0f);
}

void IconClone(Graphics& g, const Rect& box, Color col) {
    REAL cx = box.X + box.Width / 2.0f;
    REAL cy = box.Y + box.Height / 2.0f;
    REAL w = box.Width * 0.32f;
    REAL h = box.Height * 0.32f;
    Pen p(col, 1.3f);
    g.DrawRectangle(&p, cx - w * 0.95f, cy - h * 0.95f, w, h);
    g.DrawRectangle(&p, cx - w * 0.15f, cy - h * 0.15f, w, h);
}

void IconLink(Graphics& g, const Rect& box, Color col) {
    REAL cx = box.X + box.Width / 2.0f;
    REAL cy = box.Y + box.Height / 2.0f;
    REAL w = box.Width * 0.30f;
    REAL h = box.Height * 0.30f;
    Pen p(col, 1.3f);
    g.DrawRectangle(&p, cx - w, cy - h * 0.4f, w * 1.3f, h * 1.1f);
    g.DrawLine(&p, cx + w * 0.1f, cy + h * 0.3f, cx + w * 1.1f, cy - h * 0.7f);
    g.DrawLine(&p, cx + w * 0.55f, cy - h * 0.7f, cx + w * 1.1f, cy - h * 0.7f);
    g.DrawLine(&p, cx + w * 1.1f, cy - h * 0.7f, cx + w * 1.1f, cy - h * 0.15f);
}

void IconNav(Graphics& g, const Rect& box, int kind, Color col) {
    REAL cx = box.X + box.Width / 2.0f;
    REAL cy = box.Y + box.Height / 2.0f;
    REAL s = box.Width * 0.5f;
    if (kind == 0) {
        REAL q = s * 0.40f;
        REAL gap = s * 0.14f;
        SolidBrush b(col);
        g.FillRectangle(&b, cx - gap - q, cy - gap - q, q, q);
        g.FillRectangle(&b, cx + gap, cy - gap - q, q, q);
        g.FillRectangle(&b, cx - gap - q, cy + gap, q, q);
        g.FillRectangle(&b, cx + gap, cy + gap, q, q);
    } else if (kind == 1) {
        Pen p(col, 1.5f);
        g.DrawLine(&p, cx - s * 0.62f, cy - s * 0.42f, cx + s * 0.62f, cy - s * 0.42f);
        g.DrawLine(&p, cx - s * 0.62f, cy, cx + s * 0.62f, cy);
        g.DrawLine(&p, cx - s * 0.62f, cy + s * 0.42f, cx + s * 0.18f, cy + s * 0.42f);
    } else if (kind == 2) {
        GraphicsPath p;
        HeartPath(p, Rect((INT)(cx - s * 0.52f), (INT)(cy - s * 0.48f),
                          (INT)(s * 1.04f), (INT)(s * 0.96f)));
        Pen pen(col, 1.5f);
        g.DrawPath(&pen, &p);
    } else {
        Pen p(col, 1.4f);
        REAL rr = s * 0.40f;
        g.DrawEllipse(&p, cx - rr, cy - rr, rr * 2, rr * 2);
        g.DrawEllipse(&p, cx - rr * 0.3f, cy - rr * 0.3f, rr * 0.6f, rr * 0.6f);
        for (int i = 0; i < 4; ++i) {
            REAL a = i * 3.1415926f / 2.0f;
            g.DrawLine(&p, cx + cosf(a) * rr, cy + sinf(a) * rr,
                       cx + cosf(a) * (rr + 3.5f), cy + sinf(a) * (rr + 3.5f));
        }
    }
}

// 与 App::RequestWidth 等价：按目标尺寸算一个合适的下载分辨率
int RequestWidthFor(const Item& it, int needW, int needH, int capWidth, int& reqH) {
    int ow = it.w > 0 ? it.w : 3840;
    int oh = it.h > 0 ? it.h : 2160;
    double cover = std::max((double)needW / ow, (double)needH / oh);
    if (cover <= 0.0) cover = 1.0;
    double s = std::min(1.0, cover * 1.15);
    if ((double)ow * s > (double)capWidth) s = (double)capWidth / ow;
    if (s > 1.0) s = 1.0;
    if (s < cover) s = std::min(1.0, cover);
    if (s <= 0.0) s = 1.0;
    int w = (int)std::lround(ow * s);
    int h = (int)std::lround(oh * s);
    if (w < 640) { double k = 640.0 / (double)w; w = 640; h = (int)std::lround(h * k); }
    if (h < 360) { double k = 360.0 / (double)h; h = 360; w = (int)std::lround(w * k); }
    reqH = h;
    return w;
}

const wchar_t* kNavLabels[4] = {L"发现", L"分类", L"收藏", L"设置"};

} // namespace

// ────────────────────────────────────────────────────────────────────────

MainWindow::MainWindow() {}

MainWindow::~MainWindow() {
    shuttingDown_ = true;
    if (applyThread_.joinable()) applyThread_.join();
    if (previewThread_.joinable()) previewThread_.join();
    if (hwnd_) {
        KillTimer(hwnd_, 1);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    images_.clear();
}

bool MainWindow::Create(HINSTANCE inst, App* app, Library* lib, Config* cfg,
                        const std::wstring& dataDir, const std::wstring& configPath,
                        const std::wstring& cacheDir, const std::wstring& srcDir,
                        const std::wstring& curDir) {
    inst_ = inst;
    app_ = app;
    lib_ = lib;
    cfg_ = cfg;
    dataDir_ = dataDir;
    configPath_ = configPath;
    cacheDir_ = cacheDir;
    srcDir_ = srcDir;
    curDir_ = curDir;

    dpi_ = (double)GetDpiForSystem() / 96.0;
    if (dpi_ < 1.0) dpi_ = 1.0;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainWindow::WndProcStatic;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"WallpaperFeedMainWnd";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    if (!RegisterClassExW(&wc)) return false;

    int w = (int)(1320 * dpi_);
    int h = (int)(880 * dpi_);    int sx = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    hwnd_ = CreateWindowExW(0, L"WallpaperFeedMainWnd", L"WallpaperFeed 壁纸",
                            WS_OVERLAPPEDWINDOW, sx, sy, w, h,
                            nullptr, nullptr, inst, this);
    if (!hwnd_) return false;

    UpdateDpi();
    InitFonts();

    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark));
    DwmSetWindowAttribute(hwnd_, 19, &dark, sizeof(dark));

    RefreshScreens();
    RefreshCache();
    Relayout();

    LogMsg(L"主界面已创建 %dx%d", w, h);
    return true;
}

void MainWindow::UpdateDpi() {
    dpi_ = 1.0;
    if (hwnd_) {
        UINT d = GetDpiForWindow(hwnd_);
        if (d >= 96) dpi_ = (double)d / 96.0;
    }
}

void MainWindow::InitFonts() {
    famUI_ = std::make_unique<FontFamily>(L"Microsoft YaHei UI");
    if (!famUI_->IsAvailable()) famUI_ = std::make_unique<FontFamily>(L"Segoe UI");
    famSym_ = std::make_unique<FontFamily>(L"Segoe UI Symbol");

    fPage_ = std::make_unique<Font>(famUI_.get(), (REAL)S(27), FontStyleBold, UnitPixel);
    fSection_ = std::make_unique<Font>(famUI_.get(), (REAL)S(16), FontStyleBold, UnitPixel);
    fTitle_ = std::make_unique<Font>(famUI_.get(), (REAL)S(15), FontStyleBold, UnitPixel);
    fNav_ = std::make_unique<Font>(famUI_.get(), (REAL)S(14), FontStyleRegular, UnitPixel);
    fCard_ = std::make_unique<Font>(famUI_.get(), (REAL)S(13), FontStyleRegular, UnitPixel);
    fSmall_ = std::make_unique<Font>(famUI_.get(), (REAL)S(12), FontStyleRegular, UnitPixel);
    fSym_ = std::make_unique<Font>(famSym_.get(), (REAL)S(10), FontStyleBold, UnitPixel);
}

void MainWindow::Show() {
    if (!hwnd_) return;
    if (!loadedOnce_) {
        loadedOnce_ = true;
        SwitchPage(Page::Discover);
    } else if (items_.empty()) {
        // Hide() 时把列表与位图都释放了，重新打开时把数据补回来（位图按需重新解码）
        items_ = lib_->Snapshot();
    }
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    RefreshScreens();
    RefreshCache();
    Relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
    SetTimer(hwnd_, 1, 120, nullptr);
}

void MainWindow::Hide() {
    if (!hwnd_) return;
    KillTimer(hwnd_, 1);
    ShowWindow(hwnd_, SW_HIDE);
    ReleaseMemory();
}

// 关闭主界面时把缩略图位图、卡片列表、预览缓存全部还给系统，
// 只保留壁纸库的数据（数量很小），下次打开再按需重新解码。
void MainWindow::ReleaseMemory() {
    if (previewThread_.joinable()) previewThread_.join();
    previewLoading_ = false;
    previewOpen_ = false;
    previewItem_.reset();
    previewBmp_.reset();
    previewBuf_.reset();

    images_.clear();
    cards_.clear();
    items_.clear();
    hits_.clear();
    hoverCard_ = -1;
    scrollY_ = 0;

    // 通知系统回收本进程的工作集（已释放的堆页立刻还给系统）
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
    LogMsg(L"主界面已关闭，内存已释放");
}

void MainWindow::OnResolutionChanged() {
    scrollY_ = 0;
    if (page_ == Page::Discover) lib_->SetMode(LibraryMode::Discover, L"", cfg_->resolution);
    else if (page_ == Page::Category) lib_->SetMode(LibraryMode::Category, catSlug_, cfg_->resolution);
    items_ = lib_->Snapshot();
    if (hwnd_) {
        Relayout();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::Toggle() {
    if (IsVisible()) Hide(); else Show();
}

bool MainWindow::IsVisible() const {
    return hwnd_ && IsWindowVisible(hwnd_);
}

void MainWindow::RefreshScreens() {
    std::vector<ScreenInfo> s;
    bool perMon = false;
    std::wstring err;
    if (desk::EnumScreens(s, perMon, err) && !s.empty()) {
        screens_ = s;
        screenCount_ = (int)s.size();
    } else {
        screenCount_ = 1;
    }
}

void MainWindow::RefreshCache() {
    if (app_) {
        cacheBytes_ = app_->CacheBytes();
    } else {
        cacheBytes_ = DirSize(cacheDir_) + DirSize(PathJoin(dataDir_, L"thumbs"));
    }
}

// ── 布局 ────────────────────────────────────────────────────────────────

int MainWindow::GridTop(int W) const {
    if (page_ == Page::Category) {
        int avail = ContentRight(W) - ContentLeft();
        int capW = S(108), gap = S(8);
        int per = (avail + gap) / (capW + gap);
        if (per < 1) per = 1;
        int resCount = (int)GetResolutions().size() + 1;   // 含「全部尺寸」
        int resRows = (resCount + per - 1) / per;
        size_t cats = GetCategories().size();
        int rows = (int)((cats + per - 1) / per);
        return S(96) + resRows * S(44) + S(6) + rows * S(44) + S(8);
    }
    return S(100);
}

void MainWindow::Relayout() {
    cards_.clear();
    if (!hwnd_) return;
    RECT cr;
    GetClientRect(hwnd_, &cr);
    int W = cr.right, H = cr.bottom;
    if (W <= 0 || H <= 0) return;

    int top = GridTop(W);
    int left = ContentLeft();
    int right = ContentRight(W);
    int avail = right - left;
    if (avail < S(200)) avail = S(200);

    int minCard = S(290);
    int cols = avail / minCard;
    if (cols < 1) cols = 1;
    if (cols > 5) cols = 5;
    int gap = S(16);
    int cardW = (avail - gap * (cols - 1)) / cols;

    std::vector<int> colY(cols, top);
    for (const auto& it : items_) {
        int idx = 0;
        for (int i = 1; i < cols; ++i) {
            if (colY[i] < colY[idx]) idx = i;
        }
        double ratio = (it->w > 0) ? (double)it->h / it->w : 0.5625;
        int imgH = (int)(cardW * ratio);
        if (imgH < (int)(cardW * 0.62)) imgH = (int)(cardW * 0.62);
        if (imgH > (int)(cardW * 1.45)) imgH = (int)(cardW * 1.45);
        int cardH = imgH + S(58);

        CardLayout c;
        c.item = it;
        c.rc.left = left + idx * (cardW + gap);
        c.rc.top = colY[idx];
        c.rc.right = c.rc.left + cardW;
        c.rc.bottom = c.rc.top + cardH;
        c.imgH = imgH;
        cards_.push_back(c);
        colY[idx] += cardH + gap;
    }

    int maxY = top;
    for (int y : colY) if (y > maxY) maxY = y;

    if (page_ == Page::Settings) {
        contentH_ = S(1110);
    } else {
        contentH_ = maxY + S(40);
    }

    int viewH = H - top;
    maxScroll_ = contentH_ - viewH;
    if (maxScroll_ < 0) maxScroll_ = 0;
    if (scrollY_ > maxScroll_) scrollY_ = maxScroll_;
    if (scrollY_ < 0) scrollY_ = 0;
}

// ── 绘制 ────────────────────────────────────────────────────────────────

void MainWindow::Paint(HDC hdc) {
    RECT cr;
    GetClientRect(hwnd_, &cr);
    int W = cr.right, H = cr.bottom;
    if (W <= 0 || H <= 0) return;

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);

    {
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);

        SolidBrush bg(Clr(255, 14, 17, 22));
        g.FillRectangle(&bg, 0, 0, W, H);

        hits_.clear();
        imgBudget_ = 4;

        DrawSidebar(g, W, H);
        DrawHeader(g, W, H);

        int top = GridTop(W);
        if (page_ == Page::Category) DrawCategoryRow(g, W, H);
        if (page_ == Page::Settings) {
            size_t h0 = hits_.size();
            DrawSettings(g, W, H);
            // 设置项也会跟着滚动，滚到标题栏下方后同样不该被点到
            for (size_t i = h0; i < hits_.size(); ++i) hits_[i].clipTop = top;
        } else {
            size_t h0 = hits_.size();
            DrawGrid(g, W, H, top);
            for (size_t i = h0; i < hits_.size(); ++i) hits_[i].clipTop = top;
        }
        DrawScrollbar(g, W, H, top);
        DrawToast(g, W, H);
        if (previewOpen_) DrawPreview(g, W, H);
    }

    BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void MainWindow::DrawSidebar(Graphics& g, int W, int H) {
    int sw = S(196);
    SolidBrush bg(Clr(255, 17, 21, 28));
    g.FillRectangle(&bg, 0, 0, sw, H);
    SolidBrush sep(Clr(255, 27, 32, 42));
    g.FillRectangle(&sep, sw - 1, 0, 1, H);

    DrawStr(g, L"WallpaperFeed", Rect(S(24), S(24), sw - S(44), S(26)),
            fTitle_.get(), Clr(255, 233, 238, 246), StringAlignmentNear, StringAlignmentNear, false);
    DrawStr(g, L"桌面壁纸工具", Rect(S(24), S(54), sw - S(44), S(18)),
            fSmall_.get(), Clr(255, 128, 139, 158), StringAlignmentNear, StringAlignmentNear, false);

    int ny = S(88);
    for (int i = 0; i < 4; ++i) {
        RECT r{S(10), ny, sw - S(10), ny + S(44)};
        bool active = ((int)page_ == i);
        if (active) {
            FillRound(g, TR(r), S(9), Clr(255, 27, 34, 49));
            SolidBrush bar(Clr(255, 59, 130, 246));
            g.FillRectangle(&bar, S(10), ny + S(11), S(3), S(22));
        }
        Color ic = active ? Clr(255, 96, 165, 250) : Clr(255, 120, 131, 150);
        IconNav(g, Rect(S(26), ny + S(14), S(16), S(16)), i, ic);
        DrawStr(g, kNavLabels[i], Rect(S(56), ny, sw - S(66), S(44)), fNav_.get(),
                active ? Clr(255, 236, 241, 249) : Clr(255, 156, 167, 186),
                StringAlignmentNear);
        hits_.push_back({r, HIT_NAV_BASE + i});
        ny += S(48);
    }

    int by = H - S(128);
    wchar_t buf[160];
    swprintf_s(buf, L"%d 台显示器", screenCount_);
    DrawStr(g, buf, Rect(S(24), by, sw - S(40), S(18)), fSmall_.get(),
            Clr(255, 116, 127, 146), StringAlignmentNear, StringAlignmentNear, false);
    swprintf_s(buf, L"收藏 %d 张", (int)lib_->FavoriteCount());
    DrawStr(g, buf, Rect(S(24), by + S(20), sw - S(40), S(18)), fSmall_.get(),
            Clr(255, 116, 127, 146), StringAlignmentNear, StringAlignmentNear, false);
    DrawStr(g, L"本地占用 " + FormatBytes(cacheBytes_), Rect(S(24), by + S(40), sw - S(34), S(18)),
            fSmall_.get(), Clr(255, 116, 127, 146), StringAlignmentNear, StringAlignmentNear, false);
    DrawStr(g, L"v1.5.1  ·  wallpaperfeed.com", Rect(S(24), by + S(64), sw - S(34), S(18)),
            fSmall_.get(), Clr(255, 86, 96, 114), StringAlignmentNear, StringAlignmentNear, false);
}

void MainWindow::DrawHeader(Graphics& g, int W, int H) {
    int x = ContentLeft();
    int r = ContentRight(W);

    std::wstring title, sub;
    switch (page_) {
    case Page::Discover:
        title = L"发现";
        if (!cfg_->resolution.empty()) {
            sub = L"屏幕尺寸 " + cfg_->ResolutionLabel() + L" · 向下滚动自动加载";
        } else {
            sub = L"每日更新的高清壁纸 · 向下滚动自动加载";
        }
        break;
    case Page::Category:
        title = L"分类";
        if (!cfg_->resolution.empty()) {
            sub = L"屏幕尺寸 " + cfg_->ResolutionLabel() + L" · 已加载 " +
                  std::to_wstring(items_.size()) + L" 张";
        } else {
            sub = catLabel_ + L" · 已加载 " + std::to_wstring(items_.size()) + L" 张";
        }
        break;
    case Page::Favorites:
        title = L"我的收藏";
        sub = L"共 " + std::to_wstring(items_.size()) + L" 张收藏";
        break;
    default:
        title = L"设置";
        sub = L"显示方式、自动更换与本地缓存";
        break;
    }

    DrawStr(g, title, Rect(x, S(22), r - x, S(36)), fPage_.get(),
            Clr(255, 236, 241, 249), StringAlignmentNear, StringAlignmentNear, false);
    DrawStr(g, sub, Rect(x, S(62), r - x - S(110), S(20)), fSmall_.get(),
            Clr(255, 130, 141, 160), StringAlignmentNear, StringAlignmentNear);

    if (page_ == Page::Discover || page_ == Page::Category) {
        RECT br{r - S(92), S(26), r, S(62)};
        bool hov = mouseIn_ && PtInRect(&br, mouse_);
        FillRound(g, TR(br), S(8), hov ? Clr(255, 40, 48, 62) : Clr(255, 27, 33, 44));
        StrokeRound(g, TR(br), S(8), Clr(255, 40, 47, 60), 1.0f);
        DrawStr(g, L"刷新", TR(br), fSmall_.get(), Clr(255, 190, 200, 216), StringAlignmentCenter);
        hits_.push_back({br, HIT_REFRESH});
    }
}

void MainWindow::DrawCategoryRow(Graphics& g, int W, int H) {
    int x0 = ContentLeft();
    int avail = ContentRight(W) - x0;
    int capW = S(108), capH = S(36), gap = S(8);
    int per = (avail + gap) / (capW + gap);
    if (per < 1) per = 1;

    int y = S(96);

    // ── 第一排：按站点「屏幕尺寸」分类筛选（/theme/<slug>）──
    const auto& resList = GetResolutions();
    int resCount = (int)resList.size() + 1;
    for (int i = 0; i < resCount; ++i) {
        int col = i % per;
        int row = i / per;
        RECT r{x0 + col * (capW + gap), y + row * S(44),
               x0 + col * (capW + gap) + capW, y + row * S(44) + capH};
        bool on = (i == 0) ? cfg_->resolution.empty()
                           : (cfg_->resolution == resList[i - 1].slug);
        FillRound(g, TR(r), S(18), on ? Clr(255, 26, 66, 96) : Clr(255, 24, 30, 40));
        if (on) StrokeRound(g, TR(r), S(18), Clr(255, 96, 165, 250), 1.0f);
        DrawStr(g, i == 0 ? L"全部尺寸" : resList[i - 1].label, TR(r), fSmall_.get(),
                on ? Clr(255, 219, 234, 254) : Clr(255, 138, 150, 170),
                StringAlignmentCenter);
        hits_.push_back({r, HIT_RES_BASE + i});
    }
    int rows = (resCount + per - 1) / per;
    y += rows * S(44) + S(6);

    // ── 内容分类 ──
    const auto& cats = GetCategories();
    for (size_t i = 0; i < cats.size(); ++i) {
        int col = (int)i % per;
        int row = (int)i / per;
        RECT r{x0 + col * (capW + gap), y + row * S(44),
               x0 + col * (capW + gap) + capW, y + row * S(44) + capH};
        // 选了尺寸规格时走尺寸图库，分类胶囊不参与高亮
        bool on = (catSlug_ == cats[i].slug) && cfg_->resolution.empty();
        RECT rr = r;
        FillRound(g, TR(rr), S(18), on ? Clr(255, 37, 71, 128) : Clr(255, 26, 32, 42));
        if (on) StrokeRound(g, TR(rr), S(18), Clr(255, 59, 130, 246), 1.0f);
        DrawStr(g, cats[i].label, TR(rr), fSmall_.get(),
                on ? Clr(255, 226, 236, 250) : Clr(255, 150, 161, 180),
                StringAlignmentCenter);
        hits_.push_back({r, HIT_CAT_BASE + (int)i});
    }
}

void MainWindow::DrawGrid(Graphics& g, int W, int H, int top) {
    int left = ContentLeft() - S(10);
    int right = ContentRight(W) + S(10);
    Region clip(Rect(left, top, right - left, H - top));
    g.SetClip(&clip);

    for (int i = 0; i < (int)cards_.size(); ++i) {
        RECT rc = cards_[i].rc;
        rc.top -= scrollY_;
        rc.bottom -= scrollY_;
        if (rc.bottom < top - S(20) || rc.top > H) continue;
        DrawCard(g, i, i == hoverCard_);
    }

    if (items_.empty()) {
        std::wstring msg;
        if (page_ == Page::Favorites) {
            msg = L"还没有收藏的壁纸，把喜欢的图点一下心形即可";
        } else if (lib_->Loading()) {
            msg = L"正在加载壁纸…";
        } else {
            msg = L"正在准备内容…";
        }
        DrawStr(g, msg, Rect(left, top + S(70), right - left, S(30)), fSection_.get(),
                Clr(255, 120, 131, 150), StringAlignmentCenter, StringAlignmentCenter, false);
    }

    if (!cards_.empty() && cards_.size() == items_.size()) {
        int y = contentH_ - scrollY_ + S(6);
        if (y < H && y > top) {
            std::wstring msg;
            if (lib_->Loading()) msg = L"正在加载更多…";
            else if (lib_->NoMore() && page_ != Page::Favorites) msg = L"—— 已经到底了 ——";
            if (!msg.empty()) {
                DrawStr(g, msg, Rect(left, y, right - left, S(26)), fSmall_.get(),
                        Clr(255, 108, 118, 138), StringAlignmentCenter, StringAlignmentCenter, false);
            }
        }
    }

    g.ResetClip();
}

void MainWindow::DrawCard(Graphics& g, int index, bool hover) {
    const CardLayout& c = cards_[index];
    RECT rc = c.rc;
    rc.top -= scrollY_;
    rc.bottom -= scrollY_;

    int rad = S(12);
    Rect card = TR(rc);
    FillRound(g, card, rad, hover ? Clr(255, 31, 37, 49) : Clr(255, 23, 28, 37));
    if (hover) StrokeRound(g, card, rad, Clr(255, 59, 130, 246), 1.5f);

    RECT imgR{rc.left + S(6), rc.top + S(6), rc.right - S(6), rc.bottom - S(56)};
    Rect img = TR(imgR);

    Bitmap* bmp = GetImage(c.item);
    if (bmp) {
        DrawCover(g, bmp, img, rad - S(4));
    } else {
        FillRound(g, img, rad - S(4), Clr(255, 31, 37, 48));
        int st = c.item->thumbState.load();
        DrawStr(g, (st == 3) ? L"缩略图不可用" : L"载入中…", img, fSmall_.get(),
                Clr(255, 104, 114, 134), StringAlignmentCenter, StringAlignmentCenter, false);
    }

    {
        Rect tr(rc.left + S(14), rc.bottom - S(52), rc.right - rc.left - S(28), S(22));
        DrawStr(g, Utf8ToWide(c.item->title), tr, fCard_.get(),
                Clr(255, 226, 232, 242), StringAlignmentNear);
    }

    wchar_t dim[64];
    swprintf_s(dim, L"%d × %d", c.item->w, c.item->h);
    {
        Rect dr(rc.left + S(14), rc.bottom - S(28), rc.right - rc.left - S(28), S(18));
        DrawStr(g, dim, dr, fSmall_.get(), Clr(255, 118, 129, 148), StringAlignmentNear);
    }

    hits_.push_back({rc, HIT_CARD_OPEN_BASE + index});

    if (!hover) return;

    // 收藏按钮
    RECT favR{imgR.right - S(36), imgR.top + S(10), imgR.right - S(10), imgR.top + S(36)};
    bool fav = c.item->favorite.load();
    FillRound(g, TR(favR), S(13), Clr(205, 10, 12, 17));
    DrawHeart(g, Rect(favR.left + S(7), favR.top + S(6), S(12), S(12)),
              fav, fav ? Clr(255, 255, 77, 103) : Clr(240, 255, 255, 255));
    hits_.push_back({favR, HIT_CARD_BASE + index * 10 + 0});

    // 操作条：按屏数决定按钮
    int n = screenCount_;
    int btnScreens = (n <= 1) ? 1 : std::min(n, 3);
    int btnCount = (n <= 1) ? 1 : (btnScreens + 2);
    int bs = S(32), gp = S(6);
    int barW = btnCount * bs + (btnCount - 1) * gp + S(14);
    int barH = bs + S(12);
    int cx = (imgR.left + imgR.right) / 2;
    int by = imgR.bottom - S(14) - barH;
    RECT barR{cx - barW / 2, by, cx + barW / 2, by + barH};
    FillRound(g, TR(barR), S(10), Clr(218, 9, 11, 15));

    int bx = barR.left + S(7);
    int bcy = barR.top + S(6);
    for (int k = 0; k < btnCount; ++k) {
        int sub;
        if (n <= 1) sub = 1;
        else if (k < btnScreens) sub = k + 1;
        else if (k == btnScreens) sub = n + 1;
        else sub = n + 2;

        RECT br{bx, bcy, bx + bs, bcy + bs};
        bool hov = mouseIn_ && PtInRect(&br, mouse_);
        if (hov) FillRound(g, TR(br), S(7), Clr(255, 44, 54, 71));
        Color ic = hov ? Clr(255, 96, 165, 250) : Clr(232, 226, 232, 245);

        if (n <= 1) {
            IconMonitor(g, TR(br), 0, nullptr, ic);
        } else if (k < btnScreens) {
            IconMonitor(g, TR(br), k + 1, fSym_.get(), ic);
        } else if (k == btnScreens) {
            IconSpan(g, TR(br), ic);
        } else {
            IconClone(g, TR(br), ic);
        }
        hits_.push_back({br, HIT_CARD_BASE + index * 10 + sub});
        bx += bs + gp;
    }
}

void MainWindow::DrawSettings(Graphics& g, int W, int H) {
    int x = ContentLeft();
    int w = ContentRight(W) - x;
    int top = GridTop(W);
    Region clip(Rect(x - S(12), top, w + S(24), H - top));
    g.SetClip(&clip);

    int y = top - scrollY_ + S(4);
    Color txt = Clr(255, 224, 231, 241);
    Color subC = Clr(255, 124, 135, 154);

    // 自动更换
    DrawStr(g, L"自动更换", Rect(x, y, w, S(22)), fSection_.get(), txt, StringAlignmentNear,
            StringAlignmentNear, false);
    y += S(30);
    {
        RECT swR{x, y + S(4), x + S(52), y + S(32)};
        bool on = cfg_->intervalMinutes > 0;
        FillRound(g, TR(swR), S(14), on ? Clr(255, 59, 130, 246) : Clr(255, 45, 52, 65));
        int k = S(26);
        int kx = on ? swR.right - k - S(3) : swR.left + S(3);
        FillRound(g, Rect(kx, swR.top + S(3), k, k), k / 2, Clr(255, 255, 255, 255));
        hits_.push_back({swR, HIT_AUTO_TOGGLE});

        std::wstring label = on ? L"已开启：每 " + std::to_wstring(cfg_->intervalMinutes) + L" 分钟换一张"
                                : L"已关闭（可手动点击托盘换图）";
        DrawStr(g, label, Rect(x + S(66), y, w - S(70), S(32)), fCard_.get(), subC,
                StringAlignmentNear);
        y += S(44);

        int ivW = S(78), ivH = S(36), ivG = S(8);
        int perRow = (w + ivG) / (ivW + ivG);
        if (perRow < 1) perRow = 1;
        if (perRow > 8) perRow = 8;
        for (int i = 0; i < 8; ++i) {
            int col = i % perRow, row = i / perRow;
            RECT r{x + col * (ivW + ivG), y + row * (ivH + ivG),
                   x + col * (ivW + ivG) + ivW, y + row * (ivH + ivG) + ivH};
            bool on = (cfg_->intervalMinutes == kIntervals[i]);
            RECT rr = r;
            FillRound(g, TR(rr), S(8), on ? Clr(255, 37, 71, 128) : Clr(255, 26, 32, 42));
            if (on) StrokeRound(g, TR(rr), S(8), Clr(255, 59, 130, 246), 1.0f);
            DrawStr(g, kIntervalLabels[i], TR(rr), fSmall_.get(),
                    on ? Clr(255, 226, 236, 250) : Clr(255, 146, 157, 176),
                    StringAlignmentCenter);
            hits_.push_back({r, HIT_INTERVAL_BASE + i});
        }
        int rows = (8 + perRow - 1) / perRow;
        y += rows * (ivH + ivG) + S(22);

        std::wstring modeName = (cfg_->mode == L"span") ? L"跨屏合一"
                              : (cfg_->mode == L"clone") ? L"复制到所有屏" : L"每屏不同";
        DrawStr(g, L"自动换图使用显示模式：" + modeName + L"，可在托盘菜单里切换",
                Rect(x, y, w, S(20)), fSmall_.get(), subC, StringAlignmentNear,
                StringAlignmentNear);
        y += S(30);
    }

    // 其他开关
    DrawStr(g, L"其他", Rect(x, y, w, S(22)), fSection_.get(), txt, StringAlignmentNear,
            StringAlignmentNear, false);
    y += S(30);
    {
        struct ToggleRow { const wchar_t* label; bool on; int id; };
        ToggleRow rows[3] = {
            {L"开机自动启动", cfg_->autostart, HIT_AUTOSTART},
            {L"切换后弹出托盘提示", cfg_->showTips, HIT_TIPS},
            {L"关闭主界面时留在托盘", true, 0},
        };
        for (int i = 0; i < 3; ++i) {
            RECT r{x, y, x + w, y + S(40)};
            DrawStr(g, rows[i].label, Rect(x, y, w - S(120), S(40)), fCard_.get(),
                    Clr(255, 214, 221, 233), StringAlignmentNear);
            RECT swR{x + w - S(52), y + S(6), x + w, y + S(34)};
            FillRound(g, TR(swR), S(14), rows[i].on ? Clr(255, 59, 130, 246) : Clr(255, 45, 52, 65));
            int k = S(26);
            int kx = rows[i].on ? swR.right - k - S(3) : swR.left + S(3);
            FillRound(g, Rect(kx, swR.top + S(3), k, k), k / 2, Clr(255, 255, 255, 255));
            if (rows[i].id) hits_.push_back({r, rows[i].id});
            y += S(46);
        }
        y += S(6);
    }

    // 壁纸质量
    DrawStr(g, L"壁纸质量", Rect(x, y, w, S(22)), fSection_.get(), txt, StringAlignmentNear,
            StringAlignmentNear, false);
    y += S(30);
    {
        int qualities[3] = {80, 90, 95};
        const wchar_t* labels[3] = {L"标准 80", L"高清 90", L"极清 95"};
        int qW = S(104), qH = S(38), qG = S(8);
        for (int i = 0; i < 3; ++i) {
            RECT r{x + i * (qW + qG), y, x + i * (qW + qG) + qW, y + qH};
            bool on = (cfg_->jpegQuality == qualities[i]);
            RECT rr = r;
            FillRound(g, TR(rr), S(8), on ? Clr(255, 37, 71, 128) : Clr(255, 26, 32, 42));
            if (on) StrokeRound(g, TR(rr), S(8), Clr(255, 59, 130, 246), 1.0f);
            DrawStr(g, labels[i], TR(rr), fSmall_.get(),
                    on ? Clr(255, 226, 236, 250) : Clr(255, 146, 157, 176),
                    StringAlignmentCenter);
            hits_.push_back({r, HIT_QUALITY_BASE + i});
        }
        y += qH + S(26);
    }

    // 存储管理
    DrawStr(g, L"存储管理", Rect(x, y, w, S(22)), fSection_.get(), txt, StringAlignmentNear,
            StringAlignmentNear, false);
    y += S(28);
    {
        std::wstring cur = L"当前占用 " + FormatBytes(cacheBytes_);
        if (cfg_->maxCacheMb > 0) {
            cur += L" / 上限 " + FormatBytes((uint64_t)cfg_->maxCacheMb * 1024ull * 1024ull);
        }
        DrawStr(g, cur, Rect(x, y, w - S(120), S(22)), fSmall_.get(), subC, StringAlignmentNear,
                StringAlignmentNear);
        {
            int tw = TextW(g, L"立即清理", fSmall_.get()) + S(30);
            RECT r{x + w - tw, y - S(5), x + w, y + S(27)};
            bool hov = mouseIn_ && PtInRect(&r, mouse_);
            FillRound(g, TR(r), S(8), hov ? Clr(255, 40, 48, 62) : Clr(255, 28, 34, 45));
            StrokeRound(g, TR(r), S(8), Clr(255, 40, 47, 60), 1.0f);
            DrawStr(g, L"立即清理", TR(r), fSmall_.get(), Clr(255, 190, 200, 216),
                    StringAlignmentCenter);
            hits_.push_back({r, HIT_CLEAN_NOW});
        }
        y += S(32);

        struct CleanRow { const wchar_t* label; bool on; int id; };
        CleanRow rows[2] = {
            {L"自动清理旧壁纸", cfg_->autoClean, HIT_AUTOCLEAN},
            {L"保留收藏的壁纸", cfg_->keepFavorites, HIT_KEEPFAV},
        };
        for (int i = 0; i < 2; ++i) {
            RECT r{x, y, x + w, y + S(38)};
            DrawStr(g, rows[i].label, Rect(x, y, w - S(110), S(38)), fCard_.get(),
                    Clr(255, 214, 221, 233), StringAlignmentNear);
            RECT swR{x + w - S(52), y + S(4), x + w, y + S(32)};
            FillRound(g, TR(swR), S(14),
                      rows[i].on ? Clr(255, 59, 130, 246) : Clr(255, 45, 52, 65));
            int k = S(26);
            int kx = rows[i].on ? swR.right - k - S(3) : swR.left + S(3);
            FillRound(g, Rect(kx, swR.top + S(3), k, k), k / 2, Clr(255, 255, 255, 255));
            hits_.push_back({r, rows[i].id});
            y += S(42);
        }

        {
            DrawStr(g, L"保留最近", Rect(x, y, S(76), S(32)), fCard_.get(),
                    Clr(255, 214, 221, 233), StringAlignmentNear);
            const int days[4] = {7, 30, 90, 0};
            const wchar_t* labels[4] = {L"7 天", L"30 天", L"90 天", L"永久"};
            int pw = S(76), ph = S(32), pg = S(8);
            int px = x + S(84);
            for (int i = 0; i < 4; ++i) {
                RECT r{px, y, px + pw, y + ph};
                bool on = (cfg_->keepDays == days[i]);
                RECT rr = r;
                FillRound(g, TR(rr), S(8), on ? Clr(255, 37, 71, 128) : Clr(255, 26, 32, 42));
                if (on) StrokeRound(g, TR(rr), S(8), Clr(255, 59, 130, 246), 1.0f);
                DrawStr(g, labels[i], TR(rr), fSmall_.get(),
                        on ? Clr(255, 226, 236, 250) : Clr(255, 146, 157, 176),
                        StringAlignmentCenter);
                hits_.push_back({r, HIT_KEEPDAYS_BASE + i});
                px += pw + pg;
            }
            y += S(42);
        }

        {
            DrawStr(g, L"最大占用", Rect(x, y, S(76), S(32)), fCard_.get(),
                    Clr(255, 214, 221, 233), StringAlignmentNear);
            const int mbs[5] = {256, 512, 1024, 2048, 0};
            const wchar_t* labels[5] = {L"256 MB", L"512 MB", L"1 GB", L"2 GB", L"不限"};
            int pw = S(82), ph = S(32), pg = S(8);
            int px = x + S(84);
            for (int i = 0; i < 5; ++i) {
                RECT r{px, y, px + pw, y + ph};
                bool on = (cfg_->maxCacheMb == mbs[i]);
                RECT rr = r;
                FillRound(g, TR(rr), S(8), on ? Clr(255, 37, 71, 128) : Clr(255, 26, 32, 42));
                if (on) StrokeRound(g, TR(rr), S(8), Clr(255, 59, 130, 246), 1.0f);
                DrawStr(g, labels[i], TR(rr), fSmall_.get(),
                        on ? Clr(255, 226, 236, 250) : Clr(255, 146, 157, 176),
                        StringAlignmentCenter);
                hits_.push_back({r, HIT_MAXSIZE_BASE + i});
                px += pw + pg;
            }
            y += S(44);
        }

        DrawStr(g, L"超过上限时会自动清理较早的壁纸，并通过托盘气泡提示",
                Rect(x, y, w, S(20)), fSmall_.get(), Clr(255, 104, 115, 134), StringAlignmentNear,
                StringAlignmentNear);
        y += S(34);
    }

    // 本地数据
    DrawStr(g, L"本地数据", Rect(x, y, w, S(22)), fSection_.get(), txt, StringAlignmentNear,
            StringAlignmentNear, false);
    y += S(30);
    {
        struct BtnDef { const wchar_t* label; int id; };
        BtnDef defs[3] = {
            {L"打开缓存目录", HIT_OPEN_CACHE},
            {L"打开配置文件", HIT_OPEN_CONFIG},
            {L"清理缩略图缓存", HIT_CLEAR_THUMBS},
        };
        int bx = x;
        for (int i = 0; i < 3; ++i) {
            int tw = TextW(g, defs[i].label, fSmall_.get()) + S(34);
            RECT r{bx, y, bx + tw, y + S(38)};
            bool hov = mouseIn_ && PtInRect(&r, mouse_);
            FillRound(g, TR(r), S(8), hov ? Clr(255, 40, 48, 62) : Clr(255, 28, 34, 45));
            StrokeRound(g, TR(r), S(8), Clr(255, 40, 47, 60), 1.0f);
            DrawStr(g, defs[i].label, TR(r), fSmall_.get(), Clr(255, 186, 197, 214),
                    StringAlignmentCenter);
            hits_.push_back({r, defs[i].id});
            bx += tw + S(10);
        }
        y += S(50);
    }

    // 关于
    DrawStr(g, L"关于", Rect(x, y, w, S(22)), fSection_.get(), txt, StringAlignmentNear,
            StringAlignmentNear, false);
    y += S(28);
    DrawStr(g, L"WallpaperFeed v1.5.1 · 壁纸来自 wallpaperfeed.com，版权归原作者，仅供个人桌面使用",
            Rect(x, y, w, S(20)), fSmall_.get(), Clr(255, 110, 121, 140), StringAlignmentNear,
            StringAlignmentNear);
    y += S(24);

    g.ResetClip();
}

void MainWindow::DrawToast(Graphics& g, int W, int H) {
    if (toast_.empty() || GetTickCount64() > toastUntil_) return;
    int tw = TextW(g, toast_, fSmall_.get()) + S(40);
    if (tw > W - S(60)) tw = W - S(60);
    int th = S(38);
    Rect r((W - tw) / 2, H - S(62), tw, th);
    FillRound(g, r, S(10), Clr(232, 24, 29, 39));
    StrokeRound(g, r, S(10), Clr(255, 52, 62, 78), 1.0f);
    DrawStr(g, toast_, r, fSmall_.get(), Clr(255, 220, 228, 240), StringAlignmentCenter);
}

void MainWindow::DrawScrollbar(Graphics& g, int W, int H, int top) {
    if (maxScroll_ <= 0) return;
    int trackH = H - top - S(12);
    if (trackH < S(60)) return;
    int barW = S(6);
    int x = W - S(14);
    FillRound(g, Rect(x, top, barW, trackH), barW / 2, Clr(36, 255, 255, 255));

    int viewH = H - top;
    int thumbH = (int)(trackH * (double)viewH / (viewH + maxScroll_));
    if (thumbH < S(44)) thumbH = S(44);
    int ty = top + (int)((trackH - thumbH) * ((double)scrollY_ / maxScroll_));
    FillRound(g, Rect(x, ty, barW, thumbH), barW / 2, Clr(110, 90, 140, 220));
    hits_.push_back({RECT{W - S(22), top, W, H}, HIT_SCROLL_TRACK});
}

void MainWindow::DrawPreview(Graphics& g, int W, int H) {
    SolidBrush mask(Clr(176, 0, 0, 0));
    g.FillRectangle(&mask, 0, 0, W, H);
    hits_.push_back({RECT{0, 0, W, H}, HIT_PREVIEW_CLOSE});

    int pw = std::min(W - S(60), S(1180));
    int ph = std::min(H - S(60), S(790));
    Rect card((W - pw) / 2, (H - ph) / 2, pw, ph);
    FillRound(g, card, S(16), Clr(255, 22, 26, 34));
    StrokeRound(g, card, S(16), Clr(255, 42, 50, 64), 1.0f);

    Rect img(card.X + S(14), card.Y + S(14), card.Width - S(28), card.Height - S(124));
    if (previewBmp_) {
        DrawCover(g, previewBmp_.get(), img, S(12));
    } else {
        FillRound(g, img, S(12), Clr(255, 30, 36, 46));
        DrawStr(g, previewLoading_ ? L"正在加载大图…" : L"无法加载预览", img, fSection_.get(),
                Clr(255, 130, 141, 160), StringAlignmentCenter, StringAlignmentCenter, false);
    }

    std::wstring title = previewItem_ ? Utf8ToWide(previewItem_->title) : L"";
    DrawStr(g, title, Rect(img.X, img.GetBottom() + S(20), img.Width - S(120), S(26)),
            fSection_.get(), Clr(255, 232, 238, 248), StringAlignmentNear, StringAlignmentNear);

    int bs = S(40), gp = S(10);
    int by = card.GetBottom() - S(20) - bs;
    int n = screenCount_;

    // 左侧：收藏 + 打开原站
    int lx = card.X + S(20);
    {
        RECT fr{lx, by, lx + bs, by + bs};
        bool hov = mouseIn_ && PtInRect(&fr, mouse_);
        FillRound(g, TR(fr), S(9), hov ? Clr(255, 42, 50, 66) : Clr(255, 30, 36, 48));
        bool fav = previewItem_ && previewItem_->favorite.load();
        DrawHeart(g, Rect(fr.left + S(12), fr.top + S(10), S(16), S(16)),
                  fav, fav ? Clr(255, 255, 77, 103) : Clr(235, 228, 234, 246));
        hits_.push_back({fr, HIT_PREVIEW_FAV});
        lx += bs + gp;

        RECT lr{lx, by, lx + bs, by + bs};
        bool hov2 = mouseIn_ && PtInRect(&lr, mouse_);
        FillRound(g, TR(lr), S(9), hov2 ? Clr(255, 42, 50, 66) : Clr(255, 30, 36, 48));
        IconLink(g, TR(lr), Clr(235, 228, 234, 246));
        hits_.push_back({lr, HIT_PREVIEW_OPEN});
    }

    // 右侧：应用按钮
    int btnScreens = (n <= 1) ? 1 : std::min(n, 3);
    int btnCount = (n <= 1) ? 1 : (btnScreens + 2);
    int totalW = btnCount * bs + (btnCount - 1) * gp;
    int rx = card.GetRight() - S(20) - totalW;
    int bx = rx;
    for (int k = 0; k < btnCount; ++k) {
        RECT br{bx, by, bx + bs, by + bs};
        bool hov = mouseIn_ && PtInRect(&br, mouse_);
        FillRound(g, TR(br), S(9), hov ? Clr(255, 46, 56, 74) : Clr(255, 30, 36, 48));
        Color ic = hov ? Clr(255, 96, 165, 250) : Clr(240, 226, 232, 245);
        int hitId;
        if (n <= 1) {
            IconMonitor(g, TR(br), 0, nullptr, ic);
            hitId = HIT_PREVIEW_MODE;
        } else if (k < btnScreens) {
            IconMonitor(g, TR(br), k + 1, fSym_.get(), ic);
            hitId = HIT_PREVIEW_MON_BASE + k;
        } else if (k == btnScreens) {
            IconSpan(g, TR(br), ic);
            hitId = HIT_PREVIEW_SPAN;
        } else {
            IconClone(g, TR(br), ic);
            hitId = HIT_PREVIEW_CLONE;
        }
        hits_.push_back({br, hitId});
        bx += bs + gp;
    }

    // 关闭提示
    DrawStr(g, L"点击空白处关闭预览（Esc）", Rect(card.X + S(20), card.GetBottom() - S(64),
            card.Width - S(40), S(18)), fSmall_.get(), Clr(255, 96, 106, 124),
            StringAlignmentNear, StringAlignmentCenter, false);
}

// ── 图片缓存 ────────────────────────────────────────────────────────────

Bitmap* MainWindow::GetImage(const ItemPtr& item) {
    auto f = images_.find(item->id);
    if (f != images_.end()) {
        f->second.lastUse = ++useCounter_;
        return f->second.bmp.get();
    }
    if (item->thumbState.load() != 2) return nullptr;
    if (imgBudget_ <= 0) return nullptr;
    --imgBudget_;

    std::unique_ptr<std::vector<uint8_t>> buf(new std::vector<uint8_t>());
    int w = 0, h = 0;
    std::wstring err;
    if (!wic::DecodeToBGRA(item->thumbFile, *buf, w, h, err)) {
        item->thumbState = 3;
        return nullptr;
    }

    CachedImage ci;
    ci.buf = std::move(buf);
    ci.bmp = std::make_unique<Bitmap>(w, h, w * 4, PixelFormat32bppARGB, ci.buf->data());
    ci.lastUse = ++useCounter_;
    images_.emplace(item->id, std::move(ci));
    EvictImages();
    auto it2 = images_.find(item->id);
    return it2 == images_.end() ? nullptr : it2->second.bmp.get();
}

void MainWindow::EvictImages() {
    const size_t kMax = 90;
    while (images_.size() > kMax) {
        uint64_t minUse = UINT64_MAX;
        int victim = -1;
        for (const auto& kv : images_) {
            if (kv.second.lastUse < minUse) {
                minUse = kv.second.lastUse;
                victim = kv.first;
            }
        }
        if (victim < 0) break;
        images_.erase(victim);
    }
}

// ── 页面切换 ────────────────────────────────────────────────────────────

void MainWindow::SwitchPage(Page p) {
    page_ = p;
    scrollY_ = 0;
    items_.clear();

    if (p == Page::Discover) lib_->SetMode(LibraryMode::Discover, L"", cfg_->resolution);
    else if (p == Page::Category) lib_->SetMode(LibraryMode::Category, catSlug_, cfg_->resolution);
    else if (p == Page::Favorites) lib_->SetMode(LibraryMode::Favorites);
    else { /* Settings: 不动数据 */ }

    items_ = lib_->Snapshot();
    Relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SwitchCategory(int index) {
    const auto& cats = GetCategories();
    if (index < 0 || index >= (int)cats.size()) return;
    catSlug_ = cats[index].slug;
    catLabel_ = cats[index].label;
    // 站点不支持「分类 + 尺寸」叠加，点分类就回到不限尺寸
    cfg_->resolution.clear();
    cfg_->Save(configPath_);
    scrollY_ = 0;
    lib_->SetMode(LibraryMode::Category, catSlug_, L"");
    items_ = lib_->Snapshot();
    Relayout();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ── 交互 ────────────────────────────────────────────────────────────────

int MainWindow::HitTest(POINT pt) const {
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (!PtInRect(&it->rc, pt)) continue;
        // 内容区里的卡片/设置项滚到标题栏下面时已被裁掉，不能再被点到
        if (it->clipTop > 0 && pt.y < it->clipTop) continue;
        return it->id;
    }
    return 0;
}

bool MainWindow::InGridArea(POINT pt) const {
    RECT cr;
    GetClientRect(hwnd_, &cr);
    return pt.x >= ContentLeft() - S(10) && pt.y >= GridTop(cr.right) && pt.y <= cr.bottom;
}

void MainWindow::ScrollBy(int delta) {
    int oldScroll = scrollY_;
    scrollY_ += delta;
    int top = 0;
    {
        RECT cr;
        GetClientRect(hwnd_, &cr);
        top = GridTop(cr.right);
        maxScroll_ = contentH_ - (cr.bottom - top);
        if (maxScroll_ < 0) maxScroll_ = 0;
    }
    if (scrollY_ < 0) scrollY_ = 0;
    if (scrollY_ > maxScroll_) scrollY_ = maxScroll_;
    if (scrollY_ != oldScroll) InvalidateRect(hwnd_, nullptr, FALSE);

    if (maxScroll_ > 0 && scrollY_ > maxScroll_ - S(500)) lib_->RequestMore();
}

void MainWindow::Toast(const std::wstring& text, int ms) {
    toast_ = text;
    toastUntil_ = GetTickCount64() + (uint64_t)ms;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::OnClick(POINT pt) {
    int hit = HitTest(pt);

    if (previewOpen_) {
        if (hit == HIT_PREVIEW_CLOSE) { ClosePreview(); return; }
        if (hit == HIT_PREVIEW_FAV) {
            if (previewItem_) ToggleFavorite(previewItem_);
            return;
        }
        if (hit == HIT_PREVIEW_OPEN) {
            if (previewItem_) {
                std::wstring url = L"https://wallpaperfeed.com" + Utf8ToWide(previewItem_->href);
                ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            return;
        }
        if (hit == HIT_PREVIEW_MODE) { ApplyItem(previewItem_, ApplyKind::ModeDefault, -1); return; }
        if (hit == HIT_PREVIEW_SPAN) { ApplyItem(previewItem_, ApplyKind::Span, -1); return; }
        if (hit == HIT_PREVIEW_CLONE) { ApplyItem(previewItem_, ApplyKind::Clone, -1); return; }
        if (hit >= HIT_PREVIEW_MON_BASE && hit < HIT_PREVIEW_MON_BASE + 50) {
            ApplyItem(previewItem_, ApplyKind::Monitor, hit - HIT_PREVIEW_MON_BASE);
        }
        return;
    }

    if (hit >= HIT_CARD_BASE && hit < HIT_CARD_BASE + 20000) {
        OnCardAction((hit - HIT_CARD_BASE) / 10, (hit - HIT_CARD_BASE) % 10);
        return;
    }
    if (hit >= HIT_CARD_OPEN_BASE && hit < HIT_CARD_OPEN_BASE + 20000) {
        OpenPreview(hit - HIT_CARD_OPEN_BASE);
        return;
    }

    if (hit >= HIT_NAV_BASE && hit < HIT_NAV_BASE + 4) {
        Page p = (Page)(hit - HIT_NAV_BASE);
        if (p != page_) SwitchPage(p);
        else if (p == Page::Discover) lib_->RequestMore();
        return;
    }
    if (hit >= HIT_RES_BASE && hit < HIT_RES_BASE + 20) {
        int idx = hit - HIT_RES_BASE;
        const auto& resList = GetResolutions();
        std::wstring slug = (idx <= 0 || idx > (int)resList.size()) ? L""
                                                                   : resList[idx - 1].slug;
        cfg_->resolution = slug;
        cfg_->Save(configPath_);
        scrollY_ = 0;
        lib_->SetMode(LibraryMode::Category, catSlug_, slug);
        items_ = lib_->Snapshot();
        Relayout();
        InvalidateRect(hwnd_, nullptr, FALSE);
        if (slug.empty()) Toast(L"已取消尺寸筛选");
        else Toast(L"已切换到 " + cfg_->ResolutionLabel() + L" 尺寸图库");
        return;
    }
    if (hit >= HIT_CAT_BASE && hit < HIT_CAT_BASE + 50) {
        SwitchCategory(hit - HIT_CAT_BASE);
        return;
    }
    if (hit == HIT_REFRESH) {
        if (page_ == Page::Discover) lib_->SetMode(LibraryMode::Discover, L"", cfg_->resolution);
        else if (page_ == Page::Category) lib_->SetMode(LibraryMode::Category, catSlug_, cfg_->resolution);
        else if (page_ == Page::Favorites) lib_->ReloadFavorites();
        items_ = lib_->Snapshot();
        scrollY_ = 0;
        Relayout();
        Toast(L"已刷新内容");
        return;
    }
    OnSettingClick(hit);
}

void MainWindow::OnCardAction(int index, int sub) {
    if (index < 0 || index >= (int)cards_.size()) return;
    ItemPtr item = cards_[index].item;
    int n = screenCount_;
    if (sub == 0) { ToggleFavorite(item); return; }
    if (sub >= 1 && sub <= n) { ApplyItem(item, ApplyKind::Monitor, sub - 1); return; }
    if (sub == n + 1) { ApplyItem(item, ApplyKind::Span, -1); return; }
    if (sub == n + 2) { ApplyItem(item, ApplyKind::Clone, -1); return; }
}

void MainWindow::ToggleFavorite(const ItemPtr& item) {
    if (!item) return;
    lib_->ToggleFavorite(item);
    Toast(item->favorite.load() ? L"已加入收藏，图片已保存到本地" : L"已取消收藏，本地图片已删除");
}

void MainWindow::OnSettingClick(int hit) {
    if (hit == HIT_AUTO_TOGGLE) {
        cfg_->intervalMinutes = (cfg_->intervalMinutes > 0) ? 0 : 60;
        app_->SaveConfig();
        app_->NotifySettingsChanged();
        Toast(cfg_->intervalMinutes > 0 ? L"已开启自动更换（60 分钟）" : L"已关闭自动更换");
        Relayout();
        return;
    }
    if (hit >= HIT_INTERVAL_BASE && hit < HIT_INTERVAL_BASE + 8) {
        int idx = hit - HIT_INTERVAL_BASE;
        cfg_->intervalMinutes = kIntervals[idx];
        app_->SaveConfig();
        app_->NotifySettingsChanged();
        Toast(L"已设置为每 " + std::wstring(kIntervalLabels[idx]) + L" 更换");
        return;
    }
    if (hit == HIT_AUTOSTART) {
        cfg_->autostart = !cfg_->autostart;
        autostart::SetEnabled(cfg_->autostart);
        app_->SaveConfig();
        Toast(cfg_->autostart ? L"已开启开机启动" : L"已关闭开机启动");
        return;
    }
    if (hit == HIT_TIPS) {
        cfg_->showTips = !cfg_->showTips;
        app_->SaveConfig();
        Toast(cfg_->showTips ? L"已开启切换提示" : L"已关闭切换提示");
        return;
    }
    if (hit >= HIT_QUALITY_BASE && hit < HIT_QUALITY_BASE + 3) {
        int vals[3] = {80, 90, 95};
        cfg_->jpegQuality = vals[hit - HIT_QUALITY_BASE];
        app_->SaveConfig();
        Toast(L"壁纸质量已更新，下次生成生效");
        return;
    }
    if (hit == HIT_AUTOCLEAN) {
        cfg_->autoClean = !cfg_->autoClean;
        app_->SaveConfig();
        Toast(cfg_->autoClean ? L"已开启自动清理" : L"已关闭自动清理");
        return;
    }
    if (hit == HIT_KEEPFAV) {
        cfg_->keepFavorites = !cfg_->keepFavorites;
        app_->SaveConfig();
        Toast(cfg_->keepFavorites ? L"清理时会保留收藏的壁纸" : L"收藏壁纸也会被清理");
        return;
    }
    if (hit >= HIT_KEEPDAYS_BASE && hit < HIT_KEEPDAYS_BASE + 4) {
        const int days[4] = {7, 30, 90, 0};
        cfg_->keepDays = days[hit - HIT_KEEPDAYS_BASE];
        app_->SaveConfig();
        Toast(cfg_->keepDays > 0 ? L"只保留最近 " + std::to_wstring(cfg_->keepDays) + L" 天的壁纸"
                                 : L"不再按天数清理");
        return;
    }
    if (hit >= HIT_MAXSIZE_BASE && hit < HIT_MAXSIZE_BASE + 5) {
        const int mbs[5] = {256, 512, 1024, 2048, 0};
        cfg_->maxCacheMb = mbs[hit - HIT_MAXSIZE_BASE];
        app_->SaveConfig();
        Toast(cfg_->maxCacheMb > 0
                  ? L"最大占用已设为 " + FormatBytes((uint64_t)cfg_->maxCacheMb * 1024ull * 1024ull)
                  : L"已取消占用上限");
        return;
    }
    if (hit == HIT_CLEAN_NOW) {
        bool did = app_->CheckStorage(true);
        RefreshCache();
        Toast(did ? L"已清理旧壁纸，当前占用 " + FormatBytes(cacheBytes_)
                  : L"没有需要清理的内容");
        return;
    }
    if (hit == HIT_OPEN_CACHE) {
        ShellExecuteW(nullptr, L"open", cacheDir_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (hit == HIT_OPEN_CONFIG) {
        ShellExecuteW(nullptr, L"open", configPath_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (hit == HIT_CLEAR_THUMBS) {
        std::wstring pattern = PathJoin(cacheDir_, L"thumbs\\*.webp");
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        int count = 0;
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::wstring p = PathJoin(PathJoin(cacheDir_, L"thumbs"), fd.cFileName);
                if (DeleteFileW(p.c_str())) ++count;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        images_.clear();
        for (auto& it : items_) {
            if (it->thumbState.load() == 2) it->thumbState = 0;
            lib_->EnqueueThumb(it);
        }
        Toast(L"已清理 " + std::to_wstring(count) + L" 个缩略图文件");
        return;
    }
}

// ── 应用壁纸 ────────────────────────────────────────────────────────────

bool MainWindow::ApplyOnBackground(const ItemPtr& item, ApplyKind kind, int monitor,
                                   const Config& cfg, std::wstring& msg) {
    std::vector<ScreenInfo> screens;
    bool perMon = false;
    std::wstring err;
    if (!desk::EnumScreens(screens, perMon, err) || screens.empty()) {
        msg = L"检测显示器失败：" + err;
        return false;
    }

    RECT bbox = screens[0].rect;
    for (const auto& s : screens) {
        bbox.left = std::min<LONG>(bbox.left, s.rect.left);
        bbox.top = std::min<LONG>(bbox.top, s.rect.top);
        bbox.right = std::max<LONG>(bbox.right, s.rect.right);
        bbox.bottom = std::max<LONG>(bbox.bottom, s.rect.bottom);
    }
    int bboxW = (int)(bbox.right - bbox.left);
    int bboxH = (int)(bbox.bottom - bbox.top);

    ApplyKind effective = kind;
    if (kind == ApplyKind::ModeDefault) {
        effective = (cfg.mode == L"span") ? ApplyKind::Span : ApplyKind::Clone;
    }

    int needW = 0, needH = 0;
    if (effective == ApplyKind::Monitor) {
        // 系统切到「复制这些显示器」时只剩一块逻辑屏，编号会被夹到有效范围，
        // 由 desk::ApplyOne 自动回退成设置整个桌面，不再直接报错。
        if (monitor < 0) monitor = 0;
        if (monitor >= (int)screens.size()) monitor = (int)screens.size() - 1;
        needW = screens[monitor].width;
        needH = screens[monitor].height;
    } else if (effective == ApplyKind::Span) {
        needW = bboxW;
        needH = bboxH;
    } else {
        for (const auto& s : screens) {
            needW = std::max(needW, s.width);
            needH = std::max(needH, s.height);
        }
    }

    int reqH = 0;
    int reqW = RequestWidthFor(*item, needW, needH, cfg.requestCapWidth, reqH);
    wchar_t name[160];
    swprintf_s(name, L"wf_%d_%dx%d.jpg", item->id, reqW, reqH);
    std::wstring srcFile = PathJoin(srcDir_, name);

    if (!FileExists(srcFile)) {
        if (!WallpaperFeed::Download(item->id,
                                     WallpaperFeed::DetailReferer(item->href, item->id),
                                     reqW, reqH, srcFile, err)) {
            msg = L"下载壁纸失败：" + err;
            return false;
        }
    }

    std::vector<std::wstring> outs(screens.size());
    if (effective == ApplyKind::Span) {
        std::vector<RECT> slices;
        for (const auto& s : screens) {
            RECT r{};
            r.left = s.rect.left - bbox.left;
            r.top = s.rect.top - bbox.top;
            r.right = r.left + s.width;
            r.bottom = r.top + s.height;
            slices.push_back(r);
        }
        for (size_t i = 0; i < screens.size(); ++i) {
            wchar_t fn[180];
            swprintf_s(fn, L"ui_span_m%u_%d_%dx%d.jpg", (unsigned)i, item->id,
                       screens[i].width, screens[i].height);
            outs[i] = PathJoin(curDir_, fn);
        }
        if (!gfx::RenderCoverCrop(srcFile, bboxW, bboxH, slices, outs, cfg.jpegQuality, err)) {
            msg = L"生成跨屏壁纸失败：" + err;
            return false;
        }
    } else if (effective == ApplyKind::Monitor) {
        wchar_t fn[180];
        swprintf_s(fn, L"ui_one_m%d_%d_%dx%d.jpg", monitor, item->id,
                   screens[monitor].width, screens[monitor].height);
        outs[monitor] = PathJoin(curDir_, fn);
        if (!gfx::RenderCoverCrop(srcFile, screens[monitor].width, screens[monitor].height,
                                  {RECT{0, 0, screens[monitor].width, screens[monitor].height}},
                                  {outs[monitor]}, cfg.jpegQuality, err)) {
            msg = L"生成壁纸失败：" + err;
            return false;
        }
        if (!desk::ApplyOne(screens, monitor, outs[monitor], err)) {
            msg = L"设置失败：" + err;
            return false;
        }
        wchar_t buf[128];
        swprintf_s(buf, L"已应用到显示器 %d", monitor + 1);
        msg = buf;
        return true;
    } else {
        // 复制到所有屏：源图只解码一次，一次性生成各屏切片
        std::vector<std::pair<int, int>> targets;
        std::vector<std::wstring> dsts;
        for (size_t i = 0; i < screens.size(); ++i) {
            wchar_t fn[180];
            swprintf_s(fn, L"ui_clone_m%u_%d_%dx%d.jpg", (unsigned)i, item->id,
                       screens[i].width, screens[i].height);
            outs[i] = PathJoin(curDir_, fn);
            targets.push_back({screens[i].width, screens[i].height});
            dsts.push_back(outs[i]);
        }
        if (!gfx::RenderScreens(srcFile, targets, dsts, cfg.jpegQuality, err)) {
            msg = L"生成壁纸失败：" + err;
            return false;
        }
        if (!desk::ApplyPerScreen(screens, outs, err)) {
            msg = L"设置壁纸失败：" + err;
            return false;
        }
        msg = L"已复制到所有显示器";
        return true;
    }

    if (!desk::ApplyPerScreen(screens, outs, err)) {
        msg = L"设置壁纸失败：" + err;
        return false;
    }
    msg = L"已设为跨屏合一壁纸";
    return true;
}

void MainWindow::ApplyItem(const ItemPtr& item, ApplyKind kind, int monitor) {
    if (!item || !hwnd_) return;
    if (applying_) {
        Toast(L"上一张还在应用，请稍候…");
        return;
    }
    applying_ = true;
    applyOk_ = false;
    applyResult_.clear();
    pendingApply_.id = item->id;
    pendingApply_.w = item->w;
    pendingApply_.h = item->h;
    pendingApply_.title = item->title;
    pendingApply_.href = item->href;
    pendingApply_.thumbUrl = item->thumbUrl;
    pendingMon_ = monitor;
    pendingKind_ = (kind == ApplyKind::Monitor) ? 0
                  : (kind == ApplyKind::Span ? 1 : 2);
    if (kind == ApplyKind::ModeDefault) pendingKind_ = (cfg_->mode == L"span") ? 1 : 2;
    Toast(L"正在下载并应用到桌面…", 5000);

    Config snapshot = *cfg_;
    ItemPtr copy = item;
    ApplyKind k = kind;
    int mon = monitor;

    if (applyThread_.joinable()) applyThread_.join();

    applyThread_ = std::thread([this, copy, k, mon, snapshot]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::wstring msg;
        bool ok = ApplyOnBackground(copy, k, mon, snapshot, msg);
        CoUninitialize();
        applyOk_ = ok;
        applyResult_ = msg;
        PostMessageW(hwnd_, WM_UI_APPLY_DONE, ok ? 1 : 0, 0);
    });
}

// ── 预览 ────────────────────────────────────────────────────────────────

void MainWindow::OpenPreview(int index) {
    if (index < 0 || index >= (int)cards_.size()) return;
    previewItem_ = cards_[index].item;
    previewOpen_ = true;
    previewLoading_ = true;
    previewBmp_.reset();
    previewBuf_.reset();
    InvalidateRect(hwnd_, nullptr, FALSE);

    if (previewThread_.joinable()) previewThread_.join();

    wchar_t fn[128];
    swprintf_s(fn, L"ui_prev_%d_1600x900.jpg", previewItem_->id);
    std::wstring outFile = PathJoin(curDir_, fn);
    int cap = cfg_->requestCapWidth;
    ItemPtr it = previewItem_;

    previewThread_ = std::thread([this, it, outFile, cap]() {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (!FileExists(outFile)) {
            int reqH = 0;
            int reqW = RequestWidthFor(*it, 1600, 900, cap, reqH);
            std::wstring err;
            WallpaperFeed::Download(it->id, WallpaperFeed::DetailReferer(it->href, it->id),
                                    reqW, reqH, outFile, err);
        }
        CoUninitialize();
        PostMessageW(hwnd_, WM_UI_PREVIEW_READY, 0, 0);
    });
}

void MainWindow::ClosePreview() {
    previewOpen_ = false;
    previewBmp_.reset();
    previewBuf_.reset();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::FinishPreview() {
    if (!previewOpen_ || !previewItem_) return;

    wchar_t fn[128];
    swprintf_s(fn, L"ui_prev_%d_1600x900.jpg", previewItem_->id);
    std::wstring outFile = PathJoin(curDir_, fn);

    int w = 0, h = 0;
    std::wstring err;
    previewBuf_ = std::make_unique<std::vector<uint8_t>>();
    if (wic::DecodeToBGRA(outFile, *previewBuf_, w, h, err)) {
        previewBmp_ = std::make_unique<Bitmap>(w, h, w * 4, PixelFormat32bppARGB,
                                               previewBuf_->data());
    } else if (wic::DecodeToBGRA(previewItem_->thumbFile, *previewBuf_, w, h, err)) {
        previewBmp_ = std::make_unique<Bitmap>(w, h, w * 4, PixelFormat32bppARGB,
                                               previewBuf_->data());
    }
    previewLoading_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ── 窗口消息 ────────────────────────────────────────────────────────────

LRESULT CALLBACK MainWindow::WndProcStatic(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    }
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return self->HandleMessage(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void MainWindow::OnLibraryUpdated() {
    items_ = lib_->Snapshot();
    Relayout();

    RECT cr;
    GetClientRect(hwnd_, &cr);
    if (page_ != Page::Settings && cr.bottom > 0) {
        int viewH = cr.bottom - GridTop(cr.right);
        if (contentH_ < viewH + S(400)) lib_->RequestMore();
    }
    if (IsVisible()) InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::OnThumbReady() {
    if (!IsVisible()) return;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

LRESULT MainWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        Paint(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE:
        Relayout();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = (LONG)S(900);
        mm->ptMinTrackSize.y = (LONG)S(600);
        return 0;
    }

    case WM_DPICHANGED: {
        UpdateDpi();
        InitFonts();
        RECT* r = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left,
                     r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Relayout();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            tracking_ = true;
        }
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        mouse_ = pt;
        mouseIn_ = true;

        if (scrolling_ && maxScroll_ > 0) {
            int top = 0;
            RECT cr;
            GetClientRect(hwnd, &cr);
            top = GridTop(cr.right);
            int trackH = cr.bottom - top - S(12);
            int viewH = cr.bottom - top;
            int thumbH = (int)(trackH * (double)viewH / (viewH + maxScroll_));
            if (thumbH < S(44)) thumbH = S(44);
            int rel = pt.y - top - scrollDragOffset_;
            double ratio = (trackH - thumbH) > 0 ? (double)rel / (trackH - thumbH) : 0.0;
            int ny = (int)(ratio * maxScroll_);
            if (ny < 0) ny = 0;
            if (ny > maxScroll_) ny = maxScroll_;
            if (ny != scrollY_) {
                scrollY_ = ny;
                InvalidateRect(hwnd, nullptr, FALSE);
                if (scrollY_ > maxScroll_ - S(500)) lib_->RequestMore();
            }
            return 0;
        }

        int newHit = HitTest(pt);
        int newCard = -1;
        if (newHit >= HIT_CARD_OPEN_BASE && newHit < HIT_CARD_OPEN_BASE + 20000) {
            newCard = newHit - HIT_CARD_OPEN_BASE;
        } else if (newHit >= HIT_CARD_BASE && newHit < HIT_CARD_BASE + 20000) {
            newCard = (newHit - HIT_CARD_BASE) / 10;
        }
        if (newHit != hoverHit_ || newCard != hoverCard_) {
            hoverHit_ = newHit;
            hoverCard_ = newCard;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (newHit) SetCursor(LoadCursorW(nullptr, IDC_HAND));
        else SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return 0;
    }

    case WM_MOUSELEAVE:
        tracking_ = false;
        mouseIn_ = false;
        hoverCard_ = -1;
        hoverHit_ = 0;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (previewOpen_) return 0;
        ScrollBy(-delta * S(90) / WHEEL_DELTA);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        SetCapture(hwnd);
        int hit = HitTest(pt);
        if (hit == HIT_SCROLL_TRACK && maxScroll_ > 0) {
            RECT cr;
            GetClientRect(hwnd, &cr);
            int top = GridTop(cr.right);
            int trackH = cr.bottom - top - S(12);
            int viewH = cr.bottom - top;
            int thumbH = (int)(trackH * (double)viewH / (viewH + maxScroll_));
            if (thumbH < S(44)) thumbH = S(44);
            int rel = pt.y - top;
            // 若点在滑块上则保持偏移，否则直接跳转
            int curThumbTop = (int)((trackH - thumbH) * ((double)scrollY_ / maxScroll_));
            if (rel >= curThumbTop && rel <= curThumbTop + thumbH) {
                scrollDragOffset_ = rel - curThumbTop;
            } else {
                scrollDragOffset_ = thumbH / 2;
                int ny = (int)((double)(rel - thumbH / 2) / (trackH - thumbH) * maxScroll_);
                if (ny < 0) ny = 0;
                if (ny > maxScroll_) ny = maxScroll_;
                scrollY_ = ny;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            scrolling_ = true;
            return 0;
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        ReleaseCapture();
        if (scrolling_) {
            scrolling_ = false;
            return 0;
        }
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        OnClick(pt);
        return 0;
    }

    case WM_DISPLAYCHANGE: {
        // 用户切换「扩展／复制」后重新探测屏幕，卡片上的应用图标数量跟着变
        int before = screenCount_;
        RefreshScreens();
        if (page_ != Page::Settings) items_ = lib_->Snapshot();
        Relayout();
        InvalidateRect(hwnd, nullptr, FALSE);
        if (before != screenCount_) {
            LogMsg(L"显示器数量变化：%d → %d", before, screenCount_);
        }
        return 0;
    }
    case WM_TIMER: {
        if (!toast_.empty() && GetTickCount64() > toastUntil_) {
            toast_.clear();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        ++tickCount_;
        bool need = false;
        for (const auto& c : cards_) {
            if (c.item->thumbState.load() == 2 && images_.find(c.item->id) == images_.end()) {
                need = true;
                break;
            }
        }
        if (need) InvalidateRect(hwnd, nullptr, FALSE);

        // 每 ~5 秒刷新本地占用
        if (tickCount_ % 40 == 0) {
            RefreshCache();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        // 兜底：页面已切换但列表一直为空时，主动再请求（每 ~2 秒最多一次）
        if (tickCount_ % 16 == 0 && page_ != Page::Settings) {
            if (items_.empty() && !lib_->Loading() && !lib_->NoMore()) lib_->RequestMore();
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && previewOpen_) {
            ClosePreview();
            return 0;
        }
        if (wp == VK_PRIOR) { ScrollBy(-S(400)); return 0; }
        if (wp == VK_NEXT) { ScrollBy(S(400)); return 0; }
        if (wp == VK_HOME) { scrollY_ = 0; InvalidateRect(hwnd, nullptr, FALSE); return 0; }
        if (wp == VK_END) { scrollY_ = maxScroll_; InvalidateRect(hwnd, nullptr, FALSE); return 0; }
        return 0;

    case WM_CLOSE:
        Hide();
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        return 0;

    case WM_UI_APPLY_DONE: {
        applying_ = false;
        Toast(applyResult_, applyOk_ ? 3200 : 5200);
        if (applyOk_ && app_) {
            app_->RecordUiApplied(pendingApply_, pendingKind_, pendingMon_, screenCount_);
            app_->NotifyManualApply();
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_UI_PREVIEW_READY:
        FinishPreview();
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}
