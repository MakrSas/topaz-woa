/*
 * TopazRpm - RPM requests of Redmi Note 12 4G (topaz/tapas, SM6225 "khaje") under Windows.
 *
 * The RPM (resource power manager) owns the clocks and rails that the AP votes for: the IPA clock
 * (needed before IPA/GSI registers can be touched - SIM / mobile data, docs/P9_sim.md), the GPU
 * rails, bus bandwidth. Stock DT: rpm-glink (qcom,glink-rpm) over the RPM message RAM
 * memory@045f0000 (0x7000 bytes), interrupt SPI 0xC2 from the RPM, doorbell = APCS IPC bit 0
 * (mboxes = <&apcs 0>), channel "rpm_requests" (qcom,rpm-smd).
 * Layout (Linux drivers/rpmsg/qcom_glink_rpm.c): a 256-byte TOC at the end of the message RAM,
 * magic "grt0", entries {id, offset, size}; "ap2r" = AP->RPM FIFO, "r2ap" = RPM->AP FIFO, each
 * {u32 tail, u32 head, data[size]} at offset. GLINK native protocol on top, intentless.
 *
 * v0.1: read only - logs the TOC, both FIFOs' indices and the bytes waiting in them
 * (C:\TopazRpm.log). Result: both FIFOs empty (tail = head = 0), nobody used the link before us.
 * v0.2: with C:\topaz\rpm.on - GLINK VERSION handshake, OPEN "rpm_requests", one request: IPA
 * clock 100 MHz (Linux clk-smd-rpm: resource "ipa" 0x617069 id 0, key "KHz"), active set; logs
 * the RPM's answer ("msg#" ack or "err" string). Message format = Linux qcom_smd-rpm.c.
 * Result: VERSION/VERSION_ACK, RPM opens rpm_requests (rcid 3) + glink_ssr, IPA clock acked.
 * v0.3: with C:\topaz\ipa.probe, after an acked vote, read a few IPA / GSI registers (first
 * access to IPA). Layout as mainline sc7180 (IPA v4.2): ipa-reg = ipa-base + 0x40000 = 0x5840000,
 * shared SRAM 0x5847000, gsi = 0x5804000 (DT gsi-base); registers from drivers/net/ipa/reg.
 * v0.3 result: installed with devcon update over a running v0.2, the second VERSION on the live
 * link got only a VERSION_ACK and the whole SoC went down (RPM crash) before the IPA probe ran.
 * v0.4: never re-handshake - if the FIFO indices are not both 0 the link was used since boot,
 * so the driver only logs. TopazRpm must be updated by file swap + reboot, never restarted.
 * v0.5: C:\topaz\ipa.probe is one-shot (deleted before the probe), so a hang costs one reboot.
 * v0.6: after an acked IPA vote, publish it for this boot in a volatile registry key
 * (HKLM\SYSTEM\CurrentControlSet\Services\TopazRpm\State, IpaClockKhz): TopazWifi waits for it
 * before it touches IPA/GSI (ipa_fws load ahead of the modem). Volatile = gone after a reboot.
 */
#include "driver.h"

#define TOPAZ_RPM_VERSION   "v0.6"

#define MSG_RAM_PA          0x045F0000ULL
#define MSG_RAM_SIZE        0x7000
#define TOC_SIZE            256
#define TOC_MAGIC           0x67727430u     /* "grt0" */
#define FIFO_AP2R           0x61703272u     /* "ap2r" */
#define FIFO_R2AP           0x72326170u     /* "r2ap" */

#define APCS_IPC_PA         0x0F111008ULL   /* apcs_glb + 8, bit 0 = RPM */

#define CMD_VERSION         0
#define CMD_VERSION_ACK     1
#define CMD_OPEN            2
#define CMD_CLOSE           3
#define CMD_OPEN_ACK        4
#define CMD_TX_DATA         9
#define CMD_CLOSE_ACK       11
#define CMD_TX_DATA_CONT    12
#define CMD_READ_NOTIF      13
#define CMD_SIGNALS         15

#define CHAN_NAME           "rpm_requests"
#define CHAN_LCID           1

#define RPM_SERVICE_REQ     0x00716572u     /* "req" */
#define RPM_MSG_ID          0x2367736Du     /* "msg#" */
#define RPM_MSG_ERR         0x00727265u     /* "err" */
#define RPM_ACTIVE_SET      0
#define RPM_RES_IPA_CLK     0x00617069u     /* "ipa" */
#define RPM_KEY_RATE        0x007A484Bu     /* "KHz" */
#define IPA_CLK_KHZ         100000          /* ipa_data-v4.2.c core clock 100 MHz */

typedef struct _FIFO {
    ULONG Off, Size;                        /* {tail, head} at Off, data at Off + 8 */
} FIFO;

typedef struct _DEVICE_CONTEXT {
    volatile UCHAR *Ram;
    volatile ULONG *Ipc;
    FIFO    Tx, Rx;
    BOOLEAN VersionDone, OpenAck, RemoteOpen;
    USHORT  Rcid;
    ULONG   MsgId;
    LONG    LastAck;                        /* msg id of the last "msg#" from the RPM, -1 none */
    BOOLEAN LastErr;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_UNLOAD           EvtDriverUnload;
static EVT_WDF_DRIVER_DEVICE_ADD       EvtDeviceAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE EvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE EvtReleaseHardware;

static ULONG Rd(PDEVICE_CONTEXT Ctx, ULONG Off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(Ctx->Ram + Off));
}

static VOID Wr(PDEVICE_CONTEXT Ctx, ULONG Off, ULONG Val)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(Ctx->Ram + Off), Val);
}

static VOID SleepMs(ULONG Ms)
{
    LARGE_INTEGER t;
    t.QuadPart = -(LONGLONG)Ms * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

static BOOLEAN FileExists(PCWSTR Path)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;

    RtlInitUnicodeString(&name, Path);
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&h, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                                 FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0))) {
        return FALSE;
    }
    ZwClose(h);
    return TRUE;
}

static BOOLEAN DeleteFile(PCWSTR Path)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;

    RtlInitUnicodeString(&name, Path);
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&h, DELETE | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                                 FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE, NULL, 0))) {
        return FALSE;
    }
    ZwClose(h);
    return TRUE;
}

/* ---- GLINK over the message RAM (word accesses only) ------------------------- */

/* Msg = whole message, Len a multiple of 8 (glink pads every message to 8 bytes) */
static BOOLEAN TxSend(PDEVICE_CONTEXT Ctx, const VOID *Msg, ULONG Len)
{
    ULONG tail = Rd(Ctx, Ctx->Tx.Off), head = Rd(Ctx, Ctx->Tx.Off + 4), avail, i;

    avail = (tail <= head) ? Ctx->Tx.Size - head + tail : tail - head;
    if ((Len & 7) != 0 || avail <= Len) {
        LogPrint("tx: no room (%u bytes, head %x tail %x)\n", Len, head, tail);
        return FALSE;
    }
    for (i = 0; i < Len; i += 4) {
        Wr(Ctx, Ctx->Tx.Off + 8 + head, *(const ULONG *)((const UCHAR *)Msg + i));
        head += 4;
        if (head >= Ctx->Tx.Size) {
            head -= Ctx->Tx.Size;
        }
    }
    KeMemoryBarrier();
    Wr(Ctx, Ctx->Tx.Off + 4, head);
    KeMemoryBarrier();
    WRITE_REGISTER_ULONG(Ctx->Ipc, 1u << 0);
    return TRUE;
}

static VOID SendCmd(PDEVICE_CONTEXT Ctx, USHORT Cmd, USHORT P1, ULONG P2)
{
    ULONG m[2] = { (ULONG)Cmd | ((ULONG)P1 << 16), P2 };
    TxSend(Ctx, m, sizeof(m));
}

static ULONG RxAvail(PDEVICE_CONTEXT Ctx)
{
    ULONG tail = Rd(Ctx, Ctx->Rx.Off), head = Rd(Ctx, Ctx->Rx.Off + 4);
    return (head >= tail) ? head - tail : Ctx->Rx.Size - tail + head;
}

static VOID RxPeek(PDEVICE_CONTEXT Ctx, ULONG At, VOID *Dst, ULONG Len)
{
    ULONG tail = Rd(Ctx, Ctx->Rx.Off), i;

    for (i = 0; i < Len; i += 4) {
        ULONG pos = (tail + At + i) % Ctx->Rx.Size;
        *(ULONG *)((UCHAR *)Dst + i) = Rd(Ctx, Ctx->Rx.Off + 8 + pos);
    }
}

static VOID RxAdvance(PDEVICE_CONTEXT Ctx, ULONG Len)
{
    ULONG tail = Rd(Ctx, Ctx->Rx.Off) + Len;
    if (tail >= Ctx->Rx.Size) {
        tail -= Ctx->Rx.Size;
    }
    Wr(Ctx, Ctx->Rx.Off, tail);
}

/* RPM reply payload: {service_type, length} then messages {type, length, data} */
static VOID RpmReply(PDEVICE_CONTEXT Ctx, const UCHAR *P, ULONG Len)
{
    ULONG o = 8;

    if (Len < 8 || *(const ULONG *)P != RPM_SERVICE_REQ) {
        LogHex("rpm: unexpected reply:", P, min(Len, 32));
        return;
    }
    while (o + 8 <= Len) {
        ULONG type = *(const ULONG *)(P + o), l = *(const ULONG *)(P + o + 4);
        if (type == RPM_MSG_ID && l >= 4) {
            Ctx->LastAck = (LONG)*(const ULONG *)(P + o + 8);
            LogPrint("rpm: ack msg %u\n", Ctx->LastAck);
        } else if (type == RPM_MSG_ERR) {
            CHAR e[64];
            ULONG n = min(l, (ULONG)sizeof(e) - 1);
            RtlCopyMemory(e, P + o + 8, min(n, Len - o - 8));
            e[n] = 0;
            Ctx->LastErr = TRUE;
            LogPrint("rpm: ERROR \"%s\"\n", e);
        } else {
            LogPrint("rpm: message type %08x len %u\n", type, l);
        }
        o += 8 + ((l + 3) & ~3u);
    }
}

/* one message from r2ap; FALSE when nothing (complete) is waiting */
static BOOLEAN RxOne(PDEVICE_CONTEXT Ctx)
{
    ULONG avail = RxAvail(Ctx), m[2], n;
    USHORT cmd, p1;

    if (avail < 8) {
        return FALSE;
    }
    RxPeek(Ctx, 0, m, 8);
    cmd = (USHORT)m[0];
    p1  = (USHORT)(m[0] >> 16);
    switch (cmd) {
    case CMD_VERSION:
        LogPrint("glink: VERSION %u features %x\n", p1, m[1]);
        RxAdvance(Ctx, 8);
        SendCmd(Ctx, CMD_VERSION_ACK, 1, 0);
        Ctx->VersionDone = TRUE;
        break;
    case CMD_VERSION_ACK:
        LogPrint("glink: VERSION_ACK %u features %x\n", p1, m[1]);
        RxAdvance(Ctx, 8);
        Ctx->VersionDone = TRUE;
        break;
    case CMD_OPEN: {
        CHAR name[36];
        n = (8 + m[1] + 7) & ~7u;
        if (avail < n || m[1] > 32) {
            return FALSE;
        }
        RtlZeroMemory(name, sizeof(name));
        RxPeek(Ctx, 8, name, (m[1] + 3) & ~3u);
        RxAdvance(Ctx, n);
        LogPrint("glink: RPM OPEN \"%s\" rcid %u\n", name, p1);
        if (strcmp(name, CHAN_NAME) == 0) {
            Ctx->Rcid = p1;
            Ctx->RemoteOpen = TRUE;
            SendCmd(Ctx, CMD_OPEN_ACK, p1, 0);
        }
        break;
    }
    case CMD_OPEN_ACK:
        LogPrint("glink: OPEN_ACK lcid %u\n", p1);
        RxAdvance(Ctx, 8);
        Ctx->OpenAck = (p1 == CHAN_LCID);
        break;
    case CMD_TX_DATA:
    case CMD_TX_DATA_CONT: {
        ULONG c[2];
        UCHAR buf[256];
        if (avail < 16) {
            return FALSE;
        }
        RxPeek(Ctx, 8, c, 8);                       /* chunk_size, left_size */
        n = (16 + c[0] + 7) & ~7u;
        if (avail < n) {
            return FALSE;
        }
        if (c[0] <= sizeof(buf)) {
            RxPeek(Ctx, 16, buf, (c[0] + 3) & ~3u);
            RpmReply(Ctx, buf, c[0]);
        } else {
            LogPrint("glink: data chunk %u too big\n", c[0]);
        }
        RxAdvance(Ctx, n);
        break;
    }
    case CMD_CLOSE:
        LogPrint("glink: RPM CLOSE rcid %u\n", p1);
        RxAdvance(Ctx, 8);
        SendCmd(Ctx, CMD_CLOSE_ACK, p1, 0);
        Ctx->RemoteOpen = FALSE;
        break;
    case CMD_CLOSE_ACK:
    case CMD_READ_NOTIF:
    case CMD_SIGNALS:
        LogPrint("glink: cmd %u p1 %u p2 %x\n", cmd, p1, m[1]);
        RxAdvance(Ctx, 8);
        break;
    default:
        LogPrint("glink: unknown cmd %u p1 %u p2 %x, dropping the FIFO\n", cmd, p1, m[1]);
        RxAdvance(Ctx, avail);
        break;
    }
    return TRUE;
}

/* poll r2ap until Done() or Ms elapse */
static BOOLEAN Wait(PDEVICE_CONTEXT Ctx, BOOLEAN (*Done)(PDEVICE_CONTEXT), ULONG Ms)
{
    ULONG t;

    for (t = 0; t <= Ms; t++) {
        while (RxOne(Ctx)) {
        }
        if (Done(Ctx)) {
            return TRUE;
        }
        SleepMs(1);
    }
    return FALSE;
}

static BOOLEAN IsVersion(PDEVICE_CONTEXT Ctx) { return Ctx->VersionDone; }
static BOOLEAN IsOpen(PDEVICE_CONTEXT Ctx) { return Ctx->OpenAck && Ctx->RemoteOpen; }
static BOOLEAN IsAnswered(PDEVICE_CONTEXT Ctx) { return Ctx->LastAck == (LONG)Ctx->MsgId || Ctx->LastErr; }

/* one key/value request (qcom_rpm_smd_write) in the given set; TRUE if acked without error */
static BOOLEAN RpmWrite(PDEVICE_CONTEXT Ctx, ULONG Set, ULONG Type, ULONG Id, ULONG Key, ULONG Value)
{
    struct {
        USHORT Cmd, Lcid; ULONG Iid; ULONG Chunk, Left;     /* glink TX_DATA */
        ULONG Service, Length;                              /* qcom_rpm_header */
        ULONG MsgId, Flags, Type, Id, DataLen;              /* qcom_rpm_request */
        ULONG Key, KeyLen, Value;                           /* data: 16 + 40 = 56 bytes, 8-aligned */
    } m;

    RtlZeroMemory(&m, sizeof(m));
    Ctx->MsgId++;
    Ctx->LastErr = FALSE;
    m.Cmd = CMD_TX_DATA; m.Lcid = CHAN_LCID; m.Iid = 0;
    m.Chunk = 8 + 20 + 12; m.Left = 0;
    m.Service = RPM_SERVICE_REQ; m.Length = 20 + 12;
    m.MsgId = Ctx->MsgId; m.Flags = Set; m.Type = Type; m.Id = Id; m.DataLen = 12;
    m.Key = Key; m.KeyLen = 4; m.Value = Value;
    C_ASSERT(sizeof(m) == 56);
    if (!TxSend(Ctx, &m, sizeof(m))) {
        return FALSE;
    }
    if (!Wait(Ctx, IsAnswered, 1000)) {
        LogPrint("rpm: msg %u not answered in 1 s\n", Ctx->MsgId);
        return FALSE;
    }
    return !Ctx->LastErr;
}

static VOID IpaProbe(VOID);

static VOID PublishIpaClock(ULONG Khz)
{
    UNICODE_STRING key = RTL_CONSTANT_STRING(L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\TopazRpm\\State");
    UNICODE_STRING val = RTL_CONSTANT_STRING(L"IpaClockKhz");
    OBJECT_ATTRIBUTES oa;
    HANDLE h;
    NTSTATUS st;

    InitializeObjectAttributes(&oa, &key, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    st = ZwCreateKey(&h, KEY_SET_VALUE, &oa, 0, NULL, REG_OPTION_VOLATILE, NULL);
    if (NT_SUCCESS(st)) {
        st = ZwSetValueKey(h, &val, 0, REG_DWORD, &Khz, sizeof(Khz));
        ZwClose(h);
    }
    LogPrint("published IpaClockKhz=%u (volatile State key): %08x\n", Khz, st);
}

static VOID Link(PDEVICE_CONTEXT Ctx)
{
    PHYSICAL_ADDRESS pa;
    struct { ULONG Hdr[2]; CHAR Name[16]; } open;
    BOOLEAN ok;

    pa.QuadPart = APCS_IPC_PA & ~0xFFFull;
    Ctx->Ipc = (volatile ULONG *)((PUCHAR)MmMapIoSpaceEx(pa, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE) + (APCS_IPC_PA & 0xFFF));
    if (Ctx->Ipc == (volatile ULONG *)(APCS_IPC_PA & 0xFFF)) {
        LogPrint("apcs map failed\n");
        Ctx->Ipc = NULL;
        return;
    }
    Ctx->LastAck = -1;
    SendCmd(Ctx, CMD_VERSION, 1, 0);
    if (!Wait(Ctx, IsVersion, 1000)) {
        LogPrint("glink: no VERSION answer in 1 s\n");
        return;
    }
    RtlZeroMemory(&open, sizeof(open));
    open.Hdr[0] = CMD_OPEN | ((ULONG)CHAN_LCID << 16);
    open.Hdr[1] = sizeof(CHAN_NAME);
    RtlCopyMemory(open.Name, CHAN_NAME, sizeof(CHAN_NAME));
    TxSend(Ctx, &open, sizeof(open));
    if (!Wait(Ctx, IsOpen, 1000)) {
        LogPrint("glink: rpm_requests not open after 1 s (ack %u remote %u)\n", Ctx->OpenAck, Ctx->RemoteOpen);
        return;
    }
    LogPrint("glink: rpm_requests open (rcid %u)\n", Ctx->Rcid);
    ok = RpmWrite(Ctx, RPM_ACTIVE_SET, RPM_RES_IPA_CLK, 0, RPM_KEY_RATE, IPA_CLK_KHZ);
    LogPrint("rpm: IPA clock %u kHz (active set): %s\n", IPA_CLK_KHZ, ok ? "OK" : "FAILED");
    if (ok) {
        PublishIpaClock(IPA_CLK_KHZ);
    }
    if (ok && FileExists(L"\\??\\C:\\topaz\\ipa.probe")) {
        LogPrint("C:\\topaz\\ipa.probe present (deleted: %u): reading IPA / GSI registers\n",
                 DeleteFile(L"\\??\\C:\\topaz\\ipa.probe"));
        IpaProbe();
    }
}

#define IPA_REG_PA          0x05840000ULL
#define GSI_REG_PA          0x05804000ULL

static ULONG ReadPa(ULONGLONG Pa)
{
    PHYSICAL_ADDRESS pa;
    volatile ULONG *va;
    ULONG v;

    pa.QuadPart = Pa & ~0xFFFull;
    va = (volatile ULONG *)MmMapIoSpaceEx(pa, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (va == NULL) {
        return 0xDEADDEAD;
    }
    v = READ_REGISTER_ULONG((volatile ULONG *)((PUCHAR)va + (Pa & 0xFFF)));
    MmUnmapIoSpace((PVOID)va, PAGE_SIZE);
    return v;
}

static VOID IpaProbe(VOID)
{
    static const struct { ULONGLONG Pa; PCSTR Name; } r[] = {
        { IPA_REG_PA + 0x3C,    "IPA COMP_CFG" },
        { IPA_REG_PA + 0x54,    "IPA SHARED_MEM_SIZE" },
        { IPA_REG_PA + 0x210,   "IPA FLAVOR_0" },
        { GSI_REG_PA + 0x1F000, "GSI STATUS (EE 0)" },
        { GSI_REG_PA + 0x1F040, "GSI HW_PARAM_2 (EE 0)" },
    };
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(r); i++) {
        LogPrint("ipa probe: %-22s %llx = %08x\n", r[i].Name, r[i].Pa, ReadPa(r[i].Pa));
    }
}

static PCSTR FifoName(ULONG Id)
{
    return Id == FIFO_AP2R ? "ap2r (AP->RPM)" : Id == FIFO_R2AP ? "r2ap (RPM->AP)" : "?";
}

/* log up to 64 bytes from a FIFO starting at its tail, word reads only */
static VOID DumpFifo(PDEVICE_CONTEXT Ctx, ULONG Off, ULONG Size)
{
    ULONG tail = Rd(Ctx, Off), head = Rd(Ctx, Off + 4), avail, i, n;
    UCHAR buf[64];

    avail = (head >= tail) ? head - tail : Size - tail + head;
    LogPrint("    tail %x head %x size %x -> %u bytes pending\n", tail, head, Size, avail);
    if (tail >= Size || head >= Size) {
        LogPrint("    indices out of range, not dumping\n");
        return;
    }
    n = min(avail, (ULONG)sizeof(buf)) & ~3u;
    for (i = 0; i < n; i += 4) {
        ULONG pos = (tail + i) % Size;
        *(ULONG *)(buf + i) = Rd(Ctx, Off + 8 + pos);
    }
    if (n != 0) {
        LogHex("    data:", buf, n);
    }
}

static NTSTATUS Survey(PDEVICE_CONTEXT Ctx)
{
    PHYSICAL_ADDRESS pa;
    ULONG toc = MSG_RAM_SIZE - TOC_SIZE, magic, count, i;

    pa.QuadPart = MSG_RAM_PA;
    Ctx->Ram = (volatile UCHAR *)MmMapIoSpaceEx(pa, MSG_RAM_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (Ctx->Ram == NULL) {
        LogPrint("map %llx failed\n", MSG_RAM_PA);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    magic = Rd(Ctx, toc);
    count = Rd(Ctx, toc + 4);
    LogPrint("msg ram %llx: TOC magic %08x (%s), %u entries\n", MSG_RAM_PA, magic,
             magic == TOC_MAGIC ? "grt0" : "BAD", count);
    if (magic != TOC_MAGIC || count > (TOC_SIZE - 8) / 12) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    for (i = 0; i < count; i++) {
        ULONG id = Rd(Ctx, toc + 8 + 12 * i), off = Rd(Ctx, toc + 12 + 12 * i), size = Rd(Ctx, toc + 16 + 12 * i);
        LogPrint("  [%u] id %08x %s off %x size %x\n", i, id, FifoName(id), off, size);
        if (id == FIFO_AP2R) {
            Ctx->Tx.Off = off; Ctx->Tx.Size = size;
        } else if (id == FIFO_R2AP) {
            Ctx->Rx.Off = off; Ctx->Rx.Size = size;
        }
        if ((id == FIFO_AP2R || id == FIFO_R2AP) && off + 8 + size <= MSG_RAM_SIZE) {
            DumpFifo(Ctx, off, size);
        }
    }
    return STATUS_SUCCESS;
}

/* ---- WDF -------------------------------------------------------------------- */

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    LogOpen();
    LogPrint("==== TopazRpm " TOPAZ_RPM_VERSION " ====\n");
    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
    config.EvtDriverUnload = EvtDriverUnload;
    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        LogClose();
    }
    return status;
}

static VOID EvtDriverUnload(WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    LogClose();
}

static NTSTATUS EvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attrs;
    WDFDEVICE device;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = EvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = EvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attrs, &device);
    if (NT_SUCCESS(status)) {
        RtlZeroMemory(DeviceGetContext(device), sizeof(DEVICE_CONTEXT));
    }
    return status;
}

static NTSTATUS EvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    UNREFERENCED_PARAMETER(Raw);
    UNREFERENCED_PARAMETER(Translated);
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    NTSTATUS status = Survey(ctx);

    LogPrint("survey: %08x\n", status);
    if (NT_SUCCESS(status) && ctx->Tx.Size != 0 && ctx->Rx.Size != 0) {
        if (Rd(ctx, ctx->Tx.Off) != 0 || Rd(ctx, ctx->Tx.Off + 4) != 0 ||
            Rd(ctx, ctx->Rx.Off) != 0 || Rd(ctx, ctx->Rx.Off + 4) != 0) {
            LogPrint("FIFOs used since boot (driver restarted?): no second handshake - it crashed the RPM\n");
        } else if (FileExists(L"\\??\\C:\\topaz\\rpm.on")) {
            LogPrint("C:\\topaz\\rpm.on present: linking\n");
            Link(ctx);
        } else {
            LogPrint("C:\\topaz\\rpm.on absent: read only\n");
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS EvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);

    UNREFERENCED_PARAMETER(Translated);
    if (ctx->Ram != NULL) {
        MmUnmapIoSpace((PVOID)ctx->Ram, MSG_RAM_SIZE);
        ctx->Ram = NULL;
    }
    if (ctx->Ipc != NULL) {
        MmUnmapIoSpace((PVOID)((ULONG_PTR)ctx->Ipc & ~(ULONG_PTR)0xFFF), PAGE_SIZE);
        ctx->Ipc = NULL;
    }
    return STATUS_SUCCESS;
}
