/*
 * P4 groundwork, read-only: what does the WLAN DMA path look like under Windows?
 *  - apps SMMU (0xC600000, SMMUv2 "qsmmu-v500"): global config + stream match for the WLAN
 *    stream 0x1A0/mask 1 (DTB icnss iommus). S2CR type 0 = translate (context bank), 1 = bypass,
 *    2 = fault; no match -> sCR0.USFCFG decides (0 bypass, 1 fault).
 *  - copy engines at 0xC800000 + 0x240000 (ath10k wcn3990_regs), only after FW_READY.
 */
#include "Modem.h"

#define Out ModemOut

#define SMMU_PA        0x0C600000u
#define SMMU_SIZE      0x00080000u
#define WLAN_SID       0x1A0
#define WLAN_SID_MASK  0x1
#define CE_PA          (0x0C800000u + 0x240000u)
#define CE_COUNT       12

#define R32(b, o)      MmioRead32 ((UINTN)(b) + (o))

STATIC BOOLEAN SidMatch(UINT32 Smr)
{
  UINT32 id = Smr & 0xFFFF, mask = (Smr >> 16) & 0x7FFF;

  return ((id ^ WLAN_SID) & ~(mask | WLAN_SID_MASK) & 0x7FFF) == 0;
}

STATIC VOID DumpCb(UINT8 *S, UINT32 PageSize, UINT32 NumPage, UINT32 Cb)
{
  UINT8 *gr1 = S + PageSize, *cb = S + (UINTN)NumPage * PageSize + (UINTN)Cb * PageSize;

  Out ("    CB%u: CBAR %08x SCTLR %08x TCR %08x TTBR0 %08x%08x FSR %08x FAR %08x%08x\r\n", Cb,
       R32 (gr1, Cb * 4), R32 (cb, 0x0), R32 (cb, 0x30), R32 (cb, 0x24), R32 (cb, 0x20), R32 (cb, 0x58),
       R32 (cb, 0x64), R32 (cb, 0x60));
}

/* All == TRUE: every valid SMR; else only the ones matching the WLAN stream. */
VOID SmmuProbe(BOOLEAN All)
{
  UINT8 *s = MapPhys (SMMU_PA, SMMU_SIZE, FALSE);
  UINT32 cr0, id0, id1, id2, nsmr, psize, npage, i, hits = 0;

  if (s == NULL) {
    Out ("  smmu: map failed\r\n");
    return;
  }
  cr0 = R32 (s, 0x0);
  id0 = R32 (s, 0x20);
  id1 = R32 (s, 0x24);
  id2 = R32 (s, 0x28);
  nsmr  = id0 & 0xFF;
  psize = (id1 & (1u << 31)) ? 0x10000 : 0x1000;
  npage = 1u << (((id1 >> 28) & 7) + 1);
  Out ("  smmu: sCR0 %08x (CLIENTPD %u USFCFG %u) ID0 %08x ID1 %08x ID2 %08x: %u SMRs, %u CBs, page %x x%u\r\n",
       cr0, cr0 & 1, (cr0 >> 10) & 1, id0, id1, id2, nsmr, id1 & 0xFF, psize, npage);
  for (i = 0; i < nsmr && i < 128; i++) {
    UINT32 smr = R32 (s, 0x800 + 4 * i), s2cr = R32 (s, 0xC00 + 4 * i);
    BOOLEAN wlan = (smr >> 31) != 0 && SidMatch (smr);
    if ((smr >> 31) == 0 || (!All && !wlan)) {
      continue;
    }
    Out ("   SMR%u %08x (sid %x mask %x) S2CR %08x type %u cb %u%a\r\n", i, smr, smr & 0xFFFF,
         (smr >> 16) & 0x7FFF, s2cr, (s2cr >> 16) & 3, s2cr & 0xFF, wlan ? "  <== WLAN" : "");
    if (wlan) {
      hits++;
      if (((s2cr >> 16) & 3) == 0 && (UINTN)npage * psize * 2 <= SMMU_SIZE) {
        DumpCb (s, psize, npage, s2cr & 0xFF);
      }
    }
  }
  Out ("  smmu: WLAN stream %x: %a\r\n", WLAN_SID, hits != 0 ? "has a stream match (see above)" :
       ((cr0 >> 10) & 1) ? "no match, USFCFG=1 -> FAULT" : "no match, USFCFG=0 -> BYPASS");
  UnmapPhys (s, SMMU_SIZE);
}

/* After FW_READY only: the WLAN block is powered and clocked by the firmware by then. */
VOID CeProbe(VOID)
{
  UINT8 *ce;
  UINT32 i;

  LogSetLazy (FALSE);                           /* if this read hangs the bus, keep what we have */
  ce = MapPhys (CE_PA, (CE_COUNT + 1) * SIZE_4KB, FALSE);
  if (ce == NULL) {
    Out ("  ce: map failed\r\n");
    LogSetLazy (TRUE);
    return;
  }
  Out ("  ce wrapper: %08x %08x %08x %08x\r\n", R32 (ce, 0xC000), R32 (ce, 0xC004), R32 (ce, 0xC008),
       R32 (ce, 0xC00C));
  for (i = 0; i < CE_COUNT; i++) {
    UINT8 *c = ce + i * SIZE_4KB;
    Out ("  ce%u: sr %08x/%x dr %08x/%x ctrl1 %08x idx s %x/%x d %x/%x\r\n", i, R32 (c, 0x0), R32 (c, 0x8),
         R32 (c, 0xC), R32 (c, 0x14), R32 (c, 0x18), R32 (c, 0x3C), R32 (c, 0x44), R32 (c, 0x40), R32 (c, 0x48));
  }
  UnmapPhys (ce, (CE_COUNT + 1) * SIZE_4KB);
  LogSetLazy (TRUE);
}
