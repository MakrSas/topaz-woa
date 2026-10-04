# P10: OpenGL and Vulkan for apps (plan, 2026-10-04)

## Why games refuse to start
User report (2026-10-04): a game says "A video driver supporting OpenGL 2.1 or higher is required";
Lucid Blocks (Godot) says the driver "does not support the required Vulkan version" and suggests
`--rendering-driver opengl3`.

Our GPU stack exposes only the **D3D10/11 user-mode driver** (Mesa d3d10umd + freedreno over
TopazGpuW, `docs/P8_gpu.md`). There is no OpenGL ICD and no Vulkan ICD registered for the adapter, so
OpenGL falls back to Windows' GDI OpenGL 1.1 and Vulkan has no device.

## Options (cheapest first)

### 1. Microsoft "OpenCL, OpenGL and Vulkan Compatibility Pack" (no code)
- Layers OpenGL 3.3 (GLOn12) and Vulkan (Dozen) on top of **D3D12**. We have no D3D12 UMD, so it would
  run on **WARP** (D3D12 on the CPU).
- Install from the Store / msix and try the games: tells quickly whether they start at all.
- Main risk: speed - the whole renderer on the phone CPU.

### 2. Hardware OpenGL: Mesa WGL ICD on freedreno (recommended next real step)
- Mesa already has the Windows OpenGL frontend (gallium `wgl` / stw, `libgallium_wgl.dll` /
  `opengl32` replacement). We already build freedreno + our WDDM winsys (`mesa-overlay/`, CI `mesa.yml`).
- Steps:
  1. build the WGL target for freedreno with the same winsys (`fd_wddm.c`) in `mesa.yml`;
  2. present path: stw needs a way to put the back buffer on screen - start with the GDI copy path
     (stw_winsys "present" via BitBlt), later a D3DKMT present / flip through TopazGpuW;
  3. register the ICD for the adapter (`OpenGLDriverName` / `OpenGLVersion` / `OpenGLFlags` under the
     adapter's registry key, ARM64 + ARM64EC/x64 builds if x64 games should load it);
  4. test with a GL info tool, then the games (`--rendering-driver opengl3` for Godot).
- Main risk: the present path through our WDDM KMD (same class of problems as the D3D present work).

### 3. Hardware Vulkan: turnip (Mesa's Adreno Vulkan driver) on WDDM
- Turnip only has Linux kernel backends (msm, kgsl); it needs a new backend on our escapes (GEM BOs,
  submit, fences - the same calls the gallium winsys makes) plus WSI (swapchain/present) on Windows.
- Then a Vulkan ICD JSON registered for the adapter.
- Main risk: size of the port (backend + WSI), and the A610 being an older a6xx that turnip supports
  less well than newer parts.

## Order
1 (check the games start at all) -> 2 (hardware OpenGL) -> 3 (Vulkan) later.
