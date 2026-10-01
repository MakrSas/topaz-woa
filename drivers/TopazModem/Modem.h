/*
 * Windows version of uefi/TopazOtgDxe/Modem.h. Same API; the only difference is that physical
 * windows (SMEM, APCS IPC) are mapped by ModemMapInit() and used through VAs.
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

#pragma pack(1)
typedef struct { UINT32 Offset, Size, Flags; UINT16 Host0, Host1; UINT32 Cacheline, Rsvd[7]; } SMEM_PT_ENTRY;
typedef struct { UINT8 Magic[4]; UINT32 Version, NumEntries, Rsvd[5]; } SMEM_PT;
typedef struct { UINT8 Magic[4]; UINT16 Host0, Host1; UINT32 Size, FreeUncached, FreeCached, Rsvd[3]; } SMEM_PART_HDR;
typedef struct { UINT16 Canary, Item; UINT32 Size; UINT16 PadData, PadHdr; UINT32 Rsvd; } SMEM_PRIV_ENTRY;
typedef struct { UINT32 Allocated, Offset, Size, AuxBase; } SMEM_GLOBAL_ENTRY;
#pragma pack()

VOID           ModemOut(CONST CHAR8 *Fmt, ...);
SMEM_PART_HDR *SmemPartition(UINT16 A, UINT16 B);
VOID          *SmemPrivGet(SMEM_PART_HDR *P, UINT16 Item, UINT32 *Size);
VOID          *SmemPrivAlloc(SMEM_PART_HDR *P, UINT16 Item, UINT32 Size);
UINT32         SmemVersion(VOID);
VOID          *SmemGlobalGet(UINT16 Item, UINT32 *Size);
VOID           ApcsKick(UINT32 Bit);

/* GLINK over SMEM + QRTR towards the modem (remote pid 1). Seconds = 0: until gModemStop. */
VOID           GlinkQrtrSpike(UINTN Seconds, EFI_FILE_PROTOCOL *Root);
VOID           QrtrSend(UINT32 Type, UINT32 SrcPort, UINT32 DstNode, UINT32 DstPort, CONST VOID *Payload, UINT32 Len);
UINT32         QrtrModemNode(VOID);
UINTN          ModemMs(VOID);
extern volatile UINT32 *gModemState;
extern UINTN           gTrace;

/* Host services the modem expects from apps (ModemSvc.c): pd-mapper, tftp, rmtfs */
VOID           SvcInit(EFI_FILE_PROTOCOL *Root);
VOID           SvcAnnounce(VOID);
BOOLEAN        SvcRx(UINT32 SrcNode, UINT32 SrcPort, UINT32 DstPort, CONST UINT8 *Data, UINT32 Len);
VOID           SvcSummary(VOID);
UINTN          ScmAssignToModem(UINT64 Addr, UINT64 Size);
UINTN          ScmAssign(UINT64 Addr, UINT64 Size, CONST UINT32 *Vmids, UINT32 Count);
VOID           WlfwArrive(UINT32 Node, UINT32 Port);
VOID           WlfwRx(CONST UINT8 *Data, UINT32 Len);
VOID           WlfwSummary(VOID);
CONST CHAR8   *WlfwStep(VOID);
EFI_FILE_PROTOCOL *SvcRoot(VOID);

#define PORT_WLFWC         0x4004             /* our QMI client port towards WLFW */

/* ---- Windows only (port.c / ModemPas.c / driver.c) ---- */
EFI_STATUS     ModemPasTest(VOID);
EFI_STATUS     ModemMapInit(VOID);                /* SMEM, APCS IPC, GICD */
VOID          *MapPhys(UINT64 Pa, UINTN Size, BOOLEAN Wc); /* Wc: Normal NC, else Device */
VOID           UnmapPhys(VOID *Va, UINTN Size);
VOID           ModemIdle(BOOLEAN Busy);           /* 1 ms sleep when idle, short stall when busy */
VOID           LogSetLazy(BOOLEAN Lazy);          /* lazy: buffer lines, flush from ModemIdle() */
VOID           LogFlush(VOID);
extern volatile BOOLEAN gModemStop;

/* P4 probes (wlanprobe.c), read-only */
VOID           SmmuProbe(BOOLEAN All);
VOID           CeProbe(VOID);
BOOLEAN        SmmuWlanMap(VOID);

/* P4 data path: copy engines (ce.c) + HTC (htc.c), polled from the GLINK loop */
BOOLEAN        CeStart(VOID);                     /* after WLAN_MODE mission */
BOOLEAN        CePoll(VOID);                      /* TRUE if something was received */
BOOLEAN        CeSend(UINT32 Ce, CONST VOID *Data, UINT32 Len, UINT32 TransferId);
VOID           CeSummary(VOID);
VOID           HtcRx(UINT32 Ce, CONST UINT8 *Data, UINT32 Len);
VOID           HtcSummary(VOID);
BOOLEAN        HtcWmiSend(CONST VOID *Data, UINT32 Len);  /* wmi_cmd_hdr + TLVs */
VOID           WmiRx(CONST UINT8 *Data, UINT32 Len);       /* wmi.c */
VOID           WmiSummary(VOID);
VOID           WlfwSetStep(CONST CHAR8 *S);
