// realforge_engine.exe - RealForge DLSS 5
// Hote D3D12 minimal : chaque image passe dans NVIDIA DLSS (NGX) exactement comme la frame
// d'un jeu, et l'add-on ReShade "RenoDX DLSS5" (renodx-dlss5.addon64) y greffe le Neural
// Rendering DLSS 5.
//
// Le dossier de l'exe doit contenir : dxgi.dll (ReShade), renodx-dlss5.addon64,
// nvngx_dlss.dll, nvngx_dlssnr.dll et ReShade.ini ([RenoDX.DLSS5] EnableHooks=2).
//
// Usage :
//   realforge_engine.exe --in <fichier|dossier> --out <fichier|dossier> [options]
//     --mode image|sequence   image : chaque fichier est independant (reset + warm-up)
//                             sequence : frames d'une video, historique temporel conserve
//     --color linear|sdr      linear : image convertie en lumiere lineaire FP16 (comme un jeu,
//                             recommande) ; sdr : octets sRGB 8 bits tels quels          [linear]
//     --scale F               facteur de sortie (1 = DLAA, 1.5, 2, 3)          [1]
//     --warmup N              evaluations sur la 1re image (ou chaque image)    [12]
//     --frame-passes N        evaluations par frame en mode sequence            [1]
//     --reset-each            reset de l'historique a chaque frame (sequence)
//     --depth F               profondeur constante envoyee a DLSS              [0.5]
//     --preset X              preset DLSS (lettre A..O, ou 0 = defaut)          [0]
//     --max-pixels N          taille de sortie max en pixels (reduction auto)  [8294400]
//     --mix F                 melange resultat/original, 1 = 100 % NR         [1]
//     --orig P                original pour --mix (fichier, ou dossier en mode dossier)
//     --hidden                ne pas afficher la fenetre d'apercu
//     --diagnose              initialise NGX, affiche les capacites et quitte
//     --verbose               journaux NGX sur la sortie d'erreur
// Sortie standard : lignes "INFO ...", "PROGRESS i n", "DONE", "ERROR ...".

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "version.lib")

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

// ---------------------------------------------------------------- utilitaires
static bool g_verbose = false;

// ---------------------------------------------------------------- couleur
static float g_srgbToLin[256];
static void InitColorTables() {
    for (int i = 0; i < 256; ++i) {
        float c = i / 255.0f;
        g_srgbToLin[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }
}
static inline uint8_t LinToSrgb8(float v) {
    if (!(v > 0.0f)) return 0;           // gere aussi NaN
    if (v >= 1.0f) return 255;
    float c = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    return (uint8_t)std::lround(c * 255.0f);
}
static inline uint16_t FloatToHalf(float f) {
    uint32_t x; memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000u;
    int32_t e = (int32_t)((x >> 23) & 0xFF) - 127 + 15;
    uint32_t m = x & 0x7FFFFFu;
    if (e <= 0) {
        if (e < -10) return (uint16_t)sign;
        m |= 0x800000u;
        uint32_t t = 14 - e;
        uint32_t hm = m >> t;
        if ((m >> (t - 1)) & 1u) hm += 1;   // arrondi
        return (uint16_t)(sign | hm);
    }
    if (e >= 31) return (uint16_t)(sign | 0x7C00u);
    uint32_t h = sign | ((uint32_t)e << 10) | (m >> 13);
    if (m & 0x1000u) h += 1;               // arrondi au plus proche
    return (uint16_t)h;
}
static inline float HalfToFloat(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000u) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FFu, x;
    if (e == 0) {
        if (m == 0) x = sign;
        else { e = 1; while (!(m & 0x400u)) { m <<= 1; --e; } m &= 0x3FFu; x = sign | ((e + 112) << 23) | (m << 13); }
    } else if (e == 31) x = sign | 0x7F800000u | (m << 13);
    else x = sign | ((e + 112) << 23) | (m << 13);
    float f; memcpy(&f, &x, 4); return f;
}

static std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static void Out(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

[[noreturn]] static void Fail(const std::string& msg, int code = 1) {
    Out("ERROR %s", msg.c_str());
    exit(code);
}

static void Check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char b[256];
        snprintf(b, sizeof b, "%s a echoue (HRESULT 0x%08lX)", what, (unsigned long)hr);
        Fail(b);
    }
}

static void NVSDK_CONV NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    if (g_verbose) { fprintf(stderr, "NGX: %s", message); fflush(stderr); }
}

// ---------------------------------------------------------------- options
struct Options {
    std::wstring in, out;
    bool sequence = false;
    float scale = 1.0f;
    int warmup = 12;
    bool linear = true;
    int framePasses = 1;
    bool resetEach = false;
    float depth = 0.5f;
    bool depthInverted = false;
    unsigned preset = 0;
    bool hidden = false;
    bool diagnose = false;
    double maxPixels = 8294400.0;  // 3840x2160 : au-dela le Neural Rendering echoue (0xBAD00002)
    float mix = 1.0f;              // part du resultat NR dans l'image finale (0..1)
    std::wstring orig;             // image(s) d'origine pour --mix (sinon l'entree)
};

static Options ParseArgs() {
    Options o;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    auto need = [&](int& i) -> std::wstring {
        if (i + 1 >= argc) Fail("argument manquant apres " + Narrow(argv[i]));
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--in") o.in = need(i);
        else if (a == L"--out") o.out = need(i);
        else if (a == L"--mode") o.sequence = (need(i) == L"sequence");
        else if (a == L"--color") o.linear = (need(i) != L"sdr");
        else if (a == L"--scale") o.scale = (float)_wtof(need(i).c_str());
        else if (a == L"--warmup") o.warmup = _wtoi(need(i).c_str());
        else if (a == L"--frame-passes") o.framePasses = _wtoi(need(i).c_str());
        else if (a == L"--reset-each") o.resetEach = true;
        else if (a == L"--depth") o.depth = (float)_wtof(need(i).c_str());
        else if (a == L"--depth-inverted") o.depthInverted = true;
        else if (a == L"--preset") {
            std::wstring p = need(i);
            if (!p.empty() && iswalpha(p[0])) o.preset = (unsigned)(towupper(p[0]) - L'A' + 1);
            else o.preset = (unsigned)_wtoi(p.c_str());
        }
        else if (a == L"--hidden") o.hidden = true;
        else if (a == L"--diagnose") o.diagnose = true;
        else if (a == L"--max-pixels") o.maxPixels = _wtof(need(i).c_str());
        else if (a == L"--mix") o.mix = (float)_wtof(need(i).c_str());
        else if (a == L"--orig") o.orig = need(i);
        else if (a == L"--verbose") g_verbose = true;
        else Fail("option inconnue : " + Narrow(a));
    }
    LocalFree(argv);
    o.scale = std::clamp(o.scale, 1.0f, 4.0f);
    o.warmup = std::max(1, o.warmup);
    o.framePasses = std::max(1, o.framePasses);
    o.mix = std::clamp(o.mix, 0.0f, 1.0f);
    if (o.maxPixels < 65536.0) o.maxPixels = 65536.0;
    if (!o.diagnose && (o.in.empty() || o.out.empty())) Fail("--in et --out sont obligatoires");
    return o;
}

// ---------------------------------------------------------------- images (WIC)
static ComPtr<IWICImagingFactory> g_wic;

// Charge une image en RGBA8. Si elle depasse maxW x maxH (limite DLSS), elle est reduite
// (bicubique haute qualite, proportions conservees). origW/origH = taille d'origine.
static bool LoadRGBA(const std::wstring& path, UINT maxW, UINT maxH, double maxPix, UINT& w, UINT& h,
                     UINT& origW, UINT& origH, std::vector<uint8_t>& px) {
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(g_wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                WICDecodeMetadataCacheOnDemand, &dec))) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    frame->GetSize(&origW, &origH);
    ComPtr<IWICBitmapSource> src = frame;
    double k = std::min({1.0, (double)maxW / origW, (double)maxH / origH,
                         std::sqrt(maxPix / ((double)origW * origH))});
    if (k < 1.0) {
        UINT nw = std::max(16u, (UINT)std::floor(origW * k)), nh = std::max(16u, (UINT)std::floor(origH * k));
        ComPtr<IWICBitmapScaler> sc;
        if (FAILED(g_wic->CreateBitmapScaler(&sc))) return false;
        if (FAILED(sc->Initialize(frame.Get(), nw, nh, (WICBitmapInterpolationMode)4 /* HighQualityCubic */))) return false;
        src = sc;
    }
    ComPtr<IWICFormatConverter> cv;
    if (FAILED(g_wic->CreateFormatConverter(&cv))) return false;
    if (FAILED(cv->Initialize(src.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                              nullptr, 0.0, WICBitmapPaletteTypeCustom))) return false;
    cv->GetSize(&w, &h);
    px.resize((size_t)w * h * 4);
    if (FAILED(cv->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data()))) return false;
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    return true;
}

static bool SavePNG(const std::wstring& path, UINT w, UINT h, const std::vector<uint8_t>& bgr) {
    ComPtr<IWICStream> stream;
    if (FAILED(g_wic->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
    ComPtr<IWICBitmapEncoder> enc;
    if (FAILED(g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc))) return false;
    if (FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> fr;
    if (FAILED(enc->CreateNewFrame(&fr, nullptr))) return false;
    if (FAILED(fr->Initialize(nullptr))) return false;
    fr->SetSize(w, h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    fr->SetPixelFormat(&fmt);
    if (fmt != GUID_WICPixelFormat24bppBGR) return false;
    if (FAILED(fr->WritePixels(h, w * 3, (UINT)bgr.size(), const_cast<BYTE*>(bgr.data())))) return false;
    if (FAILED(fr->Commit())) return false;
    return SUCCEEDED(enc->Commit());
}

static bool IsImageExt(const fs::path& p) {
    std::wstring e = p.extension().wstring();
    for (auto& c : e) c = (wchar_t)towlower(c);
    return e == L".png" || e == L".jpg" || e == L".jpeg" || e == L".bmp" || e == L".tif" ||
           e == L".tiff" || e == L".webp" || e == L".jxr" || e == L".heic";
}

// ---------------------------------------------------------------- D3D12
struct Tex {
    ComPtr<ID3D12Resource> res;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    UINT w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
};

struct Gpu {
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue = 0;
    HANDLE fenceEvent = nullptr;

    HWND hwnd = nullptr;
    ComPtr<IDXGISwapChain3> swap;
    UINT swapW = 0, swapH = 0;

    void Begin() {
        Check(alloc->Reset(), "CommandAllocator::Reset");
        Check(list->Reset(alloc.Get(), nullptr), "CommandList::Reset");
    }
    void WaitIdle() {
        Check(queue->Signal(fence.Get(), ++fenceValue), "Queue::Signal");
        if (fence->GetCompletedValue() < fenceValue) {
            fence->SetEventOnCompletion(fenceValue, fenceEvent);
            WaitForSingleObject(fenceEvent, INFINITE);
        }
    }
    void Submit() {
        Check(list->Close(), "CommandList::Close");
        ID3D12CommandList* l[] = {list.Get()};
        queue->ExecuteCommandLists(1, l);
        WaitIdle();
        HRESULT r = device->GetDeviceRemovedReason();
        if (FAILED(r)) Check(r, "Device (TDR / device removed)");
    }
    void Transition(Tex& t, D3D12_RESOURCE_STATES to) {
        if (t.state == to) return;
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = t.res.Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = t.state;
        b.Transition.StateAfter = to;
        list->ResourceBarrier(1, &b);
        t.state = to;
    }
    Tex MakeTex(DXGI_FORMAT fmt, UINT w, UINT h, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st,
                const wchar_t* name) {
        Tex t; t.w = w; t.h = h; t.fmt = fmt; t.state = st;
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.Format = fmt; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; d.Flags = flags;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_PPV_ARGS(&t.res)),
              "CreateCommittedResource(texture)");
        t.res->SetName(name);
        return t;
    }
    ComPtr<ID3D12Resource> MakeBuffer(UINT64 size, D3D12_HEAP_TYPE type) {
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = type;
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = size; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;
        D3D12_RESOURCE_STATES st = type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                                   : D3D12_RESOURCE_STATE_COPY_DEST;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_PPV_ARGS(&r)),
              "CreateCommittedResource(buffer)");
        return r;
    }
    // Copie des pixels (lignes serrees de rowBytes) vers la texture, puis passage a l'etat 'after'.
    void Upload(Tex& t, const void* data, UINT rowBytes, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_DESC d = t.res->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT rows; UINT64 rowSize, total;
        device->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &rowSize, &total);
        auto up = MakeBuffer(total, D3D12_HEAP_TYPE_UPLOAD);
        uint8_t* m = nullptr;
        D3D12_RANGE none = {0, 0};
        Check(up->Map(0, &none, (void**)&m), "Map(upload)");
        for (UINT y = 0; y < rows; ++y)
            memcpy(m + fp.Offset + (size_t)y * fp.Footprint.RowPitch, (const uint8_t*)data + (size_t)y * rowBytes, rowBytes);
        up->Unmap(0, nullptr);
        Begin();
        Transition(t, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst = {}; dst.pResource = t.res.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src = {}; src.pResource = up.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Transition(t, after);
        Submit();
    }
    // Relit une texture (RGBA8 sRGB ou RGBA16F lineaire) et renvoie des pixels BGR 24 bits sRGB.
    std::vector<uint8_t> ReadbackBGR(Tex& t, D3D12_RESOURCE_STATES restore) {
        D3D12_RESOURCE_DESC d = t.res->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; UINT rows; UINT64 rowSize, total;
        device->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &rowSize, &total);
        auto rb = MakeBuffer(total, D3D12_HEAP_TYPE_READBACK);
        Begin();
        Transition(t, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = {}; dst.pResource = rb.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION src = {}; src.pResource = t.res.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Transition(t, restore);
        Submit();
        std::vector<uint8_t> bgr((size_t)t.w * t.h * 3);
        uint8_t* m = nullptr;
        D3D12_RANGE r = {0, (SIZE_T)total};
        Check(rb->Map(0, &r, (void**)&m), "Map(readback)");
        const bool half = t.fmt == DXGI_FORMAT_R16G16B16A16_FLOAT;
        for (UINT y = 0; y < t.h; ++y) {
            const uint8_t* s = m + fp.Offset + (size_t)y * fp.Footprint.RowPitch;
            uint8_t* o = bgr.data() + (size_t)y * t.w * 3;
            if (half) {
                const uint16_t* hs = (const uint16_t*)s;
                for (UINT x = 0; x < t.w; ++x, hs += 4, o += 3) {
                    o[0] = LinToSrgb8(HalfToFloat(hs[2]));
                    o[1] = LinToSrgb8(HalfToFloat(hs[1]));
                    o[2] = LinToSrgb8(HalfToFloat(hs[0]));
                }
            } else {
                for (UINT x = 0; x < t.w; ++x, s += 4, o += 3) { o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; }
            }
        }
        D3D12_RANGE none = {0, 0};
        rb->Unmap(0, &none);
        return bgr;
    }
};

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) return 0;  // la fenetre se ferme avec le processus
    return DefWindowProcW(h, m, w, l);
}

static void Pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}

static void CreateGpu(Gpu& g) {
    // 1) DXGI d'abord : charge dxgi.dll du dossier de l'exe (= ReShade) avant d3d12.dll
    Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&g.factory)), "CreateDXGIFactory2");

    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (g.factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)) ==
            DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d);
        if (d.VendorId == 0x10DE) { g.adapter = a; break; }
    }
    if (!g.adapter) Fail("aucun GPU NVIDIA trouve");
    DXGI_ADAPTER_DESC1 ad; g.adapter->GetDesc1(&ad);
    Out("INFO GPU %s (%llu Mo VRAM)", Narrow(ad.Description).c_str(),
        (unsigned long long)(ad.DedicatedVideoMemory >> 20));

    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    if (!d3d12) Fail("d3d12.dll introuvable");
    auto create = (PFN_D3D12_CREATE_DEVICE)GetProcAddress(d3d12, "D3D12CreateDevice");
    Check(create(g.adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g.device)), "D3D12CreateDevice");

    D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Check(g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue)), "CreateCommandQueue");
    Check(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.alloc)),
          "CreateCommandAllocator");
    Check(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc.Get(), nullptr,
                                      IID_PPV_ARGS(&g.list)), "CreateCommandList");
    g.list->Close();
    Check(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)), "CreateFence");
    g.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

static DXGI_FORMAT g_swapFmt = DXGI_FORMAT_R8G8B8A8_UNORM;

static void EnsureSwapChain(Gpu& g, UINT w, UINT h, bool hidden) {
    // Taille d'apercu : tient dans 1280x720
    float k = std::min(1.0f, std::min(1280.0f / w, 720.0f / h));
    int cw = std::max(64, (int)(w * k)), ch = std::max(64, (int)(h * k));
    if (!g.hwnd) {
        WNDCLASSW wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"RealForgeDLSS5"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        RECT r = {0, 0, cw, ch};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        g.hwnd = CreateWindowW(wc.lpszClassName, L"RealForge DLSS 5 - apercu du rendu", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                               nullptr, nullptr, wc.hInstance, nullptr);
        ShowWindow(g.hwnd, hidden ? SW_HIDE : SW_SHOWNOACTIVATE);
    } else if (g.swapW != w || g.swapH != h) {
        RECT r = {0, 0, cw, ch};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(g.hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (g.swap && g.swapW == w && g.swapH == h) return;
    g.WaitIdle();
    if (!g.swap) {
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = w; sd.Height = h; sd.Format = g_swapFmt;
        sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
        sd.Scaling = DXGI_SCALING_STRETCH; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        ComPtr<IDXGISwapChain1> s1;
        Check(g.factory->CreateSwapChainForHwnd(g.queue.Get(), g.hwnd, &sd, nullptr, nullptr, &s1),
              "CreateSwapChainForHwnd");
        g.factory->MakeWindowAssociation(g.hwnd, DXGI_MWA_NO_ALT_ENTER);
        Check(s1.As(&g.swap), "IDXGISwapChain3");
    } else {
        Check(g.swap->ResizeBuffers(2, w, h, g_swapFmt, 0), "ResizeBuffers");
    }
    g.swapW = w; g.swapH = h;
}

// ---------------------------------------------------------------- DLSS
struct Dlss {
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    UINT inW = 0, inH = 0, outW = 0, outH = 0;
};

static const char* kProjectId = "6f1c9b52-3d4e-4a7b-9e21-5c8d0f3a7b64";

static void InitNgx(Gpu& g, Dlss& d, bool diagnose) {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    static std::wstring exeDir = fs::path(exe).parent_path().wstring();
    static const wchar_t* paths[1] = {exeDir.c_str()};

    NVSDK_NGX_FeatureCommonInfo info = {};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    info.LoggingInfo.LoggingCallback = NgxLog;
    info.LoggingInfo.MinimumLoggingLevel = g_verbose ? NVSDK_NGX_LOGGING_LEVEL_VERBOSE : NVSDK_NGX_LOGGING_LEVEL_ON;
    info.LoggingInfo.DisableOtherLoggingSinks = false;

    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
                                                             exeDir.c_str(), g.device.Get(), &info,
                                                             NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(r)) {
        char b[160]; snprintf(b, sizeof b, "NVSDK_NGX_D3D12_Init a echoue (0x%08X) - pilote NVIDIA trop ancien ?", (unsigned)r);
        Fail(b);
    }
    Check(NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_GetCapabilityParameters(&d.params)) ? E_FAIL : S_OK,
          "NVSDK_NGX_D3D12_GetCapabilityParameters");

    int avail = 0, needDrv = 0, drvMaj = 0, drvMin = 0, initRes = 0;
    NVSDK_NGX_Parameter_GetI(d.params, NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
    NVSDK_NGX_Parameter_GetI(d.params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needDrv);
    NVSDK_NGX_Parameter_GetI(d.params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &drvMaj);
    NVSDK_NGX_Parameter_GetI(d.params, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &drvMin);
    NVSDK_NGX_Parameter_GetI(d.params, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initRes);
    Out("INFO NGX initialise ; DLSS disponible=%d ; pilote a mettre a jour=%d (min %d.%d) ; init=0x%08X",
        avail, needDrv, drvMaj, drvMin, (unsigned)initRes);

    HMODULE reshade = GetModuleHandleW(L"dxgi.dll");
    wchar_t dxgiPath[MAX_PATH] = L"";
    if (reshade) GetModuleFileNameW(reshade, dxgiPath, MAX_PATH);
    bool isLocal = _wcsnicmp(dxgiPath, exeDir.c_str(), exeDir.size()) == 0;
    Out("INFO dxgi.dll charge depuis %s (%s)", Narrow(dxgiPath).c_str(),
        isLocal ? "ReShade local OK" : "ATTENTION : ce n'est pas le ReShade du dossier engine");
    Out("INFO add-on RenoDX DLSS5 : %s", GetEnvironmentVariableW(L"RENODX_DLSS5_NR_RUNTIME_LOADED", nullptr, 0)
                                             ? "runtime NR charge" : "(etat visible dans ReShade.log)");
    if (!avail && !diagnose) Fail("DLSS indisponible sur ce GPU / pilote (voir Diagnostic)");
}

static NVSDK_NGX_PerfQuality_Value QualityFor(float ratio) {
    if (ratio <= 1.01f) return NVSDK_NGX_PerfQuality_Value_DLAA;
    if (ratio <= 1.55f) return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    if (ratio <= 1.75f) return NVSDK_NGX_PerfQuality_Value_Balanced;
    if (ratio <= 2.05f) return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}

static void EnsureFeature(Gpu& g, Dlss& d, const Options& o, UINT inW, UINT inH, UINT outW, UINT outH) {
    if (d.feature && d.inW == inW && d.inH == inH && d.outW == outW && d.outH == outH) return;
    g.WaitIdle();
    if (d.feature) { NVSDK_NGX_D3D12_ReleaseFeature(d.feature); d.feature = nullptr; }

    const char* hints[] = {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                           NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality};
    for (auto h : hints) NVSDK_NGX_Parameter_SetUI(d.params, h, o.preset);

    NVSDK_NGX_DLSS_Create_Params cp = {};
    cp.Feature.InWidth = inW; cp.Feature.InHeight = inH;
    cp.Feature.InTargetWidth = outW; cp.Feature.InTargetHeight = outH;
    cp.Feature.InPerfQualityValue = QualityFor((float)outW / inW);
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                              (o.linear ? (NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) : 0) |
                              (o.depthInverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0);
    cp.InEnableOutputSubrects = false;

    g.Begin();
    NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSS_EXT(g.list.Get(), 1, 1, &d.feature, d.params, &cp);
    g.Submit();
    if (NVSDK_NGX_FAILED(r)) {
        char b[160]; snprintf(b, sizeof b, "creation de la fonction DLSS impossible (0x%08X) pour %ux%u -> %ux%u",
                              (unsigned)r, inW, inH, outW, outH);
        Fail(b);
    }
    d.inW = inW; d.inH = inH; d.outW = outW; d.outH = outH;
    Out("INFO DLSS cree : %ux%u -> %ux%u (preset %u)", inW, inH, outW, outH, o.preset);
}

// ---------------------------------------------------------------- main
int wmain() {
    Options o = ParseArgs();
    InitColorTables();
    Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx");
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic)),
          "WIC");

    // Liste des entrees
    std::vector<fs::path> inputs;
    bool dirMode = false;
    if (!o.diagnose) {
        fs::path in(o.in);
        if (fs::is_directory(in)) {
            dirMode = true;
            for (auto& e : fs::directory_iterator(in))
                if (e.is_regular_file() && IsImageExt(e.path())) inputs.push_back(e.path());
            std::sort(inputs.begin(), inputs.end());
            fs::create_directories(o.out);
        } else if (fs::exists(in)) {
            inputs.push_back(in);
            fs::path op(o.out);
            if (op.has_parent_path()) fs::create_directories(op.parent_path());
        } else Fail("entree introuvable : " + Narrow(o.in));
        if (inputs.empty()) Fail("aucune image dans le dossier d'entree");
    }

    Gpu g; CreateGpu(g);
    Dlss d; InitNgx(g, d, o.diagnose);
    if (o.diagnose) {
        EnsureSwapChain(g, 256, 256, true);
        g.swap->Present(0, 0);
        NVSDK_NGX_D3D12_Shutdown1(g.device.Get());
        Out("DONE");
        return 0;
    }

    Tex color, depth, mv, output;
    const DXGI_FORMAT cfmt = o.linear ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    g_swapFmt = o.linear ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    std::vector<uint16_t> lin;
    Out("INFO mode couleur : %s", o.linear ? "lumiere lineaire FP16 (comme un jeu)" : "sRGB 8 bits");
    const size_t n = inputs.size();
    std::vector<uint8_t> px;

    for (size_t i = 0; i < n; ++i) {
        // DLSS refuse les sorties de plus de 8192 x 8192 : on reduit l'entree si besoin
        const UINT kMaxOut = 8192;
        UINT maxIn = (UINT)std::floor(kMaxOut / o.scale);
        double maxInPix = o.maxPixels / ((double)o.scale * o.scale);
        UINT w = 0, h = 0, ow = 0, oh = 0;
        if (!LoadRGBA(inputs[i].wstring(), maxIn, maxIn, maxInPix, w, h, ow, oh, px))
            Fail("lecture impossible : " + Narrow(inputs[i].wstring()));
        if (ow != w || oh != h)
            Out("INFO image %ux%u trop grande pour le Neural Rendering : traitee en %ux%u (sortie %ux%u)",
                ow, oh, w, h, (UINT)std::lround(w * o.scale), (UINT)std::lround(h * o.scale));
        UINT outW = std::max(1u, (UINT)std::lround(w * o.scale));
        UINT outH = std::max(1u, (UINT)std::lround(h * o.scale));

        bool sizeChanged = color.w != w || color.h != h || output.w != outW || output.h != outH;
        if (sizeChanged) {
            g.WaitIdle();
            color = g.MakeTex(cfmt, w, h, D3D12_RESOURCE_FLAG_NONE,
                              D3D12_RESOURCE_STATE_COPY_DEST, L"DLSS5Eval color");
            depth = g.MakeTex(DXGI_FORMAT_R32_FLOAT, w, h, D3D12_RESOURCE_FLAG_NONE,
                              D3D12_RESOURCE_STATE_COPY_DEST, L"DLSS5Eval depth");
            mv = g.MakeTex(DXGI_FORMAT_R16G16_FLOAT, w, h, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, L"DLSS5Eval motion");
            output = g.MakeTex(cfmt, outW, outH, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"DLSS5Eval output");
            std::vector<float> dz((size_t)w * h, o.depth);
            g.Upload(depth, dz.data(), w * 4, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            std::vector<uint32_t> mz((size_t)w * h, 0u);
            g.Upload(mv, mz.data(), w * 4, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            EnsureSwapChain(g, outW, outH, o.hidden);
        }
        EnsureFeature(g, d, o, w, h, outW, outH);
        if (o.linear) {
            lin.resize((size_t)w * h * 4);
            for (size_t k = 0, n4 = (size_t)w * h * 4; k < n4; k += 4) {
                lin[k] = FloatToHalf(g_srgbToLin[px[k]]);
                lin[k + 1] = FloatToHalf(g_srgbToLin[px[k + 1]]);
                lin[k + 2] = FloatToHalf(g_srgbToLin[px[k + 2]]);
                lin[k + 3] = 0x3C00;  // 1.0
            }
            g.Upload(color, lin.data(), w * 8, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        } else {
            g.Upload(color, px.data(), w * 4, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        bool fresh = !o.sequence || i == 0 || sizeChanged;
        int passes = fresh ? o.warmup : o.framePasses;
        for (int p = 0; p < passes; ++p) {
            bool reset = (p == 0) && (fresh || o.resetEach);
            g.Begin();
            NVSDK_NGX_D3D12_DLSS_Eval_Params ep = {};
            ep.Feature.pInColor = color.res.Get();
            ep.Feature.pInOutput = output.res.Get();
            ep.Feature.InSharpness = 0.0f;
            ep.pInDepth = depth.res.Get();
            ep.pInMotionVectors = mv.res.Get();
            ep.InJitterOffsetX = 0.0f; ep.InJitterOffsetY = 0.0f;
            ep.InRenderSubrectDimensions.Width = w; ep.InRenderSubrectDimensions.Height = h;
            ep.InReset = reset ? 1 : 0;
            ep.InMVScaleX = 1.0f; ep.InMVScaleY = 1.0f;
            ep.InPreExposure = 1.0f; ep.InExposureScale = 1.0f;
            ep.InFrameTimeDeltaInMsec = 16.6667f;
            NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSS_EXT(g.list.Get(), d.feature, d.params, &ep);
            if (NVSDK_NGX_FAILED(r)) {
                char b[96]; snprintf(b, sizeof b, "evaluation DLSS echouee (0x%08X)", (unsigned)r); Fail(b);
            }
            // Copie vers la fenetre d'apercu puis Present (ReShade / l'add-on avancent leur cycle par image)
            ComPtr<ID3D12Resource> bb;
            g.swap->GetBuffer(g.swap->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb));
            Tex back; back.res = bb; back.state = D3D12_RESOURCE_STATE_PRESENT;
            g.Transition(output, D3D12_RESOURCE_STATE_COPY_SOURCE);
            g.Transition(back, D3D12_RESOURCE_STATE_COPY_DEST);
            g.list->CopyResource(back.res.Get(), output.res.Get());
            g.Transition(back, D3D12_RESOURCE_STATE_PRESENT);
            g.Transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g.Submit();
            g.swap->Present(0, 0);
            g.WaitIdle();
            Pump();
        }

        // Relecture apres le Present : capte aussi un traitement NR differe au Present
        std::vector<uint8_t> bgr = g.ReadbackBGR(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        {
            // Mesure de controle : difference moyenne (0-255) entre le rendu et l'original
            double sum = 0; size_t cnt = 0;
            for (UINT y = 0; y < outH; y += 3) {
                UINT sy = std::min(h - 1, (UINT)((y + 0.5) * h / outH));
                for (UINT x = 0; x < outW; x += 3) {
                    UINT sx = std::min(w - 1, (UINT)((x + 0.5) * w / outW));
                    const uint8_t* d = &bgr[((size_t)y * outW + x) * 3];
                    const uint8_t* q = &px[((size_t)sy * w + sx) * 4];
                    sum += std::abs(d[2] - q[0]) + std::abs(d[1] - q[1]) + std::abs(d[0] - q[2]);
                    cnt += 3;
                }
            }
            Out("DIFF %.2f", cnt ? sum / cnt : 0.0);
        }
        if (o.mix < 0.999f) {
            // melange avec l'original (reechantillonne en bilineaire a la taille de sortie)
            const float a = o.mix, b = 1.0f - o.mix;
            std::vector<uint8_t> refPx;
            const std::vector<uint8_t>* ref = &px;
            UINT rw = w, rh = h;
            if (!o.orig.empty()) {
                fs::path op = dirMode ? fs::path(o.orig) / inputs[i].filename() : fs::path(o.orig);
                UINT a1, a2;
                if (LoadRGBA(op.wstring(), 1u << 20, 1u << 20, 1e18, rw, rh, a1, a2, refPx)) ref = &refPx;
                else { rw = w; rh = h; Out("INFO original introuvable pour le dosage : %s", Narrow(op.wstring()).c_str()); }
            }
            const UINT w = rw, h = rh;
            const std::vector<uint8_t>& px = *ref;
            for (UINT y = 0; y < outH; ++y) {
                float fy = std::clamp((y + 0.5f) * h / outH - 0.5f, 0.0f, (float)h - 1);
                UINT y0 = (UINT)fy, y1 = std::min(y0 + 1, h - 1); float ty = fy - y0;
                for (UINT x = 0; x < outW; ++x) {
                    float fx = std::clamp((x + 0.5f) * w / outW - 0.5f, 0.0f, (float)w - 1);
                    UINT x0 = (UINT)fx, x1 = std::min(x0 + 1, w - 1); float tx = fx - x0;
                    uint8_t* d = &bgr[((size_t)y * outW + x) * 3];
                    for (int c = 0; c < 3; ++c) {
                        auto P = [&](UINT xx, UINT yy) { return (float)px[((size_t)yy * w + xx) * 4 + c]; };
                        float v = (P(x0, y0) * (1 - tx) + P(x1, y0) * tx) * (1 - ty) +
                                  (P(x0, y1) * (1 - tx) + P(x1, y1) * tx) * ty;
                        int bi = 2 - c;  // px est RGBA, bgr est BGR
                        d[bi] = (uint8_t)std::clamp(d[bi] * a + v * b + 0.5f, 0.0f, 255.0f);
                    }
                }
            }
        }
        fs::path dst = dirMode ? fs::path(o.out) / (inputs[i].stem().wstring() + L".png") : fs::path(o.out);
        if (!SavePNG(dst.wstring(), outW, outH, bgr)) Fail("ecriture impossible : " + Narrow(dst.wstring()));
        Out("PROGRESS %zu %zu", i + 1, n);
    }

    g.WaitIdle();
    if (d.feature) NVSDK_NGX_D3D12_ReleaseFeature(d.feature);
    NVSDK_NGX_D3D12_Shutdown1(g.device.Get());
    Out("DONE");
    return 0;
}
