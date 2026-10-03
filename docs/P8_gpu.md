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
- **Root cause of the StartDevice → StopDevice loop** (DxgKrnl ETW trace: `logman start dxtrace -p
  Microsoft-Windows-DxgKrnl 0xFFFFFFFFFFFFFFFF 5 -o C:\topaz\dx.etl -ets`, restart the device,
  `logman stop`, `tracerpt -of XML`): "Adapter StartDevice has completed with status" 0xC0000034
  (OBJECT_NAME_NOT_FOUND) right after DpiReportAdapter → Dxgkrnl needs the UMD registry values
  (UserModeDriverName etc.) for a rendering adapter. v0.6 adds them in the INF. (LogConfigOverride
  IRQ in the INF was not applied to the root device: no LogConf key.)
- Note: when the phone's screen locks it sleeps and Wi-Fi (TopazWifi) pauses → SSH times out until
  the user unlocks.
- **v0.6/v0.7 (2026-10-02): the adapter STARTS** (UMD registry values fixed the c0000034; DRIVERCAPS
  must accept the 552-byte WDDM 1.3 struct). Dxgkrnl then: QueryAdapterInfo DRIVERCAPS ok →
  GetNodeMetadata → CreateDevice → CreateContext → QUERYSEGMENT ×2 → **BuildPagingBuffer ×2, but
  never SubmitCommand** → QueryAdapterInfo type 10 (not supported) → UMDRIVERPRIVATE ok. The first
  D3DKMTEscape from tgputest **never reached DxgkDdiEscape** (no log line) and the process became
  unkillable; a second test + ETW trace froze the whole phone (hard reboot). Hypothesis: Dxgkrnl's
  scheduler/VidMM waits for the two paging buffers to complete, holding the adapter lock that the
  escape needs — the root-enumerated device has **no interrupt** (LogConfigOverride ignored), so
  the usual DMA-completed path never runs. The device was removed afterwards; the phone is fine.
- Next (v0.8): boot/hang guard file (adapter refuses to start if the previous start never reached
  "stable"), log every scheduling DDI (BuildPagingBuffer operation, SubmitCommand fence, Preempt,
  QueryCurrentFence, ControlInterrupt, Dpc, Escape entry), and complete paging work without an IRQ.
- v0.9/v0.10: gave the root device the GPU IRQ (GSIV 209) through Enum\...\LogConf\BasicConfigVector
  written from the kernel (user-mode SYSTEM tasks/services could not start while the adapter was
  stuck). PnP assigned it ("Resources" tab), but then Dxgkrnl failed the start with c0000034 *before*
  calling DxgkDdiStartDevice → dropped in v0.11 (LogConf deleted). Without an IRQ the adapter starts
  but the scheduler never submits the 2 paging buffers.
- **The escape hang was my bug (found 2026-10-02):** "Escape: size 1040" reached TgEscape, then
  HwStart took a FAST_MUTEX (IRQL → APC_LEVEL; the logger silently skips writes there) and did
  synchronous file I/O (zap/SQE reads) at APC_LEVEL → the I/O completion APC can never run →
  deadlock, unkillable process, later system stalls. The DxgKrnl trace also shows the adapter is
  fine (StartDevice status 0; the 2 "paging buffers" are just Dxgkrnl mapping its paging-buffer
  allocations; processes try to open the missing UMD DLL). v0.12: KMUTEX instead of FAST_MUTEX.

## STEP A DONE — TopazGpuW v0.16 (2026-10-02): user mode → WDDM KMD → GPU
`tgputest` (D3DKMTEnumAdapters2 → our adapter by UMDRIVERPRIVATE magic → D3DKMTEscape msm ioctls):
VERSION msm 1.9; GET_PARAM gpu_id 610, gmem 0x21000, chip 0x06010001, max_freq 300 MHz, gmem_base
0x100000, VA 0x1000000+0xef000000; GEM_NEW ×2 + MMAP (user VAs), IB with CP_MEM_WRITE written by the
process, GEM_SUBMIT fence 1, WAIT_FENCE ok, **dst = 0xC0FFEE01**.
- Fixes on the way: KMUTEX instead of FAST_MUTEX (file I/O at APC_LEVEL deadlocked, v0.12); GPU
  power-cycle if a previous start left it on (v0.14); **identity SMMU** (v0.16, TGPU_IDENTITY=1):
  contiguous WC buffers below 4 GB, GPU VA = PA. The LPAE page-table path still faults on the first
  CP fetch (FSR 0x402 TF, FSYNR0 0x40 level 0, tables verified correct, also below 4 GB) — open item.
- Next: Step B = Mesa UMD (topazgpu_d3d10.dll) on this adapter.

## STEP B progress (2026-10-02, Mesa UMD on the phone)
- CI `mesa.yml` builds `topazgpu_d3d10.dll` (+ PDB, `-Ddebug=true`) since run 37006106960. Fixes:
  renderonly stubs in d3d10_gdi.c (renderonly.c needs libdrm, we pass ro=NULL), non-inline
  `trace_framebuffer_state` (clang-cl emitted no body for the `inline` definition), static zlib
  (`-Dzlib:default_library=static`; with z-1.dll the UMD failed LoadLibrary with error 126).
- Install: scp the DLL to `C:\Windows\System32\topazgpu_d3d10.dll` (INF already names it), run
  `C:\topaz\stage\TopazGpuW\d3dtest.exe` (60 s timeout script `C:\topaz\run.ps1`).
- d3dtest prints the crash stack (module+offset, unwound from the exception context). Symbolize:
  `/opt/homebrew/opt/llvm/bin/llvm-symbolizer --obj=topazgpu_d3d10.dll --relative-address --inlines <off-4>`
  with the PDB from the same CI run next to the DLL.
- First run: the UMD loads, freedreno talks to the KMD through escapes, then AV in
  `glsl_array_type` (fd6_context_create → fd_prog_init → tgsi_to_nir): the GLSL type singleton is
  created by the GL frontend on Linux; d3d10umd must call `glsl_type_singleton_init_or_ref()`.
- After the GLSL fix: **D3D11CreateDevice OK (FL 10.0) + CreateTexture2D OK**; then GEM_SUBMIT
  returned EFAULT: freedreno's `VOID2U64/U642VOID` cast through `unsigned long` (32-bit on Windows)
  and truncated the bos/cmds user pointers → fixed with uintptr_t (built, not yet tested).
- 15:49 the phone rebooted (Kernel-Power 41, cause unknown; a sound agent was also using the phone).
  After it TopazGpuW refused to start: the hang guard was only cleared when Dxgkrnl completed a DMA
  buffer, which never happens on this render-only adapter → **v0.17 clears the guard after 60 s
  alive**. v0.17 also adds the rest of Linux a6xx hw_init for a610: UBWC NC_MODE regs
  (RB/TPL1/SP 0x9, UCHE_MODE 0x800000), CP_MEM_POOL_DBG_ADDR 47, perf counter setup.
- To resume: delete `C:\topaz\gpuw.guard`, install v0.17 (WDDM update needs a reboot — ask the
  user), copy the latest UMD DLL, run `tools/gputest/run-d3dtest.ps1` (copy to C:\topaz\run.ps1).
- v0.18/v0.19 (2026-10-02 18:xx): UMD opt-in (`TOPAZGPU_ENABLE`), KMD fail-fast "wedged" + hang
  dump. **BSOD 0xCE** while updating v0.17→v0.18: the guard thread slept inside the image and the
  update unloaded it → v0.19 signals/awaits it in StopDevice/Unload. Install rule: certutil the new
  CI cert first (each run re-signs), update only while no old-style guard thread runs.
- v0.20: a610 HWCG table (Linux a612_hwcg, RBBM_CLOCK_CNTL 0xaaa8aa82) — no change in the hang.
- **First GPU work through D3D11: `d3dtest copy` PASSES** (texture with initial data →
  CopyResource → staging → ff4080ff): freedreno's blitter runs on the Adreno via our KMD.
- `d3dtest clear` (3D draw with shaders) hangs with any FD_MESA_DEBUG (sysmem, noubwc): RBBM_STATUS
  00f715a5 = CP, PC_DCALL, VPC, UCHE, SP, VSC, HLSQ, TSE, RB, CCU, LRZ busy; no CP/SMMU fault;
  IB1/IB2 fully fetched (rem 0). → the shader pipeline does not run. Test script `t.ps1 <clear|copy>
  [FD_MESA_DEBUG]` (restarts the adapter first: a wedged GPU stays wedged until restart).
- v0.21: SPTPRAC through the GMU wrapper (0x596A000 + 4*0x50d0, as Linux does for A619 holi) →
  **bugcheck 0x101**: reading that region hangs the bus on khaje. Never touch 0x596A000+ again.
  v0.22 = v0.20 behaviour.
- **Half-dead system after tests (19:25, and once before):** taskbar/explorer, "This PC", Task
  Manager, network, sshd hang while the desktop works. Cause (most likely): `devcon restart/update`
  of TopazGpuW waits for DWM to release the adapter and holds the PnP lock meanwhile. Earlier a
  disable only completed after killing dwm. → v0.25 recovers a wedged GPU inside the KMD on the next
  submit (power-cycle, zap, SMMU, CP; earlier fences marked complete); `t.ps1` no longer restarts the
  device. KMD updates still need a stop: run devcon in the background, kill dwm if it is stuck >10 s.
- Tried without effect on the draw hang: linux-firmware SQE (34188 B, on the phone as
  `C:\topaz\fw\gpu\a630_sqe.linux.fw`, vendor copy `a630_sqe.vendor.fw`; the active file is the linux
  one), FD_MESA_DEBUG direct / nofp16 / sysmem / nolrz / noubwc. KGSL a6xx_start (LineageOS sm6150
  tree) matches our init. Markers (v0.23): hang inside the draw (CP_SCRATCH7 = before-draw marker),
  RBBM_STATUS 00e70585 = RB, CCU, LRZ, VPC, UCHE, SP, HLSQ busy; RAS/TSE/PC idle.
- Next experiment (v0.24+): GMEM size override `C:\topaz\gpu.gmem` (hex) — CCU's sysmem cache is
  placed at the end of GMEM by Mesa; a smaller khaje GMEM would wedge RB/CCU.

## 3D HANG SOLVED (2026-10-02 evening): MSVC-ABI signed enum bitfields
- Root cause found with an `.rd` capture (`FD_RD_DUMP=enable`, files land in `C:\tmp`) decoded by
  `cffdump` (built on s8build: `~/work/gpu/mesa-tools/build-tools/src/freedreno/decode/cffdump`):
  every 3D draw wrote **0xffffffff into RB_DEPTH_PLANE_CNTL / GRAS_SU_DEPTH_PLANE_CNTL**.
  `struct fd6_lrz_state` has `enum a6xx_ztest_mode z_mode : 2`; under the MSVC ABI (clang-cl)
  enum bitfields are *signed*, so A6XX_INVALID_ZTEST (3) reads back as -1, passes the
  `!= A6XX_INVALID_ZTEST` check and is emitted. Also bool/enum bitfields are not packed together on
  MSVC, so `val:8` did not overlay direction/z_mode. Fix: uint32_t bitfields + casts at the register
  writes; other enum bitfields whose values do not fit a signed field got +1 bit
  (freedreno_resource.h lrz_direction, ir3 tess spacing, shader_info depth/stencil layout,
  derivative_group).
- Result: no more GPU hangs; **`d3dtest clear` = ff4080ff** (u_blitter 3D draw with shaders),
  copy OK. `tri` (own VS/PS via d3d10umd DXBC→TGSI→NIR) completes but leaves 0 — next to debug.
- Other UMD fixes on the way: tgsi_to_nir RET (d3d10umd ends every main with RET), no depth
  tracking without zsbuf, empty vertex elements for draws without an input layout.

## STEP B DONE (2026-10-02 ~21:00): D3D11 draws with real HLSL shaders on the Adreno
`d3dtest tri` (vs_4_0/ps_4_0 compiled by d3dcompiler_47 → DXBC → d3d10umd TGSI → NIR → ir3),
`tricull`, `clear`, `copy`: all ff4080ff. UMD fixes after the enum-bitfield one:
- d3d10umd: depth/stencil state variant with both off while no DSV is bound (D3D semantics);
- tgsi_to_nir: VERTEXID_NOBASE → load_vertex_id (freedreno feeds only SYSTEM_VALUE_VERTEX_ID;
  REGID4VTX was r63). DrawIndexed with BaseVertexLocation != 0 gets index+base (D3D: index) — TODO.
- Known TODOs in the UMD: nested TGSI RET ignored; shader_info/glsl enum bitfields widened, other
  enum bitfields in gallium headers checked (values fit).
Next = **step C**: TopazGpuW as the display adapter (VidPN, primary scanout copy to 0x5C000000) and
DWM on the GPU; before that: present path (DxgkDdiPresent/flush_frontbuffer), lift the
TOPAZGPU_ENABLE opt-in only when DWM-level apps work (test with a windowed D3D11 app first).

## Step C1 (2026-10-02 late): Dxgkrnl scheduler + ACPI GPU device
- `tgputest render` (D3DKMTRender ×20 on the root-enumerated adapter): Dxgkrnl built 2 paging
  buffers, **never called SubmitCommand**, TDR after 2 s → **bugcheck 0x116** (recovery failed).
- Our UEFI DSDT now has `GPU0` (_HID TPZG0610, Memory32Fixed 0x5900000/0x90000, Interrupt GSIV 209,
  uefi/acpi/tapas-DSDT-xhci.dsl). Built from the s8build tree as
  `~/work/win/uefi/Mu-topaz-v4-GPU0-RELEASE.img` (sha256 1da973fc…), **RAM-boot only**
  (`fastboot boot`); boot_b = `~/work/topaz-poweroff.img` (verified by hashing PhysicalDrive4
  partition 35 from Windows), not touched. Windows enumerates ACPI\TPZG0610\0 with both resources;
  TopazGpuW.inf has the ACPI id; Root\TopazGpuW was removed (`devcon remove`, no DWM stall).
- First start on the ACPI device: StartDevice OK (no c0000034 any more), 4× BuildPagingBuffer, still
  no SubmitCommand, then StopDevice → CM_PROB_FAILED_POST_START. **The screen went black** at that
  moment (BasicDisplay still OK, DWM restart did not help) → hard reboot. After the reboot Wi-Fi had
  no networks until one more normal reboot (modem).
- Next: DxgKrnl ETW trace of the ACPI adapter start to see the post-start failure; make
  BuildPagingBuffer emit real (non-empty) DMA content; find out why VidSch never submits.
  Note: after the flashed-UEFI reboot neither TopazGpuW instance exists (root removed, no GPU0).
- v0.26 ring logger (all IRQLs) + v0.27 HISTORYBUFFERPRECISION: still no SubmitCommand; the two
  paging buffers (MAP_APERTURE_SEGMENT, op 5) are empty and VidSch completes them by itself (ETW
  PacketType 2 completed, no timeout). Earlier "invalid NTSTATUS 0xC00000BB" came from QAI type 10.
- **Why the ACPI adapter fails** (DxgKrnl ETW, 2026-10-02 21:06): during StartDevice Dxgkrnl looks
  for the adapter's internal display ("Failed to get display config buffer sizes when looking for
  internal target/priority", "Can't reference adapter by luid"), StartDevice completes with
  **0xC01E0004 STATUS_GRAPHICS_ADAPTER_WAS_RESET**, then "The selected adapter is display only
  adapter" + StopDevice. An ACPI-enumerated display adapter is treated as the platform's integrated
  GPU that owns the panel; 0 VidPN sources is not accepted there (the root device was accepted as
  render-only). The first black screen = Windows starting to hand the panel over to it.
- **Plan C2 (needed now):** TopazGpuW = full WDDM display+render adapter on ACPI\TPZG0610: VidPN
  from TopazDisplay (bdd_dmm.cxx, one 1080x2400 XRGB8888 source/target, panel at 0x5C000000),
  standard allocations (shared primary / shadow / staging via GetStandardAllocationDriverData),
  SetVidPnSourceAddress + Present/Blt (CPU copy to the framebuffer first), DMA completion through
  DxgkCbSynchronizeExecution (works now: real interrupt object), VSync from a 60 Hz timer through
  SynchronizeExecution(NotifyInterrupt CRTC_VSYNC), StopDeviceAndReleasePostDisplayOwnership /
  takeover from BasicDisplay. The ACPI adapter is left disabled (`devcon disable @ACPI\TPZG0610\0`)
  until then; RAM-boot `Mu-topaz-v4-GPU0-RELEASE.img` for every test.

- **C2 update after the brightness work (2026-10-02 23:10):** the phone now boots with TopazDisplay v0.6 (root KMDOD)
  as the only display adapter, BasicDisplay disabled by `ConfigFlags=1`, monitor `DISPLAY\TPZ6225` with
  `BrightnessControl=1`. Consequences for C2: (1) the takeover is from TopazDisplay, not BasicDisplay — the ACPI
  GPU adapter is a POST device, so dxgkrnl should prefer it, but TopazDisplay must not keep scanning out the same
  framebuffer: disable it by registry (`ConfigFlags=1` on `ROOT\DISPLAY\0000`) for C2 boots, never hot; (2) the
  brightness interface (`topaz_bl.cxx`) must move into TopazGpuW (QueryInterface GUID_DEVINTERFACE_BRIGHTNESS), and
  as a POST adapter it may report the panel as D3DKMDT_VOT_INTERNAL; (3) recovery for a dead C2 boot = boot the
  flashed UEFI (no GPU0 device) → TopazDisplay comes back on its own; if the logo freezes there, the recovery in
  NOTES_brightness.md SUMMARY.

## Step C2 code (2026-10-02 night): TopazGpuW v0.28 = display adapter, TopazDisplay v0.8 yields
- Model taken from viogpu3d (github max8rr8/kvm-guest-drivers-windows branch viogpu3d, the KMD that
  the same Mesa d3d10umd/gdikmt branch was written for): one aperture segment, allocations with
  private data, MAP/UNMAP_APERTURE_SEGMENT handled inside BuildPagingBuffer, MMIO flips
  (FlipOnVSyncMmIo, MaxQueuedFlipOnVSync 1) and a 16.6 ms vsync reported as CRTC_VSYNC.
- TopazGpuW v0.28: `disp.c` (POST framebuffer via DxgkCbAcquirePostDisplayOwnership, fallback
  0x5C000000; 1 source/1 child INTERNAL + AlwaysConnected, `C:\topaz\gpuw.cfg` "<vot> <hpd>"
  overrides; EDID like TopazDisplay; VidPN cofunc/commit; vsync timer DPC -> SynchronizeExecution
  -> NotifyInterrupt; brightness interface (DCS 0x51 via DSI0 DMA, as topaz_bl.cxx);
  StopDeviceAndReleasePostDisplayOwnership, SystemDisplayEnable/Write), `eng.c` (CPU engine: TG_CMD
  records in the DMA-buffer private data, SubmitCommand queues them, a passive thread executes
  blt/colorfill/paging fill and completes the fence; it also copies the scanned-out primary into
  the framebuffer when SetVidPnSourceAddress changes it). Allocations: struct topazgpu_alloc in
  the private data (`topazgpu_escape.h`): UMD allocations name a BO (pixels = BO kernel VA),
  standard ones (shared primary/shadow/staging/GDI) use VidMM's aperture backing mapped at
  MAP_APERTURE_SEGMENT. Non-empty DMA buffers (4 bytes) so VidSch really calls SubmitCommand.
  No preemption (PreemptionAware 0).
- TopazDisplay v0.8: if HKLM\HARDWARE\ACPI\DSDT contains "TPZG0610" (RAM-booted GPU0 image) its
  monitor is reported disconnected -> TopazGpuW alone owns the panel; flashed UEFI (no GPU0) =
  unchanged. `C:\topaz\td.keep` disables the check. Verified: the flashed DSDT does not contain it.
- Expected first result with the UMD still opt-in (TOPAZGPU_ENABLE): DWM cannot open our UMD; we
  will see whether it falls back to WARP + our Present/Blt path (staging surfaces) or fails.
- Installed 23:34 (CI run 37061218051) by file swap: `C:\Windows\System32\drivers\TopazGpuW.sys` (v0.28)
  and `TopazDisplay.sys` (v0.8), old files kept as `*.sys.old-233432` (both services load from
  System32\drivers; the .sys files are embedded-signed, CI cert added to Root/TrustedPublisher).
  `Enum\ACPI\TPZG0610\0` is enabled (ConfigFlags 0, service TopazGpuW). Rollback: rename the
  .old files back (from Windows over SSH, or offline from TWRP) and reboot.
- Test boot: `fastboot boot ~/work/win/uefi/Mu-topaz-v4-GPU0-RELEASE.img` on s8build (sha256
  1da973fc...), never flash. Then read C:\TopazGpuW.log, C:\TopazDisplay.log, boot ETW
  `C:\topaz\boot.etl.001` (autologger TopazDxg) over SSH.

### First C2 boot (2026-10-02 23:39, GPU0 image RAM-booted)
- ACPI\TPZG0610\0 came up with problem 22 (disabled in an earlier session; ConfigFlags 0 in Enum did
  not reflect it) -> `pnputil /enable-device "ACPI\TPZG0610\0"` started it hot without trouble.
- v0.28: DxgkCbAcquirePostDisplayOwnership returned the real GOP info (1080x2400 pitch 4320 fmt 22 pa
  0x5C000000); StartDevice OK; INTERNAL child accepted -> monitor "Integrated Monitor (Topaz Panel)";
  brightness interface queried and SetBrightness(100) done.
- TopazDisplay v0.8 detected GPU0 and reported its monitor disconnected -> no desktop on it (good) but
  also none on ours: the panel kept the boot logo.
- Dxgkrnl looped "create shared primary 1080x2400 -> CommitVidPn with 0 paths -> visibility 0" and
  never paged the primary in (no MAP_APERTURE_SEGMENT for it). CCD from the user session
  (QueryDisplayConfig ALL_PATHS: our target tech 0x80000000 available 1; SetDisplayConfig
  TOPOLOGY_INTERNAL -> 1610, EXTEND -> 31) without any DDI reaching the driver.
- Suspect: viogpu3d sets `MemoryManagementCaps.SectionBackedPrimary` - a primary in an aperture
  segment needs it. v0.29 sets it and logs IsSupportedVidPn/EnumCofunc/RecommendMonitorModes.
- Tools on the phone: `C:\topaz\ccd.ps1` (QueryDisplayConfig/SetDisplayConfig, run as the console
  user via the scheduled task TopazCcd, output C:\topaz\ccd.txt). DxgKrnl ETW gave no events in
  this boot (session started fine, 0 events) - to check.

### v0.29 result (23:53, hot restart of ACPI\TPZG0610\0 worked, no PnP hang)
- SectionBackedPrimary fixed the mode set: IsSupportedVidPn/EnumCofunc/RecommendMonitorModes, then
  CommitVidPn 1 path 1080x2400 stride 4320 fmt 22, SetVidPnSourceAddress seg 1 pa 0xC0000000,
  visibility 1, flip completed by the timer vsync, first scanout copy of the primary.
- dwm.exe crashes (dwmcore.dll, 0x8898008d) - our UMD is opt-in, DWM gets no D3D device; the desktop
  is then drawn by CDD: Present Blt from a shadow surface (standard alloc type 2) to the primary.
- The shadow surface had no CPU view: VidMM mapped it with MAP_APERTURE_SEGMENT hAllocation == NULL.
  v0.30: own aperture page table (PFN per aperture page from every MAP/UNMAP), DxgkDdiPatch writes the
  final segment/address of the present source/destination into the TG_CMD, the engine maps
  aperture ranges on demand (GartMapVa).

### v0.30 result (00:00, after a fastboot RAM boot - a hot restart of the adapter no longer completes
once it scans out: PnP waits for CDD/DWM even with dwm killed; new .sys only via reboot)
- Patch works (dst seg 1 0xC0000000), but CDD's shadow surface (standard type 2) stays in segment 0
  (system memory, never mapped into the aperture): source seg 0 address 0 -> nothing to copy. CDD
  presents small dirty rects (e.g. (519,1485)-(562,1530) = the spinner).
- v0.31: Present writes patch locations for source and destination (viogpu3d does), expecting VidMM
  to make both resident in the aperture.

### v0.31 result (00:04 boot): presents reach the panel
- With the patch locations VidMM maps CDD's shadow surface into the aperture (MAP_APERTURE_SEGMENT
  with its hAllocation, 0x9e4 pages at aperture page 0x9f4); Patch: src seg 1 0xC09F4000, dst seg 1
  0xC0000000; blts copy the dirty rects (boot spinner) into the primary and to the panel.

### v0.31 on screen + next: DWM on the UMD (KMD v0.32, Mesa patch 0002)
- User photo (00:08): black screen with the CDD-drawn bits only - taskbar line, ShareX tray icon,
  keyboard icon, mouse cursor. So POST takeover + mode set + CPU present engine + scanout copy work.
  The rest stays black because dwm.exe crash-loops (dwmcore 0x8898008d): the UMD refuses DWM (opt-in)
  and CDD only presents dirty rects.
- Mesa patch `mesa-overlay/patches/0002-topazgpu-wddm-present-shared.patch` (new
  src/gallium/frontends/d3d10umd/topaz_wddm.cpp): resources with pPrimaryDesc / MISC_SHARED /
  BIND_PRESENT are created linear and get a WDDM allocation (pfnAllocateCb, private data =
  topazgpu_alloc with the BO flink name from resource_get_handle(SHARED)); Present = flush + fence wait
  + pfnPresentCb(hSrcAllocation) on a lazily created context; SetDisplayMode uses the allocation;
  RotateResourceIdentities rotates the allocation handles too; OpenResource imports the BO by name;
  DestroyResource deallocates. Opt-in now also by the file `C:\topaz\umd.enable` (DWM has no env).
- KMD v0.32: GDI texture surfaces (standard GDISURFACE, type TEXTURE: window redirection bitmaps that
  CDD fills with Present blts) are backed by a KMD-created BO and the BO name is written back into the
  allocation private data, so DWM can open them as textures on the Adreno.
- Known gaps: R8G8B8A8 sources are copied without R/B swap; GDI CPU-visible types still live in
  VidMM memory (not importable by the UMD); every present waits for the GPU (no async yet).

### DWM on the UMD, first try (00:28, KMD still v0.31, UMD with patch 0002, C:\topaz\umd.enable)
- dwm.exe now loads topazgpu_d3d10.dll and dies in it: 0xc0000409 = abort(). WER LocalDumps for
  dwm.exe (C:\topaz\dumps, DumpType 1) + own ARM64 minidump stack scan (scratch md.py) + llvm-symbolizer
  with the CI PDB: Shader_tgsi_translate -> tgsi_to_nir "unknown TGSI opcode: SAMPLE" -> abort().
  d3d10umd translates SM4 sample instructions to TGSI SAMPLE/SAMPLE_B/_L/_C/_C_LZ/_D/_I (explicit SVIEW +
  SAMPLER registers) which tgsi_to_nir never supported - our step-B tests had no textures.
- Patch 0002 now also: ttn_sample() in tgsi_to_nir (target from the SVIEW declaration, texture_index =
  view, sampler_index = sampler, lod/bias/compare from src[3], ddx/ddy src[3]/src[4], txl lod 0 for
  implicit-lod samples outside the FS, ld -> txf/txf_ms), SVIEW sources ignored like SAMPLER ones,
  unknown opcodes -> warning + 0 instead of abort(). SVIEWINFO (resinfo) still unsupported.
- Next DWM crash (0xc0000005): fd_set_sampler_views -> pipe_sampler_view_reference. d3d10umd always
  passes PIPE_MAX_SHADER_SAMPLER_VIEWS (128) views, freedreno's fd_texture_stateobj has
  textures[PIPE_MAX_SAMPLERS = 32] -> overflow. Fixed in 0002 (32 views). Dump tip: a dump written right
  after a DLL swap may still come from a process that loaded the old DLL - check the module
  timestamp in the WER event before symbolizing with the new PDB.
- UMD file log: `C:\ProgramData\topaz\umd.log.enable` -> stderr/DebugPrintf of every UMD process in
  `C:\ProgramData\topaz\umd-<pid>.log` (dir granted to Everyone, DWM runs as DWM-n); `umd.tgsi` also
  dumps each translated shader. Helper flow (scratch umdloop.sh): install DLL, kill dwm, wait for the
  WER dump, symbolize pc/lr + stack scan with the CI PDB.
- After the sampler fix DWM died differently each time (fd_bo_heap_alloc -> fd_bo_ref NULL in
  CreateDevice, heap exhaustion): every crashed DWM leaked its BOs in the KMD (handles are global,
  nobody closed them) until contiguous memory ran out. KMD v0.33: BO references are owned by the WDDM
  device that made the escape (GEM_NEW/GEM_OPEN; GEM_CLOSE drops one) and released in
  DxgkDdiDestroyDevice (also called when the process dies); GEM_NEW failures are logged with totals.

### 01:05 boot (KMD v0.33): DWM gets through shader creation, then fail-fasts
- dwmcore raises 0xc00001ad (FATAL_MEMORY_EXHAUSTION) on E_OUTOFMEMORY from D3D. UMD log with SetError
  file:line + CreateResource params: the DWM swapchain buffer (1080x2400 B8G8R8A8, bind
  PRESENT|RT|SRV) failed `is_format_supported` because fd6 refuses any bind bit it does not confirm
  (LINEAR/SHARED added by us; MISC_SHARED resources had the same problem before). Fixed: format check
  without LINEAR/SHARED/SCANOUT.
- Next (01:18): DWM creates its primaries (pPrimaryDesc, MISC 0x20000), opens a shared resource and
  creates textures; then AV in d3d10_fd_escape from munmap: freedreno never closes the fd it was given,
  so the fd_wddm slot of a destroyed D3D device kept a dangling escape context and munmap (which tries
  every open slot) called into it. Fix: fd_wddm_detach(ctx) in DestroyDevice after the screen is gone,
  slot table under an SRW lock (DWM creates devices from several threads).

### 01:24 boot: DWM alive on the UMD, black screen, then BSOD 0x14F
- With the fd fix DWM stays up (no more dumps) but the screen stays black: DWM loops ~350 times
  through "create devices -> visibility 1 -> create primaries (1080x2400 BGRA, MISC 0x20000) -> visibility
  0 -> primary destroyed while scanned out -> devices destroyed" without any SetError: it treats the
  device as lost. Suspect: DXGI Present with hDstResource (blt to DWM's primary) went to pfnPresentCb
  without hDstAllocation (gdikmt's present() never passes one). Mesa 0002 now calls pfnPresentCb
  itself with src+dst allocations and logs Present/SetDisplayMode/Rotate/Blt results.
- After the user left the phone, BSOD 0x14F PDC_WATCHDOG_TIMEOUT (1, 1, ...) at the screen-off ->
  Modern Standby transition (never tested on this port). Monitor/standby timeouts set to 0 on the phone
  (powercfg) for the tests; real fix later (TgSetPowerState / monitor power do nothing yet).

### 02:12 boot + state at the pause (2026-10-03 ~02:25)
- UMD log: DWM's SetDisplayMode(primary) -> S_OK, then every DXGI Present with Flags.Flip (0x2, no
  destination) -> pfnPresentCb returns E_FAIL (0x80004005) -> DWM drops the device and starts over
  (that is the ~350x loop). The KMD never saw a flip Present in the (count-limited) log, so Dxgkrnl
  rejects it before DxgkDdiPresent. Unknown why: to find with DxgKrnl ETW + v0.34 logging.
- DxgKrnl ETW sessions were empty because the brightness-era autologger `TopazDxg` held the provider;
  the autologger key is deleted now (02:20). Stopping it with `logman stop TopazDxg -ets` while DWM
  looped froze the phone (user reset).
- Ideas for the flip E_FAIL: (1) the flip goes through our own context (lazy pfnCreateContextCb), not
  the D3D device's; (2) DescribeAllocation reports fmt 21 for primaries while the mode is 22 (X8R8G8B8);
  (3) KMD Present for flips: viogpu3d returns success without touching the DMA buffer; (4) primaries
  are created linear in a BO and VidMM backing is ignored (SectionBackedPrimary). A fallback that
  avoids flips entirely: report DXGI_DDI_PRIMARY_DRIVER_FLAG_NO_SCANOUT in CreateResource for
  pPrimaryDesc so DXGI uses blt presents.
- Phone state: normal boot (flashed UEFI) works with TopazDisplay v0.8 (ROOT\DISPLAY\0000 OK).
  Staged for GPU0 boots: TopazGpuW v0.34 (.sys swapped), UMD with patch 0002 (System32), files
  C:\topaz\umd.enable and C:\ProgramData\topaz\umd.log.enable (UMD log), WER LocalDumps for dwm.exe
  (C:\topaz\dumps, DumpType 1), power timeouts 0 (powercfg). To make GPU0 boots show the CDD desktop
  again without DWM-on-UMD: delete C:\topaz\umd.enable.
- Next session: fastboot boot the GPU0 image, `logman create trace` DxgKrnl for a few seconds while
  DWM loops, look for the Present failure; read C:\TopazGpuW.log (flip presents) and
  C:\ProgramData\topaz\umd-<pid>.log.

### 08:46 boot (KMD v0.34): why DWM's flips fail
- DxgKrnl ETW works after removing the TopazDxg autologger (`tools`: C:\topaz\tr4.ps1 = 4-6 s trace;
  decode with Get-WinEvent -Path + task names from Get-WinEvent -ListProvider; event 494
  "AzureTriage" carries validation texts).
- Per DWM cycle: "KMD should set a non-zero initial priority for allocations" (every allocation) and
  "When opening a synchronization object, the NoGPUAccess flag specified at open time must match the
  flag specified at creation time" (DWM opens a fence of its first device on its D3D device).
  No Present event at all: the flip present fails inside the user-mode runtime (pfnPresentCb ->
  E_FAIL). Own present context (EngineAffinity 1) did not change it. No-flip mode (NO_SCANOUT
  primaries) is useless: DWM never retries with PRIMARY_OPTIONAL (it gets DXGI_DDI_ERR_UNSUPPORTED
  forever) - `umd.noflip` removed.
- viogpu3d (WDDM 1.3, same UMD model) runs DWM on 22621. Differences in our KMD: allocation priority 0,
  no SupportDirectFlip / segment DirectFlip, PreemptionAware 0, EvictionSegmentSet 0. v0.35 copies
  viogpu3d (and the phone has GraphicsDrivers\Scheduler EnablePreemption=0 like their setup).

### Flip failure SOLVED in theory (09:20): our own STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER
- Dwm-Core/DXGI/Direct3D11 ETW (C:\topaz\tr5.ps1): the D3D11 runtime journal (event 1820) says
  "Present -1071775743" = 0xC01E0001 = STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER (not ..._NOT_EXCLUSIVE_
  MODE_OWNER, that one is 0xC01E0000 - checked in the dxgkrnl disassembly on s8build ~/work/dxgre), and
  DxgKrnl ETW Present/win:Info carries the same status. TgPresent returned it for flips because Dxgkrnl
  gives flip presents no DMA private data; the check ran before the log line, so the KMD log never
  showed a flip. v0.36: Flags.Flip -> STATUS_SUCCESS without touching the buffers (viogpu3d does the same;
  the flip itself is SetVidPnSourceAddress at the next vsync).
- Also from the journal: UMD pfnDeallocateCb of resource-bound allocations -> STATUS_INVALID_PARAMETER;
  the runtime frees those itself (UMD no longer deallocates).

## STEP C2 MILESTONE (2026-10-03 09:30): Windows desktop composed by DWM on the Adreno 610
- KMD v0.36 + Mesa patch 0002 (RAM-booted GPU0 image): DWM's flips go through (DXGI Present flags 0x2
  -> S_OK, 336 flip presents in the KMD log within a minute, DWM pid stable). The user sees the
  desktop on the panel: DWM renders with d3d10umd/freedreno on the Adreno 610, the KMD flips by
  copying the BO-backed primary into the GOP framebuffer.
- Chain that works now: ACPI GPU0 POST adapter -> VidPN/mode set -> UMD primaries/swapchain as
  WDDM allocations naming freedreno BOs -> DXGI flip present (pfnPresentCb) -> DxgkDdiPresent(Flip)
  no-op -> SetVidPnSourceAddress -> timer vsync -> CPU copy BO -> framebuffer; brightness via the
  same adapter.
- Known limits / next: CPU copy of every flipped frame from write-combined BOs (slow; next: scan out
  the BO directly by programming the MDP SSPP address), present waits for the GPU fence, R/B order of
  RGBA primaries, Modern Standby (PDC watchdog 0x14F) untested/broken, preemption disabled
  system-wide (GraphicsDrivers\Scheduler EnablePreemption=0), UMD still opt-in by C:\topaz\umd.enable,
  only RAM-boot UEFI has GPU0.
