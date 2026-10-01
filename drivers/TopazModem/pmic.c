/*
 * P4 diagnostics, READ-ONLY: are the WLAN/RF rails of the WCN3950 up? Stock DT (icnss@c800000):
 * vdd-cx-mx = PM6125 L8, vdd-1.8-xo = L16, vdd-1.3-rfa = L17, vdd-3.3-ch0 = L23 (+ L9 = BT io).
 * They belong to RPM; apps can only read them, through the SPMI arbiter v5 observer channel
 * (same method as TopazButtons for PON). Never write PMIC registers from here.
 */
#include "Modem.h"

#define Out ModemOut

#define SPMI_CORE_PA     0x01C40000u
#define SPMI_CORE_SIZE   0x1100u
#define SPMI_APID_MAP    0x900u
#define SPMI_OBS_PA      0x03E00000u            /* + 0x80 * apid (EE 0 = apps) */
#define SPMI_OBS_STRIDE  0x80u
#define ARB_CMD          0x00
#define ARB_STATUS       0x08
#define ARB_RDATA0       0x18

typedef struct { CONST CHAR8 *Name; UINT8 Periph; CONST CHAR8 *Use; } RAIL;

/* qcom_spmi-regulator.c pm6125_regulators: Ln at 0x4000 + 0x100 * (n - 1) */
STATIC CONST RAIL mRails[] = {
  { "L8", 0x47, "wlan cx-mx" }, { "L9", 0x48, "bt io" }, { "L16", 0x4F, "1.8 xo" },
  { "L17", 0x50, "1.3 rfa" }, { "L23", 0x56, "3.3 ch0 (PA)" },
};

STATIC UINT32 FindApid(UINT8 *Core, UINT32 Ppid)
{
  UINT32 n, v;

  for (n = 0; n < (SPMI_CORE_SIZE - SPMI_APID_MAP) / 4; n++) {
    v = MmioRead32 ((UINTN)Core + SPMI_APID_MAP + 4 * n);
    if (v != 0 && ((v >> 8) & 0xFFF) == Ppid) {
      return n;
    }
  }
  return MAX_UINT32;
}

/* Observer read of one register; 0x1EE on error/timeout. */
STATIC UINT32 ObsRead(UINT8 *Obs, UINT8 Off)
{
  UINT32 i, st;

  MmioWrite32 ((UINTN)Obs + ARB_CMD, (1u << 27) | ((UINT32)Off << 4));
  for (i = 0; i < 1000; i++) {
    st = MmioRead32 ((UINTN)Obs + ARB_STATUS);
    if (st & 1) {
      return (st & 0xE) ? 0x1EE : (MmioRead32 ((UINTN)Obs + ARB_RDATA0) & 0xFF);
    }
    KeStallExecutionProcessor (1);
  }
  return 0x1EE;
}

VOID PmicProbe(VOID)
{
  UINT8 *core = MapPhys (SPMI_CORE_PA, SPMI_CORE_SIZE, FALSE), *obs;
  UINT32 r, sid, apid;

  if (core == NULL) {
    Out ("  pmic: map failed\r\n");
    return;
  }
  Out ("  pmic: SPMI arbiter %08x, WLAN rails (read-only; en = reg 0x46 bit7, status = 0x08):\r\n",
       MmioRead32 ((UINTN)core));
  for (r = 0; r < ARRAY_SIZE (mRails); r++) {
    for (sid = 0; sid < 2; sid++) {
      apid = FindApid (core, (sid << 8) | mRails[r].Periph);
      if (apid == MAX_UINT32) {
        continue;
      }
      obs = MapPhys (SPMI_OBS_PA + (UINT64)apid * SPMI_OBS_STRIDE, SPMI_OBS_STRIDE, FALSE);
      if (obs == NULL) {
        continue;
      }
      Out ("   %a sid %u apid %u (%a): type %x/%x status %x vset %x/%x mode %x en %x\r\n", mRails[r].Name, sid,
           apid, mRails[r].Use, ObsRead (obs, 0x04), ObsRead (obs, 0x05), ObsRead (obs, 0x08),
           ObsRead (obs, 0x40), ObsRead (obs, 0x41), ObsRead (obs, 0x45), ObsRead (obs, 0x46));
      UnmapPhys (obs, SPMI_OBS_STRIDE);
    }
  }
  UnmapPhys (core, SPMI_CORE_SIZE);
}
