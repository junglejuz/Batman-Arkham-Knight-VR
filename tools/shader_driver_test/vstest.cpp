// Load one assembled vertex shader into the real GPU driver: prints OK / FAILED hr. A driver crash kills
// only this process (exit code shows it).
#include <d3d11.h>
#include <cstdio>
#include <vector>
#pragma comment(lib, "d3d11.lib")
int main(int argc, char** argv)
{
    if (argc < 2) return 2;
    FILE* f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open %s\n", argv[1]); return 2; }
    std::vector<char> b;
    char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) b.insert(b.end(), buf, buf + n);
    fclose(f);
    ID3D11Device* dev = nullptr;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, nullptr)))
    { printf("no device\n"); return 3; }
    ID3D11VertexShader* vs = nullptr;
    HRESULT hr = dev->CreateVertexShader(b.data(), b.size(), nullptr, &vs);
    printf("%s: %s (0x%08lx)\n", argv[1], SUCCEEDED(hr) ? "OK" : "FAILED", (unsigned long)hr);
    if (vs) vs->Release();
    dev->Release();
    return SUCCEEDED(hr) ? 0 : 1;
}
