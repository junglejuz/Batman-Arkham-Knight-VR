// EYEAA 2026-10-06 - JJ: "can we turn off DOF while keeping AA?" / "only SMAA?" -> off / soft / SMAA, panel choice.
// The game skips its own SMAA when DepthOfField=False (SHCENSUS). AKVR smooths each eye's finished picture on its way
// from geo-11's eye surface to the headset image (xr.cpp, native path). Shaders: src/shaders/eyeaa.hlsl, compiled by
// tools/build_eyeaa_shaders.bat. SMAA 1x (Jimenez et al., MIT; vendor/smaa) or AKVR's own FXAA-style "soft" pass.
// Input: a copy of the eye in a UNORM view (the bytes are sRGB-encoded); output: the headset's sRGB image through an
// sRGB render-target view, the shaders write linear values (FAIL-STR-033: no double encoding).
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <cmath>
#include <DirectXPackedVector.h>
#include "smaa/AreaTex.h"
#include "smaa/SearchTex.h"
#include "shaders/eyeaa_vs_edge.h"
#include "shaders/eyeaa_ps_edge.h"
#include "shaders/eyeaa_vs_blend.h"
#include "shaders/eyeaa_ps_blend.h"
#include "shaders/eyeaa_vs_neigh.h"
#include "shaders/eyeaa_ps_neigh.h"
#include "shaders/eyeaa_vs_full.h"
#include "shaders/eyeaa_ps_soft.h"
#include "shaders/eyeaa_ps_copy.h"
#include "shaders/eyeaa_ps_glowpre.h"
#include "shaders/eyeaa_ps_glowdown.h"
#include "shaders/eyeaa_ps_glowup.h"
#include "shaders/eyeaa_ps_lumlog.h"
#include "shaders/eyeaa_ps_adapt.h"

bool akvr_dof_off();          // earlyres.cpp
bool akvr_xr_post_ok();       // xr.cpp: POSTOK - real play only (no title / menus / pause / map)
void akvr_hdr_curve_get(bool &on, float &stops);   // hudsplit.cpp: HDRCURVE
int  akvr_dof_live_state();    // earlyres.cpp: DOFLIVE
namespace {
int g_mode = 0;                  // 0 off, 1 soft, 2 SMAA
int g_state = 0;                 // 0 not built, 1 ok, -1 failed
char g_diag[160] = "off";
ID3D11VertexShader *g_vsEdge = nullptr, *g_vsBlend = nullptr, *g_vsNeigh = nullptr, *g_vsFull = nullptr;
ID3D11PixelShader *g_psEdge = nullptr, *g_psBlend = nullptr, *g_psNeigh = nullptr, *g_psSoft = nullptr, *g_psCopy = nullptr;
float g_lift = 0.0f, g_gamma = 1.0f, g_gain = 1.0f;   // GRADING
// MODGLOW 2026-10-06 - JJ: depth of field off during play is 69 -> 84 fps but "very different without the glows" (the
// game's glow lives in the same block). A dual-filter mip-chain glow on the finished eye picture, only while depth of
// field is off: bright parts (soft threshold, linear light) at half size, shrunk 5 more times, then added back up
// level by level (radius = how much the larger, softer levels add), then mixed into the picture before grading.
// The picture is display-referred (8-bit, already tone-mapped), so the threshold stands in for real light brightness.
bool g_glowOn = true; float g_glowInt = 0.6f, g_glowThr = 0.75f, g_glowRad = 0.7f;
ID3D11PixelShader *g_psGlowPre = nullptr, *g_psGlowDown = nullptr, *g_psGlowUp = nullptr;
ID3D11BlendState *g_add = nullptr;
constexpr int kLv = 8;   // MODGLOW2: 8 levels (half .. 1/256) - JJ: "need more radius"
ID3D11Texture2D *g_lv[kLv] = {}; ID3D11ShaderResourceView *g_lvSrv[kLv] = {}; ID3D11RenderTargetView *g_lvRtv[kLv] = {};
UINT g_lvW[kLv] = {}, g_lvH[kLv] = {};
// GAMEGRADE 2026-10-06: the game's colour table (see eyeaa.hlsl game_grade). Copied from the last post step's t1 while
// depth of field is on (akvr_gamegrade_grab, every 0.5 s, read back a frame later so nothing waits), kept in
// akvr_captures\\gamegrade.bin so depth of field "off" from launch still has it; applied only while depth of field is off.
bool g_ggOn = true; float g_ggAmt = 1.0f;
// POSTFX 2026-10-06 (eyeaa.hlsl post()): film curve, auto exposure, edge darkening - stand-ins while depth of field is off.
// Defaults: the curve fitted to JJ's on/off F2 pair; auto exposure neutral at that scene's brightness (ln 0.0253);
// the game's own edge darkening 0.3. Auto exposure is measured on eye 0 only, both eyes use the same value (STR-009).
bool g_curveOn = false; float g_curveP = 1.0f,   // HDRCURVE: the display curve is now an optional extra
     g_curveStops = 0.0f, g_aeAmt = 0.0f, g_aeRange = 2.0f, g_edge = 0.3f;
constexpr float kAeKey = 0.0253f, kAeTime = 1.5f;
int g_eye = 0; bool g_adInit = false; int g_adCur = 0; LARGE_INTEGER g_adLast{};
ID3D11PixelShader *g_psLumLog = nullptr, *g_psAdapt = nullptr;
ID3D11Texture2D *g_lum = nullptr; ID3D11RenderTargetView *g_lumRtv = nullptr; ID3D11ShaderResourceView *g_lumSrv = nullptr;
// CURVE2: the tone table (eyeaa.hlsl tone()), from scratchpad fit of F2 214022 -> 214017
const float kTone[256] = {0.00000f, 0.00301f, 0.00597f, 0.00889f, 0.01177f, 0.01463f, 0.01747f, 0.02031f, 0.02314f, 0.02599f, 0.02886f, 0.03175f, 0.03469f, 0.03767f, 0.04070f, 0.04380f, 0.04697f, 0.05022f, 0.05356f, 0.05700f, 0.06055f, 0.06421f, 0.06796f, 0.07181f, 0.07574f, 0.07977f, 0.08389f, 0.08809f, 0.09238f, 0.09676f, 0.10122f, 0.10577f, 0.11040f, 0.11512f, 0.11991f, 0.12480f, 0.12976f, 0.13480f, 0.13992f, 0.14512f, 0.15040f, 0.15576f, 0.16119f, 0.16673f, 0.17249f, 0.17844f, 0.18460f, 0.19093f, 0.19742f, 0.20408f, 0.21087f, 0.21778f, 0.22481f, 0.23195f, 0.23916f, 0.24646f, 0.25381f, 0.26121f, 0.26864f, 0.27610f, 0.28357f, 0.29102f, 0.29846f, 0.30587f, 0.31324f, 0.32054f, 0.32778f, 0.33493f, 0.34198f, 0.34892f, 0.35574f, 0.36244f, 0.36915f, 0.37591f, 0.38272f, 0.38957f, 0.39646f, 0.40338f, 0.41033f, 0.41730f, 0.42428f, 0.43128f, 0.43829f, 0.44530f, 0.45230f, 0.45930f, 0.46629f, 0.47326f, 0.48020f, 0.48712f, 0.49401f, 0.50086f, 0.50766f, 0.51442f, 0.52113f, 0.52778f, 0.53437f, 0.54089f, 0.54733f, 0.55371f, 0.55999f, 0.56620f, 0.57231f, 0.57832f, 0.58423f, 0.59003f, 0.59572f, 0.60130f, 0.60675f, 0.61208f, 0.61728f, 0.62233f, 0.62725f, 0.63202f, 0.63664f, 0.64110f, 0.64549f, 0.64984f, 0.65414f, 0.65839f, 0.66261f, 0.66678f, 0.67090f, 0.67499f, 0.67903f, 0.68304f, 0.68700f, 0.69092f, 0.69481f, 0.69865f, 0.70246f, 0.70623f, 0.70996f, 0.71365f, 0.71731f, 0.72093f, 0.72451f, 0.72806f, 0.73157f, 0.73505f, 0.73850f, 0.74191f, 0.74529f, 0.74863f, 0.75195f, 0.75523f, 0.75848f, 0.76170f, 0.76489f, 0.76805f, 0.77118f, 0.77428f, 0.77736f, 0.78040f, 0.78342f, 0.78641f, 0.78937f, 0.79231f, 0.79522f, 0.79811f, 0.80097f, 0.80381f, 0.80662f, 0.80941f, 0.81218f, 0.81492f, 0.81764f, 0.82035f, 0.82303f, 0.82568f, 0.82832f, 0.83094f, 0.83354f, 0.83612f, 0.83868f, 0.84123f, 0.84376f, 0.84627f, 0.84876f, 0.85124f, 0.85369f, 0.85611f, 0.85851f, 0.86088f, 0.86323f, 0.86556f, 0.86786f, 0.87014f, 0.87239f, 0.87463f, 0.87684f, 0.87903f, 0.88120f, 0.88335f, 0.88548f, 0.88760f, 0.88969f, 0.89177f, 0.89383f, 0.89587f, 0.89790f, 0.89991f, 0.90191f, 0.90389f, 0.90585f, 0.90781f, 0.90975f, 0.91168f, 0.91360f, 0.91550f, 0.91740f, 0.91928f, 0.92115f, 0.92302f, 0.92488f, 0.92672f, 0.92856f, 0.93040f, 0.93222f, 0.93404f, 0.93586f, 0.93767f, 0.93948f, 0.94128f, 0.94308f, 0.94487f, 0.94666f, 0.94845f, 0.95024f, 0.95203f, 0.95382f, 0.95561f, 0.95740f, 0.95919f, 0.96098f, 0.96278f, 0.96458f, 0.96638f, 0.96818f, 0.96999f, 0.97181f, 0.97363f, 0.97546f, 0.97729f, 0.97913f, 0.98098f, 0.98284f, 0.98470f, 0.98658f, 0.98846f, 0.99035f, 0.99226f, 0.99418f, 0.99610f, 0.99805f, 1.00000f};
ID3D11ShaderResourceView *g_toneSrv = nullptr;
ID3D11Texture2D *g_ad[2] = {}; ID3D11RenderTargetView *g_adRtv[2] = {}; ID3D11ShaderResourceView *g_adSrv[2] = {};
SRWLOCK g_ggLock = SRWLOCK_INIT;
std::vector<float> g_ggData; UINT g_ggN = 0; bool g_ggDirty = false; bool g_ggLoaded = false;
char g_ggDiag[160] = "not copied yet: switch depth of field on for a moment";
long g_ggGrabs = 0; ULONGLONG g_ggAt = 0;
volatile long g_ggVer = 0;
int g_dofOnSceneFmt = 0;   // SCENEFMT   // GAMELOOK: bumped whenever the table data changes (grab or file)
ID3D11Texture2D *g_ggTex = nullptr; ID3D11ShaderResourceView *g_ggSrv = nullptr; UINT g_ggTexN = 0;
ID3D11SamplerState *g_linear = nullptr, *g_point = nullptr;
ID3D11Buffer *g_cb = nullptr;
ID3D11ShaderResourceView *g_areaSrv = nullptr, *g_searchSrv = nullptr;
// per size
UINT g_w = 0, g_h = 0; DXGI_FORMAT g_srcFam = DXGI_FORMAT_UNKNOWN;
ID3D11Texture2D *g_color = nullptr, *g_edges = nullptr, *g_blend = nullptr;
ID3D11ShaderResourceView *g_colorSrv = nullptr, *g_edgesSrv = nullptr, *g_blendSrv = nullptr;
ID3D11RenderTargetView *g_edgesRtv = nullptr, *g_blendRtv = nullptr;
struct DstView { ID3D11Texture2D *tex; ID3D11RenderTargetView *rtv; };
std::vector<DstView> g_dst;
long g_applied = 0;

template <class T> void rel(T *&p) { if (p) { p->Release(); p = nullptr; } }

bool build(ID3D11Device *d) {
  if (g_state) return g_state == 1;
  g_state = -1;
  bool ok = SUCCEEDED(d->CreateVertexShader(g_eyeaa_vs_edge, sizeof(g_eyeaa_vs_edge), nullptr, &g_vsEdge)) &&
            SUCCEEDED(d->CreatePixelShader(g_eyeaa_ps_edge, sizeof(g_eyeaa_ps_edge), nullptr, &g_psEdge)) &&
            SUCCEEDED(d->CreateVertexShader(g_eyeaa_vs_blend, sizeof(g_eyeaa_vs_blend), nullptr, &g_vsBlend)) &&
            SUCCEEDED(d->CreatePixelShader(g_eyeaa_ps_blend, sizeof(g_eyeaa_ps_blend), nullptr, &g_psBlend)) &&
            SUCCEEDED(d->CreateVertexShader(g_eyeaa_vs_neigh, sizeof(g_eyeaa_vs_neigh), nullptr, &g_vsNeigh)) &&
            SUCCEEDED(d->CreatePixelShader(g_eyeaa_ps_neigh, sizeof(g_eyeaa_ps_neigh), nullptr, &g_psNeigh)) &&
            SUCCEEDED(d->CreateVertexShader(g_eyeaa_vs_full, sizeof(g_eyeaa_vs_full), nullptr, &g_vsFull)) &&
            SUCCEEDED(d->CreatePixelShader(g_eyeaa_ps_soft, sizeof(g_eyeaa_ps_soft), nullptr, &g_psSoft)) &&
            SUCCEEDED(d->CreatePixelShader(g_eyeaa_ps_copy, sizeof(g_eyeaa_ps_copy), nullptr, &g_psCopy)) &&
            SUCCEEDED(d->CreatePixelShader(g_eyeaa_ps_glowpre, sizeof(g_eyeaa_ps_glowpre), nullptr, &g_psGlowPre)) &&
            SUCCEEDED(d->CreatePixelShader(g_eyeaa_ps_glowdown, sizeof(g_eyeaa_ps_glowdown), nullptr, &g_psGlowDown)) &&
            SUCCEEDED(d->CreatePixelShader(g_eyeaa_ps_glowup, sizeof(g_eyeaa_ps_glowup), nullptr, &g_psGlowUp)) &&
            SUCCEEDED(d->CreatePixelShader(g_eyeaa_ps_lumlog, sizeof(g_eyeaa_ps_lumlog), nullptr, &g_psLumLog)) &&
            SUCCEEDED(d->CreatePixelShader(g_eyeaa_ps_adapt, sizeof(g_eyeaa_ps_adapt), nullptr, &g_psAdapt));
  if (!ok) { snprintf(g_diag, sizeof(g_diag), "could not make its shaders"); return false; }
  D3D11_SAMPLER_DESC sd{};
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  ok = SUCCEEDED(d->CreateSamplerState(&sd, &g_linear));
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  ok = ok && SUCCEEDED(d->CreateSamplerState(&sd, &g_point));
  D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 112; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  ok = ok && SUCCEEDED(d->CreateBuffer(&bd, nullptr, &g_cb));
  auto lut = [&](UINT w, UINT h, DXGI_FORMAT f, const void *data, UINT pitch, ID3D11ShaderResourceView **srv) {
    D3D11_TEXTURE2D_DESC td{}; td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = f;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sr{data, pitch, 0};
    ID3D11Texture2D *t = nullptr;
    bool r = SUCCEEDED(d->CreateTexture2D(&td, &sr, &t)) && SUCCEEDED(d->CreateShaderResourceView(t, nullptr, srv));
    rel(t);
    return r;
  };
  ok = ok && lut(AREATEX_WIDTH, AREATEX_HEIGHT, DXGI_FORMAT_R8G8_UNORM, areaTexBytes, AREATEX_PITCH, &g_areaSrv);
  ok = ok && lut(SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, DXGI_FORMAT_R8_UNORM, searchTexBytes, SEARCHTEX_PITCH, &g_searchSrv);
  {   // POSTFX: auto exposure targets (256x256 ln brightness with mips, two 1x1 adapted values)
    D3D11_TEXTURE2D_DESC td{}; td.Width = td.Height = 256; td.MipLevels = 9; td.ArraySize = 1; td.Format = DXGI_FORMAT_R16_FLOAT;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ok = ok && SUCCEEDED(d->CreateTexture2D(&td, nullptr, &g_lum)) && SUCCEEDED(d->CreateShaderResourceView(g_lum, nullptr, &g_lumSrv));
    D3D11_RENDER_TARGET_VIEW_DESC rv{}; rv.Format = td.Format; rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D; rv.Texture2D.MipSlice = 0;
    ok = ok && SUCCEEDED(d->CreateRenderTargetView(g_lum, &rv, &g_lumRtv));
    td.Width = td.Height = 1; td.MipLevels = 1; td.Format = DXGI_FORMAT_R32_FLOAT; td.MiscFlags = 0;
    for (int i = 0; i < 2 && ok; ++i)
      ok = SUCCEEDED(d->CreateTexture2D(&td, nullptr, &g_ad[i])) && SUCCEEDED(d->CreateShaderResourceView(g_ad[i], nullptr, &g_adSrv[i])) &&
           SUCCEEDED(d->CreateRenderTargetView(g_ad[i], nullptr, &g_adRtv[i]));
  }
  {   // CURVE2: tone table texture
    uint16_t hv[256]; for (int i = 0; i < 256; ++i) hv[i] = DirectX::PackedVector::XMConvertFloatToHalf(kTone[i]);
    D3D11_TEXTURE2D_DESC td{}; td.Width = 256; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R16_FLOAT;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sr{hv, 512, 0}; ID3D11Texture2D *t = nullptr;
    ok = ok && SUCCEEDED(d->CreateTexture2D(&td, &sr, &t)) && SUCCEEDED(d->CreateShaderResourceView(t, nullptr, &g_toneSrv));
    rel(t);
  }
  {   // MODGLOW: additive blend for the up passes
    D3D11_BLEND_DESC bl{}; bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = bl.RenderTarget[0].DestBlend = D3D11_BLEND_ONE; bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE; bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ok = ok && SUCCEEDED(d->CreateBlendState(&bl, &g_add));
  }
  if (!ok) { snprintf(g_diag, sizeof(g_diag), "could not make its samplers / tables"); return false; }
  g_state = 1;
  return true;
}

// the typeless family and the UNORM view of the eye's format; false = not an 8-bit RGBA/BGRA eye
bool families(DXGI_FORMAT f, DXGI_FORMAT &fam, DXGI_FORMAT &unorm) {
  if (f >= DXGI_FORMAT_R8G8B8A8_TYPELESS && f <= DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) { fam = DXGI_FORMAT_R8G8B8A8_TYPELESS; unorm = DXGI_FORMAT_R8G8B8A8_UNORM; return true; }
  if (f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_TYPELESS || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) { fam = DXGI_FORMAT_B8G8R8A8_TYPELESS; unorm = DXGI_FORMAT_B8G8R8A8_UNORM; return true; }
  return false;
}

bool size_targets(ID3D11Device *d, UINT w, UINT h, DXGI_FORMAT srcFmt) {
  DXGI_FORMAT fam, unorm;
  if (!families(srcFmt, fam, unorm)) { snprintf(g_diag, sizeof(g_diag), "eye picture format %d not supported", (int)srcFmt); return false; }
  if (g_color && g_w == w && g_h == h && g_srcFam == fam) return true;
  rel(g_colorSrv); rel(g_color); rel(g_edgesSrv); rel(g_edgesRtv); rel(g_edges); rel(g_blendSrv); rel(g_blendRtv); rel(g_blend);
  for (int i = 0; i < kLv; ++i) { rel(g_lvSrv[i]); rel(g_lvRtv[i]); rel(g_lv[i]); }
  g_w = w; g_h = h; g_srcFam = fam;
  D3D11_TEXTURE2D_DESC td{}; td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.Format = fam; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  bool ok = SUCCEEDED(d->CreateTexture2D(&td, nullptr, &g_color));
  D3D11_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = unorm; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
  ok = ok && SUCCEEDED(d->CreateShaderResourceView(g_color, &sv, &g_colorSrv));
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  td.Format = DXGI_FORMAT_R8G8_UNORM;
  ok = ok && SUCCEEDED(d->CreateTexture2D(&td, nullptr, &g_edges)) && SUCCEEDED(d->CreateShaderResourceView(g_edges, nullptr, &g_edgesSrv)) &&
       SUCCEEDED(d->CreateRenderTargetView(g_edges, nullptr, &g_edgesRtv));
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  ok = ok && SUCCEEDED(d->CreateTexture2D(&td, nullptr, &g_blend)) && SUCCEEDED(d->CreateShaderResourceView(g_blend, nullptr, &g_blendSrv)) &&
       SUCCEEDED(d->CreateRenderTargetView(g_blend, nullptr, &g_blendRtv));
  {   // MODGLOW levels: half, quarter ... 1/64 size, float colour
    UINT lw = w, lh = h;
    for (int i = 0; i < kLv && ok; ++i) {
      lw = (lw + 1) / 2; lh = (lh + 1) / 2; if (!lw) lw = 1; if (!lh) lh = 1;
      g_lvW[i] = lw; g_lvH[i] = lh;
      D3D11_TEXTURE2D_DESC ld = td; ld.Width = lw; ld.Height = lh; ld.Format = DXGI_FORMAT_R11G11B10_FLOAT;
      ok = SUCCEEDED(d->CreateTexture2D(&ld, nullptr, &g_lv[i])) && SUCCEEDED(d->CreateShaderResourceView(g_lv[i], nullptr, &g_lvSrv[i])) &&
           SUCCEEDED(d->CreateRenderTargetView(g_lv[i], nullptr, &g_lvRtv[i]));
    }
  }
  if (!ok) { snprintf(g_diag, sizeof(g_diag), "could not make its %ux%u pictures", w, h); g_w = 0; return false; }
  return true;
}

// an sRGB render-target view of a headset image (the shaders write linear values)
ID3D11RenderTargetView *dst_rtv(ID3D11Device *d, ID3D11Texture2D *dst) {
  for (auto &v : g_dst) if (v.tex == dst) return v.rtv;
  D3D11_TEXTURE2D_DESC dd{}; dst->GetDesc(&dd);
  DXGI_FORMAT f = dd.Format;
  if (f == DXGI_FORMAT_R8G8B8A8_TYPELESS) f = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  if (f == DXGI_FORMAT_B8G8R8A8_TYPELESS) f = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
  if (f != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && f != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
    snprintf(g_diag, sizeof(g_diag), "headset image format %d is not sRGB - left as is", (int)dd.Format);
    return nullptr;
  }
  D3D11_RENDER_TARGET_VIEW_DESC rd{}; rd.Format = f; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
  ID3D11RenderTargetView *r = nullptr;
  if (FAILED(d->CreateRenderTargetView(dst, &rd, &r))) { snprintf(g_diag, sizeof(g_diag), "headset image cannot be drawn into"); return nullptr; }
  if (g_dst.size() > 32) { for (auto &v : g_dst) rel(v.rtv); g_dst.clear(); }   // swapchains were remade
  g_dst.push_back({dst, r});
  return r;
}
} // namespace

int  akvr_eyeaa_mode() { return g_mode; }
void akvr_eyeaa_mode_set(int m) { g_mode = m < 0 ? 0 : (m > 2 ? 2 : m); if (!g_mode) snprintf(g_diag, sizeof(g_diag), "off"); }
const char *akvr_eyeaa_diag() { return g_diag; }
void akvr_eyeaa_forget_images() { for (auto &v : g_dst) rel(v.rtv); g_dst.clear(); }   // call when swapchains are destroyed

// Smooth one eye: src box -> dst (whole image). False = nothing done (the caller copies as before).
// GAMEGRADE: file next to the captures (akvr_captures\gamegrade.bin): "AKLUT1", n, n*n*n RGBA floats
static bool gg_path(wchar_t *path) {
  const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  wchar_t *slash = n ? wcsrchr(path, L'\\') : nullptr;
  if (!slash) return false;
  wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"akvr_captures");
  CreateDirectoryW(path, nullptr);
  wcscat_s(path, MAX_PATH, L"\\gamegrade.bin");
  return true;
}
static void gg_save(const std::vector<float> &v, UINT n) {
  wchar_t path[MAX_PATH]; if (!gg_path(path)) return;
  HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  DWORD w = 0; WriteFile(f, "AKLUT1", 6, &w, nullptr); WriteFile(f, &n, 4, &w, nullptr);
  WriteFile(f, v.data(), (DWORD)(v.size() * sizeof(float)), &w, nullptr);
  CloseHandle(f);
}
static void gg_load() {
  wchar_t path[MAX_PATH]; if (!gg_path(path)) return;
  HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  char tag[6] = {}; UINT n = 0; DWORD r = 0;
  if (ReadFile(f, tag, 6, &r, nullptr) && r == 6 && !memcmp(tag, "AKLUT1", 6) && ReadFile(f, &n, 4, &r, nullptr) && r == 4 && n >= 2 && n <= 32) {
    std::vector<float> v((size_t)n * n * n * 4);
    if (ReadFile(f, v.data(), (DWORD)(v.size() * sizeof(float)), &r, nullptr) && r == v.size() * sizeof(float)) {
      AcquireSRWLockExclusive(&g_ggLock);
      if (!g_ggN) { g_ggData.swap(v); g_ggN = n; g_ggDirty = true; ++g_ggVer; snprintf(g_ggDiag, sizeof(g_ggDiag), "%ux%ux%u table from the saved copy (depth of field not on yet this session)", n, n, n); }
      ReleaseSRWLockExclusive(&g_ggLock);
    }
  }
  CloseHandle(f);
}
// our own copy as a texture, rebuilt when the data changes
static bool gg_ready(ID3D11Device *d, ID3D11DeviceContext *c) {
  if (!g_ggLoaded) { g_ggLoaded = true; gg_load(); }
  AcquireSRWLockExclusive(&g_ggLock);
  if (g_ggDirty && g_ggN) {
    const UINT n = g_ggN;
    if (g_ggTexN != n) {
      rel(g_ggSrv); rel(g_ggTex); g_ggTexN = 0;
      D3D11_TEXTURE2D_DESC td{}; td.Width = n * n; td.Height = n; td.MipLevels = 1; td.ArraySize = 1;
      td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      if (SUCCEEDED(d->CreateTexture2D(&td, nullptr, &g_ggTex)) && SUCCEEDED(d->CreateShaderResourceView(g_ggTex, nullptr, &g_ggSrv))) g_ggTexN = n;
    }
    if (g_ggTexN == n) {
      std::vector<uint16_t> h(g_ggData.size());
      for (size_t i = 0; i < h.size(); ++i) h[i] = DirectX::PackedVector::XMConvertFloatToHalf(g_ggData[i]);
      c->UpdateSubresource(g_ggTex, 0, nullptr, h.data(), n * n * 8, 0);
    }
    g_ggDirty = false;
  }
  const bool ok = g_ggSrv && g_ggTexN;
  ReleaseSRWLockExclusive(&g_ggLock);
  return ok;
}

// GRADEFIT 2026-10-07 - JJ: "it appears to be the colour grade which is clipping the dynamic range". The game's colour
// table is strong (neutral 0.67 -> 0.86 / 0.99 / 0.94; green and blue reach 1.0 from ~0.7). The film exposure 2.51 was
// fitted to the DoF-on picture, which already contains the table, so the table brightened a second time and clipped.
// Fitted WITH the table (trilinear, F2 214022 -> 214017): exposure 1.87 matches the game's levels and colour (mean RGB
// .136 .206 .229 vs .130 .206 .227, 0.2% clipped vs the game's 0.4%). The weight below moves the film exposure and the
// auto-exposure neutral point between the two with the colour grade's strength.
float akvr_gamegrade_weight() { if (!g_ggLoaded) { g_ggLoaded = true; gg_load(); } return (g_ggOn && g_ggN) ? g_ggAmt : 0.0f; }
// GAMELOOK: a copy of the table for the HDR pass (hudsplit.cpp), only when it changed since `ver`
bool akvr_gamegrade_snapshot(std::vector<float> &out, UINT &n, long &ver)
{
  if (!g_ggLoaded) { g_ggLoaded = true; gg_load(); }
  if (ver == g_ggVer) return false;
  AcquireSRWLockExclusive(&g_ggLock);
  const bool ok = g_ggN > 0;
  if (ok) { out = g_ggData; n = g_ggN; ver = g_ggVer; }
  ReleaseSRWLockExclusive(&g_ggLock);
  return ok;
}
static float ae_key() {   // AUTO EXPOSURE neutral point = that scene's log-average brightness at the AE measuring point
  bool hOn; float hs; akvr_hdr_curve_get(hOn, hs);
  if (!hOn) return kAeKey;   // the game's clipped picture
  const float w = akvr_gamegrade_weight();
  return 0.0295f + (0.0226f - 0.0295f) * w;   // after the film curve at exposure 2.51 (no table) .. 1.87 (table)
}

bool akvr_eyeaa_apply(ID3D11Device *d, ID3D11DeviceContext *c, ID3D11Texture2D *src, const D3D11_BOX &box, ID3D11Texture2D *dst) {
  const bool postOk = akvr_xr_post_ok();   // POSTOK: every post effect below is off outside real play
  const bool dofOffNow = postOk && (akvr_dof_off() || akvr_dof_live_state() == 0);
  // GRADEDOF 2026-10-07 - JJ: "Make the three grading sliders only work when the depth of field is turned off."
  // (Also fixes POSTOK: gamma and gain had escaped the in-play check - the && only bound to lift.)
  const bool graded = dofOffNow && (fabsf(g_lift) > 0.001f || fabsf(g_gamma - 1.0f) > 0.001f || fabsf(g_gain - 1.0f) > 0.001f);   // GRADING
  bool hdrOn0 = false; { float hs0; akvr_hdr_curve_get(hdrOn0, hs0); }
  // NOSTANDIN 2026-10-07 - JJ: ticking "the game's look" showed the effect "for a split second" and then it looked the
  // same again. TONETEST (akvr_captures	onetest.txt): the full-range pass ran every frame after the tick - but with the
  // switch OFF these after-the-fact stand-ins (glow, colour table, curve, auto exposure, edge darkening on the finished
  // picture, all rejected earlier) quietly came back, so off and on looked alike apart from the swap. Retired: they never
  // run now; off = the game's own plain picture. Glow, table and edge live only in the full-range pass.
  (void)hdrOn0;
  const bool glow = false;   // MODGLOW (GLOWFIRST: built in the full-range pass instead)
  bool hdrOn = false; { float hs; akvr_hdr_curve_get(hdrOn, hs); }
  // GAMELOOK: with the full-range pass on, the table, curve and edge darkening are done THERE (in float, before the
  // picture is quantised); doing them again here on the finished 8-bit picture clipped and posterised (JJ).
  const bool gg = false;   // GAMEGRADE: the copied table is applied in the full-range pass only (NOSTANDIN)
  (void)hdrOn;
  const bool curve = false, ae = false, edge = false;   // POSTFX: retired (NOSTANDIN)
  const int eye = g_eye; g_eye = 0;
  if ((!g_mode && !graded && !glow && !gg && !curve && !ae && !edge) || !d || !c || !src || !dst || !build(d)) return false;
  D3D11_TEXTURE2D_DESC sdsc{}; src->GetDesc(&sdsc);
  if (sdsc.SampleDesc.Count != 1) { snprintf(g_diag, sizeof(g_diag), "eye picture is multisampled - left as is"); return false; }
  const UINT w = box.right - box.left, h = box.bottom - box.top;
  if (!size_targets(d, w, h, sdsc.Format)) return false;
  ID3D11RenderTargetView *out = dst_rtv(d, dst);
  if (!out) return false;

  // ---- save what we touch ----
  ID3D11RenderTargetView *oRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {}; ID3D11DepthStencilView *oDsv = nullptr;
  c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oRtv, &oDsv);
  UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
  D3D11_VIEWPORT ovp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
  c->RSGetViewports(&nvp, ovp);
  ID3D11VertexShader *oVS = nullptr; ID3D11PixelShader *oPS = nullptr;
  c->VSGetShader(&oVS, nullptr, nullptr); c->PSGetShader(&oPS, nullptr, nullptr);
  ID3D11ShaderResourceView *oSrv[5] = {}; c->PSGetShaderResources(0, 5, oSrv);
  ID3D11SamplerState *oSmp[2] = {}; c->PSGetSamplers(0, 2, oSmp);
  ID3D11Buffer *oVcb = nullptr, *oPcb = nullptr; c->VSGetConstantBuffers(0, 1, &oVcb); c->PSGetConstantBuffers(0, 1, &oPcb);
  ID3D11InputLayout *oIL = nullptr; c->IAGetInputLayout(&oIL);
  D3D11_PRIMITIVE_TOPOLOGY oTop; c->IAGetPrimitiveTopology(&oTop);
  ID3D11BlendState *oBS = nullptr; FLOAT oBF[4]; UINT oBM = 0; c->OMGetBlendState(&oBS, oBF, &oBM);
  ID3D11DepthStencilState *oDS = nullptr; UINT oRef = 0; c->OMGetDepthStencilState(&oDS, &oRef);
  ID3D11RasterizerState *oRS = nullptr; c->RSGetState(&oRS);

  // ---- passes ----
  c->CopySubresourceRegion(g_color, 0, 0, 0, 0, src, 0, &box);
  // GRADEDOF2 2026-10-07 - JJ: "Grading tools are still affecting everything when you've got depth of field on." The
  // edge-smoothing pass (SMAA, on) runs whatever the grading decision, and its final step always applied lift/gamma/
  // gain; `graded` only decided whether to run at all. Neutral values unless grading applies now.
  float rt[28] = {1.0f / w, 1.0f / h, (float)w, (float)h, graded ? g_lift : 0.0f, graded ? g_gamma : 1.0f, graded ? g_gain : 1.0f, 0.0f,
                  glow ? g_glowInt : 0.0f, g_glowThr, 0.5f, g_glowRad, 1.0f / w, 1.0f / h, 0.0f, 0.0f,
                  gg ? g_ggAmt : 0.0f, (float)g_ggTexN, 0.0f, 0.0f,
                  curve ? g_curveP : 0.0f, 0.0f, curve ? exp2f(g_curveStops) : 1.0f, edge ? g_edge : 0.0f,
                  ae ? g_aeAmt : 0.0f, logf(ae_key()), g_aeRange * 0.693147f, 1.0f};
  {   // MODGLOW2: level k reaches the picture with weight radius^k; divide by the sum so radius changes the spread,
      // not the brightness (x3 keeps the 0.7 look the same as before)
    float sum = 0.0f, wgt = 1.0f;
    for (int i = 0; i < kLv; ++i) { sum += wgt; wgt *= g_glowRad; }
    rt[14] = 3.0f / sum;
  }
  c->UpdateSubresource(g_cb, 0, nullptr, rt, 0, 0);
  c->IASetInputLayout(nullptr);
  c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  c->OMSetBlendState(nullptr, nullptr, 0xffffffff);
  c->OMSetDepthStencilState(nullptr, 0);
  c->RSSetState(nullptr);
  D3D11_VIEWPORT vp{0, 0, (float)w, (float)h, 0, 1};
  c->RSSetViewports(1, &vp);
  ID3D11SamplerState *smp[2] = {g_linear, g_point};
  c->PSSetSamplers(0, 2, smp);
  c->VSSetConstantBuffers(0, 1, &g_cb);
  c->PSSetConstantBuffers(0, 1, &g_cb);
  ID3D11ShaderResourceView *none7[10] = {};
  if (ae && eye == 0) {   // POSTFX: measure (eye 0 only) and ease the shared value
    LARGE_INTEGER now, fq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
    const float dt = g_adInit ? (float)(now.QuadPart - g_adLast.QuadPart) / (float)fq.QuadPart : 0.0f;
    g_adLast = now;
    rt[27] = g_adInit ? 1.0f - expf(-(dt > 0.5f ? 0.5f : dt) / kAeTime) : 1.0f;
    g_adInit = true;
    c->UpdateSubresource(g_cb, 0, nullptr, rt, 0, 0);
    c->VSSetShader(g_vsFull, nullptr, 0);
    D3D11_VIEWPORT lv{0, 0, 256, 256, 0, 1}; c->RSSetViewports(1, &lv);
    c->OMSetRenderTargets(1, &g_lumRtv, nullptr);
    c->PSSetShader(g_psLumLog, nullptr, 0);
    c->PSSetShaderResources(0, 1, &g_colorSrv);
    c->Draw(3, 0);
    c->OMSetRenderTargets(0, nullptr, nullptr);
    c->PSSetShaderResources(0, 9, none7);
    c->GenerateMips(g_lumSrv);
    const int nx = 1 - g_adCur;
    D3D11_VIEWPORT av{0, 0, 1, 1, 0, 1}; c->RSSetViewports(1, &av);
    c->OMSetRenderTargets(1, &g_adRtv[nx], nullptr);
    c->PSSetShader(g_psAdapt, nullptr, 0);
    c->PSSetShaderResources(6, 1, &g_lumSrv);
    c->PSSetShaderResources(8, 1, &g_adSrv[g_adCur]);
    c->Draw(3, 0);
    c->OMSetRenderTargets(0, nullptr, nullptr);
    c->PSSetShaderResources(0, 9, none7);
    g_adCur = nx;
    c->RSSetViewports(1, &vp);
  }
  if (ae && !g_adInit) rt[24] = 0.0f;   // nothing measured yet
  c->UpdateSubresource(g_cb, 0, nullptr, rt, 0, 0);
  if (ae) c->PSSetShaderResources(8, 1, &g_adSrv[g_adCur]);
  if (gg) c->PSSetShaderResources(7, 1, &g_ggSrv);   // (re-bound: the measuring passes cleared it)
  if (curve) c->PSSetShaderResources(9, 1, &g_toneSrv);   // CURVE2
  if (gg) c->PSSetShaderResources(7, 1, &g_ggSrv);   // GAMEGRADE (unbound with the rest at the end)
  if (glow) {   // MODGLOW
    auto pass = [&](int dstLv, ID3D11PixelShader *ps, UINT srcW, UINT srcH, ID3D11ShaderResourceView *srcSrv, int slot) {
      rt[12] = 1.0f / srcW; rt[13] = 1.0f / srcH;
      c->UpdateSubresource(g_cb, 0, nullptr, rt, 0, 0);
      D3D11_VIEWPORT lv{0, 0, (float)g_lvW[dstLv], (float)g_lvH[dstLv], 0, 1};
      c->RSSetViewports(1, &lv);
      c->OMSetRenderTargets(1, &g_lvRtv[dstLv], nullptr);
      c->PSSetShader(ps, nullptr, 0);
      c->PSSetShaderResources(slot, 1, &srcSrv);
      c->Draw(3, 0);
      c->PSSetShaderResources(0, 7, none7);
      c->OMSetRenderTargets(0, nullptr, nullptr);
    };
    c->VSSetShader(g_vsFull, nullptr, 0);
    pass(0, g_psGlowPre, w, h, g_colorSrv, 0);
    for (int i = 1; i < kLv; ++i) pass(i, g_psGlowDown, g_lvW[i - 1], g_lvH[i - 1], g_lvSrv[i - 1], 6);
    c->OMSetBlendState(g_add, nullptr, 0xffffffff);
    for (int i = kLv - 2; i >= 0; --i) pass(i, g_psGlowUp, g_lvW[i + 1], g_lvH[i + 1], g_lvSrv[i + 1], 6);
    c->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    rt[12] = 1.0f / w; rt[13] = 1.0f / h;
    c->UpdateSubresource(g_cb, 0, nullptr, rt, 0, 0);
    c->RSSetViewports(1, &vp);
    c->PSSetShaderResources(5, 1, &g_lvSrv[0]);   // read by the final pass (with_glow)
  }
  ID3D11ShaderResourceView *none[5] = {};
  if (g_mode == 2) {
    const FLOAT zero[4] = {0, 0, 0, 0};
    c->ClearRenderTargetView(g_edgesRtv, zero);
    c->OMSetRenderTargets(1, &g_edgesRtv, nullptr);
    c->VSSetShader(g_vsEdge, nullptr, 0); c->PSSetShader(g_psEdge, nullptr, 0);
    c->PSSetShaderResources(0, 1, &g_colorSrv);
    c->Draw(3, 0);
    c->PSSetShaderResources(0, 5, none);
    c->ClearRenderTargetView(g_blendRtv, zero);
    c->OMSetRenderTargets(1, &g_blendRtv, nullptr);
    c->VSSetShader(g_vsBlend, nullptr, 0); c->PSSetShader(g_psBlend, nullptr, 0);
    ID3D11ShaderResourceView *b[5] = {nullptr, g_edgesSrv, nullptr, g_areaSrv, g_searchSrv};
    c->PSSetShaderResources(0, 5, b);
    c->Draw(3, 0);
    c->PSSetShaderResources(0, 5, none);
    c->OMSetRenderTargets(1, &out, nullptr);
    c->VSSetShader(g_vsNeigh, nullptr, 0); c->PSSetShader(g_psNeigh, nullptr, 0);
    ID3D11ShaderResourceView *n[5] = {g_colorSrv, nullptr, g_blendSrv, nullptr, nullptr};
    c->PSSetShaderResources(0, 5, n);
    c->Draw(3, 0);
  } else {
    c->OMSetRenderTargets(1, &out, nullptr);
    c->VSSetShader(g_vsFull, nullptr, 0); c->PSSetShader(g_mode == 1 ? g_psSoft : g_psCopy, nullptr, 0);   // mode 0 = grading only
    c->PSSetShaderResources(0, 1, &g_colorSrv);
    c->Draw(3, 0);
  }
  c->PSSetShaderResources(0, 10, none7);

  // ---- restore ----
  c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oRtv, oDsv);
  if (nvp) c->RSSetViewports(nvp, ovp);
  c->VSSetShader(oVS, nullptr, 0); c->PSSetShader(oPS, nullptr, 0);
  c->PSSetShaderResources(0, 5, oSrv);
  c->PSSetSamplers(0, 2, oSmp);
  c->VSSetConstantBuffers(0, 1, &oVcb); c->PSSetConstantBuffers(0, 1, &oPcb);
  c->IASetInputLayout(oIL); c->IASetPrimitiveTopology(oTop);
  c->OMSetBlendState(oBS, oBF, oBM); c->OMSetDepthStencilState(oDS, oRef); c->RSSetState(oRS);
  for (auto *&r : oRtv) rel(r);
  rel(oDsv); rel(oVS); rel(oPS); for (auto *&s : oSrv) rel(s); for (auto *&s : oSmp) rel(s);
  rel(oVcb); rel(oPcb); rel(oIL); rel(oBS); rel(oDS); rel(oRS);
  ++g_applied;
  snprintf(g_diag, sizeof(g_diag), "%s%s%s%s%s%s%s, %ux%u per eye, %ld eye pictures", g_mode == 2 ? "SMAA on" : (g_mode == 1 ? "soft on" : "smoothing off"),
           graded ? " + grading" : "", glow ? " + glow" : "", gg ? " + game's colours" : "", curve ? " + film curve" : "",
           ae ? " + auto exposure" : "", edge ? " + edge darkening" : "", w, h, g_applied);
  return true;
}

// GRADING 2026-10-06 - JJ: lift / gamma / gain on the world picture (the HUD is its own layer and stays as drawn)
void akvr_grade_set(float lift, float gamma, float gain)
{
  g_lift = lift < -0.5f ? -0.5f : (lift > 0.5f ? 0.5f : lift);
  g_gamma = gamma < 0.25f ? 0.25f : (gamma > 4.0f ? 4.0f : gamma);
  g_gain = gain < 0.0f ? 0.0f : (gain > 4.0f ? 4.0f : gain);
}
void akvr_grade_get(float &lift, float &gamma, float &gain) { lift = g_lift; gamma = g_gamma; gain = g_gain; }

// MODGLOW settings
void akvr_glow_set(bool on, float intensity, float threshold, float radius)
{
  g_glowOn = on;
  g_glowInt = intensity < 0.0f ? 0.0f : (intensity > 3.0f ? 3.0f : intensity);
  g_glowThr = threshold < 0.3f ? 0.3f : (threshold > 1.0f ? 1.0f : threshold);
  g_glowRad = radius < 0.0f ? 0.0f : (radius > 3.0f ? 3.0f : radius);
}
void akvr_glow_get(bool &on, float &intensity, float &threshold, float &radius) { on = g_glowOn; intensity = g_glowInt; threshold = g_glowThr; radius = g_glowRad; }

// GAMEGRADE: called from the dispatch hook (hudsplit.cpp) when the game's last post step (CS 679d7ed1) is about to run;
// its t1 is the colour table. Copy into a staging texture every 0.5 s, read it back on a later call (no wait).
static float gg_s2l(float v) { return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f); }
void akvr_gamegrade_grab(ID3D11DeviceContext *c) {
  static ID3D11Texture2D *s_stage = nullptr; static UINT s_w = 0, s_h = 0; static DXGI_FORMAT s_fmt = DXGI_FORMAT_UNKNOWN, s_view = DXGI_FORMAT_UNKNOWN;
  static bool s_pending = false; static ULONGLONG s_next = 0, s_saved = 0; static std::vector<float> s_savedData;
  if (!c || c->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
  const ULONGLONG now = GetTickCount64();
  if (s_pending && s_stage) {
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = c->Map(s_stage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
    s_pending = false;
    if (FAILED(hr)) return;
    const UINT n = s_h;
    std::vector<float> v((size_t)n * n * n * 4);
    bool known = true;
    const bool srgb = s_view == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || s_view == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    for (UINT y = 0; y < n && known; ++y) {
      const uint8_t *row = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
      for (UINT x = 0; x < n * n; ++x) {
        float *o = &v[((size_t)y * n * n + x) * 4];
        switch (s_fmt) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
          for (int k = 0; k < 4; ++k) o[k] = row[x * 4 + k] / 255.0f; break;
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
          o[0] = row[x * 4 + 2] / 255.0f; o[1] = row[x * 4 + 1] / 255.0f; o[2] = row[x * 4] / 255.0f; o[3] = 1.0f; break;
        case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS: {
          const uint32_t p = ((const uint32_t *)row)[x];
          o[0] = (p & 1023) / 1023.0f; o[1] = ((p >> 10) & 1023) / 1023.0f; o[2] = ((p >> 20) & 1023) / 1023.0f; o[3] = 1.0f; break; }
        case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS:
          for (int k = 0; k < 4; ++k) o[k] = DirectX::PackedVector::XMConvertHalfToFloat(((const uint16_t *)row)[x * 4 + k]); break;
        case DXGI_FORMAT_R16G16B16A16_UNORM:
          for (int k = 0; k < 4; ++k) o[k] = ((const uint16_t *)row)[x * 4 + k] / 65535.0f; break;
        default: known = false; break;
        }
        if (!known) break;
        if (srgb) for (int k = 0; k < 3; ++k) o[k] = gg_s2l(o[k]);
      }
    }
    c->Unmap(s_stage, 0);
    if (!known) { snprintf(g_ggDiag, sizeof(g_ggDiag), "the game's table is in format %d - not handled yet", (int)s_fmt); return; }
    AcquireSRWLockExclusive(&g_ggLock);
    g_ggData = v; g_ggN = n; g_ggDirty = true; ++g_ggGrabs; g_ggAt = now; ++g_ggVer;
    snprintf(g_ggDiag, sizeof(g_ggDiag), "%ux%ux%u table copied from the game (format %d, %ld copies this session)", n, n, n, (int)s_fmt, g_ggGrabs);
    ReleaseSRWLockExclusive(&g_ggLock);
    float diff = s_savedData.size() == v.size() ? 0.0f : 1.0f;
    if (diff == 0.0f) for (size_t i = 0; i < v.size(); ++i) diff = (std::max)(diff, fabsf(v[i] - s_savedData[i]));
    if (diff > 0.01f && (!s_saved || now - s_saved > 10000)) { gg_save(v, n); s_savedData = v; s_saved = now; }
    return;
  }
  if (now < s_next) return;
  s_next = now + 500;
  {   // SCENEFMT 2026-10-07: what the DoF-ON final step (CS 679d7ed1) reads as its scene (t0) - compare with the DoF-off pass
    ID3D11ShaderResourceView *s0 = nullptr; c->CSGetShaderResources(0, 1, &s0);
    if (s0) { D3D11_SHADER_RESOURCE_VIEW_DESC vd0{}; s0->GetDesc(&vd0); g_dofOnSceneFmt = (int)vd0.Format; s0->Release(); }
  }
  ID3D11ShaderResourceView *srv = nullptr; c->CSGetShaderResources(1, 1, &srv);
  if (!srv) return;
  D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; srv->GetDesc(&vd);
  ID3D11Resource *res = nullptr; srv->GetResource(&res); srv->Release();
  if (!res) return;
  ID3D11Texture2D *tex = nullptr; res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&tex); res->Release();
  if (!tex) return;
  D3D11_TEXTURE2D_DESC td{}; tex->GetDesc(&td);
  if (vd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || td.Height < 2 || td.Height > 32 || td.Width != td.Height * td.Height || td.SampleDesc.Count != 1) {
    snprintf(g_ggDiag, sizeof(g_ggDiag), "the game's table slot holds a %ux%u picture - not a colour table", td.Width, td.Height);
    tex->Release(); return;
  }
  if (!s_stage || s_w != td.Width || s_h != td.Height || s_fmt != td.Format) {
    if (s_stage) { s_stage->Release(); s_stage = nullptr; }
    ID3D11Device *d = nullptr; c->GetDevice(&d);
    D3D11_TEXTURE2D_DESC sd = td; sd.MipLevels = 1; sd.ArraySize = 1; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    if (d) { d->CreateTexture2D(&sd, nullptr, &s_stage); d->Release(); }
    s_w = td.Width; s_h = td.Height; s_fmt = td.Format;
  }
  s_view = vd.Format;
  if (s_stage) {
    D3D11_BOX b{0, 0, 0, td.Width, td.Height, 1};
    c->CopySubresourceRegion(s_stage, 0, 0, 0, 0, tex, D3D11CalcSubresource(vd.Texture2D.MostDetailedMip, 0, td.MipLevels), &b);
    s_pending = true;
  }
  tex->Release();
}
void akvr_gamegrade_set(bool on, float amount) { g_ggOn = on; g_ggAmt = amount < 0.0f ? 0.0f : (amount > 2.0f ? 2.0f : amount); }   // up to 2 (JJ)
void akvr_gamegrade_get(bool &on, float &amount) { on = g_ggOn; amount = g_ggAmt; }
const char *akvr_gamegrade_diag() { return g_ggDiag; }

// POSTFX settings
void akvr_eyeaa_eye(int e) { g_eye = e; }
void akvr_post_set(bool curveOn, float contrast, float stops, float autoExp, float edge)
{
  g_curveOn = curveOn;
  g_curveP = contrast < 0.0f ? 0.0f : (contrast > 1.5f ? 1.5f : contrast);   // CURVE2: now the tone curve's strength
  g_curveStops = stops < -3.0f ? -3.0f : (stops > 3.0f ? 3.0f : stops);
  g_aeAmt = autoExp < 0.0f ? 0.0f : (autoExp > 1.0f ? 1.0f : autoExp);
  g_edge = edge < 0.0f ? 0.0f : (edge > 1.0f ? 1.0f : edge);
}
void akvr_post_get(bool &curveOn, float &contrast, float &stops, float &autoExp, float &edge)
{ curveOn = g_curveOn; contrast = g_curveP; stops = g_curveStops; autoExp = g_aeAmt; edge = g_edge; }
int akvr_dofon_scene_format() { return g_dofOnSceneFmt; }   // SCENEFMT
