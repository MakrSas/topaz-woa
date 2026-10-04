/*
 * Hardware side: the lab scripts of docs/P9_audio.md ("FIRST SOUND") in C, through TopazAudio (ta.c).
 *   first Prepare: codec bring-up once (bringup.ps1 + smmu_audio.ps1 + rx_stock.ps1 + wcd_aux.ps1)
 *   Prepare: MEM_MAP ring + pos buffer, graph SH_MEM_PULL_MODE -> CODEC_DMA_SINK (RX_CODEC_DMA_RX_1)
 *   Run:     (prepare again after a stop) start, first time SoundWire ports + AUX PA, amp on, unmute
 *   Pause:   amp off, mute, stop;  Release: close graph, unmap
 * Position: the DSP writes {frame counter, read index, ts} into the pos buffer (read at DISPATCH).
 */
#include "tspk.h"
#include "ta.h"
#include "rxtab.h"

#define RX_MACRO   0x0A600000ULL
#define VA_MACRO   0x0A730000ULL
#define TX_MACRO   0x0A620000ULL
#define RX_SWR     0x0A610000ULL
#define VA_SWR     0x0A740000ULL
#define LPI_TLMM   0x0A7C0000ULL
#define SMMU       0x0C600000ULL

#define SG    0x4001
#define CONT  0x4101
#define DMA   0x7001
#define PULL  0x7002

#define APM_GRAPH_OPEN    0x01001000
#define APM_GRAPH_PREPARE 0x01001001
#define APM_GRAPH_START   0x01001002
#define APM_GRAPH_STOP    0x01001003
#define APM_GRAPH_CLOSE   0x01001004
#define APM_SET_CFG       0x01001006
#define APM_MEM_MAP       0x0100100C
#define APM_MEM_UNMAP     0x0100100D

static BOOLEAN  g_CodecUp, g_PortsUp, g_GraphOpen, g_Stopped, g_Running;
static PULONG   g_PosVa;                       /* 4 KiB DSP position buffer */
static PHYSICAL_ADDRESS g_PosPa;
static ULONG    g_RingMap, g_PosMap;           /* DSP mem map handles */
static ULONG    g_SwrId;
static LONG     g_VolDb = -30;
static BOOLEAN  g_Mute;

/* ---------------- small helpers ---------------- */

static VOID SwrWr(ULONGLONG Master, ULONG Dev, ULONG Reg, ULONG Val)
{
    g_SwrId = (g_SwrId % 14) + 1;               /* SWR_REG_VAL_PACK: reg | id << 16 | dev << 20 | data << 24 */
    TaWr(Master + 0x300, (Val << 24) | (Dev << 20) | (g_SwrId << 16) | Reg);
    TaSleep(1);
}

static NTSTATUS Prm(const ULONG *Dw, ULONG N)
{
    ULONG op = 0, r[4] = { 0 };
    NTSTATUS status = TaGpr(2, 2, 0x0100100F, TRUE, Dw, N, &op, r, 4, 1000);

    if (NT_SUCCESS(status) && r[1] != 0) {
        LogPrint("prm %08x: status %u\r\n", Dw[1], r[1]);
        status = STATUS_UNSUCCESSFUL;
    }
    return status;
}

static VOID InitMaster(ULONGLONG M)
{
    ULONG cfg;

    TaWr(M + 0x008, 1); TaWr(M + 0x008, 1); TaWr(M + 0x1044, 1); TaSleep(2);
    TaWr(M + 0x101C, 0x2F0008); TaWr(M + 0x500, 1);
    cfg = TaRd(M + 0x1048);
    TaWr(M + 0x1048, (cfg & ~0x3E0000u) | (0x1Fu << 17));
    TaWr(M + 0x314, 3); TaWr(M + 0x1044, 2);
    TaWr(M + 0x04, 2); TaWr(M + 0x208, 0xFFFFFFFF); TaWr(M + 0x204, 0x1FDFD); TaWr(M + 0x210, 0x1FDFD);
    TaWr(M + 0x04, 3); TaWr(M + 0x314, 0x80000003);
}

/* identity context bank for the audio DMA stream 0x1C1 (smmu_audio.ps1) */
static NTSTATUS SmmuAudio(VOID)
{
    ULONG id1 = TaRd(SMMU + 0x24), nsmr = TaRd(SMMU + 0x20) & 0xFF, ncb = id1 & 0xFF;
    ULONG psize = (id1 & 0x80000000u) ? 0x10000 : 0x1000, npage = 1u << (((id1 >> 28) & 7) + 1);
    ULONG i, slot = MAXULONG, cb = MAXULONG, smr, s2;
    ULONGLONG used = 0, cbp, gr1 = SMMU + psize;

    for (i = 0; i < nsmr && i < 128; i++) {
        smr = TaRd(SMMU + 0x800 + 4 * i);
        s2 = TaRd(SMMU + 0xC00 + 4 * i);
        if ((smr >> 31) && (smr & 0xFFFF) == 0x1C1) {
            LogPrint("smmu: sid 1c1 already in SMR%u\r\n", i);
            return STATUS_SUCCESS;
        }
        if (!(smr >> 31) && slot == MAXULONG) {
            slot = i;
        }
        if ((smr >> 31) && ((s2 >> 16) & 3) == 0) {
            used |= 1ull << (s2 & 0x3F);
        }
    }
    for (i = 4; i < ncb && i < 64; i++) {
        if (!(used & (1ull << i))) {
            cb = i;
            break;
        }
    }
    if (slot == MAXULONG || cb == MAXULONG) {
        LogPrint("smmu: no free SMR/CB\r\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    cbp = SMMU + (ULONGLONG)(npage + cb) * psize;
    TaWr(cbp + 0x58, 0xFFFFFFFF); TaWr(cbp + 0x20, 0); TaWr(cbp + 0x24, 0); TaWr(cbp + 0x30, 0);
    TaWr(cbp + 0x10, 0); TaWr(cbp + 0x38, 0); TaWr(cbp + 0x3C, 0);
    TaWr(gr1 + 0x800 + 4 * cb, 1); TaWr(gr1 + 4 * cb, 0x1F000); TaWr(cbp, 0xE0);
    TaWr(SMMU + 0xC00 + 4 * slot, cb); TaWr(SMMU + 0x800 + 4 * slot, 0x80000000u | 0x1C1);
    TaWr(SMMU + 0x70, 0);
    LogPrint("smmu: sid 1c1 -> SMR%u CB%u, SMR %08x\r\n", slot, cb, TaRd(SMMU + 0x800 + 4 * slot));
    return STATUS_SUCCESS;
}

static VOID ApplyVolume(VOID)
{
    LONG db = g_Mute ? -84 : g_VolDb;

    if (g_CodecUp) {
        TaWr(RX_MACRO + 0x514, (ULONG)db & 0xFF);   /* RX_RX2_RX_VOL_CTL, s8 dB */
    }
}

/* ---------------- codec bring-up (once per boot) ---------------- */

static NTSTATUS CodecBringUp(VOID)
{
    static const ULONG pins[6] = { 0xD04, 0xD06, 0xD06, 0xD04, 0xD06, 0xD06 };
    ULONG dw[9], i;
    NTSTATUS status;

    if (!TaAdspReady()) {
        LogPrint("bring-up: ADSP / APM not ready (C:\\topaz\\audio.arm + reboot?)\r\n");
        return STATUS_DEVICE_NOT_READY;
    }
    /* 1. PRM: DCODEC vote, TX core + TX NPL clocks only (stock) */
    dw[0] = 2; dw[1] = 0x08001032; dw[2] = 4; dw[3] = 0; dw[4] = 2;
    status = Prm(dw, 5);
    for (i = 0; i < 2 && NT_SUCCESS(status); i++) {
        dw[0] = 2; dw[1] = 0x0800102C; dw[2] = 20; dw[3] = 0; dw[4] = 1;
        dw[5] = 0x30C + i; dw[6] = 19200000; dw[7] = 1; dw[8] = 0;
        status = Prm(dw, 9);
    }
    LogPrint("bring-up: prm votes/clocks %08x\r\n", status);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    /* 2. LPI pins + slew */
    for (i = 0; i < 6; i++) {
        TaWr(LPI_TLMM + 0x1000 * i, pins[i]);
    }
    TaWr(0x0A95A000, 0x3F3F);
    /* 3. VA macro fs-gen + SWR clock */
    TaWr(VA_MACRO, 1); TaWr(VA_MACRO + 4, 3); TaWr(VA_MACRO + 4, 1); TaWr(VA_MACRO + 0x80, 2);
    TaWr(VA_MACRO + 8, 2); TaWr(VA_MACRO + 8, 3); TaWr(VA_MACRO + 8, 1);
    /* 4. RX macro MCLK, FS, SWR clock; RX CGCR */
    TaWr(RX_MACRO + 0x100, 3); TaWr(RX_MACRO + 0x104, 3); TaWr(RX_MACRO + 0x104, 1);
    TaWr(RX_MACRO + 0x108, 2); TaWr(RX_MACRO + 0x108, 3); TaWr(RX_MACRO + 0x108, 1);
    TaWr(0x0A6A9098, 3); TaSleep(1); TaWr(0x0A6A9098, 1);
    /* 5. TX macro MCLK, FS, SWR clock; VA/TX CGCR */
    TaWr(TX_MACRO, 1); TaWr(TX_MACRO + 4, 3); TaWr(TX_MACRO + 4, 1);
    TaWr(TX_MACRO + 8, 2); TaWr(TX_MACRO + 8, 3); TaWr(TX_MACRO + 8, 1);
    TaWr(0x0A7EC100, 3); TaSleep(1); TaWr(0x0A7EC100, 1);
    /* 6. WCD937x reset (TLMM gpio92) low while both masters start, then high */
    TaWr(0x0055C004, 0); TaWr(0x0055C000, 0x200);
    InitMaster(RX_SWR);
    InitMaster(VA_SWR);
    TaSleep(20);
    TaWr(0x0055C004, 2);
    TaSleep(300);
    LogPrint("bring-up: swr slaves rx %x va %x, ids %08x %08x\r\n", TaRd(RX_SWR + 0x1090), TaRd(VA_SWR + 0x1090),
             TaRd(RX_SWR + 0x538), TaRd(VA_SWR + 0x538));
    /* 7. audio DMA SMMU identity */
    status = SmmuAudio();
    if (!NT_SUCCESS(status)) {
        return status;
    }
    /* 8. RX macro registers as on stock (RX2 path stays muted until Run) */
    for (i = 0; i < ARRAYSIZE(g_RxStock); i++) {
        TaWr(RX_MACRO + g_RxStock[i][0], g_RxStock[i][1]);
    }
    TaWr(RX_MACRO + 0x400, 4); TaWr(RX_MACRO + 0x480, 4); TaWr(RX_MACRO + 0x500, 4);
    /* 9. WCD937x init + RX clocks + AUX DAC (TX slave on the VA bus, dev 1) - wcd_aux.ps1 */
    SwrWr(VA_SWR, 1, 0x3103, 0xD6); TaSleep(2);
    SwrWr(VA_SWR, 1, 0x312A, 0x00);
    SwrWr(VA_SWR, 1, 0x3029, 0x85);
    SwrWr(VA_SWR, 1, 0x3001, 0xC0); TaSleep(10); SwrWr(VA_SWR, 1, 0x3001, 0x80);
    SwrWr(VA_SWR, 1, 0x30E2, 0xD9);
    SwrWr(VA_SWR, 1, 0x30A5, 0xEB);
    SwrWr(VA_SWR, 1, 0x3409, 0x08); SwrWr(VA_SWR, 1, 0x3408, 0x01); SwrWr(VA_SWR, 1, 0x3008, 0x01);
    SwrWr(VA_SWR, 1, 0x340D, 0xBC); SwrWr(VA_SWR, 1, 0x340E, 0xBC); SwrWr(VA_SWR, 1, 0x340F, 0xBC);
    SwrWr(VA_SWR, 1, 0x3408, 0x03);
    SwrWr(VA_SWR, 1, 0x3408, 0x07); SwrWr(VA_SWR, 1, 0x3409, 0x0C); SwrWr(VA_SWR, 1, 0x344F, 0x01);
    SwrWr(VA_SWR, 1, 0x3009, 0x0C); SwrWr(VA_SWR, 1, 0x300A, 0x40); SwrWr(VA_SWR, 1, 0x300B, 0x12);
    SwrWr(VA_SWR, 1, 0x3008, 0x41); SwrWr(VA_SWR, 1, 0x3008, 0xC1);
    g_CodecUp = TRUE;
    ApplyVolume();
    LogPrint("bring-up: codec ready\r\n");
    return STATUS_SUCCESS;
}

/* SoundWire data ports (ports.ps1) + AUX PA (wcd_auxpa.ps1): after the first graph start */
static VOID PortsAndPa(VOID)
{
    static const ULONG slv[][2] = { { 0x203, 7 }, { 0x230, 1 }, { 0x232, 0x1F }, { 0x233, 0 }, { 0x234, 0 },
                                    { 0x236, 0x36 }, { 0x430, 1 }, { 0x432, 7 }, { 0x433, 0 }, { 0x434, 1 } };
    ULONG i;

    TaWr(RX_SWR + 0x1264, 0x0100001F); TaWr(RX_SWR + 0x127C, 0); TaWr(RX_SWR + 0x122C, 7); TaWr(RX_SWR + 0x1274, 0x63);
    TaWr(RX_SWR + 0x1464, 0x01000107); TaWr(RX_SWR + 0x147C, 0); TaWr(RX_SWR + 0x1474, 0);
    for (i = 0; i < ARRAYSIZE(slv); i++) {
        SwrWr(RX_SWR, 1, slv[i][0], slv[i][1]);
    }
    TaWr(RX_SWR + 0x105C, 0x5000F);
    TaWr(RX_SWR + 0x300, (0x0Fu << 24) | (15u << 20) | (15u << 16) | 0x70);   /* broadcast bank switch */
    TaSleep(5);
    SwrWr(VA_SWR, 1, 0x3467, 0x05);
    SwrWr(VA_SWR, 1, 0x3128, 0x80);
    SwrWr(VA_SWR, 1, 0x346B, 0x4C);
    SwrWr(VA_SWR, 1, 0x346C, 0x7F);
    SwrWr(VA_SWR, 1, 0x346D, 0x0F);
    LogPrint("ports: rx frame b1 %08x, irq %08x\r\n", TaRd(RX_SWR + 0x105C), TaRd(RX_SWR + 0x200));
}

static VOID AmpOn(BOOLEAN On)
{
    static const UCHAR regs[][2] = { { 1, 0x7c }, { 2, 0x21 }, { 5, 0x01 }, { 3, 0x30 }, { 4, 0xc5 },
                                     { 6, 0x48 }, { 7, 0x5d }, { 8, 0x8a }, { 9, 0x0e }, { 10, 0xa4 } };
    ULONG i;
    NTSTATUS status = STATUS_SUCCESS;

    if (On) {
        for (i = 0; i < ARRAYSIZE(regs) && NT_SUCCESS(status); i++) {
            status = TaI2cWrite(0x2b, regs[i][0], regs[i][1]);
        }
    } else {
        status = TaI2cWrite(0x2b, 5, 0);
    }
    LogPrint("amp %s: %08x\r\n", On ? "on" : "off", status);
}

/* ---------------- DSP graph ---------------- */

static NTSTATUS MemMap(PHYSICAL_ADDRESS Pa, ULONG Bytes, ULONG Prop, ULONG *Handle)
{
    ULONG base = (ULONG)Pa.QuadPart & ~0xFFFu, end = ((ULONG)Pa.QuadPart + Bytes + 0xFFF) & ~0xFFFu;
    ULONG dw[5] = { 3 | (1u << 16), Prop, base, 1, end - base }, op = 0, r[4] = { 0 };
    NTSTATUS status = TaGpr(1, 1, APM_MEM_MAP, FALSE, dw, 5, &op, r, 4, 1000);

    if (NT_SUCCESS(status) && op != 0x02001001) {
        LogPrint("mem map %x: reply %08x status %u\r\n", base, op, r[1]);
        status = STATUS_UNSUCCESSFUL;
    }
    *Handle = NT_SUCCESS(status) ? r[0] : 0;
    return status;
}

static VOID MemUnmap(ULONG *Handle)
{
    if (*Handle != 0) {
        (VOID)TaApm(APM_MEM_UNMAP, FALSE, Handle, 1, NULL, 0);
        *Handle = 0;
    }
}

static NTSTATUS SgCmd(ULONG Opcode)
{
    ULONG dw[6] = { 1, 0x08001005, 8, 0, 1, SG };

    return TaApm(Opcode, TRUE, dw, 6, NULL, 0);
}

NTSTATUS HwStreamPrepare(WDFDEVICE Device, PHYSICAL_ADDRESS RingPa, ULONG RingBytes, ULONG Channels, ULONG Rate)
{
    static const ULONG open[] = {
        1, 0x08001001, 48, 0,  1,  SG, 3,  0x0800100E, 4, 2,  0x0800100F, 4, 1,  0x08001010, 4, 1,
        1, 0x08001000, 64, 0,  1,  CONT, 4,  0x08001011, 8, 1, 3,  0x08001012, 4, 1,  0x08001013, 4, 8192,  0x08001014, 4, 2,
        1, 0x08001002, 32, 0,  1,  SG, CONT, 2,  0x07001006, PULL,  0x07001023, DMA,
        1, 0x08001003, 56, 0,  2,  PULL, 1, 0x08001015, 8, 0, 1,  DMA, 1, 0x08001015, 8, 1, 0,  0,
        1, 0x08001004, 24, 0,  1,  PULL, 1, DMA, 2,  0
    };
    ULONG mf[12], pc[12], ep[8], intf[8];
    PHYSICAL_ADDRESS lo, hi, bound;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Device);
    if (Rate != 48000 || (Channels != 1 && Channels != 2)) {
        return STATUS_NOT_SUPPORTED;
    }
    if (!g_CodecUp) {
        status = CodecBringUp();
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }
    if (g_PosVa == NULL) {
        lo.QuadPart = 0; hi.QuadPart = 0xEFFFFFFF; bound.QuadPart = 0;
        g_PosVa = MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, lo, hi, bound, MmNonCached);
        if (g_PosVa == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(g_PosVa, PAGE_SIZE);
        g_PosPa = MmGetPhysicalAddress(g_PosVa);
    }
    status = MemMap(RingPa, RingBytes, 0, &g_RingMap);
    if (NT_SUCCESS(status)) {
        status = MemMap(g_PosPa, PAGE_SIZE, 2, &g_PosMap);
    }
    if (NT_SUCCESS(status)) {
        status = TaApm(APM_GRAPH_OPEN, TRUE, open, ARRAYSIZE(open), NULL, 0);
        g_GraphOpen = NT_SUCCESS(status);
    }
    /* PULL media format: PCM 16-bit, Channels, LSB aligned, Q15, LE, channel map 1(,2) */
    mf[0] = PULL; mf[1] = 0x0800100C; mf[2] = 32; mf[3] = 0;
    mf[4] = 1; mf[5] = 0x09001000; mf[6] = 20; mf[7] = Rate;
    mf[8] = 16 | (1u << 16); mf[9] = 16 | (15u << 16); mf[10] = 1 | (Channels << 16);
    mf[11] = (Channels == 2) ? (1 | (2u << 8)) : 1;
    pc[0] = PULL; pc[1] = 0x0800100A; pc[2] = 28; pc[3] = 0;
    pc[4] = (ULONG)RingPa.QuadPart; pc[5] = 1; pc[6] = RingBytes; pc[7] = g_RingMap;
    pc[8] = (ULONG)g_PosPa.QuadPart; pc[9] = 1; pc[10] = g_PosMap; pc[11] = 0;
    ep[0] = DMA; ep[1] = 0x08001017; ep[2] = 12; ep[3] = 0; ep[4] = Rate; ep[5] = 16 | (Channels << 16); ep[6] = 1; ep[7] = 0;
    intf[0] = DMA; intf[1] = 0x08001063; intf[2] = 12; intf[3] = 0; intf[4] = 1; intf[5] = 2;   /* RXTX, RX_1 */
    intf[6] = (Channels == 2) ? 3 : 1; intf[7] = 0;
    if (NT_SUCCESS(status)) status = TaApm(APM_SET_CFG, TRUE, mf, 12, NULL, 0);
    if (NT_SUCCESS(status)) status = TaApm(APM_SET_CFG, TRUE, pc, 12, NULL, 0);
    if (NT_SUCCESS(status)) status = TaApm(APM_SET_CFG, TRUE, ep, 8, NULL, 0);
    if (NT_SUCCESS(status)) status = TaApm(APM_SET_CFG, TRUE, intf, 8, NULL, 0);
    if (NT_SUCCESS(status)) status = SgCmd(APM_GRAPH_PREPARE);
    g_Stopped = FALSE;
    LogPrint("hw prepare: ring %llx + %x, %u ch: %08x (maps %x %x)\r\n", (ULONGLONG)RingPa.QuadPart, RingBytes,
             Channels, status, g_RingMap, g_PosMap);
    if (!NT_SUCCESS(status)) {
        HwStreamRelease(Device);
    }
    return status;
}

NTSTATUS HwStreamRun(WDFDEVICE Device)
{
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Device);
    if (!g_GraphOpen) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (g_Stopped) {
        (VOID)SgCmd(APM_GRAPH_PREPARE);
        g_Stopped = FALSE;
    }
    status = SgCmd(APM_GRAPH_START);
    if (NT_SUCCESS(status)) {
        if (!g_PortsUp) {
            PortsAndPa();
            g_PortsUp = TRUE;
        }
        AmpOn(TRUE);
        ApplyVolume();
        TaWr(RX_MACRO + 0x518, 0x04);
        TaWr(RX_MACRO + 0x500, 0x24);                   /* RX2 path on */
        g_Running = TRUE;
    }
    LogPrint("hw run: %08x\r\n", status);
    return status;
}

NTSTATUS HwStreamPause(WDFDEVICE Device)
{
    UNREFERENCED_PARAMETER(Device);
    if (!g_Running) {
        return STATUS_SUCCESS;
    }
    g_Running = FALSE;
    AmpOn(FALSE);
    TaWr(RX_MACRO + 0x500, 0x04);
    (VOID)SgCmd(APM_GRAPH_STOP);
    g_Stopped = TRUE;
    LogPrint("hw pause\r\n");
    return STATUS_SUCCESS;
}

VOID HwStreamRelease(WDFDEVICE Device)
{
    UNREFERENCED_PARAMETER(Device);
    if (g_Running) {
        (VOID)HwStreamPause(Device);
    }
    if (g_GraphOpen) {
        if (!g_Stopped) {
            (VOID)SgCmd(APM_GRAPH_STOP);
        }
        (VOID)SgCmd(APM_GRAPH_CLOSE);
        g_GraphOpen = FALSE;
    }
    MemUnmap(&g_RingMap);
    MemUnmap(&g_PosMap);
    LogPrint("hw release\r\n");
}

BOOLEAN HwReadPosition(ULONG *ByteIndex)
{
    if (!g_Running || g_PosVa == NULL) {
        *ByteIndex = 0;
        return FALSE;
    }
    *ByteIndex = ((volatile ULONG *)g_PosVa)[1];     /* {frame_counter, index, ts lo, ts hi} */
    return TRUE;
}

VOID HwSetVolume(WDFDEVICE Device, LONG Level, BOOLEAN Mute)
{
    UNREFERENCED_PARAMETER(Device);
    g_VolDb = Level / 65536;
    g_Mute = Mute;
    ApplyVolume();
    LogPrint("volume %d dB%s\r\n", g_VolDb, Mute ? " (muted)" : "");
}
