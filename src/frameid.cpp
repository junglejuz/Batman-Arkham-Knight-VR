// AKVR — engine frame identity ("FRAMEID", 2026-09-26)
// ---------------------------------------------------------------------------
// WHY. The headset has to be told which head pose the picture on screen was drawn
// with. A fixed "N Presents ago" was right in one place and wrong in another (JJ:
// delay 3 was steady in the garage, 2 outside), because Unreal 3's game thread runs
// ahead of its render thread by a varying amount. vrframework guide 07 section 3:
// keep the engine, render and presenter clocks separately, and "the mod never
// invents a frame number, it borrows the engine's".
//
// WHAT. UE3 keeps a game-thread frame number and a render-thread frame number (set
// from the game one when the render thread starts that frame). We do not have their
// addresses, so we find them at runtime, once, in steady gameplay:
//   1. three snapshots of the exe's writable data, 32 Presents apart; keep every
//      aligned dword that rose by about 32 both times (a per-frame counter);
//   2. watch those for 300 Presents:
//        RENDER = rises by exactly 1 on every Present (our Present hook runs at the
//                 end of the render thread's frame, so it holds THAT frame's number);
//        GAME   = moves with the camera-finalize count (finalize - GAME never
//                 spreads by more than 1);
//      and RENDER - GAME must be 0 or -1 (render is never ahead of game).
//   3. the finalize that served game frame x is x + c, with c = max(finalize - GAME).
// So at each Present the picture's camera came from finalize RENDER + c, and xr.cpp
// looks up which pose that finalize consumed. Anything odd -> fall back to the fixed
// delay; the diagnostic string says why.
// ---------------------------------------------------------------------------
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <vector>
#include "frameid.h"

namespace {

enum State { WAIT, SNAP1, SNAP2, SNAP3, TRACK, READY, FAILED };
State g_state = WAIT;

struct Region { uint8_t* base; size_t size; };
std::vector<Region> g_regions;
std::vector<uint32_t> g_snapA, g_snapB, g_snapC;
uint32_t g_waitRun = 0, g_step = 0, g_tries = 0;

struct Cand {
    volatile uint32_t* p;
    uint32_t last;
    uint32_t inc1, incOther;
    int64_t minFc, maxFc;       // value - finalize count
    bool first;
};
std::vector<Cand> g_cands;

volatile uint32_t* g_render = nullptr;
volatile uint32_t* g_game = nullptr;
int64_t g_c = 0;                // finalize index = engine frame number + g_c
char g_diag[256] = "frameid: waiting for steady gameplay";

bool read_u32(volatile uint32_t* p, uint32_t& v)
{
    __try { v = *p; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool copy_region(const Region& r, uint32_t* dst)
{
    __try { memcpy(dst, r.base, r.size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void find_regions()
{
    g_regions.clear();
    uint8_t* base = (uint8_t*)GetModuleHandleW(nullptr);
    if (!base) return;
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    size_t total = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
    {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        uint8_t* p = base + sec->VirtualAddress;
        size_t n = sec->Misc.VirtualSize & ~(size_t)3;
        // Only committed, readable pages: walk them.
        uint8_t* end = p + n;
        while (p < end)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(p, &mbi, sizeof(mbi))) break;
            uint8_t* rEnd = (uint8_t*)mbi.BaseAddress + mbi.RegionSize;
            if (rEnd > end) rEnd = end;
            const bool ok = mbi.State == MEM_COMMIT &&
                (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE)) &&
                !(mbi.Protect & PAGE_GUARD);
            if (ok && rEnd > p && total < (64u << 20))
            {
                g_regions.push_back({ p, (size_t)(rEnd - p) & ~(size_t)3 });
                total += (size_t)(rEnd - p);
            }
            p = rEnd;
        }
    }
}

bool snapshot(std::vector<uint32_t>& out)
{
    size_t words = 0;
    for (auto& r : g_regions) words += r.size / 4;
    out.assign(words, 0);
    size_t at = 0;
    for (auto& r : g_regions)
    {
        if (!copy_region(r, out.data() + at)) return false;
        at += r.size / 4;
    }
    return true;
}

void fail(const char* why)
{
    g_state = FAILED;
    _snprintf_s(g_diag, sizeof(g_diag), _TRUNCATE, "frameid: FAILED - %s (fixed delay in use)", why);
    g_snapA.clear(); g_snapA.shrink_to_fit();
    g_snapB.clear(); g_snapB.shrink_to_fit();
    g_snapC.clear(); g_snapC.shrink_to_fit();
    g_cands.clear(); g_cands.shrink_to_fit();
}

} // namespace

void akvr_frameid_tick(bool gameplay, uint64_t finalizeCount)
{
    if (g_state == READY || g_state == FAILED) return;
    if (!gameplay) { if (g_state != WAIT) { g_state = WAIT; g_cands.clear(); } g_waitRun = 0; return; }

    switch (g_state)
    {
    case WAIT:
        if (++g_waitRun < 300) return;                 // ~3 s of steady gameplay
        find_regions();
        if (g_regions.empty()) { fail("no writable sections"); return; }
        if (!snapshot(g_snapA)) { fail("snapshot 1 faulted"); return; }
        g_state = SNAP2; g_step = 0;
        _snprintf_s(g_diag, sizeof(g_diag), _TRUNCATE, "frameid: scanning (%zu regions)", g_regions.size());
        return;
    case SNAP2:
        if (++g_step < 32) return;
        if (!snapshot(g_snapB)) { fail("snapshot 2 faulted"); return; }
        g_state = SNAP3; g_step = 0;
        return;
    case SNAP3:
    {
        if (++g_step < 32) return;
        if (!snapshot(g_snapC)) { fail("snapshot 3 faulted"); return; }
        // Every aligned dword that rose by ~32 twice.
        g_cands.clear();
        size_t at = 0;
        for (auto& r : g_regions)
        {
            const size_t words = r.size / 4;
            for (size_t i = 0; i < words && g_cands.size() < 8192; ++i)
            {
                const uint32_t a = g_snapA[at + i], b = g_snapB[at + i], c = g_snapC[at + i];
                const uint32_t d1 = b - a, d2 = c - b;
                if (a > 100 && d1 >= 20 && d1 <= 44 && d2 >= 20 && d2 <= 44)
                    g_cands.push_back({ (volatile uint32_t*)(r.base + i * 4), c, 0, 0, INT64_MAX, INT64_MIN, true });
            }
            at += words;
        }
        g_snapA.clear(); g_snapA.shrink_to_fit();
        g_snapB.clear(); g_snapB.shrink_to_fit();
        g_snapC.clear(); g_snapC.shrink_to_fit();
        if (g_cands.empty()) { fail("no per-frame counters found"); return; }
        g_state = TRACK; g_step = 0;
        _snprintf_s(g_diag, sizeof(g_diag), _TRUNCATE, "frameid: tracking %zu candidates", g_cands.size());
        return;
    }
    case TRACK:
    {
        for (auto& c : g_cands)
        {
            uint32_t v;
            if (!read_u32(c.p, v)) { c.incOther += 1000; continue; }
            if (!c.first)
            {
                const uint32_t d = v - c.last;
                if (d == 1) ++c.inc1; else if (d > 2) ++c.incOther;
            }
            c.first = false;
            c.last = v;
            const int64_t rel = (int64_t)v - (int64_t)finalizeCount;
            if (rel < c.minFc) c.minFc = rel;
            if (rel > c.maxFc) c.maxFc = rel;
        }
        if (++g_step < 300) return;

        // Classify. RENDER: +1 on every Present. GAME: within 1 of the finalize count.
        const uint32_t samples = g_step - 1;
        std::vector<Cand*> renders, games;
        for (auto& c : g_cands)
        {
            if (c.incOther) continue;
            if (c.inc1 == samples) renders.push_back(&c);
            // The game counter moves with the finalizes, which sometimes step 0 or 2
            // per Present; a counter that stepped exactly 1 every time could just as
            // well be a render-side one, so it does not qualify as GAME.
            else if (c.maxFc - c.minFc <= 1) games.push_back(&c);
        }
        if (games.empty() && ++g_tries < 6)
        {
            // A perfectly steady stretch shows no 0/2 steps. Watch again, longer.
            for (auto& c : g_cands) { c.inc1 = c.incOther = 0; c.minFc = INT64_MAX; c.maxFc = INT64_MIN; c.first = true; }
            g_step = 0;
            return;
        }
        // A render counter holds a game frame number: RENDER - GAME is 0 or -1.
        for (Cand* g : games)
        {
            uint32_t gv; if (!read_u32(g->p, gv)) continue;
            for (Cand* r : renders)
            {
                if (r == g) continue;
                uint32_t rv; if (!read_u32(r->p, rv)) continue;
                const int64_t d = (int64_t)rv - (int64_t)gv;
                if (d == 0 || d == -1)
                {
                    g_render = r->p; g_game = g->p;
                    g_c = -g->minFc;     // finalize - game at its largest
                    g_state = READY;
                    _snprintf_s(g_diag, sizeof(g_diag), _TRUNCATE,
                        "frameid: READY render %p game %p (c=%lld; %zu render / %zu game candidates)",
                        (void*)g_render, (void*)g_game, (long long)g_c, renders.size(), games.size());
                    g_cands.clear(); g_cands.shrink_to_fit();
                    return;
                }
            }
        }
        char why[96];
        _snprintf_s(why, sizeof(why), _TRUNCATE, "no render/game pair (%zu render, %zu game)",
                    renders.size(), games.size());
        fail(why);
        return;
    }
    default: return;
    }
}

bool akvr_frameid_presented_finalize(uint64_t& finalizeIndex)
{
    if (g_state != READY || !g_render) return false;
    uint32_t r;
    if (!read_u32(g_render, r)) { fail("render counter unreadable"); return false; }
    const int64_t f = (int64_t)r + g_c;
    if (f <= 0) return false;
    finalizeIndex = (uint64_t)f;
    return true;
}

const char* akvr_frameid_diag() { return g_diag; }
