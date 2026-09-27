#include "present_probe.h"
#include <cstdio>

namespace {
struct Record {
    uint64_t present = 0, camera = 0;
    double t = 0, beginMs = 0, waitMs = 0, presentMs = 0, submitMs = 0;
    bool gameplay = false, after = false;
    int poseDelay = -1;
    uint64_t preHash = 0, postHash = 0;
};
struct Slot {
    ID3D11Texture2D* stage = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool pending = false;
    Record rec;
};
Slot slots[4];
Slot* active = nullptr;
uint64_t frame = 0;
Record records[2048];
unsigned head = 0, count = 0;

// Every-frame CPU timing (2026-09-26): the 1-in-30 GPU sampler above is too sparse
// to see a hitch. No GPU work, one struct per Present.
struct FrameTiming {
    uint64_t present = 0, camera = 0;
    double start = 0, waitMs = 0, beginMs = 0, submitMs = 0, presentMs = 0, hookMs = 0;
    bool gameplay = false;
};
FrameTiming frames[8192];
unsigned frameHead = 0, frameCount = 0;
FrameTiming cur;

void copy_samples(ID3D11DeviceContext* ctx, ID3D11Texture2D* src,
                  ID3D11Texture2D* stage, unsigned row) {
    D3D11_TEXTURE2D_DESC d{}; src->GetDesc(&d);
    for (unsigned y=0; y<4; ++y) for (unsigned x=0; x<4; ++x) {
        unsigned sx=d.Width*(2*x+1)/8, sy=d.Height*(2*y+1)/8;
        D3D11_BOX box{sx,sy,0,sx+1,sy+1,1};
        ctx->CopySubresourceRegion(stage,0,x,y+row,0,src,0,&box);
    }
}
uint64_t hash_rows(const D3D11_MAPPED_SUBRESOURCE& map, unsigned row) {
    uint64_t h=14695981039346656037ull;
    for (unsigned y=row;y<row+4;++y) {
        auto p=static_cast<const unsigned char*>(map.pData)+y*map.RowPitch;
        for (unsigned x=0;x<16;++x) { h^=p[x]; h*=1099511628211ull; }
    }
    return h;
}
}

double akvr_probe_clock_ms() {
    static LARGE_INTEGER freq=[] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    return double(now.QuadPart)*1000.0/double(freq.QuadPart);
}

void akvr_present_probe_begin(ID3D11DeviceContext* context) {
    ++frame;
    active=nullptr;
    cur={}; cur.present=frame; cur.start=akvr_probe_clock_ms();
    if (!context) return;
    for (auto& slot:slots) if (slot.pending) {
        D3D11_MAPPED_SUBRESOURCE map{};
        HRESULT hr=context->Map(slot.stage,0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&map);
        if (hr==DXGI_ERROR_WAS_STILL_DRAWING) continue;
        if (SUCCEEDED(hr)) {
            slot.rec.preHash=hash_rows(map,0);
            slot.rec.postHash=hash_rows(map,4);
            context->Unmap(slot.stage,0);
            records[head]=slot.rec;
            head=(head+1)%2048;
            if (count<2048) ++count;
        }
        slot.pending=false;
    }
}

void akvr_present_probe_before(ID3D11Device* device, ID3D11DeviceContext* context,
    ID3D11Texture2D* source, uint64_t cameraFrame, bool gameplay, bool afterPresent,
    double xrBeginMs, double xrWaitMs, int poseDelay) {
    cur.camera=cameraFrame; cur.gameplay=gameplay; cur.waitMs=xrWaitMs; cur.beginMs=xrBeginMs;
    if (frame%30 || !device || !context || !source) return;
    D3D11_TEXTURE2D_DESC d{}; source->GetDesc(&d);
    if (d.SampleDesc.Count!=1 || d.Width<8 || d.Height<8 ||
        (d.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && d.Format!=DXGI_FORMAT_B8G8R8A8_UNORM)) return;
    for (auto& slot:slots) if (!slot.pending) {
        if (slot.stage && slot.format!=d.Format) { slot.stage->Release(); slot.stage=nullptr; }
        if (!slot.stage) {
            D3D11_TEXTURE2D_DESC sd{};
            sd.Width=4; sd.Height=8; sd.MipLevels=1; sd.ArraySize=1;
            sd.Format=d.Format; sd.SampleDesc.Count=1;
            sd.Usage=D3D11_USAGE_STAGING; sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            if (FAILED(device->CreateTexture2D(&sd,nullptr,&slot.stage))) return;
            slot.format=d.Format;
        }
        slot.rec={}; slot.rec.present=frame; slot.rec.camera=cameraFrame;
        slot.rec.t=akvr_probe_clock_ms(); slot.rec.gameplay=gameplay;
        slot.rec.after=afterPresent; slot.rec.beginMs=xrBeginMs; slot.rec.waitMs=xrWaitMs;
        slot.rec.poseDelay=poseDelay;
        copy_samples(context,source,slot.stage,0);
        active=&slot;
        return;
    }
}

void akvr_present_probe_after(ID3D11DeviceContext* context, ID3D11Texture2D* source,
    double presentMs, double submitMs) {
    cur.presentMs=presentMs; cur.submitMs=submitMs; cur.hookMs=akvr_probe_clock_ms()-cur.start;
    frames[frameHead]=cur; frameHead=(frameHead+1)%8192; if (frameCount<8192) ++frameCount;
    if (!active || !context || !source) return;
    copy_samples(context,source,active->stage,4);
    active->rec.presentMs=presentMs; active->rec.submitMs=submitMs;
    active->pending=true; active=nullptr;
}

void akvr_present_probe_dump(const wchar_t* path) {
    FILE* file=nullptr;
    if (_wfopen_s(&file,path,L"wb") || !file) return;
    fprintf(file,"present,camera_frame,qpc_ms,gameplay,submit_after_present,xr_begin_ms,xr_wait_ms,present_ms,xr_submit_ms,pre_hash,post_hash,changed_during_present,pose_delay\n");
    for (unsigned i=0;i<count;++i) {
        const auto& r=records[(head+2048-count+i)%2048];
        fprintf(file,"%llu,%llu,%.3f,%d,%d,%.3f,%.3f,%.3f,%.3f,%llu,%llu,%d,%d\n",
            r.present,r.camera,r.t,r.gameplay,r.after,r.beginMs,r.waitMs,r.presentMs,r.submitMs,
            r.preHash,r.postHash,r.preHash!=r.postHash,r.poseDelay);
    }
    fclose(file);
}

void akvr_present_probe_dump_frames(const wchar_t* path) {
    FILE* file=nullptr;
    if (_wfopen_s(&file,path,L"wb") || !file) return;
    fprintf(file,"present,camera_frame,start_ms,interval_ms,gameplay,xr_wait_ms,xr_begin_ms,xr_submit_ms,present_ms,hook_total_ms\n");
    double prev=0;
    for (unsigned i=0;i<frameCount;++i) {
        const auto& r=frames[(frameHead+8192-frameCount+i)%8192];
        fprintf(file,"%llu,%llu,%.3f,%.3f,%d,%.3f,%.3f,%.3f,%.3f,%.3f\n",
            r.present,r.camera,r.start,i?r.start-prev:0.0,r.gameplay,r.waitMs,r.beginMs,
            r.submitMs,r.presentMs,r.hookMs);
        prev=r.start;
    }
    fclose(file);
}
