/*
 * LivpViewer — fast single-EXE Apple Live Photo (.livp) viewer
 * Build: build.bat
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <wincodec.h>
#include <mfplay.h>
#include <mfapi.h>
#include <shellapi.h>
#include <objbase.h>

#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

#include "resource.h"

#define MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mfplay.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "msimg32.lib")

static constexpr wchar_t kClassName[] = L"LivpViewerWnd";
static constexpr wchar_t kVideoClass[] = L"LivpViewerVideo";
static constexpr wchar_t kCoverClass[] = L"LivpViewerCover";
static constexpr UINT WM_APP_PLAY_ENDED = WM_APP + 1;
static constexpr UINT WM_APP_PLAY_STARTED = WM_APP + 2;
static constexpr UINT WM_APP_RETRY_CODEC = WM_APP + 3;
static constexpr int kLiveIconDraw = 40;

struct RgbImage {
    int w = 0, h = 0, stride = 0;
};

struct AppState {
    HINSTANCE hi = nullptr;
    HWND hwnd = nullptr;
    HWND hwndVideo = nullptr; // MF plays here
    HWND hwndCover = nullptr; // still overlay on top of video (avoids black flash)
    RgbImage still;
    HBITMAP hbmStill = nullptr;
    HBITMAP hbmLive = nullptr;
    int liveW = 0, liveH = 0;
    std::vector<unsigned char> videoBytes;
    std::wstring videoExt = L".mov";
    std::wstring path;
    std::wstring tempVideo;
    bool playing = false;     // play session (may still be buffering)
    bool videoShown = false;  // cover hidden; video visible
    bool hasFile = false;
    RECT liveBtn{};
    RECT prevBtn{};
    RECT nextBtn{};
    bool liveHot = false;
    bool prevHot = false;
    bool nextHot = false;
    bool mfReady = false;
    IMFPMediaPlayer* player = nullptr;
    float zoom = 1.f;
    float panX = 0.f;
    float panY = 0.f;
    bool panning = false;
    POINT panLast{};
    std::vector<std::wstring> neighbors;
    int neighborIndex = -1;
    std::wstring pendingRetryPath; // reopen after codec install
    bool pendingRetryPlay = false;
};

static AppState g;

// ---------- helpers ----------

static bool ieq(const char* a, const char* b) {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? char(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? char(*b + 32) : *b;
        if (ca != cb) return false;
        ++a; ++b;
    }
    return *a == *b;
}

static bool hasExt(const char* name, const char* ext) {
    size_t n = strlen(name), e = strlen(ext);
    return n >= e && ieq(name + (n - e), ext);
}

static bool isHeicName(const char* name) {
    return hasExt(name, ".heic") || hasExt(name, ".heif");
}

static bool runWingetInstall(const wchar_t* storeProductId) {
    wchar_t winget[MAX_PATH];
    if (!SearchPathW(nullptr, L"winget.exe", nullptr, MAX_PATH, winget, nullptr))
        return false;

    wchar_t args[512];
    wsprintfW(args,
              L"install -e --id %s --source msstore "
              L"--accept-package-agreements --accept-source-agreements",
              storeProductId);

    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = g.hwnd;
    sei.lpFile = winget;
    sei.lpParameters = args;
    sei.nShow = SW_SHOW;
    if (!ShellExecuteExW(&sei))
        return false;
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 15 * 60 * 1000);
        DWORD code = 1;
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
        // 0 = ok; some winget versions return other success codes when already installed
        return code == 0;
    }
    return true;
}

static void openStoreProduct(const wchar_t* storeProductId) {
    wchar_t uri[160];
    wsprintfW(uri, L"ms-windows-store://pdp/?ProductId=%s", storeProductId);
    ShellExecuteW(g.hwnd, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
}

static void openLivp(const wchar_t* path); // used after codec install
static void startPlayback();

// HEIF: 9PMMSR1CGPWG   HEVC(厂商免费版): 9N4WGH0Z6VHQ
// retryPath: install 成功后自动重新打开；retryPlay: 打开后再自动播放
static void offerInstallCodecs(bool needHeif, bool needHevc,
                               const wchar_t* retryPath, bool retryPlay) {
    if (!needHeif && !needHevc) return;

    wchar_t msg[640];
    wsprintfW(msg,
              L"查看此 Live Photo 需要 Windows 系统解码组件：\n\n"
              L"%s%s"
              L"是否现在自动安装？\n\n"
              L"（优先使用 winget，失败则打开微软商店；可能需要确认）",
              needHeif ? L"· HEIF 图像扩展（解码 .heic 静图）\n" : L"",
              needHevc ? L"· HEVC 视频扩展（播放 .mov / 部分 HEIC）\n" : L"");

    if (MessageBoxW(g.hwnd, msg, L"LivpViewer — 缺少解码组件",
                    MB_YESNO | MB_ICONINFORMATION | MB_DEFBUTTON1) != IDYES)
        return;

    const bool haveWinget = SearchPathW(nullptr, L"winget.exe", nullptr, MAX_PATH, nullptr, nullptr) != 0;
    if (!haveWinget) {
        if (needHeif) openStoreProduct(L"9PMMSR1CGPWG");
        if (needHevc) openStoreProduct(L"9N4WGH0Z6VHQ");
        MessageBoxW(g.hwnd,
                    L"未找到 winget，已打开微软商店。\n"
                    L"安装完成后回到本程序，重新打开该文件即可。",
                    L"LivpViewer", MB_ICONINFORMATION);
        return;
    }

    HCURSOR prev = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    bool allOk = true;
    if (needHeif && !runWingetInstall(L"9PMMSR1CGPWG")) {
        openStoreProduct(L"9PMMSR1CGPWG");
        allOk = false;
    }
    if (needHevc && !runWingetInstall(L"9N4WGH0Z6VHQ")) {
        openStoreProduct(L"9N4WGH0Z6VHQ");
        allOk = false;
    }
    SetCursor(prev);

    if (allOk && retryPath && retryPath[0]) {
        g.pendingRetryPath = retryPath;
        g.pendingRetryPlay = retryPlay;
        MessageBoxW(g.hwnd,
                    L"组件已安装（或已就绪）。\n将自动重新打开文件。",
                    L"LivpViewer", MB_ICONINFORMATION);
        PostMessageW(g.hwnd, WM_APP_RETRY_CODEC, 0, 0);
    } else if (allOk) {
        MessageBoxW(g.hwnd,
                    L"安装流程已结束。\n请重新打开该 .livp 文件后再试。",
                    L"LivpViewer", MB_ICONINFORMATION);
    } else {
        MessageBoxW(g.hwnd,
                    L"部分组件需在微软商店中确认安装。\n"
                    L"完成后请重新打开该文件。",
                    L"LivpViewer", MB_ICONINFORMATION);
    }
}

static bool isStill(const char* name) {
    return hasExt(name, ".jpg") || hasExt(name, ".jpeg") ||
           hasExt(name, ".heic") || hasExt(name, ".heif") ||
           hasExt(name, ".png") || hasExt(name, ".tif") || hasExt(name, ".tiff");
}

static bool isVideo(const char* name) {
    return hasExt(name, ".mov") || hasExt(name, ".mp4") || hasExt(name, ".m4v");
}

static bool readFileW(const wchar_t* path, std::vector<unsigned char>& out) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li{};
    if (!GetFileSizeEx(h, &li) || li.QuadPart <= 0 || li.QuadPart > 512ll * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    out.resize(size_t(li.QuadPart));
    DWORD got = 0;
    BOOL ok = ReadFile(h, out.data(), DWORD(out.size()), &got, nullptr);
    CloseHandle(h);
    return ok && got == out.size();
}

static bool writeFileW(const wchar_t* path, const void* data, size_t size) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data, DWORD(size), &written, nullptr);
    CloseHandle(h);
    return ok && written == size;
}

static bool extractLivp(const wchar_t* path,
                        std::vector<unsigned char>& stillBytes, std::string& stillName,
                        std::vector<unsigned char>& videoBytes, std::string& videoName) {
    std::vector<unsigned char> file;
    if (!readFileW(path, file)) return false;

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, file.data(), file.size(), 0)) return false;

    int bestStill = -1, bestScore = -1, bestVideo = -1;
    char name[512];
    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n; ++i) {
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
        if (isStill(name)) {
            int score = 1;
            if (hasExt(name, ".heic") || hasExt(name, ".heif")) score = 3;
            else if (hasExt(name, ".jpg") || hasExt(name, ".jpeg")) score = 2;
            if (score > bestScore) { bestScore = score; bestStill = int(i); stillName = name; }
        } else if (isVideo(name) && bestVideo < 0) {
            bestVideo = int(i);
            videoName = name;
        }
    }
    if (bestStill < 0 || bestVideo < 0) {
        mz_zip_reader_end(&zip);
        return false;
    }

    size_t sz = 0;
    void* p = mz_zip_reader_extract_to_heap(&zip, mz_uint(bestStill), &sz, 0);
    if (!p || !sz) { mz_zip_reader_end(&zip); return false; }
    stillBytes.assign((unsigned char*)p, (unsigned char*)p + sz);
    mz_free(p);

    sz = 0;
    p = mz_zip_reader_extract_to_heap(&zip, mz_uint(bestVideo), &sz, 0);
    mz_zip_reader_end(&zip);
    if (!p || !sz) return false;
    videoBytes.assign((unsigned char*)p, (unsigned char*)p + sz);
    mz_free(p);
    return true;
}

static bool decodeToHBitmap(const unsigned char* data, size_t size, RgbImage* meta, HBITMAP* outBmp) {
    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))))
        return false;

    HGLOBAL hmem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hmem) { factory->Release(); return false; }
    memcpy(GlobalLock(hmem), data, size);
    GlobalUnlock(hmem);

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(hmem, TRUE, &stream))) {
        GlobalFree(hmem);
        factory->Release();
        return false;
    }

    IWICBitmapDecoder* decoder = nullptr;
    HRESULT hr = factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) {
        stream->Release(); factory->Release();
        return false;
    }

    IWICBitmapFrameDecode* frame = nullptr;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        decoder->Release(); stream->Release(); factory->Release();
        return false;
    }

    IWICFormatConverter* conv = nullptr;
    factory->CreateFormatConverter(&conv);
    hr = conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                          nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        conv->Release(); frame->Release(); decoder->Release(); stream->Release(); factory->Release();
        return false;
    }

    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = LONG(w);
    bmi.bmiHeader.biHeight = -LONG(h);
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbmp || !bits) {
        conv->Release(); frame->Release(); decoder->Release(); stream->Release(); factory->Release();
        return false;
    }

    const UINT stride = w * 4;
    hr = conv->CopyPixels(nullptr, stride, stride * h, (BYTE*)bits);

    conv->Release();
    frame->Release();
    decoder->Release();
    stream->Release();
    factory->Release();

    if (FAILED(hr)) {
        DeleteObject(hbmp);
        return false;
    }
    if (meta) {
        meta->w = int(w);
        meta->h = int(h);
        meta->stride = int(stride);
    }
    *outBmp = hbmp;
    return true;
}

static bool loadLiveIconFromResource() {
    HRSRC res = FindResourceW(g.hi, MAKEINTRESOURCEW(IDR_LIVE), RT_RCDATA);
    if (!res) return false;
    HGLOBAL hg = LoadResource(g.hi, res);
    if (!hg) return false;
    void* data = LockResource(hg);
    DWORD size = SizeofResource(g.hi, res);
    if (!data || !size) return false;

    RgbImage meta{};
    HBITMAP bmp = nullptr;
    if (!decodeToHBitmap((const unsigned char*)data, size, &meta, &bmp))
        return false;
    g.hbmLive = bmp;
    g.liveW = meta.w;
    g.liveH = meta.h;
    return true;
}

// ---------- view transform ----------

static float fitScale(int cw, int ch) {
    if (g.still.w <= 0 || g.still.h <= 0 || cw <= 0 || ch <= 0) return 1.f;
    return (std::min)(float(cw) / float(g.still.w), float(ch) / float(g.still.h));
}

static RECT imageDestRect(int cw, int ch) {
    RECT r{0, 0, cw, ch};
    if (g.still.w <= 0 || g.still.h <= 0) return r;
    const float s = fitScale(cw, ch) * g.zoom;
    const int dw = (std::max)(1, int(std::lround(g.still.w * s)));
    const int dh = (std::max)(1, int(std::lround(g.still.h * s)));
    r.left = int(std::lround((cw - dw) * 0.5f + g.panX));
    r.top = int(std::lround((ch - dh) * 0.5f + g.panY));
    r.right = r.left + dw;
    r.bottom = r.top + dh;
    return r;
}

static void clampPan(int cw, int ch) {
    if (g.still.w <= 0) return;
    const float s = fitScale(cw, ch) * g.zoom;
    const float dw = g.still.w * s;
    const float dh = g.still.h * s;
    // Allow panning so edges can reach center-ish; keep some of image visible
    const float maxX = (std::max)(0.f, (dw - cw) * 0.5f + 40.f);
    const float maxY = (std::max)(0.f, (dh - ch) * 0.5f + 40.f);
    g.panX = (std::max)(-maxX, (std::min)(maxX, g.panX));
    g.panY = (std::max)(-maxY, (std::min)(maxY, g.panY));
}

static void updateLiveBtnRect() {
    const int sz = kLiveIconDraw;
    g.liveBtn = {14, 14, 14 + sz, 14 + sz};
}

static void updateNavBtnRects(int cw, int ch) {
    const int bw = 48;
    const int bh = 80;
    const int y = (ch - bh) / 2;
    g.prevBtn = {16, y, 16 + bw, y + bh};
    g.nextBtn = {cw - 16 - bw, y, cw - 16, y + bh};
}

static bool canNavigate() {
    return g.hasFile && g.neighbors.size() > 1 && g.neighborIndex >= 0;
}

static void drawNavButton(HDC mem, const RECT& r, bool leftArrow, bool hot) {
    const BYTE alpha = hot ? BYTE(200) : BYTE(120);
    // Soft round plate
    HDC plate = CreateCompatibleDC(mem);
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hb = CreateDIBSection(plate, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hb || !bits) {
        if (hb) DeleteObject(hb);
        DeleteDC(plate);
        return;
    }
    memset(bits, 0, size_t(w) * size_t(h) * 4);
    HGDIOBJ old = SelectObject(plate, hb);
    HBRUSH br = CreateSolidBrush(RGB(40, 40, 40));
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(80, 80, 80));
    HGDIOBJ obr = SelectObject(plate, br);
    HGDIOBJ opn = SelectObject(plate, pen);
    RoundRect(plate, 0, 0, w, h, 18, 18);
    SelectObject(plate, obr);
    SelectObject(plate, opn);
    DeleteObject(br);
    DeleteObject(pen);

    // Chevron
    HPEN ap = CreatePen(PS_SOLID, 3, RGB(255, 255, 255));
    HGDIOBJ oap = SelectObject(plate, ap);
    HGDIOBJ obr2 = SelectObject(plate, GetStockObject(NULL_BRUSH));
    const int cx = w / 2, cy = h / 2;
    if (leftArrow) {
        MoveToEx(plate, cx + 6, cy - 14, nullptr);
        LineTo(plate, cx - 8, cy);
        LineTo(plate, cx + 6, cy + 14);
    } else {
        MoveToEx(plate, cx - 6, cy - 14, nullptr);
        LineTo(plate, cx + 8, cy);
        LineTo(plate, cx - 6, cy + 14);
    }
    SelectObject(plate, oap);
    SelectObject(plate, obr2);
    DeleteObject(ap);

    // Premultiply alpha for AlphaBlend
    auto* px = static_cast<unsigned char*>(bits);
    for (int i = 0; i < w * h; ++i) {
        unsigned char* p = px + i * 4;
        if (p[0] | p[1] | p[2]) {
            p[0] = (unsigned char)(p[0] * alpha / 255);
            p[1] = (unsigned char)(p[1] * alpha / 255);
            p[2] = (unsigned char)(p[2] * alpha / 255);
            p[3] = alpha;
        }
    }

    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    AlphaBlend(mem, r.left, r.top, w, h, plate, 0, 0, w, h, bf);
    SelectObject(plate, old);
    DeleteObject(hb);
    DeleteDC(plate);
}

static void refreshNeighbors(const wchar_t* filePath) {
    g.neighbors.clear();
    g.neighborIndex = -1;
    if (!filePath || !*filePath) return;

    wchar_t dir[MAX_PATH];
    wcsncpy_s(dir, filePath, _TRUNCATE);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (!slash) return;
    *slash = L'\0';
    const std::wstring folder = dir;
    const std::wstring pattern = folder + L"\\*.livp";

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        g.neighbors.push_back(folder + L"\\" + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(g.neighbors.begin(), g.neighbors.end(),
              [](const std::wstring& a, const std::wstring& b) {
                  return _wcsicmp(a.c_str(), b.c_str()) < 0;
              });

    for (size_t i = 0; i < g.neighbors.size(); ++i) {
        if (_wcsicmp(g.neighbors[i].c_str(), filePath) == 0) {
            g.neighborIndex = (int)i;
            break;
        }
    }
}

static void openLivp(const wchar_t* path); // fwd
static void stopPlayback();

static void goNeighbor(int delta) {
    if (!canNavigate()) return;
    if (g.playing) stopPlayback();
    const int n = (int)g.neighbors.size();
    int idx = (g.neighborIndex + delta) % n;
    if (idx < 0) idx += n;
    openLivp(g.neighbors[idx].c_str());
}

// Draw still / empty hint / LIVE into any HDC sized to (cw,ch)
static void drawStillContent(HDC mem, int cw, int ch) {
    RECT rc{0, 0, cw, ch};
    HBRUSH bg = CreateSolidBrush(RGB(12, 12, 12));
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    if (g.hbmStill && g.still.w > 0) {
        RECT dest = imageDestRect(cw, ch);
        if (dest.right > dest.left && dest.bottom > dest.top) {
            HDC src = CreateCompatibleDC(mem);
            HGDIOBJ o = SelectObject(src, g.hbmStill);
            SetStretchBltMode(mem, COLORONCOLOR);
            StretchBlt(mem, dest.left, dest.top, dest.right - dest.left, dest.bottom - dest.top,
                       src, 0, 0, g.still.w, g.still.h, SRCCOPY);
            SelectObject(src, o);
            DeleteDC(src);
        }
    } else if (!g.hasFile) {
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(210, 210, 210));
        HFONT font = CreateFontW(28, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(mem, font);
        const wchar_t* tip = L"点击打开 .livp\n或拖入文件";
        RECT calc{0, 0, cw, ch};
        DrawTextW(mem, tip, -1, &calc, DT_CALCRECT | DT_CENTER | DT_WORDBREAK);
        const int th = calc.bottom - calc.top;
        const int tw = calc.right - calc.left;
        RECT textRc{(cw - tw) / 2, (ch - th) / 2, (cw - tw) / 2 + tw, (ch - th) / 2 + th};
        DrawTextW(mem, tip, -1, &textRc, DT_CENTER | DT_WORDBREAK);
        SelectObject(mem, oldFont);
        DeleteObject(font);
    }

    if (g.hasFile && g.hbmLive) {
        updateLiveBtnRect();
        const int sz = kLiveIconDraw + (g.liveHot ? 4 : 0);
        const int x = g.liveBtn.left - (g.liveHot ? 2 : 0);
        const int y = g.liveBtn.top - (g.liveHot ? 2 : 0);
        g.liveBtn = {x, y, x + sz, y + sz};

        HDC src = CreateCompatibleDC(mem);
        HGDIOBJ o = SelectObject(src, g.hbmLive);
        BLENDFUNCTION bf{AC_SRC_OVER, 0, g.liveHot ? BYTE(255) : BYTE(220), AC_SRC_ALPHA};
        AlphaBlend(mem, x, y, sz, sz, src, 0, 0, g.liveW, g.liveH, bf);
        SelectObject(src, o);
        DeleteDC(src);
    }

    if (canNavigate()) {
        updateNavBtnRects(cw, ch);
        drawNavButton(mem, g.prevBtn, true, g.prevHot);
        drawNavButton(mem, g.nextBtn, false, g.nextHot);
    } else {
        SetRectEmpty(&g.prevBtn);
        SetRectEmpty(&g.nextBtn);
    }
}

static void paintStill(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int cw = rc.right - rc.left;
    const int ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) {
        EndPaint(hwnd, &ps);
        return;
    }

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP back = CreateCompatibleBitmap(hdc, cw, ch);
    HGDIOBJ oldBmp = SelectObject(mem, back);
    drawStillContent(mem, cw, ch);
    BitBlt(hdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(back);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

static void forceRepaintStill() {
    if (!g.hwnd) return;
    if (g.hwndVideo) {
        SetWindowPos(g.hwndVideo, nullptr, 0, 0, 0, 0,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_HIDEWINDOW | SWP_NOMOVE);
    }
    if (g.hwndCover) {
        RECT rc{};
        GetClientRect(g.hwnd, &rc);
        SetWindowPos(g.hwndCover, HWND_TOP, 0, 0,
                     (std::max)(1, (int)(rc.right - rc.left)),
                     (std::max)(1, (int)(rc.bottom - rc.top)),
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(g.hwndCover, nullptr, FALSE);
        UpdateWindow(g.hwndCover);
    }
    InvalidateRect(g.hwnd, nullptr, FALSE);
    UpdateWindow(g.hwnd);
}

// ---------- MFPlay ----------

class PlayerCb final : public IMFPMediaPlayerCallback {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** pp) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFPMediaPlayerCallback)) {
            *pp = static_cast<IMFPMediaPlayerCallback*>(this);
            AddRef();
            return S_OK;
        }
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG n = InterlockedDecrement(&ref_);
        if (!n) delete this;
        return n;
    }
    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* header) override {
        if (!header) return;
        // No PLAYBACK_STARTED in MFPlay — use PLAY / MEDIAITEM_SET as "first frame ready"
        if (header->eEventType == MFP_EVENT_TYPE_PLAY ||
            header->eEventType == MFP_EVENT_TYPE_MEDIAITEM_SET) {
            PostMessageW(g.hwnd, WM_APP_PLAY_STARTED, 0, 0);
        } else if (header->eEventType == MFP_EVENT_TYPE_PLAYBACK_ENDED ||
                   header->eEventType == MFP_EVENT_TYPE_ERROR) {
            PostMessageW(g.hwnd, WM_APP_PLAY_ENDED, 0, 0);
        }
    }
private:
    LONG ref_ = 1;
};

static void ensureMf() {
    if (g.mfReady) return;
    if (SUCCEEDED(MFStartup(MF_VERSION))) g.mfReady = true;
}

static constexpr UINT_PTR kTimerClickPlay = 1;
static constexpr UINT_PTR kTimerRevealVideo = 2;
static constexpr UINT kClickPlayDelayMs = 220;

static LRESULT CALLBACK VideoWndProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK CoverWndProc(HWND, UINT, WPARAM, LPARAM);

static void layoutCover(bool show) {
    if (!g.hwndCover || !g.hwnd) return;
    RECT rc{};
    GetClientRect(g.hwnd, &rc);
    const int w = (std::max)(1, (int)(rc.right - rc.left));
    const int h = (std::max)(1, (int)(rc.bottom - rc.top));
    SetWindowPos(g.hwndCover, HWND_TOP, 0, 0, w, h,
                 SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    if (show) {
        InvalidateRect(g.hwndCover, nullptr, FALSE);
        UpdateWindow(g.hwndCover);
    }
}

static HWND createVideoChild() {
    HWND hw = CreateWindowExW(0, kVideoClass, L"",
                              WS_CHILD | WS_CLIPSIBLINGS,
                              0, 0, 1, 1, g.hwnd, nullptr, g.hi, nullptr);
    if (hw) {
        DragAcceptFiles(hw, TRUE);
        ShowWindow(hw, SW_HIDE);
    }
    return hw;
}

static void recreateVideoChild() {
    if (g.hwndVideo) {
        DestroyWindow(g.hwndVideo);
        g.hwndVideo = nullptr;
    }
    g.hwndVideo = createVideoChild();
}

// Place video under the cover; do NOT paint black (cover hides it until ready)
static void prepareVideoUnderCover() {
    if (!g.hwndVideo || !g.hwnd) return;
    RECT rc{};
    GetClientRect(g.hwnd, &rc);
    const int w = (std::max)(1, (int)(rc.right - rc.left));
    const int h = (std::max)(1, (int)(rc.bottom - rc.top));
    // Bottom of z-order, shown — EVR can render while cover occludes
    SetWindowPos(g.hwndVideo, HWND_BOTTOM, 0, 0, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    layoutCover(true); // still on top
}

static void destroyPlayerOnly() {
    if (g.player) {
        g.player->Stop();
        g.player->Shutdown();
        g.player->Release();
        g.player = nullptr;
    }
}

static void cancelPendingPlay() {
    if (g.hwnd) {
        KillTimer(g.hwnd, kTimerClickPlay);
        KillTimer(g.hwnd, kTimerRevealVideo);
    }
}

static void stopPlayback() {
    cancelPendingPlay();
    // Put still cover back FIRST so user never sees a black video surface
    g.playing = false;
    g.videoShown = false;
    layoutCover(true);
    destroyPlayerOnly();
    if (!g.tempVideo.empty()) {
        DeleteFileW(g.tempVideo.c_str());
        g.tempVideo.clear();
    }
    recreateVideoChild(); // drop EVR last-frame surface
    if (g.hwndVideo) ShowWindow(g.hwndVideo, SW_HIDE);
    InvalidateRect(g.hwndCover, nullptr, FALSE);
    UpdateWindow(g.hwndCover);
}

static void revealVideoSurface() {
    if (!g.playing || g.videoShown) return;
    g.videoShown = true;
    if (g.player) g.player->UpdateVideo();
    // Lift cover — video underneath should already have frames (no black flash)
    layoutCover(false);
}

static void startPlayback() {
    cancelPendingPlay();
    if (!g.hasFile || g.videoBytes.empty() || g.playing) return;
    ensureMf();
    if (!g.mfReady) {
        MessageBoxW(g.hwnd, L"无法初始化 Media Foundation", L"LivpViewer", MB_ICONERROR);
        return;
    }

    destroyPlayerOnly();
    g.videoShown = false;
    if (!g.tempVideo.empty()) {
        DeleteFileW(g.tempVideo.c_str());
        g.tempVideo.clear();
    }

    recreateVideoChild();
    if (!g.hwndVideo) {
        MessageBoxW(g.hwnd, L"无法创建视频窗口", L"LivpViewer", MB_ICONERROR);
        return;
    }

    wchar_t tmpDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tmpDir);
    wchar_t tmpFile[MAX_PATH];
    GetTempFileNameW(tmpDir, L"livp", 0, tmpFile);
    DeleteFileW(tmpFile);
    std::wstring path = tmpFile;
    size_t dot = path.find_last_of(L'.');
    if (dot != std::wstring::npos) path = path.substr(0, dot);
    path += g.videoExt;

    if (!writeFileW(path.c_str(), g.videoBytes.data(), g.videoBytes.size())) {
        MessageBoxW(g.hwnd, L"无法写入临时视频文件", L"LivpViewer", MB_ICONERROR);
        return;
    }
    g.tempVideo = path;

    g.playing = true;
    // Video under cover; user still sees the still image
    prepareVideoUnderCover();

    PlayerCb* cb = new PlayerCb();
    HRESULT hr = MFPCreateMediaPlayer(path.c_str(), FALSE, 0, cb, g.hwndVideo, &g.player);
    cb->Release();
    if (FAILED(hr) || !g.player) {
        g.playing = false;
        DeleteFileW(path.c_str());
        g.tempVideo.clear();
        recreateVideoChild();
        layoutCover(true);
        offerInstallCodecs(false, true, g.path.c_str(), true);
        return;
    }
    g.player->Play();
    // Reveal after frames had time to present under the cover
    SetTimer(g.hwnd, kTimerRevealVideo, 220, nullptr);
}

static void clearMedia() {
    stopPlayback();
    if (g.hbmStill) { DeleteObject(g.hbmStill); g.hbmStill = nullptr; }
    g.still = {};
    std::vector<unsigned char>().swap(g.videoBytes);
    g.path.clear();
    g.hasFile = false;
    g.liveHot = false;
    g.zoom = 1.f;
    g.panX = g.panY = 0.f;
    g.panning = false;
    g.neighbors.clear();
    g.neighborIndex = -1;
    g.prevHot = g.nextHot = false;
    forceRepaintStill();
}

static void openDialog();

static void openLivp(const wchar_t* path) {
    // Copy path first — neighbors vector may be cleared/rebuilt later
    std::wstring pathCopy = path ? path : L"";
    if (pathCopy.empty()) return;

    // Keep showing the previous still while we decode the next one (no empty-state flash)
    const bool keepWindowSize = g.hasFile;

    std::vector<unsigned char> stillBytes;
    std::vector<unsigned char> newVideo;
    std::string stillName, videoName;
    RgbImage newStill{};
    HBITMAP newBmp = nullptr;

    if (!extractLivp(pathCopy.c_str(), stillBytes, stillName, newVideo, videoName)) {
        MessageBoxW(g.hwnd, L"无法解析 .livp（需要静图 + MOV）", L"LivpViewer", MB_ICONERROR);
        return; // previous image stays
    }
    if (!decodeToHBitmap(stillBytes.data(), stillBytes.size(), &newStill, &newBmp)) {
        const bool heic = isHeicName(stillName.c_str());
        if (heic) {
            // HEIC 通常还需要 HEVC 扩展
            offerInstallCodecs(true, true, pathCopy.c_str(), false);
        } else {
            MessageBoxW(g.hwnd, L"无法解码静图。", L"LivpViewer", MB_ICONERROR);
        }
        return; // previous image stays
    }

    // Quietly end playback — cover still shows the OLD bitmap until we swap
    cancelPendingPlay();
    destroyPlayerOnly();
    g.playing = false;
    g.videoShown = false;
    if (!g.tempVideo.empty()) {
        DeleteFileW(g.tempVideo.c_str());
        g.tempVideo.clear();
    }
    recreateVideoChild();
    if (g.hwndVideo) ShowWindow(g.hwndVideo, SW_HIDE);
    layoutCover(true);

    // Atomic swap: old → new in one step, then one repaint
    if (g.hbmStill) DeleteObject(g.hbmStill);
    g.hbmStill = newBmp;
    g.still = newStill;
    g.videoBytes.swap(newVideo);
    std::vector<unsigned char>().swap(newVideo); // drop old video buffer

    g.path = pathCopy;
    g.videoExt = L".mov";
    if (hasExt(videoName.c_str(), ".mp4")) g.videoExt = L".mp4";
    else if (hasExt(videoName.c_str(), ".m4v")) g.videoExt = L".m4v";

    g.hasFile = true;
    g.zoom = 1.f;
    g.panX = g.panY = 0.f;
    g.liveHot = g.prevHot = g.nextHot = false;
    refreshNeighbors(pathCopy.c_str());

    const wchar_t* base = wcsrchr(pathCopy.c_str(), L'\\');
    base = base ? base + 1 : pathCopy.c_str();
    wchar_t title[512];
    if (canNavigate())
        wsprintfW(title, L"%s  (%d/%d) — LivpViewer", base,
                  g.neighborIndex + 1, (int)g.neighbors.size());
    else
        wsprintfW(title, L"%s — LivpViewer", base);
    SetWindowTextW(g.hwnd, title);

    // Resizing every switch causes a black flash — only size on first open
    if (!keepWindowSize) {
        int tw = g.still.w + 16;
        int th = g.still.h + 16;
        if (tw < 480) tw = 480;
        if (th < 320) th = 320;
        if (tw > 1400) tw = 1400;
        if (th > 900) th = 900;
        RECT wr{0, 0, tw, th};
        AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, 0);
        SetWindowPos(g.hwnd, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top,
                     SWP_NOMOVE | SWP_NOZORDER);
    }

    layoutCover(true);
    if (g.hwndCover) {
        InvalidateRect(g.hwndCover, nullptr, FALSE);
        UpdateWindow(g.hwndCover);
    }
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

static void openDialog() {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = L"Apple Live Photo (*.livp)\0*.livp\0All\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) openLivp(file);
}

static void applyWheelZoom(int mouseX, int mouseY, int delta) {
    if (!g.hasFile || g.playing) return;
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    const int cw = rc.right - rc.left;
    const int ch = rc.bottom - rc.top;

    const float oldZoom = g.zoom;
    const float factor = (delta > 0) ? 1.12f : (1.f / 1.12f);
    float newZoom = oldZoom * factor;
    if (newZoom < 0.2f) newZoom = 0.2f;
    if (newZoom > 8.f) newZoom = 8.f;
    if (fabsf(newZoom - 1.f) < 0.03f && (delta < 0) && oldZoom > 1.f)
        newZoom = 1.f;

    // Zoom toward cursor: keep image point under cursor stable
    const float s0 = fitScale(cw, ch) * oldZoom;
    const float s1 = fitScale(cw, ch) * newZoom;
    const float cx0 = cw * 0.5f + g.panX;
    const float cy0 = ch * 0.5f + g.panY;
    // image pixel under cursor
    const float ix = (mouseX - cx0) / s0 + g.still.w * 0.5f;
    const float iy = (mouseY - cy0) / s0 + g.still.h * 0.5f;

    g.zoom = newZoom;
    if (newZoom <= 1.001f) {
        g.zoom = 1.f;
        g.panX = g.panY = 0.f;
    } else {
        g.panX = mouseX - cw * 0.5f - (ix - g.still.w * 0.5f) * s1;
        g.panY = mouseY - ch * 0.5f - (iy - g.still.h * 0.5f) * s1;
        clampPan(cw, ch);
    }
    InvalidateRect(g.hwnd, nullptr, FALSE);
}

static LRESULT CALLBACK CoverWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        const int cw = rc.right - rc.left;
        const int ch = rc.bottom - rc.top;
        if (cw > 0 && ch > 0) {
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP back = CreateCompatibleBitmap(hdc, cw, ch);
            HGDIOBJ oldBmp = SelectObject(mem, back);
            drawStillContent(mem, cw, ch);
            BitBlt(hdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldBmp);
            DeleteObject(back);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MOUSEMOVE:
    case WM_MOUSEWHEEL:
    case WM_KEYDOWN:
    case WM_DROPFILES:
        // Forward interaction to main window
        if (msg == WM_MOUSEWHEEL) {
            // wheel coords are screen-space already in wp/lp for the target
            return SendMessageW(g.hwnd, msg, wp, lp);
        }
        if (msg == WM_DROPFILES) {
            return SendMessageW(g.hwnd, msg, wp, lp);
        }
        {
            // Convert client coords if needed — cover is same size/origin as client
            return SendMessageW(g.hwnd, msg, wp, lp);
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK VideoWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1; // no black fill — cover hides us until reveal
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        PostMessageW(g.hwnd, WM_APP_PLAY_ENDED, 0, 0);
        return 0;
    case WM_LBUTTONDBLCLK:
        cancelPendingPlay();
        stopPlayback();
        openDialog();
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE || wp == VK_SPACE)
            PostMessageW(g.hwnd, WM_APP_PLAY_ENDED, 0, 0);
        else if (wp == 'O' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            stopPlayback();
            openDialog();
        }
        return 0;
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        wchar_t path[MAX_PATH];
        if (DragQueryFileW(drop, 0, path, MAX_PATH))
            openLivp(path);
        DragFinish(drop);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        DragAcceptFiles(hwnd, TRUE);
        return 0;

    case WM_PAINT:
        paintStill(hwnd);
        return 0;

    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wp;
        RECT rc; GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(12, 12, 12));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);
        return 1;
    }

    case WM_SIZE: {
        RECT rc; GetClientRect(hwnd, &rc);
        clampPan(rc.right, rc.bottom);
        if (g.playing && g.hwndVideo) {
            SetWindowPos(g.hwndVideo, HWND_BOTTOM, 0, 0,
                         (std::max)(1, (int)(rc.right - rc.left)),
                         (std::max)(1, (int)(rc.bottom - rc.top)),
                         SWP_NOACTIVATE);
            if (g.player) g.player->UpdateVideo();
        }
        if (!g.videoShown)
            layoutCover(true);
        else
            layoutCover(false);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_TIMER:
        if (wp == kTimerClickPlay) {
            KillTimer(hwnd, kTimerClickPlay);
            if (g.hasFile && !g.playing) startPlayback();
        } else if (wp == kTimerRevealVideo) {
            KillTimer(hwnd, kTimerRevealVideo);
            revealVideoSurface();
        }
        return 0;

    case WM_MOUSEWHEEL: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        applyWheelZoom(pt.x, pt.y, GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (g.panning && g.hasFile && !g.playing) {
            g.panX += float(pt.x - g.panLast.x);
            g.panY += float(pt.y - g.panLast.y);
            g.panLast = pt;
            RECT rc; GetClientRect(hwnd, &rc);
            clampPan(rc.right, rc.bottom);
            InvalidateRect(hwnd, nullptr, FALSE);
            if (g.hwndCover) InvalidateRect(g.hwndCover, nullptr, FALSE);
            return 0;
        }
        if (g.videoShown || !g.hasFile) break;
        bool live = PtInRect(&g.liveBtn, pt) != 0;
        bool prev = canNavigate() && PtInRect(&g.prevBtn, pt) != 0;
        bool next = canNavigate() && PtInRect(&g.nextBtn, pt) != 0;
        if (live != g.liveHot || prev != g.prevHot || next != g.nextHot) {
            g.liveHot = live;
            g.prevHot = prev;
            g.nextHot = next;
            InvalidateRect(hwnd, nullptr, FALSE);
            if (g.hwndCover) InvalidateRect(g.hwndCover, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (!g.hasFile) {
            openDialog();
            return 0;
        }
        if (!g.videoShown && canNavigate() && PtInRect(&g.prevBtn, pt)) {
            cancelPendingPlay();
            goNeighbor(-1);
            return 0;
        }
        if (!g.videoShown && canNavigate() && PtInRect(&g.nextBtn, pt)) {
            cancelPendingPlay();
            goNeighbor(1);
            return 0;
        }
        if (g.playing) {
            stopPlayback();
            return 0;
        }
        cancelPendingPlay();
        SetTimer(hwnd, kTimerClickPlay, kClickPlayDelayMs, nullptr);
        return 0;
    }

    case WM_RBUTTONDOWN: {
        if (g.hasFile && !g.playing && g.zoom > 1.001f) {
            g.panning = true;
            g.panLast = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            SetCapture(hwnd);
        }
        return 0;
    }

    case WM_RBUTTONUP:
        if (g.panning) {
            g.panning = false;
            ReleaseCapture();
        }
        return 0;

    case WM_LBUTTONDBLCLK: {
        cancelPendingPlay();
        if (g.playing) stopPlayback();
        openDialog();
        return 0;
    }

    case WM_LBUTTONUP:
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            cancelPendingPlay();
            if (g.playing) stopPlayback();
            else if (g.zoom > 1.001f) {
                g.zoom = 1.f; g.panX = g.panY = 0.f;
                forceRepaintStill();
            } else PostMessageW(hwnd, WM_CLOSE, 0, 0);
        } else if (wp == 'O' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            cancelPendingPlay();
            if (g.playing) stopPlayback();
            openDialog();
        } else if (wp == '0' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            g.zoom = 1.f; g.panX = g.panY = 0.f;
            forceRepaintStill();
        } else if (wp == VK_SPACE || wp == 'L') {
            cancelPendingPlay();
            if (g.playing) stopPlayback();
            else if (g.hasFile) startPlayback();
        } else if (wp == VK_LEFT || wp == VK_PRIOR) {
            cancelPendingPlay();
            goNeighbor(-1);
        } else if (wp == VK_RIGHT || wp == VK_NEXT) {
            cancelPendingPlay();
            goNeighbor(1);
        } else if (wp == 'I' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            // Manual: install HEIF + HEVC
            const wchar_t* p = g.path.empty() ? nullptr : g.path.c_str();
            offerInstallCodecs(true, true, p, false);
        }
        return 0;

    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        wchar_t path[MAX_PATH];
        if (DragQueryFileW(drop, 0, path, MAX_PATH))
            openLivp(path);
        DragFinish(drop);
        return 0;
    }

    case WM_APP_PLAY_STARTED:
        KillTimer(hwnd, kTimerRevealVideo);
        revealVideoSurface();
        return 0;

    case WM_APP_RETRY_CODEC: {
        std::wstring path = g.pendingRetryPath;
        const bool play = g.pendingRetryPlay;
        g.pendingRetryPath.clear();
        g.pendingRetryPlay = false;
        if (!path.empty()) {
            openLivp(path.c_str());
            if (play && g.hasFile)
                startPlayback();
        }
        return 0;
    }

    case WM_APP_PLAY_ENDED:
        stopPlayback();
        return 0;

    case WM_DESTROY:
        cancelPendingPlay();
        clearMedia();
        if (g.hbmLive) { DeleteObject(g.hbmLive); g.hbmLive = nullptr; }
        if (g.mfReady) { MFShutdown(); g.mfReady = false; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int APIENTRY wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g.hi = hi;

    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hrCom) && hrCom != RPC_E_CHANGED_MODE) return 1;

    loadLiveIconFromResource();

    HICON iconBig = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    HICON iconSm = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                     GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    wc.hIcon = iconBig ? iconBig : LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm = iconSm ? iconSm : wc.hIcon;
    RegisterClassExW(&wc);

    WNDCLASSEXW vc{sizeof(vc)};
    vc.lpfnWndProc = VideoWndProc;
    vc.hInstance = hi;
    vc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    vc.hbrBackground = nullptr; // no black flash from class brush
    vc.lpszClassName = kVideoClass;
    RegisterClassExW(&vc);

    WNDCLASSEXW cc{sizeof(cc)};
    cc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    cc.lpfnWndProc = CoverWndProc;
    cc.hInstance = hi;
    cc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    cc.hbrBackground = nullptr;
    cc.lpszClassName = kCoverClass;
    RegisterClassExW(&cc);

    g.hwnd = CreateWindowExW(0, kClassName, L"LivpViewer",
                             WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, 960, 640,
                             nullptr, nullptr, hi, nullptr);
    if (!g.hwnd) {
        CoUninitialize();
        return 1;
    }

    g.hwndVideo = createVideoChild();
    g.hwndCover = CreateWindowExW(0, kCoverClass, L"",
                                  WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE,
                                  0, 0, 100, 100, g.hwnd, nullptr, hi, nullptr);
    if (!g.hwndVideo || !g.hwndCover) {
        DestroyWindow(g.hwnd);
        CoUninitialize();
        return 1;
    }
    DragAcceptFiles(g.hwndCover, TRUE);
    layoutCover(true);

    if (iconBig) SendMessageW(g.hwnd, WM_SETICON, ICON_BIG, (LPARAM)iconBig);
    if (iconSm) SendMessageW(g.hwnd, WM_SETICON, ICON_SMALL, (LPARAM)iconSm);

    ShowWindow(g.hwnd, show);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 2) openLivp(argv[1]);
    if (argv) LocalFree(argv);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return int(msg.wParam);
}
