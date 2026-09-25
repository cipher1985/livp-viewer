/*
 * XLivp - XnView format plugin for Apple Live Photo (.livp)
 *
 * .livp is a ZIP containing a still (HEIC/JPEG) + a short MOV.
 * This plugin only exposes the still image to XnView.
 *
 * Build (x64): build.bat
 * Install: copy XLivp.usr into XnViewMP\Plugins
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <objbase.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#define MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

#define API __stdcall

#define GFP_RGB  0
#define GFP_BGR  1
#define GFP_GREY 5

#define GFP_READ  0x0001
#define GFP_WRITE 0x0002

typedef struct {
    unsigned char red[256];
    unsigned char green[256];
    unsigned char blue[256];
} GFP_COLORMAP;

struct LivpData {
    int width = 0;
    int height = 0;
    int stride = 0; // bytes per line (RGB24, tightly packed)
    std::vector<unsigned char> pixels; // top-down RGB24
};

static bool iequals_ascii(const char* a, const char* b) {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (*b + 32) : *b;
        if (ca != cb) return false;
        ++a; ++b;
    }
    return *a == *b;
}

static bool has_ext(const char* name, const char* ext) {
    size_t n = strlen(name);
    size_t e = strlen(ext);
    if (n < e) return false;
    return iequals_ascii(name + (n - e), ext);
}

static bool is_still_name(const char* name) {
    return has_ext(name, ".jpg") || has_ext(name, ".jpeg") ||
           has_ext(name, ".heic") || has_ext(name, ".heif") ||
           has_ext(name, ".png") || has_ext(name, ".tif") ||
           has_ext(name, ".tiff");
}

static bool read_file_bytes(const char* path, std::vector<unsigned char>& out) {
    int wlen = MultiByteToWideChar(CP_ACP, 0, path, -1, nullptr, 0);
    if (wlen <= 0)
        return false;
    std::wstring wpath(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_ACP, 0, path, -1, &wpath[0], wlen);
    if (!wpath.empty() && wpath.back() == L'\0')
        wpath.pop_back();

    HANDLE h = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li) || li.QuadPart <= 0 || li.QuadPart > 512ll * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(li.QuadPart));
    DWORD got = 0;
    BOOL ok = ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr);
    CloseHandle(h);
    return ok && got == out.size();
}

static bool extract_still_from_livp(const char* path, std::vector<unsigned char>& out, std::string& out_name) {
    std::vector<unsigned char> file_bytes;
    if (!read_file_bytes(path, file_bytes))
        return false;

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, file_bytes.data(), file_bytes.size(), 0))
        return false;

    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    int best = -1;
    int best_score = -1;
    char name[512];

    for (mz_uint i = 0; i < n; ++i) {
        if (mz_zip_reader_is_file_a_directory(&zip, i))
            continue;
        mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
        if (!is_still_name(name))
            continue;
        int score = 1;
        if (has_ext(name, ".heic") || has_ext(name, ".heif")) score = 3;
        else if (has_ext(name, ".jpg") || has_ext(name, ".jpeg")) score = 2;
        if (score > best_score) {
            best_score = score;
            best = (int)i;
            out_name = name;
        }
    }

    if (best < 0) {
        mz_zip_reader_end(&zip);
        return false;
    }

    size_t sz = 0;
    void* p = mz_zip_reader_extract_to_heap(&zip, (mz_uint)best, &sz, 0);
    mz_zip_reader_end(&zip);
    if (!p || sz == 0)
        return false;

    out.assign((unsigned char*)p, (unsigned char*)p + sz);
    mz_free(p);
    return true;
}

static bool decode_with_wic(const unsigned char* data, size_t size, LivpData& img) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool need_uninit = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
        return false;

    IWICImagingFactory* factory = nullptr;
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        if (need_uninit) CoUninitialize();
        return false;
    }

    HGLOBAL hmem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hmem) {
        factory->Release();
        if (need_uninit) CoUninitialize();
        return false;
    }
    void* locked = GlobalLock(hmem);
    memcpy(locked, data, size);
    GlobalUnlock(hmem);

    IStream* stream = nullptr;
    hr = CreateStreamOnHGlobal(hmem, TRUE, &stream);
    if (FAILED(hr)) {
        GlobalFree(hmem);
        factory->Release();
        if (need_uninit) CoUninitialize();
        return false;
    }

    IWICBitmapDecoder* decoder = nullptr;
    hr = factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) {
        stream->Release();
        factory->Release();
        if (need_uninit) CoUninitialize();
        return false;
    }

    IWICBitmapFrameDecode* frame = nullptr;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        decoder->Release();
        stream->Release();
        factory->Release();
        if (need_uninit) CoUninitialize();
        return false;
    }

    IWICFormatConverter* converter = nullptr;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr)) {
        frame->Release();
        decoder->Release();
        stream->Release();
        factory->Release();
        if (need_uninit) CoUninitialize();
        return false;
    }

    hr = converter->Initialize(frame, GUID_WICPixelFormat24bppRGB,
                               WICBitmapDitherTypeNone, nullptr, 0.0,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        converter->Release();
        frame->Release();
        decoder->Release();
        stream->Release();
        factory->Release();
        if (need_uninit) CoUninitialize();
        return false;
    }

    UINT w = 0, h = 0;
    converter->GetSize(&w, &h);
    if (w == 0 || h == 0) {
        converter->Release();
        frame->Release();
        decoder->Release();
        stream->Release();
        factory->Release();
        if (need_uninit) CoUninitialize();
        return false;
    }

    img.width = (int)w;
    img.height = (int)h;
    img.stride = (int)w * 3;
    img.pixels.resize((size_t)img.stride * (size_t)img.height);

    hr = converter->CopyPixels(nullptr, (UINT)img.stride, (UINT)img.pixels.size(), img.pixels.data());

    converter->Release();
    frame->Release();
    decoder->Release();
    stream->Release();
    factory->Release();
    if (need_uninit) CoUninitialize();

    return SUCCEEDED(hr);
}

extern "C" BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) {
    return TRUE;
}

extern "C" BOOL API gfpGetPluginInfo(DWORD version, LPSTR label, INT label_max_size,
                                     LPSTR extension, INT extension_max_size, INT* support) {
    if (version != 0x0002)
        return FALSE;

    strncpy_s(label, (size_t)label_max_size, "Apple Live Photo (LIVP still)", _TRUNCATE);
    strncpy_s(extension, (size_t)extension_max_size, "livp", _TRUNCATE);
    *support = GFP_READ;
    return TRUE;
}

extern "C" void* API gfpLoadPictureInit(LPCSTR filename) {
    if (!filename)
        return nullptr;

    std::vector<unsigned char> still;
    std::string still_name;
    if (!extract_still_from_livp(filename, still, still_name))
        return nullptr;

    auto* data = new (std::nothrow) LivpData();
    if (!data)
        return nullptr;

    if (!decode_with_wic(still.data(), still.size(), *data)) {
        delete data;
        return nullptr;
    }
    return data;
}

extern "C" BOOL API gfpLoadPictureGetInfo(void* ptr, INT* pictype, INT* width, INT* height,
                                          INT* dpi, INT* bits_per_pixel, INT* bytes_per_line,
                                          BOOL* has_colormap, LPSTR label, INT label_max_size) {
    auto* data = static_cast<LivpData*>(ptr);
    if (!data)
        return FALSE;

    *pictype = GFP_RGB;
    *width = data->width;
    *height = data->height;
    *dpi = 72;
    *bits_per_pixel = 24;
    *bytes_per_line = data->stride;
    *has_colormap = FALSE;
    strncpy_s(label, (size_t)label_max_size, "Apple Live Photo", _TRUNCATE);
    return TRUE;
}

extern "C" BOOL API gfpLoadPictureGetLine(void* ptr, INT line, unsigned char* buffer) {
    auto* data = static_cast<LivpData*>(ptr);
    if (!data || !buffer || line < 0 || line >= data->height)
        return FALSE;

    memcpy(buffer, data->pixels.data() + (size_t)line * (size_t)data->stride, (size_t)data->stride);
    return TRUE;
}

extern "C" BOOL API gfpLoadPictureGetColormap(void* /*ptr*/, GFP_COLORMAP* /*cmap*/) {
    return FALSE;
}

extern "C" void API gfpLoadPictureExit(void* ptr) {
    delete static_cast<LivpData*>(ptr);
}
