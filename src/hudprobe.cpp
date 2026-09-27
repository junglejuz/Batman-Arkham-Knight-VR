// HUDPROBE 2026-09-26 — why does the radar flip to full size?
//
// Measured so far (PLAYBOOK_REVIEW, VPWATCH): the radar's movie keeps its shrunk viewport
// at every Present; no game-side viewport write happens between Presents; the setter has no
// direct callers. So the full-size radar is drawn by a path we only see INSIDE the frame.
// This watches the movie-view vtable (BatmanAK.exe 0x142CC3150; static dump in the review):
// every candidate slot gets a pass-through MinHook detour through a hand-written stub that
// saves rcx/rdx/r8/r9 and xmm0-3, calls probe_log(this, slot), restores them and jumps to
// the original - so the function's signature never has to be known. Each call records the
// time, thread, slot, movie, its stored viewport (+0xa0) and, on the render thread, the
// bound render-target size. Busy slots (>3000 calls/s) are only counted. F2 dumps the ring.
#include "hudprobe.h"
#include <windows.h>
#include <d3d11.h>
#include <MinHook.h>
#include <cstdio>
#include <cstring>
#include <cstdint>

void* akvr_hud_view_vtable();   // earlyres.cpp: the hooked movie-view class, or null

namespace {
    struct Rec
    {
        double t; DWORD tid; int slot; void* view;
        int bufW, bufH, l, t0, w, h;
        int rtW, rtH;
        // GFx 4 MovieImpl layout fields (disassembly of 0x1411ac780 / 0x1411aeae0):
        // +0xcc viewport scale, +0xd0 aspect, +0xe8 scale mode, +0xec alignment,
        // +0xf0..+0xfc visible frame rect (stage units)
        float vscale, vaspect; int mode, align; float fx0, fy0, fx1, fy1;
    };
    constexpr int kRing = 16384;
    Rec   g_ring[kRing];
    volatile LONG g_head = 0;
    volatile LONG g_count[64] = {};
    volatile LONG g_secCount[64] = {};
    bool  g_quiet[64] = {};          // too busy: counted only
    DWORD g_renderTid = 0;
    ID3D11DeviceContext* g_ctx = nullptr;
    bool  g_installed = false, g_tried = false;
    int   g_hooked = 0;
    ULONGLONG g_secStart = 0;
    char  g_diag[512] = "hud probe: not installed";

    void read_vp(void* view, Rec& r)
    {
        __try
        {
            const int* p = (const int*)((uint8_t*)view + 0xa0);
            r.bufW = p[0]; r.bufH = p[1]; r.l = p[2]; r.t0 = p[3]; r.w = p[4]; r.h = p[5];
            const uint8_t* b = (const uint8_t*)view;
            r.vscale = *(const float*)(b + 0xcc); r.vaspect = *(const float*)(b + 0xd0);
            r.mode = *(const int*)(b + 0xe8); r.align = *(const int*)(b + 0xec);
            r.fx0 = *(const float*)(b + 0xf0); r.fy0 = *(const float*)(b + 0xf4);
            r.fx1 = *(const float*)(b + 0xf8); r.fy1 = *(const float*)(b + 0xfc);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { r.bufW = r.bufH = -1; }
    }
    void read_rt(Rec& r)
    {
        r.rtW = r.rtH = 0;
        if (!g_ctx || r.tid != g_renderTid) return;
        ID3D11RenderTargetView* rtv = nullptr;
        g_ctx->OMGetRenderTargets(1, &rtv, nullptr);
        if (!rtv) return;
        ID3D11Resource* res = nullptr;
        rtv->GetResource(&res);
        if (res)
        {
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) && tex)
            {
                D3D11_TEXTURE2D_DESC d{}; tex->GetDesc(&d);
                r.rtW = (int)d.Width; r.rtH = (int)d.Height;
                tex->Release();
            }
            res->Release();
        }
        rtv->Release();
    }
}

extern "C" void probe_log(void* thiz, int slot)
{
    if (slot < 0 || slot >= 64) return;
    InterlockedIncrement(&g_count[slot]);
    if (g_quiet[slot]) return;
    if (InterlockedIncrement(&g_secCount[slot]) > 3000) { g_quiet[slot] = true; return; }
    Rec r{};
    LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
    r.t = 1000.0 * (double)q.QuadPart / (double)f.QuadPart;
    r.tid = GetCurrentThreadId(); r.slot = slot; r.view = thiz;
    read_vp(thiz, r);
    read_rt(r);
    const LONG i = InterlockedIncrement(&g_head) - 1;
    g_ring[i & (kRing - 1)] = r;
}

namespace {
    // push rcx/rdx/r8/r9; sub rsp,0x68; save xmm0-3; mov edx,slot; call probe_log;
    // restore; add rsp,0x68; pop; mov rax,tramp; jmp rax  (rsp 16-aligned at the call)
    uint8_t* make_stub(int slot)
    {
        static const uint8_t pre[] = {
            0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x48, 0x83, 0xEC, 0x68,
            0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30,
            0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50 };
        static const uint8_t post[] = {
            0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30,
            0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50,
            0x48, 0x83, 0xC4, 0x68, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59 };
        uint8_t* s = (uint8_t*)VirtualAlloc(nullptr, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!s) return nullptr;
        size_t n = 0;
        memcpy(s + n, pre, sizeof(pre)); n += sizeof(pre);
        s[n++] = 0xBA; *(int32_t*)(s + n) = slot; n += 4;                       // mov edx, slot
        s[n++] = 0x48; s[n++] = 0xB8; *(uint64_t*)(s + n) = (uint64_t)&probe_log; n += 8;
        s[n++] = 0xFF; s[n++] = 0xD0;                                           // call rax
        memcpy(s + n, post, sizeof(post)); n += sizeof(post);
        s[n++] = 0x48; s[n++] = 0xB8; *(uint64_t*)(s + n) = 0; n += 8;          // mov rax, tramp (patched)
        s[n++] = 0xFF; s[n++] = 0xE0;                                           // jmp rax
        return s;
    }
    void set_tramp(uint8_t* stub, void* tramp)
    {
        // the tramp immediate sits 10 bytes before the end (mov rax imm64 + jmp rax)
        const size_t at = 34 + 5 + 10 + 2 + 34 + 2;
        *(uint64_t*)(stub + at) = (uint64_t)tramp;
        FlushInstructionCache(GetCurrentProcess(), stub, 128);
    }

    // Slots to watch: every non-trivial one from the static dump. Skipped: 1 (GetMovieDef
    // getter), 8 (3-byte body), 12/13/17/19 (one-line getters/setters), 14/15 (viewport,
    // already hooked by earlyres), 28-30/33/36 (tiny thunks).
    const int kSlots[] = { 0, 2, 3, 4, 5, 6, 7, 9, 10, 11, 16, 18, 20, 21, 22, 23, 24, 25,
                           26, 27, 31, 32, 34, 35, 37, 38, 39 };

    void install()
    {
        void** vt = (void**)akvr_hud_view_vtable();
        if (!vt) return;
        g_tried = true;
        MH_Initialize();
        void* done[64] = {};
        int nDone = 0;
        for (int slot : kSlots)
        {
            void* fn = vt[slot];
            bool dup = false;
            for (int k = 0; k < nDone; ++k) if (done[k] == fn) dup = true;
            if (dup || !fn) continue;
            uint8_t* stub = make_stub(slot);
            if (!stub) continue;
            void* tramp = nullptr;
            if (MH_CreateHook(fn, stub, &tramp) != MH_OK || !tramp) continue;
            set_tramp(stub, tramp);
            if (MH_EnableHook(fn) != MH_OK) continue;
            done[nDone++] = fn;
            ++g_hooked;
        }
        g_installed = g_hooked > 0;
    }
}

void akvr_hudprobe_tick(ID3D11DeviceContext* ctx)
{
    g_ctx = ctx;
    g_renderTid = GetCurrentThreadId();
    // TIDY4 2026-09-27: HUDPROBE retired (its question is answered). Its code patches on
    // the movie functions collided with HUDLAYERS' slot-27 hook, so it no longer installs.
    // if (!g_tried) install();
    const ULONGLONG now = GetTickCount64();
    if (now - g_secStart >= 1000)
    {
        g_secStart = now;
        for (int i = 0; i < 64; ++i) g_secCount[i] = 0;
    }
    int n = snprintf(g_diag, sizeof(g_diag), "hud probe: %d functions watched; calls per slot:", g_hooked);
    for (int s : kSlots)
        if (g_count[s] && n < (int)sizeof(g_diag) - 24)
            n += snprintf(g_diag + n, sizeof(g_diag) - n, " %d:%ld%s", s, (long)g_count[s], g_quiet[s] ? "q" : "");
}

void akvr_hudprobe_dump(const wchar_t* path)
{
    FILE* f = _wfopen(path, L"w");
    if (!f) return;
    fprintf(f, "t_ms,tid,render_thread,slot,view,bufW,bufH,left,top,width,height,rtW,rtH,scale,aspect,mode,align,fx0,fy0,fx1,fy1\n");
    const LONG head = g_head;
    const LONG first = head > kRing ? head - kRing : 0;
    for (LONG i = first; i < head; ++i)
    {
        const Rec& r = g_ring[i & (kRing - 1)];
        fprintf(f, "%.3f,%lu,%d,%d,%p,%d,%d,%d,%d,%d,%d,%d,%d,%.4f,%.4f,%d,%d,%.1f,%.1f,%.1f,%.1f\n", r.t, (unsigned long)r.tid,
                r.tid == g_renderTid ? 1 : 0, r.slot, r.view, r.bufW, r.bufH, r.l, r.t0, r.w, r.h, r.rtW, r.rtH,
                r.vscale, r.vaspect, r.mode, r.align, r.fx0, r.fy0, r.fx1, r.fy1);
    }
    fclose(f);
}

const char* akvr_hudprobe_diag() { return g_diag; }
