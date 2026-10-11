# Third-party notices

Batman: Arkham Knight VR is built on other people's work. Thank you.

| Project | Licence | Source | Used for |
|---|---|---|---|
| MinHook | BSD-2-Clause | https://github.com/TsudaKageyu/minhook | function hooking |
| Dear ImGui | MIT | https://github.com/ocornut/imgui | the in-headset VR menu |
| OpenXR SDK (loader) | Apache-2.0 | https://github.com/KhronosGroup/OpenXR-SDK | talking to the PC VR software |
| SMAA (Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro, Diego Gutierrez) | MIT (in `vendor/smaa/SMAA.hlsl`) | https://github.com/iryoku/smaa | the optional edge smoothing on the way to the headset |
| Luma Framework (Filippo Tarpini, "Garamond") | custom MIT, credit required (see `LICENSE-luma-framework.md`) | https://github.com/Filoppi/Luma-Framework | the method for per-eye DLSS (jitter, camera motion vectors, sharper textures) and the lens-flare-free copies of two of the game's shaders, from Luma's Arkham Knight mod |
| NVIDIA DLSS (`akn\nvngx_dlss.dll`) | NVIDIA RTX SDK licence; © NVIDIA Corporation | https://developer.nvidia.com/rtx/dlss | upscaling and anti-aliasing (optional, NVIDIA RTX graphics cards) |

Building from source also needs NVIDIA's DLSS SDK (NGX headers and library, version 310.9.1) in
`native/third_party/ngx/`; it is not part of this repository. Download it from NVIDIA.
