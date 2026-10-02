# Mesa overlay for the Adreno 610 UMD (stage G3, docs/P8_gpu.md)

CI (`.github/workflows/mesa.yml`) clones `gitlab.freedesktop.org/max8rr8/mesa` branch `viogpu_win`
(Mesa 24.3-devel with d3d10umd + the gdikmt WDDM helper layer used by viogpu3d), copies `files/`
over it, applies `patches/*.patch` in name order and builds freedreno + d3d10umd for Windows ARM64
(MSVC cross from windows-2022). Output: `topazgpu_d3d10.dll`.
