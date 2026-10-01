// GEOSCALE 2026-09-26 — the panel's "world scale" sets geo-11's convergence.
//
// JJ, measured with geo-11's own keys: Ctrl+F6 (convergence up) makes the world look
// SMALLER, Ctrl+F5 the opposite; separation is not needed in VR. World scale 1.00 =
// convergence 500 (JJ), bigger world = lower convergence: conv = 500 / scale.
//
// LIVE CONTROL IS NOT POSSIBLE FROM HERE — three routes measured dead the same day:
//  1. NvAPI_Stereo_SetConvergence: the handle came from the real driver
//     (C:\WINDOWS\SYSTEM32\nvapi64.dll, create -140); geo-11's direct mode is not behind it.
//  2. Faking keys to geo-11 through geo11.dll's GetAsyncKeyState import: no effect.
//  3. Detouring user32 GetAsyncKeyState/GetKeyState process-wide and counting callers
//     (build KEYWHO): geo-11 0 reads, game 1.87M, mod 339k, other = MSCTF.dll, dinput8.dll.
//     geo-11 reads the keyboard some other way (raw input); reaching it would need real
//     system-wide key events, which JJ ruled out.
// So the slider writes d3dxdm.ini's dm_convergence, which geo-11 reads at launch. The file
// is edited in place: one line, same line endings, no BOM.
//
// GEOLIVE 2026-09-27 — live after all, by writing geo-11's OWN convergence value. Static
// RE of geo11.dll 0.7.11 (capstone, not a debugger): Ctrl+F5/F6 are d3dxdm.ini presets
// (`convergence = convergence * 1.05`) whose setter writes a float at [G]+0x3054 (current)
// and [G]+0x3008 (the dm_convergence it loaded), G = a global pointer (geo11+0x87E5B0).
// Every frame geo-11's direct-mode update (the function that logs "dm_convergence - %f")
// compares its stereo object's copy (+0x640) with [G]+0x3054 and, when they differ, pushes
// the new value itself. So writing +0x3054 (and +0x3008 to match) is exactly what the key
// does, with no fake input. G and both offsets are FOUND at runtime by pattern (the ini-read
// site of L"dm_convergence" and the log site), so a geo-11 update either still matches or
// cleanly falls back to next-launch. Eased toward the target with a settle threshold
// (playbook ch09 #convergence-implementation). Geo-11's own keys keep working: a value we
// did not write is adopted into the slider.
#include "geo11conv.h"
#include "hudsplit.h"   // HUDLAYER
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <string>
#include <MinHook.h>   // HUDLIVE2: geo-11 per-frame update hook

namespace {
    constexpr float kConvAt1 = 500.0f / 1.1f;   // SCALE110: the convergence-only fallback rebased the same way
    // VRSEP 2026-09-27 — JJ: in VR "convergence and separation appear to do the same thing"
    // (on a monitor separation behaves differently). Correct: geo-11's shift S - S*C/w is a
    // constant slide S (separation alone, ~0.005 of the half-width at 5: invisible) plus a
    // parallax part set by the PRODUCT S*C (the world size). A headset has no screen plane,
    // so only the product matters, and the far field should have (almost) no slide. World
    // scale now sets the product (kProductAt1 = 5 x 500 = scale 1.00, JJ's calibration) and
    // separation is held at kSepVR; the separation slider is gone.
    // SCALE110 2026-10-02 — JJ: "recalibrate the world scale so that 1.1, which is my current setting, is the new 1.0".
    // Scale = kProductAt1 / (S*C), so the old 1.10 (product 2500 / 1.1) is the new 1.00.
    constexpr float kProductAt1 = 2500.0f / 1.1f;
    constexpr float kSepVR = 1.0f;
    bool   g_tried = false, g_fileOk = false, g_userOverride = false;
    float  g_startConv = 0.0f;      // dm_convergence when this session launched
    float  g_scale = 1.0f;
    char   g_diag[320] = "geo-11 world scale: not started";

    // --- GEOLIVE -----------------------------------------------------------------------
    // One geo-11 value we drive: its current slot (what geo-11 pushes to the GPU each frame)
    // and the loaded-ini slot geo-11's own setter writes alongside it.
    struct Chan {
        uint32_t  live = 0, base = 0;
        bool      ok = false, synced = false;
        float     last = 0.0f;        // what we last wrote (or last adopted)
        ULONGLONG touch = 0;          // last slider change (SLIDERFAST: no adopting right after)
        int       ext = 0;            // times the value changed under us (geo-11 keys or geo-11 itself)
    };
    uintptr_t g_liveG = 0;          // address of geo-11's settings pointer
    Chan      g_cv, g_sp;           // convergence, separation (GEOSEP)
    int       g_findTries = 0;
    char      g_liveDiag[200] = "live link: not searched yet";
    // GEOSEP 2026-09-27 — separation, next to convergence: ini dm_separation -> [G]+0x300c
    // (convergence base +4), live [G]+0x3050 (convergence live -4); Ctrl+F3/F4 are presets
    // (`separation = separation +/- 1`) writing both, and the same per-frame geo-11 update
    // pushes +0x3050 to its stereo object (+0x644). geo11.dll 0.7.11, static RE.
    float     g_sep = 5.0f;         // slider value = geo-11 separation as-is
    // HUDDIST 2026-09-27 (JJ: "a way to move the HUD elements closer and further away"). With
    // d3dxdm dm_hud_detection=1, geo-11 shifts each detected HUD draw by clamp(depth * F, min, max),
    // F = the separation slide (per-view setup geo11+0x1CD7C2; depth [G]+0x302C, min +0x3040, max
    // +0x3044, all found here by the ini-read pattern). A world point at distance w is shifted
    // S(1 - C/w), so a HUD "d away" needs depth = 1 - C/d with the LIVE convergence C (negative in the
    // eye view): it follows world scale. 0 = far away (depth 1, the far-field slide).
    constexpr float kUnitsPerM = 100.0f;   // UE3 units ~ cm (AK lean calibration 102/m)
    uint32_t  g_hudDepthOff = 0, g_hudMinOff = 0, g_hudMaxOff = 0;
    float     g_hudDistM = 4.0f, g_hudDepthNow = 1.0f;
    // EYEVIEW 2026-09-27 (SKVR port): while on, geo-11 is held at S = the eye turn and
    // C = -(sep * conv) / S, which keeps the eye translation S*C (so the world scale) exactly
    // as the sliders set it. Written instantly (no easing: a half-way S/C pair is a wrong
    // picture), never adopted back into the sliders; the ini keeps the user's own pair.
    bool      g_evOn = false, g_evApplied = false, g_evPrev = false;
    float     g_evS = 0.0f, g_evC = 0.0f;
    float     g_startSep = -1.0f;   // dm_separation at launch (-1 = not in the ini)
    // HUDLIVE 2026-09-27 (JJ: "dig deeper into how to move the whole HUD further or closer").
    // geo11.dll 0.7.11 static RE (capstone): the HUD shift lives in geo-11's STEREO OBJECT,
    // obj = [swapchain wrapper + 0x1BD8] (the object the game presents through). Stereo setup
    // (0x1CD4B0) writes obj+0x874 = obj+0x870 = obj+0x654 = clamp(depth * [G+0x3014]) once, and
    // copies it into the eye blocks (+0x31C = -h, +0x344 = +h, +0x36C = -h). Every Present,
    // geo-11's update (0x2078E0, path at 0x207C28) compares obj+0x654 with obj+0x874 (next to
    // the convergence/separation compares GEOLIVE relies on) and, when they differ, rebuilds the
    // eye blocks FROM obj+0x874 and sets the upload flag obj+0xCC8. So writing obj+0x874 moves
    // every geo-11-detected HUD draw, live; nothing in geo-11 rewrites it per frame (only a
    // profile load or its auto-HUD keys). One value for all HUD parts (per part is impossible
    // here). Shift law as for a world point at depth w: S(1 - C/w), S = final separation
    // [G+0x3014], C = live convergence; distance 0 = far away (h = S, fuses in the eye view).
    // The code bytes are checked first; any other geo-11 build leaves this off.
    constexpr uint32_t kScToStereo = 0x1BD8, kObjDirty = 0xCC8, kObjHud = 0x874, kObjHud2 = 0x870,
                       kObjConv = 0x640, kObjSep = 0x644, kBlkL = 0x31C, kBlkR = 0x344, kBlk3 = 0x36C,
                       kGFinalSep = 0x3014, kGSep = 0x3050;
    uintptr_t g_sc = 0, g_stereo = 0, g_stereoSc = 0;
    int       g_hudCode = 0;        // 0 unchecked, 1 geo-11 code as analysed, -1 other build
    int       g_hudLag = 0;
    long      g_hudWrites = 0, g_hudBlockWrites = 0, g_hudMiss = 0;
    float     g_hudShift = 0.0f, g_hudS = 0.0f, g_hudC = 0.0f;

    bool seh_read_ptr(uintptr_t a, uintptr_t& out)
    { __try { out = *(volatile uintptr_t*)a; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
    bool seh_read_f(uintptr_t a, float& out)
    { __try { out = *(volatile float*)a; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
    bool seh_write_f(uintptr_t a, float v)
    { __try { *(volatile float*)a = v; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }

    struct Span { const uint8_t* p; size_t n; bool exec; };

    // Section spans of a loaded module, from its own headers.
    int module_spans(HMODULE m, Span* out, int cap)
    {
        const uint8_t* b = (const uint8_t*)m;
        auto dos = (const IMAGE_DOS_HEADER*)b;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        auto nt = (const IMAGE_NT_HEADERS*)(b + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        auto sec = IMAGE_FIRST_SECTION(nt);
        int n = 0;
        for (int i = 0; i < nt->FileHeader.NumberOfSections && n < cap; ++i)
            out[n++] = { b + sec[i].VirtualAddress, sec[i].Misc.VirtualSize,
                         (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 };
        return n;
    }
    // First occurrence of `pat` that starts a string (preceded by a zero byte).
    const uint8_t* find_str(const Span* s, int ns, const void* pat, size_t len)
    {
        for (int i = 0; i < ns; ++i)
        {
            if (s[i].exec || s[i].n < len + 1) continue;
            for (size_t k = 1; k + len <= s[i].n; ++k)
                if (s[i].p[k] == ((const uint8_t*)pat)[0] && s[i].p[k - 1] == 0 &&
                    memcmp(s[i].p + k, pat, len) == 0)
                    return s[i].p + k;
        }
        return nullptr;
    }
    int32_t rd32(const uint8_t* p) { int32_t v; memcpy(&v, p, 4); return v; }
    // mov r64,[rip+d] (48/4C 8B, modrm mod=00 rm=101): the global it loads, else 0.
    uintptr_t rip_mov(const uint8_t* p)
    {
        if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8B && (p[2] & 0xC7) == 0x05)
            return (uintptr_t)(p + 7) + rd32(p + 3);
        return 0;
    }
    // movss with [reg+disp32] (F3 [REX] 0F 10/11 modrm mod=10); returns disp, 0 if not.
    uint32_t movss_disp(const uint8_t* p, uint8_t op, int xmm)
    {
        if (p[0] != 0xF3) return 0;
        int k = 1;
        if ((p[1] & 0xF0) == 0x40) k = 2;
        if (p[k] != 0x0F || p[k + 1] != op) return 0;
        const uint8_t m = p[k + 2];
        if ((m & 0xC0) != 0x80 || (m & 0x07) == 0x04) return 0;   // mod=10, no SIB
        if (((m >> 3) & 7) != xmm) return 0;
        return (uint32_t)rd32(p + k + 3);
    }

    bool live_find()
    {
        HMODULE m = GetModuleHandleW(L"geo11.dll");
        if (!m) m = GetModuleHandleW(L"d3d11.dll");     // WRAP mode: geo-11 IS d3d11.dll
        if (!m) { snprintf(g_liveDiag, sizeof(g_liveDiag), "live link: geo-11 module not loaded"); return false; }
        Span sp[32]; const int ns = module_spans(m, sp, 32);
        static const wchar_t kW[] = L"dm_convergence";      // ini key, with its terminator
        static const char    kA[] = "dm_convergence - %f";  // geo-11's per-frame log line
        static const wchar_t kS[] = L"dm_separation";       // GEOSEP ini key
        const uint8_t* sw = find_str(sp, ns, kW, sizeof(kW));
        const uint8_t* sa = find_str(sp, ns, kA, sizeof(kA) - 1);
        const uint8_t* ss = find_str(sp, ns, kS, sizeof(kS));
        if (!sw || !sa) { snprintf(g_liveDiag, sizeof(g_liveDiag), "live link: geo-11 strings not found (other geo-11 version?)"); return false; }
        uintptr_t G = 0, G2 = 0, GS = 0; uint32_t baseOff = 0, liveOff = 0, sepBase = 0;
        // Pass 0: the ini read (lea rdx,L"dm_convergence"; call; mov rax,[rip+G];
        // movss [rax+base],xmm0) gives G. Pass 1: a log site (lea rdx,"dm_convergence - %f")
        // that also loads G nearby gives the live offset from the movss xmm2 load CLOSEST
        // to the lea (the value being printed).
        for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < ns; ++i)
        {
            if (!sp[i].exec) continue;
            const uint8_t* c = sp[i].p; const size_t n = sp[i].n;
            for (size_t k = 48; k + 80 < n; ++k)
            {
                if (c[k] != 0x48 || c[k + 1] != 0x8D || c[k + 2] != 0x15) continue;   // lea rdx,[rip+d]
                const uintptr_t t = (uintptr_t)(c + k + 7) + rd32(c + k + 3);
                if (pass == 0 && t == (uintptr_t)sw && !G)
                {
                    for (size_t j = 7; j < 64 && !G; ++j)
                        if (uintptr_t g = rip_mov(c + k + j))
                            if (uint32_t off = movss_disp(c + k + j + 7, 0x11, 0)) { G = g; baseOff = off; }
                }
                else if (pass == 0 && ss && t == (uintptr_t)ss && !GS)
                {   // GEOSEP: same shape of ini read for dm_separation
                    for (size_t j = 7; j < 64 && !GS; ++j)
                        if (uintptr_t g = rip_mov(c + k + j))
                            if (uint32_t off = movss_disp(c + k + j + 7, 0x11, 0)) { GS = g; sepBase = off; }
                }
                else if (pass == 1 && G && t == (uintptr_t)sa && !G2)
                {
                    bool loadsG = false; uint32_t off = 0; int best = 1 << 30;
                    for (int j = -40; j < 40; ++j)
                    {
                        const uint8_t* q = c + k + j;
                        if (rip_mov(q) == G) loadsG = true;
                        const uint32_t d = movss_disp(q, 0x10, 2);
                        if (d && abs(j) < best) { best = abs(j); off = d; }
                    }
                    if (loadsG && off) { G2 = G; liveOff = off; }
                }
            }
        }
        if (!G || !G2 || G != G2 || !baseOff || !liveOff || baseOff == liveOff ||
            baseOff > 0x100000 || liveOff > 0x100000)
        {
            snprintf(g_liveDiag, sizeof(g_liveDiag), "live link: code pattern not matched (ini %d, log %d, same %d)",
                     G != 0, G2 != 0, G && G == G2);
            return false;
        }
        g_liveG = G; g_cv.live = liveOff; g_cv.base = baseOff;
        // GEOSEP: only when the separation ini slot sits right after convergence's, as in
        // 0.7.11 - then its live slot sits right before convergence's live slot.
        const bool sepOk = GS == G && sepBase == baseOff + 4;
        // HUDDIST: ini reads of the HUD keys: mov rax,[rip+G]; add rax,imm32 ... lea rdx,L"key"
        auto store_for = [&](const wchar_t* key, size_t keyBytes) -> uint32_t {
            const uint8_t* ks = find_str(sp, ns, key, keyBytes);
            if (!ks) return 0;
            for (int i = 0; i < ns; ++i)
            {
                if (!sp[i].exec) continue;
                const uint8_t* c = sp[i].p; const size_t n = sp[i].n;
                for (size_t k = 48; k + 8 < n; ++k)
                {
                    if (c[k] != 0x48 || c[k + 1] != 0x8D || c[k + 2] != 0x15) continue;
                    if ((uintptr_t)(c + k + 7) + rd32(c + k + 3) != (uintptr_t)ks) continue;
                    for (size_t j = k - 40; j < k; ++j)
                        if (rip_mov(c + j) == G && c[j + 7] == 0x48 && c[j + 8] == 0x05)
                        { uint32_t off; memcpy(&off, c + j + 9, 4); if (off > 0x1000 && off < 0x100000) return off; }
                }
            }
            return 0;
        };
        static const wchar_t kHD[] = L"dm_static_hud_depth";
        static const wchar_t kHMin[] = L"dm_auto_hud_offset_min";
        static const wchar_t kHMax[] = L"dm_auto_hud_offset_max";
        g_hudDepthOff = store_for(kHD, sizeof(kHD));
        g_hudMinOff   = store_for(kHMin, sizeof(kHMin));
        g_hudMaxOff   = store_for(kHMax, sizeof(kHMax));
        if (sepOk) { g_sp.live = liveOff - 4; g_sp.base = sepBase; }
        snprintf(g_liveDiag, sizeof(g_liveDiag), "live link: found (geo-11 +0x%llX, +0x%X / +0x%X), separation %s",
                 (unsigned long long)(G - (uintptr_t)m), liveOff, baseOff, sepOk ? "found" : "NOT matched");
        return true;
    }

    // One channel per Present: adopt geo-11's own key changes into the slider, ease toward
    // the slider's target with a settle threshold (playbook ch09 #convergence-implementation).
    template <class ToGeo, class FromGeo>
    bool chan_tick(uintptr_t p, Chan& c, float& shown, ToGeo toGeo, FromGeo fromGeo,
                   float lo, float hi, float settleFrac, float settleMin)
    {
        float live = 0.0f;
        if (!c.live || !seh_read_f(p + c.live, live) || !(live >= lo && live <= hi)) { c.ok = false; return false; }
        c.ok = true;
        if (!c.synced)
        {   // first contact: the slider shows what geo-11 is running now; nothing moves
            c.synced = true; c.last = live; shown = fromGeo(live);
        }
        else if (fabsf(live - c.last) > 1e-4f * fmaxf(fabsf(c.last), 0.01f))
        {   // changed by someone else (geo-11's keys): follow it - but not within 1.5 s of a
            // slider change, or a geo-11 pull-back would drag the slider with it (JJ: "like
            // something is pushing against them"); the counter says if it happens.
            ++c.ext; c.last = live;
            if (GetTickCount64() - c.touch > 1500) shown = fromGeo(live);
        }
        const float target = toGeo(shown);
        if (fabsf(target - live) > 1e-6f + 1e-5f * fabsf(target))
        {
            float next = live + (target - live) * 0.15f;             // ~0.5 s to settle at 45 Hz
            if (fabsf(target - next) < fmaxf(settleMin, settleFrac * fabsf(target))) next = target;
            if (seh_write_f(p + c.live, next)) { seh_write_f(p + c.base, next); c.last = next; }
        }
        return true;
    }

    bool live_find_safe()
    {
        __try { return live_find(); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        { snprintf(g_liveDiag, sizeof(g_liveDiag), "live link: search faulted, next-launch mode only"); g_findTries = 99; return false; }
    }

    // Every Present once found.
    void live_tick()
    {
        uintptr_t p = 0;
        if (!seh_read_ptr(g_liveG, p) || !p)
        { g_cv.ok = g_sp.ok = false; snprintf(g_liveDiag, sizeof(g_liveDiag), "live link: geo-11 settings not readable yet"); return; }
        // HUDDIST (settings-slot writes, TIDY4: dead) replaced by HUDLIVE: hud_tick() below.
        if (g_evOn && g_cv.live && g_sp.live && g_evS > 1.0f)
        {
            const float S = g_evS;
            const float C = -(kProductAt1 / g_scale) / S;   // VRSEP: the product is the world scale
            float ls = 0.0f, lc = 0.0f;
            if (!seh_read_f(p + g_sp.live, ls) || !seh_read_f(p + g_cv.live, lc)) { g_evApplied = false; return; }
            if (fabsf(ls - S) > 1e-3f || fabsf(lc - C) > 1e-4f * fabsf(C) + 1e-6f)
            {
                seh_write_f(p + g_sp.live, S); seh_write_f(p + g_sp.base, S);
                seh_write_f(p + g_cv.live, C); seh_write_f(p + g_cv.base, C);
            }
            g_evApplied = seh_read_f(p + g_sp.live, ls) && seh_read_f(p + g_cv.live, lc) &&
                          fabsf(ls - S) <= 1e-3f && fabsf(lc - C) <= 1e-4f * fabsf(C) + 1e-6f;
            g_evC = C;
            g_cv.ok = g_sp.ok = true;
            g_cv.synced = g_sp.synced = true;   // never adopt S/C back into the sliders
            g_cv.last = C; g_sp.last = S;
            g_evPrev = true;
            return;
        }
        g_evApplied = false;
        if (g_evPrev && g_cv.live && g_sp.live)
        {   // EVGAME: leaving the eye view (menus): straight back to the user's normal pair
            g_evPrev = false;
            const float c2 = kProductAt1 / (g_scale * kSepVR);
            seh_write_f(p + g_sp.live, kSepVR); seh_write_f(p + g_sp.base, kSepVR);
            seh_write_f(p + g_cv.live, c2);     seh_write_f(p + g_cv.base, c2);
            g_sp.last = kSepVR; g_cv.last = c2; g_cv.synced = g_sp.synced = true;
            return;
        }
        if (!g_sp.live)
        {   // separation not found in this geo-11: convergence alone, as GEOLIVE did
            chan_tick(p, g_cv, g_scale, [](float s) { return kConvAt1 / s; }, [](float c) { return kConvAt1 / c; },
                      0.01f, 100000.0f, 0.002f, 0.0f);
            return;
        }
        // VRSEP: separation held at kSepVR, convergence = product / separation.
        float ls = 0.0f, lc = 0.0f;
        if (!seh_read_f(p + g_sp.live, ls) || !seh_read_f(p + g_cv.live, lc) ||
            !(ls > 0.0f && ls < 1000.0f && lc > 0.01f && lc < 1000000.0f))
        { g_cv.ok = g_sp.ok = false; return; }
        g_cv.ok = g_sp.ok = true;
        if (!g_cv.synced)
        {   // first contact: the slider shows the size geo-11 is running now; nothing moves
            g_cv.synced = g_sp.synced = true; g_cv.last = lc; g_sp.last = ls;
            g_scale = kProductAt1 / (ls * lc);
        }
        else if (fabsf(lc - g_cv.last) > 1e-4f * g_cv.last || fabsf(ls - g_sp.last) > 1e-4f * g_sp.last)
        {   // geo-11's own keys (Ctrl+F3..F6) changed it: follow the size (not right after a
            // slider change, see SLIDERFAST)
            if (fabsf(lc - g_cv.last) > 1e-4f * g_cv.last) ++g_cv.ext;
            if (fabsf(ls - g_sp.last) > 1e-4f * g_sp.last) ++g_sp.ext;
            g_cv.last = lc; g_sp.last = ls;
            if (GetTickCount64() - g_cv.touch > 1500) g_scale = kProductAt1 / (ls * lc);
        }
        if (fabsf(ls - kSepVR) > 1e-3f)
        {   // move separation to kSepVR keeping the product, so the size does not jump
            const float c2 = ls * lc / kSepVR;
            if (seh_write_f(p + g_sp.live, kSepVR) && seh_write_f(p + g_cv.live, c2))
            {
                seh_write_f(p + g_sp.base, kSepVR); seh_write_f(p + g_cv.base, c2);
                ls = kSepVR; lc = c2; g_sp.last = ls; g_cv.last = lc;
            }
        }
        const float target = kProductAt1 / (g_scale * kSepVR);
        if (fabsf(target - lc) > 1e-5f * target)
        {
            float next = lc + (target - lc) * 0.15f;             // ~0.5 s to settle at 45 Hz
            if (fabsf(target - next) < 0.002f * target) next = target;
            if (seh_write_f(p + g_cv.live, next)) { seh_write_f(p + g_cv.base, next); g_cv.last = next; }
        }
    }

    // ---- HUDLIVE -------------------------------------------------------------------------
    bool seh_write_u8(uintptr_t a, uint8_t v)
    { __try { *(volatile uint8_t*)a = v; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
    // The geo-11 code this relies on, byte for byte (geo11.dll 0.7.11, sha1 600ea47f...).
    bool hud_code_ok()
    {
        HMODULE m = GetModuleHandleW(L"geo11.dll");
        if (!m) m = GetModuleHandleW(L"d3d11.dll");
        if (!m) return false;
        struct Sig { uint32_t rva; const char* hex; };
        static const Sig sigs[] = {
            // final separation: [G+0x3014] = [G+0x3010] * [G+0x3050] * k
            { 0x207A50, "f30f108210300000f30f598250300000f30f590568cb5a00f30f118214300000" },
            // mov rax,[rbx+0x1bd8]; mov byte [rax+0xcc8],1   (upload flag)
            { 0x207A70, "488b83d81b0000c680c80c000001" },
            // mov rcx,[rbx+0x1bd8]; mov eax,[rcx+0x874]; mov [rcx+0x654],eax
            { 0x207C04, "488b8bd81b00008b8174080000898154060000" },
            // eye blocks rebuilt from +0x874: left -h (+0x31c), right +h (+0x344), third -h (+0x36c)
            { 0x207E79, "488b83d81b0000f30f1080740800000f57c2f30f11801c030000" },
            { 0x207F45, "488b8bd81b00008b8174080000898144030000" },
            { 0x208007, "488b83d81b0000f30f1080740800000f57c2f30f11806c030000" },
            // stereo setup: movss [rbx+0x874],[rbx+0x870],[rbx+0x654] = h
            { 0x1CD7FC, "f30f119b74080000f30f119b70080000f30f119b54060000" },
            // the per-frame update itself (hooked, HUDLIVE2): prologue, then mov rbx,rcx (= wrapper)
            { 0x2078E0, "48895c241055488d6c24d04881ec30010000" },
        };
        const uint8_t* b = (const uint8_t*)m;
        auto dos = (const IMAGE_DOS_HEADER*)b;
        auto nt = (const IMAGE_NT_HEADERS*)(b + dos->e_lfanew);
        const uint32_t image = nt->OptionalHeader.SizeOfImage;
        for (const Sig& sg : sigs)
        {
            const size_t n = strlen(sg.hex) / 2;
            if (sg.rva + n > image) return false;
            for (size_t i = 0; i < n; ++i)
            {
                unsigned v = 0;
                sscanf_s(sg.hex + i * 2, "%2x", &v);
                if (b[sg.rva + i] != (uint8_t)v) return false;
            }
        }
        return true;
    }
    bool hud_code_ok_safe() { __try { return hud_code_ok(); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
    // HUDLIVE2 2026-09-28 — first run: "stereo object NOT found" every frame. The Present we
    // hook is the REAL swapchain's, one layer below geo-11's wrapper (the wrapper holds the real
    // one at +8 and calls it), so [our swapchain + 0x1BD8] was never geo-11's. The wrapper is the
    // argument of geo-11's own per-frame update (0x2078E0, rcx), so that is hooked instead.
    typedef uintptr_t (*G11UpdateFn)(void* wrapper);
    G11UpdateFn        g_origUpd = nullptr;
    volatile uintptr_t g_ctx = 0;
    int                g_updHook = 0;       // 0 not tried, 1 on, -1 failed
    uintptr_t hkG11Update(void* wrapper) { g_ctx = (uintptr_t)wrapper; return g_origUpd(wrapper); }
    void hook_update()
    {
        HMODULE m = GetModuleHandleW(L"geo11.dll");
        if (!m) m = GetModuleHandleW(L"d3d11.dll");
        g_updHook = -1;
        if (!m) return;
        void* fn = (uint8_t*)m + 0x2078E0;
        MH_Initialize();                         // already initialised elsewhere: harmless
        if (MH_CreateHook(fn, (void*)&hkG11Update, (void**)&g_origUpd) == MH_OK && MH_EnableHook(fn) == MH_OK)
            g_updHook = 1;
    }
    bool near_eq(float a, float b) { return fabsf(a - b) <= 1e-3f * fmaxf(fabsf(a), fabsf(b)) + 1e-5f; }

    // Every Present, after live_tick (so S and C are this frame's).
    void hud_tick()
    {
        if (g_hudCode == 0) g_hudCode = hud_code_ok_safe() ? 1 : -1;
        if (g_hudCode < 0 || g_sp.live != kGSep || !g_cv.live) return;
        if (g_updHook == 0) hook_update();
        if (g_updHook < 0) return;
        g_sc = g_ctx;                           // the wrapper geo-11 last updated
        uintptr_t p = 0;
        float S = 0.0f, C = 0.0f, sep = 0.0f;
        if (!seh_read_ptr(g_liveG, p) || !p || !seh_read_f(p + kGFinalSep, S) || !seh_read_f(p + g_cv.live, C) ||
            !seh_read_f(p + kGSep, sep) || !std::isfinite(S) || !std::isfinite(C) || S == 0.0f)
            return;
        // The stereo object, through the swapchain wrapper the game presents with. Found when
        // its own convergence/separation copies match geo-11's live pair; followed every frame
        // (a new swapchain or a different pointer drops it).
        uintptr_t o = 0;
        if (!g_sc || !seh_read_ptr(g_sc + kScToStereo, o) || o < 0x10000 || (o & 7)) { g_stereo = 0; ++g_hudMiss; return; }
        if (o != g_stereo || g_sc != g_stereoSc)
        {
            float oc = 0.0f, os = 0.0f;
            g_stereo = 0;
            if (!seh_read_f(o + kObjConv, oc) || !seh_read_f(o + kObjSep, os) || !near_eq(oc, C) || !near_eq(os, sep))
            { ++g_hudMiss; return; }                             // not it, or mid-change: next frame
            g_stereo = o; g_stereoSc = g_sc;
        }
        float h = S;                                            // far away: the far-field slide
        if (g_hudDistM > 0.05f) h = S * (1.0f - C / (g_hudDistM * kUnitsPerM));
        // HUDLAYER: the HUD is drawn into our own image and shown as a layer at the HUD distance;
        // geo-11 must draw it identically in both eyes (shift 0) so that image holds ONE flat HUD.
        if (akvr_hudsplit_layer_live()) h = 0.0f;
        const float lim = 60.0f * fabsf(S);
        if (h > lim) h = lim;
        if (h < -lim) h = -lim;
        g_hudShift = h; g_hudS = S; g_hudC = C;
        float cur = 0.0f;
        if (!seh_read_f(o + kObjHud, cur)) { g_stereo = 0; return; }
        if (!near_eq(cur, h))
        {   // geo-11 sees +0x654 != +0x874 on its next update and rebuilds both eyes' blocks
            seh_write_f(o + kObjHud, h); seh_write_f(o + kObjHud2, h);
            ++g_hudWrites; g_hudLag = 0;
            return;
        }
        float r = 0.0f;
        if (seh_read_f(o + kBlkR, r) && !near_eq(r, h))
        {   // the other update path (0x207A50) never rebuilds the blocks: after 3 Presents,
            // write them the way the rebuild does, and flag the upload
            if (++g_hudLag >= 3)
            {
                seh_write_f(o + kBlkL, -h); seh_write_f(o + kBlkR, h); seh_write_f(o + kBlk3, -h);
                seh_write_u8(o + kObjDirty, 1);
                ++g_hudBlockWrites; g_hudLag = 0;
            }
        }
        else g_hudLag = 0;
    }

    std::wstring geo_path(const wchar_t* file)
    {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(GetModuleHandleW(L"geo11.dll"), path, MAX_PATH);
        wchar_t* slash = wcsrchr(path, (wchar_t)0x5c);   // backslash
        if (!slash) return L"";
        *(slash + 1) = 0;
        return std::wstring(path) + file;
    }
    bool read_all(const std::wstring& p, std::string& out)
    {
        FILE* f = _wfopen(p.c_str(), L"rb");
        if (!f) return false;
        char buf[4096]; size_t n;
        out.clear();
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
        fclose(f);
        return true;
    }
    // Position and length of the "<key> = X" line (not commented out; the key must be
    // followed by spaces and '=', so dm_convergence never matches dm_convergence_rate).
    bool find_line(const std::string& s, const char* key, size_t& at, size_t& len, float& value)
    {
        const size_t kl = strlen(key);
        size_t pos = 0;
        while (pos < s.size())
        {
            size_t eol = s.find('\n', pos);
            if (eol == std::string::npos) eol = s.size();
            size_t end = (eol > pos && s[eol - 1] == '\r') ? eol - 1 : eol;
            if (s.compare(pos, kl, key) == 0)
            {
                size_t eq = pos + kl;
                while (eq < end && (s[eq] == ' ' || s[eq] == '\t')) ++eq;
                if (eq < end && s[eq] == '=')
                {
                    value = (float)atof(s.c_str() + eq + 1);
                    at = pos; len = end - pos;
                    return true;
                }
            }
            pos = eol + 1;
        }
        return false;
    }
    // Rewrite one "<key> = value" line in place (temp file + move, line endings kept).
    // Returns true when the file now holds the value.
    bool write_line(const char* key, float value, const char* fmt)
    {
        const std::wstring p = geo_path(L"d3dxdm.ini");
        std::string s;
        size_t at = 0, len = 0;
        float old = 0.0f;
        if (!read_all(p, s) || !find_line(s, key, at, len, old)) return false;
        char num[32]; snprintf(num, sizeof(num), fmt, value);
        if (fabsf((float)atof(num) - old) < 1e-4f) return true;       // unchanged: no write
        char line[80]; snprintf(line, sizeof(line), "%s = %s", key, num);
        s.replace(at, len, line);
        const std::wstring tmp = p + L".akvrtmp";
        FILE* f = _wfopen(tmp.c_str(), L"wb");
        if (!f) return false;
        const bool ok = fwrite(s.data(), 1, s.size(), f) == s.size();
        fclose(f);
        if (!ok || !MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(tmp.c_str()); return false; }
        return true;
    }
}

void akvr_geo11conv_tick(IUnknown*)
{
    // GEOLIVE: search once geo-11 has had time to start, retry a few times, then tick.
    static int frames = 0;
    ++frames;
    if (!g_liveG && g_findTries < 5 && frames % 120 == 0) { ++g_findTries; live_find_safe(); }
    if (g_liveG) { live_tick(); hud_tick(); }   // HUDLIVE after the S/C writes
    if (g_tried) return;
    g_tried = true;
    std::string s;
    size_t at = 0, len = 0;
    float v = 0.0f;
    if (!read_all(geo_path(L"d3dxdm.ini"), s) || !find_line(s, "dm_convergence", at, len, v) || !(v > 0.0f))
    { snprintf(g_diag, sizeof(g_diag), "geo-11 world scale: dm_convergence not found in d3dxdm.ini"); return; }
    g_fileOk = true;
    g_startConv = v;
    g_scale = kConvAt1 / v;                     // the slider shows what geo-11 launched with
    float sv = 0.0f;
    if (find_line(s, "dm_separation", at, len, sv) && sv >= 0.0f)
    {   // GEOSEP / VRSEP: the size is the product of the two
        g_startSep = sv; g_sep = sv;
        if (sv > 0.0f) g_scale = kProductAt1 / (sv * v);
    }
    std::string u;
    g_userOverride = read_all(geo_path(L"d3dx_user.ini"), u) && u.find("convergence") != std::string::npos;
    snprintf(g_diag, sizeof(g_diag),
             "geo-11 launched at convergence %.1f (world scale %.2f). Slider applies at NEXT launch; Ctrl+F5/F6 = live.%s",
             g_startConv, g_scale,
             g_userOverride ? " WARNING: d3dx_user.ini holds a saved convergence (Ctrl+F7) that overrides this." : "");
}

// Write dm_convergence for the next launch (called when the slider is let go).
void akvr_geo11conv_commit()
{
    if (!g_fileOk) return;
    const float sc = g_scale < 0.25f ? 0.25f : g_scale;
    float conv = kConvAt1 / sc;
    if (g_startSep >= 0.0f)
    {   // VRSEP: next launch starts at separation kSepVR with the same size
        write_line("dm_separation", kSepVR, "%.2f");
        conv = kProductAt1 / (sc * kSepVR);
    }
    if (!write_line("dm_convergence", conv, "%.1f")) return;
    snprintf(g_diag, sizeof(g_diag),
             "geo-11 launched at %.1f. SAVED %.1f (world scale %.2f): restart the game to apply. Ctrl+F5/F6 = live.%s",
             g_startConv, conv, g_scale,
             g_userOverride ? " WARNING: d3dx_user.ini (Ctrl+F7) overrides this." : "");
}

float akvr_geo11conv_scale() { return g_scale; }
void  akvr_geo11conv_scale_set(float s) { g_scale = s < 0.25f ? 0.25f : (s > 3.0f ? 3.0f : s); g_cv.touch = GetTickCount64(); }
bool  akvr_geo11conv_ready() { return g_fileOk; }
bool  akvr_geo11conv_live() { return g_cv.ok; }
float akvr_geo11sep_value() { return g_sep; }
void  akvr_geo11sep_set(float v) { g_sep = v < 0.0f ? 0.0f : (v > 100.0f ? 100.0f : v); g_sp.touch = GetTickCount64(); }
bool  akvr_geo11sep_available() { return g_sp.ok || g_startSep >= 0.0f; }
bool  akvr_geo11sep_live() { return g_sp.ok; }
void  akvr_geo11_eyeview(bool on, float S) { g_evOn = on; g_evS = S; }
void  akvr_geo11_hud_dist_set(float m) { g_hudDistM = m < 0.0f ? 0.0f : (m > 20.0f ? 20.0f : m); }   // HUDSPLIT: 0-20 m (JJ)
float akvr_geo11_hud_dist() { return g_hudDistM; }
void* akvr_geo11_wrapper() { return (void*)g_ctx; }   // HUDSPLIT3
bool  akvr_geo11_hud_dist_ok() { return g_hudCode > 0 && g_stereo != 0; }   // HUDLIVE
void  akvr_geo11_swapchain(void* sc) { g_sc = (uintptr_t)sc; }
const char* akvr_geo11_hud_diag()
{
    static char d[240];
    if (g_hudCode < 0) snprintf(d, sizeof(d), "HUD distance: OFF - this geo-11 build is not the one analysed (0.7.11)");
    else if (g_hudCode == 0 || !g_liveG) snprintf(d, sizeof(d), "HUD distance: waiting for geo-11");
    else if (g_updHook < 0) snprintf(d, sizeof(d), "HUD distance: OFF - could not hook geo-11's per-frame update");
    else if (!g_stereo) snprintf(d, sizeof(d), "HUD distance: geo-11 stereo object NOT found yet (%ld misses, update hook %s, wrapper %s)",
                                 g_hudMiss, g_updHook > 0 ? "on" : "not yet", g_ctx ? "seen" : "never seen");
    else snprintf(d, sizeof(d), "HUD distance: LIVE, shift %.5f (S %.5f, C %.3f), writes %ld, eye-block writes %ld",
                  g_hudShift, g_hudS, g_hudC, g_hudWrites, g_hudBlockWrites);
    return d;
}
float akvr_geo11_hud_depth_now() { return g_hudDepthNow; }
bool  akvr_geo11_eyeview_applied() { return g_evApplied; }
const char* akvr_geo11_eyeview_diag()
{
    static char d[200];
    if (!g_evOn) snprintf(d, sizeof(d), "eye view: off this launch");
    else if (!g_cv.live || !g_sp.live) snprintf(d, sizeof(d), "eye view: WAITING for the geo-11 live link (separation must be found)");
    else snprintf(d, sizeof(d), "eye view: geo-11 separation %.1f  convergence %.3f  %s", g_evS, g_evC,
                  g_evApplied ? "APPLIED" : "not applied yet");
    return d;
}
const char* akvr_geo11conv_diag()
{
    static char both[560];
    snprintf(both, sizeof(both), "%s\n   %s   changed outside the sliders: world %d, separation %d",
             g_cv.ok ? "LIVE: the sliders move geo-11 now (also saved for next launch)." : g_diag, g_liveDiag, g_cv.ext, g_sp.ext);
    return both;
}
