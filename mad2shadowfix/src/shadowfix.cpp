// Madagascar 2 shadow-pass diagnostics/experiment mod.
//
// Loaded by mad2modloader from mad2/mods/*.dll. Rather than replacing
// d3d9.dll (which conflicts with DXVK/wine3d/dxwrapper/dgVoodoo all
// wanting to own that filename), this hooks Direct3DCreate9/Ex at the
// IAT level, in-process, after the real backend's d3d9.dll is already
// loaded and resolved by the normal Windows/Wine loader. That makes it
// backend-agnostic: it works the same whether the active d3d9.dll is
// DXVK's, wine3d's builtin, or something chained through dxwrapper.
//
// Background: on all native Linux backends tried so far (DXVK, wine3d),
// the game binds the 1024x1024 D3DFMT_R32F shadow render target and its
// depth-stencil surface, then issues ZERO draw calls before restoring
// the backbuffer -- the shadow-casting geometry is being culled/skipped
// entirely on the CPU side, before any GPU work happens. This mod's
// purpose is to (a) capture much more targeted diagnostics around that
// exact moment than earlier blanket-verbose logging did, and (b) let a
// few untested hypotheses (shader-model caps downgrade, constant-count
// spoof, query-result forcing) be flipped on via environment variables
// without recompiling.
#define INITGUID
#include <windows.h>
#include <d3d9.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include "../../mad2sharedlog/mad2sharedlog.h"
#include "../../mad2hookutil/include/mad2hookutil_api.h"

// ---------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------

// Default is silent: the fix itself (disabling alpha test during the
// shadow pass, see disableShadowAlphaTest below) needs no logging to
// work, and per-call/per-frame logging measurably hurt frame time during
// testing. Set SHADOWFIX_LOG_LEVEL=1 or 2 to bring back diagnostics.
static DWORD g_LogLevel = 0;
static bool g_TraceShadowPass = false;

static void LogV(const char* tag, const char* fmt, va_list args) {
    char combinedTag[96];
    snprintf(combinedTag, sizeof(combinedTag), "[mad2shadowfix][T%04lX]%s", (unsigned long)GetCurrentThreadId(), tag);
    Mad2Log_Write(combinedTag, fmt, args);
}

// Always emitted once at startup regardless of log level, so the log
// file (if anyone looks) at least confirms the mod loaded and shows
// what config it's running with.
static void LogAlways(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogV("", fmt, args);
    va_end(args);
}

static void Log(DWORD level, const char* fmt, ...) {
    if (level > g_LogLevel) return;
    va_list args;
    va_start(args, fmt);
    LogV("", fmt, args);
    va_end(args);
}

// Always emitted (regardless of SHADOWFIX_LOG_LEVEL) when shadow-pass
// tracing is enabled; used to get a surgical, low-noise trace of every
// call that happens while the shadow render target is bound, instead of
// wading through a global verbose log.
static void TraceSP(const char* fmt, ...) {
    if (!g_TraceShadowPass) return;
    va_list args;
    va_start(args, fmt);
    LogV("[SHADOWPASS]", fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------
// Config (environment variables, read once at startup)
// ---------------------------------------------------------------------

struct Config {
    DWORD logLevel = 0;
    bool traceShadowPass = false;

    // Shader-model-downgrade experiment: does the engine pick a
    // different (working) shadow codepath if it thinks the GPU only
    // supports a lower shader model, the way a weak AMD APU might
    // report on Windows?
    DWORD vsVerMajor = 0, vsVerMinor = 0; // 0 = no override
    DWORD psVerMajor = 0, psVerMinor = 0;

    // research.md flagged this as untested: skinned shadow shader uses
    // constants up to c251, close to the reported 256 limit.
    DWORD maxVsConst = 0; // 0 = no override

    // If the engine gates shadow-caster visibility on an occlusion/event
    // query result, forcing GetData to always report "visible" might
    // unstick it.
    bool forceQueryOk = false;

    // R32F is a 32-bit float format; many Vulkan drivers don't support
    // linear filtering on it (VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT
    // often isn't set for VK_FORMAT_R32_SFLOAT). If the game requests
    // linear filtering for the shadow map sampler and DXVK silently
    // falls back to point/undefined behavior, the PCF shadow lookup
    // could come out wrong. This forces point filtering on that specific
    // sampler stage to test the theory.
    bool forcePointShadowFilter = false;

    // Reads back the actual shadow-map texture contents (via
    // GetRenderTargetData) after a shadow pass that drew something, to
    // check whether it holds real depth data or garbage/uniform values.
    bool dumpShadowMap = false;
    DWORD maxShadowMapDumps = 3;

    // THE FIX: the shadow-pass pixel shader alpha-tests every fragment
    // (meant for hair/foliage cutouts). Under DXVK/RADV that test fails
    // for effectively every pixel, so draws "succeed" at the API level
    // but the shadow map ends up blank -- confirmed via GetRenderTargetData
    // readback (100% zero pixels with it on, real depth variance with it
    // off) and visually in-game. Cutout shadow casters (hair, foliage)
    // will render as solid silhouettes instead of correctly cut out,
    // which is a minor visual regression compared to no shadows at all.
    // Default on; set SHADOWFIX_DISABLE_SHADOW_ALPHATEST=0 to turn off.
    bool disableShadowAlphaTest = true;

    // Silently remaps D3DFMT_D32 -> D3DFMT_D24X8 and D3DFMT_D24X4S4 ->
    // D3DFMT_D24S8 on every texture/render-target/depth-stencil surface
    // the game creates (see RemapUnsupportedDepthFormat below), added to
    // work around DXVK not supporting those two depth formats. Unlike
    // every other override in this struct this ran unconditionally, with
    // no way to A/B it against the alpha-test fix -- suspected of causing
    // intermittent player/companion shadow loss (skinned-character shadow
    // casters may request one of the remapped formats specifically).
    // Default on (preserves prior behavior); set
    // SHADOWFIX_REMAP_DEPTH_FORMAT=0 to disable and test without it.
    bool remapDepthFormat = true;

    DWORD spoofVendor = 0, spoofDevice = 0; // 0 = no override
    bool forceHwVp = false;
    bool blockDepthAsRT = false;
    float depthBiasScale = 1.0f;
    float slopeBiasScale = 1.0f;
    bool haveDepthBiasScale = false;
    bool haveSlopeBiasScale = false;
} g_Config;

static bool EnvBool(const char* name) {
    char val[32];
    return GetEnvironmentVariableA(name, val, sizeof(val)) && atoi(val) != 0;
}

static bool EnvBoolDefault(const char* name, bool defaultVal) {
    char val[32];
    if (!GetEnvironmentVariableA(name, val, sizeof(val))) return defaultVal;
    return atoi(val) != 0;
}

static bool EnvU32(const char* name, DWORD* out) {
    char val[32];
    if (!GetEnvironmentVariableA(name, val, sizeof(val))) return false;
    *out = strtoul(val, nullptr, 0);
    return true;
}

static bool EnvFloat(const char* name, float* out) {
    char val[32];
    if (!GetEnvironmentVariableA(name, val, sizeof(val))) return false;
    *out = (float)atof(val);
    return true;
}

static bool EnvVersion(const char* name, DWORD* major, DWORD* minor) {
    char val[32];
    if (!GetEnvironmentVariableA(name, val, sizeof(val))) return false;
    unsigned int ma = 0, mi = 0;
    if (sscanf(val, "%u.%u", &ma, &mi) < 1) return false;
    *major = ma;
    *minor = mi;
    return true;
}

static void LoadConfig() {
    EnvU32("SHADOWFIX_LOG_LEVEL", &g_Config.logLevel);
    g_LogLevel = g_Config.logLevel;
    g_Config.traceShadowPass = EnvBool("SHADOWFIX_TRACE_SHADOW_PASS");
    g_TraceShadowPass = g_Config.traceShadowPass;
    EnvVersion("SHADOWFIX_VS_VERSION", &g_Config.vsVerMajor, &g_Config.vsVerMinor);
    EnvVersion("SHADOWFIX_PS_VERSION", &g_Config.psVerMajor, &g_Config.psVerMinor);
    EnvU32("SHADOWFIX_MAX_VS_CONST", &g_Config.maxVsConst);
    g_Config.forceQueryOk = EnvBool("SHADOWFIX_FORCE_QUERY_OK");
    g_Config.forcePointShadowFilter = EnvBool("SHADOWFIX_FORCE_POINT_SHADOW_FILTER");
    g_Config.dumpShadowMap = EnvBool("SHADOWFIX_DUMP_SHADOWMAP");
    EnvU32("SHADOWFIX_MAX_SHADOWMAP_DUMPS", &g_Config.maxShadowMapDumps);
    g_Config.disableShadowAlphaTest = EnvBoolDefault("SHADOWFIX_DISABLE_SHADOW_ALPHATEST", true);
    g_Config.remapDepthFormat = EnvBoolDefault("SHADOWFIX_REMAP_DEPTH_FORMAT", true);
    EnvU32("SHADOWFIX_SPOOF_VENDOR", &g_Config.spoofVendor);
    EnvU32("SHADOWFIX_SPOOF_DEVICE", &g_Config.spoofDevice);
    g_Config.forceHwVp = EnvBool("SHADOWFIX_FORCE_HW_VP");
    g_Config.blockDepthAsRT = EnvBool("SHADOWFIX_BLOCK_DEPTH_RT");
    g_Config.haveDepthBiasScale = EnvFloat("SHADOWFIX_DEPTHBIAS_SCALE", &g_Config.depthBiasScale);
    g_Config.haveSlopeBiasScale = EnvFloat("SHADOWFIX_SLOPEBIAS_SCALE", &g_Config.slopeBiasScale);

    LogAlways("shadowfix loaded. Config: logLevel=%u traceShadowPass=%d vsVer=%u.%u psVer=%u.%u maxVsConst=%u "
           "forceQueryOk=%d remapDepthFormat=%d spoofVendor=0x%X spoofDevice=0x%X forceHwVp=%d blockDepthAsRT=%d "
           "depthBiasScale=%f slopeBiasScale=%f forcePointShadowFilter=%d\n",
        g_Config.logLevel, g_Config.traceShadowPass, g_Config.vsVerMajor, g_Config.vsVerMinor, g_Config.psVerMajor,
        g_Config.psVerMinor, g_Config.maxVsConst, g_Config.forceQueryOk, g_Config.remapDepthFormat,
        g_Config.spoofVendor, g_Config.spoofDevice, g_Config.forceHwVp, g_Config.blockDepthAsRT,
        g_Config.depthBiasScale, g_Config.slopeBiasScale, g_Config.forcePointShadowFilter);
}

// IAT/vtable hooking now provided by aa_mad2hookutil.dll -- see
// ../../mad2hookutil/include/mad2hookutil_api.h. Resolved synchronously
// (not lazily) since aa_mad2hookutil.dll is guaranteed already loaded by
// the time this mod's own DllMain runs (see that header for why).

// ---------------------------------------------------------------------
// Shader identity tracking (for cross-referencing shaders_dump/*.bin)
// ---------------------------------------------------------------------

static std::unordered_map<void*, int> g_VsIds;
static std::unordered_map<void*, int> g_PsIds;

// Textures matching the shadow map's known dimensions/format (1024x1024
// D3DFMT_R32F). If the shadow pass itself is drawing real geometry (as
// confirmed by draw-call counts) but no shadow is visible on screen, the
// bug likely lives in how this texture gets sampled back during normal
// scene rendering rather than in the depth-write pass. Recording the
// pointer at creation time lets SetTexture cross-reference it below.
static std::unordered_set<void*> g_ShadowMapTextures;
static int g_ShadowMapDumpCount = 0;

static void DumpShader(const char* prefix, const DWORD* pFunction, int id) {
    if (!pFunction) return;
    int size = 0;
    while (pFunction[size] != 0x0000FFFF) {
        if (++size > 65536) return;
    }
    size++;
    char filepath[MAX_PATH];
    snprintf(filepath, sizeof(filepath), "shaders_dump/%s_%d.bin", prefix, id);
    CreateDirectoryA("shaders_dump", nullptr);
    FILE* f = fopen(filepath, "wb");
    if (f) {
        fwrite(pFunction, sizeof(DWORD), size, f);
        fclose(f);
    }
}

// ---------------------------------------------------------------------
// IDirect3DQuery9 proxy
// ---------------------------------------------------------------------

class ProxyQuery9 : public IDirect3DQuery9 {
public:
    ProxyQuery9(IDirect3DQuery9* pReal, D3DQUERYTYPE type) : m_pReal(pReal), m_RefCount(1), m_Type(type) {}
    virtual ~ProxyQuery9() {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override {
        if (riid == IID_IUnknown || riid == IID_IDirect3DQuery9) {
            *ppvObj = this;
            AddRef();
            return S_OK;
        }
        return m_pReal->QueryInterface(riid, ppvObj);
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_RefCount); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG count = InterlockedDecrement(&m_RefCount);
        if (count == 0) {
            m_pReal->Release();
            delete this;
            return 0;
        }
        return count;
    }

    STDMETHODIMP GetDevice(IDirect3DDevice9** ppDevice) override { return m_pReal->GetDevice(ppDevice); }
    STDMETHODIMP_(D3DQUERYTYPE) GetType() override { return m_pReal->GetType(); }
    STDMETHODIMP_(DWORD) GetDataSize() override { return m_pReal->GetDataSize(); }

    STDMETHODIMP Issue(DWORD dwIssueFlags) override {
        TraceSP("IDirect3DQuery9::Issue type=%d flags=0x%X\n", m_Type, dwIssueFlags);
        Log(2, "IDirect3DQuery9::Issue type=%d flags=0x%X\n", m_Type, dwIssueFlags);
        return m_pReal->Issue(dwIssueFlags);
    }

    STDMETHODIMP GetData(void* pData, DWORD dwSize, DWORD dwGetDataFlags) override {
        HRESULT hr = m_pReal->GetData(pData, dwSize, dwGetDataFlags);
        DWORD reported = (pData && dwSize >= sizeof(DWORD)) ? *reinterpret_cast<DWORD*>(pData) : 0;
        TraceSP("IDirect3DQuery9::GetData type=%d flags=0x%X -> hr=0x%08X data=%u\n", m_Type, dwGetDataFlags, hr,
                reported);
        Log(2, "IDirect3DQuery9::GetData type=%d flags=0x%X -> hr=0x%08X data=%u\n", m_Type, dwGetDataFlags, hr,
            reported);

        if (g_Config.forceQueryOk && m_Type == D3DQUERYTYPE_OCCLUSION && pData && dwSize >= sizeof(DWORD)) {
            if (hr != S_OK || reported == 0) {
                Log(1, "  [Override] Forcing occlusion query result to non-zero (was hr=0x%08X data=%u)\n", hr,
                    reported);
                *reinterpret_cast<DWORD*>(pData) = 0xFFFF;
                hr = S_OK;
            }
        }
        return hr;
    }

private:
    IDirect3DQuery9* m_pReal;
    ULONG m_RefCount;
    D3DQUERYTYPE m_Type;
};

// ---------------------------------------------------------------------
// IDirect3DDevice9 proxy
// ---------------------------------------------------------------------

static const char* RStateName(D3DRENDERSTATETYPE s) {
    switch (s) {
        case D3DRS_ZENABLE: return "ZENABLE";
        case D3DRS_CULLMODE: return "CULLMODE";
        case D3DRS_ZFUNC: return "ZFUNC";
        case D3DRS_ALPHATESTENABLE: return "ALPHATESTENABLE";
        case D3DRS_STENCILENABLE: return "STENCILENABLE";
        case D3DRS_SCISSORTESTENABLE: return "SCISSORTESTENABLE";
        case D3DRS_COLORWRITEENABLE: return "COLORWRITEENABLE";
        case D3DRS_DEPTHBIAS: return "DEPTHBIAS";
        case D3DRS_SLOPESCALEDEPTHBIAS: return "SLOPESCALEDEPTHBIAS";
        default: return nullptr;
    }
}

class ProxyDirect3DDevice9 : public IDirect3DDevice9 {
public:
    ProxyDirect3DDevice9(IDirect3DDevice9* pReal) : m_pReal(pReal), m_RefCount(1) {}
    virtual ~ProxyDirect3DDevice9() {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override {
        if (riid == IID_IUnknown || riid == IID_IDirect3DDevice9) {
            *ppvObj = this;
            AddRef();
            return S_OK;
        }
        return m_pReal->QueryInterface(riid, ppvObj);
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_RefCount); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG count = InterlockedDecrement(&m_RefCount);
        if (count == 0) {
            m_pReal->Release();
            delete this;
            return 0;
        }
        return count;
    }

    STDMETHODIMP TestCooperativeLevel() override { return m_pReal->TestCooperativeLevel(); }
    STDMETHODIMP_(UINT) GetAvailableTextureMem() override { return m_pReal->GetAvailableTextureMem(); }
    STDMETHODIMP EvictManagedResources() override { return m_pReal->EvictManagedResources(); }
    STDMETHODIMP GetDirect3D(IDirect3D9** ppD3D9) override { return m_pReal->GetDirect3D(ppD3D9); }

    STDMETHODIMP GetDeviceCaps(D3DCAPS9* pCaps) override {
        HRESULT hr = m_pReal->GetDeviceCaps(pCaps);
        if (SUCCEEDED(hr) && pCaps) {
            ApplyCapsOverrides(pCaps);
        }
        return hr;
    }

    STDMETHODIMP GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE* pMode) override {
        return m_pReal->GetDisplayMode(iSwapChain, pMode);
    }
    STDMETHODIMP GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* pParameters) override {
        return m_pReal->GetCreationParameters(pParameters);
    }
    STDMETHODIMP SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9* pCursorBitmap) override {
        return m_pReal->SetCursorProperties(XHotSpot, YHotSpot, pCursorBitmap);
    }
    STDMETHODIMP_(void) SetCursorPosition(int X, int Y, DWORD Flags) override {
        return m_pReal->SetCursorPosition(X, Y, Flags);
    }
    STDMETHODIMP_(BOOL) ShowCursor(BOOL bShow) override { return m_pReal->ShowCursor(bShow); }
    STDMETHODIMP CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters,
                                           IDirect3DSwapChain9** pSwapChain) override {
        return m_pReal->CreateAdditionalSwapChain(pPresentationParameters, pSwapChain);
    }
    STDMETHODIMP GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** pSwapChain) override {
        return m_pReal->GetSwapChain(iSwapChain, pSwapChain);
    }
    STDMETHODIMP_(UINT) GetNumberOfSwapChains() override { return m_pReal->GetNumberOfSwapChains(); }
    STDMETHODIMP Reset(D3DPRESENT_PARAMETERS* pPresentationParameters) override {
        Log(1, "IDirect3DDevice9::Reset\n");
        return m_pReal->Reset(pPresentationParameters);
    }
    STDMETHODIMP Present(CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride,
                         CONST RGNDATA* pDirtyRegion) override {
        return m_pReal->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
    }
    STDMETHODIMP GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type,
                               IDirect3DSurface9** ppBackBuffer) override {
        return m_pReal->GetBackBuffer(iSwapChain, iBackBuffer, Type, ppBackBuffer);
    }
    STDMETHODIMP GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus) override {
        return m_pReal->GetRasterStatus(iSwapChain, pRasterStatus);
    }
    STDMETHODIMP SetDialogBoxMode(BOOL bEnableDialogs) override { return m_pReal->SetDialogBoxMode(bEnableDialogs); }
    STDMETHODIMP_(void) SetGammaRamp(UINT swapchain_idx, DWORD flags, CONST D3DGAMMARAMP* ramp) override {
        return m_pReal->SetGammaRamp(swapchain_idx, flags, ramp);
    }
    STDMETHODIMP_(void) GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP* pRamp) override {
        return m_pReal->GetGammaRamp(iSwapChain, pRamp);
    }

    STDMETHODIMP CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                               IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle) override {
        bool isShadowMapCandidate = (Width == 1024 && Height == 1024 && Format == D3DFMT_R32F);
        Format = RemapUnsupportedDepthFormat(Format, "CreateTexture");
        HRESULT hr = m_pReal->CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
        if (isShadowMapCandidate && SUCCEEDED(hr) && ppTexture && *ppTexture) {
            Log(1, "CreateTexture: tagged %p as shadow-map texture (1024x1024 R32F, Usage=0x%X Pool=%d)\n",
                (void*)*ppTexture, Usage, Pool);
            g_ShadowMapTextures.insert(*ppTexture);
        }
        return hr;
    }
    STDMETHODIMP CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format,
                                     D3DPOOL Pool, IDirect3DVolumeTexture9** ppVolumeTexture,
                                     HANDLE* pSharedHandle) override {
        Format = RemapUnsupportedDepthFormat(Format, "CreateVolumeTexture");
        return m_pReal->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture,
                                             pSharedHandle);
    }
    STDMETHODIMP CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                   IDirect3DCubeTexture9** ppCubeTexture, HANDLE* pSharedHandle) override {
        Format = RemapUnsupportedDepthFormat(Format, "CreateCubeTexture");
        return m_pReal->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle);
    }
    STDMETHODIMP CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
                                    IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle) override {
        return m_pReal->CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle);
    }
    STDMETHODIMP CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                   IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE* pSharedHandle) override {
        return m_pReal->CreateIndexBuffer(Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle);
    }
    STDMETHODIMP CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
                                    DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface,
                                    HANDLE* pSharedHandle) override {
        Log(2, "IDirect3DDevice9::CreateRenderTarget %ux%u Format=%d\n", Width, Height, Format);
        Format = RemapUnsupportedDepthFormat(Format, "CreateRenderTarget");
        return m_pReal->CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality, Lockable,
                                           ppSurface, pSharedHandle);
    }
    STDMETHODIMP CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format,
                                           D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard,
                                           IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) override {
        Log(2, "IDirect3DDevice9::CreateDepthStencilSurface %ux%u Format=%d MSType=%d\n", Width, Height, Format,
            MultiSample);
        Format = RemapUnsupportedDepthFormat(Format, "CreateDepthStencilSurface");
        return m_pReal->CreateDepthStencilSurface(Width, Height, Format, MultiSample, MultisampleQuality, Discard,
                                                   ppSurface, pSharedHandle);
    }
    STDMETHODIMP UpdateSurface(IDirect3DSurface9* src_surface, CONST RECT* src_rect, IDirect3DSurface9* dst_surface,
                               CONST POINT* dst_point) override {
        return m_pReal->UpdateSurface(src_surface, src_rect, dst_surface, dst_point);
    }
    STDMETHODIMP UpdateTexture(IDirect3DBaseTexture9* pSourceTexture,
                              IDirect3DBaseTexture9* pDestinationTexture) override {
        return m_pReal->UpdateTexture(pSourceTexture, pDestinationTexture);
    }
    STDMETHODIMP GetRenderTargetData(IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface) override {
        return m_pReal->GetRenderTargetData(pRenderTarget, pDestSurface);
    }
    STDMETHODIMP GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9* pDestSurface) override {
        return m_pReal->GetFrontBufferData(iSwapChain, pDestSurface);
    }
    STDMETHODIMP StretchRect(IDirect3DSurface9* src_surface, CONST RECT* src_rect, IDirect3DSurface9* dst_surface,
                             CONST RECT* dst_rect, D3DTEXTUREFILTERTYPE filter) override {
        return m_pReal->StretchRect(src_surface, src_rect, dst_surface, dst_rect, filter);
    }
    STDMETHODIMP ColorFill(IDirect3DSurface9* surface, CONST RECT* rect, D3DCOLOR color) override {
        return m_pReal->ColorFill(surface, rect, color);
    }
    STDMETHODIMP CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool,
                                             IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) override {
        return m_pReal->CreateOffscreenPlainSurface(Width, Height, Format, Pool, ppSurface, pSharedHandle);
    }

    STDMETHODIMP SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget) override {
        if (RenderTargetIndex == 0) {
            bool isShadowRT = false;
            D3DSURFACE_DESC desc{};
            if (pRenderTarget && SUCCEEDED(pRenderTarget->GetDesc(&desc))) {
                isShadowRT = (desc.Width == 1024 && desc.Height == 1024 && desc.Format == D3DFMT_R32F);
            }
            if (isShadowRT) m_currentShadowRTSurface = pRenderTarget;
            if (isShadowRT && !m_isShadowPass) {
                EnterShadowPass();
            } else if (!isShadowRT && m_isShadowPass) {
                ExitShadowPass();
            }
        }
        return m_pReal->SetRenderTarget(RenderTargetIndex, pRenderTarget);
    }
    STDMETHODIMP GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9** ppRenderTarget) override {
        return m_pReal->GetRenderTarget(RenderTargetIndex, ppRenderTarget);
    }
    STDMETHODIMP SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil) override {
        if (m_isShadowPass) {
            D3DSURFACE_DESC desc{};
            if (pNewZStencil && SUCCEEDED(pNewZStencil->GetDesc(&desc))) {
                TraceSP("SetDepthStencilSurface %ux%u Format=%d\n", desc.Width, desc.Height, desc.Format);
            } else {
                TraceSP("SetDepthStencilSurface NULL\n");
            }
        }
        return m_pReal->SetDepthStencilSurface(pNewZStencil);
    }
    STDMETHODIMP GetDepthStencilSurface(IDirect3DSurface9** ppZStencilSurface) override {
        return m_pReal->GetDepthStencilSurface(ppZStencilSurface);
    }

    STDMETHODIMP BeginScene() override {
        if (m_isShadowPass) TraceSP("BeginScene\n");
        return m_pReal->BeginScene();
    }
    STDMETHODIMP EndScene() override {
        if (m_isShadowPass) TraceSP("EndScene\n");
        return m_pReal->EndScene();
    }
    STDMETHODIMP Clear(DWORD rect_count, CONST D3DRECT* rects, DWORD flags, D3DCOLOR color, float z,
                       DWORD stencil) override {
        if (m_isShadowPass) {
            m_shadowClears++;
            TraceSP("Clear rectCount=%u flags=0x%X color=0x%08X z=%f stencil=%u\n", rect_count, flags, color, z,
                    stencil);
        }
        return m_pReal->Clear(rect_count, rects, flags, color, z, stencil);
    }

    STDMETHODIMP SetTransform(D3DTRANSFORMSTATETYPE state, CONST D3DMATRIX* matrix) override {
        if (m_isShadowPass) {
            m_shadowTransforms++;
            TraceSP("SetTransform state=%d\n", state);
        }
        return m_pReal->SetTransform(state, matrix);
    }
    STDMETHODIMP GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) override {
        return m_pReal->GetTransform(State, pMatrix);
    }
    STDMETHODIMP MultiplyTransform(D3DTRANSFORMSTATETYPE state, CONST D3DMATRIX* matrix) override {
        return m_pReal->MultiplyTransform(state, matrix);
    }
    STDMETHODIMP SetViewport(CONST D3DVIEWPORT9* viewport) override {
        if (m_isShadowPass && viewport) {
            TraceSP("SetViewport X=%u Y=%u W=%u H=%u MinZ=%f MaxZ=%f\n", viewport->X, viewport->Y, viewport->Width,
                    viewport->Height, viewport->MinZ, viewport->MaxZ);
        }
        return m_pReal->SetViewport(viewport);
    }
    STDMETHODIMP GetViewport(D3DVIEWPORT9* pViewport) override { return m_pReal->GetViewport(pViewport); }
    STDMETHODIMP SetMaterial(CONST D3DMATERIAL9* material) override { return m_pReal->SetMaterial(material); }
    STDMETHODIMP GetMaterial(D3DMATERIAL9* pMaterial) override { return m_pReal->GetMaterial(pMaterial); }
    STDMETHODIMP SetLight(DWORD index, CONST D3DLIGHT9* light) override { return m_pReal->SetLight(index, light); }
    STDMETHODIMP GetLight(DWORD Index, D3DLIGHT9* pLight) override { return m_pReal->GetLight(Index, pLight); }
    STDMETHODIMP LightEnable(DWORD Index, BOOL Enable) override { return m_pReal->LightEnable(Index, Enable); }
    STDMETHODIMP GetLightEnable(DWORD Index, BOOL* pEnable) override { return m_pReal->GetLightEnable(Index, pEnable); }
    STDMETHODIMP SetClipPlane(DWORD index, CONST float* plane) override { return m_pReal->SetClipPlane(index, plane); }
    STDMETHODIMP GetClipPlane(DWORD Index, float* pPlane) override { return m_pReal->GetClipPlane(Index, pPlane); }

    STDMETHODIMP SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) override {
        if (State == D3DRS_DEPTHBIAS || State == D3DRS_SLOPESCALEDEPTHBIAS) {
            float fVal = *reinterpret_cast<float*>(&Value);
            if (State == D3DRS_DEPTHBIAS && g_Config.haveDepthBiasScale) {
                fVal *= g_Config.depthBiasScale;
                Value = *reinterpret_cast<DWORD*>(&fVal);
                Log(1, "  [Override] DEPTHBIAS scaled -> %f\n", fVal);
            } else if (State == D3DRS_SLOPESCALEDEPTHBIAS && g_Config.haveSlopeBiasScale) {
                fVal *= g_Config.slopeBiasScale;
                Value = *reinterpret_cast<DWORD*>(&fVal);
                Log(1, "  [Override] SLOPESCALEDEPTHBIAS scaled -> %f\n", fVal);
            }
        }
        if (m_isShadowPass) {
            m_shadowRenderStates++;
            const char* name = RStateName(State);
            if (name) {
                float asFloat = *reinterpret_cast<float*>(&Value);
                TraceSP("SetRenderState %s(%d) Value=%u (as float=%f)\n", name, State, Value, asFloat);
            } else {
                TraceSP("SetRenderState state=%d Value=%u\n", State, Value);
            }
            if (g_Config.disableShadowAlphaTest && State == D3DRS_ALPHATESTENABLE && Value != 0) {
                Log(2, "  [Override] Forcing ALPHATESTENABLE off during shadow pass\n");
                Value = 0;
            }
        }
        return m_pReal->SetRenderState(State, Value);
    }
    STDMETHODIMP GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue) override {
        return m_pReal->GetRenderState(State, pValue);
    }
    STDMETHODIMP CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB) override {
        return m_pReal->CreateStateBlock(Type, ppSB);
    }
    STDMETHODIMP BeginStateBlock() override { return m_pReal->BeginStateBlock(); }
    STDMETHODIMP EndStateBlock(IDirect3DStateBlock9** ppSB) override { return m_pReal->EndStateBlock(ppSB); }
    STDMETHODIMP SetClipStatus(CONST D3DCLIPSTATUS9* clip_status) override {
        return m_pReal->SetClipStatus(clip_status);
    }
    STDMETHODIMP GetClipStatus(D3DCLIPSTATUS9* pClipStatus) override { return m_pReal->GetClipStatus(pClipStatus); }
    STDMETHODIMP GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture) override {
        return m_pReal->GetTexture(Stage, ppTexture);
    }
    STDMETHODIMP SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture) override {
        if (m_isShadowPass) {
            m_shadowTextureSets++;
            TraceSP("SetTexture Stage=%u Texture=%p\n", Stage, (void*)pTexture);
        }
        if (pTexture && g_ShadowMapTextures.count(pTexture)) {
            if ((int)Stage != m_shadowMapStage) {
                Log(1,
                    "Shadow-map texture %p sampled: Stage=%u inShadowPass=%d currentVS=vs_%d.bin currentPS=ps_%d.bin\n",
                    (void*)pTexture, Stage, m_isShadowPass, m_currentVsId, m_currentPsId);
            }
            m_shadowMapStage = (int)Stage;
        }
        return m_pReal->SetTexture(Stage, pTexture);
    }
    STDMETHODIMP GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue) override {
        return m_pReal->GetTextureStageState(Stage, Type, pValue);
    }
    STDMETHODIMP SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) override {
        return m_pReal->SetTextureStageState(Stage, Type, Value);
    }
    STDMETHODIMP GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue) override {
        return m_pReal->GetSamplerState(Sampler, Type, pValue);
    }
    STDMETHODIMP SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) override {
        if ((int)Sampler == m_shadowMapStage &&
            (Type == D3DSAMP_MINFILTER || Type == D3DSAMP_MAGFILTER || Type == D3DSAMP_MIPFILTER ||
             Type == D3DSAMP_ADDRESSU || Type == D3DSAMP_ADDRESSV)) {
            Log(2, "Shadow-map sampler (stage %d) state Type=%d Value=%u\n", m_shadowMapStage, Type, Value);
            if (g_Config.forcePointShadowFilter &&
                (Type == D3DSAMP_MINFILTER || Type == D3DSAMP_MAGFILTER || Type == D3DSAMP_MIPFILTER) &&
                Value != D3DTEXF_NONE) {
                Log(1, "  [Override] Forcing shadow sampler filter Type=%d %u -> D3DTEXF_POINT\n", Type, Value);
                Value = D3DTEXF_POINT;
            }
        }
        return m_pReal->SetSamplerState(Sampler, Type, Value);
    }
    STDMETHODIMP ValidateDevice(DWORD* pNumPasses) override { return m_pReal->ValidateDevice(pNumPasses); }
    STDMETHODIMP SetPaletteEntries(UINT palette_idx, CONST PALETTEENTRY* entries) override {
        return m_pReal->SetPaletteEntries(palette_idx, entries);
    }
    STDMETHODIMP GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY* pEntries) override {
        return m_pReal->GetPaletteEntries(PaletteNumber, pEntries);
    }
    STDMETHODIMP SetCurrentTexturePalette(UINT PaletteNumber) override {
        return m_pReal->SetCurrentTexturePalette(PaletteNumber);
    }
    STDMETHODIMP GetCurrentTexturePalette(UINT* PaletteNumber) override {
        return m_pReal->GetCurrentTexturePalette(PaletteNumber);
    }
    STDMETHODIMP SetScissorRect(CONST RECT* rect) override {
        if (m_isShadowPass && rect) {
            TraceSP("SetScissorRect (%ld,%ld)-(%ld,%ld)\n", (long)rect->left, (long)rect->top, (long)rect->right,
                    (long)rect->bottom);
        }
        return m_pReal->SetScissorRect(rect);
    }
    STDMETHODIMP GetScissorRect(RECT* pRect) override { return m_pReal->GetScissorRect(pRect); }
    STDMETHODIMP SetSoftwareVertexProcessing(BOOL bSoftware) override {
        if (g_Config.forceHwVp && bSoftware) {
            Log(1, "SetSoftwareVertexProcessing: overriding TRUE -> FALSE\n");
            bSoftware = FALSE;
        }
        return m_pReal->SetSoftwareVertexProcessing(bSoftware);
    }
    STDMETHODIMP_(BOOL) GetSoftwareVertexProcessing() override { return m_pReal->GetSoftwareVertexProcessing(); }
    STDMETHODIMP SetNPatchMode(float nSegments) override { return m_pReal->SetNPatchMode(nSegments); }
    STDMETHODIMP_(float) GetNPatchMode() override { return m_pReal->GetNPatchMode(); }

    STDMETHODIMP DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) override {
        if (m_isShadowPass) {
            m_shadowDrawCalls++;
            TraceSP("DrawPrimitive type=%d start=%u count=%u\n", PrimitiveType, StartVertex, PrimitiveCount);
        }
        return m_pReal->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
    }
    STDMETHODIMP DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex,
                                      UINT NumVertices, UINT startIndex, UINT primCount) override {
        if (m_isShadowPass) {
            m_shadowDrawCalls++;
            TraceSP("DrawIndexedPrimitive type=%d baseVtx=%d minVtx=%u numVtx=%u startIdx=%u primCount=%u\n",
                    PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
        }
        return m_pReal->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex,
                                              primCount);
    }
    STDMETHODIMP DrawPrimitiveUP(D3DPRIMITIVETYPE primitive_type, UINT primitive_count, CONST void* data,
                                 UINT stride) override {
        if (m_isShadowPass) {
            m_shadowDrawCalls++;
            TraceSP("DrawPrimitiveUP type=%d count=%u\n", primitive_type, primitive_count);
        }
        return m_pReal->DrawPrimitiveUP(primitive_type, primitive_count, data, stride);
    }
    STDMETHODIMP DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE primitive_type, UINT min_vertex_idx, UINT vertex_count,
                                        UINT primitive_count, CONST void* index_data, D3DFORMAT index_format,
                                        CONST void* data, UINT stride) override {
        if (m_isShadowPass) {
            m_shadowDrawCalls++;
            TraceSP("DrawIndexedPrimitiveUP type=%d count=%u\n", primitive_type, primitive_count);
        }
        return m_pReal->DrawIndexedPrimitiveUP(primitive_type, min_vertex_idx, vertex_count, primitive_count,
                                                index_data, index_format, data, stride);
    }
    STDMETHODIMP ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount,
                                 IDirect3DVertexBuffer9* pDestBuffer, IDirect3DVertexDeclaration9* pVertexDecl,
                                 DWORD Flags) override {
        return m_pReal->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags);
    }
    STDMETHODIMP CreateVertexDeclaration(CONST D3DVERTEXELEMENT9* elements,
                                         IDirect3DVertexDeclaration9** declaration) override {
        return m_pReal->CreateVertexDeclaration(elements, declaration);
    }
    STDMETHODIMP SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl) override {
        if (m_isShadowPass) TraceSP("SetVertexDeclaration %p\n", (void*)pDecl);
        return m_pReal->SetVertexDeclaration(pDecl);
    }
    STDMETHODIMP GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl) override {
        return m_pReal->GetVertexDeclaration(ppDecl);
    }
    STDMETHODIMP SetFVF(DWORD FVF) override { return m_pReal->SetFVF(FVF); }
    STDMETHODIMP GetFVF(DWORD* pFVF) override { return m_pReal->GetFVF(pFVF); }

    STDMETHODIMP CreateVertexShader(CONST DWORD* pFunction, IDirect3DVertexShader9** ppShader) override {
        static int vs_id = 0;
        int id = vs_id++;
        DumpShader("vs", pFunction, id);
        HRESULT hr = m_pReal->CreateVertexShader(pFunction, ppShader);
        if (FAILED(hr)) {
            Log(1, "CreateVertexShader failed! hr=0x%08X\n", hr);
        } else {
            Log(2, "CreateVertexShader id=%d -> %p\n", id, ppShader ? (void*)*ppShader : nullptr);
            if (ppShader && *ppShader) g_VsIds[*ppShader] = id;
        }
        return hr;
    }
    STDMETHODIMP SetVertexShader(IDirect3DVertexShader9* pShader) override {
        auto it = g_VsIds.find(pShader);
        int id = (it != g_VsIds.end()) ? it->second : -1;
        m_currentVsId = id;
        if (m_isShadowPass) {
            m_lastShadowVsId = id;
            TraceSP("SetVertexShader %p (vs_%d.bin)\n", (void*)pShader, id);
        }
        return m_pReal->SetVertexShader(pShader);
    }
    STDMETHODIMP GetVertexShader(IDirect3DVertexShader9** ppShader) override {
        return m_pReal->GetVertexShader(ppShader);
    }
    STDMETHODIMP SetVertexShaderConstantF(UINT reg_idx, CONST float* data, UINT count) override {
        if (m_isShadowPass) TraceSP("SetVertexShaderConstantF reg=%u count=%u\n", reg_idx, count);
        return m_pReal->SetVertexShaderConstantF(reg_idx, data, count);
    }
    STDMETHODIMP GetVertexShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) override {
        return m_pReal->GetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
    }
    STDMETHODIMP SetVertexShaderConstantI(UINT reg_idx, CONST int* data, UINT count) override {
        return m_pReal->SetVertexShaderConstantI(reg_idx, data, count);
    }
    STDMETHODIMP GetVertexShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount) override {
        return m_pReal->GetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount);
    }
    STDMETHODIMP SetVertexShaderConstantB(UINT reg_idx, CONST BOOL* data, UINT count) override {
        return m_pReal->SetVertexShaderConstantB(reg_idx, data, count);
    }
    STDMETHODIMP GetVertexShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount) override {
        return m_pReal->GetVertexShaderConstantB(StartRegister, pConstantData, BoolCount);
    }
    STDMETHODIMP SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData, UINT OffsetInBytes,
                                UINT Stride) override {
        if (m_isShadowPass) {
            m_shadowStreamSets++;
            TraceSP("SetStreamSource stream=%u buf=%p offset=%u stride=%u\n", StreamNumber, (void*)pStreamData,
                    OffsetInBytes, Stride);
        }
        return m_pReal->SetStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride);
    }
    STDMETHODIMP GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData, UINT* OffsetInBytes,
                                 UINT* pStride) override {
        return m_pReal->GetStreamSource(StreamNumber, ppStreamData, OffsetInBytes, pStride);
    }
    STDMETHODIMP SetStreamSourceFreq(UINT StreamNumber, UINT Divider) override {
        if (m_isShadowPass) TraceSP("SetStreamSourceFreq stream=%u divider=%u\n", StreamNumber, Divider);
        return m_pReal->SetStreamSourceFreq(StreamNumber, Divider);
    }
    STDMETHODIMP GetStreamSourceFreq(UINT StreamNumber, UINT* Divider) override {
        return m_pReal->GetStreamSourceFreq(StreamNumber, Divider);
    }
    STDMETHODIMP SetIndices(IDirect3DIndexBuffer9* pIndexData) override {
        if (m_isShadowPass) TraceSP("SetIndices %p\n", (void*)pIndexData);
        return m_pReal->SetIndices(pIndexData);
    }
    STDMETHODIMP GetIndices(IDirect3DIndexBuffer9** ppIndexData) override { return m_pReal->GetIndices(ppIndexData); }

    STDMETHODIMP CreatePixelShader(CONST DWORD* pFunction, IDirect3DPixelShader9** ppShader) override {
        static int ps_id = 0;
        int id = ps_id++;
        DumpShader("ps", pFunction, id);
        HRESULT hr = m_pReal->CreatePixelShader(pFunction, ppShader);
        if (FAILED(hr)) {
            Log(1, "CreatePixelShader failed! hr=0x%08X\n", hr);
        } else {
            Log(2, "CreatePixelShader id=%d -> %p\n", id, ppShader ? (void*)*ppShader : nullptr);
            if (ppShader && *ppShader) g_PsIds[*ppShader] = id;
        }
        return hr;
    }
    STDMETHODIMP SetPixelShader(IDirect3DPixelShader9* pShader) override {
        auto it = g_PsIds.find(pShader);
        int id = (it != g_PsIds.end()) ? it->second : -1;
        m_currentPsId = id;
        if (m_isShadowPass) {
            m_lastShadowPsId = id;
            TraceSP("SetPixelShader %p (ps_%d.bin)\n", (void*)pShader, id);
        }
        return m_pReal->SetPixelShader(pShader);
    }
    STDMETHODIMP GetPixelShader(IDirect3DPixelShader9** ppShader) override { return m_pReal->GetPixelShader(ppShader); }
    STDMETHODIMP SetPixelShaderConstantF(UINT reg_idx, CONST float* data, UINT count) override {
        return m_pReal->SetPixelShaderConstantF(reg_idx, data, count);
    }
    STDMETHODIMP GetPixelShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) override {
        return m_pReal->GetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount);
    }
    STDMETHODIMP SetPixelShaderConstantI(UINT reg_idx, CONST int* data, UINT count) override {
        return m_pReal->SetPixelShaderConstantI(reg_idx, data, count);
    }
    STDMETHODIMP GetPixelShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount) override {
        return m_pReal->GetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount);
    }
    STDMETHODIMP SetPixelShaderConstantB(UINT reg_idx, CONST BOOL* data, UINT count) override {
        return m_pReal->SetPixelShaderConstantB(reg_idx, data, count);
    }
    STDMETHODIMP GetPixelShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount) override {
        return m_pReal->GetPixelShaderConstantB(StartRegister, pConstantData, BoolCount);
    }
    STDMETHODIMP DrawRectPatch(UINT handle, CONST float* segment_count, CONST D3DRECTPATCH_INFO* patch_info) override {
        return m_pReal->DrawRectPatch(handle, segment_count, patch_info);
    }
    STDMETHODIMP DrawTriPatch(UINT handle, CONST float* segment_count, CONST D3DTRIPATCH_INFO* patch_info) override {
        return m_pReal->DrawTriPatch(handle, segment_count, patch_info);
    }
    STDMETHODIMP DeletePatch(UINT Handle) override { return m_pReal->DeletePatch(Handle); }

    STDMETHODIMP CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery) override {
        Log(1, "CreateQuery type=%d\n", Type);
        if (!ppQuery) return m_pReal->CreateQuery(Type, ppQuery);
        IDirect3DQuery9* pReal = nullptr;
        HRESULT hr = m_pReal->CreateQuery(Type, &pReal);
        if (SUCCEEDED(hr) && pReal) {
            *ppQuery = new ProxyQuery9(pReal, Type);
        } else {
            *ppQuery = nullptr;
        }
        return hr;
    }

private:
    IDirect3DDevice9* m_pReal;
    ULONG m_RefCount;
    bool m_isShadowPass = false;
    UINT m_shadowDrawCalls = 0;
    UINT m_shadowClears = 0;
    UINT m_shadowTransforms = 0;
    UINT m_shadowRenderStates = 0;
    UINT m_shadowTextureSets = 0;
    UINT m_shadowStreamSets = 0;
    int m_lastShadowVsId = -1;
    int m_lastShadowPsId = -1;
    int m_currentVsId = -1;
    int m_currentPsId = -1;
    int m_shadowMapStage = -1;
    IDirect3DSurface9* m_currentShadowRTSurface = nullptr;
    DWORD m_shadowEnterTick = 0;

    void EnterShadowPass() {
        m_isShadowPass = true;
        m_shadowDrawCalls = 0;
        m_shadowClears = 0;
        m_shadowTransforms = 0;
        m_shadowRenderStates = 0;
        m_shadowTextureSets = 0;
        m_shadowStreamSets = 0;
        m_lastShadowVsId = -1;
        m_lastShadowPsId = -1;
        m_shadowEnterTick = GetTickCount();
        Log(1, "Entering shadow pass (RT=1024x1024 R32F)\n");
        TraceSP("--- Enter shadow pass ---\n");
    }

    void ExitShadowPass() {
        DWORD elapsed = GetTickCount() - m_shadowEnterTick;
        Log(1,
            "Exiting shadow pass: draws=%u clears=%u transforms=%u renderStates=%u textureSets=%u streamSets=%u "
            "elapsedMs=%u lastVS=vs_%d.bin lastPS=ps_%d.bin\n",
            m_shadowDrawCalls, m_shadowClears, m_shadowTransforms, m_shadowRenderStates, m_shadowTextureSets,
            m_shadowStreamSets, elapsed, m_lastShadowVsId, m_lastShadowPsId);
        TraceSP("--- Exit shadow pass (draws=%u) ---\n", m_shadowDrawCalls);

        if (g_Config.dumpShadowMap && m_shadowDrawCalls > 0 && g_ShadowMapDumpCount < g_Config.maxShadowMapDumps &&
            m_currentShadowRTSurface) {
            DumpShadowMapContents(m_currentShadowRTSurface, m_shadowDrawCalls);
        }
        m_isShadowPass = false;
    }

    void DumpShadowMapContents(IDirect3DSurface9* pShadowSurface, UINT drawCalls) {
        D3DSURFACE_DESC desc;
        if (FAILED(pShadowSurface->GetDesc(&desc))) return;

        IDirect3DSurface9* pSysMem = nullptr;
        HRESULT hr =
            m_pReal->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &pSysMem, nullptr);
        if (FAILED(hr) || !pSysMem) {
            Log(1, "DumpShadowMap: CreateOffscreenPlainSurface failed hr=0x%08X\n", hr);
            return;
        }
        hr = m_pReal->GetRenderTargetData(pShadowSurface, pSysMem);
        if (FAILED(hr)) {
            Log(1, "DumpShadowMap: GetRenderTargetData failed hr=0x%08X\n", hr);
            pSysMem->Release();
            return;
        }
        D3DLOCKED_RECT lr;
        hr = pSysMem->LockRect(&lr, nullptr, D3DLOCK_READONLY);
        if (FAILED(hr)) {
            Log(1, "DumpShadowMap: LockRect failed hr=0x%08X\n", hr);
            pSysMem->Release();
            return;
        }

        float minV = 3.402823e38f, maxV = -3.402823e38f;
        double sum = 0.0;
        UINT zeroCount = 0, oneCount = 0, count = 0;
        for (UINT y = 0; y < desc.Height; y++) {
            const float* row = reinterpret_cast<const float*>(reinterpret_cast<const BYTE*>(lr.pBits) + (size_t)y * lr.Pitch);
            for (UINT x = 0; x < desc.Width; x++) {
                float v = row[x];
                if (v < minV) minV = v;
                if (v > maxV) maxV = v;
                sum += v;
                if (v == 0.0f) zeroCount++;
                if (v == 1.0f) oneCount++;
                count++;
            }
        }

        int dumpId = g_ShadowMapDumpCount++;
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "shadowmap_dump_%d.raw", dumpId);
        FILE* f = fopen(path, "wb");
        if (f) {
            for (UINT y = 0; y < desc.Height; y++) {
                fwrite(reinterpret_cast<const BYTE*>(lr.pBits) + (size_t)y * lr.Pitch, 1, desc.Width * 4, f);
            }
            fclose(f);
        }
        pSysMem->UnlockRect();

        Log(1,
            "DumpShadowMap #%d (draws=%u): %ux%u format=%d min=%g max=%g avg=%g zeroPixels=%u/%u onePixels=%u/%u "
            "-> %s\n",
            dumpId, drawCalls, desc.Width, desc.Height, desc.Format, minV, maxV, sum / count, zeroCount, count,
            oneCount, count, path);
        pSysMem->Release();
    }

    D3DFORMAT RemapUnsupportedDepthFormat(D3DFORMAT format, const char* ctx) {
        if (!g_Config.remapDepthFormat) return format;
        if (format == D3DFMT_D32) {
            Log(1, "[Remap] %s: D3DFMT_D32 -> D3DFMT_D24X8\n", ctx);
            return D3DFMT_D24X8;
        }
        if (format == (D3DFORMAT)79 /* D3DFMT_D24X4S4 */) {
            Log(1, "[Remap] %s: D3DFMT_D24X4S4 -> D3DFMT_D24S8\n", ctx);
            return D3DFMT_D24S8;
        }
        return format;
    }

    void ApplyCapsOverrides(D3DCAPS9* pCaps) {
        Log(2,
            "GetDeviceCaps: VertexShaderVersion=0x%08X PixelShaderVersion=0x%08X MaxVertexShaderConst=%u "
            "DevCaps=0x%08X RasterCaps=0x%08X\n",
            pCaps->VertexShaderVersion, pCaps->PixelShaderVersion, pCaps->MaxVertexShaderConst, pCaps->DevCaps,
            pCaps->RasterCaps);

        if (g_Config.vsVerMajor != 0) {
            DWORD newVer = D3DVS_VERSION(g_Config.vsVerMajor, g_Config.vsVerMinor);
            Log(1, "  [Override] VertexShaderVersion 0x%08X -> 0x%08X\n", pCaps->VertexShaderVersion, newVer);
            pCaps->VertexShaderVersion = newVer;
        }
        if (g_Config.psVerMajor != 0) {
            DWORD newVer = D3DPS_VERSION(g_Config.psVerMajor, g_Config.psVerMinor);
            Log(1, "  [Override] PixelShaderVersion 0x%08X -> 0x%08X\n", pCaps->PixelShaderVersion, newVer);
            pCaps->PixelShaderVersion = newVer;
        }
        if (g_Config.maxVsConst != 0) {
            Log(1, "  [Override] MaxVertexShaderConst %u -> %u\n", pCaps->MaxVertexShaderConst, g_Config.maxVsConst);
            pCaps->MaxVertexShaderConst = g_Config.maxVsConst;
        }
    }
};

// ---------------------------------------------------------------------
// IDirect3D9 proxy (only needs to intercept adapter/caps queries and
// CreateDevice; everything else passes straight through)
// ---------------------------------------------------------------------

class ProxyDirect3D9 : public IDirect3D9 {
public:
    ProxyDirect3D9(IDirect3D9* pReal) : m_pReal(pReal), m_RefCount(1) {}
    virtual ~ProxyDirect3D9() {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObj) override {
        if (riid == IID_IUnknown || riid == IID_IDirect3D9) {
            *ppvObj = this;
            AddRef();
            return S_OK;
        }
        return m_pReal->QueryInterface(riid, ppvObj);
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_RefCount); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG count = InterlockedDecrement(&m_RefCount);
        if (count == 0) {
            m_pReal->Release();
            delete this;
            return 0;
        }
        return count;
    }

    STDMETHODIMP RegisterSoftwareDevice(void* pInitializeFunction) override {
        return m_pReal->RegisterSoftwareDevice(pInitializeFunction);
    }
    STDMETHODIMP_(UINT) GetAdapterCount() override { return m_pReal->GetAdapterCount(); }

    STDMETHODIMP GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier) override {
        HRESULT hr = m_pReal->GetAdapterIdentifier(Adapter, Flags, pIdentifier);
        if (SUCCEEDED(hr) && pIdentifier) {
            Log(1, "GetAdapterIdentifier Adapter=%u Driver=%s Desc=%s VendorId=0x%X DeviceId=0x%X\n", Adapter,
                pIdentifier->Driver, pIdentifier->Description, pIdentifier->VendorId, pIdentifier->DeviceId);
            if (g_Config.spoofVendor) pIdentifier->VendorId = g_Config.spoofVendor;
            if (g_Config.spoofDevice) pIdentifier->DeviceId = g_Config.spoofDevice;
        }
        return hr;
    }
    STDMETHODIMP_(UINT) GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) override {
        return m_pReal->GetAdapterModeCount(Adapter, Format);
    }
    STDMETHODIMP EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode) override {
        return m_pReal->EnumAdapterModes(Adapter, Format, Mode, pMode);
    }
    STDMETHODIMP GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode) override {
        return m_pReal->GetAdapterDisplayMode(Adapter, pMode);
    }
    STDMETHODIMP CheckDeviceType(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat,
                                D3DFORMAT BackBufferFormat, BOOL bWindowed) override {
        return m_pReal->CheckDeviceType(Adapter, DevType, AdapterFormat, BackBufferFormat, bWindowed);
    }
    STDMETHODIMP CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage,
                                  D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) override {
        if (g_Config.blockDepthAsRT && RType == D3DRTYPE_TEXTURE && (Usage & D3DUSAGE_RENDERTARGET)) {
            switch (CheckFormat) {
                case D3DFMT_D16_LOCKABLE:
                case D3DFMT_D32:
                case D3DFMT_D15S1:
                case D3DFMT_D24S8:
                case D3DFMT_D24X8:
                case D3DFMT_D24X4S4:
                case D3DFMT_D16:
                    Log(2, "CheckDeviceFormat Format=%d blocked as depth-RT\n", CheckFormat);
                    return D3DERR_NOTAVAILABLE;
                default: break;
            }
        }
        HRESULT hr = m_pReal->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat);
        Log(2, "CheckDeviceFormat Format=%d Usage=0x%X RType=%d -> hr=0x%08X\n", CheckFormat, Usage, RType, hr);
        return hr;
    }
    STDMETHODIMP CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat,
                                           BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType,
                                           DWORD* pQualityLevels) override {
        return m_pReal->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType,
                                                    pQualityLevels);
    }
    STDMETHODIMP CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat,
                                       D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) override {
        return m_pReal->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat,
                                                DepthStencilFormat);
    }
    STDMETHODIMP CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat,
                                             D3DFORMAT TargetFormat) override {
        return m_pReal->CheckDeviceFormatConversion(Adapter, DeviceType, SourceFormat, TargetFormat);
    }

    STDMETHODIMP GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps) override {
        HRESULT hr = m_pReal->GetDeviceCaps(Adapter, DeviceType, pCaps);
        if (SUCCEEDED(hr) && pCaps) {
            Log(1,
                "IDirect3D9::GetDeviceCaps Adapter=%u VertexShaderVersion=0x%08X PixelShaderVersion=0x%08X "
                "MaxVertexShaderConst=%u\n",
                Adapter, pCaps->VertexShaderVersion, pCaps->PixelShaderVersion, pCaps->MaxVertexShaderConst);
            if (g_Config.vsVerMajor != 0) pCaps->VertexShaderVersion = D3DVS_VERSION(g_Config.vsVerMajor, g_Config.vsVerMinor);
            if (g_Config.psVerMajor != 0) pCaps->PixelShaderVersion = D3DPS_VERSION(g_Config.psVerMajor, g_Config.psVerMinor);
            if (g_Config.maxVsConst != 0) pCaps->MaxVertexShaderConst = g_Config.maxVsConst;
        }
        return hr;
    }
    STDMETHODIMP_(HMONITOR) GetAdapterMonitor(UINT Adapter) override { return m_pReal->GetAdapterMonitor(Adapter); }

    STDMETHODIMP CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
                              D3DPRESENT_PARAMETERS* pPresentationParameters,
                              IDirect3DDevice9** ppReturnedDeviceInterface) override {
        if (pPresentationParameters) {
            Log(1,
                "CreateDevice Adapter=%u DevType=%d Windowed=%d BackBuffer=%ux%u Format=%d SwapEffect=%d "
                "MultiSample=%d BehaviorFlags=0x%08X\n",
                Adapter, DeviceType, pPresentationParameters->Windowed, pPresentationParameters->BackBufferWidth,
                pPresentationParameters->BackBufferHeight, pPresentationParameters->BackBufferFormat,
                pPresentationParameters->SwapEffect, pPresentationParameters->MultiSampleType, BehaviorFlags);
        }
        if (g_Config.forceHwVp && (BehaviorFlags & D3DCREATE_SOFTWARE_VERTEXPROCESSING)) {
            BehaviorFlags = (BehaviorFlags & ~D3DCREATE_SOFTWARE_VERTEXPROCESSING) | D3DCREATE_HARDWARE_VERTEXPROCESSING;
            Log(1, "  [Override] Forcing hardware vertex processing\n");
        }

        IDirect3DDevice9* pRealDevice = nullptr;
        HRESULT hr = m_pReal->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters,
                                           &pRealDevice);
        if (SUCCEEDED(hr) && pRealDevice && ppReturnedDeviceInterface) {
            Log(1, "CreateDevice succeeded (%p), wrapping in ProxyDirect3DDevice9\n", (void*)pRealDevice);
            *ppReturnedDeviceInterface = new ProxyDirect3DDevice9(pRealDevice);
        } else {
            Log(1, "CreateDevice failed! hr=0x%08X\n", hr);
            if (ppReturnedDeviceInterface) *ppReturnedDeviceInterface = nullptr;
        }
        return hr;
    }

private:
    IDirect3D9* m_pReal;
    ULONG m_RefCount;
};

// ---------------------------------------------------------------------
// Direct3DCreate9/Ex hook entry points (these are what get patched into
// callers' IATs in place of the real d3d9.dll exports)
// ---------------------------------------------------------------------

typedef IDirect3D9* (WINAPI* Direct3DCreate9_t)(UINT SDKVersion);
typedef HRESULT(WINAPI* Direct3DCreate9Ex_t)(UINT SDKVersion, IDirect3D9Ex** ppDirect3D9Ex);

static Direct3DCreate9_t RealDirect3DCreate9 = nullptr;
static Direct3DCreate9Ex_t RealDirect3DCreate9Ex = nullptr;

static IDirect3D9* WINAPI HookDirect3DCreate9(UINT SDKVersion) {
    Log(1, "Direct3DCreate9(SDKVersion=%u) called\n", SDKVersion);
    if (!RealDirect3DCreate9) {
        Log(1, "ERROR: RealDirect3DCreate9 is null\n");
        return nullptr;
    }
    IDirect3D9* pReal = RealDirect3DCreate9(SDKVersion);
    if (!pReal) {
        Log(1, "RealDirect3DCreate9 returned NULL\n");
        return nullptr;
    }
    Log(1, "RealDirect3DCreate9 -> %p, wrapping\n", (void*)pReal);
    return new ProxyDirect3D9(pReal);
}

static HRESULT WINAPI HookDirect3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppDirect3D9Ex) {
    Log(1, "Direct3DCreate9Ex(SDKVersion=%u) called\n", SDKVersion);
    if (!RealDirect3DCreate9Ex) {
        Log(1, "ERROR: RealDirect3DCreate9Ex is null\n");
        return D3DERR_NOTAVAILABLE;
    }
    // Not wrapped (the game itself only ever calls the non-Ex entry
    // point; this passthrough just avoids leaving a dangling IAT hook
    // if something else calls it).
    return RealDirect3DCreate9Ex(SDKVersion, ppDirect3D9Ex);
}

// ---------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------

static void InstallHooks() {
    HMODULE hD3D9 = GetModuleHandleA("d3d9.dll");
    if (!hD3D9) {
        Log(1, "ERROR: d3d9.dll not loaded yet; cannot hook Direct3DCreate9\n");
        return;
    }
    // Fallback in case no importer is found to chain onto (see below).
    RealDirect3DCreate9 = reinterpret_cast<Direct3DCreate9_t>(
        reinterpret_cast<void*>(GetProcAddress(hD3D9, "Direct3DCreate9")));
    RealDirect3DCreate9Ex = reinterpret_cast<Direct3DCreate9Ex_t>(
        reinterpret_cast<void*>(GetProcAddress(hD3D9, "Direct3DCreate9Ex")));
    Log(1, "Real d3d9.dll=%p Direct3DCreate9=%p Direct3DCreate9Ex=%p\n", (void*)hD3D9, (void*)RealDirect3DCreate9,
        (void*)RealDirect3DCreate9Ex);

    // Chain onto whatever's currently in the IAT slot rather than always
    // calling straight into d3d9.dll -- if a future mod already hooked
    // Direct3DCreate9, that gives us its hook as the
    // "real" function to call through, so both mods' proxies compose
    // regardless of mad2/mods/ load order instead of one silently
    // clobbering the other.
    void* prev1 = nullptr;
    int n1 = Mad2HookUtil_PatchIatAllModules("d3d9.dll", "Direct3DCreate9", reinterpret_cast<void*>(HookDirect3DCreate9), &prev1);
    if (prev1) RealDirect3DCreate9 = reinterpret_cast<Direct3DCreate9_t>(prev1);
    int n2 = 0;
    if (RealDirect3DCreate9Ex) {
        void* prev2 = nullptr;
        n2 = Mad2HookUtil_PatchIatAllModules("d3d9.dll", "Direct3DCreate9Ex", reinterpret_cast<void*>(HookDirect3DCreate9Ex),
                                 &prev2);
        if (prev2) RealDirect3DCreate9Ex = reinterpret_cast<Direct3DCreate9Ex_t>(prev2);
    }
    Log(1, "Patched Direct3DCreate9 in %d module(s) (chained real=%p), Direct3DCreate9Ex in %d module(s)\n", n1,
        (void*)RealDirect3DCreate9, n2);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        LoadConfig();
        InstallHooks();
    }
    return TRUE;
}
