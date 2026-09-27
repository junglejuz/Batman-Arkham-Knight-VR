#include "../src/present_probe.h"
#include <fstream>
#include <string>
#include <vector>
#include <cstdio>

int main() {
    ID3D11Device* dev=nullptr; ID3D11DeviceContext* ctx=nullptr;
    if (FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,
        D3D11_SDK_VERSION,&dev,nullptr,&ctx))) return 1;
    D3D11_TEXTURE2D_DESC d{};
    d.Width=64; d.Height=32; d.MipLevels=1; d.ArraySize=1;
    d.Format=DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count=1;
    d.Usage=D3D11_USAGE_DEFAULT;
    ID3D11Texture2D* tex=nullptr;
    if (FAILED(dev->CreateTexture2D(&d,nullptr,&tex))) return 2;
    std::vector<unsigned> pixels(64*32,0xff0000ff);
    ctx->UpdateSubresource(tex,0,nullptr,pixels.data(),64*4,0);
    for (int i=0;i<30;++i) akvr_present_probe_begin(ctx);
    akvr_present_probe_before(dev,ctx,tex,123,true,true,2.0,1.0,2);
    for (auto& p:pixels) p=0xff00ff00;
    ctx->UpdateSubresource(tex,0,nullptr,pixels.data(),64*4,0);
    akvr_present_probe_after(ctx,tex,3.0,4.0);
    ctx->Flush(); // Test only; production never flushes or waits for the probe.
    for (int i=0;i<30;++i) { Sleep(2); akvr_present_probe_begin(ctx); }
    akvr_present_probe_before(dev,ctx,tex,456,true,false,2.0,1.0,1);
    akvr_present_probe_after(ctx,tex,3.0,4.0); // unchanged pixels
    ctx->Flush();
    for (int i=0;i<30;++i) { Sleep(2); akvr_present_probe_begin(ctx); }
    wchar_t path[MAX_PATH]; GetTempPathW(MAX_PATH,path);
    std::wstring out=std::wstring(path)+L"akvr-probe-test-"+std::to_wstring(GetCurrentProcessId())+L".csv";
    akvr_present_probe_dump(out.c_str());
    std::ifstream file(out); std::string header,a,b;
    std::getline(file,header); std::getline(file,a); std::getline(file,b);
    file.close(); DeleteFileW(out.c_str());
    const bool ok=a.find("30,123,")==0 && a.size()>4 && a.compare(a.size()-4,4,",1,2")==0
        && b.find("60,456,")==0 && b.size()>4 && b.compare(b.size()-4,4,",0,1")==0;
    tex->Release(); ctx->Release(); dev->Release();
    if (!ok) std::fprintf(stderr,"Probe failed: %s / %s\n",a.c_str(),b.c_str());
    return ok ? 0 : 3;
}
