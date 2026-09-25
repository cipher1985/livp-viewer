/* Quick harness: load XLivp.usr and decode a .livp */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

typedef BOOL (__stdcall *GetInfoFn)(DWORD, LPSTR, INT, LPSTR, INT, INT*);
typedef void* (__stdcall *InitFn)(LPCSTR);
typedef BOOL (__stdcall *InfoFn)(void*, INT*, INT*, INT*, INT*, INT*, INT*, BOOL*, LPSTR, INT);
typedef BOOL (__stdcall *LineFn)(void*, INT, unsigned char*);
typedef void (__stdcall *ExitFn)(void*);

int main(int argc, char** argv) {
    const char* dll = argc > 1 ? argv[1] : "XLivp.usr";
    const char* file = argc > 2 ? argv[2] : "sample.livp";

    HMODULE m = LoadLibraryA(dll);
    if (!m) { printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }

    GetInfoFn gi = (GetInfoFn)GetProcAddress(m, "gfpGetPluginInfo");
    InitFn init = (InitFn)GetProcAddress(m, "gfpLoadPictureInit");
    InfoFn info = (InfoFn)GetProcAddress(m, "gfpLoadPictureGetInfo");
    LineFn line = (LineFn)GetProcAddress(m, "gfpLoadPictureGetLine");
    ExitFn exitfn = (ExitFn)GetProcAddress(m, "gfpLoadPictureExit");
    if (!gi || !init || !info || !line || !exitfn) {
        printf("missing exports\n");
        return 1;
    }

    char label[128], ext[64];
    INT support = 0;
    if (!gi(2, label, 128, ext, 64, &support)) {
        printf("gfpGetPluginInfo failed\n");
        return 1;
    }
    printf("plugin: %s  ext=%s  support=%d\n", label, ext, support);

    void* p = init(file);
    if (!p) { printf("init failed for %s\n", file); return 1; }

    INT pictype, w, h, dpi, bpp, bpl;
    BOOL cmap;
    if (!info(p, &pictype, &w, &h, &dpi, &bpp, &bpl, &cmap, label, 128)) {
        printf("getinfo failed\n");
        return 1;
    }
    printf("image: %s  %dx%d  bpp=%d  bpl=%d  type=%d\n", label, w, h, bpp, bpl, pictype);

    unsigned char* buf = (unsigned char*)malloc((size_t)bpl);
    if (!line(p, 0, buf)) { printf("getline failed\n"); return 1; }
    printf("first pixel RGB: %u %u %u\n", buf[0], buf[1], buf[2]);
    free(buf);
    exitfn(p);
    printf("OK\n");
    return 0;
}
