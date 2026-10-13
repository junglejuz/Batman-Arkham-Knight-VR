// HUDSPLIT / HUDLAYER 2026-09-28 — the HUD on its own headset layer.
//
// Why: JJ's HUD jitter is the HUD baked into a 45 Hz picture that the headset shows twice and
// re-aims at 90 Hz, so head-locked content hops every other refresh. Shifting it inside the
// picture only added lag (HUDSTEADY, parked). A layer the headset places itself, every refresh,
// cannot hop. Playbook (commit b955f22): 04-ui-and-hud.md "Get UI off the backbuffer and onto a
// layer" and pattern HUD-004 (clear to transparent, separate alpha blend, premultiplied submit,
// kill switch).
//
// Static RE (BatmanAK.exe): every Flash movie is drawn by ONE render-thread function,
// FGFxEngine::RenderUI_RenderThread 0x1405de350 (FGFxRenderUI command, vtable 0x141f99ad8).
// Params (rdx): +0x8 movie count, +0x10 "render to scene colour" flag. Flag clear: drawn straight
// onto the main viewport target - there is no separate HUD image.
//
// Runtime (JJ, 2026-09-28): skipping the call hides the WHOLE HUD. Real-context hooks (below
// geo-11) see none of it; the GAME-FACING context (geo-11's wrapper, layer "game") sees it: one
// immediate context, ~30 draws/frame into the 2888x2860 f28 main target (bind flags 0xa8), plus
// single draws into 32x32 / 96x128 Scaleform filter targets.
//
// HUDLAYER: while the call runs on the game context in steady gameplay, every bind of the main
// target is swapped for our own image H (same size/format, created through the game device),
// cleared to transparent at the first swap; blend states get alpha = ONE / INV_SRC_ALPHA and
// alpha writes on (HUD-004) while H is bound; the game's own target and blend state are put back
// when the call returns. xr.cpp copies H into a headset swapchain and submits it as a quad in
// VIEW space (head-locked, placed by the compositor every refresh) at the HUD distance.
// geo-11's HUD shift is held at 0 while this runs (geo11conv.cpp), so H holds ONE flat HUD.
#include "akn_link.h"
#include "hudsplit.h"
#include "geo11conv.h"
#include "xr.h"   // RETSQUASH: akvr_xr_game_tan
#include "camera.h"   // NEARRAIN: akvr_camera_base_axes
#include "vslayout.h"   // MKBATCH: batched HUD vertex shader layouts
struct ID3D11DeviceContext;
bool akvr_hud_room_all();   // earlyres.cpp: ROOMALL (whole HUD in the room) - BANDOFF
int  akvr_hud_target_points(float* xy, int max, int rtW, int rtH, float* off, float* xf);   // earlyres.cpp: TARGETDEPTH / TARGETSTOCK / TARGETSCALE; off = view depth per point (TARGETDIST)
const char* akvr_marker_dist_diag();   // earlyres.cpp: TARGETDIST
bool akvr_hud_marks_ready();                                                              // earlyres.cpp: MARKFIRST
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <MinHook.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>
#include "../../akvr_native/src/flare_shaders.h"   // FLAREOFF3: Luma's two AK lens flare shaders (generated, credit inside)
#include <d3dcompiler.h>

namespace {
    constexpr uintptr_t kRenderUI = 0x1405de350;
    const unsigned char kSig[] = { 0x48,0x8B,0xC4, 0x48,0x89,0x58,0x20, 0x55, 0x56, 0x57, 0x41,0x54,
                                   0x41,0x55, 0x41,0x56, 0x41,0x57, 0x48,0x83,0xEC,0x50 };
    const char* kLayer[3] = { "game", "real-imm", "real-def" };

    typedef void (*RenderUIFn)(void* engine, void* params);
    RenderUIFn g_orig = nullptr;
    int  g_state = 0;              // 0 not tried, 1 hooked, -1 code differs, -2 hook failed
    int  g_drawState = 0;          // immediate draw counters: 1 ok
    int  g_defState = 0;           // deferred: 1 hooked (separate code), 2 same code as immediate, <0 failed
    int  g_defHooked = 0;
    int  g_gameState = 0;          // game side: 0 waiting for geo-11, 1 hooked, 2 same as real, <0 failed
    int  g_gameHooked = 0;
    char g_gameModule[64] = "";
    ID3D11DeviceContext* g_ctx = nullptr;       // real immediate context (below geo-11)
    ID3D11DeviceContext* g_gameCtx = nullptr;   // game-facing immediate context (geo-11 wrapper)
    volatile DWORD g_scopeTid = 0; // non-zero while RenderUI runs
    DWORD g_presentTid = 0;
    volatile LONG g_calls = 0;
    // Whole-frame census per layer.
    volatile LONG g_cDraw[3] = {}, g_cBind[3] = {}, g_cFinish[3] = {}, g_cExec[3] = {};
    LONG g_rDraw[3] = {}, g_rBind[3] = {}, g_rFinish[3] = {}, g_rExec[3] = {};
    SRWLOCK g_lock = SRWLOCK_INIT;

    // ---- HUDLAYER state (all on the render thread) ----
    bool g_layerWant = true;                    // panel / settings "hudlayer"
    volatile bool g_layerGate = false;          // steady gameplay, from xr.cpp
    // MENUONE 2026-09-29 — JJ: with the HUD hung in the room, the title and save screens showed every text
    // twice (the desktop, geo-11's picture, once). The fix tags menu text pieces differently, so the split
    // put some in the picture and some on the layer, and with the layer room-fixed the two drift apart.
    // Nothing on a menu points at the world: on the main menu the whole UI goes to the layer, no split.
    volatile bool g_layerMenu = false;
    bool g_scopeLayer = false;                  // redirect armed for the running call
    bool g_hBound = false, g_hCleared = false;
    int  g_scopeSubs = 0;
    ID3D11Texture2D* g_H = nullptr;
    ID3D11RenderTargetView* g_Hrtv = nullptr;
    int  g_Hw = 0, g_Hh = 0, g_Hfmt = 0;
    int  g_Herr = 0;                            // 0 ok, 1 texture, 2 view
    int  g_Hreal = -1;                          // H is a real d3d11 texture: -1 not made yet, 0 no, 1 yes
    char g_Hmodule[64] = "";
    ID3D11Texture2D* g_Hsrc = nullptr;          // the REAL texture that holds H's pixels (not owned)
    // MENUCRASH 2026-10-06 - JJ: opening the game's graphics menu (VR, geo-11) crashed: "Rendering thread exception"
    // in d3d11 CreateShaderResourceView called from xr.cpp hud_conv_draw on g_Hsrc. geo-11 rebuilt the real texture
    // behind H (the menu re-applies the display settings) and g_Hsrc, read once, pointed at freed memory. The path
    // it was found by is kept and re-read every frame (hsrc_refresh), so g_Hsrc follows geo-11's current texture.
    uintptr_t g_HsrcOff = 0, g_HsrcSub = 0;    // H+off -> p; texture = p (sub == -1) or *(p + sub)
    uintptr_t g_texVt = 0;                      // a real d3d11 texture's function table
    long g_HsrcMoves = 0, g_HsrcLost = 0;
    char g_Hfound[240] = "";                    // what the unwrap search found (diag)
    UINT g_reqN = 0;                            // the game's last requested targets (restore)
    ID3D11RenderTargetView* g_reqRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* g_reqDsv = nullptr;
    bool g_reqValid = false;
    ID3D11BlendState* g_reqBlend = nullptr;     // the game's last requested blend (restore)
    FLOAT g_reqFactor[4] = { 1, 1, 1, 1 };
    UINT  g_reqMask = 0xffffffff;
    bool  g_reqBlendValid = false;
    struct BV { ID3D11BlendState* orig; ID3D11BlendState* var; };
    BV   g_bv[64]; int g_bvN = 0, g_bvFail = 0, g_bvOther = 0;   // g_bvOther: non-plain blend modes seen
    unsigned g_lastSubFrame = 0;                // g_frame when H last received the HUD
    // HUDSPLIT (2026-09-28, JJ: the reticle and the target-distance element lost their scene depth in
    // the layer). With the fix-patch script's HUDSPLIT edit, the 13 HUD shaders read a switch in cb13:
    // 1 = flat pieces only, 2 = scene-depth pieces only (fix filter 2 / 42), unbound = all. While H is
    // bound, each HUD draw is issued twice: flat pieces into H, scene-depth pieces into the game's own
    // target with the game's own blend, where geo-11 and the fix place them at the depth they point at.
    int  g_splitReady = -1;                     // shader edit installed: -1 unknown, 0 no, 1 yes
    bool g_splitWant = true;
    ID3D11Buffer* g_cbFlat = nullptr;
    ID3D11Buffer* g_cbDepth = nullptr;
    ID3D11Buffer* g_cbAll = nullptr;                   // PSMARK: cb13 = 0, keep everything (marked no-colour draws)
    ID3D11Buffer* g_cb13Orig = nullptr;         // the game's own slot 13 at the start of the call
    ID3D11RenderTargetView* g_subRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    long g_splitDraws = 0, g_splitDrawsRep = 0;
    // RETFLAT band (JJ 2026-09-28: the compass flickered with the split on): pieces whose origin is in the
    // top g_bandPct %% of the view stay flat (cb13[0].y = that line in clip y). 0 = no band.
    // JJ 2026-09-28: 30 is good; above it the reticles stop following depth. 2026-09-29: at 30 the compass split
    // into two pieces when the head moved; 28 fixed it -> fixed at 28, slider removed from the panel.
    float g_bandPct = 28.0f;
    float g_bandSent = -1.0f;
    // EDGEBAND (fix step 1f, 2026-09-29): JJ with the HUD hung in the room (HUDWORLD) - the compass and the
    // gameplay tips near the bottom still moved with the head. All 13 HUD shaders now also keep pieces whose
    // origin is in the bottom g_bottomPct %% on the layer (cb13[1].x = that line's distance below the centre,
    // clip y), and the top band reaches all 13 (it was only in the two filter-42 shaders). 0 = no strip.
    float g_bottomPct = 0.0f;   // EDGEBAND retired the same night (split elements doubled); 0 = no strip
    float g_bottomSent = -1.0f;
    // RETSQUASH: the game frame's tan half-angles for the shader's edge-squash correction (cb13[0].zw).
    bool  g_squashWant = false;   // PANELTIDY 2026-09-28: JJ - never worked in the headset; removed from the panel, always off
    float g_tanSent[2] = { -1.0f, -1.0f };
    // TARGETDEPTH (fix step 1k, 2026-10-02): the on-target parts' points sent to the shaders (cb13 rows 1 / 2)
    bool  g_targetWant = true;
    // TARGETANCHOR 2026-10-02: 0.12 -> 0.15. The reticle's "RB" prompt sits 0.105 from its point (F2: (1765, 302) twips) -
    // at the view edges JJ saw the reticle "separate their elements a little". Every other HUD part is room-marked, so the
    // wider circle only takes pieces of on-target parts.
    float g_targetRadius = 0.15f;            // clip units (the drawn HUD box)
    float g_tgtSent[44] = { -9.0f };   // TARGETMULTI: cb13 rows 1-10; TARGETUP: + row 11
    float g_tgtXf[4] = {};                   // TARGETSCALE: cb13 row 4 sent last frame (kx-1, ky-1, bx, by)
    float g_framePx = 0.0f, g_framePy = 0.0f;   // FRAMEPAIR: the markers' one-frame shift (clip), for the diag
    int   g_tgtN = 0; float g_tgtXY[16] = {};   // TARGETANCHOR: all points (list markers first, single parts last)
    // TARGETMOVE (fix step 1m, 2026-10-02): move the on-target parts by the head turn the game's HUD does not know about
    bool  g_markerHead = true;
    // TARGETFACE (fix step 1q, 2026-10-02): the on-target parts face the eye (cb13 row 3 = tan half-angles, on)
    bool  g_markerFace = true;
    // TARGETUP2: which camera gives the world's up: 0 = the live fields (the frame being drawn), k = the one recorded
    // k-1 Presents back (2 = TARGETUP's first build, which JJ saw "fighting" the roll)
    int   g_markerUpLag = 0;
    int   g_markerLag = -1;                 // the drawn camera: k Presents back; -1 = the pose delay in use - 1
    int   g_markerHudLag = 1;                // TARGETMOVE2: the markers' camera is this many Presents older (0 = off)
    float g_tgtOff[4] = {};
    // PANELTIDY5 2026-10-11: the TARGETTRACE recorder (test tools, F2 targets.csv) removed.
    long g_layerFrames = 0, g_subsRep = 0, g_restores = 0;
    // HUD-004 proof: alpha census of H, read back every ~2 s.
    ID3D11Texture2D* g_stage = nullptr;
    int  g_stageState = 0;                      // 0 idle, 1 copy queued
    unsigned g_stageFrame = 0;
    float g_covAny = -1, g_covSolid = 0, g_covPart = 0;
    // LAYERSHOT 2026-09-29 (JJ: doubled HUD pieces after EDGEBAND): F2 also saves the layer image (slice 0) as a
    // BMP, colour on black, so what went to the layer can be compared with the picture capture.
    wchar_t g_shotPath[MAX_PATH] = L"";
    volatile LONG g_shotWant = 0;
    void save_layer_bmp(const D3D11_MAPPED_SUBRESOURCE& m)
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, g_shotPath, L"wb") != 0 || !f) return;
        const int w = g_Hw, h = g_Hh, rowBytes = w * 3, pad = (4 - rowBytes % 4) % 4;
        const uint32_t img = (uint32_t)((rowBytes + pad) * h);
        uint8_t hd[54] = { 'B', 'M' };
        *(uint32_t*)(hd + 2) = 54 + img; *(uint32_t*)(hd + 10) = 54; *(uint32_t*)(hd + 14) = 40;
        *(int32_t*)(hd + 18) = w; *(int32_t*)(hd + 22) = h; *(uint16_t*)(hd + 26) = 1; *(uint16_t*)(hd + 28) = 24;
        *(uint32_t*)(hd + 34) = img;
        fwrite(hd, 1, 54, f);
        const bool bgra = g_Hfmt >= 87 && g_Hfmt <= 93;
        uint8_t* line = new uint8_t[rowBytes + pad]();
        for (int y = h - 1; y >= 0; --y)
        {
            const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
            for (int x = 0; x < w; ++x)
            {
                const uint8_t* p = row + x * 4;
                line[x * 3 + 0] = bgra ? p[0] : p[2];
                line[x * 3 + 1] = p[1];
                line[x * 3 + 2] = bgra ? p[2] : p[0];
            }
            fwrite(line, 1, rowBytes + pad, f);
        }
        delete[] line;
        fclose(f);
    }

    struct Bind { void* ctx; int layer; int ctxType; int same; void* tex; int w, h, fmt, arr, bindFlags, misc, binds, draws; };
    struct Ctx  { void* ctx; int layer; int type; int same; int bind; int drawsNoBind; int binds; int draws; };
    struct Scope
    {
        int scene, movies; DWORD tid;
        int nb; Bind b[12];
        int nc; Ctx c[8];
        int finishes[3], execs[3], otherDraws, subs;
    };
    Scope g_cur[4]; int g_curN = 0;
    Scope g_rep[4]; int g_repN = 0;
    unsigned g_frame = 0;

    struct Row { unsigned frame; int idx; LONG draw[3], bind[3], fin[3], exe[3]; Scope s; };
    constexpr int kRing = 256;
    Row  g_ring[kRing];
    int  g_ringHead = 0, g_ringCount = 0;

    // VSID 2026-09-28 — JJ: with the split on, the top-right "TARGET DETAIL" panel was doubled: it is drawn
    // by a Scaleform vertex shader that is NOT one of the fix's 13 HUD shaders, so it ignored the cb13
    // switch and was drawn fully in both passes (layer AND picture). Now the second pass only runs while one
    // of the 13 patched shaders is bound. They are recognised when created: 3Dmigoto's shader hash is
    // FNV-1 64 (offset 0) over the original bytecode, the same 16 hex digits as the ShaderFixesDM names.
    const uint64_t kHudVsHash[13] = {
        0x9938094af96353c0ull, 0x3b819a7e86e9d631ull, 0x05154232f7872d0dull, 0x599bd060016570bbull,
        0xfd60f2d764b91756ull, 0xc9b47e60935f3f03ull, 0x4b432a87d072f988ull, 0x54cd897e5ce3b8b3ull,
        0xe12863b9484b4660ull, 0x7d8fcdc225ce3fc7ull, 0xef1c1604346331e2ull, 0x9689d605c06946bfull,
        0x91e2b2225bf3ea04ull };
    SRWLOCK g_vsLock = SRWLOCK_INIT;
    // VSID4 run: 64 of 64 filled (the game creates the 13 many times) - later copies would have lost their depth.
    constexpr int kHudVsCap = 1024;
    void* g_hudVs[kHudVsCap] = {}; int g_hudVsN = 0;   // shader objects of the 13 (several creations possible)
    int8_t g_hudVsIdx[kHudVsCap] = {};                 // MARKREC: which of the 13 (kHudVsHash index)
    bool  g_hudVsFound[13] = {};
    volatile LONG g_vsSeen = 0;
    int   g_vsHooks = 0;
    void* g_curVs = nullptr;                           // VS bound on the game immediate context
    uint64_t fnv64(const void* p, size_t n)
    {
        uint64_t h = 0; const unsigned char* b = (const unsigned char*)p;
        for (size_t i = 0; i < n; ++i) { h *= 0x100000001b3ull; h ^= b[i]; }
        return h;
    }
    // VSID2 2026-09-28 — first run: 22964 creations seen, 0 matched. The only hook sat on the REAL device, which
    // gets geo-11's own (auto-patched / replacement) bytecode, never the game's original. The 13 HUD
    // replacements are recognised instead by the HUDSPLIT drop position l(-10, -10, 0, 1) in their bytecode;
    // the game-side shader object is then paired with the real one geo-11 binds for it (see VSSet below).
    void* g_realHud[64] = {}; int g_realHudN = 0;
    bool has_drop_const(const void* code, SIZE_T len)
    {
        static const uint32_t pat[4] = { 0xC1200000u, 0xC1200000u, 0x00000000u, 0x3F800000u };
        const unsigned char* b = (const unsigned char*)code;
        for (SIZE_T i = 0; i + 16 <= len; i += 4)
            if (memcmp(b + i, pat, 16) == 0) return true;
        return false;
    }
    bool real_is_hud(void* vs)
    {
        if (!vs) return false;
        AcquireSRWLockShared(&g_vsLock);
        bool hit = false;
        for (int i = 0; i < g_realHudN && !hit; ++i) hit = g_realHud[i] == vs;
        ReleaseSRWLockShared(&g_vsLock);
        return hit;
    }
    // game shader object -> patched? learnt from the real VS geo-11 binds while the game's call runs
    struct VsMap { void* game; int hud; };
    VsMap g_vsMap[512]; int g_vsMapN = 0;
    thread_local void* t_realVs = nullptr;
    // VSID3 2026-09-28 — VSID2 run: 64 geo-11 copies found, 0 game shaders paired (geo-11 forwards the game's
    // immediate-context calls later, not inside the call). geo-11 is 3Dmigoto: HackerContext keeps
    // `UINT64 mCurrentVertexShader` (HackerContext.h:204), set synchronously in its VSSetShader. Find that
    // field in the game-facing context object by looking for one of the 13 known hashes right after the game
    // binds a shader inside the HUD call, then read it at every HUD draw.
    int  g_vsHashOff = -1;          // byte offset in the game context object, -1 unknown
    int  g_vsScanTries = 0;
    bool is_hud_hash(uint64_t h) { for (uint64_t k : kHudVsHash) if (h == k) return true; return false; }
    bool seh_u64(const void* p, uint64_t& v) { __try { v = *(const uint64_t*)p; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
    void vs_hash_scan(ID3D11DeviceContext* c)
    {
        if (g_vsHashOff >= 0 || g_vsScanTries > 400) return;
        ++g_vsScanTries;
        for (int off = 8; off < 0x4000; off += 8)
        {
            uint64_t v = 0;
            if (!seh_u64((const char*)c + off, v)) break;
            if (is_hud_hash(v)) { g_vsHashOff = off; return; }
        }
    }
    int cur_vs_hash_hud(ID3D11DeviceContext* c)   // 1 HUD shader, 0 other, -1 unknown
    {
        if (g_vsHashOff < 0) return -1;
        uint64_t v = 0;
        if (!seh_u64((const char*)c + g_vsHashOff, v)) return -1;
        return is_hud_hash(v) ? 1 : 0;
    }
    void vs_map_set(void* game, bool hud)
    {
        if (!game) return;
        AcquireSRWLockExclusive(&g_vsLock);
        int i = 0;
        for (; i < g_vsMapN; ++i) if (g_vsMap[i].game == game) break;
        if (i == g_vsMapN && g_vsMapN < 512) g_vsMap[g_vsMapN++] = { game, 0 };
        if (i < 512) g_vsMap[i].hud = hud ? 1 : 0;
        ReleaseSRWLockExclusive(&g_vsLock);
    }
    int vs_map_get(void* game)   // 1 patched, 0 not, -1 unknown
    {
        int r = -1;
        AcquireSRWLockShared(&g_vsLock);
        for (int i = 0; i < g_vsMapN; ++i) if (g_vsMap[i].game == game) { r = g_vsMap[i].hud; break; }
        ReleaseSRWLockShared(&g_vsLock);
        return r;
    }
    void vs_note(const void* code, SIZE_T len, ID3D11VertexShader* vs)
    {
        InterlockedIncrement(&g_vsSeen);
        if (!code || !vs) return;
        bool drop = false;
        __try { drop = has_drop_const(code, len); } __except (EXCEPTION_EXECUTE_HANDLER) { drop = false; }
        if (drop)
        {
            AcquireSRWLockExclusive(&g_vsLock);
            if (g_realHudN < 64) g_realHud[g_realHudN++] = vs;
            ReleaseSRWLockExclusive(&g_vsLock);
        }
        uint64_t h = 0;
        __try { h = fnv64(code, len); } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        for (int i = 0; i < 13; ++i)
            if (h == kHudVsHash[i])
            {
                AcquireSRWLockExclusive(&g_vsLock);
                g_hudVsFound[i] = true;
                if (g_hudVsN < kHudVsCap) { g_hudVsIdx[g_hudVsN] = (int8_t)i; g_hudVs[g_hudVsN++] = vs; }
                ReleaseSRWLockExclusive(&g_vsLock);
                return;
            }
    }
    bool vs_is_hud(void* vs)
    {
        if (!vs) return false;
        AcquireSRWLockShared(&g_vsLock);
        bool hit = false;
        for (int i = 0; i < g_hudVsN && !hit; ++i) hit = g_hudVs[i] == vs;
        ReleaseSRWLockShared(&g_vsLock);
        return hit || vs_map_get(vs) == 1;
    }
    typedef HRESULT (__stdcall* CreateVSFn)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
    CreateVSFn oCreateVS[3] = {};
    void* g_createVsTarget[3] = {};
    void sh_note(const void* code, SIZE_T len, void* obj);   // DRAWPROBE, below
    // MKGS 2026-10-11 (JJ: the card method split the marker's elements and changed its size - "It should be fixed at the
    // source"): the geo-11 version placed every HUD vertex in 3D per eye INSIDE the game's own HUD draw (patched HUD
    // shaders). Here: for each small vertex shader the game creates, its output signature is read from the bytecode and a
    // matching geometry shader is built on first use. AKN binds it on a marker piece's per-eye draw: every vertex's HUD
    // position (x, y) becomes K0 + x Kx + y Ky (cb13) = that eye's clip position of the point on an upright card at the
    // marker's world point - an exact perspective per vertex, the piece stays in its own draw, nothing cut or resized.
    // MKGS2 2026-10-11 (JJ 13:3x: markers "still rotating on their own axes slightly when moving my head"): the 13:29 log said
    // "geometry shaders built 0, failed 0" - every marker draw took the affine row fallback, so MKGS never ran. The fixed
    // 2048-slot table filled up with the game's world vertex shaders long before the HUD's were created, so the HUD shaders
    // were never found. Now: an unbounded map keyed by the shader object, holding only the generated GS source (a few hundred
    // bytes; nothing for shaders whose signature is not supported).
    struct MkGsEnt { std::string src; ID3D11GeometryShader* gs = nullptr; int state = 0; };   // state 0 not built, 1 built, -1 failed / unsupported
    std::unordered_map<void*, MkGsEnt> g_mkGs; SRWLOCK g_mkGsLock = SRWLOCK_INIT;
    volatile LONG c_mkGsBuilt = 0, c_mkGsFail = 0, c_mkGsNoted = 0, c_mkGsUnsup = 0, c_mkGsMiss = 0;
    char g_mkGsErr[256] = {};   // the first build failure's message
    std::string mkgs_source(const std::vector<uint8_t>& c);
    void mkgs_note(const void* code, SIZE_T len, void* vs)
    {
        if (!code || !vs || len < 32) return;
        // the device just accepted these bytes; mkgs_source bounds-checks every read against the length
        std::string src = mkgs_source(std::vector<uint8_t>((const uint8_t*)code, (const uint8_t*)code + len));
        AcquireSRWLockExclusive(&g_mkGsLock);
        MkGsEnt& e = g_mkGs[vs];   // a reused object address replaces the old entry
        if (e.gs) { e.gs->Release(); e.gs = nullptr; }
        e.src = std::move(src); e.state = e.src.empty() ? -1 : 0;
        ReleaseSRWLockExclusive(&g_mkGsLock);
        InterlockedIncrement(e.state == 0 ? &c_mkGsNoted : &c_mkGsUnsup);
    }
    // the GS source for the VS's output signature (DXBC OSGN); empty = not supported (packed registers, odd masks)
    std::string mkgs_source(const std::vector<uint8_t>& c)
    {
        if (c.size() < 32 || memcmp(c.data(), "DXBC", 4) != 0) return "";
        const uint32_t nch = *(const uint32_t*)(c.data() + 28);
        for (uint32_t k = 0; k < nch && 32 + k * 4 + 4 <= c.size(); ++k)
        {
            const uint32_t off = *(const uint32_t*)(c.data() + 32 + k * 4);
            if (off + 8 > c.size()) return "";
            const char* fc = (const char*)c.data() + off;
            if (memcmp(fc, "OSGN", 4) != 0) continue;
            const uint8_t* d = c.data() + off + 8; const uint32_t dsz = *(const uint32_t*)(c.data() + off + 4);
            if (off + 8 + dsz > c.size() || dsz < 8) return "";
            const uint32_t cnt = *(const uint32_t*)d;
            std::string st = "struct V {\n"; int lastReg = -1; bool havePos = false;
            for (uint32_t i = 0; i < cnt; ++i)
            {
                const uint8_t* e = d + 8 + i * 24; if (e + 24 > d + dsz) return "";
                const uint32_t nameOff = *(const uint32_t*)e, semIdx = *(const uint32_t*)(e + 4), sysv = *(const uint32_t*)(e + 8), ctype = *(const uint32_t*)(e + 12), reg = *(const uint32_t*)(e + 16);
                const uint8_t mask = e[20];
                if (nameOff >= dsz) return "";
                const char* name = (const char*)d + nameOff;
                if ((int)reg <= lastReg) return "";   // two outputs share a register: not handled
                lastReg = (int)reg;
                int comps = 0; for (int b = 0; b < 4; ++b) if (mask & (1 << b)) ++comps;
                if (comps == 0 || mask != ((1 << comps) - 1)) return "";
                const char* ty = ctype == 1 ? "uint" : ctype == 2 ? "int" : "float";
                char line[160];
                if (sysv == 1) { if (comps != 4) return ""; _snprintf_s(line, sizeof(line), _TRUNCATE, "  float4 pos : SV_Position;\n"); havePos = true; }
                else _snprintf_s(line, sizeof(line), _TRUNCATE, "  %s%s f%u : %s%u;\n", ty, comps == 1 ? "" : comps == 2 ? "2" : comps == 3 ? "3" : "4", i, name, semIdx);
                st += line;
            }
            if (!havePos) return "";
            st += "};\n";
            return "cbuffer AkvrMk : register(b13) { float4 K0; float4 Kx; float4 Ky; float4 Kp; };\n" + st +
                   "[maxvertexcount(3)] void main(triangle V i[3], inout TriangleStream<V> s)\n"
                   "{ [unroll] for (int k = 0; k < 3; ++k) { V o = i[k]; float2 p = o.pos.xy / o.pos.w; float4 q = K0 + p.x * Kx + p.y * Ky; q.z = 0.5 * q.w; o.pos = q; s.Append(o); } s.RestartStrip(); }\n";
        }
        return "";
    }
    ID3D11GeometryShader* mkgs_get(void* vsv)
    {
        ID3D11VertexShader* vs = (ID3D11VertexShader*)vsv;
        if (!vs) return nullptr;
        AcquireSRWLockExclusive(&g_mkGsLock);
        auto it = g_mkGs.find(vs);
        MkGsEnt* e = it != g_mkGs.end() ? &it->second : nullptr;
        if (!e) InterlockedIncrement(&c_mkGsMiss);   // created before our hook, or by another device
        ID3D11GeometryShader* out = nullptr;
        if (e && e->state == 0)
        {
            e->state = -1;
            const std::string& src = e->src;
            using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
            static CompileFn compile = nullptr; if (!compile) { HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll"); compile = m ? (CompileFn)GetProcAddress(m, "D3DCompile") : nullptr; }
            ID3DBlob* b = nullptr; ID3DBlob* er = nullptr; ID3D11Device* dev = nullptr; vs->GetDevice(&dev);
            if (!src.empty() && compile && dev && SUCCEEDED(compile(src.data(), src.size(), "akvr_mkgs", nullptr, nullptr, "main", "gs_5_0", 0, 0, &b, &er)) && b &&
                SUCCEEDED(dev->CreateGeometryShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &e->gs)))
            { e->state = 1; InterlockedIncrement(&c_mkGsBuilt); }
            else
            {
                InterlockedIncrement(&c_mkGsFail);
                if (!g_mkGsErr[0]) _snprintf_s(g_mkGsErr, sizeof(g_mkGsErr), _TRUNCATE, "%s", !compile ? "no d3dcompiler_47" : er ? (const char*)er->GetBufferPointer() : "CreateGeometryShader failed");
            }
            if (b) b->Release(); if (er) er->Release(); if (dev) dev->Release();
        }
        if (e && e->state == 1) out = e->gs;
        ReleaseSRWLockExclusive(&g_mkGsLock);
        return out;
    }
    template <int K> HRESULT __stdcall hkCreateVS(ID3D11Device* d, const void* code, SIZE_T len, ID3D11ClassLinkage* cl, ID3D11VertexShader** out)
    {
        HRESULT hr = oCreateVS[K](d, code, len, cl, out);
        if (SUCCEEDED(hr) && out) { vs_note(code, len, *out); sh_note(code, len, *out); if (*out) mc_queue(code, len, *out, 1); if (*out) mkgs_note(code, len, *out); }   // MKSINGLE / MKGS
        return hr;
    }

    // Shader names (DRAWPROBE 2026-10-01): every VS/PS/CS the game creates is named here (FNV-1 64, the ShaderFixesDM
    // names; CRC-32 for Luma's), for FARRAIN and AKN.
    // PANELTIDY5 2026-10-11: the rain hunt (DRAWPROBE 1-4, RAINPARTS, RAINWRITER, NORAIN) and the test-tools switch removed.
    constexpr int kShMap = 1 << 17;
    struct ShEnt { void* obj; uint64_t h; uint32_t crc; };   // DLSS D0: crc = Luma's shader name (CRC-32 of the bytecode)
    ShEnt   g_shMap[kShMap];
    SRWLOCK g_shLock = SRWLOCK_INIT;
    uint32_t sh_slot(const void* p) { return (uint32_t)((((uintptr_t)p) >> 4) * 2654435761u) & (kShMap - 1); }
    void sh_put(void* obj, uint64_t h, uint32_t crc = 0)
    {
        if (!obj) return;
        AcquireSRWLockExclusive(&g_shLock);
        for (uint32_t i = sh_slot(obj), n = 0; n < 256; ++n, i = (i + 1) & (kShMap - 1))
            if (!g_shMap[i].obj || g_shMap[i].obj == obj) { g_shMap[i] = { obj, h, crc }; break; }
        ReleaseSRWLockExclusive(&g_shLock);
    }
    uint32_t sh_get_crc(const void* obj)
    {
        if (!obj) return 0;
        uint32_t c = 0;
        AcquireSRWLockShared(&g_shLock);
        for (uint32_t i = sh_slot(obj), n = 0; n < 256; ++n, i = (i + 1) & (kShMap - 1))
        {
            if (!g_shMap[i].obj) break;
            if (g_shMap[i].obj == obj) { c = g_shMap[i].crc; break; }
        }
        ReleaseSRWLockShared(&g_shLock);
        return c;
    }
    uint32_t crc32_buf(const void* p, size_t n)
    {   // standard CRC-32 (reflected 0xEDB88320), as ReShade's compute_crc32 that Luma names shaders with
        static uint32_t tab[256]; static bool init = false;
        if (!init) { for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; tab[i] = c; } init = true; }
        uint32_t c = 0xFFFFFFFFu; const uint8_t* b = (const uint8_t*)p;
        for (size_t i = 0; i < n; ++i) c = tab[(c ^ b[i]) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }
    uint64_t sh_get(const void* obj)
    {
        if (!obj) return 0;
        uint64_t h = 0;
        AcquireSRWLockShared(&g_shLock);
        for (uint32_t i = sh_slot(obj), n = 0; n < 256; ++n, i = (i + 1) & (kShMap - 1))
        {
            if (!g_shMap[i].obj) break;
            if (g_shMap[i].obj == obj) { h = g_shMap[i].h; break; }
        }
        ReleaseSRWLockShared(&g_shLock);
        return h;
    }
    // PANELTIDY5 2026-10-11: SHADERDUMP / DOFDUMP (shader files written at every start) removed.
    // SUITGLOSS3 2026-10-09: pixel shaders that read the camera DIRECTION rows of the view buffer (cb1[11..13]) but never
    // its position row (cb1[14]) take gloss / rim from the camera's aim. Every pixel shader is disassembled once on a
    // worker thread and flagged; AKN gives those draws the head-free game camera's rows (akvr_shader_camdir).
    typedef HRESULT(WINAPI* DisasmFn)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob**);
    struct McJob { void* obj; std::vector<uint8_t> code; int kind = 0; };   // kind 0 pixel shader, 1 vertex shader (MKSINGLE)
    std::vector<McJob> g_mcQ; SRWLOCK g_mcLock = SRWLOCK_INIT; HANDLE g_mcEvent = nullptr; volatile LONG g_mcStarted = 0;
    constexpr int kMcMap = 1 << 14;
    volatile LONG c_mcSeen = 0, c_vrFound = 0;
    // SUITGLOSS2 2026-10-09 (JJ: the matcap swap changed nothing): shaders that read the camera DIRECTION rows cb1[11..13]
    // without the position row cb1[14] (e.g. a rim / gloss from the camera's forward axis = depends on the eye's AIM only)
    void* volatile g_vrSet[kMcMap] = {};
    bool vr_has(const void* obj)
    {
        if (!obj) return false;
        for (uint32_t i = sh_slot(obj) & (kMcMap - 1), n = 0; n < 64; ++n, i = (i + 1) & (kMcMap - 1)) { void* v = g_vrSet[i]; if (!v) return false; if (v == obj) return true; }
        return false;
    }
    void vr_add(void* obj)
    {
        for (uint32_t i = sh_slot(obj) & (kMcMap - 1), n = 0; n < 64; ++n, i = (i + 1) & (kMcMap - 1))
            if (InterlockedCompareExchangePointer((PVOID volatile*)&g_vrSet[i], obj, nullptr) == nullptr || g_vrSet[i] == obj) return;
    }
    // MKSINGLE: vertex shaders that place ONE piece per draw (position x / y = dp4 with cb0[6] / cb0[7], immediate indexing)
    void* volatile g_v1Set[kMcMap] = {}; volatile LONG c_v1Found = 0;
    bool v1_has(const void* obj)
    {
        if (!obj) return false;
        for (uint32_t i = sh_slot(obj) & (kMcMap - 1), n = 0; n < 64; ++n, i = (i + 1) & (kMcMap - 1)) { void* v = g_v1Set[i]; if (!v) return false; if (v == obj) return true; }
        return false;
    }
    void v1_add(void* obj)
    {
        for (uint32_t i = sh_slot(obj) & (kMcMap - 1), n = 0; n < 64; ++n, i = (i + 1) & (kMcMap - 1))
            if (InterlockedCompareExchangePointer((PVOID volatile*)&g_v1Set[i], obj, nullptr) == nullptr || g_v1Set[i] == obj) return;
    }
    bool v1_pattern(const char* t)
    {
        if (strstr(t, "cb0[r")) return false;   // per-vertex (batched) placement
        const char* d = strstr(t, ", position"); if (!d) return false;
        const char* o = d; while (o > t && *o != 'o') --o;   // "dcl_output_siv oN.xyzw, position"
        int reg = -1; if (sscanf_s(o, "o%d", &reg) != 1 || reg < 0) return false;
        char fx[24], fy[24]; snprintf(fx, sizeof(fx), "dp4 o%d.x, ", reg); snprintf(fy, sizeof(fy), "dp4 o%d.y, ", reg);
        const char* ax = strstr(t, fx); const char* ay = strstr(t, fy);
        if (!ax || !ay) return false;
        const char* ex = strchr(ax, '\n'); const char* ey = strchr(ay, '\n');
        const std::string lx(ax, ex ? ex - ax : strlen(ax)), ly(ay, ey ? ey - ay : strlen(ay));
        return lx.find("cb0[6]") != std::string::npos && ly.find("cb0[7]") != std::string::npos;
    }
    // MKBATCH 2026-10-09: batched HUD vertex shaders (several pieces per draw, each with its own position rows in cb0)
    struct VbEntry { void* volatile obj; VsBatchLayout L; volatile LONG ready; };
    constexpr int kVbMap = 4096; VbEntry g_vb[kVbMap] = {}; volatile LONG c_vbFound = 0;
    void vb_add(void* obj, const VsBatchLayout& L)
    {
        for (uint32_t i = sh_slot(obj) & (kVbMap - 1), n = 0; n < 64; ++n, i = (i + 1) & (kVbMap - 1))
            if (InterlockedCompareExchangePointer((PVOID volatile*)&g_vb[i].obj, obj, nullptr) == nullptr || g_vb[i].obj == obj)
            {   // ready written last: a reader seeing it sees the whole layout
                g_vb[i].L = L; MemoryBarrier(); g_vb[i].ready = 1; return;
            }
    }
    // SPRITEAXES 2026-10-14 (JJ: chimney smoke sprites differ per eye and roll with head roll): UE3 particle sprite vertex shaders
    // spread each card along camera right / up from their own cb0 rows (sincos of the sprite's rotation, then
    // "mul rC, rS.xxxx, cb0[A]" + "mad rC, -rK.xxxx, cb0[B]"). Found at creation; AKN gives those two rows the head-free game
    // camera's axes per draw (playbook FAIL-STR-023: orient billboards once, for both eyes). 32 of AK's sprite shaders (rows 23/24 ..).
    struct SpEntry { void* volatile obj; int a, b, w; };   // w: the facing-mode weights row (x camera axes, y velocity, z axis lock, w face the camera position)
    constexpr int kSpMap = 2048; SpEntry g_sp[kSpMap] = {}; volatile LONG c_spFound = 0;
    bool sprite_rows(const char* t, int& a, int& b, int& w)
    {
        const char* p = t;
        while ((p = strstr(p, "sincos ")) != nullptr)
        {
            int s1 = -1, c1 = -1; char line[160] = {};
            const char* e = strchr(p, '\n'); if (!e) return false;
            if (sscanf_s(p, "sincos r%d.x, r%d.x,", &s1, &c1) == 2)
            {
                const char* l2 = e + 1; while (*l2 == ' ') ++l2;
                const char* e2 = strchr(l2, '\n'); if (!e2) return false;
                const char* l3 = e2 + 1; while (*l3 == ' ') ++l3;
                int d1 = -1, s2 = -1, n1 = -1, d2 = -1, c2 = -1, n2 = -1;
                if (sscanf_s(l2, "mul r%d.xyzw, r%d.xxxx, cb0[%d].xyzw", &d1, &s2, &n1) == 3 && s2 == s1 &&
                    sscanf_s(l3, "mad r%d.xyzw, -r%d.xxxx, cb0[%d].xyzw", &d2, &c2, &n2) == 3 && c2 == c1 && d2 == d1 && n1 >= 0 && n2 >= 0 && n1 < 256 && n2 < 256)
                {   // SPRITEFACE: the next line weights the camera-axes card: "mad rX, rC.xyzw, cb0[W].xxxx, ..."
                    a = n1; b = n2; w = -1;
                    const char* e3 = strchr(l3, '\n'); if (e3) { const char* l4 = e3 + 1; while (*l4 == ' ') ++l4; int d3 = -1, c3 = -1, n3 = -1; if (sscanf_s(l4, "mad r%d.xyzw, r%d.xyzw, cb0[%d].xxxx", &d3, &c3, &n3) == 3 && c3 == d1 && n3 >= 0 && n3 < 256) w = n3; }
                    return true; }
            }
            p = e;
        }
        return false;
    }
    void sp_add(void* obj, int a, int b, int w)
    {
        for (uint32_t i = sh_slot(obj) & (kSpMap - 1), n = 0; n < 64; ++n, i = (i + 1) & (kSpMap - 1))
            if (InterlockedCompareExchangePointer((PVOID volatile*)&g_sp[i].obj, obj, nullptr) == nullptr || g_sp[i].obj == obj) { g_sp[i].a = a; g_sp[i].b = b; g_sp[i].w = w; MemoryBarrier(); return; }
    }
    bool sp_get(const void* obj, int& a, int& b, int& w)
    {
        if (!obj) return false;
        for (uint32_t i = sh_slot(obj) & (kSpMap - 1), n = 0; n < 64; ++n, i = (i + 1) & (kSpMap - 1))
        { void* v = g_sp[i].obj; if (!v) return false; if (v == obj) { a = g_sp[i].a; b = g_sp[i].b; w = g_sp[i].w; return a >= 0 && b >= 0; } }
        return false;
    }
    // out[32]: rowX, rowY, stride, rows, base, rowW, nPiece, prow[12], pstride[12] (MKBATCH / MKTAG)
    bool vb_get(const void* obj, int* out)
    {
        if (!obj) return false;
        for (uint32_t i = sh_slot(obj) & (kVbMap - 1), n = 0; n < 64; ++n, i = (i + 1) & (kVbMap - 1))
        {
            void* v = g_vb[i].obj; if (!v) return false;
            if (v == obj)
            {
                if (!g_vb[i].ready) return false;
                MemoryBarrier(); const VsBatchLayout& L = g_vb[i].L;
                out[0] = L.rowX; out[1] = L.rowY; out[2] = L.stride; out[3] = L.rows; out[4] = L.base; out[5] = L.rowW; out[6] = L.nPiece;
                for (int q = 0; q < 12; ++q) { out[7 + q] = L.prow[q]; out[19 + q] = L.pstride[q]; }
                return true;
            }
        }
        return false;
    }
    bool vr_pattern(const char* t)
    {   // camera direction rows read, camera position row never (positions must keep each eye's own turned frame)
        const char* body = strstr(t, "dcl_temps"); if (!body) body = t;
        const bool dir = strstr(body, "cb1[11]") || strstr(body, "cb1[12]") || strstr(body, "cb1[13]");
        return dir && !strstr(body, "cb1[14]");
    }
    // LIGHTDIR 2026-10-14 (JJ: the chimney smoke brightens looking up, turns GREEN looking far down, flat game steady): AK's sprite
    // pixel shaders (07ac19831f7c0c35) get light directions in VIEW space (cb0[25] / [27] / [29]) and turn them into the world with
    // the view's inverse rows cb1[15..17]; the directions match the head-free camera, the rows follow the head -> the lights swing
    // with the head. Flagged here; AKN gives those draws the head-free camera's inverse rows (akvr_shader_lightdir).
    void* volatile g_ldSet[kMcMap] = {}; volatile LONG c_ldFound = 0;
    bool ld_has(const void* obj) { if (!obj) return false; for (uint32_t i = sh_slot(obj) & (kMcMap - 1), n = 0; n < 64; ++n, i = (i + 1) & (kMcMap - 1)) { void* v = g_ldSet[i]; if (!v) return false; if (v == obj) return true; } return false; }
    void ld_add(void* obj) { for (uint32_t i = sh_slot(obj) & (kMcMap - 1), n = 0; n < 64; ++n, i = (i + 1) & (kMcMap - 1)) if (InterlockedCompareExchangePointer((PVOID volatile*)&g_ldSet[i], obj, nullptr) == nullptr || g_ldSet[i] == obj) return; }
    bool ld_pattern(const char* t)
    {   // "mul rN.xyz, cb0[K].yyyy, cb1[16].xyzx": a constant vector turned by the view's inverse rows
        const char* p = t;
        while ((p = strstr(p, ".yyyy, cb1[16].xyz")) != nullptr) { const char* ls = p; while (ls > t && ls[-1] != '\n') --ls; if (strstr(ls, "cb0[") && strstr(ls, "cb0[") < p && strncmp(ls + strspn(ls, " "), "mul ", 4) == 0) return true; ++p; }
        return false;
    }
    DWORD WINAPI mc_worker(LPVOID)
    {
        HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
        DisasmFn dis = m ? (DisasmFn)GetProcAddress(m, "D3DDisassemble") : nullptr;
        for (;;)
        {
            WaitForSingleObject(g_mcEvent, 1000);
            for (;;)
            {
                McJob j;
                AcquireSRWLockExclusive(&g_mcLock);
                if (g_mcQ.empty()) { ReleaseSRWLockExclusive(&g_mcLock); break; }
                j = std::move(g_mcQ.back()); g_mcQ.pop_back();
                ReleaseSRWLockExclusive(&g_mcLock);
                if (!dis) continue;
                ID3DBlob* txt = nullptr;
                if (SUCCEEDED(dis(j.code.data(), j.code.size(), 0, nullptr, &txt)) && txt)
                {
                    InterlockedIncrement(&c_mcSeen);
                    if (j.kind == 0 && vr_pattern((const char*)txt->GetBufferPointer())) { vr_add(j.obj); InterlockedIncrement(&c_vrFound); }   // SUITGLOSS3
                    if (j.kind == 0 && ld_pattern((const char*)txt->GetBufferPointer())) { ld_add(j.obj); InterlockedIncrement(&c_ldFound); }   // LIGHTDIR
                    if (j.kind == 1 && v1_pattern((const char*)txt->GetBufferPointer())) { v1_add(j.obj); InterlockedIncrement(&c_v1Found); }   // MKSINGLE
                    else if (j.kind == 1) { VsBatchLayout bl; if (vs_batch_layout((const char*)txt->GetBufferPointer(), bl)) { vb_add(j.obj, bl); InterlockedIncrement(&c_vbFound); } }   // MKBATCH
                    if (j.kind == 1) { int sa = -1, sb = -1, sw = -1; if (sprite_rows((const char*)txt->GetBufferPointer(), sa, sb, sw)) { sp_add(j.obj, sa, sb, sw); InterlockedIncrement(&c_spFound); } }   // SPRITEAXES
                    if (j.kind == 1)
                    {   // SPRITEFIND 2026-10-14 (JJ: chimney smoke sprites differ per eye and roll with the head): vertex shaders that turn
                        // a sprite (sincos) or build quads from vertex / instance ids are written to akvr_captures\sprite_vs\<hash>.txt, so the
                        // smoke's camera-axis rows can be read offline (playbook FAIL-STR-023: orient billboards once for both eyes)
                        const char* t = (const char*)txt->GetBufferPointer();
                        static volatile LONG s_spN = 0;
                        if (false && (strstr(t, "sincos") || (strstr(t, "vertex_id") && strstr(t, "instance_id"))) && s_spN < 400)   /* SPRITEFIND off 2026-10-14: DIAGKIT captures save shader code */
                        {
                            InterlockedIncrement(&s_spN);
                            wchar_t dir[MAX_PATH]; GetModuleFileNameW(nullptr, dir, MAX_PATH); if (wchar_t* sl = wcsrchr(dir, L'\\')) sl[1] = 0;
                            wcscat_s(dir, L"akvr_captures\\sprite_vs\\"); CreateDirectoryW(dir, nullptr);
                            wchar_t fn[MAX_PATH]; swprintf_s(fn, L"%s%016llx.txt", dir, (unsigned long long)sh_get(j.obj));
                            if (FILE* f = _wfopen(fn, L"wb")) { fwrite(t, 1, strlen(t), f); fclose(f); }
                        }
                    }
                    txt->Release();
                }
            }
        }
    }
    bool mc_copy(void* dst, const void* src, SIZE_T n) { __try { memcpy(dst, src, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
    void mc_queue(const void* code, SIZE_T len, void* obj, int kind = 0)
    {
        if (!code || !obj || len < 32 || len > (1u << 20)) return;
        if (!InterlockedExchange(&g_mcStarted, 1)) { g_mcEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr); CloseHandle(CreateThread(nullptr, 0, mc_worker, nullptr, 0, nullptr)); }
        McJob j; j.obj = obj; j.kind = kind; j.code.resize(len);
        if (!mc_copy(j.code.data(), code, len)) return;
        AcquireSRWLockExclusive(&g_mcLock); g_mcQ.push_back(std::move(j)); ReleaseSRWLockExclusive(&g_mcLock);
        if (g_mcEvent) SetEvent(g_mcEvent);
    }
    // DIAGKIT 2026-10-14 (JJ: test features in the overlay "so that we don't have to continually build new ones"): every shader's code is
    // kept by its name (~100 MB), so a capture can save the code of the shaders on screen (akvr_shader_save) - no SPRITEFIND-style rebuilds.
    std::unordered_map<uint64_t, std::vector<uint8_t>> g_shCode; SRWLOCK g_shCodeLock = SRWLOCK_INIT;
    void sh_store(uint64_t h, const void* code, SIZE_T len)
    {   // DIAGKIT (no __try here: the vector needs unwinding; mc_copy guards the read)
        std::vector<uint8_t> v(len); if (!mc_copy(v.data(), code, len)) return;
        AcquireSRWLockExclusive(&g_shCodeLock); if (!g_shCode.count(h)) g_shCode.emplace(h, std::move(v)); ReleaseSRWLockExclusive(&g_shCodeLock);
    }
    void sh_note(const void* code, SIZE_T len, void* obj)
    {
        if (!code || !obj) return;
        uint64_t h = 0;
        __try { h = fnv64(code, len); } __except (EXCEPTION_EXECUTE_HANDLER) { h = 0; }
        if (h && len < (1u << 20)) sh_store(h, code, len);   // DIAGKIT
        uint32_t crc = 0;
        __try { crc = crc32_buf(code, len); } __except (EXCEPTION_EXECUTE_HANDLER) { crc = 0; }
        if (h) { sh_put(obj, h, crc); }
    }
    typedef HRESULT (__stdcall* CreatePSFn)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
    CreatePSFn oCreatePS[3] = {};
    void* g_createPsTarget[3] = {};
    int   g_psHooks = 0;
    // FLAREOFF3 2026-10-10 (JJ: lens flares still on; then crashes in 4 of 5 sessions once AKN swapped them at every
    // PSSetShader / CSSetShader bind - those hooks also ran for Virtual Desktop's in-process device and the game's
    // recording threads). Now Luma's way: when the game CREATES the flare sprite PS (CRC32 0x2CCEAEA7) or the bloom /
    // flare ghosts / lens dirt CS (0xB42A7F40), Luma's copy compiled with DISABLE_LENS_FLARE_AND_LENS_DIRT=1 is created
    // instead and handed to the game as its shader. Once per shader at load; nothing per frame. Setting flareoff= (1).
    int g_flareOff = 1; volatile LONG g_flareSwapped[2] = {}; char g_flareDiag[200] = "lens flare off: the game has not created its flare shaders yet";
    bool flare_code(int kind, std::vector<uint8_t>& out)
    {   // compiled once per kind (game load thread)
        static std::vector<uint8_t> s_code[2]; static int s_state[2] = {};
        if (s_state[kind] == 0)
        {
            s_state[kind] = -1;
            using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
            HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
            CompileFn compile = m ? (CompileFn)GetProcAddress(m, "D3DCompile") : nullptr;
            const D3D_SHADER_MACRO mac[] = { { "DISABLE_LENS_FLARE_AND_LENS_DIRT", "1" }, { nullptr, nullptr } };
            ID3DBlob* b = nullptr; ID3DBlob* e = nullptr;
            const char* src = kind == 0 ? kFlarePsHlsl : kBloomCsHlsl; const size_t n = kind == 0 ? sizeof(kFlarePsHlsl) - 1 : sizeof(kBloomCsHlsl) - 1;
            if (compile && SUCCEEDED(compile(src, n, kind == 0 ? "LensFlare_0x2CCEAEA7" : "BloomLensFlareLensDirt_0xB42A7F40", mac, nullptr, "main", kind == 0 ? "ps_5_0" : "cs_5_0", 0, 0, &b, &e)) && b)
            { s_code[kind].assign((const uint8_t*)b->GetBufferPointer(), (const uint8_t*)b->GetBufferPointer() + b->GetBufferSize()); s_state[kind] = 1; }
            if (b) b->Release(); if (e) e->Release();
        }
        if (s_state[kind] != 1) return false;
        out = s_code[kind];
        return true;
    }
    int flare_kind_of(const void* code, SIZE_T len)
    {
        if (!g_flareOff || !code || len < 32) return -1;
        uint32_t crc = 0;
        __try { crc = crc32_buf(code, len); } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
        return crc == 0x2CCEAEA7u ? 0 : crc == 0xB42A7F40u ? 1 : -1;
    }
    void flare_note(int kind, bool ok)
    {
        if (ok) InterlockedIncrement(&g_flareSwapped[kind]);
        _snprintf_s(g_flareDiag, sizeof(g_flareDiag), _TRUNCATE, "lens flare off %d: flare sprite shader replaced %ld x, bloom / ghosts / dirt shader replaced %ld x%s",
                    g_flareOff, g_flareSwapped[0], g_flareSwapped[1], ok ? "" : " - a replacement FAILED, the game's own is used");
    }
    template <int K> HRESULT __stdcall hkCreatePS(ID3D11Device* d, const void* code, SIZE_T len, ID3D11ClassLinkage* cl, ID3D11PixelShader** out)
    {
        if (flare_kind_of(code, len) == 0)
        {   // FLAREOFF3
            std::vector<uint8_t> rep;
            if (flare_code(0, rep))
            {
                HRESULT hr2 = oCreatePS[K](d, rep.data(), rep.size(), cl, out);
                flare_note(0, SUCCEEDED(hr2));
                if (SUCCEEDED(hr2) && out && *out) { sh_note(code, len, *out); return hr2; }   // keeps the game shader's own hash
            }
            else flare_note(0, false);
        }
        HRESULT hr = oCreatePS[K](d, code, len, cl, out);
        if (SUCCEEDED(hr) && out && *out) { sh_note(code, len, *out); mc_queue(code, len, *out); }   // SUITGLOSS
        return hr;
    }
    // shaders bound on each game-side context (immediate + the game's deferred ones)
    struct CtxSh { void* ctx; void* vs; void* ps; };
    CtxSh g_ctxSh[32] = {};
    CtxSh* ctx_sh(void* c, bool claim)
    {
        for (int i = 0; i < 32; ++i)
        {
            void* cur = g_ctxSh[i].ctx;
            if (cur == c) return &g_ctxSh[i];
            if (!cur)
            {
                if (!claim) return nullptr;
                if (InterlockedCompareExchangePointer(&g_ctxSh[i].ctx, c, nullptr) == nullptr) return &g_ctxSh[i];
                if (g_ctxSh[i].ctx == c) return &g_ctxSh[i];
            }
        }
        return nullptr;
    }
    typedef void (__stdcall* PSSetFn)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
    PSSetFn oPSSetG = nullptr;
    // PANELTIDY5 2026-10-11: SHCENSUS (60 frames of shaders.csv on every F2) removed.
    // (The SWEEP test that found these is removed: CLEANTESTS 2026-10-09.)
    // PANELTIDY5 2026-10-11: BLUROFF / SKIPSTEPS and GAMEAA (the game's SMAA dropped) are removed.
    // PANELTIDY5 2026-10-11: NORAIN (the panel's "rain off" test switch) removed.
    // PANELTIDY5: always on, as it effectively was (the removed HDRCURVE was on by default); the bound pixel shader per context
    bool ps_track() { return true; }
    void __stdcall hkPSSetG(ID3D11DeviceContext* c, ID3D11PixelShader* ps, ID3D11ClassInstance* const* ci, UINT n)
    {
        if (ps_track()) if (CtxSh* s = ctx_sh(c, true)) s->ps = ps;
        oPSSetG(c, ps, ci, n);
    }
    // PANELTIDY5 2026-10-11: the HDRCURVE / GAMELOOK / GLOWFIRST pass (the game's look with depth of field off) is removed.
    // PANELTIDY5 2026-10-11: DRAWPROBE2-4 (draw kinds, see-through groups, hide every draw / everything but the rain,
    // the compute-shader list), RAINPARTS, NEARRAIN / NEARRAIN2 / NEARRAIN3, RAINSTRETCH, RAINSPLIT and RAINWRITER removed.
    // FRAMEPAIR 2026-10-02 — JJ: "Head pose delay of 2 makes the rain go rock solid ... and it makes the [reticle] and the
    // distance HUD marker go rock solid. The problem with that is that the world becomes jittery." Off since THREADSYNC;
    // the settings framefix / framedelay are still kept and reported.
    volatile LONG g_frameFix = 0;            // FRAMEPAIR for the HUD markers (setting framefix; off since THREADSYNC)
    volatile LONG g_frameWorld = 2;          // FRAMEPAIR2: the head pose delay the rain and the markers match (JJ: 2)

    void* g_ctxCs[32] = {};   // compute shader bound, by g_ctxSh index
    uint64_t cs_hash(ID3D11DeviceContext* c)   // the bound compute shader's name (FARRAIN)
    {
        CtxSh* s = ctx_sh(c, true);
        return s ? sh_get(g_ctxCs[s - g_ctxSh]) : 0;
    }
    typedef void (__stdcall* CSSetFn)(ID3D11DeviceContext*, ID3D11ComputeShader*, ID3D11ClassInstance* const*, UINT);
    CSSetFn oCSSetG = nullptr;
    void __stdcall hkCSSetG(ID3D11DeviceContext* c, ID3D11ComputeShader* cs, ID3D11ClassInstance* const* ci, UINT n)
    {
        if (CtxSh* s = ctx_sh(c, true)) g_ctxCs[s - g_ctxSh] = cs;
        oCSSetG(c, cs, ci, n);
    }
    // FARRAIN 2026-10-01: around the rain simulation's dispatch (the fix's CS a96594b16ceb399b, patch step 1i), CS
    // cb13 holds the game camera's own forward (w = 1): the shader keeps its rain regions ahead of the GAME camera, so
    // head turns no longer swing them. Unbound = 0 = the fix's shader unchanged.
    constexpr uint64_t kRainCs = 0xa96594b16ceb399bull;
    ID3D11Buffer* g_farCb = nullptr;
    volatile LONG g_farOn = 1, g_farBinds = 0, g_farNoAxes = 0;
    ID3D11Buffer* far_cb_pre(ID3D11DeviceContext* c)
    {
        if (!g_farCb)
        {
            ID3D11Device* dev = nullptr; c->GetDevice(&dev);
            if (!dev) return nullptr;
            D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            ID3D11Buffer* b = nullptr;
            if (SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &b)) && b)
                if (InterlockedCompareExchangePointer((void**)&g_farCb, b, nullptr) != nullptr) b->Release();
            dev->Release();
            if (!g_farCb) return nullptr;
        }
        float d[12] = {};
        float* fwd = d; float* right = d + 4; float* up = d + 8;
        const bool ok = akvr_camera_base_axes(fwd, right, up);
        if (!ok) InterlockedIncrement(&g_farNoAxes);
        float row[4] = { fwd[0], fwd[1], fwd[2], ok ? 1.0f : 0.0f };
        c->UpdateSubresource(g_farCb, 0, nullptr, row, 0, 0);
        ID3D11Buffer* old = nullptr;
        c->CSGetConstantBuffers(13, 1, &old);
        c->CSSetConstantBuffers(13, 1, (ID3D11Buffer* const*)&g_farCb);
        InterlockedIncrement(&g_farBinds);
        return old;
    }
    void far_cb_post(ID3D11DeviceContext* c, ID3D11Buffer* old)
    {
        c->CSSetConstantBuffers(13, 1, &old);
        if (old) old->Release();
    }
    typedef void (__stdcall* DispatchFn)(ID3D11DeviceContext*, UINT, UINT, UINT);
    typedef void (__stdcall* DrawIndFn)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
    DispatchFn oDispatchG = nullptr;
    void __stdcall hkDispatchG(ID3D11DeviceContext* c, UINT x, UINT y, UINT z)
    {
        const uint64_t h = cs_hash(c);
        if (h == kRainCs && g_farOn) { ID3D11Buffer* ob = far_cb_pre(c); oDispatchG(c, x, y, z); far_cb_post(c, ob); return; }   // FARRAIN
        oDispatchG(c, x, y, z);
    }
    DrawIndFn oDispatchIndG = nullptr;
    void __stdcall hkDispatchIndG(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o)
    {
        const uint64_t h = cs_hash(c);
        if (h == kRainCs && g_farOn) { ID3D11Buffer* ob = far_cb_pre(c); oDispatchIndG(c, b, o); far_cb_post(c, ob); return; }   // FARRAIN
        oDispatchIndG(c, b, o);
    }
    typedef HRESULT (__stdcall* CreateCSFn)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11ComputeShader**);
    CreateCSFn oCreateCS[3] = {};
    void* g_createCsTarget[3] = {};
    int   g_csHooks = 0;
    template <int K> HRESULT __stdcall hkCreateCS(ID3D11Device* d, const void* code, SIZE_T len, ID3D11ClassLinkage* cl, ID3D11ComputeShader** out)
    {
        if (flare_kind_of(code, len) == 1)
        {   // FLAREOFF3
            std::vector<uint8_t> rep;
            if (flare_code(1, rep))
            {
                HRESULT hr2 = oCreateCS[K](d, rep.data(), rep.size(), cl, out);
                flare_note(1, SUCCEEDED(hr2));
                if (SUCCEEDED(hr2) && out && *out) { sh_note(code, len, *out); return hr2; }
            }
            else flare_note(1, false);
        }
        HRESULT hr = oCreateCS[K](d, code, len, cl, out);
        if (SUCCEEDED(hr) && out && *out) sh_note(code, len, *out);
        return hr;
    }

    Scope* cur_scope() { return g_curN > 0 ? &g_cur[g_curN - 1] : nullptr; }
    bool scope_on() { return g_scopeTid != 0; }

    Ctx* ctx_entry(Scope* s, ID3D11DeviceContext* c, int layer)
    {
        for (int i = 0; i < s->nc; ++i) if (s->c[i].ctx == c) return &s->c[i];
        if (s->nc >= 8) return nullptr;
        Ctx& e = s->c[s->nc++];
        memset(&e, 0, sizeof(e));
        e.ctx = c; e.layer = layer; e.bind = -1;
        e.same = GetCurrentThreadId() == s->tid ? 1 : 0;
        e.type = -1;
        __try { e.type = (int)c->GetType(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return &e;
    }

    void tex_desc(ID3D11RenderTargetView* rtv, Bind& b)
    {
        if (!rtv) return;
        ID3D11Resource* res = nullptr; rtv->GetResource(&res);
        if (!res) return;
        b.tex = res;
        ID3D11Texture2D* t = nullptr;
        if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) && t)
        {
            D3D11_TEXTURE2D_DESC d{}; t->GetDesc(&d);
            b.w = (int)d.Width; b.h = (int)d.Height; b.fmt = (int)d.Format; b.arr = (int)d.ArraySize;
            b.bindFlags = (int)d.BindFlags; b.misc = (int)d.MiscFlags;
            t->Release();
        }
        res->Release();   // pointer kept only as an identity for the log
    }

    void note_bind(ID3D11DeviceContext* c, ID3D11RenderTargetView* const* rtvs, UINT n, int layer)
    {
        InterlockedIncrement(&g_cBind[layer]);
        if (!scope_on()) return;
        AcquireSRWLockExclusive(&g_lock);
        if (Scope* s = cur_scope())
            if (Ctx* e = ctx_entry(s, c, layer))
            {
                ++e->binds;
                if (!rtvs || n == 0 || !rtvs[0]) e->bind = -1;
                else
                {
                    Bind b; memset(&b, 0, sizeof(b));
                    tex_desc(rtvs[0], b);
                    int found = -1;
                    for (int i = 0; i < s->nb; ++i)
                        if (s->b[i].tex == b.tex && s->b[i].ctx == c) { found = i; break; }
                    if (found < 0 && s->nb < 12)
                    {
                        b.ctx = c; b.layer = layer; b.ctxType = e->type; b.same = e->same;
                        s->b[s->nb] = b; found = s->nb++;
                    }
                    if (found >= 0) ++s->b[found].binds;
                    e->bind = found;
                }
            }
        ReleaseSRWLockExclusive(&g_lock);
    }

    void note_draw(ID3D11DeviceContext* c, int layer)
    {
        InterlockedIncrement(&g_cDraw[layer]);
        if (!scope_on()) return;
        AcquireSRWLockExclusive(&g_lock);
        if (Scope* s = cur_scope())
        {
            if (GetCurrentThreadId() != s->tid) ++s->otherDraws;
            if (Ctx* e = ctx_entry(s, c, layer))
            {
                ++e->draws;
                if (e->bind >= 0) ++s->b[e->bind].draws;
                else ++e->drawsNoBind;
            }
        }
        ReleaseSRWLockExclusive(&g_lock);
    }
    void note_list(bool finish, int layer)
    {
        InterlockedIncrement(finish ? &g_cFinish[layer] : &g_cExec[layer]);
        if (!scope_on()) return;
        AcquireSRWLockExclusive(&g_lock);
        if (Scope* s = cur_scope()) ++(finish ? s->finishes : s->execs)[layer];
        ReleaseSRWLockExclusive(&g_lock);
    }

    // ---- HUDLAYER helpers (defined after the detours need them) ----
    bool layer_bind(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* r, ID3D11DepthStencilView* d,
                    ID3D11RenderTargetView** out);
    bool layer_blend(ID3D11DeviceContext* c, ID3D11BlendState* bs, const FLOAT f[4], UINT mask);
    bool split_now(ID3D11DeviceContext* c);
    void split_pre(ID3D11DeviceContext* c);
    void split_mid(ID3D11DeviceContext* c);
    void split_post(ID3D11DeviceContext* c);
    bool layer_only_now();
    bool hud_wait_skip();   // HUDWAIT
    void layer_only_pre(ID3D11DeviceContext* c);
    void layer_only_post(ID3D11DeviceContext* c);

    // --- detours: one set of originals per layer, same shapes ---
    typedef void (__stdcall* DrawIndexedFn)(ID3D11DeviceContext*, UINT, UINT, INT);
    typedef void (__stdcall* DrawFn)(ID3D11DeviceContext*, UINT, UINT);
    typedef void (__stdcall* DrawIdxInstFn)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
    typedef void (__stdcall* DrawInstFn)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
    typedef void (__stdcall* OMSetRTFn)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
    typedef void (__stdcall* OMSetRTUAVFn)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*,
                                           UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
    typedef void (__stdcall* BlendFn)(ID3D11DeviceContext*, ID3D11BlendState*, const FLOAT[4], UINT);
    typedef void (__stdcall* VSSetFn)(ID3D11DeviceContext*, ID3D11VertexShader*, ID3D11ClassInstance* const*, UINT);
    extern ID3D11DeviceContext* g_gameCtx;
    typedef void (__stdcall* ExecFn)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
    typedef HRESULT (__stdcall* FinishFn)(ID3D11DeviceContext*, BOOL, ID3D11CommandList**);


    // MARKREC 2026-09-30 — JJ: "hang in the room" on the compass keeps the colour mark on the part (found in place
    // 8801 / missing 3) but the compass stays doubled, so the mark does not reach the 7 colour HUD shaders as
    // expected. Record what the game really uploads: shadow copies of the game's VS constant buffers (Map/Unmap and
    // UpdateSubresource on the game-facing context, only inside the HUD call), and per split HUD draw the shader
    // (one of the 13) and every float in cb0 with |v| in 0.001..0.003 (the mark is -1/512). F2 writes the last frame.
    typedef void (__stdcall* VSSetCBFn)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
    typedef HRESULT (__stdcall* MapFn)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
    typedef void (__stdcall* UnmapFn)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
    typedef void (__stdcall* UpdSubFn)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
    VSSetCBFn oVSSetCB = nullptr; MapFn oMapG = nullptr; UnmapFn oUnmapG = nullptr; UpdSubFn oUpdSubG = nullptr;
    VSSetCBFn oPSSetCB = nullptr;                          // PSMARK: same shape as VSSetConstantBuffers
    struct Shadow { void* key; UINT size; void* mapped; uint8_t data[4096]; };
    Shadow g_sh[16] = {}; int g_shN = 0;
    void* g_vsCb0 = nullptr;                               // cb0 bound for the VS on the game context
    void* g_psCb0 = nullptr;                               // PSMARK: cb0 bound for the PS
    struct MarkRow { int vs; int split; int nHits; int where[4]; float val[4]; UINT cbSize;
                     int psHits; int psWhere[2]; float psVal[2]; UINT psSize; int layerOnly;
                     // CBDUMP 2026-09-30: where it landed (1 = our HUD image bound, 0 = not), the draw's counts, and
                     // the full constants, so two F2s (four compass pieces shown / hidden) show which draws they are.
                     int toH; UINT cnt, inst; uint8_t vsData[1024]; uint8_t psData[256]; };
    MarkRow g_mrCur[96], g_mrRep[96]; int g_mrCurN = 0, g_mrRepN = 0; unsigned g_mrRepFrame = 0;
    // WORLDCARD 2026-10-06 - JJ: a HUD card (upgrade info) moves opposite to head turns on the room-fixed HUD layer; its
    // movement is not in the parts' 2D matrices (hudlayers.txt identical across three F2s), so the per-draw constants
    // are needed. Without test tools F2 now records 2 HUD frames and then writes hudmarks.csv / hudcb.txt.
    volatile LONG g_mrRecLeft = 0;
    wchar_t g_mrPath[MAX_PATH] = L"";
    Shadow* sh_find(void* key, bool add)
    {
        for (int i = 0; i < g_shN; ++i) if (g_sh[i].key == key) return &g_sh[i];
        if (!add || g_shN >= 16) return nullptr;
        Shadow& s = g_sh[g_shN++]; s.key = key; s.size = 0; s.mapped = nullptr;
        __try { D3D11_BUFFER_DESC d{}; ((ID3D11Buffer*)key)->GetDesc(&d); s.size = d.ByteWidth > 4096 ? 4096 : d.ByteWidth; }
        __except (EXCEPTION_EXECUTE_HANDLER) { s.size = 0; }
        return &s;
    }
    bool snoop_ctx(ID3D11DeviceContext* c);
    void __stdcall hkVSSetCB(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11Buffer* const* b)
    {
        if (snoop_ctx(c) && start == 0 && n >= 1 && b) { g_vsCb0 = b[0]; if (b[0]) sh_find(b[0], true); }
        oVSSetCB(c, start, n, b);
    }
    void __stdcall hkPSSetCB(ID3D11DeviceContext* c, UINT start, UINT n, ID3D11Buffer* const* b)
    {
        if (snoop_ctx(c) && start == 0 && n >= 1 && b) { g_psCb0 = b[0]; if (b[0]) sh_find(b[0], true); }
        oPSSetCB(c, start, n, b);
    }
    // PSMARK 2026-09-30 — JJ: four compass pieces carry the mark but stay only in the head-locked picture: they are
    // drawn by HUD shaders whose VERTEX shader never sees a colour transform (3b819a7e, 599bd060, e12863b9,
    // 7d8fcdc2), so the colour - and the mark - must reach the PIXEL shader's constants. For those, the mark is looked
    // for in the first two rows of the bound PS cb0 (where a colour pair would sit), and a marked draw goes whole to
    // the layer on the CPU side (drawn once, cb13 = 0 so the shader keeps it). F2 hudmarks.csv shows ps hits either way.
    bool vs_no_colour(int vs) { return vs == 1 || vs == 3 || vs == 8 || vs == 9; }
    bool ps_marked()
    {
        Shadow* s = g_psCb0 ? sh_find(g_psCb0, false) : nullptr;
        if (!s || s->size < 32) return false;
        const float* f = (const float*)s->data;
        for (int i = 0; i < 8; ++i) if (f[i] < -0.0002f && f[i] > -0.02f) return true;   // PARTTAG5: stacked marks too
        return false;
    }
    HRESULT __stdcall hkMapG(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP t, UINT f, D3D11_MAPPED_SUBRESOURCE* m)
    {
        const HRESULT hr = oMapG(c, r, sub, t, f, m);
        if (SUCCEEDED(hr) && m && snoop_ctx(c)) if (Shadow* s = sh_find(r, false)) s->mapped = m->pData;
        return hr;
    }
    void __stdcall hkUnmapG(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub)
    {
        if (snoop_ctx(c))
            if (Shadow* s = sh_find(r, false))
                if (s->mapped && s->size) { __try { memcpy(s->data, s->mapped, s->size); } __except (EXCEPTION_EXECUTE_HANDLER) {} s->mapped = nullptr; }
        oUnmapG(c, r, sub);
    }
    void __stdcall hkUpdSubG(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* src, UINT rp, UINT dp)
    {
        if (snoop_ctx(c) && !box && src)
            if (Shadow* s = sh_find(r, false))
                if (s->size) { __try { memcpy(s->data, src, s->size); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
        oUpdSubG(c, r, sub, box, src, rp, dp);
    }
    int vs_hud_index(void* vs);
    void mark_record(ID3D11DeviceContext* c, bool split, UINT cnt, UINT inst)
    {
        if (g_mrRecLeft <= 0 || g_mrCurN >= 96) return;   // WORLDCARD: only the 2 HUD frames after an F2
        MarkRow& m = g_mrCur[g_mrCurN++];
        m.vs = vs_hud_index(g_curVs); m.split = split ? 1 : 0; m.nHits = 0; m.cbSize = 0;
        m.toH = g_hBound ? 1 : 0; m.cnt = cnt; m.inst = inst;   // CBDUMP
        memset(m.vsData, 0, sizeof(m.vsData)); memset(m.psData, 0, sizeof(m.psData));
        if (Shadow* sv = g_vsCb0 ? sh_find(g_vsCb0, false) : nullptr) memcpy(m.vsData, sv->data, sv->size < 1024 ? sv->size : 1024);
        if (Shadow* sp = g_psCb0 ? sh_find(g_psCb0, false) : nullptr) memcpy(m.psData, sp->data, sp->size < 256 ? sp->size : 256);
        Shadow* s = g_vsCb0 ? sh_find(g_vsCb0, false) : nullptr;
        if (!s || !s->size) { m.nHits = -1; m.psHits = 0; m.psSize = 0; m.layerOnly = 0; return; }
        m.cbSize = s->size;
        m.psHits = 0; m.psSize = 0; m.layerOnly = 0;
        if (Shadow* p = g_psCb0 ? sh_find(g_psCb0, false) : nullptr)
        {
            m.psSize = p->size;
            const float* pf = (const float*)p->data;
            for (UINT i = 0; i < p->size / 4; ++i)
            {
                const float a = pf[i] < 0.0f ? -pf[i] : pf[i];
                if (a > 0.001f && a < 0.003f) { if (m.psHits < 2) { m.psWhere[m.psHits] = (int)i; m.psVal[m.psHits] = pf[i]; } ++m.psHits; }
            }
        }
        m.layerOnly = split && vs_no_colour(m.vs) && ps_marked() ? 1 : 0;
        const float* f = (const float*)s->data;
        for (UINT i = 0; i < s->size / 4; ++i)
        {
            const float a = f[i] < 0.0f ? -f[i] : f[i];
            if (a > 0.001f && a < 0.003f)
            {
                if (m.nHits < 4) { m.where[m.nHits] = (int)i; m.val[m.nHits] = f[i]; }
                ++m.nHits;
            }
        }
    }

    template <int L> struct Det
    {
        static inline DrawIndexedFn oDrawIndexed = nullptr;
        static inline DrawFn oDraw = nullptr;
        static inline DrawIdxInstFn oDrawIdxInst = nullptr;
        static inline DrawInstFn oDrawInst = nullptr;
        static inline OMSetRTFn oOMSetRT = nullptr;
        static inline OMSetRTUAVFn oOMSetRTUAV = nullptr;
        static inline BlendFn oBlend = nullptr;
        static inline VSSetFn oVSSet = nullptr;
        static inline ExecFn oExec = nullptr;
        static inline FinishFn oFinish = nullptr;
        // HUDSPLIT: on the game layer, while H is bound, each draw goes twice (flat -> H, depth -> picture).
        static void __stdcall DrawIndexed(ID3D11DeviceContext* c, UINT n, UINT s, INT b)
        {
            note_draw(c, L);
            if (L == 0 && snoop_ctx(c)) mark_record(c, split_now(c), n, 1);   // MARKREC
            if (L == 0 && split_now(c) && hud_wait_skip()) return;   // HUDWAIT: no HUD until its parts are known
            if (L == 0 && split_now(c) && layer_only_now()) { layer_only_pre(c); oDrawIndexed(c, n, s, b); layer_only_post(c); return; }   // PSMARK
            if (L == 0 && split_now(c)) { split_pre(c); oDrawIndexed(c, n, s, b); split_mid(c); oDrawIndexed(c, n, s, b); split_post(c); return; }
            if (L == 0 && c == g_gameCtx && akvr_hudsplit_active())
            {   // VSID2: geo-11 may bind its real shader only when the draw arrives - learn the pairing here
                t_realVs = nullptr;
                oDrawIndexed(c, n, s, b);
                if (t_realVs && g_curVs) vs_map_set(g_curVs, real_is_hud(t_realVs));
                return;
            }
            oDrawIndexed(c, n, s, b);
        }
        static void __stdcall Draw(ID3D11DeviceContext* c, UINT n, UINT s)
        {
            note_draw(c, L);
            if (L == 0 && snoop_ctx(c)) mark_record(c, split_now(c), n, 1);   // MARKREC
            if (L == 0 && split_now(c) && hud_wait_skip()) return;   // HUDWAIT: no HUD until its parts are known
            if (L == 0 && split_now(c) && layer_only_now()) { layer_only_pre(c); oDraw(c, n, s); layer_only_post(c); return; }   // PSMARK
            if (L == 0 && split_now(c)) { split_pre(c); oDraw(c, n, s); split_mid(c); oDraw(c, n, s); split_post(c); return; }
            if (L == 0 && c == g_gameCtx && akvr_hudsplit_active())
            {   // VSID2: geo-11 may bind its real shader only when the draw arrives - learn the pairing here
                t_realVs = nullptr;
                oDraw(c, n, s);
                if (t_realVs && g_curVs) vs_map_set(g_curVs, real_is_hud(t_realVs));
                return;
            }
            oDraw(c, n, s);
        }
        static void __stdcall DrawIdxInst(ID3D11DeviceContext* c, UINT a, UINT i, UINT s, INT b, UINT si)
        {
            note_draw(c, L);
            if (L == 0 && snoop_ctx(c)) mark_record(c, split_now(c), a, i);   // MARKREC
            if (L == 0 && split_now(c) && hud_wait_skip()) return;   // HUDWAIT: no HUD until its parts are known
            if (L == 0 && split_now(c) && layer_only_now()) { layer_only_pre(c); oDrawIdxInst(c, a, i, s, b, si); layer_only_post(c); return; }   // PSMARK
            if (L == 0 && split_now(c)) { split_pre(c); oDrawIdxInst(c, a, i, s, b, si); split_mid(c); oDrawIdxInst(c, a, i, s, b, si); split_post(c); return; }
            if (L == 0 && c == g_gameCtx && akvr_hudsplit_active())
            {   // VSID2: geo-11 may bind its real shader only when the draw arrives - learn the pairing here
                t_realVs = nullptr;
                oDrawIdxInst(c, a, i, s, b, si);
                if (t_realVs && g_curVs) vs_map_set(g_curVs, real_is_hud(t_realVs));
                return;
            }
            oDrawIdxInst(c, a, i, s, b, si);
        }
        static void __stdcall DrawInst(ID3D11DeviceContext* c, UINT a, UINT i, UINT s, UINT si)
        {
            note_draw(c, L);
            if (L == 0 && snoop_ctx(c)) mark_record(c, split_now(c), a, i);   // MARKREC
            if (L == 0 && split_now(c) && hud_wait_skip()) return;   // HUDWAIT: no HUD until its parts are known
            if (L == 0 && split_now(c) && layer_only_now()) { layer_only_pre(c); oDrawInst(c, a, i, s, si); layer_only_post(c); return; }   // PSMARK
            if (L == 0 && split_now(c)) { split_pre(c); oDrawInst(c, a, i, s, si); split_mid(c); oDrawInst(c, a, i, s, si); split_post(c); return; }
            if (L == 0 && c == g_gameCtx && akvr_hudsplit_active())
            {   // VSID2: geo-11 may bind its real shader only when the draw arrives - learn the pairing here
                t_realVs = nullptr;
                oDrawInst(c, a, i, s, si);
                if (t_realVs && g_curVs) vs_map_set(g_curVs, real_is_hud(t_realVs));
                return;
            }
            oDrawInst(c, a, i, s, si);
        }
        static void __stdcall OMSetRT(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* r, ID3D11DepthStencilView* d)
        {
            note_bind(c, r, n, L);
            ID3D11RenderTargetView* sub[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
            if (L == 0 && layer_bind(c, n, r, d, sub)) { oOMSetRT(c, n, sub, d); return; }
            oOMSetRT(c, n, r, d);
        }
        static void __stdcall OMSetRTUAV(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* r, ID3D11DepthStencilView* d,
                                         UINT us, UINT un, ID3D11UnorderedAccessView* const* u, const UINT* uc)
        {
            if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
            {
                note_bind(c, r, n, L);
                ID3D11RenderTargetView* sub[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
                if (L == 0 && layer_bind(c, n, r, d, sub)) { oOMSetRTUAV(c, n, sub, d, us, un, u, uc); return; }
            }
            oOMSetRTUAV(c, n, r, d, us, un, u, uc);
        }
        static void __stdcall Blend(ID3D11DeviceContext* c, ID3D11BlendState* bs, const FLOAT f[4], UINT m)
        {
            if (L == 0 && layer_blend(c, bs, f, m)) return;
            oBlend(c, bs, f, m);
        }
        static void __stdcall VSSet(ID3D11DeviceContext* c, ID3D11VertexShader* vs, ID3D11ClassInstance* const* ci, UINT n)
        {
            if (L == 0)
            {
                if (c == g_gameCtx) g_curVs = vs;   // VSID: which shader the next HUD draw uses
                if (ps_track()) if (CtxSh* sh = ctx_sh(c, true)) sh->vs = vs;   // DRAWPROBE
                t_realVs = nullptr;
                oVSSet(c, vs, ci, n);
                if (t_realVs && vs) vs_map_set(vs, real_is_hud(t_realVs));   // VSID2: geo-11 bound its real shader now
                if (c == g_gameCtx && akvr_hudsplit_active() && !g_hudVsN) vs_hash_scan(c);  // VSID3 (only without VSID4 names)
                return;
            }
            t_realVs = vs;                               // VSID2: real level (below geo-11)
            oVSSet(c, vs, ci, n);
        }
        static void __stdcall Exec(ID3D11DeviceContext* c, ID3D11CommandList* l, BOOL r) { note_list(false, L); oExec(c, l, r); }
        static HRESULT __stdcall Finish(ID3D11DeviceContext* c, BOOL r, ID3D11CommandList** l) { note_list(true, L); return oFinish(c, r, l); }
    };
    using G = Det<0>;

    // A copy of the game's blend state with HUD-004's alpha: coverage accumulates (ONE,
    // INV_SRC_ALPHA) and alpha is always written, so H ends up premultiplied with true coverage.
    ID3D11BlendState* blend_variant(ID3D11DeviceContext* c, ID3D11BlendState* bs)
    {
        if (!bs) return nullptr;                      // default state: no blending, alpha written as is
        for (int i = 0; i < g_bvN; ++i) if (g_bv[i].orig == bs) return g_bv[i].var;
        if (g_bvN >= 64) return bs;
        D3D11_BLEND_DESC d{}; bs->GetDesc(&d);
        const int n = d.IndependentBlendEnable ? 8 : 1;
        for (int i = 0; i < n; ++i)
        {
            D3D11_RENDER_TARGET_BLEND_DESC& t = d.RenderTarget[i];
            // HUDLAYER5 — JJ: a dark rectangle behind part of the compass strip. Only a PLAIN see-through
            // blend (src ONE or SRC_ALPHA, dest INV_SRC_ALPHA, add) can be carried by a see-through layer as
            // coverage. Every other Scaleform blend mode (add, screen, multiply, lighten/darken = MIN/MAX,
            // subtract) acts on the picture behind it; on our cleared image a multiply turns into a dark
            // patch. Those leave the layer's alpha untouched: light they add still shows, darkening is lost.
            const bool plain = t.BlendOp == D3D11_BLEND_OP_ADD && t.DestBlend == D3D11_BLEND_INV_SRC_ALPHA &&
                               (t.SrcBlend == D3D11_BLEND_ONE || t.SrcBlend == D3D11_BLEND_SRC_ALPHA);
            if (t.BlendEnable && plain)
            {
                t.SrcBlendAlpha = D3D11_BLEND_ONE; t.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; t.BlendOpAlpha = D3D11_BLEND_OP_ADD;
                if (t.RenderTargetWriteMask & 7) t.RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
            }
            else if (t.BlendEnable)
            {
                t.SrcBlendAlpha = D3D11_BLEND_ZERO; t.DestBlendAlpha = D3D11_BLEND_ONE; t.BlendOpAlpha = D3D11_BLEND_OP_ADD;
                ++g_bvOther;
            }
            else if (t.RenderTargetWriteMask & 7) t.RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
        }
        ID3D11Device* dev = nullptr; c->GetDevice(&dev);
        ID3D11BlendState* v = nullptr;
        if (!dev || FAILED(dev->CreateBlendState(&d, &v)) || !v) { ++g_bvFail; v = nullptr; }
        if (dev) dev->Release();
        if (!v) return bs;
        g_bv[g_bvN++] = { bs, v };                    // kept for the process lifetime (few dozen)
        return v;
    }

    void module_of(const void* p, char* out, size_t cap);
    int copy_group(int f)
    {
        if (f >= 27 && f <= 32) return 27;     // R8G8B8A8_*
        if (f == 87 || f == 90 || f == 91) return 90;   // B8G8R8A8_*
        return f;
    }
    // H is made through the game (geo-11) device but copied by the REAL context in xr.cpp and in
    // the coverage readback. Only safe if geo-11 handed back a real d3d11 texture (same device as
    // the real context), not a stand-in object of its own. Checked once per H.
    // HUDLAYER2 2026-09-28 — first run: H's function table lives in geo11.dll: geo-11 hands the game a
    // stand-in object for every texture and keeps the real one inside. Find it without calling anything
    // on unknown memory: a real d3d11 texture is recognised by its EXACT function-table pointer, read from
    // a 1x1 texture we create on the real device. Search H's first 0x200 bytes, and one pointer deeper.
    bool seh_qword(uintptr_t a, uintptr_t& v)
    {
        __try { v = *(const uintptr_t*)a; return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool plausible(uintptr_t p) { return p > 0x10000 && p < 0x7fffffffffffull && (p & 7) == 0; }
    void check_H_real()
    {
        g_Hreal = 0; g_Hsrc = nullptr; g_Hfound[0] = 0;
        if (!g_H || !g_ctx) return;
        module_of(*(void**)g_H, g_Hmodule, sizeof(g_Hmodule));
        ID3D11Device* rd = nullptr; g_ctx->GetDevice(&rd);
        if (!rd) return;
        ID3D11Device* hd = nullptr; g_H->GetDevice(&hd);
        const bool direct = hd && hd == rd;
        if (hd) hd->Release();
        g_HsrcOff = 0;   // MENUCRASH: 0 = H itself (nothing to follow)
        if (direct) { g_Hsrc = g_H; g_Hreal = 1; strcpy_s(g_Hfound, sizeof(g_Hfound), "H itself"); rd->Release(); return; }
        uintptr_t texVt = 0;
        {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = 1; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            ID3D11Texture2D* t = nullptr;
            if (SUCCEEDED(rd->CreateTexture2D(&td, nullptr, &t)) && t) { texVt = *(uintptr_t*)t; t->Release(); }
        }
        int k = 0;
        if (!texVt) { strcpy_s(g_Hfound, sizeof(g_Hfound), "no reference texture"); rd->Release(); return; }
        const uintptr_t base = (uintptr_t)g_H;
        for (uintptr_t off = 8; off < 0x200; off += 8)
        {
            uintptr_t p = 0, vt = 0;
            if (!seh_qword(base + off, p) || !plausible(p) || !seh_qword(p, vt)) continue;
            uintptr_t hits[2] = { 0, 0 }; uintptr_t sub2[2] = { 0, 0 }; int nh = 0;
            if (vt == texVt) { hits[nh] = p; sub2[nh] = (uintptr_t)-1; ++nh; }
            else
                for (uintptr_t o2 = 0; o2 < 0x80 && nh < 2; o2 += 8)
                {
                    uintptr_t q = 0, vt2 = 0;
                    if (seh_qword(p + o2, q) && plausible(q) && seh_qword(q, vt2) && vt2 == texVt) { hits[nh] = q; sub2[nh] = o2; ++nh; }
                }
            for (int i = 0; i < nh; ++i)
            {
                ID3D11Texture2D* t = (ID3D11Texture2D*)hits[i];
                D3D11_TEXTURE2D_DESC d{}; t->GetDesc(&d);
                if (k < (int)sizeof(g_Hfound) - 48)
                {
                    if (sub2[i] == (uintptr_t)-1)
                        k += _snprintf_s(g_Hfound + k, sizeof(g_Hfound) - k, _TRUNCATE, "+0x%X: %ux%u f%d arr%u ms%u; ",
                                         (unsigned)off, d.Width, d.Height, (int)d.Format, d.ArraySize, d.SampleDesc.Count);
                    else
                        k += _snprintf_s(g_Hfound + k, sizeof(g_Hfound) - k, _TRUNCATE, "+0x%X>+0x%X: %ux%u f%d arr%u ms%u; ",
                                         (unsigned)off, (unsigned)sub2[i], d.Width, d.Height, (int)d.Format, d.ArraySize, d.SampleDesc.Count);
                }
                // First real texture of H's size and byte layout: with the HUD shift at 0 either eye's copy holds the same HUD.
                if (!g_Hsrc && (int)d.Width == g_Hw && (int)d.Height == g_Hh && d.ArraySize >= 1 && d.SampleDesc.Count == 1 &&
                    copy_group((int)d.Format) == copy_group(g_Hfmt))
                { g_Hsrc = t; g_HsrcOff = off; g_HsrcSub = sub2[i]; g_texVt = texVt; }   // MENUCRASH: keep the path
            }
        }
        if (!k) strcpy_s(g_Hfound, sizeof(g_Hfound), "no real texture inside geo-11's object");
        g_Hreal = g_Hsrc ? 1 : 0;
        rd->Release();
    }
    // MENUCRASH: re-read the real texture through the path check_H_real found; false = none valid now.
    bool hsrc_refresh()
    {
        if (!g_H) return false;
        if (g_Hreal != 1 || !g_Hsrc)
        {   // lost (xr.cpp fault or an earlier failed refresh): search again at most once a second
            static ULONGLONG s_last = 0;
            const ULONGLONG now = GetTickCount64();
            if (g_Hreal == -1 || now - s_last < 1000) return false;
            s_last = now;
            check_H_real();
            return g_Hreal == 1 && g_Hsrc != nullptr;
        }
        if (!g_HsrcOff) return true;                       // H itself, owned by us
        uintptr_t p = 0, t = 0, vt = 0;
        bool ok = seh_qword((uintptr_t)g_H + g_HsrcOff, p) && plausible(p);
        if (ok)
        {
            if (g_HsrcSub == (uintptr_t)-1) t = p;
            else ok = seh_qword(p + g_HsrcSub, t) && plausible(t);
        }
        ok = ok && seh_qword(t, vt) && vt == g_texVt;
        if (ok && t != (uintptr_t)g_Hsrc)
        {
            D3D11_TEXTURE2D_DESC d{}; ((ID3D11Texture2D*)t)->GetDesc(&d);
            ok = (int)d.Width == g_Hw && (int)d.Height == g_Hh && d.SampleDesc.Count == 1 && copy_group((int)d.Format) == copy_group(g_Hfmt);
            if (ok) { g_Hsrc = (ID3D11Texture2D*)t; ++g_HsrcMoves; }
        }
        if (!ok)
        {   // search H again (once per loss): geo-11 may have put the new texture somewhere else
            ++g_HsrcLost;
            check_H_real();
            ok = g_Hreal == 1 && g_Hsrc != nullptr;
        }
        return ok;
    }
    bool ensure_H(ID3D11DeviceContext* c, int w, int h, int fmt)
    {
        if (g_H && g_Hw == w && g_Hh == h && g_Hfmt == fmt) return g_Hrtv != nullptr;
        if (g_Hrtv) { g_Hrtv->Release(); g_Hrtv = nullptr; }
        if (g_H) { g_H->Release(); g_H = nullptr; }
        if (g_stage) { g_stage->Release(); g_stage = nullptr; g_stageState = 0; }
        g_Hw = w; g_Hh = h; g_Hfmt = fmt;
        ID3D11Device* dev = nullptr; c->GetDevice(&dev);   // the game device: geo-11 knows about H
        if (!dev) { g_Herr = 1; return false; }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = (DXGI_FORMAT)fmt; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        g_Herr = 0;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_H)) || !g_H) { g_H = nullptr; g_Herr = 1; }
        else if (FAILED(dev->CreateRenderTargetView(g_H, nullptr, &g_Hrtv)) || !g_Hrtv) { g_Hrtv = nullptr; g_Herr = 2; }
        dev->Release();
        check_H_real();
        return g_Hrtv != nullptr;
    }

    bool layer_bind(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* r, ID3D11DepthStencilView* d,
                    ID3D11RenderTargetView** out)
    {
        if (!g_scopeLayer || c != g_gameCtx || !akvr_hudsplit_active()) return false;
        const UINT k = n > D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT ? D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT : n;
        g_reqN = k; g_reqDsv = d; g_reqValid = true;
        for (UINT i = 0; i < k; ++i) g_reqRtv[i] = r ? r[i] : nullptr;
        bool main = false;
        if (k >= 1 && r && r[0])
        {
            Bind b; memset(&b, 0, sizeof(b));
            tex_desc(r[0], b);
            // MENULIST 2026-10-06 - JJ: the graphics menu flickered and showed only its first four options; earlier
            // the same menu crashed the game. F2 01:59: every third frame the menu draws its option list into a
            // 1472x1216 picture (69 draws) and a 1024x64 strip, then puts them on screen. "Any target 1000x1000 or
            // bigger" took the list picture for the HUD: H was thrown away and made at 1472x1216, the list went into
            // it instead of its own picture, and H was made again at full size the next frame (flicker, missing
            // options; xr.cpp then read the freed H = the MENUCRASH crash). A target smaller than H is the game's own.
            const bool notSmaller = !g_H || (long long)b.w * b.h >= (long long)g_Hw * g_Hh;
            main = b.w >= 1000 && b.h >= 1000 && b.arr == 1 && notSmaller && ensure_H(c, b.w, b.h, b.fmt) && g_Hreal == 1;
        }
        if (!main)
        {
            if (g_hBound)
            {   // leaving H for a filter target: the game's own blend again
                g_hBound = false;
                if (g_reqBlendValid) G::oBlend(c, g_reqBlend, g_reqFactor, g_reqMask);
            }
            return false;
        }
        if (!g_hCleared)
        {
            const FLOAT zero[4] = { 0, 0, 0, 0 };
            c->ClearRenderTargetView(g_Hrtv, zero);
            g_hCleared = true;
        }
        for (UINT i = 0; i < k; ++i) out[i] = r[i];
        out[0] = g_Hrtv;
        for (UINT i = 0; i < k; ++i) g_subRtv[i] = out[i];
        if (!g_hBound && g_reqBlendValid) G::oBlend(c, blend_variant(c, g_reqBlend), g_reqFactor, g_reqMask);
        g_hBound = true;
        ++g_scopeSubs;
        return true;
    }

    bool layer_blend(ID3D11DeviceContext* c, ID3D11BlendState* bs, const FLOAT f[4], UINT mask)
    {
        if (!g_scopeLayer || c != g_gameCtx || !akvr_hudsplit_active()) return false;
        g_reqBlend = bs; g_reqMask = mask; g_reqBlendValid = true;
        for (int i = 0; i < 4; ++i) g_reqFactor[i] = f ? f[i] : 1.0f;
        if (!g_hBound) return false;
        G::oBlend(c, blend_variant(c, bs), f, mask);
        return true;
    }

    bool snoop_ctx(ID3D11DeviceContext* c) { return c == g_gameCtx && g_gameCtx && akvr_hudsplit_active(); }
    int vs_hud_index(void* vs)
    {
        if (!vs) return -1;
        int r = -1;
        AcquireSRWLockShared(&g_vsLock);
        for (int i = 0; i < g_hudVsN; ++i) if (g_hudVs[i] == vs) { r = g_hudVsIdx[i]; break; }
        ReleaseSRWLockShared(&g_vsLock);
        return r;
    }
    int hud_names_found()
    {
        int n = 0;
        AcquireSRWLockShared(&g_vsLock);
        for (bool b : g_hudVsFound) n += b ? 1 : 0;
        ReleaseSRWLockShared(&g_vsLock);
        return n;
    }
    bool split_vs_ok(ID3D11DeviceContext* c)
    {
        // VSID4: the game-facing device named the shaders at creation - exact answer, and a shader
        // outside the 13 (TARGET DETAIL panel) is drawn once, into the layer.
        if (hud_names_found() > 0) return vs_is_hud(g_curVs);
        const int h = cur_vs_hash_hud(c);
        if (h >= 0) return h == 1;                   // VSID3: geo-11's own record of the bound shader
        if (vs_is_hud(g_curVs)) return true;          // VSID2 pairing, if it ever worked
        return true;                                 // unknown: draw twice as in HUDLAYER6b (reticle at depth)
    }
    bool split_now(ID3D11DeviceContext* c)
    {
        return g_splitReady == 1 && g_splitWant && !g_layerMenu && g_scopeLayer && g_hBound && c == g_gameCtx &&
               g_cbFlat && g_cbDepth && akvr_hudsplit_active() && split_vs_ok(c);
    }
    void split_pre(ID3D11DeviceContext* c) { c->VSSetConstantBuffers(13, 1, &g_cbFlat); }
    // PSMARK: one draw into H (already bound) with cb13 = 0, so the shader's split tail keeps the piece.
    long g_layerOnlyDraws = 0, g_layerOnlyRep = 0;
    // MARKFIRST 2026-10-02: until the first HUD-part read of a gameplay stretch has landed its room marks, every HUD draw
    // goes whole to the room layer (nothing of the HUD in the head-locked picture).
    // HUDWAIT 2026-10-02 — JJ: "when first entering gameplay, the distance marker is in the middle of the screen and not locked
    // onto its target, and then it quickly jumps to the target" - the room layer drew it at its shrunk-HUD spot during that
    // wait. The HUD is now left out until the marks are ready (~0.7 s); the draw hooks return before this is asked.
    // at most 1.5 s per wait, so the HUD can never stay hidden if the marks never arrive
    bool hud_wait_skip()
    {
        static ULONGLONG s_since = 0;
        if (akvr_hud_marks_ready()) { s_since = 0; return false; }
        const ULONGLONG now = GetTickCount64();
        if (!s_since) s_since = now;
        return now - s_since < 1500;
    }
    bool layer_only_now() { return g_cbAll && ((vs_no_colour(vs_hud_index(g_curVs)) && ps_marked()) || !akvr_hud_marks_ready()); }
    void layer_only_pre(ID3D11DeviceContext* c) { c->VSSetConstantBuffers(13, 1, &g_cbAll); }
    void layer_only_post(ID3D11DeviceContext* c) { c->VSSetConstantBuffers(13, 1, &g_cbFlat); ++g_layerOnlyDraws; }
    void split_mid(ID3D11DeviceContext* c)
    {   // the game's own target and blend: scene-depth pieces land in the 3D picture as before
        G::oOMSetRT(c, g_reqN, g_reqRtv, g_reqDsv);
        if (g_reqBlendValid) G::oBlend(c, g_reqBlend, g_reqFactor, g_reqMask);
        c->VSSetConstantBuffers(13, 1, &g_cbDepth);
    }
    void split_post(ID3D11DeviceContext* c)
    {
        G::oOMSetRT(c, g_reqN, g_subRtv, g_reqDsv);
        if (g_reqBlendValid) G::oBlend(c, blend_variant(c, g_reqBlend), g_reqFactor, g_reqMask);
        c->VSSetConstantBuffers(13, 1, &g_cbFlat);
        ++g_splitDraws;
    }
    // Is the HUDSPLIT shader edit installed? (AKVR-fix-patches.ps1 marker in both scene-depth shaders.)
    void split_check()
    {
        if (g_splitReady != -1) return;
        g_splitReady = 0;
        wchar_t dir[MAX_PATH] = L"";
        GetModuleFileNameW(nullptr, dir, MAX_PATH);
        wchar_t* sl = wcsrchr(dir, L'\\');
        if (!sl) return;
        *(sl + 1) = 0;
        const wchar_t* files[2] = { L"ShaderFixesDM\\05154232f7872d0d-vs.txt", L"ShaderFixesDM\\9938094af96353c0-vs.txt" };
        for (const wchar_t* fn : files)
        {
            std::wstring p = std::wstring(dir) + fn;
            FILE* f = nullptr;
            if (_wfopen_s(&f, p.c_str(), L"rb") != 0 || !f) return;
            std::string all; char buf[4096]; size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, n);
            fclose(f);
            if (all.find("// AKVR HUDSPLIT") == std::string::npos) return;
        }
        g_splitReady = 1;
    }
    void split_buffers(ID3D11DeviceContext* c)
    {
        if (g_cbFlat && g_cbDepth) return;
        ID3D11Device* dev = nullptr; c->GetDevice(&dev);
        if (!dev) return;
        // SWITCHFIX 2026-09-30: EDGEBAND's edit left a comment in the middle of this line, which commented out the
        // Usage and BindFlags - the switch buffers were created without CONSTANT_BUFFER, never reached the shaders
        // (cb13 read 0 = keep everything), and every split HUD piece was drawn in BOTH the layer and the picture
        // (JJ's "doubled HUD" from EDGEBAND on). Back to one row, as the shaders declare (CB13[1]).
        // TARGETDEPTH (fix step 1k): three rows - row 0 as before, rows 1 / 2 = the "stays on its target" points.
        // TARGETMOVE (fix step 1m): + row 3 = the points' screen offsets for the head turn.
        // TARGETSCALE (fix step 1n): + row 4 = the map from AKVR's HUD box back to the movie's own layout.
        const float v1[48] = { 1 }, v2[48] = { 2 }, v0[48] = { 0 };   // TARGETMULTI: 11 rows; TARGETUP (fix 1r): 12
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 192; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        g_bandSent = -1.0f;   // written with the band line on first use
        D3D11_SUBRESOURCE_DATA sd{};
        sd.pSysMem = v1; if (!g_cbFlat && FAILED(dev->CreateBuffer(&bd, &sd, &g_cbFlat))) g_cbFlat = nullptr;
        sd.pSysMem = v2; if (!g_cbDepth && FAILED(dev->CreateBuffer(&bd, &sd, &g_cbDepth))) g_cbDepth = nullptr;
        sd.pSysMem = v0; if (!g_cbAll && FAILED(dev->CreateBuffer(&bd, &sd, &g_cbAll))) g_cbAll = nullptr;
        dev->Release();
    }

    void layer_begin()
    {
        g_mrCurN = 0;   // MARKREC
        g_hBound = false; g_hCleared = false; g_scopeSubs = 0; g_reqValid = false; g_reqBlendValid = false;
        g_splitDraws = 0;
        if (!g_scopeLayer) return;
        split_check();
        if (g_splitReady == 1 && g_splitWant)
        {
            split_buffers(g_gameCtx);
            float th = 0.0f, tv = 0.0f;
            if (g_squashWant) akvr_xr_game_tan(th, tv);
            // BANDOFF 2026-10-01 — JJ: the grapple reticle "splits into its various elements when it reaches the edge
            // of the field of view". All four F2s had it at the TOP of the HUD (part y -6835..-8997 twips of a 7680
            // half-height), inside the 28% compass band, which pushes pieces out of the 3D picture onto the layer.
            // The band predates ROOMALL: with the whole HUD in the room every other part is marked for the room
            // anyway, so the band only ever caught the "stays on its target" parts. Band only without ROOMALL.
            const float band = akvr_hud_room_all() ? 0.0f : g_bandPct;
            // TARGETDEPTH (fix step 1k): rows 1 and 2 = the "stays on its target" parts' points (clip x, y, radius,
            // 1 = on), live every frame. Only with the whole HUD in the room - the shaders pull every unmarked piece
            // near a point into the 3D picture, which is right only when everything else is marked for the room.
            // TARGETMULTI (fix step 1p) 2026-10-02: up to 8 points (world-marker lists give one per visible marker): points
            // 1-2 in rows 1-2, points 3-8 in rows 5-10; row 4 = the movie-layout map (1n), row 3 unused (the shaders take the
            // depth point through the map). The one-frame marker shift (FRAMEPAIR) is gone since THREADSYNC.
            float tp[16] = {}, toff[16] = {}, txf[4] = {};
            const int nTp = (akvr_hud_room_all() && g_targetWant && g_Hw > 0 && g_Hh > 0)
                          ? akvr_hud_target_points(tp, 8, g_Hw, g_Hh, toff, txf) : 0;
            g_tgtN = nTp; memcpy(g_tgtXY, tp, sizeof(g_tgtXY));
            float rows[44] = {};                                   // cb13 rows 1-11
            for (int k = 0; k < nTp; ++k)
            {
                const int r = k < 2 ? k + 1 : k + 3;               // 1, 2, 5, 6, ... 10
                float* q = rows + (r - 1) * 4;
                // TARGETDIST (fix step 1s): .w = the point's view depth in world units when known (> 1.5), else 1 = on
                q[0] = tp[k * 2]; q[1] = tp[k * 2 + 1]; q[2] = g_targetRadius; q[3] = toff[k] > 1.5f ? toff[k] : 1.0f;
            }
            g_tgtOff[0] = g_tgtOff[1] = g_tgtOff[2] = g_tgtOff[3] = 0.0f;
            g_framePx = g_framePy = 0.0f;
            // TARGETSCALE (fix step 1n): row 4 moves the pieces themselves, per vertex, in the same frame as their draw
            for (int k = 0; k < 4; ++k) g_tgtXf[k] = (nTp > 0 && g_markerHead) ? txf[k] : 0.0f;
            rows[12] = g_tgtXf[0]; rows[13] = g_tgtXf[1]; rows[14] = g_tgtXf[2]; rows[15] = g_tgtXf[3];
            // TARGETFACE (fix step 1q) 2026-10-02 — JJ: the reticle and the distance marker "turn on their y-axis as you
            // turn your head". Row 3 = the game frame's tan half-angles + on: the shaders lay each on-target piece on the
            // plane square to the line of sight to its point. Its own row, so RETSQUASH (row 0 .zw) stays off.
            if (nTp > 0 && g_markerHead && g_markerFace)
            {
                float fh = 0.0f, fv = 0.0f;
                akvr_xr_game_tan(fh, fv);
                if (fh > 0.0f && fv > 0.0f) { rows[8] = fh; rows[9] = fv; rows[10] = 1.0f; }
                // TARGETUP (fix step 1r) 2026-10-02 — JJ: the distance marker "is still rotating on the z-axis when rolling
                // your head". Row 11 = the world's up in the drawn camera's frame (x right, y up, z forward): the shaders
                // keep each on-target piece's up along it, so the marker stays upright in the world when the head rolls.
                // Level camera = (0, 1, 0) = 1q. The camera recorded one Present back (the HUD's frame; roll is slow).
                float cf[3], cr[3], cu[3], cp[3];
                const bool upOk = g_markerUpLag <= 0 ? akvr_camera_live_axes(cf, cr, cu)
                                                     : akvr_camera_pose_ago(g_markerUpLag - 1, cf, cr, cu, cp);   // TARGETUP2
                if (upOk) { rows[40] = cr[2]; rows[41] = cu[2]; rows[42] = cf[2]; }
                else rows[41] = 1.0f;
            }
            if (g_cbFlat && g_cbDepth && (g_bandSent != band || memcmp(rows, g_tgtSent, sizeof(rows)) != 0 ||
                                          fabsf(th - g_tanSent[0]) > 1e-4f || fabsf(tv - g_tanSent[1]) > 1e-4f))
            {   // .y = the band line in clip y (0 = no band); .zw = tan half-angles (0 = no squash fix)
                const float y = band > 0.5f ? 1.0f - 2.0f * band / 100.0f : 0.0f;
                float f1[48] = { 1, y, th, tv }, f2[48] = { 2, y, th, tv };   // 12 rows = the buffer's 192 bytes
                memcpy(f1 + 4, rows, sizeof(rows)); memcpy(f2 + 4, rows, sizeof(rows));
                g_gameCtx->UpdateSubresource(g_cbFlat, 0, nullptr, f1, 0, 0);
                g_gameCtx->UpdateSubresource(g_cbDepth, 0, nullptr, f2, 0, 0);
                g_bandSent = band; g_bottomSent = g_bottomPct; g_tanSent[0] = th; g_tanSent[1] = tv;
                memcpy(g_tgtSent, rows, sizeof(rows));
            }
            g_cb13Orig = nullptr;
            g_gameCtx->VSGetConstantBuffers(13, 1, &g_cb13Orig);   // released at the end, after restoring it
        }
        // What is bound now, in case the HUD draws without binding anything first.
        ID3D11BlendState* bs = nullptr; FLOAT f[4] = { 1, 1, 1, 1 }; UINT m = 0xffffffff;
        g_gameCtx->OMGetBlendState(&bs, f, &m);
        g_reqBlend = bs; g_reqMask = m; g_reqBlendValid = true;
        for (int i = 0; i < 4; ++i) g_reqFactor[i] = f[i];
        if (bs) bs->Release();                        // still bound, so still alive
    }
    void layer_end()
    {
        if (!g_scopeLayer) return;
        if (g_hBound)
        {   // hand the game back its own target and blend, so nothing after the HUD lands in H
            if (g_reqValid) G::oOMSetRT(g_gameCtx, g_reqN, g_reqRtv, g_reqDsv);
            if (g_reqBlendValid) G::oBlend(g_gameCtx, g_reqBlend, g_reqFactor, g_reqMask);
            g_hBound = false;
            ++g_restores;
        }
        if (g_splitReady == 1 && g_splitWant && g_cbFlat)
        {   // the game's own slot 13 back
            g_gameCtx->VSSetConstantBuffers(13, 1, &g_cb13Orig);
            if (g_cb13Orig) { g_cb13Orig->Release(); g_cb13Orig = nullptr; }
        }
        g_splitDrawsRep = g_splitDraws;
        g_layerOnlyRep = g_layerOnlyDraws; g_layerOnlyDraws = 0;   // PSMARK
        if (g_mrCurN > 0) { memcpy(g_mrRep, g_mrCur, sizeof(MarkRow) * g_mrCurN); g_mrRepN = g_mrCurN; g_mrRepFrame = g_frame; }   // MARKREC
        if (g_scopeSubs > 0) { g_lastSubFrame = g_frame; ++g_layerFrames; }
        g_subsRep = g_scopeSubs;
    }

    // AUDIOSYNC3: the last screen UI call - its movie count and draws (the menu being drawn at all)
    volatile int g_uiMovies = 0; volatile LONG g_uiDraws = 0; volatile ULONGLONG g_uiAt = 0;
    void hkRenderUI(void* engine, void* params)
    {
        int scene = -1, movies = -1;
        __try { scene = *(int*)((char*)params + 0x10); movies = *(int*)((char*)params + 0x8); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        InterlockedIncrement(&g_calls);
        // PANELTIDY5 2026-10-11: the hide-the-HUD test (akvr_hudsplit_hide_set, no caller) removed.
        AcquireSRWLockExclusive(&g_lock);
        if (g_curN < 4)
        {
            Scope& s = g_cur[g_curN++];
            memset(&s, 0, sizeof(s));
            s.scene = scene; s.movies = movies; s.tid = GetCurrentThreadId();
        }
        ReleaseSRWLockExclusive(&g_lock);
        g_scopeLayer = g_layerWant && g_layerGate && scene == 0 && g_gameCtx && g_gameState == 1 && g_Hreal != 0 &&
                       G::oOMSetRT && G::oBlend && GetCurrentThreadId() == g_presentTid;
        layer_begin();
        g_scopeTid = GetCurrentThreadId();
        const LONG d0 = g_cDraw[0];
        g_orig(engine, params);
        if (scene == 0) { g_uiMovies = movies; g_uiDraws = g_cDraw[0] - d0; g_uiAt = GetTickCount64(); }   // AUDIOSYNC3
        g_scopeTid = 0;
        layer_end();
        g_scopeLayer = false;
        AcquireSRWLockExclusive(&g_lock);
        if (Scope* s = cur_scope()) s->subs = g_scopeSubs;
        ReleaseSRWLockExclusive(&g_lock);
    }

    bool hook(void* target, void* detour, void** orig)
    {
        return MH_CreateHook(target, detour, orig) == MH_OK && MH_EnableHook(target) == MH_OK;
    }
    // Hook vt[slot] unless it is the same code as ref[slot] (already hooked there).
    // Returns 1 hooked, 0 shared, -1 failed.
    int hook_slot(void** vt, void** ref, int slot, void* detour, void** orig)
    {
        if (ref && vt[slot] == ref[slot]) return 0;
        return hook(vt[slot], detour, orig) ? 1 : -1;
    }
    // Hook the draw/bind/list slots of vt on layer L; ref = table already covered. Returns
    // number hooked, or -1 if any failed.
    template <int L> int hook_table(void** vt, void** ref, bool binds, bool exec, bool finish, bool blend)
    {
        using D = Det<L>;
        int r[10] = {
            hook_slot(vt, ref, 12, (void*)&D::DrawIndexed, (void**)&D::oDrawIndexed),
            hook_slot(vt, ref, 13, (void*)&D::Draw, (void**)&D::oDraw),
            hook_slot(vt, ref, 20, (void*)&D::DrawIdxInst, (void**)&D::oDrawIdxInst),
            hook_slot(vt, ref, 21, (void*)&D::DrawInst, (void**)&D::oDrawInst),
            binds ? hook_slot(vt, ref, 33, (void*)&D::OMSetRT, (void**)&D::oOMSetRT) : 0,
            binds ? hook_slot(vt, ref, 34, (void*)&D::OMSetRTUAV, (void**)&D::oOMSetRTUAV) : 0,
            blend ? hook_slot(vt, ref, 35, (void*)&D::Blend, (void**)&D::oBlend) : 0,
            blend ? hook_slot(vt, ref, 11, (void*)&D::VSSet, (void**)&D::oVSSet) : 0,
            exec ? hook_slot(vt, ref, 58, (void*)&D::Exec, (void**)&D::oExec) : 0,
            finish ? hook_slot(vt, ref, 114, (void*)&D::Finish, (void**)&D::oFinish) : 0 };
        int n = 0;
        for (int v : r) { if (v < 0) return -1; n += v; }
        return n;
    }

    void module_of(const void* p, char* out, size_t cap)
    {
        HMODULE m = nullptr;
        out[0] = 0;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCWSTR)p, &m) || !m) { strcpy_s(out, cap, "(no module)"); return; }
        char path[MAX_PATH] = "";
        GetModuleFileNameA(m, path, MAX_PATH);
        const char* b = strrchr(path, '\\');
        strcpy_s(out, cap, b ? b + 1 : path);
    }

    // HUDSPLIT3: the game-facing context, through geo-11's swapchain wrapper.
    bool get_game_ctx(void* wrapper, ID3D11DeviceContext** out)
    {
        __try
        {
            ID3D11Device* dev = nullptr;
            if (FAILED(((IDXGISwapChain*)wrapper)->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev) return false;
            dev->GetImmediateContext(out);
            dev->Release();
            return *out != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void try_game_side()
    {
        if (g_gameState != 0 || !g_ctx) return;
        void* w = akvr_geo11_wrapper();
        if (!w) return;                                   // geo-11 not seen yet: try next frame
        ID3D11DeviceContext* gc = nullptr;
        if (!get_game_ctx(w, &gc)) { g_gameState = -1; return; }
        void** gvt = *(void***)gc;
        void** rvt = *(void***)g_ctx;
        module_of(gvt[12], g_gameModule, sizeof(g_gameModule));
        if (gvt[12] == rvt[12]) { g_gameState = 2; gc->Release(); return; }   // geo-11 handed back the real one
        const int n = hook_table<0>(gvt, nullptr, true, true, true, true);
        // MARKREC: constant-buffer traffic on the game context (slots 7 VSSetConstantBuffers, 14 Map, 15 Unmap, 48 UpdateSubresource)
        hook_slot(gvt, nullptr, 7, (void*)&hkVSSetCB, (void**)&oVSSetCB);
        hook_slot(gvt, nullptr, 16, (void*)&hkPSSetCB, (void**)&oPSSetCB);   // PSMARK
        hook_slot(gvt, nullptr, 9, (void*)&hkPSSetG, (void**)&oPSSetG);      // DRAWPROBE: PSSetShader
        // PANELTIDY5 2026-10-11: the probe-only hooks (PS textures, indirect draws, RAINWRITER's CS views / copies) removed.
        hook_slot(gvt, nullptr, 41, (void*)&hkDispatchG, (void**)&oDispatchG);  // FARRAIN: Dispatch
        hook_slot(gvt, nullptr, 42, (void*)&hkDispatchIndG, (void**)&oDispatchIndG);   // FARRAIN: DispatchIndirect
        hook_slot(gvt, nullptr, 69, (void*)&hkCSSetG, (void**)&oCSSetG);        // FARRAIN: CSSetShader
        hook_slot(gvt, nullptr, 14, (void*)&hkMapG, (void**)&oMapG);
        hook_slot(gvt, nullptr, 15, (void*)&hkUnmapG, (void**)&oUnmapG);
        hook_slot(gvt, nullptr, 48, (void*)&hkUpdSubG, (void**)&oUpdSubG);
        g_gameHooked = n;
        g_gameState = n < 0 ? -2 : 1;
        g_gameCtx = gc;                                   // keeps our reference: lives as long as the game
    }

    // HUD-004 proof: how much of H is covered, solid, and partly transparent.
    void coverage_tick()
    {
        if (!g_Hsrc || !g_ctx || g_Hreal != 1 || !hsrc_refresh()) return;   // MENUCRASH
        if (g_stageState == 1 && g_frame - g_stageFrame >= 2)
        {
            D3D11_MAPPED_SUBRESOURCE m{};
            HRESULT hr = g_ctx->Map(g_stage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
            g_stageState = 0;
            if (FAILED(hr)) return;
            long any = 0, solid = 0, part = 0, tot = 0;
            const bool bgra = g_Hfmt >= 87 && g_Hfmt <= 93;   // alpha is byte 3 in both 8-bit orders
            (void)bgra;
            for (int y = 0; y < g_Hh; y += 4)
            {
                const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
                for (int x = 0; x < g_Hw; x += 4)
                {
                    const uint8_t a = row[x * 4 + 3];
                    ++tot;
                    if (a) ++any;
                    if (a == 255) ++solid; else if (a) ++part;
                }
            }
            if (g_shotWant == 2) { save_layer_bmp(m); g_shotWant = 0; }   // LAYERSHOT
            g_ctx->Unmap(g_stage, 0);
            if (tot) { g_covAny = 100.0f * any / tot; g_covSolid = 100.0f * solid / tot; g_covPart = 100.0f * part / tot; }
            return;
        }
        const bool shot = g_shotWant == 1;   // LAYERSHOT: F2 asked for the layer image
        if (g_stageState == 0 && shot && g_frame - g_lastSubFrame <= 1)   // PANELTIDY5 2026-10-11: F2 only (the 90-frame test read-back removed)
        {
            const int bpp = (g_Hfmt >= 27 && g_Hfmt <= 32) || (g_Hfmt >= 87 && g_Hfmt <= 93) ? 4 : 0;
            if (!bpp) return;                              // only 8-bit RGBA / BGRA are counted
            if (!g_stage)
            {
                ID3D11Device* dev = nullptr; g_ctx->GetDevice(&dev);
                if (!dev) return;
                D3D11_TEXTURE2D_DESC td{};
                td.Width = (UINT)g_Hw; td.Height = (UINT)g_Hh; td.MipLevels = 1; td.ArraySize = 1;
                td.Format = (DXGI_FORMAT)g_Hfmt; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_STAGING;
                td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_stage))) g_stage = nullptr;
                dev->Release();
                if (!g_stage) return;
            }
            g_ctx->CopySubresourceRegion(g_stage, 0, 0, 0, 0, g_Hsrc, 0, nullptr);   // slice 0 (HUDLAYER3)
            g_stageState = 1; g_stageFrame = g_frame;
            if (shot) g_shotWant = 2;
        }
    }
}

bool akvr_hudsplit_active()
{
    const DWORD t = g_scopeTid;
    return t != 0 && t == GetCurrentThreadId();
}

void akvr_hudsplit_install(ID3D11DeviceContext* ctx)
{
    if (g_state != 0 || !ctx) return;
    g_ctx = ctx;
    MH_Initialize();
    const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    void* fn = (void*)(base + (kRenderUI - 0x140000000));
    bool same = false;
    __try { same = memcmp(fn, kSig, sizeof(kSig)) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { same = false; }
    if (!same) g_state = -1;
    else g_state = hook(fn, (void*)&hkRenderUI, (void**)&g_orig) ? 1 : -2;

    // Real immediate context: draws + ExecuteCommandList (binds come from hooks.cpp's observer).
    void** vt = *(void***)ctx;
    g_drawState = hook_table<1>(vt, nullptr, false, true, false, false) == 5 ? 1 : -2;
    hook_slot(vt, nullptr, 11, (void*)&Det<1>::VSSet, (void**)&Det<1>::oVSSet);   // VSID2

    // Real deferred contexts: a throwaway one of our own, only to read its function table.
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    ID3D11DeviceContext* dc = nullptr;
    if (dev && SUCCEEDED(dev->CreateDeferredContext(0, &dc)) && dc)
    {
        void** dvt = *(void***)dc;
        const int n = hook_table<2>(dvt, vt, true, false, true, false);
        hook_slot(dvt, vt, 11, (void*)&Det<2>::VSSet, (void**)&Det<2>::oVSSet);   // VSID2 (if its own code)
        g_defHooked = n;
        g_defState = n < 0 ? -2 : (n > 1 ? 1 : 2);
        dc->Release();
    }
    else g_defState = -1;
    if (dev) dev->Release();
}

// Called from hooks.cpp's real immediate-context OMSetRenderTargets / ...AndUnorderedAccessViews hooks.
void akvr_hudsplit_note_bind(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* const* rtvs, unsigned n)
{
    if (n == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) return;
    note_bind(ctx, rtvs, n, 1);
}

static void marks_write(const wchar_t* path);   // WORLDCARD (below)
void akvr_hudsplit_tick()
{
    g_presentTid = GetCurrentThreadId();
    ++g_frame;
    if (g_mrRecLeft > 0 && InterlockedDecrement(&g_mrRecLeft) == 0 && g_mrPath[0]) { marks_write(g_mrPath); g_mrPath[0] = 0; }   // WORLDCARD
    try_game_side();
    coverage_tick();
    for (int l = 0; l < 3; ++l)
    {
        g_rDraw[l] = InterlockedExchange(&g_cDraw[l], 0);
        g_rBind[l] = InterlockedExchange(&g_cBind[l], 0);
        g_rFinish[l] = InterlockedExchange(&g_cFinish[l], 0);
        g_rExec[l] = InterlockedExchange(&g_cExec[l], 0);
    }
    AcquireSRWLockExclusive(&g_lock);
    // One row per frame even with no HUD call (census), plus one per extra call.
    const int rows = g_curN > 0 ? g_curN : 1;
    for (int i = 0; i < rows; ++i)
    {
        Row& r = g_ring[g_ringHead];
        memset(&r, 0, sizeof(r));
        r.frame = g_frame; r.idx = i < g_curN ? i : -1;
        for (int l = 0; l < 3; ++l)
        { r.draw[l] = g_rDraw[l]; r.bind[l] = g_rBind[l]; r.fin[l] = g_rFinish[l]; r.exe[l] = g_rExec[l]; }
        if (i < g_curN) { r.s = g_cur[i]; g_rep[i] = g_cur[i]; }
        g_ringHead = (g_ringHead + 1) % kRing;
        if (g_ringCount < kRing) ++g_ringCount;
    }
    g_repN = g_curN; g_curN = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

void akvr_hudsplit_layer_set(bool on) { g_layerWant = on; }
bool akvr_hudsplit_layer() { return g_layerWant; }
void akvr_hudsplit_layer_gate(bool gameplay, bool menu) { g_layerGate = gameplay || menu; g_layerMenu = menu && !gameplay; }

void akvr_hudsplit_source_lost() { g_Hsrc = nullptr; g_Hreal = 0; ++g_HsrcLost; }   // MENUCRASH: xr.cpp caught a fault on it
// AKN M5: engine stereo - the HUD picture comes from AKN (its HUD pass redirected into its own texture)
bool akvr_xr_akn_fresh();   // xr.cpp: this Present carries a fresh 3D pair
long g_aknHudHeld = 0;    // HUDHOLD: frames shown with the previous (complete) HUD sheet
static long g_aknHudSeq = -1; static unsigned g_aknHudFrame = 0; static bool g_aknHudAny = false;
bool akvr_hudsplit_layer_live()
{
    if (akvr_engine_stereo()) return g_aknHudAny && g_frame - g_aknHudFrame <= 2;
    return g_layerWant && g_H && g_Hreal == 1 && g_frame - g_lastSubFrame <= 2;
}
ID3D11Texture2D* akvr_hudsplit_layer_image(unsigned& w, unsigned& h, int& fmt)
{
    if (akvr_engine_stereo())
    {
        long seq = 0;
        ID3D11Texture2D* t = akn_get_hud(&seq);
        // HUDHOLD 2026-10-08 (JJ: HUD elements flicker - not the compass, which is drawn in the 3D picture, not on the sheet):
        // a frame with a fresh 3D pair but no NEW HUD sheet (the HUD a frame behind) went out with no HUD layer at all.
        // The sheet still holds the last complete HUD then (it is cleared only when the next one starts): show it.
        // Flat frames still get none (their HUD is in the flat picture).
        if (!t) return nullptr;
        if (seq == g_aknHudSeq) { if (!akvr_xr_akn_fresh()) return nullptr; ++g_aknHudHeld; }
        g_aknHudSeq = seq; g_aknHudFrame = g_frame; g_aknHudAny = true;
        D3D11_TEXTURE2D_DESC d; t->GetDesc(&d);
        w = d.Width; h = d.Height; fmt = (int)d.Format;
        return t;
    }
    // Fresh = the HUD went into H for the frame being presented now (tick runs before submit).
    if (!g_layerWant || !g_H || g_frame - g_lastSubFrame > 1) return nullptr;
    if (!hsrc_refresh()) return nullptr;   // MENUCRASH: may search again after a loss   // MENUCRASH: follow geo-11 if it rebuilt the texture
    w = (unsigned)g_Hw; h = (unsigned)g_Hh; fmt = g_Hfmt;
    return g_Hsrc;
}

static const char* ctx_kind(int t) { return t == 0 ? "immediate" : (t == 1 ? "deferred" : "?"); }

const char* akvr_hudsplit_layer_diag()
{
    static char d[1400];   // TARGETANCHOR: + every target point
    if (!g_layerWant) { _snprintf_s(d, sizeof(d), _TRUNCATE, "HUD layer: off (HUD painted into the picture)"); return d; }
    const char* why = g_state != 1 ? "HUD draw not hooked" : (g_gameState != 1 ? "game-side context not hooked"
                    : (g_Hreal == 0 ? "OFF - no real image found inside geo-11's stand-in"
                    : (!g_layerGate ? "waiting for gameplay" : (g_Herr ? (g_Herr == 1 ? "could not make the HUD image" : "could not make its view")
                    : (g_frame - g_lastSubFrame > 1 ? "HUD not redirected this frame" : "LIVE")))));
    if (g_covAny < 0)
        _snprintf_s(d, sizeof(d), _TRUNCATE, "HUD layer: %s; image %dx%d f%d (%s; real: %s; geo-11 rebuilt it %ld/%ld); %ld swaps last call, %ld frames, restores %ld, blend copies %d (special %d, fails %d)",
                    why, g_Hw, g_Hh, g_Hfmt, g_Hmodule, g_Hfound, g_HsrcMoves, g_HsrcLost, g_subsRep, g_layerFrames, g_restores, g_bvN, g_bvOther, g_bvFail);
    else
        _snprintf_s(d, sizeof(d), _TRUNCATE, "HUD layer: %s; image %dx%d f%d (real: %s); %ld swaps last call, %ld frames, restores %ld, blend copies %d (special %d, fails %d); "
                    "covered %.2f%% (solid %.2f%%, see-through %.2f%%)",
                    why, g_Hw, g_Hh, g_Hfmt, g_Hfound, g_subsRep, g_layerFrames, g_restores, g_bvN, g_bvOther, g_bvFail, g_covAny, g_covSolid, g_covPart);
    {   // TARGETDEPTH: the on-target points sent last frame (clip x, y)
        const size_t len = strlen(d);
        _snprintf_s(d + len, sizeof(d) - len, _TRUNCATE, " | target points %d: (%.3f, %.3f) (%.3f, %.3f) r %.2f, markers on target %s, depth point move (%.3f, %.3f) (%.3f, %.3f), layout map k-1 %.3f / %.3f, b %.3f / %.3f | frame pair %s (rain/markers at head pose delay %ld), marker shift %.4f / %.4f",
                    g_tgtN, g_tgtXY[0], g_tgtXY[1], g_tgtXY[2], g_tgtXY[3], g_targetRadius, g_markerHead ? "ON" : "off",
                    g_tgtOff[0], g_tgtOff[1], g_tgtOff[2], g_tgtOff[3], g_tgtXf[0], g_tgtXf[1], g_tgtXf[2], g_tgtXf[3],
                    g_frameFix ? "ON" : "off", (long)g_frameWorld, g_framePx, g_framePy);
        for (int i = 0; i < g_tgtN && i < 8; ++i)   // TARGETANCHOR: every point (the single parts are the last ones)
        {
            const size_t l = strlen(d);
            _snprintf_s(d + l, sizeof(d) - l, _TRUNCATE, "%s(%.3f, %.3f)", i ? " " : " | all points: ", g_tgtXY[i * 2], g_tgtXY[i * 2 + 1]);
        }
        const size_t len3 = strlen(d);
        _snprintf_s(d + len3, sizeof(d) - len3, _TRUNCATE, " | %s", akvr_marker_dist_diag());   // TARGETDIST
    }
    return d;
}

const char* akvr_hudsplit_diag()
{
    static char d[1600];
    int k = 0;
    const char* st = g_state == 1 ? "hooked" : (g_state == -1 ? "OFF - code differs from the analysed game build"
                     : (g_state == -2 ? "OFF - hook failed" : "waiting"));
    const char* ds = g_defState == 1 ? "own code" : (g_defState == 2 ? "same code as immediate"
                     : (g_defState == 0 ? "not tried" : "FAILED"));
    const char* gs = g_gameState == 1 ? "hooked" : (g_gameState == 2 ? "geo-11 returned the REAL context"
                     : (g_gameState == 0 ? "waiting for geo-11" : (g_gameState == -1 ? "no device from wrapper" : "hook FAILED")));
    k += _snprintf_s(d + k, sizeof(d) - k, _TRUNCATE,
                     "HUD split probe: %s; game side %s (%s, %d fns); real deferred %s; HUD calls last frame %d, total %ld",
                     st, gs, g_gameModule, g_gameHooked, ds, g_repN, g_calls);
    k += _snprintf_s(d + k, sizeof(d) - k, _TRUNCATE,
                     "\n      per frame  draws game %ld / real-imm %ld / real-def %ld;  binds %ld / %ld / %ld;"
                     "  lists recorded %ld / %ld / %ld, played %ld / %ld / %ld",
                     g_rDraw[0], g_rDraw[1], g_rDraw[2], g_rBind[0], g_rBind[1], g_rBind[2],
                     g_rFinish[0], g_rFinish[1], g_rFinish[2], g_rExec[0], g_rExec[1], g_rExec[2]);
    for (int i = 0; i < g_repN && k > 0 && k < (int)sizeof(d) - 100; ++i)
    {
        const Scope& s = g_rep[i];
        k += _snprintf_s(d + k, sizeof(d) - k, _TRUNCATE,
                         "\n      HUD call #%d %s movies %d, %s thread; swaps to HUD image %d; lists recorded %d/%d/%d played %d/%d/%d; other-thread draws %d",
                         i, s.scene == 0 ? "screen" : (s.scene > 0 ? "scene-colour" : "?"), s.movies,
                         s.tid == g_presentTid ? "present" : "OTHER", s.subs, s.finishes[0], s.finishes[1], s.finishes[2],
                         s.execs[0], s.execs[1], s.execs[2], s.otherDraws);
        for (int j = 0; j < s.nc && k < (int)sizeof(d) - 90; ++j)
            k += _snprintf_s(d + k, sizeof(d) - k, _TRUNCATE, "\n        %s ctx %p %s%s: binds %d, draws %d (%d with no target seen)",
                             kLayer[s.c[j].layer], s.c[j].ctx, ctx_kind(s.c[j].type), s.c[j].same ? "" : " OTHER-THREAD",
                             s.c[j].binds, s.c[j].draws, s.c[j].drawsNoBind);
        for (int j = 0; j < s.nb && k < (int)sizeof(d) - 70; ++j)
            k += _snprintf_s(d + k, sizeof(d) - k, _TRUNCATE, "\n        %s target %dx%d f%d arr %d on %s: binds %d draws %d",
                             kLayer[s.b[j].layer], s.b[j].w, s.b[j].h, s.b[j].fmt, s.b[j].arr, ctx_kind(s.b[j].ctxType),
                             s.b[j].binds, s.b[j].draws);
    }
    return d;
}

static void marks_write(const wchar_t* path);
void akvr_hudsplit_marks_dump(const wchar_t* path)
{
    if (!path) return;
    wcsncpy_s(g_mrPath, path, _TRUNCATE);   // WORLDCARD: record 2 HUD frames first, write in akvr_hudsplit_tick
    InterlockedExchange(&g_mrRecLeft, 3);
}
static void marks_write(const wchar_t* path)
{
    FILE* f = nullptr;
    if (!path || _wfopen_s(&f, path, L"w") != 0 || !f) return;
    fprintf(f, "# MARKREC: HUD draws of the last HUD frame (%u). vs = index into the 13 (-1 = not one of them); hits = floats in cb0\n", g_mrRepFrame);
    fprintf(f, "# with |v| in 0.001..0.003 (the part mark is -0.001953), as float index (row = idx/4, component = idx%%4) and value.\n");
    fprintf(f, "# hooks: VSSetCB %d Map %d Unmap %d UpdSub %d, shadows %d\n", oVSSetCB ? 1 : 0, oMapG ? 1 : 0, oUnmapG ? 1 : 0, oUpdSubG ? 1 : 0, g_shN);
    fprintf(f, "# PSMARK: marked draws of the 4 no-colour shaders sent whole to the layer, last frame: %ld\n", g_layerOnlyRep);
    fprintf(f, "draw,vs,vs_hash,split,cb_bytes,hits,idx1,val1,idx2,val2,idx3,val3,idx4,val4,ps_bytes,ps_hits,ps_idx1,ps_val1,ps_idx2,ps_val2,layer_only\n");
    for (int i = 0; i < g_mrRepN; ++i)
    {
        const MarkRow& m = g_mrRep[i];
        fprintf(f, "%d,%d,%016llx,%d,%u,%d", i, m.vs, m.vs >= 0 ? (unsigned long long)kHudVsHash[m.vs] : 0ull, m.split, m.cbSize, m.nHits);
        for (int k = 0; k < 4; ++k)
            if (k < m.nHits) fprintf(f, ",%d,%.6f", m.where[k], m.val[k]); else fprintf(f, ",,");
        fprintf(f, ",%u,%d", m.psSize, m.psHits);
        for (int k = 0; k < 2; ++k)
            if (k < m.psHits) fprintf(f, ",%d,%.6f", m.psWhere[k], m.psVal[k]); else fprintf(f, ",,");
        fprintf(f, ",%d\n", m.layerOnly);
    }
    fclose(f);
    // CBDUMP: the same draws with their full constants (non-zero rows), next to hudmarks.csv as hudcb.txt.
    std::wstring p2(path);
    const size_t dot = p2.rfind(L"hudmarks.csv");
    if (dot == std::wstring::npos) return;
    p2.replace(dot, 12, L"hudcb.txt");
    FILE* g = nullptr;
    if (_wfopen_s(&g, p2.c_str(), L"w") != 0 || !g) return;
    fprintf(g, "CBDUMP frame %u: per HUD draw, VS cb0 and PS cb0 rows that are not all zero (row: x y z w)\n", g_mrRepFrame);
    for (int i = 0; i < g_mrRepN; ++i)
    {
        const MarkRow& m = g_mrRep[i];
        fprintf(g, "\n== draw %d  vs %d (%016llx)  split %d  toH %d  count %u  instances %u  vs_bytes %u  ps_bytes %u\n", i, m.vs,
                m.vs >= 0 ? (unsigned long long)kHudVsHash[m.vs] : 0ull, m.split, m.toH, m.cnt, m.inst, m.cbSize, m.psSize);
        const float* v = (const float*)m.vsData;
        const UINT vr = (m.cbSize < 1024 ? m.cbSize : 1024) / 16;
        for (UINT r = 0; r < vr; ++r)
            if (v[r * 4] || v[r * 4 + 1] || v[r * 4 + 2] || v[r * 4 + 3])
                fprintf(g, "  vs r%-3u %10.6f %10.6f %10.6f %10.6f\n", r, v[r * 4], v[r * 4 + 1], v[r * 4 + 2], v[r * 4 + 3]);
        const float* q = (const float*)m.psData;
        const UINT pr = (m.psSize < 256 ? m.psSize : 256) / 16;
        for (UINT r = 0; r < pr; ++r)
            if (q[r * 4] || q[r * 4 + 1] || q[r * 4 + 2] || q[r * 4 + 3])
                fprintf(g, "  ps r%-3u %10.6f %10.6f %10.6f %10.6f\n", r, q[r * 4], q[r * 4 + 1], q[r * 4 + 2], q[r * 4 + 3]);
    }
    fclose(g);
}

void akvr_hudsplit_layer_shot(const wchar_t* path)
{
    if (!path || g_shotWant) return;
    wcsncpy_s(g_shotPath, path, _TRUNCATE);
    g_shotWant = 1;
}

void akvr_hudsplit_dump(const wchar_t* path)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"w") != 0 || !f) return;
    fprintf(f, "# %s\n# %s\n", akvr_hudsplit_diag(), akvr_hudsplit_layer_diag());
    fprintf(f, "frame,call,draws_game,draws_real_imm,draws_real_def,binds_game,binds_real_imm,binds_real_def,"
               "lists_rec_game,lists_rec_imm,lists_rec_def,lists_play_game,lists_play_imm,lists_play_def,"
               "scene_colour,movies,on_present_thread,other_thread_draws,swaps,"
               "kind,layer,ctx,ctx_type,same_thread,binds,draws,draws_no_target,tex,w,h,fmt,array,bind_flags,misc_flags\n");
    AcquireSRWLockShared(&g_lock);
    const int start = (g_ringHead - g_ringCount + kRing) % kRing;
    for (int n = 0; n < g_ringCount; ++n)
    {
        const Row& r = g_ring[(start + n) % kRing];
        const Scope& s = r.s;
        char pre[256];
        _snprintf_s(pre, sizeof(pre), _TRUNCATE, "%u,%d,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%d,%d,%d,%d,%d",
                    r.frame, r.idx, r.draw[0], r.draw[1], r.draw[2], r.bind[0], r.bind[1], r.bind[2],
                    r.fin[0], r.fin[1], r.fin[2], r.exe[0], r.exe[1], r.exe[2],
                    r.idx >= 0 ? s.scene : -1, r.idx >= 0 ? s.movies : -1,
                    r.idx >= 0 ? (s.tid == g_presentTid ? 1 : 0) : -1, s.otherDraws, s.subs);
        fprintf(f, "%s,%s,,,,,,,,,,,,,,\n", pre, r.idx >= 0 ? "scope" : "frame");
        if (r.idx < 0) continue;
        for (int j = 0; j < s.nc; ++j)
            fprintf(f, "%s,ctx,%s,%p,%d,%d,%d,%d,%d,,,,,,,\n", pre, kLayer[s.c[j].layer], s.c[j].ctx, s.c[j].type,
                    s.c[j].same, s.c[j].binds, s.c[j].draws, s.c[j].drawsNoBind);
        for (int j = 0; j < s.nb; ++j)
            fprintf(f, "%s,target,%s,%p,%d,%d,%d,%d,,%p,%d,%d,%d,%d,0x%x,0x%x\n", pre, kLayer[s.b[j].layer],
                    s.b[j].ctx, s.b[j].ctxType, s.b[j].same, s.b[j].binds, s.b[j].draws, s.b[j].tex, s.b[j].w,
                    s.b[j].h, s.b[j].fmt, s.b[j].arr, s.b[j].bindFlags, s.b[j].misc);
    }
    ReleaseSRWLockShared(&g_lock);
    fclose(f);
}

void akvr_hudsplit_split_set(bool on) { g_splitWant = on; }
bool akvr_hudsplit_split() { return g_splitWant; }
const char* akvr_hudsplit_split_diag()
{
    static char d[400];
    if (!g_splitWant) _snprintf_s(d, sizeof(d), _TRUNCATE, "reticle split: off (everything in the layer)");
    else if (g_splitReady == 0) _snprintf_s(d, sizeof(d), _TRUNCATE, "reticle split: the fix is not patched for it - run AKVR-fix-patches.bat with the game closed");
    else if (g_splitReady < 0) _snprintf_s(d, sizeof(d), _TRUNCATE, "reticle split: waiting for the HUD layer");
    else _snprintf_s(d, sizeof(d), _TRUNCATE, "reticle split: ON, %ld HUD pieces drawn twice last frame%s | %s", g_splitDrawsRep,
                     (g_cbFlat && g_cbDepth) ? "" : " - could not make its switch buffers", akvr_hudsplit_vs_diag());
    return d;
}
void  akvr_hudsplit_band_set(float pct) { g_bandPct = pct < 0.0f ? 0.0f : (pct > 80.0f ? 80.0f : pct); }
float akvr_hudsplit_band() { return g_bandPct; }
void  akvr_hudsplit_bottom_set(float pct) { g_bottomPct = pct < 0.0f ? 0.0f : (pct > 45.0f ? 45.0f : pct); }
float akvr_hudsplit_bottom() { return g_bottomPct; }

// PANELTIDY5 2026-10-11: the rain-hunt exports (DRAWPROBE, RAINWRITER, NEARRAIN, RAINSTRETCH, RAINSPLIT, RAINPARTS,
// NORAIN) and the test-tools switch removed.
bool akvr_far_rain() { return g_farOn != 0; }                            // FARRAIN
void akvr_far_rain_set(bool on) { InterlockedExchange(&g_farOn, on ? 1 : 0); }
const char* akvr_far_rain_diag()
{
    static char d[160];
    _snprintf_s(d, sizeof(d), _TRUNCATE, "rain follows the game camera: %s, bound to %ld rain simulation runs (%ld without camera axes)",
                g_farOn ? "ON" : "off", (long)g_farBinds, (long)g_farNoAxes);
    return d;
}
bool akvr_frame_fix() { return g_frameFix != 0; }                       // FRAMEPAIR
void akvr_frame_fix_set(bool on) { InterlockedExchange(&g_frameFix, on ? 1 : 0); }
int  akvr_frame_world() { return (int)g_frameWorld; }
void akvr_frame_world_set(int k) { InterlockedExchange(&g_frameWorld, k < 1 ? 1 : (k > 4 ? 4 : k)); }   // FRAMEPAIR2: a head pose delay
bool akvr_marker_head() { return g_markerHead; }                        // TARGETMOVE
void akvr_marker_head_set(bool on) { g_markerHead = on; }
bool akvr_marker_face() { return g_markerFace; }                        // TARGETFACE
int  akvr_marker_up_lag() { return g_markerUpLag; }                     // TARGETUP2
void akvr_marker_up_lag_set(int k) { g_markerUpLag = k < 0 ? 0 : (k > 4 ? 4 : k); }
void akvr_marker_face_set(bool on) { g_markerFace = on; }
int  akvr_marker_lag() { return g_markerLag; }
void akvr_marker_lag_set(int k) { g_markerLag = k < -1 ? -1 : (k > 8 ? 8 : k); }
int  akvr_marker_hud_lag() { return g_markerHudLag; }                   // TARGETMOVE2
void akvr_marker_hud_lag_set(int n) { g_markerHudLag = n < 0 ? 0 : (n > 4 ? 4 : n); }

// VSID: called from hooks.cpp's device / swapchain creation hooks, as early as possible (AK creates its shaders at
// load). Hooks CreateVertexShader of each distinct device function table (game-facing wrapper and real device).
void akvr_hudsplit_watch_device(IUnknown* devUnk)
{
    if (!devUnk) return;
    ID3D11Device* dev = nullptr;
    if (FAILED(devUnk->QueryInterface(__uuidof(ID3D11Device), (void**)&dev)) || !dev) return;
    void* fn = (*(void***)dev)[12];
    void* fnPs = (*(void***)dev)[15];   // DRAWPROBE: CreatePixelShader
    void* fnCs = (*(void***)dev)[18];   // DRAWPROBE4: CreateComputeShader
    dev->Release();
    {
        bool seen = false;
        for (int i = 0; i < g_csHooks; ++i) seen = seen || g_createCsTarget[i] == fnCs;
        if (!seen && g_csHooks < 3)
        {
            MH_Initialize();
            void* det = g_csHooks == 0 ? (void*)&hkCreateCS<0> : (g_csHooks == 1 ? (void*)&hkCreateCS<1> : (void*)&hkCreateCS<2>);
            if (MH_CreateHook(fnCs, det, (void**)&oCreateCS[g_csHooks]) == MH_OK && MH_EnableHook(fnCs) == MH_OK)
                g_createCsTarget[g_csHooks++] = fnCs;
        }
    }
    {
        bool seen = false;
        for (int i = 0; i < g_psHooks; ++i) seen = seen || g_createPsTarget[i] == fnPs;
        if (!seen && g_psHooks < 3)
        {
            MH_Initialize();
            void* det = g_psHooks == 0 ? (void*)&hkCreatePS<0> : (g_psHooks == 1 ? (void*)&hkCreatePS<1> : (void*)&hkCreatePS<2>);
            if (MH_CreateHook(fnPs, det, (void**)&oCreatePS[g_psHooks]) == MH_OK && MH_EnableHook(fnPs) == MH_OK)
                g_createPsTarget[g_psHooks++] = fnPs;
        }
    }
    for (int i = 0; i < g_vsHooks; ++i) if (g_createVsTarget[i] == fn) return;
    if (g_vsHooks >= 3) return;
    MH_Initialize();
    void* det = g_vsHooks == 0 ? (void*)&hkCreateVS<0> : (g_vsHooks == 1 ? (void*)&hkCreateVS<1> : (void*)&hkCreateVS<2>);
    if (MH_CreateHook(fn, det, (void**)&oCreateVS[g_vsHooks]) == MH_OK && MH_EnableHook(fn) == MH_OK)
        g_createVsTarget[g_vsHooks++] = fn;
}
// VSID4 2026-09-28 — VSID3 run: "field NOT found". geo-11 does not keep the 3Dmigoto hash per bound shader: its
// own frame-analysis log prints VSSetShader "hash=0000000000000017 / 07 / 2f" (flags, not hashes), so there was
// nothing to find. Instead see the device the GAME talks to at creation: BatmanAK.exe delay-loads d3d11.dll and
// its only d3d11 import is D3D11CreateDevice (delay name table next to CreateDXGIFactory, exe offset 0x3011D72).
// Our entry in that slot calls the export (geo-11's hook runs inside it, as for the game) and hands the device the
// game receives - geo-11's wrapper - to akvr_hudsplit_watch_device. Its CreateVertexShader sees the ORIGINAL
// bytecode and returns the object the game later binds, so the FNV names match the 13 ShaderFixesDM names exactly.
namespace {
    typedef HRESULT (WINAPI* CreateDeviceFn)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                              ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    int  g_gdState = 0;                 // 0 not tried, 1 delay slot, 2 plain import, -1 import not found
    volatile LONG g_gdCalls = 0, g_gdDevices = 0;
    HRESULT WINAPI hkGameCreateDevice(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE sw, UINT fl, const D3D_FEATURE_LEVEL* lv, UINT nlv,
                                      UINT sdk, ID3D11Device** dev, D3D_FEATURE_LEVEL* got, ID3D11DeviceContext** ctx)
    {
        InterlockedIncrement(&g_gdCalls);
        HMODULE m = GetModuleHandleW(L"d3d11.dll");
        if (!m) m = LoadLibraryW(L"d3d11.dll");
        CreateDeviceFn real = m ? (CreateDeviceFn)GetProcAddress(m, "D3D11CreateDevice") : nullptr;   // geo-11's hook sits in it
        if (!real) return E_FAIL;
        HRESULT hr = real(a, t, sw, fl, lv, nlv, sdk, dev, got, ctx);
        if (SUCCEEDED(hr) && dev && *dev) { InterlockedIncrement(&g_gdDevices); akvr_hudsplit_watch_device(*dev); }
        return hr;
    }
    bool swap_slot(void* slot, void* repl)
    {
        DWORD old = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
        *(void**)slot = repl;
        VirtualProtect(slot, sizeof(void*), old, &old);
        return true;
    }
    bool is_create_device(BYTE* base, const IMAGE_THUNK_DATA* n)
    {
        return !IMAGE_SNAP_BY_ORDINAL(n->u1.Ordinal) &&
               strcmp((const char*)((IMAGE_IMPORT_BY_NAME*)(base + n->u1.AddressOfData))->Name, "D3D11CreateDevice") == 0;
    }
    // Name tables of the delay-load descriptors (RVA-based), then the plain import table.
    int patch_d3d11_create(HMODULE mod)
    {
        BYTE* base = (BYTE*)mod;
        auto* dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return -1;
        auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return -1;
        auto& dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
        if (dd.VirtualAddress)
            for (auto* d = (IMAGE_DELAYLOAD_DESCRIPTOR*)(base + dd.VirtualAddress); d->DllNameRVA; ++d)
            {
                if (!d->Attributes.RvaBased || _stricmp((const char*)(base + d->DllNameRVA), "d3d11.dll") != 0) continue;
                auto* iat = (IMAGE_THUNK_DATA*)(base + d->ImportAddressTableRVA);
                for (auto* n = (IMAGE_THUNK_DATA*)(base + d->ImportNameTableRVA); n->u1.AddressOfData; ++n, ++iat)
                    if (is_create_device(base, n)) return swap_slot(&iat->u1.Function, (void*)&hkGameCreateDevice) ? 1 : -1;
            }
        auto& id = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (id.VirtualAddress)
            for (auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + id.VirtualAddress); imp->Name; ++imp)
            {
                if (_stricmp((const char*)(base + imp->Name), "d3d11.dll") != 0) continue;
                auto* iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
                for (auto* n = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
                     n->u1.AddressOfData; ++n, ++iat)
                    if (is_create_device(base, n)) return swap_slot(&iat->u1.Function, (void*)&hkGameCreateDevice) ? 2 : -1;
            }
        return -1;
    }
}
void akvr_hudsplit_hook_game_device()
{
    if (g_gdState != 0) return;
    __try { g_gdState = patch_d3d11_create(GetModuleHandleW(nullptr)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_gdState = -1; }
}
const char* akvr_hudsplit_vs_diag()
{
    static char d[320];
    const int n = hud_names_found();
    const char* how = n > 0 ? "game device names the HUD shaders - only they get depth"
                    : (g_vsHashOff >= 0 ? "geo-11 current-shader field FOUND"
                    : "shaders NOT recognised - drawing all HUD pieces twice");
    const char* gd = g_gdState == 1 ? "delay slot" : (g_gdState == 2 ? "import" : (g_gdState == 0 ? "not tried" : "NOT FOUND"));
    _snprintf_s(d, sizeof(d), _TRUNCATE, "HUD shader check: %s | %d of 13 by name, %d objects; game device hook %s, %ld calls, %ld devices; %ld created, %d hooks",
                how, n, g_hudVsN, gd, g_gdCalls, g_gdDevices, g_vsSeen, g_vsHooks);
    return d;
}
void akvr_hudsplit_squash_set(bool on) { g_squashWant = on; }
bool akvr_hudsplit_squash() { return g_squashWant; }
// AUDIOSYNC3: the screen UI drawn in the last 150 ms: movies and draws of the last call (0 / 0 = none)
void akvr_hudsplit_ui_now(int& movies, int& draws)
{
    const bool fresh = g_uiAt && GetTickCount64() - g_uiAt < 150;
    movies = fresh ? g_uiMovies : 0;
    draws = fresh ? (int)g_uiDraws : 0;
}

// RAINONCE 2026-10-07: AKN names the compute shader bound at a dispatch (the game's shaders are named at creation here)

// DIAGKIT: write one shader's disassembly (by its name) to a file; 1 = written
extern "C" __declspec(dllexport) int akvr_shader_save(unsigned long long h, const wchar_t* path)
{
    std::vector<uint8_t> code;
    AcquireSRWLockShared(&g_shCodeLock); auto it = g_shCode.find(h); if (it != g_shCode.end()) code = it->second; ReleaseSRWLockShared(&g_shCodeLock);
    if (code.empty() || !path) return 0;
    typedef HRESULT(WINAPI* DisFn)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob**);
    static DisFn dis = nullptr; if (!dis) { HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll"); dis = m ? (DisFn)GetProcAddress(m, "D3DDisassemble") : nullptr; }
    ID3DBlob* txt = nullptr; if (!dis || FAILED(dis(code.data(), code.size(), 0, nullptr, &txt)) || !txt) return 0;
    FILE* f = _wfopen(path, L"wb"); if (f) { fwrite(txt->GetBufferPointer(), 1, txt->GetBufferSize() > 0 ? txt->GetBufferSize() - 1 : 0, f); fclose(f); }
    txt->Release();
    return f ? 1 : 0;
}
extern "C" __declspec(dllexport) unsigned long long akvr_shader_hash(const void* obj) { return sh_get(obj); }

// DLSS D0 2026-10-08: Luma's shader names (CRC-32) for AKN - the SMAA passes Luma swaps for DLSS
extern "C" __declspec(dllexport) unsigned akvr_shader_crc(const void* obj) { return sh_get_crc(obj); }
extern "C" __declspec(dllexport) const char* akvr_flare_diag() { return g_flareDiag; }   // FLAREOFF3 (AKN logs it)
void akvr_flareoff_set(int v) { g_flareOff = v ? 1 : 0; }   // FLAREOFF3: settings flareoff=

// SUITGLOSS3: does this pixel shader take its gloss from the camera's aim?
extern "C" __declspec(dllexport) int akvr_shader_camdir(const void* obj) { return vr_has(obj) ? 1 : 0; }   // SUITGLOSS3
extern "C" __declspec(dllexport) int akvr_shader_lightdir(const void* obj) { return ld_has(obj) ? 1 : 0; }   // LIGHTDIR
extern "C" __declspec(dllexport) int akvr_vs_one_piece(const void* obj) { return v1_has(obj) ? 1 : 0; }   // MKSINGLE
extern "C" __declspec(dllexport) long akvr_vs_one_piece_count() { return c_v1Found; }
// MKBATCH / MKTAG: out[32] = x row, y row, rows per piece, rows in cb0, base, w row (-1 = 2D), n piece rows, rows[12],
// steps[12] - all for piece 0 (position translation = .w of the x / y rows)
extern "C" __declspec(dllexport) int akvr_vs_sprite_rows(const void* obj, int* out) { int a = -1, b = -1, w = -1; if (!out || !sp_get(obj, a, b, w)) return 0; out[0] = a; out[1] = b; out[2] = w; return 1; }   // out[3]   // SPRITEAXES (AKN)
extern "C" __declspec(dllexport) int akvr_vs_batch_layout(const void* obj, int* out) { return out && vb_get(obj, out) ? 1 : 0; }
extern "C" __declspec(dllexport) long akvr_vs_batch_count() { return c_vbFound; }

// MKGS: the per-vertex 3D placement geometry shader for this HUD vertex shader (built on first use; null = not supported)
extern "C" __declspec(dllexport) ID3D11GeometryShader* akvr_marker_gs(void* vs) { return mkgs_get(vs); }
extern "C" __declspec(dllexport) long akvr_marker_gs_counts(long* failed) { if (failed) *failed = c_mkGsFail; return c_mkGsBuilt; }
// MKGS2: vertex shaders remembered (supported / unsupported signature), marker lookups that found no entry, first build error
extern "C" __declspec(dllexport) const char* akvr_marker_gs_stats(long* noted, long* unsup, long* miss)
{ if (noted) *noted = c_mkGsNoted; if (unsup) *unsup = c_mkGsUnsup; if (miss) *miss = c_mkGsMiss; return g_mkGsErr; }
