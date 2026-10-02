/*
 * TopazAudio: ADSP (LPASS Hexagon) bring-up for sound on Redmi Note 12 4G (khaje / SM6225).
 * Infrastructure (compat.h, port.c, log.c, fileio.c, Smem.c, smc.asm) is copied from
 * drivers/TopazModem and keeps its names (ModemOut, ModemMapInit, gModemStop, ...).
 * Facts (stock DT): remoteproc-adsp PAS id 1, carveout 0x53800000+0x2300000 (inside the UEFI
 * "PIL Reserved" range), SMP2P adsp items in 443 / out 429 with APCS IPC bit 10, GLINK edge
 * "lpass" remote pid 2 with APCS IPC bit 8, channels IPCRTR (QRTR) and adsp_apps (GPR).
 */
#pragma once
#include "compat.h"

extern UINTN gSmemVa, gApcsVa, gGicdVa;
#define SMEM_BASE          gSmemVa
#define SMEM_PA            0x46000000u
#define SMEM_SIZE          0x00200000u
#define SMEM_GLOBAL_HOST   0xFFFE
#define APCS_PA            0x0F111000u
#define APCS_IPC           (gApcsVa + 8)      /* apcs_glb mailbox, Linux offset 8 */
#define GICD_PA            0x0F200000u

#define ADSP_PID           2                  /* SMEM host / SMP2P / GLINK remote pid */
#define ADSP_PAS_ID        1
/* DT qcom,smem = <443 429> is <inbound outbound> (Linux SMP2P_INBOUND = 0); v0.1/v0.2 had them
   swapped and the ADSP took over item 443 as its own */
#define ADSP_SMP2P_OUT     429                /* apps -> adsp */
#define ADSP_SMP2P_IN      443                /* adsp -> apps */
#define ADSP_SMP2P_BIT     10                 /* smp2p-adsp mboxes = <&apcs_glb 10> */
#define ADSP_GLINK_BIT     8                  /* glink-edge mboxes = <&apcs_glb 8> */
#define ADSP_WDOG_INTID    (32 + 0x11A)       /* remoteproc-adsp "wdog" SPI */

#pragma pack(1)
typedef struct { UINT32 Offset, Size, Flags; UINT16 Host0, Host1; UINT32 Cacheline, Rsvd[7]; } SMEM_PT_ENTRY;
typedef struct { UINT8 Magic[4]; UINT32 Version, NumEntries, Rsvd[5]; } SMEM_PT;
typedef struct { UINT8 Magic[4]; UINT16 Host0, Host1; UINT32 Size, FreeUncached, FreeCached, Rsvd[3]; } SMEM_PART_HDR;
typedef struct { UINT16 Canary, Item; UINT32 Size; UINT16 PadData, PadHdr; UINT32 Rsvd; } SMEM_PRIV_ENTRY;
typedef struct { UINT32 Allocated, Offset, Size, AuxBase; } SMEM_GLOBAL_ENTRY;
#pragma pack()

/* infrastructure (port.c, log.c, Smem.c) */
VOID           ModemOut(CONST CHAR8 *Fmt, ...);   /* EDK2 formats: %a %s %lu %r */
SMEM_PART_HDR *SmemPartition(UINT16 A, UINT16 B);
VOID          *SmemPrivGet(SMEM_PART_HDR *P, UINT16 Item, UINT32 *Size);
VOID          *SmemPrivAlloc(SMEM_PART_HDR *P, UINT16 Item, UINT32 Size);
UINT32         SmemVersion(VOID);
VOID          *SmemGlobalGet(UINT16 Item, UINT32 *Size);
VOID           ApcsKick(UINT32 Bit);
EFI_STATUS     ModemMapInit(VOID);                /* SMEM, APCS IPC, GICD */
VOID          *MapPhys(UINT64 Pa, UINTN Size, BOOLEAN Wc); /* Wc: Normal NC, else Device */
VOID           UnmapPhys(VOID *Va, UINTN Size);
VOID           ModemIdle(BOOLEAN Busy);           /* 1 ms sleep when idle, short stall when busy */
VOID           LogSetLazy(BOOLEAN Lazy);
VOID           LogFlush(VOID);
extern volatile BOOLEAN gModemStop;               /* driver stop request */

/* Pas.c: TZ PAS boot + SMP2P */
EFI_STATUS     AdspBoot(VOID);                    /* whole flow, returns when the driver stops */
UINTN          AudMs(VOID);                       /* ms since AdspBoot() started */
extern volatile UINT32 *gAdspState;               /* adsp SMP2P "slave-kernel" (bit0 fatal) */

/* Glink.c: GLINK over SMEM, several channels on the lpass edge */
typedef VOID (*GLINK_RX)(CONST UINT8 *Data, UINT32 Len);
VOID           GlinkInit(SMEM_PART_HDR *Part);
BOOLEAN        GlinkPoll(VOID);                   /* TRUE if something moved */
VOID           GlinkRegister(CONST CHAR8 *Name, GLINK_RX Rx, BOOLEAN OpenFirst);
BOOLEAN        GlinkChanUp(CONST CHAR8 *Name);
BOOLEAN        GlinkSend(CONST CHAR8 *Name, CONST VOID *Data, UINT32 Len);
VOID           GlinkSummary(VOID);

/* Qrtr.c: QRTR on IPCRTR + pd-mapper for the ADSP */
VOID           QrtrInit(VOID);
VOID           QrtrPoll(VOID);
VOID           QrtrSummary(VOID);

/* Gpr.c: GPR (AudioReach packet router) on adsp_apps */
VOID           GprInit(VOID);
VOID           GprPoll(VOID);
VOID           GprSummary(VOID);

/* amp.c: read-only speaker amp detection on QUP0 SE1 I2C (shared with TopazBattery) */
VOID           AmpProbe(VOID);
