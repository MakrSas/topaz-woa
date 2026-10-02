# P8: GPU (Adreno 610) for Windows — research and plan (2026-10-01)

## Why
Without a render driver Windows composes on the CPU: `TopazDisplay` is display-only (KMDOD), so DWM
and every D3D app run on WARP. Measured over SSH: `dwm` ~0.7 core at idle UI; a Flutter app with blur
(FlClash) drops the whole screen to ~2 fps while its window is open; Windows animations and
transparency are choppy. The CPU part is done (TopazCpu: gold cluster 300 → 2803 MHz); smooth
animations need the GPU.

## Path 1 (Qualcomm's Windows driver) — rejected
- Package: WOA-Project/Qualcomm-Reference-Drivers `7180_CLS/200.0.32.0/qcdx7180.cab` (32 174 023 B,
  Snapdragon 7c = SC7180, Adreno 618) + `qcpep.wd7180.cab`. Copies on s8build `~/work/gpu/`
  (proprietary — never commit them).
- INF: `ACPI\QCOM083A` (+ SUBSYS CLS07180/IDP07180/CLS08180...). KMD `qcdxkm7180.sys` is shared with
  850/8180/8280 (firmware names `qcdxkmsuc{7180,8180,8280,850}.mbn`), UMD `qcdx11/12arm64xum7180.dll`,
  compiler `qcdxarm64xcompiler7180.dll`.
- The KMD is built around a **GMU** (`GmuPwrStart`, `GmuPwrRequestPowerOn`, `GmuPwrGxBwVote`,
  `GmuPwrQueryGxPowerState`, `DisableGmuACD`...). Every chip it targets has one (A618/630/680/690).
  **A610 has no GMU** (Linux drives it through a "GMU wrapper"). Making it work = reverse engineering
  the KMD or emulating a GMU, plus the SC7180 PEP (clock/rail topology of another SoC). Not worth it.
- The UMD/compiler contain the A610 chip id (shared Qualcomm compiler), but the UMD↔KMD private
  protocol is undocumented, so the UMD cannot be reused with our own KMD either.

## Path 2 (own driver on Mesa freedreno) — chosen
| Stage | What | Cost |
|---|---|---|
| G1 | "GPU alive": KMDF test driver `TopazGpu` — clocks/GDSC on, GPU SMMU identity bank, zap shader via TZ PAS, SQE microcode, ring buffer, CP_ME_INIT, CP_MEM_WRITE test, IRQ | ~1 day |
| G2 | WDDM **render-only** KMD (TopazDisplay stays the display-only adapter; Windows copies frames across adapters): allocations in system RAM, GPU VA = PA (identity SMMU; buffers below 4 GB phys: A610 has `ADRENO_QUIRK_4GB_VA`), DxgkDdiSubmitCommand → ring, IRQ → fences, reset on hang | days |
| G3 | UMD: Mesa `src/gallium/frontends/d3d10umd` (D3D10 DDI, used by VMware's Windows driver with svga) + freedreno gallium (A610 is upstream) + a new freedreno winsys over WDDM callbacks (pfnAllocateCb / pfnRenderCb / escapes) instead of msm DRM ioctls; Mesa built for Windows ARM64 in CI | days–weeks |
| G4 | DWM on the GPU, then tuning (UBWC, GMEM, clocks/CX vote via RPM) | — |

## Hardware facts (stock DT `s8build ~/work/topaz/backup/fdt.dts` + mainline `sm6115.dtsi`)
- GPU `qcom,kgsl-3d0@5900000`: regs 0x5900000 (0x90000) + cx_dbgc 0x5961000 (0x800); IRQ SPI 0xB1
  (177) level high → GSIV 209; chipid **0x06010001** "Adreno610v2"; nvmem speed_bin/gaming_bin;
  initial pwrlevel 6; DDR bus table; `vdd-supply` = gpu_gx_gdsc, `vddcx-supply` = gpu_cx_gdsc.
- GPU clock controller 0x5990000 (gpucc-khaje; Linux `drivers/clk/qcom/gpucc-sm6115.c` is the
  closest): **GPU_CX_GDSC** gdscr 0x106c (abs 0x599106c, votable), **GPU_GX_GDSC** gdscr 0x100c
  (abs 0x599100c, CLAMP_IO | SW_RESET, parent CX); syscons gx_domain_addr 0x5991508, cx_hw_ctrl
  0x5991540, gx_sw_reset 0x5991008. Both GDSCs' parent supply = `pm6125_s3_level` (RPM) — the GPU runs
  on VDD_CX (mainline: `power-domains = <&rpmpd SM6115_VDDCX>`); CX is on already, high GPU clocks
  will need an RPM level vote later.
- GMU wrapper (mainline) 0x596a000, 0x30000.
- GPU SMMU (`kgsl-smmu`, mainline `adreno_smmu`) 0x59a0000, TBU 0x59c5000; mainline
  `iommus = <&adreno_smmu 0 1>`.
- Clock names in the stock DT: core_clk (gpucc gx_gfx3d), rbbmtimer, iface, ahb (gpucc), mem_clk
  (gcc gpu_memnoc_gfx), gmu_clk, smmu_vote, apb_pclk, gpu_cc_ahb, gcc_gpu_memnoc_gfx,
  gpu_cc_hlos1_vote_gpu_smmu, gcc_gpu_snoc_dvm_gfx (map to offsets with gcc-sm6115.c / gpucc-sm6115.c).
- Zap shader memory region: **0x55B15000, 0x2000** (inside Mu's "PIL Reserved" 0x4AB00000+0xB800000,
  so Windows never uses it). GPU PAS id **13** (Linux `GPU_PAS_ID`).
- Firmware: SQE `a630_sqe.fw` (linux-firmware, 34 188 B; s8build `~/work/gpu/a630_sqe.fw`); zap
  `a610_zap.mdt/.bNN` from the vendor partition (`/firmware` or `/firmware-mbn`): ColorOS repack
  `images/super.img`, vendor_a at offset 6343884800, length 691810304 (LP metadata parser
  `~/work/gpu/lp.py`); extracted image `~/work/gpu/vendor_a.img` (EROFS; old erofs-utils on s8build:
  `fsck.erofs --extract=DIR`). **Extracted to s8build `~/work/gpu/vfw/`:** `a610_zap.mdt` 6860 B,
  `.b00` 148, `.b01` 6712, `.b02` 2096, `a610_zap.elf` 14384, vendor `a630_sqe.fw` 32504 B (md5
  b0f92bf1…; differs from linux-firmware's 34188 B — use the vendor one first, it is what KGSL ran).

## Linux reference (s8build `~/work/topaz/linux/drivers/gpu/drm/msm/adreno/`)
- `a6xx_catalog.c:683` A610 entry: SQE a630_sqe.fw, zap a610_zap.mdt, gmem 128K+4K, quirk 4GB_VA,
  hwcg `a612_hwcg`, protect `a630_protect`, gbif_cx `a640_gbif`, gmu_cgc_mode 0x00020202,
  prim_fifo_threshold 0x00080000, funcs `a6xx_gmuwrapper_funcs`.
- `a6xx_gpu.c`: `hw_init` 1173 (register setup; a610 specifics: GBIF_QSB_SIDE0..3 = 0x00071620,
  RBBM_GBIF_CLIENT_QOS_CNTL 3, CP_ROQ_THRESHOLDS 0x00800060 / 0x40201b16, CP_MEM_POOL_SIZE 48,
  RBBM_INTERFACE_HANG_INT_CNTL (1<<30)|0x3ffff), `a6xx_ucode_load` 1046, `a6xx_cp_init` 895,
  `a6xx_pm_resume` 2158, gmuwrapper funcs 2824; zap: `adreno_zap_shader_load` in adreno_gpu.c.

## G1 plan in detail
1. GCC GPU clocks on (cfg_ahb, memnoc_gfx, snoc_dvm_gfx), GPU CC: CX GDSC on, AHB/CXO/smmu-vote
   clocks, PLL0 configured + locked, GX_GFX3D RCG at a low frequency, GX GDSC on. Verify GDSC
   PWR_ON status bits before touching 0x5900000 — **reading GPU registers with clocks or GDSC off
   hangs the bus** (same trap as the WLAN CE registers before WLAN_MODE).
2. GPU SMMU 0x59a0000: identity context bank (as TopazModem v0.7 did for the apps SMMU: clone a UEFI
   bank, SCTLR M=0, S2CR type 0 → bank) for the GPU stream ids.
3. Zap: copy `a610_zap.*` to `C:\topaz\fw\image\`, TZ PAS init/mem-setup/auth-and-reset with id 13
   (TopazModem `ModemPas.c` has the SCM calls), region 0x55B15000.
4. `hw_init` register list for A610, CP protect, UBWC config; SQE into a 4 KB-aligned buffer below
   4 GB → CP_SQE_INSTR_BASE; 32 KB ring → CP_RB_BASE / CP_RB_CNTL / CP_RB_RPTR_ADDR; CP_SQE_CNTL = 1;
   CP_ME_INIT (a6xx_cp_init); CP_SET_SECURE_MODE 0 (needs the zap); wait RPTR == WPTR.
5. Test: CP_MEM_WRITE magic → read back from RAM; CP_EVENT_WRITE + RBBM IRQ mask → see SPI 177.
   Log `C:\TopazGpu.log`; deploy over SSH like TopazCpu.

## Risks
- GPU CC PLL programming (alpha PLL config from gpucc-sm6115.c) and the CX level for the chosen clock.
- The GPU SMMU might have secure banks owned by TZ/hyp; the identity trick worked on the apps SMMU.
- WDDM render-only + display-only cross-adapter path on ARM64 with our KMDOD display driver.
- Mesa on Windows ARM64: d3d10umd is maintained for VMware; freedreno has never run on Windows.

## Results
- **TopazGpu v0.1** (2026-10-01, installed over SSH): the bootloader leaves the GPU **fully off** —
  GPU_CX_GDSC and GPU_GX_GDSC = 0x00222001 (SW_COLLAPSE, no PWR_ON), CX GDS HW ctrl 0x1540 =
  0x00005ff4, GPU CC PLL0/PLL1 mode 0 (unconfigured), gx_gfx3d / gmu RCGs root off, all GPU CC
  branches CLK_OFF (cxo/ahb/gx_cxo enable bits set but off), GCC: gpu_cfg_ahb running, memnoc_gfx
  enabled but off, snoc_dvm/iref/throttle off, vote 0x79004 = 0x00c05d5b (GPU GPLL0 bit 15 not set).
- **TopazGpu v0.2** (fe1f7be): one-shot power-up (`C:\topaz\gpu.on`): GCC GPLL0 vote + bimc_gpu_axi /
  memnoc_gfx / snoc_dvm branches → gpucc cxo/ahb → CX GDSC (poll 0x1540 bit31) → gmu RCG 200 MHz
  (GPLL0/3) + cx_gmu + smmu vote → GX GDSC (BCR 0x1008 pulse, clamp 0x1508 release, poll 0x100c
  bit31) → gx_gfx3d RCG 300 MHz (GPLL0/2) + branch (force mem core/periph) → read RBBM_STATUS,
  CP_HW_FAULT, CP_ALWAYS_ON_COUNTER (should tick ~19.2 MHz). Powers down on device stop.
- **TopazGpu v0.2** (2026-10-01): CX GDSC came up (PWR_ON), but the GPU CC cxo/ahb branches were
  polled before it and stayed off → abort. They live in the CX domain.
- **TopazGpu v0.3 (2026-10-02): GPU POWERED AND RESPONDING.** Order: GCC GPLL0 vote (0x79004 bit 15)
  + bimc_gpu_axi → CX GDSC (0x106c, status 0x1540 = 0x80001fe0) → gpucc cxo/ahb running → gmu RCG
  GPLL0/3 (cfg 0x505) + cx_gmu + smmu vote running → GX GDSC (BCR pulse, clamp release, 0x100c =
  0xf8222000 PWR_ON) → gx_gfx3d RCG GPLL0/2 = 300 MHz (cfg 0x503) + branch (0x6221) running. GPU reads:
  RBBM_STATUS 0x00000001, CP_HW_FAULT 0, CP_ALWAYS_ON_COUNTER 0x27143 → 0x59547 in ~10.7 ms
  (19.2 MHz) — no bus hang. GCC memnoc_gfx / snoc_dvm_gfx still report CLK_OFF (BRANCH_VOTED, Linux
  does not poll them) — check once the CP touches memory.
- Next (v0.4): GPU SMMU 0x59a0000 state dump (read only), then identity bank; zap via PAS 13; SQE;
  ring; CP_ME_INIT; CP_MEM_WRITE test.
- **TopazGpu v0.4** (2026-10-02): GPU SMMU 0x59a0000 = SMMU-500, sCR0 0x00240406 (CLIENTPD 0,
  USFCFG 1 → unmatched streams fault), ID0 0x4c017e04 / ID1 0x20000005 / ID2 0x00005511: **4 SMRs, 5
  context banks**, 4 KB pages × 8 (CB space at +0x8000). All SMRs invalid, S2CRs type 2 (fault), CB
  registers hold reset garbage → nothing configured; we own it (GPU SIDs: mainline `<&adreno_smmu 0 1>`
  = SID 0 mask 1). Zap: init_image 0/0, but mem_setup(0x55B15000, 0x830) → 0xffcfffba and
  auth_and_reset → 0xffcfffbc.
- **TopazGpu v0.5: ZAP SHADER ACCEPTED BY TZ.** Fix = mem_setup size aligned to 4 KB like Linux
  mdt_loader (`max_addr = ALIGN(paddr + memsz, SZ_4K)` → 0x1000). Sequence: PAS shutdown(13) →
  init_image(13, PA of the whole .mdt, 6860 B) → mem_setup(13, 0x55B15000, 0x1000) → copy
  a610_zap.b02 to 0x55B15000 → auth_and_reset(13): all 0/0.

## G1 v0.6 plan (CP start) — everything needed, collected from Linux
Order (Linux a6xx `hw_init` + `a6xx_cp_init`; zap is already loaded by v0.5 code before this):
1. GPU SMMU 0x59a0000 identity bank (as TopazModem `SmmuWlanMap`): CB0 page at +0x8000 (4 KB pages
   × 8): FSR(+0x58)=~0, TTBR0/TCR/TCR2/MAIR = 0, GR1 (+0x1000) CBA2R[0](+0x800)=1 (VA64), CBAR[0]=
   0x0001f000, SCTLR(+0x0)=0xE0 (M=0 → pass-through) last; S2CR0(+0xC00)=0 (type 0, cb 0), SMR0(+0x800)
   = (1<<31) | (1<<16) | 0 (SID 0 mask 1). GPU addresses = physical; keep buffers below 4 GB
   (A610 has ADRENO_QUIRK_4GB_VA anyway).
2. hw_init register subset (dword offsets, write = byte offset ×4): GBIF_HALT 0x3c45=0 and
   RBBM_GBIF_HALT 0x16=0; RBBM_SECVID_TSB_CNTL 0xF803=0, TSB_TRUSTED_BASE 0xF800/1=0, SIZE 0xF802=0;
   *_ADDR_MODE_CNTL=1: CP 0x842, VSC 0xC01, GRAS 0x8601, RB 0x8e05, PC 0x9e01, HLSQ 0xbe05, VFD 0xa601,
   VPC 0x9601, UCHE 0xE00, SP 0xae01, TPL1 0xb601, RBBM_SECVID_TSB 0xF810; GBIF_QSB_SIDE0..3
   0x3c03..0x3c06 = 0x00071620; RBBM_GBIF_CLIENT_QOS_CNTL 0x11 = 3; UCHE_WRITE_RANGE_MAX 0xE05/6,
   TRAP_BASE 0xE09/a, WRITE_THRU_BASE 0xE07/8 (Linux: trap base 0x1fffffffff000-ish, range max +0xfc0);
   UCHE_GMEM_RANGE_MIN 0xE0B = 1 MB, MAX 0xE0D = 1 MB + 132 KB - 1; UCHE_FILTER_CNTL 0xE18 = 0x804,
   UCHE_CACHE_WAYS 0xE17 = 4; CP_ROQ_THRESHOLDS_2 0x8c2 = 0x00800060, _1 0x8c1 = 0x40201b16;
   CP_MEM_POOL_SIZE 0x8C3 = 48; PC_DBG_ECO_CNTL 0x9e00 = 0x00080000; CP_AHB_CNTL 0x98d = 1;
   RBBM_INTERFACE_HANG_INT_CNTL 0x1f = (1<<30)|0x3ffff; UCHE_CLIENT_PF 0xe19 = 0x81. (HWCG, CP protect,
   UBWC skipped in G1.)
3. SQE: vendor `a630_sqe.fw` **without its first dword** (adreno_fw_create_bo copies fw+4) into a
   4 KB-aligned buffer below 4 GB → CP_SQE_INSTR_BASE 0x830/0x831.
4. Ring 32 KB below 4 GB → CP_RB_BASE 0x800/0x801; CP_RB_CNTL 0x802 = BUFSZ ilog2(32K/8)=12 |
   BLKSZ ilog2(32/8)=2 <<8 | NO_UPDATE (bit 27) = 0x0800020c. RPTR is read from CP_RB_RPTR 0x806,
   the host writes CP_RB_WPTR 0x807 (in dwords).
5. CP_SQE_CNTL 0x808 = 1.
6. Packets: PKT7(op, n) = 0x70000000 | n | PAR(n)<<15 | (op & 0x7f)<<16 | PAR(op)<<23,
   PAR(v) = (0x9669 >> (0xF & (v ^ v>>4 ^ v>>8 ^ … ^ v>>28))) & 1. CP_ME_INIT 0x48 ×8:
   0x2f, 3, 0x20000000, 0, 0, 0, 0, 0 → WPTR → wait RPTR == WPTR. Then CP_SET_SECURE_MODE 0x66 ×1: 0
   (needs the zap) → wait. Then CP_MEM_WRITE 0x3d ×3: addr lo, addr hi, 0xC0FFEE00 → wait → read
   the buffer from the CPU. Opcodes: NOP 0x10, WAIT_FOR_IDLE 0x26, EVENT_WRITE 0x46, WHERE_AM_I 0x62.
7. On a timeout log RBBM_STATUS 0x210, RBBM_INT_0_STATUS 0x201, CP_HW_FAULT 0x821, RPTR/WPTR, SMMU
   CB0 FSR/FAR (+0x58/+0x60) — a GPU memory access through the SMMU would show up there.

## G1 DONE — TopazGpu v0.6 (2026-10-02, commit ce2b611): the GPU executes commands
```
SMMU: SMR0 80010000 S2CR0 00000000 CBAR0 0001f000 CBA2R0 00000001 CB0 SCTLR 000000e0
SQE started: RBBM_STATUS 00c00015 CP_HW_FAULT 00000000 rptr 0
CP_ME_INIT: done (rptr 9) after ~10 us, RBBM_STATUS 00000001
CP_SET_SECURE_MODE 0: done (rptr 11) after ~10 us, RBBM_STATUS 00e41007
CP_MEM_WRITE: done (rptr 15) after ~10 us, RBBM_STATUS 00000001
test buffer: c0ffee00 (0xC0FFEE00 = the GPU wrote to memory)
[after test] SMMU gfsr 00000000 CB0 FSR 00000400 (FORMAT bits only, no fault)
```
- Power-up (v0.3) + zap via PAS 13 (v0.5) + identity SMMU CB0 for SID 0 mask 1 + hw_init subset +
  vendor SQE (32500 B after dropping the first dword) + 32 KB ring with NO_UPDATE + CP_ME_INIT +
  CP_SET_SECURE_MODE 0 + CP_MEM_WRITE: all work. GPU addresses = physical (buffers < 4 GB,
  MmNonCached contiguous).
- Everything is still one-shot via `C:\topaz\gpu.on`; the device stop path stops the SQE, powers
  the GPU down and frees the buffers.

## Next (G1.5 → G2)
- G1.5: IRQ (SPI 177 → GSIV 209; RBBM_INT_0_MASK 0x38, CP_EVENT_WRITE / CACHE_FLUSH_TS with IRQ),
  a real draw/blit from a freedreno-generated command stream (e.g. CP_BLIT / 2D clear into a buffer,
  then copy to the framebuffer at 0x5C000000) to prove rendering, HWCG + CP protect + UBWC from Linux.
- G2: WDDM render-only KMD (DXGKDDI for allocation, submit, fences on the ring above, IRQ, reset),
  TopazDisplay stays the display-only adapter.
- G3: Mesa d3d10umd + freedreno + WDDM winsys built for Windows ARM64 in CI.

## G2/G3 architecture (decided 2026-10-02)
Template found: **viogpu3d** (virtio-win PR #943, KMD branch `max8rr8/kvm-guest-drivers-windows`
`viogpu3d`, dir `viogpu/viogpu3d`, BSD-3, ~7000 lines: driver.cpp, viogpu_adapter.cpp,
viogpu_allocation.cpp, viogpu_command.cpp, viogpu_device.cpp, viogpu_vidpn.cpp) + Mesa branch
`max8rr8/mesa viogpu_win` = upstream d3d10umd + virgl gallium driver + a WDDM winsys
(`src/gallium/winsys/virgl/gdi/virgl_gdi_winsys.c`, 1124 lines) that talks to the KMD only through
the D3DKMT thunks: D3DKMTCreateAllocation / Lock / CreateContext / Render (cmd buffer + allocation
list + patch list) / Escape / QueryAdapterInfo. With it **DWM works** (glitches in WinUI3, VS Code
needs PIPE_QUERY_TIMESTAMP_DISJOINT). KMD: WDDM 1.3, one **aperture segment** (GPU reaches system
pages through a GPU-side mapping programmed from DxgkDdiBuildPagingBuffer), paging buffer in
segment 1, preemption disabled system-wide.
- Our version: **KMD** = viogpu3d structure with virtio replaced by the TopazGpu G1 code (power,
  zap, SQE, ring) + aperture mapping = **real SMMU stage-1 page tables** in CB0 (ARMv8 LPAE, 4 KB
  granule, TTBR0) instead of the identity bank + display = the fixed 1080×2400 framebuffer at
  0x5C000000 (replaces TopazDisplay; one adapter does render + scanout). **UMD** = Mesa 25.3.0 (last
  release with d3d10umd; removed on main) + freedreno gallium (A610 = GPUId(610) in
  freedreno_devices.py) + a new **WDDM backend for src/freedreno/drm** modelled on
  virgl_gdi_winsys.c, built for Windows ARM64 in CI (meson, d3d10 dll name e.g. `topazgpu_d3d10`).
- Local sources (Mac): `~/work-gpu/mesa` (25.3.0 sparse), `~/work-gpu/mesa-viogpu`,
  `~/work-gpu/viogpu-kmd`. s8build `~/work/gpu/` has firmware + vendor image.

### Memory/submit model (decided 2026-10-02)
freedreno a6xx needs **stable GPU VAs from BO creation** (softpin: texture descriptors and state
objects hold raw iovas written by the CPU), which WDDM 1.x patch lists cannot provide. So:
- Every fd_bo is a WDDM allocation (like virgl_gdi_winsys), but its **real backing and GPU VA are
  owned by our KMD**: DxgkDdiCreateAllocation allocates pages (MmAllocatePagesForMdlEx, CPU side
  write-combined), maps them into **our own SMMU stage-1 page tables** (CB0, ARMv8 LPAE, 4 KB
  granule, 39-bit VA, GPU VA < 4 GB for ADRENO_QUIRK_4GB_VA) at a KMD-chosen VA. VidMM only sees the
  allocation in one big aperture segment whose paging ops are no-ops.
- UMD↔KMD private interface = **DxgkDdiEscape mirroring the Linux msm DRM ioctls**: GET_PARAM
  (chip id 0x06010001, GMEM size/base, max freq, timestamp), GEM_INFO (iova, map into the calling
  process), GEM_CPU_PREP (wait idle for a BO), SUBMIT (IB list = iova+size, BO list, returns fence),
  WAIT_FENCE. Submission writes CP_INDIRECT_BUFFER packets into the kernel ring (G1 code) + a fence
  CP_MEM_WRITE / CP_EVENT_WRITE; completion first by polling, then the GPU IRQ (SPI 177).
- Mesa: new `src/freedreno/drm/wddm/` backend = copy of `msm/` with drmIoctl → gdikmt escape;
  `freedreno_ringbuffer_sp.c` (softpin) reused; target `d3d10umd` creates `fd_screen` on it.
- Present: DxgkDdiPresent/Blt from the source allocation's backing to the framebuffer (CPU copy first,
  GPU blit later). Flush waits for the fence at first (simple, correct), async later.

### WDDM plan in safe steps (decided 2026-10-02)
UMD side (mesa-overlay): freedreno's msm backend runs unchanged on a **libdrm shim over
DxgkDdiEscape** (`src/freedreno/drm/wddm/fd_wddm.c`, protocol `topazgpu_escape.h`: one escape = one
msm DRM ioctl with the Linux structs; extra VERSION / MMAP / MUNMAP). Sharing/present (later):
shared + display-target resources also get a WDDM allocation whose private data carries the global
BO name (KMD fills it in DxgkDdiGetStandardAllocationDriverData for runtime-created primaries);
`flush_frontbuffer` → gdikmt present (pfnPresentCb) like virgl_gdi_winsys.
- **Step A** — `TopazGpuW` (new WDDM KMD) installed as a **render-only second adapter**
  (no VidPN sources; TopazDisplay keeps the screen, so a broken build cannot black out the desktop;
  SSH is the safety net). KMD = GPU bring-up from TopazGpu G1 + SMMU LPAE page tables + the msm
  ioctls (VERSION, GET_PARAM, GEM_NEW/INFO/CLOSE/FLINK/OPEN, MMAP/MUNMAP into the calling process,
  GEM_CPU_PREP, GEM_MADVISE, SUBMITQUEUE_NEW/CLOSE/QUERY, GEM_SUBMIT → CP_INDIRECT_BUFFER on the
  kernel ring + fence, WAIT_FENCE). Tested with a small user-mode tool calling D3DKMTEscape (GEM_NEW
  → MMAP → write a CP_MEM_WRITE IB → SUBMIT → WAIT_FENCE → check).
- **Step B** — Mesa UMD (`topazgpu_d3d10.dll`) on that adapter: a D3D11 test app (FL 10.0/10.1)
  picks the adapter explicitly, renders into a texture, reads it back.
- **Step C** — make TopazGpuW the display adapter too (VidPN: one 1080×2400 mode, primary scanout
  by copying the primary allocation into the framebuffer 0x5C000000, later a GPU blit) and let DWM
  move onto the GPU (viogpu3d proved DWM works on d3d10umd).

### TopazGpuW install notes (2026-10-02)
- TopazDisplay is TDR-disabled (System event 4113 "stopped responding and was disabled") on **every
  boot** since its first install (events back to the 2022 clock) — pre-existing, not caused by
  TopazGpuW; the screen keeps working. Hot-adding a display-class device also triggers it.
- v0.1/v0.2 (child DDIs set, no VidPN DDIs): Dxgkrnl calls StartDevice then StopDevice at once, no
  QueryAdapterInfo. v0.3 (all display DDIs NULL): DxgkInitialize → c0000059. v0.4: the full DDI set
  with 0 sources (docs: "implement all DDIs ... but report 0 VidPN sources/targets").
- A driver update of a WDDM KMD needs a reboot (CM_PROB_NEED_RESTART), ask the user first.
