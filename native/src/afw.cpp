// AKN AFW - alternate frame warping (2026-10-10, JJ: "an option for AFW rendering ... choose between native and AFW").
// akvr_native/NOTES.md "AFW PLAN 2026-10-10". Idea from PureDark's UEVR AFW (github PureDark/UEVR, branch AFW,
// PDAFWPlugin.h): each game frame draws ONE eye (alternating), and the OTHER eye is built from that same frame instead of
// being shown stale. His warp is a closed D3D12 plugin; this is our own D3D11 version, same three modes:
//   1 this frame:     the drawn eye moved into the other eye's view through its depth; holes stretched from the background
//   2 previous frame: the other eye's own last real picture, brought to now through this frame's depth
//   3 combined:       1, with its holes (disocclusions, the other eye's outer edge) filled from 2   (default)
// Pass 1 (render size): every depth pixel of the drawn eye -> its point in the world -> the other eye's pixel; the nearest
// point wins (InterlockedMin on the distance). Pass 2 (headset size): each pixel of the other eye takes that distance (or,
// in a hole, the farthest one nearby = the background), goes back into the world and picks its colour from the drawn eye
// (or from the other eye's previous picture).
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "dlss.h"

void akn_log(const char* fmt, ...);   // probe.cpp
#define logf akn_log

namespace {
volatile LONG g_on = -1;          // -1 = not read from akvr_settings.ini yet
volatile LONG g_mode = 3, g_win = 24, g_lum = 1, g_dbg = 0, g_log = 0;
volatile LONG g_feather = 120;   // FEATHER: blend zone at the drawn eye's left / right edge, thousandths of its width (0 = hard cut)
bool g_tried = false, g_ok = false;
ID3D11Device* g_dev = nullptr;
ID3DDeviceContextState* g_state = nullptr;
ID3D11ComputeShader* g_csSplat = nullptr; ID3D11ComputeShader* g_csGather = nullptr;
ID3D11Buffer* g_cbSplat = nullptr; ID3D11Buffer* g_cbGather = nullptr;
ID3D11SamplerState* g_lin = nullptr;
// the other eye's distance per render pixel (R32_UINT = float bits; 0xFFFFFFFF = nothing landed here)
ID3D11Texture2D* g_key = nullptr; ID3D11UnorderedAccessView* g_keyU = nullptr; ID3D11ShaderResourceView* g_keyS = nullptr;
UINT g_rw = 0, g_rh = 0;
// each eye's last real picture (headset size) + its view x lens without the jitter
ID3D11Texture2D* g_real[2] = {}; ID3D11ShaderResourceView* g_realS[2] = {};
double g_realVP[2][16] = {}; unsigned long long g_realAt[2] = {}; bool g_realOk[2] = {};
ID3D11Texture2D* g_out = nullptr; ID3D11UnorderedAccessView* g_outU = nullptr;
UINT g_ow = 0, g_oh = 0; DXGI_FORMAT g_fmt = DXGI_FORMAT_UNKNOWN;
// hole counts (every 16th pixel): [0] sampled, [1] disocclusion holes, [2] edge holes, [3] filled from the previous picture
ID3D11Buffer* g_stats = nullptr; ID3D11UnorderedAccessView* g_statsU = nullptr; ID3D11Buffer* g_statsRb[3] = {}; int g_statsI = 0; bool g_statsPend[3] = {};
double g_holePct = 0, g_edgePct = 0, g_prevPct = 0;
// graphics-card time of the warp
struct Tq { ID3D11Query* dj = nullptr; ID3D11Query* a = nullptr; ID3D11Query* b = nullptr; bool pending = false; } g_tq[4];
int g_tqI = 0; double g_tqSum = 0; long g_tqN = 0, g_warps = 0, g_fails = 0, g_noPrev = 0; double g_ms = 0;
unsigned long long g_n = 0;   // warps done (the frame clock of this module)

int read_int(const char* buf, const char* key, int def)
{
    const size_t kl = strlen(key);
    for (const char* p = buf; (p = strstr(p, key)) != nullptr; p += kl)
        if ((p == buf || p[-1] == '\n') && p[kl] == '=') return atoi(p + kl + 1);
    return def;
}
void load_settings()
{
    if (g_on >= 0) return;
    LONG on = 0;
    wchar_t ini[MAX_PATH]; GetModuleFileNameW(nullptr, ini, MAX_PATH);
    if (wchar_t* sl = wcsrchr(ini, L'\\')) { sl[1] = 0; wcscat_s(ini, L"akvr_settings.ini");
        FILE* f = nullptr; _wfopen_s(&f, ini, L"rb");
        if (f) { static char buf[1 << 16]; const size_t n = fread(buf, 1, sizeof(buf) - 1, f); fclose(f); buf[n] = 0;
            on = read_int(buf, "afw", 0) ? 1 : 0; const int m = read_int(buf, "afwmode", 3); g_mode = m >= 1 && m <= 3 ? m : 3; } }
    g_on = on;
    logf("AFW: %s at start, warp mode %ld (akvr_settings.ini afw= / afwmode=)", on ? "ON" : "off", g_mode);
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
void vp_of(const float* V, const float* P, float jxN, float jyN, double out[16])
{   // view x lens (row vectors), the lens with its jitter offsets taken out
    double v[16], p[16];
    for (int i = 0; i < 16; ++i) { v[i] = V[i]; p[i] = P[i]; }
    p[2 * 4 + 0] -= jxN; p[2 * 4 + 1] -= jyN;
    mul4(v, p, out);
}

// pass 1 - render size: the drawn eye's depth pixels splatted into the other eye's view (nearest wins)
const char kSplat[] =
    "Texture2D<float> depthT : register(t0); RWTexture2D<uint> keyU : register(u0);"
    "cbuffer C : register(b0) { float4 m0, m1, m2, m3; float4 a3; float4 rs; };"   // m = inverse(drawn VP, jittered) x other VP; a3 = inverse's w column
    "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {"
    " if (id.x >= (uint)rs.x || id.y >= (uint)rs.y) return;"
    " float d = depthT.Load(int3(id.xy, 0));"
    " float4 p = float4((id.x + 0.5) * rs.z * 2.0 - 1.0, 1.0 - (id.y + 0.5) * rs.w * 2.0, d, 1.0);"
    " float4 c = p.x * m0 + p.y * m1 + p.z * m2 + m3;"
    " float hw = dot(p, a3);"
    " if (hw < 0) { c = -c; hw = -hw; }"
    " if (c.w <= 1e-12) return;"
    " float z = hw > 1e-20 ? c.w / hw : 1e30;"   // the point's distance in the other eye's view (w = view z)
    " float2 n = c.xy / c.w;"
    " int2 q = int2(floor((n.x * 0.5 + 0.5) * rs.x), floor((0.5 - n.y * 0.5) * rs.y));"
    " if (q.x < 0 || q.y < 0 || q.x >= (int)rs.x || q.y >= (int)rs.y) return;"
    " uint o; InterlockedMin(keyU[q], asuint(min(z, 1e30)), o); }";

// pass 2 - headset size: each pixel of the other eye picks its colour
const char kGather[] =
    "Texture2D<uint> keyT : register(t0); Texture2D<float4> srcC : register(t1); Texture2D<float4> prevC : register(t2);"
    "RWTexture2D<float4> outC : register(u0); RWByteAddressBuffer stats : register(u1); SamplerState lin : register(s0);"
    "cbuffer C : register(b0) { float4 g0, g1, g2, g3; float4 h0, h1, h2, h3; float4 os; float4 rs; float4 pz; float4 fl; };"
    // g = inverse(other VP) x drawn VP (no jitter); h = inverse(other VP) x the other eye's last real VP
    // pz = lens depth terms P22, P32, hole search width, mode; fl = previous picture usable, debug, edge blend width (source uv)
    "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {"
    " if (id.x >= (uint)os.x || id.y >= (uint)os.y) return;"
    " float2 uv = (id.xy + 0.5) * os.zw;"
    " float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);"
    " int2 kp = min(int2(uv * rs.xy), int2(rs.xy) - 1);"
    " uint k = keyT.Load(int3(kp, 0));"
    " int hole = 0;"
    " if (k == 0xFFFFFFFF) {"
    "  int win = (int)pz.z; int st = max(1, win / 12); uint best = 0;"
    "  for (int dy = -1; dy <= 1; ++dy) for (int dx = -win; dx <= win; dx += st) {"
    "   int2 q = kp + int2(dx, dy); if (q.x < 0 || q.y < 0 || q.x >= (int)rs.x || q.y >= (int)rs.y) continue;"
    "   uint v = keyT.Load(int3(q, 0)); if (v != 0xFFFFFFFF && v > best) best = v; }"   // farthest = the background
    "  if (best != 0) { k = best; hole = 1; } else { k = asuint(1e30); hole = 2; } }"
    " float z = asfloat(k);"
    " float4 p = float4(ndc, pz.x + pz.y / max(z, 1e-6), 1.0);"
    " float4 cs = p.x * g0 + p.y * g1 + p.z * g2 + g3;"
    " float2 su = cs.w > 1e-12 ? float2(cs.x / cs.w * 0.5 + 0.5, 0.5 - cs.y / cs.w * 0.5) : uv;"
    " float4 col = srcC.SampleLevel(lin, su, 0);"
    " int mode = (int)pz.w; bool fromPrev = false;"
    " if (fl.x > 0.5 && (mode == 2 || (mode == 3 && hole != 0))) {"
    "  float4 cp = p.x * h0 + p.y * h1 + p.z * h2 + h3;"
    "  if (cp.w > 1e-12) { float2 pu = float2(cp.x / cp.w * 0.5 + 0.5, 0.5 - cp.y / cp.w * 0.5);"
    "   if (all(pu >= 0.0) && all(pu <= 1.0)) { col = prevC.SampleLevel(lin, pu, 0); fromPrev = true; } } }"
    // FEATHER (JJ 2026-10-10: vertical lines on the very left and very right, from dark to slightly brighter; capture
    // c011732: a 46 -> 40 step exactly where the drawn eye's picture ends): near the drawn eye's left / right edge, blend
    // over to the other eye's own previous picture, so the two sources meet without a line
    " else if (fl.x > 0.5 && mode == 3 && fl.z > 0.0) {"
    "  float e = saturate(min(su.x, 1.0 - su.x) / fl.z);"
    "  if (e < 1.0) { float4 cp = p.x * h0 + p.y * h1 + p.z * h2 + h3;"
    "   if (cp.w > 1e-12) { float2 pu = float2(cp.x / cp.w * 0.5 + 0.5, 0.5 - cp.y / cp.w * 0.5);"
    "    if (all(pu >= 0.0) && all(pu <= 1.0)) { float w = e * e * (3.0 - 2.0 * e); col = lerp(prevC.SampleLevel(lin, pu, 0), col, w); fromPrev = w < 0.5; } } } }"
    " if (fl.y > 0.5 && hole != 0) col = hole == 1 ? float4(1, 0, 1, 1) : float4(0, 1, 1, 1);"
    " if (fl.y > 1.5 && fromPrev) col = float4(col.r * 0.5, col.g * 0.5 + 0.5, col.b * 0.5, col.a);"
    " outC[id.xy] = col;"
    " if (((id.x | id.y) & 3) == 0) { uint o; stats.InterlockedAdd(0, 1, o);"
    "  if (hole == 1) stats.InterlockedAdd(4, 1, o); if (hole == 2) stats.InterlockedAdd(8, 1, o); if (fromPrev) stats.InterlockedAdd(12, 1, o); } }";

bool build(ID3D11Device* dev)
{
    if (g_tried) return g_ok;
    g_tried = true; g_dev = dev;
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    CompileFn compile = m ? (CompileFn)GetProcAddress(m, "D3DCompile") : nullptr;
    if (!compile) { logf("AFW: no shader compiler - off"); return false; }
    auto cs = [&](const char* src, size_t n, const char* name, ID3D11ComputeShader** out) {
        ID3DBlob* b = nullptr; ID3DBlob* err = nullptr;
        if (FAILED(compile(src, n, name, nullptr, nullptr, "main", "cs_5_0", 0, 0, &b, &err)) || !b)
        { logf("AFW: %s shader failed: %s", name, err ? (const char*)err->GetBufferPointer() : "?"); if (err) err->Release(); return false; }
        if (err) err->Release();
        const bool ok = SUCCEEDED(dev->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, out)); b->Release(); return ok; };
    if (!cs(kSplat, sizeof(kSplat) - 1, "afw_splat", &g_csSplat) || !cs(kGather, sizeof(kGather) - 1, "afw_gather", &g_csGather)) return false;
    D3D11_BUFFER_DESC bd{}; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.ByteWidth = 96; if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_cbSplat))) return false;
    bd.ByteWidth = 192; if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_cbGather))) return false;
    D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sd, &g_lin))) return false;
    D3D11_BUFFER_DESC sb{}; sb.ByteWidth = 16; sb.Usage = D3D11_USAGE_DEFAULT; sb.BindFlags = D3D11_BIND_UNORDERED_ACCESS; sb.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (FAILED(dev->CreateBuffer(&sb, nullptr, &g_stats))) return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.Format = DXGI_FORMAT_R32_TYPELESS; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = 4; ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (FAILED(dev->CreateUnorderedAccessView(g_stats, &ud, &g_statsU))) return false;
    D3D11_BUFFER_DESC rb{}; rb.ByteWidth = 16; rb.Usage = D3D11_USAGE_STAGING; rb.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto& r : g_statsRb) dev->CreateBuffer(&rb, nullptr, &r);
    ID3D11Device1* d1 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&d1)) && d1)
    {   // a private context state, as DLSS uses: the game's bindings are swapped back untouched (STRBLEND: set all we use)
        const D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel();
        d1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &g_state);
        d1->Release();
    }
    g_ok = g_state != nullptr;
    logf("AFW: warp passes %s", g_ok ? "ready" : "NOT ready (no private context state)");
    return g_ok;
}
void drop_sized()
{
    for (int e = 0; e < 2; ++e) { if (g_realS[e]) g_realS[e]->Release(); if (g_real[e]) g_real[e]->Release(); g_realS[e] = nullptr; g_real[e] = nullptr; g_realOk[e] = false; }
    if (g_outU) g_outU->Release(); if (g_out) g_out->Release(); g_outU = nullptr; g_out = nullptr; g_ow = g_oh = 0;
}
bool ensure(UINT rw, UINT rh, UINT ow, UINT oh, DXGI_FORMAT fmt)
{
    if (rw != g_rw || rh != g_rh || !g_key)
    {
        if (g_keyU) g_keyU->Release(); if (g_keyS) g_keyS->Release(); if (g_key) g_key->Release(); g_keyU = nullptr; g_keyS = nullptr; g_key = nullptr;
        D3D11_TEXTURE2D_DESC d{}; d.Width = rw; d.Height = rh; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R32_UINT; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_key)) || FAILED(g_dev->CreateUnorderedAccessView(g_key, nullptr, &g_keyU)) || FAILED(g_dev->CreateShaderResourceView(g_key, nullptr, &g_keyS)))
        { logf("AFW: distance texture %ux%u FAILED", rw, rh); return false; }
        g_rw = rw; g_rh = rh;
        logf("AFW: distance texture %ux%u", rw, rh);
    }
    if (ow != g_ow || oh != g_oh || fmt != g_fmt || !g_out)
    {
        drop_sized();
        D3D11_TEXTURE2D_DESC d{}; d.Width = ow; d.Height = oh; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        for (int e = 0; e < 2; ++e)
            if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_real[e])) || FAILED(g_dev->CreateShaderResourceView(g_real[e], nullptr, &g_realS[e]))) { logf("AFW: eye picture copy %ux%u f%d FAILED", ow, oh, (int)fmt); drop_sized(); return false; }
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_out)) || FAILED(g_dev->CreateUnorderedAccessView(g_out, nullptr, &g_outU))) { logf("AFW: warped eye %ux%u f%d FAILED", ow, oh, (int)fmt); drop_sized(); return false; }
        g_ow = ow; g_oh = oh; g_fmt = fmt;
        logf("AFW: eye pictures %ux%u format %d", ow, oh, (int)fmt);
    }
    return true;
}
void tq_collect(ID3D11DeviceContext* c)
{
    for (auto& q : g_tq)
    {
        if (!q.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{}; UINT64 a = 0, b = 0;
        if (c->GetData(q.dj, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (c->GetData(q.a, &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || c->GetData(q.b, &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        q.pending = false;
        if (!dj.Disjoint && dj.Frequency && b > a) { g_tqSum += (double)(b - a) * 1000.0 / (double)dj.Frequency; ++g_tqN; }
    }
}
Tq* tq_begin(ID3D11DeviceContext* c)
{
    tq_collect(c);
    Tq& q = g_tq[g_tqI++ & 3];
    if (q.pending) return nullptr;
    if (!q.dj) { D3D11_QUERY_DESC d{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }; g_dev->CreateQuery(&d, &q.dj); d.Query = D3D11_QUERY_TIMESTAMP; g_dev->CreateQuery(&d, &q.a); g_dev->CreateQuery(&d, &q.b); }
    if (!q.dj || !q.a || !q.b) return nullptr;
    c->Begin(q.dj); c->End(q.a);
    return &q;
}
void stats_collect(ID3D11DeviceContext* c)
{   // hole counts of a few frames ago (no stall: DO_NOT_WAIT)
    for (int i = 0; i < 3; ++i)
    {
        if (!g_statsPend[i] || !g_statsRb[i]) continue;
        D3D11_MAPPED_SUBRESOURCE ms{};
        if (c->Map(g_statsRb[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms) != S_OK) continue;
        const UINT* s = (const UINT*)ms.pData;
        if (s[0]) { g_holePct = 100.0 * s[1] / s[0]; g_edgePct = 100.0 * s[2] / s[0]; g_prevPct = 100.0 * s[3] / s[0]; }
        c->Unmap(g_statsRb[i], 0);
        g_statsPend[i] = false;
    }
}
}

bool akn_afw_on() { load_settings(); return g_on > 0; }
int  akn_afw_lum() { return (int)g_lum; }
int  akn_afw_mode() { return (int)g_mode; }
void akn_afw_reset() { g_realOk[0] = g_realOk[1] = false; }
bool akn_afw_cmd(const char* line)
{
    int v = 0;
    if (sscanf_s(line, "afwmode %d", &v) == 1) { g_mode = v >= 1 && v <= 3 ? v : 3; logf("command: AFW warp mode %ld (1 this frame, 2 previous frame, 3 combined)", g_mode); return true; }
    if (sscanf_s(line, "afwwin %d", &v) == 1) { g_win = v < 1 ? 1 : v > 256 ? 256 : v; logf("command: AFW hole search %ld render pixels", g_win); return true; }
    if (sscanf_s(line, "afwlum %d", &v) == 1) { g_lum = v; logf("command: AFW shared exposure %d", v); return true; }
    if (sscanf_s(line, "afwdbg %d", &v) == 1) { g_dbg = v; logf("command: AFW debug %d (1 holes painted: magenta behind objects, cyan outer edge; 2 also tints what came from the previous picture)", v); return true; }
    if (sscanf_s(line, "afwfeather %d", &v) == 1) { g_feather = v < 0 ? 0 : v > 400 ? 400 : v; logf("command: AFW edge blend %ld / 1000 of the eye width", g_feather); return true; }
    if (sscanf_s(line, "afwlog %d", &v) == 1) { g_log = v; logf("command: AFW log %d frames", v); return true; }
    if (sscanf_s(line, "afw %d", &v) == 1)
    {
        load_settings();
        const LONG was = g_on; g_on = v ? 1 : 0;
        if (was != g_on) { akn_afw_reset(); akn_dlss_reset(); }   // each eye's history restarts in the new rhythm
        logf("command: AFW %s (warp mode %ld)", g_on ? "ON - one eye drawn per frame, the other warped" : "off - both eyes drawn every frame", g_mode);
        return true;
    }
    return false;
}
ID3D11Texture2D* akn_afw_warp(ID3D11DeviceContext* c, int eye, ID3D11Texture2D* pic, const float* V, const float* P, float jx, float jy, const float* dV, const float* dP)
{
    if (eye < 0 || eye > 1 || !pic || !V || !P || !dV || !dP) return nullptr;
    ID3D11Device* dev = nullptr; c->GetDevice(&dev); if (!dev) return nullptr;
    const bool built = build(dev); dev->Release();
    if (!built) return nullptr;
    ID3D11Resource* depth = akn_dlss_depth(eye);
    ID3D11ShaderResourceView* depthS = akn_dlss_srv(depth);
    if (!depth || !depthS) { if ((g_fails++ % 300) == 0) logf("AFW: eye %d has no depth yet (DLSS must be on) - the drawn eye goes to both", eye); return nullptr; }
    D3D11_TEXTURE2D_DESC pd{}; pic->GetDesc(&pd);
    D3D11_TEXTURE2D_DESC dd{};
    { ID3D11Texture2D* t = nullptr; if (FAILED(depth->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) || !t) return nullptr; t->GetDesc(&dd); t->Release(); }
    {   // the drawn area = DLSS's input size; the depth buffer can be bigger (its top-left part is used)
        UINT rw = 0, rh = 0; akn_dlss_render_size(eye, rw, rh);
        if (rw && rh && rw <= dd.Width && rh <= dd.Height) { dd.Width = rw; dd.Height = rh; }
    }
    if (!ensure(dd.Width, dd.Height, pd.Width, pd.Height, pd.Format)) return nullptr;
    const int o = eye ^ 1;
    // matrices (row vectors): drawn eye with and without the jitter, the other eye (no jitter)
    const double jxN = 2.0 * jx / (double)dd.Width, jyN = -2.0 * jy / (double)dd.Height;
    double vpJ[16], vpU[16], vpD[16], invJ[16], invD[16], S[16], G[16], H[16];
    vp_of(V, P, 0.0f, 0.0f, vpJ); vp_of(V, P, (float)jxN, (float)jyN, vpU); vp_of(dV, dP, 0.0f, 0.0f, vpD);
    if (!inv4(vpJ, invJ) || !inv4(vpD, invD)) { ++g_fails; return nullptr; }
    mul4(invJ, vpD, S); mul4(invD, vpU, G);
    const bool prevOk = g_realOk[o] && g_n - g_realAt[o] <= 2;   // the other eye was drawn on the last frame
    if (prevOk) mul4(invD, g_realVP[o], H); else memcpy(H, G, sizeof(H));
    if (!prevOk) ++g_noPrev;
    c->CopyResource(g_real[eye], pic);
    float cs[24], cg[48];
    for (int i = 0; i < 16; ++i) cs[i] = (float)S[i];
    for (int i = 0; i < 4; ++i) cs[16 + i] = (float)invJ[i * 4 + 3];   // the inverse's w column
    cs[20] = (float)dd.Width; cs[21] = (float)dd.Height; cs[22] = 1.0f / dd.Width; cs[23] = 1.0f / dd.Height;
    for (int i = 0; i < 16; ++i) { cg[i] = (float)G[i]; cg[16 + i] = (float)H[i]; }
    cg[32] = (float)pd.Width; cg[33] = (float)pd.Height; cg[34] = 1.0f / pd.Width; cg[35] = 1.0f / pd.Height;
    cg[36] = (float)dd.Width; cg[37] = (float)dd.Height; cg[38] = 1.0f / dd.Width; cg[39] = 1.0f / dd.Height;
    cg[40] = dP[2 * 4 + 2]; cg[41] = dP[3 * 4 + 2]; cg[42] = (float)g_win; cg[43] = (float)g_mode;
    cg[44] = prevOk ? 1.0f : 0.0f; cg[45] = (float)g_dbg; cg[46] = (float)g_feather / 1000.0f; cg[47] = 0;
    c->UpdateSubresource(g_cbSplat, 0, nullptr, cs, 0, 0);
    c->UpdateSubresource(g_cbGather, 0, nullptr, cg, 0, 0);
    ID3D11DeviceContext1* c1 = nullptr; ID3DDeviceContextState* prev = nullptr;
    if (FAILED(c->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1)) || !c1) return nullptr;
    c1->SwapDeviceContextState(g_state, &prev);
    Tq* tq = tq_begin(c);
    const UINT clr[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
    c->ClearUnorderedAccessViewUint(g_keyU, clr);
    const bool statNow = (g_n % 30) == 0;
    if (statNow) { const UINT z[4] = {}; c->ClearUnorderedAccessViewUint(g_statsU, z); }
    c->CSSetShader(g_csSplat, nullptr, 0);
    c->CSSetConstantBuffers(0, 1, &g_cbSplat);
    c->CSSetShaderResources(0, 1, &depthS);
    c->CSSetUnorderedAccessViews(0, 1, &g_keyU, nullptr);
    c->Dispatch((dd.Width + 7) / 8, (dd.Height + 7) / 8, 1);
    ID3D11UnorderedAccessView* nu[2] = {}; ID3D11ShaderResourceView* ns[3] = {};
    c->CSSetUnorderedAccessViews(0, 2, nu, nullptr); c->CSSetShaderResources(0, 3, ns);
    ID3D11ShaderResourceView* gs[3] = { g_keyS, g_realS[eye], g_realS[o] };
    ID3D11UnorderedAccessView* gu[2] = { g_outU, g_statsU };
    c->CSSetShader(g_csGather, nullptr, 0);
    c->CSSetConstantBuffers(0, 1, &g_cbGather);
    c->CSSetShaderResources(0, 3, gs);
    c->CSSetUnorderedAccessViews(0, 2, gu, nullptr);
    c->CSSetSamplers(0, 1, &g_lin);
    c->Dispatch((pd.Width + 7) / 8, (pd.Height + 7) / 8, 1);
    c->CSSetUnorderedAccessViews(0, 2, nu, nullptr); c->CSSetShaderResources(0, 3, ns);
    if (tq) { c->End(tq->b); c->End(tq->dj); tq->pending = true; }
    if (statNow && g_statsRb[g_statsI] && !g_statsPend[g_statsI]) { c->CopyResource(g_statsRb[g_statsI], g_stats); g_statsPend[g_statsI] = true; g_statsI = (g_statsI + 1) % 3; }
    stats_collect(c);
    c1->SwapDeviceContextState(prev, nullptr); if (prev) prev->Release(); c1->Release();
    memcpy(g_realVP[eye], vpU, sizeof(vpU)); g_realAt[eye] = g_n; g_realOk[eye] = true;
    if (g_log > 0) { --g_log; logf("AFW frame %llu: drew eye %d, warped into eye %d, previous picture %s, depth %ux%u -> %ux%u", g_n, eye, o, prevOk ? "used" : "not usable", dd.Width, dd.Height, pd.Width, pd.Height); }
    ++g_n; ++g_warps;
    return g_out;
}
const char* akn_afw_diag()
{
    static char s[300];
    if (g_tqN) { g_ms = g_tqSum / g_tqN; g_tqSum = 0; g_tqN = 0; }
    snprintf(s, sizeof(s), "AFW %s mode %ld | warps %ld (no depth %ld, no previous picture %ld) | graphics card ms per warp %.2f | holes behind objects %.1f %%, outer edge %.1f %%, from the previous picture %.1f %%",
             g_on > 0 ? "on" : "off", g_mode, g_warps, g_fails, g_noPrev, g_ms, g_holePct, g_edgePct, g_prevPct);
    return s;
}
