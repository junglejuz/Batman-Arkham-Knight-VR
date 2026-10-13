// AKN probe (milestone 1): does Arkham Knight draw a whole frame twice cleanly, and what does it cost?
//
// Static map (akvr_native/re/ak.py, BatmanAK.exe Steam 54,632,960 bytes; RVAs, the exe is ASLR):
//   0xBF8000 FViewport::Draw(this, bShouldPresent)   BeginDrawing / EndDrawing commands, canvas, client Draw
//   0xAD3EA0 UGameEngine::RedrawViewports(this, bShouldPresent)   calls Draw at 0xAD3EE0 (game viewport)
//            called once per frame from the engine tick at 0xAF003F
//   0x9F6220 ULocalPlayer::CalcSceneView (6 args) - also called from Draw's own player loop
//   0x9F87B0 / 0x41BF00 the two viewport-client Draw implementations (vtable slot +0x18)
// Method: batman_city RE_NOTES "The viewport-draw root" (double the root on the per-frame caller only, SEH,
// auto-disarm) and playbook 17 #side-effect-gate. Doubling here draws the SAME view twice: it measures the
// cost and the stability of a second pass, not stereo yet.
//
// Switch: active only with -akn on the command line. Modes (F9 with the game window in front, or -akntest):
//   0 = normal, 1/2 = the world queued twice (BeginRenderingViewFamily 0x81B670 from RA 0x9F1563, run 6).
// -akntest: F10 (game in front) starts 25 s per mode 0,1,0,1, a summary per phase. Log: <game>\akn\akn_log.txt
#include <windows.h>
#include <intrin.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <algorithm>
#include <d3d11.h>
#include <tlhelp32.h>   // HWDET
#include "MinHook.h"
#include "dlss.h"   // DLSS D1
#include <d3dcompiler.h>   // FLAREOFF
#include "flare_shaders.h"   // FLAREOFF: Luma's two AK lens flare shaders

namespace {
uintptr_t g_base = 0;
wchar_t g_dir[MAX_PATH] = {};
FILE* g_log = nullptr;
CRITICAL_SECTION g_cs;

void logf(const char* fmt, ...)
{
    if (!g_log) return;
    EnterCriticalSection(&g_cs);
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
    LeaveCriticalSection(&g_cs);
}

struct Site { uintptr_t ra; volatile LONG n; };
struct Census
{
    Site s[16] = {};
    void add(uintptr_t ra)
    {
        for (auto& e : s)
        {
            if (e.ra == ra) { InterlockedIncrement(&e.n); return; }
            if (e.ra == 0 && InterlockedCompareExchange64((volatile LONG64*)&e.ra, (LONG64)ra, 0) == 0) { InterlockedIncrement(&e.n); return; }
            if (e.ra == ra) { InterlockedIncrement(&e.n); return; }
        }
    }
    void print(const char* name)
    {
        char b[1024]; int k = snprintf(b, sizeof(b), "  callers of %s:", name);
        for (auto& e : s) if (e.ra) k += snprintf(b + k, sizeof(b) - k, " %llX x%ld", (unsigned long long)(e.ra - g_base), e.n);
        logf("%s", b);
    }
};

// ---- hooks ----
using DrawFn = void(__fastcall*)(void*, uint32_t);
using RedrawFn = void(__fastcall*)(void*, uint32_t);
using CalcFn = void*(__fastcall*)(void*, void*, void*, void*, void*, void*);
using ClientDrawFn = void(__fastcall*)(void*, void*, void*);
DrawFn o_Draw = nullptr; RedrawFn o_Redraw = nullptr; CalcFn o_Calc = nullptr;
ClientDrawFn o_CdA = nullptr, o_CdB = nullptr;

volatile LONG c_draw = 0, c_redraw = 0, c_calc = 0, c_cdA = 0, c_cdB = 0, c_second = 0, c_secondFail = 0;
volatile LONG64 t_draw = 0, t_redraw = 0, t_second = 0;   // QPC ticks summed
volatile LONG64 t_drawMax = 0;
Census k_draw, k_redraw, k_calc;
volatile LONG g_mode = 0;          // 0 normal, 1 double (first without present), 2 double (both present)
volatile LONG g_armed = 1;         // cleared by a fault in the second pass
DWORD g_gameTid = 0;
uintptr_t g_raTickRedraw = 0;      // RedrawViewports' return address inside the engine tick
uintptr_t g_raRedrawDraw = 0;      // Draw's return address inside RedrawViewports (the game viewport)
thread_local bool tl_inTickRedraw = false, tl_inSecond = false;
thread_local int tl_calcInPass = 0;
volatile LONG c_calcFirstPass = 0, c_calcSecondPass = 0;

LONG64 qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
void maxto(volatile LONG64& m, LONG64 v) { LONG64 o = m; while (v > o) { LONG64 p = InterlockedCompareExchange64(&m, v, o); if (p == o) break; o = p; } }

Census k_cdA, k_cdB;
volatile LONG64 t_cdA = 0;
volatile LONG g_stacksLeft = 6;
DWORD g_tid[5] = {};   // Draw, Redraw, Calc, A, B
void note_tid(int i) { if (!g_tid[i]) g_tid[i] = GetCurrentThreadId(); }
void stack_once(const char* name)
{
    if (g_stacksLeft <= 0 || InterlockedDecrement(&g_stacksLeft) < 0) return;
    void* fr[12] = {}; const USHORT n = RtlCaptureStackBackTrace(1, 12, fr, nullptr);
    char b[512]; int k = snprintf(b, sizeof(b), "stack into %s (tid %lu):", name, GetCurrentThreadId());
    for (USHORT i = 0; i < n; ++i)
    {
        const uintptr_t a = (uintptr_t)fr[i];
        k += snprintf(b + k, sizeof(b) - k, (a >= g_base && a < g_base + 0x4000000) ? " %llX" : " (%llX)", (unsigned long long)(a >= g_base && a < g_base + 0x4000000 ? a - g_base : a));
    }
    logf("%s", b);
}
int second_pass(void* vp, uint32_t present)
{
    __try { o_Draw(vp, present); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

void __fastcall hk_Draw(void* vp, uint32_t present)
{
    const uintptr_t ra = (uintptr_t)_ReturnAddress();
    k_draw.add(ra); note_tid(0);
    InterlockedIncrement(&c_draw);
    const LONG mode = g_mode;
    if (false && mode && g_armed && !tl_inSecond && tl_inTickRedraw && ra == g_raRedrawDraw)
    {
        tl_inSecond = true; tl_calcInPass = 0;
        const LONG64 a = qpc();
        const int ok = second_pass(vp, mode == 2 ? present : 0);
        InterlockedAdd64(&t_second, qpc() - a);
        InterlockedAdd(&c_calcSecondPass, tl_calcInPass);
        tl_inSecond = false;
        if (ok) InterlockedIncrement(&c_second);
        else { InterlockedIncrement(&c_secondFail); g_armed = 0; logf("SECOND PASS FAULTED - disarmed"); }
    }
    const LONG64 a = qpc();
    o_Draw(vp, present);
    const LONG64 d = qpc() - a;
    InterlockedAdd64(&t_draw, d); maxto(t_drawMax, d);
}
void __fastcall hk_Redraw(void* eng, uint32_t present)
{
    const uintptr_t ra = (uintptr_t)_ReturnAddress();
    k_redraw.add(ra); note_tid(1);
    InterlockedIncrement(&c_redraw);
    if (!g_gameTid) g_gameTid = GetCurrentThreadId();
    const bool tick = ra == g_raTickRedraw;
    const bool prev = tl_inTickRedraw; tl_inTickRedraw = tick;
    const LONG64 a = qpc();
    o_Redraw(eng, present);
    InterlockedAdd64(&t_redraw, qpc() - a);
    tl_inTickRedraw = prev;
}
using SceneFn = void(__fastcall*)(void*);
SceneFn o_Scene = nullptr;
volatile LONG c_scene = 0; volatile LONG64 t_scene = 0;
Census k_scene;
uintptr_t g_raTickScene = 0;
// LIVE 2026-10-06 run 3: the second scene pass ran but the game's own pass then skipped the world. Client Draw
// (0x9F87B0) returns at 0x9F88E3 when CanvasObject (UCanvas at client+0x48, made from "CanvasObject"/"Engine")
// still holds its FCanvas (+0x94): the scene pass sets it and the late HUD pass (0x9F1230, client vtbl+0x18) clears
// it. So the extra pass puts that field back to what it was before it ran.
volatile LONG c_canvasReset = 0;
int second_scene(void* vp)
{
    __try
    {
        uintptr_t uc = 0;
        const uintptr_t client = *(uintptr_t*)((uintptr_t)vp + 0x5C);
        if (client) uc = *(uintptr_t*)(client + 0x48);
        const uint64_t before = uc ? *(uint64_t*)(uc + 0x94) : 0;
        o_Scene(vp);
        if (uc) { *(uint64_t*)(uc + 0x94) = before; InterlockedIncrement(&c_canvasReset); }
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
// LIVE 2026-10-06: the world is drawn here, not in FViewport::Draw (that one only adds the canvas/HUD and presents).
void __fastcall hk_Scene(void* vp)
{
    const uintptr_t ra = (uintptr_t)_ReturnAddress();
    k_scene.add(ra);
    InterlockedIncrement(&c_scene);
    const LONG mode = g_mode;
    if (false && mode && g_armed && !tl_inSecond && ra == g_raTickScene)   // run 4: Scaleform AS3 fatal
    {
        tl_inSecond = true; tl_calcInPass = 0;
        const LONG64 a = qpc();
        const int ok = second_scene(vp);
        InterlockedAdd64(&t_second, qpc() - a);
        InterlockedAdd(&c_calcSecondPass, tl_calcInPass);
        tl_inSecond = false;
        if (ok) InterlockedIncrement(&c_second);
        else { InterlockedIncrement(&c_secondFail); g_armed = 0; logf("SECOND SCENE PASS FAULTED - disarmed"); }
    }
    tl_calcInPass = 0;
    const LONG64 a = qpc();
    o_Scene(vp);
    InterlockedAdd64(&t_scene, qpc() - a);
    InterlockedAdd(&c_calcFirstPass, tl_calcInPass);
}
// RUN 5: the view set up early (client Draw A builds the family + CalcSceneView, scene entry) is rendered late by
// 0x9F1230 (client vtbl+0x18, from FViewport::Draw): 0x8E9E90(world-ish global, family, 0, float, int) at 0x9F1444,
// HUD after it. Doubling that call renders the world twice from the same view; Scaleform stays single.
using RenderFn = void(__fastcall*)(void*, void*, uint64_t, float, uint32_t);
RenderFn o_Render = nullptr;
volatile LONG c_render = 0; volatile LONG64 t_render = 0;
Census k_render;
uintptr_t g_raLateRender = 0;
int second_render(void* a, void* fam, uint64_t c, float f, uint32_t e)
{
    __try { o_Render(a, fam, c, f, e); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
void __fastcall hk_Render(void* a, void* fam, uint64_t c, float f, uint32_t e)
{
    const uintptr_t ra = (uintptr_t)_ReturnAddress();
    k_render.add(ra);
    InterlockedIncrement(&c_render);
    const LONG mode = g_mode;
    if (false && mode && g_armed && !tl_inSecond && ra == g_raLateRender)   // run 5: not the world render (see run 6)
    {
        tl_inSecond = true;
        const LONG64 t0 = qpc();
        const int ok = second_render(a, fam, c, f, e);
        InterlockedAdd64(&t_second, qpc() - t0);
        tl_inSecond = false;
        if (ok) InterlockedIncrement(&c_second);
        else { InterlockedIncrement(&c_secondFail); g_armed = 0; logf("SECOND RENDER FAULTED - disarmed"); }
    }
    const LONG64 t0 = qpc();
    o_Render(a, fam, c, f, e);
    InterlockedAdd64(&t_render, qpc() - t0);
}
// RUN 6 (STATIC 2026-10-07): 0x8E9E90 walks a per-object list and runs ~2x a frame at 0.07 ms - not the world.
// The world is queued by BeginRenderingViewFamily(canvas, family) = 0x81B670, called once from 0x9F155E
// (RA 0x9F1563) with the family client Draw A built (client+0x140): it bumps the frame counter 0x143A3149C,
// news the scene renderer (0x1F0 bytes, ctor 0x818530(family, hit proxies, canvas transform)) and enqueues the
// draw-scene command. The renderer copies the family, so calling it twice queues two full world renders.
using BrvfFn = void(__fastcall*)(void*, void*);
BrvfFn o_Brvf = nullptr;
volatile LONG c_brvf = 0; volatile LONG64 t_brvf = 0;
Census k_brvf;
uintptr_t g_raBrvf = 0;
int second_brvf(void* cv, void* fam)
{
    __try { o_Brvf(cv, fam); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
extern void* volatile g_fam1; extern void* volatile g_fam2; extern volatile LONG g_stereo; void drop_fam2();
extern volatile LONG g_shCacheGen;   // PERF1 (below)
extern int g_mkScratchN;   // MKGLOW2 (below)
void mk_scratch_set(ID3D11Resource* r, bool on, unsigned f);   // MKGLOW5 (below)
extern unsigned g_mkHudFrame;   // MKGLOW5 (below)
extern volatile LONG c_mkGlow, c_mkSticky;   // MKGLOW2 / MKSTICKY2 (below)
// MKDRAWN 2026-10-11 (JJ: the distance marker "jitters, like tiny little jitters"): where the game actually DREW each
// marker's routed pieces in this HUD frame (sheet clip units, bounding box) - AKVR's HUD-tree read races the game thread
// (the next frame's layout can already be in place at Present), so the card was placed up to a frame off its pixels.
struct MkDrawn { float x0, y0, x1, y1; int n; };
MkDrawn g_mkDrawn[8] = {};
void mk_drawn_add(int b, float x, float y) { if (b < 0 || b >= 8 || !(x == x) || !(y == y)) return; MkDrawn& d = g_mkDrawn[b]; if (!d.n) { d.x0 = d.x1 = x; d.y0 = d.y1 = y; } else { if (x < d.x0) d.x0 = x; if (x > d.x1) d.x1 = x; if (y < d.y0) d.y0 = y; if (y > d.y1) d.y1 = y; } ++d.n; }
volatile LONG g_trace10 = 10;
// RANDSYNC 2026-10-07 (JJ, 6th Quest test: "the rain is not the same in each eye"). RAINDUMP, same camera + frame number
// in both eyes: the rain draw (103530 vertices, vs 62c3cb6af9d6796c) gets DIFFERENT transforms per eye (VS b2 c9-c14:
// rotation + offset rows) = random numbers drawn again for the second render. Give both eye renders the same dice:
// bit 1 = UE3's global seed GSRandSeed (0x143A28C24, appSRand inlined 128x), bit 2 = the C runtime's per-thread rand()
// state (msvcr100 _getptd()->_holdrand). Saved before the right eye's render, put back before the left eye's - on the
// game thread (around the right eye's BeginRenderingViewFamily) and on the render thread (eye 1 begin -> eye 0 begin).
volatile LONG g_randSync = 0;
// ONCETIME 2026-10-07 (JJ, 6th Quest test: rain differs per eye; his tip: SWBF2's particles were spawned per view).
// Run 26: same camera in both eyes -> rain differs while PLAYING (1.23) but is identical while PAUSED (0.02) = the rain
// is stepped by game time once PER RENDER: the right eye's render steps it, the left eye's steps it again (playbook
// side-effect gate "advances twice"). The view family carries the frame's elapsed world time: give the second render
// (the game's own, left eye) zero elapsed time, so the rain (and anything else stepped per render) advances once.
volatile LONG g_deltaZero = 0, g_famDump = 0, g_deltaOff = -1, c_deltaZeroed = 0;
// RTWAIT 2026-10-08: run 27 - zero elapsed time for the left render changed nothing (1.55 vs 1.16). Paused = identical, so
// suspect a RACE: the game thread updates the rain for the next frame while the render thread draws the right eye and
// then the left eye. Test: hold the game thread after the left eye's BeginRenderingViewFamily until the render thread
// has finished the left eye's render (bounded wait, 60 ms).
volatile LONG g_rtWait = 0, c_rtWaits = 0, c_rtWaitTimeouts = 0; volatile LONG64 t_rtWait = 0; HANDLE g_rtDone = nullptr;
int delta_offset(const uint8_t* fam)
{   // FSceneViewFamily: ... CurrentWorldTime, DeltaWorldTime, CurrentRealTime: the first small positive float after a
    // larger time value (searched once, logged)
    if (g_deltaOff >= 0) return g_deltaOff;
    for (int o = 0x14; o <= 0x6C; o += 4)
    {
        const float d = *(const float*)(fam + o), t = *(const float*)(fam + o - 4);
        if (d > 0.002f && d < 0.2f && t > 1.0f && t < 1.0e7f) { g_deltaOff = o; logf("ONCETIME: elapsed world time found at family +0x%X (%.4f s, world time %.2f s)", o, d, t); return o; }
    }
    return -1;
}   // run 25: no effect on the rain (0.97 vs 0.88 without) - off by default
using GetPtdFn = void* (__cdecl*)(); using RandFn = int (__cdecl*)();
GetPtdFn p_getptd = nullptr; RandFn p_rand = nullptr; volatile LONG g_ptdState = -1;   // -1 untested, 0 unusable, 1 ok

volatile uint32_t* rand_seed() { return (volatile uint32_t*)(g_base + 0x3A28C24); }   // UE3 GSRandSeed
uint32_t* crt_holdrand()
{
    if (g_ptdState == 0) return nullptr;
    if (!p_getptd)
    {
        HMODULE m = GetModuleHandleW(L"msvcr100.dll");
        p_getptd = m ? (GetPtdFn)GetProcAddress(m, "_getptd") : nullptr; p_rand = m ? (RandFn)GetProcAddress(m, "rand") : nullptr;
        if (!p_getptd || !p_rand) { g_ptdState = 0; logf("RANDSYNC: msvcr100 _getptd/rand not found - C runtime dice not shared"); return nullptr; }
    }
    uint8_t* t = (uint8_t*)p_getptd();
    uint32_t* h = t ? (uint32_t*)(t + 0x1C) : nullptr;
    if (h && g_ptdState < 0)
    {   // self-test: one rand() must advance _holdrand by the CRT's own formula and return its high bits
        const uint32_t h0 = *h; const int r = p_rand(); const uint32_t h1 = *h; *h = h0;
        const bool ok = h1 == h0 * 214013u + 2531011u && r == (int)((h1 >> 16) & 0x7fff);
        g_ptdState = ok ? 1 : 0;
        logf("RANDSYNC: C runtime dice %s (holdrand %08x -> %08x, rand %d)", ok ? "found" : "NOT where expected - not shared", h0, h1, r);
        if (!ok) return nullptr;
    }
    return h;
}
struct RandSave { uint32_t seed, hold; bool hasHold; };
RandSave rand_save() { RandSave r{}; r.seed = *rand_seed(); uint32_t* h = (g_randSync & 2) ? crt_holdrand() : nullptr; r.hasHold = h != nullptr; r.hold = h ? *h : 0; return r; }
void rand_restore(const RandSave& r) { if (g_randSync & 1) *rand_seed() = r.seed; if ((g_randSync & 2) && r.hasHold) if (uint32_t* h = crt_holdrand()) *h = r.hold; }
volatile LONG c_randGame = 0, c_randRender = 0;
volatile LONG g_frameSync = 1;       // command "framesync 0|1"        // M5c: trace the first 10 stereo frames step by step
volatile LONG g_steady = 0;          // M5c crash 2026-10-07: frames in a row at the normal render site, no hitch
ULONGLONG g_lastBrvfMs = 0;
void __fastcall hk_Brvf(void* cv, void* fam)
{
    const uintptr_t ra = (uintptr_t)_ReturnAddress();
    if (ra == g_raBrvf)
    {
        const ULONGLONG now = GetTickCount64();
        if (g_lastBrvfMs && now - g_lastBrvfMs > 250) { if (g_steady >= 30) logf("hitch %llu ms - stereo paused until 30 steady frames", now - g_lastBrvfMs); g_steady = 0; InterlockedIncrement(&g_shCacheGen); }
        else if (g_steady < 1000000) InterlockedIncrement(&g_steady);
        g_lastBrvfMs = now;
    }
    if (g_trace10 > 0) logf("TRACE10 BRVF ra %llX fam %p (fam1 %p fam2 %p) steady %ld", (unsigned long long)(ra - g_base), fam, g_fam1, g_fam2, g_steady);
    k_brvf.add(ra);
    InterlockedIncrement(&c_brvf);
    void* const f2 = g_fam2;
    if (f2 && fam == g_fam1 && ra == g_raBrvf && !tl_inSecond)
    {
        // M3: the right eye's family renders first; its picture is copied out on the render thread (hk_RenderVF)
        tl_inSecond = true;
        const LONG64 t0 = qpc();
        // FRAMESYNC (JJ's 4th Quest test: lighting/reflections differ per eye; same-camera pictures differed only in the
        // rain): BeginRenderingViewFamily bumps two frame counters (0x143A3149C always, 0x143A314A0 on the normal path)
        // that seed per-frame randomness (rain, noise patterns). Put them back after the right eye's render so the left
        // eye's render sees the SAME numbers.
        volatile uint32_t* fc1 = (volatile uint32_t*)(g_base + 0x3A3149C);
        volatile uint32_t* fc2 = (volatile uint32_t*)(g_base + 0x3A314A0);
        const uint32_t f1 = *fc1, f2c = *fc2;
        const RandSave rs = rand_save();   // RANDSYNC: the left eye's render draws the same random numbers
        if (g_famDump > 0)
        {
            InterlockedDecrement(&g_famDump);
            for (int w = 0; w < 2; ++w)
            {
                const uint8_t* b = (const uint8_t*)(w ? f2 : fam); char line[1200]; int o = _snprintf_s(line, sizeof(line), _TRUNCATE, "FAMDUMP %s family:", w ? "right (clone)" : "left (game's)");
                for (int i = 0; i < 0x70 && o > 0; i += 4) o += _snprintf_s(line + o, sizeof(line) - o, _TRUNCATE, " +%02X %08x(%g)", i, *(const uint32_t*)(b + i), *(const float*)(b + i));
                logf("%s", line);
            }
        }
        const int ok = second_brvf(cv, f2);
        if (g_frameSync) { *fc1 = f1; *fc2 = f2c; }
        if (g_randSync) { rand_restore(rs); InterlockedIncrement(&c_randGame); }
        drop_fam2();
        InterlockedAdd64(&t_second, qpc() - t0);
        tl_inSecond = false;
        if (ok) InterlockedIncrement(&c_second);
        else { InterlockedIncrement(&c_secondFail); g_stereo = 0; logf("RIGHT EYE RENDER FAULTED - stereo off"); }
    }
    else if (g_mode && g_armed && !tl_inSecond && ra == g_raBrvf)
    {
        tl_inSecond = true;
        const LONG64 t0 = qpc();
        const int ok = second_brvf(cv, fam);
        InterlockedAdd64(&t_second, qpc() - t0);
        tl_inSecond = false;
        if (ok) InterlockedIncrement(&c_second);
        else { InterlockedIncrement(&c_secondFail); g_armed = 0; logf("SECOND WORLD RENDER FAULTED - disarmed"); }
    }
    const LONG64 t0 = qpc();
    int dOff = -1; float dSave = 0.0f;
    if (g_deltaZero && f2 && fam == g_fam1 && ra == g_raBrvf && !tl_inSecond && fam)
    {   // ONCETIME: the right eye's render already stepped this frame's time
        dOff = delta_offset((const uint8_t*)fam);
        if (dOff >= 0) { float* d = (float*)((uint8_t*)fam + dOff); dSave = *d; *d = 0.0f; InterlockedIncrement(&c_deltaZeroed); }
    }
    if (g_rtWait && f2 && !g_rtDone) g_rtDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_rtWait && f2 && g_rtDone) ResetEvent(g_rtDone);
    o_Brvf(cv, fam);
    if (dOff >= 0) *(float*)((uint8_t*)fam + dOff) = dSave;
    if (g_rtWait && f2 && g_rtDone && fam == g_fam1 && ra == g_raBrvf && !tl_inSecond)
    {   // RTWAIT: until the left eye's render (render thread) is done
        const LONG64 w0 = qpc();
        if (WaitForSingleObject(g_rtDone, 60) == WAIT_TIMEOUT) InterlockedIncrement(&c_rtWaitTimeouts);
        InterlockedIncrement(&c_rtWaits); InterlockedAdd64(&t_rtWait, qpc() - w0);
    }
    InterlockedAdd64(&t_brvf, qpc() - t0);
}
void* stereo_calc(void* p, void* fam, void* loc, void* rot, void* vp, void* drawer);
extern volatile LONG g_stereo;
extern thread_local int tl_eye;
extern volatile LONG g_capWant;
extern int g_afterLogs; extern int g_drawLogs;
extern char g_capTag[16]; extern volatile LONG g_traceWant;
extern volatile LONG g_asym; extern int g_asymLogs; extern float g_tan[2][4];
extern volatile LONG g_lumShare, c_lumCopies;
extern volatile LONG g_rogue, c_rogueOps; DWORD WINAPI rogue_thread(LPVOID);
extern ID3D11Device* volatile g_gameDevHint;
extern volatile LONG g_eyeMark;
extern volatile LONG g_frameSync;
extern volatile LONG g_histSwap, g_histLog, c_histCopies; extern int g_candN; extern float g_eyeShift[2];
extern volatile LONG g_rainOnce, c_rainCs[2][3], c_rainSkip; void akvr_panel_cmd(int on);
extern volatile LONG g_rainDump;   // RAINDUMP (below)
extern volatile unsigned long long g_skipVs, g_skipLo, g_skipHi; extern volatile LONG g_vsInv;   // SKIPVS / VSINV (below)
extern volatile LONG g_xeye;   // XEYE (below)
extern volatile LONG g_flareOff;   // FLAREOFF (below)
void vw_uavs(ID3D11DeviceContext* c, ID3D11UnorderedAccessView* const* u, UINT n, bool deferred); void vw_rtvs(ID3D11DeviceContext* c, ID3D11RenderTargetView* const* v, UINT n, bool deferred); extern ID3D11Resource* g_vwRes; extern ULONGLONG g_vwUntil;   // VOLWATCH (below)
extern volatile unsigned long long g_fxWatch; extern volatile LONG g_lightLag; extern volatile LONG g_fxcbArm; extern volatile LONG g_fxcbHave[2]; extern volatile LONG g_spriteAxes, c_spriteAxes, g_vwArm, g_vcDumps, g_smokeLight, c_smokeLight; extern volatile LONG g_hrOn; extern "C" __declspec(dllexport) void akn_hudring_dump(const char* why, int delayMs); extern volatile LONG g_rainMask, c_rainMask; extern volatile LONG g_rainLight2, c_rainLight2; extern volatile LONG g_rainLight, c_rainLight, g_rainRelight, c_rainRelit, c_rainRelightSkip;   // RAINLIGHT / RAINRELIGHT (below)
extern volatile LONG g_flatLines; extern ULONGLONG g_flatUntil; extern int g_flatSigN; }   // (namespace pause)
extern "C" int akn_detective_active();
namespace {
void hud_rec_dump(const char* tag);   // HUDREC (below)
extern volatile LONG g_pnOn, g_pnArmed, g_pnLines, g_psSkip, c_psSkipped; extern ULONGLONG g_pnArmT; extern volatile ULONGLONG g_pnArmAt; void ps_new_list();   // PSNEW / PSSKIP (below)
extern double g_qpf;   // HOOKBATCH timing (below)
extern volatile LONG g_shCacheGen;   // PERF1: shader-name caches, renewed on load hitches (below)
extern volatile LONG g_histSwap, g_histLog;   // PERF1: the HISTSWAP detector only runs for the test (below)
inline bool h_on() { return g_histSwap || g_histLog > 0; }
extern int g_rtLogs, g_rtListLogs;   // PERF1: render-target bookkeeping only for captures / the first log lines (below)
extern volatile LONG g_lumShare;   // PERF1 (below)
volatile LONG g_sweepWant = 0;   // PERF1: "perfsweep" - one feature off per 8 s phase, pairs/s + graphics-card load each
extern volatile LONG g_mipBias;   // MIPBIAS (below)
void mk_census(const char* tag);   // MK3 (below)
extern volatile LONG g_hudBlend, c_bvSwaps, c_bvOther; extern int g_bvN;   // HUDBLEND (below)
extern volatile LONG g_mkDump, g_mkForce; extern float g_mkForceXY[2];   // MK1 / MK2 (below)
extern volatile LONG g_rainView, g_farRain, g_farRow, g_rainCbDump, g_rainPause, g_nearRain, g_projLog, g_rainDrawDump, g_rainDepth, g_rainDepthLog, g_d0, g_d0Logs; extern volatile LONG g_rdHave[2];   // RAINVIEW / FARRAIN2 (below)   // RAINONCE / PANELTEST (below)
extern volatile LONG g_hudOn; extern volatile bool g_hudPhase; extern volatile LONG g_capHud;
extern volatile bool g_camOk;
extern volatile unsigned long long g_famStamp; unsigned long long fin_now();   // POSESTAMP (defined with the renderer tags)
extern volatile LONG g_steady; extern volatile LONG g_trace10;
uintptr_t g_raCalcA = 0;   // CalcSceneView's per-frame caller in client Draw A (LIVE run 6: RA 0x9F8B3A)
// FIRSTPERSON 2026-10-11: the game's ULocalPlayer (CalcSceneView's this on the per-frame call) for AKVR's first-person
// camera (player controller at +0x5C, akvr camera.cpp FIRSTPERSON)
void* volatile g_aknLocalPlayer = nullptr;
extern "C" __declspec(dllexport) void* akn_local_player() { return g_aknLocalPlayer; }
void* __fastcall hk_Calc(void* p, void* fam, void* loc, void* rot, void* vp, void* drawer)
{
    if ((uintptr_t)_ReturnAddress() == g_raCalcA) g_aknLocalPlayer = p;   // FIRSTPERSON
    k_calc.add((uintptr_t)_ReturnAddress()); note_tid(2);
    if (c_calc < 2) stack_once("CalcSceneView");
    InterlockedIncrement(&c_calc);
    ++tl_calcInPass;
    if (g_stereo && g_armed && g_camOk && g_steady >= 30 && tl_eye < 0 && (uintptr_t)_ReturnAddress() == g_raCalcA) return stereo_calc(p, fam, loc, rot, vp, drawer);
    return o_Calc(p, fam, loc, rot, vp, drawer);
}
void __fastcall hk_CdA(void* t, void* vp, void* cv)
{
    k_cdA.add((uintptr_t)_ReturnAddress()); note_tid(3);
    if (c_cdA < 4) stack_once("client Draw A");
    InterlockedIncrement(&c_cdA);
    const LONG64 a = qpc(); o_CdA(t, vp, cv); InterlockedAdd64(&t_cdA, qpc() - a);
}
void __fastcall hk_CdB(void* t, void* vp, void* cv) { k_cdB.add((uintptr_t)_ReturnAddress()); note_tid(4); InterlockedIncrement(&c_cdB); o_CdB(t, vp, cv); }

// ---- M2 ENGINE STEREO (2026-10-07): two views in ONE view family, left and right half of the window ----
// UE3 split screen: a player's view rect = viewport size x (Origin, Size) at player +0xA8/+0xAC (origin x/y) and
// +0xB0/+0xB4 (size x/y), read at the top of CalcSceneView 0x9F6220. CalcSceneView news a 0xA20-byte FSceneView,
// runs its ctor 0x82EB30 (RA 0x9F7301) and appends it to family->Views (family +0 data, +8 count). Calling it twice
// with half widths gives a two-view family that BeginRenderingViewFamily renders in one pass.
// Per-eye history: the second view gets its own FSceneViewState from AllocateViewState(0,0) = 0x82F860 (0x8850 bytes,
// the game stores its own in player +0xC8, ULocalPlayer at 0x9F93F8). A 0..3 counter at 0x143098180 (RVA 0x3098180)
// advances once per CalcSceneView; the second eye gets the same value as the first.
// FSceneView ctor (34 args, all integer-class slots): a1 this, a2 family, a3 ViewState, a4 -1, ... a11 X, a12 Y,
// a13 X, a14 Y, a15 SizeX, a16 SizeY (floats in their stack slots), a17 ViewMatrix*, a18 ProjectionMatrix*.
// Method: batman_city RE_NOTES "Stereo seam decision" (edit the ctor's INPUTS; the engine derives the rest).
using U = uint64_t;
using CtorFn = void*(__fastcall*)(U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U);
CtorFn o_Ctor = nullptr;
using AllocVsFn = void*(__fastcall*)(uint32_t, uint32_t);
AllocVsFn p_AllocVs = nullptr;
volatile LONG g_stereo = 0;          // F9 (or -aknstereo): two half-width views per frame
float g_ipd = 6.4f;                  // engine units between the eyes (aknipd=<n> on the command line)
void* g_vs2 = nullptr;               // the second eye's view state
void* g_vs3 = nullptr;               // vs mode 3: own view state with the game's +0x750 copied in
void* g_vs4 = nullptr;               // vs mode 4: zeroed memory + the view state ctor 0x82CE20 (like the game's start-up one)
void* g_vs5 = nullptr;               // vs mode 5: AllocateViewState + +0x750 zeroed
using VsCtorFn = void*(__fastcall*)(void*, uint32_t, uint32_t);
VsCtorFn p_VsCtor = nullptr;
volatile uint32_t* g_jitIdx = nullptr;
uintptr_t g_raCtorCalc = 0;
thread_local int tl_eye = -1;
volatile LONG c_stereoFrames = 0, c_ctorEye[2] = {};
struct M44 { float m[4][4]; };
int g_ctorLogs = 0;
void log_m(const char* n, const float* m)
{
    logf("  %s [%.4f %.4f %.4f %.4f] [%.4f %.4f %.4f %.4f] [%.4f %.4f %.4f %.4f] [%.3f %.3f %.3f %.3f]", n,
         m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
}
// DETSHARE 2026-10-11 23:0x (default 4 -> 0): the right eye's private view state (mode 4 + VSSPLIT / VSSYNC) was the cause of
// detective mode's half-lit right eye, the right eye's missing lighting after detective mode and from the start (JJ, live
// "vs 0": "The two eyes are consistent now", exit "looks better now too"; pair_shareoff / pair_shareon). Per-eye history now
// lives in each eye's own DLSS feature. Mode 4 also swapped view states on every detective toggle - the suspect for the
// SteamVR driver crashes. Back with "vs 4" or aknvs=4.
volatile LONG g_vsMode = 0;          // second eye's view state: 0 = share the game's, 1 = its own, 2 = none, 4 = own + VSSPLIT (aknvs=<n>, command "vs n")
volatile bool g_camOk = false;        // the last mono view had a finite projection
volatile LONG g_asym = 0;             // command "asym 1": per-eye off-centre lens from g_tan
volatile LONG g_mkBase = 1;            // MARKERBASE (below hk_Proj): command "markerbase 0|1"
extern volatile LONG g_gloss;   // SUITGLOSS (defined with the rain copy)
volatile LONG g_hudAll = 0, g_hudDef = 0, g_hudStat = 0;   // HUDTRACE (see c_hudRedirect below)
volatile LONG g_hudDsvOn = 1;          // HUDDSV (below hud_ready): command "huddsv 0|1"
// HUDSKIP 2026-10-09 (JJ: the pause and weapon wheel screens "show up within black windows, so those backgrounds need to
// be removed"): HUD draws dropped by vertex shader + the size of their texture (slot 0), e.g. the wheels' vignette =
// vs 9689d605c06946bf with a 256x16 gradient. Commands "hudskip add <vs hex> <w> <h>", "hudskip clear".
struct HudSkip { unsigned long long vs; UINT w, h; };
// Default rules (HB run 01:25: the pause backdrop = vs 3b819a7e86e9d631 with a 256x1 gradient - removing it took the
// sheet's cover from 120 to 23 cells and the edge darkness from 122 to 0, the menu text stayed; the wheels' vignette =
// vs 9689d605c06946bf with a 256x16 gradient, the first visible draw in both wheel captures c000911 / c010816). They
// act only while AKVR's pause look is live (command "hudskipon 0|1", sent by AKVR), never on gameplay HUD or menus.
HudSkip g_hudSkip[8] = { { 0x3b819a7e86e9d631ull, 256, 1 }, { 0x9689d605c06946bfull, 256, 16 } };
volatile LONG g_hudSkipN = 2, f_hudSkipped = 0, g_hudSkipOn = 0;
// HUDSKIPIDX 2026-10-09 (JJ: the wheels / mission dialogs still sit in black windows; the wheel's backdrop is not the
// gradient draw): HUD draws dropped by their index in the frame's sheet (bisection), "hudskipidx a b" (-1 = off); a
// single index logs that draw's vertex shader + texture once.
volatile LONG g_hsiLo = -1, g_hsiHi = -1, f_hudIdx = 0, g_hsiLogged = 0;
// DETECTIVE 2026-10-09 (JJ: after detective mode the right eye stays half in it - orange x-ray, flat blue light; and a
// vignette in each eye): "vssnap <tag>" saves both eyes' view states (akn\\vs_<tag>_<eye>.bin) for an offline diff;
// "psinv N" lists the pixel shaders of N frames' eye renders (akn\\ps_inventory.txt); "skipps <hex>" hides one in the
// eye renders (0 = off).
void* volatile g_vsOf[2] = {}; volatile LONG g_vsSnap = 0; char g_vsSnapTag[24] = "";
volatile unsigned long long g_skipPs = 0; volatile LONG g_psInv = 0;
// DIAGKIT 2026-10-14 (JJ: "build into the overlay test features ... so that we don't have to continually build new ones"): the panel's
// TESTS drawer drives these. FXSTEP: "fxcollect" lists the pixel shaders drawn in the eye renders (4 frames), "fxstep +1 / -1" hides them
// one at a time (the draw is skipped), "fxstep 0" shows all; akn_fx_status gives the panel its line.
unsigned long long g_fxList[4096]; volatile LONG g_fxN = 0, g_fxIdx = -1, g_fxCollect = 0;
unsigned long long g_psSet[4096]; volatile LONG g_psN = 0;
// DETVIG 2026-10-09 (JJ: detective mode's vignette "should be removed per eye and a new one added across both eyes"):
// CAPDET c022421 found it - hiding pixel shader ce8b103bb56ebabe (detective mode's final composite: vignette + Batman's
// rim outline) took the corners from 1.0 to 18.8 with the rest of the look kept. Hidden in the eye renders (command
// "detvig 0|1"); seeing it = detective mode is on (export akn_detective_active, AKVR then draws ONE vignette over both
// eyes, view-fixed). Shader names cached per object (no name lookup per draw).
volatile LONG g_detVigSkip = 1; volatile ULONGLONG g_detSeenMs = 0;
// DETHOLD 2026-10-11 (JJ, SteamVR at 22-43 frames/s: right eye wrong after detective mode, a white border flicking during it):
// detective mode counted as over after only 100 ms without its composite shader; at ~22 frames/s (45 ms apart, plus thread
// jitter) that dropped out every few seconds - the right eye flipped between its detective and normal-play view states 12
// times in 50 s (log 22:27:44-22:28:37), and the normal-play state was used IN detective mode, which leaves it half in
// detective mode afterwards. Also the detection ran only while "detvig" hid the shader: the DET2 test step "detvig 0" made
// the normal-play state in the middle of detective mode. Now: detection always runs; one hold time for every user (view
// state choice, exit, AKVR's vignette); per-second line "detective: composite seen/s, longest gap".
volatile LONG g_detHoldMs = 600, c_detSeen = 0, g_detGapMax = 0;   // command "dethold <ms>"
inline bool det_on() { const ULONGLONG s = g_detSeenMs; return s && GetTickCount64() - s < (ULONGLONG)g_detHoldMs; }
// VSFRESH 2026-10-09 (JJ: right eye still wrong after detective mode - c025418: right eye without fog, light shafts and
// rain, left fine; the rain step stopped running in the right eye's render at the exit): the game puts its own view
// state back on the exit, never ours. When detective mode ends (AKN stops seeing its composite shader) the right eye
// gets a NEW view state, made the same way as the first one (vs mode 4). The old one is left allocated (the render
// thread may still be using it this frame). Command "vsfresh" = do it now; "vsfreshdet 0|1" = on detective exit.
volatile LONG g_vsFreshDet = 0, g_vsFreshWant = 0, c_vsFresh = 0;
// VSSPLIT 2026-10-09 (JJ live tests 03:09 / 03:24): the ORIGINAL right-eye view state is right in detective mode and on
// the map but stays half in detective mode after the exit; a FRESH one is right in normal play but wrong in detective
// mode (white lights) and on the map (real city). So the right eye keeps both: the original while detective mode or a
// flat screen (map; AKVR akvr_flat_screen_now) is up, a second one (made at the first detective exit, never used in
// detective mode) in normal play. Command "vssplit 0|1".
volatile LONG g_vsSplit = 1, c_vsSplitSwaps = 0; void* g_vsNorm = nullptr;
volatile LONG g_mkEye = 1;   // (default ON again 2026-10-11 13:xx: MKFACE2 - the records show it world-locked; the old path turns with the head) MKEYE (RETHINK MARKERS 2, below hud_ready): command "mkeye 0|1"
volatile LONG g_mkLagMode = 0, g_mkLagUse = 0;   // (default 0 2026-10-11: the record showed no frame lag to fix)   // MKLAG (below mk_eye_frame): 0 pair's frame, 1 the other frame, 2 auto
volatile LONG g_mkRecFrames = 0; FILE* g_mkRecF = nullptr;   // MKREC: per-frame marker record (command "mkrec N", panel button)
volatile LONG g_mkGS = 1;   // MKGS: marker pieces placed per vertex in 3D per eye by a geometry shader in their own draw; "mkgs 0|1"
volatile LONG g_mkCard = 0;   // (MKCARD off 2026-10-11: JJ - elements split, size changed, "hacky")   // MKCARD: marker pieces into the flat marker sheet + a perspective card per eye (1) / moved per eye in their draw (0); "mkcard 0|1"
volatile LONG g_mkFaceCam = 1;   // MKFACE2: markers face the game camera's direction (1) or the viewer's eyes (0); command "mkfacecam 0|1"
volatile LONG g_mkFacePos = 1, c_mkFaced = 0;   // MKFACEPOS: the card tilts to face the game camera's position (City MARKERFACE); "mkfacepos 0|1"
// DETCALLOUT (below mk_cam_centre): detective scan callout as one marker at its box's object; "scan 0|1", "scanmin <units>"
volatile LONG g_scanOn = 1, g_mkScanIdx = -1, c_scanPieces = 0, c_scanSeen = 0, c_scanPlaced = 0, c_scanReads = 0, c_scanSky = 0, c_scanPasses = 0;
volatile LONG g_scanBase = 1, g_scanExact = 1, c_scanExact = 0; float g_scanMatchD = 0.0f;
volatile LONG g_hudSigOn = 0, g_hsigLines = 0;   // HUDSIG (hud_sig_note) - off by default since HR5 quiet (2026-10-13): command "recorders 1"
float g_flipR = 0.12f;   // FLIPRAD 2026-10-14: FLIPREC's search radius (command "fliprad <r>"; the flat distance number sits ~0.11-0.15 from the marker point)
volatile LONG g_recOn = 0;   // HR5 quiet: HUDCOVER readback, SCANREC / SCANPIECE / FLIPREC / MKMISS event lines only with "recorders 1" (counters stay)
volatile LONG g_mkCarry2 = 1, c_mkCarry2 = 0;   // MKCARRY2 (mk_eye_frame)
volatile LONG g_mkCont = 1, c_mkContHeld = 0, c_mkContTake = 0, g_mkContLines = 0;   // MKCONT (mk_eye_frame)
volatile LONG g_mkHeldNow = 0, c_mkHeldDrop = 0;   // HELDDROP: this HUD frame's map / point was held back; marked pieces out of reach are left out, not drawn flat
volatile LONG c_mkMissNone = 0, c_mkMissReach = 0, g_mkMissLines = 0;   // MKMISS
volatile LONG c_mkPlaceJump = 0, g_mkPlaceLines = 0;   // MKPLACE
volatile LONG g_heldLines = 0, g_markedLines = 0;   // HELDREC
volatile LONG g_tagFade = 1, c_mkTagFaded = 0;   // TAGFADE (command "tagfade 0|1")
volatile LONG g_mkStickyOn = 0;   // MKSTICKYOFF (mk_sticky)
volatile LONG g_flipLines = 0, c_flipSingle = 0, c_flipBatch = 0, g_scanRecLines = 0, g_scanPieceLines = 0;   // FLIPREC / SCANREC / SCANPIECE
volatile LONG g_bhStep = -1, g_bhWait = 0, g_bhMax = 0; float g_bhBase = -1.0f;   // BLACKHUNT
volatile LONG g_hrOn = 0, g_hrDumps = 0; volatile LONGLONG g_hrDumpAt = 0; char g_hrWhy[48] = "";   // HUDRING
float g_scanWpub[3] = {}, g_scanSheet[2] = {}; volatile LONG g_scanWok = 0; char g_scanNearDiag[220] = "";   // SCANLINE: the box's world point this HUD frame (export akn_scan_point)   // SCANPROJ: detective projections head-free; box at the projected object point
float g_scanMin = 600.0f; char g_scanDiag[400] = "";   // DETCALLOUT2: 300 -> 600 (JJ's run: hits at 311 / 435 units = Batman / the ledge)
volatile LONG g_depthProbe = 0;   // DEPTHPROBE: frames left to compare the SMAA-time depth (DLSS's) with the pre-clear copy; "depthprobe N"
volatile LONG g_mkAnchorOn = 0;   // (default OFF: JJ "even worse") MKANCHOR: command "mkanchor 0|1"
volatile LONG g_markSheet = 1, g_mkPieces = 0, f_mkRouted = 0, g_mkRoutedLast = 0, c_mkRouted = 0, c_mkFlat = 0, c_mkNoCb = 0; float g_mkReach = 0.09f;   // MKREACH 2026-10-09: 0.08 left the bracket corners / distance text on the HUD sheet; MKBATCH 23:4x: back to 0.09 - 0.16 pulled a piece 0.11 away (another HUD element) onto the marker, and the brackets / glow are batched pieces now routed one by one   // MARKSHEET (below hud_ready)
volatile LONG g_mkByTag = 1, c_mkTagged = 0, c_mkTagLost = 0, c_mkOffSheet = 0; float g_mkTagReach = 0.35f;   // TAGREACH REVERTED 2026-10-14 (HUDREC: at 0.15 the glow / a corner fell flat on ~18 % of frames - JJ "box and glow have detached"); was: 2026-10-14 (JJ: the compass's green objective box flickers off like the distance marker - the box carries the marker mark and joined the marker whenever the marker came within 0.35 of it): 0.35 -> 0.15   // MKTAG (commands mkmode / mktag / mktagreach)
// VSCOPY 2026-10-09 (JJ 03:09, c030900/c030937: with a FRESH right-eye view state detective mode turns lights white
// and the map shows the real city - the game sets up every view state it already knows on those switches, a fresh one
// misses it; dozens of fields keep start-up values (0x1f at +0xC/+0x54/+0x574..). So VSFRESH is off by default.) The
// ORIGINAL right-eye view state after the detective exit (c025418) differed from the game's only in two flags,
// +0xB0 (game 1, right 0) and +0xC0, which match between eyes in every normal capture: copied game -> right each
// frame. Live list: "vscopy <hex offset> <hex bytes>" adds, "vscopy clear" empties (up to 16 ranges).
void hwdet_command(const char* arg);   // HWDET (below)
struct VsRange { LONG off, len; };
VsRange g_vsCopy[16] = {}; volatile LONG g_vsCopyN = 0;   // (+0xB0 did not fix the exit - live test 03:24)   // +0xC0 must NOT be copied: crashed at the first stereo frame (03:14, write to 0x24 at 0x1064C80) and in gameplay (HF8); +0xB0 alone ran fine (HF8)
struct PsName { const void* obj; unsigned long long h; };
PsName g_psCache[512] = {};
// VSSYNC 2026-10-09 (JJ: after detective mode the right eye stays flat blue; c022405/21/35 view-state diff: the game's
// view state +0x5D0..+0x65F (9 x RGB, the ambient light coefficients, sampled at +0x5B0) is zeroed in BOTH eyes by
// detective mode and restored on exit only in the GAME's own; the right eye's stayed 0). Copied game -> right eye each
// frame (command "vssync 0|1").
volatile LONG g_vsSync = 1;
// HUDVP 2026-10-09 (JJ: the main menu's button prompts "too high ... sitting within a layout that is for a smaller
// resolution. In fact, the buttons do look smaller too"): HUD parts our Scaleform resize (AKVR HUDFULL) never reaches
// still draw with the WINDOW's full viewport (render size) onto the sheet (DLSS output size) -> laid out in its top-left
// 58 %. While the sheet is the target, a viewport of exactly the whole window (and its scissor) is stretched to the whole
// sheet; the game's own request comes back when it binds another target. Command "hudvp 0|1".
volatile LONG g_hudVp = 1, f_hudVpScaled = 0;
int g_asymLogs = 0;
// Quest 3-like test lens (deg: left eye outer 52, inner 42, up 45, down 50); right eye mirrored. command "fov l r u d"
float g_tan[2][4] = { { -1.2799f, 0.9004f, 1.0000f, -1.1918f }, { -0.9004f, 1.2799f, 1.0000f, -1.1918f } };
int g_mono_proj_logs = 0;
float g_eyeOff[2] = { -3.2f, 3.2f };        // eye offsets along view x, game units (from AKVR when g_eyesApi)
volatile bool g_eyesApi = false;
volatile LONG g_eyeSame = 0;
float g_eyeShift[2] = { 0, 0 };       // command "eyeshift <eye> <units>" (leak test, JJ's 5th Quest test)          // command "eyesame 1": both eyes from the same camera and lens (exposure test)
volatile LONG c_eyesCalls = 0, c_presents = 0;
volatile LONG g_akvrFresh = 0, g_akvrFlat = 0;   // AKVR's own count of frames shown as the AKN pair vs the flat fallback   // telemetry: AKVR's calls

void log_vs(const char* n, void* vsp)
{
    __try
    {
        const uint64_t* q = (const uint64_t*)((uintptr_t)vsp + 0x740);
        logf("  %s view state %p: vtbl %llX, +0x740.. %llX %llX %llX %llX", n, vsp, (unsigned long long)(*(uint64_t*)vsp - g_base),
             (unsigned long long)q[0], (unsigned long long)q[1], (unsigned long long)q[2], (unsigned long long)q[3]);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { logf("  %s view state %p unreadable", n, vsp); }
}
// command file <game>kn\command.txt, read every 100 ms and deleted: "stereo 0|1", "vs 0|1|2", "ipd <units>"
extern volatile LONG g_capDetBusy; DWORD WINAPI capdet_seq(LPVOID);   // CAPDET (below)
void do_command(char* line)
{
        int v = 0; float x = 0;
        if (akn_afw_cmd(line)) return;   // AFW: afw / afwmode / afwwin / afwlum / afwdbg / afwlog (afw.cpp)
        if (sscanf_s(line, "rainrelight %d", &v) == 1) { g_rainRelight = v; logf("command: rain lit again in the second eye %d (RAINRELIGHT)", v);  return; }
        if (strncmp(line, "fxcollect", 9) == 0) { g_skipPs = 0; g_psN = 0; g_fxCollect = 1; g_psInv = 4; logf("command: FXSTEP collecting the effects on screen"); return; }   // DIAGKIT
        if (sscanf_s(line, "fxstep %d", &v) == 1) {   // DIAGKIT
            if (v == 0 || g_fxN <= 0) { g_fxIdx = -1; g_skipPs = 0; logf("command: FXSTEP all effects shown"); return; }
            LONG i = g_fxIdx + (v > 0 ? 1 : -1); if (i < 0) i = g_fxN - 1; if (i >= g_fxN) i = 0; g_fxIdx = i; g_skipPs = g_fxList[i];
            logf("FXSTEP: hiding effect %ld of %ld - pixel shader %016llx", i + 1, g_fxN, g_fxList[i]); return; }
        if (strncmp(line, "psnewarm", 8) == 0) { g_pnOn = 1; g_pnArmed = 0; g_pnArmT = GetTickCount64() + 2000; g_pnLines = 0; g_pnArmAt = g_pnArmT; logf("command: PSNEW baseline for 2 s, then new effects are logged"); return; }   // DIAGKIT
        if (strncmp(line, "fxwatch ", 8) == 0) { g_fxWatch = _strtoui64(line + 8, nullptr, 16); logf("command: FXCB watches pixel shader %016llx at each capture", (unsigned long long)g_fxWatch); return; }
        if (sscanf_s(line, "lightlag %d", &v) == 1) { g_lightLag = v < 0 ? 0 : v > 3 ? 3 : v; logf("command: sprite light directions from the game camera %ld frame(s) back (LIGHTLAG)", (long)g_lightLag); return; }
        if (sscanf_s(line, "smokelight %d", &v) == 1) { g_smokeLight = v; logf("command: smoke light grid lit along the game camera, not the head %d (SMOKELIGHT)", v); return; }
        if (sscanf_s(line, "spriteaxes %d", &v) == 1) { g_spriteAxes = v; logf("command: particle sprites face the head-free camera %d (SPRITEAXES)", v); return; }
        if (sscanf_s(line, "rainmask %d", &v) == 1) { g_rainMask = v; logf("command: rain marked for DLSS %d (RAINMASK)", v); return; }
        if (sscanf_s(line, "rainlight2 %d", &v) == 1) { g_rainLight2 = v; logf("command: rain lit from the eye that drew the light picture %d (RAINLIGHT2)", v);  return; }
        if (sscanf_s(line, "rainlight %d", &v) == 1) { g_rainLight = v; logf("command: rain lit from the eye's own spot %d (RAINLIGHT)", v);  return; }
        if (sscanf_s(line, "mkcont %d", &v) == 1) { g_mkCont = v; g_mkContLines = 0; logf("command: marker points kept through one-off jumps %d (MKCONT)", v);  return; }
        if (strncmp(line, "psnew list", 10) == 0) { ps_new_list(); return; }
        if (sscanf_s(line, "psnew %d", &v) == 1) { g_pnOn = 1; g_pnArmed = v; g_pnArmT = GetTickCount64(); g_pnLines = 0; logf("command: PSNEW %s (shaders known so far are the baseline)", v ? "armed" : "collecting");  return; }
        if (strncmp(line, "flatwin", 7) == 0) { g_flatUntil = GetTickCount64() + 700; g_flatLines = 0; g_flatSigN = 0; logf("FLATREC: manual window - flat HUD draws for 0.7 s");  return; }
        if (strncmp(line, "psskip ", 7) == 0) { g_psSkip = (LONG)strtoul(line + 7, nullptr, 16); c_psSkipped = 0; logf("command: PSSKIP %08X (no shader bound in its place)", (unsigned)g_psSkip);  return; }
        if (strncmp(line, "fliprad ", 8) == 0) { g_flipR = (float)atof(line + 8); g_flipLines = 0; logf("command: FLIPREC radius %.3f (lines reset)", g_flipR);  return; }
        if (strncmp(line, "perfsweep", 9) == 0) { g_sweepWant = 1; return; }   // PERF1: the feature cost sweep (worker loop)
        if (sscanf_s(line, "cutlog %d", &v) == 1) {   // PERF1: FULLCUT's CPU readback (telemetry) on / off
            using FcFn = void(*)(int); HMODULE m = GetModuleHandleW(L"dinput8.dll"); FcFn f = m ? (FcFn)GetProcAddress(m, "akvr_set_cutlog") : nullptr;
            if (f) f(v); logf("command: full-screen cut probe log %d (%s)", v, f ? "sent to AKVR" : "AKVR export missing"); return; }
        if (sscanf_s(line, "fullcut %d", &v) == 1) {   // FULLCUT (AKVR): full-screen fades darken the whole view
            using FcFn = void(*)(int); HMODULE m = GetModuleHandleW(L"dinput8.dll"); FcFn f = m ? (FcFn)GetProcAddress(m, "akvr_set_fullcut") : nullptr;
            if (f) f(v); logf("command: full-screen cuts over the whole view %d (%s)", v, f ? "sent to AKVR" : "AKVR export missing"); return; }
        if (sscanf_s(line, "flareoff %d", &v) == 1) { logf("command: flareoff is no longer live - set flareoff=%d in akvr_settings.ini, it applies at the next start (FLAREOFF3 swaps at shader creation)", v); return; }   // FLAREOFF
        if (sscanf_s(line, "stereo %d", &v) == 1) { g_stereo = v; logf("command: stereo %d", v); }
        else if (sscanf_s(line, "vs %d", &v) == 1) { g_vsMode = v; logf("command: second eye view state mode %d", v); }
        else if (sscanf_s(line, "ipd %f", &x) == 1) { g_ipd = x; logf("command: eye separation %.2f", x); }
        else if (strncmp(line, "after", 5) == 0) { g_afterLogs = 40; g_drawLogs = 80; logf("command: log one frame after the left eye"); }
        else if (strncmp(line, "capture", 7) == 0) { char t[16] = "a"; sscanf_s(line + 7, " %15s", t, (unsigned)sizeof(t)); strcpy_s(g_capTag, t); g_capWant = 7; g_capHud = 1; logf("command: capture both eyes, the pair and the HUD (tag %s)", t); }
        else if (sscanf_s(line, "asym %d", &v) == 1) { g_asym = v; g_asymLogs = 0; logf("command: asymmetric lens %d", v); }
        else if (strncmp(line, "fov", 3) == 0)
        {
            float a[4];
            if (sscanf_s(line + 3, "%f %f %f %f", &a[0], &a[1], &a[2], &a[3]) == 4)
            {
                const float k = 3.14159265f / 180.0f;
                g_tan[0][0] = -tanf(a[0] * k); g_tan[0][1] = tanf(a[1] * k); g_tan[0][2] = tanf(a[2] * k); g_tan[0][3] = -tanf(a[3] * k);
                g_tan[1][0] = -g_tan[0][1]; g_tan[1][1] = -g_tan[0][0]; g_tan[1][2] = g_tan[0][2]; g_tan[1][3] = g_tan[0][3];
                g_asymLogs = 0; logf("command: left eye lens outer %.1f inner %.1f up %.1f down %.1f deg (right mirrored)", a[0], a[1], a[2], a[3]);
            }
        }
        else if (strncmp(line, "mark", 4) == 0 && (line[4] == ' ' || line[4] == 0 || line[4] == 10 || line[4] == 13)) { char* e = line + strlen(line); while (e > line && (e[-1] == 10 || e[-1] == 13)) *--e = 0; logf("MARK%s", line + 4); }   // not "markercut" / "markerbase"
        else if (strncmp(line, "pairshot", 8) == 0) { char t[16] = "p"; sscanf_s(line + 8, " %15s", t, (unsigned)sizeof(t)); strcpy_s(g_capTag, t); g_capWant = 4; logf("command: pair picture only (tag %s)", t); }
        else if (sscanf_s(line, "histswap %d", &v) == 1) { g_histSwap = v; g_histLog = 1; logf("command: own history per eye %d", v); }
        else if (strncmp(line, "eyeshift", 8) == 0) { int e = 0; float u = 0; if (sscanf_s(line + 8, "%d %f", &e, &u) == 2 && e >= 0 && e < 2) { g_eyeShift[e] = u; logf("command: eye %d shifted %.0f units", e, u); } }
        else if (sscanf_s(line, "framesync %d", &v) == 1) { g_frameSync = v; logf("command: frame number sync %d", v); }
        else if (sscanf_s(line, "rainonce %d", &v) == 1) { g_rainOnce = v; logf("command: rain step once per frame %d (1 = simulation, 2 = splashes, 3 = both: skipped in the second eye)", v); }
        else if (sscanf_s(line, "rtwait %d", &v) == 1) { g_rtWait = v; logf("command: game thread waits for both eye renders %d", v); }
        else if (sscanf_s(line, "deltazero %d", &v) == 1) { g_deltaZero = v; logf("command: second render with zero elapsed time %d", v); }
        else if (sscanf_s(line, "famdump %d", &v) == 1) { g_famDump = v; logf("command: dump both view families for %d frames", v); }
        else if (sscanf_s(line, "randsync %d", &v) == 1) { g_randSync = v; logf("command: same random numbers for both eyes %d (1 = UE3 seed, 2 = C runtime, 3 = both)", v); }
        else if (sscanf_s(line, "raindepth %d", &v) == 1) { g_rainDepth = v; g_rainDepthLog = 2; logf("command: rain drawn from the simulation's origin per eye %d", v); }
        else if (sscanf_s(line, "rainvp %d", &v) == 1) { g_rainDrawDump = v; g_rdHave[0] = g_rdHave[1] = 0; logf("command: compare the rain draw's camera constants between the eyes %d times", v); }
        else if (sscanf_s(line, "markerbase %d", &v) == 1) { g_mkBase = v; logf("command: markers laid out from the head-free camera %d", v); }   // MARKERBASE
        else if (sscanf_s(line, "markercut %d", &v) == 1) {   // MARKERCUT test switch (AKVR)
            using McFn = void(*)(int); HMODULE m = GetModuleHandleW(L"dinput8.dll"); McFn f = m ? (McFn)GetProcAddress(m, "akvr_set_markercut") : nullptr;
            if (f) f(v); logf("command: markers into the eyes %d (%s)", v, f ? "sent to AKVR" : "AKVR export missing"); }
        else if (sscanf_s(line, "hudstat %d", &v) == 1) { g_hudStat = v; logf("command: HUD sheet trace for %d frames", v); }   // HUDTRACE
        else if (sscanf_s(line, "blackkey %d", &v) == 1) {   // BLACKKEY test switch (AKVR: pure-black backdrops removed on the pause look)
            using BkFn = void(*)(int); HMODULE m = GetModuleHandleW(L"dinput8.dll"); BkFn f = m ? (BkFn)GetProcAddress(m, "akvr_set_blackkey") : nullptr;
            if (f) f(v); logf("command: black backdrops removed %d (%s)", v, f ? "sent to AKVR" : "AKVR export missing"); }
        else if (sscanf_s(line, "backext %d", &v) == 1) {   // BACKEXT test switch (AKVR: the pause backdrop carried over the whole view)
            using BeFn = void(*)(int); HMODULE m = GetModuleHandleW(L"dinput8.dll"); BeFn f = m ? (BeFn)GetProcAddress(m, "akvr_set_backext") : nullptr;
            if (f) f(v); logf("command: pause backdrop over the whole view %d (%s)", v, f ? "sent to AKVR" : "AKVR export missing"); }
        else if (strncmp(line, "padbtn", 6) == 0) {   // PADINJECT (AKVR): hold pad buttons for the game
            unsigned mask = 0; int ms = 0; sscanf_s(line + 6, " %x %d", &mask, &ms);
            using PiFn = void(*)(int, int); HMODULE m = GetModuleHandleW(L"dinput8.dll"); PiFn f = m ? (PiFn)GetProcAddress(m, "akvr_pad_inject") : nullptr;
            if (f) f((int)mask, ms); logf("command: pad buttons %04x for %d ms (%s)", mask, ms, f ? "sent to AKVR" : "AKVR export missing"); }
        else if (strncmp(line, "hudskipidx", 10) == 0) { long a = -1, b = -1; sscanf_s(line + 10, " %ld %ld", &a, &b); g_hsiHi = b < a ? a : b; g_hsiLo = a; g_hsiLogged = 0; logf("command: HUD draws %ld..%ld dropped", a, g_hsiHi); }
        else if (strncmp(line, "vssnap", 6) == 0) { sscanf_s(line + 6, " %23s", g_vsSnapTag, (unsigned)sizeof(g_vsSnapTag)); g_vsSnap = 1; logf("command: view state snapshot %s", g_vsSnapTag); }
        else if (sscanf_s(line, "detvigtest %d", &v) == 1) {   // DETVIG test switch (AKVR: the shared vignette as if detective mode were on)
            using DtFn = void(*)(int); HMODULE m = GetModuleHandleW(L"dinput8.dll"); DtFn f = m ? (DtFn)GetProcAddress(m, "akvr_set_detvig_test") : nullptr;
            if (f) f(v); logf("command: shared detective vignette test %d (%s)", v, f ? "sent to AKVR" : "AKVR export missing"); }
        else if (strncmp(line, "akvrdiag", 8) == 0) { using DgFn = const char*(*)(); HMODULE m = GetModuleHandleW(L"dinput8.dll"); DgFn f = m ? (DgFn)GetProcAddress(m, "akvr_mirror_diag") : nullptr; logf("AKVR: %s", f ? f() : "diag export missing"); }
        else if (sscanf_s(line, "dethold %d", &v) == 1) { g_detHoldMs = v < 50 ? 50 : v; logf("command: detective mode held %ld ms after its composite was last seen (DETHOLD)", (long)g_detHoldMs); }
        else if (sscanf_s(line, "detvig %d", &v) == 1) { g_detVigSkip = v; logf("command: detective mode's per-eye vignette hidden %d", v); }   // DETVIG
        else if (strncmp(line, "vscopy clear", 12) == 0) { g_vsCopyN = 0; logf("command: view-state copy list empty"); }   // VSCOPY
        else if (strncmp(line, "vscopy ", 7) == 0)
        {   // VSCOPY: "vscopy <hex offset> <hex bytes>"
            unsigned o = 0, n = 0;
            if (sscanf_s(line + 7, "%x %x", &o, &n) == 2 && g_vsCopyN < 16) { g_vsCopy[g_vsCopyN].off = (LONG)o; g_vsCopy[g_vsCopyN].len = (LONG)n; InterlockedIncrement(&g_vsCopyN); }
            logf("command: view-state copy game -> right eye, %ld ranges (last +0x%X, 0x%X bytes)", g_vsCopyN, o, n);
        }
        else if (strncmp(line, "hwdet", 5) == 0) hwdet_command(line + 5 + (line[5] == ' '));   // HWDET
        else if (sscanf_s(line, "mkgs %d", &v) == 1) { g_mkGS = v; logf("command: marker pieces placed per vertex in 3D (MKGS) %d", v); }
        else if (sscanf_s(line, "mkcard %d", &v) == 1) { g_mkCard = v; logf("command: markers as perspective cards (MKCARD) %d", v); }
        else if (sscanf_s(line, "mkfacecam %d", &v) == 1) { g_mkFaceCam = v; logf("command: markers face %s (MKFACE2)", v ? "the game camera's direction" : "the viewer's eyes"); }
        else if (sscanf_s(line, "scan %d", &v) == 1) { g_scanOn = v; using SsFn = void(*)(int); HMODULE hm = GetModuleHandleW(L"dinput8.dll"); SsFn f = hm ? (SsFn)GetProcAddress(hm, "akvr_scan_set") : nullptr; if (f) f(v); logf("command: detective scan callout on its object %d (DETCALLOUT)", v); }
        else if (sscanf_s(line, "depthprobe %d", &v) == 1) { g_depthProbe = v; logf("command: depth probe for %d left-eye renders", v); }
        else if (strncmp(line, "scancut", 7) == 0) { int on = 1; float r = 0.0f; sscanf_s(line + 7, "%d %f", &on, &r); using ScFn = void(*)(int, float); HMODULE hm = GetModuleHandleW(L"dinput8.dll"); ScFn f = hm ? (ScFn)GetProcAddress(hm, "akvr_scan_cut") : nullptr; if (f) f(on, r); logf("command: scan box drawn from a small square only %d (r %.3f) (SCANCUT)", on, r); }
        else if (strncmp(line, "blackhunt", 9) == 0) { if (g_bhStep < 0) { g_bhStep = 0; g_bhBase = -1.0f; g_bhWait = 1; logf("command: BLACKHUNT (find the menus' black backdrop draw)"); } }
        else if (strncmp(line, "transfull", 9) == 0) { int on = 1; float dd = 0.0f, zz = 0.0f; sscanf_s(line + 9, "%d %f %f", &on, &dd, &zz); using TfFn = void(*)(int, float, float); HMODULE hm = GetModuleHandleW(L"dinput8.dll"); TfFn f = hm ? (TfFn)GetProcAddress(hm, "akvr_transfull") : nullptr; if (f) f(on, dd, zz); logf("command: full-view transitions %d (distance %.2f, enlargement %.2f; 0 = unchanged) (TRANSFULL)", on, dd, zz); }
        else if (sscanf_s(line, "mksticky %d", &v) == 1) { g_mkStickyOn = v; logf("command: hold unmarked pieces of a recently marked shader with the marker %d (MKSTICKY)", v); }
        else if (sscanf_s(line, "tagfade %d", &v) == 1) { g_tagFade = v; logf("command: marker mark found under the piece's own fade %d (TAGFADE)", v); }
        else if (sscanf_s(line, "recorders %d", &v) == 1) { g_recOn = v; g_hudSigOn = v; g_flipLines = 0; g_mkMissLines = 0; g_scanRecLines = 0; g_scanPieceLines = 0; logf("command: test recorders %d (HUDSIG, HUDCOVER, SCANREC, SCANPIECE, FLIPREC / MKMISS lines)", v); }
        else if (sscanf_s(line, "mkcarry %d", &v) == 1) { g_mkCarry2 = v; logf("command: marker points kept through frames without any %d (MKCARRY2)", v); }
        else if (sscanf_s(line, "partid %d", &v) == 1) { using PiFn = void(*)(int); HMODULE hm = GetModuleHandleW(L"dinput8.dll"); PiFn f = hm ? (PiFn)GetProcAddress(hm, "akvr_partid_set") : nullptr; if (f) f(v); g_flipLines = 0; logf("command: per-part room marks %d (PARTID; table akvr_captures\partid.txt)", v); }
        else if (sscanf_s(line, "hudring %d", &v) == 1) { g_hrOn = v; g_hrDumps = 0;   /* RINGRESET */ logf("command: HUD sheet ring %d (HUDRING)", v); }
        else if (sscanf_s(line, "hudsig %d", &v) == 1) { g_hudSigOn = v; g_hsigLines = 0; logf("command: HUD draw kinds log %d (HUDSIG)", v); }
        else if (sscanf_s(line, "scanown %d", &v) == 1) { using SoFn = void(*)(int); HMODULE hm = GetModuleHandleW(L"dinput8.dll"); SoFn f = hm ? (SoFn)GetProcAddress(hm, "akvr_scan_own_set") : nullptr; if (f) f(v); logf("command: scan box point from the game's own HUD placement %d (SCANOWN; 0 = with our HUD-layer shrink)", v); }
        else if (sscanf_s(line, "scanbase %d", &v) == 1) { g_scanBase = v; logf("command: detective projections answered head-free %d (SCANPROJ)", v); }
        else if (sscanf_s(line, "scanexact %d", &v) == 1) { g_scanExact = v; logf("command: scan box at the projected object point %d (SCANPROJ; 0 = depth march)", v); }
        else if (strncmp(line, "scanmin ", 8) == 0) { g_scanMin = (float)atof(line + 8); if (g_scanMin < 10.0f) g_scanMin = 10.0f; logf("command: scan depth ignores surfaces nearer than %.0f units", g_scanMin); }
        else if (sscanf_s(line, "mkfacepos %d", &v) == 1) { g_mkFacePos = v; logf("command: markers %s (MKFACEPOS)", v ? "tilt to face the game camera's position" : "stand upright facing the camera's heading"); }
        else if (sscanf_s(line, "mkrec %d", &v) == 1) { g_mkRecFrames = v; logf("command: marker record for %d HUD frames (akn\\mkrec_<time>.csv)", v); }   // MKREC
        else if (sscanf_s(line, "mkanchor %d", &v) == 1) { g_mkAnchorOn = v; logf("command: markers placed about their own layout anchor (MKANCHOR) %d", v); }
        else if (sscanf_s(line, "mklag %d", &v) == 1) { g_mkLagMode = v; logf("command: marker layout frame %s", v == 0 ? "the pair's own" : v == 1 ? "the other kept frame" : "chosen by measurement (auto)"); }   // MKLAG
        else if (sscanf_s(line, "mkeye %d", &v) == 1) { g_mkEye = v; logf("command: marker pieces drawn per eye at their world point (MKEYE) %d", v); }
        else if (sscanf_s(line, "marksheet %d", &v) == 1) { g_markSheet = v; logf("command: marker pieces into their own sheet %d", v); }   // MARKSHEET
        else if (sscanf_s(line, "mkpieces %d", &v) == 1) { g_mkPieces = v; logf("command: log %d HUD draws (marker routing)", v); }
        else if (sscanf_s(line, "mkmode %d", &v) == 1) { g_mkByTag = v; logf("command: marker pieces chosen by %s", v ? "the marker mark (MKTAG)" : "position"); }
        else if (sscanf_s(line, "mktag %d", &v) == 1) { using F = void(*)(int); HMODULE hm = GetModuleHandleW(L"dinput8.dll"); F f = hm ? (F)GetProcAddress(hm, "akvr_marker_tag_set") : nullptr; if (f) f(v); logf("command: marker mark in the HUD tree %d (%s)", v, f ? "sent" : "AKVR export missing"); }
        else if (sscanf_s(line, "mkworld %d", &v) == 1) { using F = void(*)(int); HMODULE hm = GetModuleHandleW(L"dinput8.dll"); F f = hm ? (F)GetProcAddress(hm, "akvr_marker_world_set") : nullptr; if (f) f(v); logf("command: marker card in the world %d (%s)", v, f ? "sent" : "AKVR export missing"); }
        else if (strncmp(line, "mktagreach ", 11) == 0) { g_mkTagReach = (float)atof(line + 11); logf("command: marked piece reach %.3f", g_mkTagReach); }
        else if (strncmp(line, "mkreach ", 8) == 0) { g_mkReach = (float)atof(line + 8); logf("command: marker piece reach %.3f", g_mkReach); }
        else if (sscanf_s(line, "vssplit %d", &v) == 1) { g_vsSplit = v; logf("command: right eye view state per mode %d", v); }   // VSSPLIT
        else if (sscanf_s(line, "vsfreshdet %d", &v) == 1) { g_vsFreshDet = v; logf("command: new right-eye view state on detective exit %d", v); }   // VSFRESH
        else if (strncmp(line, "vsfresh", 7) == 0) { g_vsFreshWant = 1; logf("command: new right-eye view state now"); }   // VSFRESH
        else if (sscanf_s(line, "vssync %d", &v) == 1) { g_vsSync = v; logf("command: ambient light copied to the right eye %d", v); }   // VSSYNC
        else if (sscanf_s(line, "psinv %d", &v) == 1) { g_psN = 0; g_psInv = v; logf("command: pixel shader inventory over %d frames", v); }
        else if (strncmp(line, "skipps", 6) == 0) { char hx[32] = "0"; sscanf_s(line + 6, " %31s", hx, (unsigned)sizeof(hx)); g_skipPs = _strtoui64(hx, nullptr, 16); logf("command: hide pixel shader %016llx in the eye renders", g_skipPs); }
        else if (sscanf_s(line, "hudskipon %d", &v) == 1) { g_hudSkipOn = v; logf("command: HUD backdrop skip rules %s", v ? "ON (pause look)" : "off"); }   // HUDSKIP
        else if (strncmp(line, "hudskip", 7) == 0) {   // HUDSKIP
            char a[32] = ""; unsigned w = 0, hh = 0; const int k = sscanf_s(line + 7, " %31s %u %u", a, (unsigned)sizeof(a), &w, &hh);
            if (k >= 1 && strcmp(a, "clear") == 0) { g_hudSkipN = 0; logf("command: HUD skip rules cleared"); }
            else if (k >= 2 && strcmp(a, "add") == 0) { }
            if (k >= 1 && strcmp(a, "add") == 0 && g_hudSkipN < 8) {
                unsigned long long vsh = 0; unsigned w2 = 0, h2 = 0; char hx[32] = "";
                if (sscanf_s(line + 7, " add %31s %u %u", hx, (unsigned)sizeof(hx), &w2, &h2) >= 1) { vsh = _strtoui64(hx, nullptr, 16); g_hudSkip[g_hudSkipN].vs = vsh; g_hudSkip[g_hudSkipN].w = w2; g_hudSkip[g_hudSkipN].h = h2; ++g_hudSkipN;
                    logf("command: HUD skip rule %ld: vs %016llx texture %ux%u", g_hudSkipN, vsh, w2, h2); } } }
        else if (strncmp(line, "menufitstat", 11) == 0) {   // MENUFIT2 counters (AKVR)
            using McFn = long(*)(long*); HMODULE m = GetModuleHandleW(L"dinput8.dll"); McFn f = m ? (McFn)GetProcAddress(m, "akvr_menufit_counts") : nullptr;
            long seen = -1; const long fixes = f ? f(&seen) : -1; logf("MENUFIT2: %ld screen-size answers changed to 16:9, %ld screen-size questions", fixes, seen); }
        else if (sscanf_s(line, "hudvp %d", &v) == 1) { g_hudVp = v; logf("command: whole-window HUD viewports stretched to the sheet %d", v); }   // HUDVP
        else if (sscanf_s(line, "menufit %d", &v) == 1) {   // MENUFIT2 test switch (AKVR: 16:9 screen-size answers to pop-up menus)
            using MfFn = void(*)(int); HMODULE m = GetModuleHandleW(L"dinput8.dll"); MfFn f = m ? (MfFn)GetProcAddress(m, "akvr_set_menufit") : nullptr;
            if (f) f(v); logf("command: pop-up menus told 16:9 %d (%s)", v, f ? "sent to AKVR" : "AKVR export missing"); }
        else if (sscanf_s(line, "huddsv %d", &v) == 1) { g_hudDsvOn = v; logf("command: the HUD sheet's own depth/stencil %d", v); }   // HUDDSV
        else if (sscanf_s(line, "hudall %d", &v) == 1) { g_hudAll = v; logf("command: every window-buffer bind outside the eye renders to the HUD sheet %d", v); }
        else if (sscanf_s(line, "huddef %d", &v) == 1) { g_hudDef = v; logf("command: recorded window-buffer binds to the HUD sheet %d", v); }
        else if (sscanf_s(line, "hudfull %d", &v) == 1) {   // HUDTRACE: AKVR's HUDFULL (HUD laid out at the headset size) on / off
            using HfFn = void(*)(int); HMODULE m = GetModuleHandleW(L"dinput8.dll"); HfFn f = m ? (HfFn)GetProcAddress(m, "akvr_set_hudfull") : nullptr;
            if (f) f(v); logf("command: HUD at the headset size %d (%s)", v, f ? "sent to AKVR" : "AKVR export missing"); }
        else if (sscanf_s(line, "mapflat %d", &v) == 1) {   // MAPFLAT test switch (AKVR: map / pause screens the flat game's way)
            using MfFn = void(*)(int); HMODULE m = GetModuleHandleW(L"dinput8.dll"); MfFn f = m ? (MfFn)GetProcAddress(m, "akvr_set_map_flat") : nullptr;
            if (f) f(v); logf("command: map screen the flat game's way %d (%s)", v, f ? "sent to AKVR" : "AKVR export missing"); }
        else if (sscanf_s(line, "gloss %d", &v) == 1) { g_gloss = v ? 1 : 0; logf("command: suit gloss from the head-free camera %d", g_gloss); }   // SUITGLOSS3
        else if (sscanf_s(line, "hudineye %d", &v) == 1) {   // HUDINEYE test switch (AKVR draws the HUD into the eyes)
            using HieFn = void(*)(int); HMODULE m = GetModuleHandleW(L"dinput8.dll"); HieFn f = m ? (HieFn)GetProcAddress(m, "akvr_set_hud_in_eye") : nullptr;
            if (f) f(v); logf("command: HUD in the eyes %d (%s)", v, f ? "sent to AKVR" : "AKVR export missing"); }
        else if (sscanf_s(line, "hudblend %d", &v) == 1) { g_hudBlend = v; logf("command: HUD coverage blend %d (swaps so far %ld, other modes %ld, variants %d)", v, c_bvSwaps, c_bvOther, g_bvN); }
        else if (strncmp(line, "capmk", 5) == 0) {   // MK3: JJ's panel capture - both eyes + HUD + marker data in one go
            char t[16] = "c"; sscanf_s(line + 5, " %15s", t, (unsigned)sizeof(t)); strcpy_s(g_capTag, t); g_capWant = 4; g_capHud = 1; g_mkDump = 80; g_projLog = 40; g_mkPieces = 200;   // + MKPIECE log (marker routing)
            mk_census(t); hud_rec_dump(t); g_vsInv = 3; g_vwArm = 1; g_vcDumps = 0;
            if (!g_fxCollect) { g_psN = 0; g_psInv = 3; }   // DIAGKIT: + akn\ps_inventory.txt (AKVR saves the drawn shaders' code into the capture)
            if (g_hrOn) akn_hudring_dump(t, 1);   // DIAGKIT replay: the last frames before the button
            if (g_pnOn) ps_new_list();   // DIAGKIT: effects that appeared since watching started
            if (g_skipPs || g_fxWatch) { g_fxcbHave[0] = g_fxcbHave[1] = 0; g_fxcbArm = 1; }   // FXCB: its constants in the next two eye renders
            if (g_skipPs) logf("CAPTURE: effect hidden now: pixel shader %016llx (%ld of %ld)", (unsigned long long)g_skipPs, g_fxIdx + 1, g_fxN); }   // SPRITEFIND: + the vertex shaders drawn now (akns_inventory.txt)
        else if (strncmp(line, "capdet", 6) == 0) {   // CAPDET, now on request only (CAPSLIM 2026-10-10: it ran on EVERY capture,
            // up to 14 extra 45 MB pairs per press - drive E filled up, 70 GB in akn\, and a capture came out empty)
            char t[16] = "d"; sscanf_s(line + 6, " %15s", t, (unsigned)sizeof(t));
            if (!InterlockedExchange(&g_capDetBusy, 1)) { char* a = new char[16]; strcpy_s(a, 16, t); HANDLE th = CreateThread(nullptr, 0, capdet_seq, a, 0, nullptr); if (th) CloseHandle(th); else { delete[] a; g_capDetBusy = 0; } } }
        else if (strncmp(line, "mkforce", 7) == 0) { int x = 0, y = 0; if (sscanf_s(line + 7, "%d %d", &x, &y) == 2) { g_mkForceXY[0] = x / 1000.0f; g_mkForceXY[1] = y / 1000.0f; g_mkForce = 1; logf("command: MK2 marker projection forced to %.3f %.3f", g_mkForceXY[0], g_mkForceXY[1]); } else { g_mkForce = 0; logf("command: MK2 marker projection free"); } }
        else if (sscanf_s(line, "mkdump %d", &v) == 1) { g_mkDump = v; g_projLog = 40; logf("command: MK1 HUD draw dump %d draws + 40 PROJ lines", v); }
        else if (sscanf_s(line, "mipbias %d", &v) == 1) { g_mipBias = v; logf("command: texture mip bias %d (0 off, 1 log2(scale)-1, 2 log2(scale))", v); }
        else if (sscanf_s(line, "xeye %d", &v) == 1) { g_xeye = v; logf("command: cross-eye texture check for %d frames", v); }
        else if (sscanf_s(line, "dlsspreset %d", &v) == 1) { akn_dlss_cmd("preset", v); logf("command: DLSS preset %d", v); }
        else if (sscanf_s(line, "dlssdepthinv %d", &v) == 1) { akn_dlss_cmd("depthinv", v); logf("command: DLSS depth inverted %d", v); }
        else if (sscanf_s(line, "dlssmvjit %d", &v) == 1) { akn_dlss_cmd("mvjit", v); logf("command: DLSS motion vectors jittered %d", v); }
        else if (sscanf_s(line, "dlssmv %d", &v) == 1) { akn_dlss_cmd("mvmode", v); logf("command: DLSS motion %d (0 game buffer, 1 camera, 2 camera + moving objects)", v); }
        else if (sscanf_s(line, "dlssmvsigny %d", &v) == 1) { akn_dlss_cmd("mvsigny", v); logf("command: DLSS motion vector y sign %d", v); }
        else if (sscanf_s(line, "d0probe %d", &v) == 1) { g_d0 = v; g_d0Logs = 0; logf("command: DLSS groundwork probe %d", v); }
        else if (sscanf_s(line, "projlog %d", &v) == 1) { g_projLog = v; logf("command: log the next %d world-to-screen projections", v); }
        else if (sscanf_s(line, "rainview %d", &v) == 1) { g_rainView = v; logf("command: rain view %d (0 normal, 1 rain hidden, 2 only the rain)", v); }
        else if (sscanf_s(line, "rainpause %d", &v) == 1) { g_rainPause = v; logf("command: rain paused %d", v); }
        else if (sscanf_s(line, "farrain %d", &v) == 1) { g_farRain = v; logf("command: rain regions follow the game camera, not the head %d", v); }
        else if (sscanf_s(line, "nearrain %d", &v) == 1) { g_nearRain = v; logf("command: rain block around the game camera, not the head %d", v); }
        else if (sscanf_s(line, "farrow %d", &v) == 1) { g_farRow = v; logf("command: rain camera-forward row %d", v); }
        else if (sscanf_s(line, "raincb %d", &v) == 1) { g_rainCbDump = v; logf("command: dump the rain step's constants %d times", v); }
        else if (sscanf_s(line, "vsinv %d", &v) == 1) { g_vsInv = v; logf("command: list the vertex shaders of %d frame(s)", v); }
        else if (strncmp(line, "skiprange ", 10) == 0) { char* e = nullptr; g_skipLo = _strtoui64(line + 10, &e, 16); g_skipHi = e ? _strtoui64(e, nullptr, 16) : 0; logf("command: hide draws with vertex shaders %016llx..%016llx", g_skipLo, g_skipHi); }
        else if (strncmp(line, "skipvs ", 7) == 0) { g_skipVs = _strtoui64(line + 7, nullptr, 16); logf("command: hide draws with vertex shader %016llx in both eyes", g_skipVs); }
        else if (sscanf_s(line, "raindump %d", &v) == 1) { g_rainDump = v; logf("command: compare the rain draw's inputs between the eyes for %d frames", v); }
        else if (sscanf_s(line, "panel %d", &v) == 1) { akvr_panel_cmd(v); logf("command: VR panel %d", v); }
        else if (sscanf_s(line, "eyemark %d", &v) == 1) { g_eyeMark = v; logf("command: eye mark %d", v); }
        else if (sscanf_s(line, "rogue %d", &v) == 1) { if (v && !g_rogue) { g_rogue = v; CreateThread(nullptr, 0, rogue_thread, nullptr, 0, nullptr); } else if (!v) g_rogue = 0; logf("command: rogue device %d", v); }
        else if (sscanf_s(line, "lumshare %d", &v) == 1) { g_lumShare = v; logf("command: shared exposure %d", v); }
        else if (sscanf_s(line, "eyesame %d", &v) == 1) { g_eyeSame = v; logf("command: eyes identical %d", v); }
        else if (sscanf_s(line, "hud %d", &v) == 1) { g_hudOn = v; if (!v) g_hudPhase = false; logf("command: HUD redirect %d", v); }
        else if (strncmp(line, "trace", 5) == 0) { g_traceWant = 3; logf("command: trace one render per eye"); }
}
// CAPDET 2026-10-09 (JJ: detective mode leaves the right eye half in it, plus a vignette in each eye; the fake headset
// cannot switch detective mode on - pad-only and no keyboard key in the config files): every panel capture also saves
// both eyes' view states (vs_<tag>_<eye>.bin) and the eye renders' pixel shaders (ps_<tag>.txt); shaders not in the
// normal-gameplay reference (akn\\ps_normal_ref.txt, from the fake-headset run) are then hidden one at a time with a
// pair picture each (pair_<tag>s<i>.bmp, log "CAPDET") - a capture in detective mode runs the vignette hunt by itself.
volatile LONG g_capDetBusy = 0;
DWORD WINAPI capdet_seq(LPVOID arg)
{
    char tag[16]; strcpy_s(tag, (const char*)arg); delete[] (char*)arg;
    Sleep(1500);   // the capture's own pictures first
    strcpy_s(g_vsSnapTag, tag); g_vsSnap = 1; Sleep(400);
    g_psN = 0; g_psInv = 4;
    for (int i = 0; i < 40 && g_psInv > 0; ++i) Sleep(100);
    wchar_t dir[MAX_PATH]; GetModuleFileNameW(nullptr, dir, MAX_PATH); wchar_t* sl = wcsrchr(dir, L'\\'); if (sl) sl[1] = 0; wcscat_s(dir, L"akn\\");
    auto read_set = [&](const wchar_t* name, std::vector<unsigned long long>& v) {
        wchar_t p2[MAX_PATH]; wcscpy_s(p2, dir); wcscat_s(p2, name);
        FILE* f = nullptr; _wfopen_s(&f, p2, L"r"); if (!f) return false;
        char ln[64]; while (fgets(ln, sizeof(ln), f)) { const unsigned long long h = _strtoui64(ln, nullptr, 16); if (h) v.push_back(h); }
        fclose(f); return true; };
    std::vector<unsigned long long> inv, ref;
    read_set(L"ps_inventory.txt", inv);
    {   // keep this capture's list
        wchar_t src[MAX_PATH], dst[MAX_PATH]; wcscpy_s(src, dir); wcscat_s(src, L"ps_inventory.txt");
        _snwprintf_s(dst, MAX_PATH, _TRUNCATE, L"%sps_%hs.txt", dir, tag); CopyFileW(src, dst, FALSE);
    }
    std::vector<unsigned long long> cand;
    if (read_set(L"ps_normal_ref.txt", ref))
        for (auto h : inv) if (std::find(ref.begin(), ref.end(), h) == ref.end() && cand.size() < 14) cand.push_back(h);
    logf("CAPDET %s: %zu pixel shaders, %zu not in the normal reference%s", tag, inv.size(), cand.size(), ref.empty() ? " (no reference file)" : "");
    for (size_t i = 0; i < cand.size(); ++i)
    {
        g_skipPs = cand[i]; Sleep(350);
        _snprintf_s(g_capTag, sizeof(g_capTag), _TRUNCATE, "%.9ss%zu", tag, i);
        InterlockedOr(&g_capWant, 4);
        for (int k = 0; k < 40 && (g_capWant & 4); ++k) Sleep(100);
        logf("CAPDET %s s%zu: pixel shader %016llx hidden -> pair_%s.bmp", tag, i, cand[i], g_capTag);
    }
    g_skipPs = 0;
    InterlockedExchange(&g_capDetBusy, 0);
    return 0;
}
// HWDET 2026-10-09 (JJ: right eye stays half in detective mode after the exit; copying the view-state fields the
// game changes (+0xC0) crashed both ways, so: find the GAME CODE that resets the game's own view state on the exit,
// to run the same reset on the right eye's). Hardware write-watch (method: swbf2vr hwwatch, memory
// hw-read-watch-finds-readers): DR0..DR3 = game view state + the given offsets (4 bytes, writes only) on every
// thread; each new writer logs its exe offset, the watch that fired and its registers. "hwdet <off> [off..]" arms
// (hex offsets, up to 4), "hwdet off" disarms.
volatile LONG g_hwN = 0; uintptr_t g_hwSite[96][2] = {}; uintptr_t g_hwAddr[4] = {};
LONG CALLBACK hw_handler(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(ep->ContextRecord->Dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
    const unsigned dr = (unsigned)(ep->ContextRecord->Dr6 & 0xF); ep->ContextRecord->Dr6 = 0;
    const uintptr_t rip = ep->ContextRecord->Rip;   // the instruction AFTER the write
    const LONG n = g_hwN; bool seen = false;
    for (LONG i = 0; i < n && i < 96; ++i) if (g_hwSite[i][0] == rip && g_hwSite[i][1] == dr) { seen = true; break; }
    if (!seen && n < 96)
    {
        const LONG i = InterlockedIncrement(&g_hwN) - 1;
        if (i < 96)
        {
            g_hwSite[i][0] = rip; g_hwSite[i][1] = dr;
            const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
            const CONTEXT& c = *ep->ContextRecord;
            uintptr_t ret = 0; __try { ret = *(const uintptr_t*)c.Rsp; } __except (EXCEPTION_EXECUTE_HANDLER) {}
            const int w = dr & 1 ? 0 : dr & 2 ? 1 : dr & 4 ? 2 : 3;
            logf("HWDET write to game view state +0x%llX: after exe+0x%llX (static 0x%llX) tid %lu [rsp] exe+0x%llX | rax %llX rbx %llX rcx %llX rdx %llX rsi %llX rdi %llX r8 %llX r9 %llX",
                 (unsigned long long)(g_hwAddr[w] - (uintptr_t)g_vsOf[0]), (unsigned long long)(rip - base), (unsigned long long)(rip - base + 0x140000000ull),
                 GetCurrentThreadId(), (unsigned long long)(ret - base), c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.R8, c.R9);
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}
int hw_arm_all()
{
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te{ sizeof te }; int n = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
    {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) != (DWORD)-1)
        {
            CONTEXT c{}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &c))
            {
                c.Dr0 = g_hwAddr[0]; c.Dr1 = g_hwAddr[1]; c.Dr2 = g_hwAddr[2]; c.Dr3 = g_hwAddr[3];
                unsigned long long d7 = c.Dr7 & ~0xFFFF00FFull;
                for (int k = 0; k < 4; ++k)   // Lk enable, RWk = 01 (writes), LENk = 11 (4 bytes)
                    if (g_hwAddr[k]) d7 |= (1ull << (2 * k)) | (1ull << (16 + 4 * k)) | (3ull << (18 + 4 * k));
                c.Dr7 = d7;
                if (SetThreadContext(t, &c)) ++n;
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    return n;
}
void hwdet_command(const char* arg)
{
    static bool handler = false;
    if (!handler) handler = AddVectoredExceptionHandler(1, hw_handler) != nullptr;
    for (auto& a : g_hwAddr) a = 0;
    if (strncmp(arg, "off", 3) == 0 || !g_vsOf[0]) { logf("HWDET off (%s): %d threads", g_vsOf[0] ? "asked" : "no game view state yet", hw_arm_all()); return; }
    unsigned o[4] = {}; const int k = sscanf_s(arg, "%x %x %x %x", &o[0], &o[1], &o[2], &o[3]);
    for (int i = 0; i < k && i < 4; ++i) g_hwAddr[i] = ((uintptr_t)g_vsOf[0] + o[i]) & ~(uintptr_t)3;
    g_hwN = 0;
    logf("HWDET armed on %d threads: game view state %p + %d offsets (first +0x%X)", hw_arm_all(), g_vsOf[0], k, o[0]);
}
void poll_commands()
{
    wchar_t p[MAX_PATH]; wcscpy_s(p, g_dir); wcscat_s(p, L"command.txt");
    FILE* f = _wfopen(p, L"r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) do_command(line);
    fclose(f);
    DeleteFileW(p);
}
float fslot(U v) { float f; uint32_t u = (uint32_t)v; memcpy(&f, &u, 4); return f; }
extern M44 g_eyeVP[2][2]; extern volatile LONG g_eyeVPok[2];   // RAINDEPTH (below)
float g_eyeJit[2][2] = {};   // DLSS D1: this frame's jitter per eye (render pixels), carried with the render tag
// MARKERPROBE 2026-10-08 (JJ: grapple reticles not attached to their world objects; VR_WORLD_MARKERS_GUIDE.md §2):
// FSceneView::Project 0x81FF10 (view, out FVector4, world point) - UCanvas::execProject calls it at RA 0x86DC0D. Log
// who projects which points with which view (eye views are noted in the FSceneView ctor below).
void* volatile g_viewOf[3] = {};   // the last view built for: [0] left eye, [1] right eye, [2] mono / other
using ProjFn = float* (__fastcall*)(void*, float*, const float*);
ProjFn o_Proj = nullptr;
volatile LONG g_projLog = 0, c_projCalls = 0;
struct RaCount { uintptr_t ra; volatile LONG n; };
RaCount g_projRa[24] = {};

// MK1 2026-10-08 (JJ: fix the reticles and distance markers, VR_WORLD_MARKERS_GUIDE.md): probe stage.
// (a) MARKERBASE check: the game camera rebuilt from AKVR's axes + position must equal the game's own view matrix
//     (left eye, before our eye changes); then the same build from the HEAD-FREE axes + position = the reference view.
// (b) HUD pieces: VS cb0 rows of every HUD-phase draw (Scaleform placement), next to the PROJ calls of that frame.
M44 g_mkGameV, g_mkGameP; volatile LONG g_mkGameOk = 0;
M44 g_mkVfull, g_mkVbase; volatile LONG g_mkBaseOk = 0;
// LIGHTLAG 2026-10-14 (JJ: smoke steady with head moves now, "but when you move the camera with the gamepad, it still changes colour"):
// the sprite light directions are made a frame or more before the draw (UE3 thread lag) - LIGHTDIR can use the head-free camera of N
// frames back. "lightlag 0..3" (panel "light delay"), default 1.
M44 g_mkVbaseHist[4]; volatile LONG g_mkHistHead = 0, g_lightLag = 1;
volatile LONG g_mkDump = 0, g_mkDumpN = 0, g_mkChecks = 0;
volatile LONG g_mkForce = 0; float g_mkForceXY[2] = {};   // MK2: command "mkforce x y" (thousandths), "mkforce off"
int g_mkSignR = 1, g_mkSignU = 1;
using MkAxesFn = int(*)(float*, float*, float*, float*, float*, float*);
MkAxesFn p_mkAxes = nullptr;
void mk_view(const float o[3], const float f[3], const float r[3], const float u[3], int sr, int su, M44& v)
{
    memset(&v, 0, sizeof(v));
    for (int i = 0; i < 3; ++i) { v.m[i][0] = sr * r[i]; v.m[i][1] = su * u[i]; v.m[i][2] = f[i]; }
    for (int k = 0; k < 3; ++k) v.m[3][k] = -(o[0] * v.m[0][k] + o[1] * v.m[1][k] + o[2] * v.m[2][k]);
    v.m[3][3] = 1.0f;
}
float mk_diff(const M44& a, const M44& b, bool rot)
{
    float d = 0; for (int i = 0; i < (rot ? 3 : 4); ++i) for (int k = 0; k < 3; ++k) { const float x = fabsf(a.m[i][k] - b.m[i][k]); if (x > d) d = x; }
    return d;
}
void mk_xform(const float w[3], const M44& v, const M44& p, float c[4])
{
    float e[4];
    for (int k = 0; k < 4; ++k) e[k] = w[0] * v.m[0][k] + w[1] * v.m[1][k] + w[2] * v.m[2][k] + v.m[3][k];
    for (int k = 0; k < 4; ++k) c[k] = e[0] * p.m[0][k] + e[1] * p.m[1][k] + e[2] * p.m[2][k] + e[3] * p.m[3][k];
}
extern volatile LONG g_hudSeq;
// MARKERCUT 2026-10-08 (JJ: distance marker / reticles not on their objects, moving against the head; Arkham City
// MARKEREYE + the geo-11 on-target points, VR_WORLD_MARKERS_GUIDE): every Canvas.Project (RA 0x86DC0D) of a game frame
// is kept as world point W + the game's own screen point (left eye NDC); AKVR matches them to the HUD's on-target points
// and draws each marker's HUD area into each eye at W (akn_markers / akn_pair_vp exports).
extern volatile unsigned long long g_famStamp;   // below
struct McPt { float w[3], r[2]; };
McPt g_mcPts[2][16]; int g_mcN[2] = {}; int g_mcCur = 0; unsigned long long g_mcStampCur = 0;
unsigned long long g_mcStampBuf[2] = {};   // PAIRPTS: the frame stamp of each buffer
extern volatile unsigned long long g_pairStamp;
SRWLOCK g_mcLock = SRWLOCK_INIT;
M44 g_mcEyeVP[2], g_mcPubVP[2]; int g_mcEyeOk = 0; volatile LONG g_mcPubOk = 0;
M44 g_mcPubBVP; volatile LONG g_mcPubBvpOk = 0;   // MKEYE: the head-free game camera x the game's lens the pair's HUD was laid out with
M44 mul44(const M44& a, const M44& b)
{
    M44 r; for (int i = 0; i < 4; ++i) for (int k = 0; k < 4; ++k) r.m[i][k] = a.m[i][0] * b.m[0][k] + a.m[i][1] * b.m[1][k] + a.m[i][2] * b.m[2][k] + a.m[i][3] * b.m[3][k];
    return r;
}
void mc_note(const float* world, const float* r)
{
    AcquireSRWLockExclusive(&g_mcLock);
    const unsigned long long st = g_famStamp;
    if (st != g_mcStampCur) { g_mcStampCur = st; g_mcCur ^= 1; g_mcN[g_mcCur] = 0; g_mcStampBuf[g_mcCur] = st; }
    int& n = g_mcN[g_mcCur]; bool dup = false;
    for (int i = 0; i < n; ++i) if (fabsf(g_mcPts[g_mcCur][i].w[0] - world[0]) + fabsf(g_mcPts[g_mcCur][i].w[1] - world[1]) + fabsf(g_mcPts[g_mcCur][i].w[2] - world[2]) < 0.5f) { dup = true; break; }
    if (!dup && n < 16) { McPt& q = g_mcPts[g_mcCur][n++]; q.w[0] = world[0]; q.w[1] = world[1]; q.w[2] = world[2]; q.r[0] = r[0]; q.r[1] = r[1]; }
    ReleaseSRWLockExclusive(&g_mcLock);
}
// SCANPROJ: the detective HUD's projections (RA 0x5A2BB1) per game frame, like mc_note (same lock, own buffers)
// (g_scanBase / g_scanExact: with the DETCALLOUT globals)
McPt g_spPts[2][64]; int g_spN[2] = {}; int g_spCur = 0; unsigned long long g_spStampCur = 0, g_spStampBuf[2] = {};
void scan_proj_note(const float* world, const float* r)
{
    AcquireSRWLockExclusive(&g_mcLock);
    const unsigned long long st = g_famStamp;
    if (st != g_spStampCur) { g_spStampCur = st; g_spCur ^= 1; g_spN[g_spCur] = 0; g_spStampBuf[g_spCur] = st; }
    int& n = g_spN[g_spCur];
    bool dup = false;
    for (int i = 0; i < n && !dup; ++i) dup = fabsf(g_spPts[g_spCur][i].w[0] - world[0]) + fabsf(g_spPts[g_spCur][i].w[1] - world[1]) + fabsf(g_spPts[g_spCur][i].w[2] - world[2]) < 0.5f;
    if (!dup && n < 64) { McPt& q = g_spPts[g_spCur][n++]; q.w[0] = world[0]; q.w[1] = world[1]; q.w[2] = world[2]; q.r[0] = r[0]; q.r[1] = r[1]; }
    ReleaseSRWLockExclusive(&g_mcLock);
}
// out: per point W xyz + R xy (5 floats), this frame's then the previous frame's; returns the count
extern "C" __declspec(dllexport) int akn_markers(float* out, int max)
{
    if (!out || max <= 0) return 0;
    int k = 0;
    AcquireSRWLockShared(&g_mcLock);
    // PAIRPTS 2026-10-11: the points of the SHOWN pair's frame first (the game thread can already be in the next frame's HUD)
    const int first = g_mcStampBuf[g_mcCur ^ 1] == g_pairStamp && g_mcStampBuf[g_mcCur] != g_pairStamp ? (g_mcCur ^ 1) : g_mcCur;
    for (int f = 0; f < 2; ++f) { const int s = f == 0 ? first : first ^ 1; for (int i = 0; i < g_mcN[s] && k < max; ++i, ++k) { const McPt& q = g_mcPts[s][i]; out[k * 5] = q.w[0]; out[k * 5 + 1] = q.w[1]; out[k * 5 + 2] = q.w[2]; out[k * 5 + 3] = q.r[0]; out[k * 5 + 4] = q.r[1]; } }
    ReleaseSRWLockShared(&g_mcLock);
    return k;
}
// HUDSIZE 2026-10-08 (JJ's captures 22:34-22:44: map HUD the wrong size, main menu button prompt out of place): AKVR
// laid the screen HUD out at engineres x the LIVE engineshape (925, re-measured and saved mid-session) while this HUD
// texture was made at the launch shape (811 -> 2320 wide): HUD draws had a 2640-wide viewport on a 2320-wide sheet =
// stretched 14 % sideways, the right edge cut. AKVR now asks for THIS size (fixed for the session at the first call).
extern "C" __declspec(dllexport) int akn_hud_out_size(int* w, int* h)
{
    if (!w || !h || akn_dlss_mode() <= 0) return 0;
    UINT ow = 0, oh = 0; akn_dlss_out_size(ow, oh);
    *w = (int)ow; *h = (int)oh;
    return ow > 0 && oh > 0;
}
// out: the left and the right eye's view x lens (row vectors, unjittered) of the newest complete pair
extern float g_mcPubAx[9]; extern volatile LONG g_mcPubAxOk;
// PAIRAXES: the head-free game camera's axes (forward, right, up; world) the shown pair was built with
extern "C" __declspec(dllexport) int akn_pair_axes(float* out9)
{
    AcquireSRWLockShared(&g_mcLock); const int ok = g_mcPubAxOk ? 1 : 0; if (ok) memcpy(out9, g_mcPubAx, sizeof(float) * 9); ReleaseSRWLockShared(&g_mcLock);
    return ok;
}
extern "C" __declspec(dllexport) int akn_pair_vp(float* out32)
{
    if (!out32 || !g_mcPubOk) return 0;
    AcquireSRWLockShared(&g_mcLock); memcpy(out32, &g_mcPubVP[0].m[0][0], 64); memcpy(out32 + 16, &g_mcPubVP[1].m[0][0], 64); ReleaseSRWLockShared(&g_mcLock);
    return 1;
}
void mk_census(const char* tag)
{
    char cl[600]; int o = _snprintf_s(cl, sizeof(cl), _TRUNCATE, "MK3 capture %s: world-to-screen callers so far (total %ld):", tag, c_projCalls);
    for (auto& e : g_projRa) if (e.ra && o > 0) o += _snprintf_s(cl + o, sizeof(cl) - o, _TRUNCATE, " %llX x%ld", (unsigned long long)e.ra, e.n);
    logf("%s", cl);
}
// MARKERBASE 2026-10-08 (JJ's headset test 22:3x: the distance marker "is still turning on its own axis when I move my
// head ... this should only happen when the camera is turned with the mouse or the gamepad"; captures c224419-433: the
// 59 m marker wanders over the HUD sheet as the head turns). Canvas.Project ran with the LEFT EYE's view (head, eye
// offset and turned lens), so the game laid the marker out - place and 3D tilt - from the head. Answer it from the
// head-free game camera and the game's own lens (VR_WORLD_MARKERS_GUIDE §3, Arkham City MARKERBASE): the sheet shows
// the flat game's marker, which moves only with the game camera; MARKERCUT still puts it on its object per eye.
float* __fastcall hk_Proj(void* view, float* out, const float* world)
{
    float* r = o_Proj(view, out, world);
    const uintptr_t ra = (uintptr_t)_ReturnAddress() - g_base;
    const float own[4] = { r ? r[0] : 0.f, r ? r[1] : 0.f, r ? r[2] : 0.f, r ? r[3] : 0.f };
    if (g_mkBase && r && world && ra == 0x86DC0D && g_mkBaseOk && g_mkGameOk)
    {
        float c[4]; mk_xform(world, g_mkVbase, g_mkGameP, c);
        if (fabsf(c[3]) > 1e-4f) { const float rw = 1.0f / c[3]; r[0] = c[0] * rw; r[1] = c[1] * rw; r[2] = c[2] * rw; r[3] = c[3]; }   // UE3 FSceneView::Project
    }
    // SCANPROJ 2026-10-12 (JJ: the scan box + line "moving all around the place with your head movements and camera movements";
    // his log 16:3x: this caller ran ~1,000/s in detective mode only): the detective HUD projects each scannable object here -
    // its world point and the screen point the box is drawn at. Answered head-free like Canvas.Project (MARKERBASE), and kept
    // so the box's marker takes the object's EXACT world point (moving drones included) instead of a depth guess.
    if (ra == 0x5A2BB1 && r && world)
    {
        if (g_scanBase && g_mkBaseOk && g_mkGameOk)
        {
            float c[4]; mk_xform(world, g_mkVbase, g_mkGameP, c);
            if (fabsf(c[3]) > 1e-4f) { const float rw = 1.0f / c[3]; r[0] = c[0] * rw; r[1] = c[1] * rw; r[2] = c[2] * rw; r[3] = c[3]; }
        }
        scan_proj_note(world, r);
    }
    if (g_mkForce && r && ra == 0x86DC0D) { r[0] = g_mkForceXY[0]; r[1] = g_mkForceXY[1]; }   // MK2 calibration: a known screen point
    if (r && world && ra == 0x86DC0D) mc_note(world, r);   // MARKERCUT
    InterlockedIncrement(&c_projCalls);
    for (auto& e : g_projRa) { if (e.ra == ra) { InterlockedIncrement(&e.n); break; } if (!e.ra) { e.ra = ra; InterlockedIncrement(&e.n); break; } }
    if (g_projLog > 0 && InterlockedDecrement(&g_projLog) >= 0 && r && world)
    {
        const char* who = view == g_viewOf[0] ? "left eye" : view == g_viewOf[1] ? "right eye" : view == g_viewOf[2] ? "mono" : "other";
        float cf[4] = {}, cb[4] = {};
        if (g_mkBaseOk && g_mkGameOk) { mk_xform(world, g_mkVfull, g_mkGameP, cf); mk_xform(world, g_mkVbase, g_mkGameP, cb); }
        logf("PROJ ra %llX thread %lu view %p (%s) world %.0f %.0f %.0f -> %.3f %.3f depth %.4f w %.1f (game's own %.3f %.3f) | MK1 rebuilt full %.3f %.3f, head-free %.3f %.3f (hud frame %ld)", (unsigned long long)ra, GetCurrentThreadId(), view, who, world[0], world[1], world[2], r[0], r[1], r[2], r[3], own[0], own[1],
             cf[3] ? cf[0] / cf[3] : 0.f, cf[3] ? cf[1] / cf[3] : 0.f, cb[3] ? cb[0] / cb[3] : 0.f, cb[3] ? cb[1] / cb[3] : 0.f, g_hudSeq);
    }
    return r;
}
// AFW 2026-10-10 (afw.cpp, NOTES "AFW PLAN"): an AFW frame draws ONE eye with the game's own family; the other eye's view
// + lens (built from the same game camera, no jitter) is the warp's destination, carried with the render tag
volatile bool g_afwFrame = false;     // game thread: the views being built belong to an AFW frame
M44 g_afwDstV = {}, g_afwDstP = {}; volatile LONG g_afwDstOk = 0;
int g_afwNext = 0;                    // the eye the next AFW frame draws (game thread; one clock, STR-012 / STR-014)
volatile LONG c_afwEye[2] = {};
unsigned long long jitter_frame() { return g_afwFrame ? (unsigned long long)c_stereoFrames >> 1 : (unsigned long long)c_stereoFrames; }   // AFW: each eye walks the whole jitter sequence
// One eye's view + lens from the game's own (view space x right, y up, z forward; row vectors, translation in row 3).
// jit: add this frame's DLSS jitter (noted in g_eyeJit[eye]). Was inline in hk_Ctor before AFW.
void eye_build(M44& view, M44& proj, int eye, float w, float h, bool jit)
{
        // EYESAME (5c test T1b): both eyes from one spot with one lens -> identical content, so any brightness gap left
        // is per-eye exposure (each eye has its own view state = its own eye adaptation)
        const float ex = (g_eyeSame == 1 || g_eyeSame == 2) ? 0.0f : g_eyesApi ? g_eyeOff[eye] : (eye == 0 ? -0.5f : 0.5f) * g_ipd;
        view.m[3][0] -= ex + g_eyeShift[eye];          // EYESHIFT (leak test): extra sideways move of this eye only
        view.m[3][1] -= 0.5f * g_eyeShift[eye];        // ...and up, so shadows and reflections change clearly
        // square pixels for this eye's rect (the game builds its locked 16:9 shape for whatever rect it gets)
        if (g_asym == 2)
        {
            // TURNEDEYE (JJ's 5th Quest test: lighting, shadows and reflections differ per eye; T17: same spot, mirrored
            // off-centre lenses -> lights missing in one eye = AK's light culling assumes a CENTRED lens; geo-11 era:
            // "off-centre projection made lights/decals slide"). Each eye: a centred lens just covering its field of
            // view, the camera turned to that field's centre (yaw = mean of left/right angles, pitch = mean of up/down).
            // AKVR declares the same turned direction + centred lens, so drawn = declared.
            const int le = (g_eyeSame == 1 || g_eyeSame == 3) ? 0 : eye;   // RF2: eyesame 3 = each eye its own spot, both the left lens
            const float aL = atanf(g_tan[le][0]), aR = atanf(g_tan[le][1]), aU = atanf(g_tan[le][2]), aD = atanf(g_tan[le][3]);
            const float yc = 0.5f * (aL + aR), pc = 0.5f * (aU + aD);
            const float hH = 0.5f * (aR - aL), hV = 0.5f * (aU - aD);
            proj.m[0][0] = 1.0f / tanf(hH); proj.m[1][1] = 1.0f / tanf(hV); proj.m[2][0] = 0.0f; proj.m[2][1] = 0.0f;
            if (jit && akn_dlss_mode() > 0)
            {   // DLSS D1: a sub-pixel lens offset per frame (both eyes the same phase); DLSS gets it in render pixels
                const float vw = w, vh = h;
                float jx = 0, jy = 0; akn_dlss_jitter(jitter_frame(), jx, jy);
                if (vw > 0 && vh > 0) { proj.m[2][0] += 2.0f * jx / vw; proj.m[2][1] += -2.0f * jy / vh; g_eyeJit[eye][0] = jx; g_eyeJit[eye][1] = jy; }
            }
            // eye frame in view space (x right, y up, z forward): forward f, right r, up u; p_eye = p_view * [r u f]
            const float cy = cosf(yc), sy = sinf(yc), cp = cosf(pc), sp = sinf(pc);
            const float f[3] = { sy * cp, sp, cy * cp }, r[3] = { cy, 0.0f, -sy }, u[3] = { -sy * sp, cp, -cy * sp };
            M44 out;
            for (int i = 0; i < 4; ++i)
            {
                out.m[i][0] = view.m[i][0] * r[0] + view.m[i][1] * r[1] + view.m[i][2] * r[2];
                out.m[i][1] = view.m[i][0] * u[0] + view.m[i][1] * u[1] + view.m[i][2] * u[2];
                out.m[i][2] = view.m[i][0] * f[0] + view.m[i][1] * f[1] + view.m[i][2] * f[2];
                out.m[i][3] = view.m[i][3];
            }
            view = out;
            if (g_asymLogs < 2) { ++g_asymLogs; logf("eye %d turned lens: yaw %.2f pitch %.2f deg, half-angles %.2f x %.2f deg", eye, yc * 57.29578f, pc * 57.29578f, hH * 57.29578f, hV * 57.29578f); log_m("eye proj", &proj.m[0][0]); }
        }
        else if (g_asym)
        {
            // M4: per-eye off-centre lens from the four edge tangents (OpenXR angles; left/down negative). Row-vector
            // LH projection: x_ndc = (x/z)*m00 + m20. Keep the game's depth terms (near plane) and add its own small
            // x/y offsets (temporal jitter, if any) on top of ours.
            const int le = g_eyeSame == 1 ? 0 : eye;   // eyesame 2: same spot but each eye its own lens (T17)
            float l = g_tan[le][0], r = g_tan[le][1], u = g_tan[le][2], d = g_tan[le][3];
            // GEO11OFF (JJ's 3rd Quest test): the game's own x/y offsets were AKVR's FULLVIEW vertical shift (0.19 NDC on the
            // Quest's uneven lens), added on top of ours = a doubled shift -> shear on head turns. AK has no TAA jitter
            // (offsets 0 in the plain game, M4 log), so the lens is ours alone.
            const float jx = 0.0f, jy = 0.0f;
            proj.m[0][0] = 2.0f / (r - l); proj.m[2][0] = -(r + l) / (r - l) + jx;
            proj.m[1][1] = 2.0f / (u - d); proj.m[2][1] = -(u + d) / (u - d) + jy;
            if (g_asymLogs < 2) { ++g_asymLogs; logf("eye %d lens tan l %.3f r %.3f u %.3f d %.3f (game x/y offsets %.5f %.5f)", eye, l, r, u, d, jx, jy); log_m("eye proj", &proj.m[0][0]); }
        }
        else if (w > 0 && h > 0)
        {
            proj.m[0][0] = proj.m[1][1] * (h / w);
            // MAPFLAT (AKVR: map / plain pause screens = the game's own lens): DLSS still needs this frame's jitter
            proj.m[2][0] = 0.0f; proj.m[2][1] = 0.0f;
            if (jit && akn_dlss_mode() > 0)
            {
                float jx = 0, jy = 0; akn_dlss_jitter(jitter_frame(), jx, jy);
                proj.m[2][0] += 2.0f * jx / w; proj.m[2][1] += -2.0f * jy / h; g_eyeJit[eye][0] = jx; g_eyeJit[eye][1] = jy;
            }
        }
}
void* __fastcall hk_Ctor(U a1, U a2, U a3, U a4, U a5, U a6, U a7, U a8, U a9, U a10, U a11, U a12, U a13, U a14, U a15, U a16,
                         U a17, U a18, U a19, U a20, U a21, U a22, U a23, U a24, U a25, U a26, U a27, U a28, U a29, U a30, U a31, U a32, U a33, U a34)
{
    const int eye = tl_eye;
    g_viewOf[eye == 0 || eye == 1 ? eye : 2] = (void*)a1;   // MARKERPROBE
    if ((eye == 0 || eye == 1) && a3) g_vsOf[eye] = (void*)a3;   // DETECTIVE: this eye's view state
    if (g_vsSnap > 0 && eye == 0 && g_vsOf[0] && g_vsOf[1] && (uintptr_t)_ReturnAddress() == g_raCtorCalc)
    {   // both view states, as they are while this frame's views are built
        g_vsSnap = 0;
        for (int e = 0; e < 2; ++e)
        {
            wchar_t path[MAX_PATH]; GetModuleFileNameW(nullptr, path, MAX_PATH); wchar_t* sl = wcsrchr(path, L'\\');
            if (sl) _snwprintf_s(sl + 1, MAX_PATH - (sl + 1 - path), _TRUNCATE, L"akn\\vs_%hs_%d.bin", g_vsSnapTag, e);
            FILE* f = nullptr; _wfopen_s(&f, path, L"wb");
            if (f) { __try { fwrite(g_vsOf[e], 1, 0x8850, f); } __except (EXCEPTION_EXECUTE_HANDLER) {} fclose(f); }
        }
        logf("DETECTIVE: view states saved (tag %s): game %p, right eye %p", g_vsSnapTag, g_vsOf[0], g_vsOf[1]);
    }
    if (eye < 0 && (uintptr_t)_ReturnAddress() == g_raCtorCalc && a18)
    {
        // mono frame: arm stereo once the game's camera is real (the first frames carry an infinite projection)
        const float m11 = ((const M44*)a18)->m[1][1];
        g_camOk = m11 > 0.01f && m11 < 100.0f;
        if (g_camOk && g_mono_proj_logs < 1) { ++g_mono_proj_logs; log_m("game's own proj", &((const M44*)a18)->m[0][0]); log_m("game's own view", &((const M44*)a17)->m[0][0]); }
    }
    M44 view, proj;
    if (eye >= 0 && (uintptr_t)_ReturnAddress() == g_raCtorCalc && a17 && a18)
    {
        InterlockedIncrement(&c_ctorEye[eye]);
        view = *(const M44*)a17; proj = *(const M44*)a18;
        if (eye == 0 || g_afwFrame) { g_mkGameV = view; g_mkGameP = proj; g_mkGameOk = 1; }   // MK1 (AFW: the frame's only view)
        const float w = fslot(a15), h = fslot(a16);
        if (g_ctorLogs < 4)
        {
            ++g_ctorLogs;
            logf("view ctor eye %d: rect %.0f,%.0f %.0fx%.0f state %llX", eye, fslot(a11), fslot(a12), w, h, (unsigned long long)a3);
            log_m("view", &view.m[0][0]); log_m("proj", &proj.m[0][0]);
        }
        // AFW: the other eye's view + lens from the SAME game camera, no jitter = where the warp puts the drawn eye
        if (g_afwFrame) { M44 ov = view, op = proj; eye_build(ov, op, eye ^ 1, w, h, false); g_afwDstV = ov; g_afwDstP = op; g_afwDstOk = 1; g_eyeVP[eye ^ 1][0] = ov; g_eyeVP[eye ^ 1][1] = op; g_eyeVPok[eye ^ 1] = 1; }
        eye_build(view, proj, eye, w, h, true);
        a17 = (U)&view; a18 = (U)&proj;
        g_eyeVP[eye][0] = view; g_eyeVP[eye][1] = proj; g_eyeVPok[eye] = 1;   // RAINDEPTH
    }
    void* res = o_Ctor(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20, a21, a22, a23, a24, a25, a26, a27, a28, a29, a30, a31, a32, a33, a34);
    return res;
}
int second_eye(void* p, void* fam, void* loc, void* rot, void* vp, void* drawer)
{
    __try { o_Calc(p, fam, loc, rot, vp, drawer); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
// M3 (2026-10-07): M2's two views in one family broke the game's post effects (JJ: glows in the wrong place, the
// right half purple behind Batman with a blocky mask) - AK's post passes assume one view filling the target. So each
// eye now gets its OWN family with one full-size view. The game's family (0x70 bytes, ctor 0x821E90 in client Draw A
// at 0x9F8AA7, stored at client +0x140) is still empty (Views count +8 == 0) when CalcSceneView runs: clone it then,
// build the right eye's view into the clone, render the clone first (BeginRenderingViewFamily), and free it right
// after (dtor 0x823E10 + GMalloc free 0xFA2A80, the game's own pair at 0x9F2406) - the scene renderer copies the views.
using FamDtorFn = void(__fastcall*)(void*);
using FreeFn = void(__fastcall*)(void*);
using MallocFn = void*(__fastcall*)(uint32_t, uint32_t);
FamDtorFn p_FamDtor = nullptr; FreeFn p_Free = nullptr; MallocFn p_Malloc = nullptr;
void* volatile g_fam1 = nullptr;   // the game's family this frame (left eye)
void* volatile g_fam2 = nullptr;   // our clone (right eye)
volatile bool g_fam1Afw = false;   // AFW: the game's family this frame draws ONE eye (g_fam1Eye), no clone
volatile int g_fam1Eye = 0;
void drop_fam2()
{
    void* f = g_fam2;
    if (!f) return;
    g_fam2 = nullptr;
    __try { p_FamDtor(f); p_Free(f); }
    __except (EXCEPTION_EXECUTE_HANDLER) { logf("freeing the right-eye family FAULTED"); }
}
bool akvr_axes(float base[3], float full[3]);   // FARRAIN2, below
float g_calcBase[6] = {}; volatile LONG g_calcBaseOk = 0;   // NEARRAIN2: this frame's game camera without the head
using PosDeltaFn = int(*)(float*);
PosDeltaFn p_posDelta = nullptr;
// PAIRAXES 2026-10-11 (JJ: "Could the jitter be the head pose delay? We had issues in the past with the head pose delay on
// markers being different from the world"): AKVR shaped the marker card from the NEWEST head-free camera (its ring, 0
// Presents ago) while the shown pair was built from the camera of its own frame - a frame apart whenever the camera turns.
// The axes used to build this frame's views travel with the pair (renderer tag -> publish with akn_pair_vp).
float g_mkAx[9] = {}; volatile LONG g_mkAxOk = 0;   // game thread: this frame's head-free axes (fwd, right, up)
M44 g_rendBVP; volatile LONG g_rendBVPok = 0;   // MKEYE: render thread, the eye render in progress
float g_rendAx[9] = {}; volatile LONG g_rendAxOk = 0;   // render thread: the eye render in progress
float g_mcPubAx[9] = {}; volatile LONG g_mcPubAxOk = 0;   // published with the pair (g_mcLock)
void mk_build(const float* loc)
{
    if (!p_mkAxes) { HMODULE m = GetModuleHandleW(L"dinput8.dll"); if (m) p_mkAxes = (MkAxesFn)GetProcAddress(m, "akvr_marker_axes"); }
    float bf[3], br[3], bu[3], ff[3], fr[3], fu[3];
    if (!p_mkAxes || !loc || !g_calcBaseOk || !p_mkAxes(bf, br, bu, ff, fr, fu)) { g_mkBaseOk = 0; g_mkAxOk = 0; return; }
    for (int k = 0; k < 3; ++k) { g_mkAx[k] = bf[k]; g_mkAx[3 + k] = br[k]; g_mkAx[6 + k] = bu[k]; } g_mkAxOk = 1;   // PAIRAXES
    if (g_mkGameOk && g_mkChecks < 3)
    {   // which signs of right / up rebuild the game's own matrix
        float best = 1e9f; int bs = 1, bu2 = 1;
        for (int sr = -1; sr <= 1; sr += 2) for (int su = -1; su <= 1; su += 2) { M44 t; mk_view(loc, ff, fr, fu, sr, su, t); const float d = mk_diff(t, g_mkGameV, true); if (d < best) { best = d; bs = sr; bu2 = su; } }
        g_mkSignR = bs; g_mkSignU = bu2;
        M44 t; mk_view(loc, ff, fr, fu, bs, bu2, t);
        ++g_mkChecks;
        logf("MK1 base check: rebuilt game camera vs the game's view: rotation diff %.5f, whole %.3f (signs right %d up %d)", best, mk_diff(t, g_mkGameV, false), bs, bu2);
        if (g_mkChecks == 1) { log_m("MK1 game view", &g_mkGameV.m[0][0]); log_m("MK1 rebuilt", &t.m[0][0]); log_m("MK1 game proj", &g_mkGameP.m[0][0]); }
    }
    mk_view(loc, ff, fr, fu, g_mkSignR, g_mkSignU, g_mkVfull);
    mk_view(g_calcBase, bf, br, bu, g_mkSignR, g_mkSignU, g_mkVbase);
    { const LONG h = (g_mkHistHead + 1) & 3; g_mkVbaseHist[h] = g_mkVbase; g_mkHistHead = h; }   // LIGHTLAG
    g_mkBaseOk = 1;
}
void* stereo_calc(void* p, void* fam, void* loc, void* rot, void* vp, void* drawer)
{
    if (g_vsSync && g_vsOf[0] && g_vsOf[1] && g_vsOf[0] != g_vsOf[1])   // VSSYNC: the ambient light coefficients, game -> right eye
        __try {
            memcpy((uint8_t*)g_vsOf[1] + 0x5B0, (const uint8_t*)g_vsOf[0] + 0x5B0, 0x660 - 0x5B0);
            for (LONG i = 0; i < g_vsCopyN && i < 16; ++i)   // VSCOPY
                if (g_vsCopy[i].off >= 0 && g_vsCopy[i].len > 0 && g_vsCopy[i].off + g_vsCopy[i].len <= 0x8850)
                    memcpy((uint8_t*)g_vsOf[1] + g_vsCopy[i].off, (const uint8_t*)g_vsOf[0] + g_vsCopy[i].off, g_vsCopy[i].len);
        } __except (EXCEPTION_EXECUTE_HANDLER) { g_vsSync = 0; }
    drop_fam2();   // left over from a frame whose render was skipped
    g_calcBaseOk = 0;   // NEARRAIN2: filled after the game's CalcSceneView (the view location is an OUTPUT of it)
    if (*(int32_t*)((uintptr_t)fam + 8) != 0) return o_Calc(p, fam, loc, rot, vp, drawer);   // not a fresh family
    const uintptr_t pl = (uintptr_t)p;
    void*& vs = *(void**)(pl + 0xC8);
    void* const vs0 = vs;
    if (g_vsMode == 3 && !g_vs3)
    {
        g_vs3 = p_AllocVs(0, 0);
        if (g_vs3 && vs0) { log_vs("game's", vs0); log_vs("new (before)", g_vs3); *(uint64_t*)((uintptr_t)g_vs3 + 0x750) = *(uint64_t*)((uintptr_t)vs0 + 0x750); log_vs("new (+0x750 copied)", g_vs3); }
        if (!g_vs3) g_vsMode = 0;
    }
    bool vsSpecial = false;   // VSSPLIT: detective mode or a flat screen (map) now
    {   // VSFRESH: detective mode just ended (seen in the last 300 ms .. now gone for 300 ms)
        static bool detWas = false;
        const bool detNow = det_on();   // DETHOLD
        const bool detExit = detWas && !detNow;   // (VSSPLIT bug 04:18: tested detWas after it was overwritten)
        if (detExit && g_vsFreshDet) g_vsFreshWant = 1;
        detWas = detNow;
        using FlatFn = int (*)(); static FlatFn flat = nullptr; static bool tried = false;
        if (!tried) { HMODULE m = GetModuleHandleW(L"dinput8.dll"); flat = m ? (FlatFn)GetProcAddress(m, "akvr_flat_screen_now") : nullptr; tried = m != nullptr; }
        vsSpecial = detNow || (flat && flat());   // DETHOLD (was 100 ms: flipped mid-detective at low frame rates)
        if (g_vsSplit && g_vsMode == 4 && g_vs4 && !g_vsNorm && detExit)
        {   // VSSPLIT: the normal-play view state, made once at the first detective exit
            void* m = p_Malloc(0x8850, 8);
            if (m) { memset(m, 0, 0x8850); p_VsCtor(m, 0, 0); g_vsNorm = m; logf("VSSPLIT: right eye normal-play view state %p (detective / map: %p)", m, g_vs4); }
        }
    }
    if (g_vsFreshWant && g_vsMode == 4 && g_vs4)
    {
        g_vsFreshWant = 0;
        void* m = p_Malloc(0x8850, 8);
        if (m) { memset(m, 0, 0x8850); p_VsCtor(m, 0, 0); logf("VSFRESH: right eye view state renewed (%p -> %p, #%ld)", g_vs4, m, InterlockedIncrement(&c_vsFresh)); g_vs4 = m; }
    }
    if (g_vsMode == 4 && !g_vs4)
    {
        // the game's own view state was made at start-up in fresh (zeroed) memory; renderer fields the ctor leaves
        // alone (+0x750 = a per-frame light pointer, set only when one is found) then read as empty
        void* m = p_Malloc(0x8850, 8);
        if (m) { memset(m, 0, 0x8850); p_VsCtor(m, 0, 0); g_vs4 = m; log_vs("game's", vs0); log_vs("new zeroed", m); }
        else g_vsMode = 0;
    }
    if (g_vsMode == 5 && !g_vs5)
    {
        g_vs5 = p_AllocVs(0, 0);
        if (g_vs5) { *(uint64_t*)((uintptr_t)g_vs5 + 0x750) = 0; log_vs("new, +0x750 zeroed", g_vs5); }
        else g_vsMode = 0;
    }
    if (g_vsMode == 1 && !g_vs2)
    {
        g_vs2 = p_AllocVs(0, 0);
        logf("second view state %p (game's %p)", g_vs2, vs0);
        if (g_vs2 && vs0) { log_vs("game's", vs0); log_vs("new", g_vs2); }
        if (!g_vs2) { g_vsMode = 0; }
    }
    // the right eye's own view state (VSSPLIT: the normal-play one outside detective mode / flat screens)
    void* vsR = g_vsMode == 1 ? g_vs2 : g_vsMode == 3 ? g_vs3 : g_vsMode == 4 ? g_vs4 : g_vsMode == 5 ? g_vs5 : g_vsMode == 2 ? nullptr : vs0;
    if (g_vsMode == 4 && g_vsSplit && g_vsNorm && !vsSpecial) vsR = g_vsNorm;   // VSSPLIT
    auto after_calc = [&]() {   // NEARRAIN2 (run 34: read BEFORE the call it was 0,0,0 - CalcSceneView writes the location): this
        // frame's game camera without the head = the view location minus AKVR's head offset; AKVR's base axes
        if (!p_posDelta) { HMODULE m = GetModuleHandleW(L"dinput8.dll"); if (m) p_posDelta = (PosDeltaFn)GetProcAddress(m, "akvr_rain_pos_delta"); }
        float d[3] = {}, full[3];
        const bool okD = p_posDelta && p_posDelta(d), okF = akvr_axes(g_calcBase + 3, full);
        if (loc && okD && okF) { const float* L = (const float*)loc; for (int k = 0; k < 3; ++k) g_calcBase[k] = L[k] - d[k]; g_calcBaseOk = 1; }
        mk_build((const float*)loc);   // MK1
    };
    if (akn_afw_on())
    {   // AFW 2026-10-10: no clone - the game's own family draws ONE eye, left and right on alternate frames; afw.cpp warps
        // it into the other eye after its render. The eye is decided here (game thread) and carried with the family to
        // the render thread (STR-012); the right eye keeps its own view state = its own history (M3b T3, STR-015).
        const int e = g_afwNext;
        if (e == 1) vs = vsR;
        tl_eye = e; g_afwFrame = true; g_afwDstOk = 0;
        void* r = o_Calc(p, fam, loc, rot, vp, drawer);
        after_calc();
        vs = vs0; tl_eye = -1; g_afwFrame = false;
        g_fam1 = fam; g_fam1Afw = true; g_fam1Eye = e;
        g_famStamp = fin_now();
        g_afwNext = e ^ 1; InterlockedIncrement(&c_afwEye[e]);
        InterlockedIncrement(&c_stereoFrames);
        return r;
    }
    void* f2 = p_Malloc(0x70, 8);
    if (!f2) return o_Calc(p, fam, loc, rot, vp, drawer);
    memcpy(f2, fam, 0x70);
    *(uint64_t*)f2 = 0; *(uint64_t*)((uintptr_t)f2 + 8) = 0;   // its own (empty) Views array
    const uint32_t jit = *g_jitIdx;
    tl_eye = 0;
    void* r = o_Calc(p, fam, loc, rot, vp, drawer);
    after_calc();
    const uint32_t jitAfter = *g_jitIdx; *g_jitIdx = jit;
    tl_eye = 1; vs = vsR;
    {   static void* last = nullptr; if (vs != last) { if (last) { InterlockedIncrement(&c_vsSplitSwaps); logf("VSSPLIT: right eye view state %p (%s)", vs, vs == g_vsNorm ? "normal play" : "detective / map"); } last = vs; } }
    const int ok = second_eye(p, f2, loc, rot, vp, drawer);
    *g_jitIdx = jitAfter;
    vs = vs0; tl_eye = -1;
    g_fam1 = fam; g_fam2 = f2; g_fam1Afw = false;
    g_famStamp = fin_now();
    if (g_trace10 > 0) logf("TRACE10 stereo_calc fam1 %p fam2 %p views %d/%d vs %p", fam, f2, *(int32_t*)((uintptr_t)fam + 8), *(int32_t*)((uintptr_t)f2 + 8), vs0);
    if (!ok) { drop_fam2(); g_stereo = 0; g_armed = 0; logf("SECOND EYE VIEW FAULTED - stereo off"); }
    else InterlockedIncrement(&c_stereoFrames);
    return r;
}

// ---- render thread: which eye is this scene render, and its finished picture ----
// BeginRenderingViewFamily news the scene renderer (ctor 0x818530(this, family, hit proxies, transform, int)) on the
// game thread; the render thread runs RenderViewFamily 0x81B3C0(renderer) from the draw-scene command 0x81B650.
// We note which family each renderer came from, and after 0x81B3C0 returns the bound render target holds that eye's
// finished world picture (the HUD comes later, onto the left eye only).
using RCtorFn = void*(__fastcall*)(void*, void*, void*, void*, U);
using RenderVFFn = void(__fastcall*)(void*);
RCtorFn o_RCtor = nullptr; RenderVFFn o_RenderVF = nullptr;
struct RTag { void* r; int eye; unsigned long long stamp; float basePos[3], baseFwd[3]; bool baseOk; M44 v, p; bool vpOk; float jit[2]; bool afw; M44 dv, dp; bool dOk; float ax[9]; bool axOk; M44 bvp; bool bvpOk; };   // PAIRAXES: ax   // MKEYE: bvp   // AFW: one eye this frame + the other eye's view (warp target)
float g_rendJit[2] = {};   // DLSS D1: the jitter of the eye render in progress
M44 g_rendDV = {}, g_rendDP = {}; volatile LONG g_rendDOk = 0;   // AFW: the other eye's view + lens for the render in progress
thread_local bool tl_afw = false;   // AFW: the eye render in progress is an AFW frame
volatile LONG c_afwPairs = 0, c_afwMono = 0;   // AFW: pairs with a warped eye / the drawn eye shown in both
// RAINDEPTH 2026-10-08 (JJ: paused / isolated rain "on a flat plane, all at the same depth"). RAINVP: the rain draw
// (vs f50d1365e929b3a0) projects buffer positions with VS cb0[6..9] = (translate to the DRAWING eye) x view x proj -
// row 9 is ~0 in both eyes, i.e. the positions are relative to the camera that drew them. One shared simulation
// (RAINONCE) is relative to ONE origin, so each eye put the same drops at the same place relative to itself = no
// parallax. Fix: rows 6..9 = translate(O) x this eye's view x this eye's lens, O = the simulation's origin.
M44 g_eyeVP[2][2] = {}; volatile LONG g_eyeVPok[2] = {};
M44 g_rendV, g_rendP; volatile LONG g_rendVPok = 0;
float g_frameO[3] = {}; volatile LONG g_frameOok = 0;
// NEARRAIN2 2026-10-08: the game camera WITHOUT the head for the frame whose views are being built (game thread), carried
// with each eye render to the render thread and from there to the worker threads that record the rain step
float g_rendBase[6] = {}; volatile LONG g_rendBaseOk = 0;
// POSESTAMP (JJ's 4th Quest test 2026-10-07: everything jitters on head turns): the pair carries AKVR's camera-finalize
// count from the moment both eyes' views were built (game thread), so AKVR can present it with EXACTLY the head pose
// that camera used - no fixed "N Presents ago" (FRAMEID's engine counters break: the 2nd render bumps GFrameNumber twice).
using FinNowFn = unsigned long long(*)();
FinNowFn p_finNow = nullptr;
volatile unsigned long long g_famStamp = 0;     // stamp of the views built this game frame (the left eye's family)
volatile unsigned long long g_pairStamp = 0;    // stamp of the last complete pair
volatile LONG g_poseMatched = 0, g_poseTotal = 0, g_poseDelay = 0;   // AKVR's report: presents paired by stamp
unsigned long long fin_now()
{
    if (!p_finNow) { HMODULE m = GetModuleHandleW(L"dinput8.dll"); if (m) p_finNow = (FinNowFn)GetProcAddress(m, "akvr_finalize_now"); }
    return p_finNow ? p_finNow() : 0;
}
RTag g_rtag[16] = {}; volatile LONG g_rtagN = 0;
volatile LONG c_rtEye[2] = {}, c_rtOther = 0;
void* __fastcall hk_RCtor(void* self, void* fam, void* hp, void* xf, U a5)
{
    int eye = -1;
    const bool afw = fam && fam == g_fam1 && g_stereo && g_fam1Afw;   // AFW: the eye was decided with the views (stereo_calc)
    if (fam && fam == g_fam2) eye = 1; else if (fam && fam == g_fam1 && g_stereo) eye = afw ? g_fam1Eye : 0;
    if (eye >= 0)
    {
        const LONG i = InterlockedIncrement(&g_rtagN) & 15; g_rtag[i].r = self; g_rtag[i].eye = eye; g_rtag[i].stamp = g_famStamp;
        g_rtag[i].baseOk = g_calcBaseOk != 0; for (int k = 0; k < 3; ++k) { g_rtag[i].basePos[k] = g_calcBase[k]; g_rtag[i].baseFwd[k] = g_calcBase[3 + k]; }
        g_rtag[i].vpOk = g_eyeVPok[eye] != 0; g_rtag[i].v = g_eyeVP[eye][0]; g_rtag[i].p = g_eyeVP[eye][1];   // RAINDEPTH
        g_rtag[i].jit[0] = g_eyeJit[eye][0]; g_rtag[i].jit[1] = g_eyeJit[eye][1];   // DLSS D1
        g_rtag[i].axOk = g_mkAxOk != 0; for (int k = 0; k < 9; ++k) g_rtag[i].ax[k] = g_mkAx[k];   // PAIRAXES
        g_rtag[i].bvpOk = g_mkBaseOk && g_mkGameOk; if (g_rtag[i].bvpOk) g_rtag[i].bvp = mul44(g_mkVbase, g_mkGameP);   // MKEYE
        g_rtag[i].afw = afw; g_rtag[i].dOk = afw && g_afwDstOk; if (afw) { g_rtag[i].dv = g_afwDstV; g_rtag[i].dp = g_afwDstP; }   // AFW
    }
    if (g_trace10 > 0) logf("TRACE10 renderer ctor %p fam %p -> eye %d", self, fam, eye);
    return o_RCtor(self, fam, hp, xf, a5);
}
ID3D11Device* g_dev = nullptr;       // the GAME's device: taken from its context inside the first eye render
ID3D11Device* g_hookDev = nullptr;
ID3D11DeviceContext* g_gameCtx = nullptr;   // the game's immediate context (seen inside an eye render); hooks ignore all others
thread_local ID3D11Resource* tl_lastCopyDst = nullptr;   // full-size copy destination inside the eye render = the window buffer   // any device (held): only to reach the shared context function table for hooking
// M5a: what AKN publishes to AKVR (render thread writes, AKVR reads at Present on the same thread/context)
ID3D11Texture2D* g_pair = nullptr;          // 2W x H: left half = eye 0 (left), right half = eye 1 (right), world only
volatile LONG g_pairSeq = 0;                // +1 when both halves of a frame are in
int g_pairHalves = 0;                       // bits of the halves written this frame
ID3D11Texture2D* g_hudTex = nullptr;        // the HUD pass, redirected (transparent where there is no HUD)
ID3D11RenderTargetView* g_hudRTV = nullptr;
volatile LONG g_hudOn = 0;                  // command "hud 1" / akn_set_hud
volatile bool g_hudPhase = false;           // between the left eye's render and the frame's end: HUD binds are ours
volatile bool g_hudClearPending = false;
volatile LONG g_hudSeq = 0;                 // frames whose HUD went to g_hudTex
volatile LONG c_hudRedirect = 0, c_hudClear = 0, c_hudCopy = 0;
// HUDTRACE 2026-10-09 (JJ: "all the HUD elements are still flickering" - compass, level info, distance marker, wheel,
// pause and map text; his captures c000749-752: the compass is on the HUD sheet in some frames and missing in others;
// JJ: it seems to have started with HUDFULL). Per frame: HUD draws on the sheet, redirected binds, window-buffer binds
// that MISSED the sheet (immediate outside the HUD window; deferred lists outside the eye renders), and the sheet's
// coverage (command "hudstat N"). Candidate fixes, live: "hudall 1" = every window-buffer bind outside the eye renders
// goes to the sheet, cleared right after AKVR took it; "huddef 1" = deferred lists' window-buffer binds too;
// "hudfull 0" (AKVR) = the HUD laid out at the render size again.
volatile LONG f_hudFrameDraws = 0, g_hudDrawsLast = 0;   // MENUSOUND: HUD-sheet draws in the last frame (AKVR's start-up sound hold)
volatile LONG f_hudDraws = 0, f_hudRedir = 0, f_hudMissImm = 0, f_hudBbDef = 0, f_hudDefRedir = 0, f_hudDsvBind = 0, f_hudDsvMis = 0;
volatile LONG g_capHud = 0;                 // save the HUD at the next frame start (it is complete then)
bool make_like(ID3D11Texture2D* src, UINT w, UINT bind, ID3D11Texture2D** out)
{
    D3D11_TEXTURE2D_DESC d; src->GetDesc(&d);
    d.Width = w; d.MipLevels = 1; d.ArraySize = 1; d.SampleDesc.Count = 1; d.SampleDesc.Quality = 0;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind; d.CPUAccessFlags = 0; d.MiscFlags = 0;
    return SUCCEEDED(g_dev->CreateTexture2D(&d, nullptr, out));
}
volatile LONG g_capWant = 0;   // bit per eye: save the next render of that eye to akn\eye<N>_<tag>_*.bmp
char g_capTag[16] = "a";
ID3D11Resource* volatile g_bb = nullptr;   // the window buffer = first target the HUD pass sets after the left eye
volatile bool g_eye0Done = false;
float h2f(uint16_t h)
{
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? m / 1024.0f / 16384.0f : e == 31 ? 65504.0f : (1.0f + m / 1024.0f) * ldexpf(1.0f, (int)e - 15);
    return s ? -v : v;
}
uint8_t f2b(float v) { v = v / (1.0f + v); v = v < 0 ? 0 : v > 1 ? 1 : v; return (uint8_t)(powf(v, 1.0f / 2.2f) * 255.0f + 0.5f); }   // Reinhard + gamma, just to see HDR content
float f11(uint32_t v, int mb) { const uint32_t e = v >> mb, m = v & ((1u << mb) - 1); return e == 0 ? 0.0f : (1.0f + m / float(1u << mb)) * ldexpf(1.0f, (int)e - 15); }
void save_bmp(ID3D11DeviceContext* ctx, ID3D11Resource* res, int eye, int idx = -1, const wchar_t* name = nullptr, bool alpha = false)
{
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex))) return;
    D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    sd.MipLevels = 1; sd.ArraySize = 1; sd.SampleDesc.Count = 1; sd.SampleDesc.Quality = 0;
    ID3D11Texture2D* st = nullptr;
    if (d.SampleDesc.Count == 1 && SUCCEEDED(g_dev->CreateTexture2D(&sd, nullptr, &st)))
    {
        ctx->CopySubresourceRegion(st, 0, 0, 0, 0, tex, 0, nullptr);
        D3D11_MAPPED_SUBRESOURCE m;
        // 7-run 2026-10-07: unknown formats were read as 4 bytes/pixel; the 1-byte R8 working picture (f61) then read
        // past the mapped rows (caught, but an access violation in our code). Bytes per pixel per format, others skipped.
        const int f = (int)d.Format;
        const int bpp = (f == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8 : (f == DXGI_FORMAT_R8_UNORM) ? 1 : (f == DXGI_FORMAT_R8G8_UNORM || f == DXGI_FORMAT_R16_FLOAT) ? 2
                      : (f == DXGI_FORMAT_R10G10B10A2_UNORM || f == DXGI_FORMAT_R11G11B10_FLOAT || f == DXGI_FORMAT_R16G16_FLOAT || f == DXGI_FORMAT_R32_FLOAT ||
                         f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM ||
                         f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8X8_UNORM) ? 4 : 0;
        if (bpp == 0) { logf("capture skipped: format %d not readable here", f); st->Release(); tex->Release(); return; }
        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m)))
        {
            const int w = (int)d.Width, h = (int)d.Height, row = (w * 3 + 3) & ~3;
            wchar_t fn[MAX_PATH];
            if (name) swprintf_s(fn, L"%s%s_%S.bmp", g_dir, name, g_capTag);
            else if (idx < 0) swprintf_s(fn, L"%seye%d_%S_bb.bmp", g_dir, eye, g_capTag); else swprintf_s(fn, L"%seye%d_%S_%02d_f%d.bmp", g_dir, eye, g_capTag, idx, (int)d.Format);
            FILE* f = _wfopen(fn, L"wb");
            if (f)
            {
                BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
                fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + row * h;
                ih.biSize = sizeof(ih); ih.biWidth = w; ih.biHeight = h; ih.biPlanes = 1; ih.biBitCount = 24;
                fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f);
                uint8_t* line = (uint8_t*)calloc(row, 1);
                for (int y = h - 1; y >= 0; --y)
                {
                    const uint8_t* src = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
                    for (int x = 0; x < w; ++x)
                    {
                        uint8_t r = 0, g = 0, b = 0;
                        if (d.Format == DXGI_FORMAT_R10G10B10A2_UNORM)
                        { const uint32_t v = ((const uint32_t*)src)[x]; r = (uint8_t)((v & 1023) >> 2); g = (uint8_t)(((v >> 10) & 1023) >> 2); b = (uint8_t)(((v >> 20) & 1023) >> 2); }
                        else if (d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
                        { const uint16_t* q = (const uint16_t*)src + x * 4; r = f2b(h2f(q[0])); g = f2b(h2f(q[1])); b = f2b(h2f(q[2])); }
                        else if (d.Format == DXGI_FORMAT_R11G11B10_FLOAT)
                        { const uint32_t v = ((const uint32_t*)src)[x]; r = f2b(f11(v & 2047, 6)); g = f2b(f11((v >> 11) & 2047, 6)); b = f2b(f11(v >> 22, 5)); }
                        else if (d.Format == DXGI_FORMAT_R8_UNORM) { r = g = b = src[x]; }
                        else if (d.Format == DXGI_FORMAT_R32_FLOAT) { r = g = b = f2b(((const float*)src)[x]); }
                        else if (d.Format == DXGI_FORMAT_R8G8_UNORM) { r = src[x * 2]; g = src[x * 2 + 1]; b = 0; }
                        else if (d.Format == DXGI_FORMAT_R16G16_FLOAT) { const uint16_t* q = (const uint16_t*)src + x * 2; r = f2b(h2f(q[0])); g = f2b(h2f(q[1])); b = 0; }
                        else if (d.Format == DXGI_FORMAT_R16_FLOAT) { const uint16_t* q = (const uint16_t*)src + x; r = g = b = f2b(h2f(q[0])); }
                        else if (d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_B8G8R8X8_UNORM)
                        { b = src[x * 4]; g = src[x * 4 + 1]; r = src[x * 4 + 2]; }
                        else { r = src[x * 4]; g = src[x * 4 + 1]; b = src[x * 4 + 2]; }
                        if (alpha) { const uint8_t a = (d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) ? src[x * 4 + 3] : 255; r = g = b = a; }
                        line[x * 3] = b; line[x * 3 + 1] = g; line[x * 3 + 2] = r;
                    }
                    fwrite(line, row, 1, f);
                }
                free(line); fclose(f);
            }
            ctx->Unmap(st, 0);
            logf("captured eye %d #%d: %p %ux%u format %d", eye, idx, (void*)res, d.Width, d.Height, (int)d.Format);
        }
        st->Release();
    }
    else logf("capture eye %d: cannot read %ux%u format %d samples %u", eye, d.Width, d.Height, (int)d.Format, d.SampleDesc.Count);
    tex->Release();
}
IDXGISwapChain* g_sc = nullptr;
void res_desc(ID3D11Resource* r, char* b, size_t n);
using OMSetRTFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
OMSetRTFn o_OMSetRT = nullptr;
thread_local bool tl_inEyeRender = false;
thread_local int tl_eye_render = -1;
thread_local unsigned long long tl_renderStamp = 0;   // POSESTAMP: the stamp of the renderer this thread is running   // 6a: the eye whose render this thread is in
thread_local ID3D11Resource* tl_lastRT = nullptr;   // not ref-held: the game keeps its targets alive
thread_local int tl_rtSets = 0;
thread_local char tl_rtList[6000];
thread_local ID3D11Resource* tl_big[16]; thread_local int tl_bigN = 0;
thread_local ID3D11Resource* tl_prevRT = nullptr;
int g_rtListLogs = 2;
volatile bool g_afterEye0 = false;   // set after the first left-eye scene render, cleared at the next right-eye one
int g_afterLogs = 0;   // armed by command "after"
ID3D11Resource* g_afterPrev = nullptr;
void h_write(ID3D11Resource* r);   // HISTSWAP detector (below)
using ClearRTVFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);
ClearRTVFn o_ClearRTV = nullptr;
// the HUD texture exists (made from the window buffer's shape) and is cleared once per frame before its first use
using ClearDSVFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
extern ClearDSVFn o_ClrDSV;   // defined with the clear hooks below
ID3D11DepthStencilView* g_hudDSV = nullptr;   // HUDDSV (below)
// MARKSHEET 2026-10-09 (JJ: "do that one instead" - the structural fix for HUD holes; VR_WORLD_MARKERS_GUIDE 3.3-3.4,
// Batman City markers.cpp on_hud_draw): every HUD draw that is a piece of a world marker (a flat Scaleform piece whose
// placement sits within reach of a marker AKVR placed last frame) goes into its own MARKER SHEET instead of the HUD
// sheet. AKVR then draws the HUD sheet whole (nothing cut out = no holes, other HUD never moves) and the marker sheet
// as per-eye cards at each marker's world point. Piece placement = VS cb0 (4x4, translation row 3 in NDC), from a CPU
// copy of the game's last upload (Map/Unmap, UpdateSubresource). Commands: "marksheet 0|1", "mkreach <ndc>",
// "mkpieces N" (log N HUD draws: placement, nearest marker, routed).
ID3D11Texture2D* g_mkTex = nullptr; ID3D11RenderTargetView* g_mkRTV = nullptr;
float g_mkC[8][2] = {}; int g_mkCN = 0;   // (MARKSHEET switches: next to VSSPLIT, above the commands)
// MKEYE 2026-10-11 (RETHINK MARKERS 2, NOTES.md; JJ: "This all feels very hacky" ... "look at how we did it in the Geo11
// version because they were rock solid" ... "They were at object distance"): a marker piece is never cut out of a sheet
// and re-placed later. Inside its own draw it is drawn twice - into a left-eye and a right-eye marker layer - each time
// moved, through its own placement rows, from where the game laid it out (head-free camera, MARKERBASE) to where THAT eye
// sees its world point, as a card facing the game camera (VR_WORLD_MARKERS_GUIDE §3-5, Batman City MARKEREYE / MARKERCARD).
// Every number comes from the frame being drawn: the Canvas.Project points of the pair's frame, the pair's eye views and
// the head-free view the HUD was laid out with (carried with the render tag). AKVR lays each eye layer over that eye's
// picture 1:1. No HUD-tree reads, no centres, no windows.
ID3D11Texture2D* g_mkEyeTex[2] = {}; ID3D11RenderTargetView* g_mkEyeRTV[2] = {};
struct MkEyeT { float a[4], t[2]; };   // HUD-sheet NDC -> eye NDC: q = A p + t (A row-major)
MkEyeT g_mkT[8][2] = {};
int g_mkLastMk = -1;   // the marker of the last piece drawn per eye this HUD frame (a glow paste's owner)
volatile LONG c_mkEyePieces = 0, c_mkEyeWhole = 0, c_mkEyeNoMap = 0, c_mkEyeNoView = 0, c_mkEyeFrames = 0, c_mkEyeMarkers = 0;
struct MkPlan { int kind, mk1; UINT bytes; int rowX, rowY, rowW, st, lim, nFar; unsigned long long nearM, dropM; signed char mk[64]; };   // dropM: HELDDROP2
volatile LONG c_mkEyeK[4] = {}, c_mkEyeFail = 0;   // MKEYE2: draws per kind (1 one-piece rows, 2 batch rows, 3 viewport), fallbacks
char g_mkEyeDiag[320] = "";                        // MKEYE2: the first marker's placement check
MkPlan g_mkPlan = {};   // kind 1 = one piece (rows 6 / 7), 2 = batch, 3 = whole draw moved by the viewport (glow paste)
bool inv44(const M44& a, M44& o)
{
    const float* m = &a.m[0][0]; float inv[16];
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (fabsf(det) < 1e-20f || !(det == det)) return false;
    float* d = &o.m[0][0]; for (int i = 0; i < 16; ++i) d[i] = inv[i] / det;
    return true;
}
// MKLAG 2026-10-11 (JJ: "The cause of the jitter previously with the Geo11 version was the head pose delay which needed to
// be at 2 whereas the game was at three" - the HUD markers were laid out one frame apart from the world): the game thread
// can already run the next frame (Canvas.Project of frame N+1) while the render thread draws frame N's HUD, so the layout
// being drawn may belong to either kept frame of points. Measured in play, not guessed: per HUD frame the first marker
// piece's sheet position against the marker point of each candidate; while the marker moves on screen, the candidate that
// keeps that offset steady is the layout's frame. Chosen every 90 moving frames (auto), logged; "mklag 0|1|2".
float g_mkLagC[2][2] = {}; bool g_mkLagCOk = false;           // this HUD frame's marker-0 point, both candidates (sheet)
float g_mkLagP[2] = {}; bool g_mkLagPOk = false;              // this HUD frame's first marker-0 piece (sheet)
double g_mkLagSum[2] = {}; int g_mkLagN = 0; char g_mkLagDiag[200] = "MKLAG: not measured yet";
void mk_lag_piece(int b, float x, float y) { if (b == 0 && !g_mkLagPOk) { g_mkLagP[0] = x; g_mkLagP[1] = y; g_mkLagPOk = true; } }
// MKANCHOR 2026-10-11 (JJ: "It's stable when holding completely still, but ... when moving the camera, it jitters"; MKLAG:
// the first piece's offset from the marker point changes 0.006-0.012 per moving frame against EITHER kept frame = the
// game's widget does not follow the projected point rigidly - it eases / lags by part of a frame): each marker's pieces
// are placed about their OWN layout anchor this frame = the first piece drawn for it minus that piece's usual offset
// from the marker point (learnt while the marker point is still). The marker then sits exactly on W in each eye
// whatever the widget's easing; its pieces keep their relative layout.
// Also measured (rotation with head turns): the first piece's offset and shape change while the marker point is still.
struct MkAnc { float w[3], o[2], c[2], sh[4]; bool ok, cOk, shOk; ULONGLONG t; };
MkAnc g_mkAnc[8] = {};
int g_mkAncSlot[8] = {};                     // this frame: marker -> anchor slot (-1 none)
bool g_mkAncDone[8] = {};                    // this frame: first piece seen
float g_mkF0[8][2][2] = {}, g_mkJm[8][2][4] = {};   // this frame: per marker and eye, W's eye point and the card's J
float g_mkSx = 1.0f, g_mkSy = 1.0f;
volatile LONG c_mkAncUsed = 0, c_mkAncLearn = 0, c_mkAncReject = 0, c_mkAncNew = 0;
double g_mkStillPos = 0.0, g_mkStillShape = 0.0; LONG g_mkStillN = 0;
int mk_anc_slot(const float* W)
{
    const ULONGLONG now = GetTickCount64(); int fr = -1;
    for (int k = 0; k < 8; ++k)
    {
        MkAnc& a = g_mkAnc[k];
        if (!a.t || now - a.t > 2000) { if (fr < 0) fr = k; continue; }
        const float dx = a.w[0] - W[0], dy = a.w[1] - W[1], dz = a.w[2] - W[2];
        if (dx * dx + dy * dy + dz * dz < 60.0f * 60.0f) { a.w[0] = W[0]; a.w[1] = W[1]; a.w[2] = W[2]; a.t = now; return k; }
    }
    if (fr < 0) return -1;
    MkAnc& a = g_mkAnc[fr]; a = MkAnc{}; a.w[0] = W[0]; a.w[1] = W[1]; a.w[2] = W[2]; a.t = now; InterlockedIncrement(&c_mkAncNew);
    return fr;
}
// the first piece of marker b this frame (sheet position, its placement rows' linear part)
void mk_anchor_piece(int b, float px, float py, const float* X, const float* Y)
{
    if (b < 0 || b >= g_mkCN || g_mkAncDone[b]) return;
    g_mkAncDone[b] = true;
    const int k = g_mkAncSlot[b]; if (k < 0) return;
    MkAnc& a = g_mkAnc[k];
    const float cx = g_mkC[b][0], cy = g_mkC[b][1];
    const float ox = px - cx, oy = py - cy;
    const bool still = a.cOk && fabsf(cx - a.c[0]) + fabsf(cy - a.c[1]) < 0.0005f;
    const float sh[4] = { X ? X[0] : 0.0f, X ? X[1] : 0.0f, Y ? Y[0] : 0.0f, Y ? Y[1] : 0.0f };
    if (still && a.ok)
    {   // measurement: does the layout itself move or reshape while the marker point stands still (head turns)?
        g_mkStillPos += fabsf(ox - a.o[0]) + fabsf(oy - a.o[1]);
        if (a.shOk) g_mkStillShape += fabsf(sh[0] - a.sh[0]) + fabsf(sh[1] - a.sh[1]) + fabsf(sh[2] - a.sh[2]) + fabsf(sh[3] - a.sh[3]);
        ++g_mkStillN;
    }
    for (int i = 0; i < 4; ++i) a.sh[i] = sh[i]; a.shOk = X && Y;
    a.c[0] = cx; a.c[1] = cy; a.cOk = true;
    if (still)
    {
        if (!a.ok || fabsf(ox - a.o[0]) + fabsf(oy - a.o[1]) > 0.05f) { a.o[0] = ox; a.o[1] = oy; a.ok = true; }
        else { a.o[0] += (ox - a.o[0]) * 0.1f; a.o[1] += (oy - a.o[1]) * 0.1f; }
        InterlockedIncrement(&c_mkAncLearn);
    }
    if (!g_mkAnchorOn || !a.ok) return;
    if (fabsf(ox - a.o[0]) + fabsf(oy - a.o[1]) > 0.06f) { InterlockedIncrement(&c_mkAncReject); return; }   // another piece came first
    const float G[2] = { px - a.o[0], py - a.o[1] };   // this frame's layout anchor (sheet)
    for (int e = 0; e < 2; ++e)
    {
        MkEyeT& T = g_mkT[b][e];
        T.t[0] = g_mkF0[b][e][0] - (T.a[0] * G[0] + T.a[1] * G[1]);
        T.t[1] = g_mkF0[b][e][1] - (T.a[2] * G[0] + T.a[3] * G[1]);
    }
    InterlockedIncrement(&c_mkAncUsed);
}
void mk_lag_measure()
{   // at a HUD frame start: compare the frame just drawn with the one before it
    static float pc[2][2] = {}, pp[2] = {}; static bool have = false;
    const bool now = g_mkLagCOk && g_mkLagPOk;
    if (now && have)
    {
        const float mv = fabsf(g_mkLagC[0][0] - pc[0][0]) + fabsf(g_mkLagC[0][1] - pc[0][1]);
        if (mv > 0.002f && mv < 0.2f)
        {
            for (int k = 0; k < 2; ++k)
            {
                const float ox = g_mkLagP[0] - g_mkLagC[k][0], oy = g_mkLagP[1] - g_mkLagC[k][1];
                const float px = pp[0] - pc[k][0], py = pp[1] - pc[k][1];
                g_mkLagSum[k] += fabsf(ox - px) + fabsf(oy - py);
            }
            if (++g_mkLagN >= 90)
            {
                const double a = g_mkLagSum[0] / g_mkLagN, b = g_mkLagSum[1] / g_mkLagN;
                if (g_mkLagMode == 2 && (a > b * 1.3 || b > a * 1.3)) g_mkLagUse = b < a ? 1 : 0;
                _snprintf_s(g_mkLagDiag, sizeof(g_mkLagDiag), _TRUNCATE, "MKLAG: piece offset change per moving frame - pair's frame %.4f, other frame %.4f -> using %s (%s)",
                            a, b, (g_mkLagMode == 2 ? g_mkLagUse : g_mkLagMode) ? "the other frame" : "the pair's frame", g_mkLagMode == 2 ? "auto" : "set");
                logf("%s", g_mkLagDiag);
                g_mkLagSum[0] = g_mkLagSum[1] = 0.0; g_mkLagN = 0;
            }
        }
    }
    have = now;
    if (now) { for (int k = 0; k < 2; ++k) { pc[k][0] = g_mkLagC[k][0]; pc[k][1] = g_mkLagC[k][1]; } pp[0] = g_mkLagP[0]; pp[1] = g_mkLagP[1]; }
    g_mkLagCOk = false;   // (g_mkLagPOk: cleared after MKREC reads it, end of mk_eye_frame)
}
float g_mkCardN[3] = {};   // MKREC: marker 0's card facing (world, toward the viewer side)
// MKCARD 2026-10-11 (JJ after MKUPRIGHT: "it's still subtly rotating on x and y axis when turning my head left and right or
// up and down"): moving the pieces inside their own draw is an AFFINE map (Scaleform's 2D shaders write w = 1), so a card
// off the view centre had no perspective taper - the eye reads the missing taper as the card swinging on its x / y axis.
// The pieces go into the flat marker sheet (their own draws, same routing) and each marker is shown as a real
// perspective quad per eye (AKVR's CARDPERSP drawing): an upright card at W facing the camera's heading, up = world up,
// sized by the game's layout scale, its sheet window centred on this frame's marker point.
struct MkCardD { float w[3], ax[3], ay[3], c[2]; };
float g_mkK[8][2][12] = {};   // MKGS: per marker and eye, K0 / Kx / Ky (4 floats each)   // W, world half-axes per unit of sheet NDC, sheet centre
MkCardD g_mkCardD[8] = {}; int g_mkCardDN = 0; M44 g_mkCardEv[2];
// MKFACE: an eye's camera position from its view x lens (row vectors): the point with clip x = y = w = 0
bool mk_cam_centre(const M44& M, float C[3])
{
    const int ks[3] = { 0, 1, 3 }; double A[3][3], b[3];
    for (int r = 0; r < 3; ++r) { for (int j = 0; j < 3; ++j) A[r][j] = M.m[j][ks[r]]; b[r] = -M.m[3][ks[r]]; }
    const double det = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) - A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) + A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
    if (fabs(det) < 1e-12) return false;
    for (int c = 0; c < 3; ++c)
    {
        double T[3][3]; for (int r = 0; r < 3; ++r) for (int j = 0; j < 3; ++j) T[r][j] = j == c ? b[r] : A[r][j];
        C[c] = (float)((T[0][0] * (T[1][1] * T[2][2] - T[1][2] * T[2][1]) - T[0][1] * (T[1][0] * T[2][2] - T[1][2] * T[2][0]) + T[0][2] * (T[1][0] * T[2][1] - T[1][1] * T[2][0])) / det);
    }
    return true;
}
// DETCALLOUT 2026-10-12 (JJ: detective mode's scan box "should be locked on to the drones as they move around"): the game
// places the box at the scanned object's screen point but makes no projection call we can hook (capmk 23:03:51: Canvas.Project
// projected only the 118 m objective). Its world point is found from the picture, as Batman City's AIMDEPTH does for its aim
// reticles: a one-thread compute pass marches the HEAD-FREE camera's ray through the box (kScanN geometric distances) and
// returns the first sample at or behind the LEFT eye's depth (the depth buffer holds the left eye at HUD time; its own
// view x lens, from the shown pair). Read back three passes later, never waited on. The distance, not the point, is kept,
// and applied to the current frame's ray, so a moving drone keeps its box. The depth convention (inverted or not) comes from
// the eye's own matrix: the sign of the device-depth change between a near and a far sample.
constexpr int kScanN = 96;
constexpr float kScanSpan = 250.0f;   // last sample = g_scanMin x kScanSpan
struct ScanRay { float c[3], d[3]; M44 vp; bool ok; } g_scanRay = {};
float g_scanDist = 0.0f;              // last answer: distance along the ray (game units); 0 = none yet
float g_scanRaw = -1.0f;              // DETCALLOUT diag: hit index of the last answer (-1 none, 999 sky)
ID3D11DeviceContext* g_mkCtx = nullptr;   // the context mk_frame_start runs on (the HUD sheet clear)
ID3D11ComputeShader* g_scanCs = nullptr; ID3D11Buffer* g_scanCb = nullptr; ID3D11Buffer* g_scanOut = nullptr;
ID3D11UnorderedAccessView* g_scanUav = nullptr; ID3D11Buffer* g_scanStage[4] = {}; float g_scanStageMin[4] = {};
unsigned g_scanK = 0; bool g_scanFailed = false; float g_scanSignDbg = 0.0f; UINT g_scanRwDbg = 0, g_scanRhDbg = 0;
bool scan_ensure(ID3D11Device* dev)
{
    if (g_scanCs) return true;
    if (g_scanFailed) return false;
    static const char cs[] =
        // DC1 run 7: the march on the graphics card (geometric steps, then 12 halvings between the last step in front and the
        // first behind: the distance to ~0.002 % instead of the 6 % step)
        "Texture2D<float> D : register(t0);\n"
        "cbuffer C : register(b0) { float4 P; float4 Cm; float4 Dr; float4 M0; float4 M1; float4 M2; float4 M3; };\n"
        "RWBuffer<uint> R : register(u0);\n"
        "bool beyond(float t) {\n"
        "  float3 w = Cm.xyz + Dr.xyz * t;\n"
        "  float4 c = w.x * M0 + w.y * M1 + w.z * M2 + M3;\n"
        "  if (c.w <= 1e-3) return false;\n"
        "  float2 n = c.xy / c.w;\n"
        "  float2 px = float2((n.x * 0.5 + 0.5) * P.x, (0.5 - n.y * 0.5) * P.y);\n"
        "  if (px.x < 0 || px.y < 0 || px.x >= P.x || px.y >= P.y) return false;\n"
        "  float dd = D.Load(int3(int2(px), 0));\n"
        "  return P.w * (c.z / c.w - dd) >= 0;\n"
        "}\n"
        "float tAt(float i) { return Cm.w * pow(Dr.w, i / (P.z - 1)); }\n"
        "[numthreads(1, 1, 1)] void main() {\n"
        "  uint hit = 0xFFFF; float a = 0, b = 0;\n"
        "  for (uint i = 0; i < (uint)P.z; ++i) { float t = tAt((float)i); if (beyond(t)) { hit = i; b = t; a = i > 0 ? tAt((float)i - 1) : t; break; } }\n"
        "  float dist = 0;\n"
        "  if (hit != 0xFFFF) { for (uint k = 0; k < 12; ++k) { float m = 0.5 * (a + b); if (beyond(m)) b = m; else a = m; } dist = b; }\n"
        "  R[0] = hit; R[1] = asuint(dist);\n"
        "}\n";
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    CompileFn compile = m ? (CompileFn)GetProcAddress(m, "D3DCompile") : nullptr;
    ID3DBlob* b = nullptr; ID3DBlob* err = nullptr;
    bool ok = compile && SUCCEEDED(compile(cs, sizeof(cs) - 1, "akn_scandepth", nullptr, nullptr, "main", "cs_5_0", 0, 0, &b, &err)) && b &&
              SUCCEEDED(dev->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_scanCs));
    if (b) b->Release();
    if (err) { logf("DETCALLOUT: depth shader: %s", (const char*)err->GetBufferPointer()); err->Release(); }
    D3D11_BUFFER_DESC cd{ 7 * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    D3D11_BUFFER_DESC od{ 32, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, 0, 0 };   // hit + 7 diagnostic words
    D3D11_BUFFER_DESC sd{ 32, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0 };
    ok = ok && SUCCEEDED(dev->CreateBuffer(&cd, nullptr, &g_scanCb)) && SUCCEEDED(dev->CreateBuffer(&od, nullptr, &g_scanOut));
    if (ok)
    {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.Format = DXGI_FORMAT_R32_UINT; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = 8;
        ok = SUCCEEDED(dev->CreateUnorderedAccessView(g_scanOut, &ud, &g_scanUav));
    }
    for (int i = 0; i < 4 && ok; ++i) ok = SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &g_scanStage[i]));
    if (!ok) { g_scanFailed = true; logf("DETCALLOUT: depth pass setup FAILED - the scan callout stays on the HUD sheet"); return false; }
    logf("DETCALLOUT: depth pass ready");
    return true;
}
float scan_sample_dist(float minD, int i) { return minD * powf(kScanSpan, (float)i / (float)(kScanN - 1)); }
void scan_pass(ID3D11DeviceContext* c, const M44& vp);   // DETCALLOUT: defined at the end (calls the original, unhooked context functions)
// the HUD frame's markers: centres (HUD-sheet NDC) for the piece routing + each eye's mapping. False = not available.
// HUDREC 2026-10-14 (JJ: the compass box and the distance marker still flicker to another place, DLSS on or off; "if we can't fix it
// this time, then we're just going to leave it"): every HUD frame's marker numbers in a ring of 400 (~8 s), written to
// akn\hudrec_<tag>.txt by the panel capture - JJ presses capture right after a flicker and the frames before it are on file.
struct HudRec { ULONGLONG t; LONG seq; int n; float c[2], c1[2], w[3], e0[2], e1[2], xf[4]; LONG routed, eyePieces, flat, missReach, missNone, held, drop, tagLost; int heldNow, det; };
HudRec g_hrec[400]; volatile LONG g_hrecN = 0;
bool mk_eye_frame()
{
    using MapFn = int (*)(float*); static MapFn p_map = nullptr; static bool s_tried = false;
    if (!s_tried) { HMODULE m = GetModuleHandleW(L"dinput8.dll"); p_map = m ? (MapFn)GetProcAddress(m, "akvr_hud_layout_map") : nullptr; s_tried = m != nullptr; }
    float xf[4] = {};
    if (!p_map || !p_map(xf)) { InterlockedIncrement(&c_mkEyeNoMap); return false; }
    g_mkHeldNow = 0;   // HELDDROP
    {   // MAPCONT 2026-10-14 (RB13 00:56:18: at a detective exit the marker point jumped (0.002 -0.082) -> (-0.166 0.053) for ONE HUD
        // frame while its world point and the game's layout point stayed put = the HUD layout map (AKVR's shrink / move of the HUD
        // movie) changed for that frame). A new map must hold 3 HUD frames before markers use it; until then the last one is used.
        static float s_acc[4] = {}; static bool s_have = false; static float s_cand[4] = {}; static int s_run = 0;
        auto same = [](const float* a, const float* b) { for (int k = 0; k < 4; ++k) if (fabsf(a[k] - b[k]) > 0.004f) return false; return true; };
        if (!s_have || same(xf, s_acc) || !g_mkCont) { memcpy(s_acc, xf, sizeof(s_acc)); s_have = true; s_run = 0; }
        else
        {
            s_run = same(xf, s_cand) ? s_run + 1 : 1; memcpy(s_cand, xf, sizeof(s_cand));
            if (g_recOn && g_mkContLines < 60) { InterlockedIncrement(&g_mkContLines); logf("MAPCONT: HUD layout map %.4f %.4f %.4f %.4f -> %.4f %.4f %.4f %.4f (frame %d of 3)", s_acc[0], s_acc[1], s_acc[2], s_acc[3], xf[0], xf[1], xf[2], xf[3], s_run); }
            if (s_run >= 3) { memcpy(s_acc, xf, sizeof(s_acc)); s_run = 0; InterlockedIncrement(&c_mkContTake); }
            else { memcpy(xf, s_acc, sizeof(s_acc)); InterlockedIncrement(&c_mkContHeld); g_mkHeldNow = 1; }
        }
    }
    mk_lag_measure();   // MKLAG
    g_mkScanIdx = -1;   // DETCALLOUT: set again below when the callout's marker is placed
    g_scanWok = 0;      // SCANLINE: no box point this frame yet
    McPt pts[16], oth[16]; int n = 0, no = 0; M44 bvp, ev[2]; bool ok = false;
    float ax[9] = {}; bool axOk = false;   // MKFACE: the game camera's right picks "right" only
    AcquireSRWLockShared(&g_mcLock);
    axOk = g_mcPubAxOk != 0; if (axOk) memcpy(ax, g_mcPubAx, sizeof(ax));
    ok = g_mcPubOk && g_mcPubBvpOk;
    if (ok) { bvp = g_mcPubBVP; ev[0] = g_mcPubVP[0]; ev[1] = g_mcPubVP[1]; }
    const int sb = g_mcStampBuf[g_mcCur] == g_pairStamp ? g_mcCur : g_mcStampBuf[g_mcCur ^ 1] == g_pairStamp ? (g_mcCur ^ 1) : g_mcCur;
    const int stampHit = g_mcStampBuf[g_mcCur] == g_pairStamp ? 1 : g_mcStampBuf[g_mcCur ^ 1] == g_pairStamp ? 2 : 0;   // MKJUMP: 1 newest, 2 older, 0 none matched
    n = g_mcN[sb]; for (int i = 0; i < n; ++i) pts[i] = g_mcPts[sb][i];
    no = g_mcN[sb ^ 1]; for (int i = 0; i < no; ++i) oth[i] = g_mcPts[sb ^ 1][i];   // MKLAG: the other kept frame
    ReleaseSRWLockShared(&g_mcLock);
    {   // MKCARRY2 2026-10-13 (JJ: "a duplicate copy of the distance marker and its surrounding box flickering off to the side every
        // three or four seconds"; NOTES MKGLOW: ~9 % of seconds with no matched marker point = the whole marker flat on the HUD sheet
        // that frame): a HUD frame with no projected marker points keeps the game's last ones for 250 ms - the pieces (still marked
        // as marker pieces) are placed by the last world point instead of falling back to the flat HUD
        static McPt s_last[16]; static int s_lastN = 0; static ULONGLONG s_lastAt = 0; const ULONGLONG nowT = GetTickCount64();
        if (n > 0) { for (int i = 0; i < n && i < 16; ++i) s_last[i] = pts[i]; s_lastN = n < 16 ? n : 16; s_lastAt = nowT; }
        else if (g_mkCarry2 && s_lastN > 0 && nowT - s_lastAt < 250) { n = s_lastN; for (int i = 0; i < n; ++i) pts[i] = s_last[i]; InterlockedIncrement(&c_mkCarry2); }
    }
    {   // MKCONT 2026-10-14 (JJ: "the distance marker still flickers over to a copy"; FLATREC 00:5x: at a detective exit the frame's
        // only projected point was (-0.166 0.053) instead of the marker's (0.002 -0.082) for ONE frame - another object's Canvas.Project
        // answer; every marker piece snapped to it = the marker at another place and size). Each marker keeps its world point: a point
        // whose world position matches none of the last accepted ones (300 units) must persist 3 HUD frames before it replaces them;
        // until then the last accepted points are used (as the game's own HUD keeps a marker where it was). Command "mkcont 0|1".
        static McPt s_acc[16]; static int s_accN = 0; static ULONGLONG s_accAt = 0; static McPt s_cand[16]; static int s_candN = 0, s_candRun = 0;
        const ULONGLONG nowT = GetTickCount64();
        auto sameW = [](const McPt& a, const McPt& b) { const float dx = a.w[0] - b.w[0], dy = a.w[1] - b.w[1], dz = a.w[2] - b.w[2]; return dx * dx + dy * dy + dz * dz < 300.0f * 300.0f; };
        if (g_mkCont && n > 0)
        {
            bool allKnown = s_accN > 0;
            for (int i = 0; i < n && allKnown; ++i) { bool k = false; for (int j = 0; j < s_accN && !k; ++j) k = sameW(pts[i], s_acc[j]); allKnown = k; }
            if (s_accN == 0 || allKnown || nowT - s_accAt > 250) { s_accN = n < 16 ? n : 16; for (int i = 0; i < s_accN; ++i) s_acc[i] = pts[i]; s_accAt = nowT; s_candN = 0; s_candRun = 0; }
            else
            {   // a new world point: count how long the SAME new set persists
                bool same = s_candN == n; for (int i = 0; i < n && same; ++i) same = sameW(pts[i], s_cand[i]);
                s_candRun = same ? s_candRun + 1 : 1; s_candN = n < 16 ? n : 16; for (int i = 0; i < s_candN; ++i) s_cand[i] = pts[i];
                if (s_candRun >= 3) { s_accN = s_candN; for (int i = 0; i < s_accN; ++i) s_acc[i] = s_cand[i]; s_accAt = nowT; s_candN = 0; s_candRun = 0; InterlockedIncrement(&c_mkContTake); }
                else
                {
                    InterlockedIncrement(&c_mkContHeld); g_mkHeldNow = 1;
                    if (g_recOn && g_mkContLines < 60) { InterlockedIncrement(&g_mkContLines); logf("MKCONT: new point %.3f %.3f (world %.0f %.0f %.0f) held back (frame %d of 3) - kept %.3f %.3f (world %.0f %.0f %.0f)", pts[0].r[0], pts[0].r[1], pts[0].w[0], pts[0].w[1], pts[0].w[2], s_candRun, s_acc[0].r[0], s_acc[0].r[1], s_acc[0].w[0], s_acc[0].w[1], s_acc[0].w[2]); }
                    n = s_accN; for (int i = 0; i < n; ++i) pts[i] = s_acc[i];
                }
            }
        }
    }
    if (!ok) { InterlockedIncrement(&c_mkEyeNoView); return false; }
    M44 inv; if (!inv44(bvp, inv)) { InterlockedIncrement(&c_mkEyeNoView); return false; }
    const float sx = 1.0f + xf[0], sy = 1.0f + xf[1];
    if (fabsf(sx) < 1e-4f || fabsf(sy) < 1e-4f) { InterlockedIncrement(&c_mkEyeNoMap); return false; }
    int scanPt = -1;   // DETCALLOUT: the scan box this frame (AKVR: its point on the HUD sheet) -> ray, depth pass, marker
    g_scanRay.ok = false;
    if (g_scanOn && g_hudTex)
    {
        using ScFn = int (*)(int, int, float*); static ScFn p_sc = nullptr; static bool s_scTried = false;
        if (!s_scTried) { HMODULE hm = GetModuleHandleW(L"dinput8.dll"); p_sc = hm ? (ScFn)GetProcAddress(hm, "akvr_scan_callout") : nullptr; s_scTried = hm != nullptr; }
        D3D11_TEXTURE2D_DESC hd{}; g_hudTex->GetDesc(&hd);
        float c2[2];
        if (p_sc && p_sc((int)hd.Width, (int)hd.Height, c2))
        {
            InterlockedIncrement(&c_scanSeen);
            const float R[2] = { c2[0] * sx + xf[2], c2[1] * sy + xf[3] };   // sheet -> the game's own layout (head-free view NDC)
            // SCANPROJ: the object the game projected to the box's screen point (this pair's frame, else the other kept one)
            bool exact = false;
            if (g_scanExact && n < 16)
            {
                float bd = 0.06f * 0.06f; int bi = -1, bs = -1;
                float nd = 1e9f; int nn = 0; float nr[2] = {}; float nw[3] = {};   // SCANPROJ diag: the nearest point whatever its distance
                AcquireSRWLockShared(&g_mcLock);
                for (int f = 0; f < 2; ++f)
                {
                    const int s = (g_spStampBuf[g_spCur] == g_pairStamp) == (f == 0) ? g_spCur : (g_spCur ^ 1);
                    for (int i = 0; i < g_spN[s]; ++i) { const float dx = g_spPts[s][i].r[0] - R[0], dy = g_spPts[s][i].r[1] - R[1], d2 = dx * dx + dy * dy; ++nn; if (d2 < nd) { nd = d2; nr[0] = g_spPts[s][i].r[0]; nr[1] = g_spPts[s][i].r[1]; for (int k = 0; k < 3; ++k) nw[k] = g_spPts[s][i].w[k]; } if (d2 < bd) { bd = d2; bi = i; bs = s; } }
                    if (bi >= 0) break;
                }
                _snprintf_s(g_scanNearDiag, sizeof(g_scanNearDiag), _TRUNCATE, "projected points %d, nearest %.3f at %.3f %.3f (world %.0f %.0f %.0f) vs box layout %.3f %.3f", nn, nn ? sqrtf(nd) : -1.0f, nr[0], nr[1], nw[0], nw[1], nw[2], R[0], R[1]);
                if (bi >= 0) { for (int k = 0; k < 3; ++k) pts[n].w[k] = g_spPts[bs][bi].w[k]; }
                ReleaseSRWLockShared(&g_mcLock);
                if (bi >= 0)
                {   // SCANHUD 2026-10-13 (JJ: "the lines and the box should all be at HUD depth for detective mode"): no world marker any
                    // more - the box stays a HUD piece (marker sheet) and AKVR moves it on the HUD plane to where the object is seen
                    for (int k = 0; k < 3; ++k) g_scanWpub[k] = pts[n].w[k];
                    g_scanSheet[0] = c2[0]; g_scanSheet[1] = c2[1]; g_scanWok = 1;
                    exact = true; InterlockedIncrement(&c_scanExact); InterlockedIncrement(&c_scanPlaced); g_scanMatchD = sqrtf(bd);
                }
            }
            float C[3], P[3]; bool rok = !exact && mk_cam_centre(bvp, C);   // the depth march only without a projected point
            if (rok)
            {
                float h[4]; for (int k = 0; k < 4; ++k) h[k] = R[0] * inv.m[0][k] + R[1] * inv.m[1][k] + 0.5f * inv.m[2][k] + inv.m[3][k];
                rok = fabsf(h[3]) > 1e-12f;
                if (rok) for (int k = 0; k < 3; ++k) P[k] = h[k] / h[3];
            }
            float d[3] = {}, dl = 0.0f;
            if (rok) { for (int k = 0; k < 3; ++k) d[k] = P[k] - C[k]; dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]); rok = dl > 1e-3f; }
            if (rok)
            {
                for (int k = 0; k < 3; ++k) d[k] /= dl;
                AcquireSRWLockShared(&g_mcLock); float f3[3] = { g_mcPubAx[0], g_mcPubAx[1], g_mcPubAx[2] }; const bool fok = g_mcPubAxOk != 0; ReleaseSRWLockShared(&g_mcLock);
                if (fok && d[0] * f3[0] + d[1] * f3[1] + d[2] * f3[2] < 0.0f) for (int k = 0; k < 3; ++k) d[k] = -d[k];   // in front of the camera
                for (int k = 0; k < 3; ++k) { g_scanRay.c[k] = C[k]; g_scanRay.d[k] = d[k]; }
                g_scanRay.vp = ev[0]; g_scanRay.ok = true;
                if (g_scanDist > 0.0f)
                {   // SCANHUD: the depth march's point, exported the same way (no world marker)
                    for (int k = 0; k < 3; ++k) g_scanWpub[k] = C[k] + d[k] * g_scanDist;
                    g_scanSheet[0] = c2[0]; g_scanSheet[1] = c2[1]; g_scanWok = 1; InterlockedIncrement(&c_scanPlaced);
                }
            }
        }
    }
    int m = 0;
    for (int i = 0; i < n && m < 8; ++i)
    {
        const float* W = pts[i].w; float R[2] = { pts[i].r[0], pts[i].r[1] };
        float R1[2] = { R[0], R[1] };   // MKLAG: the same marker in the other kept frame (nearest world point)
        { float bd = 300.0f * 300.0f; for (int j = 0; j < no; ++j) { const float dx = oth[j].w[0] - W[0], dy = oth[j].w[1] - W[1], dz = oth[j].w[2] - W[2], d2 = dx * dx + dy * dy + dz * dz; if (d2 < bd) { bd = d2; R1[0] = oth[j].r[0]; R1[1] = oth[j].r[1]; } } }
        const int lagUse = g_mkLagMode == 2 ? g_mkLagUse : g_mkLagMode;
        const float R0[2] = { R[0], R[1] };
        if (m == 0) { g_mkLagC[0][0] = (R0[0] - xf[2]) / sx; g_mkLagC[0][1] = (R0[1] - xf[3]) / sy; g_mkLagC[1][0] = (R1[0] - xf[2]) / sx; g_mkLagC[1][1] = (R1[1] - xf[3]) / sy; g_mkLagCOk = true; }
        if (fabsf(R[0]) > 1.2f || fabsf(R[1]) > 1.2f) continue;   // off screen: edge arrows stay on the HUD sheet
        float cb[4]; for (int k = 0; k < 4; ++k) cb[k] = W[0] * bvp.m[0][k] + W[1] * bvp.m[1][k] + W[2] * bvp.m[2][k] + bvp.m[3][k];
        if (cb[3] <= 1e-3f) continue;
        // MKFACE 2026-10-11 (MKREC 045423, JJ's recorded run: while he tilted his head the marker's up followed the
        // "head-free" game camera's up, which swung up to 28 deg from the world's up - the game camera inherits the head
        // roll the mod writes into it; and the card map built from that camera added ~1 deg of rotation noise per frame on
        // head turns). The geo-11 version's rule (TARGETFACE + TARGETUP): the marker is a card at W facing the viewer's line
        // of sight, its up = the WORLD's up, its size = the game's layout scale at W's depth. The game camera's orientation
        // is no longer used (its right axis only picks which way is "right").
        float ce[2][3];
        if (!mk_cam_centre(ev[0], ce[0]) || !mk_cam_centre(ev[1], ce[1])) continue;
        float cn[3] = { 0.5f * (ce[0][0] + ce[1][0]) - W[0], 0.5f * (ce[0][1] + ce[1][1]) - W[1], 0.5f * (ce[0][2] + ce[1][2]) - W[2] };
        { const float l = sqrtf(cn[0] * cn[0] + cn[1] * cn[1] + cn[2] * cn[2]); if (l < 1.0f) continue; for (int k = 0; k < 3; ++k) cn[k] /= l; }
        // MKFACE2 2026-10-11 (JJ after MKFACE: "Much better. The only problem is they're still rotating with head rotations. They
        // should only rotate with camera rotations"; MKREC 050406: marker up = world up within 0.05 deg in every phase, and the
        // layout point R stays put while the head turns = the game camera's AIM is head-free, only its roll leaks): the card
        // faces the GAME CAMERA's direction (turns only when the camera turns), not the viewer's eyes (which move with the head);
        // up stays the world's.
        if (axOk && g_mkFaceCam)
        {   // MKUPRIGHT 2026-10-11 (JJ after MKFACE2: "It's subtle but they are rotating"; MKREC 131221: marker up 0.2-0.9 deg off
            // the world's up): facing the camera's FULL direction leaned the card back (AK's camera looks down at Batman), and a
            // leaning card seen from the side does not keep the world's verticals. The card stands upright: it faces the camera's
            // heading only (turns with camera turns), its up is exactly the world's up.
            const float hl = sqrtf(ax[0] * ax[0] + ax[1] * ax[1]);
            if (hl > 0.2f) { cn[0] = -ax[0] / hl; cn[1] = -ax[1] / hl; cn[2] = 0.0f; }
            else { cn[0] = -ax[0]; cn[1] = -ax[1]; cn[2] = -ax[2]; const float l = sqrtf(cn[0] * cn[0] + cn[1] * cn[1] + cn[2] * cn[2]); if (l < 1e-4f) continue; for (int k = 0; k < 3; ++k) cn[k] /= l; }
        }
        float cup[3] = { 0.0f, 0.0f, 1.0f };   // UE3 world up (z)
        { const float d = cup[0] * cn[0] + cup[1] * cn[1] + cup[2] * cn[2]; for (int k = 0; k < 3; ++k) cup[k] -= d * cn[k];
          float l = sqrtf(cup[0] * cup[0] + cup[1] * cup[1] + cup[2] * cup[2]);
          if (l < 0.2f && axOk) { for (int k = 0; k < 3; ++k) cup[k] = ax[6 + k]; const float d2 = cup[0] * cn[0] + cup[1] * cn[1] + cup[2] * cn[2]; for (int k = 0; k < 3; ++k) cup[k] -= d2 * cn[k]; l = sqrtf(cup[0] * cup[0] + cup[1] * cup[1] + cup[2] * cup[2]); }
          if (l < 1e-4f) continue; for (int k = 0; k < 3; ++k) cup[k] /= l; }
        const float sgr = (float)g_mkSignR;   // the game view's own sign for the camera's right (MK1 base check)
        float crt[3] = { axOk ? sgr * ax[3] : cup[1] * cn[2] - cup[2] * cn[1], axOk ? sgr * ax[4] : cup[2] * cn[0] - cup[0] * cn[2], axOk ? sgr * ax[5] : cup[0] * cn[1] - cup[1] * cn[0] };
        { const float d = crt[0] * cn[0] + crt[1] * cn[1] + crt[2] * cn[2], d2 = crt[0] * cup[0] + crt[1] * cup[1] + crt[2] * cup[2];
          for (int k = 0; k < 3; ++k) crt[k] -= d * cn[k] + d2 * cup[k];
          const float l = sqrtf(crt[0] * crt[0] + crt[1] * crt[1] + crt[2] * crt[2]); if (l < 1e-4f) continue; for (int k = 0; k < 3; ++k) crt[k] /= l; }
        if (m == 0) { g_mkCardN[0] = cn[0]; g_mkCardN[1] = cn[1]; g_mkCardN[2] = cn[2]; }   // MKREC: the card's facing
        {   // MKCARD: this marker's card (filled before the per-eye maps; kept if the marker passes them)
            MkCardD& d = g_mkCardD[m];
            for (int k = 0; k < 3; ++k) { d.w[k] = W[k]; }
            d.c[0] = (R[0] - xf[2]) / sx; d.c[1] = (R[1] - xf[3]) / sy;
        }
        float fsx = g_mkGameP.m[0][0] != 0.0f ? cb[3] / g_mkGameP.m[0][0] : 0.0f, fsy = g_mkGameP.m[1][1] != 0.0f ? cb[3] / g_mkGameP.m[1][1] : 0.0f;   // world units per layout NDC unit at W
        if (fsx == 0.0f || fsy == 0.0f) continue;
        // MKFACEPOS 2026-10-11 (JJ: the markers "sit solidly on their intended world objects but they are angled straight out
        // towards the rest of the HUD. They should be angled towards the camera (not head tracking though). We fixed this in
        // Arkham City"): City's MARKERFACE (batman_city markers.cpp upright_card). The upright card faced the camera's HEADING,
        // so a marker above, below or to the side was seen at a slant, like the flat HUD. Now the card tilts to face the GAME
        // camera's POSITION (head-free: head moves change nothing, only camera moves turn it); its right edge stays level (no
        // roll), up = normal x right. Size: fitted so the game camera sees exactly the game's layout at W (first order). The
        // fit uses a roll-free frame (forward = the camera's aim, right = level): AKN's head-free camera still carries the head
        // ROLL (MKREC 045423), and fitting from its matrix would make the size follow head tilts.
        if (g_mkFacePos && axOk)
        {
            float C[3];
            const float fl = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]), fh = sqrtf(ax[0] * ax[0] + ax[1] * ax[1]);
            if (mk_cam_centre(bvp, C) && fl > 1e-4f && fh > 0.2f * fl)
            {
                float n[3] = { C[0] - W[0], C[1] - W[1], C[2] - W[2] };
                const float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]), nh = sqrtf(n[0] * n[0] + n[1] * n[1]);
                if (nl > 1.0f && nh > 0.2f * nl)   // not with the camera almost straight above / below the marker
                {
                    for (int k = 0; k < 3; ++k) n[k] /= nl;
                    const float gr[3] = { sgr * ax[3], sgr * ax[4], sgr * ax[5] };   // the game view's right (sign only)
                    float rt[3] = { -n[1] * nl / nh, n[0] * nl / nh, 0.0f };
                    if (rt[0] * gr[0] + rt[1] * gr[1] < 0.0f) { rt[0] = -rt[0]; rt[1] = -rt[1]; }
                    float u[3] = { n[1] * rt[2] - n[2] * rt[1], n[2] * rt[0] - n[0] * rt[2], n[0] * rt[1] - n[1] * rt[0] };
                    if (u[2] < 0.0f) { u[0] = -u[0]; u[1] = -u[1]; u[2] = -u[2]; }
                    // roll-free game camera: forward f, level right r0, up u0
                    const float f[3] = { ax[0] / fl, ax[1] / fl, ax[2] / fl };
                    float r0[3] = { f[1] / (fh / fl), -f[0] / (fh / fl), 0.0f };
                    if (r0[0] * gr[0] + r0[1] * gr[1] < 0.0f) { r0[0] = -r0[0]; r0[1] = -r0[1]; }
                    float u0[3] = { r0[1] * f[2] - r0[2] * f[1], r0[2] * f[0] - r0[0] * f[2], r0[0] * f[1] - r0[1] * f[0] };
                    if (u0[2] < 0.0f) { u0[0] = -u0[0]; u0[1] = -u0[1]; u0[2] = -u0[2]; }
                    const float d[3] = { W[0] - C[0], W[1] - C[1], W[2] - C[2] };
                    const float X = d[0] * r0[0] + d[1] * r0[1] + d[2] * r0[2], Y = d[0] * u0[0] + d[1] * u0[1] + d[2] * u0[2], Z = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
                    if (Z > 1.0f)
                    {   // layout NDC change per world unit along the card's right / up at W (x = P00 X / Z, y = P11 Y / Z)
                        const float p00 = fabsf(g_mkGameP.m[0][0]), p11 = fabsf(g_mkGameP.m[1][1]);
                        const float rf = rt[0] * f[0] + rt[1] * f[1] + rt[2] * f[2], uf = u[0] * f[0] + u[1] * f[1] + u[2] * f[2];
                        const float dx = p00 * ((rt[0] * r0[0] + rt[1] * r0[1] + rt[2] * r0[2]) * Z - X * rf) / (Z * Z);
                        const float dy = p11 * ((u[0] * u0[0] + u[1] * u0[1] + u[2] * u0[2]) * Z - Y * uf) / (Z * Z);
                        if (dx > 1e-9f && dy > 1e-9f)
                        {
                            for (int k = 0; k < 3; ++k) { cn[k] = n[k]; crt[k] = rt[k]; cup[k] = u[k]; }
                            fsx = 1.0f / dx; fsy = 1.0f / dy;
                            if (m == 0) { g_mkCardN[0] = cn[0]; g_mkCardN[1] = cn[1]; g_mkCardN[2] = cn[2]; }
                            InterlockedIncrement(&c_mkFaced);
                        }
                    }
                }
            }
        }
        for (int k = 0; k < 3; ++k) { g_mkCardD[m].ax[k] = sx * fsx * crt[k]; g_mkCardD[m].ay[k] = sy * fsy * cup[k]; }   // MKCARD: per unit of sheet NDC
        {   // MKGS: a sheet point (x, y) -> layout (sx x + bx, sy y + by) -> on the card: W + (layout - R) scaled along right / up
            // -> each eye's clip position: linear in (x, y, 1)
            float P0[3]; for (int k = 0; k < 3; ++k) P0[k] = W[k] + (xf[2] - R[0]) * fsx * crt[k] + (xf[3] - R[1]) * fsy * cup[k];
            for (int e = 0; e < 2; ++e)
                for (int j = 0; j < 4; ++j)
                {
                    const M44& M = ev[e];
                    g_mkK[m][e][j] = P0[0] * M.m[0][j] + P0[1] * M.m[1][j] + P0[2] * M.m[2][j] + M.m[3][j];
                    g_mkK[m][e][4 + j] = sx * fsx * (crt[0] * M.m[0][j] + crt[1] * M.m[1][j] + crt[2] * M.m[2][j]);
                    g_mkK[m][e][8 + j] = sy * fsy * (cup[0] * M.m[0][j] + cup[1] * M.m[1][j] + cup[2] * M.m[2][j]);
                }
        }
        bool good = true;
        for (int e = 0; e < 2 && good; ++e)
        {
            // MKFACE: this eye's picture of a card at W facing the viewer, up = world up, sized like the game's layout at W
            auto F = [&](float x, float y, float out[2]) {
                const float P[3] = { W[0] + x * fsx * crt[0] + y * fsy * cup[0], W[1] + x * fsx * crt[1] + y * fsy * cup[1], W[2] + x * fsx * crt[2] + y * fsy * cup[2] };
                float c[4]; for (int k = 0; k < 4; ++k) c[k] = P[0] * ev[e].m[0][k] + P[1] * ev[e].m[1][k] + P[2] * ev[e].m[2][k] + ev[e].m[3][k];
                if (c[3] <= 1e-3f) return false;
                out[0] = c[0] / c[3]; out[1] = c[1] / c[3]; return true; };
            const float hh = 0.01f; float f0[2], fx[2], fy[2];
            if (!F(0.0f, 0.0f, f0) || !F(hh, 0.0f, fx) || !F(0.0f, hh, fy)) { good = false; break; }
            const float J00 = (fx[0] - f0[0]) / hh, J10 = (fx[1] - f0[1]) / hh, J01 = (fy[0] - f0[0]) / hh, J11 = (fy[1] - f0[1]) / hh;
            MkEyeT& T = g_mkT[m][e];
            T.a[0] = J00 * sx; T.a[1] = J01 * sy; T.a[2] = J10 * sx; T.a[3] = J11 * sy;
            g_mkF0[m][e][0] = f0[0]; g_mkF0[m][e][1] = f0[1];   // MKANCHOR
            const float bx = xf[2] - R[0], by = xf[3] - R[1];
            T.t[0] = f0[0] + J00 * bx + J01 * by; T.t[1] = f0[1] + J10 * bx + J11 * by;
        }
        if (!good) continue;
        if (m == 0)
        {   // MKEYE2: F(R) must equal this eye's own projection of W (else the layout point and the head-free view disagree)
            float pe[2][2] = {};
            for (int e = 0; e < 2; ++e) { float c4[4]; for (int k = 0; k < 4; ++k) c4[k] = W[0] * ev[e].m[0][k] + W[1] * ev[e].m[1][k] + W[2] * ev[e].m[2][k] + ev[e].m[3][k];
                                          if (c4[3] > 1e-3f) { pe[e][0] = c4[0] / c4[3]; pe[e][1] = c4[1] / c4[3]; } }
            float rb[2] = {}; if (cb[3] > 1e-3f) { rb[0] = cb[0] / cb[3]; rb[1] = cb[1] / cb[3]; }
            _snprintf_s(g_mkEyeDiag, sizeof(g_mkEyeDiag), _TRUNCATE, "first marker: layout point %.3f %.3f, head-free view puts W at %.3f %.3f | left eye: W at %.3f %.3f, map A %.3f %.3f %.3f %.3f t %.3f %.3f | right eye: W at %.3f %.3f, map t %.3f %.3f",
                        R[0], R[1], rb[0], rb[1], pe[0][0], pe[0][1], g_mkT[0][0].a[0], g_mkT[0][0].a[1], g_mkT[0][0].a[2], g_mkT[0][0].a[3], g_mkT[0][0].t[0], g_mkT[0][0].t[1], pe[1][0], pe[1][1], g_mkT[0][1].t[0], g_mkT[0][1].t[1]);
        }
        g_mkC[m][0] = (R[0] - xf[2]) / sx; g_mkC[m][1] = (R[1] - xf[3]) / sy;
        g_mkAncSlot[m] = mk_anc_slot(W); g_mkAncDone[m] = false;   // MKANCHOR
        if (m == 0 && g_mkRecFrames > 0)
        {   // MKREC 2026-10-11 (JJ will run the test himself): per HUD frame, per eye - W's eye point, the map, and the picture
            // angles of the world's up, the head-free camera's up / right through W, and the marker's own up / right (A
            // applied to the layout's axes); plus the previous frame's first piece in each eye relative to W's point then.
            static ULONGLONG s_t0 = 0; static float s_prevT[2][6] = {}, s_prevF[2][2] = {}; static bool s_prevOk = false;
            if (!g_mkRecF)
            {
                wchar_t path[MAX_PATH]; SYSTEMTIME st; GetLocalTime(&st);
                _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%smkrec_%02d%02d%02d.csv", g_dir, st.wHour, st.wMinute, st.wSecond);
                _wfopen_s(&g_mkRecF, path, L"w");
                s_t0 = GetTickCount64(); s_prevOk = false;
                if (g_mkRecF) fprintf(g_mkRecF, "ms,stamp,Rx,Ry,afw,cardHeading,cardTilt,"
                    "L_Wx,L_Wy,L_A00,L_A01,L_A10,L_A11,L_tx,L_ty,L_worldUp,L_camUp,L_camRight,L_mkUp,L_mkRight,L_pieceDx,L_pieceDy,"
                    "R_Wx,R_Wy,R_A00,R_A01,R_A10,R_A11,R_tx,R_ty,R_worldUp,R_camUp,R_camRight,R_mkUp,R_mkRight,R_pieceDx,R_pieceDy\n");
                if (g_mkRecF) logf("MKREC: recording %ld HUD frames", (long)g_mkRecFrames);
                else { logf("MKREC: cannot open the record file - stopped"); g_mkRecFrames = 0; }
            }
            if (g_mkRecF)
            {
                float ax[9] = {}; bool axOk = false;
                AcquireSRWLockShared(&g_mcLock); axOk = g_mcPubAxOk != 0; if (axOk) memcpy(ax, g_mcPubAx, sizeof(ax)); ReleaseSRWLockShared(&g_mcLock);
                fprintf(g_mkRecF, "%llu,%llu,%.5f,%.5f,%d,%.3f,%.3f", GetTickCount64() - s_t0, (unsigned long long)g_pairStamp, R[0], R[1], g_afwFrame ? 1 : 0,
                        atan2f(g_mkCardN[1], g_mkCardN[0]) * 57.29578f, asinf(g_mkCardN[2] < -1.0f ? -1.0f : g_mkCardN[2] > 1.0f ? 1.0f : g_mkCardN[2]) * 57.29578f);
                for (int e = 0; e < 2; ++e)
                {
                    auto proj = [&](const float P[3], float o[2]) { float c4[4]; for (int k = 0; k < 4; ++k) c4[k] = P[0] * ev[e].m[0][k] + P[1] * ev[e].m[1][k] + P[2] * ev[e].m[2][k] + ev[e].m[3][k];
                                                                  if (c4[3] <= 1e-3f) return false; o[0] = c4[0] / c4[3]; o[1] = c4[1] / c4[3]; return true; };
                    float pw[2] = {}; proj(W, pw);
                    auto ang = [&](const float d[3]) { float q[2] = {}; const float P[3] = { W[0] + d[0] * 50.0f, W[1] + d[1] * 50.0f, W[2] + d[2] * 50.0f }; if (!proj(P, q)) return 999.0f; return atan2f(q[1] - pw[1], q[0] - pw[0]) * 57.29578f; };
                    const float up[3] = { 0.0f, 0.0f, 1.0f };
                    const float wu = ang(up), cu = axOk ? ang(ax + 6) : 999.0f, cr = axOk ? ang(ax + 3) : 999.0f;
                    const MkEyeT& T = g_mkT[0][e];
                    const float mu = atan2f(T.a[3], T.a[1]) * 57.29578f, mr = atan2f(T.a[2], T.a[0]) * 57.29578f;
                    float pdx = 0.0f, pdy = 0.0f;
                    if (s_prevOk && g_mkLagPOk)
                    {   // the first piece drawn last frame, through last frame's map, relative to W's point last frame
                        pdx = s_prevT[e][0] * g_mkLagP[0] + s_prevT[e][1] * g_mkLagP[1] + s_prevT[e][4] - s_prevF[e][0];
                        pdy = s_prevT[e][2] * g_mkLagP[0] + s_prevT[e][3] * g_mkLagP[1] + s_prevT[e][5] - s_prevF[e][1];
                    }
                    fprintf(g_mkRecF, ",%.5f,%.5f,%.4f,%.4f,%.4f,%.4f,%.5f,%.5f,%.2f,%.2f,%.2f,%.2f,%.2f,%.5f,%.5f", pw[0], pw[1], T.a[0], T.a[1], T.a[2], T.a[3], T.t[0], T.t[1], wu, cu, cr, mu, mr, pdx, pdy);
                    for (int k = 0; k < 4; ++k) s_prevT[e][k] = T.a[k]; s_prevT[e][4] = T.t[0]; s_prevT[e][5] = T.t[1];
                    s_prevF[e][0] = pw[0]; s_prevF[e][1] = pw[1];
                }
                fprintf(g_mkRecF, "\n");
                s_prevOk = true;
                if (InterlockedDecrement(&g_mkRecFrames) <= 0) { fclose(g_mkRecF); g_mkRecF = nullptr; logf("MKREC: done"); }
            }
        }
        if (i == scanPt)
        {   // DETCALLOUT
            g_mkScanIdx = m; InterlockedIncrement(&c_scanPlaced);
            for (int k = 0; k < 3; ++k) g_scanWpub[k] = W[k]; g_scanWok = 1;   // SCANLINE: AKVR draws the line to here
            float pe[2][3] = {};
            for (int e = 0; e < 2; ++e) { float c4[4]; for (int k = 0; k < 4; ++k) c4[k] = W[0] * ev[e].m[0][k] + W[1] * ev[e].m[1][k] + W[2] * ev[e].m[2][k] + ev[e].m[3][k];
                                          pe[e][2] = c4[3]; if (fabsf(c4[3]) > 1e-3f) { pe[e][0] = c4[0] / c4[3]; pe[e][1] = c4[1] / c4[3]; } }
            float rb[2] = {}; if (cb[3] > 1e-3f) { rb[0] = cb[0] / cb[3]; rb[1] = cb[1] / cb[3]; }
            _snprintf_s(g_scanDiag, sizeof(g_scanDiag), _TRUNCATE, "marker %d: sheet %.3f %.3f, layout %.3f %.3f, head-free view puts W at %.3f %.3f (w %.0f) | W %.0f %.0f %.0f | left eye %.3f %.3f (w %.0f) right eye %.3f %.3f | card scale %.1f %.1f",
                        m, g_mkC[m][0], g_mkC[m][1], R[0], R[1], rb[0], rb[1], cb[3], W[0], W[1], W[2], pe[0][0], pe[0][1], pe[0][2], pe[1][0], pe[1][1], fsx, fsy);
        }
        if (m == 0)
        {   // MKPLACE 2026-10-13 (JJ: the distance marker flickers "to another location for one frame"; not reproduced with the fake headset's
            // still head): where the first marker's world point lands in each eye this HUD frame; a frame far off the line of the two before
            // (a jump that comes straight back) is counted and logged - separates our placement from compositing / timing in the headset
            static float s_p[2][2][2] = {}; static int s_pn = 0;
            float now[2][2] = {}; bool okp = true;
            for (int e = 0; e < 2 && okp; ++e) { float c4[4]; for (int k = 0; k < 4; ++k) c4[k] = W[0] * ev[e].m[0][k] + W[1] * ev[e].m[1][k] + W[2] * ev[e].m[2][k] + ev[e].m[3][k]; if (c4[3] <= 1e-3f) okp = false; else { now[e][0] = c4[0] / c4[3]; now[e][1] = c4[1] / c4[3]; } }
            if (okp)
            {
                if (s_pn >= 2)
                    for (int e = 0; e < 2; ++e)
                    {
                        // the LAST frame against the line between the one before it and this one: a one-frame outlier (out and straight back);
                        // a turn starting or stopping bends the path but does not leave one frame off both neighbours
                        const float mx = 0.5f * (s_p[0][e][0] + now[e][0]), my = 0.5f * (s_p[0][e][1] + now[e][1]), dx = s_p[1][e][0] - mx, dy = s_p[1][e][1] - my;
                        const float sx = now[e][0] - s_p[0][e][0], sy = now[e][1] - s_p[0][e][1];
                        if (dx * dx + dy * dy > 0.03f * 0.03f && dx * dx + dy * dy > 0.25f * (sx * sx + sy * sy)) { InterlockedIncrement(&c_mkPlaceJump); if (g_mkPlaceLines < 200) { InterlockedIncrement(&g_mkPlaceLines); logf("MKPLACE one-frame outlier eye %d: %.3f %.3f between %.3f %.3f and %.3f %.3f | W %.0f %.0f %.0f | HUD seq %ld", e, s_p[1][e][0], s_p[1][e][1], s_p[0][e][0], s_p[0][e][1], now[e][0], now[e][1], W[0], W[1], W[2], g_hudSeq - 1); } break; }
                    }
                for (int e = 0; e < 2; ++e) { s_p[0][e][0] = s_p[1][e][0]; s_p[0][e][1] = s_p[1][e][1]; s_p[1][e][0] = now[e][0]; s_p[1][e][1] = now[e][1]; }
                if (s_pn < 2) ++s_pn;
            }
            else s_pn = 0;
        }
        ++m;
    }
    {   // MKJUMP 2026-10-13 (JJ: "the occasional flickering of the distance marker to another location for one frame"; MKCARRY2 never
        // fired = the lists were never empty): every HUD frame whose list of projected marker points differs from the last one
        // (count, or a point moved > 200 units) is logged with both lists and which kept game frame matched the pair's stamp
        static McPt s_pv[16]; static int s_pn = 0; static LONG s_lines = 0;
        bool ch = n != s_pn;
        for (int i = 0; i < n && !ch; ++i) { float bd = 1e30f; for (int j = 0; j < s_pn; ++j) { const float dx = pts[i].w[0] - s_pv[j].w[0], dy = pts[i].w[1] - s_pv[j].w[1], dz = pts[i].w[2] - s_pv[j].w[2], d2 = dx * dx + dy * dy + dz * dz; if (d2 < bd) bd = d2; } if (bd > 200.0f * 200.0f) ch = true; }
        if (ch && s_lines < 400)
        {
            ++s_lines; char a[600] = ""; int o = 0;
            o += _snprintf_s(a + o, sizeof(a) - o, _TRUNCATE, "MKJUMP: points %d -> %d (pair stamp matched %s, markers placed %d) | now:", s_pn, n, stampHit == 1 ? "newest" : stampHit == 2 ? "older" : "NONE", m);
            for (int i = 0; i < n && i < 5; ++i) o += _snprintf_s(a + o, sizeof(a) - o, _TRUNCATE, " W %.0f %.0f %.0f R %.3f %.3f;", pts[i].w[0], pts[i].w[1], pts[i].w[2], pts[i].r[0], pts[i].r[1]);
            o += _snprintf_s(a + o, sizeof(a) - o, _TRUNCATE, " | before:");
            for (int i = 0; i < s_pn && i < 5; ++i) o += _snprintf_s(a + o, sizeof(a) - o, _TRUNCATE, " W %.0f %.0f %.0f R %.3f %.3f;", s_pv[i].w[0], s_pv[i].w[1], s_pv[i].w[2], s_pv[i].r[0], s_pv[i].r[1]);
            logf("%s", a);
        }
        s_pn = n < 16 ? n : 16; for (int i = 0; i < s_pn; ++i) s_pv[i] = pts[i];
    }
    g_mkCN = m; InterlockedIncrement(&c_mkEyeFrames); InterlockedAdd(&c_mkEyeMarkers, m);
    {   // HUDREC: this HUD frame
        HudRec& r = g_hrec[(InterlockedIncrement(&g_hrecN) - 1) % 400];
        r.t = GetTickCount64(); r.seq = g_hudSeq; r.n = m; r.c[0] = m ? g_mkC[0][0] : 0; r.c[1] = m ? g_mkC[0][1] : 0; r.c1[0] = m > 1 ? g_mkC[1][0] : 0; r.c1[1] = m > 1 ? g_mkC[1][1] : 0;
        for (int k = 0; k < 3; ++k) r.w[k] = n > 0 ? pts[0].w[k] : 0;
        r.e0[0] = g_mkF0[0][0][0]; r.e0[1] = g_mkF0[0][0][1]; r.e1[0] = g_mkF0[0][1][0]; r.e1[1] = g_mkF0[0][1][1];
        for (int k = 0; k < 4; ++k) r.xf[k] = xf[k];
        r.routed = g_mkRoutedLast; r.eyePieces = c_mkEyePieces; r.flat = c_mkFlat; r.missReach = c_mkMissReach; r.missNone = c_mkMissNone; r.held = c_mkContHeld; r.drop = c_mkHeldDrop; r.tagLost = c_mkTagLost;
        r.heldNow = g_mkHeldNow; r.det = akn_detective_active();
    }
    g_mkCardDN = m; g_mkCardEv[0] = ev[0]; g_mkCardEv[1] = ev[1];   // MKCARD
    g_mkLagPOk = false;   // MKLAG / MKREC: this frame's first piece comes next
    return true;
}
// a piece's placement rows through this eye's mapping (W = its w row, or none for a flat piece)
void mk_xrows(float* X, float* Y, const float* W, const MkEyeT& T)
{
    float x[4], y[4]; for (int i = 0; i < 4; ++i) { x[i] = X[i]; y[i] = Y[i]; }
    for (int i = 0; i < 4; ++i)
    {
        const float w = W ? W[i] : (i == 3 ? 1.0f : 0.0f);
        X[i] = T.a[0] * x[i] + T.a[1] * y[i] + T.t[0] * w;
        Y[i] = T.a[2] * x[i] + T.a[3] * y[i] + T.t[1] * w;
    }
}
using MkCentresFn = int (*)(float*, int); MkCentresFn p_mkCentres = nullptr; bool p_mkCentresTried = false;
void mk_frame_start()
{   // at the HUD sheet clear: last frame's routed count, this frame's marker centres from AKVR (sheet NDC)
    g_mkRoutedLast = InterlockedExchange(&f_mkRouted, 0);
    // MKGLOW4: the scratch chain is NOT per frame - Flash caches a filter's result and pastes it for many frames without
    // drawing the scratch again (the glow went into 3D only 1-4 times a second); a target leaves the set only when
    // something else is drawn into it
    for (auto& d : g_mkDrawn) d = MkDrawn{};   // MKDRAWN: this HUD frame's drawn marker pieces
    ++g_mkHudFrame;   // MKGLOW5: marks older than 30 HUD frames no longer route a paste
    if (!p_mkCentresTried) { HMODULE m = GetModuleHandleW(L"dinput8.dll"); p_mkCentres = m ? (MkCentresFn)GetProcAddress(m, "akvr_marker_centres") : nullptr; p_mkCentresTried = m != nullptr; }
    g_mkLastMk = -1; g_mkScanIdx = -1;
    if (g_mkEye && g_mkEyeRTV[0] && g_mkEyeRTV[1]) { if (!mk_eye_frame()) g_mkCN = 0; return; }   // MKEYE: this frame's own points
    float xy[16] = {}; const int n = p_mkCentres ? p_mkCentres(xy, 8) : 0;
    g_mkCN = n < 0 ? 0 : n > 8 ? 8 : n;
    for (int i = 0; i < g_mkCN; ++i) { g_mkC[i][0] = xy[i * 2]; g_mkC[i][1] = xy[i * 2 + 1]; }
}
bool hud_ready(ID3D11DeviceContext* c)
{
    if (!g_hudTex)
    {
        ID3D11Texture2D* bb = nullptr;
        if (!g_bb || FAILED(g_bb->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&bb))) return false;
        D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d);
        // HUDFULL 2026-10-08 (JJ: with DLSS "the HUD is now very low resolution"): AKVR lays the HUD out at the headset
        // size (its Scaleform viewports scaled up), so the HUD picture is the DLSS output size, not the small window
        UINT ow = 0, oh = 0; if (akn_dlss_mode() > 0) akn_dlss_out_size(ow, oh);
        if (ow > d.Width && oh > d.Height) { d.Width = ow; d.Height = oh; }
        D3D11_TEXTURE2D_DESC hd = d; hd.MipLevels = 1; hd.ArraySize = 1; hd.SampleDesc = { 1, 0 }; hd.Usage = D3D11_USAGE_DEFAULT;
        hd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE; hd.CPUAccessFlags = 0; hd.MiscFlags = 0;
        const bool ok = SUCCEEDED(g_dev->CreateTexture2D(&hd, nullptr, &g_hudTex)) &&
                        SUCCEEDED(g_dev->CreateRenderTargetView(g_hudTex, nullptr, &g_hudRTV));
        bb->Release();
        if (!ok) { logf("HUD texture: create FAILED"); g_hudOn = 0; g_hudPhase = false; return false; }
        logf("HUD texture %ux%u format %d", d.Width, d.Height, (int)d.Format);
        if (FAILED(g_dev->CreateTexture2D(&hd, nullptr, &g_mkTex)) || FAILED(g_dev->CreateRenderTargetView(g_mkTex, nullptr, &g_mkRTV)))
        { logf("MARKSHEET: marker sheet create FAILED - off"); g_markSheet = 0; if (g_mkTex) { g_mkTex->Release(); g_mkTex = nullptr; } }   // MARKSHEET
        for (int e = 0; e < 2; ++e)   // MKEYE: one marker layer per eye, the eye picture's size and lens
            if (FAILED(g_dev->CreateTexture2D(&hd, nullptr, &g_mkEyeTex[e])) || FAILED(g_dev->CreateRenderTargetView(g_mkEyeTex[e], nullptr, &g_mkEyeRTV[e])))
            { logf("MKEYE: eye marker layer create FAILED - off"); g_mkEye = 0; }
    }
    if (g_hudClearPending)
    {
        g_hudClearPending = false;
        const FLOAT z[4] = { 0, 0, 0, 0 };
        (o_ClearRTV ? o_ClearRTV : nullptr) ? o_ClearRTV(c, g_hudRTV, z) : c->ClearRenderTargetView(g_hudRTV, z);
        InterlockedIncrement(&c_hudClear); InterlockedIncrement(&g_hudSeq); InterlockedExchange(&f_hudIdx, 0);   // HUDSKIPIDX
        if (g_hudDSV && o_ClrDSV) o_ClrDSV(c, g_hudDSV, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);   // HUDDSV
        if (g_mkRTV) { o_ClearRTV ? o_ClearRTV(c, g_mkRTV, z) : c->ClearRenderTargetView(g_mkRTV, z); }   // MARKSHEET
        for (int e = 0; e < 2; ++e) if (g_mkEyeRTV[e]) { o_ClearRTV ? o_ClearRTV(c, g_mkEyeRTV[e], z) : c->ClearRenderTargetView(g_mkEyeRTV[e], z); }   // MKEYE
        g_mkCtx = c;   // DETCALLOUT
        mk_frame_start();
    }
    return true;
}
// HUDDSV 2026-10-09 (JJ: "all the HUD elements are flickering" ... "seems to only happen after we told DLSS not to scale
// the HUD"; HF run: the sheet switches between 4 and 10 covered cells every few frames with the same 28 draws). Since
// HUDFULL the sheet is the DLSS OUTPUT size (2624x2860) but the game binds its window buffer together with its window
// depth/stencil (the RENDER size, 1520x1656): D3D11 refuses a target + depth pair of different sizes, so the redirected
// bind fails and those draws (Scaleform's stencil-masked pieces, e.g. the compass strip) go to whatever was bound
// before. The sheet gets its own depth/stencil of its own size; the game's clears of its window depth during the HUD
// go to it.
ID3D11Texture2D* g_hudDsTex = nullptr; ID3D11DepthStencilView* g_hudGameDsv = nullptr;
ID3D11DepthStencilView* hud_dsv_for(ID3D11DepthStencilView* d)
{
    if (!d || !g_hudTex || !g_dev) return d;
    InterlockedIncrement(&f_hudDsvBind);
    ID3D11Resource* r = nullptr; d->GetResource(&r);
    ID3D11Texture2D* t = nullptr; if (r) { r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t); r->Release(); }
    if (!t) return d;
    D3D11_TEXTURE2D_DESC dd{}; t->GetDesc(&dd); t->Release();
    D3D11_TEXTURE2D_DESC hd{}; g_hudTex->GetDesc(&hd);
    if (dd.Width == hd.Width && dd.Height == hd.Height) return d;   // same size (DLSS off): the game's own is fine
    InterlockedIncrement(&f_hudDsvMis);
    if (!g_hudDsvOn) return d;
    if (!g_hudDSV)
    {
        D3D11_TEXTURE2D_DESC nd = dd; nd.Width = hd.Width; nd.Height = hd.Height; nd.MipLevels = 1; nd.ArraySize = 1; nd.SampleDesc = { 1, 0 };
        nd.Usage = D3D11_USAGE_DEFAULT; nd.CPUAccessFlags = 0; nd.MiscFlags = 0; nd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        D3D11_DEPTH_STENCIL_VIEW_DESC vd{}; d->GetDesc(&vd);
        vd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D; vd.Texture2D.MipSlice = 0;
        if (FAILED(g_dev->CreateTexture2D(&nd, nullptr, &g_hudDsTex)) || FAILED(g_dev->CreateDepthStencilView(g_hudDsTex, &vd, &g_hudDSV)))
        { logf("HUDDSV: own depth/stencil %ux%u f%d FAILED", nd.Width, nd.Height, (int)nd.Format); g_hudDsvOn = 0; return d; }
        logf("HUDDSV: the game's HUD depth/stencil is %ux%u f%d, the sheet %ux%u - the sheet gets its own", dd.Width, dd.Height, (int)dd.Format, hd.Width, hd.Height);
        if (o_ClrDSV && g_gameCtx) o_ClrDSV(g_gameCtx, g_hudDSV, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    }
    g_hudGameDsv = d;
    return g_hudDSV;
}
// DLSS D0 2026-10-08 (akvr_native/DLSS_PLAN.md): find Luma's swap points per eye - SMAA edge PS 0x067FF80F (depth =
// its SRV1), weights PS 0x2F55F98E, neighbourhood CS 0x42C0137E (SRV0 scene in, UAV0 out) - and the motion vectors
// (the R16G16_FLOAT target the game clears). Names = CRC-32 of the bytecode (AKVR's creation hook, akvr_shader_crc).
extern volatile LONG g_eyeNow;   // DEFERRED, below
using ShHashFn = unsigned long long(*)(const void*); extern ShHashFn p_shHash;   // AKVR's shader names, below
volatile LONG g_d0 = 0, c_d0[3][3] = {}, c_mvClear[3] = {}, g_d0Logs = 0;
using CrcFn = unsigned(*)(const void*);
CrcFn p_crc = nullptr; bool p_crcTried = false;
// PERF1 2026-10-10 (JJ: "we've lost about five to ten frames"; audit: 1-3 locked AKVR lookups per draw on every worker
// thread): a small per-thread cache in front of AKVR's shader names / CRCs. Renewed on every load hitch (g_shCacheGen).
volatile LONG g_shCacheGen = 1;
struct ShC { const void* o; unsigned long long h; LONG gen; };
thread_local ShC tl_shName[1024], tl_shCrc[512];
unsigned long long sh_hash_c(const void* o)
{
    if (!o || !p_shHash) return 0;
    ShC& e = tl_shName[((uintptr_t)o >> 4) & 1023]; const LONG g = g_shCacheGen;
    if (e.o == o && e.gen == g) return e.h;
    e.o = o; e.h = p_shHash(o); e.gen = g; return e.h;
}
unsigned sh_crc(const void* o)
{
    if (!p_crcTried) { HMODULE m = GetModuleHandleW(L"dinput8.dll"); p_crc = m ? (CrcFn)GetProcAddress(m, "akvr_shader_crc") : nullptr; p_crcTried = m != nullptr; }
    if (!p_crc || !o) return 0;
    ShC& e = tl_shCrc[((uintptr_t)o >> 4) & 511]; const LONG g = g_shCacheGen;
    if (e.o == o && e.gen == g) return (unsigned)e.h;
    e.o = o; e.h = p_crc(o); e.gen = g; return (unsigned)e.h;
}
int d0_where(bool deferred) { if (deferred) { const LONG e = g_eyeNow; return e == 0 || e == 1 ? (int)e : 2; } return tl_inEyeRender && tl_eye_render >= 0 && tl_eye_render < 2 ? tl_eye_render : 2; }
void res_desc(ID3D11Resource* r, char* b, size_t n);
void d0_srv(const char* what, ID3D11ShaderResourceView* v, char* out, size_t n)
{
    char d[96] = "none";
    if (v) { ID3D11Resource* r = nullptr; v->GetResource(&r); if (r) { res_desc(r, d, sizeof(d)); r->Release(); } }
    _snprintf_s(out, n, _TRUNCATE, "%s %s", what, d);
}
// D0b motion-vector hunt (g_d0 = 2): every pass in an eye render that reads or writes an R16G16_FLOAT texture
void d0_mv_note(const char* kind, unsigned long long name, int slot, ID3D11Resource* r, int w)
{
    if (!r) return;
    ID3D11Texture2D* t = nullptr;
    if (FAILED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) || !t) return;
    D3D11_TEXTURE2D_DESC d{}; t->GetDesc(&d); t->Release();
    if (d.Format != DXGI_FORMAT_R16G16_FLOAT && d.Format != DXGI_FORMAT_R16G16_TYPELESS) return;
    static unsigned long long seen[64]; static volatile LONG n = 0;
    const unsigned long long key = name ^ ((unsigned long long)slot << 56) ^ ((unsigned long long)kind[0] << 48);
    for (LONG i = 0; i < n && i < 64; ++i) if (seen[i] == key) return;
    const LONG i = InterlockedIncrement(&n) - 1; if (i >= 64) return; seen[i] = key;
    logf("D0b R16G16 texture %ux%u (fmt %d) as %s slot %d of shader %016llx in %s", d.Width, d.Height, (int)d.Format, kind, slot, name, w == 0 ? "left eye" : w == 1 ? "right eye" : "no eye render");
}
void d0_mv_scan_draw(ID3D11DeviceContext* c, int w)
{
    ID3D11PixelShader* ps = nullptr; c->PSGetShader(&ps, nullptr, nullptr);
    const unsigned long long nm = ps && p_shHash ? sh_hash_c(ps) : 0; if (ps) ps->Release();
    ID3D11RenderTargetView* rt[4] = {}; c->OMGetRenderTargets(4, rt, nullptr);
    for (int i = 0; i < 4; ++i) if (rt[i]) { ID3D11Resource* r = nullptr; rt[i]->GetResource(&r); d0_mv_note("render target", nm, i, r, w); if (r) r->Release(); rt[i]->Release(); }
    ID3D11ShaderResourceView* sv[8] = {}; c->PSGetShaderResources(0, 8, sv);
    for (int i = 0; i < 8; ++i) if (sv[i]) { ID3D11Resource* r = nullptr; sv[i]->GetResource(&r); d0_mv_note("PS input", nm, i, r, w); if (r) r->Release(); sv[i]->Release(); }
}
void d0_mv_scan_cs(ID3D11DeviceContext* c, int w)
{
    ID3D11ComputeShader* cs = nullptr; c->CSGetShader(&cs, nullptr, nullptr);
    const unsigned long long nm = cs && p_shHash ? sh_hash_c(cs) : 0; if (cs) cs->Release();
    ID3D11ShaderResourceView* sv[8] = {}; c->CSGetShaderResources(0, 8, sv);
    for (int i = 0; i < 8; ++i) if (sv[i]) { ID3D11Resource* r = nullptr; sv[i]->GetResource(&r); d0_mv_note("CS input", nm, i, r, w); if (r) r->Release(); sv[i]->Release(); }
    ID3D11UnorderedAccessView* uv[4] = {}; c->CSGetUnorderedAccessViews(0, 4, uv);
    for (int i = 0; i < 4; ++i) if (uv[i]) { ID3D11Resource* r = nullptr; uv[i]->GetResource(&r); d0_mv_note("CS output", nm, i, r, w); if (r) r->Release(); uv[i]->Release(); }
}
void scan_copy_depth(ID3D11DeviceContext* c, bool deferred, ID3D11Resource* r);   // DETCALLOUT (end of file)
void probe_copy_smaa(ID3D11DeviceContext* c, bool deferred, ID3D11Resource* r);   // DEPTHPROBE (end of file)
void depth_probe_after(ID3D11DeviceContext* c);
// DLSS D1: the game's SMAA is replaced - its edge pass hands us the depth (input 1) and is skipped with the weights
// pass; the neighbourhood pass becomes a plain copy (input -> output)
bool dlss_draw_skip(ID3D11DeviceContext* c, bool deferred)
{
    if (akn_dlss_mode() <= 0) return false;
    const int w = d0_where(deferred);
    if (w > 1) return false;
    ID3D11PixelShader* ps = nullptr; c->PSGetShader(&ps, nullptr, nullptr);
    const unsigned crc = sh_crc(ps); if (ps) ps->Release();
    if (crc == 0x067FF80Fu)
    {
        ID3D11ShaderResourceView* sv = nullptr; c->PSGetShaderResources(1, 1, &sv);
        if (sv) { ID3D11Resource* r = nullptr; sv->GetResource(&r); if (r) { akn_dlss_note_depth(w, r); if (w == 0) { scan_copy_depth(c, deferred, r); if (g_depthProbe > 0) probe_copy_smaa(c, deferred, r); } r->Release(); } sv->Release(); }   // DETCALLOUT: the depth is only whole here
        return true;
    }
    return crc == 0x2F55F98Eu;
}
bool dlss_dispatch_replace(ID3D11DeviceContext* c, bool deferred)
{
    if (akn_dlss_mode() <= 0) return false;
    if (d0_where(deferred) > 1) return false;
    ID3D11ComputeShader* cs = nullptr; c->CSGetShader(&cs, nullptr, nullptr);
    const unsigned crc = sh_crc(cs); if (cs) cs->Release();
    if (crc != 0x42C0137Eu) return false;
    ID3D11ShaderResourceView* sv = nullptr; c->CSGetShaderResources(0, 1, &sv);
    ID3D11UnorderedAccessView* uv = nullptr; c->CSGetUnorderedAccessViews(0, 1, &uv);
    ID3D11Resource* a = nullptr; ID3D11Resource* b = nullptr;
    if (sv) sv->GetResource(&a);
    if (uv) uv->GetResource(&b);
    const bool ok = a && b && a != b;
    if (ok)
    {   // unbind the output first (a bound UAV cannot be a copy target), copy, put it back
        ID3D11UnorderedAccessView* none = nullptr; UINT keep = (UINT)-1;
        c->CSSetUnorderedAccessViews(0, 1, &none, &keep);
        c->CopyResource(b, a);
        c->CSSetUnorderedAccessViews(0, 1, &uv, &keep);
    }
    if (a) a->Release(); if (b) b->Release(); if (sv) sv->Release(); if (uv) uv->Release();
    return ok;
}
void d0_draw(ID3D11DeviceContext* c, bool deferred)
{
    if (!g_d0) return;
    if (g_d0 >= 2) { const int w = d0_where(deferred); if (w < 2) d0_mv_scan_draw(c, w); }
    ID3D11PixelShader* ps = nullptr; c->PSGetShader(&ps, nullptr, nullptr);
    const unsigned crc = sh_crc(ps); if (ps) ps->Release();
    const int k = crc == 0x067FF80Fu ? 0 : crc == 0x2F55F98Eu ? 1 : -1;
    if (k < 0) return;
    const int w = d0_where(deferred);
    InterlockedIncrement(&c_d0[k][w]);
    if (g_d0Logs < 12 && InterlockedIncrement(&g_d0Logs) <= 12)
    {
        ID3D11ShaderResourceView* sv[2] = {}; c->PSGetShaderResources(0, 2, sv);
        char a[128], b[128]; d0_srv("t0", sv[0], a, sizeof(a)); d0_srv("t1", sv[1], b, sizeof(b));
        ID3D11RenderTargetView* rt = nullptr; c->OMGetRenderTargets(1, &rt, nullptr);
        char rd[96] = "none"; if (rt) { ID3D11Resource* r = nullptr; rt->GetResource(&r); if (r) { res_desc(r, rd, sizeof(rd)); r->Release(); } rt->Release(); }
        logf("D0 SMAA %s pass in %s (%s context): %s | %s | target %s", k ? "weights" : "edge", w == 0 ? "left eye" : w == 1 ? "right eye" : "no eye render", c->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE ? "immediate" : "recorded", a, b, rd);
        for (auto* v : sv) if (v) v->Release();
    }
}
void d0_dispatch(ID3D11DeviceContext* c, bool deferred)
{
    if (!g_d0) return;
    if (g_d0 >= 2) { const int w = d0_where(deferred); if (w < 2) d0_mv_scan_cs(c, w); }
    ID3D11ComputeShader* cs = nullptr; c->CSGetShader(&cs, nullptr, nullptr);
    const unsigned crc = sh_crc(cs); if (cs) cs->Release();
    if (crc != 0x42C0137Eu) return;
    const int w = d0_where(deferred);
    InterlockedIncrement(&c_d0[2][w]);
    if (g_d0Logs < 18 && InterlockedIncrement(&g_d0Logs) <= 18)
    {
        ID3D11ShaderResourceView* sv = nullptr; c->CSGetShaderResources(0, 1, &sv);
        ID3D11UnorderedAccessView* uv = nullptr; c->CSGetUnorderedAccessViews(0, 1, &uv);
        char a[128]; d0_srv("t0 (scene)", sv, a, sizeof(a));
        char o[96] = "none"; if (uv) { ID3D11Resource* r = nullptr; uv->GetResource(&r); if (r) { res_desc(r, o, sizeof(o)); r->Release(); } uv->Release(); }
        logf("D0 SMAA neighbourhood pass (DLSS goes here) in %s (%s context): %s | u0 (out) %s", w == 0 ? "left eye" : w == 1 ? "right eye" : "no eye render", c->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE ? "immediate" : "recorded", a, o);
        if (sv) sv->Release();
    }
}
void d0_clear(ID3D11DeviceContext* c, ID3D11RenderTargetView* v, bool deferred)
{
    if (!g_d0 || !v) return;
    ID3D11Resource* r = nullptr; v->GetResource(&r); if (!r) return;
    ID3D11Texture2D* t = nullptr;
    if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) && t)
    {
        D3D11_TEXTURE2D_DESC d{}; t->GetDesc(&d); t->Release();
        if (d.Format == DXGI_FORMAT_R16G16_FLOAT)
        {
            const int w = d0_where(deferred); InterlockedIncrement(&c_mvClear[w]);
            static volatile LONG logs = 0;
            if (logs < 4 && InterlockedIncrement(&logs) <= 4) logf("D0 motion vectors (R16G16_FLOAT cleared) %ux%u in %s (%s)", d.Width, d.Height, w == 0 ? "left eye" : w == 1 ? "right eye" : "no eye render", deferred ? "recorded" : "immediate");
        }
    }
    r->Release();
}
// D0 timing: each eye render's processor time (render thread) and graphics-card time (timestamp queries)
volatile LONG64 t_eyeCpu[2] = {}; volatile LONG c_eyeCpu[2] = {};
struct GpuT { ID3D11Query* dj = nullptr; ID3D11Query* a = nullptr; ID3D11Query* b = nullptr; bool pending = false; };
GpuT g_gt[2][6]; int g_gtIdx[2] = {}; double g_gpuMsSum[2] = {}; LONG g_gpuMsN[2] = {};
void gt_collect(ID3D11DeviceContext* c, int eye)
{
    for (auto& q : g_gt[eye])
    {
        if (!q.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{}; UINT64 a = 0, b = 0;
        if (c->GetData(q.dj, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (c->GetData(q.a, &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || c->GetData(q.b, &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        q.pending = false;
        if (!dj.Disjoint && dj.Frequency && b > a) { g_gpuMsSum[eye] += (double)(b - a) * 1000.0 / (double)dj.Frequency; ++g_gpuMsN[eye]; }
    }
}
GpuT* gt_begin(ID3D11Device* dev, ID3D11DeviceContext* c, int eye)
{
    gt_collect(c, eye);
    GpuT& q = g_gt[eye][g_gtIdx[eye] % 6];
    if (q.pending) return nullptr;
    if (!q.dj) { D3D11_QUERY_DESC d{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }; dev->CreateQuery(&d, &q.dj); d.Query = D3D11_QUERY_TIMESTAMP; dev->CreateQuery(&d, &q.a); dev->CreateQuery(&d, &q.b); }
    if (!q.dj || !q.a || !q.b) return nullptr;
    c->Begin(q.dj); c->End(q.a);
    ++g_gtIdx[eye];
    return &q;
}
void gt_end(ID3D11DeviceContext* c, GpuT* q) { if (!q) return; c->End(q->b); c->End(q->dj); q->pending = true; }
void STDMETHODCALLTYPE hk_ClearRTV(ID3D11DeviceContext* c, ID3D11RenderTargetView* v, const FLOAT col[4])
{
    d0_clear(c, v, false);
    if (tl_inEyeRender && v && h_on()) { ID3D11Resource* r = nullptr; v->GetResource(&r); if (r) { h_write(r); r->Release(); } }
    if ((g_hudPhase || g_hudAll) && !tl_inEyeRender && v && g_bb && c == g_gameCtx)
    {
        ID3D11Resource* r = nullptr; v->GetResource(&r);
        const bool isBB = r == g_bb; if (r) r->Release();
        if (isBB && hud_ready(c)) { const FLOAT z[4] = { 0, 0, 0, 0 }; o_ClearRTV(c, g_hudRTV, z); return; }   // keep the HUD texture transparent
    }
    if (g_mkScratchN && v && c == g_gameCtx && !tl_inEyeRender) { ID3D11Resource* r = nullptr; v->GetResource(&r); if (r) { mk_scratch_set(r, false, 0); r->Release(); } }   // MKGLOW5: a cleared target holds no marker
    o_ClearRTV(c, v, col);
}
void xe_rtvs(ID3D11RenderTargetView* const* v, UINT n, ID3D11DepthStencilView* d, bool deferred);   // XEYE, below
bool g_hudBound = false;   // the game's context has our HUD texture bound (render thread only)
// HUDVP (see g_hudVp): the game's requested viewport / scissor, and the sheet's version of a whole-window one
using RSVPFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
using RSScFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_RECT*);
RSVPFn o_RSVP = nullptr; RSScFn o_RSSc = nullptr;
D3D11_VIEWPORT g_reqVp[4] = {}; UINT g_reqVpN = 0; D3D11_RECT g_reqSc[4] = {}; UINT g_reqScN = 0; bool g_vpScaledNow = false;
bool hudvp_scale(float& sx, float& sy, UINT& winW, UINT& winH)
{
    if (!g_hudVp || !g_hudTex || !g_bb) return false;
    ID3D11Texture2D* bb = nullptr; if (FAILED(g_bb->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return false;
    D3D11_TEXTURE2D_DESC bd{}; bb->GetDesc(&bd); bb->Release();
    D3D11_TEXTURE2D_DESC hd{}; g_hudTex->GetDesc(&hd);
    if (bd.Width == hd.Width && bd.Height == hd.Height) return false;
    sx = (float)hd.Width / (float)bd.Width; sy = (float)hd.Height / (float)bd.Height; winW = bd.Width; winH = bd.Height;
    return true;
}
// apply the game's last requested viewport + scissor to the sheet (stretched when it is the whole window) or as asked
void hudvp_apply(ID3D11DeviceContext* c, bool toSheet)
{
    if (!o_RSVP || !o_RSSc) return;
    float sx = 1, sy = 1; UINT ww = 0, wh = 0;
    const bool can = toSheet && hudvp_scale(sx, sy, ww, wh);
    g_vpScaledNow = false;
    if (g_reqVpN)
    {
        D3D11_VIEWPORT v[4]; for (UINT i = 0; i < g_reqVpN; ++i) v[i] = g_reqVp[i];
        if (can && fabsf(v[0].TopLeftX) < 0.5f && fabsf(v[0].TopLeftY) < 0.5f && fabsf(v[0].Width - ww) < 0.5f && fabsf(v[0].Height - wh) < 0.5f)
        { v[0].Width *= sx; v[0].Height *= sy; g_vpScaledNow = true; InterlockedIncrement(&f_hudVpScaled); }
        o_RSVP(c, g_reqVpN, v);
    }
    if (g_reqScN)
    {
        D3D11_RECT r[4]; for (UINT i = 0; i < g_reqScN; ++i) r[i] = g_reqSc[i];
        if (g_vpScaledNow && r[0].right <= (LONG)ww && r[0].bottom <= (LONG)wh)
        { r[0].left = (LONG)lroundf(r[0].left * sx); r[0].top = (LONG)lroundf(r[0].top * sy); r[0].right = (LONG)lroundf(r[0].right * sx); r[0].bottom = (LONG)lroundf(r[0].bottom * sy); }
        o_RSSc(c, g_reqScN, r);
    }
}
void STDMETHODCALLTYPE hk_RSVP(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* v)
{
    if (c == g_gameCtx && v && n) { g_reqVpN = n < 4 ? n : 4; for (UINT i = 0; i < g_reqVpN; ++i) g_reqVp[i] = v[i]; if (g_hudBound) { hudvp_apply(c, true); return; } }
    o_RSVP(c, n, v);
}
void STDMETHODCALLTYPE hk_RSSc(ID3D11DeviceContext* c, UINT n, const D3D11_RECT* r)
{
    if (c == g_gameCtx && r && n) { g_reqScN = n < 4 ? n : 4; for (UINT i = 0; i < g_reqScN; ++i) g_reqSc[i] = r[i]; if (g_hudBound) { hudvp_apply(c, true); return; } }
    o_RSSc(c, n, r);
}
// HUDBLEND 2026-10-08 (JJ: weapon wheels and other see-through HUD pieces look "posterised", "in the wrong blending
// mode"). The HUD goes into our cleared transparent texture, but the game's blend states only care about colour (it
// draws onto an opaque picture), so the texture's alpha is not true coverage; AKVR's layer converts c / a to linear
// (HUDLAYER4), so a too-small alpha blows faint colours up into flat bands. Port of AKVR hudsplit blend_variant
// (HUD-004 / HUDLAYER5, geo-11 era): while the HUD texture is bound, a plain see-through blend gets alpha
// (ONE, INV_SRC_ALPHA, add) with alpha written = premultiplied with true coverage; other blend modes (add, multiply,
// min/max) leave the alpha alone. Live: command "hudblend 0|1".
using BlendFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11BlendState*, const FLOAT[4], UINT);
BlendFn o_Blend = nullptr;
ID3D11BlendState* g_reqBlend = nullptr; FLOAT g_reqFactor[4] = { 1, 1, 1, 1 }; UINT g_reqMask = 0xffffffff; bool g_reqValid = false;
struct BlendVar { ID3D11BlendState* o; ID3D11BlendState* v; };
BlendVar g_bv[64]; int g_bvN = 0;
volatile LONG g_hudBlend = 1, c_bvSwaps = 0, c_bvOther = 0, c_bvFail = 0;
ID3D11BlendState* blend_variant(ID3D11DeviceContext* c, ID3D11BlendState* bs)
{
    if (!bs) return nullptr;                      // default state: no blending, alpha written as is
    for (int i = 0; i < g_bvN; ++i) if (g_bv[i].o == bs) return g_bv[i].v;
    if (g_bvN >= 64) return bs;
    D3D11_BLEND_DESC d{}; bs->GetDesc(&d);
    const int n = d.IndependentBlendEnable ? 8 : 1;
    for (int i = 0; i < n; ++i)
    {
        D3D11_RENDER_TARGET_BLEND_DESC& t = d.RenderTarget[i];
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
            InterlockedIncrement(&c_bvOther);
        }
        else if (t.RenderTargetWriteMask & 7) t.RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    }
    ID3D11Device* dev = nullptr; c->GetDevice(&dev);
    ID3D11BlendState* v = nullptr;
    if (!dev || FAILED(dev->CreateBlendState(&d, &v)) || !v) { InterlockedIncrement(&c_bvFail); v = nullptr; }
    if (dev) dev->Release();
    if (!v) return bs;
    if (g_bvN < 3) logf("HUDBLEND: variant for blend %p: colour %d/%d op %d, alpha now ONE/INV_SRC_ALPHA (enable %d)", (void*)bs, (int)d.RenderTarget[0].SrcBlend, (int)d.RenderTarget[0].DestBlend, (int)d.RenderTarget[0].BlendOp, (int)d.RenderTarget[0].BlendEnable);
    g_bv[g_bvN++] = { bs, v };                    // kept for the process lifetime (a few dozen)
    return v;
}
void STDMETHODCALLTYPE hk_Blend(ID3D11DeviceContext* c, ID3D11BlendState* bs, const FLOAT f[4], UINT m)
{
    if (c == g_gameCtx)
    {
        g_reqBlend = bs; if (f) memcpy(g_reqFactor, f, sizeof(g_reqFactor)); else { g_reqFactor[0] = g_reqFactor[1] = g_reqFactor[2] = g_reqFactor[3] = 1.0f; }
        g_reqMask = m; g_reqValid = true;
        if (g_hudBound && g_hudBlend) { InterlockedIncrement(&c_bvSwaps); o_Blend(c, blend_variant(c, bs), f, m); return; }
    }
    o_Blend(c, bs, f, m);
}
void STDMETHODCALLTYPE hk_OMSetRT(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* v, ID3D11DepthStencilView* d)
{
    if (g_vwRes) vw_rtvs(c, v, n, false);   // VOLWATCH
    xe_rtvs(v, n, d, false);
    if (tl_inEyeRender && !g_dev) { c->GetDevice(&g_dev); g_gameCtx = c; logf("game device %p (from the render thread's context %p)", (void*)g_dev, (void*)c); }
    if (c != g_gameCtx) { o_OMSetRT(c, n, v, d); return; }   // another device (Virtual Desktop's compositor, AKVR's own): not ours
    if ((g_hudPhase || g_hudAll) && !tl_inEyeRender && n && n <= 8 && v && v[0] && g_bb)
    {
        ID3D11Resource* r = nullptr; v[0]->GetResource(&r);
        const bool isBB = r == g_bb; if (r) r->Release();
        if (isBB && hud_ready(c))
        {
            ID3D11RenderTargetView* vv[8]; for (UINT i = 0; i < n; ++i) vv[i] = v[i];
            vv[0] = g_hudRTV;
            InterlockedIncrement(&c_hudRedirect); InterlockedIncrement(&f_hudRedir);
            o_OMSetRT(c, n, vv, hud_dsv_for(d));   // HUDDSV
            g_hudBound = true;   // HUDBLEND: the current blend gets its coverage variant
            hudvp_apply(c, true);   // HUDVP
            if (o_Blend && g_reqValid && g_hudBlend) o_Blend(c, blend_variant(c, g_reqBlend), g_reqFactor, g_reqMask);
            return;
        }
    }
    else if (!tl_inEyeRender && n && v && v[0] && g_bb && g_hudStat > 0)
    {   // HUDTRACE: a window-buffer bind the sheet did not get
        ID3D11Resource* r = nullptr; v[0]->GetResource(&r);
        if (r == g_bb) InterlockedIncrement(&f_hudMissImm);
        if (r) r->Release();
    }
    if (g_hudBound)
    {   // HUDBLEND: leaving the HUD texture - the game's own blend state back
        g_hudBound = false;
        if (o_Blend && g_reqValid) o_Blend(c, g_reqBlend, g_reqFactor, g_reqMask);
        if (g_vpScaledNow) hudvp_apply(c, false);   // HUDVP: the game's own viewport back
    }
    if (!tl_inEyeRender && g_afterLogs > 0 && g_afterEye0 && n && v && v[0])
    {
        // between the left eye's scene render and the next right-eye render: where the final picture is made
        ID3D11Resource* r = nullptr; v[0]->GetResource(&r);
        if (r)
        {
            ID3D11Texture2D* t = nullptr; D3D11_TEXTURE2D_DESC d{};
            if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t))) { t->GetDesc(&d); t->Release(); }
            if (r != g_afterPrev)
            {
                char b[512]; int k = snprintf(b, sizeof(b), "after-scene target %p %ux%u/f%d tid %lu, stack:", (void*)r, d.Width, d.Height, (int)d.Format, GetCurrentThreadId());
                void* fr[10] = {}; const USHORT nf = RtlCaptureStackBackTrace(1, 10, fr, nullptr);
                for (USHORT i = 0; i < nf; ++i) { const uintptr_t a = (uintptr_t)fr[i]; if (a >= g_base && a < g_base + 0x4000000) k += snprintf(b + k, sizeof(b) - k, " %llX", (unsigned long long)(a - g_base)); }
                logf("%s", b);
                if (--g_afterLogs == 0) logf("after-scene log done");
            }
            g_afterPrev = r;
            r->Release();
        }
    }
    if (tl_inEyeRender && h_on())
    {
        for (UINT i = 0; v && i < n && i < 8 && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL; ++i) if (v[i]) { ID3D11Resource* r = nullptr; v[i]->GetResource(&r); if (r) { h_write(r); r->Release(); } }
        if (d) { ID3D11Resource* r = nullptr; d->GetResource(&r); if (r) { h_write(r); r->Release(); } }
    }
    if (tl_inEyeRender && n && v && v[0] && (g_capWant || g_rtLogs < 4 || g_rtListLogs > 0))   // PERF1: captures / first logs only
    {
        ID3D11Resource* r = nullptr; v[0]->GetResource(&r);
        if (r)
        {
            ID3D11Texture2D* t = nullptr; D3D11_TEXTURE2D_DESC d{};
            if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t))) { t->GetDesc(&d); t->Release(); }
            // the eye's picture = the last full-size colour target (post effects end there; small helpers follow)
            if (d.Width >= 1000)
            {
                tl_lastRT = r;
                bool seen = false; for (int i = 0; i < tl_bigN; ++i) seen |= tl_big[i] == r;
                if (!seen && tl_bigN < 16) tl_big[tl_bigN++] = r;
            }
            if (g_rtListLogs > 0 && tl_rtSets < 120 && r != tl_prevRT)
            {
                char b[96]; snprintf(b, sizeof(b), " %p:%ux%u/f%d", (void*)r, d.Width, d.Height, (int)d.Format);
                if (strlen(tl_rtList) + strlen(b) < sizeof(tl_rtList) - 1) strcat_s(tl_rtList, b);
            }
            tl_prevRT = r;
            r->Release(); ++tl_rtSets;
        }
    }
    o_OMSetRT(c, n, v, d);
}
// draws between the left eye's scene and the next frame: the final combine (SMAA, glow, blur) and the HUD
using DrawFn11 = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIdxFn11 = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
DrawFn11 o_Draw11 = nullptr; DrawIdxFn11 o_DrawIdx11 = nullptr;
int g_drawLogs = 0;
void res_desc(ID3D11Resource* r, char* b, size_t n)
{
    ID3D11Texture2D* t = nullptr; D3D11_TEXTURE2D_DESC d{};
    if (r && SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t))) { t->GetDesc(&d); t->Release(); snprintf(b, n, "%p:%ux%u/f%d", (void*)r, d.Width, d.Height, (int)d.Format); }
    else snprintf(b, n, "%p", (void*)r);
}
void log_draw(ID3D11DeviceContext* c, const char* kind, UINT count)
{
    if (!g_afterEye0 || g_drawLogs <= 0 || tl_inEyeRender || c != g_gameCtx) return;
    --g_drawLogs;
    char b[2048]; int k = snprintf(b, sizeof(b), "after-scene %s %u:", kind, count);
    ID3D11PixelShader* ps = nullptr; c->PSGetShader(&ps, nullptr, nullptr); k += snprintf(b + k, sizeof(b) - k, " ps %p", (void*)ps); if (ps) ps->Release();
    ID3D11RenderTargetView* rtv = nullptr; c->OMGetRenderTargets(1, &rtv, nullptr);
    if (rtv) { ID3D11Resource* r = nullptr; rtv->GetResource(&r); char d[96]; res_desc(r, d, sizeof(d)); k += snprintf(b + k, sizeof(b) - k, " -> %s", d); if (r) r->Release(); rtv->Release(); }
    ID3D11ShaderResourceView* srv[8] = {}; c->PSGetShaderResources(0, 8, srv);
    for (int i = 0; i < 8; ++i) if (srv[i]) { ID3D11Resource* r = nullptr; srv[i]->GetResource(&r); char d[96]; res_desc(r, d, sizeof(d)); k += snprintf(b + k, sizeof(b) - k, " t%d=%s", i, d); if (r) r->Release(); srv[i]->Release(); }
    void* fr[12] = {}; const USHORT nf = RtlCaptureStackBackTrace(1, 12, fr, nullptr);
    k += snprintf(b + k, sizeof(b) - k, " | stack");
    for (USHORT i = 0; i < nf; ++i) { const uintptr_t a = (uintptr_t)fr[i]; if (a >= g_base && a < g_base + 0x4000000) k += snprintf(b + k, sizeof(b) - k, " %llX", (unsigned long long)(a - g_base)); }
    logf("%s", b);
}
// HISTSWAP (JJ's 5th Quest test 2026-10-07: "lighting, shadows and reflections completely different in each eye";
// SWBF2 had a sun-shadow buffer shared between the eyes). Effects that keep data "from last time" (reflection/lighting
// history, light shafts, particle state) know nothing of eyes: the left eye's render continues from what the right
// eye's render just left, and the other way round. Detector: per eye render, every resource READ (shader input) or
// read-written (UAV) before anything WROTE it in that render = state carried over. Those both eyes use are listed;
// with HISTSWAP each eye keeps its own copy: restored before its render, saved after.
struct HEnt { ID3D11Resource* r; bool written; bool rbw; };
thread_local HEnt tl_h[768]; thread_local int tl_hN = 0;
ID3D11Resource* g_hist[2][96] = {}; int g_histN[2] = {};       // per eye: carried-over resources found in its last render
ID3D11Resource* g_cand[96] = {}; int g_candN = 0;              // in BOTH eyes' renders: the shared ones
ID3D11Resource* g_bak[2][96] = {};                             // HISTSWAP copies, per eye per candidate
volatile LONG g_histSwap = 0;      // command "histswap 0|1"
volatile LONG g_histLog = 1, c_histCopies = 0;
HEnt* h_find(ID3D11Resource* r) { for (int i = 0; i < tl_hN; ++i) if (tl_h[i].r == r) return &tl_h[i]; return nullptr; }
void h_read(ID3D11Resource* r)  { if (!tl_inEyeRender || !r || !h_on()) return; if (!h_find(r) && tl_hN < 768) tl_h[tl_hN++] = { r, false, true }; }
void h_write(ID3D11Resource* r) { if (!tl_inEyeRender || !r || !h_on()) return; HEnt* e = h_find(r); if (e) e->written = true; else if (tl_hN < 768) tl_h[tl_hN++] = { r, true, false }; }
void h_rw(ID3D11Resource* r)    { if (!tl_inEyeRender || !r || !h_on()) return; HEnt* e = h_find(r); if (e) e->written = true; else if (tl_hN < 768) tl_h[tl_hN++] = { r, true, true }; }
void h_views_read(ID3D11ShaderResourceView* const* v, UINT n) { if (!h_on()) return; for (UINT i = 0; v && i < n; ++i) if (v[i]) { ID3D11Resource* r = nullptr; v[i]->GetResource(&r); if (r) { h_read(r); r->Release(); } } }
void h_uavs(ID3D11UnorderedAccessView* const* u, UINT n) { if (!h_on()) return; for (UINT i = 0; u && i < n; ++i) if (u[i]) { ID3D11Resource* r = nullptr; u[i]->GetResource(&r); if (r) { h_rw(r); r->Release(); } } }
using SetSRVFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
using SetUAVFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
using ClearUAVuFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11UnorderedAccessView*, const UINT[4]);
using ClearUAVfFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11UnorderedAccessView*, const FLOAT[4]);
using ClearDSVFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
SetSRVFn o_PSSRV = nullptr, o_VSSRV = nullptr, o_CSSRV = nullptr; SetUAVFn o_CSUAV = nullptr;
ClearUAVuFn o_ClrUAVu = nullptr; ClearUAVfFn o_ClrUAVf = nullptr; ClearDSVFn o_ClrDSV = nullptr;
// XEYE 2026-10-08 (JJ: "some of the reflections on Batman appear to be mismatched per eye ... like Arkham City's
// puddles"; Arkham City: the reflection picture was rendered in ONE eye's pass and reused by the other). For N frames,
// every texture bound for reading / writing in an eye render (main and recorded drawing) is noted with the eye; at
// the end the textures READ by an eye that never WRITES them, while the other eye does, are listed.
struct XEnt { unsigned char writers, readers; LONG reads; };
std::unordered_map<void*, XEnt> g_xe; SRWLOCK g_xeLock = SRWLOCK_INIT;
volatile LONG g_xeye = 0;
void xe_note(ID3D11Resource* r, int eye, bool write)
{
    if (!r || eye < 0 || eye > 1) return;
    AcquireSRWLockExclusive(&g_xeLock);
    XEnt& e = g_xe[(void*)r];
    if (write) e.writers |= (unsigned char)(1 << eye); else { e.readers |= (unsigned char)(1 << eye); ++e.reads; }
    ReleaseSRWLockExclusive(&g_xeLock);
}
int xe_eye(bool deferred) { if (deferred) { const LONG e = g_eyeNow; return e == 0 || e == 1 ? (int)e : -1; } return tl_inEyeRender ? tl_eye_render : -1; }
void xe_srvs(ID3D11ShaderResourceView* const* v, UINT n, bool deferred)
{
    if (g_xeye <= 0 || !v) return;
    const int eye = xe_eye(deferred); if (eye < 0) return;
    for (UINT i = 0; i < n && i < 32; ++i) if (v[i]) { ID3D11Resource* r = nullptr; v[i]->GetResource(&r); if (r) { xe_note(r, eye, false); r->Release(); } }
}
// DETCALLOUT (DC1 run 4, 03:36: the DLSS-path "depth" read 1.0 at every sample even right after the left eye's render): the
// depth buffers the game binds during the LEFT eye's render, counted; the scan pass reads the most-bound one (the scene depth)
SRWLOCK g_scanDsLock = SRWLOCK_INIT;
struct ScanDs { ID3D11Resource* r; LONG n; UINT w, h; DXGI_FORMAT f; UINT bind; };
ScanDs g_scanDs[8] = {}; int g_scanDsN = 0;
void scan_note_dsv(ID3D11DepthStencilView* d, bool deferred)
{
    if (!d || !g_scanOn || xe_eye(deferred) != 0) return;
    ID3D11Resource* r = nullptr; d->GetResource(&r); if (!r) return;
    AcquireSRWLockExclusive(&g_scanDsLock);
    int k = 0; for (; k < g_scanDsN; ++k) if (g_scanDs[k].r == r) { ++g_scanDs[k].n; break; }
    if (k == g_scanDsN && g_scanDsN < 8)
    {
        ScanDs& e = g_scanDs[g_scanDsN++]; e = ScanDs{ r, 1, 0, 0, DXGI_FORMAT_UNKNOWN, 0 };
        ID3D11Texture2D* t = nullptr; if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) && t) { D3D11_TEXTURE2D_DESC td; t->GetDesc(&td); e.w = td.Width; e.h = td.Height; e.f = td.Format; e.bind = td.BindFlags; t->Release(); }
    }
    ReleaseSRWLockExclusive(&g_scanDsLock);
    r->Release();
}
// the most-bound depth buffer of the last left-eye render that can be read (SHADER_RESOURCE), then the table starts again
ID3D11Resource* scan_take_dsv(UINT& w, UINT& h, int& fmt, LONG& binds)
{
    ID3D11Resource* best = nullptr; w = h = 0; fmt = 0; binds = 0;
    AcquireSRWLockExclusive(&g_scanDsLock);
    for (int k = 0; k < g_scanDsN; ++k)
        if ((g_scanDs[k].bind & D3D11_BIND_SHADER_RESOURCE) && g_scanDs[k].n > binds) { best = g_scanDs[k].r; binds = g_scanDs[k].n; w = g_scanDs[k].w; h = g_scanDs[k].h; fmt = (int)g_scanDs[k].f; }
    static int s_log = 0;
    if (s_log < 3 && g_scanDsN) { ++s_log; for (int k = 0; k < g_scanDsN; ++k) logf("DETCALLOUT: left-eye depth buffer %p %ux%u format %d bind 0x%x, bound %ld x%s", (void*)g_scanDs[k].r, g_scanDs[k].w, g_scanDs[k].h, (int)g_scanDs[k].f, g_scanDs[k].bind, g_scanDs[k].n, g_scanDs[k].r == best ? " <- used" : ""); }
    g_scanDsN = 0;
    ReleaseSRWLockExclusive(&g_scanDsLock);
    return best;
}
void xe_rtvs(ID3D11RenderTargetView* const* v, UINT n, ID3D11DepthStencilView* d, bool deferred)
{
    scan_note_dsv(d, deferred);   // DETCALLOUT
    if (g_xeye <= 0) return;
    const int eye = xe_eye(deferred); if (eye < 0) return;
    for (UINT i = 0; v && i < n && i < 8; ++i) if (v[i]) { ID3D11Resource* r = nullptr; v[i]->GetResource(&r); if (r) { xe_note(r, eye, true); r->Release(); } }
    if (d) { ID3D11Resource* r = nullptr; d->GetResource(&r); if (r) { xe_note(r, eye, true); r->Release(); } }
}
void xe_uavs(ID3D11UnorderedAccessView* const* u, UINT n, bool deferred)
{
    if (g_xeye <= 0 || !u) return;
    const int eye = xe_eye(deferred); if (eye < 0) return;
    for (UINT i = 0; i < n && i < 8; ++i) if (u[i]) { ID3D11Resource* r = nullptr; u[i]->GetResource(&r); if (r) { xe_note(r, eye, true); r->Release(); } }
}
void xe_end_frame()
{   // after the left eye's render (the second): count down, report at 0
    if (g_xeye <= 0 || InterlockedDecrement(&g_xeye) > 0) return;
    struct Row { void* r; XEnt e; };
    std::vector<Row> rows;
    AcquireSRWLockExclusive(&g_xeLock);
    for (auto& kv : g_xe) { const XEnt& e = kv.second; if (e.writers && (e.readers & ~e.writers)) rows.push_back({ kv.first, e }); }
    const size_t total = g_xe.size(); g_xe.clear();
    ReleaseSRWLockExclusive(&g_xeLock);
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.e.reads > b.e.reads; });
    logf("XEYE: %zu textures seen; %zu read by an eye that never draws them (the other eye does):", total, rows.size());
    for (size_t i = 0; i < rows.size() && i < 40; ++i)
    {
        char d[96]; res_desc((ID3D11Resource*)rows[i].r, d, sizeof(d));
        // XEYE2: the texture's layout (array / cube / mips) and, for small 2D ones, a picture of it
        char extra[96] = "";
        ID3D11Texture2D* t2 = nullptr;
        if (SUCCEEDED(((ID3D11Resource*)rows[i].r)->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t2)) && t2)
        {
            D3D11_TEXTURE2D_DESC td{}; t2->GetDesc(&td);
            _snprintf_s(extra, sizeof(extra), _TRUNCATE, " | array %u mips %u%s bind 0x%x", td.ArraySize, td.MipLevels, (td.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) ? " CUBE" : "", td.BindFlags);
            if (td.Width <= 1024 && td.Height <= 1024 && td.Width >= 64 && g_dev)
            {
                ID3D11DeviceContext* ic = nullptr; g_dev->GetImmediateContext(&ic);
                if (ic) { wchar_t nm[32]; swprintf_s(nm, L"xeye_%zu", i); save_bmp(ic, t2, 0, -1, nm); ic->Release(); }
            }
            t2->Release();
        }
        logf("  XEYE %s%s | drawn by %s, read by %s, %ld reads", d, extra, rows[i].e.writers == 1 ? "left" : rows[i].e.writers == 2 ? "right" : "both",
             (rows[i].e.readers & ~rows[i].e.writers) == 1 ? "left" : "right", rows[i].e.reads);
    }
}
// MIPBIAS 2026-10-08 (DLSS_PLAN D1 "negative mip bias for the smaller render", never built; Luma's AK mod does it:
// Luma Framework core.hpp CreateCustomSampler mode 4 + super_resolution.h GetMipLODBias): with the eye rendered small,
// the world's textures are picked for the SMALL picture (blurrier mip levels); DLSS then rebuilds full-size edges but
// not texture detail. Every anisotropic sampler (world materials; post passes use point / linear and stay as they are)
// is swapped at bind time for a twin with MipLODBias += log2(render / output) - 1 and 16x anisotropy. The game makes its
// samplers before our hooks exist, so the twins are made on first use. Live: command "mipbias 0|1|2"
// (0 off, 1 Luma's log2(scale) - 1 = default, 2 milder log2(scale)).
volatile LONG g_mipBias = 1;
struct MipTwin { ID3D11SamplerState* t[4]; bool aniso; };   // [3] = +4 blur (diagnostic: proves the swap reaches the world)
std::unordered_map<ID3D11SamplerState*, MipTwin> g_mipTwins; SRWLOCK g_mipLock = SRWLOCK_INIT;
volatile LONG g_mipSwaps = 0, g_mipMade = 0;
struct MipC { ID3D11SamplerState* s; ID3D11SamplerState* r; int mode; LONG gen; };
thread_local MipC tl_mip[256];   // PERF1: the twin per sampler, per thread (the map lock was taken on every sampler bind)
ID3D11SamplerState* mip_twin_locked(ID3D11SamplerState* s, int mode);
ID3D11SamplerState* mip_twin(ID3D11SamplerState* s, int mode)
{
    if (!s || mode <= 0 || mode > 3) return s;
    MipC& e = tl_mip[((uintptr_t)s >> 4) & 255]; const LONG g = g_shCacheGen;
    if (e.s == s && e.mode == mode && e.gen == g) return e.r;
    ID3D11SamplerState* r = mip_twin_locked(s, mode);
    e.s = s; e.r = r; e.mode = mode; e.gen = g;
    return r;
}
ID3D11SamplerState* mip_twin_locked(ID3D11SamplerState* s, int mode)
{
    AcquireSRWLockShared(&g_mipLock);
    auto it = g_mipTwins.find(s);
    ID3D11SamplerState* r = it != g_mipTwins.end() ? (it->second.aniso ? it->second.t[mode] : s) : nullptr;
    ReleaseSRWLockShared(&g_mipLock);
    if (r) return r;
    D3D11_SAMPLER_DESC d{}; s->GetDesc(&d);
    // MIPBIAS2: AK draws much of the world (the rooftop ground) with plain trilinear samplers, not anisotropic (blur test
    // +4 on the 6 anisotropic ones barely changed the ground). Also take linear-filtered samplers that TILE (wrap /
    // mirror on U or V) = surface materials; screen passes clamp and keep their sampler.
    const bool tiles = d.AddressU == D3D11_TEXTURE_ADDRESS_WRAP || d.AddressU == D3D11_TEXTURE_ADDRESS_MIRROR || d.AddressV == D3D11_TEXTURE_ADDRESS_WRAP || d.AddressV == D3D11_TEXTURE_ADDRESS_MIRROR;
    const bool lin = d.Filter == D3D11_FILTER_MIN_MAG_MIP_LINEAR || d.Filter == D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    MipTwin tw{ { nullptr, nullptr, nullptr, nullptr }, d.Filter == D3D11_FILTER_ANISOTROPIC || d.Filter == D3D11_FILTER_COMPARISON_ANISOTROPIC || (lin && tiles) };
    const bool keepFilter = !(d.Filter == D3D11_FILTER_ANISOTROPIC || d.Filter == D3D11_FILTER_COMPARISON_ANISOTROPIC);
    {   // census: every distinct sampler the eye renders bind (first 60)
        static volatile LONG n = 0;
        if (InterlockedIncrement(&n) <= 60) logf("MIPBIAS census: sampler %p filter 0x%x aniso %u bias %.2f lod %.1f..%.1f address %d/%d/%d", (void*)s, (unsigned)d.Filter, d.MaxAnisotropy, d.MipLODBias, d.MinLOD, d.MaxLOD, (int)d.AddressU, (int)d.AddressV, (int)d.AddressW);
    }
    if (tw.aniso)
    {
        const float sc = akn_dlss_scale(), base = log2f(sc > 0.05f && sc < 1.0f ? sc : 1.0f);
        ID3D11Device* dev = nullptr; s->GetDevice(&dev);
        for (int m = 1; m <= 3 && dev; ++m)
        {
            D3D11_SAMPLER_DESC b = d;
            b.MipLODBias = std::clamp(m == 3 ? d.MipLODBias + 4.0f : d.MipLODBias + base - (m == 1 ? 1.0f : 0.0f), D3D11_MIP_LOD_BIAS_MIN, D3D11_MIP_LOD_BIAS_MAX);
            if (!keepFilter) b.MaxAnisotropy = D3D11_REQ_MAXANISOTROPY;   // trilinear ones keep their filter (Luma: forcing AF can break things)
            b.MinLOD = (std::max)(d.MinLOD + (std::min)(b.MipLODBias - d.MipLODBias, 0.0f), 0.0f);
            if (FAILED(dev->CreateSamplerState(&b, &tw.t[m]))) tw.t[m] = nullptr;
        }
        if (dev) dev->Release();
        if (!tw.t[1] || !tw.t[2] || !tw.t[3]) { for (int m = 1; m <= 3; ++m) if (tw.t[m]) tw.t[m]->Release(); tw.aniso = false; }
        else if (InterlockedIncrement(&g_mipMade) <= 12) logf("MIPBIAS: twin for sampler %p filter 0x%x: bias %.2f -> %.2f / %.2f, anisotropy %u", (void*)s, (unsigned)d.Filter, d.MipLODBias, d.MipLODBias + base - 1.0f, d.MipLODBias + base, d.MaxAnisotropy);
    }
    AcquireSRWLockExclusive(&g_mipLock);
    auto ins = g_mipTwins.emplace(s, tw);
    if (!ins.second && tw.aniso) { for (int m = 1; m <= 3; ++m) tw.t[m]->Release(); }   // another thread made it first
    const MipTwin& u = ins.first->second;
    r = u.aniso ? u.t[mode] : s;
    ReleaseSRWLockExclusive(&g_mipLock);
    return r;
}
// only while DLSS renders small; samplers bound outside the eye renders (HUD, menus) are left alone too
bool mip_wanted(bool deferred) { const LONG m = g_mipBias; return m > 0 && akn_dlss_mode() > 1 && (deferred ? (g_eyeNow == 0 || g_eyeNow == 1) : tl_inEyeRender); }
using SampFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11SamplerState* const*);
SampFn o_PSSamp = nullptr, od_PSSamp = nullptr;
void mip_call(SampFn f, ID3D11DeviceContext* c, UINT s0, UINT n, ID3D11SamplerState* const* v, bool deferred)
{
    if (!v || n == 0 || n > 16 || !mip_wanted(deferred)) { f(c, s0, n, v); return; }
    ID3D11SamplerState* a[16]; const int mode = (int)g_mipBias; bool any = false;
    for (UINT i = 0; i < n; ++i) { a[i] = mip_twin(v[i], mode); any |= a[i] != v[i]; }
    if (any) ++g_mipSwaps;   // PERF1: telemetry, no locked increment
    f(c, s0, n, any ? a : v);
}
void STDMETHODCALLTYPE hk_PSSamp(ID3D11DeviceContext* c, UINT s0, UINT n, ID3D11SamplerState* const* v) { mip_call(o_PSSamp, c, s0, n, v, false); }
void STDMETHODCALLTYPE hkd_PSSamp(ID3D11DeviceContext* c, UINT s0, UINT n, ID3D11SamplerState* const* v) { mip_call(od_PSSamp, c, s0, n, v, true); }
void STDMETHODCALLTYPE hk_PSSRV(ID3D11DeviceContext* c, UINT s0, UINT n, ID3D11ShaderResourceView* const* v) { xe_srvs(v, n, false); if (tl_inEyeRender) h_views_read(v, n); o_PSSRV(c, s0, n, v); }
void STDMETHODCALLTYPE hk_VSSRV(ID3D11DeviceContext* c, UINT s0, UINT n, ID3D11ShaderResourceView* const* v) { xe_srvs(v, n, false); if (tl_inEyeRender) h_views_read(v, n); o_VSSRV(c, s0, n, v); }
void STDMETHODCALLTYPE hk_CSSRV(ID3D11DeviceContext* c, UINT s0, UINT n, ID3D11ShaderResourceView* const* v) { xe_srvs(v, n, false); if (tl_inEyeRender) h_views_read(v, n); o_CSSRV(c, s0, n, v); }
void STDMETHODCALLTYPE hk_CSUAV(ID3D11DeviceContext* c, UINT s0, UINT n, ID3D11UnorderedAccessView* const* u, const UINT* ic) { xe_uavs(u, n, false); if (g_vwRes) vw_uavs(c, u, n, false); if (tl_inEyeRender) h_uavs(u, n); o_CSUAV(c, s0, n, u, ic); }
void STDMETHODCALLTYPE hk_ClrUAVu(ID3D11DeviceContext* c, ID3D11UnorderedAccessView* u, const UINT v[4]) { if (tl_inEyeRender && u && h_on()) { ID3D11Resource* r = nullptr; u->GetResource(&r); if (r) { h_write(r); r->Release(); } } o_ClrUAVu(c, u, v); }
void STDMETHODCALLTYPE hk_ClrUAVf(ID3D11DeviceContext* c, ID3D11UnorderedAccessView* u, const FLOAT v[4]) { if (tl_inEyeRender && u && h_on()) { ID3D11Resource* r = nullptr; u->GetResource(&r); if (r) { h_write(r); r->Release(); } } o_ClrUAVf(c, u, v); }
void scan_clear_copy(ID3D11DeviceContext* c, bool deferred, ID3D11DepthStencilView* d, UINT f);   // DETCALLOUT (end of file)
void STDMETHODCALLTYPE hk_ClrDSV(ID3D11DeviceContext* c, ID3D11DepthStencilView* d, UINT f, FLOAT z, UINT8 st) {
    if (tl_inEyeRender && tl_eye_render == 0 && d) scan_clear_copy(c, false, d, f);   // DETCALLOUT: the world's depth before a mid-render clear
    if (g_hudDsvOn && g_hudDSV && d && d == g_hudGameDsv && !tl_inEyeRender && c == g_gameCtx && (g_hudPhase || g_hudAll)) { o_ClrDSV(c, g_hudDSV, f, z, st); return; }   // HUDDSV
    if (tl_inEyeRender && d && h_on()) { ID3D11Resource* r = nullptr; d->GetResource(&r); if (r) { h_write(r); r->Release(); } } o_ClrDSV(c, d, f, z, st); }
void hist_end_render(int eye)
{
    int n = 0;
    for (int i = 0; i < tl_hN && n < 96; ++i) if (tl_h[i].rbw && tl_h[i].written) g_hist[eye][n++] = tl_h[i].r;
    g_histN[eye] = n;
    if (eye != 0) return;
    // the shared ones: carried over in BOTH eyes' renders this frame
    int m = 0;
    for (int i = 0; i < g_histN[0] && m < 96; ++i)
        for (int j = 0; j < g_histN[1]; ++j) if (g_hist[0][i] == g_hist[1][j]) { g_cand[m++] = g_hist[0][i]; break; }
    if (m != g_candN) { for (int e = 0; e < 2; ++e) for (int i = 0; i < 96; ++i) if (g_bak[e][i]) { g_bak[e][i]->Release(); g_bak[e][i] = nullptr; } }
    g_candN = m;
    if (g_histLog > 0 && m)
    {
        --g_histLog;
        char b[3000]; int k = snprintf(b, sizeof(b), "HISTORY carried over in both eyes' renders: %d (left %d, right %d):", m, g_histN[0], g_histN[1]);
        for (int i = 0; i < m; ++i)
        {
            ID3D11Texture2D* t = nullptr; D3D11_TEXTURE2D_DESC d{}; ID3D11Buffer* bu = nullptr; D3D11_BUFFER_DESC bd{};
            if (SUCCEEDED(g_cand[i]->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t))) { t->GetDesc(&d); t->Release(); k += snprintf(b + k, sizeof(b) - k, " %ux%u/f%d", d.Width, d.Height, (int)d.Format); }
            else if (SUCCEEDED(g_cand[i]->QueryInterface(__uuidof(ID3D11Buffer), (void**)&bu))) { bu->GetDesc(&bd); bu->Release(); k += snprintf(b + k, sizeof(b) - k, " buf%u", bd.ByteWidth); }
            else k += snprintf(b + k, sizeof(b) - k, " other");
        }
        logf("%s", b);
    }
}
bool make_copy_of(ID3D11Resource* r, ID3D11Resource** out)
{
    ID3D11Texture2D* t = nullptr; ID3D11Buffer* bu = nullptr;
    if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)))
    { D3D11_TEXTURE2D_DESC d; t->GetDesc(&d); t->Release(); d.Usage = D3D11_USAGE_DEFAULT; d.CPUAccessFlags = 0; d.MiscFlags &= D3D11_RESOURCE_MISC_TEXTURECUBE;
      d.BindFlags = 0; ID3D11Texture2D* c = nullptr; if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &c))) return false; *out = c; return true; }
    if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Buffer), (void**)&bu)))
    { D3D11_BUFFER_DESC d; bu->GetDesc(&d); bu->Release(); d.Usage = D3D11_USAGE_DEFAULT; d.CPUAccessFlags = 0; d.BindFlags = 0;
      d.MiscFlags &= (D3D11_RESOURCE_MISC_BUFFER_STRUCTURED | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS); if (d.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS) d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      if (!(d.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS)) d.StructureByteStride = (d.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) ? d.StructureByteStride : 0;
      ID3D11Buffer* c = nullptr; if (FAILED(g_dev->CreateBuffer(&d, nullptr, &c))) return false; *out = c; return true; }
    return false;
}
// before eye E's render: its own history back in; after: saved (a fresh copy per eye; the first frame starts from the shared one)
void hist_swap(ID3D11DeviceContext* ctx, int eye, bool before)
{
    if (!g_histSwap || !g_dev || !ctx) return;
    for (int i = 0; i < g_candN; ++i)
    {
        ID3D11Resource* r = g_cand[i];
        if (!r) continue;
        if (before) { if (g_bak[eye][i]) { ctx->CopyResource(r, g_bak[eye][i]); InterlockedIncrement(&c_histCopies); } }
        else
        {
            if (!g_bak[eye][i] && !make_copy_of(r, &g_bak[eye][i])) { g_bak[eye][i] = nullptr; continue; }
            ctx->CopyResource(g_bak[eye][i], r); InterlockedIncrement(&c_histCopies);
        }
    }
}
// T1 trace: what writes full-size pictures inside an eye's render without a plain render target
using CopyResFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
using CopySubFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
using ResolveFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, ID3D11Resource*, UINT, DXGI_FORMAT);
using DispatchFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
using OMRTUAVFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
CopyResFn o_CopyRes = nullptr; CopySubFn o_CopySub = nullptr; ResolveFn o_Resolve = nullptr; DispatchFn o_Dispatch = nullptr; OMRTUAVFn o_OMRTUAV = nullptr;
volatile LONG g_traceWant = 0;   // bit per eye
thread_local bool tl_trace = false;
void note_big(ID3D11Resource* r)
{
    if (!tl_inEyeRender || !r || !g_capWant) return;   // PERF1: only a capture reads tl_big
    ID3D11Texture2D* t = nullptr; D3D11_TEXTURE2D_DESC d{};
    if (FAILED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t))) return;
    t->GetDesc(&d); t->Release();
    if (d.Width < 1000) return;
    bool seen = false; for (int i = 0; i < tl_bigN; ++i) seen |= tl_big[i] == r;
    if (!seen && tl_bigN < 16) tl_big[tl_bigN++] = r;
}
// M3b trace: each eye's render ENDS with a full-size copy into the window buffer -> that destination is the window buffer
void note_copy_dst(ID3D11Resource* r)
{
    if (!r) return;
    ID3D11Texture2D* t = nullptr; D3D11_TEXTURE2D_DESC d{};
    if (FAILED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t))) return;
    t->GetDesc(&d); t->Release();
    if (d.Width >= 640 && d.Height >= 640 && d.SampleDesc.Count == 1) tl_lastCopyDst = r;
}
void trace_line(const char* what, ID3D11Resource* dst, ID3D11Resource* src)
{
    if (!tl_trace) return;
    char a[96], b[96]; res_desc(dst, a, sizeof(a)); res_desc(src, b, sizeof(b));
    logf("  trace %s dst %s src %s (target sets so far %d)", what, a, b, tl_rtSets);
}
void STDMETHODCALLTYPE hk_CopyRes(ID3D11DeviceContext* c, ID3D11Resource* d, ID3D11Resource* sr)
{
    if (tl_inEyeRender) { h_read(sr); h_write(d); note_big(d); note_copy_dst(d); trace_line("CopyResource", d, sr); }
    else if (c == g_gameCtx && g_hudPhase && d && d == g_bb && hud_ready(c)) { InterlockedIncrement(&c_hudCopy); d = g_hudTex; }
    o_CopyRes(c, d, sr);
}
void STDMETHODCALLTYPE hk_CopySub(ID3D11DeviceContext* c, ID3D11Resource* d, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource* sr, UINT ss, const D3D11_BOX* bx)
{
    if (tl_inEyeRender) { h_read(sr); h_write(d); note_big(d); note_copy_dst(d); trace_line("CopySubresourceRegion", d, sr); }
    else if (c == g_gameCtx && g_hudPhase && d && d == g_bb && hud_ready(c)) { InterlockedIncrement(&c_hudCopy); d = g_hudTex; }
    o_CopySub(c, d, ds, x, y, z, sr, ss, bx);
}
void STDMETHODCALLTYPE hk_Resolve(ID3D11DeviceContext* c, ID3D11Resource* d, UINT ds, ID3D11Resource* sr, UINT ss, DXGI_FORMAT f) { if (tl_inEyeRender) { note_big(d); trace_line("ResolveSubresource", d, sr); } o_Resolve(c, d, ds, sr, ss, f); }
// 6a SHARED EXPOSURE (2026-10-07, 5c run 3 T1b: same camera both eyes, right eye 2.3 % darker): each eye's render
// computes its adapted luminance into 1x1 float targets (compute, R16_FLOAT seen in the M3b trace). Per eye we note
// them in write order; after the LEFT eye's render the right eye's ones get the left eye's values, so both eyes start
// every frame from one exposure (VR_ESSENTIALS STR-009; SWBF2VR shared EV).
ID3D11Resource* g_lum[2][6] = {}; int g_lumN[2] = {}; thread_local int tl_lumN = 0;
volatile LONG g_lumShare = 0;   // command "lumshare 0|1" - OFF: the 1x1 is a pooled per-frame target, not the history (6a run)
volatile LONG c_lumCopies = 0;
int g_lumLogs = 2;
void note_lum(ID3D11Resource* r)
{
    if (tl_eye_render < 0 || tl_lumN >= 6) return;
    ID3D11Texture2D* t = nullptr; D3D11_TEXTURE2D_DESC d{};
    if (FAILED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t))) return;
    t->GetDesc(&d); t->Release();
    if (d.Width != 1 || d.Height != 1) return;
    for (int i = 0; i < tl_lumN; ++i) if (g_lum[tl_eye_render][i] == r) return;
    g_lum[tl_eye_render][tl_lumN++] = r;
}
// RAINONCE 2026-10-07 (JJ's 6th Quest test: "the rain is not the same in each eye"): the rain is placed by compute
// shaders (geo-11 era RAINWRITER: a96594b16ceb399b simulation - keeps rain regions ahead of the camera via its forward,
// bc5c6aebf60c9308 splashes). With two engine views they may run once PER EYE (stepped twice, each from its own camera).
// Count them per eye (names from AKVR's shader table) and, with "rainonce", skip them in the second eye (left, eye 0:
// the clone renders first) so both eyes draw the same rain buffer.
using ShHashFn = unsigned long long(*)(const void*);
ShHashFn p_shHash = nullptr; bool p_shHashTried = false;
volatile LONG g_rainOnce = 1, c_rainCs[2][3] = {}, c_rainSkip = 0;
// RAINPAIR 2026-10-09 (JJ: "in the left eye, the rain has stopped moving"; log 02:54:06 on: after detective mode the
// one remaining rain step per frame was tagged left and skipped = no step at all). The step is recorded on worker
// threads, so its eye tag is a guess; instead the FIRST step of each stereo pair runs and later ones in the same pair
// are skipped (pair number advances when the right eye's render starts; FARRAIN2 gives the step the game camera, so
// which eye runs it does not matter). Steps outside the eye renders always run.
volatile LONG g_rainPair = 0, g_rainRan[2] = { -1, -1 };
// FARRAIN2 2026-10-08 (JJ: "part of the rain is connected to my head movements", as in the geo-11 version). geo-11 era
// RAINWRITER: the rain simulation gets cb0[11] = camera forward x 512 (head included) and keeps its far rain regions
// that far ahead along the view - they swing with head turns. Engine mode: give that one dispatch a copy of cb0 with
// the GAME camera's forward (AKVR's base axes, no head) in that row. RAINCB: dump cb0 to confirm the row.
volatile LONG g_rainPause = 0;   // RAINPAUSE 2026-10-08 (JJ: "an option to pause the rain, like the pause menu does")
volatile LONG g_nearRain = 1, c_nearPatched = 0;   // NEARRAIN2: also the camera position (head lean / height off)
volatile LONG g_farRain = 1, g_farRow = 11, g_rainCbDump = 0, c_farPatched = 0, c_farNoAxes = 0;
ID3D11Buffer* g_farCopy = nullptr; ID3D11Buffer* g_farVec = nullptr; UINT g_farCopySize = 0;
ID3D11Buffer* g_cbSnap = nullptr; UINT g_cbSnapSize = 0; volatile LONG g_cbSnapPending = 0;
ID3D11Buffer* g_cbSnapP = nullptr; volatile LONG g_cbSnapPWant = 0, g_cbSnapPHave = 0;   // the patched copy, same frame
float g_cbAxes[6] = {}; float g_cbBase[6] = {}; LONG g_cbBaseOk = 0;   // base forward, head-included forward at the dump
using AxesFn = int(*)(float*, float*);
AxesFn p_axes = nullptr;
bool akvr_axes(float base[3], float full[3])
{
    if (!p_axes) { HMODULE m = GetModuleHandleW(L"dinput8.dll"); if (m) p_axes = (AxesFn)GetProcAddress(m, "akvr_rain_axes"); }
    return p_axes && p_axes(base, full);
}
ID3D11Buffer* make_buf(ID3D11DeviceContext* c, UINT size, UINT bind)
{
    ID3D11Device* dev = nullptr; c->GetDevice(&dev); if (!dev) return nullptr;
    D3D11_BUFFER_DESC d{}; d.ByteWidth = size; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind;
    ID3D11Buffer* b = nullptr; dev->CreateBuffer(&d, nullptr, &b); dev->Release(); return b;
}
// RAINLIGHT 2026-10-14 (JJ: in the flat game the rain is brighter near bright lights - the moon, volumetric beams; in the headset
// "this brighter area is offset to where it should be, and ... a section of it is being masked by something large moving in and out").
// The simulation (a96594b16ceb399b) lights each drop by projecting it to the screen and sampling the eye's light picture (t2):
// clip = (pos - cb0[10]) x cb0[6..9]. NEARRAIN2 puts the head-free game camera in cb0[10] but rows 6..9 are still the EYE's
// view x lens, so every drop read the light picture at a spot shifted by the head / eye offset. Fix: row 9 += (C_hf - C_eye) x rows 6..8
// (C_eye = the original cb0[10]), computed on the graphics card by a 1-thread compute pass into the patched copy. "rainlight 0|1".
using DispFnD = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT); extern DispFnD od_Disp;   // (defined with the deferred hooks)
volatile LONG g_rainLight = 1, c_rainLight = 0; bool g_rlFailed = false;
// RAINLIGHT2 2026-10-14 (JJ on RAINRELIGHT: "Left and right eyes are not fusing for the bright rain" - own lighting per eye = different
// drop colours per eye). Fake 01:3x: each eye's own step put its bright rain off its own beam too (right eye: right of the beam, left
// eye: left of it) = the light picture t2 the step samples is the PREVIOUS render's = the OTHER eye's (left renders last). The step runs
// once (RAINONCE, in the right eye's render, first) and now samples t2 with the view x lens of the eye that drew it: the last published
// LEFT eye (g_mcPubVP[0], unjittered): cb0 rows 6..8 = its rows 0..2, row 9 = C x rows 0..2 + row 3 (C = the camera in row 10). One colour
// per drop for both eyes (fuses), lit where the light picture really is. "rainlight2 0|1".
volatile LONG g_rainLight2 = 1, c_rainLight2 = 0; ID3D11Buffer* g_rl2Up = nullptr;
// RAINLIGHT3 2026-10-14 (JJ on RAINLIGHT2: "fine at first, but then when you turn the camera towards the brighter part, the rain will
// disappear from that brighter area entirely ... connected to the camera somehow"): the view x lens was read when the step was RECORDED
// (worker threads), not when it runs - a different frame than the light picture while the camera / head moves. Now the left eye's
// view x lens goes into a GPU buffer on the render thread when its picture is finished (g_rl3VP, immediate context), and a 1-thread
// pass builds rows 6..9 from it when the step RUNS (same order as the picture). RAINLIGHT2's CPU copy stays as the fallback.
ID3D11Buffer* g_rl3VP = nullptr; ID3D11ComputeShader* g_rl3Cs = nullptr; ID3D11Buffer* g_rl3Out = nullptr; ID3D11UnorderedAccessView* g_rl3Uav = nullptr; bool g_rl3Failed = false; volatile LONG c_rl3 = 0, g_rl3Have = 0;
void rain_vp_publish(ID3D11DeviceContext* c, const M44& L)
{   // render thread, immediate context, right after the left eye's picture
    if (!g_rainLight2) return;
    if (!g_rl3VP) { ID3D11Device* dev = nullptr; c->GetDevice(&dev); if (!dev) return; D3D11_BUFFER_DESC d{ 64, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 }; dev->CreateBuffer(&d, nullptr, &g_rl3VP); dev->Release(); if (!g_rl3VP) return; }
    c->UpdateSubresource(g_rl3VP, 0, nullptr, &L.m[0][0], 0, 0); g_rl3Have = 1;
}
bool rain_l3_setup(ID3D11DeviceContext* c)
{
    if (g_rl3Cs) return true; if (g_rl3Failed) return false;
    ID3D11Device* dev = nullptr; c->GetDevice(&dev); if (!dev) return false;
    static const char cs[] =
        "cbuffer V : register(b0) { float4 m[4]; };\n"
        "cbuffer H : register(b1) { float4 hf; };\n"
        "RWBuffer<uint> R : register(u0);\n"
        "[numthreads(1, 1, 1)] void main() {\n"
        "  float4 r3 = hf.x * m[0] + hf.y * m[1] + hf.z * m[2] + m[3];\n"
        "  [unroll] for (uint k = 0; k < 3; ++k) { R[k * 4] = asuint(m[k].x); R[k * 4 + 1] = asuint(m[k].y); R[k * 4 + 2] = asuint(m[k].z); R[k * 4 + 3] = asuint(m[k].w); }\n"
        "  R[12] = asuint(r3.x); R[13] = asuint(r3.y); R[14] = asuint(r3.z); R[15] = asuint(r3.w);\n"
        "}\n";
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    CompileFn compile = m ? (CompileFn)GetProcAddress(m, "D3DCompile") : nullptr;
    ID3DBlob* b = nullptr; ID3DBlob* err = nullptr;
    bool ok = compile && SUCCEEDED(compile(cs, sizeof(cs) - 1, "akn_rainlight3", nullptr, nullptr, "main", "cs_5_0", 0, 0, &b, &err)) && b &&
              SUCCEEDED(dev->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_rl3Cs));
    if (b) b->Release();
    if (err) { logf("RAINLIGHT3: shader: %s", (const char*)err->GetBufferPointer()); err->Release(); }
    D3D11_BUFFER_DESC od{ 64, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, 0, 0 };
    ok = ok && SUCCEEDED(dev->CreateBuffer(&od, nullptr, &g_rl3Out));
    if (ok) { D3D11_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.Format = DXGI_FORMAT_R32_UINT; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = 16;
              ok = SUCCEEDED(dev->CreateUnorderedAccessView(g_rl3Out, &ud, &g_rl3Uav)); }
    dev->Release();
    if (!ok) { g_rl3Failed = true; if (g_rl3Cs) { g_rl3Cs->Release(); g_rl3Cs = nullptr; } logf("RAINLIGHT3: setup FAILED - the recorded copy is used"); return false; }
    logf("RAINLIGHT3: ready");
    return true;
}
// RAINRELIGHT 2026-10-14 (JJ: the bright rain round the moon / light beams is "offset to where it should be" with "something large moving
// in and out"; fake 01:3x: with the step run in each eye the left eye's bright rain moved back into the beam). The simulation lights each
// drop from the light picture of the eye that runs it; RAINONCE ran it in the right eye only, so the left eye showed the right eye's
// lighting (~14 deg off, Batman from the right eye's view cutting through it). The left eye now runs the step again with the time step
// cb0[13].x = 0 (positions, age and wrap unchanged; cb0[13].y, the streak length, kept) = same drops, its own lighting. "rainrelight 0|1".
volatile LONG g_rainRelight = 0, c_rainRelit = 0, c_rainRelightSkip = 0; thread_local bool tl_rainRelight = false; ID3D11Buffer* g_rlZero = nullptr;
ID3D11ComputeShader* g_rlCs = nullptr; ID3D11Buffer* g_rlCb = nullptr; ID3D11Buffer* g_rlOut = nullptr; ID3D11UnorderedAccessView* g_rlUav = nullptr;
bool rain_light_setup(ID3D11DeviceContext* c)
{
    if (g_rlCs) return true; if (g_rlFailed) return false;
    ID3D11Device* dev = nullptr; c->GetDevice(&dev); if (!dev) return false;
    static const char cs[] =
        "cbuffer O : register(b0) { float4 r[11]; };\n"
        "cbuffer H : register(b1) { float4 hf; };\n"
        "RWBuffer<uint> R : register(u0);\n"
        "[numthreads(1, 1, 1)] void main() {\n"
        "  float3 d = hf.xyz - r[10].xyz;\n"
        "  float4 o = r[9] + d.x * r[6] + d.y * r[7] + d.z * r[8];\n"
        "  R[0] = asuint(o.x); R[1] = asuint(o.y); R[2] = asuint(o.z); R[3] = asuint(o.w);\n"
        "}\n";
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    CompileFn compile = m ? (CompileFn)GetProcAddress(m, "D3DCompile") : nullptr;
    ID3DBlob* b = nullptr; ID3DBlob* err = nullptr;
    bool ok = compile && SUCCEEDED(compile(cs, sizeof(cs) - 1, "akn_rainlight", nullptr, nullptr, "main", "cs_5_0", 0, 0, &b, &err)) && b &&
              SUCCEEDED(dev->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_rlCs));
    if (b) b->Release();
    if (err) { logf("RAINLIGHT: shader: %s", (const char*)err->GetBufferPointer()); err->Release(); }
    D3D11_BUFFER_DESC cd{ 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    D3D11_BUFFER_DESC od{ 16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, 0, 0 };
    ok = ok && SUCCEEDED(dev->CreateBuffer(&cd, nullptr, &g_rlCb)) && SUCCEEDED(dev->CreateBuffer(&od, nullptr, &g_rlOut));
    if (ok) { D3D11_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.Format = DXGI_FORMAT_R32_UINT; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = 4;
              ok = SUCCEEDED(dev->CreateUnorderedAccessView(g_rlOut, &ud, &g_rlUav)); }
    dev->Release();
    if (!ok) { g_rlFailed = true; if (g_rlCs) { g_rlCs->Release(); g_rlCs = nullptr; } logf("RAINLIGHT: setup FAILED - rain lighting left as it was"); return false; }
    logf("RAINLIGHT: ready");
    return true;
}
// the patched copy's row 9, from the game's original cb0 (b0) and the head-free camera position; the game's compute state is put back
void rain_light_patch(ID3D11DeviceContext* c, ID3D11Buffer* orig, const float pos[3])
{
    if (!g_rainLight || !od_Disp || !rain_light_setup(c)) return;
    const float hf[4] = { pos[0], pos[1], pos[2], 1.0f };
    c->UpdateSubresource(g_rlCb, 0, nullptr, hf, 0, 0);
    ID3D11ComputeShader* oCs = nullptr; ID3D11ClassInstance* oCi[8] = {}; UINT oCiN = 8; c->CSGetShader(&oCs, oCi, &oCiN);
    ID3D11Buffer* oCb[2] = {}; c->CSGetConstantBuffers(0, 2, oCb);
    ID3D11UnorderedAccessView* oUav = nullptr; c->CSGetUnorderedAccessViews(0, 1, &oUav);
    ID3D11Buffer* cbs[2] = { orig, g_rlCb }; const UINT keep = (UINT)-1;
    c->CSSetShader(g_rlCs, nullptr, 0); c->CSSetConstantBuffers(0, 2, cbs); c->CSSetUnorderedAccessViews(0, 1, &g_rlUav, &keep);
    od_Disp(c, 1, 1, 1);
    c->CSSetShader(oCs, oCi, oCiN); c->CSSetConstantBuffers(0, 2, oCb); c->CSSetUnorderedAccessViews(0, 1, &oUav, &keep);
    if (oCs) oCs->Release(); for (UINT i = 0; i < oCiN && i < 8; ++i) if (oCi[i]) oCi[i]->Release(); for (auto* x : oCb) if (x) x->Release(); if (oUav) oUav->Release();
    const D3D11_BOX bx{ 0, 0, 0, 16, 1, 1 };
    c->CopySubresourceRegion(g_farCopy, 0, 9 * 16, 0, 0, g_rlOut, 0, &bx);
    InterlockedIncrement(&c_rainLight);
}
// before the rain simulation dispatch: returns the original cb0 (to put back) or null
ID3D11Buffer* rain_cb_pre(ID3D11DeviceContext* c)
{
    if (!g_farRain && g_rainCbDump <= 0) return nullptr;
    ID3D11Buffer* cb = nullptr; c->CSGetConstantBuffers(0, 1, &cb);
    if (!cb) return nullptr;
    D3D11_BUFFER_DESC d{}; cb->GetDesc(&d);
    if (g_rainCbDump > 0 && !g_cbSnapPending)
    {   // record a copy; the render thread reads it back after the eye render
        if (!g_cbSnap || g_cbSnapSize != d.ByteWidth) { if (g_cbSnap) g_cbSnap->Release(); g_cbSnap = make_buf(c, d.ByteWidth, 0); g_cbSnapSize = d.ByteWidth; }
        if (g_cbSnap) { g_cbSnapPWant = 1; g_cbSnapPHave = 0; c->CopyResource(g_cbSnap, cb); (void)akvr_axes(g_cbAxes, g_cbAxes + 3); for (int k = 0; k < 6; ++k) g_cbBase[k] = g_rendBase[k]; g_cbBaseOk = g_rendBaseOk; g_cbSnapPending = 1; InterlockedDecrement(&g_rainCbDump); }
    }
    if (!g_farRain || g_farRow < 0 || (UINT)(g_farRow * 16 + 12) > d.ByteWidth) { cb->Release(); return nullptr; }
    float base[3], full[3], pos[3] = {};
    const bool frameBase = g_rendBaseOk != 0;   // NEARRAIN2: the rendered frame's own game camera (exact frame)
    if (frameBase) { for (int k = 0; k < 3; ++k) { pos[k] = g_rendBase[k]; base[k] = g_rendBase[3 + k]; } }
    else if (!akvr_axes(base, full)) { InterlockedIncrement(&c_farNoAxes); cb->Release(); return nullptr; }
    if (!g_farCopy || g_farCopySize != d.ByteWidth) { if (g_farCopy) g_farCopy->Release(); g_farCopy = make_buf(c, d.ByteWidth, D3D11_BIND_CONSTANT_BUFFER); g_farCopySize = d.ByteWidth; }
    if (!g_farVec) g_farVec = make_buf(c, 32, 0);
    if (!g_farCopy || !g_farVec) { cb->Release(); return nullptr; }
    // run 33 (RAINCB, head yaw 20 / pitch 15): cb0[10] = camera position (w 1), cb0[11] = camera forward x 512 (w 0),
    // both with the head. Row 11 always (far regions); row 10 too with nearrain (the block around the camera)
    const float v[8] = { pos[0], pos[1], pos[2], 1.0f, base[0] * 512.0f, base[1] * 512.0f, base[2] * 512.0f, 0.0f };
    c->UpdateSubresource(g_farVec, 0, nullptr, v, 0, 0);
    c->CopyResource(g_farCopy, cb);
    const bool nearToo = g_nearRain && frameBase && g_farRow == 11;
    D3D11_BOX bx{ nearToo ? 0u : 16u, 0, 0, 28, 1, 1 };   // row 10 xyzw (+ row 11 xyz), or row 11 xyz only
    c->CopySubresourceRegion(g_farCopy, 0, (UINT)g_farRow * 16 - (nearToo ? 16 : 0), 0, 0, g_farVec, 0, &bx);
    if (nearToo)
    {
        InterlockedIncrement(&c_nearPatched);
        bool done2 = false;
        if (g_rainLight2 && g_rl3Have && g_rl3VP && !tl_rainRelight && d.ByteWidth >= 10 * 16 && od_Disp && rain_l3_setup(c) && rain_light_setup(c))
        {   // RAINLIGHT3: rows 6..9 built on the graphics card when the step runs, from the left eye's view x lens of the picture it reads
            const float hf[4] = { pos[0], pos[1], pos[2], 1.0f };
            c->UpdateSubresource(g_rlCb, 0, nullptr, hf, 0, 0);
            ID3D11ComputeShader* oCs = nullptr; ID3D11ClassInstance* oCi[8] = {}; UINT oCiN = 8; c->CSGetShader(&oCs, oCi, &oCiN);
            ID3D11Buffer* oCb[2] = {}; c->CSGetConstantBuffers(0, 2, oCb);
            ID3D11UnorderedAccessView* oUav = nullptr; c->CSGetUnorderedAccessViews(0, 1, &oUav);
            ID3D11Buffer* cbs[2] = { g_rl3VP, g_rlCb }; const UINT keep = (UINT)-1;
            c->CSSetShader(g_rl3Cs, nullptr, 0); c->CSSetConstantBuffers(0, 2, cbs); c->CSSetUnorderedAccessViews(0, 1, &g_rl3Uav, &keep);
            od_Disp(c, 1, 1, 1);
            c->CSSetShader(oCs, oCi, oCiN); c->CSSetConstantBuffers(0, 2, oCb); c->CSSetUnorderedAccessViews(0, 1, &oUav, &keep);
            if (oCs) oCs->Release(); for (UINT i = 0; i < oCiN && i < 8; ++i) if (oCi[i]) oCi[i]->Release(); for (auto* x : oCb) if (x) x->Release(); if (oUav) oUav->Release();
            const D3D11_BOX b64{ 0, 0, 0, 64, 1, 1 }; c->CopySubresourceRegion(g_farCopy, 0, 6 * 16, 0, 0, g_rl3Out, 0, &b64);
            done2 = true; InterlockedIncrement(&c_rl3); InterlockedIncrement(&c_rainLight2);
        }
        if (!done2 && g_rainLight2 && g_mcPubOk && !tl_rainRelight && d.ByteWidth >= 10 * 16)
        {   // RAINLIGHT2: the left eye's view x lens (it drew the light picture this step reads)
            M44 L; AcquireSRWLockShared(&g_mcLock); L = g_mcPubVP[0]; ReleaseSRWLockShared(&g_mcLock);
            float r[16];
            for (int k = 0; k < 3; ++k) for (int j = 0; j < 4; ++j) r[k * 4 + j] = L.m[k][j];
            for (int j = 0; j < 4; ++j) r[12 + j] = pos[0] * L.m[0][j] + pos[1] * L.m[1][j] + pos[2] * L.m[2][j] + L.m[3][j];
            if (!g_rl2Up) g_rl2Up = make_buf(c, 64, 0);
            if (g_rl2Up) { c->UpdateSubresource(g_rl2Up, 0, nullptr, r, 0, 0); const D3D11_BOX b64{ 0, 0, 0, 64, 1, 1 }; c->CopySubresourceRegion(g_farCopy, 0, 6 * 16, 0, 0, g_rl2Up, 0, &b64); done2 = true; InterlockedIncrement(&c_rainLight2); }
        }
        if (!done2) rain_light_patch(c, cb, pos);   // RAINLIGHT
    }
    if (tl_rainRelight)
    {   // RAINRELIGHT: time step 0 for this run
        if (!g_rlZero) g_rlZero = make_buf(c, 16, 0);
        if (g_rlZero && d.ByteWidth >= 14 * 16) { const D3D11_BOX zb{ 0, 0, 0, 4, 1, 1 }; c->CopySubresourceRegion(g_farCopy, 0, 13 * 16, 0, 0, g_rlZero, 0, &zb); InterlockedIncrement(&c_rainRelit); }
        else { tl_rainRelight = false; c->CSSetConstantBuffers(0, 1, &cb); cb->Release(); InterlockedIncrement(&c_rainRelightSkip); return (ID3D11Buffer*)1; }   // cannot hold the drops: skip
        tl_rainRelight = false;
    }
    if (g_cbSnapPWant)
    {
        if (!g_cbSnapP) g_cbSnapP = make_buf(c, d.ByteWidth, 0);
        if (g_cbSnapP) { c->CopyResource(g_cbSnapP, g_farCopy); g_cbSnapPHave = 1; }
        g_cbSnapPWant = 0;
    }
    c->CSSetConstantBuffers(0, 1, &g_farCopy);
    InterlockedIncrement(&c_farPatched);
    return cb;   // released by rain_cb_post
}
void rain_cb_post(ID3D11DeviceContext* c, ID3D11Buffer* cb) { if (cb) { c->CSSetConstantBuffers(0, 1, &cb); cb->Release(); } }
UINT read_buf(ID3D11DeviceContext* c, ID3D11Buffer* b, std::vector<uint8_t>& out);   // RAINDUMP, below
void rain_cb_readback(ID3D11DeviceContext* c)
{   // render thread, after an eye render: the recorded copy has executed
    if (!g_cbSnapPending || !g_cbSnap) return;
    std::vector<uint8_t> v;
    const UINT n = read_buf(c, g_cbSnap, v);
    g_cbSnapPending = 0;
    if (!n) { logf("RAINCB: readback failed"); return; }
    logf("RAINCB: rain simulation cb0 (%u bytes); game camera forward %.3f %.3f %.3f, with the head %.3f %.3f %.3f", n, g_cbAxes[0], g_cbAxes[1], g_cbAxes[2], g_cbAxes[3], g_cbAxes[4], g_cbAxes[5]);
    logf("RAINCB: this render's game camera without the head (%s): position %.1f %.1f %.1f, forward %.3f %.3f %.3f", g_cbBaseOk ? "ok" : "MISSING", g_cbBase[0], g_cbBase[1], g_cbBase[2], g_cbBase[3], g_cbBase[4], g_cbBase[5]);
    const float* f = (const float*)v.data();
    for (UINT r = 0; r < n / 16 && r < 32; ++r) logf("  cb0[%u] %g %g %g %g", r, f[r * 4], f[r * 4 + 1], f[r * 4 + 2], f[r * 4 + 3]);
    if (g_cbSnapPHave && g_cbSnapP)
    {
        std::vector<uint8_t> w; const UINT m = read_buf(c, g_cbSnapP, w); g_cbSnapPHave = 0;
        if (m >= 192) { const float* q = (const float*)w.data(); logf("RAINCB patched: cb0[10] %g %g %g %g | cb0[11] %g %g %g %g", q[40], q[41], q[42], q[43], q[44], q[45], q[46], q[47]); }
    }
}   // run 32: 1 = rain simulation once per frame (same-camera 0.009 vs 1.2)
// DEFERRED 2026-10-08: 23 vertex shaders only in the eye renders' draws (runs 29-31) and the rain compute step never
// seen = AK records most of its drawing on DEFERRED contexts (worker threads) and plays them back. Those contexts have
// their own function table: hook it too, and tag work by the eye render in progress (g_eyeNow, set by the render thread).
volatile LONG g_eyeNow = -1, c_defDraw = 0, c_defDisp = 0, c_execCL = 0;
thread_local bool tl_rainSim = false;
int where_now(bool deferred) { if (!deferred) return tl_inEyeRender && tl_eye_render >= 0 && tl_eye_render < 2 ? tl_eye_render : 2; const LONG e = g_eyeNow; return e == 0 || e == 1 ? (int)e : 2; }
bool rain_skip(ID3D11DeviceContext* c, bool deferred = false)
{
    if (!p_shHashTried) { HMODULE m = GetModuleHandleW(L"dinput8.dll"); p_shHash = m ? (ShHashFn)GetProcAddress(m, "akvr_shader_hash") : nullptr; p_shHashTried = m != nullptr; }
    if (!p_shHash || (!g_rainOnce && !g_farRain && !g_rainPause && akn_dlss_mode() <= 0 && g_rainCbDump <= 0 && g_rainDump <= 0 && g_vsInv <= 0)) return false;   // test switches only
    ID3D11ComputeShader* cs = nullptr; c->CSGetShader(&cs, nullptr, nullptr);
    const unsigned long long h = cs ? sh_hash_c(cs) : 0; if (cs) cs->Release();
    if (g_vsInv > 0 || g_rainDump > 0)
    {   // inventory: every distinct compute shader and where it runs (the first 80), so the rain step can be named
        static unsigned long long seen[400][2] = {}; static volatile LONG n = 0;
        const int wh0 = where_now(deferred); const int wh = wh0 == 2 ? -1 : wh0;
        bool known = false; const LONG cnt = n;
        for (LONG i = 0; i < cnt && i < 400; ++i) if (seen[i][0] == h && (long long)seen[i][1] == wh) { known = true; break; }
        if (!known && cnt < 400) { const LONG i = InterlockedIncrement(&n) - 1; if (i < 400) { seen[i][0] = h; seen[i][1] = (unsigned long long)(long long)wh; logf("compute shader %016llx (%s%s) runs in %s", h, cs ? "named" : "none bound", deferred ? ", recorded" : "", wh == 0 ? "the left eye's render" : wh == 1 ? "the right eye's render" : "no eye render"); } }
    }
    if (h == 0xed72bff815f35134ull && akn_dlss_mode() > 0)
    {   // DLSS D1: this eye's motion vectors (R16G16_FLOAT, render size) - found as this pass's input 1 (D0b)
        ID3D11ShaderResourceView* sv = nullptr; c->CSGetShaderResources(1, 1, &sv);
        if (sv) { ID3D11Resource* r = nullptr; sv->GetResource(&r); const int w = where_now(deferred); if (r && w < 2) akn_dlss_note_mv(w, r); if (r) r->Release(); sv->Release(); }
    }
    const int k = h == 0xa96594b16ceb399bull ? 0 : h == 0xbc5c6aebf60c9308ull ? 1 : -1;
    if (k < 0) return false;
    const int w = where_now(deferred);
    InterlockedIncrement(&c_rainCs[k][w]);
    if (w < 2 && (g_rainOnce & (1 << k)))
    {   // RAINPAIR
        const LONG pr = g_rainPair;
        if (InterlockedExchange(&g_rainRan[k], pr) == pr)
        {   // RAINRELIGHT 2026-10-14: the second eye runs the step again with time step 0 - drops stay put, lit from this eye's own view
            if (k == 0 && g_rainRelight && !g_rainPause && deferred) { tl_rainSim = true; tl_rainRelight = true; return false; }
            InterlockedIncrement(&c_rainSkip); return true;
        }
    }
    if (g_rainPause && k == 0) return true;   // RAINPAUSE: no simulation step at all - the rain hangs still
    if (k == 0) tl_rainSim = true;   // FARRAIN2: the caller patches cb0 around this dispatch
    return false;
}
using DispatchIndFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
DispatchIndFn o_DispatchInd = nullptr;
void STDMETHODCALLTYPE hk_DispatchInd(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o) { if (rain_skip(c)) return; o_DispatchInd(c, b, o); }
// deferred-context detours (own originals: a different function table)
using DispIndFnD = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using DrawFnD = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using DrawIdxFnD = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawIIFnD = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawIFnD = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using ExecFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using ClrFnD = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT*);
ClrFnD od_Clr = nullptr;
ClearDSVFn od_ClrDSV = nullptr;   // DETCALLOUT: deferred depth clears
void STDMETHODCALLTYPE hkd_ClrDSV(ID3D11DeviceContext* c, ID3D11DepthStencilView* d, UINT f, FLOAT z, UINT8 st) { if (g_eyeNow == 0 && d) scan_clear_copy(c, true, d, f); od_ClrDSV(c, d, f, z, st); }
void d0_clear(ID3D11DeviceContext* c, ID3D11RenderTargetView* v, bool deferred);   // DLSS D0, below
void STDMETHODCALLTYPE hkd_Clr(ID3D11DeviceContext* c, ID3D11RenderTargetView* v, const FLOAT* col) { d0_clear(c, v, true); od_Clr(c, v, col); }
using SrvFnD = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
using RtFnD = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using UavFnD = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
SrvFnD od_PSSRV = nullptr, od_VSSRV = nullptr, od_CSSRV = nullptr; RtFnD od_RT = nullptr; UavFnD od_CSUAV = nullptr;
void xe_srvs(ID3D11ShaderResourceView* const* v, UINT n, bool deferred); void xe_uavs(ID3D11UnorderedAccessView* const* u, UINT n, bool deferred);
void STDMETHODCALLTYPE hkd_PSSRV(ID3D11DeviceContext* c, UINT s0, UINT n, ID3D11ShaderResourceView* const* v) { xe_srvs(v, n, true); od_PSSRV(c, s0, n, v); }
void STDMETHODCALLTYPE hkd_VSSRV(ID3D11DeviceContext* c, UINT s0, UINT n, ID3D11ShaderResourceView* const* v) { xe_srvs(v, n, true); od_VSSRV(c, s0, n, v); }
void STDMETHODCALLTYPE hkd_CSSRV(ID3D11DeviceContext* c, UINT s0, UINT n, ID3D11ShaderResourceView* const* v) { xe_srvs(v, n, true); od_CSSRV(c, s0, n, v); }
extern ID3D11RenderTargetView* g_hudRTV;
void STDMETHODCALLTYPE hkd_RT(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* v, ID3D11DepthStencilView* d)
{
    if (g_vwRes) vw_rtvs(c, v, n, true);   // VOLWATCH
    xe_rtvs(v, n, d, true);
    if (g_eyeNow < 0 && n && n <= 8 && v && v[0] && g_bb && (g_hudStat > 0 || g_hudDef))
    {   // HUDTRACE: a recorded (deferred) window-buffer bind outside the eye renders - the immediate redirect never sees it
        ID3D11Resource* r = nullptr; v[0]->GetResource(&r);
        const bool isBB = r == g_bb; if (r) r->Release();
        if (isBB) InterlockedIncrement(&f_hudBbDef);
        if (isBB && g_hudDef && g_hudRTV)
        {
            ID3D11RenderTargetView* vv[8]; for (UINT i = 0; i < n; ++i) vv[i] = v[i];
            vv[0] = g_hudRTV; InterlockedIncrement(&f_hudDefRedir);
            od_RT(c, n, vv, d); return;
        }
    }
    od_RT(c, n, v, d);
}
void STDMETHODCALLTYPE hkd_CSUAV(ID3D11DeviceContext* c, UINT s0, UINT n, ID3D11UnorderedAccessView* const* u, const UINT* ic) { xe_uavs(u, n, true); if (g_vwRes) vw_uavs(c, u, n, true); od_CSUAV(c, s0, n, u, ic); }
DispFnD od_Disp = nullptr; DispIndFnD od_DispInd = nullptr; DrawFnD od_Draw = nullptr; DrawIdxFnD od_DrawIdx = nullptr; DrawIIFnD od_DrawII = nullptr; DrawIFnD od_DrawI = nullptr; ExecFn o_Exec = nullptr;
bool skip_vs_d(ID3D11DeviceContext* c);   // below
void rain_depth_post(ID3D11DeviceContext* c);   // RAINDEPTH, below
void STDMETHODCALLTYPE hkd_Disp(ID3D11DeviceContext* c, UINT x, UINT y, UINT z)
{
    ++c_defDisp; tl_rainSim = false; d0_dispatch(c, true);
    if (dlss_dispatch_replace(c, true)) return;   // DLSS D1
    if (rain_skip(c, true)) return;
    if (tl_rainSim) { tl_rainSim = false; const bool rl = tl_rainRelight; ID3D11Buffer* ob = rain_cb_pre(c); if (ob == (ID3D11Buffer*)1 || (rl && !ob)) { tl_rainRelight = false; InterlockedIncrement(&c_rainRelightSkip); return; } od_Disp(c, x, y, z); rain_cb_post(c, ob); return; }   // RAINRELIGHT
    od_Disp(c, x, y, z);
}
void STDMETHODCALLTYPE hkd_DispInd(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o)
{
    ++c_defDisp; tl_rainSim = false;
    if (rain_skip(c, true)) return;
    if (tl_rainSim) { tl_rainSim = false; const bool rl = tl_rainRelight; ID3D11Buffer* ob = rain_cb_pre(c); if (ob == (ID3D11Buffer*)1 || (rl && !ob)) { tl_rainRelight = false; InterlockedIncrement(&c_rainRelightSkip); return; } od_DispInd(c, b, o); rain_cb_post(c, ob); return; }   // RAINRELIGHT
    od_DispInd(c, b, o);
}
// RAINMASK 2026-10-14 (JJ: with DLSS off the bright rain in the light beam stays; with DLSS it fades away as the camera turns toward it):
// our DLSS motion vectors are camera-only, so the falling drops reproject wrongly and DLSS averages them out, worst on a bright background.
// The rain draw (kRainVs) is drawn a second time into a per-eye R8 mask (same target size, same depth test, a pixel shader returning 1),
// cleared at each eye render's start, and handed to DLSS as its "bias current colour" mask = trust this frame's pixels there. "rainmask 0|1".
volatile LONG g_rainMask = 1, c_rainMask = 0; thread_local bool tl_rainMaskNow = false; thread_local int tl_rainMaskEye = -1;
ID3D11Texture2D* g_rmTex[2] = {}; ID3D11RenderTargetView* g_rmRtv[2] = {}; ID3D11PixelShader* g_rmPs = nullptr; bool g_rmFailed = false; volatile LONG g_rmUsed[2] = {};
bool rain_mask_ready(ID3D11DeviceContext* c, int eye, UINT w, UINT h)
{
    if (eye < 0 || eye > 1 || g_rmFailed) return false;
    ID3D11Device* dev = nullptr; c->GetDevice(&dev); if (!dev) return false;
    bool ok = true;
    if (!g_rmPs)
    {
        static const char ps[] = "float4 main() : SV_Target { return float4(1, 1, 1, 1); }\n";
        using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll"); CompileFn compile = m ? (CompileFn)GetProcAddress(m, "D3DCompile") : nullptr;
        ID3DBlob* b = nullptr; ID3DBlob* err = nullptr;
        ok = compile && SUCCEEDED(compile(ps, sizeof(ps) - 1, "akn_rainmask", nullptr, nullptr, "main", "ps_5_0", 0, 0, &b, &err)) && b && SUCCEEDED(dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_rmPs));
        if (b) b->Release(); if (err) err->Release();
        if (!ok) { g_rmFailed = true; dev->Release(); logf("RAINMASK: shader FAILED - off"); return false; }
    }
    if (g_rmTex[eye]) { D3D11_TEXTURE2D_DESC d{}; g_rmTex[eye]->GetDesc(&d); if (d.Width != w || d.Height != h) { g_rmRtv[eye]->Release(); g_rmRtv[eye] = nullptr; g_rmTex[eye]->Release(); g_rmTex[eye] = nullptr; } }
    if (!g_rmTex[eye])
    {
        D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8_UNORM; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ok = SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &g_rmTex[eye])) && SUCCEEDED(dev->CreateRenderTargetView(g_rmTex[eye], nullptr, &g_rmRtv[eye]));
        if (ok) logf("RAINMASK: eye %d mask %ux%u", eye, w, h); else { g_rmFailed = true; logf("RAINMASK: mask FAILED - off"); }
    }
    dev->Release();
    return ok;
}
// render thread, immediate context, at each eye render's start
void rain_mask_clear(ID3D11DeviceContext* c, int eye)
{
    g_rmUsed[eye & 1] = 0;
    if (eye >= 0 && eye <= 1 && g_rmRtv[eye]) { const FLOAT z[4] = {}; c->ClearRenderTargetView(g_rmRtv[eye], z); }
}
// right after the rain draw, same context: the same draw into this eye's mask
template <class F> void rain_mask_draw(ID3D11DeviceContext* c, F draw)
{
    tl_rainMaskNow = false;
    const int eye = tl_rainMaskEye; if (!g_rainMask || eye < 0 || eye > 1 || akn_dlss_mode() <= 0) return;
    ID3D11RenderTargetView* rt[8] = {}; ID3D11DepthStencilView* ds = nullptr; c->OMGetRenderTargets(8, rt, &ds);
    if (rt[0])
    {
        ID3D11Resource* r = nullptr; rt[0]->GetResource(&r); D3D11_TEXTURE2D_DESC d{}; ID3D11Texture2D* t = nullptr;
        if (r && SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) && t) { t->GetDesc(&d); t->Release(); }
        if (r) r->Release();
        if (d.Width && rain_mask_ready(c, eye, d.Width, d.Height))
        {
            ID3D11PixelShader* ops = nullptr; ID3D11ClassInstance* oci[8] = {}; UINT ocn = 8; c->PSGetShader(&ops, oci, &ocn);
            ID3D11BlendState* obs = nullptr; FLOAT obf[4]; UINT om = 0; c->OMGetBlendState(&obs, obf, &om);
            c->OMSetRenderTargets(1, &g_rmRtv[eye], ds); c->PSSetShader(g_rmPs, nullptr, 0); c->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            draw();
            c->OMSetRenderTargets(8, rt, ds); c->PSSetShader(ops, oci, ocn); c->OMSetBlendState(obs, obf, om);
            if (ops) ops->Release(); for (UINT i = 0; i < ocn && i < 8; ++i) if (oci[i]) oci[i]->Release(); if (obs) obs->Release();
            g_rmUsed[eye] = 1; InterlockedIncrement(&c_rainMask);
        }
    }
    for (auto* x : rt) if (x) x->Release(); if (ds) ds->Release();
}
void STDMETHODCALLTYPE hkd_Draw(ID3D11DeviceContext* c, UINT n, UINT s0) { ++c_defDraw; d0_draw(c, true); if (dlss_draw_skip(c, true)) return; if (skip_vs_d(c)) return; od_Draw(c, n, s0); rain_depth_post(c); }
void STDMETHODCALLTYPE hkd_DrawIdx(ID3D11DeviceContext* c, UINT n, UINT s0, INT b0) { ++c_defDraw; d0_draw(c, true); if (dlss_draw_skip(c, true)) return; if (skip_vs_d(c)) return; od_DrawIdx(c, n, s0, b0); rain_depth_post(c); }
void STDMETHODCALLTYPE hkd_DrawII(ID3D11DeviceContext* c, UINT a, UINT n, UINT b, INT d, UINT e) { ++c_defDraw; if (skip_vs_d(c)) return; od_DrawII(c, a, n, b, d, e); if (tl_rainMaskNow) rain_mask_draw(c, [&] { od_DrawII(c, a, n, b, d, e); }); rain_depth_post(c); }
void STDMETHODCALLTYPE hkd_DrawI(ID3D11DeviceContext* c, UINT a, UINT n, UINT b, UINT e) { ++c_defDraw; if (skip_vs_d(c)) return; od_DrawI(c, a, n, b, e); if (tl_rainMaskNow) rain_mask_draw(c, [&] { od_DrawI(c, a, n, b, e); }); rain_depth_post(c); }
void STDMETHODCALLTYPE hk_Exec(ID3D11DeviceContext* c, ID3D11CommandList* l, BOOL r) { InterlockedIncrement(&c_execCL); o_Exec(c, l, r); }
// PANELTEST: open/close AKVR's VR panel from the test script (exactly as the double stick click), log its state
using PanelSetFn = void(*)(int); using PanelStateFn = unsigned long long(*)();
PanelSetFn p_panelSet = nullptr; PanelStateFn p_panelState = nullptr;
void panel_link() { if (!p_panelSet) { HMODULE m = GetModuleHandleW(L"dinput8.dll"); if (m) { p_panelSet = (PanelSetFn)GetProcAddress(m, "akvr_panel_set"); p_panelState = (PanelStateFn)GetProcAddress(m, "akvr_panel_state"); } } }
void akvr_panel_cmd(int on) { panel_link(); if (p_panelSet) p_panelSet(on); else logf("VR panel: AKVR export missing"); }
void smoke_light_pre(ID3D11DeviceContext* c); void smoke_light_post(ID3D11DeviceContext* c);   // SMOKELIGHT (below)
// VOLCB 2026-10-14 (JJ: in the flat game the smoke "stays consistently dark when you look up and down"; in VR it brightens looking up):
// the grid is rebuilt in each eye's render (VOLWATCH) - for WHICH camera? When the grid's compute pass runs (immediate context, output =
// the watched grid), its constant buffers 0..3 are read back once per eye and logged, beside this eye's view x lens, the head-free game
// camera x lens and both inverses, so the rows can be matched to a camera.
volatile LONG g_vcDumps = 0;
void vol_cb_dump(ID3D11DeviceContext* c)
{
    if (!g_vwRes || g_vcDumps >= 2 || GetTickCount64() > g_vwUntil) return;
    ID3D11UnorderedAccessView* uu[8] = {}; c->CSGetUnorderedAccessViews(0, 8, uu);
    bool hit = false;
    for (auto* u : uu) if (u) { ID3D11Resource* r = nullptr; u->GetResource(&r); if (r == g_vwRes) hit = true; if (r) r->Release(); u->Release(); }
    if (!hit) return;
    const int eye = tl_inEyeRender ? tl_eye_render : -1;
    InterlockedIncrement(&g_vcDumps);
    ID3D11Device* dev = nullptr; c->GetDevice(&dev); if (!dev) return;
    ID3D11Buffer* cbs[4] = {}; c->CSGetConstantBuffers(0, 4, cbs);
    logf("VOLCB: grid pass in eye %d - its constants follow", eye);
    for (int k = 0; k < 4; ++k)
    {
        if (!cbs[k]) continue;
        D3D11_BUFFER_DESC d{}; cbs[k]->GetDesc(&d);
        D3D11_BUFFER_DESC sd{ d.ByteWidth, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0 };
        ID3D11Buffer* st = nullptr;
        if (SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &st)) && st)
        {
            c->CopyResource(st, cbs[k]);
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(c->Map(st, 0, D3D11_MAP_READ, 0, &m)))
            {
                const float* f = (const float*)m.pData; const UINT rows = d.ByteWidth / 16;
                logf("VOLCB: cb%d %u bytes", k, d.ByteWidth);
                for (UINT i = 0; i < rows && i < 64; ++i) if (f[i * 4] || f[i * 4 + 1] || f[i * 4 + 2] || f[i * 4 + 3]) logf("VOLCB:   cb%d[%u] %.6g %.6g %.6g %.6g", k, i, f[i * 4], f[i * 4 + 1], f[i * 4 + 2], f[i * 4 + 3]);
                c->Unmap(st, 0);
            }
            st->Release();
        }
        cbs[k]->Release();
    }
    dev->Release();
    auto dumpM = [](const char* what, const M44& M) { for (int i = 0; i < 4; ++i) logf("VOLCB: %s row %d %.6g %.6g %.6g %.6g", what, i, M.m[i][0], M.m[i][1], M.m[i][2], M.m[i][3]); };
    dumpM("eye view", g_rendV); dumpM("eye lens", g_rendP);
    M44 vp = mul44(g_rendV, g_rendP); dumpM("eye view x lens", vp);
    M44 inv; if (inv44(vp, inv)) dumpM("eye view x lens INVERSE", inv);
    if (g_mcPubBvpOk) { dumpM("head-free camera x lens", g_mcPubBVP); M44 bi; if (inv44(g_mcPubBVP, bi)) dumpM("head-free camera x lens INVERSE", bi); }
}
void STDMETHODCALLTYPE hk_Dispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z)
{
    if (g_vwRes) vol_cb_dump(c);   // VOLCB
    d0_dispatch(c, false);
    if (dlss_dispatch_replace(c, false)) return;   // DLSS D1: SMAA neighbourhood -> copy
    if (rain_skip(c)) return;
    if (tl_inEyeRender && (h_on() || g_capWant || tl_trace || g_lumShare || akn_afw_on()))   // PERF1: was every dispatch
    {
        ID3D11UnorderedAccessView* u[4] = {}; c->CSGetUnorderedAccessViews(0, 4, u);
        for (int i = 0; i < 4; ++i) if (u[i])
        {
            ID3D11Resource* r = nullptr; u[i]->GetResource(&r);
            if (r) { h_rw(r); note_big(r); note_lum(r); if (tl_trace) { char a[96]; res_desc(r, a, sizeof(a)); logf("  trace Dispatch %ux%ux%u u%d %s", x, y, z, i, a); } r->Release(); }
            u[i]->Release();
        }
    }
    smoke_light_pre(c); o_Dispatch(c, x, y, z); smoke_light_post(c);
}
void STDMETHODCALLTYPE hk_OMRTUAV(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* v, ID3D11DepthStencilView* d, UINT us, UINT un, ID3D11UnorderedAccessView* const* u, const UINT* ic)
{
    if (tl_inEyeRender && (h_on() || g_capWant || tl_trace))   // PERF1
    {
        for (UINT i = 0; v && i < n && i < 8 && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL; ++i) if (v[i]) { ID3D11Resource* r = nullptr; v[i]->GetResource(&r); if (r) { note_big(r); if (tl_trace) trace_line("OMSetRTandUAV rt", r, nullptr); r->Release(); } }
        for (UINT i = 0; u && i < un && i < 8; ++i) if (u[i]) { ID3D11Resource* r = nullptr; u[i]->GetResource(&r); if (r) { h_rw(r); note_big(r); if (tl_trace) trace_line("OMSetRTandUAV uav", r, nullptr); r->Release(); } }
    }
    o_OMRTUAV(c, n, v, d, us, un, u, ic);
}
// RAINDUMP 2026-10-07 (JJ, 6th Quest test: "the rain is not the same in each eye"). Same camera + shared frame number in
// both eyes still gives different rain; sharing the view state does not help; the named rain compute steps never run
// (enhanced rain is off). So the rain draw's inputs change between the two eye renders. With "eyesame 1", any input of
// the rain draw (instance data, buffers, constants) that differs between the eyes is the culprit: snapshot the big
// instanced draws per eye (vertex buffers, vertex-shader buffers, VS + PS constants) and log what differs.
volatile LONG g_rainDump = 0, g_instInvN = 0;
struct DSnap { unsigned long long vs, ps; UINT inst; int indirect; unsigned long long h[24]; UINT sz[24]; float cb[12][512]; };
DSnap g_snap[2][12]; int g_snapN[2] = {};
unsigned long long fnv_buf(const uint8_t* p, size_t n) { unsigned long long h = 1469598103934665603ull; for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; } return h; }
UINT read_buf(ID3D11DeviceContext* c, ID3D11Buffer* b, std::vector<uint8_t>& out)
{
    out.clear();
    D3D11_BUFFER_DESC d{}; b->GetDesc(&d);
    if (!d.ByteWidth || d.ByteWidth > (16u << 20)) return 0;
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags &= D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    if (!(d.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED)) d.StructureByteStride = 0;
    ID3D11Device* dev = nullptr; c->GetDevice(&dev); if (!dev) return 0;
    ID3D11Buffer* st = nullptr; const HRESULT hr = dev->CreateBuffer(&d, nullptr, &st); dev->Release();
    if (FAILED(hr) || !st) return 0;
    o_CopyRes(c, st, b);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(c->Map(st, 0, D3D11_MAP_READ, 0, &m))) { out.assign((const uint8_t*)m.pData, (const uint8_t*)m.pData + d.ByteWidth); c->Unmap(st, 0); }
    st->Release();
    return (UINT)out.size();
}
void snap_buf(ID3D11DeviceContext* c, ID3D11Buffer* b, DSnap& s, int slot, int cbi)
{
    std::vector<uint8_t> v;
    const UINT n = read_buf(c, b, v);
    if (!n) { s.sz[slot] = 0xFFFFFFFF; return; }
    s.h[slot] = fnv_buf(v.data(), n); s.sz[slot] = n;
    if (cbi >= 0) memcpy(s.cb[cbi], v.data(), n < 2048 ? n : 2048);
}
volatile LONG c_instCalls[4] = {}, c_instInEye = 0, c_instBig = 0;
void any_draw(ID3D11DeviceContext* c, UINT count, UINT inst, int kind);
struct TopDraw { unsigned long long total, vs, ps; UINT count, inst; int kind; };
TopDraw g_top[2][30] = {};
void inst_draw(ID3D11DeviceContext* c, UINT inst, int indirect)
{
    return;   // RAINDUMP2: the dump goes through any_draw (every draw kind); the instanced-only probe found nothing (kept for reference)
    InterlockedIncrement(&c_instCalls[indirect ? 2 + (indirect - 1) : (inst ? 0 : 1)]);
    if (tl_inEyeRender) InterlockedIncrement(&c_instInEye);
    if (inst >= 256) InterlockedIncrement(&c_instBig);
    if (!p_shHash) { HMODULE m = GetModuleHandleW(L"dinput8.dll"); p_shHash = m ? (ShHashFn)GetProcAddress(m, "akvr_shader_hash") : nullptr; }
    if (!tl_inEyeRender || tl_eye_render < 0 || tl_eye_render > 1 || !p_shHash) return;
    if (!indirect && inst < 256) return;
    ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr; c->VSGetShader(&vs, nullptr, nullptr); c->PSGetShader(&ps, nullptr, nullptr);
    const unsigned long long hv = vs ? sh_hash_c(vs) : 0, hp = ps ? sh_hash_c(ps) : 0; if (vs) vs->Release(); if (ps) ps->Release();
    {   // inventory of the big instanced draws (first 150)
        static unsigned long long seen[150][3] = {};
        const LONG cnt = g_instInvN; bool known = false;
        for (LONG i = 0; i < cnt && i < 150; ++i) if (seen[i][0] == hv && seen[i][1] == hp && seen[i][2] == (unsigned long long)tl_eye_render) { known = true; break; }
        if (!known && cnt < 150) { const LONG i = InterlockedIncrement(&g_instInvN) - 1; if (i < 150) { seen[i][0] = hv; seen[i][1] = hp; seen[i][2] = tl_eye_render; logf("instanced draw vs %016llx ps %016llx instances %u%s in the %s eye's render", hv, hp, inst, indirect ? " (count from the GPU)" : "", tl_eye_render ? "right" : "left"); } }
    }
    if (g_rainDump <= 0) return;
    const bool rain = hv == 0xf50d1365e929b3a0ull || hp == 0x5d787946eda54077ull || inst >= 4096 || indirect;
    const int e = tl_eye_render;
    if (!rain || g_snapN[e] >= 12) return;
    DSnap& s = g_snap[e][g_snapN[e]++]; memset(&s, 0, sizeof(s)); s.vs = hv; s.ps = hp; s.inst = inst; s.indirect = indirect;
    ID3D11Buffer* vb[4] = {}; UINT st[4] = {}, of[4] = {}; c->IAGetVertexBuffers(0, 4, vb, st, of);
    for (int i = 0; i < 4; ++i) if (vb[i]) { snap_buf(c, vb[i], s, i, -1); vb[i]->Release(); }
    ID3D11ShaderResourceView* sr[8] = {}; c->VSGetShaderResources(0, 8, sr);
    for (int i = 0; i < 8; ++i) if (sr[i])
    {
        ID3D11Resource* r = nullptr; sr[i]->GetResource(&r);
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN; if (r) r->GetType(&dim);
        if (r && dim == D3D11_RESOURCE_DIMENSION_BUFFER) snap_buf(c, (ID3D11Buffer*)r, s, 4 + i, -1); else if (r) s.sz[4 + i] = 0xFFFFFFFE;   // texture: not compared
        if (r) r->Release();
        sr[i]->Release();
    }
    ID3D11Buffer* cb[8] = {}; c->VSGetConstantBuffers(0, 8, cb);
    for (int i = 0; i < 8; ++i) if (cb[i]) { snap_buf(c, cb[i], s, 12 + i, i); cb[i]->Release(); }
    ID3D11Buffer* pc[4] = {}; c->PSGetConstantBuffers(0, 4, pc);
    for (int i = 0; i < 4; ++i) if (pc[i]) { snap_buf(c, pc[i], s, 20 + i, 8 + i); pc[i]->Release(); }
}
// RAINDUMP2: the rain draw is NOT a big instanced draw here (0 draws with 256+ instances in 150k instanced draws) - while
// dumping, every draw kind is checked: the rain's own shaders (geo-11 era names) or a very large vertex count
void any_draw(ID3D11DeviceContext* c, UINT count, UINT inst, int kind)
{
    if (g_rainDump <= 0 || !tl_inEyeRender || tl_eye_render < 0 || tl_eye_render > 1 || !p_shHash) return;
    ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr; c->VSGetShader(&vs, nullptr, nullptr); c->PSGetShader(&ps, nullptr, nullptr);
    const unsigned long long hv = vs ? sh_hash_c(vs) : 0, hp = ps ? sh_hash_c(ps) : 0; if (vs) vs->Release(); if (ps) ps->Release();
    const unsigned long long total = (unsigned long long)count * (inst ? inst : 1);
    {   // the 30 biggest draws per eye in the dump frames (a map, in case the rain's names changed)
        TopDraw* t = g_top[tl_eye_render]; int m = 0;
        for (int i = 1; i < 30; ++i) if (t[i].total < t[m].total) m = i;
        if (total > t[m].total) t[m] = { total, hv, hp, count, inst, kind };
    }
    const bool rain = hv == 0xf50d1365e929b3a0ull || hp == 0x5d787946eda54077ull || total >= 60000;
    const int e = tl_eye_render;
    if (!rain || g_snapN[e] >= 12) return;
    static const char* kn[] = { "Draw", "DrawIndexed", "DrawInstanced", "DrawIndexedInstanced" };
    logf("RAINDUMP candidate in the %s eye: %s count %u instances %u vs %016llx ps %016llx", e ? "right" : "left", kn[kind & 3], count, inst, hv, hp);
    DSnap& s = g_snap[e][g_snapN[e]++]; memset(&s, 0, sizeof(s)); s.vs = hv; s.ps = hp; s.inst = inst ? inst : count; s.indirect = 0;
    ID3D11Buffer* vb[4] = {}; UINT st[4] = {}, of[4] = {}; c->IAGetVertexBuffers(0, 4, vb, st, of);
    for (int i = 0; i < 4; ++i) if (vb[i]) { snap_buf(c, vb[i], s, i, -1); vb[i]->Release(); }
    ID3D11ShaderResourceView* sr[8] = {}; c->VSGetShaderResources(0, 8, sr);
    for (int i = 0; i < 8; ++i) if (sr[i])
    {
        ID3D11Resource* r = nullptr; sr[i]->GetResource(&r);
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN; if (r) r->GetType(&dim);
        if (r && dim == D3D11_RESOURCE_DIMENSION_BUFFER) snap_buf(c, (ID3D11Buffer*)r, s, 4 + i, -1); else if (r) s.sz[4 + i] = 0xFFFFFFFE;
        if (r) r->Release();
        sr[i]->Release();
    }
    ID3D11Buffer* cb[8] = {}; c->VSGetConstantBuffers(0, 8, cb);
    for (int i = 0; i < 8; ++i) if (cb[i]) { snap_buf(c, cb[i], s, 12 + i, i); cb[i]->Release(); }
    ID3D11Buffer* pc[4] = {}; c->PSGetConstantBuffers(0, 4, pc);
    for (int i = 0; i < 4; ++i) if (pc[i]) { snap_buf(c, pc[i], s, 20 + i, 8 + i); pc[i]->Release(); }
}
void rain_dump_end(int eye)
{   // after the LEFT eye's render (the second): compare with the right eye's snapshots
    if (g_rainDump <= 0 || eye != 0) return;
    static const char* nm[24] = { "vertex buffer 0", "vertex buffer 1", "vertex buffer 2", "vertex buffer 3", "VS buffer t0", "VS buffer t1", "VS buffer t2", "VS buffer t3",
        "VS buffer t4", "VS buffer t5", "VS buffer t6", "VS buffer t7", "VS constants b0", "VS constants b1", "VS constants b2", "VS constants b3", "VS constants b4",
        "VS constants b5", "VS constants b6", "VS constants b7", "PS constants b0", "PS constants b1", "PS constants b2", "PS constants b3" };
    const int n = g_snapN[0] < g_snapN[1] ? g_snapN[0] : g_snapN[1];
    logf("RAINDUMP frame: %d big draws in the right eye, %d in the left | instanced calls so far: %ld (+%ld zero) indirect %ld/%ld, in eye renders %ld, 256+ instances %ld", g_snapN[1], g_snapN[0], c_instCalls[0], c_instCalls[1], c_instCalls[2], c_instCalls[3], c_instInEye, c_instBig);
    for (int k = 0; k < n; ++k)
    {
        const DSnap& R = g_snap[1][k]; const DSnap& L = g_snap[0][k];
        char line[1400]; int o = _snprintf_s(line, sizeof(line), _TRUNCATE, "  draw %d vs %016llx ps %016llx inst %u/%u%s:", k, R.vs, R.ps, R.inst, L.inst, R.indirect ? " indirect" : "");
        if (R.vs != L.vs || R.ps != L.ps) o += _snprintf_s(line + o, sizeof(line) - o, _TRUNCATE, " DIFFERENT SHADERS (left vs %016llx)", L.vs);
        for (int i = 0; i < 24 && o > 0 && o < (int)sizeof(line) - 120; ++i)
        {
            if (!R.sz[i] && !L.sz[i]) continue;
            if (R.sz[i] >= 0xFFFFFFFE) continue;
            if (R.h[i] == L.h[i] && R.sz[i] == L.sz[i]) { o += _snprintf_s(line + o, sizeof(line) - o, _TRUNCATE, " %s same(%u)", nm[i], R.sz[i]); continue; }
            o += _snprintf_s(line + o, sizeof(line) - o, _TRUNCATE, " %s DIFFERS(%u/%u)", nm[i], R.sz[i], L.sz[i]);
        }
        logf("%s", line);
        for (int i = 12; i < 24; ++i)
        {
            if (!R.sz[i] || R.sz[i] >= 0xFFFFFFFE || R.h[i] == L.h[i]) continue;
            const int ci = i - 12; const UINT nf = (R.sz[i] < 2048 ? R.sz[i] : 2048) / 4;
            char d[1200]; int q = _snprintf_s(d, sizeof(d), _TRUNCATE, "    %s floats differing (index: right / left):", nm[i]); int shown = 0;
            for (UINT f = 0; f < nf && shown < 24 && q > 0 && q < (int)sizeof(d) - 60; ++f)
                if (R.cb[ci][f] != L.cb[ci][f]) { ++shown; q += _snprintf_s(d + q, sizeof(d) - q, _TRUNCATE, " c%u.%c %g / %g", f / 4, "xyzw"[f % 4], R.cb[ci][f], L.cb[ci][f]); }
            logf("%s", d);
        }
    }
    for (int e = 1; e >= 0; --e) for (int i = 0; i < 30; ++i) if (g_top[e][i].total)
        logf("  big draw %s eye: %llu vertices (%u x %u, kind %d) vs %016llx ps %016llx", e ? "right" : "left", g_top[e][i].total, g_top[e][i].count, g_top[e][i].inst, g_top[e][i].kind, g_top[e][i].vs, g_top[e][i].ps);
    memset(g_top, 0, sizeof(g_top));
    g_snapN[0] = g_snapN[1] = 0;
    InterlockedDecrement(&g_rainDump);
}
using DrawIIFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawIFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using DrawIndFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
DrawIIFn o_DrawII = nullptr; DrawIFn o_DrawI = nullptr; DrawIndFn o_DrawIIInd = nullptr, o_DrawIInd = nullptr;
// SKIPVS 2026-10-07: hide every draw with one vertex shader in the eye renders (is the 103530-vertex draw the rain?)
volatile unsigned long long g_skipVs = 0, g_skipLo = 0, g_skipHi = 0;   // SKIPRANGE: bisection over shader names
volatile LONG g_vsInv = 0;   // VSINV: list every vertex shader drawn in the eye renders (one frame) to akns_inventory.txt
unsigned long long g_vsSet[4096]; volatile LONG g_vsN = 0;
void vs_inv_note(ID3D11DeviceContext* c, bool deferred = false)
{
    if (g_vsInv <= 0 || !p_shHash || (deferred ? g_eyeNow < 0 : !tl_inEyeRender)) return;
    ID3D11VertexShader* vs = nullptr; c->VSGetShader(&vs, nullptr, nullptr);
    unsigned long long h = vs ? sh_hash_c(vs) : 0; if (!h) h = (unsigned long long)(uintptr_t)vs; if (vs) vs->Release();   // unnamed: its address
    if (!h) return;
    const LONG n = g_vsN;
    for (LONG i = 0; i < n; ++i) if (g_vsSet[i] == h) return;
    if (n < 4096) { const LONG i = InterlockedIncrement(&g_vsN) - 1; if (i < 4096) g_vsSet[i] = h; }   // render + worker threads
}
void vs_inv_end(int eye)
{
    if (g_vsInv <= 0 || eye != 0) return;
    if (InterlockedDecrement(&g_vsInv) > 0) return;   // collect over several whole frames, write at the end
    wchar_t path[MAX_PATH]; GetModuleFileNameW(nullptr, path, MAX_PATH); wchar_t* sl = wcsrchr(path, L'\\'); if (sl) wcscpy_s(sl + 1, MAX_PATH - (sl + 1 - path), L"akn\\vs_inventory.txt");
    FILE* f = nullptr; _wfopen_s(&f, path, L"w");
    if (f) { for (LONG i = 0; i < g_vsN; ++i) fprintf(f, "%016llx\n", g_vsSet[i]); fclose(f); }
    logf("VSINV: %ld vertex shaders in this frame's eye renders -> akn\\vs_inventory.txt", g_vsN);
    g_vsN = 0;
}
bool skip_vs_any(ID3D11DeviceContext* c, bool deferred);
bool skip_vs(ID3D11DeviceContext* c) { return skip_vs_any(c, false); }
bool skip_vs_d(ID3D11DeviceContext* c) { return skip_vs_any(c, true); }
volatile LONG g_rainDepth = 1, c_rainDepth = 0, g_rainDepthLog = 2;
// PERFRING 2026-10-11 (PERFSWEEP: gloss 0.83 ms, rain depth 0.46 ms per frame): every patched draw wrote ONE copy buffer and
// ONE rows buffer per eye that the previous draw was still reading = the graphics card had to finish that draw first.
// 32 copy + rows buffers per eye, used in turn; the content of each draw is exactly as before.
constexpr int kCbRing = 32;
struct CbRing { ID3D11Buffer* b[kCbRing] = {}; ID3D11Buffer* rows[kCbRing] = {}; UINT size = 0, rowsSize = 0; volatile LONG next = 0; };
ID3D11Buffer* make_buf(ID3D11DeviceContext* c, UINT size, UINT bind);
bool ring_next(ID3D11DeviceContext* c, CbRing& r, UINT size, UINT rowsSize, ID3D11Buffer*& copy, ID3D11Buffer*& rows)
{
    if (r.size != size) { for (auto& x : r.b) { if (x) x->Release(); x = nullptr; } r.size = size; }
    r.rowsSize = rowsSize;
    const int i = (int)(InterlockedIncrement(&r.next) & (kCbRing - 1));
    if (!r.b[i]) r.b[i] = make_buf(c, size, D3D11_BIND_CONSTANT_BUFFER);
    if (!r.rows[i]) r.rows[i] = make_buf(c, rowsSize, 0);
    copy = r.b[i]; rows = r.rows[i];
    return copy && rows;
}
CbRing g_rdRing[2], g_glRing[2]; using LdFn = int(*)(const void*);
thread_local ID3D11Buffer* tl_rdOrig = nullptr;
// SUITGLOSS3 2026-10-09 (JJ chose it in the panel): AK's character pixel shaders take gloss / rim from the camera's AIM:
// they read the view buffer's direction rows cb1[11..13] but never its position row cb1[14] (AKVR flags them at creation,
// akvr_shader_camdir). With turned eye cameras each eye's aim differs, so the gloss differed per eye (and slid with the
// head in geo-11). For those draws only, PS b1 is a copy of the view buffer whose rows 11-13 come from the head-free game
// camera (MARKERBASE g_mkVbase): both eyes match and the gloss stays put on head turns. Command "gloss 0|1".
volatile LONG g_gloss = 1, c_glossDraws = 0;

thread_local ID3D11Buffer* tl_glOrig = nullptr;
using CamDirFn = int(*)(const void*); CamDirFn p_camDir = nullptr; CamDirFn p_lightDir = nullptr; volatile LONG c_lightDir = 0; bool p_camDirTried = false;   // LIGHTDIR
ID3D11Buffer* make_buf(ID3D11DeviceContext* c, UINT size, UINT bind);
// SMOKELIGHT 2026-10-14 (JJ: the smoke brightens looking up, darkens looking down - in the flat game it stays dark; VOLCB: the light grid
// (3D, ~84x92x64, R8G8B8A8) is built per eye with that eye's own view x lens - no camera mix-up; its view buffer also carries the camera's
// forward axis (cb1[24].xyz, w = view depth offset), a typical shortcut for light scattering = follows the HEAD in VR). Test switch, like
// SUITGLOSS3: the grid pass gets a cb1 copy whose row 24.xyz is the head-free game camera's forward. "smokelight 0|1" (default 0, panel A/B).
volatile LONG g_smokeLight = 1, c_smokeLight = 0; CbRing g_slRing; ID3D11Buffer* tl_slOrig = nullptr;   // 1 = LIGHTDIR (head-free light rows for flagged pixel shaders, default), 2 = + the forward-row experiment, 0 = off
bool smoke_grid_pass(ID3D11DeviceContext* c)
{
    ID3D11UnorderedAccessView* uu[8] = {}; c->CSGetUnorderedAccessViews(0, 8, uu);
    bool hit = false;
    for (auto* u : uu) if (u)
    {
        if (!hit) { ID3D11Resource* r = nullptr; u->GetResource(&r);
            ID3D11Texture3D* t3 = nullptr; if (r && SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture3D), (void**)&t3)) && t3) { D3D11_TEXTURE3D_DESC d{}; t3->GetDesc(&d); hit = d.Depth == 64 && d.Format == DXGI_FORMAT_R8G8B8A8_UNORM && d.Width < 400 && d.Height < 400; t3->Release(); }
            if (r) r->Release(); }
        u->Release();
    }
    return hit;
}
void smoke_light_pre(ID3D11DeviceContext* c)
{
    if (g_smokeLight < 2 || !tl_inEyeRender || !g_rendBaseOk || !smoke_grid_pass(c)) return;
    ID3D11Buffer* cb = nullptr; c->CSGetConstantBuffers(1, 1, &cb);
    if (!cb) return;
    D3D11_BUFFER_DESC d{}; cb->GetDesc(&d);
    if (d.ByteWidth < 25 * 16) { cb->Release(); return; }
    ID3D11Buffer* copy = nullptr; ID3D11Buffer* rbuf = nullptr;
    if (!ring_next(c, g_slRing, d.ByteWidth, 16, copy, rbuf)) { cb->Release(); return; }
    const float f[4] = { g_rendBase[3], g_rendBase[4], g_rendBase[5], 0.0f };
    c->UpdateSubresource(rbuf, 0, nullptr, f, 0, 0);
    c->CopyResource(copy, cb);
    const D3D11_BOX bx{ 0, 0, 0, 12, 1, 1 };   // xyz only - w (the depth offset) stays the game's
    c->CopySubresourceRegion(copy, 0, 24 * 16, 0, 0, rbuf, 0, &bx);
    c->CSSetConstantBuffers(1, 1, &copy);
    tl_slOrig = cb;
    InterlockedIncrement(&c_smokeLight);
}
void smoke_light_post(ID3D11DeviceContext* c) { if (tl_slOrig) { c->CSSetConstantBuffers(1, 1, &tl_slOrig); tl_slOrig->Release(); tl_slOrig = nullptr; } }
// SPRITEFRAME: the frame particle cards are built in (SPRITEAXES) and - LIGHTFRAME 2026-10-14 - their camera-relative lights are turned in.
// T[comp][axis], axes 0 right, 1 up, 2 forward (UE3 view order). spriteaxes 2 = upright (world vertical, level heading), 1 = head-free camera.
void sprite_frame(float T[3][3])
{
    for (int k = 0; k < 3; ++k) for (int a = 0; a < 3; ++a) T[k][a] = g_mkVbase.m[k][a];
    if (g_spriteAxes == 2 || g_spriteAxes == 3 || g_spriteAxes == 4 || g_spriteAxes == 5)
    {   // SPRITEUP: up = world vertical, right = the camera's right made level, forward = level heading
        const float upz = T[2][1] >= 0.0f ? 1.0f : -1.0f;
        float r[3] = { T[0][0], T[1][0], 0.0f }; float rl = sqrtf(r[0] * r[0] + r[1] * r[1]);
        if (rl > 1e-3f)
        {
            r[0] /= rl; r[1] /= rl;
            const float u[3] = { 0.0f, 0.0f, upz };
            float f[3] = { r[1] * u[2] - r[2] * u[1], r[2] * u[0] - r[0] * u[2], r[0] * u[1] - r[1] * u[0] };   // right x up
            if (f[0] * T[0][2] + f[1] * T[1][2] + f[2] * T[2][2] < 0.0f && f[0] * T[0][2] + f[1] * T[1][2] < 0.0f) { f[0] = -f[0]; f[1] = -f[1]; f[2] = -f[2]; }
            for (int k = 0; k < 3; ++k) { T[k][0] = r[k]; T[k][1] = u[k]; T[k][2] = f[k]; }
            if (g_spriteAxes == 4)
            {   // SPRITESTEEP 2026-10-14 (JJ: upright is the most stable but "very skewed" close up looking straight up; tilting with the stick
                // looks better but the colours come and go a little): upright up to 40 deg of camera pitch (stick, head-free), then tilting in
                // smoothly so the cards face you fully at straight up / down
                float cz = g_mkVbase.m[2][2]; if (cz > 1.0f) cz = 1.0f; if (cz < -1.0f) cz = -1.0f;
                const float a = asinf(cz), lim = 0.6981317f;   // 40 deg
                const float t = a > lim ? (a - lim) * (1.5707963f / (1.5707963f - lim)) : a < -lim ? (a + lim) * (1.5707963f / (1.5707963f - lim)) : 0.0f;
                if (t != 0.0f)
                {
                    const float st = sinf(t) * upz, ct = cosf(t);
                    float f2[3] = { f[0] * ct, f[1] * ct, f[2] * ct + st };
                    const float u2[3] = { f2[1] * r[2] - f2[2] * r[1], f2[2] * r[0] - f2[0] * r[2], f2[0] * r[1] - f2[1] * r[0] };   // forward x right
                    const float s2 = (u2[2] * upz >= 0.0f) ? 1.0f : -1.0f;
                    for (int k = 0; k < 3; ++k) { T[k][1] = u2[k] * s2; T[k][2] = f2[k]; }
                }
            }
            if (g_spriteAxes == 3 && g_rendVPok)
            {   // SPRITEPITCH 2026-10-14 (JJ: "turn on x-axis rotation on them again so that they face the camera when you look up at them"):
                // tilt about the level right axis to the elevation of where this eye looks (stick + head pitch; both eyes share it), no roll
                float ez = g_rendV.m[2][2]; if (ez > 0.995f) ez = 0.995f; if (ez < -0.995f) ez = -0.995f;   // eye forward, world z (UE3 view column 2 = forward)
                const float h = sqrtf(1.0f - ez * ez);
                float f2[3] = { f[0] * h, f[1] * h, f[2] * h + upz * ez };
                const float u2[3] = { f2[1] * r[2] - f2[2] * r[1], f2[2] * r[0] - f2[0] * r[2], f2[0] * r[1] - f2[1] * r[0] };   // forward x right
                float s2 = (u2[2] * upz >= 0.0f) ? 1.0f : -1.0f;
                for (int k = 0; k < 3; ++k) { T[k][1] = u2[k] * s2; T[k][2] = f2[k]; }
            }
        }
    }
}
void gloss_pre(ID3D11DeviceContext* c, int eye)
{
    if ((!g_gloss && g_smokeLight < 1) || eye < 0 || eye > 1 || !g_mkBaseOk) return;
    if (!p_camDirTried) { HMODULE m = GetModuleHandleW(L"dinput8.dll"); p_camDir = m ? (CamDirFn)GetProcAddress(m, "akvr_shader_camdir") : nullptr; p_lightDir = m ? (CamDirFn)GetProcAddress(m, "akvr_shader_lightdir") : nullptr; p_camDirTried = m != nullptr; }
    if (!p_camDir) return;
    ID3D11PixelShader* ps = nullptr; c->PSGetShader(&ps, nullptr, nullptr);
    const bool dir = g_gloss && ps && p_camDir(ps); const bool ld = g_smokeLight >= 1 && ps && p_lightDir && p_lightDir(ps); if (ps) ps->Release();   // LIGHTDIR
    if (!dir && !ld) return;
    ID3D11Buffer* cb = nullptr; c->PSGetConstantBuffers(1, 1, &cb);
    if (!cb) return;
    D3D11_BUFFER_DESC d{}; cb->GetDesc(&d);
    if (d.ByteWidth < 432) { cb->Release(); return; }   // rows 0..26 at least
    ID3D11Buffer* copy = nullptr; ID3D11Buffer* rbuf = nullptr;   // PERFRING
    if (!ring_next(c, g_glRing[eye], d.ByteWidth, 96, copy, rbuf)) { cb->Release(); return; }
    float rows[24];   // 0..11: rows 11..13 = world axis k in view space (the game's own layout, proven 2026-10-09); 12..23: rows 15..17 = the inverse
    for (int k = 0; k < 3; ++k) { for (int j = 0; j < 3; ++j) rows[k * 4 + j] = g_mkVbase.m[k][j]; rows[k * 4 + 3] = 0.0f; }
    // LIGHTFRAME 2026-10-14 (FXCB 03:40, 3 captures at 3 stick pitches: cb0[25] = (0, 0.5, 0) every time = the lights are fixed CAMERA-relative
    // directions, made for a card that faces the camera; with upright cards (SPRITEUP) they must turn with the CARD's frame, not the camera's)
    float LT[3][3]; sprite_frame(LT);
    if (g_lightLag && g_spriteAxes != 2) { const M44& Lv = g_mkVbaseHist[(g_mkHistHead - (g_lightLag & 3)) & 3]; for (int k = 0; k < 3; ++k) for (int a = 0; a < 3; ++a) LT[k][a] = Lv.m[k][a]; }   // LIGHTLAG (head-free cards only)
    for (int k = 0; k < 3; ++k) { for (int j = 0; j < 3; ++j) rows[12 + k * 4 + j] = LT[j][k]; rows[12 + k * 4 + 3] = 0.0f; }   // LIGHTDIR (cb1[15] = column 0 of rows 11..13, proven by VOLCB)
    c->UpdateSubresource(rbuf, 0, nullptr, rows, 0, 0);
    c->CopyResource(copy, cb);
    if (dir) { D3D11_BOX bx{ 0, 0, 0, 48, 1, 1 }; c->CopySubresourceRegion(copy, 0, 11 * 16, 0, 0, rbuf, 0, &bx); }
    if (ld && d.ByteWidth >= 18 * 16) { D3D11_BOX bx{ 48, 0, 0, 96, 1, 1 }; c->CopySubresourceRegion(copy, 0, 15 * 16, 0, 0, rbuf, 0, &bx); InterlockedIncrement(&c_lightDir); }
    c->PSSetConstantBuffers(1, 1, &copy);
    tl_glOrig = cb;   // put back after the draw (gloss_post)
    InterlockedIncrement(&c_glossDraws);
}
void gloss_post(ID3D11DeviceContext* c) { if (tl_glOrig) { c->PSSetConstantBuffers(1, 1, &tl_glOrig); tl_glOrig->Release(); tl_glOrig = nullptr; } }
ID3D11Buffer* make_buf(ID3D11DeviceContext* c, UINT size, UINT bind);
void rain_depth_pre(ID3D11DeviceContext* c, int eye)
{
    if (!g_rainDepth || eye < 0 || eye > 1 || !g_rendVPok || !g_frameOok) return;
    ID3D11Buffer* cb = nullptr; c->VSGetConstantBuffers(0, 1, &cb);
    if (!cb) return;
    D3D11_BUFFER_DESC d{}; cb->GetDesc(&d);
    if (d.ByteWidth < 160) { cb->Release(); return; }
    ID3D11Buffer* copy = nullptr; ID3D11Buffer* rbuf = nullptr;   // PERFRING
    if (!ring_next(c, g_rdRing[eye], d.ByteWidth, 64, copy, rbuf)) { cb->Release(); return; }
    // A = view x proj (doubles: the world translation is large), rows 0..2 as they are, row 3 = O x A(0..2) + A(3)
    double A[4][4];
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j)
    { double v = 0; for (int k = 0; k < 4; ++k) v += (double)g_rendV.m[i][k] * (double)g_rendP.m[k][j]; A[i][j] = v; }
    float rows[16];
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 4; ++j) rows[i * 4 + j] = (float)A[i][j];
    for (int j = 0; j < 4; ++j) rows[12 + j] = (float)(g_frameO[0] * A[0][j] + g_frameO[1] * A[1][j] + g_frameO[2] * A[2][j] + A[3][j]);
    if (g_rainDepthLog > 0 && InterlockedDecrement(&g_rainDepthLog) >= 0)
    {   // the game's own row 9 for an origin at THIS eye = (eyePos x A(0..2) + A(3)): must be ~(0, 0, near, 0)
        const M44& V = g_rendV; double e[3];
        for (int k = 0; k < 3; ++k) e[k] = -(V.m[3][0] * V.m[k][0] + V.m[3][1] * V.m[k][1] + V.m[3][2] * V.m[k][2]);
        logf("RAINDEPTH eye %d: rows 6-8 %g %g %g %g | %g %g %g %g | %g %g %g %g; row 9 with origin O %g %g %g %g (O %.1f %.1f %.1f); with origin = this eye %g %g %g %g",
             eye, rows[0], rows[1], rows[2], rows[3], rows[4], rows[5], rows[6], rows[7], rows[8], rows[9], rows[10], rows[11], rows[12], rows[13], rows[14], rows[15],
             g_frameO[0], g_frameO[1], g_frameO[2],
             e[0] * A[0][0] + e[1] * A[1][0] + e[2] * A[2][0] + A[3][0], e[0] * A[0][1] + e[1] * A[1][1] + e[2] * A[2][1] + A[3][1],
             e[0] * A[0][2] + e[1] * A[1][2] + e[2] * A[2][2] + A[3][2], e[0] * A[0][3] + e[1] * A[1][3] + e[2] * A[2][3] + A[3][3]);
    }
    c->UpdateSubresource(rbuf, 0, nullptr, rows, 0, 0);
    c->CopyResource(copy, cb);
    D3D11_BOX bx{ 0, 0, 0, 64, 1, 1 };
    c->CopySubresourceRegion(copy, 0, 96, 0, 0, rbuf, 0, &bx);
    c->VSSetConstantBuffers(0, 1, &copy);
    tl_rdOrig = cb;   // put back after the draw (rain_depth_post)
    InterlockedIncrement(&c_rainDepth);
}
// SPRITEAXES 2026-10-14 (JJ: the chimney smoke "sprites ... show up differently in the left and right eyes" and rotate when he rolls his
// head; playbook FAIL-STR-023 / ch14 #per-view-decisions: orient billboards once, for both eyes, from one reference view). AK's particle
// sprite vertex shaders spread each card along camera right / up from two cb0 rows (AKVR finds the rows per shader, akvr_vs_sprite_rows).
// Per draw those two rows are turned from this eye's camera frame into the head-free game camera's frame (Q = eye view x head-free
// view^T, 3x3, on the graphics card from the game's own values) in a copy of cb0: both eyes get the same card, no head roll. "spriteaxes 0|1".
volatile LONG c_charFace = 0; volatile LONG g_spriteAxes = 2, c_spriteAxes = 0; CbRing g_sp1Ring[2]; thread_local ID3D11Buffer* tl_sp1Orig = nullptr;   // SPRITEFACE: 5 (default) = each card upright, facing the head-free camera POSITION   // SPRITESTEEP: 4 (default) = upright, tilting in past 40 deg of camera pitch   // SPRITEPITCH: 3 (default) = tilt to where the eye looks, no roll   // SPRITEUP 2026-10-14 (JJ: "still tilting ... when you look up at it. It should just be completely facing outwards, perfectly vertical"): 2 = upright cards (world vertical + the camera's heading), 1 = the head-free camera's own axes   // JJ 2026-10-14: "I want always upright" (default 2)
CbRing g_spRing[2]; thread_local ID3D11Buffer* tl_spOrig = nullptr;
ID3D11ComputeShader* g_spCs = nullptr; ID3D11Buffer* g_spCb = nullptr; ID3D11Buffer* g_spOut = nullptr; ID3D11UnorderedAccessView* g_spUav = nullptr; bool g_spFailed = false;
using SpRowsFn = int(*)(const void*, int*); SpRowsFn p_spRows = nullptr; bool p_spRowsTried = false;
bool sprite_setup(ID3D11DeviceContext* c)
{
    if (g_spCs) return true; if (g_spFailed) return false;
    ID3D11Device* dev = nullptr; c->GetDevice(&dev); if (!dev) return false;
    static const char cs[] =
        "cbuffer O : register(b0) { float4 r[64]; };\n"
        "cbuffer Q : register(b1) { float4 q0; float4 q1; float4 q2; uint4 rows; };\n"
        "RWBuffer<uint> R : register(u0);\n"
        "float3 turn(float3 a) { return a.x * q0.xyz + a.y * q1.xyz + a.z * q2.xyz; }\n"
        "[numthreads(1, 1, 1)] void main() {\n"
        "  float4 a = r[rows.x], b = r[rows.y];\n"
        "  float3 ta = turn(a.xyz), tb = turn(b.xyz);\n"
        "  R[0] = asuint(ta.x); R[1] = asuint(ta.y); R[2] = asuint(ta.z); R[3] = asuint(a.w);\n"
        "  R[4] = asuint(tb.x); R[5] = asuint(tb.y); R[6] = asuint(tb.z); R[7] = asuint(b.w);\n"
        "}\n";
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll"); CompileFn compile = m ? (CompileFn)GetProcAddress(m, "D3DCompile") : nullptr;
    ID3DBlob* bl = nullptr; ID3DBlob* err = nullptr;
    bool ok = compile && SUCCEEDED(compile(cs, sizeof(cs) - 1, "akn_spriteaxes", nullptr, nullptr, "main", "cs_5_0", 0, 0, &bl, &err)) && bl &&
              SUCCEEDED(dev->CreateComputeShader(bl->GetBufferPointer(), bl->GetBufferSize(), nullptr, &g_spCs));
    if (bl) bl->Release();
    if (err) { logf("SPRITEAXES: shader: %s", (const char*)err->GetBufferPointer()); err->Release(); }
    D3D11_BUFFER_DESC cd{ 64, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    D3D11_BUFFER_DESC od{ 32, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, 0, 0 };
    ok = ok && SUCCEEDED(dev->CreateBuffer(&cd, nullptr, &g_spCb)) && SUCCEEDED(dev->CreateBuffer(&od, nullptr, &g_spOut));
    if (ok) { D3D11_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.Format = DXGI_FORMAT_R32_UINT; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = 8; ok = SUCCEEDED(dev->CreateUnorderedAccessView(g_spOut, &ud, &g_spUav)); }
    dev->Release();
    if (!ok) { g_spFailed = true; if (g_spCs) { g_spCs->Release(); g_spCs = nullptr; } logf("SPRITEAXES: setup FAILED - off"); return false; }
    logf("SPRITEAXES: ready");
    return true;
}
// VOLWATCH 2026-10-14 (JJ: the smoke "goes bright when you look up at it and dark when you look down" - only the smoke, before today's
// fixes too): the sprite shader takes light from a screen-space 3D grid (t1, sampled at this eye's screen spot + depth slice). If the pass
// that fills it works for another camera than the eye drawing the smoke, the smoke reads the wrong cells. Armed by the panel capture: the
// grid the next sprite draw reads is noted, and for 0.2 s every compute pass / draw that WRITES it is logged with its shader, eye and
// render, beside the eyes that read it.
ID3D11Resource* g_vwRes = nullptr; volatile LONG g_vwArm = 0, g_vwLines = 0; ULONGLONG g_vwUntil = 0;
void vw_log_write(ID3D11DeviceContext* c, ID3D11Resource* r, bool compute, bool deferred)
{
    if (!r || r != g_vwRes || GetTickCount64() > g_vwUntil || g_vwLines >= 80) return;
    unsigned long long h = 0;
    if (compute) { ID3D11ComputeShader* cs = nullptr; c->CSGetShader(&cs, nullptr, nullptr); if (cs) { h = sh_hash_c(cs); cs->Release(); } }
    else { ID3D11PixelShader* ps = nullptr; c->PSGetShader(&ps, nullptr, nullptr); if (ps) { h = sh_hash_c(ps); ps->Release(); } }
    const int eye = deferred ? (int)g_eyeNow : (tl_inEyeRender ? tl_eye_render : -1);
    InterlockedIncrement(&g_vwLines);
    logf("VOLWATCH: %s %016llx (bound now: %s) WRITES the smoke's light grid - eye %d (-1 = outside the eye renders), %s context, render stamp %llu",
         compute ? "compute shader" : "pixel shader", h, compute ? "as its output" : "as a target", eye, deferred ? "recorded" : "immediate", (unsigned long long)tl_renderStamp);
}
void vw_uavs(ID3D11DeviceContext* c, ID3D11UnorderedAccessView* const* u, UINT n, bool deferred)
{
    if (!g_vwRes || !u) return;
    for (UINT i = 0; i < n && i < 8; ++i) if (u[i]) { ID3D11Resource* r = nullptr; u[i]->GetResource(&r); if (r) { vw_log_write(c, r, true, deferred); r->Release(); } }
}
void vw_rtvs(ID3D11DeviceContext* c, ID3D11RenderTargetView* const* v, UINT n, bool deferred)
{
    if (!g_vwRes || !v) return;
    for (UINT i = 0; i < n && i < 8; ++i) if (v[i]) { ID3D11Resource* r = nullptr; v[i]->GetResource(&r); if (r) { vw_log_write(c, r, false, deferred); r->Release(); } }
}
void vw_note_reader(ID3D11DeviceContext* c, int eye)
{   // at a sprite draw (SPRITEAXES found its shader): which grid it reads
    if (!g_vwArm && !g_vwRes) return;
    ID3D11ShaderResourceView* sv = nullptr; c->VSGetShaderResources(1, 1, &sv);
    if (!sv) return;
    ID3D11Resource* r = nullptr; sv->GetResource(&r); sv->Release();
    if (!r) return;
    if (g_vwArm && !g_vwRes)
    {
        g_vwArm = 0; g_vwRes = r; r->AddRef(); g_vwUntil = GetTickCount64() + 200; g_vwLines = 0;
        D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim); UINT w = 0, h = 0, dd = 0; int fmt = 0;
        ID3D11Texture3D* t3 = nullptr; if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture3D), (void**)&t3)) && t3) { D3D11_TEXTURE3D_DESC d{}; t3->GetDesc(&d); w = d.Width; h = d.Height; dd = d.Depth; fmt = (int)d.Format; t3->Release(); }
        logf("VOLWATCH: the smoke reads light grid %p (%ux%ux%u format %d, kind %d) - watching its writers for 0.2 s", (void*)r, w, h, dd, fmt, (int)dim);
    }
    static LONGLONG s_last = -1;
    if (r == g_vwRes && GetTickCount64() <= g_vwUntil && (LONGLONG)tl_renderStamp * 2 + eye != s_last && g_vwLines < 80)
    { s_last = (LONGLONG)tl_renderStamp * 2 + eye; InterlockedIncrement(&g_vwLines); logf("VOLWATCH: smoke drawn READING the grid - eye %d, render stamp %llu", eye, (unsigned long long)tl_renderStamp); }
    r->Release();
    if (g_vwRes && GetTickCount64() > g_vwUntil + 1000) { g_vwRes->Release(); g_vwRes = nullptr; }   // done
}
void sprite_pre(ID3D11DeviceContext* c, int eye, bool deferred)
{
    if (!g_spriteAxes || eye < 0 || eye > 1 || !g_mkBaseOk || !g_rendVPok) return;
    if (!p_spRowsTried) { HMODULE m = GetModuleHandleW(L"dinput8.dll"); p_spRows = m ? (SpRowsFn)GetProcAddress(m, "akvr_vs_sprite_rows") : nullptr; p_spRowsTried = m != nullptr; }
    if (!p_spRows) return;
    ID3D11VertexShader* vs = nullptr; c->VSGetShader(&vs, nullptr, nullptr);
    int rows[3] = { -1, -1, -1 }; const bool sp = vs && p_spRows(vs, rows); if (vs) vs->Release();
    if (!sp || rows[0] < 0 || rows[1] < 0 || rows[0] > 63 || rows[1] > 63) return;
    vw_note_reader(c, eye);   // VOLWATCH
    ID3D11Buffer* cb = nullptr; c->VSGetConstantBuffers(0, 1, &cb);
    if (!cb) return;
    D3D11_BUFFER_DESC d{}; cb->GetDesc(&d);
    const UINT need = (UINT)((rows[0] > rows[1] ? rows[0] : rows[1]) + 1) * 16;
    if (d.ByteWidth < need || !sprite_setup(c)) { cb->Release(); return; }
    const UINT csWidth = d.ByteWidth < 1024 ? d.ByteWidth : 1024;   // the pass reads rows 0..63 at most
    (void)csWidth;
    ID3D11Buffer* copy = nullptr; ID3D11Buffer* rbuf = nullptr;
    if (!ring_next(c, g_spRing[eye], d.ByteWidth, 16, copy, rbuf)) { cb->Release(); return; }
    // Q (row-vector): world vector a -> a . Meye . Mhf^T (into the eye's view frame, back out through the head-free camera's)
    float T[3][3]; sprite_frame(T);   // SPRITEUP / SPRITEAXES: the card frame
    float Q[3][3];
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) { double v = 0; for (int k = 0; k < 3; ++k) v += (double)g_rendV.m[i][k] * (double)T[j][k]; Q[i][j] = (float)v; }
    struct { float q[3][4]; UINT rows[4]; } qc = {};
    for (int i = 0; i < 3; ++i) { for (int j = 0; j < 3; ++j) qc.q[i][j] = Q[i][j]; qc.q[i][3] = 0.0f; }
    qc.rows[0] = (UINT)rows[0]; qc.rows[1] = (UINT)rows[1];
    c->UpdateSubresource(g_spCb, 0, nullptr, &qc, 0, 0);
    ID3D11ComputeShader* oCs = nullptr; ID3D11ClassInstance* oCi[8] = {}; UINT oCiN = 8; c->CSGetShader(&oCs, oCi, &oCiN);
    ID3D11Buffer* oCb[2] = {}; c->CSGetConstantBuffers(0, 2, oCb);
    ID3D11UnorderedAccessView* oUav = nullptr; c->CSGetUnorderedAccessViews(0, 1, &oUav);
    ID3D11Buffer* cbs[2] = { cb, g_spCb }; const UINT keep = (UINT)-1;
    c->CSSetShader(g_spCs, nullptr, 0); c->CSSetConstantBuffers(0, 2, cbs); c->CSSetUnorderedAccessViews(0, 1, &g_spUav, &keep);
    if (deferred) { if (od_Disp) od_Disp(c, 1, 1, 1); } else c->Dispatch(1, 1, 1);
    c->CSSetShader(oCs, oCi, oCiN); c->CSSetConstantBuffers(0, 2, oCb); c->CSSetUnorderedAccessViews(0, 1, &oUav, &keep);
    if (oCs) oCs->Release(); for (UINT i = 0; i < oCiN && i < 8; ++i) if (oCi[i]) oCi[i]->Release(); for (auto* x : oCb) if (x) x->Release(); if (oUav) oUav->Release();
    c->CopyResource(copy, cb);
    const D3D11_BOX b0{ 0, 0, 0, 16, 1, 1 }, b1{ 16, 0, 0, 32, 1, 1 };
    c->CopySubresourceRegion(copy, 0, (UINT)rows[0] * 16, 0, 0, g_spOut, 0, &b0);
    c->CopySubresourceRegion(copy, 0, (UINT)rows[1] * 16, 0, 0, g_spOut, 0, &b1);
    if (g_spriteAxes == 5 && rows[2] > 0 && rows[2] < 64 && (UINT)(rows[2] + 1) * 16 <= d.ByteWidth && g_rendBaseOk)
    {   // SPRITEFACE 2026-10-14 (JJ: turning the camera with the stick swings every card about its vertical axis, "facing away from it"):
        // the shader's own mode W = upright cards that face the camera POSITION (cross with world up); weights (0, 0, 0, 1), and the view
        // buffer's camera point cb1[8] (translated world, the eye at 0) moved to the head-free camera: C_hf - C_eye -> both eyes alike
        ID3D11Buffer* wb = nullptr; ID3D11Buffer* wrow = nullptr;
        static CbRing s_wRing[2];
        if (ring_next(c, s_wRing[eye], 16, 16, wb, wrow))
        {
            const float wts[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            c->UpdateSubresource(wrow, 0, nullptr, wts, 0, 0);
            const D3D11_BOX b16{ 0, 0, 0, 16, 1, 1 };
            c->CopySubresourceRegion(copy, 0, (UINT)rows[2] * 16, 0, 0, wrow, 0, &b16);
        }
        ID3D11Buffer* cb1 = nullptr; c->VSGetConstantBuffers(1, 1, &cb1);
        if (cb1)
        {
            D3D11_BUFFER_DESC d1{}; cb1->GetDesc(&d1);
            ID3D11Buffer* c1 = nullptr; ID3D11Buffer* r1 = nullptr;
            if (d1.ByteWidth >= 9 * 16 && ring_next(c, g_sp1Ring[eye], d1.ByteWidth, 16, c1, r1))
            {
                const M44& V = g_rendV; float e[3];
                for (int k = 0; k < 3; ++k) e[k] = -(V.m[3][0] * V.m[k][0] + V.m[3][1] * V.m[k][1] + V.m[3][2] * V.m[k][2]);   // this eye's world position
                // CHARFACE 2026-10-14 (JJ: "they should keep facing my character instead"): the point the cards face = Batman (AKVR's skeleton
                // lookup, akvr_character_pos); the head-free camera when he is not found (Batmobile, menus)
                using CharFn = int(*)(float*, float*); static CharFn p_char = nullptr; static bool s_ct = false;
                if (!s_ct) { HMODULE m = GetModuleHandleW(L"dinput8.dll"); p_char = m ? (CharFn)GetProcAddress(m, "akvr_character_pos") : nullptr; s_ct = m != nullptr; }
                float tgt[3] = { g_rendBase[0], g_rendBase[1], g_rendBase[2] }; float org[3];
                if (p_char && p_char(org, nullptr)) { tgt[0] = org[0]; tgt[1] = org[1]; tgt[2] = org[2]; InterlockedIncrement(&c_charFace); }
                const float p8[4] = { tgt[0] - e[0], tgt[1] - e[1], tgt[2] - e[2], 1.0f };
                c->UpdateSubresource(r1, 0, nullptr, p8, 0, 0);
                c->CopyResource(c1, cb1);
                const D3D11_BOX b16{ 0, 0, 0, 16, 1, 1 };
                c->CopySubresourceRegion(c1, 0, 8 * 16, 0, 0, r1, 0, &b16);
                c->VSSetConstantBuffers(1, 1, &c1);
                tl_sp1Orig = cb1;   // put back after the draw
            }
            else cb1->Release();
        }
    }
    c->VSSetConstantBuffers(0, 1, &copy);
    tl_spOrig = cb;   // put back after the draw (rain_depth_post)
    InterlockedIncrement(&c_spriteAxes);
}
void gloss_post(ID3D11DeviceContext* c);
void rain_depth_post(ID3D11DeviceContext* c) { gloss_post(c); if (tl_sp1Orig) { c->VSSetConstantBuffers(1, 1, &tl_sp1Orig); tl_sp1Orig->Release(); tl_sp1Orig = nullptr; }   /* SPRITEFACE */ if (tl_spOrig) { c->VSSetConstantBuffers(0, 1, &tl_spOrig); tl_spOrig->Release(); tl_spOrig = nullptr; }   /* SPRITEAXES */ if (tl_rdOrig) { c->VSSetConstantBuffers(0, 1, &tl_rdOrig); tl_rdOrig->Release(); tl_rdOrig = nullptr; } }
volatile LONG g_rainDrawDump = 0;   // RAINVP 2026-10-08 (JJ: paused / isolated rain looks flat): the rain draw's VS cb0 per eye
ID3D11Buffer* g_rdSnap[2] = {}; UINT g_rdSize[2] = {}; volatile LONG g_rdHave[2] = {};
UINT read_buf(ID3D11DeviceContext* c, ID3D11Buffer* b, std::vector<uint8_t>& out);
ID3D11Buffer* make_buf(ID3D11DeviceContext* c, UINT size, UINT bind);
void rain_draw_snap(ID3D11DeviceContext* c, int eye)
{
    if (eye < 0 || eye > 1 || g_rdHave[eye]) return;
    ID3D11Buffer* cb = nullptr; c->VSGetConstantBuffers(0, 1, &cb);
    if (!cb) return;
    D3D11_BUFFER_DESC d{}; cb->GetDesc(&d);
    if (!g_rdSnap[eye] || g_rdSize[eye] != d.ByteWidth) { if (g_rdSnap[eye]) g_rdSnap[eye]->Release(); g_rdSnap[eye] = make_buf(c, d.ByteWidth, 0); g_rdSize[eye] = d.ByteWidth; }
    if (g_rdSnap[eye]) { c->CopyResource(g_rdSnap[eye], cb); g_rdHave[eye] = 1; }
    cb->Release();
}
void rain_draw_readback(ID3D11DeviceContext* c)
{
    if (g_rainDrawDump <= 0 || !g_rdHave[0] || !g_rdHave[1]) return;
    std::vector<uint8_t> a, b; const UINT na = read_buf(c, g_rdSnap[1], a), nb = read_buf(c, g_rdSnap[0], b);
    g_rdHave[0] = g_rdHave[1] = 0; InterlockedDecrement(&g_rainDrawDump);
    logf("RAINVP: rain draw VS cb0, right eye (%u bytes) | left eye (%u bytes)", na, nb);
    const float* R = (const float*)a.data(); const float* L = (const float*)b.data();
    for (UINT r = 0; r < 13 && (r + 1) * 16 <= na && (r + 1) * 16 <= nb; ++r)
        logf("  cb0[%u] R %g %g %g %g | L %g %g %g %g", r, R[r * 4], R[r * 4 + 1], R[r * 4 + 2], R[r * 4 + 3], L[r * 4], L[r * 4 + 1], L[r * 4 + 2], L[r * 4 + 3]);
}
volatile LONG g_rainView = 0;   // RAINVIEW 2026-10-08 (JJ: the panel's "hide the rain" / "show only the rain"): 1 hide, 2 only
constexpr unsigned long long kRainVs = 0xf50d1365e929b3a0ull;   // the world rain draw (run 32 bisection; geo-11 era name)
ID3D11Buffer* make_buf(ID3D11DeviceContext* c, UINT size, UINT bind);
UINT read_buf(ID3D11DeviceContext* c, ID3D11Buffer* b, std::vector<uint8_t>& out);
// FXCB 2026-10-14 (DIAGKIT; JJ: the smoke still changes colour when the game camera pitches with the stick; flat game steady): with an
// effect selected in the panel's "find a broken effect" stepper, a capture also records the constants that effect is given - its pixel
// shader's cb0..cb2 at its first draw in the next left- and right-eye renders (copied on the recording context, read back after the eye
// render) - plus every camera AKN knows (eye view, game view, game camera with head, head-free camera now and 1..3 frames back).
volatile unsigned long long g_fxWatch = 0;   // FXCB: an effect watched without hiding it ("fxwatch <hash>", 0 = the stepper's); default = AK's sprite smoke PS
volatile LONG g_fxcbArm = 0; struct FxCb { ID3D11Buffer* st[3]; UINT bytes[3]; int eye; }; FxCb g_fxcb[2] = {}; volatile LONG g_fxcbHave[2] = {};
void fxcb_note(ID3D11DeviceContext* c, int eye)
{   // a draw of the selected effect: copy its pixel shader constants (once per eye per capture)
    if (!g_fxcbArm || eye < 0 || eye > 1 || g_fxcbHave[eye] || !g_dev) return;
    if (InterlockedExchange(&g_fxcbHave[eye], 1)) return;
    ID3D11Buffer* cbs[3] = {}; c->PSGetConstantBuffers(0, 3, cbs);
    FxCb& f = g_fxcb[eye]; f.eye = eye;
    for (int k = 0; k < 3; ++k)
    {
        f.st[k] = nullptr; f.bytes[k] = 0;
        if (!cbs[k]) continue;
        D3D11_BUFFER_DESC d{}; cbs[k]->GetDesc(&d);
        D3D11_BUFFER_DESC sd{ d.ByteWidth, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0 };
        if (SUCCEEDED(g_dev->CreateBuffer(&sd, nullptr, &f.st[k])) && f.st[k]) { c->CopyResource(f.st[k], cbs[k]); f.bytes[k] = d.ByteWidth; }
        cbs[k]->Release();
    }
}
void fxcb_end(int eye)
{   // render thread, after the eye render: the copies have run
    if (eye < 0 || eye > 1 || g_fxcbHave[eye] != 1 || !g_dev) return;
    g_fxcbHave[eye] = 2;
    ID3D11DeviceContext* ic = nullptr; g_dev->GetImmediateContext(&ic); if (!ic) return;
    FxCb& f = g_fxcb[eye];
    logf("FXCB: eye %d - constants of the selected effect (pixel shader %016llx)", eye, (unsigned long long)(g_skipPs ? g_skipPs : g_fxWatch));
    for (int k = 0; k < 3; ++k)
    {
        if (!f.st[k]) continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ic->Map(f.st[k], 0, D3D11_MAP_READ, 0, &m)))
        {
            const float* v = (const float*)m.pData; const UINT rows = f.bytes[k] / 16;
            for (UINT i = 0; i < rows && i < 64; ++i) if (v[i * 4] || v[i * 4 + 1] || v[i * 4 + 2] || v[i * 4 + 3]) logf("FXCB:   eye %d cb%d[%u] %.6g %.6g %.6g %.6g", eye, k, i, v[i * 4], v[i * 4 + 1], v[i * 4 + 2], v[i * 4 + 3]);
            ic->Unmap(f.st[k], 0);
        }
        f.st[k]->Release(); f.st[k] = nullptr;
    }
    ic->Release();
    auto dumpM = [eye](const char* what, const M44& M) { for (int i = 0; i < 4; ++i) logf("FXCB:   eye %d %s row %d %.6g %.6g %.6g %.6g", eye, what, i, M.m[i][0], M.m[i][1], M.m[i][2], M.m[i][3]); };
    dumpM("eye view", g_rendV);
    if (g_mkGameOk) dumpM("game view (as the game made it)", g_mkGameV);
    dumpM("game camera with head", g_mkVfull);
    dumpM("head-free camera now", g_mkVbase);
    for (int b = 1; b <= 3; ++b) { char w[48]; _snprintf_s(w, sizeof(w), _TRUNCATE, "head-free camera %d back", b); dumpM(w, g_mkVbaseHist[(g_mkHistHead - b) & 3]); }
    if (g_fxcbHave[0] == 2 && g_fxcbHave[1] == 2) { g_fxcbArm = 0; g_fxcbHave[0] = g_fxcbHave[1] = 0; logf("FXCB: done"); }
}
bool ps_note_skip(ID3D11DeviceContext* c, bool deferred)   // DETECTIVE: PS inventory / skip in the eye renders
{
    if (!p_shHash || (deferred ? g_eyeNow < 0 : !tl_inEyeRender)) return false;   // DETHOLD: detection always runs (was only with detvig / psinv / skipps)
    ID3D11PixelShader* ps = nullptr; c->PSGetShader(&ps, nullptr, nullptr);
    unsigned long long h = 0;
    if (ps)
    {   // DETVIG: per-object name cache
        PsName& e = g_psCache[((uintptr_t)ps >> 4) & 511];
        if (e.obj == ps) h = e.h; else { h = sh_hash_c(ps); e.h = h; e.obj = ps; }
        ps->Release();
    }
    if (h == 0xce8b103bb56ebabeull)
    {   // DETVIG; DETHOLD: gaps between sightings while in detective mode
        const ULONGLONG now = GetTickCount64(), last = g_detSeenMs;
        if (last && now - last < 5000 && (LONG)(now - last) > g_detGapMax) g_detGapMax = (LONG)(now - last);
        g_detSeenMs = now; InterlockedIncrement(&c_detSeen);
        if (g_detVigSkip) return true;
    }
    if (!h) return false;
    if (g_psInv > 0 && h)
    {
        const LONG n = g_psN; bool known = false;
        for (LONG i = 0; i < n && i < 4096; ++i) if (g_psSet[i] == h) { known = true; break; }
        if (!known && n < 4096) { const LONG i = InterlockedIncrement(&g_psN) - 1; if (i < 4096) g_psSet[i] = h; }
    }
    if (g_fxcbArm && h && h == (g_skipPs ? g_skipPs : g_fxWatch)) fxcb_note(c, deferred ? (int)g_eyeNow : tl_eye_render);   // FXCB
    return g_skipPs && h == g_skipPs;
}
extern "C" __declspec(dllexport) int akn_detective_active() { return det_on() ? 1 : 0; }   // DETVIG (DETHOLD: same hold as the view state)
void ps_inv_end(int eye)
{
    if (g_psInv == 1 && eye == 0 && g_fxCollect) { const LONG n = g_psN < 4096 ? g_psN : 4096; for (LONG i = 0; i < n; ++i) g_fxList[i] = g_psSet[i]; g_fxN = n; g_fxIdx = -1; g_skipPs = 0; g_fxCollect = 0; logf("FXSTEP: %ld effects collected", n); }   // DIAGKIT
    if (g_psInv <= 0 || eye != 0) return;
    if (InterlockedDecrement(&g_psInv) > 0) return;
    wchar_t path[MAX_PATH]; GetModuleFileNameW(nullptr, path, MAX_PATH); wchar_t* sl = wcsrchr(path, L'\\'); if (sl) wcscpy_s(sl + 1, MAX_PATH - (sl + 1 - path), L"akn\\ps_inventory.txt");
    FILE* f = nullptr; _wfopen_s(&f, path, L"w");
    if (f) { for (LONG i = 0; i < g_psN && i < 4096; ++i) fprintf(f, "%016llx\n", g_psSet[i]); fclose(f); }
    logf("PSINV: %ld pixel shaders in the eye renders -> akn\\ps_inventory.txt", g_psN);
    g_psN = 0;
}
bool skip_vs_any(ID3D11DeviceContext* c, bool deferred)
{
    vs_inv_note(c, deferred);
    if (ps_note_skip(c, deferred)) return true;   // (also DETVIG: detective mode's per-eye vignette)
    if ((!g_skipVs && !g_skipHi && !g_rainView && g_rainDrawDump <= 0 && !g_rainDepth && !g_gloss && !g_spriteAxes) || !p_shHash || (deferred ? g_eyeNow < 0 : !tl_inEyeRender)) return false;
    ID3D11VertexShader* vs = nullptr; c->VSGetShader(&vs, nullptr, nullptr);
    unsigned long long h = vs ? sh_hash_c(vs) : 0; if (!h) h = (unsigned long long)(uintptr_t)vs; if (vs) vs->Release();
    if (g_rainDrawDump > 0 && h == kRainVs) rain_draw_snap(c, deferred ? (int)g_eyeNow : tl_eye_render);   // RAINVP
    if (g_rainView == 1 && h == kRainVs) return true;
    if (h == kRainVs) rain_depth_pre(c, deferred ? (int)g_eyeNow : tl_eye_render);   // RAINDEPTH (undone after the draw)
    if (h == kRainVs && g_rainView != 1) { tl_rainMaskNow = true; tl_rainMaskEye = deferred ? (int)g_eyeNow : tl_eye_render; }   // RAINMASK
    gloss_pre(c, deferred ? (int)g_eyeNow : tl_eye_render);   // SUITGLOSS (undone after the draw)
    if (h != kRainVs) sprite_pre(c, deferred ? (int)g_eyeNow : tl_eye_render, deferred);   // SPRITEAXES (undone after the draw)
    if (g_rainView == 2 && h != kRainVs) return true;
    return h && (h == g_skipVs || (g_skipHi && h >= g_skipLo && h <= g_skipHi));
}
void STDMETHODCALLTYPE hk_DrawII(ID3D11DeviceContext* c, UINT a, UINT n, UINT b, INT d, UINT e) { if (skip_vs(c)) return; inst_draw(c, n, 0); any_draw(c, a, n, 3); o_DrawII(c, a, n, b, d, e); if (tl_rainMaskNow) rain_mask_draw(c, [&] { o_DrawII(c, a, n, b, d, e); }); rain_depth_post(c); }
void STDMETHODCALLTYPE hk_DrawI(ID3D11DeviceContext* c, UINT a, UINT n, UINT b, UINT e) { if (skip_vs(c)) return; inst_draw(c, n, 0); any_draw(c, a, n, 2); o_DrawI(c, a, n, b, e); if (tl_rainMaskNow) rain_mask_draw(c, [&] { o_DrawI(c, a, n, b, e); }); rain_depth_post(c); }
void STDMETHODCALLTYPE hk_DrawIIInd(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o) { inst_draw(c, 0, 1); o_DrawIIInd(c, b, o); }
void STDMETHODCALLTYPE hk_DrawIInd(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o) { inst_draw(c, 0, 2); o_DrawIInd(c, b, o); }
ID3D11Buffer* g_mkStage = nullptr;
void mk_dump_draw(ID3D11DeviceContext* c, const char* kind, UINT n)
{
    if (g_mkDump <= 0 || !g_hudPhase || c != g_gameCtx || tl_inEyeRender) return;
    if (InterlockedDecrement(&g_mkDump) < 0) return;
    ID3D11Buffer* cb = nullptr; c->VSGetConstantBuffers(0, 1, &cb);
    float f[64] = {}; UINT bw = 0; UINT sz[4] = {};
    if (cb)
    {
        D3D11_BUFFER_DESC bd{}; cb->GetDesc(&bd); bw = bd.ByteWidth;
        if (!g_mkStage && g_dev) { D3D11_BUFFER_DESC sd{}; sd.ByteWidth = 256; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; g_dev->CreateBuffer(&sd, nullptr, &g_mkStage); }
        if (g_mkStage)
        {
            const D3D11_BOX box{ 0, 0, 0, bw < 256 ? bw : 256, 1, 1 };
            c->CopySubresourceRegion(g_mkStage, 0, 0, 0, 0, cb, 0, &box);
            D3D11_MAPPED_SUBRESOURCE ms{};
            if (SUCCEEDED(c->Map(g_mkStage, 0, D3D11_MAP_READ, 0, &ms))) { memcpy(f, ms.pData, box.right); c->Unmap(g_mkStage, 0); }
        }
        cb->Release();
    }
    {   // MK1b: every VS constant slot's size
        ID3D11Buffer* cbs[4] = {}; c->VSGetConstantBuffers(0, 4, cbs);
        for (int i = 0; i < 4; ++i) if (cbs[i]) { D3D11_BUFFER_DESC bd{}; cbs[i]->GetDesc(&bd); sz[i] = bd.ByteWidth; cbs[i]->Release(); }
    }
    char tx[64] = "none";
    {   ID3D11ShaderResourceView* sv = nullptr; c->PSGetShaderResources(0, 1, &sv);
        if (sv) { ID3D11Resource* r = nullptr; sv->GetResource(&r); if (r) { res_desc(r, tx, sizeof(tx)); r->Release(); } sv->Release(); } }
    ID3D11VertexShader* vs = nullptr; c->VSGetShader(&vs, nullptr, nullptr);
    const unsigned long long nm = vs && p_shHash ? sh_hash_c(vs) : 0; if (vs) vs->Release();
    D3D11_VIEWPORT vp{}; UINT nv = 1; c->RSGetViewports(&nv, &vp);
    logf("MK1 hud draw #%ld (hud frame %ld) %s %u vs %016llx cb0 %u B | vp %.0f,%.0f %.0fx%.0f | r0 %.4f %.4f %.4f %.4f | r1 %.4f %.4f %.4f %.4f | r2 %.4f %.4f %.4f %.4f | r3 %.4f %.4f %.4f %.4f | r4 %.4f %.4f %.4f %.4f",
         InterlockedIncrement(&g_mkDumpN), g_hudSeq, kind, n, nm, bw, vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height,
         f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15], f[16], f[17], f[18], f[19]);
    char bl[64] = "blend none";
    {   // HUDBLEND: the game's own blend request for this draw (colour src/dest/op, alpha src/dest, write mask)
        if (g_reqValid && g_reqBlend) { D3D11_BLEND_DESC bd{}; g_reqBlend->GetDesc(&bd); const auto& t = bd.RenderTarget[0];
            _snprintf_s(bl, sizeof(bl), _TRUNCATE, "blend %d c %d/%d/%d a %d/%d m %x", (int)t.BlendEnable, (int)t.SrcBlend, (int)t.DestBlend, (int)t.BlendOp, (int)t.SrcBlendAlpha, (int)t.DestBlendAlpha, (unsigned)t.RenderTargetWriteMask); }
    }
    char b[1400]; int k = snprintf(b, sizeof(b), "MK1   %s, slots %u/%u/%u/%u, texture %s, rows 5-15:", bl, sz[0], sz[1], sz[2], sz[3], tx);
    for (int r = 5; r < 16 && (UINT)(r * 16) < bw; ++r) k += snprintf(b + k, sizeof(b) - k, " [%.4f %.4f %.4f %.4f]", f[r * 4], f[r * 4 + 1], f[r * 4 + 2], f[r * 4 + 3]);
    logf("%s", b);
}
// HUDSIG 2026-10-13 (JJ: main menu "black box overlay covering the menu items and Batman"; the bat transition "needs to be full
// field of view and closer to the camera, same as all the other mods"): which HUD draws make them is unknown, and the bats are too
// short to capture by hand. Every HUD-sheet draw kind (vertex + pixel shader, texture size) is logged when first seen, when it comes
// back after 1.5 s away, and when it goes (how long, how many draws, its viewport) - the transitions show up as kinds that live
// only for their second or two. Command "hudsig 0|1" (default on, at most 800 lines).
struct HudSig { unsigned long long vs, ps; UINT w, h; ULONGLONG first, last; LONG n, idxMin, idxMax; float vpW, vpH; bool active; };
HudSig g_hsig[512] = {}; LONG g_hsigN = 0; SRWLOCK g_hsigLock = SRWLOCK_INIT;   // (g_hudSigOn / g_hsigLines: with the DETCALLOUT globals)
// HUDRING 2026-10-13 (JJ: bat transitions "full field of view and closer"; the main menu "black box"): the bats last about a
// second, too short for a capture by hand. The finished HUD sheet is copied into a ring of 12 (every 3rd stereo frame, ~0.7 s),
// and AKVR asks for a dump at each screen change (akn_hudring_dump): 0.4 s later the ring is written oldest first at half size
// over grey (black shows) as akning<n>_<k>_<ms>.bmp. At most 12 dumps per session; "hudring 0|1" (default on in this test build).
constexpr int kHrN = 24;   // RINGEVERY 2026-10-13: every HUD frame (was every third of 12 - a one-frame flicker fell between)
ID3D11Texture2D* g_hrTex[kHrN] = {}; ID3D11Texture2D* g_hrMk[kHrN] = {}; ID3D11Texture2D* g_hrMkR[kHrN] = {}; UINT g_hrX0 = 0, g_hrY0 = 0; ULONGLONG g_hrT[kHrN] = {}; int g_hrHead = 0, g_hrFill = 0, g_hrFrame = 0;   // RINGBOTH: + right eye's layer, middle 60 % crop
// (g_hrOn / g_hrDumps / g_hrDumpAt / g_hrWhy: with the DETCALLOUT globals)
// EYERING 2026-10-14 (JJ: "the entire fov seems to rescale briefly when entering and exiting detective mode"): per finished pair,
// the left eye's world picture (middle 60 %) and both eyes' lens numbers (projection m00 m11 + off-centre m20 m21), kept beside the HUD
// ring and written with it: a lens change = ours, a steady lens with a scaled picture = the game's own screen effect
ID3D11Texture2D* g_erTex[kHrN] = {}; float g_erLens[kHrN][8] = {}; ULONGLONG g_erT[kHrN] = {}; int g_erHead = 0, g_erFill = 0; M44 g_erP[2] = {};
void eye_ring_note(ID3D11DeviceContext* c)
{
    if (!g_hrOn || !g_pair || !g_dev) return;
    D3D11_TEXTURE2D_DESC d{}; g_pair->GetDesc(&d); const UINT ew = d.Width / 2;
    const UINT w = (ew * 6 / 10) & ~1u, h = (d.Height * 6 / 10) & ~1u;
    if (g_erTex[0]) { D3D11_TEXTURE2D_DESC o{}; g_erTex[0]->GetDesc(&o); if (o.Width != w || o.Height != h || o.Format != d.Format) { for (auto& t : g_erTex) if (t) { t->Release(); t = nullptr; } g_erFill = 0; } }
    if (!g_erTex[0])
    {
        D3D11_TEXTURE2D_DESC rd = d; rd.Width = w; rd.Height = h; rd.Usage = D3D11_USAGE_DEFAULT; rd.BindFlags = 0; rd.CPUAccessFlags = 0; rd.MiscFlags = 0; rd.MipLevels = 1; rd.ArraySize = 1;
        for (auto& t : g_erTex) if (FAILED(g_dev->CreateTexture2D(&rd, nullptr, &t))) { for (auto& u : g_erTex) if (u) { u->Release(); u = nullptr; } logf("EYERING: create FAILED"); g_erFill = 0; return; }
        logf("EYERING: ring of %d left-eye pictures %ux%u format %d", kHrN, w, h, (int)d.Format);
    }
    const UINT x0 = (ew - w) / 2, y0 = (d.Height - h) / 2; const D3D11_BOX bx{ x0, y0, 0, x0 + w, y0 + h, 1 };
    c->CopySubresourceRegion(g_erTex[g_erHead], 0, 0, 0, 0, g_pair, 0, &bx);
    float* L = g_erLens[g_erHead];
    L[0] = g_erP[0].m[0][0]; L[1] = g_erP[0].m[1][1]; L[2] = g_erP[0].m[2][0]; L[3] = g_erP[0].m[2][1];
    L[4] = g_erP[1].m[0][0]; L[5] = g_erP[1].m[1][1]; L[6] = g_erP[1].m[2][0]; L[7] = g_erP[1].m[2][1];
    g_erT[g_erHead] = GetTickCount64(); g_erHead = (g_erHead + 1) % kHrN; if (g_erFill < kHrN) ++g_erFill;
}
void eye_ring_write(ID3D11DeviceContext* c, int dn, ULONGLONG tNew)
{
    const int n = g_erFill; if (!n) return;
    char keep[16]; strcpy_s(keep, g_capTag); strncpy_s(g_capTag, g_hrWhy, _TRUNCATE);
    for (int k = 0; k < n; ++k)
    {
        const int slot = (g_erHead - n + k + 2 * kHrN) % kHrN; const float* L = g_erLens[slot];
        const int ms = (int)((LONGLONG)tNew - (LONGLONG)g_erT[slot]);
        wchar_t nm[64]; swprintf_s(nm, L"ring%02d_%02d_%04dms_eyeL", dn, k, ms); save_bmp(c, g_erTex[slot], 0, -1, nm);
        logf("EYERING dump %d frame %d (%d ms before the newest HUD frame): left lens %.5f %.5f off %.5f %.5f | right lens %.5f %.5f off %.5f %.5f", dn, k, ms, L[0], L[1], L[2], L[3], L[4], L[5], L[6], L[7]);
    }
    strcpy_s(g_capTag, keep);
}
void hud_ring_write(ID3D11DeviceContext* c)
{
    const int n = g_hrFill; if (!n || !g_dev) return;
    const int dn = InterlockedIncrement(&g_hrDumps);
    D3D11_TEXTURE2D_DESC d{}; g_hrTex[0]->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr; if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &st)) || !st) return;
    const bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    const int w = (int)d.Width / 2, h = (int)d.Height / 2, row = (w * 3 + 3) & ~3;
    const ULONGLONG tNew = g_hrT[(g_hrHead + kHrN - 1) % kHrN];
    for (int k2 = 0; k2 < 3 * n; ++k2)
    {
        const int k = k2 / 3, which = k2 % 3; ID3D11Texture2D* const* ring = which == 2 ? g_hrMkR : which ? g_hrMk : g_hrTex;   // RINGEVERY / RINGBOTH: HUD sheet + both eyes' marker layers
        const int slot = (g_hrHead - n + k + 2 * kHrN) % kHrN;
        if (!ring[slot]) continue;
        c->CopyResource(st, ring[slot]);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(c->Map(st, 0, D3D11_MAP_READ, 0, &m))) continue;
        wchar_t fn[MAX_PATH]; swprintf_s(fn, L"%sring%02d_%02d_%04dms_%S_%s.bmp", g_dir, dn, k, (int)(tNew - g_hrT[slot]), g_hrWhy, which == 2 ? L"mkR" : which ? L"mkL" : L"hud");
        if (FILE* f = _wfopen(fn, L"wb"))
        {
            BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
            fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + row * h;
            ih.biSize = sizeof(ih); ih.biWidth = w; ih.biHeight = h; ih.biPlanes = 1; ih.biBitCount = 24;
            fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f);
            uint8_t* line = (uint8_t*)calloc(row, 1);
            for (int y = h - 1; y >= 0; --y)
            {
                const uint8_t* src = (const uint8_t*)m.pData + (size_t)(y * 2) * m.RowPitch;
                for (int x = 0; x < w; ++x)
                {
                    const uint8_t* q = src + x * 8; const int a = q[3];
                    int r = bgra ? q[2] : q[0], g = q[1], b = bgra ? q[0] : q[2];
                    // premultiplied sheet over grey 96: c + 96 * (1 - a)
                    r = r + 96 * (255 - a) / 255; g = g + 96 * (255 - a) / 255; b = b + 96 * (255 - a) / 255;
                    line[x * 3] = (uint8_t)(b > 255 ? 255 : b); line[x * 3 + 1] = (uint8_t)(g > 255 ? 255 : g); line[x * 3 + 2] = (uint8_t)(r > 255 ? 255 : r);
                }
                fwrite(line, row, 1, f);
            }
            free(line); fclose(f);
        }
        c->Unmap(st, 0);
    }
    st->Release();
    eye_ring_write(c, dn, tNew);   // EYERING
    logf("HUDRING dump %d (%s): %d sheets over the last %llu ms", dn, g_hrWhy, n, n > 1 ? tNew - g_hrT[(g_hrHead - n + 2 * kHrN) % kHrN] : 0ull);
}
void hud_ring_frame(ID3D11DeviceContext* c)   // at a stereo frame's start: the previous HUD sheet is complete
{
    if (!g_hrOn || !g_hudTex || !g_dev || g_hrDumps >= 12) return;
    D3D11_TEXTURE2D_DESC d{}; g_hudTex->GetDesc(&d);
    if (d.SampleDesc.Count != 1 || !(d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
        d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)) return;
    if (g_hrTex[0]) { D3D11_TEXTURE2D_DESC o{}; g_hrTex[0]->GetDesc(&o); if (o.Width != ((d.Width * 6 / 10) & ~1u) || o.Height != ((d.Height * 6 / 10) & ~1u) || o.Format != d.Format) { for (auto& t : g_hrTex) if (t) { t->Release(); t = nullptr; } for (auto& t : g_hrMk) if (t) { t->Release(); t = nullptr; } for (auto& t : g_hrMkR) if (t) { t->Release(); t = nullptr; } g_hrFill = 0; } }
    if (!g_hrTex[0])
    {
        D3D11_TEXTURE2D_DESC rd = d; rd.Usage = D3D11_USAGE_DEFAULT; rd.BindFlags = 0; rd.CPUAccessFlags = 0; rd.MiscFlags = 0; rd.MipLevels = 1; rd.ArraySize = 1;
        rd.Width = (d.Width * 6 / 10) & ~1u; rd.Height = (d.Height * 6 / 10) & ~1u; g_hrX0 = (d.Width - rd.Width) / 2; g_hrY0 = (d.Height - rd.Height) / 2;   // RINGBOTH
        for (auto& t : g_hrTex) if (FAILED(g_dev->CreateTexture2D(&rd, nullptr, &t))) { for (auto& u : g_hrTex) if (u) { u->Release(); u = nullptr; } g_hrOn = 0; logf("HUDRING: ring textures FAILED - off"); return; }
        if (g_mkEyeTex[0]) { D3D11_TEXTURE2D_DESC md{}; g_mkEyeTex[0]->GetDesc(&md); if (md.Width == d.Width && md.Height == d.Height && md.Format == d.Format) { for (auto& t : g_hrMk) if (FAILED(g_dev->CreateTexture2D(&rd, nullptr, &t))) t = nullptr; for (auto& t : g_hrMkR) if (FAILED(g_dev->CreateTexture2D(&rd, nullptr, &t))) t = nullptr; } }
        logf("HUDRING: ring of %d HUD sheets %ux%u format %d, every frame, with the left marker layer %d", kHrN, d.Width, d.Height, (int)d.Format, g_hrMk[0] ? 1 : 0);
    }
    {   // DETRING 2026-10-13 (JJ, watching the fake run: "i saw the distance marker flicker to the wrong location when you exited detective
        // mode"): the ring is written 0.5 s after detective mode switches on or off - the HUD sheet frames around the switch
        static int s_det = -1; const ULONGLONG ds = g_detSeenMs; const int d = ds && GetTickCount64() - ds < 80 ? 1 : 0;   // RINGRAW 2026-10-14: the composite itself (no 600 ms hold) - the real exit
        if (false && s_det >= 0 && d != s_det && !g_hrDumpAt && g_hrDumps < 12) { strcpy_s(g_hrWhy, d ? "detective_on" : "detective_off"); g_hrDumpAt = (LONGLONG)GetTickCount64() + 200; logf("DETRING: detective %s (right eye's view state swapped now) - HUD sheet ring written in 0.2 s", d ? "on" : "off"); }
        s_det = d;
    }
    const LONGLONG at = g_hrDumpAt;
    if (at && (LONGLONG)GetTickCount64() >= at) { g_hrDumpAt = 0; hud_ring_write(c); g_hrFill = 0; }
    ++g_hrFrame;
    {   D3D11_TEXTURE2D_DESC o{}; g_hrTex[0]->GetDesc(&o); const D3D11_BOX bx{ g_hrX0, g_hrY0, 0, g_hrX0 + o.Width, g_hrY0 + o.Height, 1 };
        c->CopySubresourceRegion(g_hrTex[g_hrHead], 0, 0, 0, 0, g_hudTex, 0, &bx); g_hrT[g_hrHead] = GetTickCount64();
        if (g_hrMk[g_hrHead] && g_mkEyeTex[0]) c->CopySubresourceRegion(g_hrMk[g_hrHead], 0, 0, 0, 0, g_mkEyeTex[0], 0, &bx);
        if (g_hrMkR[g_hrHead] && g_mkEyeTex[1]) c->CopySubresourceRegion(g_hrMkR[g_hrHead], 0, 0, 0, 0, g_mkEyeTex[1], 0, &bx); }
    g_hrHead = (g_hrHead + 1) % kHrN; if (g_hrFill < kHrN) ++g_hrFill;
}
// BLACKHUNT 2026-10-13 (JJ: the menus' "black box overlay covering the menu items and Batman"; ring dump 2: an opaque black 16:9
// band on the HUD sheet in the save-select menu): find its draw - each HUD-sheet draw in turn is dropped (HUDSKIPIDX) for 2 frames
// and the sheet's opaque-black share measured (64 x 64 samples); HUDSKIPIDX logs the dropped draw's shaders + texture. Started by
// AKVR's capture in a menu / the pause screen (command "blackhunt").
ID3D11Texture2D* g_bhStage = nullptr;
float sheet_black(ID3D11DeviceContext* c)
{
    D3D11_TEXTURE2D_DESC d{}; g_hudTex->GetDesc(&d);
    if (g_bhStage) { D3D11_TEXTURE2D_DESC o{}; g_bhStage->GetDesc(&o); if (o.Width != d.Width || o.Height != d.Height || o.Format != d.Format) { g_bhStage->Release(); g_bhStage = nullptr; } }
    if (!g_bhStage) { D3D11_TEXTURE2D_DESC sd = d; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0; sd.MipLevels = 1; sd.ArraySize = 1;
                      if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &g_bhStage))) { g_bhStage = nullptr; return -1.0f; } }
    c->CopyResource(g_bhStage, g_hudTex);
    D3D11_MAPPED_SUBRESOURCE m{}; if (FAILED(c->Map(g_bhStage, 0, D3D11_MAP_READ, 0, &m))) return -1.0f;
    int n = 0;
    for (int gy = 0; gy < 64; ++gy) for (int gx = 0; gx < 64; ++gx)
    {
        const uint8_t* q = (const uint8_t*)m.pData + (size_t)((gy + 0.5f) / 64.0f * d.Height) * m.RowPitch + (size_t)((gx + 0.5f) / 64.0f * d.Width) * 4;
        if (q[3] > 230 && q[0] < 13 && q[1] < 13 && q[2] < 13) ++n;
    }
    c->Unmap(g_bhStage, 0);
    return n / 4096.0f;
}
void black_hunt_frame(ID3D11DeviceContext* c)
{
    if (g_bhStep < 0 || !g_hudTex || !g_dev) return;
    if (g_bhWait > 0) { --g_bhWait; return; }
    const float cov = sheet_black(c);
    if (g_bhBase < 0.0f)
    {
        g_bhBase = cov; g_bhMax = g_hudDrawsLast < 120 ? g_hudDrawsLast : 120;
        logf("BLACKHUNT start: opaque black %.3f of the sheet, %ld HUD draws per frame - dropping each in turn", cov, g_bhMax);
        if (g_bhMax <= 0) { g_bhStep = -1; return; }
    }
    else
    {
        logf("BLACKHUNT drop #%ld: opaque black %.3f (all drawn %.3f)%s", g_bhStep, cov, g_bhBase, cov < g_bhBase * 0.5f ? "  <== THIS DRAW" : "");
        if (++g_bhStep >= g_bhMax) { g_hsiLo = g_hsiHi = -1; g_bhStep = -1; logf("BLACKHUNT done"); return; }
    }
    g_hsiLo = g_hsiHi = g_bhStep; g_hsiLogged = 0; g_bhWait = 2;
}
// HUDCOVER 2026-10-13 (REC1: the menus' "black box" = the game's fade to black on the HUD sheet; the bats end fully black too):
// how much of the sheet's 16:9 band is covered and how dark - 16 rows of the band copied each stereo frame (W x 1 each, into a
// W x 16 staging ring of 3) and read 2 frames later without waiting. Export akn_hud_cover (AKVR: full-view transitions).
ID3D11Texture2D* g_hcStage[3] = {}; int g_hcI = 0; volatile float g_hcAlpha = 0.0f, g_hcDark = 1.0f; volatile LONG g_hcOk = 0;
void hud_cover_frame(ID3D11DeviceContext* c)
{
    D3D11_TEXTURE2D_DESC d{}; g_hudTex->GetDesc(&d);
    if (d.SampleDesc.Count != 1 || d.Width < 64 || d.Height < 64) return;
    const bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    if (!(bgra || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS)) return;
    const int k = g_hcI % 3;
    if (g_hcStage[k]) { D3D11_TEXTURE2D_DESC o{}; g_hcStage[k]->GetDesc(&o); if (o.Width != d.Width || o.Format != d.Format) { g_hcStage[k]->Release(); g_hcStage[k] = nullptr; } }
    if (!g_hcStage[k]) { D3D11_TEXTURE2D_DESC sd = d; sd.Height = 16; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0; sd.MipLevels = 1; sd.ArraySize = 1;
                         if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &g_hcStage[k]))) { g_hcStage[k] = nullptr; return; } }
    const float bandH = d.Width * 9.0f / 16.0f < d.Height ? d.Width * 9.0f / 16.0f : (float)d.Height, top = 0.5f * (d.Height - bandH);
    for (UINT r = 0; r < 16; ++r)
    {
        const UINT y = (UINT)(top + (r + 0.5f) / 16.0f * bandH);
        D3D11_BOX b{ 0, y, 0, d.Width, y + 1, 1 };
        c->CopySubresourceRegion(g_hcStage[k], 0, 0, r, 0, g_hudTex, 0, &b);
    }
    ++g_hcI;
    if (g_hcI < 3) return;
    const int rd = g_hcI % 3;   // the copy made 2 frames ago
    D3D11_MAPPED_SUBRESOURCE m{};
    if (!g_hcStage[rd] || c->Map(g_hcStage[rd], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) return;
    double a = 0, rgb = 0; int n = 0;
    for (UINT r = 0; r < 16; ++r) for (UINT x = 8; x < d.Width; x += 16)
    { const uint8_t* q = (const uint8_t*)m.pData + (size_t)r * m.RowPitch + (size_t)x * 4; a += q[3]; rgb += (q[0] + q[1] + q[2]) / 3.0; ++n; }
    c->Unmap(g_hcStage[rd], 0);
    const float al = n ? (float)(a / n / 255.0) : 0.0f;
    g_hcAlpha = al; g_hcDark = al > 0.01f ? (float)(rgb / n / 255.0) / al : 1.0f; g_hcOk = 1;   // darkness of what covers (premultiplied sheet)
}
extern "C" __declspec(dllexport) int akn_hud_cover(float* alpha, float* bright) { if (!g_hcOk || !alpha || !bright) return 0; *alpha = g_hcAlpha; *bright = g_hcDark; return 1; }   // HUDCOVER (AKVR)
// PARTID (AKVR): a room mark value -> the HUD part index (akvr_captures\partid.txt); -1 = no part id
int part_of(const float* r4) { for (int c = 0; c < 4; ++c) { const float v = r4[c]; if (v < -0.0019531f + 1e-7f && v > -0.0028f) { const int k = (int)lroundf((-0.0019531250f - v) / 4.0e-7f) - 1; if (k >= 0) return k; } } return -1; }
void hud_sig_sweep();
void hud_sig_note(ID3D11DeviceContext* c, LONG idx)
{
    if (!p_shHash || g_hsigLines >= 800) return;
    { static ULONGLONG s_sw = 0; const ULONGLONG t = GetTickCount64(); if (t - s_sw > 1000) { s_sw = t; hud_sig_sweep(); } }   // (the per-second log may not run in menus)
    ID3D11VertexShader* vs = nullptr; c->VSGetShader(&vs, nullptr, nullptr); const unsigned long long hv = vs ? sh_hash_c(vs) : 0; if (vs) vs->Release();
    ID3D11PixelShader* ps = nullptr; c->PSGetShader(&ps, nullptr, nullptr); const unsigned long long hp = ps ? sh_hash_c(ps) : 0; if (ps) ps->Release();
    UINT w = 0, h = 0; ID3D11ShaderResourceView* sv = nullptr; c->PSGetShaderResources(0, 1, &sv);
    if (sv) { ID3D11Resource* r = nullptr; sv->GetResource(&r); ID3D11Texture2D* t = nullptr; if (r) { r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t); r->Release(); }
              if (t) { D3D11_TEXTURE2D_DESC d{}; t->GetDesc(&d); w = d.Width; h = d.Height; t->Release(); } sv->Release(); }
    D3D11_VIEWPORT vp{}; UINT nv = 1; c->RSGetViewports(&nv, &vp);
    const ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_hsigLock);
    LONG k = 0; for (; k < g_hsigN; ++k) if (g_hsig[k].vs == hv && g_hsig[k].ps == hp && g_hsig[k].w == w && g_hsig[k].h == h) break;
    if (k == g_hsigN)
    {
        if (g_hsigN < 512) { HudSig& e = g_hsig[g_hsigN++]; e = HudSig{ hv, hp, w, h, now, now, 1, idx, idx, vp.Width, vp.Height, true }; InterlockedIncrement(&g_hsigLines);
                             logf("HUDSIG new: vs %016llx ps %016llx texture %ux%u | draw #%ld on the sheet, viewport %.0fx%.0f", hv, hp, w, h, idx, vp.Width, vp.Height); }
    }
    else
    {
        HudSig& e = g_hsig[k];
        if (!e.active) { InterlockedIncrement(&g_hsigLines); logf("HUDSIG back after %llu ms: vs %016llx ps %016llx texture %ux%u | draw #%ld, viewport %.0fx%.0f", now - e.last, hv, hp, w, h, idx, vp.Width, vp.Height);
                         e.active = true; e.first = now; e.n = 0; e.idxMin = e.idxMax = idx; }
        e.last = now; ++e.n; if (idx < e.idxMin) e.idxMin = idx; if (idx > e.idxMax) e.idxMax = idx;
    }
    ReleaseSRWLockExclusive(&g_hsigLock);
}
void hud_sig_sweep()   // once a second: kinds that stopped
{
    const ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_hsigLock);
    for (LONG k = 0; k < g_hsigN && g_hsigLines < 800; ++k)
    {
        HudSig& e = g_hsig[k];
        if (!e.active || now - e.last < 1500) continue;
        e.active = false; InterlockedIncrement(&g_hsigLines);
        logf("HUDSIG gone: vs %016llx ps %016llx texture %ux%u | shown %llu ms, %ld draws, sheet draws #%ld-#%ld", e.vs, e.ps, e.w, e.h, e.last - e.first, e.n, e.idxMin, e.idxMax);
    }
    ReleaseSRWLockExclusive(&g_hsigLock);
}
bool hud_skip_draw(ID3D11DeviceContext* c)   // HUDSKIP: only while the HUD sheet is the target
{
    const LONG idx = InterlockedIncrement(&f_hudIdx) - 1;   // HUDSKIPIDX: this draw's index on the frame's sheet
    if (g_hudSigOn) hud_sig_note(c, idx);   // HUDSIG
    if (g_hsiLo >= 0 && idx >= g_hsiLo && idx <= g_hsiHi)
    {
        if (g_hsiLo == g_hsiHi && !g_hsiLogged && p_shHash)
        {
            g_hsiLogged = 1;
            ID3D11VertexShader* vs = nullptr; c->VSGetShader(&vs, nullptr, nullptr); const unsigned long long h = vs ? sh_hash_c(vs) : 0; if (vs) vs->Release();
            ID3D11PixelShader* ps = nullptr; c->PSGetShader(&ps, nullptr, nullptr); const unsigned long long hp = ps ? sh_hash_c(ps) : 0; if (ps) ps->Release();
            char tx[64] = "none"; ID3D11ShaderResourceView* sv = nullptr; c->PSGetShaderResources(0, 1, &sv);
            if (sv) { ID3D11Resource* r = nullptr; sv->GetResource(&r); if (r) { res_desc(r, tx, sizeof(tx)); r->Release(); } sv->Release(); }
            logf("HUDSKIPIDX draw %ld: vs %016llx ps %016llx texture %s", idx, h, hp, tx);
        }
        InterlockedIncrement(&f_hudSkipped); return true;
    }
    if (!g_hudSkipN || !g_hudSkipOn || !p_shHash) return false;
    ID3D11VertexShader* vs = nullptr; c->VSGetShader(&vs, nullptr, nullptr);
    const unsigned long long h = vs ? sh_hash_c(vs) : 0; if (vs) vs->Release();
    if (!h) return false;
    UINT w = 0, hh = 0;
    for (LONG i = 0; i < g_hudSkipN; ++i)
    {
        if (g_hudSkip[i].vs != h) continue;
        if (!w)
        {
            ID3D11ShaderResourceView* sv = nullptr; c->PSGetShaderResources(0, 1, &sv);
            if (sv) { ID3D11Resource* r = nullptr; sv->GetResource(&r); ID3D11Texture2D* t = nullptr; if (r) { r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t); r->Release(); }
                      if (t) { D3D11_TEXTURE2D_DESC d{}; t->GetDesc(&d); w = d.Width; hh = d.Height; t->Release(); } sv->Release(); }
        }
        if (!g_hudSkip[i].w || (g_hudSkip[i].w == w && g_hudSkip[i].h == hh)) { InterlockedIncrement(&f_hudSkipped); return true; }
    }
    return false;
}
// MARKSHEET: CPU copy of small constant buffers uploaded during the HUD phase (first 64 bytes = the placement)
// MKBATCH 2026-10-09: the WHOLE buffer (up to 4 KB = 256 rows): batched HUD shaders keep one placement per piece further
// down (05154232: rows 16 / 17 + 6k, 4b432a87: rows 12 / 13 + 4k). Reading the game's mapped memory back is slow (write-
// combined), so during the HUD phase Map hands the game OUR memory and Unmap copies it on (fast) - see hk_Map.
constexpr UINT kCbShBytes = 4096;
struct CbShadow { ID3D11Buffer* b; UINT bytes; float m[kCbShBytes / 4]; };   // rows 6 / 7: one-piece placement
CbShadow g_cbSh[128] = {}; ID3D11Buffer* g_vsCb0 = nullptr;
struct MapSwap { ID3D11Resource* r; void* real; UINT bytes; std::vector<uint8_t> mem; };
thread_local MapSwap tl_maps[4] = {};
thread_local ID3D11Resource* tl_mapRes = nullptr; thread_local void* tl_mapPtr = nullptr;
CbShadow& cb_slot(ID3D11Buffer* b) { return g_cbSh[((uintptr_t)b >> 4) & 127]; }
bool hud_cb_buffer(ID3D11Resource* r, UINT& width)
{
    D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return false;
    D3D11_BUFFER_DESC d{}; ((ID3D11Buffer*)r)->GetDesc(&d); width = d.ByteWidth;
    return (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) && d.ByteWidth >= 16;   // MKTAG: small pixel-shader colour buffers too
}
using VSCBFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using UpdSubFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
VSCBFn o_VSCB = nullptr; MapFn o_Map = nullptr; UnmapFn o_Unmap = nullptr; UpdSubFn o_UpdSub = nullptr;
void STDMETHODCALLTYPE hk_VSCB(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* b)
{
    if (c == g_gameCtx && s == 0 && n && b) g_vsCb0 = b[0];
    o_VSCB(c, s, n, b);
}
HRESULT STDMETHODCALLTYPE hk_Map(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP t, UINT f, D3D11_MAPPED_SUBRESOURCE* m)
{
    const HRESULT hr = o_Map(c, r, sub, t, f, m);
    UINT w = 0;
    if (SUCCEEDED(hr) && m && m->pData && g_markSheet && c == g_gameCtx && (g_hudPhase || g_hudAll) && !tl_inEyeRender && r && t == D3D11_MAP_WRITE_DISCARD && hud_cb_buffer(r, w))
    {
        MapSwap* ms = nullptr;   // MKBATCH: the game writes into our (cached) memory; Unmap forwards it
        for (MapSwap& x : tl_maps) if (!x.r) { ms = &x; break; }
        if (ms && w <= 65536) { ms->r = r; ms->real = m->pData; ms->bytes = w; if (ms->mem.size() < w) ms->mem.resize(w); m->pData = ms->mem.data(); }
        else { tl_mapRes = r; tl_mapPtr = m->pData; }   // no free slot: the first 128 bytes, read back
    }
    return hr;
}
void STDMETHODCALLTYPE hk_Unmap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub)
{
    if (r) for (MapSwap& x : tl_maps) if (x.r == r)
    {
        memcpy(x.real, x.mem.data(), x.bytes);
        CbShadow& e = cb_slot((ID3D11Buffer*)r); e.b = (ID3D11Buffer*)r;
        e.bytes = x.bytes < kCbShBytes ? x.bytes : kCbShBytes; memcpy(e.m, x.mem.data(), e.bytes);
        x.r = nullptr; x.real = nullptr;
        break;
    }
    if (r && r == tl_mapRes && tl_mapPtr)
    {
        CbShadow& e = cb_slot((ID3D11Buffer*)r); e.b = (ID3D11Buffer*)r; e.bytes = 128;
        __try { memcpy(e.m, tl_mapPtr, 128); } __except (EXCEPTION_EXECUTE_HANDLER) { e.b = nullptr; }
        tl_mapRes = nullptr; tl_mapPtr = nullptr;
    }
    o_Unmap(c, r, sub);
}
void STDMETHODCALLTYPE hk_UpdSub(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* bx, const void* src, UINT rp, UINT dp)
{
    UINT w = 0;
    if (g_markSheet && c == g_gameCtx && (g_hudPhase || g_hudAll) && !tl_inEyeRender && r && src && (!bx || bx->left == 0) && hud_cb_buffer(r, w))
    {   // MKBATCH: whole buffer (a box covers only part: then that part)
        const UINT n = bx ? (bx->right < w ? bx->right : w) : w;
        CbShadow& e = cb_slot((ID3D11Buffer*)r); e.b = (ID3D11Buffer*)r; e.bytes = n < kCbShBytes ? n : kCbShBytes;
        __try { memcpy(e.m, src, e.bytes); } __except (EXCEPTION_EXECUTE_HANDLER) { e.b = nullptr; }
    }
    o_UpdSub(c, r, sub, bx, src, rp, dp);
}
// SCANREC 2026-10-13 (JJ: the second line "still there"): which draws go to the scan box's sheet - the box (plus signs) vs the
// game's own line - by vertex + pixel shader + texture; each kind logged on first sight and at every 300th draw
struct ScanKind { unsigned long long vs, ps; UINT w, h; LONG n; };
ScanKind g_scanKinds[24] = {}; LONG g_scanKindN = 0;
void scan_rec(ID3D11DeviceContext* c, unsigned long long vh, const char* how, int pieces, int scanPieces, float tx, float ty)
{
    if (!g_recOn || g_scanRecLines >= 300) return;
    ID3D11PixelShader* ps = nullptr; c->PSGetShader(&ps, nullptr, nullptr); const unsigned long long hp = ps && p_shHash ? sh_hash_c(ps) : 0; if (ps) ps->Release();
    UINT w = 0, h = 0; ID3D11ShaderResourceView* sv = nullptr; c->PSGetShaderResources(0, 1, &sv);
    if (sv) { ID3D11Resource* r = nullptr; sv->GetResource(&r); ID3D11Texture2D* t = nullptr; if (r) { r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t); r->Release(); }
              if (t) { D3D11_TEXTURE2D_DESC d{}; t->GetDesc(&d); w = d.Width; h = d.Height; t->Release(); } sv->Release(); }
    LONG k = 0; for (; k < g_scanKindN; ++k) if (g_scanKinds[k].vs == vh && g_scanKinds[k].ps == hp && g_scanKinds[k].w == w && g_scanKinds[k].h == h) break;
    if (k == g_scanKindN) { if (g_scanKindN >= 24) return; g_scanKinds[g_scanKindN++] = ScanKind{ vh, hp, w, h, 0 }; }
    const LONG n = ++g_scanKinds[k].n;
    if (n == 1 || n % 300 == 0) { InterlockedIncrement(&g_scanRecLines);
        logf("SCANREC kind %ld (%s): vs %016llx ps %016llx texture %ux%u | pieces in the draw %d, callout pieces %d, first at %.3f %.3f | seen %ld times", k, how, vh, hp, w, h, pieces, scanPieces, tx, ty, n); }
}
// before a HUD draw: is it a marker piece? then draw it into the marker sheet (returns true; mk_route_end puts the
// HUD sheet back)
// MKBATCH: 0 = not a marker draw, 1 = routed whole (mk_route_end after the draw), 2 = split (mk_split_draw does it)
// MKGLOW2 + MKSTICKY 2026-10-11 (JJ: the distance marker "pulsing in depth and kept flickering out to a duplicate above
// it, particularly the glow effect under the text, but also occasionally the text"; captures c013428-c013504: the glow
// patch stays on the HUD sheet; MKPIECE: the glow paste c9b47e60 never carries the mark (20 / 20 "tag 0 -> hud sheet"),
// and marker text pieces (fd60f2d7 at 0.07 from the marker point) lose the mark on some frames = flat on those frames).
// MKGLOW2: a marked piece drawn into a small scratch target (the glow filter's source) puts that target in a set; blur
// passes that read a set target add their own target; any other draw into a set target takes it out (the target is
// reused). A HUD-sheet draw that reads a set target = the marker's glow paste -> the marker sheet, whole.
// MKSTICKY: a piece shader seen MARKED within the last 0.5 s stays with the marker while unmarked, if the piece lies
// within g_mkReach (0.09, the old position rule's safe distance) of the marker point.
float mk_tag_value()
{
    static float t = 0.0f; static bool tried = false;
    if (!tried) { HMODULE hm = GetModuleHandleW(L"dinput8.dll"); if (hm) { tried = true; using TagFn = float(*)(); TagFn tf = (TagFn)GetProcAddress(hm, "akvr_marker_tag_value"); t = tf ? tf() : 0.0f; } }
    return t;
}
bool mk_tag_in(const float* f, int n)
{
    const float t = mk_tag_value();
    if (t == 0.0f) return false;
    for (int i = 0; i < n; ++i) if (fabsf(f[i] - t) < 2.0e-4f) return true;
    return false;
}
// MKGLOW5 2026-10-11 (JJ: "the HUD element labeled fear on the left hand side is misplaced. It's now lower than it should
// be and it's skewed"; eyes.bmp 022718: FEAR shifts between the eyes like the 48m card, not like the HUD = drawn inside
// the marker card). Flash reuses its filter targets for other glowing HUD (FEAR), and MKGLOW4 kept a target marked across
// frames, so FEAR's glow paste read a still-marked target and went whole into the card. Now each mark carries the HUD
// frame of the marker drawing it came from; a paste counts as the marker's only within 30 HUD frames of it, any clear of
// the target unmarks it, and targets are tracked even while no marker is on screen (their own redraws unmark them).
struct MkScr { ID3D11Resource* r; unsigned f; };
MkScr g_mkScratch[8] = {}; int g_mkScratchN = 0; volatile LONG c_mkGlow = 0; unsigned g_mkHudFrame = 0;
int mk_scratch_find(ID3D11Resource* r) { for (int i = 0; i < g_mkScratchN; ++i) if (g_mkScratch[i].r == r) return i; return -1; }
void mk_scratch_set(ID3D11Resource* r, bool on, unsigned f)
{
    const int i = mk_scratch_find(r);
    if (!on) { if (i >= 0) g_mkScratch[i] = g_mkScratch[--g_mkScratchN]; return; }
    if (i >= 0) g_mkScratch[i].f = f;
    else if (g_mkScratchN < 8) g_mkScratch[g_mkScratchN++] = MkScr{ r, f };
}
// the newest marker frame among the marked targets the pixel shader reads, or -1
long long mk_reads_scratch_f(ID3D11DeviceContext* c)
{
    if (!g_mkScratchN) return -1;
    ID3D11ShaderResourceView* v[4] = {}; c->PSGetShaderResources(0, 4, v);
    long long best = -1;
    for (int i = 0; i < 4; ++i) if (v[i])
    {
        ID3D11Resource* r = nullptr; v[i]->GetResource(&r);
        if (r) { const int k = mk_scratch_find(r); if (k >= 0 && (long long)g_mkScratch[k].f > best) best = g_mkScratch[k].f; r->Release(); }
        v[i]->Release();
    }
    return best;
}
bool mk_scratch_fresh(long long f) { return f >= 0 && g_mkHudFrame - (unsigned)f <= 30; }
bool mk_reads_scratch(ID3D11DeviceContext* c) { return mk_scratch_fresh(mk_reads_scratch_f(c)); }
// a draw into target res: marked by the marker's own (tagged) pieces, or by copying a fresh marked target; else unmarked
void mk_scratch_write(ID3D11Resource* res, bool tagged, ID3D11DeviceContext* c)
{
    if (tagged) { mk_scratch_set(res, true, g_mkHudFrame); return; }
    const long long rf = mk_reads_scratch_f(c);
    if (mk_scratch_fresh(rf)) mk_scratch_set(res, true, (unsigned)rf); else mk_scratch_set(res, false, 0);
}
// MKGLOW3: binding a scratch target turns g_hudBound off, so mk_route_begin never saw those draws - tracked here,
// for every HUD-phase draw of the game's context that is NOT aimed at the HUD sheet
extern ID3D11Buffer* g_vsCb0;
void mk_scratch_track(ID3D11DeviceContext* c)
{
    ID3D11RenderTargetView* rt = nullptr; c->OMGetRenderTargets(1, &rt, nullptr);
    if (!rt) return;
    if (rt != g_hudRTV)
    {
        ID3D11Resource* res = nullptr; rt->GetResource(&res);
        if (res)
        {
            bool tagged = false;
            if (g_vsCb0) { const CbShadow& e = cb_slot(g_vsCb0); if (e.b == g_vsCb0) tagged = mk_tag_in(e.m, (int)(e.bytes / 4)); }
            mk_scratch_write(res, tagged, c);   // MKGLOW5
            res->Release();
        }
    }
    rt->Release();
}
// MKSTICKY2 (JJ: "it was also the text sometimes ... and the border around it"): the border corners sit ~0.09 from the
// marker point (MKREACH note), so the hold reaches 0.10 (0.11 once pulled in a neighbouring HUD element)
float g_mkStickyReach = 0.10f; volatile LONG c_mkSticky = 0;
struct MkSticky { unsigned long long vh; ULONGLONG t; };
MkSticky g_mkSticky[16] = {};
void mk_sticky_note(unsigned long long vh)
{
    if (!vh) return;
    const ULONGLONG now = GetTickCount64(); MkSticky* o = &g_mkSticky[0];
    for (auto& e : g_mkSticky) { if (e.vh == vh) { e.t = now; return; } if (e.t < o->t) o = &e; }
    o->vh = vh; o->t = now;
}
// MKSTICKYOFF 2026-10-13 (HR5; JJ: the distance marker still flickers "to a different location and different size every now and again"):
// TAGFADE recognises the marker pieces whose mark is scaled by their fade (fake: held 0 in normal play), and in detective mode the only
// pieces the hold still grabbed (RB7: 18 / 18) carry the ROOM mark, faded = flat HUD pulled onto the marker for a frame. Off by default;
// command "mksticky 1" restores it.
bool mk_sticky(unsigned long long vh)
{
    if (!vh || !g_mkStickyOn) return false;
    const ULONGLONG now = GetTickCount64();
    for (const auto& e : g_mkSticky) if (e.vh == vh) return now - e.t < 5000;   // MKSTICKY3: 5 s (the text is marked ~1 frame in 7)
    return false;
}
struct MkSplit { UINT bytes; float hud[kCbShBytes / 4], mk[kCbShBytes / 4]; } g_mkSplit = {}; volatile LONG c_mkSplit = 0;
int mk_route_begin(ID3D11DeviceContext* c, ID3D11DepthStencilView** dsv)
{
    if (!g_markSheet || !g_mkRTV || !g_hudBound) return 0;
    if (g_mkCN == 0 && g_mkPieces <= 0) return 0;
    const CbShadow& e = cb_slot(g_vsCb0);
    if (!g_vsCb0 || e.b != g_vsCb0) { InterlockedIncrement(&c_mkNoCb); return 0; }
    {   // MKSHEETONLY 2026-10-10 (JJ: the glow "flashing above it at a bigger size"): only draws aimed at the HUD sheet. A
        // Flash glow draws the marker again into a small scratch target (60x92), blurs it and pastes it back; routing those
        // scratch draws put them in the marker sheet at the scratch target's place and size.
        ID3D11RenderTargetView* rt = nullptr; c->OMGetRenderTargets(1, &rt, nullptr);
        const bool onSheet = rt && rt == g_hudRTV;
        if (!onSheet && rt && g_mkCN > 0)
        {   // MKGLOW2: the glow scratch chain (marked piece -> scratch, blur reads a scratch -> its target; anything else clears)
            ID3D11Resource* res = nullptr; rt->GetResource(&res);
            if (res) { mk_scratch_write(res, mk_tag_in(e.m, (int)(e.bytes / 4)), c); res->Release(); }   // MKGLOW5
        }
        if (rt) rt->Release();
        if (!onSheet) { InterlockedIncrement(&c_mkOffSheet); return 0; }
    }
    const float* m = e.m;
    unsigned long long vh = 0;
    {   ID3D11VertexShader* vsh = nullptr; c->VSGetShader(&vsh, nullptr, nullptr);
        if (vsh) { static struct { const void* o; unsigned long long h; } cache[64] = {}; auto& ce = cache[((uintptr_t)vsh >> 4) & 63];
                   if (ce.o == vsh) vh = ce.h; else if (p_shHash) { vh = p_shHash(vsh); ce.o = vsh; ce.h = vh; } vsh->Release(); } }
    // MKSINGLE: one piece per draw (position = cb0 rows 6 / 7); MKBATCH: several, each with its own rows (layout read by
    // AKVR from the shader code). 3b819a7e = the pause backdrop: never a marker.
    using OneFn = int(*)(const void*); static OneFn p_one = nullptr; static bool s_oneTried = false;
    using LayFn = int(*)(const void*, int*); static LayFn p_lay = nullptr;
    using TagFn = float(*)(); static float s_tag = 0.0f;
    if (!s_oneTried)
    {
        HMODULE hm = GetModuleHandleW(L"dinput8.dll"); s_oneTried = hm != nullptr;
        p_one = hm ? (OneFn)GetProcAddress(hm, "akvr_vs_one_piece") : nullptr;
        p_lay = hm ? (LayFn)GetProcAddress(hm, "akvr_vs_batch_layout") : nullptr;
        TagFn tf = hm ? (TagFn)GetProcAddress(hm, "akvr_marker_tag_value") : nullptr; s_tag = tf ? tf() : 0.0f;
    }
    bool one = vh == 0x9938094af96353c0ull, layOk = false; int lay[32] = {};
    if ((vh >> 32) != 0x3b819a7eull)
    {
        ID3D11VertexShader* v2 = nullptr; c->VSGetShader(&v2, nullptr, nullptr);
        if (v2) { if (!one && p_one) one = p_one(v2) != 0; if (!one && p_lay) layOk = p_lay(v2, lay) != 0; v2->Release(); }
    }
    // MKTAG 2026-10-09 (JJ: "do you need to rethink your strategy?"): AKVR puts its own invisible colour mark on the HUD
    // parts marked "stays on its target"; every piece drawn from them carries it in its colour rows (VS cb0, per piece in
    // a batch) or in the pixel shader's constants. A piece goes to the marker sheet only if it carries the mark - position
    // only picks WHICH marker (any marker point within g_mkTagReach), so no neighbour is pulled in and no marker piece is
    // left behind. Command "mkmode 0" = the old position rule.
    const float tag = s_tag;
    auto isTag = [&](float v) { return tag != 0.0f && fabsf(v - tag) < 2.0e-4f; };
    auto scanTag = [&](const float* f, int n, int* where) { int h = 0; for (int i = 0; i < n; ++i) if (isTag(f[i])) { if (where && h < 4) where[h] = i; ++h; } return h; };
    // TAGFADE 2026-10-13 (HR2; JJ: the distance marker's number "in a different location", the box flicking there for a frame): HELDREC
    // (300 of 300 held pieces, fake run RB6): pieces coloured by their own colour ADD (multiply 0 - glows, the number's glow) carry the mark
    // SCALED by their own fade: alpha add = mark x alpha multiply, exact (-0.00390625 x 0.23828125 = -0.00093079). The mark is never lost;
    // the old check wanted it unscaled. A colour row pair (add, multiply) whose alpha add equals mark x alpha multiply is marked too.
    auto fadedTag = [&](const float* addRow, const float* multRow) { const float ma = multRow[3], aa = addRow[3], want = tag * ma;
        return g_tagFade && tag != 0.0f && ma > 0.01f && ma < 1.001f && fabsf(want) > 1.0e-7f && fabsf(aa - want) <= 1.0e-7f + 1.0e-4f * fabsf(want); };
    // DETCALLOUT: pieces of detective mode's scan callout carry their own mark; they all go to the callout's one marker
    static float s_scanV = 0.0f; static bool s_scanTried = false;
    if (!s_scanTried) { HMODULE hm = GetModuleHandleW(L"dinput8.dll"); s_scanTried = hm != nullptr; using SvFn = float(*)(); SvFn f = hm ? (SvFn)GetProcAddress(hm, "akvr_scan_tag_value") : nullptr; s_scanV = f ? f() : 0.0f; }
    // SCANHUD: the callout's pieces go to the MARKER SHEET unmoved (AKVR draws that sheet on the HUD plane, shifted to the object)
    const int scanMk = g_scanOn && s_scanV != 0.0f && g_mkRTV ? 0 : -1;
    auto scanHits = [&](const float* f, int n) { int h = 0; if (scanMk >= 0) for (int i = 0; i < n; ++i) if (fabsf(f[i] - s_scanV) < 2.0e-4f) ++h; return h; };
    int psHits = 0, psWhere[4] = { -1, -1, -1, -1 };
    {   ID3D11Buffer* pb = nullptr; c->PSGetConstantBuffers(0, 1, &pb);
        if (pb) { const CbShadow& x = cb_slot(pb); if (x.b == pb) psHits = scanTag(x.m, (int)(x.bytes / 4), psWhere); pb->Release(); } }
    const bool byTag = g_mkByTag != 0 && tag != 0.0f;
    const float reach = byTag ? g_mkTagReach : g_mkReach;
    auto nearest = [&](float px, float py, float& d2) { int b = -1; d2 = reach * reach; for (int i = 0; i < g_mkCN; ++i) { const float dx = px - g_mkC[i][0], dy = py - g_mkC[i][1]; if (dx * dx + dy * dy < d2) { d2 = dx * dx + dy * dy; b = i; } } return b; };
    int best = -1; float bd = 0.0f;
    if (one)
    {
        if (e.bytes < 128) return 0;
        const float tx = m[27], ty = m[31];
        int vsWhere[4] = { -1, -1, -1, -1 };
        // not the placement rows 6 / 7 (a position can equal the mark: 94f88871 row 6.w in the MKTAG fake run)
        const int vsHits = scanTag(m, 24, vsWhere) + scanTag(m + 32, (int)(e.bytes / 4) - 32, nullptr);
        const int sHits = scanHits(m, 24) + scanHits(m + 32, (int)(e.bytes / 4) - 32);   // DETCALLOUT
        bool faded = false;   // TAGFADE: any (row, row + 1) colour pair outside the placement rows 6 / 7
        if (vsHits == 0 && sHits == 0) { const int nr = (int)(e.bytes / 16); for (int r = 0; r + 1 < nr && r < 64 && !faded; ++r) if (r != 5 && r != 6 && r != 7) faded = fadedTag(m + r * 4, m + (r + 1) * 4); }
        if (faded) InterlockedIncrement(&c_mkTagFaded);
        const bool tagged = vsHits > 0 || sHits > 0 || faded;   // MKPSOFF: the PS cb0 can be the previous (marker) draw's, unused by this shader
        best = nearest(tx, ty, bd);
        if (sHits > 0)
        {   // SCANHUD: a callout piece - the whole draw to the marker sheet, unmoved
            InterlockedIncrement(&c_scanPieces);
            scan_rec(c, vh, "single", 1, 1, tx, ty);   // SCANREC
            if (g_recOn && g_scanPieceLines < 400) { InterlockedIncrement(&g_scanPieceLines); logf("SCANPIECE vs %016llx single: r6 %.4f %.4f %.4f %.4f | r7 %.4f %.4f %.4f %.4f | at %.3f %.3f | box layout point %.3f %.3f", vh, m[24], m[25], m[26], m[27], m[28], m[29], m[30], m[31], tx, ty, g_scanSheet[0], g_scanSheet[1]); }   // SCANPIECE
            ID3D11RenderTargetView* cur = nullptr; c->OMGetRenderTargets(1, &cur, dsv); if (cur) cur->Release();
            o_OMSetRT(c, 1, &g_mkRTV, *dsv);
            InterlockedIncrement(&f_mkRouted); InterlockedIncrement(&c_mkRouted);
            return 1;
        }
        const bool sticky = !tagged && byTag && best >= 0 && bd <= g_mkStickyReach * g_mkStickyReach && mk_sticky(vh);   // MKSTICKY
        if (sticky) InterlockedIncrement(&c_mkSticky);
        if (sticky && g_recOn && g_heldLines < 300)   // HELDREC (HR2 measurement): an unmarked piece kept with the marker - what the game did to the mark
        { InterlockedIncrement(&g_heldLines); logf("HELDREC single vs %016llx t %.3f %.3f d %.3f | r12 %.8f %.8f %.8f %.8f | r13 %.8f %.8f %.8f %.8f | mark %.8f", vh, tx, ty, sqrtf(bd), e.bytes >= 224 ? m[48] : 0.0f, e.bytes >= 224 ? m[49] : 0.0f, e.bytes >= 224 ? m[50] : 0.0f, e.bytes >= 224 ? m[51] : 0.0f, e.bytes >= 224 ? m[52] : 0.0f, e.bytes >= 224 ? m[53] : 0.0f, e.bytes >= 224 ? m[54] : 0.0f, e.bytes >= 224 ? m[55] : 0.0f, tag); }
        if (tagged && g_recOn && g_markedLines < 120)   // HELDREC: a MARKED piece of the same shaders for comparison
        { InterlockedIncrement(&g_markedLines); logf("MARKEDREC single vs %016llx t %.3f %.3f | r12 %.8f %.8f %.8f %.8f | r13 %.8f %.8f %.8f %.8f", vh, tx, ty, e.bytes >= 224 ? m[48] : 0.0f, e.bytes >= 224 ? m[49] : 0.0f, e.bytes >= 224 ? m[50] : 0.0f, e.bytes >= 224 ? m[51] : 0.0f, e.bytes >= 224 ? m[52] : 0.0f, e.bytes >= 224 ? m[53] : 0.0f, e.bytes >= 224 ? m[54] : 0.0f, e.bytes >= 224 ? m[55] : 0.0f); }
        const bool route = best >= 0 && (!byTag || tagged || sticky);
        if (route) mk_drawn_add(best, tx, ty);   // MKDRAWN
        if (route) mk_lag_piece(best, tx, ty);   // MKLAG
        if (route && g_mkEye) mk_anchor_piece(best, tx, ty, m + 24, m + 28);   // MKANCHOR
        if (tagged && best >= 0) mk_sticky_note(vh);
        if (g_mkPieces > 0) { InterlockedDecrement(&g_mkPieces); logf("MKPIECE vs %016llx single t %.3f %.3f | tag vs %d (at %d %d) ps %d (at %d) | markers %d (first %.3f %.3f) nearest %d (d %.3f) -> %s", vh, tx, ty, vsHits, vsWhere[0], vsWhere[1], psHits, psWhere[0], g_mkCN, g_mkC[0][0], g_mkC[0][1], best, best >= 0 ? sqrtf(bd) : -1.0f, route ? "MARKER SHEET" : "hud sheet"); }
        if (tagged) InterlockedIncrement(&c_mkTagged);
        if (!route && best >= 0 && bd < g_flipR * g_flipR)
        {   // FLIPREC 2026-10-13 (JJ: the distance marker flickers to another place every few s): a piece next to a marker point that
            // stays on the HUD sheet this frame - its rows 0-5 (where the mark rides) for the root cause
            InterlockedIncrement(&c_flipSingle);
            if (g_recOn && g_flipLines < 600) { InterlockedIncrement(&g_flipLines);
                logf("FLIPREC single vs %016llx t %.3f %.3f d %.3f (reach %.3f) tag %d sticky-shader %d | r6 %.4f %.4f %.4f %.4f | r7 %.4f %.4f %.4f %.4f | colour r12 %.8f %.8f %.8f %.8f | colour r13 %.8f %.8f %.8f %.8f | part %d / %d",
                     vh, tx, ty, sqrtf(bd), g_mkReach, vsHits, (int)mk_sticky(vh), m[24], m[25], m[26], m[27], m[28], m[29], m[30], m[31], e.bytes >= 224 ? m[48] : 0.0f, e.bytes >= 224 ? m[49] : 0.0f, e.bytes >= 224 ? m[50] : 0.0f, e.bytes >= 224 ? m[51] : 0.0f, e.bytes >= 224 ? m[52] : 0.0f, e.bytes >= 224 ? m[53] : 0.0f, e.bytes >= 224 ? m[54] : 0.0f, e.bytes >= 224 ? m[55] : 0.0f, e.bytes >= 224 ? part_of(m + 48) : -1, e.bytes >= 224 ? part_of(m + 52) : -1); }
        }
        if (tagged && !route)
        {   // MKMISS 2026-10-13 (JJ: the distance marker flickers "to another location for one frame"; MKJUMP: the game's marker point
            // never changed): a piece carrying the MARKER mark that stays on the flat sheet this HUD frame - no marker placed, or none in reach
            if (g_mkCN == 0) InterlockedIncrement(&c_mkMissNone); else InterlockedIncrement(&c_mkMissReach);
            if (g_mkHeldNow && g_mkCont) { InterlockedIncrement(&c_mkHeldDrop); return 4; }   // HELDDROP: left out this frame, never flat
            if (g_recOn && g_mkMissLines < 300) { InterlockedIncrement(&g_mkMissLines); logf("MKMISS single vs %016llx t %.3f %.3f: markers this HUD frame %d (first %.3f %.3f), nearest %d d %.3f (reach %.3f), HUD seq %ld", vh, tx, ty, g_mkCN, g_mkC[0][0], g_mkC[0][1], best, best >= 0 ? sqrtf(bd) : -1.0f, g_mkReach, g_hudSeq); }
        }
        if (!route) return 0;
        if (g_mkEye && !g_mkCard && g_mkEyeRTV[0] && best >= 0)
        {   // MKEYE: one piece - its rows 6 / 7 moved per eye (a partly known buffer: the whole draw moved by the viewport)
            D3D11_BUFFER_DESC b1{}; g_vsCb0->GetDesc(&b1);
            g_mkPlan.kind = (e.bytes >= b1.ByteWidth && b1.ByteWidth <= kCbShBytes && b1.ByteWidth >= 128) ? 1 : 3;
            g_mkPlan.mk1 = best; g_mkPlan.bytes = b1.ByteWidth; g_mkPlan.nFar = 0; g_mkLastMk = best;
            g_mkPlan.rowX = 6; g_mkPlan.rowY = 7;   // one-piece placement rows
            ID3D11RenderTargetView* cur = nullptr; c->OMGetRenderTargets(1, &cur, dsv); if (cur) cur->Release();
            InterlockedIncrement(&c_mkEyePieces); InterlockedIncrement(&f_mkRouted); InterlockedIncrement(&c_mkRouted);
            return 3;
        }
    }
    else
    {
        D3D11_BUFFER_DESC bdsc{}; g_vsCb0->GetDesc(&bdsc);
        // the whole buffer must be known (the split re-uploads it); the game sends only the rows its pieces use
        if (!layOk || e.bytes < bdsc.ByteWidth || bdsc.ByteWidth > kCbShBytes)
        {
            int w[4] = { -1, -1, -1, -1 }; const int vsHits = scanTag(m, (int)(e.bytes / 4), w);
            // (DETCALLOUT2: a whole draw carrying the callout's mark is NOT routed any more - JJ 2026-10-12: the distance marker's
            // text "would flip onto that plane": a draw of unknown layout holds other HUD pieces with the callout's)
            // MKGLOW 2026-10-10: a marked draw of a shader that is neither one-piece nor a known batch (c9b47e60 = the glow
            // filter pasting its blurred copy, mark at row 6) is the marker's own: whole draw to the marker sheet
            const bool glow = g_mkCN > 0 && mk_reads_scratch(c);   // MKGLOW2: reads the marker's glow scratch
            if (glow) InterlockedIncrement(&c_mkGlow);
            const bool whole = (byTag && vsHits > 0 && g_mkCN > 0) || glow;
            if (g_mkPieces > 0) { InterlockedDecrement(&g_mkPieces); logf("MKPIECE vs %016llx other (layout %d, shadow %u of %u bytes) | tag vs %d (at %d %d) ps %d -> %s", vh, layOk ? 1 : 0, e.bytes, bdsc.ByteWidth, vsHits, w[0], w[1], psHits, whole ? "MARKER SHEET (whole)" : "hud sheet"); }
            if (vsHits) InterlockedIncrement(&c_mkTagged);
            if (!whole) { if (vsHits) InterlockedIncrement(&c_mkTagLost); InterlockedIncrement(&c_mkFlat); return 0; }
            if (g_mkEye && !g_mkCard && g_mkEyeRTV[0])
            {   // MKEYE: a whole draw (the glow paste) - moved per eye by the viewport, with the marker drawn last
                const int o = g_mkLastMk >= 0 && g_mkLastMk < g_mkCN ? g_mkLastMk : (g_mkCN == 1 ? 0 : -1);
                if (o < 0) { InterlockedIncrement(&c_mkFlat); return 0; }
                g_mkPlan.kind = 3; g_mkPlan.mk1 = o; g_mkPlan.nFar = 0;
                if ((vh >> 32) == 0xc9b47e60ull && e.bytes >= 192 && bdsc.ByteWidth <= kCbShBytes && e.bytes >= bdsc.ByteWidth)
                {   // MKEYE2: the glow paste's own placement rows (VS c9b47e60: SV_Position = dp4(v, cb0[8]), dp4(v, cb0[9]),
                    // w = 1) - the full card map, so it turns with the game camera like the rest of the marker
                    g_mkPlan.kind = 1; g_mkPlan.rowX = 8; g_mkPlan.rowY = 9; g_mkPlan.bytes = bdsc.ByteWidth;
                }
                ID3D11RenderTargetView* cur = nullptr; c->OMGetRenderTargets(1, &cur, dsv); if (cur) cur->Release();
                InterlockedIncrement(&c_mkEyeWhole); InterlockedIncrement(&f_mkRouted); InterlockedIncrement(&c_mkRouted);
                return 3;
            }
            ID3D11RenderTargetView* cur = nullptr; c->OMGetRenderTargets(1, &cur, dsv);
            if (cur) cur->Release();
            o_OMSetRT(c, 1, &g_mkRTV, *dsv);
            InterlockedIncrement(&f_mkRouted); InterlockedIncrement(&c_mkRouted);
            return 1;
        }
        const int rowX = lay[0], rowY = lay[1], st = lay[2], rowW = lay[5], np = lay[6];
        const int lim = (int)(bdsc.ByteWidth / 16) < lay[3] ? (int)(bdsc.ByteWidth / 16) : lay[3];
        unsigned long long nearM = 0, scanM = 0, missM = 0; int nNear = 0, nFar = 0, nTag = 0, firstNear = -1, nAll = 0; float fx = 0.0f, fy = 0.0f;
        const bool stickyV = byTag && mk_sticky(vh);   // MKSTICKY
        memset(g_mkPlan.mk, -1, sizeof(g_mkPlan.mk));   // MKEYE: each piece's marker
        for (int k = 0; k < 64; ++k)
        {
            bool inside = rowX + k * st < lim && rowY + k * st < lim && (rowW < 0 || rowW + k * st < lim);
            for (int q = 0; q < np && inside; ++q) inside = lay[7 + q] + k * lay[19 + q] < lim;
            if (!inside) break;
            const float* X = m + (rowX + k * st) * 4; const float* Y = m + (rowY + k * st) * 4;
            if (!X[0] && !X[1] && !X[3] && !Y[0] && !Y[1] && !Y[3]) continue;   // unused slot
            ++nAll;
            float px = X[3], py = Y[3];
            if (rowW >= 0) { const float W = m[(rowW + k * st) * 4 + 3]; if (fabsf(W) > 1e-6f) { px /= W; py /= W; } }
            bool tagged = false;   // MKPSOFF (see above): only the piece's own rows
            for (int q = 0; q < np && !tagged; ++q)   // colour / uv rows only, not the placement
                if (lay[7 + q] != rowX && lay[7 + q] != rowY && lay[7 + q] != rowW) tagged = scanTag(m + (lay[7 + q] + k * lay[19 + q]) * 4, 4, nullptr) > 0;
            for (int q = 0; q < np && !tagged; ++q)   // TAGFADE: (add, multiply) pairs of this piece's colour rows
                if (lay[7 + q] != rowX && lay[7 + q] != rowY && lay[7 + q] != rowW) { const int r = lay[7 + q] + k * lay[19 + q]; if (r + 1 < lim && r + 1 != rowX && r + 1 != rowY && fadedTag(m + r * 4, m + (r + 1) * 4)) { tagged = true; InterlockedIncrement(&c_mkTagFaded); } }
            bool sTag = false;   // DETCALLOUT
            for (int q = 0; q < np && !sTag && scanMk >= 0; ++q)
                if (lay[7 + q] != rowX && lay[7 + q] != rowY && lay[7 + q] != rowW) sTag = scanHits(m + (lay[7 + q] + k * lay[19 + q]) * 4, 4) > 0;
            if (g_recOn && sTag && g_scanPieceLines < 400)
            {   // SCANPIECE 2026-10-13 (the second line: which callout piece is it?): each callout piece of a batch - its placement rows
                // (6/7-style X / Y of this piece: scale + translation) against the box's own point on the sheet
                InterlockedIncrement(&g_scanPieceLines);
                logf("SCANPIECE vs %016llx piece %d of %d: X %.4f %.4f %.4f %.4f | Y %.4f %.4f %.4f %.4f | at %.3f %.3f | box layout point %.3f %.3f", vh, k, nAll + 1,
                     X[0], X[1], X[2], X[3], Y[0], Y[1], Y[2], Y[3], px, py, g_scanSheet[0], g_scanSheet[1]);
            }
            if (sTag) { scanM |= 1ull << k; InterlockedIncrement(&c_scanPieces); continue; }   // SCANHUD: to the marker sheet below
            if (tagged) ++nTag;
            float d2 = 0.0f; int b = nearest(px, py, d2);
            const bool held = !tagged && stickyV && b >= 0 && d2 <= g_mkStickyReach * g_mkStickyReach;   // MKSTICKY2
            if (held) InterlockedIncrement(&c_mkSticky);
            if ((held || tagged) && g_recOn && (held ? g_heldLines < 300 : g_markedLines < 120))   // HELDREC: held (unmarked) vs marked batch pieces
            {
                InterlockedIncrement(held ? &g_heldLines : &g_markedLines);
                char rows[512] = ""; int o = 0;
                for (int q = 0; q < np && q < 4; ++q) { const int r = lay[7 + q] + k * lay[19 + q]; if (r < lim && lay[7 + q] != rowX && lay[7 + q] != rowY && lay[7 + q] != rowW) o += _snprintf_s(rows + o, sizeof(rows) - o, _TRUNCATE, " | row %d: %.8f %.8f %.8f %.8f", r, m[r * 4], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3]); }
                logf("%s batch vs %016llx piece %d, t %.3f %.3f d %.3f%s", held ? "HELDREC" : "MARKEDREC", vh, k, px, py, sqrtf(d2), rows);
            }
            const bool isNear = b >= 0 && (!byTag || tagged || held);
            if (!isNear && b >= 0 && d2 < g_flipR * g_flipR)
            {   // FLIPREC: a batched piece next to a marker point left on the HUD sheet - its colour rows
                InterlockedIncrement(&c_flipBatch);
                if (g_recOn && g_flipLines < 600) { InterlockedIncrement(&g_flipLines);
                    char rows[512] = ""; int o = 0;
                    for (int q = 0; q < np && q < 4; ++q) { const int r = lay[7 + q] + k * lay[19 + q]; if (r * 4 + 3 < lim * 4 && lay[7 + q] != rowX && lay[7 + q] != rowY && lay[7 + q] != rowW) o += _snprintf_s(rows + o, sizeof(rows) - o, _TRUNCATE, " | row %d: %.8f %.8f %.8f %.8f (part %d)", r, m[r * 4], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3], part_of(m + r * 4)); }
                    logf("FLIPREC batch vs %016llx piece %d of the draw, t %.3f %.3f d %.3f tag %d held %d%s", vh, k, px, py, sqrtf(d2), (int)tagged, (int)held, rows); }
            }
            if (isNear) mk_drawn_add(b, px, py);   // MKDRAWN
            if (isNear) mk_lag_piece(b, px, py);   // MKLAG
            if (isNear && g_mkEye) mk_anchor_piece(b, px, py, X, Y);   // MKANCHOR
            if (tagged && !isNear) missM |= 1ull << k;   // HELDDROP
            if (isNear) { nearM |= 1ull << k; ++nNear; g_mkPlan.mk[k] = (signed char)b; if (firstNear < 0) { firstNear = k; best = b; bd = d2; fx = px; fy = py; } } else ++nFar;
        }
        if (scanM && !nearM)
        {   // SCANHUD: the callout's pieces in a batch - the legacy split: the rest on the HUD sheet, these on the marker sheet
            const bool all = (int)__popcnt64(scanM) == nAll;
            scan_rec(c, vh, all ? "batch, all callout" : "batch, split", nAll, (int)__popcnt64(scanM), 0.0f, 0.0f);   // SCANREC
            ID3D11RenderTargetView* cur = nullptr; c->OMGetRenderTargets(1, &cur, dsv); if (cur) cur->Release();
            InterlockedIncrement(&f_mkRouted); InterlockedIncrement(&c_mkRouted);
            if (all) { o_OMSetRT(c, 1, &g_mkRTV, *dsv); return 1; }
            g_mkSplit.bytes = bdsc.ByteWidth;
            memcpy(g_mkSplit.hud, m, bdsc.ByteWidth); memcpy(g_mkSplit.mk, m, bdsc.ByteWidth);
            for (int k = 0; k < 64; ++k)
            {
                const int rx = rowX + k * st, ry = rowY + k * st;
                if (rx >= lim || ry >= lim) break;
                float* t = (scanM >> k & 1) ? g_mkSplit.hud : g_mkSplit.mk;
                memset(t + rx * 4, 0, 16); memset(t + ry * 4, 0, 16);
            }
            InterlockedIncrement(&c_mkSplit);
            return 2;
        }
        if (nTag) InterlockedIncrement(&c_mkTagged);
        if (nTag && nNear) mk_sticky_note(vh);   // MKSTICKY
        if (nTag > 0 && nNear < nTag)
        {   // MKMISS: marked pieces of a batch left on the flat sheet
            if (g_mkCN == 0) InterlockedIncrement(&c_mkMissNone); else InterlockedIncrement(&c_mkMissReach);
            if (g_mkHeldNow && g_mkCont && nNear == 0 && missM && bdsc.ByteWidth <= kCbShBytes)
            {   // HELDDROP: the batch's other pieces on the HUD sheet as usual, its marked pieces left out this frame (nothing to the marker sheet)
                g_mkSplit.bytes = bdsc.ByteWidth; memcpy(g_mkSplit.hud, m, bdsc.ByteWidth); memcpy(g_mkSplit.mk, m, bdsc.ByteWidth);
                for (int k = 0; k < 64; ++k)
                {
                    const int rx = rowX + k * st, ry = rowY + k * st; if (rx >= lim || ry >= lim) break;
                    memset(g_mkSplit.mk + rx * 4, 0, 16); memset(g_mkSplit.mk + ry * 4, 0, 16);
                    if (missM >> k & 1) { memset(g_mkSplit.hud + rx * 4, 0, 16); memset(g_mkSplit.hud + ry * 4, 0, 16); }
                }
                ID3D11RenderTargetView* cur = nullptr; c->OMGetRenderTargets(1, &cur, dsv); if (cur) cur->Release();
                InterlockedIncrement(&c_mkHeldDrop); InterlockedIncrement(&c_mkSplit);
                return 2;
            }
            if (g_recOn && g_mkMissLines < 300) { InterlockedIncrement(&g_mkMissLines); logf("MKMISS batch vs %016llx: marked %d, placed with a marker %d, markers this HUD frame %d (first %.3f %.3f), HUD seq %ld", vh, nTag, nNear, g_mkCN, g_mkC[0][0], g_mkC[0][1], g_hudSeq); }
        }
        if (g_mkPieces > 0) { InterlockedDecrement(&g_mkPieces); logf("MKPIECE vs %016llx batch x%d y%d w%d +%d (%u B): pieces %d, tagged %d (ps %d), to marker %d (first %d at %.3f %.3f, d %.3f) | markers %d (first %.3f %.3f) -> %s", vh, rowX, rowY, rowW, st, bdsc.ByteWidth, nAll, nTag, psHits, nNear, firstNear, fx, fy, firstNear >= 0 ? sqrtf(bd) : -1.0f, g_mkCN, g_mkC[0][0], g_mkC[0][1], nNear == 0 ? "hud sheet" : nFar == 0 ? "MARKER SHEET" : "SPLIT"); }
        if (nNear == 0) return 0;
        if (g_mkEye && !g_mkCard && g_mkEyeRTV[0])
        {   // MKEYE: each marker piece's rows moved per eye by its own marker; the rest stays on the HUD sheet
            g_mkPlan.kind = 2; g_mkPlan.bytes = bdsc.ByteWidth; g_mkPlan.rowX = rowX; g_mkPlan.rowY = rowY; g_mkPlan.rowW = rowW;
            g_mkPlan.st = st; g_mkPlan.lim = lim; g_mkPlan.nearM = nearM; g_mkPlan.nFar = nFar; g_mkLastMk = best;
            g_mkPlan.dropM = (g_mkHeldNow && g_mkCont) ? missM : 0ull; if (g_mkPlan.dropM) InterlockedIncrement(&c_mkHeldDrop);   // HELDDROP2: marked pieces out of reach on a held frame - left out, not on the HUD sheet
            ID3D11RenderTargetView* cur = nullptr; c->OMGetRenderTargets(1, &cur, dsv); if (cur) cur->Release();
            InterlockedAdd(&c_mkEyePieces, nNear); InterlockedIncrement(&f_mkRouted); InterlockedIncrement(&c_mkRouted);
            return 3;
        }
        if (nFar > 0)
        {   // MKBATCH: some pieces belong to the marker, some not - two draws, each with the other side's pieces folded to a point
            g_mkSplit.bytes = bdsc.ByteWidth;
            memcpy(g_mkSplit.hud, m, bdsc.ByteWidth); memcpy(g_mkSplit.mk, m, bdsc.ByteWidth);
            for (int k = 0; k < 64; ++k)
            {
                const int rx = rowX + k * st, ry = rowY + k * st;
                if (rx >= lim || ry >= lim) break;
                float* t = (nearM >> k & 1) ? g_mkSplit.hud : g_mkSplit.mk;
                memset(t + rx * 4, 0, 16); memset(t + ry * 4, 0, 16);
            }
            ID3D11RenderTargetView* cur = nullptr; c->OMGetRenderTargets(1, &cur, dsv);
            if (cur) cur->Release();
            InterlockedIncrement(&f_mkRouted); InterlockedIncrement(&c_mkRouted); InterlockedIncrement(&c_mkSplit);
            return 2;
        }
    }
    ID3D11RenderTargetView* cur = nullptr; c->OMGetRenderTargets(1, &cur, dsv);
    if (cur) cur->Release();
    o_OMSetRT(c, 1, &g_mkRTV, *dsv);
    InterlockedIncrement(&f_mkRouted); InterlockedIncrement(&c_mkRouted);
    return 1;
}
// MKBATCH: the split draw - our copy of cb0 without the marker's pieces into the HUD sheet, then with ONLY them into the
// marker sheet; the game's own cb0 bound again after
ID3D11Buffer* mk_sub_buffer_old(ID3D11DeviceContext* c, UINT bytes, int which);
// MKEYE2 2026-10-11 (JJ: "The actual text for the distance marker is not inside the box. It floats around the area of the
// box"; capture c035436: the "52m" text on the HUD sheet although its batch 05154232 was routed): the old pool held FOUR
// buffer sizes for the whole session and returned nothing for a fifth - that draw fell back to the HUD sheet, unmoved.
// Now: sizes rounded up to 256 / 512 / 1024 / 2048 / 4096 bytes (a constant buffer may be larger than the shader reads),
// 8 buffers per size and slot in turn (no write into a buffer the previous draw still reads - PERFRING).
ID3D11Buffer* mk_sub_buffer(ID3D11DeviceContext* c, UINT bytes, int which)
{
    static ID3D11Buffer* ring[5][3][8] = {}; static unsigned turn[5][3] = {};
    int k = 0; UINT sz = 256; while (sz < bytes && k < 4) { sz <<= 1; ++k; }
    if (bytes > sz || which < 0 || which > 2) return nullptr;
    const unsigned i = turn[k][which]++ & 7;
    ID3D11Buffer*& b = ring[k][which][i];
    if (!b)
    {
        ID3D11Device* dev = nullptr; c->GetDevice(&dev); if (!dev) return nullptr;
        D3D11_BUFFER_DESC d{}; d.ByteWidth = sz; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        dev->CreateBuffer(&d, nullptr, &b); dev->Release();
    }
    return b;
}
ID3D11Buffer* mk_sub_buffer_old(ID3D11DeviceContext* c, UINT bytes, int which)
{
    static struct { UINT w; ID3D11Buffer* b[3]; } pool[4] = {};   // MKEYE: 0 HUD sheet, 1 / 2 the eyes
    for (auto& p : pool)
    {
        if (p.w == bytes && p.b[which]) return p.b[which];
        if (p.w == 0 || p.w == bytes)
        {
            ID3D11Device* dev = nullptr; c->GetDevice(&dev); if (!dev) return nullptr;
            D3D11_BUFFER_DESC d{}; d.ByteWidth = bytes; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            ID3D11Buffer* b = nullptr; dev->CreateBuffer(&d, nullptr, &b); dev->Release();
            if (!b) return nullptr;
            p.w = bytes; p.b[which] = b; return b;
        }
    }
    return nullptr;
}
template <class F> void mk_split_draw(ID3D11DeviceContext* c, ID3D11DepthStencilView* dsv, F draw)
{
    ID3D11Buffer* game = g_vsCb0; ID3D11Buffer* a = mk_sub_buffer(c, g_mkSplit.bytes, 0); ID3D11Buffer* b = mk_sub_buffer(c, g_mkSplit.bytes, 1);
    if (!a || !b) { draw(); if (dsv) dsv->Release(); return; }
    o_UpdSub(c, a, 0, nullptr, g_mkSplit.hud, 0, 0);   // (g_mkSplit's arrays are kCbShBytes long: MKEYE2 buffers may be bigger than .bytes)
    o_UpdSub(c, b, 0, nullptr, g_mkSplit.mk, 0, 0);
    o_VSCB(c, 0, 1, &a); draw();                                   // HUD sheet: everything but the marker
    o_OMSetRT(c, 1, &g_mkRTV, dsv); o_VSCB(c, 0, 1, &b); draw();   // marker sheet: only the marker
    o_OMSetRT(c, 1, &g_hudRTV, dsv); o_VSCB(c, 0, 1, &game);
    if (dsv) dsv->Release();
}
// MKEYE: draw a routed HUD draw per eye (and its other pieces on the HUD sheet), then put the game's state back
volatile LONG c_mkGsDraws = 0, c_mkGsNone = 0;   // MKGS
ID3D11Buffer* mk_k_buffer(ID3D11DeviceContext* c)
{   // a ring of 16 small constant buffers for K (no write into one the previous draw still reads)
    static ID3D11Buffer* ring[16] = {}; static unsigned turn = 0;
    ID3D11Buffer*& b = ring[turn++ & 15];
    if (!b) { ID3D11Device* dev = nullptr; c->GetDevice(&dev); if (dev) { D3D11_BUFFER_DESC d{}; d.ByteWidth = 64; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_CONSTANT_BUFFER; dev->CreateBuffer(&d, nullptr, &b); dev->Release(); } }
    return b;
}
template <class F> void mk_eye_exec(ID3D11DeviceContext* c, ID3D11DepthStencilView* dsv, F draw)
{
    const MkPlan& P = g_mkPlan;
    ID3D11Buffer* game = g_vsCb0;
    if (g_mkGS)
    {   // MKGS: every vertex of the piece placed in 3D for each eye by a geometry shader, inside the piece's own draw
        using GsFn = ID3D11GeometryShader* (*)(void*); static GsFn p_gs = nullptr; static bool s_t = false;
        if (!s_t) { HMODULE hm = GetModuleHandleW(L"dinput8.dll"); p_gs = hm ? (GsFn)GetProcAddress(hm, "akvr_marker_gs") : nullptr; s_t = hm != nullptr; }
        ID3D11VertexShader* vsc = nullptr; c->VSGetShader(&vsc, nullptr, nullptr);
        ID3D11GeometryShader* gs = (p_gs && vsc) ? p_gs(vsc) : nullptr;
        if (vsc) vsc->Release();
        if (gs)
        {
            ID3D11GeometryShader* ogs = nullptr; c->GSGetShader(&ogs, nullptr, nullptr);
            ID3D11Buffer* ocb = nullptr; c->GSGetConstantBuffers(13, 1, &ocb);
            ID3D11RasterizerState* rs = nullptr; c->RSGetState(&rs);
            bool sc = false; if (rs) { D3D11_RASTERIZER_DESC rd{}; rs->GetDesc(&rd); sc = rd.ScissorEnable != 0; rs->Release(); }
            D3D11_RECT orc[16]; UINT nrc = 16; if (sc) c->RSGetScissorRects(&nrc, orc); else nrc = 0;
            D3D11_TEXTURE2D_DESC hd{}; if (g_hudTex) g_hudTex->GetDesc(&hd);
            const D3D11_RECT full{ 0, 0, (LONG)hd.Width, (LONG)hd.Height };
            auto eyeDraw = [&](int b) {
                if (b < 0 || b >= g_mkCN) return;
                if (sc) c->RSSetScissorRects(1, &full);
                c->GSSetShader(gs, nullptr, 0);
                for (int eye = 0; eye < 2; ++eye)
                {
                    ID3D11Buffer* kb = mk_k_buffer(c); if (!kb) continue;
                    float k16[16] = {}; memcpy(k16, g_mkK[b][eye], sizeof(float) * 12);
                    o_UpdSub(c, kb, 0, nullptr, k16, 0, 0); c->GSSetConstantBuffers(13, 1, &kb);
                    o_OMSetRT(c, 1, &g_mkEyeRTV[eye], dsv); draw();
                    InterlockedIncrement(&c_mkGsDraws);
                }
                c->GSSetShader(ogs, nullptr, 0);
                if (sc && nrc) c->RSSetScissorRects(nrc, orc);
                o_OMSetRT(c, 1, &g_hudRTV, dsv);
            };
            if (P.kind == 2)
            {   // a batch: the HUD-sheet copy without the marker pieces, then per marker a copy with only its pieces
                const CbShadow& e = cb_slot(game);
                static float tmp[kCbShBytes / 4];
                const UINT bytes = P.bytes;
                if (e.b == game && bytes <= kCbShBytes)
                {
                    auto copyKeep = [&](int keep, int which) {   // keep: marker index to keep, -1 = keep the non-marker pieces
                        memcpy(tmp, e.m, bytes);
                        for (int k = 0; k < 64; ++k)
                        {
                            const int rx = P.rowX + k * P.st, ry = P.rowY + k * P.st;
                            if (rx >= P.lim || ry >= P.lim) break;
                            const bool pnear = (P.nearM >> k & 1) != 0;
                            const bool kept = keep < 0 ? (!pnear && !(P.dropM >> k & 1)) : (pnear && P.mk[k] == keep);   // HELDDROP2
                            if (!kept) { memset(tmp + rx * 4, 0, 16); memset(tmp + ry * 4, 0, 16); }
                        }
                        ID3D11Buffer* bb = mk_sub_buffer(c, bytes, which);
                        if (bb) { o_UpdSub(c, bb, 0, nullptr, tmp, 0, 0); o_VSCB(c, 0, 1, &bb); }
                        return bb != nullptr;
                    };
                    if (P.nFar > 0 && copyKeep(-1, 0)) draw();
                    bool seen[8] = {};
                    for (int k = 0; k < 64; ++k)
                        if ((P.nearM >> k & 1) && P.mk[k] >= 0 && P.mk[k] < 8 && !seen[P.mk[k]])
                        { seen[P.mk[k]] = true; if (copyKeep(P.mk[k], 1)) eyeDraw(P.mk[k]); }
                    o_VSCB(c, 0, 1, &game);
                }
                else draw();
            }
            else eyeDraw(P.mk1);   // one piece or a whole draw (glow paste): the game's own constants, placed per vertex
            c->GSSetConstantBuffers(13, 1, &ocb); if (ocb) ocb->Release(); if (ogs) ogs->Release();
            if (dsv) dsv->Release();
            return;
        }
        InterlockedIncrement(&c_mkGsNone);
    }
    // Scaleform's scissor (if on) is in HUD-sheet pixels: open it for the moved copies
    ID3D11RasterizerState* rs = nullptr; c->RSGetState(&rs);
    bool sc = false; if (rs) { D3D11_RASTERIZER_DESC rd{}; rs->GetDesc(&rd); sc = rd.ScissorEnable != 0; rs->Release(); }
    D3D11_RECT orc[16]; UINT nrc = 16; if (sc) c->RSGetScissorRects(&nrc, orc); else nrc = 0;
    D3D11_TEXTURE2D_DESC hd{}; if (g_hudTex) g_hudTex->GetDesc(&hd);
    const D3D11_RECT full{ 0, 0, (LONG)hd.Width, (LONG)hd.Height };
    if (P.kind == 3)
    {
        D3D11_VIEWPORT ov[16]; UINT nv = 16; c->RSGetViewports(&nv, ov);
        if (nv >= 1 && P.mk1 >= 0)
        {
            if (sc) c->RSSetScissorRects(1, &full);
            for (int eye = 0; eye < 2; ++eye)
            {   // x' = a00 x + t.x, y' = a11 y + t.y (a card's small shear is left out for a whole draw)
                const MkEyeT& T = g_mkT[P.mk1][eye];
                D3D11_VIEWPORT v = ov[0];
                v.Width = ov[0].Width * T.a[0]; v.Height = ov[0].Height * T.a[3];
                v.TopLeftX = ov[0].TopLeftX + (T.t[0] + 1.0f) * 0.5f * ov[0].Width - v.Width * 0.5f;
                v.TopLeftY = ov[0].TopLeftY + (1.0f - T.t[1]) * 0.5f * ov[0].Height - v.Height * 0.5f;
                c->RSSetViewports(1, &v);
                o_OMSetRT(c, 1, &g_mkEyeRTV[eye], dsv); draw();
            }
            c->RSSetViewports(nv, ov);
            InterlockedIncrement(&c_mkEyeK[3]);
        }
    }
    else
    {
        const CbShadow& e = cb_slot(game);
        static float tmp[kCbShBytes / 4];
        const UINT bytes = P.bytes;
        ID3D11Buffer* bh = mk_sub_buffer(c, bytes, 0); ID3D11Buffer* be[2] = { mk_sub_buffer(c, bytes, 1), mk_sub_buffer(c, bytes, 2) };
        if (bh && be[0] && be[1] && bytes <= kCbShBytes && e.b == game)
        {
            if (P.kind == 2 && P.nFar > 0)
            {   // HUD sheet: everything but the marker pieces
                memcpy(tmp, e.m, bytes);
                for (int k = 0; k < 64; ++k)
                {
                    const int rx = P.rowX + k * P.st, ry = P.rowY + k * P.st;
                    if (rx >= P.lim || ry >= P.lim) break;
                    if ((P.nearM | P.dropM) >> k & 1) { memset(tmp + rx * 4, 0, 16); memset(tmp + ry * 4, 0, 16); }   // HELDDROP2
                }
                o_UpdSub(c, bh, 0, nullptr, tmp, 0, 0); o_VSCB(c, 0, 1, &bh); draw();
            }
            if (sc) c->RSSetScissorRects(1, &full);
            for (int eye = 0; eye < 2; ++eye)
            {
                memcpy(tmp, e.m, bytes);
                if (P.kind == 1) mk_xrows(tmp + P.rowX * 4, tmp + P.rowY * 4, nullptr, g_mkT[P.mk1][eye]);
                else
                    for (int k = 0; k < 64; ++k)
                    {
                        const int rx = P.rowX + k * P.st, ry = P.rowY + k * P.st, rw = P.rowW >= 0 ? P.rowW + k * P.st : -1;
                        if (rx >= P.lim || ry >= P.lim || rw >= P.lim) break;
                        const int b = P.mk[k];
                        if ((P.nearM >> k & 1) && b >= 0 && b < g_mkCN) mk_xrows(tmp + rx * 4, tmp + ry * 4, rw >= 0 ? tmp + rw * 4 : nullptr, g_mkT[b][eye]);
                        else { memset(tmp + rx * 4, 0, 16); memset(tmp + ry * 4, 0, 16); }
                    }
                o_UpdSub(c, be[eye], 0, nullptr, tmp, 0, 0); o_VSCB(c, 0, 1, &be[eye]);
                o_OMSetRT(c, 1, &g_mkEyeRTV[eye], dsv); draw();
            }
            o_VSCB(c, 0, 1, &game);
        }
        else { draw(); InterlockedIncrement(&c_mkEyeFail); }   // nothing known to move: the HUD sheet as before
        if (bh && be[0] && be[1] && bytes <= kCbShBytes && e.b == game) InterlockedIncrement(&c_mkEyeK[P.kind & 3]);
    }
    if (sc && nrc) c->RSSetScissorRects(nrc, orc);
    o_OMSetRT(c, 1, &g_hudRTV, dsv);
    if (dsv) dsv->Release();
}
void mk_route_end(ID3D11DeviceContext* c, ID3D11DepthStencilView* dsv)
{
    o_OMSetRT(c, 1, &g_hudRTV, dsv);
    if (dsv) dsv->Release();
}
// MKDRAWN: per expected marker (AKVR's centres, in order): expected x y, drawn bounding-box centre x y, pieces
extern "C" __declspec(dllexport) int akn_marker_drawn(float* out, int max)
{
    const int n = g_mkCN < max ? g_mkCN : max;
    for (int i = 0; i < n; ++i) { const MkDrawn& d = g_mkDrawn[i]; out[i * 5] = g_mkC[i][0]; out[i * 5 + 1] = g_mkC[i][1]; out[i * 5 + 2] = 0.5f * (d.x0 + d.x1); out[i * 5 + 3] = 0.5f * (d.y0 + d.y1); out[i * 5 + 4] = (float)d.n; }
    return n;
}
// MKEYE: the per-eye marker layers (AKVR lays layer e over eye picture e, 1:1); 0 = MKEYE off
extern "C" __declspec(dllexport) int akn_marker_eye_mode() { return g_mkEye && g_mkEyeTex[0] && g_mkEyeTex[1] && g_markSheet ? 1 : 0; }
// MKCARD: 1 = AKVR draws AKN's perspective cards from the flat marker sheet (akn_marker_cards), not the eye layers
extern "C" __declspec(dllexport) int akn_marker_card_mode() { return akn_marker_eye_mode() && g_mkCard ? 1 : 0; }
// per marker: sheet centre x y, then per eye the corners TL TR BL BR as NDC x, NDC y, clip w (26 floats); r = the sheet
// window's half size (sheet NDC) AKVR samples, scale = its marker size factor
extern "C" __declspec(dllexport) int akn_marker_cards(float r, float scale, float* out, int max)
{
    if (!out || !akn_marker_card_mode()) return 0;
    int n = 0;
    const float cs[4][2] = { { -1.0f, 1.0f }, { 1.0f, 1.0f }, { -1.0f, -1.0f }, { 1.0f, -1.0f } };
    for (int i = 0; i < g_mkCardDN && n < max; ++i)
    {
        const MkCardD& d = g_mkCardD[i]; float* o = out + n * 26; bool ok = true;
        o[0] = d.c[0]; o[1] = d.c[1];
        for (int e = 0; e < 2 && ok; ++e)
            for (int k = 0; k < 4 && ok; ++k)
            {
                float P[3]; for (int a = 0; a < 3; ++a) P[a] = d.w[a] + r * scale * (cs[k][0] * d.ax[a] + cs[k][1] * d.ay[a]);
                const M44& M = g_mkCardEv[e]; float c4[4];
                for (int j = 0; j < 4; ++j) c4[j] = P[0] * M.m[0][j] + P[1] * M.m[1][j] + P[2] * M.m[2][j] + M.m[3][j];
                if (c4[3] <= 1e-3f) { ok = false; break; }
                float* q = o + 2 + e * 12 + k * 3; q[0] = c4[0] / c4[3]; q[1] = c4[1] / c4[3]; q[2] = c4[3];
            }
        if (ok) ++n;
    }
    return n;
}
extern "C" __declspec(dllexport) ID3D11Texture2D* akn_get_eye_marker_hud(int eye, long* routedLastFrame)
{
    if (routedLastFrame) *routedLastFrame = g_mkRoutedLast;
    return eye >= 0 && eye < 2 && akn_marker_eye_mode() ? g_mkEyeTex[eye] : nullptr;
}
extern "C" __declspec(dllexport) ID3D11Texture2D* akn_get_marker_hud(long* routedLastFrame)   // MARKSHEET (AKVR)
{
    if (routedLastFrame) *routedLastFrame = g_mkRoutedLast;
    return g_markSheet ? g_mkTex : nullptr;
}
// FLATREC 2026-10-14 (JJ: "the distance marker still flickers over to a copy"; RB10: the number + corner brackets land on the FLAT
// HUD sheet around each detective switch; RB11: FLIPREC never saw them - text glyphs are placed in pixels, not rows 6 / 7): for 0.7 s
// after the detective composite appears or disappears (raw, no hold), every HUD-sheet draw left flat is logged with its vertex shader,
// its placement rows and every value in its constants that looks like a mark (-0.0045 .. -0.0005). Needs "recorders 1".
volatile LONG g_flatLines = 0, c_flatWin = 0; ULONGLONG g_flatUntil = 0; unsigned long long g_flatSig[512]; int g_flatSigN = 0;   // one line per distinct piece per window
void flat_rec(ID3D11DeviceContext* c)
{
    if (!g_recOn) return;
    static int s_det = -1; const ULONGLONG now = GetTickCount64(), ds = g_detSeenMs; const int d = ds && now - ds < 80 ? 1 : 0;
    if (s_det >= 0 && d != s_det) { g_flatUntil = now + 700; g_flatLines = 0; g_flatSigN = 0; InterlockedIncrement(&c_flatWin); logf("FLATREC: detective %s - flat HUD draws for 0.7 s", d ? "on" : "off"); }
    s_det = d;
    if (now > g_flatUntil || g_flatLines >= 400) return;
    ID3D11RenderTargetView* rt = nullptr; c->OMGetRenderTargets(1, &rt, nullptr); const bool onSheet = rt && rt == g_hudRTV; if (rt) rt->Release();
    if (!onSheet || !g_vsCb0) return;
    const CbShadow& e = cb_slot(g_vsCb0); if (e.b != g_vsCb0) return;
    unsigned long long vh = 0; { ID3D11VertexShader* v = nullptr; c->VSGetShader(&v, nullptr, nullptr); if (v) { if (p_shHash) vh = p_shHash(v); v->Release(); } }
    char marks[400] = ""; int o = 0, nm = 0; const int nf = (int)(e.bytes / 4);
    for (int i = 0; i < nf && nm < 14; ++i) { const float f = e.m[i]; if (f < -0.0005f && f > -0.0045f) { o += _snprintf_s(marks + o, sizeof(marks) - o, _TRUNCATE, " [%d]%.7f", i, f); ++nm; } }
    unsigned long long sig = vh ^ ((unsigned long long)e.bytes << 48);
    for (int i = 0; i < nf; ++i) { const float f = e.m[i]; if (f < -0.0005f && f > -0.0045f) sig = sig * 1099511628211ull ^ (unsigned long long)(i * 131 + (int)(f * -1.0e6f)); }
    for (int k = 0; k < g_flatSigN; ++k) if (g_flatSig[k] == sig) return;
    if (g_flatSigN < 512) g_flatSig[g_flatSigN++] = sig;
    InterlockedIncrement(&g_flatLines);
    char pcs[700] = ""; int po = 0;
    {   // FLATREC2: a batch's own pieces (AKVR's layout from the shader code): where each sits and its colour rows' alpha
        using LayFn = int(*)(const void*, int*); static LayFn p_lay2 = nullptr; static bool t2 = false;
        if (!t2) { HMODULE hm = GetModuleHandleW(L"dinput8.dll"); p_lay2 = hm ? (LayFn)GetProcAddress(hm, "akvr_vs_batch_layout") : nullptr; t2 = hm != nullptr; }
        int lay[32] = {}; ID3D11VertexShader* v = nullptr; c->VSGetShader(&v, nullptr, nullptr);
        const bool ok = v && p_lay2 && p_lay2(v, lay); if (v) v->Release();
        if (ok)
            for (int k = 0; k < 24; ++k)
            {
                const int rx = lay[0] + k * lay[2], ry = lay[1] + k * lay[2]; if ((rx + 1) * 4 > nf || (ry + 1) * 4 > nf) break;
                const float* X = e.m + rx * 4; const float* Y = e.m + ry * 4; if (!X[0] && !X[1] && !X[3] && !Y[0] && !Y[1] && !Y[3]) continue;
                float px = X[3], py = Y[3]; if (lay[5] >= 0 && (lay[5] + k * lay[2] + 1) * 4 <= nf) { const float W = e.m[(lay[5] + k * lay[2]) * 4 + 3]; if (fabsf(W) > 1e-6f) { px /= W; py /= W; } }
                const int r0 = lay[7] + k * lay[19], r1 = lay[8] + k * lay[20];
                const float a0 = (r0 + 1) * 4 <= nf ? e.m[r0 * 4 + 3] : 0.0f, a1 = (r1 + 1) * 4 <= nf ? e.m[r1 * 4 + 3] : 0.0f, c0 = (r0 + 1) * 4 <= nf ? e.m[r0 * 4] : 0.0f, c1 = (r1 + 1) * 4 <= nf ? e.m[r1 * 4] : 0.0f;
                po += _snprintf_s(pcs + po, sizeof(pcs) - po, _TRUNCATE, " | p%d at %.3f %.3f rows %d:%.5f/%.5f %d:%.5f/%.5f", k, px, py, r0, c0, a0, r1, c1, a1);
            }
    }
    logf("FLATREC flat vs %016llx (%u B) markers %d (first %.3f %.3f) | r6.w %.3f r7.w %.3f | marks:%s%s", vh, e.bytes, g_mkCN, g_mkCN ? g_mkC[0][0] : 0.0f, g_mkCN ? g_mkC[0][1] : 0.0f, nf > 27 ? e.m[27] : 0.0f, nf > 31 ? e.m[31] : 0.0f, nm ? marks : " none", pcs);
}
void STDMETHODCALLTYPE hk_Draw11(ID3D11DeviceContext* c, UINT n, UINT s0) { if (g_hudBound) { InterlockedIncrement(&f_hudDraws); InterlockedIncrement(&f_hudFrameDraws); if (hud_skip_draw(c)) return; } mk_dump_draw(c, "Draw", n); d0_draw(c, false); if (dlss_draw_skip(c, false)) return; if (skip_vs(c)) return; log_draw(c, "Draw", n); any_draw(c, n, 0, 0); if (!g_hudBound && g_hudPhase && c == g_gameCtx && !tl_inEyeRender && g_markSheet) mk_scratch_track(c); ID3D11DepthStencilView* mkd = nullptr; const int mk = g_hudBound ? mk_route_begin(c, &mkd) : 0; if (mk == 4) { rain_depth_post(c); return; } if (mk == 3) mk_eye_exec(c, mkd, [&] { o_Draw11(c, n, s0); }); else if (mk == 2) mk_split_draw(c, mkd, [&] { o_Draw11(c, n, s0); }); else { if (mk == 0 && g_hudBound) flat_rec(c); o_Draw11(c, n, s0); if (mk == 1) mk_route_end(c, mkd); } rain_depth_post(c); }   // FLATREC
void STDMETHODCALLTYPE hk_DrawIdx11(ID3D11DeviceContext* c, UINT n, UINT s0, INT b0) { if (g_hudBound) { InterlockedIncrement(&f_hudDraws); InterlockedIncrement(&f_hudFrameDraws); if (hud_skip_draw(c)) return; } mk_dump_draw(c, "DrawIndexed", n); d0_draw(c, false); if (dlss_draw_skip(c, false)) return; if (skip_vs(c)) return; log_draw(c, "DrawIndexed", n); any_draw(c, n, 0, 1); if (!g_hudBound && g_hudPhase && c == g_gameCtx && !tl_inEyeRender && g_markSheet) mk_scratch_track(c); ID3D11DepthStencilView* mkd = nullptr; const int mk = g_hudBound ? mk_route_begin(c, &mkd) : 0; if (mk == 4) { rain_depth_post(c); return; } if (mk == 3) mk_eye_exec(c, mkd, [&] { o_DrawIdx11(c, n, s0, b0); }); else if (mk == 2) mk_split_draw(c, mkd, [&] { o_DrawIdx11(c, n, s0, b0); }); else { if (mk == 0 && g_hudBound) flat_rec(c); o_DrawIdx11(c, n, s0, b0); if (mk == 1) mk_route_end(c, mkd); } rain_depth_post(c); }   // FLATREC
// JJ's 2nd Quest test 2026-10-07: "the latest device" was Virtual Desktop's (created last, in-process) and its context
// function table is NOT the game's (D3D11 keeps different tables, e.g. single-threaded vs thread-safe devices) -> the
// hooks never saw the game (pairs 0, mono). Now: the device AKVR hands over (the one behind the game's swap chain); with
// -akvr we wait for it (up to 15 s of 3D), without AKVR (desktop tests, no VD) the held device as before.
ID3D11Device* volatile g_gameDevHint = nullptr;
volatile bool g_hooked = false;
ULONGLONG g_firstRenderMs = 0;
// FLAREOFF 2026-10-10 (JJ: "We need to turn off the lens flares ... The last time we tried to turn them off, it didn't
// work"; captures c012642 / c012647: red rings and ghosts around the crane lights, strongest in the right eye). The config
// switch (LENSFLAREOFF, LensFlares=False) never touched them: AK draws them in two shaders of its own post chain, named
// by Luma's AK mod (Garamond): the flare sprite pixel shader 0x2CCEAEA7 and the bloom + flare ghosts + lens dirt compute
// shader 0xB42A7F40 (names = CRC32 of the bytecode; AKVR keeps that CRC per shader object, export akvr_shader_crc).
// Luma's own copies of both (flare_shaders.h), compiled with DISABLE_LENS_FLARE_AND_LENS_DIRT=1, are bound in their
// place whenever the game binds the originals (immediate and recorded contexts). Command "flareoff 0|1" (default 1).
volatile LONG g_flareOff = 1, c_flarePs = 0, c_flareCs = 0;
ID3D11PixelShader* g_flarePs = nullptr; ID3D11ComputeShader* g_flareCs = nullptr; volatile LONG g_flareState = 0;   // 1 ready, -1 failed
SRWLOCK g_flLock = SRWLOCK_INIT; std::unordered_map<const void*, int> g_flKind;   // shader object -> 0 other, 1 flare PS, 2 bloom CS
using ShCrcFn = unsigned (*)(const void*); ShCrcFn p_shCrc = nullptr; bool p_shCrcTried = false;
int flare_kind(const void* sh)
{
    AcquireSRWLockShared(&g_flLock); auto it = g_flKind.find(sh); const int k = it != g_flKind.end() ? it->second : -1; ReleaseSRWLockShared(&g_flLock);
    if (k >= 0) return k;
    if (!p_shCrcTried) { HMODULE m = GetModuleHandleW(L"dinput8.dll"); p_shCrc = m ? (ShCrcFn)GetProcAddress(m, "akvr_shader_crc") : nullptr; p_shCrcTried = m != nullptr; }
    const unsigned crc = p_shCrc ? p_shCrc(sh) : 0;
    const int kind = crc == 0x2CCEAEA7u ? 1 : crc == 0xB42A7F40u ? 2 : 0;
    if (kind) logf("FLAREOFF: the game's %s found (object %p) - replaced while flareoff is on", kind == 1 ? "lens flare pixel shader 0x2CCEAEA7" : "bloom / flare / lens dirt compute shader 0xB42A7F40", sh);
    AcquireSRWLockExclusive(&g_flLock); g_flKind[sh] = kind; ReleaseSRWLockExclusive(&g_flLock);
    return kind;
}
bool flare_build(ID3D11DeviceContext* c)
{
    if (g_flareState) return g_flareState == 1;
    if (InterlockedCompareExchange(&g_flareState, 2, 0) != 0) return false;   // another thread builds them
    LONG ok = -1;
    ID3D11Device* dev = nullptr; c->GetDevice(&dev);
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    CompileFn compile = m ? (CompileFn)GetProcAddress(m, "D3DCompile") : nullptr;
    if (dev && compile)
    {
        const D3D_SHADER_MACRO mac[] = { { "DISABLE_LENS_FLARE_AND_LENS_DIRT", "1" }, { nullptr, nullptr } };
        ID3DBlob* b1 = nullptr; ID3DBlob* b2 = nullptr; ID3DBlob* e = nullptr;
        bool k = SUCCEEDED(compile(kFlarePsHlsl, sizeof(kFlarePsHlsl) - 1, "LensFlare_0x2CCEAEA7", mac, nullptr, "main", "ps_5_0", 0, 0, &b1, &e));
        if (e) { if (!k) logf("FLAREOFF: flare shader failed: %s", (const char*)e->GetBufferPointer()); e->Release(); e = nullptr; }
        k = k && SUCCEEDED(compile(kBloomCsHlsl, sizeof(kBloomCsHlsl) - 1, "BloomLensFlareLensDirt_0xB42A7F40", mac, nullptr, "main", "cs_5_0", 0, 0, &b2, &e));
        if (e) { if (!k) logf("FLAREOFF: bloom shader failed: %s", (const char*)e->GetBufferPointer()); e->Release(); }
        k = k && SUCCEEDED(dev->CreatePixelShader(b1->GetBufferPointer(), b1->GetBufferSize(), nullptr, &g_flarePs));
        k = k && SUCCEEDED(dev->CreateComputeShader(b2->GetBufferPointer(), b2->GetBufferSize(), nullptr, &g_flareCs));
        if (b1) b1->Release(); if (b2) b2->Release();
        if (k) ok = 1;
    }
    if (dev) dev->Release();
    logf("FLAREOFF: replacement shaders %s", ok == 1 ? "ready (Luma's, lens flare and lens dirt off)" : "NOT built - the game's own stay");
    g_flareState = ok;
    return ok == 1;
}
using PSSetShaderFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
using CSSetShaderFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11ComputeShader*, ID3D11ClassInstance* const*, UINT);
PSSetShaderFn o_PSSetSh = nullptr, od_PSSetSh = nullptr; CSSetShaderFn o_CSSetSh = nullptr, od_CSSetSh = nullptr;
bool flare_game_ctx(ID3D11DeviceContext* c)
{   // FLAREOFF2: only the game's own device (Virtual Desktop's in-process device passes through the same hooks)
    ID3D11Device* d = nullptr; c->GetDevice(&d);
    const bool game = d && (d == g_gameDevHint || d == g_dev);
    if (d) d->Release();
    return game;
}
ID3D11PixelShader* flare_ps(ID3D11DeviceContext* c, ID3D11PixelShader* ps)
{
    if (!g_flareOff || !ps || flare_kind(ps) != 1 || !flare_game_ctx(c) || !flare_build(c)) return ps;
    InterlockedIncrement(&c_flarePs); return g_flarePs;
}
ID3D11ComputeShader* flare_cs(ID3D11DeviceContext* c, ID3D11ComputeShader* cs)
{
    if (!g_flareOff || !cs || flare_kind(cs) != 2 || !flare_game_ctx(c) || !flare_build(c)) return cs;
    InterlockedIncrement(&c_flareCs); return g_flareCs;
}
// PSNEW 2026-10-14 (JJ: "the entire fov seems to rescale briefly when entering and exiting detective mode"; RB12 EYERING: both
// lenses identical through every switch, but the left eye's picture is zoomed ~1.3x and blurred for ~11 frames before detective mode
// shows = a screen effect of the game's own): every pixel / compute shader the game binds is noted by its CRC (Luma's naming); while
// "psnew 1" is armed, a shader never bound before is logged once with the time - the transition's own passes appear at the switch.
// "psnew list" prints every shader first seen since arming (first / last time, binds). "psskip <crc hex>" binds NO shader in its place
// (live A/B: what the picture does without that pass); "psskip 0" = off.
struct PsSeen { unsigned crc; int cs; ULONGLONG first, last; LONG binds; bool fresh; };
SRWLOCK g_pnLock = SRWLOCK_INIT; std::unordered_map<const void*, PsSeen> g_pnMap; volatile LONG g_pnOn = 0, g_pnArmed = 0, g_pnLines = 0; ULONGLONG g_pnArmT = 0;
volatile LONG g_psSkip = 0, c_psSkipped = 0;
volatile ULONGLONG g_pnArmAt = 0;   // DIAGKIT psnewarm
bool ps_seen(const void* sh, int cs)   // true = skip this bind (PSSKIP)
{
    if (g_pnArmAt && GetTickCount64() >= g_pnArmAt) { g_pnArmAt = 0; g_pnArmed = 1; logf("PSNEW: armed - effects drawn from now on that were not drawn before are logged"); }
    if (!sh || (!g_pnOn && !g_psSkip)) return false;
    if (!p_shCrcTried) { HMODULE m = GetModuleHandleW(L"dinput8.dll"); p_shCrc = m ? (ShCrcFn)GetProcAddress(m, "akvr_shader_crc") : nullptr; p_shCrcTried = m != nullptr; }
    const ULONGLONG now = GetTickCount64(); unsigned crc = 0; bool isNew = false;
    AcquireSRWLockExclusive(&g_pnLock);
    auto it = g_pnMap.find(sh);
    if (it == g_pnMap.end()) { crc = p_shCrc ? p_shCrc(sh) : 0; PsSeen e{ crc, cs, now, now, 1, g_pnArmed != 0 }; g_pnMap[sh] = e; isNew = g_pnArmed != 0; }
    else { crc = it->second.crc; it->second.last = now; ++it->second.binds; }
    ReleaseSRWLockExclusive(&g_pnLock);
    if (isNew && g_pnLines < 400) { InterlockedIncrement(&g_pnLines); logf("PSNEW %s shader %08X (object %p) first bound %llu ms after arming", cs ? "compute" : "pixel", crc, sh, now - g_pnArmT); }
    if (g_psSkip && crc == (unsigned)g_psSkip) { InterlockedIncrement(&c_psSkipped); return true; }
    return false;
}
void ps_new_list()
{
    AcquireSRWLockShared(&g_pnLock); int n = 0;
    for (const auto& kv : g_pnMap) if (kv.second.fresh) { ++n; logf("PSNEW list: %s %08X first %llu ms, last %llu ms after arming, binds %ld", kv.second.cs ? "compute" : "pixel", kv.second.crc, kv.second.first - g_pnArmT, kv.second.last - g_pnArmT, kv.second.binds); }
    ReleaseSRWLockShared(&g_pnLock); logf("PSNEW list: %d shaders first bound since arming (%zu known)", n, g_pnMap.size());
}
void STDMETHODCALLTYPE hk_PSSetSh(ID3D11DeviceContext* c, ID3D11PixelShader* ps, ID3D11ClassInstance* const* ci, UINT n) { if (ps_seen(ps, 0)) ps = nullptr; o_PSSetSh(c, flare_ps(c, ps), ci, n); }   // PSNEW / PSSKIP
void STDMETHODCALLTYPE hkd_PSSetSh(ID3D11DeviceContext* c, ID3D11PixelShader* ps, ID3D11ClassInstance* const* ci, UINT n) { if (ps_seen(ps, 0)) ps = nullptr; od_PSSetSh(c, flare_ps(c, ps), ci, n); }   // PSNEW / PSSKIP
void STDMETHODCALLTYPE hk_CSSetSh(ID3D11DeviceContext* c, ID3D11ComputeShader* cs, ID3D11ClassInstance* const* ci, UINT n) { if (ps_seen(cs, 1)) cs = nullptr; o_CSSetSh(c, flare_cs(c, cs), ci, n); }   // PSNEW / PSSKIP
void STDMETHODCALLTYPE hkd_CSSetSh(ID3D11DeviceContext* c, ID3D11ComputeShader* cs, ID3D11ClassInstance* const* ci, UINT n) { if (ps_seen(cs, 1)) cs = nullptr; od_CSSetSh(c, flare_cs(c, cs), ci, n); }   // PSNEW / PSSKIP
void STDMETHODCALLTYPE hk2_PSSetSh(ID3D11DeviceContext* c, ID3D11PixelShader* ps, ID3D11ClassInstance* const* ci, UINT n) { if (ps_seen(ps, 0)) ps = nullptr; o_PSSetSh(c, ps, ci, n); }   // PSNEW / PSSKIP 2026-10-14
void STDMETHODCALLTYPE hk2d_PSSetSh(ID3D11DeviceContext* c, ID3D11PixelShader* ps, ID3D11ClassInstance* const* ci, UINT n) { if (ps_seen(ps, 0)) ps = nullptr; od_PSSetSh(c, ps, ci, n); }
void STDMETHODCALLTYPE hk2_CSSetSh(ID3D11DeviceContext* c, ID3D11ComputeShader* cs, ID3D11ClassInstance* const* ci, UINT n) { if (ps_seen(cs, 1)) cs = nullptr; o_CSSetSh(c, cs, ci, n); }
void STDMETHODCALLTYPE hk2d_CSSetSh(ID3D11DeviceContext* c, ID3D11ComputeShader* cs, ID3D11ClassInstance* const* ci, UINT n) { if (ps_seen(cs, 1)) cs = nullptr; od_CSSetSh(c, cs, ci, n); }
void hook_context()
{
    if (g_hooked) return;
    if (!g_firstRenderMs) g_firstRenderMs = GetTickCount64();
    ID3D11Device* dev = g_gameDevHint;
    const char* why = "the game's device, from AKVR";
    if (!dev)
    {
        const bool akvr = wcsstr(GetCommandLineW(), L"-akvr") != nullptr;
        if (akvr && GetTickCount64() - g_firstRenderMs < 15000) return;   // AKVR will tell us which device is the game's
        dev = g_hookDev; why = akvr ? "AKVR never named the game's device - held device" : "held device (no AKVR)";
    }
    if (!dev) return;
    g_hooked = true;
    ID3D11DeviceContext* ctx = nullptr; dev->GetImmediateContext(&ctx);
    if (!ctx) return;
    logf("context hooks installed via device %p (%s), context table %p", (void*)dev, why, (void*)*(void***)ctx);
    void* fn = (*(void***)ctx)[33];   // ID3D11DeviceContext::OMSetRenderTargets
    // HOOKBATCH 2026-10-10 (JJ: "it pauses for quite a while on the title screen after you've already clicked"; log 03:18:55
    // -> 03:18:59.6: these ~40 hooks took 4.5 s on the render thread at the first 3D frame - MH_EnableHook freezes and
    // resumes every thread of the game PER hook): all queued, applied with one freeze at the end.
    const LONG64 tHk0 = qpc();
    if (MH_CreateHook(fn, (void*)&hk_OMSetRT, (void**)&o_OMSetRT) == MH_OK && MH_QueueEnableHook(fn) == MH_OK) logf("OMSetRenderTargets hook queued");
    void* fd = (*(void***)ctx)[13]; void* fi = (*(void***)ctx)[12];   // Draw, DrawIndexed
    if (MH_CreateHook(fd, (void*)&hk_Draw11, (void**)&o_Draw11) == MH_OK) MH_QueueEnableHook(fd);
    if (MH_CreateHook(fi, (void*)&hk_DrawIdx11, (void**)&o_DrawIdx11) == MH_OK) MH_QueueEnableHook(fi);
    void** vt = *(void***)ctx;
    struct { int slot; void* det; void** orig; } hs[] = { { 47, (void*)&hk_CopyRes, (void**)&o_CopyRes }, { 46, (void*)&hk_CopySub, (void**)&o_CopySub },
        { 57, (void*)&hk_Resolve, (void**)&o_Resolve }, { 41, (void*)&hk_Dispatch, (void**)&o_Dispatch }, { 42, (void*)&hk_DispatchInd, (void**)&o_DispatchInd }, { 34, (void*)&hk_OMRTUAV, (void**)&o_OMRTUAV },
        { 50, (void*)&hk_ClearRTV, (void**)&o_ClearRTV },
        { 8, (void*)&hk_PSSRV, (void**)&o_PSSRV }, { 25, (void*)&hk_VSSRV, (void**)&o_VSSRV }, { 67, (void*)&hk_CSSRV, (void**)&o_CSSRV },
        { 68, (void*)&hk_CSUAV, (void**)&o_CSUAV }, { 51, (void*)&hk_ClrUAVu, (void**)&o_ClrUAVu }, { 52, (void*)&hk_ClrUAVf, (void**)&o_ClrUAVf },
        { 53, (void*)&hk_ClrDSV, (void**)&o_ClrDSV }, { 10, (void*)&hk_PSSamp, (void**)&o_PSSamp },   // MIPBIAS
        { 35, (void*)&hk_Blend, (void**)&o_Blend },   // HUDBLEND
        { 44, (void*)&hk_RSVP, (void**)&o_RSVP }, { 45, (void*)&hk_RSSc, (void**)&o_RSSc },   // HUDVP
        { 7, (void*)&hk_VSCB, (void**)&o_VSCB }, { 14, (void*)&hk_Map, (void**)&o_Map }, { 15, (void*)&hk_Unmap, (void**)&o_Unmap }, { 48, (void*)&hk_UpdSub, (void**)&o_UpdSub },   // MARKSHEET
        { 20, (void*)&hk_DrawII, (void**)&o_DrawII }, { 21, (void*)&hk_DrawI, (void**)&o_DrawI }, { 39, (void*)&hk_DrawIIInd, (void**)&o_DrawIIInd }, { 40, (void*)&hk_DrawIInd, (void**)&o_DrawIInd },   // RAINDUMP
        { 9, (void*)&hk2_PSSetSh, (void**)&o_PSSetSh }, { 69, (void*)&hk2_CSSetSh, (void**)&o_CSSetSh } };   // PSNEW / PSSKIP (lean: no flare lookups)
        // (FLAREOFF bind hooks on slots 9 / 69 removed 2026-10-10 - FLAREOFF3 swaps the flare shaders at creation in AKVR)
    for (auto& h : hs)
    {
        const MH_STATUS a = MH_CreateHook(vt[h.slot], h.det, h.orig); const MH_STATUS b = a == MH_OK ? MH_QueueEnableHook(vt[h.slot]) : a;
        if (a != MH_OK || b != MH_OK) logf("context hook slot %d (%p) FAILED: create %d enable %d", h.slot, vt[h.slot], (int)a, (int)b);
    }
    logf("copy / resolve / dispatch / RT+UAV hooks in");
    {   // FLAREOFF2: akvr_settings.ini flareoff=0 leaves the game's own flare shaders alone (default 1)
        wchar_t ini[MAX_PATH]; GetModuleFileNameW(nullptr, ini, MAX_PATH);
        if (wchar_t* sl = wcsrchr(ini, L'\\')) { sl[1] = 0; wcscat_s(ini, L"akvr_settings.ini");
            FILE* f = nullptr; _wfopen_s(&f, ini, L"rb");
            if (f) { char line[256]; while (fgets(line, sizeof(line), f)) { int v = 0; if (sscanf_s(line, "flareoff=%d", &v) == 1) g_flareOff = v; } fclose(f); } }
        logf("FLAREOFF: lens flare + lens dirt off %ld at start (akvr_settings.ini flareoff=, command flareoff N)", g_flareOff);
    }
    // (FLAREOFF: no compile here any more - FLAREOFF3 in AKVR)
    {   // DEFERRED: the deferred contexts' function table (one of our own shows which it is)
        if (MH_CreateHook(vt[58], (void*)&hk_Exec, (void**)&o_Exec) == MH_OK) MH_QueueEnableHook(vt[58]);   // ExecuteCommandList
        ID3D11DeviceContext* dc = nullptr;
        if (SUCCEEDED(dev->CreateDeferredContext(0, &dc)) && dc)
        {
            void** dvt = *(void***)dc;
            if (dvt != vt)
            {
                struct { int slot; void* det; void** orig; } ds[] = { { 41, (void*)&hkd_Disp, (void**)&od_Disp }, { 42, (void*)&hkd_DispInd, (void**)&od_DispInd },
                    { 13, (void*)&hkd_Draw, (void**)&od_Draw }, { 12, (void*)&hkd_DrawIdx, (void**)&od_DrawIdx }, { 20, (void*)&hkd_DrawII, (void**)&od_DrawII }, { 21, (void*)&hkd_DrawI, (void**)&od_DrawI }, { 50, (void*)&hkd_Clr, (void**)&od_Clr },
                    { 8, (void*)&hkd_PSSRV, (void**)&od_PSSRV }, { 25, (void*)&hkd_VSSRV, (void**)&od_VSSRV }, { 67, (void*)&hkd_CSSRV, (void**)&od_CSSRV },
                    { 33, (void*)&hkd_RT, (void**)&od_RT }, { 68, (void*)&hkd_CSUAV, (void**)&od_CSUAV },   // XEYE
                    { 10, (void*)&hkd_PSSamp, (void**)&od_PSSamp }, { 53, (void*)&hkd_ClrDSV, (void**)&od_ClrDSV },   // MIPBIAS; DETCALLOUT
                    { 9, (void*)&hk2d_PSSetSh, (void**)&od_PSSetSh }, { 69, (void*)&hk2d_CSSetSh, (void**)&od_CSSetSh } };   // PSNEW / PSSKIP
                int okN = 0;
                for (auto& h : ds) { if (dvt[h.slot] == vt[h.slot]) continue; if (MH_CreateHook(dvt[h.slot], h.det, h.orig) == MH_OK && MH_QueueEnableHook(dvt[h.slot]) == MH_OK) ++okN; }
                logf("deferred context table %p: %d draw/dispatch hooks in", (void*)dvt, okN);
            }
            else logf("deferred contexts share the immediate table");
            dc->Release();
        }
        else logf("deferred context: could not create one");
    }
    {   // HOOKBATCH: every queued context hook switched on with one freeze of the game's threads
        const LONG64 tHk1 = qpc(); const MH_STATUS st = MH_ApplyQueued(); const LONG64 tHk2 = qpc();
        logf("context hooks switched on in one batch: %s - set-up %.0f ms, switch-on %.0f ms", st == MH_OK ? "ok" : "FAILED", (tHk1 - tHk0) * 1000.0 / g_qpf, (tHk2 - tHk1) * 1000.0 / g_qpf);
    }
    ctx->Release();
}
int g_rtLogs = 0;
// EYEMARK (test T12, JJ's 3rd Quest test: each eye got the other's picture): the middle 30 % of each half of the pair
// painted left = red, right = blue, so the headset capture shows which picture reached which eye
volatile LONG g_eyeMark = 0;
ID3D11Texture2D* g_markTex[2] = {};
void mark_eye(ID3D11DeviceContext* ctx, int eye, const D3D11_TEXTURE2D_DESC& d)
{
    const UINT bandH = d.Height * 3 / 10;
    if (!g_markTex[eye])
    {
        const bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_B8G8R8X8_UNORM;
        const uint8_t r = eye == 0 ? 255 : 0, b = eye == 0 ? 0 : 255;
        const uint32_t px = bgra ? (0xFF000000u | (r << 16) | b) : (0xFF000000u | (b << 16) | r);
        uint32_t* mem = (uint32_t*)malloc((size_t)d.Width * bandH * 4);
        if (!mem) return;
        for (size_t i = 0; i < (size_t)d.Width * bandH; ++i) mem[i] = px;
        D3D11_TEXTURE2D_DESC md{}; md.Width = d.Width; md.Height = bandH; md.MipLevels = 1; md.ArraySize = 1; md.Format = d.Format;
        md.SampleDesc.Count = 1; md.Usage = D3D11_USAGE_DEFAULT;
        D3D11_SUBRESOURCE_DATA init{ mem, d.Width * 4, 0 };
        if (FAILED(g_dev->CreateTexture2D(&md, &init, &g_markTex[eye]))) g_markTex[eye] = nullptr;
        free(mem);
        if (!g_markTex[eye]) { logf("eye mark: texture FAILED (format %d)", (int)d.Format); g_eyeMark = 0; return; }
    }
    ctx->CopySubresourceRegion(g_pair, 0, eye == 0 ? 0 : d.Width, (d.Height - bandH) / 2, 0, g_markTex[eye], 0, nullptr);
}
void after_eye_render(int eye)
{
    if (!g_dev) return;
    ID3D11DeviceContext* ctx = nullptr; g_dev->GetImmediateContext(&ctx);
    if (!ctx) return;
    if (g_bb)
    {
        ID3D11Texture2D* bb = nullptr;
        if (SUCCEEDED(g_bb->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&bb)))
        {
            D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d);
            // DLSS D1: the eye picture is the small render; DLSS writes the full-size eye (headset size) instead
            if (eye == 0 && g_depthProbe > 0) depth_probe_after(ctx);   // DEPTHPROBE
            if (eye == 0 && g_rendVPok && g_scanOn && g_scanRay.ok) scan_pass(ctx, mul44(g_rendV, g_rendP));   // DETCALLOUT: the left eye's depth, still there
            ID3D11Texture2D* up = akn_dlss_mode() > 0 ? akn_dlss_eval(ctx, eye, bb, g_rendJit[0], g_rendJit[1], &g_rendV.m[0][0], &g_rendP.m[0][0], g_rendVPok != 0) : nullptr;
            if (up) { D3D11_TEXTURE2D_DESC ud; up->GetDesc(&ud); d.Width = ud.Width; d.Height = ud.Height; }
            ID3D11Texture2D* const eyePic = up ? up : bb;
            if (g_pair) { D3D11_TEXTURE2D_DESC pd; g_pair->GetDesc(&pd); if (pd.Width != d.Width * 2 || pd.Height != d.Height || pd.Format != d.Format) { g_pair->Release(); g_pair = nullptr; logf("pair texture: size changed"); } }
            if (!g_pair && make_like(bb, d.Width * 2, D3D11_BIND_SHADER_RESOURCE, &g_pair))
            {   // make_like copies the window buffer's height: fix it for the DLSS size
                D3D11_TEXTURE2D_DESC pd; g_pair->GetDesc(&pd);
                if (pd.Height != d.Height) { g_pair->Release(); g_pair = nullptr; pd.Height = d.Height; pd.Width = d.Width * 2; g_dev->CreateTexture2D(&pd, nullptr, &g_pair); }
                logf("pair texture %ux%u format %d%s", d.Width * 2, d.Height, (int)d.Format, up ? " (DLSS output size)" : "");
            }
            if (g_pair && tl_afw)
            {   // AFW (afw.cpp): the drawn eye into its half, the same picture warped into the other eye's view into the other
                // half, and a whole pair published every frame with this frame's stamp (STR-002: one stamp, both eyes)
                const int o = eye ^ 1;
                ctx->CopySubresourceRegion(g_pair, 0, eye == 0 ? 0 : d.Width, 0, 0, eyePic, 0, nullptr);
                ID3D11Texture2D* wp = (up && g_rendVPok && g_rendDOk) ? akn_afw_warp(ctx, eye, eyePic, &g_rendV.m[0][0], &g_rendP.m[0][0], g_rendJit[0], g_rendJit[1], &g_rendDV.m[0][0], &g_rendDP.m[0][0]) : nullptr;
                ctx->CopySubresourceRegion(g_pair, 0, o == 0 ? 0 : d.Width, 0, 0, wp ? wp : eyePic, 0, nullptr);   // no warp: the drawn eye in both (flat, never a stale eye)
                InterlockedIncrement(wp ? &c_afwPairs : &c_afwMono);
                if (g_eyeMark) { mark_eye(ctx, 0, d); mark_eye(ctx, 1, d); }
                if (g_rendVPok && g_rendDOk)
                {   // MARKERCUT: both eyes' view x lens without the jitter
                    M44 pp = g_rendP; pp.m[2][0] = 0.0f; pp.m[2][1] = 0.0f;
                    AcquireSRWLockExclusive(&g_mcLock); g_mcPubVP[eye] = mul44(g_rendV, pp); g_mcPubVP[o] = mul44(g_rendDV, g_rendDP); rain_vp_publish(ctx, g_mcPubVP[0]);   /* RAINLIGHT3 */ g_mcPubOk = 1;
                    for (int k = 0; k < 9; ++k) g_mcPubAx[k] = g_rendAx[k]; g_mcPubAxOk = g_rendAxOk; g_mcPubBVP = g_rendBVP; g_mcPubBvpOk = g_rendBVPok; ReleaseSRWLockExclusive(&g_mcLock);   // PAIRAXES / MKEYE
                }
                g_erP[eye] = g_rendP; g_erP[o] = g_rendDP; eye_ring_note(ctx);   // EYERING
                g_pairStamp = tl_renderStamp; InterlockedIncrement(&g_pairSeq); g_pairHalves = 0; g_mcEyeOk = 0;
                if (g_capWant & 4) { InterlockedAnd(&g_capWant, ~4); save_bmp(ctx, g_pair, 0, -1, L"pair"); }
                // one exposure for both eyes: the drawn eye's adaptation into the other eye's view state (Stage 6a, 14 temporal trap)
                if (akn_afw_lum() && g_lumN[0] == g_lumN[1])
                    for (int i = 0; i < g_lumN[eye]; ++i)
                        if (g_lum[eye][i] && g_lum[o][i] && g_lum[eye][i] != g_lum[o][i]) { ctx->CopyResource(g_lum[o][i], g_lum[eye][i]); InterlockedIncrement(&c_lumCopies); }
            }
            else if (g_pair)
            {
                ctx->CopySubresourceRegion(g_pair, 0, eye == 0 ? 0 : d.Width, 0, 0, eyePic, 0, nullptr);
                if (g_eyeMark) mark_eye(ctx, eye, d);
                g_pairHalves |= 1 << eye; g_erP[eye] = g_rendP;   // EYERING
                if (g_rendVPok) { M44 pp = g_rendP; pp.m[2][0] = 0.0f; pp.m[2][1] = 0.0f; g_mcEyeVP[eye] = mul44(g_rendV, pp); g_mcEyeOk |= 1 << eye; }   // MARKERCUT: unjittered
                if (eye == 0) { if (g_pairHalves == 3) { eye_ring_note(ctx); g_pairStamp = tl_renderStamp; InterlockedIncrement(&g_pairSeq);   // EYERING
                    if (g_mcEyeOk == 3) { AcquireSRWLockExclusive(&g_mcLock); g_mcPubVP[0] = g_mcEyeVP[0]; rain_vp_publish(ctx, g_mcEyeVP[0]);   /* RAINLIGHT3 */ g_mcPubVP[1] = g_mcEyeVP[1]; g_mcPubOk = 1;
                        for (int k = 0; k < 9; ++k) g_mcPubAx[k] = g_rendAx[k]; g_mcPubAxOk = g_rendAxOk; g_mcPubBVP = g_rendBVP; g_mcPubBvpOk = g_rendBVPok; ReleaseSRWLockExclusive(&g_mcLock); } }   // PAIRAXES / MKEYE
                    g_pairHalves = 0; g_mcEyeOk = 0; }
                if (eye == 0 && (g_capWant & 4)) { InterlockedAnd(&g_capWant, ~4); save_bmp(ctx, g_pair, 0, -1, L"pair"); }
                if (eye == 0)
                {
                    if (g_lumLogs > 0 && g_lumN[0] && g_lumN[1])
                    {
                        --g_lumLogs;
                        char b[600]; int k = snprintf(b, sizeof(b), "exposure targets (1x1): left eye %d, right eye %d |", g_lumN[0], g_lumN[1]);
                        for (int i = 0; i < g_lumN[0] && i < 6; ++i) k += snprintf(b + k, sizeof(b) - k, " L%d %p", i, (void*)g_lum[0][i]);
                        for (int i = 0; i < g_lumN[1] && i < 6; ++i) k += snprintf(b + k, sizeof(b) - k, " R%d %p", i, (void*)g_lum[1][i]);
                        logf("%s", b);
                    }
                    if (g_lumShare && g_lumN[0] == g_lumN[1])
                        for (int i = 0; i < g_lumN[0]; ++i)
                            if (g_lum[0][i] && g_lum[1][i] && g_lum[0][i] != g_lum[1][i]) { ctx->CopyResource(g_lum[1][i], g_lum[0][i]); InterlockedIncrement(&c_lumCopies); }
                }
            }
            bb->Release();
        }
    }
    ID3D11Resource* res = tl_lastRT;
    if (res) res->AddRef();
    {
        {
        if (res)
        {
            if (g_rtLogs < 4)
            {
                ++g_rtLogs;
                ID3D11Texture2D* t = nullptr;
                ID3D11Resource* bb = nullptr;
                if (g_sc) g_sc->GetBuffer(0, __uuidof(ID3D11Resource), (void**)&bb);
                if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t))) { D3D11_TEXTURE2D_DESC d; t->GetDesc(&d); logf("eye %d: last target written %p %ux%u format %d (%d target sets this render)%s", eye, (void*)res, d.Width, d.Height, (int)d.Format, tl_rtSets, res == bb ? " = the window's back buffer" : ""); t->Release(); }
                if (bb) bb->Release();
            }
            if (g_capWant & (1 << eye))
            {
                InterlockedAnd(&g_capWant, ~(1 << eye));
                if (g_bb) save_bmp(ctx, g_bb, eye, -1); else logf("capture: window buffer not known yet");
                for (int i = 0; i < tl_bigN; ++i) save_bmp(ctx, tl_big[i], eye, i);
            }
            res->Release();
        }
        else if (g_rtLogs < 4) { ++g_rtLogs; logf("eye %d render wrote no render target", eye); }
        }
    }
    ctx->Release();
}
void __fastcall hk_RenderVF(void* r)
{
    int eye = -1;
    unsigned long long stamp = 0;
    bool afw = false;   // AFW: this frame's only eye render (frame start and end in one)
    for (auto& t : g_rtag) if (t.r == r)
    {
        eye = t.eye; stamp = t.stamp; t.r = nullptr; afw = t.afw;
        g_rendDOk = t.dOk ? 1 : 0; if (t.dOk) { g_rendDV = t.dv; g_rendDP = t.dp; }   // AFW: the warp's target view
        g_rendAxOk = t.axOk ? 1 : 0; for (int k = 0; k < 9; ++k) g_rendAx[k] = t.ax[k];   // PAIRAXES
        g_rendBVPok = t.bvpOk ? 1 : 0; if (t.bvpOk) g_rendBVP = t.bvp;   // MKEYE
        if (t.baseOk) { for (int k = 0; k < 3; ++k) { g_rendBase[k] = t.basePos[k]; g_rendBase[3 + k] = t.baseFwd[k]; } g_rendBaseOk = 1; } else g_rendBaseOk = 0;   // NEARRAIN2
        g_rendV = t.v; g_rendP = t.p; g_rendVPok = t.vpOk ? 1 : 0;   // RAINDEPTH
        g_rendJit[0] = t.jit[0]; g_rendJit[1] = t.jit[1];   // DLSS D1
        if ((t.eye == 1 || t.afw) && !g_rainPause)
        {   // the right eye's render runs the rain simulation: its origin is this frame's base camera (NEARRAIN2), or the
            // right eye itself without it. Paused: the drops stay where they were, so the origin stays too.
            if (g_nearRain && g_farRain && t.baseOk) { for (int k = 0; k < 3; ++k) g_frameO[k] = t.basePos[k]; g_frameOok = 1; }
            else if (t.vpOk)
            {   // eye position = -translation x rotation^T (row-vector view)
                const M44& V = t.v;
                for (int k = 0; k < 3; ++k) g_frameO[k] = -(V.m[3][0] * V.m[k][0] + V.m[3][1] * V.m[k][1] + V.m[3][2] * V.m[k][2]);
                g_frameOok = 1;
            }
        }
        break;
    }
    tl_renderStamp = stamp;
    tl_afw = afw;
    const bool first = eye >= 0 && (afw || eye == 1), last = eye >= 0 && (afw || eye == 0);   // the frame's first / last eye render
    hook_context();
    if (g_trace10 > 0) logf("TRACE10 render thread: RenderViewFamily %p eye %d begin", r, eye);
    if (eye >= 0 && g_rainMask) { ID3D11DeviceContext* ic = nullptr; if (g_dev) g_dev->GetImmediateContext(&ic); if (ic) { rain_mask_clear(ic, eye); ic->Release(); } }   // RAINMASK
    if (first && g_afterEye0) { g_afterEye0 = false; g_afterLogs = 0; }
    {   // RANDSYNC (render thread): the right eye renders first - the left eye's render starts from the same dice
        static RandSave s_rt{}; static bool s_have = false;
        if (eye == 1 && !afw) { s_rt = rand_save(); s_have = true; }
        else if (eye == 0 && !afw && s_have && g_randSync) { rand_restore(s_rt); s_have = false; InterlockedIncrement(&c_randRender); }
    }
    if (first)
    {
        InterlockedIncrement(&g_rainPair);   // RAINPAIR: a new stereo pair begins
        g_hudPhase = false;   // the previous frame's HUD is complete now (also ended by akn_present)
        if (g_hrOn && g_hudTex && g_dev) { ID3D11DeviceContext* rc = nullptr; g_dev->GetImmediateContext(&rc); if (rc) { hud_ring_frame(rc); rc->Release(); } }   // HUDRING
        if (g_recOn && g_hudTex && g_dev) { ID3D11DeviceContext* cc = nullptr; g_dev->GetImmediateContext(&cc); if (cc) { hud_cover_frame(cc); cc->Release(); } }   // HUDCOVER
        if (g_bhStep >= 0 && g_hudTex && g_dev) { ID3D11DeviceContext* bc = nullptr; g_dev->GetImmediateContext(&bc); if (bc) { black_hunt_frame(bc); bc->Release(); } }   // BLACKHUNT
        if (g_capHud && g_hudTex && g_dev)
        {
            g_capHud = 0;
            ID3D11DeviceContext* c = nullptr; g_dev->GetImmediateContext(&c);
            if (c) { save_bmp(c, g_hudTex, 0, -1, L"hud_rgb"); save_bmp(c, g_hudTex, 0, -1, L"hud_alpha", true);
                     // PROMPTCAP 2026-10-11 (JJ: "A to rescue" and other hints never show): world-attached pieces go to the
                     // per-eye marker layers, which no capture saved - a prompt routed there was invisible to every check
                     if (g_mkEye) for (int k = 0; k < 2; ++k) if (g_mkEyeTex[k]) save_bmp(c, g_mkEyeTex[k], 0, -1, k ? L"mk_right" : L"mk_left");
                     c->Release(); }
        }
    }
    tl_trace = eye >= 0 && (g_traceWant & (1 << eye)) != 0;
    if (tl_trace) { InterlockedAnd(&g_traceWant, ~(1 << eye)); logf("TRACE eye %d render begins", eye); g_rtListLogs = 1; }
    tl_eye_render = eye; tl_lumN = 0; tl_lastCopyDst = nullptr; tl_hN = 0;
    if (eye >= 0 && g_histSwap && g_dev) { ID3D11DeviceContext* hc = nullptr; g_dev->GetImmediateContext(&hc); if (hc) { hist_swap(hc, eye, true); hc->Release(); } }
    tl_inEyeRender = eye >= 0; tl_lastRT = nullptr; tl_rtSets = 0; tl_rtList[0] = 0; tl_prevRT = nullptr; tl_bigN = 0;
    if (eye >= 0) g_eyeNow = eye;   // DEFERRED: worker threads record for this eye
    GpuT* gq = nullptr; ID3D11DeviceContext* gc = nullptr;   // DLSS D0 timing
    if (eye >= 0 && g_d0 && g_dev) { g_dev->GetImmediateContext(&gc); if (gc) gq = gt_begin(g_dev, gc, eye); }
    const LONG64 tEye0 = qpc();
    o_RenderVF(r);
    if (eye >= 0) { InterlockedAdd64(&t_eyeCpu[eye], qpc() - tEye0); InterlockedIncrement(&c_eyeCpu[eye]); }
    if (gc) { gt_end(gc, gq); gc->Release(); }
    g_eyeNow = -1;
    tl_inEyeRender = false;
    if (eye >= 0) g_lumN[eye] = tl_lumN;
    if (eye >= 0)
    {
        hist_end_render(eye);
        if (g_histSwap && g_dev) { ID3D11DeviceContext* hc = nullptr; g_dev->GetImmediateContext(&hc); if (hc) { hist_swap(hc, eye, false); hc->Release(); } }
    }
    // BBSTEADY 2026-10-09: a capture's GPU stall made the eye render's last full-size copy land in a 1520x1656 f26 scene
    // buffer for ONE frame (15:46:57 / 15:47:02); taking it as the window buffer rebuilt the pair in f26 = a blown-out
    // frame in the headset. A window buffer of another format must be the last copy in 3 eye renders running.
    static ID3D11Resource* s_bbCand = nullptr; static int s_bbCandN = 0;
    bool bbTake = eye >= 0 && tl_lastCopyDst && tl_lastCopyDst != g_bb;
    if (bbTake && g_bb)
    {
        D3D11_RESOURCE_DIMENSION d1, d2; tl_lastCopyDst->GetType(&d1); g_bb->GetType(&d2);
        DXGI_FORMAT f1 = DXGI_FORMAT_UNKNOWN, f2 = DXGI_FORMAT_UNKNOWN;
        if (d1 == D3D11_RESOURCE_DIMENSION_TEXTURE2D) { D3D11_TEXTURE2D_DESC t; ((ID3D11Texture2D*)tl_lastCopyDst)->GetDesc(&t); f1 = t.Format; }
        if (d2 == D3D11_RESOURCE_DIMENSION_TEXTURE2D) { D3D11_TEXTURE2D_DESC t; ((ID3D11Texture2D*)g_bb)->GetDesc(&t); f2 = t.Format; }
        if (f1 != f2)
        {
            s_bbCandN = (s_bbCand == tl_lastCopyDst) ? s_bbCandN + 1 : 1; s_bbCand = tl_lastCopyDst;
            if (s_bbCandN < 3) { bbTake = false; static LONG s_n = 0; if (InterlockedIncrement(&s_n) <= 20) logf("window buffer: ignored a one-off copy into format %d (keeping %d)", (int)f1, (int)f2); }
        }
    }
    if (eye >= 0 && tl_lastCopyDst == g_bb) s_bbCandN = 0;
    if (bbTake)
    {
        char dsc[96]; res_desc(tl_lastCopyDst, dsc, sizeof(dsc));
        logf("window buffer = %s (the eye render's final copy)", dsc);
        g_bb = tl_lastCopyDst;
    }
    tl_eye_render = -1;
    if (tl_trace) { tl_trace = false; logf("TRACE eye %d render ends (%d target sets)", eye, tl_rtSets); }
    rain_dump_end(eye);   // RAINDUMP
    if (last) xe_end_frame();   // XEYE
    if (last && g_rainDrawDump > 0 && g_dev) { ID3D11DeviceContext* ic = nullptr; g_dev->GetImmediateContext(&ic); if (ic) { rain_draw_readback(ic); ic->Release(); } }   // RAINVP
    if (last && g_cbSnapPending && g_dev) { ID3D11DeviceContext* ic = nullptr; g_dev->GetImmediateContext(&ic); if (ic) { rain_cb_readback(ic); ic->Release(); } }   // RAINCB
    if (last && g_rtDone) SetEvent(g_rtDone);   // RTWAIT
    vs_inv_end(eye);   // VSINV
    ps_inv_end(eye);   // DETECTIVE
    if (g_fxcbArm) fxcb_end(eye);   // FXCB
    if (last && g_afterLogs > 0) g_afterEye0 = true;
    if (last) { g_eye0Done = true; if (g_hudOn) { if (!g_hudAll) g_hudClearPending = true; g_hudPhase = true; } }   // HUDTRACE: hudall clears at akn_present
    if (eye >= 0 && g_rtListLogs > 0) { --g_rtListLogs; logf("eye %d render targets in order:%s", eye, tl_rtList); }
    if (g_trace10 > 0) { logf("TRACE10 render thread: RenderViewFamily %p eye %d end", r, eye); if (last) InterlockedDecrement(&g_trace10); }
    if (eye >= 0) { InterlockedIncrement(&c_rtEye[eye]); __try { after_eye_render(eye); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (eye >= 0 && g_hooked && !g_dev)
    {
        static int s_miss = 0;
        if (++s_miss == 240) logf("HOOKS MISS THE GAME: 240 eye renders and the hooked functions never ran on the game's context (wrong device table) - no pairs will be made");
    }
    else InterlockedIncrement(&c_rtOther);
}
// the game's D3D11 device (the probe loads at process start, before the game makes it)
using CreateDevFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using CreateDevSCFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
CreateDevFn o_CreateDev = nullptr; CreateDevSCFn o_CreateDevSC = nullptr;
// M5c crash (2026-10-07): under AKVR a throwaway device (kiero's vtable probe) is created and released at start-up;
// taking the FIRST device made hook_context call a dead one. Only the game's own call (from BatmanAK.exe) counts,
// and we hold a reference so it can never dangle.
bool from_game(void* ra)
{
    static uintptr_t lo = 0, hi = 0;
    if (!lo) { const auto* dos = (const IMAGE_DOS_HEADER*)g_base; const auto* nt = (const IMAGE_NT_HEADERS*)(g_base + dos->e_lfanew); lo = g_base; hi = g_base + nt->OptionalHeader.SizeOfImage; }
    return (uintptr_t)ra >= lo && (uintptr_t)ra < hi;
}
void take_device(ID3D11Device* d, void* ra, const char* via)
{
    // M5c run 2: AKVR's kiero probe makes a device FIRST, the game's comes after; the context hooks must sit on the
    // game's functions, so the most recent device wins (each held: a dead probe device crashed us in run 1)
    logf("D3D11 device %p (%s, caller %s %p) - held; the latest one is used for hooking", (void*)d, via, from_game(ra) ? "the game" : "another module", ra);
    if (d == g_hookDev) return;
    d->AddRef(); g_hookDev = d;
}
HRESULT WINAPI hk_CreateDev(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE m, UINT f, const D3D_FEATURE_LEVEL* fl, UINT n, UINT v, ID3D11Device** d, D3D_FEATURE_LEVEL* ofl, ID3D11DeviceContext** c)
{
    void* ra = _ReturnAddress();
    const HRESULT hr = o_CreateDev(a, t, m, f, fl, n, v, d, ofl, c);
    if (SUCCEEDED(hr) && d && *d) take_device(*d, ra, "D3D11CreateDevice");
    return hr;
}
HRESULT WINAPI hk_CreateDevSC(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE m, UINT f, const D3D_FEATURE_LEVEL* fl, UINT n, UINT v, const DXGI_SWAP_CHAIN_DESC* sd, IDXGISwapChain** sc, ID3D11Device** d, D3D_FEATURE_LEVEL* ofl, ID3D11DeviceContext** c)
{
    void* ra = _ReturnAddress();
    const HRESULT hr = o_CreateDevSC(a, t, m, f, fl, n, v, sd, sc, d, ofl, c);
    if (SUCCEEDED(hr) && d && *d) take_device(*d, ra, "D3D11CreateDeviceAndSwapChain");
    return hr;
}
volatile LONG g_rogue = 0; volatile LONG c_rogueOps = 0;
DWORD WINAPI rogue_thread(LPVOID)
{
    HMODULE m = GetModuleHandleW(L"d3d11.dll");
    auto create = m ? (CreateDevFn)GetProcAddress(m, "D3D11CreateDevice") : nullptr;
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    const UINT flags = g_rogue == 2 ? D3D11_CREATE_DEVICE_SINGLETHREADED : 0;   // 2: a different context table, like VD's
    if (!create || FAILED(create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx)))
    { logf("rogue device: create FAILED"); return 0; }
    D3D11_TEXTURE2D_DESC d{}; d.Width = 2048; d.Height = 2048; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    d.SampleDesc.Count = 1; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *a = nullptr, *b = nullptr; ID3D11RenderTargetView* v = nullptr;
    dev->CreateTexture2D(&d, nullptr, &a); dev->CreateTexture2D(&d, nullptr, &b); if (a) dev->CreateRenderTargetView(a, nullptr, &v);
    logf("rogue device %p running (%s; 2048x2048 sRGB binds, clears and copies every 3 ms on its own context, table %p)", (void*)dev, flags ? "single-threaded" : "thread-safe", (void*)*(void***)ctx);
    const FLOAT col[4] = { 1, 0, 1, 1 };
    while (g_rogue && v && b)
    {
        ctx->OMSetRenderTargets(1, &v, nullptr); ctx->ClearRenderTargetView(v, col); ctx->CopyResource(b, a); ctx->Flush();
        InterlockedIncrement(&c_rogueOps); Sleep(3);
    }
    if (v) v->Release(); if (a) a->Release(); if (b) b->Release(); ctx->Release(); dev->Release();
    logf("rogue device stopped");
    return 0;
}
void hook_d3d()
{
    HMODULE m = LoadLibraryW(L"d3d11.dll");
    if (!m) { logf("d3d11.dll not loaded"); return; }
    void* a = (void*)GetProcAddress(m, "D3D11CreateDevice"); void* b = (void*)GetProcAddress(m, "D3D11CreateDeviceAndSwapChain");
    if (a && MH_CreateHook(a, (void*)&hk_CreateDev, (void**)&o_CreateDev) == MH_OK) MH_EnableHook(a);
    if (b && MH_CreateHook(b, (void*)&hk_CreateDevSC, (void**)&o_CreateDevSC) == MH_OK) MH_EnableHook(b);
    logf("D3D11 create hooks in");
}

bool sig_ok(uintptr_t rva, const char* hex)
{
    const uint8_t* p = (const uint8_t*)(g_base + rva);
    for (size_t i = 0; hex[2 * i]; ++i)
    {
        unsigned v = 0; sscanf_s(hex + 2 * i, "%2x", &v);
        if (p[i] != (uint8_t)v) return false;
    }
    return true;
}
// BATEVENT 2026-10-13 (JJ: the bat transition between menus / into gameplay "needs to be full field of view and closer to the
// camera, same as all the other mods"; REC1: the bats are drawn on the HUD sheet and share textures + shaders with the menu text,
// so no draw identifies them): the game's own sound - BatmanAK.exe EXPORTS Wwise AK::SoundEngine::PostEvent (id / char / wchar),
// as Arkham Origins does (Origins O1D: Play_UI_Fader_Bats = 0xE8BB4D2C). Every new event id is logged once (names too) so the
// fade-to-black's event can be found; akn_bat_age_ms() tells AKVR how long ago the bats started.
using AkPostIdFn = uint32_t(__cdecl*)(uint32_t, uint64_t, uint32_t, void*, void*, uint32_t, void*, uint32_t);
using AkPostAFn = uint32_t(__cdecl*)(const char*, uint64_t, uint32_t, void*, void*, uint32_t, void*, uint32_t);
using AkPostWFn = uint32_t(__cdecl*)(const wchar_t*, uint64_t, uint32_t, void*, void*, uint32_t, void*, uint32_t);
using AkHashFn = uint32_t(__cdecl*)(const char*);
AkPostIdFn o_akPostId = nullptr; AkPostAFn o_akPostA = nullptr; AkPostWFn o_akPostW = nullptr; AkHashFn p_akHash = nullptr;
volatile uint32_t g_batId = 0; volatile LONGLONG g_batAt = 0; volatile LONG c_batEvents = 0, g_akSeenN = 0, g_akHooked = 0;
uint32_t g_akSeen[512] = {};
void ak_event(uint32_t id, const char* name, const wchar_t* wname)
{
    const ULONGLONG now = GetTickCount64();
    if (id && id == g_batId) { const LONGLONG prev = InterlockedExchange64(&g_batAt, (LONGLONG)now); if (!prev || now - (ULONGLONG)prev > 300) { InterlockedIncrement(&c_batEvents); logf("BATEVENT: Play_UI_Fader_Bats #%ld", c_batEvents); } }
    if (!id) return;
    for (LONG i = 0; i < g_akSeenN; ++i) if (g_akSeen[i] == id) return;
    if (g_akSeenN >= 512) return;
    g_akSeen[InterlockedIncrement(&g_akSeenN) - 1] = id;
    char nm[96] = "";
    __try { if (name) strncpy_s(nm, name, _TRUNCATE); else if (wname) { int k = 0; for (; k < 95 && wname[k]; ++k) nm[k] = (char)wname[k]; nm[k] = 0; } } __except (EXCEPTION_EXECUTE_HANDLER) { nm[0] = 0; }
    logf("AKEVENT new: id %u %s%s", id, nm[0] ? "name " : "(posted by id)", nm);
}
uint32_t __cdecl hk_akPostId(uint32_t id, uint64_t obj, uint32_t fl, void* cb, void* ck, uint32_t n, void* ext, uint32_t pl) { ak_event(id, nullptr, nullptr); return o_akPostId(id, obj, fl, cb, ck, n, ext, pl); }
uint32_t __cdecl hk_akPostA(const char* nm, uint64_t obj, uint32_t fl, void* cb, void* ck, uint32_t n, void* ext, uint32_t pl) { uint32_t id = 0; __try { id = (p_akHash && nm) ? p_akHash(nm) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { id = 0; } ak_event(id, nm, nullptr); return o_akPostA(nm, obj, fl, cb, ck, n, ext, pl); }
uint32_t __cdecl hk_akPostW(const wchar_t* nm, uint64_t obj, uint32_t fl, void* cb, void* ck, uint32_t n, void* ext, uint32_t pl)
{
    uint32_t id = 0; char a[96] = "";
    __try { if (nm) { int k = 0; for (; k < 95 && nm[k]; ++k) a[k] = (char)nm[k]; a[k] = 0; id = p_akHash ? p_akHash(a) : 0; } } __except (EXCEPTION_EXECUTE_HANDLER) { id = 0; }
    ak_event(id, a[0] ? a : nullptr, nullptr); return o_akPostW(nm, obj, fl, cb, ck, n, ext, pl);
}
void ak_hooks_queue()   // HOOKBATCH: queued, switched on with the start-up batch
{
    HMODULE exe = GetModuleHandleW(nullptr);
    p_akHash = (AkHashFn)GetProcAddress(exe, "?GetIDFromString@SoundEngine@AK@@YAKPEBD@Z");
    void* t[3] = { (void*)GetProcAddress(exe, "?PostEvent@SoundEngine@AK@@YAKK_KKP6AXW4AkCallbackType@@PEAUAkCallbackInfo@@@ZPEAXKPEAUAkExternalSourceInfo@@K@Z"),
                   (void*)GetProcAddress(exe, "?PostEvent@SoundEngine@AK@@YAKPEBD_KKP6AXW4AkCallbackType@@PEAUAkCallbackInfo@@@ZPEAXKPEAUAkExternalSourceInfo@@K@Z"),
                   (void*)GetProcAddress(exe, "?PostEvent@SoundEngine@AK@@YAKPEB_W_KKP6AXW4AkCallbackType@@PEAUAkCallbackInfo@@@ZPEAXKPEAUAkExternalSourceInfo@@K@Z") };
    if (!p_akHash || !t[0] || !t[1] || !t[2]) { logf("BATEVENT: Wwise exports not found - no bat trigger"); return; }
    uint32_t id = 0; __try { id = p_akHash("Play_UI_Fader_Bats"); } __except (EXCEPTION_EXECUTE_HANDLER) { id = 0; }
    g_batId = id;
    logf("BATEVENT: Play_UI_Fader_Bats = %u (0x%08X; Origins / City 0xE8BB4D2C)", id, id);
    void* d[3] = { (void*)&hk_akPostId, (void*)&hk_akPostA, (void*)&hk_akPostW }; void** o[3] = { (void**)&o_akPostId, (void**)&o_akPostA, (void**)&o_akPostW };
    int ok = 0; for (int i = 0; i < 3; ++i) if (MH_CreateHook(t[i], d[i], o[i]) == MH_OK && MH_QueueEnableHook(t[i]) == MH_OK) ++ok;
    g_akHooked = ok; logf("BATEVENT: %d of 3 Wwise PostEvent hooks queued", ok);
}
extern "C" __declspec(dllexport) long akn_bat_age_ms() { const LONGLONG at = g_batAt; return at ? (long)(GetTickCount64() - (ULONGLONG)at) : -1; }   // BATEVENT (AKVR)
// MENUSTATE 2026-10-13 (RB1: a transition signal must come from the GAME's state - City MENUSTATE: UGFxMoviePlayer Start / Close
// natives found by name in UE3's native table). AK (64-bit UE3): the table entry is { const char* "<Class>exec<Func>"; impl } (16
// bytes); the names exist in BatmanAK.exe (UGFxMoviePlayerexecStart / Close, URGFxMovieUIexecStart, RGFxMovieFrontMostexecShouldFade).
// Each call is logged with the movie object's class + name (GNames found live: entry 0 "None", entry 1 "ByteProperty").
namespace ms {
uint8_t* g_namesArr = nullptr; int g_textOff = -1; bool g_wide = false; int g_objNameOff = -1; bool g_namesTried = false;
bool rd(const void* p, size_t n) { MEMORY_BASIC_INFORMATION mi{}; if (!p || !VirtualQuery(p, &mi, sizeof(mi))) return false; if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false; return (const uint8_t*)p + n <= (const uint8_t*)mi.BaseAddress + mi.RegionSize; }
bool text_is(const uint8_t* p, const char* t, bool wide) { const size_t n = strlen(t) + 1; if (!rd(p, wide ? 2 * n : n)) return false; for (size_t i = 0; i < n; ++i) if ((wide ? ((const uint16_t*)p)[i] : p[i]) != (uint8_t)t[i]) return false; return true; }
bool name_text(int idx, char* out, size_t n)
{
    out[0] = 0; if (!g_namesArr || idx < 0) return false;
    __try {
        uint8_t** data = *(uint8_t***)g_namesArr; const int num = *(const int*)(g_namesArr + 8);
        if (idx >= num || !rd(data + idx, 8)) return false;
        const uint8_t* e = data[idx]; if (!e) return false;
        const uint8_t* t = e + g_textOff; const size_t cw = g_wide ? 2 : 1; size_t i = 0;
        for (; i + 1 < n; ++i) { if (!rd(t + i * cw, cw)) return false; const unsigned c = g_wide ? ((const uint16_t*)t)[i] : t[i]; if (!c) break; if (c < 0x20 || c > 0x7E) return false; out[i] = (char)c; }
        out[i] = 0; return i > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; return false; }
}
bool find_names()
{
    const uint8_t* base = (const uint8_t*)GetModuleHandleW(nullptr);
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned si = 0; si < nt->FileHeader.NumberOfSections; ++si, ++sec)
    {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE) || (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* lo = (uint8_t*)base + sec->VirtualAddress; const size_t len = sec->Misc.VirtualSize & ~(size_t)7;
        for (size_t o = 0; o + 16 <= len; o += 8)
        {
            uint8_t* p = lo + o; if ((o & 0xFFF) == 0 && !rd(p, 16)) { o += 0xFF8; continue; }
            __try {
                const int num = *(const int*)(p + 8), mx = *(const int*)(p + 12);
                if (num < 2000 || num > 5000000 || mx < num || mx > 8000000) continue;
                uint8_t** data = *(uint8_t***)p; if (!rd(data, 16)) continue;
                const uint8_t* e0 = data[0]; const uint8_t* e1 = data[1]; if (!rd(e0, 16) || !rd(e1, 16)) continue;
                for (int w = 0; w < 2; ++w) for (int off = 0x4; off <= 0x20; off += 4)
                    if (text_is(e0 + off, "None", w != 0) && text_is(e1 + off, "ByteProperty", w != 0)) { g_wide = w != 0; g_textOff = off; g_namesArr = p; logf("MENUSTATE: name table at %p (%d names, %s at entry +0x%X)", p, num, w ? "UTF-16" : "ANSI", off); return true; }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
    return false;
}
void describe(const void* o, char* out, size_t n)
{
    snprintf(out, n, "%p", o);
    if (!g_namesArr) { if (g_namesTried) return; g_namesTried = true; if (!find_names()) { logf("MENUSTATE: name table not found"); return; } }
    const uint8_t* obj = (const uint8_t*)o; char t[96], nm[96] = "?", cl[96] = "?";
    __try {
        if (g_objNameOff < 0)
            for (int off = 0x10; off <= 0x80 && g_objNameOff < 0; off += 4)
            {
                if (!rd(obj + off, 16)) break;
                const uint8_t* cls = *(const uint8_t* const*)(obj + off + 8);
                if (!name_text(*(const int*)(obj + off), t, sizeof t) || !rd(cls, off + 16)) continue;
                const uint8_t* meta = *(const uint8_t* const*)(cls + off + 8);
                if (!name_text(*(const int*)(cls + off), t, sizeof t) || !rd(meta, off + 16)) continue;
                if (name_text(*(const int*)(meta + off), t, sizeof t) && strcmp(t, "Class") == 0) { g_objNameOff = off; logf("MENUSTATE: object name at +0x%X, class at +0x%X", off, off + 8); }
            }
        if (g_objNameOff >= 0 && rd(obj + g_objNameOff, 16))
        {
            name_text(*(const int*)(obj + g_objNameOff), nm, sizeof nm);
            const uint8_t* c = *(const uint8_t* const*)(obj + g_objNameOff + 8); if (rd(c, g_objNameOff + 8)) name_text(*(const int*)(c + g_objNameOff), cl, sizeof cl);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    snprintf(out, n, "%s %s (%p)", cl, nm, o);
}
// the exec thunk's address from the native table: name string in .rdata -> 8-byte reference in .data -> impl 8 bytes later
void* find_native(const char* full)
{
    const uint8_t* base = (const uint8_t*)GetModuleHandleW(nullptr);
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    const size_t fl = strlen(full) + 1; const uint8_t* str = nullptr;
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned si = 0; si < nt->FileHeader.NumberOfSections && !str; ++si, ++sec)
    {
        if (sec->Characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_WRITE)) continue;
        const uint8_t* lo = base + sec->VirtualAddress; const size_t len = sec->Misc.VirtualSize;
        for (size_t o = 1; o + fl <= len; ++o) if (lo[o] == (uint8_t)full[0] && lo[o - 1] == 0 && memcmp(lo + o, full, fl) == 0) { str = lo + o; break; }
    }
    if (!str) return nullptr;
    const uint8_t* text = nullptr; size_t textLen = 0;
    sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned si = 0; si < nt->FileHeader.NumberOfSections; ++si, ++sec) if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) { text = base + sec->VirtualAddress; textLen = sec->Misc.VirtualSize; break; }
    sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned si = 0; si < nt->FileHeader.NumberOfSections; ++si, ++sec)
    {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        const uint8_t* lo = base + sec->VirtualAddress; const size_t len = sec->Misc.VirtualSize & ~(size_t)7;
        for (size_t o = 0; o + 16 <= len; o += 8)
        {
            if (*(const uint8_t* const*)(lo + o) != str) continue;
            void* impl = *(void* const*)(lo + o + 8);
            if (text && (const uint8_t*)impl >= text && (const uint8_t*)impl < text + textLen) return impl;
        }
    }
    return nullptr;
}
using ExecFn = void(__fastcall*)(void*, void*, void*);
struct T { const char* full; ExecFn orig; LONG n; };
T g_t[] = { { "UGFxMoviePlayerexecStart", nullptr, 0 }, { "UGFxMoviePlayerexecClose", nullptr, 0 }, { "URGFxMovieUIexecStart", nullptr, 0 }, { "RGFxMovieFrontMostexecShouldFade", nullptr, 0 } };
template <int I> void __fastcall det(void* self, void* stack, void* result)
{
    const LONG n = InterlockedIncrement(&g_t[I].n);
    if (n <= 200) { char who[200]; describe(self, who, sizeof who); logf("MENUSTATE %s on %s (#%ld)", g_t[I].full, who, n); }
    g_t[I].orig(self, stack, result);
}
void* const g_det[] = { (void*)&det<0>, (void*)&det<1>, (void*)&det<2>, (void*)&det<3> };
void queue()
{
    int ok = 0;
    for (int i = 0; i < 4; ++i)
    {
        void* f = find_native(g_t[i].full);
        if (!f) { logf("MENUSTATE: %s not found in the native table", g_t[i].full); continue; }
        if (MH_CreateHook(f, g_det[i], (void**)&g_t[i].orig) == MH_OK && MH_QueueEnableHook(f) == MH_OK) { ++ok; logf("MENUSTATE: %s at RVA %llX queued", g_t[i].full, (unsigned long long)((uint8_t*)f - (uint8_t*)GetModuleHandleW(nullptr))); }
    }
    logf("MENUSTATE: %d of 4 menu natives queued", ok);
}
}   // namespace ms
bool hook(uintptr_t rva, const char* sig, void* det, void** orig, const char* name)
{
    if (!sig_ok(rva, sig)) { logf("SIGNATURE MISMATCH %s at %llX - not hooked", name, (unsigned long long)rva); return false; }
    void* t = (void*)(g_base + rva);
    if (MH_CreateHook(t, det, orig) != MH_OK || MH_QueueEnableHook(t) != MH_OK) { logf("hook FAILED %s", name); return false; }   // HOOKBATCH: applied after the list
    logf("hooked %s at %llX", name, (unsigned long long)rva);
    return true;
}

double g_qpf = 1.0;

// GPU load straight from the driver (NVML, System32 nvml.dll), sampled every 100 ms by the worker and averaged per
// test phase - run 6 had no external nvidia-smi log. busy% x graphics clock = a work figure that survives the card
// changing clocks at a 30 fps cap.
struct NvUtil { unsigned gpu, mem; };
struct NvMem { unsigned long long total, free, used; };
using NvInitFn = int(*)(); using NvHandleFn = int(*)(unsigned, void**);
using NvUtilFn = int(*)(void*, NvUtil*); using NvClockFn = int(*)(void*, int, unsigned*); using NvMemFn = int(*)(void*, NvMem*);
NvUtilFn nv_util = nullptr; NvClockFn nv_clock = nullptr; NvMemFn nv_mem = nullptr; void* g_nvDev = nullptr;
void nv_start()
{
    HMODULE m = LoadLibraryW(L"nvml.dll");
    if (!m) { logf("NVML: nvml.dll not found - no GPU figures"); return; }
    auto init = (NvInitFn)GetProcAddress(m, "nvmlInit_v2"); auto h = (NvHandleFn)GetProcAddress(m, "nvmlDeviceGetHandleByIndex_v2");
    nv_util = (NvUtilFn)GetProcAddress(m, "nvmlDeviceGetUtilizationRates"); nv_clock = (NvClockFn)GetProcAddress(m, "nvmlDeviceGetClockInfo");
    nv_mem = (NvMemFn)GetProcAddress(m, "nvmlDeviceGetMemoryInfo");
    if (!init || !h || !nv_util || !nv_clock || !nv_mem || init() != 0 || h(0, &g_nvDev) != 0) { g_nvDev = nullptr; logf("NVML: init failed - no GPU figures"); return; }
    logf("NVML: GPU figures on");
}
struct GpuAcc { double busy = 0, clk = 0, work = 0, memMB = 0; int n = 0; };
GpuAcc g_gpuPhase, g_gpuSec;
void gpu_sample()
{
    if (!g_nvDev) return;
    NvUtil u{}; unsigned c = 0; NvMem mm{};
    if (nv_util(g_nvDev, &u) != 0 || nv_clock(g_nvDev, 0, &c) != 0 || nv_mem(g_nvDev, &mm) != 0) return;
    GpuAcc* accs[2] = { &g_gpuPhase, &g_gpuSec };
    for (GpuAcc* a : accs) { a->busy += u.gpu; a->clk += c; a->work += u.gpu * c / 100.0; a->memMB += mm.used / 1048576.0; ++a->n; }
}
void gpu_report(const char* tag, GpuAcc& a)
{
    if (a.n) logf("%s GPU busy %.1f%% | clock %.0f MHz | work %.0f (busy x MHz) | VRAM %.0f MB | %d samples", tag, a.busy / a.n, a.clk / a.n, a.work / a.n, a.memMB / a.n, a.n);
    a = GpuAcc{};
}
struct Snap { LONG draw, redraw, calc, cdA, cdB, second, fail, calc1, calc2; LONG64 tDraw, tRedraw, tSecond, tA; ULONGLONG cpu100ns; ULONGLONG tick; LONG scene; LONG64 tScene; LONG render; LONG64 tRender; LONG brvf; LONG64 tBrvf; };
Snap snap()
{
    Snap s{ c_draw, c_redraw, c_calc, c_cdA, c_cdB, c_second, c_secondFail, c_calcFirstPass, c_calcSecondPass, t_draw, t_redraw, t_second, t_cdA, 0, GetTickCount64(), c_scene, t_scene, c_render, t_render, c_brvf, t_brvf };
    FILETIME a, b, k, u; GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u);
    s.cpu100ns = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
    return s;
}
void report(const char* tag, const Snap& a, const Snap& b)
{
    const double sec = (b.tick - a.tick) / 1000.0;
    if (sec <= 0) return;
    const LONG fr = b.redraw - a.redraw, dr = b.draw - a.draw;
    const double ms = 1000.0 / g_qpf;
    logf("%s %.1fs | frames/s %.1f | WORLD/s %.1f, ms/world %.3f | render/s %.1f, ms/render %.2f | scene/s %.1f, ms/scene %.2f | Draw/s %.1f, ms/Draw %.2f | Redraw ms/frame %.2f | second/s %.1f (ms %.2f, faults %ld) | "
         "CalcSceneView/s %.1f (in 1st pass %.2f/frame, in 2nd %.2f/second) | clientDraw A/s %.1f (ms %.2f) B/s %.1f | CPU %.0f%% of one core",
         tag, sec, fr / sec, (b.brvf - a.brvf) / sec, (b.brvf - a.brvf) ? (b.tBrvf - a.tBrvf) * ms / (b.brvf - a.brvf) : 0.0, (b.render - a.render) / sec, (b.render - a.render) ? (b.tRender - a.tRender) * ms / (b.render - a.render) : 0.0, (b.scene - a.scene) / sec, (b.scene - a.scene) ? (b.tScene - a.tScene) * ms / (b.scene - a.scene) : 0.0, dr / sec, dr ? (b.tDraw - a.tDraw) * ms / dr : 0.0, fr ? (b.tRedraw - a.tRedraw) * ms / fr : 0.0,
         (b.second - a.second) / sec, (b.second - a.second) ? (b.tSecond - a.tSecond) * ms / (b.second - a.second) : 0.0, b.fail,
         (b.calc - a.calc) / sec, fr ? double(b.calc1 - a.calc1) / fr : 0.0,
         (b.second - a.second) ? double(b.calc2 - a.calc2) / (b.second - a.second) : 0.0,
         (b.cdA - a.cdA) / sec, (b.cdA - a.cdA) ? (b.tA - a.tA) * ms / (b.cdA - a.cdA) : 0.0, (b.cdB - a.cdB) / sec, (b.cpu100ns - a.cpu100ns) / 1e5 / sec);
}
bool game_in_front()
{
    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

DWORD WINAPI worker(LPVOID)
{
    // the exe is not packed (AKVR patches it from DllMain), but give the loader time to finish
    MH_Initialize();
    hook_d3d();
    Sleep(500);
    nv_start();
    g_raTickRedraw = g_base + 0xAF282C;   // LIVE 2026-10-06: the per-frame caller (AF0044 ran once)
    g_raRedrawDraw = g_base + 0xAD3EE5;
    bool ok = true;
    ok &= hook(0xBF8000, "488bc45556574154415541564157488d", (void*)&hk_Draw, (void**)&o_Draw, "FViewport::Draw");
    ok &= hook(0xAD3EA0, "48895c241048896c2418574883ec2048", (void*)&hk_Redraw, (void**)&o_Redraw, "RedrawViewports");
    ok &= hook(0x9F6220, "488bc448895010555657415441554156", (void*)&hk_Calc, (void**)&o_Calc, "CalcSceneView");
    ok &= hook(0x9F87B0, "488bc44c894018488950105553565741", (void*)&hk_CdA, (void**)&o_CdA, "ClientDraw A");
    g_raTickScene = g_base + 0xAAA506;
    g_raLateRender = g_base + 0x9F1449;
    ok &= hook(0x8E9E90, "488bc444894018488950104889480855", (void*)&hk_Render, (void**)&o_Render, "render view family");
    g_raBrvf = g_base + 0x9F1563;
    ok &= hook(0x81B670, "488bc4554154415541564157488d6898", (void*)&hk_Brvf, (void**)&o_Brvf, "BeginRenderingViewFamily");
    g_raCalcA = g_base + 0x9F8B3A;
    g_raCtorCalc = g_base + 0x9F7301;
    g_jitIdx = (volatile uint32_t*)(g_base + 0x3098180);
    if (sig_ok(0x82F860, "40574883ec3048c7442420feffffff48")) p_AllocVs = (AllocVsFn)(g_base + 0x82F860);
    else { logf("SIGNATURE MISMATCH AllocateViewState"); ok = false; }
    if (sig_ok(0x82CE20, "488bc44889480855565741544155488d")) p_VsCtor = (VsCtorFn)(g_base + 0x82CE20); else { logf("SIGNATURE MISMATCH view state ctor"); ok = false; }
    p_FamDtor = (FamDtorFn)(g_base + 0x823E10); p_Free = (FreeFn)(g_base + 0xFA2A80); p_Malloc = (MallocFn)(g_base + 0xFA2A40);
    if (!sig_ok(0x823E10, "48894c2408574883ec3048c7442420fe") || !sig_ok(0xFA2A40, "448bc9488b0d")) { logf("SIGNATURE MISMATCH family dtor / malloc"); ok = false; }
    ok &= hook(0x818530, "488bc44c894018488948085556574154", (void*)&hk_RCtor, (void**)&o_RCtor, "scene renderer ctor");
    ok &= hook(0x81B3C0, "488bc455488bec4883ec6048c745c0fe", (void*)&hk_RenderVF, (void**)&o_RenderVF, "RenderViewFamily (render thread)");
    ok &= hook(0x82EB30, "48894c2408574883ec3048c7442420fe", (void*)&hk_Ctor, (void**)&o_Ctor, "FSceneView ctor");
    hook(0x81FF10, "4883ec384c8bca488d542420e8afd3ff", (void*)&hk_Proj, (void**)&o_Proj, "FSceneView::Project (markers)");   // MARKERPROBE (optional)
    { const wchar_t* c = wcsstr(GetCommandLineW(), L"aknipd="); if (c) g_ipd = (float)_wtof(c + 7); }
    if (wcsstr(GetCommandLineW(), L"-aknstereo")) g_stereo = 1;
    { const wchar_t* c = wcsstr(GetCommandLineW(), L"aknvs="); if (c) g_vsMode = _wtoi(c + 6); }
    logf("exports ready (akn_api_version 1)");
    logf("engine stereo %s at start, eye separation %.2f units, second view state mode %ld (F9 / command.txt)", g_stereo ? "ON" : "off", g_ipd, g_vsMode);
    ok &= hook(0xBDDEC0, "40574883ec3048c7442420feffffff48", (void*)&hk_Scene, (void**)&o_Scene, "scene draw entry");
    ok &= hook(0x41BF00, "488bc448894808555356574154415541", (void*)&hk_CdB, (void**)&o_CdB, "ClientDraw B");
    // (BATEVENT / AKEVENT Wwise hooks and MENUSTATE movie hooks not installed - release tidy 2026-10-14; bats parked, HUD_REBUILD_PLAN HR4)
    {   // HOOKBATCH: the start-up hooks switched on together (each one alone froze the game's threads for ~0.12 s)
        const LONG64 t0 = qpc(); const MH_STATUS st = MH_ApplyQueued();
        logf("start-up hooks switched on in one batch: %s (%.0f ms)", st == MH_OK ? "ok" : "FAILED", (qpc() - t0) * 1000.0 / g_qpf);
        if (st != MH_OK) ok = false;
    }
    if (!ok) { logf("not all hooks in - doubling stays OFF"); g_armed = 0; }
    const bool autotest = wcsstr(GetCommandLineW(), L"-akntest") != nullptr;
    logf("ready (M2 engine-stereo build). %s", autotest ? "auto test: press F10 in gameplay -> 25 s each of normal, doubled, normal, doubled" : "F9 toggles engine stereo");
    Snap last = snap(), phase = last;
    int phaseIdx = -1; ULONGLONG phaseAt = 0; bool f9 = false; static bool s_f10 = false;
    static const LONG plan[4] = { 0, 1, 0, 1 };
    for (;;)
    {
        Sleep(100);
        poll_commands();
        gpu_sample();
        {   // PERF1 PERFSWEEP 2026-10-10 (JJ: "we've lost about five to ten frames"): 8 phases x 8 s, one feature off each
            // (2 s to settle, 6 s measured): stereo pairs / s and graphics-card load, a table at the end. JJ stands still.
            static const char* kOn[8]  = { "", "gloss 0", "mipbias 0", "raindepth 0", "hudineye 0", "markercut 0", "farrain 0", "" };
            static const char* kOff[8] = { "", "gloss 1", "mipbias 1", "raindepth 1", "hudineye 1", "markercut 1", "farrain 1", "" };
            static const char* kName[8] = { "everything on", "suit gloss off", "texture sharpness off", "rain depth off", "HUD in the eyes off", "markers into the eyes off", "far rain off", "everything on again" };
            static int s_ph = -1; static ULONGLONG s_at = 0; static LONG s_p0 = 0; static bool s_meas = false; static double s_r[8][3] = {};
            auto run = [](const char* c) { if (c && *c) { char b[64]; strcpy_s(b, c); do_command(b); } };
            if (g_sweepWant) { g_sweepWant = 0; s_ph = 0; s_at = GetTickCount64(); s_meas = false; logf("PERFSWEEP: 8 phases x 8 s - stand still, look at one spot"); }
            if (s_ph >= 0)
            {
                const ULONGLONG el = GetTickCount64() - s_at;
                if (!s_meas && el >= 2000) { s_meas = true; s_p0 = g_pairSeq; g_gpuPhase = GpuAcc{}; }
                if (el >= 8000)
                {
                    const GpuAcc a = g_gpuPhase;
                    s_r[s_ph][0] = (g_pairSeq - s_p0) / 6.0; s_r[s_ph][1] = a.n ? a.busy / a.n : 0.0; s_r[s_ph][2] = a.n ? a.clk / a.n : 0.0;
                    logf("PERFSWEEP phase %d %-26s pairs/s %.1f | GPU busy %.1f%% at %.0f MHz", s_ph, kName[s_ph], s_r[s_ph][0], s_r[s_ph][1], s_r[s_ph][2]);
                    run(kOff[s_ph]);
                    if (++s_ph >= 8)
                    {
                        const double base = 0.5 * (s_r[0][0] + s_r[7][0]);
                        logf("PERFSWEEP RESULT (base %.1f pairs/s = %.2f ms):", base, base > 0 ? 1000.0 / base : 0.0);
                        for (int i = 1; i < 7; ++i) logf("  %-26s %.1f pairs/s (%+.1f) = %+.2f ms per frame for the feature", kName[i], s_r[i][0], s_r[i][0] - base, (base > 0 && s_r[i][0] > 0) ? 1000.0 / base - 1000.0 / s_r[i][0] : 0.0);
                        s_ph = -1;
                    }
                    else { s_at = GetTickCount64(); s_meas = false; run(kOn[s_ph]); }
                }
            }
        }
        const bool f9now = (GetAsyncKeyState(VK_F9) & 0x8000) != 0 && game_in_front();
        if (f9now && !f9 && !autotest) { g_stereo = !g_stereo; logf("engine stereo -> %s", g_stereo ? "ON" : "off"); }
        f9 = f9now;
        const Snap now = snap();
        if (now.tick - last.tick >= 1000)
        {
            report("1s", last, now);
            logf("  stereo %ld frames %ld, eye views %ld / %ld, eye renders (render thread) %ld / %ld, other renders %ld | pairs %ld | HUD frames %ld redirects %ld copies %ld (hud %ld) | AKVR eyes %ld presents %ld | exposure copies %ld (share %ld) | AKVR shown: stereo %ld flat %ld | rogue ops %ld | pose: stamp-matched %ld of %ld, delay %ld (stamp %llu) | history shared %d, swap %ld, copies %ld", g_stereo, c_stereoFrames, c_ctorEye[0], c_ctorEye[1], c_rtEye[0], c_rtEye[1], c_rtOther, g_pairSeq, g_hudSeq, c_hudRedirect, c_hudCopy, g_hudOn, c_eyesCalls, c_presents, c_lumCopies, g_lumShare, g_akvrFresh, g_akvrFlat, c_rogueOps, g_poseMatched, g_poseTotal, g_poseDelay, g_pairStamp, g_candN, g_histSwap, c_histCopies);
            {   // RAINONCE / PANELTEST telemetry
                static LONG lr[2][3] = {}; LONG d[2][3];
                for (int k = 0; k < 2; ++k) for (int w = 0; w < 3; ++w) { const LONG v = c_rainCs[k][w]; d[k][w] = v - lr[k][w]; lr[k][w] = v; }
                panel_link(); const unsigned long long ps = p_panelState ? p_panelState() : 0;
                static int padTick = 0; using RouteFn = const char*(*)();
                static RouteFn p_route = nullptr; if (!p_route) { HMODULE m = GetModuleHandleW(L"dinput8.dll"); if (m) p_route = (RouteFn)GetProcAddress(m, "akvr_pad_route"); }
                if (p_route && (padTick++ % 10) == 0) logf("  %s", p_route());   // GAMEROUTE: which route sees JJ's pad
                {   // PATHREC (AKVR): frames that show the old eye picture with its HUD again, and the HUD layer on top
                    using PdFn = const char* (*)(); static PdFn pf = nullptr; static bool pt = false;
                    if (!pt) { HMODULE hm = GetModuleHandleW(L"dinput8.dll"); pf = hm ? (PdFn)GetProcAddress(hm, "akvr_path_diag") : nullptr; pt = hm != nullptr; }
                    if (pf) logf("  %s", pf());
                    static LONG l1 = 0, l2 = 0; const LONG d1 = c_flipSingle - l1, d2 = c_flipBatch - l2; l1 = c_flipSingle; l2 = c_flipBatch;
                    if (d1 || d2) logf("  FLIPREC: pieces next to a marker left on the HUD sheet/s - single %ld, batched %ld", d1, d2);
                    { static LONG lc = 0; const LONG dc = c_mkCarry2 - lc; lc = c_mkCarry2; if (dc) logf("  MKCARRY2: HUD frames with no marker point that kept the last ones/s %ld", dc); }
                    { static LONG lh = 0, lt = 0; const LONG dh = c_mkContHeld - lh, dt = c_mkContTake - lt; lh = c_mkContHeld; lt = c_mkContTake; static LONG ld = 0; const LONG dd = c_mkHeldDrop - ld; ld = c_mkHeldDrop; if (dh || dt || dd) logf("  MKCONT: one-off marker point / HUD map jumps held back/s %ld, new ones taken after 3 frames/s %ld, marked pieces left out on held frames/s %ld", dh, dt, dd); }
                    { static LONG lf = 0; const LONG df = c_mkTagFaded - lf; lf = c_mkTagFaded; if (df) logf("  TAGFADE: marker pieces recognised by their faded mark/s %ld", df); }
                    { static LONG lj = 0; const LONG dj = c_mkPlaceJump - lj; lj = c_mkPlaceJump; if (dj) logf("  MKPLACE: frames where the marker jumped off its own path/s %ld", dj); }
                    { static LONG ln = 0, lr = 0; const LONG dn = c_mkMissNone - ln, dr = c_mkMissReach - lr; ln = c_mkMissNone; lr = c_mkMissReach; if (dn || dr) logf("  MKMISS: marked marker pieces left on the flat sheet/s - no marker this frame %ld, out of reach %ld", dn, dr); }
                }
                {   // SUITGLOSS
                    static LONG lastD = 0; const LONG dd = c_glossDraws - lastD; lastD = c_glossDraws;
                    logf("  suit gloss (head-free aim): %s, draws corrected/s %ld", g_gloss ? "on" : "off", dd);
                    {   static LONG ls = 0, lw = 0; const LONG ds = c_detSeen - ls, dw = c_vsSplitSwaps - lw; ls = c_detSeen; lw = c_vsSplitSwaps;   // DETHOLD
                        if (ds || dw) logf("  detective: composite seen/s %ld, longest gap %ld ms (hold %ld ms), right-eye view state swaps/s %ld", ds, (long)InterlockedExchange(&g_detGapMax, 0), (long)g_detHoldMs, dw); }
                }
                {   // MKSINGLE: one-piece HUD shaders known, marker pieces routed / left on the HUD sheet (other shaders)
                    using CntFn = long(*)(); static CntFn p_cnt = nullptr, p_bcnt = nullptr; if (!p_cnt) { HMODULE hm = GetModuleHandleW(L"dinput8.dll"); if (hm) { p_cnt = (CntFn)GetProcAddress(hm, "akvr_vs_one_piece_count"); p_bcnt = (CntFn)GetProcAddress(hm, "akvr_vs_batch_count"); } }
                    static LONG lR = 0, lF = 0, lS = 0; const LONG dR = c_mkRouted - lR, dF = c_mkFlat - lF, dS = c_mkSplit - lS; lR = c_mkRouted; lF = c_mkFlat; lS = c_mkSplit;
                    static LONG lT = 0, lL = 0; const LONG dT = c_mkTagged - lT, dL = c_mkTagLost - lL; lT = c_mkTagged; lL = c_mkTagLost;
                    { static LONG lg = 0, lh = 0; const LONG dg = c_mkGlow - lg, dh = c_mkSticky - lh; lg = c_mkGlow; lh = c_mkSticky;
                      if (dg || dh) logf("  markers: glow pastes into 3D/s %ld, unmarked pieces held with the marker/s %ld (MKGLOW2 / MKSTICKY2)", dg, dh); }
                    { static LONG lp = 0, lw = 0, lm = 0, lv = 0, lf = 0, lk = 0;   // MKEYE
                      const LONG dp = c_mkEyePieces - lp, dw = c_mkEyeWhole - lw, dm = c_mkEyeNoMap - lm, dv = c_mkEyeNoView - lv, df = c_mkEyeFrames - lf, dk = c_mkEyeMarkers - lk;
                      lp = c_mkEyePieces; lw = c_mkEyeWhole; lm = c_mkEyeNoMap; lv = c_mkEyeNoView; lf = c_mkEyeFrames; lk = c_mkEyeMarkers;
                      static LONG lfc = 0; const LONG dfc = c_mkFaced - lfc; lfc = c_mkFaced;   // MKFACEPOS
                      {   static LONG a1 = 0, a2 = 0, a3 = 0, a4 = 0, a5 = 0, a6 = 0;   // DETCALLOUT
                          const LONG s1 = c_scanSeen - a1, s2 = c_scanPlaced - a2, s3 = c_scanPieces - a3, s4 = c_scanPasses - a4, s5 = c_scanReads - a5, s6 = c_scanSky - a6;
                          a1 = c_scanSeen; a2 = c_scanPlaced; a3 = c_scanPieces; a4 = c_scanPasses; a5 = c_scanReads; a6 = c_scanSky;
                          static int s_sdN = 0; if ((!s1 || ++s_sdN % 5 == 0) && akn_detective_active()) { using SdFn = const char* (*)(); static SdFn f = nullptr; static bool t = false; if (!t) { HMODULE hm = GetModuleHandleW(L"dinput8.dll"); f = hm ? (SdFn)GetProcAddress(hm, "akvr_scan_diag") : nullptr; t = hm != nullptr; } if (f) logf("  DETCALLOUT: %s | %s", s1 ? "box shown" : "no box", f()); }   // SCANLINE2: every 5 s while a box shows too
                          static LONG a7 = 0; const LONG s7 = c_scanExact - a7; a7 = c_scanExact;
                          hud_sig_sweep();   // HUDSIG
                          if (s1 || s3) logf("  DETCALLOUT: scan box seen/s %ld, placed as a marker/s %ld (at the projected object point/s %ld, match %.3f), callout pieces routed/s %ld | depth passes/s %ld, answers %ld (sky %ld), distance %.0f units (hit %.0f), skip nearer than %.0f | %s | %s", s1, s2, s7, g_scanMatchD, s3, s4, s5, s6, g_scanDist, g_scanRaw, g_scanMin, g_scanDiag, g_scanNearDiag); }
                      if (g_mkEye) logf("  MKEYE: marker pieces drawn per eye/s %ld, glow pastes per eye/s %ld | HUD frames with markers placed %ld (markers %ld, faced to the camera %ld, mkfacepos %ld), no layout map %ld, no pair view %ld", dp, dw, df, dk, dfc, (long)g_mkFacePos, dm, dv);
                      static LONG lk1 = 0, lk2 = 0, lk3 = 0, lfl = 0;   // MKEYE2
                      const LONG d1 = c_mkEyeK[1] - lk1, d2 = c_mkEyeK[2] - lk2, d3 = c_mkEyeK[3] - lk3, dfl = c_mkEyeFail - lfl;
                      lk1 = c_mkEyeK[1]; lk2 = c_mkEyeK[2]; lk3 = c_mkEyeK[3]; lfl = c_mkEyeFail;
                      if (g_mkEye && (d1 || d2 || d3 || dfl)) logf("  MKEYE2: draws moved per eye/s - one piece %ld, batch %ld, by viewport %ld, left unmoved %ld | %s | %s", d1, d2, d3, dfl, g_mkEyeDiag, g_mkLagDiag);
                      static LONG lau = 0, lal = 0, lar = 0, lan = 0;   // MKANCHOR
                      const LONG dau = c_mkAncUsed - lau, dal = c_mkAncLearn - lal, dar = c_mkAncReject - lar, dan = c_mkAncNew - lan;
                      lau = c_mkAncUsed; lal = c_mkAncLearn; lar = c_mkAncReject; lan = c_mkAncNew;
                      const LONG sn = g_mkStillN; const double sp = g_mkStillPos, ss = g_mkStillShape; g_mkStillN = 0; g_mkStillPos = 0.0; g_mkStillShape = 0.0;
                      { static LONG lgd = 0, lgn = 0; const LONG dgd = c_mkGsDraws - lgd, dgn = c_mkGsNone - lgn; lgd = c_mkGsDraws; lgn = c_mkGsNone;   // MKGS
                        using GcFn = long (*)(long*); static GcFn p_gc = nullptr; if (!p_gc) { HMODULE hm = GetModuleHandleW(L"dinput8.dll"); if (hm) p_gc = (GcFn)GetProcAddress(hm, "akvr_marker_gs_counts"); }
                        long gf = 0; const long gb = p_gc ? p_gc(&gf) : -1;
                        using GtFn = const char* (*)(long*, long*, long*); static GtFn p_gt = nullptr; if (!p_gt) { HMODULE hm = GetModuleHandleW(L"dinput8.dll"); if (hm) p_gt = (GtFn)GetProcAddress(hm, "akvr_marker_gs_stats"); }
                        long gno = -1, gun = -1, gmi = -1; const char* ger = p_gt ? p_gt(&gno, &gun, &gmi) : "";   // MKGS2
                        if (g_mkEye && g_mkGS && (dgd || dgn)) logf("  MKGS: marker draws placed per vertex in 3D per eye/s %ld, without a geometry shader (row fallback)/s %ld | geometry shaders built %ld, failed %ld | MKGS2: vertex shaders known %ld (unsupported %ld), lookups not found %ld%s%.200s", dgd, dgn, gb, gf, gno, gun, gmi, ger && ger[0] ? " | first error: " : "", ger ? ger : ""); }
                      if (g_mkEye && (dau || dal || dar || sn)) logf("  MKANCHOR: placed about the layout anchor/s %ld, learnt %ld, rejected %ld, new markers %ld | while the marker point stands still (%ld frames): the first piece moves %.5f and reshapes %.5f per frame",
                                                                   dau, dal, dar, dan, sn, sn ? sp / sn : 0.0, sn ? ss / sn : 0.0); }
                    using SetsFn = long(*)(); static SetsFn p_sets = nullptr; if (!p_sets) { HMODULE hm = GetModuleHandleW(L"dinput8.dll"); if (hm) p_sets = (SetsFn)GetProcAddress(hm, "akvr_marker_tag_sets"); }
                    logf("  markers: HUD shaders known one-piece %ld batched %ld | draws to the marker sheet/s %ld (split %ld), not routable/s %ld | MKTAG mode %ld: marked draws/s %ld, marked but not routable/s %ld, marks set %ld | off-sheet draws skipped %ld", p_cnt ? p_cnt() : -1L, p_bcnt ? p_bcnt() : -1L, dR, dS, dF, g_mkByTag, dT, dL, p_sets ? p_sets() : -1L, c_mkOffSheet);
                }
                logf("  dice shared: game thread %ld, render thread %ld (mode %ld) | once-time: left renders with zero elapsed time %ld (mode %ld, offset %ld) | render wait %ld (timeouts %ld) | recorded draws %ld dispatches %ld, playbacks %ld | far rain patched %ld (no axes %ld, near %ld) | rain depth draws %ld", c_randGame, c_randRender, g_randSync, c_deltaZeroed, g_deltaZero, g_deltaOff, c_rtWaits, c_rtWaitTimeouts, c_defDraw, c_defDisp, c_execCL, c_farPatched, c_farNoAxes, c_nearPatched, c_rainDepth);
                { static LONG lr = 0; const LONG dr = c_rainLight - lr; lr = c_rainLight; static LONG lrr = 0, lrs = 0; const LONG drr = c_rainRelit - lrr, drs = c_rainRelightSkip - lrs; lrr = c_rainRelit; lrs = c_rainRelightSkip; static LONG lsl = 0; const LONG dsl = c_smokeLight - lsl; lsl = c_smokeLight; static LONG lld = 0; const LONG dld = c_lightDir - lld; lld = c_lightDir; logf("  LIGHTDIR: sprite / lit draws given the head-free light rows/s %ld (smokelight %ld; grid forward/s %ld)", dld, (long)g_smokeLight, dsl); static LONG lsp = 0; const LONG dsp = c_spriteAxes - lsp; lsp = c_spriteAxes; static LONG lcf = 0; const LONG dcf = c_charFace - lcf; lcf = c_charFace; logf("  SPRITEAXES: particle sprite draws turned to the head-free camera/s %ld (spriteaxes %ld), facing Batman/s %ld", dsp, (long)g_spriteAxes, dcf); static LONG lm = 0; const LONG dm2 = c_rainMask - lm; lm = c_rainMask; logf("  RAINMASK: rain draws marked for DLSS/s %ld (rainmask %ld)", dm2, (long)g_rainMask); static LONG l2 = 0; const LONG d2 = c_rainLight2 - l2; l2 = c_rainLight2; logf("  RAINLIGHT2: rain lit from the left eye's view/s %ld (rainlight2 %ld)", d2, (long)g_rainLight2); logf("  RAINLIGHT: rain lighting spots corrected/s %ld (rainlight %ld) | RAINRELIGHT: second-eye relights/s %ld, skipped %ld", dr, (long)g_rainLight, drr, drs); }
                logf("  rain steps/s: simulation left %ld right %ld outside %ld, splashes left %ld right %ld outside %ld (once %ld, skipped %ld) | VR panel state %llx (shown %d, ImGui %d, drew %d, to headset %d, pad %d, chord down %d, buttons %04llx, chord fires %llu)",
                     d[0][0], d[0][1], d[0][2], d[1][0], d[1][1], d[1][2], g_rainOnce, c_rainSkip, ps & 0xff, (int)(ps & 1), (int)((ps >> 2) & 1), (int)((ps >> 3) & 1), (int)((ps >> 4) & 1), (int)((ps >> 5) & 1), (int)((ps >> 6) & 1), (ps >> 16) & 0xffff, ps >> 32);
            }
            if (g_d0)
            {   // DLSS D0: per eye render processor / graphics-card time, SMAA passes and motion-vector clears per second
                static LONG64 lt[2] = {}; static LONG lc[2] = {}, ld[3][3] = {}, lmv[3] = {};
                double cpu[2] = {}, gpu[2] = {};
                for (int e = 0; e < 2; ++e)
                {
                    const LONG64 t = t_eyeCpu[e]; const LONG n = c_eyeCpu[e];
                    cpu[e] = n > lc[e] ? (t - lt[e]) * 1000.0 / g_qpf / (n - lc[e]) : 0.0; lt[e] = t; lc[e] = n;
                    gpu[e] = g_gpuMsN[e] ? g_gpuMsSum[e] / g_gpuMsN[e] : 0.0; g_gpuMsSum[e] = 0; g_gpuMsN[e] = 0;
                }
                LONG d[3][3], mv[3];
                for (int k = 0; k < 3; ++k) for (int w = 0; w < 3; ++w) { d[k][w] = c_d0[k][w] - ld[k][w]; ld[k][w] = c_d0[k][w]; }
                for (int w = 0; w < 3; ++w) { mv[w] = c_mvClear[w] - lmv[w]; lmv[w] = c_mvClear[w]; }
                logf("  D0 eye render ms: processor L %.2f R %.2f, graphics card L %.2f R %.2f | SMAA edge L/R/other %ld/%ld/%ld weights %ld/%ld/%ld neighbourhood %ld/%ld/%ld | motion-vector clears %ld/%ld/%ld",
                     cpu[0], cpu[1], gpu[0], gpu[1], d[0][0], d[0][1], d[0][2], d[1][0], d[1][1], d[1][2], d[2][0], d[2][1], d[2][2], mv[0], mv[1], mv[2]);
            }
            {   // FLAREOFF: binds replaced per second
                static LONG lp = 0, lc = 0; const LONG dp = c_flarePs - lp, dc = c_flareCs - lc; lp = c_flarePs; lc = c_flareCs;
                using FsFn = int(*)(float*); static FsFn p_fs = nullptr; if (!p_fs) { HMODULE hm = GetModuleHandleW(L"dinput8.dll"); if (hm) p_fs = (FsFn)GetProcAddress(hm, "akvr_fullcut_state"); }
                float cutA = 0.0f; const int cutN = p_fs ? p_fs(&cutA) : -1;   // FULLCUT telemetry
                using FdFn = const char*(*)(); static FdFn p_fd = nullptr; if (!p_fd) { HMODULE hm = GetModuleHandleW(L"dinput8.dll"); if (hm) p_fd = (FdFn)GetProcAddress(hm, "akvr_fullcut_diag"); }
                if (p_fd) logf("  full-screen cut probe: %s", p_fd());   // FULLCUT3: the black box's shape on the HUD sheet
                using PdFn = const char*(*)(); static PdFn p_pd = nullptr; if (!p_pd) { HMODULE hm = GetModuleHandleW(L"dinput8.dll"); if (hm) p_pd = (PdFn)GetProcAddress(hm, "akvr_perf_diag"); }
                if (p_pd) logf("  %s", p_pd());   // PERFTIME: AKVR's processor time per VR frame / HUD step
                using FlFn = const char*(*)(); static FlFn p_fl = nullptr; if (!p_fl) { HMODULE hm = GetModuleHandleW(L"dinput8.dll"); if (hm) p_fl = (FlFn)GetProcAddress(hm, "akvr_flare_diag"); }
                logf("  %s | full-screen cut now %.2f (frames so far %d)", p_fl ? p_fl() : "lens flare: AKVR export missing", cutA, cutN);   // FLAREOFF3 (AKVR)
            }
            {   // AFW: frames per eye, pairs warped / shown flat, the warp's cost and holes
                static LONG lE[2] = {}, lP = 0, lM = 0; const LONG dL = c_afwEye[0] - lE[0], dR = c_afwEye[1] - lE[1], dP = c_afwPairs - lP, dM = c_afwMono - lM;
                lE[0] = c_afwEye[0]; lE[1] = c_afwEye[1]; lP = c_afwPairs; lM = c_afwMono;
                if (akn_afw_on() || dP || dM) logf("  %s | frames/s drawing left %ld right %ld | pairs/s warped %ld, drawn eye in both %ld", akn_afw_diag(), dL, dR, dP, dM);
            }
            if (akn_dlss_mode() > 0) logf("  %s | mip bias %ld: sampler swaps/s %ld, twins %ld", akn_dlss_diag(), g_mipBias, InterlockedExchange(&g_mipSwaps, 0), g_mipMade);   // DLSS D1, MIPBIAS
            {   // MENUCHECK: AKVR's main-menu state (the test harness waits for 0 before gameplay tests)
                using MmFn = int(*)(); static MmFn mm = nullptr; if (!mm) { HMODULE m = GetModuleHandleW(L"dinput8.dll"); if (m) mm = (MmFn)GetProcAddress(m, "akvr_in_main_menu"); }
                logf("  main menu %d", mm ? mm() : -1);
                using HdFn = const char*(*)(); static HdFn hd = nullptr; if (!hd) { HMODULE m = GetModuleHandleW(L"dinput8.dll"); if (m) hd = (HdFn)GetProcAddress(m, "akvr_hie_diag_c"); }
                if (hd) logf("  %s", hd());   // MARKERCUT probe
                using FpFn = const char*(*)(); static FpFn fpd = nullptr; if (!fpd) { HMODULE m = GetModuleHandleW(L"dinput8.dll"); if (m) fpd = (FpFn)GetProcAddress(m, "akvr_fp_diag_c"); }
                if (fpd) logf("  %s", fpd());   // FIRSTPERSON
            }
            {   // MARKERPROBE: who projected world points this second
                static int tick = 0;
                if ((tick++ % 5) == 0 && c_projCalls)
                {
                    char line[600]; int o = _snprintf_s(line, sizeof(line), _TRUNCATE, "  world-to-screen callers (total %ld):", c_projCalls);
                    for (auto& e : g_projRa) if (e.ra && o > 0) o += _snprintf_s(line + o, sizeof(line) - o, _TRUNCATE, " %llX x%ld", (unsigned long long)e.ra, e.n);
                    logf("%s", line);
                }
            }
            gpu_report("1s", g_gpuSec);
            if (now.redraw - last.redraw > 0 && (now.redraw % 600) < 60) { k_draw.print("Draw"); k_redraw.print("Redraw"); k_calc.print("CalcSceneView"); k_cdA.print("client Draw A"); k_cdB.print("client Draw B"); k_scene.print("scene entry"); k_render.print("render family"); k_brvf.print("BeginRenderingViewFamily");
              logf("  threads: Draw %lu Redraw %lu Calc %lu A %lu B %lu", g_tid[0], g_tid[1], g_tid[2], g_tid[3], g_tid[4]); }
            last = now;
        }
        if (autotest && g_armed)
        {
            const bool f10now = (GetAsyncKeyState(VK_F10) & 0x8000) != 0 && game_in_front();
            const bool f10press = f10now && !s_f10; s_f10 = f10now;
            if (phaseIdx < 0 && f10press) { phaseIdx = 0; phaseAt = now.tick; phase = now; g_mode = plan[0]; g_gpuPhase = GpuAcc{}; logf("PHASE 0 mode %ld", g_mode); }
            else if (phaseIdx >= 0 && phaseIdx < 4 && now.tick - phaseAt >= 25000)
            {
                char t[32]; snprintf(t, sizeof(t), "PHASE %d mode %ld SUMMARY", phaseIdx, plan[phaseIdx]);
                report(t, phase, now);
                gpu_report(t, g_gpuPhase);
                ++phaseIdx; phase = now; phaseAt = now.tick;
                if (phaseIdx < 4) { g_mode = plan[phaseIdx]; logf("PHASE %d mode %ld", phaseIdx, g_mode); }
                else { g_mode = 0; logf("auto test done"); k_brvf.print("BeginRenderingViewFamily"); k_draw.print("Draw"); k_redraw.print("Redraw"); k_calc.print("CalcSceneView"); }
            }
        }
    }
}
}

// ---- exports for AKVR (M5): looked up with GetProcAddress(GetModuleHandle("XINPUT9_1_0.dll"), ...) ----
extern "C" __declspec(dllexport) int akn_api_version() { return 1; }
// AFW (AKVR overlay): bit 0 = on, bits 4-7 = warp mode; and the status line
extern "C" __declspec(dllexport) int akn_afw_state() { return (akn_afw_on() ? 1 : 0) | (akn_afw_mode() << 4); }
extern "C" __declspec(dllexport) const char* akn_afw_status() { return akn_afw_diag(); }
extern "C" __declspec(dllexport) int akn_active() { return g_base && g_stereo && g_armed ? 1 : 0; }
// the eye pair (2W x H, left half = left eye) and its counter (+1 per complete frame); null until the first 3D frame
extern "C" __declspec(dllexport) ID3D11Texture2D* akn_pair(LONG* seq) { if (seq) *seq = g_pairSeq; return g_pair; }
// the HUD texture (window size, transparent where there is no HUD) and its counter (+1 per frame it was used)
extern "C" __declspec(dllexport) ID3D11Texture2D* akn_hud(LONG* seq) { if (seq) *seq = g_hudSeq; return g_hudOn ? g_hudTex : nullptr; }
// SCANLINE 2026-10-12: the detective scan box's world point of the newest HUD frame (AKVR draws the line from it to the label)
extern "C" __declspec(dllexport) int akn_scan_point(float* w3) { if (!w3 || !g_scanWok) return 0; w3[0] = g_scanWpub[0]; w3[1] = g_scanWpub[1]; w3[2] = g_scanWpub[2]; return 1; }
// SCANHUD: the box's world point + its own point on the HUD sheet (AKVR moves the marker sheet from there to the object's spot)
extern "C" __declspec(dllexport) int akn_scan_box(float* out5) { if (!out5 || !g_scanWok) return 0; for (int k = 0; k < 3; ++k) out5[k] = g_scanWpub[k]; out5[3] = g_scanSheet[0]; out5[4] = g_scanSheet[1]; return 1; }
extern "C" __declspec(dllexport) long akn_hud_draws_last() { return g_hudDrawsLast; }
extern "C" __declspec(dllexport) void akn_hudsig_reset(const char* why) { AcquireSRWLockExclusive(&g_hsigLock); g_hsigN = 0; g_hsigLines = 0; ReleaseSRWLockExclusive(&g_hsigLock); logf("HUDSIG reset (%s): every draw kind is new again - the bat transition's kinds come and go within ~1.5 s", why ? why : "?"); }   // BATREC
extern "C" __declspec(dllexport) void akn_hudring_dump(const char* why, int delayMs) { if (!g_hrOn || g_hrDumps >= 12 || g_hrDumpAt) return; strncpy_s(g_hrWhy, why ? why : "x", _TRUNCATE); for (char* p = g_hrWhy; *p; ++p) if (!isalnum((unsigned char)*p)) *p = '_'; g_hrDumpAt = (LONGLONG)GetTickCount64() + (delayMs > 0 ? delayMs : 400); }   // HUDRING   // MENUSOUND (AKVR audiosync)
extern "C" __declspec(dllexport) void akn_set_hud(int on) { g_hudOn = on; if (!on) g_hudPhase = false; }
// per-eye lens (tangents l, r, u, d for eye 0 then eye 1; left/down negative) and eye offsets along view x (game units)
extern "C" __declspec(dllexport) void akn_set_eyes(const float* tan8, const float* off2, int asym)
{
    if (tan8) for (int e = 0; e < 2; ++e) for (int i = 0; i < 4; ++i) g_tan[e][i] = tan8[e * 4 + i];
    if (off2) { g_eyeOff[0] = off2[0]; g_eyeOff[1] = off2[1]; g_eyesApi = true; }
    g_asym = asym;
    // telemetry (stage 5c T0/T2): the first set, then every 600th
    if ((InterlockedIncrement(&c_eyesCalls) % 600) == 1)
    {
        logf("eyes from AKVR #%ld: L tan %.4f %.4f %.4f %.4f | R tan %.4f %.4f %.4f %.4f | offsets %.3f %.3f units | asym %d", c_eyesCalls,
             g_tan[0][0], g_tan[0][1], g_tan[0][2], g_tan[0][3], g_tan[1][0], g_tan[1][1], g_tan[1][2], g_tan[1][3], g_eyeOff[0], g_eyeOff[1], asym);
        g_asymLogs = 0;
    }
}
// AKVR's Present hook: the frame (and its HUD) is complete
void hud_stat_present();   // HUDTRACE, below
extern "C" __declspec(dllexport) void akn_present()
{
    g_hudPhase = false; InterlockedIncrement(&c_presents);
    g_hudDrawsLast = InterlockedExchange(&f_hudFrameDraws, 0);   // MENUSOUND
    if (g_hudStat > 0) hud_stat_present();
    if (g_hudAll && g_hudOn) g_hudClearPending = true;   // HUDTRACE hudall: AKVR has taken this frame's sheet - the next HUD bind starts a new one
}
// HUDTRACE: the sheet's coverage per frame (16 x 16 cells with any alpha), read 2 presents late without stalling
ID3D11Texture2D* g_hsStage[3] = {}; LONG g_hsCnt[3][8] = {}; int g_hsI = 0;
void hud_stat_present()
{
    if (!g_hudTex || !g_dev || !g_gameCtx) return;
    ID3D11DeviceContext* c = g_gameCtx;
    D3D11_TEXTURE2D_DESC d{}; g_hudTex->GetDesc(&d);
    const int k = g_hsI % 3;
    if (!g_hsStage[k]) { D3D11_TEXTURE2D_DESC sd = d; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0; if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &g_hsStage[k]))) return; }
    c->CopyResource(g_hsStage[k], g_hudTex);
    g_hsCnt[k][0] = g_hudSeq; g_hsCnt[k][1] = InterlockedExchange(&f_hudDraws, 0); g_hsCnt[k][2] = InterlockedExchange(&f_hudRedir, 0);
    g_hsCnt[k][3] = InterlockedExchange(&f_hudMissImm, 0); g_hsCnt[k][4] = InterlockedExchange(&f_hudBbDef, 0); g_hsCnt[k][5] = InterlockedExchange(&f_hudDefRedir, 0);
    g_hsCnt[k][6] = InterlockedExchange(&f_hudDsvBind, 0); g_hsCnt[k][7] = InterlockedExchange(&f_hudDsvMis, 0);
    ++g_hsI;
    if (g_hsI < 3) return;
    const int r = g_hsI % 3;   // the copy made 2 presents ago
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(c->Map(g_hsStage[r], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) { logf("HUDSTAT copy not ready"); return; }
    int cells = 0; unsigned hash = 2166136261u; double asum = 0, edge = 0; int edgeN = 0;
    for (UINT y = d.Height * 3 / 10; y < d.Height * 6 / 10; y += 8) for (UINT x = 0; x < d.Width / 20; x += 8) { edge += ((const uint8_t*)m.pData)[y * m.RowPitch + x * 4 + 3]; ++edgeN; }
    for (int cy = 0; cy < 16; ++cy) for (int cx = 0; cx < 16; ++cx)
    {
        bool any = false;
        for (UINT y = cy * d.Height / 16; y < (cy + 1) * d.Height / 16; y += 8)
            for (UINT x = cx * d.Width / 16; x < (cx + 1) * d.Width / 16; x += 8)
            { const UINT a = ((const uint8_t*)m.pData)[y * m.RowPitch + x * 4 + 3]; if (a > 8) any = true; asum += a; }
        if (any) { ++cells; hash = (hash ^ (unsigned)(cy * 16 + cx)) * 16777619u; }
    }
    c->Unmap(g_hsStage[r], 0);
    logf("HUDSTAT seq %ld draws %ld redirects %ld | missed: immediate %ld, recorded %ld (redirected %ld) | sheet cells %d hash %08x alpha %.0f | depth binds %ld (size mismatch %ld, own %d) | edge alpha %.0f skipped %ld vp scaled %ld",
         g_hsCnt[r][0], g_hsCnt[r][1], g_hsCnt[r][2], g_hsCnt[r][3], g_hsCnt[r][4], g_hsCnt[r][5], cells, hash, asum / 1000.0, g_hsCnt[r][6], g_hsCnt[r][7], (int)g_hudDsvOn, edgeN ? edge / edgeN : 0.0, InterlockedExchange(&f_hudSkipped, 0), InterlockedExchange(&f_hudVpScaled, 0));
    InterlockedDecrement(&g_hudStat);
}
// AKVR's telemetry: how many frames it showed from the pair and how many as the flat fallback (judge T8)
extern "C" __declspec(dllexport) void akn_report(LONG fresh, LONG flat) { g_akvrFresh = fresh; g_akvrFlat = flat; }
// POSESTAMP: the stamp of the newest complete pair (0 = none / AKVR's export missing)
extern "C" __declspec(dllexport) unsigned long long akn_pair_stamp() { return g_pairStamp; }
extern "C" __declspec(dllexport) void akn_report_pose(LONG matched, LONG total, LONG delay) { g_poseMatched = matched; g_poseTotal = total; g_poseDelay = delay; }
// AKVR: the device behind the game's swap chain (its Present hook) - the only one whose context table we hook
extern "C" __declspec(dllexport) void akn_set_game_device(ID3D11Device* d)
{
    if (!d || g_gameDevHint == d) return;
    d->AddRef(); g_gameDevHint = d;
    logf("AKVR named the game's device: %p", (void*)d);
}

namespace {
volatile LONG g_vehLogs = 0;
LONG CALLBACK akn_veh(EXCEPTION_POINTERS* e)
{
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || InterlockedIncrement(&g_vehLogs) > 5) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t a = (uintptr_t)e->ExceptionRecord->ExceptionAddress;
    HMODULE me = nullptr; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&akn_veh, &me);
    HMODULE m = nullptr; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &m);
    wchar_t mn[MAX_PATH] = L"?"; if (m) GetModuleFileNameW(m, mn, MAX_PATH);
    const wchar_t* leaf = wcsrchr(mn, L'\\'); leaf = leaf ? leaf + 1 : mn;
    char b[1400]; int k = snprintf(b, sizeof(b), "CRASH RECORDER: access violation (%s %llX) at %llX in %S+%llX, thread %lu, eye %d inEyeRender %d inSecond %d hudPhase %d | rsp words:",
        e->ExceptionRecord->ExceptionInformation[0] == 8 ? "execute" : e->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
        (unsigned long long)e->ExceptionRecord->ExceptionInformation[1], (unsigned long long)a, leaf, (unsigned long long)(m ? a - (uintptr_t)m : 0),
        GetCurrentThreadId(), tl_eye, (int)tl_inEyeRender, (int)tl_inSecond, (int)g_hudPhase);
    __try
    {
        const uint64_t* sp = (const uint64_t*)e->ContextRecord->Rsp;
        for (int i = 0, n = 0; i < 160 && n < 16; ++i)
        {
            HMODULE mm = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)sp[i], &mm) && mm)
            {
                wchar_t nn[MAX_PATH]; GetModuleFileNameW(mm, nn, MAX_PATH); const wchar_t* l2 = wcsrchr(nn, L'\\'); l2 = l2 ? l2 + 1 : nn;
                k += snprintf(b + k, sizeof(b) - k, " %S+%llX", l2, (unsigned long long)(sp[i] - (uint64_t)mm)); ++n;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    logf("%s", b);
    return EXCEPTION_CONTINUE_SEARCH;
}
}

void akn_start(HMODULE self)
{
    if (!wcsstr(GetCommandLineW(), L"-akn")) return;
    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    GetModuleFileNameW(self, g_dir, MAX_PATH);
    for (int i = (int)wcslen(g_dir) - 1; i >= 0; --i) if (g_dir[i] == L'\\') { g_dir[i + 1] = 0; break; }
    wcscat_s(g_dir, L"akn\\");
    CreateDirectoryW(g_dir, nullptr);
    wchar_t lp[MAX_PATH]; wcscpy_s(lp, g_dir); wcscat_s(lp, L"akn_log.txt");
    {   // LOGKEEP 2026-10-11 (JJ's SteamVR crash at 19:01: two relaunches overwrote that session's log): keep the last two
        wchar_t p1[MAX_PATH], p2[MAX_PATH]; wcscpy_s(p1, g_dir); wcscat_s(p1, L"akn_log_prev.txt"); wcscpy_s(p2, g_dir); wcscat_s(p2, L"akn_log_prev2.txt");
        MoveFileExW(p1, p2, MOVEFILE_REPLACE_EXISTING); MoveFileExW(lp, p1, MOVEFILE_REPLACE_EXISTING);
    }
    InitializeCriticalSection(&g_cs);
    g_log = _wfopen(lp, L"w");
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpf = (double)f.QuadPart;
    logf("AKN probe M1 build " __DATE__ " " __TIME__ ", exe base %llX", (unsigned long long)g_base);
    AddVectoredExceptionHandler(1, akn_veh);
    CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); akn_start(h); }
    return TRUE;
}

// PANELRAIN 2026-10-08: AKVR's panel sends AKN commands directly (same words as akn\\command.txt)
extern "C" __declspec(dllexport) void akn_cmd(const char* text)
{
    char line[128]; strncpy_s(line, text ? text : "", _TRUNCATE);
    do_command(line);
}
extern "C" __declspec(dllexport) int akn_rain_view() { return (int)g_rainView; }
extern "C" __declspec(dllexport) int akn_far_rain() { return (int)g_farRain; }
extern "C" __declspec(dllexport) int akn_rain_pause() { return (int)g_rainPause; }

// DLSS D1: the log for other source files (dlss.cpp)
void akn_log(const char* fmt, ...)
{
    char b[1024]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    logf("%s", b);
}
namespace {   // DETCALLOUT: scan_pass was declared inside the file's unnamed namespace
// the left eye's depth copied at the moment the game's SMAA edge pass reads it (the only time it is whole), while a box shows
ID3D11Texture2D* g_scanCopy = nullptr; ID3D11Resource* g_scanCopySrc = nullptr; volatile LONG g_scanCopied = 0, c_scanCopies = 0;
volatile LONG g_scanClears = 0, g_scanFromClear = 0, g_scanClearLog = 0;   // depth clears this left-eye render; copy came from one
// DC1 run 6 (03:52 depth dump): at the SMAA pass the depth holds only Batman (0.88-0.93 = ~1 m), the world reads 1.0 - UE3 clears
// depth for the foreground group mid-render. The depth is copied just BEFORE each depth clear of the left eye's render except the
// first (the render's own start); the last such copy holds the world.
bool scan_copy_into(ID3D11DeviceContext* c, bool deferred, ID3D11Resource* r);
void scan_clear_copy(ID3D11DeviceContext* c, bool deferred, ID3D11DepthStencilView* d, UINT f)
{
    if ((!g_scanOn || !g_scanRay.ok) && g_depthProbe <= 0) return;   // DEPTHPROBE also takes the copy
    if (!(f & D3D11_CLEAR_DEPTH)) return;
    const LONG k = InterlockedIncrement(&g_scanClears);
    ID3D11Resource* r = nullptr; d->GetResource(&r); if (!r) return;
    D3D11_TEXTURE2D_DESC td{}; ID3D11Texture2D* t = nullptr; if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) && t) { t->GetDesc(&td); t->Release(); }
    if (g_scanClearLog < 40) { ++g_scanClearLog; logf("DETCALLOUT: left-eye depth clear #%ld %ux%u format %d (%s)%s", k, td.Width, td.Height, (int)td.Format, deferred ? "deferred" : "immediate", k >= 2 ? " - copied" : ""); }
    if (k >= 2 && (td.BindFlags & D3D11_BIND_SHADER_RESOURCE) && scan_copy_into(c, deferred, r)) g_scanFromClear = 1;
    r->Release();
}
void scan_copy_depth(ID3D11DeviceContext* c, bool deferred, ID3D11Resource* r)
{
    if (!g_scanOn || !g_scanRay.ok || !r || !g_dev || !c) return;
    if (g_scanFromClear) return;   // a mid-render clear's copy (the world) is better than this one (foreground only)
    if (r != g_scanCopySrc || !g_scanCopy)
    {
        ID3D11Texture2D* t = nullptr; D3D11_TEXTURE2D_DESC d{};
        if (FAILED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) || !t) return;
        t->GetDesc(&d); t->Release();
        if (d.SampleDesc.Count != 1) return;
        d.Usage = D3D11_USAGE_DEFAULT; d.CPUAccessFlags = 0; d.MiscFlags = 0; d.MipLevels = 1; d.ArraySize = 1;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (d.BindFlags & D3D11_BIND_DEPTH_STENCIL);
        if (g_scanCopy) { g_scanCopy->Release(); g_scanCopy = nullptr; }
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_scanCopy))) { g_scanCopy = nullptr; logf("DETCALLOUT: depth copy texture FAILED (format %d)", (int)d.Format); return; }
        g_scanCopySrc = r;
        logf("DETCALLOUT: depth copy %ux%u format %d (from the SMAA edge pass, %s context)", d.Width, d.Height, (int)d.Format, deferred ? "deferred" : "immediate");
    }
    if (deferred) c->CopyResource(g_scanCopy, r); else o_CopyRes(c, g_scanCopy, r);
    g_scanCopied = 1; InterlockedIncrement(&c_scanCopies);
}
bool scan_copy_into(ID3D11DeviceContext* c, bool deferred, ID3D11Resource* r)
{
    if (!g_dev || !c || !r) return false;
    if (r != g_scanCopySrc || !g_scanCopy)
    {
        ID3D11Texture2D* t = nullptr; D3D11_TEXTURE2D_DESC d{};
        if (FAILED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) || !t) return false;
        t->GetDesc(&d); t->Release();
        if (d.SampleDesc.Count != 1) return false;
        d.Usage = D3D11_USAGE_DEFAULT; d.CPUAccessFlags = 0; d.MiscFlags = 0; d.MipLevels = 1; d.ArraySize = 1;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (d.BindFlags & D3D11_BIND_DEPTH_STENCIL);
        if (g_scanCopy) { g_scanCopy->Release(); g_scanCopy = nullptr; }
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_scanCopy))) { g_scanCopy = nullptr; return false; }
        g_scanCopySrc = r;
    }
    if (deferred) c->CopyResource(g_scanCopy, r); else o_CopyRes(c, g_scanCopy, r);
    g_scanCopied = 1; InterlockedIncrement(&c_scanCopies);
    return true;
}
// once per HUD frame while the box shows: read an older answer, then queue this frame's march
// DC1 run 3 (03:31): at HUD time the depth reads 1.0 everywhere (cleared) - the pass runs right after the LEFT eye's render
// instead (after_eye_render, where DLSS takes the same depth), with that render's own view x lens; the ray is the latest HUD
// frame's (one frame older; only the distance is kept)
void scan_pass(ID3D11DeviceContext* c, const M44& vpNow)
{
    if (!c || !g_scanRay.ok) return;
    if (!o_Map || !o_Unmap || !o_OMSetRT || !o_UpdSub || !o_CSSRV || !o_CSUAV || !o_Dispatch || !o_CopyRes) return;
    ID3D11Device* dev = nullptr; c->GetDevice(&dev); if (!dev) return;
    const bool ready = scan_ensure(dev);
    dev->Release();
    if (!ready) return;
    if (g_scanK >= 3)
    {   // the answer from three passes ago (long finished; never wait)
        const unsigned r = (g_scanK - 3) % 4;
        D3D11_MAPPED_SUBRESOURCE ms{};
        if (o_Map(c, g_scanStage[r], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms) == S_OK)
        {
            const unsigned* rr = (const unsigned*)ms.pData; const unsigned hit = rr[0]; float dist; memcpy(&dist, rr + 1, 4);
            o_Unmap(c, g_scanStage[r], 0);
            InterlockedIncrement(&c_scanReads);
            if (hit < (unsigned)kScanN && dist > 0.0f)
            {   // DETCALLOUT2: the median of the last 7 answers (JJ's run jumped 67 -> 3 -> 52 m for single frames), then
                // small changes eased (no depth shimmer), a lasting jump (another object) taken at once
                static float s_h[7] = {}; static int s_hn = 0, s_hi = 0;
                s_h[s_hi] = dist; s_hi = (s_hi + 1) % 7; if (s_hn < 7) ++s_hn;
                float tmp[7]; for (int i = 0; i < s_hn; ++i) tmp[i] = s_h[i];
                std::sort(tmp, tmp + s_hn); const float med = tmp[s_hn / 2];
                const float old = g_scanDist;
                g_scanDist = (old > 0.0f && fabsf(med - old) < 0.15f * old) ? old + 0.35f * (med - old) : med;
                g_scanRaw = (float)hit;
            }
            else { InterlockedIncrement(&c_scanSky); g_scanRaw = 999.0f; }   // sky: keep the last distance
        }
    }
    UINT bw = 0, bh = 0; int bf = 0; LONG bn = 0;
    ID3D11Resource* depth = scan_take_dsv(bw, bh, bf, bn);   // (diag: logs the left eye's depth buffers)
    // DC1 run 5 (03:42): the scene depth (1520x1656 format 19) ALSO reads 1.0 after the render - it is cleared or reused before
    // the render ends. Our copy, taken at the game's SMAA edge pass (where DLSS gets it), is read instead.
    depth = g_scanCopied ? (ID3D11Resource*)g_scanCopy : nullptr;
    g_scanCopied = 0; g_scanClears = 0; g_scanFromClear = 0;   // the next left-eye render starts again
    static int s_dump = 3;   // DC1 run 6 diag (done: the copied depth, 16 x 16 grid read on the processor); 0 = dump 3 again
    if (depth && s_dump < 3)
    {
        ++s_dump;
        D3D11_TEXTURE2D_DESC sd{}; g_scanCopy->GetDesc(&sd);
        sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
        ID3D11Texture2D* st = nullptr;
        if (SUCCEEDED(g_dev->CreateTexture2D(&sd, nullptr, &st)) && st)
        {
            o_CopyRes(c, st, g_scanCopy);
            D3D11_MAPPED_SUBRESOURCE ms{};
            if (o_Map(c, st, 0, D3D11_MAP_READ, 0, &ms) == S_OK)
            {
                const UINT bpp = sd.Format == DXGI_FORMAT_R32G8X24_TYPELESS ? 8 : 4;
                float mn = 1e30f, mx = -1e30f; int below = 0; char row[512]; int k = 0;
                for (int gy = 0; gy < 16; ++gy)
                    for (int gx = 0; gx < 16; ++gx)
                    {
                        const UINT x = (UINT)((gx + 0.5f) / 16.0f * sd.Width), y = (UINT)((gy + 0.5f) / 16.0f * sd.Height);
                        float v; memcpy(&v, (const uint8_t*)ms.pData + (size_t)y * ms.RowPitch + (size_t)x * bpp, 4);
                        if (sd.Format == DXGI_FORMAT_R24G8_TYPELESS) v = (float)(*(const uint32_t*)((const uint8_t*)ms.pData + (size_t)y * ms.RowPitch + (size_t)x * 4) & 0xFFFFFF) / 16777215.0f;
                        mn = v < mn ? v : mn; mx = v > mx ? v : mx; if (v < 0.9999f) ++below;
                        if (gy == 8 && k < 480) k += snprintf(row + k, sizeof(row) - k, " %.4f", v);
                    }
                o_Unmap(c, st, 0);
                logf("DETCALLOUT depth dump: %ux%u format %d - min %.6f max %.6f, %d of 256 below 0.9999 | middle row:%s", sd.Width, sd.Height, (int)sd.Format, mn, mx, below, row);
            }
            st->Release();
        }
    }
    ID3D11ShaderResourceView* dsrv = depth ? akn_dlss_srv(depth) : nullptr;
    if (!dsrv) return;
    D3D11_TEXTURE2D_DESC dd{};
    { ID3D11Texture2D* t = nullptr; if (FAILED(depth->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) || !t) return; t->GetDesc(&dd); t->Release(); }
    UINT rw = 0, rh = 0; akn_dlss_render_size(0, rw, rh);
    if (!rw || !rh || rw > dd.Width || rh > dd.Height) { rw = dd.Width; rh = dd.Height; }
    // the march's inputs: drawn area, steps, depth direction | ray start + nearest distance | ray direction + span | view x lens
    float cb[28] = {};
    const M44& M = vpNow;
    auto proj = [&](float t, float o[4]) {
        const float P[3] = { g_scanRay.c[0] + g_scanRay.d[0] * t, g_scanRay.c[1] + g_scanRay.d[1] * t, g_scanRay.c[2] + g_scanRay.d[2] * t };
        for (int k = 0; k < 4; ++k) o[k] = P[0] * M.m[0][k] + P[1] * M.m[1][k] + P[2] * M.m[2][k] + M.m[3][k]; };
    float n4[4], f4[4]; proj(g_scanMin, n4); proj(g_scanMin * kScanSpan, f4);
    if (n4[3] <= 1e-3f || f4[3] <= 1e-3f) return;
    const float sign = (f4[2] / f4[3]) > (n4[2] / n4[3]) ? 1.0f : -1.0f;   // normal: depth grows with distance
    cb[0] = (float)rw; cb[1] = (float)rh; cb[2] = (float)kScanN; cb[3] = sign; g_scanSignDbg = sign; g_scanRwDbg = rw; g_scanRhDbg = rh;
    for (int k = 0; k < 3; ++k) { cb[4 + k] = g_scanRay.c[k]; cb[8 + k] = g_scanRay.d[k]; }
    cb[7] = g_scanMin; cb[11] = kScanSpan;
    for (int r = 0; r < 4; ++r) for (int k = 0; k < 4; ++k) cb[12 + r * 4 + k] = M.m[r][k];
    // our own compute pass inside the game's HUD: targets unbound around it, every compute binding put back
    ID3D11RenderTargetView* rtv[8] = {}; ID3D11DepthStencilView* dsv = nullptr; c->OMGetRenderTargets(8, rtv, &dsv);
    ID3D11ComputeShader* ocs = nullptr; ID3D11ClassInstance* oci[256]; UINT nci = 256; c->CSGetShader(&ocs, oci, &nci);
    ID3D11Buffer* ocb = nullptr; c->CSGetConstantBuffers(0, 1, &ocb);
    ID3D11ShaderResourceView* osrv = nullptr; c->CSGetShaderResources(0, 1, &osrv);
    ID3D11UnorderedAccessView* ouav = nullptr; c->CSGetUnorderedAccessViews(0, 1, &ouav);
    o_OMSetRT(c, 0, nullptr, nullptr);
    o_UpdSub(c, g_scanCb, 0, nullptr, cb, 0, 0);
    c->CSSetShader(g_scanCs, nullptr, 0);
    c->CSSetConstantBuffers(0, 1, &g_scanCb);
    o_CSSRV(c, 0, 1, &dsrv);
    o_CSUAV(c, 0, 1, &g_scanUav, nullptr);
    o_Dispatch(c, 1, 1, 1);
    const unsigned slot = g_scanK % 4;
    o_CopyRes(c, g_scanStage[slot], g_scanOut); g_scanStageMin[slot] = g_scanMin; ++g_scanK;
    InterlockedIncrement(&c_scanPasses);
    c->CSSetShader(ocs, nci ? oci : nullptr, nci); c->CSSetConstantBuffers(0, 1, &ocb); o_CSSRV(c, 0, 1, &osrv);
    const UINT keep = (UINT)-1; o_CSUAV(c, 0, 1, &ouav, &keep);
    o_OMSetRT(c, 8, rtv, dsv);
    if (ocs) ocs->Release(); for (UINT i = 0; i < nci; ++i) if (oci[i]) oci[i]->Release();
    if (ocb) ocb->Release(); if (osrv) osrv->Release(); if (ouav) ouav->Release();
    for (auto* r : rtv) if (r) r->Release(); if (dsv) dsv->Release();
}

// DEPTHPROBE 2026-10-12 (DC1 lead): does the depth DLSS / camera motion get (the SMAA edge pass input) hold the world, or only the
// foreground group as in detective mode? One left-eye render: that depth vs the copy made before the last mid-render depth clear,
// 32 x 32 grid each, read on the processor.
ID3D11Texture2D* g_probeSmaa = nullptr; volatile LONG g_probeSmaaOk = 0;
void probe_copy_smaa(ID3D11DeviceContext* c, bool deferred, ID3D11Resource* r)
{
    if (!g_dev || !c || !r) return;
    if (!g_probeSmaa)
    {
        ID3D11Texture2D* t = nullptr; D3D11_TEXTURE2D_DESC d{};
        if (FAILED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) || !t) return;
        t->GetDesc(&d); t->Release();
        d.Usage = D3D11_USAGE_DEFAULT; d.CPUAccessFlags = 0; d.MiscFlags = 0; d.MipLevels = 1; d.ArraySize = 1;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (d.BindFlags & D3D11_BIND_DEPTH_STENCIL);
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_probeSmaa))) { g_probeSmaa = nullptr; return; }
    }
    if (deferred) c->CopyResource(g_probeSmaa, r); else o_CopyRes(c, g_probeSmaa, r);
    g_probeSmaaOk = 1;
}
void probe_stats(ID3D11DeviceContext* c, ID3D11Texture2D* tex, const char* what)
{
    if (!tex) { logf("DEPTHPROBE %s: none", what); return; }
    D3D11_TEXTURE2D_DESC sd{}; tex->GetDesc(&sd);
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &st)) || !st) return;
    o_CopyRes(c, st, tex);
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (o_Map(c, st, 0, D3D11_MAP_READ, 0, &ms) == S_OK)
    {
        const UINT bpp = sd.Format == DXGI_FORMAT_R32G8X24_TYPELESS ? 8 : 4;
        int below = 0, n = 0; float mn = 2.0f, mx = -1.0f; double sum = 0.0;
        for (int gy = 0; gy < 32; ++gy)
            for (int gx = 0; gx < 32; ++gx)
            {
                const UINT x = (UINT)((gx + 0.5f) / 32.0f * sd.Width), y = (UINT)((gy + 0.5f) / 32.0f * sd.Height);
                float v; memcpy(&v, (const uint8_t*)ms.pData + (size_t)y * ms.RowPitch + (size_t)x * bpp, 4);
                ++n; if (v < 0.9999f) { ++below; sum += v; } mn = v < mn ? v : mn; mx = v > mx ? v : mx;
            }
        o_Unmap(c, st, 0);
        logf("DEPTHPROBE %s: %ux%u format %d - %d of %d grid points hold depth (< 0.9999), min %.5f max %.5f, mean of those %.5f", what, sd.Width, sd.Height, (int)sd.Format, below, n, mn, mx, below ? sum / below : 0.0);
    }
    st->Release();
}
void depth_probe_after(ID3D11DeviceContext* c)
{
    if (!c || !g_dev || !o_Map || !o_Unmap || !o_CopyRes) return;
    probe_stats(c, g_probeSmaaOk ? g_probeSmaa : nullptr, "SMAA-pass depth (DLSS / camera motion input)");
    probe_stats(c, g_scanFromClear ? g_scanCopy : nullptr, "copy before the last mid-render clear (the world)");
    logf("DEPTHPROBE: left-eye depth clears this render %ld, detective %d", (long)g_scanClears, akn_detective_active());
    g_probeSmaaOk = 0;
    if (!g_scanRay.ok) { g_scanCopied = 0; g_scanClears = 0; g_scanFromClear = 0; }
    InterlockedDecrement(&g_depthProbe);
}
}
namespace {
void hud_rec_dump(const char* tag)
{   // HUDREC: the ring, oldest first; counters as per-frame differences
    wchar_t fn[MAX_PATH]; swprintf_s(fn, L"%shudrec_%S.txt", g_dir, tag);
    FILE* f = _wfopen(fn, L"w"); if (!f) return;
    const LONG n = g_hrecN; const LONG first = n > 400 ? n - 400 : 0; const ULONGLONG tEnd = n ? g_hrec[(n - 1) % 400].t : 0;
    fprintf(f, "ms_before_capture seq markers  pt0_x pt0_y  pt1_x pt1_y | world0 | left_eye_pt right_eye_pt | map sx sy tx ty | routed eyePieces flat missReach missNone held drop tagLost heldNow detective (per-frame differences)\n");
    const HudRec* pr = nullptr;
    for (LONG i = first; i < n; ++i)
    {
        const HudRec& r = g_hrec[i % 400];
        auto d = [&](LONG a, LONG b) { return pr ? a - b : 0; };
        fprintf(f, "%6lld %6ld %d  %7.3f %7.3f  %7.3f %7.3f | %9.0f %9.0f %7.0f | %7.3f %7.3f  %7.3f %7.3f | %.4f %.4f %.4f %.4f | %ld %ld %ld %ld %ld %ld %ld %ld %d %d\n",
                (long long)(tEnd - r.t), r.seq, r.n, r.c[0], r.c[1], r.c1[0], r.c1[1], r.w[0], r.w[1], r.w[2], r.e0[0], r.e0[1], r.e1[0], r.e1[1], r.xf[0], r.xf[1], r.xf[2], r.xf[3],
                r.routed, d(r.eyePieces, pr ? pr->eyePieces : 0), d(r.flat, pr ? pr->flat : 0), d(r.missReach, pr ? pr->missReach : 0), d(r.missNone, pr ? pr->missNone : 0), d(r.held, pr ? pr->held : 0), d(r.drop, pr ? pr->drop : 0), d(r.tagLost, pr ? pr->tagLost : 0), r.heldNow, r.det);
        pr = &r;
    }
    fclose(f); logf("HUDREC: %ld HUD frames written to hudrec_%s.txt", n - first, tag);
}
}   // namespace (HUDREC)
// RAINMASK: dlss.cpp reads this eye's mask (null = none this render)
extern "C" ID3D11Texture2D* akn_rain_mask(int eye) { return (eye == 0 || eye == 1) && g_rainMask && g_rmUsed[eye] ? g_rmTex[eye] : nullptr; }
// DIAGKIT: the panel's status line for the TESTS drawer
extern "C" __declspec(dllexport) int akn_diag_status(char* out, int n)
{
    if (!out || n < 8) return 0;
    if (g_fxCollect) _snprintf_s(out, n, _TRUNCATE, "collecting the effects on screen ...");
    else if (g_fxIdx >= 0 && g_fxN > 0) _snprintf_s(out, n, _TRUNCATE, "hiding effect %ld of %ld  (%016llx)", g_fxIdx + 1, g_fxN, (unsigned long long)g_skipPs);
    else if (g_fxN > 0) _snprintf_s(out, n, _TRUNCATE, "%ld effects collected - all shown", g_fxN);
    else _snprintf_s(out, n, _TRUNCATE, "no effects collected yet");
    return 1;
}
