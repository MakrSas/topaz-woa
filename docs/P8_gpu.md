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

### After the milestone: user feedback + plan (09:40)
- Photos: desktop, taskbar, Start menu with acrylic blur rendered by DWM on the GPU. Laggy. Artifacts:
  Start icon disappears, search icon only partly drawn, Task Manager window partly drawn with a white
  area (probably surfaces DWM opens from other processes / GDI surfaces the UMD cannot import yet).
  Faint ghost icons on the right edge are most likely AMOLED burn-in from Android, not ours.
  Rotation: only portrait (no path rotation support reported; auto-rotate needs an accelerometer driver
  on the SSC/ADSP).
- Next: direct scanout (MDP SSPP fetch from the flipped BO). Stock DT: MDP 0x5E00000 (sde-off 0x1000),
  SSPP VIG0 +0x5000, DMA0 +0x25000 (only vig + dma on khaje), CTL0 +0x2000, LM0 +0x45000, INTF1
  +0x6b800; MDP SMMU stream IDs 0x420 (mask 0x2, unsecure) and 0x421 (secure) on the apps SMMU
  0x0C600000. v0.37 logs (read-only) the SSPP/CTL/LM/INTF registers and the apps SMMU SMR/S2CR/CB
  entries of these SIDs at start, to see which pipe fetches the GOP buffer and whether MDP fetches are
  translated.
- Stale frames (09:38-09:39 photos: a zoomed "Управление компьютером" open-animation frame stays at the
  bottom, disappears and comes back): d3d10umd's RotateResourceIdentities rotated the pipe_resource
  pointers, but the render-target views keep their pipe_resource, so DWM drew into a different buffer
  than the one it presented (viogpu3d has the same code and "rendering glitches"). Patch 0002 adds
  fd_resource_rotate_storage() in freedreno_resource.c (rotates bo/valid/layout, invalidates batch
  cache + rebinds like the shadow swap, new seqno) and calls it after a flush; hAllocation follows its BO.
- Experiments with C:\ProgramData\topaz\umd.env (Mesa env for DWM, read by the UMD): FD_MESA_DEBUG=sysmem
  -> same stale regions (not GMEM). TOPAZ_NOROTATE=1 (no rotation, always the same buffer flipped) ->
  everything black except what DWM redraws in that frame (touch trail, selection), with gmem and with
  sysmem. Stale regions vanish when touched (DWM's touch feedback redraws them). No DXGI pfnBlt calls.
  Reading: DWM copies the undamaged area forward from what it believes is the previous frame's buffer;
  with no rotation that is a never-rendered (black) buffer, with our rotation an older frame -> the
  rotation direction is probably reversed. Switch TOPAZ_ROTATE_REVERSE=1 added to test.

### Stale regions FIXED (10:15) + state
- Logged sequence (default direction, storage rotation): DWM renders into the resource at rotate index
  [1] and presents it; before drawing it copies the undamaged area forward with ResourceCopyRegion from
  index [0] (the previously presented buffer). Allocations rotate a->b->c consistently with the BOs.
  With fd_resource_rotate_storage (default direction) + this build the user reports: no stale
  remnants any more. Earlier "not better" results came from tests mixed with the pointer-rotation
  build / experiment switches; TOPAZ_NOROTATE and TOPAZ_ROTATE_REVERSE are wrong (black / partial) and
  stay only as switches. umd.env on the phone is back to the default (no switches).
- Still open (user, 10:15): Start button icon missing and the search icon partly drawn (likely a
  rendering bug in Mesa for those taskbar elements: next to look at with an isolated test), slight
  tearing (we copy the scanned-out buffer with the CPU while DWM can already render the next frame into
  the same pages? no - tearing is the 10 MB CPU copy racing the panel scanout), low FPS (~20: CPU copy of
  every flip from write-combined BOs + a GPU fence wait per present).
- Next steps, in order: (1) direct scanout via MDP (KMD v0.37 with the read-only MDP/SMMU survey is
  installed and runs at the next GPU0 boot), (2) asynchronous present (no fence wait), (3) taskbar
  icon rendering bug, (4) path rotation (portrait only now), (5) Modern Standby.

### MDP survey (10:21 boot, v0.37) -> direct scanout (v0.38)
- MDP hw rev 0x600a0000. VIG0 (+0x5000): src_size 0x09600438 (2400x1080), SRC0_ADDR 0x5C000000,
  YSTRIDE0 0x10e0 (4320), format 0x000237ff, unpack 0x03020001; DMA0 unused. CTL0 layer0 0x01000002
  (VIG0 on LM0 stage), intf_active 2 (INTF1). LM0 out 2400x1080. INTF1 timing_en 1.
- apps SMMU 0x0C600000: 50 SMRs / 48 CBs, 4K pages, numpage 64. SMR4 id 0x420 mask 0x2 -> S2CR translate
  CB3, but CB3 SCTLR 0xe0 (M = 0: translation off) -> MDP fetches physical addresses directly.
- v0.38: flips to BO-backed full-screen primaries write VIG0 SRC0_ADDR = BO PA (identity GPU mapping,
  contiguous, < 4 GB) and YSTRIDE0 = BO pitch (4352), then CTL0_FLUSH = VIG0 (bit 0) | CTL (bit 17);
  other primaries / stop / bugcheck screen / destroyed primary -> back to 0x5C000000 stride 4320 + CPU
  copy. Kill switch C:\topaz\gpuw.nodirect. Direct mode is only enabled when VIG0 still holds the
  POST framebuffer address/stride at start.
- 10:22 the taskbar hung once; restarting explorer.exe fixed it.

### v0.38 result + missing UI pieces found (10:30)
- Direct scanout works ("direct scanout: on", every DWM flip "(direct scanout)"): user reports higher
  FPS, still below 60, same artifacts (Start icon, empty windows).
- UMD logs: "flush_submit_list: submit failed: -22" + msm_dump_submit in dwm and other processes ->
  the KMD rejected GEM_SUBMITs with more than 64 IBs (DWM sent 66): the whole submit (part of a frame)
  was dropped. v0.39: up to 512 IBs (heap arrays; ring 8192 dwords), every rejected/failed submit is
  logged, and "fps: N flips/s" is logged once a second by the vsync DPC.

### v0.39 result (10:34)
- No rejected submits any more; the Start icon is complete. fps log: 7-60 flips/s depending on activity.
- Remaining: (a) stale regions are back after the reboot (old window copy at its previous position, the
  zoomed open-animation frame) - with direct scanout flip completion is still reported by the 60 Hz
  timer, not by the panel's vsync, so DWM's idea of which buffer is on screen can be wrong; next: real
  MDP vsync interrupt (INTF1/CTL) for flip completion; (b) small dashes/bars next to text since the
  first GPU run - probably the glyph atlas updated by the CPU while the GPU still samples it (UMD
  map/update synchronisation).
- v0.40: flip completion for direct scanout only once CTL0_FLUSH's VIG0 bit cleared (MDP latched the
  new address at the panel vsync); CRTC_VSYNC reports the address really on screen (g_ShownPa). fps line
  also counts how often a flip had to wait for the latch.
- Glitch test (10:40): FD_MESA_DEBUG=noubwc,notile does not remove the thin dashed lines/arcs; they appear
  where DWM redraws small regions (touch-feedback circle) and were also on the user avatar of the
  welcome screen -> not glyph/texture layout. d3d10umd scissor/copy-box conversions are correct.
  Suspect the flip-completion timing (v0.39 reports flips done by the timer before the MDP latch, DWM
  may start drawing into the buffer still being fetched). v0.40 staged; umd.env back to defaults.

## STATE AT STOP (2026-10-03 ~11:15) - read this first when resuming
### Where we are
- GPU0 RAM-boot (`fastboot boot ~/work/win/uefi/Mu-topaz-v4-GPU0-RELEASE.img` on s8build): Windows desktop
  composed by DWM on the Adreno 610 through Mesa d3d10umd+freedreno, TopazGpuW v0.40 on the phone
  (System32\drivers\TopazGpuW.sys), direct MDP scanout (VIG0 SRC0_ADDR = BO PA, flip done when the CTL
  flush bit clears), brightness slider through the GPU adapter, fps 13-60 (log "fps:" line), no rejected
  submits (512-IB limit). UMD = latest mesa.yml artifact (patch 0002 incl. storage rotation, umd.env,
  TopazWddm* logging) in System32\topazgpu_d3d10.dll; C:\topaz\umd.enable, C:\ProgramData\topaz\umd.log.enable
  present; umd.env = defaults. Normal (flashed) boot = TopazDisplay v0.8, unchanged.
- Open symptoms: (1) stale regions (zoomed window-open animation frame, old window copy) until the area is
  redrawn; (2) 1-px dashed lines/arcs where DWM redraws small regions (touch circle, menu text, user
  avatar); (3) popup drop shadows clipped with a hard edge; portrait only; Modern Standby BSOD 0x14F
  (timeouts set to 0); preemption disabled system-wide.

### Fresh analysis (independent agent, 2026-10-03, code reading only)
- Rotate/present contract is CONSISTENT: dxgiddi.h `pResources` "0 <= 1, 1 <= 2" = our default direction
  (TOPAZ_ROTATE_REVERSE / TOPAZ_NOROTATE are wrong by definition); the runtime does not rotate allocation
  handles itself and Dxgkrnl scans out exactly the allocation the UMD passed to pfnPresentCb -> presented
  buffer == scanned-out BO. DWM copies everything OUTSIDE the damage rect D from [0] (previous frame) and
  redraws only inside D (NOROTATE -> black outside D proves it). So stale content means pixels INSIDE D
  are not written in that frame -> a draw-side (shader/translation) bug, not flip/rotation. v0.40
  changing nothing fits.
- Defects found in our tgsi_to_nir/d3d10umd path (mesa patch 0002):
  (a) ttn_sample ignores the resource swizzle on Src[1] (e.g. t0.wxyz for scalar .a reads; gallivm applies
      it) -> alpha masks read the wrong channel (often 0).
  (b) D3D `discard` compiled as terminate (nir_discard -> ir3 kill) instead of demote: helper pixels die,
      derivatives/LOD of the rest of a 2x2 quad become garbage -> typical dashed 1-px artifacts along
      curves. ir3 supports demote.
  (c) SVIEWINFO (resinfo/GetDimensions) unimplemented in tgsi_to_nir -> returns 0 (blur 1/size = inf).
  Minor: KMD FILL paging op writes into BO-backed allocations (VidMM does not own that memory) -> skip
  FILL when Al->Bo != NULL.
- Checked and correct: scissor conversion, viewport scissor, a6xx 2D blit rects for linear BGRA, MSAA
  not involved.
### Next steps (in order)
1. Zero-cost: with umd.tgsi on, grep DWM's umd-*.log for "unknown TGSI opcode" (SVIEWINFO), SAMPLE with
   non-xyzw SVIEW swizzle, KILL/KILL_IF followed by DDX/DDY/SAMPLE.
2. Fixes behind umd.env switches, then make default:
   A ttn_sample: `r = nir_pad_vector_imm_int(...); swz = Src[1].Register.Swizzle{X,Y,Z,W}; return
     nir_swizzle(b, r, swz, 4);` (TOPAZ_SVSWZ=1)
   B ttn_kill/ttn_kill_if: nir_demote / nir_demote_if + info.fs.uses_demote = true (TOPAZ_DEMOTE=1)
   C TGSI_OPCODE_SVIEWINFO -> nir_texop_txs (texture_index = Src[1].Index, lod = src[0].x) +
     nir_texop_query_levels for .w, like ttn_txq.
3. Decisive experiment TOPAZ_POISON=1: after fd_resource_rotate_storage clear hResources[1] (next back
   buffer) to magenta (create_surface + clear_render_target). Magenta on screen = pixels in D not written
   (draw-side); grey arcs staying = drawn wrong -> (b); shadow falloff magenta = not drawn, background =
   transparent -> (a)/(c). TOPAZ_FULLCOPY=1 (copy all of [0] into [1]) as a cross-check.
4. KMD: ignore DXGK_OPERATION_FILL for BO-backed allocations.
5. Later: async present (no fence wait), path rotation, Modern Standby, then flashing a GPU0 UEFI only
   with the user's consent.

## C2 follow-up (2026-10-03, session 2): shader-translation fixes behind switches
- Step 1 (zero-cost check, umd.tgsi on, fresh dwm pid 8916, 53 shaders, little UI activity): no "unknown TGSI
  opcode", no SVIEWINFO, no KILL/KILL_IF, every SAMPLE has the default xyzw SVIEW swizzle (DDX/DDY present
  next to SAMPLE in a few FS). So on this sample none of (a)/(b)/(c) triggers yet; more UI (text, popups
  with shadows, blur) is needed to catch them - the fixes are implemented anyway.
- Mesa local tree: ~/work-mesa/mesa (max8rr8/mesa viogpu_win + overlay + 0001 + 0002 committed as "base").
  New patch mesa-overlay/patches/0003-topazgpu-shader-fixes.patch (= `git diff -- src/gallium` on top of
  base; kept separate from 0002, applied in name order by mesa.yml). Switches (umd.env, `NAME=1`):
  - TOPAZ_SVSWZ: ttn_sample applies Src[1] resource swizzle to the sampled vec4.
  - TOPAZ_DEMOTE: KILL/KILL_IF -> nir_demote(_if) (helper lanes stay alive; this Mesa has no info.fs.uses_demote, uses_discard is set).
  - TOPAZ_SVINFO: SVIEWINFO -> nir_texop_txs (+ query_levels in .w) with texture_index = SVIEW index.
  - TOPAZ_POISON: after fd_resource_rotate_storage the next back buffer hResources[1] is cleared magenta.
- 12:00 UMD from CI run 37108930901 (0001+0002+0003) installed without reboot (old DLL kept as
  System32\topazgpu_d3d10.dll.old-<hhmmss>). Phase 1 experiment: umd.env = `TOPAZ_POISON=1` only (fixes off).
- 12:10 POISON result (photos): everything magenta except the regions DWM redrew/copied that frame (explorer
  window body, a status-bar strip) - same picture as TOPAZ_NOROTATE with black. The log shows why the test is
  INVALID: DWM does NOT copy the whole undamaged area each frame (Presents #3->#4, #4->#5 have no
  ResourceCopyRegion at all; others have 4-6 small copies). It relies on every back buffer retaining its
  content and only syncs/redraws damage. Poisoning destroys that, so magenta says nothing about unwritten
  pixels. Replaced by TOPAZ_FULLCOPY=1 (new back buffer [1] := full copy of [0] after rotation): outside DWM's
  damage the content is then exactly the previous frame, so remaining artifacts are inside the damage rect
  (draw-side) and stale regions caused by diverged buffers must vanish.
- umd.env now = TOPAZ_SVSWZ=1, TOPAZ_DEMOTE=1, TOPAZ_SVINFO=1 (fixes on) for the user's visual check.
- 12:30 user photos with SVSWZ+DEMOTE+SVINFO on: artifacts all still present (old window copies at their
  previous position, big blurred white areas, dashed 1-px lines in the window frame, clipped Start-menu
  shadow) - no visible gain, so (a)/(b)/(c) are not the (main) cause. Next A/B: umd.env = TOPAZ_FULLCOPY=1
  only (CI run 37109561377).
- 12:45 FULLCOPY result (4 photos): ZERO change. Stale content is therefore not caused by diverged
  back buffers (the new buffer started as an exact copy of the last presented frame). User observation: the
  big white blob at the bottom is a ~10x enlarged copy of the window's title bar ("...ЛЕНИЕ КОМПЬ", icon) -
  i.e. a window quad drawn far larger than the damage area and never overdrawn. New hypothesis: DWM draws
  with the RS scissor = damage rect; if something makes a draw ignore/misapply the scissor (or a draw uses a
  wrong transform), over-draw lands OUTSIDE DWM's damage area and DWM never repaints it (which also fits "no
  change with FULLCOPY/POISON"). Added TOPAZ_DRAWLOG=1 (every Draw*/ClearRTV logged with RT pointer + size,
  viewport, RS-scissor flag, scissor rect) to compare draws against the stale area.
- 13:00 DRAWLOG analysis (umd-2356.log, DWM pid 2356; 1080x2400 swapchain RT; all draws have rs_scissor=1, vp = full):
  window close animation: draws with scissor 0,2,928,2314 -> 0,2,940,2194 -> (frame k+2) 0,0,1047,2194 (n=6 x2),
  0,36,1047,794 (n=6 x2), 0,0,1047,2194 (n=6) -> later frames only 794/711/665. So DWM DID draw the
  full-height damage rect once (the cleanup frame), yet the screen keeps the zoomed-window blob at rows
  ~1310-2300. The copies (ResourceCopyRegion) = the buffer-age sync of the complement of the damage
  (4 copies for a rectangle damage). Conclusion: the pixels of the cleanup draw in rows >1310 were not
  produced correctly -> draw-side bug, not flip/copy/rotation/scissor-state. Next: log PS SRVs + copy boxes
  (DRAWLOG now prints t0..t2 texture ptr/size/format per draw, and the copy boxes) and compare with the
  stale rect.
- 13:15 Decisive next experiment: TOPAZ_DRAWLOG=1 + trigger file C:\ProgramData\topaz\umd.dump -> at the next
  Present dump the presented frame and the last 12 large (>=400x400) textures sampled as BMPs
  (C:\ProgramData\topaz\umd-dump-<pid>-<n>-frame0/tex<i>.bmp). If the wallpaper texture itself contains the
  blob -> memory aliasing / bad upload of the texture; if the texture is fine but the frame is not -> the
  draw sampling it is wrong.
- 13:45 DUMP result (DWM pid 3552, after the user reproduced the bottom blob): the wallpaper texture
  (1080x2400 BGRA, tex8) and the desktop-icons layer (tex9) are intact; the presented frame holds the blob
  (full width, rows ~956..2315, flat window-body colour 235,246,249 plus a zoomed window title/icon).
  Pixel check: blob colour is constant over dark and light wallpaper areas -> not additive blending; the
  wallpaper simply is not what ends up there. DRAWLOG: frames #55-#58 redraw the whole screen (scissor
  0,0,1080,2400: wallpaper, icons, window layers incl. the 1234x706 window texture), all later frames are tiny
  updates with exact complement copies. So a draw in a full-screen frame paints the zoomed window title over the
  wallpaper -> wrong geometry/constants for some draw, i.e. most likely a CPU/GPU synchronisation hazard on
  dynamic vertex/constant buffers (DWM uses Map NO_OVERWRITE/DISCARD and UpdateSubresourceUP between draws; the
  draws execute later from the batch). Experiments added: TOPAZ_FLUSHDRAW=1 (flush + fence wait after every
  draw: fully serialized) and TOPAZ_SYNCMAP=1 (NO_OVERWRITE maps become synchronized).
- 14:00 TOPAZ_FLUSHDRAW=1 result (user): window contents no longer disappear, nothing stale at the bottom, popup
  shadows correct. Only the thin dashed strokes remain. => artifacts (1) and (3) are a CPU/GPU sync hazard
  (data the CPU changes before deferred draws execute). (2) dashed strokes is a separate issue. Next: narrow it
  (TOPAZ_SYNCMAP=1 alone).
- 14:10 TOPAZ_SYNCMAP=1 alone (NO_OVERWRITE maps synchronized, i.e. flush + wait when the buffer is pending/busy):
  also clean (no stale blocks, shadows ok) but low FPS. => the hazard is on Map(WRITE_NOOVERWRITE). Next split:
  TOPAZ_SYNCMAP=2 = only flush the pending batches before an unsynchronized map (no wait). Fixed by =2 ->
  data overwritten under draws still recorded in an unflushed batch; not fixed -> the GPU still reads the buffer
  after the submit, i.e. freedreno's BO busy tracking (fence) is wrong on WDDM.
- 14:20 TOPAZ_SYNCMAP=2 (flush only, no wait) is also clean -> the hazard is data overwritten under draws still in
  an UNFLUSHED batch. ROOT CAUSE: D3D10DDI_QUERY_EVENT -> PIPE_QUERY_GPU_FINISHED, which freedreno a6xx does not
  implement (create_query returns NULL; fd_sw_create_query has no such type). d3d10umd QueryGetData with a NULL
  handle returns "done" (S_OK) at once. DWM uses event queries to decide when its NO_OVERWRITE vertex/constant ring
  space is free again, so it overwrote vertices/constants of draws that were not even submitted: stale windows,
  zoomed window copies, clipped shadows. Fix: EVENT queries use a deferred fence (pipe->flush(&fence,
  PIPE_FLUSH_DEFERRED) at QueryEnd; QueryGetData = fence_finish(timeout 0), flushing unless DO_NOT_FLUSH).
  TOPAZ_OLDEVENT=1 restores the old behaviour for comparison.
- Also reported (14:19 photo): icons in the quick-settings flyout (Wi-Fi, airplane mode, battery saver) are
  missing / very faint - glyph (Segoe Fluent Icons) rendering, to investigate next.
- 14:30 EVENT-query fix installed (no switches): FPS 43 (fpsbench: moving window, tools/gpu/fpsbench.ps1 via a
  /IT scheduled task; baseline 43, SYNCMAP=2 31) but the stale zoomed blocks are STILL there -> DWM does not rely
  on event queries for this (fix kept, it is correct anyway). Next: TOPAZ_MAPLOG=1 (buffer maps DISCARD/NOOVERWRITE
  + IaSetVertexBuffers offsets) to see the ring pattern.
- 14:45 MAPLOG (pid 6104): DWM's dynamic rings = one vertex buffer (160000 B, ~1590 NOOVERWRITE maps, 7 DISCARDs
  = wraps) and one index buffer (16000 B, ~1585 NOOVERWRITE, 10 DISCARDs); VB offset always 0 (DWM uses
  start/base vertex). User: "make it the normal base" -> the flush before every NO_OVERWRITE map (former
  TOPAZ_SYNCMAP=2) is now the DEFAULT (TOPAZ_SYNCMAP=0 disables it). Cost: fpsbench 43 -> ~31 FPS.
  Open: real root cause (why freedreno does not see the ring region as still in use / why DISCARD-renaming is
  not enough) to win the FPS back.
- 15:00 Dashed 1-px strokes: DWM copies the damage complement from the previous buffer every frame, often as 1-2 px
  strips (118x1, 2x115, 9x1 ...) exactly along damage edges where the strokes show. Experiment TOPAZ_COPYPATH=1:
  swapchain ResourceCopyRegion through util_resource_copy_region (CPU map/memcpy) instead of freedreno's blit.
- Note (2026-10-03): the user's phone reports codename **tapas** (Redmi Note 12 4G without NFC); topaz is the NFC variant of the same SM6225 platform. Names in the repo stay "topaz".
- 15:10 TOPAZ_COPYPATH=1 (CPU swapchain copies): strokes remain (thin vertical lines inside the window body,
  dashed horizontal scrollbar thumb) -> not the complement copies. FPS unchanged (29). Next experiment
  TOPAZ_UPDSYNC=1: ResourceUpdateSubresourceUP on textures without DISCARD_RANGE (freedreno then skips its
  shadow/staging upload path).
- 15:20 TOPAZ_UPDSYNC=1: strokes remain (faint straight horizontal/vertical lines through the window at seemingly regular positions -> suspect GMEM bin edges). Next: FD_MESA_DEBUG=sysmem.
- 15:35 FD_MESA_DEBUG=sysmem: strokes remain (not GMEM bins). DUMP (pid 1276): the mmc window texture
  (1182x1820) is clean, the presented frame has the dashed/straight 1-px strokes (rows of the tree view, right
  pane) -> added by DWM's composition on our stack, not by the app/upload. The window is composed at ~0.86 scale.
  Strokes look like leftover pixels exactly at damage-rect edges. Experiment TOPAZ_SCPAD=1/2: scissor 1 px wider
  (right/bottom, or all sides).
- 15:50 Strokes reproduced by me: tools/gpu/fpsbench.ps1 -Borderless moved over the mmc window, then a frame dump.
  The dump matches the phone (strokes are in memory, not a scanout/cache effect). TOPAZ_SCPAD=2 and
  IR3_SHADER_DEBUG=nofp16 do not change them. The mmc window texture is clean. A borderless moving window leaves
  1-px OUTLINES at its previous positions (grey 128 on white = 50% black): a 1-px fringe painted outside DWM's
  damage area, never repainted. Suspect: border colour / AA fringe. d3d10umd never set
  pipe_sampler_state.border_color_format (freedreno uses it for the border colour layout): now
  R32G32B32A32_FLOAT by default (TOPAZ_BCFMT=0 = old).
- 16:10 Correction: the outlines are transient (visible while the window moves, gone after it closes) and are
  there with and without TOPAZ_BCFMT and TOPAZ_SCPAD=2 (dumps taken mid-movement with tools: strokes2.ps1). The
  BCFMT change stays (correct API use) but is not the fix. Outline rows/columns equal the top/left edges
  (miny/minx) of DWM's damage scissors, and they appear only over the DPI-scaled mmc window, not over the
  wallpaper; colour = 50% grey. New tool: umd.probe (file with a row number) -> for one frame, row Y read back
  after every draw/copy into the 1080-wide RT ("PROBE k ..." in the UMD log).
- 16:30 PROBE (row 1620, 40 frames, borderless window moving right over the mmc window): frame 15 damage scissor
  161..1057; the mmc layer draw fills the exposed strip 161..181 with 240 except the strip's first column 161,
  which gets 214 (~75% coverage blended over the old blue); the next frame does not touch col 161 again -> outline.
  DWM draws these partial layer pieces with shader edge-AA and expects full coverage when the edge lies on a
  pixel boundary; on our stack the edge pixel gets partial coverage. Next: identify the PS (probe/DRAWLOG now
  print the bound PS tokens pointer; umd.tgsi dumps are tagged "(the TGSI above is PS <ptr>)").
- 16:50 ROOT CAUSE of the dashed strokes: the DPI-scaled window layer is drawn by DWM's downscale PS (4 SAMPLEs
  at IN.xy + DDX/DDY * {+-0.375, +-0.125}). Probe: every wrong edge pixel is a strip edge whose 2x2-quad partner
  lies outside the damage scissor (left edges all odd x: 445, 413, 581, ...; right edges where x+1 is cut).
  On a6xx scissored-out pixels apparently do not serve as helper lanes, so the derivatives at that column
  are garbage and the taps sample elsewhere -> 1-px wrong columns/rows left behind (DWM never repaints them).
  Fix: scissor rects aligned to even coordinates (whole quads), default on; TOPAZ_SCALIGN=0 = old.
- 17:05 Quad-aligned hardware scissor alone moved the bad column to the added pixel (DWM clips strip GEOMETRY
  too, the extra column got only some layers). Proper fix (default, TOPAZ_FSSCISSOR=0 disables): hardware
  scissor widened to whole 2x2 quads + the exact D3D scissor applied in every pixel shader: ShaderTGSI injects
  "KILL_IF pos outside CONST[15][0]" (pipe CB slot 15 is unused by D3D's 14 CBs; Rasterizer.cpp uploads
  (minx, miny, 16384-maxx, 16384-maxy) via the stream uploader on scissor/RS changes, zeros = no clip), and
  KILL/KILL_IF are now compiled as demote by default (TOPAZ_DEMOTE=0 = terminate), so the cut pixels stay
  helper lanes and derivatives at the edge are right.
- 17:20 FS-scissor+demote result: much worse - strong dotted diagonals along triangle seams of the strips (also
  with TOPAZ_DEMOTE=0): on this stack ANY kill/demote in the PS breaks helper lanes outside the primitive.
  FS scissor and demote are opt-in again (TOPAZ_FSSCISSOR=1, TOPAZ_DEMOTE=1). New default (TOPAZ_SCFIX=0
  disables): quad-aligned hardware scissor, and the extra 1-px strips (aligned minus real rect) are saved to a
  scratch texture before the draws and copied back before anything can observe them (next scissor, RT change,
  RS scissor off / unscissored draw, ClearRTV, ResourceCopy(Region), Present).
- 17:35 SCFIX result: the distinct grey vertical strokes are gone, only very faint light dots remain (also faint dots along triangle seams of the strips -> derivatives at triangle seams slightly off too). Experiment TOPAZ_DERIV=1/2 (fine/coarse fddx/fddy for TGSI DDX/DDY).
- 17:50 TOPAZ_DERIV=1 (fine) / =2 (coarse): no clear gain (fine slightly worse on seams); default stays nir fddx. umd.env back to defaults (SCFIX on).
- 18:05 User: strokes almost gone, horizontal ones at the bottom remain. Repro: the mmc window's bottom content
  row (y 1871 in that layout) gets 2x2-quad garbage (pairs 196/76/240) written by the downscale PS; it is not a
  scissor edge. TOPAZ_DERIV fine/coarse: no change (86/119/86 bad pixels). Next: alpha of the window texture's
  bottom resize-border rows (the 4 taps reach into them): TOPAZ_DUMPALPHA=1 dumps alpha as grey.
- 18:20 Window texture alpha (TOPAZ_DUMPALPHA): bottom rows fully opaque (255), only the top-left corner pixels
  are 0 -> no transparent border issue. The bad row is the LAST row of the window quad (texture 1818 rows x
  0.86 scale ends at y 1871); the garbage comes in horizontal pixel pairs (per 2x2 quad), i.e. the derivative-
  driven taps of the downscale PS differ per quad on the bottom edge row. Not solved yet; candidates: the bottom
  edge row's quads straddle two triangles / helper rows below the primitive, or the taps reach the black rows
  1804-1817 of the redirection surface. umd.env back to defaults.

### OPEN ISSUE (parked 2026-10-03 18:30, user: "not very disturbing"): horizontal strokes at window bottom edges
- Symptom: after something moved over a window (touch trail, another window), a faint dashed horizontal line stays
  on the bottom edge of DPI-scaled windows (seen on mmc.exe "Computer Management", composed at ~0.86 scale).
- What is known:
  - The window texture (redirection surface, e.g. 1182x1818) is clean and fully opaque at the bottom rows
    (TOPAZ_DUMPALPHA dump); rows 1804-1817 of it are dark (0/207/209 green), likely the resize border.
  - The bad screen row is the LAST row covered by the window quad (1818 * 0.86 + top -> y 1871 in the test layout).
  - Written by DWM's downscale PS (TGSI: 4x SAMPLE at IN[2].xy + DDX/DDY(IN[2]) * {0.375,-0.125,-0.375,0.125},
    averaged *0.25, alpha forced 1, times IN[1]). Bad values come in horizontal PAIRS (per 2x2 quad):
    e.g. row 1871 = 196 196 / 76 76 / 240 240 ... -> per-quad derivative-dependent taps.
  - Not fixed by: TOPAZ_DERIV=1 (fddx_fine) / =2 (coarse); SCFIX (it is not a scissor edge); interpolateAtOffset is
    not an escape (ir3 lowers load_barycentric_at_offset to dsx/dsy as well).
- Related facts on this stack (a6xx gen1 / A610, Mesa viogpu_win 24.3 ir3):
  - scissored-out pixels are not helper lanes (fixed by SCFIX: quad-aligned scissor + strip save/restore);
  - ANY kill/demote in a PS breaks helper lanes along triangle seams (TOPAZ_FSSCISSOR=1 / TOPAZ_DEMOTE=1 show
    strong dotted diagonals) -> helper-lane handling in ir3/a6xx is the common suspect.
- Next steps when resumed: (1) probe the bottom row with the per-draw probe and identify the exact draw + its
  vertex positions (log VB contents for that draw) to see whether the quad edge is fractional (e.g. 1871.x) and the
  row's quad partner row lies outside the primitive; (2) compare ir3 disasm of the downscale PS (IR3_SHADER_DEBUG=
  disasm into the UMD log) for how dsx/dsy and helper lanes are set up (lodpixmask/pixlodenable, (jp)/early-
  exit); (3) test a6xx SP_FS_CTRL_REG0 / helper-related bits; (4) fallback: treat edge rows like SCFIX (expand the
  drawn region by one row/column and restore), only for draws whose PS uses derivatives.
- 18:45 FPS work. FD_MESA_DEBUG=inorder + TOPAZ_SYNCMAP=0: 34 FPS but stale-window artifacts still there (user) -> not batch reordering. Present does pipe->flush + fence_finish(INFINITE) every frame (CPU and GPU strictly serial). Next: MAPLOG with freedreno BO/batch tracking state before/after every DISCARD (does freedreno rename the ring BO?).
- 18:55 MAPLOG with tracking state: ring DISCARDs DO rename (VB 160000 / IB 16000: batches 0x1/0x3 -> new BO); small CB DISCARDs have batches 0 (no rename needed). FPS with TOPAZ_SYNCMAP=0+MAPLOG 23 (logging cost). Next: TOPAZ_SYNCMAP=3 (flush only before VB maps) / =4 (only IB maps).
- 19:05 TOPAZ_SYNCMAP=3 (flush only before vertex-buffer NO_OVERWRITE maps): user sees no stale copies/shadow problems, only the bottom (and top) edge strokes -> the hazard is on the VERTEX ring. User: Chrome windows show no strokes, Explorer/mmc windows do (GDI/DirectComposition content composed by DWM).
- 19:15 Frame dump with Explorer open: tab titles, address bar, file list TEXT missing. explorer.exe (pid 5192) had loaded the UMD at 10:45 = an old build without the day's fixes (each process keeps the DLL it loaded) -> explorer restarted; NOTE: after a UMD swap, restart explorer and other GPU apps too, not only dwm.
- 19:25 After explorer was restarted on the current UMD it hung, then the phone switched off. RAM-booted GPU0 again:
  explorer keeps crashing, FPS very low, and TopazWifi died (no SSH). Plan: normal boot (no GPU0, no UMD) to get
  SSH back and collect explorer crash dumps / UMD logs. Safety switch added: C:\topaz\umd.dwmonly -> only dwm.exe
  opens the Adreno UMD, every other process fails OpenAdapter (D3D falls back).
- 19:45 Recovery: TopazWifi did not start in the normal boot because its boot guard C:\topaz\wifi.boot survived
  a crash within the first 20 s of a boot (driver refuses to start while it exists). User deleted it by hand ->
  Wi-Fi/SSH back. LESSON: after a crash during boot, check/delete C:\topaz\wifi.boot.
- Post-mortem of the explorer problem (GPU0 session 15:27-15:38): WER AppHangB1 for explorer.exe, hang type
  0x8000000 (cross-process wait); no bugcheck (Event 41/6008 only = hard hang / power-off, no dump). KMD log:
  no rejected/failed submits, no GEM_NEW failures, many "device gone" lines (processes recreating devices),
  ~550 BOs / 296 MB peak. UMD logs show nothing fatal. Cause not identified yet.
- Installed for the next GPU0 boot: UMD with C:\topaz\umd.dwmonly (present) -> only dwm.exe uses the Adreno UMD,
  explorer & apps fall back to the Microsoft renderer. umd.env = defaults.
- 19:55 GPU0 boot with umd.dwmonly: stable, dwm on Adreno, fpsbench 26. BUT explorer's XAML part (tabs, address
  bar, command bar) is blank: apps get E_FAIL from OpenAdapter on adapter 0 and XAML does not fall back to WARP.
  Screenshot tool: C:\topaz\shot.ps1 via a /IT scheduled task (GDI CopyFromScreen; not DPI-aware yet).
  Added TOPAZ_TIMING=1 (per 60 presents: frame time, flush, GPU wait in Present, NO_OVERWRITE flushes/frame) and
  TOPAZ_NOPRESENTWAIT=1 (skip the CPU fence wait in Present; experiment).
- 20:05 TIMING (dwm, base): frame 33-38 ms, GPU wait in Present 0.03 ms (GPU already idle), flush 0.15 ms, 31-37 NO_OVERWRITE flushes per frame; fpsbench clusters at exactly 30 -> vsync-quantised (flip waits for the MDP latch), frame work slightly > 16.7 ms. Measuring time spent in the NO_OVERWRITE flushes and in pfnPresentCb next.
- 20:15 TIMING: the NO_OVERWRITE flushes cost 7.8-10.7 ms per frame (~0.28 ms each, 27-37/frame), pfnPresentCb 0.08 ms; frame 32-39 ms. Checked freedreno rebind after DISCARD rename (fd_set_vertex_buffers sets rsc->dirty, rebind marks VTXBUF) - looks right. Next: RING trace (TOPAZ_MAPLOG: start vertex/index per draw) to see whether DWM reuses ring regions without DISCARD.
- 20:25 RING trace (SYNCMAP=0): 15 VB-ring epochs, no draw range overlaps within an epoch, base vertices monotonic -> DWM never reuses ring space without DISCARD; the ring is not the culprit (the VB-map flush only masks something else). New suspect: constant buffers (2080/96/80/64/32 B) are DISCARD-mapped with batches 0x0 (freedreno thinks no pending draw reads them) -> written in place under pending draws. Experiment TOPAZ_CBFLUSH=1 (flush before CB DISCARD maps) with TOPAZ_SYNCMAP=0.
- 20:40 TOPAZ_CBFLUSH=1 (+SYNCMAP=0): 35 FPS but shadows broken -> constant-buffer DISCARD is not it; base restored
  (shadows OK again).
- EXPLORER CRASHES FOUND: WER APPCRASH explorer.exe in topazgpu_d3d10.dll, c0000005 at +0x1639a4
  (fd_resource_copy_region, freedreno_blitter.c:433) and +0x1f0d50 (check_append_bo / fd_submit_append_bo).
  Cause: the SCFIX strip state (scratch texture, pending strips, RT ref) and the RS-scissor flag were process
  globals; explorer has several D3D devices/screens, so a scratch texture of one screen was used on another.
  Fix: state moved into Device (Device::topazScFix, Device::topazRsScissor), freed in DestroyDevice (it also
  leaked one full-RT texture per destroyed device before).
- 20:55 Explorer stable with the per-device SCFIX (user). FPS work: RING trace corrected for indexed draws: all
  3776 ring draws are DrawIndexed; IB ring (16000 B) 23 epochs, no overlapping index ranges, base vertices
  monotonic -> DWM's ring usage is clean. The VB-map flush happens before almost every draw, so it masks any
  CPU write between draws. Next suspect: constant buffers updated with UpdateSubresourceUP (Map DISCARD on CBs is
  rare). Experiment TOPAZ_UPDFLUSH=1 (CB) / 2 (any buffer) / 3 (any) with TOPAZ_SYNCMAP=0.
- 21:05 ROOT CAUSE of stale/zoomed windows + clipped shadows FOUND: TOPAZ_SYNCMAP=0 + TOPAZ_UPDFLUSH=1 (flush
  only before UpdateSubresourceUP of CONSTANT buffers, 0-1.5 per frame) is clean (user) and 37 FPS. DWM rewrites a
  constant buffer with UpdateSubresourceUP between draws; freedreno does not see the pending draws' read of that
  CB (batch_mask 0 for CBs in MAPLOG) and writes in place, so earlier draws of the batch get the new constants.
  New defaults: TOPAZ_UPDFLUSH=1 (on), TOPAZ_SYNCMAP=0 (the per-NO_OVERWRITE flush is gone).
  Proper fix later: make freedreno track CB reads (or shadow/rename CBs on UpdateSubresourceUP) instead of the flush.
- 21:20 FPS analysis with real windows (tools/gpu/movebench.ps1 moves the user's Chrome window; fpsbench -Big):
  DWM 22-26 flips/s, frame 36-42 ms, but dwm uses only ~0.3 core (3.1 s CPU in 10 s) -> it waits ~29 ms per frame.
  GPU wait/present cb negligible. KMD vsync = a 16 ms KTIMER: the "per second" fps lines come every ~0.86 s, i.e. the
  timer fires on ~14-15.6 ms clock ticks, unrelated to the panel's real vsync; a direct flip is only reported done
  at the first tick after the MDP latch (~8 extra waits/s) -> DWM loses periods.
- TopazGpuW v0.41: 1 ms EX_TIMER_HIGH_RESOLUTION poll; a pending direct flip is reported done right after the
  CTL0_FLUSH VIG0 bit clears (= real panel vsync) and the 60 Hz vsync phase is re-locked to that moment
  (g_NextVsync = latch + 16.667 ms); fallback to the old KTIMER if ExAllocateTimer fails.
- 21:45 TopazGpuW v0.41 on the phone (GPU0 RAM boot): "vsync: 1 ms high-resolution timer". movebench (Chrome
  window moved): 40-44 flips/s (v0.40: 22-25); fpsbench -Big ~32, small ~26 (bench window has no content).
  DWM TIMING still ~37 ms average frame (includes idle frames). Next: DWM CPU per frame (~13 ms) / UMD overhead.
- 22:00 Touch-drag is the real limit: dragging Chrome/Explorer by finger gives 6-9 DWM flips/s, the same window moved by script gives 40+. TopazTouch v0.5 logs reads/s and I2C read time per second while touching.
- 22:20 Touch path measured: TopazTouch reads 147/s (1.1 ms I2C reads, v0.5 stats), a WinForms window gets ~150
  mouse/touch moves/s (tools/gpu/touchrate.ps1) -> input is fine. DxgKrnl ETW during a finger drag of Explorer
  (15 s, decoded on the Mac from tracerpt CSV): 342k events, 160k Profiler start/stop pairs from dwm.exe, of which
  74833 DxgkEscape (~5000 escapes/s ~ 300 per present) -> DWM busy-polls through our msm escapes. VSync 63/s,
  DxgkPresent 16/s. Added TOPAZ_ESCLOG=1 (escape counts per DRM command per second, fd_wddm.c).
- 22:35 ESCLOG during a finger drag (DWM): per second ~1800 x (0x42 GEM_NEW, 2x 0x43 GEM_INFO, 0x01, 0x02, 0x09
  GEM_CLOSE), 0x46 SUBMIT ~450, 0x48 MADVISE ~200 -> DWM allocates and frees ~110 BOs per frame through the KMD
  (contiguous allocations!). freedreno's BO cache misses: find_in_bucket() only reuses BOs whose fd_bo_state() is
  IDLE, which depends on the GPU-written userspace fence in the pipe control BO. Checking it: TIMING now also logs
  "FENCES control <GPU-written> last <emitted>". NOTE for the local tree: fd_wddm.c edits live in
  mesa-overlay/files only (local ~/work-mesa copy is the base version; do not diff it into 0003).
- 22:50 FENCES: control == last (the GPU-written userspace fence is fine), ~27 submits per frame. GEM_NEW histogram:
  all ~2300/s are 16-64 KiB CACHED_COHERENT = RING_FLAGS cmdstream/state-object suballoc BOs (fd_bo_new_ring,
  SUBALLOC_SIZE 32K) that freedreno's ring cache does not reuse. Added BO-cache counters (hits, miss, busy-head
  = find_in_bucket gave up because the oldest entry is busy, put, expired) to the TIMING log.
- 23:00 BO cache counters during a Chrome move: hits 744, miss 9106, busy-head 0, put 744 per 60 frames -> freed BOs mostly bypass the cache (not a busy problem). Added try_recycle reason counters.
- 23:10 ROOT CAUSE of the BO churn: try_recycle counters: ~9000 freed BOs per 60 frames counted as "nocache", 0 as
  "ring". fd_bo::bo_reuse is `enum { NO_CACHE=0, BO_CACHE=1, RING_CACHE=2 } bo_reuse : 2;` - with MSVC/clang-cl
  enum bitfields are signed, so RING_CACHE is read back as -2 and never matches: every cmdstream/state-object
  BO (32 KiB RING_FLAGS) was freed through the KMD and reallocated (~2300 GEM_NEW + mmap + munmap + GEM_CLOSE
  per second, contiguous allocations in the KMD). Fix: bitfield widened to 3 bits (same class of bug as the
  earlier gl_tess_spacing / lrz_direction fixes in 0001).
- 23:20 bo_reuse fix result: BO cache hits 11276 / miss 1 per 60 frames, GEM_NEW gone; Chrome move 45-49 FPS. Now
  GEM_MADVISE escapes ~11000/s (every cache put/get) -> answered in fd_wddm.c without an escape (KMD never purges).
  Also new: TIMING "gpu wait" 16.5 ms per present (to investigate).
- 23:30 With MADVISE answered locally: escapes ~60-280/s (were ~5000). Chrome move 35-40, big window 80-93 flips/s
  (>60 = counting artefact to check). TIMING "gpu wait" ~15 ms per present -> now GPU-bound. GPU core clock is
  300 MHz (hw.c: gfx3d RCG = GPLL0/2); A610 on SM6225 goes to ~950 MHz -> next: higher clock (needs the GX
  voltage question answered; behind a kill switch).
