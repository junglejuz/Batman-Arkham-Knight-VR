// AKVR — camera capture + control (Milestones 2 & 3)
// ------------------------------------------------------------------
// M2 (read): at the instruction  0F 2E 83 70 05 00 00 (ucomiss xmm0,[rbx+0x570])
//   RBX == camera base. A hand-built code-cave trampoline stores RBX so we can
//   read pos/rot/FOV (offsets in akvr/CAMERA_MAP.md).
//
// M3 (write): to make our values win, we NOP the game's own writes to the
//   camera fields (codenamegamma's KillX = pos/rot, ModFOV = FOV), then poke our
//   commanded values in every frame. Suppression is applied only while freecam
//   is active, so normal play is untouched when it's off.
//
// All code patches are applied with other threads frozen so nothing executes a
// half-written instruction.
// ------------------------------------------------------------------
#include "camera.h"
#include "xr.h"           // akvr_xr_head_deg / akvr_xr_session_running (M4a-3)
#include "gamepad.h"      // SWINGEASE: right stick in use
#include <windows.h>
#include <tlhelp32.h>
#include <cstring>
#include <cmath>
#include <cstdio>         // camera trace CSV dump
#include <cstdlib>
#include <atomic>
#include <MinHook.h>      // projVR: whole-function hook on BuildProjectionMatrix

namespace
{
    // ---- signatures (verified unique vs the 2023 exe) ----
    const uint8_t kReadPat[7]  = { 0x0F,0x2E,0x83,0x70,0x05,0x00,0x00 };            // ucomiss xmm0,[rbx+0x570]
    const uint8_t kKillXPat[8] = { 0x89,0x83,0x74,0x05,0x00,0x00,0x8B,0x47 };        // mov [rbx+0x574],eax ...
    const uint8_t kFovPat[11]  = { 0x89,0x83,0x8C,0x05,0x00,0x00,0x48,0x8B,0x5C,0x24,0x30 }; // mov [rbx+0x58C],eax

    // Field offsets from camera base
    // NOTE: yaw/pitch are the reverse of the table's labels — verified in-game
    // 2026-07-23: 0x580 = pitch (+ = up), 0x584 = yaw (+ = turn right).
    enum { OFF_X=0x574, OFF_Y=0x578, OFF_Z=0x57C, OFF_PITCH=0x580, OFF_YAW=0x584, OFF_ROLL=0x588, OFF_FOV=0x58C };
    const float ROT2DEG = 360.0f / 65536.0f;
    const float DEG2ROT = 65536.0f / 360.0f;
    const float DEG2RAD = 3.14159265f / 180.0f;
    const float RAD2DEG = 180.0f / 3.14159265f;

    bool        g_installed = false;
    uintptr_t   g_target    = 0;
    uint64_t*   g_slot      = nullptr;  // cave+0 : stub stores RBX here
    uint64_t*   g_counter   = nullptr;  // cave+8 : stub bumps this each finalize call
    uint8_t     g_readOrig[7]{};       // original bytes at the M2 read-hook target

    // ---- module range ----
    bool module_range(uint8_t*& base, size_t& size)
    {
        HMODULE mod = GetModuleHandleW(L"BatmanAK.exe");
        if (!mod) mod = GetModuleHandleW(nullptr);
        if (!mod) return false;
        auto dos = (IMAGE_DOS_HEADER*)mod;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        auto nt = (IMAGE_NT_HEADERS*)((uint8_t*)mod + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
        base = (uint8_t*)mod; size = nt->OptionalHeader.SizeOfImage;
        return true;
    }
    uintptr_t scan(const uint8_t* pat, size_t len)
    {
        uint8_t* base; size_t size;
        if (!module_range(base, size)) return 0;
        for (size_t i = 0; i + len <= size; ++i)
            if (memcmp(base + i, pat, len) == 0) return (uintptr_t)(base + i);
        return 0;
    }

    // ---- freeze/thaw other threads and write code safely ----
    // Suspending alone is NOT enough: a suspended thread keeps its instruction
    // pointer, and if that pointer is parked INSIDE the bytes we rewrite, it resumes
    // mid-instruction of the NEW bytes — the old and new instruction boundaries don't
    // generally line up. The 5-byte patches got away with it (1 instr <-> 1 instr,
    // only boundary is +0); the 12-byte projection patch (2 instrs <-> jmp+NOPs) does
    // not, and crashed the game with a garbage-address fatal error when toggling VR
    // off (2026-07-25). So: freeze, CHECK every frozen thread's RIP, and if anyone is
    // inside the target region, thaw, yield, and try again — the region is a dozen
    // bytes out of a whole frame, so a clear attempt comes within a try or two.
    void write_code(void* dst, const void* src, size_t len)
    {
        DWORD old;
        VirtualProtect(dst, len, PAGE_EXECUTE_READWRITE, &old);

        const uintptr_t lo = (uintptr_t)dst, hi = (uintptr_t)dst + len;
        DWORD me = GetCurrentProcessId(), self = GetCurrentThreadId();

        for (int attempt = 0; attempt < 200; ++attempt)
        {
            HANDLE frozen[256]; int nFrozen = 0;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap != INVALID_HANDLE_VALUE)
            {
                THREADENTRY32 te{}; te.dwSize = sizeof(te);
                if (Thread32First(snap, &te))
                    do {
                        if (te.th32OwnerProcessID == me && te.th32ThreadID != self && nFrozen < 256)
                            if (HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID))
                            { SuspendThread(t); frozen[nFrozen++] = t; }
                    } while (Thread32Next(snap, &te));
                CloseHandle(snap);
            }

            bool clear = true;
            for (int i = 0; i < nFrozen; ++i)
            {
                CONTEXT c{}; c.ContextFlags = CONTEXT_CONTROL;
                // RIP == lo is fine (it will execute the new first instruction whole);
                // anywhere strictly inside is not.
                if (GetThreadContext(frozen[i], &c) && c.Rip > lo && c.Rip < hi) { clear = false; break; }
            }

            // Last attempt: write anyway — a torn patch is a small chance of a crash,
            // a silently-skipped patch is guaranteed wrong behaviour with no message.
            if (clear || attempt == 199)
            {
                memcpy(dst, src, len);
                FlushInstructionCache(GetCurrentProcess(), dst, len);
                for (int i = 0; i < nFrozen; ++i) { ResumeThread(frozen[i]); CloseHandle(frozen[i]); }
                break;
            }
            for (int i = 0; i < nFrozen; ++i) { ResumeThread(frozen[i]); CloseHandle(frozen[i]); }
            Sleep(0);
        }
        VirtualProtect(dst, len, old, &old);
    }

    uint8_t* alloc_near(uintptr_t target, size_t size)
    {
        const uintptr_t range = 0x7FFF0000ULL;
        uintptr_t lo = (target > range) ? target - range : 0x10000, hi = target + range;
        for (uintptr_t p = (target & ~0xFFFFULL) + 0x10000; p < hi; p += 0x10000)
            if (void* r = VirtualAlloc((void*)p, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)) return (uint8_t*)r;
        for (uintptr_t p = (target & ~0xFFFFULL) - 0x10000; p > lo; p -= 0x10000)
            if (void* r = VirtualAlloc((void*)p, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)) return (uint8_t*)r;
        return nullptr;
    }

    // ---- write-suppression patches (KillX + ModFOV) ----
    struct Suppressor {
        uintptr_t addr = 0; size_t len = 0;
        uint8_t orig[64]{}, patched[64]{};
        bool build_killx() {
            addr = scan(kKillXPat, sizeof(kKillXPat)); if (!addr) return false;
            len = 51;                                   // 6 stores(6B) interleaved w/ 5 loads(3B)
            memcpy(orig, (void*)addr, len);
            memcpy(patched, orig, len);
            const int stores[6] = {0,9,18,27,36,45};    // NOP each 6-byte store, keep the 8B 47 xx loads
            for (int s : stores) memset(patched + s, 0x90, 6);
            return true;
        }
        bool build_fov() {
            addr = scan(kFovPat, sizeof(kFovPat)); if (!addr) return false;
            len = 6;                                     // just the 6-byte store
            memcpy(orig, (void*)addr, len);
            memset(patched, 0x90, len);
            return true;
        }
        void apply(bool on) { if (addr) write_code((void*)addr, on ? patched : orig, len); }
    };
    Suppressor g_killx, g_fov;
    bool g_suppReady = false;

    // ---- freecam state ----
    FreecamState g_fc{};
    ULONGLONG    g_fcLast = 0;

    inline bool key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

    uintptr_t cam_base()
    {
        if (!g_slot) return 0;
        uintptr_t b = (uintptr_t)*g_slot;
        return (b < 0x10000 || b > 0x00007FFFFFFFFFFFULL) ? 0 : b;
    }
}

// =============================== M2 ================================
bool akvr_camera_install()
{
    if (g_installed) return true;
    uintptr_t target = scan(kReadPat, sizeof(kReadPat));
    if (!target) return false;
    g_target = target;

    uint8_t* cave = alloc_near(target, 0x1000);
    if (!cave) return false;

    memcpy(g_readOrig, (void*)target, sizeof(g_readOrig));

    *(uint64_t*)cave       = 0;                        // [0]  cam ptr slot
    *(uint64_t*)(cave + 8) = 0;                        // [8]  finalize counter
    g_slot    = (uint64_t*)cave;
    g_counter = (uint64_t*)(cave + 8);
    uint8_t* code = cave + 16, * p = code;
    *p++ = 0x50;                                       // push rax
    *p++ = 0x48; *p++ = 0x89; *p++ = 0xD8;             // mov  rax, rbx
    *p++ = 0x48; *p++ = 0xA3; *(uint64_t*)p = (uint64_t)cave;       p += 8;  // mov [cave+0], rax  (cam ptr)
    *p++ = 0x48; *p++ = 0xA1; *(uint64_t*)p = (uint64_t)(cave + 8); p += 8;  // mov rax, [cave+8]  (counter)
    *p++ = 0x48; *p++ = 0xFF; *p++ = 0xC0;             // inc  rax
    *p++ = 0x48; *p++ = 0xA3; *(uint64_t*)p = (uint64_t)(cave + 8); p += 8;  // mov [cave+8], rax  (counter++)
    *p++ = 0x58;                                       // pop  rax
    memcpy(p, kReadPat, sizeof(kReadPat)); p += sizeof(kReadPat);   // original instr (sets flags for the je)
    *p++ = 0xE9; *(int32_t*)p = (int32_t)((target + 7) - ((uintptr_t)p + 4)); p += 4;  // jmp back

    uint8_t patch[7];
    patch[0] = 0xE9;
    *(int32_t*)(patch + 1) = (int32_t)((uintptr_t)code - (target + 5));
    patch[5] = 0x90; patch[6] = 0x90;
    write_code((void*)target, patch, sizeof(patch));

    // Prepare (but don't yet apply) the write-suppression patches.
    g_suppReady = g_killx.build_killx() & g_fov.build_fov();

    g_installed = true;
    return true;
}
bool      akvr_camera_installed()      { return g_installed; }
uintptr_t akvr_camera_target()         { return g_target; }
uint64_t  akvr_camera_finalize_count() { return g_counter ? *g_counter : 0; }


CameraView akvr_camera_read()
{
    CameraView v{};
    uintptr_t base = cam_base();
    if (!base) return v;
    __try {
        v.x=*(float*)(base+OFF_X); v.y=*(float*)(base+OFF_Y); v.z=*(float*)(base+OFF_Z);
        v.yaw=*(int32_t*)(base+OFF_YAW); v.pitch=*(int32_t*)(base+OFF_PITCH); v.roll=*(int32_t*)(base+OFF_ROLL);
        v.fov=*(float*)(base+OFF_FOV); v.valid=true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { v.valid=false; }
    return v;
}

// =============================== M3 ================================
void akvr_freecam_toggle()
{
    if (!g_fc.on)
    {
        CameraView v = akvr_camera_read();
        if (!v.valid) return;                 // need a live camera to seed from
        g_fc.x=v.x; g_fc.y=v.y; g_fc.z=v.z;
        g_fc.yaw=v.yaw*ROT2DEG; g_fc.pitch=v.pitch*ROT2DEG; g_fc.roll=v.roll*ROT2DEG;
        g_fc.fov=v.fov;
        if (g_fc.speed <= 0.0f) g_fc.speed = 800.0f;
        if (g_suppReady) { g_killx.apply(true); g_fov.apply(true); g_fc.suppressed=true; }
        g_fcLast = GetTickCount64();
        g_fc.on = true;
    }
    else
    {
        if (g_fc.suppressed) { g_killx.apply(false); g_fov.apply(false); g_fc.suppressed=false; }
        g_fc.on = false;
    }
}
bool akvr_freecam_on() { return g_fc.on; }
FreecamState akvr_freecam_state() { return g_fc; }

void akvr_freecam_update()
{
    if (!g_fc.on) return;
    uintptr_t base = cam_base();
    if (!base) return;

    ULONGLONG now = GetTickCount64();
    float dt = (now - g_fcLast) / 1000.0f;
    if (dt > 0.1f) dt = 0.1f;
    g_fcLast = now;

    float spd  = g_fc.speed * (key(VK_SHIFT) ? 5.0f : 1.0f);
    float rot  = 90.0f * dt;                       // deg/sec
    float mv   = spd * dt;

    // rotation (arrow keys — unused by in-game movement)
    if (key(VK_LEFT))  g_fc.yaw   -= rot;
    if (key(VK_RIGHT)) g_fc.yaw   += rot;
    if (key(VK_UP))    g_fc.pitch += rot;
    if (key(VK_DOWN))  g_fc.pitch -= rot;
    if (g_fc.pitch >  89.0f) g_fc.pitch =  89.0f;
    if (g_fc.pitch < -89.0f) g_fc.pitch = -89.0f;

    // camera-relative basis (UE-ish guess: X fwd, Y right, Z up; +yaw clockwise)
    float yr = g_fc.yaw * DEG2RAD, pr = g_fc.pitch * DEG2RAD;
    float fx = cosf(yr) * cosf(pr), fy = sinf(yr) * cosf(pr), fz = sinf(pr);
    float rx = -sinf(yr),           ry = cosf(yr);   // right vector (sign verified in-game)

    // translation (Numpad — no conflict with WASD)
    if (key(VK_NUMPAD8)) { g_fc.x += fx*mv; g_fc.y += fy*mv; g_fc.z += fz*mv; }
    if (key(VK_NUMPAD2)) { g_fc.x -= fx*mv; g_fc.y -= fy*mv; g_fc.z -= fz*mv; }
    if (key(VK_NUMPAD6) || key(VK_OEM_PERIOD)) { g_fc.x += rx*mv; g_fc.y += ry*mv; }  // strafe R (also '.')
    if (key(VK_NUMPAD4) || key(VK_OEM_COMMA))  { g_fc.x -= rx*mv; g_fc.y -= ry*mv; }  // strafe L (also ',')
    if (key(VK_NUMPAD9)) { g_fc.z += mv; }
    if (key(VK_NUMPAD7)) { g_fc.z -= mv; }
    if (key(VK_ADD))      g_fc.fov += 20.0f * dt;
    if (key(VK_SUBTRACT)) g_fc.fov -= 20.0f * dt;
    if (key(VK_NUMPAD3))  g_fc.speed *= (1.0f + dt);   // faster
    if (key(VK_NUMPAD1))  g_fc.speed *= (1.0f - dt);   // slower

    __try {
        *(float*)  (base+OFF_X)     = g_fc.x;
        *(float*)  (base+OFF_Y)     = g_fc.y;
        *(float*)  (base+OFF_Z)     = g_fc.z;
        *(int32_t*)(base+OFF_YAW)   = (int32_t)(g_fc.yaw   * DEG2ROT);
        *(int32_t*)(base+OFF_PITCH) = (int32_t)(g_fc.pitch * DEG2ROT);
        *(int32_t*)(base+OFF_ROLL)  = (int32_t)(g_fc.roll  * DEG2ROT);
        *(float*)  (base+OFF_FOV)   = g_fc.fov;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// =============================== M4a-3 ============================
// Additive head-tracking. A second code hook at the finalize epilogue adds our
// int32 rotation delta to the fields AFTER the game finalizes the camera, so
// the controller keeps steering (no write-suppression). Delta values are
// computed on the Present thread (akvr_head_update) and dropped into cave slots
// as plain int32; the hand-assembled stub does pure integer `add`s (no xmm).
namespace
{
    // Cave layout (0x1000 alloc from akvr_camera_install):
    //   +0x00 cam ptr | +0x08 finalize counter | +0x10 M2 read stub
    //   +0x80/84/88   int32 dYaw/dPitch/dRoll  (rotation, rotator units)
    //   +0x90/94/98   float dPosX/dPosY/dPosZ  (lean, world units, camera-frame)
    //   +0x100        epilogue additive stub
    bool      g_htInstalled = false;
    bool      g_htOn        = false;
    uintptr_t g_epiAddr     = 0;                 // ModFOV_addr + 6  ('mov rbx,[rsp+0x30]')
    uint8_t   g_epiOrig[5]{}, g_epiJmp[5]{};
    bool      g_epiPatched  = false;             // detour written into live code? (write ONCE, never toggle)
    uint8_t*  g_htEnable    = nullptr;           // cave byte: stub runs its body only when this is 1
    // Rotation: C++ composes base∘head against the base the stub saved, then stores the
    // DIFFERENCE (composed - base) here; the stub adds it to the game's FRESH base.
    // ORBITFIX 2026-09-27: these used to hold the FULL rotator and the stub overwrote
    // with it, so every frame the camera sat at the game's new orbit position but
    // pointed with LAST frame's stick angle (F2 traces: drawn minus correct = -1 whole
    // frame of spin, every row). Batman, at the orbit centre, jittered by that varying
    // step; the far background barely moved. Playbook ch01 #delay-synthetic-latch-head.
    int32_t*  g_dYaw = nullptr, * g_dPitch = nullptr, * g_dRoll = nullptr;   // head delta to add
    int32_t*  g_bYaw = nullptr, * g_bPitch = nullptr, * g_bRoll = nullptr;   // pure base (saved by stub)
    float*    g_dPosX = nullptr, * g_dPosY = nullptr, * g_dPosZ = nullptr;
    float*    g_dFov = nullptr;
    // FOVABS 2026-10-02: the FOV is now WRITTEN, not added. F2 (Batmobile entry, 22:52): the game widened its FOV by
    // ~34 deg on the transition's first frame; the additive delta (computed from the frame before) let one frame through
    // at 138 deg. The stub saves the game's own FOV (g_bFov, for ZOOMVIG) and writes g_aFov when g_fovAbs is 1.
    float*    g_bFov = nullptr;
    float*    g_aFov = nullptr;
    int32_t*  g_fovAbs = nullptr;
    float     g_refQx = 0, g_refQy = 0, g_refQz = 0, g_refQw = 1;  // recenter reference orientation
    // Recenter is YAW-ONLY. OpenXR's LOCAL space is gravity-aligned, so head pitch
    // and roll are already absolute and must pass through untouched; only the
    // heading needs zeroing. Storing the full reference orientation and taking
    // conj(ref)*cur re-expresses the head delta in the reference's own frame, so
    // any pitch you happened to have at recenter time tilts the axes the Euler
    // extraction reads — a pure yaw then reports a roll component.
    float     g_refYawDeg = 0.0f;                            // recenter heading only
    float     g_refPX = 0, g_refPY = 0, g_refPZ = 0;         // recenter reference (meters)
    float     g_htYaw = 0, g_htPitch = 0, g_htRoll = 0;      // applied rot delta (deg, overlay)
    float     g_htLX = 0, g_htLY = 0, g_htLZ = 0;            // head lean (meters, overlay)

    // Per-axis OpenXR->game rotator sign. yaw is KNOWN opposite (OpenXR
    // +yaw = head-left, game +yaw = turn-right). pitch/roll are in-game
    // calibration knobs — flip the sign if a head look/tilt goes the wrong way.
    // Roll IS injected: head tilt must carry through for true 6DOF.
    const float YAW_SIGN   = -1.0f;
    // PITCH flipped +1 -> -1 on 2026-07-25 (in-game: look-up looked down). The
    // compose path pitches by rotating about `right0 = (-sin yaw, cos yaw, 0)`,
    // which for fwd=+X, up=+Z actually points LEFT (fwd x up = -right0), so a
    // positive rotation about it pitches DOWN while OpenXR's +pitch means looking
    // UP. Roll is unaffected — rotating about `fwd` decomposes 1:1 (checked).
    const float PITCH_SIGN = -1.0f;
    const float ROLL_SIGN  = -1.0f;   // Euler roll (Tait-Bryan) — no false roll on yaw+pitch; stereo fixed via -froll eye offset

    // World units per real meter (positional lean gain). UE3 default is 1uu=1cm
    // -> 100; but in third-person a true-scale lean reads as tiny against a
    // distant scene, so this is a live-tunable knob (PageUp/PageDown) that JJ
    // dials by feel. The value that feels right feeds the real 1:1 calibration.
    // 102 = JJ's calibrated 1:1 (kLeanAt1 in hooks.cpp). The old default of 200
    // came back whenever the settings file was regenerated: lean ran ~2x (2026-09-26).
    float g_posScale = 102.0f;

    // Fine-tune TRIM (degrees) on top of the auto Quest-3-locked FOV. The game's
    // render FOV is now forced to the headset's real horizontal FOV every frame
    // (see akvr_head_update) so it no longer "zooms the game" — this is just an
    // optional nudge (Home/End), default 0. Render + display share the FOV → 1:1.
    float g_fovDelta = 0.0f;
    // ZOOMVIG 2026-09-29: the game's OWN FOV this frame (before our lock), read by xr.cpp's zoom
    // vignette - the right-stick-click zoom narrows it, but our lock cancels the magnification.
    volatile float g_gameFov = 0.0f;
    bool g_testNoRot = false, g_testNoPos = false, g_testKeepFov = false;   // CAMTEST (not saved)
    // CAMRESTORE 2026-10-02 — JJ: getting in/out of the Batmobile the camera "pops"; with head tracking off the game's
    // transition plays. F2 23:06 (eight transitions): on each transition's first frame the game's own camera jumped by
    // EXACTLY the head offset of the frame before (e.g. -8.29/-8.82 deg vs -8.28/-8.84) - the game starts the transition
    // from the camera fields as last drawn (head included) and the stub then adds the head again. After every drawn
    // frame (at Present, once per finalize) the fields get the game's own values back: what was drawn is unchanged,
    // the game only ever reads back its own camera.
    bool     g_camRestore = true;
    uint64_t g_restoredFc = ~0ull;     // the finalize whose values were last put back
    // CAMSMOOTH 2026-10-02 — JJ after CAMRESTORE: getting in is fine now, "but getting out, I definitely do" see a pop.
    // F2 23:15: ~0.7 s into each transition the game's OWN camera position jumps ~30 units in one frame (three frames'
    // worth of its path, e.g. steps 12.6 -> 46.2 -> 18.4), frames evenly 18 ms apart (not a hitch); the same jump is in
    // the 22:52 F2 from before CAMRESTORE. On a TV a tiny hitch; next to the car in VR a pop. Called by the finalize
    // stub (game thread) with the camera: when the game's position leaves its own steady path by more than 15 units in
    // one frame, the excess is absorbed and let out again over ~10 frames (x0.8 a frame). Steps over 400 units are
    // cuts (fast travel, menu shots) and pass straight through. g_smoothApplied is what was added this finalize, so
    // CAMRESTORE gives the game back its own position.
    bool  g_camSmooth = true;
    float g_smPrev[3] = {}, g_smVel[3] = {}, g_smCorr[3] = {};
    bool  g_smHave = false;
    volatile float g_smoothApplied[3] = {};
    volatile LONG g_smoothEvents = 0;
    // (TILTSMOOTH, superseded the same night by SWINGEASE below, which covers its tilt step too)
    // TILTSMOOTH 2026-10-02 — JJ with CAMSMOOTH: getting out of the Batmobile "still pops". F2 23:23: the position jumps
    // are gone; what is left is the game's own camera TILTING 3.3-3.5 deg in one frame (sometimes 1.6 more the next) from
    // a still camera, with no sideways turn, at the start of each get in / get out. In every F2 since 1 Oct a still-camera
    // tilt step of 1.5+ deg with under 0.3 deg of turn happens only there (36 times; never on the stick - its first steps
    // come with turn or are smaller). Such a step is soaked up and let out at 0.3 deg a frame; steps over 25 deg are cuts.
    float g_tsPrevYaw = 0.0f, g_tsPrevPitch = 0.0f, g_tsVel = 0.0f, g_tsCorr = 0.0f;
    bool  g_tsHave = false;
    volatile float g_tiltApplied = 0.0f;   // degrees added to the pitch field this finalize (CAMRESTORE puts the base back)
    volatile float g_copyApplied[2] = {};   // AIMHEAD: the copied head taken off this finalize (degrees, yaw / pitch)
    bool g_rbHead = false;   // AIMTRIGGER: the last read-back kept the head in the rotation (left trigger held)
    long g_aimFrames = 0;    // AIMTRIGGER2 diag: frames the head was left in for aiming
    float wrap180(float a) { while (a > 180.0f) a -= 360.0f; while (a < -180.0f) a += 360.0f; return a; }
    // SWINGEASE 2026-10-02 — JJ with TILTSMOOTH: "I still notice the pop when getting out of the Batmobile"; and "is this
    // fix going to be applicable if there are other vehicles in the game or other situations". F2 23:29: no one-frame
    // jumps left; getting out the game swings its camera DOWN ~55 deg in 0.37 s (up to 3.9 deg a frame, ~200 deg/s) and
    // stops dead - deliberate camera work, a whole-world swing in VR. General rule (any vehicle, takedown, scripted move):
    // when the game tilts its camera faster than 1 deg a frame and the right stick is idle, the shown tilt eases after it
    // - speed at most 1.3 deg a frame, changing by at most 0.15 deg a frame (no dead start or stop) - then hands back
    // exactly. Tilt only: the car's own turning is sideways and must not lag. Replayed on six F2 paths (scratch
    // swingsim): 2-15 eases each, lag <= 26 deg for ~0.5 s on the get-out, speed changes <= 0.15 deg/frame.
    bool  g_swingEase = true;
    float g_seShown = 0.0f, g_sePrev = 0.0f, g_seVel = 0.0f, g_seLastStep = 0.0f;
    bool  g_seHave = false, g_seActive = false;
    void tilt_smooth(uintptr_t cam)
    {
        g_tiltApplied = 0.0f;
        if (!g_bPitch) return;
        const float pitch = (float)(*g_bPitch) * ROT2DEG + g_copyApplied[1];   // the game's own, less a copied head (AIMHEAD)
        if (!g_seHave) { g_seShown = g_sePrev = pitch; g_seVel = g_seLastStep = 0.0f; g_seActive = false; g_seHave = true; return; }
        const float dp = wrap180(pitch - g_sePrev);
        g_sePrev = pitch;
        const float old = g_seShown;
        const bool stick = akvr_gamepad_right_stick_ms() < 250;   // the player is turning the camera
        if (fabsf(dp) > 25.0f || !g_swingEase) { g_seShown = pitch; g_seActive = false; g_seVel = 0.0f; }   // a cut / off
        else if (g_seActive && stick)
        {   // hand over to the player quickly but not in one frame
            g_seShown += wrap180(pitch - g_seShown) * 0.5f;
            g_seVel = 0.0f;
            if (fabsf(wrap180(pitch - g_seShown)) < 0.1f) { g_seShown = pitch; g_seActive = false; }
        }
        else if (!g_seActive && fabsf(dp) > 1.0f && !stick)
        {
            g_seActive = true;
            InterlockedIncrement(&g_smoothEvents);
            g_seVel = g_seLastStep;                            // carry on at the speed already shown
        }
        else if (!g_seActive)
            g_seShown = pitch;
        if (g_seActive && !stick)
        {
            const float d = wrap180(pitch - g_seShown);
            const float vt = fmaxf(-1.3f, fminf(1.3f, 0.25f * d));
            g_seVel += fmaxf(-0.15f, fminf(0.15f, vt - g_seVel));
            g_seShown += g_seVel;
            if (fabsf(wrap180(pitch - g_seShown)) < 0.05f && fabsf(dp) <= 1.0f && fabsf(g_seVel) < 0.2f)
            { g_seShown = pitch; g_seActive = false; g_seVel = 0.0f; }
        }
        g_seLastStep = fabsf(dp) > 25.0f ? 0.0f : wrap180(g_seShown - old);
        const float corr = wrap180(g_seShown - pitch);
        if (corr != 0.0f)
        {
            *(int32_t*)(cam + OFF_PITCH) += (int32_t)(corr / ROT2DEG);
            g_tiltApplied = corr;
        }
    }
    // AIMHEAD 2026-10-03 — JJ: "the Batmobile's target reticle for the weapon previously was locked to your face. So that
    // you could use your head for aiming ... it's now locked." CAMRESTORE gave the game its own rotation back after every
    // frame, and the Batmobile's weapon aims along the rotation the game reads back - so the head no longer aimed. The
    // rotation now goes back WITH the head (as before CAMRESTORE); position, FOV and the smoothing still go back to the
    // game's own. The double head at get in / out is handled where it happens instead: on the frame the game copies the
    // drawn camera (its own rotation jumps by exactly the last frame's head offset, within 0.2 deg) that copy is taken off
    // again, easing out over 1.2 s as the game's move takes over. Replayed on the two pre-CAMRESTORE F2 traces: all 15
    // copies found, the shown jump 5-12 deg -> under 1 deg, no other frames touched.
    // AIMHEAD2 2026-10-03 — JJ: "the camera pop when exiting the Batmobile is back. It's most noticeable when looking
    // down at the vehicle." F2 23:07 (before CAMRESTORE) at 53.71 s: the game copied the head's yaw exactly (-3.25) but
    // its pitch only in part (-7.29 of -9.85: its own move started in the same frame), so the both-axes test missed it
    // and the shown view jumped 7 deg. Looking far down a clamped pitch copy would miss it the same way. Now a copy is
    // also (b) one axis exact while the other moves toward its head offset (no further than it + 3 deg), or (c) no head
    // yaw and the pitch jumps toward the head pitch, no further than it - both only from a still camera. The whole
    // one-frame jump comes off (not the head offset), and a new copy adds to what is still easing out. Replayed on seven
    // F2 traces (driving, stick turns, menus): the 15 known copies + the 53.71 s one, nothing else.
    bool  g_aimHead = true;
    float g_cfPrevBase[2] = {}, g_cfPrevHead[2] = {}, g_cfPrevStep[2] = { 9.0f, 9.0f }, g_cfC0[2] = {};
    bool  g_cfHave = false;
    float sgnf(float a) { return a > 0.0f ? 1.0f : -1.0f; }
    bool copy_seen(const float dB[2], const float hp[2], bool still)
    {
        if (fmaxf(fabsf(hp[0]), fabsf(hp[1])) > 0.5f && fabsf(dB[0] - hp[0]) < 0.2f && fabsf(dB[1] - hp[1]) < 0.2f)
            return true;                                                     // (a) both axes exact
        if (!still) return false;
        for (int a = 0; a < 2; ++a)
        {   // (b) one axis exact, the other toward its head offset
            const int o = 1 - a;
            if (fabsf(hp[a]) > 0.5f && fabsf(dB[a] - hp[a]) < 0.2f)
            {
                if (fabsf(hp[o]) < 0.5f && fabsf(dB[o]) < 3.0f) return true;
                if (fabsf(hp[o]) >= 0.5f && sgnf(dB[o]) == sgnf(hp[o]) && fabsf(dB[o]) <= fabsf(hp[o]) + 3.0f) return true;
            }
        }
        return fabsf(hp[0]) < 0.5f && fabsf(dB[0]) < 0.2f && fabsf(hp[1]) > 1.5f && fabsf(dB[1]) >= 1.5f &&
               sgnf(dB[1]) == sgnf(hp[1]) && fabsf(dB[1]) <= fabsf(hp[1]) + 0.2f;   // (c) straight down / up
    }
    // COPYHOLD 2026-10-03 — JJ with AIMHEAD2: the get-out pop is "not as bad but still there". F2 00:46: every copy was
    // caught (11 in 50 s), but the 1.2 s ease-out assumed the game's own move replaces the copied angle. Getting out
    // looking down at the car it KEEPS it (64.66 s: base pitch held at -62 for 4 s), so the doubled head slid back in,
    // 6-7 deg over 1.2 s. The correction now fades only while the game moves its camera by itself (stick idle):
    // x exp(-moved / 15 deg) per finalize, so a kept copy stays corrected and a swing to the game's own target (91.2 s:
    // -68 -> -22 deg) takes it away (11.8 -> 0.5 deg). The player's stick turns keep it (the copy stays in the game's
    // state); a cut (> 25 deg in one finalize) clears it.
    void copy_fix(uintptr_t cam)
    {
        g_copyApplied[0] = g_copyApplied[1] = 0.0f;
        if (!g_bYaw || !g_bPitch || !g_dYaw || !g_dPitch) return;
        const float base[2] = { (float)(*g_bYaw) * ROT2DEG, (float)(*g_bPitch) * ROT2DEG };    // the game's own, this finalize
        const float head[2] = { wrap180((float)(*g_dYaw) * ROT2DEG), wrap180((float)(*g_dPitch) * ROT2DEG) };   // added now
        if (g_cfHave)
        {
            const float dB[2] = { wrap180(base[0] - g_cfPrevBase[0]), wrap180(base[1] - g_cfPrevBase[1]) };
            const bool stick = akvr_gamepad_right_stick_ms() < 250;   // the player is turning the camera
            const bool still = fmaxf(fabsf(g_cfPrevStep[0]), fabsf(g_cfPrevStep[1])) < 0.3f && !stick;
            // AIMTRIGGER: a copy of the head is only possible when the game last read the head back
            static const float kNoHead[2] = { 0.0f, 0.0f };
            if (g_aimHead && copy_seen(dB, g_rbHead ? g_cfPrevHead : kNoHead, still))
            {   // the game started from the drawn camera: take the jump off again (on top of what is still held)
                g_cfC0[0] -= dB[0];
                g_cfC0[1] -= dB[1];
                InterlockedIncrement(&g_smoothEvents);
            }
            else if (fmaxf(fabsf(dB[0]), fabsf(dB[1])) > 25.0f) { g_cfC0[0] = g_cfC0[1] = 0.0f; }   // a cut
            else if (!stick)
            {
                const float f = expf(-(fabsf(dB[0]) + fabsf(dB[1])) / 15.0f);
                g_cfC0[0] *= f; g_cfC0[1] *= f;
            }
            if (fabsf(g_cfC0[0]) < 0.01f && fabsf(g_cfC0[1]) < 0.01f) g_cfC0[0] = g_cfC0[1] = 0.0f;
            g_cfPrevStep[0] = dB[0]; g_cfPrevStep[1] = dB[1];
        }
        g_cfPrevBase[0] = base[0]; g_cfPrevBase[1] = base[1];
        g_cfPrevHead[0] = head[0]; g_cfPrevHead[1] = head[1];
        g_cfHave = true;
        if (!g_aimHead) { g_cfC0[0] = g_cfC0[1] = 0.0f; return; }
        if (g_cfC0[0] == 0.0f && g_cfC0[1] == 0.0f) return;
        const float c[2] = { g_cfC0[0], g_cfC0[1] };
        *(int32_t*)(cam + OFF_YAW)   += (int32_t)(c[0] / ROT2DEG);
        *(int32_t*)(cam + OFF_PITCH) += (int32_t)(c[1] / ROT2DEG);
        g_copyApplied[0] = c[0]; g_copyApplied[1] = c[1];
    }
    // DIVEFIX 2026-10-05 — a player: "when diving camera sometimes spazzes out, not all the time" (video: the view
    // flips during a dive from a high tower). The head was composed onto the game's OWN pitch (akvr_head_update, one
    // frame earlier) and handed over as Euler deltas, but the stub adds them to the game's pitch PLUS SWINGEASE's lag
    // (up to ~26 deg in a dive) and the copy fix. Once the game's dive tilt (its limit is -71.4) plus a head looking
    // down passes straight down, the deltas are the flipped Euler form (yaw and roll +180), so the eased lag was then
    // added the wrong way round. Replay (tools/divesim.py, game -8 -> -71.4 in 0.35 s, head 25 deg down): a 78 deg
    // one-frame jump; with the head also turned 15-30 deg the view sat 60-70 deg off for the whole ease. Now, in the
    // rigid mode (pitchunlink 2), the stub's callback composes the head again onto the camera it is about to show
    // (the game's fresh angle + the copy fix + the ease), every finalize, and writes that rotation. Replay: worst step
    // 1.5 deg (the ease itself), 0 deg off. The head pose comes from the last akvr_head_update (seqlock).
    volatile LONG g_exSeq = 0;       // odd while akvr_head_update is writing the head below
    float g_exHead[3] = {};          // head yaw / pitch / roll as composed (radians, g_htYaw.. signs)
    float g_exQ[4] = {};             // the headset orientation they came from (HUDSTEADY ring)
    volatile bool g_exOn = false;    // rigid mode with the head live
    struct ExRec { int32_t y, p, r; float q[4]; };
    ExRec g_exRec = {};              // the last rotation written by exact_head, as (field - saved base)
    volatile LONG g_exRecSeq = 0;
    LONG g_exWrites = 0;
    void exact_head(uintptr_t cam);  // below decompose_basis
    void __fastcall smooth_cb(uintptr_t cam)
    {
        __try
        {
            copy_fix(cam);      // AIMHEAD: the get in / out copy of the head taken off
            tilt_smooth(cam);   // SWINGEASE (its own switch; replaced TILTSMOOTH)
            if (g_exOn) exact_head(cam);   // DIVEFIX: the head onto the camera as it will be shown
            float* P = (float*)(cam + OFF_X);
            const float p[3] = { P[0], P[1], P[2] };
            g_smoothApplied[0] = g_smoothApplied[1] = g_smoothApplied[2] = 0.0f;
            if (!g_camSmooth || !(p[0] == p[0])) { g_smHave = false; return; }
            if (!g_smHave)
            {
                for (int i = 0; i < 3; ++i) { g_smPrev[i] = p[i]; g_smVel[i] = 0.0f; g_smCorr[i] = 0.0f; }
                g_smHave = true;
                return;
            }
            float d[3], dev[3];
            for (int i = 0; i < 3; ++i) { d[i] = p[i] - g_smPrev[i]; dev[i] = d[i] - g_smVel[i]; }
            const float step = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            const float ad = sqrtf(dev[0] * dev[0] + dev[1] * dev[1] + dev[2] * dev[2]);
            const float sp = sqrtf(g_smVel[0] * g_smVel[0] + g_smVel[1] * g_smVel[1] + g_smVel[2] * g_smVel[2]);
            for (int i = 0; i < 3; ++i) g_smPrev[i] = p[i];
            // replayed on JJ's F2 paths first (scratch smsim): jumps 62-72 -> 28-34 a frame, offsets <= ~40, 13-24 events
            // in 80-100 s; the first version froze the speed after a jump and then fired every frame.
            if (step > 400.0f || ad > 120.0f)
            {   // a cut (fast travel, menu shots, a respawn): pass straight through, start again from here
                for (int i = 0; i < 3; ++i) { g_smVel[i] = step > 400.0f ? 0.0f : d[i]; g_smCorr[i] = 0.0f; }
                return;
            }
            for (int i = 0; i < 3; ++i) g_smCorr[i] *= 0.8f;
            const float thr = 8.0f + 0.3f * sp;              // allowed change of step this frame (faster = more)
            if (ad > thr)
            {   // a jump off the path: absorb the part above the allowance, keep tracking the speed
                const float k = thr / ad;
                for (int i = 0; i < 3; ++i)
                {
                    const float devc = dev[i] * k;
                    g_smCorr[i] -= dev[i] - devc;
                    g_smVel[i] += 0.5f * devc;
                }
                InterlockedIncrement(&g_smoothEvents);
            }
            else
                for (int i = 0; i < 3; ++i) g_smVel[i] = 0.5f * g_smVel[i] + 0.5f * d[i];
            for (int i = 0; i < 3; ++i)
            {
                P[i] = p[i] + g_smCorr[i];
                g_smoothApplied[i] = g_smCorr[i];
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_smHave = false; }
    }

    // AER stereo: which eye we're rendering this frame (0=left, 1=right, set by xr),
    // and the half eye-separation in world units (depth strength). Tunable ([ / ]).
    // Default 2.5 = the value that felt right on first test (2026-07-24).
    int   g_curEye       = 0;
    float g_stereoHalfUU = 2.5f;

    // Shoulder cancel: constant sideways slide of the viewpoint along the camera's
    // right vector, in world units. Cancels Arkham's over-the-shoulder framing so you
    // stand where you're looking. 0 = the game's own framing. Positive = move right.
    float g_shoulderUU = 0.0f;
    // MENUSIDE 2026-10-02 — JJ: on the main menu "the camera is looking straight at Batman, with the menu items on the
    // left ... move the camera over to the left a bit so that Batman is more to the right and the menu items are a bit
    // more readable". A slide along the camera's right vector while the main menu is detected (negative = left).
    // MENUSIDE2: JJ - "when you move your head right the camera moves towards Batman in a weird motion" and "Batman's
    // camera needs to be fixed to how it was before": the slide rode the HEAD-turned right vector. Now the game camera's
    // own (base yaw, level) right, and 0 by default (the menu text moves instead: MENUTEXT, earlyres).
    float g_menuSideUU = -35.0f;   // MENUSIDE4: JJ - at -80 "Batman is way off to the right now"
    // MENUSIDE3: a small slide left again, now on the fixed direction (MENUSIDE2); MENUSIDE4: -80 -> -35

    // DECOUPLED PITCH — 2026-09-26 (vrframework guide 09 section 7). JJ: correct at
    // eye level, but with the camera lowered or raised high, looking left/right also
    // ROLLS. Head yaw turns about the game world's vertical; with the game camera
    // tilted, that axis is tilted relative to the player's real neck, so a pure head
    // turn arrives with roll. Scaling the game's own pitch toward zero in gameplay
    // makes the virtual vertical the real one. 0 = level (guide default), 1 = old.
    // Menus (screen mode) always keep the game's framing.
    float g_basePitchKeep = 0.0f;
    // PITCHUNLINK 2026-09-26 — JJ: "unlink the camera from the pitch so the normal game
    // pitch control works, without the roll as you look left and right" (other mods do
    // it). The roll with the game's tilt kept came from the yaw-axis BLEND below: past
    // 40 deg of camera pitch, head yaw turned about the camera's own tilted up (built for
    // the menu dioramas), which is exactly a rolled horizon. Unlinked = the game's FULL
    // pitch, and head yaw ALWAYS about the world vertical in gameplay:
    //     heading = game heading + head yaw, pitch = game pitch + head pitch, roll = head
    // The rendered horizon can then never roll from a head turn; the stick pitches the
    // view as in the flat game. Cost (why it is a choice, not the default): with the
    // camera tilted, the head's turn is about the virtual vertical rather than the
    // player's tilted-view axis, so near the steep end the scene sweeps slightly
    // rather than sliding. Pole guard below keeps the total short of straight up/down.
    // TIPPED (mode 2) 2026-09-26 — JJ, with mode 1: "when the camera is lifted up and
    // pitching down, moving your head left and right causes the camera to roll". That is
    // mode 1's cost, now measured by eye: a head yaw about the world vertical, seen by a
    // camera pitched p, is a yaw of w*cos(p) about the view's up PLUS a roll of w*sin(p)
    // about the view axis (70% of the turn at 45 deg). Mode 2 composes rigidly, the way
    // UEVR does with decoupled pitch off: camera = game heading + game pitch, THEN the
    // whole head rotation in that frame. A head turn is then a pure turn about the view's
    // own up: the picture slides and never rotates. What it costs instead, and why no
    // mode can avoid a cost: the game's tilt becomes a real tilt of the world in front of
    // you, so looking 90 deg to the side shows that world's horizon sloped by the tilt.
    //   0 = level (stick moves the camera height only), 1 = tilt, horizon kept level,
    //   2 = tilt, world tipped (head turns slide cleanly).
    int   g_pitchUnlink = 2;               // TILTBOX: default = follows the game camera (ROLLSIGN-fixed)
    // STICKHEIGHT 2026-09-26 — JJ, mode 0: "no roll, but the pitch feels dead". AK's boom
    // already lifts the camera as the stick tilts down (camera traces), but with the view
    // level that lift is easy to miss. Other mods make the stick an obvious height control.
    // Extra lift = -sin(game pitch) x this many metres (at the world scale, via the lean
    // scale), on top of the game's own boom: stick fully down (-55 deg) = +0.82 x value.
    // Mode 0, gameplay only. The view never tilts, so it adds no roll.
    float g_stickHeightM = 2.0f;
    // The other two axes of the same camera offset (2026-08-05). Same units, same
    // finalized camera basis: + = up, + = forward (toward what you are looking at).
    float g_offUpUU    = 0.0f;
    float g_offFwdUU   = 0.0f;
    // Master switch for everything the camera gained on 2026-08-05, tied to the panel's
    // "camera fix" tick box so one control reverts the lot. DEFAULT OFF: the shipped
    // default has to be the behaviour we know is good.
    bool  g_camFixOn   = false;

    // ---- CAMERA TRACE -------------------------------------------------------
    // JJ 2026-08-05: spinning and releasing the right stick leaves the camera stopped
    // slightly FORWARD of where it should be, and he asked to log each eye's camera
    // position through a spin so we can analyse it instead of guessing. A ring buffer
    // written every finalize (cheap, no allocation, no I/O on the hot path) and dumped
    // to CSV on a key, so the spin can be performed FIRST and captured after the fact.
    struct TraceRec
    {
        double   t;                      // seconds since the first record
        uint64_t frame;                  // camera-finalize count
        int      eye;                    // -1 centred, 0 left, 1 right
        float    baseYaw;                // the game's OWN heading, degrees
        float    fyaw, fpitch, froll;    // finalized rotation we composed, degrees
        float    camX, camY, camZ;       // the live camera fields as they stand now
        float    dx, dy, dz;             // the position delta we are writing
        float    eyeUU;                  // AER eye offset applied this frame
        float    leanX, leanY, leanZ;    // head lean, metres
        // The head half of the story. Without these the trace shows the camera moving
        // but not what we asked it to do, which is exactly the ambiguity that has made
        // the menu-roll and the off-to-the-right aim impossible to pin down.
        float    headYaw, headPitch, headRoll;   // applied rotation delta from recentre, deg
        float    baseYawRaw, basePitch, baseRoll;// the game's OWN rotator, deg (pre-composition)
        // The DISPLAY side. The camera rotator is now provably exact (final_roll equals
        // head_roll to 4 dp across 8192 frames, no leak from head yaw), so any roll JJ
        // still sees has to be the compositor's half: what the layer pose claims versus
        // what the game actually rendered. These are the other two legs of the old
        // ROLL DIAG triangle — head / game / display.
        float    rawHeadYaw, rawHeadPitch, rawHeadRoll;  // absolute head euler, deg
        float    dispRoll;                                // roll of the submitted layer pose, deg
        // THE LAST BLIND SPOT. Everything above is what we ASK for. These three are what
        // the camera fields actually HOLD when read back — so if anything re-writes the
        // rotator after our epilogue stub on certain cameras, it shows up here as
        // live_roll disagreeing with final_roll, and nowhere else.
        float    liveYaw, livePitch, liveRoll;
    };
    const int  kTraceCap = 8192;         // ~48 s of both eyes at 85 fps
    TraceRec*  g_trace     = nullptr;
    int        g_traceHead = 0;          // next write slot
    int        g_traceLen  = 0;          // how many are valid
    LARGE_INTEGER g_traceFreq{}, g_traceT0{};

    void trace_record(float eyeUU, float dx, float dy, float dz,
                      float fyaw, float fpitch, float froll)
    {
        if (!g_trace)
        {
            g_trace = (TraceRec*)calloc(kTraceCap, sizeof(TraceRec));
            if (!g_trace) return;
            QueryPerformanceFrequency(&g_traceFreq);
            QueryPerformanceCounter(&g_traceT0);
        }
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        CameraView v = akvr_camera_read();

        TraceRec& r = g_trace[g_traceHead];
        r.t = g_traceFreq.QuadPart
                ? (double)(now.QuadPart - g_traceT0.QuadPart) / (double)g_traceFreq.QuadPart
                : 0.0;
        r.frame  = g_counter ? *g_counter : 0;
        r.eye    = g_curEye;
        r.baseYaw = g_bYaw ? (float)(*g_bYaw) * ROT2DEG : 0.0f;
        r.fyaw = fyaw * RAD2DEG; r.fpitch = fpitch * RAD2DEG; r.froll = froll * RAD2DEG;
        r.camX = v.x; r.camY = v.y; r.camZ = v.z;
        r.dx = dx; r.dy = dy; r.dz = dz;
        r.eyeUU = eyeUU;
        r.leanX = g_htLX; r.leanY = g_htLY; r.leanZ = g_htLZ;
        r.headYaw = g_htYaw; r.headPitch = g_htPitch; r.headRoll = g_htRoll;
        r.baseYawRaw = r.baseYaw;
        r.basePitch  = g_bPitch ? (float)(*g_bPitch) * ROT2DEG : 0.0f;
        r.baseRoll   = g_bRoll  ? (float)(*g_bRoll)  * ROT2DEG : 0.0f;
        akvr_xr_head_deg(r.rawHeadYaw, r.rawHeadPitch, r.rawHeadRoll);
        r.dispRoll   = akvr_xr_layer_roll();
        r.liveYaw    = v.yaw   * ROT2DEG;
        r.livePitch  = v.pitch * ROT2DEG;
        r.liveRoll   = v.roll  * ROT2DEG;

        g_traceHead = (g_traceHead + 1) % kTraceCap;
        if (g_traceLen < kTraceCap) ++g_traceLen;
    }

    // RE02: wide-render / centre-crop. Render the 16:9 game frame at a wider
    // horizontal FOV so the vertical FOV reaches the headset's ~90°, then crop
    // the centre headset-HFOV back out at submit time. Softens horizontally but
    // fills the field of view instead of the 16:9 mail-slot.
    bool  g_wideRender = false;

    // Delta rotation via quaternion composition, NOT Euler subtraction. Subtracting
    // two independently-extracted Euler triples (cur - ref) is only valid when
    // pitch/roll are near zero — 3D rotations don't commute, so as soon as pitch is
    // non-level (normal head posture) a pure yaw turn bleeds into an apparent roll.
    // Composing the quaternions first and extracting Euler ONCE from the true delta
    // eliminates that crosstalk. (w,x,y,z) Hamilton product; (w) last in the struct
    // to match XrQuaternionf's (x,y,z,w) member order at the call site.
    struct Quat { float x, y, z, w; };
    inline Quat quat_conj(const Quat& q) { return { -q.x, -q.y, -q.z, q.w }; }
    inline Quat quat_mul(const Quat& a, const Quat& b)
    {
        return {
            a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
            a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
            a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
            a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z
        };
    }
    // Same formula/convention as xr.cpp's quat_to_euler (must match — this is applied
    // to the delta quaternion here rather than the absolute head orientation there).
    void quat_to_euler_ypr(const Quat& q, float& yaw, float& pitch, float& roll)
    {
        float sinp = 2.0f * (q.w*q.x - q.y*q.z);
        pitch = (fabsf(sinp) >= 1.0f) ? copysignf(90.0f, sinp) : asinf(sinp) * (180.0f / 3.14159265f);
        yaw   = atan2f(2.0f*(q.w*q.y + q.x*q.z), 1.0f - 2.0f*(q.x*q.x + q.y*q.y)) * (180.0f / 3.14159265f);
        roll  = atan2f(2.0f*(q.w*q.z + q.x*q.y), 1.0f - 2.0f*(q.z*q.z + q.x*q.x)) * (180.0f / 3.14159265f);
    }

    // --- proper rotation composition (fixes yaw-induced roll) --------------------
    // The additive approach (field += head euler) breaks because rotations don't
    // compose by adding euler components — with any base pitch, a head yaw leaks
    // into roll. Instead we build the game's base orientation and the head offset as
    // real orientations, compose them, and decompose ONCE into the game's rotator.
    // Everything uses the freecam-VERIFIED game basis: forward=(cosY cosP, sinY cosP,
    // sinP), right=(-sinY, cosY, 0), up = forward×right (= +Z level), roll about fwd.
    struct V3 { float x, y, z; };
    inline V3 v_add(V3 a, V3 b) { return { a.x+b.x, a.y+b.y, a.z+b.z }; }
    inline V3 v_scale(V3 a, float s) { return { a.x*s, a.y*s, a.z*s }; }
    inline float v_dot(V3 a, V3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
    inline V3 v_cross(V3 a, V3 b) { return { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x }; }
    inline V3 v_norm(V3 a) { float m = sqrtf(v_dot(a,a)); return (m > 1e-6f) ? v_scale(a, 1.0f/m) : a; }
    // Rodrigues: rotate v about unit axis by angle (radians).
    inline V3 v_rot(V3 v, V3 axis, float ang)
    {
        float c = cosf(ang), s = sinf(ang);
        V3 t1 = v_scale(v, c);
        V3 t2 = v_scale(v_cross(axis, v), s);
        V3 t3 = v_scale(axis, v_dot(axis, v) * (1.0f - c));
        return v_add(v_add(t1, t2), t3);
    }
    // Rotate a vector by a quaternion (v' = q v q^-1). Used to find the head's true
    // forward for a decoupled ROLL (swing-twist), instead of the yaw/pitch-coupled
    // Euler roll that breaks stereo when you tilt your head.
    inline V3 quat_rot(const Quat& q, V3 v)
    {
        V3 u{ q.x, q.y, q.z };
        V3 t = v_scale(v_cross(u, v), 2.0f);
        return v_add(v, v_add(v_scale(t, q.w), v_cross(u, t)));
    }
    void build_basis(float yaw, float pitch, float roll, V3& fwd, V3& right, V3& up)
    {
        float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
        fwd = { cy*cp, sy*cp, sp };
        V3 right0 = { -sy, cy, 0.0f };
        V3 up0 = v_cross(fwd, right0);
        float cr = cosf(roll), sr = sinf(roll);
        right = v_add(v_scale(right0, cr), v_scale(up0, sr));
        up    = v_add(v_scale(up0, cr), v_scale(right0, -sr));
    }
    void decompose_basis(V3 fwd, V3 right, float& yaw, float& pitch, float& roll)
    {
        fwd = v_norm(fwd);

        // GIMBAL LOCK — fixed 2026-08-05. JJ: "a couple of cameras during the MENUS
        // that are looking down at Batman — when you turn your head side to side, the
        // camera rolls as well. This doesn't happen in game."
        //
        // Taking the heading as atan2(fwd.y, fwd.x) fails when the camera looks nearly
        // straight down: fwd.z -> +-1 leaves fwd.x and fwd.y as noise, so yaw swings
        // wildly, `right0` is built from that swinging yaw, and the roll measured
        // against it swings with it. Turning your head then rolls the world. Gameplay
        // cameras are never that steep, which is exactly why only the menus show it.
        //
        // The heading is still perfectly well defined there — it is just carried by the
        // RIGHT vector instead of the forward one. So take it from whichever of the two
        // has more to say: compare their HORIZONTAL magnitudes and use the better
        // conditioned one. No magic threshold, and the two agree when roll is ~0, so
        // switching between them cannot pop.
        // MEASURED AND CORRECTED 2026-08-05 — the first version of this compared the two
        // HORIZONTAL magnitudes and took whichever was larger. That is far too eager:
        // the forward's horizontal length is cos(elevation), and the right vector is
        // horizontal whenever roll is small, so the test flipped to the right-vector
        // branch for ANY camera past 45 degrees of pitch — nowhere near the singularity
        // it was meant to guard.
        //
        // JJ's own idea caught it: he asked to look around a menu and have the camera
        // rotations compared against his head rotations. On a 60-degree-down menu camera
        // the camera was following his head at only **0.64x in yaw and 0.15x in roll**,
        // while a level camera tracked at 1.00x. I had been explaining that away as
        // geometry. It was this line.
        //
        // The right-vector formula is only valid at roll ~= 0, so it must be reserved for
        // the case where the forward genuinely carries no heading at all: within a couple
        // of degrees of straight up or down. Everywhere else, trust the forward.
        const float hFwd = sqrtf(fwd.x * fwd.x + fwd.y * fwd.y);
        if (hFwd > 0.02f)                       // ~88.9 deg from vertical
            yaw = atan2f(fwd.y, fwd.x);
        else
            yaw = atan2f(-right.x, right.y);   // right == (-sin yaw, cos yaw, 0) at roll 0

        float z = fwd.z; z = z < -1.0f ? -1.0f : (z > 1.0f ? 1.0f : z);
        pitch = asinf(z);
        V3 right0 = { -sinf(yaw), cosf(yaw), 0.0f };
        V3 up0 = v_cross(fwd, right0);
        roll = atan2f(v_dot(right, up0), v_dot(right, right0));
    }
    float wrap_pi(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }
    // DIVEFIX: (yaw + 180, +-180 - pitch, roll + 180) is the same rotation. Past straight down decompose_basis gives
    // that flipped form (yaw and roll 180 away from the base); take whichever form is nearer the base, so the deltas
    // stay small and carry on smoothly through the pole (pitch may then pass -90, which the engine draws as is).
    void euler_nearest(float& y, float& p, float& r, float by, float bp, float br)
    {
        const float ay = y + 3.14159265f, ap = (p < 0.0f ? -3.14159265f : 3.14159265f) - p, ar = r + 3.14159265f;
        const float now = fabsf(wrap_pi(y - by)) + fabsf(wrap_pi(p - bp)) + fabsf(wrap_pi(r - br));
        const float alt = fabsf(wrap_pi(ay - by)) + fabsf(wrap_pi(ap - bp)) + fabsf(wrap_pi(ar - br));
        if (alt < now) { y = ay; p = ap; r = ar; }
    }
    // DIVEFIX (see g_exSeq): the rigid composition of akvr_head_update (TIPPED + ROLLSIGN), done again in the finalize
    // stub onto the camera about to be shown - the game's fresh angle plus copy_fix and tilt_smooth, exactly as they
    // added them to the fields.
    void exact_head(uintptr_t cam)
    {
        if (!g_bYaw || !g_bPitch || !g_bRoll) return;
        float h[3], q[4];
        for (int tries = 0;; ++tries)
        {
            const LONG s0 = g_exSeq;
            MemoryBarrier();
            memcpy(h, g_exHead, sizeof(h)); memcpy(q, g_exQ, sizeof(q));
            MemoryBarrier();
            if (!(s0 & 1) && s0 == g_exSeq) break;
            if (tries > 64) return;   // the stub's own add stands this frame
            YieldProcessor();
        }
        const int32_t cY = (int32_t)(g_copyApplied[0] / ROT2DEG), cP = (int32_t)(g_copyApplied[1] / ROT2DEG);
        const int32_t tP = (int32_t)(g_tiltApplied / ROT2DEG);
        const int32_t sYaw = (int32_t)((uint32_t)*g_bYaw + (uint32_t)cY);
        const int32_t sPitch = (int32_t)((uint32_t)*g_bPitch + (uint32_t)cP + (uint32_t)tP);
        const int32_t sRoll = *g_bRoll;
        V3 fwd, right, up;
        build_basis((float)sYaw * ROT2DEG * DEG2RAD, (float)sPitch * ROT2DEG * DEG2RAD, (float)sRoll * ROT2DEG * DEG2RAD, fwd, right, up);
        float b2yaw, b2pitch, b2roll; decompose_basis(fwd, right, b2yaw, b2pitch, b2roll);
        euler_nearest(b2yaw, b2pitch, b2roll, (float)sYaw * ROT2DEG * DEG2RAD, (float)sPitch * ROT2DEG * DEG2RAD, (float)sRoll * ROT2DEG * DEG2RAD);   // DIVEFIX2
        V3 bf, br, bu; build_basis(b2yaw, b2pitch, 0.0f, bf, br, bu);
        V3 hf, hr, hu; build_basis(h[0], -h[1], -h[2], hf, hr, hu);   // -gp: PITCH_SIGN note; -gr: ROLLSIGN
        auto toCam = [&](V3 v) { return v_add(v_add(v_scale(bf, v.x), v_scale(br, v.y)), v_scale(bu, v.z)); };
        fwd = v_norm(toCam(hf)); right = v_norm(toCam(hr));
        float fyaw, fpitch, froll; decompose_basis(fwd, right, fyaw, fpitch, froll);
        froll = -froll;                                                // ROLLSIGN: UE3's roll direction
        euler_nearest(fyaw, fpitch, froll, b2yaw, b2pitch, b2roll);
        const float RAD2ROT = 65536.0f / (2.0f * 3.14159265f);
        const int32_t dY = (int32_t)(wrap_pi(fyaw - b2yaw) * RAD2ROT);
        const int32_t dP = (int32_t)(wrap_pi(fpitch - b2pitch) * RAD2ROT);
        const int32_t dR = (int32_t)(wrap_pi(froll - b2roll) * RAD2ROT);
        *(int32_t*)(cam + OFF_YAW)   = (int32_t)((uint32_t)sYaw   + (uint32_t)dY);
        *(int32_t*)(cam + OFF_PITCH) = (int32_t)((uint32_t)sPitch + (uint32_t)dP);
        *(int32_t*)(cam + OFF_ROLL)  = (int32_t)((uint32_t)sRoll  + (uint32_t)dR);
        InterlockedIncrement(&g_exRecSeq);
        g_exRec.y = (int32_t)((uint32_t)cY + (uint32_t)dY);
        g_exRec.p = (int32_t)((uint32_t)cP + (uint32_t)tP + (uint32_t)dP);
        g_exRec.r = dR;
        memcpy(g_exRec.q, q, sizeof(q));
        InterlockedIncrement(&g_exRecSeq);
        ++g_exWrites;
    }
}

// =============================== RE04 Win 1 ============================
// BuildProjectionMatrix convergence patch. Luma's open-source Arkham Knight
// mod points to VA 0x1400104FF (RVA 0x104FF), where the game stores the
// projection jitter and a constant 1.0f scale:
//
//   0x1400104FF  48 89 43 20                 mov [rbx+0x20], rax
//   0x140010503  48 C7 43 2C 00 00 80 3F     mov qword [rbx+0x2C], 1.0f
//
// A sub-pixel NDC offset and a convergence shift are the same operation at
// different sizes. We replace the 12 bytes with a jmp to a cave, load a
// per-eye packed X/Y offset, then run the stolen instructions.
namespace
{
    const uint8_t kProjPat[12] = { 0x48,0x89,0x43,0x20, 0x48,0xC7,0x43,0x2C,0x00,0x00,0x80,0x3F };
    uintptr_t g_projAddr = 0;
    uint8_t*  g_projCave = nullptr;
    uint8_t     g_projOrig[12]{};       // original bytes at BuildProjectionMatrix
    float     g_convergence = 0.0f;

    inline uint64_t pack_floats(float x, float y)
    {
        union { float f; uint32_t u; } ux = { x }, uy = { y };
        return ((uint64_t)uy.u << 32) | ux.u;
    }

    bool akvr_proj_install()
    {
        if (g_projCave) return true;
        uintptr_t target = scan(kProjPat, sizeof(kProjPat));
        if (!target) return false;
        g_projAddr = target;

        uint8_t* cave = alloc_near(target, 0x1000);
        if (!cave) return false;
        g_projCave = cave;

        memcpy(g_projOrig, (void*)target, sizeof(g_projOrig));
        *(uint64_t*)cave = pack_floats(0.0f, 0.0f); // slot at cave+0

        uint8_t* code = cave + 16, *p = code;
        // mov rax, [cave+0]
        *p++ = 0x48; *p++ = 0xA1; *(uint64_t*)p = (uint64_t)cave; p += 8;
        // mov [rbx+0x20], rax   (stolen)
        *p++ = 0x48; *p++ = 0x89; *p++ = 0x43; *p++ = 0x20;
        // mov qword [rbx+0x2C], 1.0f   (stolen)
        *p++ = 0x48; *p++ = 0xC7; *p++ = 0x43; *p++ = 0x2C;
        *(uint32_t*)p = 0; p += 4; *(uint32_t*)p = 0x3F800000; p += 4;
        // jmp back
        *p++ = 0xE9; *(int32_t*)p = (int32_t)((target + 12) - ((uintptr_t)p + 4)); p += 4;

        FlushInstructionCache(GetCurrentProcess(), code, (size_t)(p - code));

        uint8_t patch[12];
        patch[0] = 0xE9;
        *(int32_t*)(patch + 1) = (int32_t)((uintptr_t)code - (target + 5));
        memset(patch + 5, 0x90, 7);
        write_code((void*)target, patch, sizeof(patch));
        return true;
    }

    void akvr_proj_uninstall()
    {
        if (g_projCave && g_projAddr)
        {
            write_code((void*)g_projAddr, g_projOrig, sizeof(g_projOrig));
            g_projCave = nullptr;
            g_projAddr = 0;
        }
    }

    void proj_refresh_jitter()
    {
        if (!g_projCave) return;
        float x = 0.0f;
        if (g_htOn) {
            if (g_curEye == 0) x = g_convergence;
            else if (g_curEye == 1) x = -g_convergence;
        }
        *(uint64_t*)g_projCave = pack_floats(x, 0.0f);
    }
}

void  akvr_head_convergence_add(float d) { akvr_head_convergence_set(g_convergence + d); }
float akvr_head_convergence() { return g_convergence; }
void  akvr_head_convergence_set(float v)
{
    g_convergence = v;
    if (g_convergence < -0.5f) g_convergence = -0.5f;
    if (g_convergence >  0.5f) g_convergence =  0.5f;
    proj_refresh_jitter();
}

// =============================== projVR ================================
// Engine-level square: rewrite the game's PROJECTION to the Quest's FOV instead
// of fighting the render resolution (Mutar / AnvilEngine2VR technique — see the
// projection-aspect memory note & vrframework guide 08). BuildProjectionMatrix
// (entry 0xDF before the kProjPat spot) writes horizontal scale to out[0] (m00)
// and vertical to out[5] (m11); their ratio IS the aspect. We MinHook it, let it
// run, and only on the MAIN 16:9 camera (ratio ~0.5625) overwrite m00/m11 with the
// headset's per-eye FOV. xr.cpp then presents the whole frame at that FOV — full
// square view, no crop, no wasted render. Depth terms are left untouched.
namespace
{
    using BuildProjFn = void* (*)(void*, float, float, float, float, float, float);
    BuildProjFn oBuildProj = nullptr;
    bool  g_projVR = false;          // rewrite the projection?
    bool  g_projVRHooked = false;
    volatile float g_pvRatio = 0.0f; // last main-camera aspect (m00/m11) — diag
    volatile int   g_pvHits  = 0;    // projections rewritten (running) — diag
    std::atomic<uint64_t> g_projectionObservations{0}; // independent of rewriting
    volatile float g_pvOrigFov = 0.0f; // game's ORIGINAL horizontal FOV (deg) — convention check
    // 2026-08-08: the horizontal-only readout could not distinguish "the vertical
    // rewrite never happened" from "it happened and something undid it" — and the
    // image was stretched vertically by ~1.95x, which is exactly the difference
    // between a 16:9 vertical (27.1 deg half) and the headset's (45 deg half).
    // So record BOTH axes, BEFORE and READ BACK AFTER, plus what we rejected.
    volatile float g_pvOrigVFov = 0.0f;  // game's ORIGINAL vertical FOV (deg)
    volatile float g_pvPostFov  = 0.0f;  // read back from the matrix after we wrote it
    volatile float g_pvPostVFov = 0.0f;
    volatile ULONGLONG g_pvCamTick = 0, g_pvOtherTick = 0;   // PAUSELOOK: last main view at / not at the camera FOV
    volatile ULONGLONG g_pvIgnTick = 0;   // PAUSEFOV2: last projection left out of the test (not frame-shaped)
    volatile float g_pvCamFov = 0, g_pvCamRatio = 0, g_pvOtherFov = 0, g_pvOtherRatio = 0, g_pvOtherField = 0, g_pvIgnFov = 0, g_pvIgnRatio = 0;
    volatile int   g_pvSkips    = 0;     // projections seen but NOT matched
    volatile float g_pvSkipFov  = 0.0f;  // HFOV of the widest one we skipped
    volatile float g_pvSkipRatio= 0.0f;

    // PROJTIGHT: every distinct projection shape the game builds (ratio, HFOV), with
    // a count, for the status file — so which of them is the radar is a reading.
    struct ProjSeen { float ratio, fov; unsigned n; };
    ProjSeen g_projSeen[12] = {};
    volatile LONG g_projSeenLock = 0;
    void proj_seen(float ratio, float fov)
    {
        if (InterlockedCompareExchange(&g_projSeenLock, 1, 0) != 0) return;   // never block
        int free = -1;
        for (int i = 0; i < 12; ++i)
        {
            if (g_projSeen[i].n && fabsf(g_projSeen[i].ratio - ratio) < 0.005f
                && fabsf(g_projSeen[i].fov - fov) < 0.5f) { ++g_projSeen[i].n; free = -2; break; }
            if (!g_projSeen[i].n && free == -1) free = i;
        }
        if (free >= 0) g_projSeen[free] = { ratio, fov, 1 };
        InterlockedExchange(&g_projSeenLock, 0);
    }

    void* hkBuildProj(void* out, float a1, float a2, float a3, float a4, float a5, float a6)
    {
        void* r = oBuildProj(out, a1, a2, a3, a4, a5, a6);
        if (out)
        {
            float* m = (float*)out;
            float x = m[0], y = m[5];
            if (x > 1e-4f && y > 1e-4f)
            {
                // Which projection is the main camera? Two facts, both measured:
                //  1. This engine hard-locks its camera to 16:9 (bConstrainAspectRatio),
                //     so the main camera builds ratio ~0.5625 EVEN WHEN the frame is
                //     square — confirmed 2026-07-27 with a forced 3200x3200 buffer.
                //  2. A frame shape we chose could, in principle, come through as the
                //     frame's own height/width.
                // So accept EITHER band. Keying it only to the backbuffer (as it briefly
                // was) meant that on a square frame nothing matched, projVR silently did
                // nothing, and the 16:9 render got stretched into the square buffer — the
                // skew JJ saw. The FOV sanity band keeps shadow/reflection passes out.
                float ratio = x / y;
                // PER-EYE, not the whole source: under geo-11 the source spans both
                // eyes, so its aspect is twice too wide and this band would never match.
                uint32_t bbW = 0, bbH = 0; akvr_xr_eye_image_size(bbW, bbH);
                float want = (bbW && bbH) ? (float)bbH / (float)bbW : 0.5625f;
                float fovDeg = 2.0f * atanf(1.0f / x) * (180.0f / 3.14159265f);
                // PROJTIGHT 2026-09-26 — JJ: the Batmobile radar "blinks on and off,
                // randomly, very very quickly". The old band (0.45..0.72) also took
                // any texture-sized render — a 1024x720 radar view is 0.703 — and
                // rewrote it with the headset FOV and the FULLVIEW offset. The main
                // camera is hard-locked to 16:9 (bConstrainAspectRatio), 0.5625.
                bool sixteenNine = (ratio > 0.55f && ratio < 0.575f);
                proj_seen(ratio, 2.0f * atanf(1.0f / x) * (180.0f / 3.14159265f));
                bool frameShaped = (ratio > want * 0.88f && ratio < want * 1.12f);
                if ((sixteenNine || frameShaped) && fovDeg > 40.0f && fovDeg < 140.0f)
                {
                    // Record the game's original HFOV so we can confirm m00 == 1/tan(halfH).
                    // If this reads a sane ~65-80 deg, the convention is standard and our
                    // rewrite formula is right (so any skew is a FOV-match issue, not layout).
                    g_pvOrigFov  = fovDeg;
                    g_pvOrigVFov = 2.0f * atanf(1.0f / y) * (180.0f / 3.14159265f);
                    g_pvRatio = ratio;
                    g_projectionObservations.fetch_add(1, std::memory_order_relaxed);
                    {   // PAUSELOOK: is the main view still drawn through the player camera (its FOV)? The map
                        // draws from its own camera, so a different FOV means the pause look must not run.
                        float camFov = 0.0f;
                        const uintptr_t cb = cam_base();
                        if (cb) __try { camFov = *(float*)(cb + OFF_FOV); } __except (EXCEPTION_EXECUTE_HANDLER) { camFov = 0.0f; }
                        // PAUSEFOV 2026-10-03 — JJ: "the pause menu is appearing in a window again". Since CAMRESTORE
                        // the camera's FOV field holds the GAME's FOV between frames, while the main view is drawn at
                        // the headset FOV the stub writes (F2 23:29: field 55.9, main view 104.0), so a paused view
                        // never matched and the pause fell back to the window. The headset FOV counts as the camera's.
                        const float lockFov = (g_fovAbs && *g_fovAbs && g_aFov) ? *g_aFov : 0.0f;
                        // PAUSEFOV2: JJ's pause F2 (00:45:53) still said "main view through the player camera: no". With
                        // a square eye image the main view (and the map's) is frame-shaped; the 16:9 projections seen
                        // since FOVABS (0.568 at the game's own FOV, from the camera between frames) are not the main
                        // view, so they no longer count as "something else". Diag: the last of each, for the status file.
                        const bool eye169 = want > 0.55f && want < 0.575f;
                        const bool mainShaped = frameShaped || eye169;
                        const bool camMatch = (camFov > 1.0f && fabsf(fovDeg - camFov) < 1.0f) || (lockFov > 1.0f && fabsf(fovDeg - lockFov) < 1.0f);
                        if (camMatch && mainShaped) { g_pvCamTick = GetTickCount64(); g_pvCamFov = fovDeg; g_pvCamRatio = ratio; }
                        else if (mainShaped) { g_pvOtherTick = GetTickCount64(); g_pvOtherFov = fovDeg; g_pvOtherRatio = ratio; g_pvOtherField = camFov; }
                        else { g_pvIgnTick = GetTickCount64(); g_pvIgnFov = fovDeg; g_pvIgnRatio = ratio; }
                    }
                    if (g_projVR)
                    {
                        float hH = 0.86f, hV = 0.86f;
                        akvr_xr_eye_half_fov(hH, hV);
                        if (hH > 0.05f) m[0] = 1.0f / tanf(hH);
                        if (hV > 0.05f) m[5] = 1.0f / tanf(hV);
                        // FULLVIEW: off-centre vertically, so the rows cover the eye's
                        // real up..down (39.4 / 50.6 deg on Quest 3). m[9] is M[2][1] in
                        // UE3's row-vector matrix: ndc_y = (y/z)*m[5] + m[9]. ADDED, not
                        // set, so any temporal-AA jitter already there survives.
                        m[9] += akvr_xr_eye_v_offset();
                        g_pvHits++;
                    }
                    // This is only the hook's output, not proof of the projection
                    // reaching the world draw. Validate changes against the pixels.
                    g_pvPostFov  = 2.0f * atanf(1.0f / m[0]) * (180.0f / 3.14159265f);
                    g_pvPostVFov = 2.0f * atanf(1.0f / m[5]) * (180.0f / 3.14159265f);
                }
                else
                {
                    // The ones we walked past. If the MAIN camera is in here, projVR is
                    // rewriting something else and the picture will never be right.
                    g_pvSkips++;
                    if (fovDeg > g_pvSkipFov) { g_pvSkipFov = fovDeg; g_pvSkipRatio = ratio; }
                }
            }
        }
        return r;
    }
}

// What the SETTINGS asked for, remembered independently of whether the hook exists yet.
// settings_load() runs on the first frame after ImGui is up; akvr_projvr_install() runs
// from the camera install path, which can be LATER. The old code did
// `g_projVR = on && g_projVRHooked`, so a load that arrived first was silently dropped to
// OFF — and the next settings_save() then wrote that 0 back over the user's file, making
// the toggle impossible to persist. Remember the wish, apply it when the hook lands.
static bool g_projVRWanted = false;

bool akvr_projvr_install()
{
    if (g_projVRHooked) return true;
    uintptr_t conv = scan(kProjPat, sizeof(kProjPat));   // the mid-function spot
    if (!conv) return false;
    void* target = (void*)(conv - 0xDF);                 // BuildProjectionMatrix entry
    MH_Initialize();                                     // idempotent
    if (MH_CreateHook(target, (void*)&hkBuildProj, (void**)&oBuildProj) != MH_OK) return false;
    if (MH_EnableHook(target) != MH_OK) return false;
    g_projVRHooked = true;
    // The hook exists now — honour a setting that arrived before it did.
    if (g_projVRWanted) { g_projVR = true; akvr_xr_set_projvr(true); }
    return true;
}
bool akvr_projvr_ok()  { return g_projVRHooked; }
// PAUSELOOK: the main view was built at the player camera's FOV in the last 0.3 s, and nothing else for 0.6 s.
bool akvr_camera_main_view_live()
{
    const ULONGLONG now = GetTickCount64();
    // PAUSEFOV3 2026-10-03 — JJ's pause F2 (01:00:37, "pause test" line): every paused frame builds the player view at
    // the camera's FOV (0.990@69.5) AND a second frame-shaped view at 75.0, so "nothing else for 0.6 s" never held.
    // The player view being drawn is what matters; another view beside it no longer blocks the pause look.
    return g_projVRHooked && now - g_pvCamTick < 300;
}
// PAUSEFOV2: what the pause test saw last ("match 0.990@104.0 12 ms ago | other ... | left out ...")
const char* akvr_camera_main_view_diag()
{
    static char s[256];
    const ULONGLONG now = GetTickCount64();
    auto ago = [&](ULONGLONG t) { return t ? (long long)(now - t) : -1ll; };
    snprintf(s, sizeof(s), "match %.3f@%.1f %lld ms ago | other %.3f@%.1f (field %.1f) %lld ms ago | left out %.3f@%.1f %lld ms ago",
             g_pvCamRatio, g_pvCamFov, ago(g_pvCamTick), g_pvOtherRatio, g_pvOtherFov, g_pvOtherField, ago(g_pvOtherTick),
             g_pvIgnRatio, g_pvIgnFov, ago(g_pvIgnTick));
    return s;
}
// PROJTIGHT: "ratio@HFOVxcount" for every distinct projection the game built.
const char* akvr_projection_seen()
{
    static char s[512];
    int n = 0; s[0] = 0;
    for (int i = 0; i < 12; ++i)
        if (g_projSeen[i].n && n < (int)sizeof(s) - 40)
            n += snprintf(s + n, sizeof(s) - n, "%.3f@%.1fx%u ", g_projSeen[i].ratio, g_projSeen[i].fov, g_projSeen[i].n);
    return s;
}

uint64_t akvr_projection_observation_count()
{
    return g_projectionObservations.load(std::memory_order_relaxed);
}
// Report the WISH, not just the live state, so the panel checkbox and settings_save()
// round-trip what the user asked for even on a frame where the hook isn't up yet.
// (Without this the box reads unticked at startup and saves a 0 over the user's file.)
bool akvr_projvr()     { return g_projVR || g_projVRWanted; }
void akvr_projvr_set(bool on)
{
    g_projVRWanted = on;
    g_projVR = on && g_projVRHooked;
    akvr_xr_set_projvr(g_projVR);   // keep the present side (xr.cpp) in lock-step
}
void akvr_projvr_diag(float& ratio, int& hits, float& origFovDeg)
{
    ratio = g_pvRatio; hits = g_pvHits; origFovDeg = g_pvOrigFov;
}
// Both axes, before and after, plus what we rejected — see the note by g_pvOrigVFov.
void akvr_projvr_diag2(float& origH, float& origV, float& postH, float& postV,
                       int& skips, float& skipFov, float& skipRatio)
{
    origH = g_pvOrigFov; origV = g_pvOrigVFov;
    postH = g_pvPostFov; postV = g_pvPostVFov;
    skips = g_pvSkips;   skipFov = g_pvSkipFov; skipRatio = g_pvSkipRatio;
}

// =============================== RE02 ==================================
// True square render — defeat the engine's 16:9 aspect LOCK.
//
// Found 2026-07-26 by static analysis (BatmanAK.exe). The black band + skew were
// NOT a viewport-rect problem — the Arkham engine hard-constrains the camera to
// 16:9 in its view-setup, so a square buffer just gets a 16:9 slice letterboxed
// into it. The function at VA 0x140112dd0 is that constraint. At its top:
//
//   F6 42 74 01   test byte [rdx+0x74], 1     ; rdx->bConstrainAspectRatio flag
//   48 8B DA      mov  rbx, rdx
//   48 8B F9      mov  rdi, rcx
//   0F 84 rel32   je   0x140112f7e            ; flag OFF -> "not constrained" branch
//   ...           <constrained path: pick 4:3 vs 16:9, letterbox>   (flag ON, today)
//   0x140112f7e:  <not-constrained path: copy the FULL view rect, no letterbox>
//
// The engine ALREADY has an un-constrained path — it copies the full viewport rect
// with no 16:9 selection. It's just gated behind the flag, which ships ON. We force
// the `je` to an unconditional `jmp`, so the game always takes the un-constrained
// path and renders at the buffer's true aspect. On a square buffer that's a genuine
// square render (fills it, no black band); the projection reads the same rect, so
// the skew goes with it. One 6-byte in-place patch, no code cave.
namespace
{
    // test byte [rdx+0x74],1 ; mov rbx,rdx ; mov rdi,rcx ; je rel32
    const uint8_t kVpPat[]  = { 0xF6,0x42,0x74,0x01, 0x48,0x8B,0xDA, 0x48,0x8B,0xF9, 0x0F,0x84,0,0,0,0 };
    const bool    kVpMask[] = { 1,1,1,1, 1,1,1, 1,1,1, 1,1,0,0,0,0 };
    uintptr_t g_vpAddr = 0;
    uint8_t   g_vpOrig[6]{};
    bool      g_vpOn = false;

    uintptr_t scan_masked(const uint8_t* pat, const bool* mask, size_t len)
    {
        uint8_t* base; size_t size;
        if (!module_range(base, size)) return 0;
        for (size_t i = 0; i + len <= size; ++i)
        {
            bool ok = true;
            for (size_t j = 0; j < len; ++j)
                if (mask[j] && base[i + j] != pat[j]) { ok = false; break; }
            if (ok) return (uintptr_t)(base + i);
        }
        return 0;
    }
}

bool akvr_vp_square_install()
{
    if (g_vpOn) return true;
    uintptr_t s = scan_masked(kVpPat, kVpMask, sizeof(kVpPat));
    if (!s) return false;

    uintptr_t jeAt   = s + 10;                                   // the 0F 84 (je) opcode
    int32_t   jeRel  = *(int32_t*)(jeAt + 2);
    uintptr_t target = (jeAt + 6) + jeRel;                       // the not-constrained branch

    memcpy(g_vpOrig, (void*)jeAt, sizeof(g_vpOrig));             // 6 bytes: 0F 84 rel32
    uint8_t patch[6];
    patch[0] = 0xE9;                                             // jmp rel32 — always un-constrained
    *(int32_t*)(patch + 1) = (int32_t)(target - (jeAt + 5));
    patch[5] = 0x90;                                             // NOP to fill the 6th byte
    write_code((void*)jeAt, patch, sizeof(patch));

    g_vpAddr = s;
    g_vpOn = true;
    return true;
}

bool akvr_vp_square_on() { return g_vpOn; }
uintptr_t akvr_vp_square_addr() { return g_vpAddr; }

// ---- RE02: wide-render / centre-crop ------------------------------------
bool  akvr_head_wide_render() { return g_wideRender; }
void  akvr_head_wide_render_set(bool on) { g_wideRender = on; }

float akvr_head_render_hfov()
{
    float hfov = akvr_xr_headset_hfov_deg();
    if (g_wideRender && hfov > 10.0f)
    {
        float vfov = akvr_xr_headset_vfov_deg();
        if (vfov > 10.0f)
            hfov = 2.0f * atanf(tanf(vfov * 0.5f * DEG2RAD) * (16.0f / 9.0f)) * RAD2DEG;
    }
    return hfov;
}

// HUDSTEADY 2026-09-28 — which head pose built the frame the game just finalized. Every
// delta triple handed to the epilogue stub is kept with the headset orientation it came
// from. The stub adds the triple to the game's fresh base and saves that base, so after a
// finalize (camera rotator - saved base) IS the triple that frame used, exactly (compared
// modulo 65536 in case the game normalises the rotator). Written on the Present thread,
// read on the game thread; the newest match wins (a still head repeats triples).
namespace {
    struct DeltaRec { int32_t y, p, r; float q[4]; };
    DeltaRec      g_dRing[32];
    volatile LONG g_dHead = 0;
}
static void head_ring_push(int32_t y, int32_t p, int32_t r, float qx, float qy, float qz, float qw)
{
    DeltaRec& d = g_dRing[g_dHead & 31];
    d.q[0] = qx; d.q[1] = qy; d.q[2] = qz; d.q[3] = qw;
    d.y = y; d.p = p; d.r = r;
    InterlockedIncrement(&g_dHead);
}
// HUDSTEADY2: the newest orientation handed to the stub (what the NEXT finalize will use if
// no Present lands before it).
bool akvr_head_newest_quat(float q[4])
{
    const LONG h = g_dHead;
    if (h <= 0 || !g_htOn) return false;
    memcpy(q, g_dRing[(h - 1) & 31].q, sizeof(float) * 4);
    return true;
}
bool akvr_head_frame_quat(float q[4], int* age)
{
    const uintptr_t b = cam_base();
    if (!b || !g_bYaw || !g_htOn) return false;
    uint32_t cy = 0, cp = 0, cr = 0;
    __try { cy = *(uint32_t*)(b + OFF_YAW); cp = *(uint32_t*)(b + OFF_PITCH); cr = *(uint32_t*)(b + OFF_ROLL); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    const uint32_t dy = (cy - (uint32_t)*g_bYaw) & 0xFFFF, dp = (cp - (uint32_t)*g_bPitch) & 0xFFFF,
                   dr = (cr - (uint32_t)*g_bRoll) & 0xFFFF;
    if (g_exOn)
    {   // DIVEFIX: the finalize callback wrote the rotation itself (head + the copy fix + the ease)
        ExRec e; LONG s0 = g_exRecSeq; MemoryBarrier(); e = g_exRec; MemoryBarrier();
        if (!(s0 & 1) && s0 == g_exRecSeq && ((uint32_t)e.y & 0xFFFF) == dy && ((uint32_t)e.p & 0xFFFF) == dp && ((uint32_t)e.r & 0xFFFF) == dr)
        { memcpy(q, e.q, sizeof(e.q)); if (age) *age = 0; return true; }
    }
    const LONG h = g_dHead;
    for (int k = 0; k < 32 && k < h; ++k)
    {
        const DeltaRec& d = g_dRing[(h - 1 - k) & 31];
        if (((uint32_t)d.y & 0xFFFF) == dy && ((uint32_t)d.p & 0xFFFF) == dp && ((uint32_t)d.r & 0xFFFF) == dr)
        { memcpy(q, d.q, sizeof(d.q)); if (age) *age = k; return true; }
    }
    return false;
}

bool akvr_head_install()
{
    if (g_htInstalled) return true;
    if (!g_installed && !akvr_camera_install()) return false;
    if (!g_slot || !g_fov.addr) return false;          // need the cave + ModFOV anchor
    g_epiAddr = g_fov.addr + 6;                         // the 5-byte 'mov rbx,[rsp+0x30]'

    uint8_t* cave = (uint8_t*)g_slot;
    g_dYaw   = (int32_t*)(cave + 0x80);
    g_dPitch = (int32_t*)(cave + 0x84);
    g_dRoll  = (int32_t*)(cave + 0x88);
    g_dPosX  = (float*)  (cave + 0x90);
    g_dPosY  = (float*)  (cave + 0x94);
    g_dPosZ  = (float*)  (cave + 0x98);
    g_dFov   = (float*)  (cave + 0x9C);
    g_bFov   = (float*)  (cave + 0xB0);   // FOVABS (0xAC is the enable byte; the stub starts at 0x100)
    g_aFov   = (float*)  (cave + 0xB4);
    g_fovAbs = (int32_t*)(cave + 0xB8);
    g_bYaw   = (int32_t*)(cave + 0xA0);
    g_bPitch = (int32_t*)(cave + 0xA4);
    g_bRoll  = (int32_t*)(cave + 0xA8);
    *g_dYaw = *g_dPitch = *g_dRoll = 0;
    *g_bYaw = *g_bPitch = *g_bRoll = 0;
    *g_dPosX = *g_dPosY = *g_dPosZ = 0.0f;
    *g_dFov = 0.0f;
    *g_bFov = 0.0f; *g_aFov = 0.0f; *g_fovAbs = 0;   // FOVABS
    g_htEnable = (uint8_t*)(cave + 0xAC);   // free slot after g_bRoll(0xA8..0xAC)
    *g_htEnable = 0;                        // start disabled; the byte gates the stub body

    // Stub: preserve rax+flags. ROTATION = compose/overwrite: for each field, save
    // the game's just-written base into the base slot, then overwrite with our
    // precomputed FULL rotator (C++ composes full = base∘head off the saved base).
    // LEAN/FOV = still additive (movss/addss). rbx == cam base at the epilogue.
    //
    // The detour is installed ONCE and left in place; turning VR off no longer
    // rewrites live code (that torn write was the exit-VR crash). Instead the stub
    // reads g_htEnable at the top: when 0 it skips straight to the stolen instruction
    // and returns, so a disabled mod is a cheap pass-through with the game's own
    // camera write standing untouched. Flipping the byte is always safe.
    uint8_t* stub = cave + 0x100, * p = stub;
    *p++ = 0x50;                                                    // push rax
    *p++ = 0x9C;                                                    // pushfq
    // if (g_htEnable == 0) goto passthrough;
    *p++ = 0xA0; *(uint64_t*)p = (uint64_t)g_htEnable; p += 8;      // mov al,[g_htEnable]
    *p++ = 0x84; *p++ = 0xC0;                                       // test al,al
    *p++ = 0x0F; *p++ = 0x84;                                       // jz rel32 -> passthrough
    uint8_t* jzDisp = p; *(uint32_t*)p = 0; p += 4;                 //   (displacement patched below)
    auto emit_rot = [&](uint32_t off, int32_t* base, int32_t* delta) {
        *p++ = 0x8B; *p++ = 0x83; *(uint32_t*)p = off; p += 4;          // mov eax,[rbx+off]  (base)
        *p++ = 0xA3; *(uint64_t*)p = (uint64_t)base; p += 8;            // mov [baseSlot],eax
        *p++ = 0xA1; *(uint64_t*)p = (uint64_t)delta; p += 8;           // mov eax,[deltaSlot]
        *p++ = 0x01; *p++ = 0x83; *(uint32_t*)p = off; p += 4;          // add [rbx+off],eax  (ORBITFIX: fresh base + head)
    };
    emit_rot(OFF_YAW,   g_bYaw,   g_dYaw);
    emit_rot(OFF_PITCH, g_bPitch, g_dPitch);
    emit_rot(OFF_ROLL,  g_bRoll,  g_dRoll);
    // CAMSMOOTH: smooth_cb(rbx) - the game's fresh position, before our deltas. Volatile registers saved, stack aligned.
    *p++ = 0x55;                                                    // push rbp
    *p++ = 0x48; *p++ = 0x89; *p++ = 0xE5;                          // mov rbp,rsp
    *p++ = 0x51; *p++ = 0x52;                                       // push rcx; push rdx
    *p++ = 0x41; *p++ = 0x50; *p++ = 0x41; *p++ = 0x51;             // push r8; push r9
    *p++ = 0x41; *p++ = 0x52; *p++ = 0x41; *p++ = 0x53;             // push r10; push r11
    *p++ = 0x48; *p++ = 0x83; *p++ = 0xE4; *p++ = 0xF0;             // and rsp,-16
    *p++ = 0x48; *p++ = 0x81; *p++ = 0xEC; *(uint32_t*)p = 0x80; p += 4;   // sub rsp,0x80
    {
        static const uint8_t xr[6] = { 0x44, 0x4C, 0x54, 0x5C, 0x64, 0x6C };   // [rsp+disp8] with xmm0..5
        for (int i = 0; i < 6; ++i) { *p++ = 0x0F; *p++ = 0x11; *p++ = xr[i]; *p++ = 0x24; *p++ = (uint8_t)(0x20 + 0x10 * i); }   // movups [rsp+..],xmmi
        *p++ = 0x48; *p++ = 0x89; *p++ = 0xD9;                      // mov rcx,rbx
        *p++ = 0x48; *p++ = 0xB8; *(uint64_t*)p = (uint64_t)&smooth_cb; p += 8;   // mov rax,smooth_cb
        *p++ = 0xFF; *p++ = 0xD0;                                   // call rax
        for (int i = 0; i < 6; ++i) { *p++ = 0x0F; *p++ = 0x10; *p++ = xr[i]; *p++ = 0x24; *p++ = (uint8_t)(0x20 + 0x10 * i); }   // movups xmmi,[rsp+..]
    }
    *p++ = 0x48; *p++ = 0x8D; *p++ = 0x65; *p++ = 0xD0;             // lea rsp,[rbp-0x30]
    *p++ = 0x41; *p++ = 0x5B; *p++ = 0x41; *p++ = 0x5A;             // pop r11; pop r10
    *p++ = 0x41; *p++ = 0x59; *p++ = 0x41; *p++ = 0x58;             // pop r9; pop r8
    *p++ = 0x5A; *p++ = 0x59;                                       // pop rdx; pop rcx
    *p++ = 0x5D;                                                    // pop rbp

    // --- positional lean (float) ---
    auto emit_pos = [&](float* slot, uint32_t off) {
        *p++ = 0x48; *p++ = 0xB8; *(uint64_t*)p = (uint64_t)slot; p += 8;      // mov rax, slot
        *p++ = 0xF3; *p++ = 0x0F; *p++ = 0x10; *p++ = 0x00;                    // movss xmm0,[rax]
        *p++ = 0xF3; *p++ = 0x0F; *p++ = 0x58; *p++ = 0x83; *(uint32_t*)p = off; p += 4; // addss xmm0,[rbx+off]
        *p++ = 0xF3; *p++ = 0x0F; *p++ = 0x11; *p++ = 0x83; *(uint32_t*)p = off; p += 4; // movss [rbx+off],xmm0
    };
    *p++ = 0x48; *p++ = 0x83; *p++ = 0xEC; *p++ = 0x10;            // sub rsp,0x10
    *p++ = 0x0F; *p++ = 0x11; *p++ = 0x04; *p++ = 0x24;            // movups [rsp],xmm0
    emit_pos(g_dPosX, OFF_X);
    emit_pos(g_dPosY, OFF_Y);
    emit_pos(g_dPosZ, OFF_Z);
    emit_pos(g_dFov,  OFF_FOV);                                    // FOV += our offset (0 while FOVABS writes it)
    // FOVABS: save the game's own FOV, then (when g_fovAbs) write the headset's
    *p++ = 0x8B; *p++ = 0x83; *(uint32_t*)p = OFF_FOV; p += 4;          // mov eax,[rbx+OFF_FOV]
    *p++ = 0xA3; *(uint64_t*)p = (uint64_t)g_bFov; p += 8;            // mov [g_bFov],eax
    *p++ = 0xA1; *(uint64_t*)p = (uint64_t)g_fovAbs; p += 8;          // mov eax,[g_fovAbs]
    *p++ = 0x85; *p++ = 0xC0;                                         // test eax,eax
    *p++ = 0x74; *p++ = 15;                                           // jz +15 (skip the next two)
    *p++ = 0xA1; *(uint64_t*)p = (uint64_t)g_aFov; p += 8;            // mov eax,[g_aFov]   (9 bytes)
    *p++ = 0x89; *p++ = 0x83; *(uint32_t*)p = OFF_FOV; p += 4;        // mov [rbx+OFF_FOV],eax (6 bytes)
    *p++ = 0x0F; *p++ = 0x10; *p++ = 0x04; *p++ = 0x24;            // movups xmm0,[rsp]
    *p++ = 0x48; *p++ = 0x83; *p++ = 0xC4; *p++ = 0x10;            // add rsp,0x10

    // passthrough: (jz above lands here when the mod is disabled)
    *(uint32_t*)jzDisp = (uint32_t)(p - (jzDisp + 4));             // rel32 from end-of-jz to here
    *p++ = 0x9D;                                                    // popfq
    *p++ = 0x58;                                                    // pop rax
    memcpy(p, (void*)g_epiAddr, 5); p += 5;                         // relocated 'mov rbx,[rsp+0x30]'
    *p++ = 0xE9; *(int32_t*)p = (int32_t)((g_epiAddr + 5) - ((uintptr_t)p + 4)); p += 4;  // jmp back
    FlushInstructionCache(GetCurrentProcess(), stub, (size_t)(p - stub));

    // The 5-byte detour we'll swap in at the epilogue when head-tracking is ON.
    memcpy(g_epiOrig, (void*)g_epiAddr, 5);
    g_epiJmp[0] = 0xE9;
    *(int32_t*)(g_epiJmp + 1) = (int32_t)((uintptr_t)stub - (g_epiAddr + 5));

    g_htInstalled = true;
    // 2026-07-25: convergence patch DISABLED. akvr_proj_install() writes a 12-byte
    // patch into BuildProjectionMatrix (live game code) — the strongest remaining
    // crash suspect and, per JJ, VR worked before this "convergence thing" existed.
    // Left out entirely; the convergence slider is now a no-op (proj_refresh_jitter
    // early-returns while g_projCave is null). Re-enable only with a safer install.
    // akvr_proj_install();

    // projVR: install the projection-rewrite hook now (dormant — g_projVR defaults
    // OFF, so nothing is rewritten until the "square view (projection)" toggle). This
    // is a clean MinHook whole-function detour, NOT the crashy inline convergence patch.
    akvr_projvr_install();
    return true;
}

namespace {
    // Turn head-tracking on/off. The detour into the stub is written into live game
    // code exactly ONCE (the first time we enable) and then left there forever —
    // rewriting it back out on every VR-exit was the torn-write that popped the
    // "Fatal error!" box. After that first install, on/off is just a data byte the
    // stub reads at its top, which can never tear.
    void head_apply(bool on)
    {
        if (!g_htInstalled) return;
        if (on && !g_epiPatched) { write_code((void*)g_epiAddr, g_epiJmp, 5); g_epiPatched = true; }
        if (g_htEnable) *g_htEnable = on ? 1 : 0;
    }
}

void akvr_head_recenter()
{
    float y, p, r, px, py, pz; akvr_xr_head_pose(y, p, r, px, py, pz);
    akvr_xr_head_quat(g_refQx, g_refQy, g_refQz, g_refQw);
    // Keep the heading only — see g_refYawDeg.
    {
        Quat rq{ g_refQx, g_refQy, g_refQz, g_refQw };
        float ry, rp, rr; quat_to_euler_ypr(rq, ry, rp, rr);
        g_refYawDeg = ry;
    }
    g_refPX = px; g_refPY = py; g_refPZ = pz;
    akvr_xr_hud_reanchor();   // HUDWORLD: the room-fixed HUD hangs in front of this new straight ahead
}

void akvr_head_toggle()
{
    if (!g_htOn)
    {
        if (akvr_freecam_on()) akvr_freecam_toggle();   // mutually exclusive with freecam
        if (!akvr_head_install()) return;
        akvr_head_recenter();
        // Seed FULL + base rotator from the live camera so the first overwrite is a
        // no-op (head delta is 0 at recenter) — otherwise the stub would snap the
        // camera to a zero orientation on frame 1.
        CameraView v = akvr_camera_read();
        if (v.valid)
        {
            *g_bYaw = v.yaw;
            *g_bPitch = v.pitch;
            *g_bRoll = v.roll;
        }
        *g_dYaw = *g_dPitch = *g_dRoll = 0;   // ORBITFIX: delta slots, zero = game's own angle
        *g_dPosX = *g_dPosY = *g_dPosZ = 0.0f;
        *g_dFov = 0.0f; if (g_fovAbs) *g_fovAbs = 0;   // FOVABS
        head_apply(true);
        g_htOn = true;
    }
    else
    {
        g_exOn = false;   // DIVEFIX
        head_apply(false);
        // Do NOT unpatch the projection hook here. Removing live code was the crash
        // (see write_code) and it isn't needed: with head-tracking off the cave's
        // offset slot is set to 0 below, which makes the patch a no-op — the game
        // gets exactly the zero jitter it would have written itself. The patch only
        // comes out at process shutdown.
        if (g_dYaw) { *g_dYaw = *g_dPitch = *g_dRoll = 0; *g_dPosX = *g_dPosY = *g_dPosZ = 0.0f; *g_dFov = 0.0f; if (g_fovAbs) *g_fovAbs = 0; }
        g_htOn = false;
        proj_refresh_jitter();   // g_htOn now false -> writes 0 into the cave slot
    }
}

namespace {
    // PAUSELOOK 2026-09-30 — JJ: "the pause screen in Sekiro now stays in full 360 view and places the pause menu
    // window on top. Can we do that with this one instead of cutting to a scaled down window floating in black
    // space?" AK's pause stops the camera finalize (so the stub never adds the head), but the renderer keeps
    // building the main view every frame (mode trace: proj_hits 2 per frame through the whole pause) from the
    // camera's view fields - the fields freecam writes. While xr.cpp reports a live pause, write them here each
    // Present exactly as the stub would: the game's last own rotator (saved by the stub) + the head delta, and the
    // position the last finalize left minus the lean it carried, plus the lean now. SKVR's PAUSELOOK (run 91) is
    // the same idea, confirmed there. Only after 2 Presents without a finalize, so it never races the game.
    bool     g_pauseWriting = false;
    float    g_pauseBase[3] = {};
    uint64_t g_pauseFc = 0;
    int      g_pauseStill = 0;
    long     g_pauseWrites = 0;
    void pause_look_write()
    {
        const uint64_t fc = akvr_camera_finalize_count();
        g_pauseStill = fc == g_pauseFc ? g_pauseStill + 1 : 0;
        g_pauseFc = fc;
        const uintptr_t b = cam_base();
        // PAUSEEDGE: from the first Present without a finalize (it was 2), whenever a pause could be starting
        if (!(akvr_xr_pause_live() || akvr_xr_pause_candidate()) || g_pauseStill < 1 || !b) { g_pauseWriting = false; return; }
        __try
        {
            if (!g_pauseWriting)
            {
                const bool own = g_restoredFc == fc;   // CAMRESTORE: already the game's own position
                g_pauseBase[0] = *(float*)(b + OFF_X) - (own ? 0.0f : *g_dPosX);
                g_pauseBase[1] = *(float*)(b + OFF_Y) - (own ? 0.0f : *g_dPosY);
                g_pauseBase[2] = *(float*)(b + OFF_Z) - (own ? 0.0f : *g_dPosZ);
                g_pauseWriting = true;
            }
            *(int32_t*)(b + OFF_YAW)   = (int32_t)((uint32_t)*g_bYaw   + (uint32_t)*g_dYaw);
            *(int32_t*)(b + OFF_PITCH) = (int32_t)((uint32_t)*g_bPitch + (uint32_t)*g_dPitch);
            *(int32_t*)(b + OFF_ROLL)  = (int32_t)((uint32_t)*g_bRoll  + (uint32_t)*g_dRoll);
            *(float*)(b + OFF_X) = g_pauseBase[0] + *g_dPosX;
            *(float*)(b + OFF_Y) = g_pauseBase[1] + *g_dPosY;
            *(float*)(b + OFF_Z) = g_pauseBase[2] + *g_dPosZ;
            // PAUSEFOV: the paused world at the headset FOV, as the stub would draw it (CAMRESTORE left the game's own)
            if (g_fovAbs && *g_fovAbs && g_aFov && *g_aFov > 10.0f) *(float*)(b + OFF_FOV) = *g_aFov;
            ++g_pauseWrites;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_pauseWriting = false; }
    }
}
long akvr_camera_pause_writes() { return g_pauseWrites; }

void akvr_head_update()
{
    if (!g_htOn || !g_dYaw) { g_pauseWriting = false; g_exOn = false; return; }
    if (!akvr_xr_session_running())
    {   // pass base through unchanged (FULL rotator = base, so the overwrite is a
        // no-op) and drop the additive lean/fov — never write 0 rotation or the
        // stub would snap the camera to a zero orientation.
        *g_dYaw = *g_dPitch = *g_dRoll = 0;   // ORBITFIX: zero delta = the game's own angle
        *g_dPosX = *g_dPosY = *g_dPosZ = 0.0f; *g_dFov = 0.0f; if (g_fovAbs) *g_fovAbs = 0; g_pauseWriting = false; g_exOn = false; return; }

    // CAMRESTORE: what the stub added at the finalize that just ran (the slots are rewritten below)
    const uint64_t rsFc = akvr_camera_finalize_count();
    const float rsPos[3] = { *g_dPosX + g_smoothApplied[0], *g_dPosY + g_smoothApplied[1], *g_dPosZ + g_smoothApplied[2] };   // + CAMSMOOTH
    const bool rsFovAbs = g_fovAbs && *g_fovAbs != 0;
    const float rsCopy[2] = { g_copyApplied[0], g_copyApplied[1] };   // AIMHEAD
    const float rsTilt = g_tiltApplied;                               // SWINGEASE
    float y, p, r, px, py, pz; akvr_xr_head_pose(y, p, r, px, py, pz);
    float qx, qy, qz, qw; akvr_xr_head_quat(qx, qy, qz, qw);

    // --- rotation: proper composition (base ∘ head), NOT euler add ---------------
    // Adding head euler to the game's rotator fields leaks yaw into roll whenever the
    // base is pitched (confirmed in-game). Instead: rebuild the game's pure base
    // orientation (saved by the stub last finalize), turn it by the head — yaw about
    // world up, then pitch and roll about the camera's own axes — decompose ONCE,
    // and write the FULL rotator.
    float byaw   = (float)(*g_bYaw)   * ROT2DEG * DEG2RAD;
    float bpitch = (float)(*g_bPitch) * ROT2DEG * DEG2RAD;
    float broll  = (float)(*g_bRoll)  * ROT2DEG * DEG2RAD;
    V3 fwd, right, up; build_basis(byaw, bpitch, broll, fwd, right, up);
    float b2yaw, b2pitch, b2roll; decompose_basis(fwd, right, b2yaw, b2pitch, b2roll);   // base reference
    // DIVEFIX2 2026-10-05 — JJ with DIVEFIX: "the camera still seems to flip out when doing a high dive". F2 23:07:
    // the game's OWN pitch goes past straight down in a dive (-90.5 .. -93.1; the -71.4 "limit" was only the traces
    // we had), and each crossing is a 175-180 deg one-frame step in the shown view (t 226.27, 272.77, 295.97, 298.06).
    // Past -90 decompose_basis returns the base as (yaw + 180, -89, roll 180); the rigid composition builds the base
    // frame from yaw/pitch only, so that roll 180 was dropped and the world turned upside down. Keep the base in the
    // game's own form (the nearest one), which build_basis handles past the pole.
    euler_nearest(b2yaw, b2pitch, b2roll, byaw, bpitch, broll);

    // Head offset: strip ONLY the recenter heading (delta = Ry(-refYaw) * cur), which
    // leaves pitch/roll gravity-absolute — see g_refYawDeg. Note neither plain
    // quaternion order works here: conj(ref)*cur leaks a recentre pitch into yaw, and
    // cur*conj(ref) leaks a recentre yaw into roll. Removing just the heading is the
    // only form that keeps all three axes clean.
    Quat cur{ qx, qy, qz, qw };
    float refHalf = -g_refYawDeg * DEG2RAD * 0.5f;
    Quat refYawInv{ 0.0f, sinf(refHalf), 0.0f, cosf(refHalf) };   // OpenXR +Y is up
    Quat delta = quat_mul(refYawInv, cur);
    // Euler yaw/pitch/roll of the head delta. Roll here is the Tait-Bryan roll (the LAST
    // rotation) — it is exactly zero for a pure yaw+pitch, so it does NOT introduce a false
    // roll when you look up/down and turn (the swing-twist version did). The stereo-on-roll
    // fix lives on the eye-offset side (built from -froll below), not here.
    float hy, hp, hr; quat_to_euler_ypr(delta, hy, hp, hr);
    g_htYaw = YAW_SIGN * hy; g_htPitch = PITCH_SIGN * hp; g_htRoll = ROLL_SIGN * hr;   // overlay/diag

    // SCREEN MODE NOW ACTUALLY FREEZES THE CAMERA — 2026-08-05.
    //
    // F2 captures of the save-game screen settled a very long hunt: the 2D menu panel is
    // pixel-identical between a head-left and a head-right shot, while Batman swings
    // across the frame and rotates. A moving 3D scene behind a pinned overlay is what JJ
    // was reading as "roll", and no correction to the camera maths could ever fix it —
    // the maths was right (verified 1:1 on all three axes, and the game keeps what we
    // write). The camera simply should not be head-driven on a menu.
    //
    // F6 only ever changed the DISPLAY (head-pinned vs world-fixed layer, and the screen
    // FOV scale). It never stopped head rotation reaching the camera, which is why JJ
    // reported it "just removes the stereo" and did nothing for the roll. That was a
    // wrong assumption of mine about our own switch, tested twice.
    //
    // So: in screen mode the head contributes NOTHING to the camera — no rotation, no
    // lean. The game's own framing is left exactly as it drew it and the picture hangs
    // still in the world for you to look around. The FOV lock below is deliberately left
    // alone, because that is what F11 disturbs and why F11 "zooms in" instead of being a
    // clean test.
    const bool screenFrozen = akvr_xr_screen_frozen();   // SCREENTRACK: F6-style screen may keep the head
    if (screenFrozen) g_htYaw = g_htPitch = g_htRoll = 0.0f;
    float gy = g_htYaw * DEG2RAD, gp = g_htPitch * DEG2RAD, gr = g_htRoll * DEG2RAD;
    // History kept because it explains why the terms are what they are: head yaw must
    // turn about the real-world vertical (your neck does), and head pitch about the
    // world-HORIZONTAL right rather than a rolled camera's own right — pitching about a
    // tilted right leaked roll into the view on menu cameras ("looking up/down also
    // rotates the head, only on the menu", 2026-07-26). Both properties survive below;
    // what changed is WHAT those rotations are applied to.
    //
    // REBUILT 2026-08-05 — the menu spin, fixed properly rather than traded away.
    //
    // The old construction rotated the ALREADY-PITCHED forward about world up. That is
    // fine while the camera is near level, but a menu camera pitched 60 deg down gets
    // swept around a CONE, and a cone sweep is image rotation — so turning your head
    // spun the picture. Measured: with the head turned but not rolled, the reported roll
    // stayed ~0 (the composition was innocent), yet the view visibly rotated, because the
    // rotation was in the forward vector's path, not in the roll term.
    //
    // JJ pushed back on "world up vs camera up, pick your poison" and he was right —
    // that was a false choice. The camera's PITCH is FRAMING, not the player's body
    // orientation: your body stays upright while the camera looks down at Batman. So the
    // head must be composed in a LEVEL frame built from the camera's HEADING alone, and
    // the camera's framing pitch applied afterwards.
    //
    // Head pitch and camera pitch are both rotations about the same horizontal right
    // axis once the heading is settled, so they COMMUTE and simply add. The whole thing
    // collapses to three scalars — no vector gymnastics, no cone, no axis to choose:
    //
    //     heading = base heading + head yaw     (about world up, always level)
    //     pitch   = base pitch   + head pitch   (about the horizontal right)
    //     roll    = head roll only              (base tilt still dropped, as before)
    //
    // Checks: head at rest reproduces the game's framing exactly; a head turn under a
    // 60 deg-down camera now PANS across the scene at constant tilt with the horizon
    // level and no spin; and with a near-level camera it is arithmetically what the old
    // path produced, so gameplay is unchanged.
    // THE YAW AXIS — the real regression, identified 2026-08-05 from JJ's description
    // "it's Batman looking down at him, and everything around him is black".
    //
    // On 2026-07-25 head yaw was moved from the camera's OWN up axis to the WORLD
    // vertical, to stop the horizon tipping during gameplay. That was right for
    // gameplay and wrong for these menu cameras, and JJ remembered them working before.
    //
    // The two axes do completely different things to a camera pointing 60 deg down:
    //   * about the WORLD vertical -> the subject ROTATES in frame (a cone sweep)
    //   * about the CAMERA'S own up -> the subject SLIDES sideways, no rotation
    // With Batman on black there is no horizon to level, so all you can see is him
    // spinning. It is not a trade-off between two flawed options (my earlier framing,
    // rightly rejected): the two camera KINDS genuinely want different axes. A gameplay
    // camera represents your body in the world, so the world vertical is correct. A
    // menu camera is a viewport onto a diorama, where the tilt is framing and its own
    // axis is correct.
    //
    // So pick by how far the camera is from level, and blend so nothing pops:
    //   |pitch| <= 40 deg  -> world vertical  (gameplay, bit-identical to before)
    //   |pitch| >= 60 deg  -> the camera's own up (menus)
    // At t = 0 this reduces EXACTLY to `heading = b2yaw + gy`, because rotating the
    // base forward about the world vertical preserves its elevation and simply adds gy
    // to the heading — so gameplay is provably unchanged.
    float headingRad, elevRad;
    {
        // Decoupled pitch: the game's tilt kept in gameplay (see g_basePitchKeep).
        // The rotator write below still measures against the real b2pitch.
        // The live main menu follows the same tilt setting as gameplay (JJ: with the
        // game's full pitch kept, head turns rolled Batman - the same geometry as the
        // original gameplay complaint). Only a frozen screen keeps its framing as-is.
        const bool unlink = g_pitchUnlink != 0 && !screenFrozen && !akvr_xr_main_menu_detected();
        const float basePitch = b2pitch * (screenFrozen || unlink ? 1.0f : g_basePitchKeep);
        V3 bf, brv, bu;
        build_basis(b2yaw, basePitch, 0.0f, bf, brv, bu);
        const float pitchDeg = fabsf(basePitch * RAD2DEG);
        float t = (pitchDeg - 40.0f) / 20.0f;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        if (unlink) t = 0.0f;                 // PITCHUNLINK: world vertical, never the tilted up
        V3 axis = { (1.0f - t) * 0.0f + t * bu.x,
                    (1.0f - t) * 0.0f + t * bu.y,
                    (1.0f - t) * 1.0f + t * bu.z };
        if (v_dot(axis, axis) < 1e-6f) axis = V3{ 0.0f, 0.0f, 1.0f };
        axis = v_norm(axis);
        V3 f = v_rot(bf, axis, gy);
        f = v_norm(f);
        headingRad = atan2f(f.y, f.x);
        float z = f.z; z = z < -1.0f ? -1.0f : (z > 1.0f ? 1.0f : z);
        elevRad = asinf(z);
    }
    // MINUS gp is deliberate — the two halves of the codebase disagree on which way
    // pitch counts, and this is where they meet. `g_htPitch` carries PITCH_SIGN = -1,
    // chosen back on 2026-07-25 for the OLD construction: that one pitched by rotating
    // the forward about `cross(WORLD_UP, fwd)`, and a POSITIVE rotation about that axis
    // tips the view DOWN, so the sign had to be inverted to make "look up" look up.
    // `build_basis`/`decompose_basis` use the opposite and more natural convention —
    // `fwd.z = sin(pitch)`, so positive pitch is UP. Adding gp therefore inverted the
    // head's pitch (JJ, immediately: "head pitch is now reversed"). Subtracting it
    // undoes PITCH_SIGN and lands back in build_basis's convention.
    // Yaw and roll need no such flip: rotating about +Z, and rolling `right0` about
    // `fwd`, both already match build_basis exactly — which is why ONLY pitch was wrong.
    float pitchRad   = elevRad - gp;   // elevRad == b2pitch when the yaw axis is world up
    // Beyond vertical the basis would flip. Clamp instead: a menu camera at -60 plus a
    // 40 deg head-down would otherwise turn the world upside down.
    // PITCHUNLINK pole guard: with the game's pitch plus the head's, a yaw about the world
    // vertical near straight down spins the picture (the menu "cone"); stop at 80.
    const float kPitchLim = (g_pitchUnlink == 1 && !screenFrozen ? 80.0f : 89.0f) * DEG2RAD;
    if (pitchRad >  kPitchLim) pitchRad =  kPitchLim;
    if (pitchRad < -kPitchLim) pitchRad = -kPitchLim;
    build_basis(headingRad, pitchRad, gr, fwd, right, up);
    // TIPPED (mode 2): the head's full rotation, built in a level frame at heading 0
    // (x = forward, y = right, z = up; exactly what build_basis(0,0,0) gives), carried
    // into the game camera's own frame (heading + pitch, no roll). At zero game pitch
    // this is bit-for-bit the construction above, so level gameplay is unchanged.
    // ROLLSIGN 2026-09-27 — why mode 2 still rolled for JJ while UEVR's default (the same
    // rigid composition) does not: build_basis/decompose_basis roll the OPPOSITE way to UE3's
    // FRotationMatrix (checked numerically: build_basis(y,p,r) == UE(y,p,-r)). Head-only roll
    // hid it (ROLL_SIGN and the written froll cancel), but here roll is CREATED by composing
    // yaw with the game's pitch, and it was written mirrored: F2 23:59 at pitch -41, head
    // yaw 12.6 wrote +15.5 deg where UE needed -8.6. Fix: build the head in UE's sense (-gr
    // in our basis), compose, and hand UE the roll back in its sense (-froll). At zero game
    // pitch this is still exactly the level path (froll = gr).
    // MENUTIPPED 2026-09-30 — JJ: on the title screen, the shot looking down on Batman from above sat "right down
    // low, you have to tilt your head down to see it". The live main menu was kept LEVEL (pitchkeep 0) since the
    // full-pitch menu rolled Batman on head turns (GEOSHAPE, 2026-09-26) - before TIPPED and ROLLSIGN made the rigid
    // composition roll-free. With mode 2 the menu now uses it too: the menu camera's framing stays straight ahead,
    // head turns slide the picture. (Modes 0/1 keep the old level menu.)
    const bool tipped = g_pitchUnlink == 2 && !screenFrozen;
    if (tipped)
    {
        V3 bf, br, bu; build_basis(b2yaw, b2pitch, 0.0f, bf, br, bu);
        V3 hf, hr, hu; build_basis(gy, -gp, -gr, hf, hr, hu);  // -gp: PITCH_SIGN note; -gr: ROLLSIGN
        auto toCam = [&](V3 v) { return v_add(v_add(v_scale(bf, v.x), v_scale(br, v.y)), v_scale(bu, v.z)); };
        fwd = v_norm(toCam(hf)); right = v_norm(toCam(hr)); up = v_norm(toCam(hu));
    }

    float fyaw, fpitch, froll; decompose_basis(fwd, right, fyaw, fpitch, froll);
    if (tipped) froll = -froll;                 // ROLLSIGN: back into UE3's roll direction
    euler_nearest(fyaw, fpitch, froll, b2yaw, b2pitch, b2roll);   // DIVEFIX: no 180 flip in the deltas past straight down
    // DIVEFIX: the head for the stub's callback, which composes it again onto the camera as shown (rigid mode only)
    InterlockedIncrement(&g_exSeq);
    g_exHead[0] = gy; g_exHead[1] = gp; g_exHead[2] = gr;
    g_exQ[0] = qx; g_exQ[1] = qy; g_exQ[2] = qz; g_exQ[3] = qw;
    InterlockedIncrement(&g_exSeq);
    g_exOn = tipped && !g_testNoRot;
    // The eye separation must ride the GAME'S ACTUAL rendered right — which comes from the
    // rotator we're about to write (fyaw/fpitch/froll), NOT the pre-decompose `right`.
    // decompose_basis isn't a perfect inverse for roll, so `right` and the game's rendered
    // right diverge at a tilt; offsetting along `right` then adds a vertical component and
    // the eyes split. Rebuild the right from the exact written rotator (freecam-verified
    // basis) so the separation is purely sideways at any roll.
    // Eye-separation axis must match the DISPLAY's roll, which the diagnostic shows is
    // the OPPOSITE sign of the game's rendered roll (head/game +37 vs display -37). Build
    // the eye-offset right with the roll NEGATED so the parallax lines up with where the
    // compositor actually shows the frame → fuses at a tilt. (froll=0 upright → no change.)
    V3 gFwd, gRight, gUp; build_basis(fyaw, fpitch, -froll, gFwd, gRight, gUp);
    const float RAD2ROT = 65536.0f / (2.0f * 3.14159265f);
    auto wrapPi = [](float a){ while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; };
    // Head contribution only; the stub adds it to the game's fresh base (ORBITFIX). Exact
    // for any change in the game's heading between frames (the composition is heading-
    // invariant); a game pitch change is carried to first order, which is sub-degree.
    *g_dYaw   = (int32_t)(wrapPi(fyaw   - b2yaw)   * RAD2ROT);
    *g_dPitch = (int32_t)(wrapPi(fpitch - b2pitch) * RAD2ROT);
    *g_dRoll  = (int32_t)(wrapPi(froll  - b2roll)  * RAD2ROT);
    head_ring_push(*g_dYaw, *g_dPitch, *g_dRoll, qx, qy, qz, qw);   // HUDSTEADY

    // --- positional lean (world units) ---
    // OpenXR LOCAL delta, meters: +X room-right, +Y up, -Z forward.
    float lx = px - g_refPX, ly = py - g_refPY, lz = pz - g_refPZ;
    g_htLX = lx; g_htLY = ly; g_htLZ = lz;
    float rightAmt = lx, upAmt = ly, fwdAmt = -lz;
    // ...and screen mode freezes the lean too, so the menu cannot drift as you move.
    if (screenFrozen) { rightAmt = upAmt = fwdAmt = 0.0f; }

    // Rotate room lean into the camera's facing so lean-forward goes where the
    // camera looks. Basis as freecam verified: forward=(cos,sin), right=(-sin,cos), +Z up.
    //
    // Lean maps through the BASE (controller) yaw, NOT the finalized one. The old code
    // read the live 0x584 field, which already contains the head yaw we injected last
    // frame — so head rotation fed back into position and turning your head slid the
    // camera sideways (reported in-game 2026-07-25 as "positional translation during
    // head yaw"). Your room doesn't rotate when you turn your neck; the boom's heading
    // is what the room hangs off.
    float baseYawRad = (float)(*g_bYaw) * ROT2DEG * DEG2RAD;
    float cy = cosf(baseYawRad), sy = sinf(baseYawRad);
    // World-unit displacement = head lean (meters * posScale) + AER stereo eye offset.
    // The eye offset shifts the camera left/right along its right vector each frame,
    // so the left/right renders differ → the two eyes fuse depth.
    float rightW = rightAmt * g_posScale;
    float fwdW   = fwdAmt   * g_posScale;
    float ox = cy * fwdW - sy * rightW;   // forward.x + right.x
    float oy = sy * fwdW + cy * rightW;   // forward.y + right.y

    // The AER eye offset is the ONE thing that does belong on the FINAL view's right
    // vector — that's the direction your eyes are actually separated along, so it has
    // to follow the head. (Separate basis from the lean above, deliberately.)
    float eyeUU  = (g_curEye < 0) ? 0.0f                               // native stereo: geo-11 owns the separation, camera stays centered
                 : (g_curEye == 1 ? +1.0f : -1.0f) * g_stereoHalfUU;   // AER: +right eye, -left eye
    // ...and NO separation in screen mode. With the camera frozen, the only thing left
    // moving between the two eye frames was this lateral shift — and screen mode submits
    // BOTH eyes at the same fixed pose, so the compositor is handed two images taken
    // from different places and told they share a viewpoint. They cannot fuse, which is
    // the double vision JJ reported the moment the camera stopped moving. A flat 2D
    // menu wants both eyes identical: same camera, same image, no depth to reconstruct.
    if (screenFrozen) eyeUU = 0.0f;
    // Separate the eyes along the HEAD'S TRUE right vector (already rolled/pitched into
    // `right`), NOT a yaw-only horizontal axis. Otherwise rolling your head keeps the eye
    // baseline world-flat while your eyes are tilted → vertical disparity → stereo breaks.
    // (When roll=0 this is identical to the old -sin/cos, so gameplay upright is unchanged.)
    ox += gRight.x * eyeUU;
    oy += gRight.y * eyeUU;

    // --- Shoulder cancel -----------------------------------------------------
    // Arkham frames its third-person camera OFF to one side of the subject (classic
    // over-the-shoulder). On a monitor that reads as style; in VR it reads as your
    // body standing to one side of where you're looking — JJ 2026-08-04: "my point of
    // view is not in the centre underneath the Batmobile, I am to the left of it".
    // Cancel it with a constant slide along the camera's own right vector. Not the
    // AER eye offset (that must stay symmetric or stereo breaks) and not the lean
    // (that's your head) — a third, separate term, tunable because the offset differs
    // per camera mode. Positive = move the viewpoint RIGHT.
    // Two more axes on the same idea, added 2026-08-05 on JJ's request: the shoulder
    // slide was only ever the RIGHT axis of a general camera offset. Up and forward
    // ride the same finalized basis, so all three stay glued to the camera however it
    // is pitched or rolled, and all three are independent of the AER eye separation
    // (which must stay symmetric) and of the head lean (which is you moving).
    //
    // GATED 2026-08-05 on the panel's "camera fix" tick box, on JJ's instruction, so
    // ONE switch reverts every camera change made that day. At zero offsets the two
    // branches are arithmetically identical, so this should make no difference — which
    // is exactly why it is worth being able to prove rather than argue. The OFF branch
    // is the pre-2026-08-05 expression, character for character.
    const float stickLiftUU = (g_pitchUnlink == 0 && g_basePitchKeep < 0.001f && !screenFrozen)
                            ? -sinf(b2pitch) * g_stickHeightM * g_posScale : 0.0f;   // STICKHEIGHT
    const float menuSideUU = akvr_xr_main_menu_detected() ? g_menuSideUU : 0.0f;   // MENUSIDE
    ox += -sy * menuSideUU;   // MENUSIDE2: the base camera's level right (-sin, cos), not the head's
    oy +=  cy * menuSideUU;
    if (g_camFixOn)
    {
        ox += gRight.x * g_shoulderUU + gUp.x * g_offUpUU + gFwd.x * g_offFwdUU;
        oy += gRight.y * g_shoulderUU + gUp.y * g_offUpUU + gFwd.y * g_offFwdUU;
        *g_dPosX = ox;
        *g_dPosY = oy;
        *g_dPosZ = upAmt * g_posScale + gRight.z * (eyeUU + g_shoulderUU)
                 + gUp.z * g_offUpUU + gFwd.z * g_offFwdUU + stickLiftUU;
    }
    else
    {
        ox += gRight.x * g_shoulderUU;
        oy += gRight.y * g_shoulderUU;
        *g_dPosX = ox;
        *g_dPosY = oy;
        *g_dPosZ = upAmt * g_posScale + gRight.z * (eyeUU + g_shoulderUU) + stickLiftUU;
    }

    // --- Trace ---------------------------------------------------------------
    // JJ 2026-08-05: "when spinning and letting go of the right stick, the camera
    // stops slightly FORWARD of where it should — can we log the camera positions of
    // each eye as it spins and where it lands?" Everything that question needs is in
    // scope right here, so record it now rather than reconstruct it later.
    trace_record(eyeUU, ox, oy, *g_dPosZ, fyaw, fpitch, froll);

    // --- FOV: lock the game's render FOV to the Quest 3's real horizontal FOV ---
    // No more "zoom the game": the headset value drives the render FOV, and the
    // headset layer uses that same FOV, so head motion stays 1:1. We keep the stub
    // additive — recover the game's own FOV, then add exactly the delta to hit the
    // target. If the headset FOV isn't known yet, add nothing (leave game default).
    float target = akvr_head_render_hfov();
    // FOVABS: written by the stub, so a sudden FOV change by the game never shows for a frame.
    if (target > 10.0f && !g_testKeepFov)
    {
        g_gameFov = *g_bFov;                   // ZOOMVIG: the game's own FOV, saved by the stub
        *g_aFov = target + g_fovDelta;
        *g_dFov = 0.0f;
        *g_fovAbs = 1;
    }
    else
    {
        *g_dFov = 0.0f;
        *g_fovAbs = 0;
        if (*g_bFov > 1.0f) g_gameFov = *g_bFov;
    }
    // CAMTEST 2026-10-02 (Batmobile enter/exit: the transition plays with head tracking off): switch off one part of the
    // head's write at a time - rotation, position - to find which one stops it.
    if (g_testNoRot) { *g_dYaw = *g_dPitch = *g_dRoll = 0; }
    if (g_testNoPos) { *g_dPosX = *g_dPosY = *g_dPosZ = 0.0f; }
    pause_look_write();   // PAUSELOOK
    akvr_camera_record_rotators();   // NEARRAIN2
    // CAMRESTORE: the frame has been drawn; give the game its own camera back (once per finalize, never in the pause view)
    const uintptr_t cb = cam_base();
    if (g_camRestore && cb && rsFc != g_restoredFc && !g_pauseWriting && akvr_camera_finalize_count() == rsFc)   // no new finalize since
    {
        __try
        {
            // AIMTRIGGER 2026-10-03 — JJ on COPYHOLD: still "pops a little" getting out, now more looking straight at
            // the car, "we seem to be going around in circles". F2 01:01: at a steep camera the head reaches the game's
            // rotation as a mix of yaw, pitch AND roll (roll 9.7 -> 19.0 on the copy frame) that changes as the game's
            // camera moves, so a copied head can never be taken off exactly. Root fix: the game gets its own rotation
            // back (full CAMRESTORE, JJ: "it was fine before") except while the left trigger is held (Batmobile battle
            // mode / gadget aim), when the head stays in for aiming. Transitions start from the game's own camera.
            const bool aimNow = g_aimHead && akvr_gamepad_left_trigger_ms() < 300;
            g_rbHead = aimNow;
            if (aimNow) ++g_aimFrames;
            if (aimNow)
            {   // AIMHEAD: the rotation keeps the head (the Batmobile aims with it); only our corrections come off
                *(int32_t*)(cb + OFF_YAW)   -= (int32_t)(rsCopy[0] / ROT2DEG);
                *(int32_t*)(cb + OFF_PITCH) -= (int32_t)((rsCopy[1] + rsTilt) / ROT2DEG);
            }
            else
            {
                *(int32_t*)(cb + OFF_YAW)   = *g_bYaw;
                *(int32_t*)(cb + OFF_PITCH) = *g_bPitch;
                *(int32_t*)(cb + OFF_ROLL)  = *g_bRoll;
            }
            *(float*)(cb + OFF_X) -= rsPos[0];
            *(float*)(cb + OFF_Y) -= rsPos[1];
            *(float*)(cb + OFF_Z) -= rsPos[2];
            if (rsFovAbs && *g_bFov > 1.0f) *(float*)(cb + OFF_FOV) = *g_bFov;
            g_restoredFc = rsFc;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_restoredFc = rsFc; }
    }
}

float akvr_camera_game_fov() { return g_gameFov; }   // ZOOMVIG: 0 until the FOV lock has run

// NEARRAIN 2026-10-01: the game's OWN camera axes this frame (the base rotator the stub saved at the last finalize,
// before the head was added), UE3 world space, FRotationMatrix convention: X forward, Y right, Z up. The fix's rain
// shader turns the camera-kept rain block from the head-turned camera (read from its own view-projection) back to
// this one, so it hangs still when only the head moves. False while head tracking is off (then nothing to undo).
namespace {
    void ue3_axes(int32_t yaw, int32_t pitch, int32_t roll, float fwd[3], float right[3], float up[3])
    {
        const float y = (float)yaw * ROT2DEG * DEG2RAD, p = (float)pitch * ROT2DEG * DEG2RAD, r = (float)roll * ROT2DEG * DEG2RAD;
        const float sy = sinf(y), cy = cosf(y), sp = sinf(p), cp = cosf(p), sr = sinf(r), cr = cosf(r);
        fwd[0] = cp * cy;                 fwd[1] = cp * sy;                 fwd[2] = sp;
        right[0] = sr * sp * cy - cr * sy; right[1] = sr * sp * sy + cr * cy; right[2] = -sr * cp;
        up[0] = -(cr * sp * cy + sr * sy); up[1] = cy * sr - cr * sp * sy;   up[2] = cr * cp;
    }
    // NEARRAIN2: the game camera (base) and the drawn camera (base + head, the fields after the finalize) per Present,
    // so the rain correction can use the pair from k Presents ago (the game may place the block with an older camera).
    struct CamRec { int32_t by, bp, br, fy, fp, fr; float dx, dy, dz; float px, py, pz; bool ok; };   // NEARRAIN3: + the head's position offset; FRAMEPAIR: + the drawn camera's position
    CamRec   g_camRing[16] = {};
    volatile LONG g_camHead = 0;
}
void akvr_camera_record_rotators()   // once per Present, after the head update (and the pause write)
{
    CamRec rec{};
    const uintptr_t b = cam_base();
    if (g_htOn && g_bYaw && b)
    {
        __try
        {
            rec.fy = *(int32_t*)(b + OFF_YAW); rec.fp = *(int32_t*)(b + OFF_PITCH); rec.fr = *(int32_t*)(b + OFF_ROLL);
            rec.by = *g_bYaw; rec.bp = *g_bPitch; rec.br = *g_bRoll; rec.ok = true;
            rec.dx = *g_dPosX; rec.dy = *g_dPosY; rec.dz = *g_dPosZ;   // NEARRAIN3
            rec.px = *(float*)(b + OFF_X); rec.py = *(float*)(b + OFF_Y); rec.pz = *(float*)(b + OFF_Z);   // FRAMEPAIR
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { rec.ok = false; }
    }
    const LONG h = g_camHead + 1;
    g_camRing[h & 15] = rec;
    InterlockedExchange(&g_camHead, h);
}
// TARGETUP2 2026-10-02 — JJ: rolling the head, the reticle and the distance marker are "kind of trying to roll but then
// trying to stay upright at the same time". TARGETUP took the world's up from the camera recorded one Present back; the
// ring is written at Present, AFTER the head update for the NEXT frame, so ring 1 is two head updates older than the
// frame whose HUD is being drawn - the correction trailed the roll. The live fields (the last finalize = the frame being
// rendered) are read here instead.
bool akvr_camera_live_axes(float fwd[3], float right[3], float up[3])
{
    const uintptr_t b = cam_base();
    if (!g_htOn || !b) return false;
    int32_t y = 0, p = 0, r = 0;
    __try { y = *(int32_t*)(b + OFF_YAW); p = *(int32_t*)(b + OFF_PITCH); r = *(int32_t*)(b + OFF_ROLL); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    ue3_axes(y, p, r, fwd, right, up);
    return true;
}
bool akvr_camera_base_axes(float fwd[3], float right[3], float up[3])
{
    if (!g_htOn || !g_dYaw || !g_bYaw) return false;
    ue3_axes(*g_bYaw, *g_bPitch, *g_bRoll, fwd, right, up);
    return true;
}
// NEARRAIN3 2026-10-01 — JJ: "previously the rain was attached to head position translation, and now it's attached
// somehow to head rotation while staying mostly hanging in space". The block follows the camera's POSITION, never
// its rotation: undo only the position offset AKVR adds to the camera (lean, eye height, stick lift, shoulder), as
// recorded k Presents ago. World units, the same ones the stub adds to the camera fields.
bool akvr_camera_pos_delta_ago(int k, float d[3])
{
    if (k < 0 || k > 12) return false;
    const CamRec rec = g_camRing[(g_camHead - k) & 15];
    if (!rec.ok) return false;
    d[0] = rec.dx; d[1] = rec.dy; d[2] = rec.dz;
    return true;
}
bool akvr_camera_axes_ago(int k, float bf[3], float br[3], float bu[3], float ff[3], float fr[3], float fu[3])
{
    if (k < 0 || k > 12) return false;
    const CamRec rec = g_camRing[(g_camHead - k) & 15];
    if (!rec.ok) return false;
    ue3_axes(rec.by, rec.bp, rec.br, bf, br, bu);
    ue3_axes(rec.fy, rec.fp, rec.fr, ff, fr, fu);
    return true;
}

// FRAMEPAIR 2026-10-02 — JJ: "Head pose delay of 2 makes the rain go rock solid and stable, and it makes the [reticle]
// and the distance HUD marker go rock solid. The problem with that is that the world becomes jittery." UE3's one-frame
// thread lag (kept on: 44 -> 84 fps under geo-11): the world is drawn with an older camera than the one the game used
// for the rain and the HUD markers. The drawn camera (axes + position) recorded k Presents ago.
bool akvr_camera_pose_ago(int k, float f[3], float r[3], float u[3], float pos[3])
{
    if (k < 0 || k > 12) return false;
    const CamRec rec = g_camRing[(g_camHead - k) & 15];
    if (!rec.ok) return false;
    ue3_axes(rec.fy, rec.fp, rec.fr, f, r, u);
    pos[0] = rec.px; pos[1] = rec.py; pos[2] = rec.pz;
    return true;
}

bool akvr_camera_base_yaw_deg(float& deg)
{
    // g_bYaw holds the game's OWN yaw for the frame that just finalized — the stub
    // saves it a moment before overwriting the field with base∘head, so it carries
    // no head rotation at all. That is what yaw folding must use: the compositor
    // already time-warps each eye by its own head delta, and folding the head in
    // again here would rotate the stale eye twice.
    // The stub only writes it while the mod is actually driving the camera, so an
    // idle slot would be a stale number, not a heading — report "no data" instead.
    if (!g_bYaw || !g_htOn || !g_htEnable || !*g_htEnable) return false;
    deg = (float)(*g_bYaw) * ROT2DEG;
    return true;
}

HeadTrackState akvr_head_state()
{
    HeadTrackState s{};
    s.on = g_htOn; s.installed = g_htInstalled;
    s.yaw = g_htYaw; s.pitch = g_htPitch; s.roll = g_htRoll;
    s.leanX = g_htLX; s.leanY = g_htLY; s.leanZ = g_htLZ;
    s.posScale = g_posScale;
    s.fovDelta = g_fovDelta;
    return s;
}

void  akvr_head_pos_scale_mul(float f)
{
    g_posScale *= f;
    if (g_posScale < 5.0f)    g_posScale = 5.0f;
    if (g_posScale > 5000.0f) g_posScale = 5000.0f;
}
float akvr_head_pos_scale() { return g_posScale; }
void  akvr_head_pos_scale_set(float v)
{
    g_posScale = v;
    if (g_posScale < 5.0f)    g_posScale = 5.0f;
    if (g_posScale > 5000.0f) g_posScale = 5000.0f;
}

void  akvr_head_fov_add(float d)
{
    g_fovDelta += d;
    if (g_fovDelta < -70.0f) g_fovDelta = -70.0f;   // keep the final FOV sane
    if (g_fovDelta >  70.0f) g_fovDelta =  70.0f;    // headroom to grow the square view
}
float akvr_head_fov_delta() { return g_fovDelta; }
void  akvr_head_fov_set(float v)
{
    g_fovDelta = v;
    if (g_fovDelta < -70.0f) g_fovDelta = -70.0f;
    if (g_fovDelta >  70.0f) g_fovDelta =  70.0f;
}

void  akvr_head_set_eye(int e)
{
    g_curEye = (e < 0) ? -1 : (e == 1) ? 1 : 0;
    proj_refresh_jitter();
}
void  akvr_head_stereo_add(float d)
{
    g_stereoHalfUU += d;
    if (g_stereoHalfUU < 0.0f)  g_stereoHalfUU = 0.0f;    // 0 = mono (no separation)
    if (g_stereoHalfUU > 80.0f) g_stereoHalfUU = 80.0f;
}
float akvr_head_stereo() { return g_stereoHalfUU; }

void  akvr_head_shoulder_set(float v)
{
    g_shoulderUU = v;
    if (g_shoulderUU < -200.0f) g_shoulderUU = -200.0f;
    if (g_shoulderUU >  200.0f) g_shoulderUU =  200.0f;
}
float akvr_head_shoulder() { return g_shoulderUU; }
void  akvr_head_menu_side_set(float v) { g_menuSideUU = v < -400.0f ? -400.0f : (v > 400.0f ? 400.0f : v); }   // MENUSIDE
float akvr_head_menu_side() { return g_menuSideUU; }
void  akvr_head_pitch_keep_set(float v) { g_basePitchKeep = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
float akvr_head_pitch_keep() { return g_basePitchKeep; }
void  akvr_head_pitch_unlink_set(int mode) { g_pitchUnlink = mode < 0 ? 0 : (mode > 2 ? 2 : mode); }
int   akvr_head_pitch_unlink() { return g_pitchUnlink; }
void  akvr_head_stick_height_set(float m) { g_stickHeightM = m < 0.0f ? 0.0f : (m > 8.0f ? 8.0f : m); }
float akvr_head_stick_height() { return g_stickHeightM; }
void  akvr_head_shoulder_add(float d) { akvr_head_shoulder_set(g_shoulderUU + d); }

void  akvr_head_offset_up_set(float v)
{
    g_offUpUU = v;
    if (g_offUpUU < -400.0f) g_offUpUU = -400.0f;
    if (g_offUpUU >  400.0f) g_offUpUU =  400.0f;
}
float akvr_head_offset_up() { return g_offUpUU; }

void  akvr_head_camfix_set(bool on) { g_camFixOn = on; }
bool  akvr_head_camfix()            { return g_camFixOn; }

// Dump the ring buffer, oldest first, next to the exe. Called off the hot path (a
// key press in Present), so plain file I/O is fine here.
int akvr_camera_trace_dump(const wchar_t* path)
{
    if (!g_trace || g_traceLen <= 0) return 0;
    FILE* f = _wfopen(path, L"wb");
    if (!f) return 0;
    fprintf(f, "t_sec,frame,eye,base_yaw_deg,final_yaw_deg,final_pitch_deg,final_roll_deg,"
               "cam_x,cam_y,cam_z,delta_x,delta_y,delta_z,eye_offset_uu,"
               "lean_x_m,lean_y_m,lean_z_m,"
               "head_yaw_deg,head_pitch_deg,head_roll_deg,"
               "base_pitch_deg,base_roll_deg,"
               "raw_head_yaw,raw_head_pitch,raw_head_roll,display_roll,"
               "live_yaw,live_pitch,live_roll\n");
    // Oldest first: when the buffer has wrapped, the oldest record is the one we are
    // about to overwrite next.
    int start = (g_traceLen == kTraceCap) ? g_traceHead : 0;
    for (int i = 0; i < g_traceLen; ++i)
    {
        const TraceRec& r = g_trace[(start + i) % kTraceCap];
        fprintf(f, "%.6f,%llu,%d,%.4f,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,"
                   "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
                   "%.4f,%.4f,%.4f,%.4f,%.4f,"
                   "%.4f,%.4f,%.4f,%.4f,"
                   "%.4f,%.4f,%.4f\n",
                r.t, (unsigned long long)r.frame, r.eye, r.baseYaw,
                r.fyaw, r.fpitch, r.froll, r.camX, r.camY, r.camZ,
                r.dx, r.dy, r.dz, r.eyeUU, r.leanX, r.leanY, r.leanZ,
                r.headYaw, r.headPitch, r.headRoll, r.basePitch, r.baseRoll,
                r.rawHeadYaw, r.rawHeadPitch, r.rawHeadRoll, r.dispRoll,
                r.liveYaw, r.livePitch, r.liveRoll);
    }
    fclose(f);
    return g_traceLen;
}

void  akvr_head_offset_fwd_set(float v)
{
    g_offFwdUU = v;
    if (g_offFwdUU < -400.0f) g_offFwdUU = -400.0f;
    if (g_offFwdUU >  400.0f) g_offFwdUU =  400.0f;
}
float akvr_head_offset_fwd() { return g_offFwdUU; }
void  akvr_head_stereo_set(float v)
{
    g_stereoHalfUU = v;
    if (g_stereoHalfUU < 0.0f)  g_stereoHalfUU = 0.0f;
    if (g_stereoHalfUU > 80.0f) g_stereoHalfUU = 80.0f;
}

// =============================== Shutdown cleanup ============================
// Restore the original game code for all hand-assembled patches so the process
// can exit without executing code in caves that may reference DLL state.
void akvr_camera_shutdown()
{
    // This only runs at process exit. Restoring the original bytes is a courtesy,
    // not a necessity (the process is dying), so if anything faults here we must
    // NOT let it surface as the game's "Fatal error" box on the way out — swallow it.
    __try {
        if (g_installed && g_target)
            write_code((void*)g_target, g_readOrig, sizeof(g_readOrig));
        if (g_epiPatched && g_epiAddr)   // only restore the detour if we actually wrote it
            write_code((void*)g_epiAddr, g_epiOrig, sizeof(g_epiOrig));
        akvr_proj_uninstall();
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// CAMTEST: Batmobile transition test switches (panel only, not saved)
bool akvr_camtest_get(int which) { return which == 0 ? g_testNoRot : (which == 1 ? g_testNoPos : g_testKeepFov); }
void akvr_camtest_set(int which, bool on) { if (which == 0) g_testNoRot = on; else if (which == 1) g_testNoPos = on; else g_testKeepFov = on; }
bool akvr_cam_restore() { return g_camRestore; }               // CAMRESTORE
void akvr_cam_restore_set(bool on) { g_camRestore = on; }
bool akvr_cam_smooth() { return g_camSmooth; }                 // CAMSMOOTH
void akvr_cam_smooth_set(bool on) { g_camSmooth = on; }
long akvr_cam_smooth_events() { return g_smoothEvents; }
bool akvr_cam_swing() { return g_swingEase; }                  // SWINGEASE
void akvr_cam_swing_set(bool on) { g_swingEase = on; }
bool akvr_cam_aim_head() { return g_aimHead; }                 // AIMHEAD
bool akvr_cam_aim_head_now() { return g_rbHead; }              // AIMTRIGGER2: the head is in the read-back now
long akvr_cam_exact_writes() { return g_exWrites; }            // DIVEFIX: finalizes whose rotation exact_head wrote
bool akvr_cam_exact_on() { return g_exOn; }
long akvr_cam_aim_head_frames() { return g_aimFrames; }
void akvr_cam_aim_head_set(bool on) { g_aimHead = on; }
