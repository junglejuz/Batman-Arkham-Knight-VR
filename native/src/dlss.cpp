// AKN DLSS (stage D1, akvr_native/DLSS_PLAN.md) - one NVIDIA DLSS Super Resolution feature per eye.
// Method after Luma's Arkham Knight mod (Luma Framework, Filippo Tarpini / Garamond, "Custom MIT": reuse with credit):
// jitter the eye lens, motion vectors + depth from the game's own passes, skip the game's SMAA. Our placement: the END
// of each eye render - the finished (small) eye picture is upscaled into the full-size headset picture.
// DLSS DLL: nvngx_dlss.dll 310.9.1 (DLSS 4.x Super Resolution - JJ: newest 4.x, NOT DLSS 5) in the akn folder.
#include <windows.h>
#include <d3d11_1.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"
#include "dlss.h"
#include <d3dcompiler.h>
#include <map>

void akn_log(const char* fmt, ...);   // probe.cpp
#define logf akn_log

namespace {
int   g_mode = -1;                 // akvr_settings.ini dlss=: 0 off, 1 DLAA, 2 ultra quality, 3 quality, 4 balanced, 5 performance
UINT  g_outW = 0, g_outH = 0;      // the headset eye size (engineres x engineshape, as AKVR derives it)
float g_scale = 1.0f;
wchar_t g_dir[MAX_PATH] = L"";     // ...\Binaries\Win64\akn\ (the DLL's folder)
bool  g_ngxTried = false, g_ngxOk = false;
NVSDK_NGX_Parameter* g_caps = nullptr;
ID3D11Device* g_dev = nullptr;
struct Eye
{
    NVSDK_NGX_Handle* h = nullptr; NVSDK_NGX_Parameter* p = nullptr; bool failed = false;
    UINT rw = 0, rh = 0;
    ID3D11Texture2D* in = nullptr; ID3D11Texture2D* out = nullptr; ID3D11Texture2D* mvCopy = nullptr;
    ID3D11Resource* mv = nullptr; ID3D11Resource* depth = nullptr;   // weak: the game's own, noted this eye render
    bool reset = true; long evals = 0, skips = 0;
    UINT newW = 0, newH = 0; int newN = 0;   // a size change waiting to last
    double prevVP[16] = {}; bool prevOk = false;   // CAMMV: last frame's view x lens of this eye, without the jitter
    ID3D11Texture2D* mvOut = nullptr; ID3D11UnorderedAccessView* mvUav = nullptr;
} g_eye[2];
volatile LONG g_preset = 0;        // 0 = DLSS default for this build; else NVSDK_NGX_DLSS_Hint_Render_Preset_A.. (1..)
volatile LONG g_depthInv = 1, g_mvJit = 1;
volatile LONG g_mvSignY = 1;       // test switch: +1 as Luma, -1 flips the vertical motion
// CAMMV 2026-10-08 (JJ in the headset: "the background is still shimmering" with the rain off). AK's velocity buffer
// holds MOVING objects only (motion blur reconstructs the camera's own motion from depth); in VR the head never stops,
// so DLSS got "no motion" for the whole background and mis-aligned its history. Our own pass: every pixel's point from
// this eye's depth + camera, projected with the same eye's camera of the previous frame = the camera motion; where the
// game's buffer has a moving object, its own value is kept (mode 2). Motion is written in render pixels, without jitter.
volatile LONG g_mvMode = 1;        // D1m: 1 best (still-head change 1.07 vs 1.84 / 2.21); 0 the game's buffer only (Luma), 1 camera motion only, 2 camera + the game's moving objects
ID3D11ComputeShader* g_mvCS = nullptr; ID3D11Buffer* g_mvCB = nullptr; int g_mvState = 0;
std::map<ID3D11Resource*, ID3D11ShaderResourceView*> g_srvCache;
ID3DDeviceContextState* g_state = nullptr;
// DLSS cost on the graphics card (timestamp queries around each evaluate, read back a few frames later)
struct Tq { ID3D11Query* dj = nullptr; ID3D11Query* a = nullptr; ID3D11Query* b = nullptr; bool pending = false; };
Tq g_tq[2][6]; int g_tqI[2] = {}; double g_tqSum = 0; long g_tqN = 0; double g_lastMs = 0;
void tq_collect(ID3D11DeviceContext* c)
{
    for (auto& row : g_tq) for (auto& q : row)
    {
        if (!q.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{}; UINT64 a = 0, b = 0;
        if (c->GetData(q.dj, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (c->GetData(q.a, &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || c->GetData(q.b, &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        q.pending = false;
        if (!dj.Disjoint && dj.Frequency && b > a) { g_tqSum += (double)(b - a) * 1000.0 / (double)dj.Frequency; ++g_tqN; }
    }
}
Tq* tq_begin(ID3D11DeviceContext* c, int eye)
{
    tq_collect(c);
    Tq& q = g_tq[eye][g_tqI[eye]++ % 6];
    if (q.pending) return nullptr;
    if (!q.dj) { D3D11_QUERY_DESC d{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }; g_dev->CreateQuery(&d, &q.dj); d.Query = D3D11_QUERY_TIMESTAMP; g_dev->CreateQuery(&d, &q.a); g_dev->CreateQuery(&d, &q.b); }
    if (!q.dj || !q.a || !q.b) return nullptr;
    c->Begin(q.dj); c->End(q.a);
    return &q;
}
void tq_end(ID3D11DeviceContext* c, Tq* q) { if (q) { c->End(q->b); c->End(q->dj); q->pending = true; } }

float halton(int i, int b) { float f = 1, r = 0; while (i > 0) { f /= (float)b; r += f * (float)(i % b); i /= b; } return r; }

int read_int(const char* buf, const char* key, int def)
{
    const size_t kl = strlen(key);
    for (const char* p = buf; (p = strstr(p, key)) != nullptr; p += kl)
        if ((p == buf || p[-1] == '\n') && p[kl] == '=') return atoi(p + kl + 1);
    return def;
}
void load_settings()
{
    if (g_mode >= 0) return;
    g_mode = 0;
    wchar_t ini[MAX_PATH]; GetModuleFileNameW(nullptr, ini, MAX_PATH);
    wchar_t* sl = wcsrchr(ini, L'\\'); if (!sl) return; sl[1] = 0;
    wcscpy_s(g_dir, ini); wcscat_s(g_dir, L"akn");
    wcscat_s(ini, L"akvr_settings.ini");
    FILE* f = nullptr; _wfopen_s(&f, ini, L"rb"); if (!f) return;
    static char buf[1 << 16]; const size_t n = fread(buf, 1, sizeof(buf) - 1, f); fclose(f); buf[n] = 0;
    g_mode = read_int(buf, "dlss", 0);
    const int eh = read_int(buf, "engineres", 2448), shape = read_int(buf, "engineshape", 911);
    g_outH = (UINT)eh; g_outW = (UINT)(((long long)eh * shape) / 1000) & ~7u;
    static const float kScale[6] = { 1.0f, 1.0f, 0.77f, 0.667f, 0.58f, 0.5f };
    g_scale = g_mode >= 0 && g_mode <= 5 ? kScale[g_mode] : 1.0f;
    logf("DLSS: mode %d (scale %.3f), headset eye %ux%u (from akvr_settings.ini)", g_mode, g_scale, g_outW, g_outH);
}
bool ngx_init(ID3D11Device* dev)
{
    if (g_ngxTried) return g_ngxOk;
    g_ngxTried = true; g_dev = dev;
    const wchar_t* paths[1] = { g_dir };
    NVSDK_NGX_FeatureCommonInfo info{}; info.PathListInfo.Path = paths; info.PathListInfo.Length = 1;
    // our own project GUID (unregistered with NVIDIA, like Luma's)
    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID("5b2c8e41-7a6d-4f0e-9c3b-2d8e1f6a4b70", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", g_dir, dev, &info);
    if (NVSDK_NGX_FAILED(r)) { logf("DLSS: NGX init FAILED 0x%08x (nvngx_dlss.dll in %ls?)", (unsigned)r, g_dir); return false; }
    r = NVSDK_NGX_D3D11_GetCapabilityParameters(&g_caps);
    int avail = 0; if (g_caps) g_caps->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
    g_ngxOk = NVSDK_NGX_SUCCEED(r) && avail;
    logf("DLSS: NGX %s (super resolution available %d)", g_ngxOk ? "ready" : "NOT usable", avail);
    ID3D11Device1* d1 = nullptr;
    if (g_ngxOk && SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&d1)) && d1)
    {   // a private context state: DLSS runs inside it, the game's state is swapped back untouched
        const D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel();
        d1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &g_state);
        d1->Release();
    }
    return g_ngxOk;
}
void mul4(const double a[16], const double b[16], double o[16])
{
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) { double v = 0; for (int k = 0; k < 4; ++k) v += a[i * 4 + k] * b[k * 4 + j]; o[i * 4 + j] = v; }
}
bool inv4(const double m[16], double o[16])
{   // Gauss-Jordan with partial pivoting
    double a[4][8];
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 8; ++j) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c)
    {
        int p = c; for (int r = c + 1; r < 4; ++r) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-30) return false;
        if (p != c) for (int j = 0; j < 8; ++j) { const double t = a[c][j]; a[c][j] = a[p][j]; a[p][j] = t; }
        const double d = a[c][c]; for (int j = 0; j < 8; ++j) a[c][j] /= d;
        for (int r = 0; r < 4; ++r) if (r != c) { const double f = a[r][c]; for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j]; }
    }
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) o[i * 4 + j] = a[i][j + 4];
    return true;
}
ID3D11ShaderResourceView* srv_for(ID3D11Resource* r)
{
    auto it = g_srvCache.find(r);
    if (it != g_srvCache.end()) return it->second;
    ID3D11Texture2D* t = nullptr; D3D11_TEXTURE2D_DESC d{};
    if (FAILED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) || !t) return nullptr;
    t->GetDesc(&d); t->Release();
    DXGI_FORMAT f = d.Format;
    if (f == DXGI_FORMAT_R32G8X24_TYPELESS || f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) f = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    else if (f == DXGI_FORMAT_R32_TYPELESS || f == DXGI_FORMAT_D32_FLOAT) f = DXGI_FORMAT_R32_FLOAT;
    else if (f == DXGI_FORMAT_R24G8_TYPELESS || f == DXGI_FORMAT_D24_UNORM_S8_UINT) f = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    else if (f == DXGI_FORMAT_R16G16_TYPELESS) f = DXGI_FORMAT_R16G16_FLOAT;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = f; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* v = nullptr;
    if (FAILED(g_dev->CreateShaderResourceView(r, &sd, &v))) { logf("DLSS: camera motion - no view on a texture of format %d", (int)d.Format); v = nullptr; }
    g_srvCache[r] = v;
    return v;
}
bool mv_build()
{
    if (g_mvState) return g_mvState == 1;
    g_mvState = -1;
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    CompileFn compile = m ? (CompileFn)GetProcAddress(m, "D3DCompile") : nullptr;
    if (!compile) { logf("DLSS: camera motion - no shader compiler"); return false; }
    static const char cs[] =
        "Texture2D<float> depthT : register(t0); Texture2D<float2> gameMv : register(t1); RWTexture2D<float2> outMv : register(u0);"
        "cbuffer C : register(b0) { float4 r0, r1, r2, r3; float4 sz; float4 jit; };"
        "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {"
        " if (id.x >= (uint)sz.x || id.y >= (uint)sz.y) return;"
        " float d = depthT.Load(int3(id.xy, 0));"
        " float2 ndc = float2((id.x + 0.5) * sz.z * 2.0 - 1.0, 1.0 - (id.y + 0.5) * sz.w * 2.0);"
        " float4 p = ndc.x * r0 + ndc.y * r1 + d * r2 + r3;"
        " float2 prev = p.xy / p.w;"
        " float2 mvN = prev - (ndc - jit.xy);"
        " float2 mv = float2(mvN.x * sz.x * 0.5, -mvN.y * sz.y * 0.5);"
        " if (jit.z > 1.5) { float2 g = gameMv.Load(int3(id.xy, 0)); if (abs(g.x) + abs(g.y) > 1e-6) mv = float2(g.x * sz.x * 0.5, g.y * sz.y * 0.5 * jit.w); }"
        " outMv[id.xy] = mv; }";
    ID3DBlob* b = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(compile(cs, sizeof(cs) - 1, "cammv", nullptr, nullptr, "main", "cs_5_0", 0, 0, &b, &err)) || !b)
    { logf("DLSS: camera motion shader failed: %s", err ? (const char*)err->GetBufferPointer() : "?"); if (err) err->Release(); return false; }
    if (err) err->Release();
    const bool ok = SUCCEEDED(g_dev->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_mvCS));
    b->Release();
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 96; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (!ok || FAILED(g_dev->CreateBuffer(&bd, nullptr, &g_mvCB))) return false;
    g_mvState = 1;
    logf("DLSS: camera motion pass ready");
    return true;
}
bool make_tex(UINT w, UINT h, DXGI_FORMAT f, UINT bind, ID3D11Texture2D** out)
{
    D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = f; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind;
    return SUCCEEDED(g_dev->CreateTexture2D(&d, nullptr, out)) && *out;
}
bool make_feature(ID3D11DeviceContext* c, Eye& e, UINT rw, UINT rh)
{
    if (e.h) { NVSDK_NGX_D3D11_ReleaseFeature(e.h); e.h = nullptr; }
    if (!e.p && NVSDK_NGX_FAILED(NVSDK_NGX_D3D11_AllocateParameters(&e.p))) return false;
    const int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
                      (g_depthInv ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0) | ((g_mvJit && g_mvMode == 0) ? NVSDK_NGX_DLSS_Feature_Flags_MVJittered : 0);
    if (g_preset > 0)
    {
        const char* const keys[] = { NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality,
                                     NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                                     NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance };
        for (const char* k : keys) NVSDK_NGX_Parameter_SetUI(e.p, k, (unsigned)g_preset);
    }
    NVSDK_NGX_DLSS_Create_Params cp{};
    cp.Feature.InWidth = rw; cp.Feature.InHeight = rh; cp.Feature.InTargetWidth = g_outW; cp.Feature.InTargetHeight = g_outH;
    cp.Feature.InPerfQualityValue = g_mode == 1 ? NVSDK_NGX_PerfQuality_Value_DLAA : g_mode == 2 ? NVSDK_NGX_PerfQuality_Value_UltraQuality :
                                    g_mode == 3 ? NVSDK_NGX_PerfQuality_Value_MaxQuality : g_mode == 4 ? NVSDK_NGX_PerfQuality_Value_Balanced : NVSDK_NGX_PerfQuality_Value_MaxPerf;
    cp.InFeatureCreateFlags = flags;
    NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT(c, &e.h, e.p, &cp);
    if (NVSDK_NGX_FAILED(r)) { cp.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_Balanced; r = NGX_D3D11_CREATE_DLSS_EXT(c, &e.h, e.p, &cp); }
    logf("DLSS: eye feature %ux%u -> %ux%u, flags 0x%x, preset %ld: %s (0x%08x)", rw, rh, g_outW, g_outH, flags, g_preset, NVSDK_NGX_SUCCEED(r) ? "created" : "FAILED", (unsigned)r);
    e.rw = rw; e.rh = rh; e.reset = true;
    return NVSDK_NGX_SUCCEED(r) && e.h;
}
}

int   akn_dlss_mode() { load_settings(); return g_mode; }
float akn_dlss_scale() { load_settings(); return g_scale; }
void  akn_dlss_out_size(UINT& w, UINT& h) { load_settings(); w = g_outW; h = g_outH; }
void  akn_dlss_jitter(unsigned long long frame, float& jpx, float& jpy)
{   // Halton(2,3) sub-pixel offset in render pixels, -0.5..0.5; phase count 8 x (out/in)^2 (NVIDIA's guide)
    load_settings();
    if (g_mode <= 0) { jpx = jpy = 0; return; }
    const float ratio = g_scale > 0.01f ? 1.0f / g_scale : 1.0f;
    const int phases = (int)lrintf(8.0f * ratio * ratio);
    const int i = (int)(frame % (unsigned long long)(phases > 0 ? phases : 8)) + 1;
    jpx = halton(i, 2) - 0.5f; jpy = halton(i, 3) - 0.5f;
}
void akn_dlss_note_mv(int eye, ID3D11Resource* r) { if (eye == 0 || eye == 1) g_eye[eye].mv = r; }
void akn_dlss_note_depth(int eye, ID3D11Resource* r) { if (eye == 0 || eye == 1) g_eye[eye].depth = r; }
void akn_dlss_cmd(const char* what, int v)
{
    if (!strcmp(what, "preset")) g_preset = v;
    else if (!strcmp(what, "depthinv")) g_depthInv = v;
    else if (!strcmp(what, "mvjit")) g_mvJit = v;
    else if (!strcmp(what, "mvsigny")) { g_mvSignY = v; return; }
    else if (!strcmp(what, "mvmode")) g_mvMode = v;
    for (auto& e : g_eye) e.rw = 0;   // recreate the features with the new flags
}
ID3D11Texture2D* akn_dlss_eval(ID3D11DeviceContext* c, int eye, ID3D11Texture2D* src, float jpx, float jpy, const float* V, const float* P, bool vpOk)
{
    load_settings();
    if (g_mode <= 0 || eye < 0 || eye > 1 || !src) return nullptr;
    Eye& e = g_eye[eye];
    if (e.failed) return nullptr;
    ID3D11Device* dev = nullptr; c->GetDevice(&dev); if (!dev) return nullptr;
    const bool ok = ngx_init(dev); dev->Release();
    if (!ok) { e.failed = true; return nullptr; }
    if (!e.mv || !e.depth) { if ((e.skips++ % 300) == 0) logf("DLSS: eye %d - no %s yet, not upscaled", eye, !e.mv ? "motion vectors" : "depth"); return nullptr; }
    D3D11_TEXTURE2D_DESC sd{}; src->GetDesc(&sd);
    if (e.h && (e.rw != sd.Width || e.rh != sd.Height))
    {   // a different size: rebuild only if it lasts (30 frames) - a one-off render is skipped, the history kept
        if (sd.Width != e.newW || sd.Height != e.newH) { e.newW = sd.Width; e.newH = sd.Height; e.newN = 0; }
        if (++e.newN < 30) return nullptr;
    }
    if (e.rw != sd.Width || e.rh != sd.Height || !e.h)
    {
        e.newN = 0;
        if (e.in) { e.in->Release(); e.in = nullptr; }
        if (!e.out && !make_tex(g_outW, g_outH, sd.Format, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, &e.out)) { logf("DLSS: output texture %ux%u f%d FAILED", g_outW, g_outH, (int)sd.Format); e.failed = true; return nullptr; }
        if (!make_tex(sd.Width, sd.Height, sd.Format, D3D11_BIND_SHADER_RESOURCE, &e.in)) { e.failed = true; return nullptr; }
        if (!make_feature(c, e, sd.Width, sd.Height)) { e.failed = true; return nullptr; }
    }
    c->CopyResource(e.in, src);   // the window buffer cannot be read by a shader: a plain copy first (as PureDark's)
    // CAMMV: this eye's motion = camera motion (+ the game's moving objects)
    ID3D11Resource* mvRes = e.mv; float mvScaleX = (float)e.rw * 0.5f, mvScaleY = (float)e.rh * 0.5f * (float)g_mvSignY;
    if (g_mvMode > 0 && vpOk && V && P && mv_build())
    {
        if (!e.mvOut || [&] { D3D11_TEXTURE2D_DESC md{}; e.mvOut->GetDesc(&md); return md.Width != e.rw || md.Height != e.rh; }())
        {
            if (e.mvUav) { e.mvUav->Release(); e.mvUav = nullptr; } if (e.mvOut) { e.mvOut->Release(); e.mvOut = nullptr; }
            if (make_tex(e.rw, e.rh, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, &e.mvOut)) g_dev->CreateUnorderedAccessView(e.mvOut, nullptr, &e.mvUav);
        }
        // this frame's lens with and without the jitter (render pixels -> lens offset, as AKN put it in)
        double Pj[16], Pu[16], Vd[16], VPj[16], VPu[16], inv[16], M[16];
        for (int i = 0; i < 16; ++i) { Pj[i] = P[i]; Pu[i] = P[i]; Vd[i] = V[i]; }
        const double jxN = 2.0 * jpx / (double)e.rw, jyN = -2.0 * jpy / (double)e.rh;
        Pu[2 * 4 + 0] -= jxN; Pu[2 * 4 + 1] -= jyN;
        mul4(Vd, Pj, VPj); mul4(Vd, Pu, VPu);
        ID3D11ShaderResourceView* dv = srv_for(e.depth); ID3D11ShaderResourceView* gv = srv_for(e.mv);
        if (e.mvUav && dv && gv && inv4(VPj, inv))
        {
            if (e.prevOk) mul4(inv, e.prevVP, M); else mul4(inv, VPu, M);   // first frame: no motion
            float cb[24];
            for (int i = 0; i < 16; ++i) cb[i] = (float)M[i];
            cb[16] = (float)e.rw; cb[17] = (float)e.rh; cb[18] = 1.0f / (float)e.rw; cb[19] = 1.0f / (float)e.rh;
            cb[20] = (float)jxN; cb[21] = (float)jyN; cb[22] = (float)g_mvMode; cb[23] = (float)g_mvSignY;
            c->UpdateSubresource(g_mvCB, 0, nullptr, cb, 0, 0);
            ID3D11DeviceContext1* c1 = nullptr; ID3DDeviceContextState* prev = nullptr;
            if (g_state && SUCCEEDED(c->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1)) && c1) c1->SwapDeviceContextState(g_state, &prev);
            ID3D11ShaderResourceView* sv[2] = { dv, gv };
            c->CSSetShader(g_mvCS, nullptr, 0); c->CSSetShaderResources(0, 2, sv); c->CSSetUnorderedAccessViews(0, 1, &e.mvUav, nullptr);
            c->CSSetConstantBuffers(0, 1, &g_mvCB);
            c->Dispatch((e.rw + 7) / 8, (e.rh + 7) / 8, 1);
            ID3D11ShaderResourceView* nsv[2] = {}; ID3D11UnorderedAccessView* nuv = nullptr;
            c->CSSetShaderResources(0, 2, nsv); c->CSSetUnorderedAccessViews(0, 1, &nuv, nullptr);
            if (c1) { if (prev) { c1->SwapDeviceContextState(prev, nullptr); prev->Release(); } c1->Release(); }
            mvRes = e.mvOut; mvScaleX = 1.0f; mvScaleY = 1.0f;   // already in render pixels
        }
        for (int i = 0; i < 16; ++i) e.prevVP[i] = VPu[i];
        e.prevOk = true;
    }
    NVSDK_NGX_D3D11_DLSS_Eval_Params ep{};
    ep.Feature.pInColor = e.in; ep.Feature.pInOutput = e.out;
    ep.pInDepth = e.depth; ep.pInMotionVectors = mvRes;
    ep.InJitterOffsetX = jpx; ep.InJitterOffsetY = jpy;
    ep.InRenderSubrectDimensions.Width = e.rw; ep.InRenderSubrectDimensions.Height = e.rh;
    ep.InReset = e.reset ? 1 : 0; e.reset = false;
    ep.InMVScaleX = mvScaleX; ep.InMVScaleY = mvScaleY;   // the game's buffer: [-1, 1] (Luma); our camera motion: pixels
    ID3D11DeviceContext1* c1 = nullptr; ID3DDeviceContextState* prev = nullptr;
    if (g_state && SUCCEEDED(c->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1)) && c1) c1->SwapDeviceContextState(g_state, &prev);
    Tq* tq = tq_begin(c, eye);
    const NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_DLSS_EXT(c, e.h, e.p, &ep);
    tq_end(c, tq);
    if (c1) { if (prev) { c1->SwapDeviceContextState(prev, nullptr); prev->Release(); } c1->Release(); }
    if (NVSDK_NGX_FAILED(r)) { if ((e.skips++ % 300) == 0) logf("DLSS: eye %d evaluate FAILED 0x%08x", eye, (unsigned)r); return nullptr; }
    ++e.evals;
    return e.out;
}
const char* akn_dlss_diag()
{
    static char s[300];
    if (g_tqN) { g_lastMs = g_tqSum / g_tqN; g_tqSum = 0; g_tqN = 0; }
    snprintf(s, sizeof(s), "DLSS mode %d scale %.2f preset %ld | eye evals L %ld R %ld | skipped L %ld R %ld | ngx %d | graphics card ms per evaluate %.2f", g_mode, g_scale, g_preset, g_eye[0].evals, g_eye[1].evals, g_eye[0].skips, g_eye[1].skips, (int)g_ngxOk, g_lastMs);
    return s;
}
void akn_dlss_reset() { for (auto& e : g_eye) { e.reset = true; e.prevOk = false; } }
// AFW (afw.cpp): the eye's depth (render size, the game's own, valid right after that eye's render) and a view on it
void akn_dlss_render_size(int eye, UINT& w, UINT& h) { w = h = 0; if (eye == 0 || eye == 1) { w = g_eye[eye].rw; h = g_eye[eye].rh; } }   // AFW: the drawn area (DLSS input)
ID3D11Resource* akn_dlss_depth(int eye) { return eye == 0 || eye == 1 ? g_eye[eye].depth : nullptr; }
ID3D11ShaderResourceView* akn_dlss_srv(ID3D11Resource* r) { return r && g_dev ? srv_for(r) : nullptr; }
