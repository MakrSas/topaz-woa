/*
 * TopazCpu - CPU frequency of Redmi Note 12 4G (topaz, SM6225) under Windows.
 *
 * There is no Qualcomm PEP/PPM in this port, so nothing drives DVFS and the cores stay at the
 * level the bootloader left. The SoC's CPU frequency hardware (stock DT "qcom,cpufreq-hw") has one
 * block per cluster: 0xF521000 = domain 0 (cpu0-3, silver), 0xF523000 = domain 1 (cpu4-7, gold).
 * Layout from Linux qcom-cpufreq-hw.c (v1): ENABLE 0x0 bit0, DCVS_CTRL 0xbc bit0 = per-core
 * votes, FREQ_LUT 0x110 / VOLT_LUT 0x114 in 32-byte rows (src [31:30], core count [18:16],
 * L value [7:0]; volt [11:0] mV), CURRENT_VOTE 0x704 [9:0] = L value the hardware runs at
 * (x 19.2 MHz), PERF_STATE 0x920 = LUT index the OS asks for. Voltage and thermal limits are
 * handled by the hardware itself.
 *
 * v0.1: logs both tables and the current level (read only). With C:\topaz\cpu.max present it
 * asks for the highest non-turbo level of both clusters and puts the original levels back when
 * the device stops. Log: C:\TopazCpu.log.
 */
#include "driver.h"

#define TOPAZ_CPU_VERSION   "v0.1"

#define NUM_DOMAINS         2
#define DOMAIN_SIZE         0x1000
#define REG_ENABLE          0x000
#define REG_DCVS_CTRL       0x0BC
#define REG_FREQ_LUT        0x110
#define REG_VOLT_LUT        0x114
#define REG_CURRENT_VOTE    0x704
#define REG_PERF_STATE      0x920
#define LUT_ROW             32
#define LUT_ENTRIES         12              /* stock DT qcom,max-lut-entries */
#define LUT_TURBO_IND       1
#define XO_KHZ              19200
#define ALT_KHZ             300000          /* GPLL0 600 MHz / 2 when src = 0 */
#define CORES_PER_DOMAIN    4
#define POLL_MS             1000

static const ULONGLONG g_DomainPa[NUM_DOMAINS] = { 0x0F521000ULL, 0x0F523000ULL };
static const PCSTR g_DomainName[NUM_DOMAINS] = { "silver", "gold" };

typedef struct _CPU_DOMAIN {
    volatile UCHAR *Base;
    ULONG   FreqKhz[LUT_ENTRIES];           /* 0 = no usable level at this index */
    ULONG   Levels;                         /* rows read up to the end of the table */
    ULONG   TopIdx;                         /* highest non-turbo level */
    BOOLEAN PerCore;
    BOOLEAN Changed;
    ULONG   Orig[CORES_PER_DOMAIN];
    ULONG   LastVote;
    ULONG   LastState;
} CPU_DOMAIN;

typedef struct _DEVICE_CONTEXT {
    CPU_DOMAIN Dom[NUM_DOMAINS];
    BOOLEAN    HwReady;
    BOOLEAN    WantMax;
    PKTHREAD   Thread;
    KEVENT     StopEvent;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_UNLOAD           EvtDriverUnload;
static EVT_WDF_DRIVER_DEVICE_ADD       EvtDeviceAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE EvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE EvtReleaseHardware;
static EVT_WDF_DEVICE_D0_ENTRY         EvtD0Entry;
static EVT_WDF_DEVICE_D0_EXIT          EvtD0Exit;

static ULONG Rd(CPU_DOMAIN *D, ULONG Off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(D->Base + Off));
}

static VOID Wr(CPU_DOMAIN *D, ULONG Off, ULONG Val)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(D->Base + Off), Val);
}

static ULONG VoteMhz(ULONG Vote)
{
    return (Vote & 0x3FF) * XO_KHZ / 1000;
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

/* ---- Tables ------------------------------------------------------------------ */

/* Same walk as Linux qcom_cpufreq_hw_read_lut(): turbo rows (core count 1) are skipped, two equal
   frequencies in a row end the table. */
static VOID ReadLut(ULONG N, CPU_DOMAIN *D)
{
    ULONG i, f, v, src, lval, cores, mv, prev = 0;

    D->TopIdx = 0;
    D->Levels = 0;
    for (i = 0; i < LUT_ENTRIES; i++) {
        f = Rd(D, REG_FREQ_LUT + i * LUT_ROW);
        v = Rd(D, REG_VOLT_LUT + i * LUT_ROW);
        src = f >> 30;
        lval = f & 0xFF;
        cores = (f >> 16) & 0x7;
        mv = v & 0xFFF;
        D->FreqKhz[i] = src ? lval * XO_KHZ : ALT_KHZ;
        LogPrint("d%u %s lut[%2u] %08x %08x -> %4u MHz %4u mV cores %u%s\n", N, g_DomainName[N], i, f, v,
                 D->FreqKhz[i] / 1000, mv, cores, cores == LUT_TURBO_IND ? " turbo" : "");
        D->Levels = i + 1;
        if (i > 0 && D->FreqKhz[i] == prev) {
            D->FreqKhz[i] = 0;
            break;
        }
        if (cores == LUT_TURBO_IND) {
            prev = D->FreqKhz[i];
            D->FreqKhz[i] = 0;
            continue;
        }
        prev = D->FreqKhz[i];
        D->TopIdx = i;
    }
}

static VOID DumpState(ULONG N, CPU_DOMAIN *D, PCSTR Tag)
{
    ULONG st = Rd(D, REG_PERF_STATE), vote = Rd(D, REG_CURRENT_VOTE);

    LogPrint("d%u %s %s: perf_state %u (%u MHz asked) vote %08x = %u MHz\n", N, g_DomainName[N], Tag, st,
             st < LUT_ENTRIES ? D->FreqKhz[st] / 1000 : 0, vote, VoteMhz(vote));
}

/* ---- Level control ------------------------------------------------------------- */

static VOID SetLevel(CPU_DOMAIN *D, const ULONG *Idx)
{
    ULONG c, n = D->PerCore ? CORES_PER_DOMAIN : 1;

    for (c = 0; c < n; c++) {
        Wr(D, REG_PERF_STATE + 4 * c, Idx[c]);
    }
}

static VOID ApplyMax(PDEVICE_CONTEXT Ctx)
{
    ULONG d, c, top[CORES_PER_DOMAIN];
    LARGE_INTEGER wait;
    CPU_DOMAIN *D;

    for (d = 0; d < NUM_DOMAINS; d++) {
        D = &Ctx->Dom[d];
        if (D->FreqKhz[D->TopIdx] == 0) {
            LogPrint("d%u: no usable level, left alone\n", d);
            continue;
        }
        for (c = 0; c < CORES_PER_DOMAIN; c++) {
            D->Orig[c] = Rd(D, REG_PERF_STATE + 4 * c);
            top[c] = D->TopIdx;
        }
        SetLevel(D, top);
        D->Changed = TRUE;
        LogPrint("d%u %s: perf_state -> %u (%u MHz)\n", d, g_DomainName[d], D->TopIdx, D->FreqKhz[D->TopIdx] / 1000);
    }
    wait.QuadPart = -10000LL * 20;
    KeDelayExecutionThread(KernelMode, FALSE, &wait);
    for (d = 0; d < NUM_DOMAINS; d++) {
        DumpState(d, &Ctx->Dom[d], "after");
    }
}

static VOID Restore(PDEVICE_CONTEXT Ctx)
{
    ULONG d;

    for (d = 0; d < NUM_DOMAINS; d++) {
        if (Ctx->Dom[d].Changed) {
            SetLevel(&Ctx->Dom[d], Ctx->Dom[d].Orig);
            Ctx->Dom[d].Changed = FALSE;
            DumpState(d, &Ctx->Dom[d], "restored");
        }
    }
}

/* ---- Monitor ------------------------------------------------------------------ */

static KSTART_ROUTINE PollThread;

/* Logs every change of the requested level or of the level the hardware runs at (thermal limits
   show up as vote < asked), at most once a second; a summary line every minute. */
static VOID PollThread(PVOID Context)
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;
    LARGE_INTEGER period;
    ULONG d, c, st, vote, ticks = 0, lines = 0, top[CORES_PER_DOMAIN];
    CPU_DOMAIN *D;

    period.QuadPart = -10000LL * POLL_MS;
    while (KeWaitForSingleObject(&ctx->StopEvent, Executive, KernelMode, FALSE, &period) == STATUS_TIMEOUT) {
        ticks++;
        for (d = 0; d < NUM_DOMAINS; d++) {
            D = &ctx->Dom[d];
            for (c = 0; c < CORES_PER_DOMAIN; c++) {
                top[c] = D->TopIdx;
            }
            st = Rd(D, REG_PERF_STATE);
            vote = Rd(D, REG_CURRENT_VOTE) & 0x3FF;
            if ((st != D->LastState || vote != D->LastVote) && lines < 600) {
                LogPrint("t=%us d%u perf_state %u vote %u MHz\n", ticks, d, st, VoteMhz(vote));
                lines++;
            }
            if (D->Changed && st != D->TopIdx) {
                LogPrint("t=%us d%u perf_state changed to %u by someone else, setting %u again\n", ticks, d, st, D->TopIdx);
                SetLevel(D, top);
                st = D->TopIdx;
            }
            D->LastState = st;
            D->LastVote = vote;
        }
        if (ticks % 60 == 0) {
            LogPrint("t=%us silver %u MHz, gold %u MHz\n", ticks, VoteMhz(ctx->Dom[0].LastVote), VoteMhz(ctx->Dom[1].LastVote));
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

/* ---- Hardware ------------------------------------------------------------------- */

static NTSTATUS HwInit(PDEVICE_CONTEXT Ctx)
{
    PHYSICAL_ADDRESS pa;
    ULONG d, en, dcvs;
    CPU_DOMAIN *D;

    for (d = 0; d < NUM_DOMAINS; d++) {
        D = &Ctx->Dom[d];
        pa.QuadPart = (LONGLONG)g_DomainPa[d];
        D->Base = (volatile UCHAR *)MmMapIoSpaceEx(pa, DOMAIN_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
        if (D->Base == NULL) {
            LogPrint("d%u: map %llx failed\n", d, g_DomainPa[d]);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        en = Rd(D, REG_ENABLE);
        dcvs = Rd(D, REG_DCVS_CTRL);
        D->PerCore = (dcvs & 1) != 0;
        LogPrint("d%u %s @%llx: enable %08x dcvs_ctrl %08x (per-core %u) perf_state %u %u %u %u\n", d, g_DomainName[d],
                 g_DomainPa[d], en, dcvs, D->PerCore, Rd(D, REG_PERF_STATE), Rd(D, REG_PERF_STATE + 4),
                 Rd(D, REG_PERF_STATE + 8), Rd(D, REG_PERF_STATE + 12));
        if (!(en & 1)) {
            LogPrint("d%u: cpufreq hardware not enabled\n", d);
            return STATUS_DEVICE_NOT_READY;
        }
        ReadLut(d, D);
        LogPrint("d%u %s: %u rows, top level %u = %u MHz\n", d, g_DomainName[d], D->Levels, D->TopIdx,
                 D->FreqKhz[D->TopIdx] / 1000);
        DumpState(d, D, "now");
        D->LastState = Rd(D, REG_PERF_STATE);
        D->LastVote = Rd(D, REG_CURRENT_VOTE) & 0x3FF;
    }
    Ctx->HwReady = TRUE;
    return STATUS_SUCCESS;
}

static VOID HwDeinit(PDEVICE_CONTEXT Ctx)
{
    ULONG d;

    Ctx->HwReady = FALSE;
    for (d = 0; d < NUM_DOMAINS; d++) {
        if (Ctx->Dom[d].Base != NULL) {
            MmUnmapIoSpace((PVOID)Ctx->Dom[d].Base, DOMAIN_SIZE);
            Ctx->Dom[d].Base = NULL;
        }
    }
}

/* ---- WDF -------------------------------------------------------------------- */

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    LogOpen();
    LogPrint("==== TopazCpu " TOPAZ_CPU_VERSION " ====\n");
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
    pnp.EvtDeviceD0Entry = EvtD0Entry;
    pnp.EvtDeviceD0Exit = EvtD0Exit;
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
    LogPrint("HwInit: %08x\n", HwInit(DeviceGetContext(Device)));
    return STATUS_SUCCESS;
}

static NTSTATUS EvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    UNREFERENCED_PARAMETER(Translated);
    HwDeinit(DeviceGetContext(Device));
    return STATUS_SUCCESS;
}

static NTSTATUS EvtD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);
    HANDLE h;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(PreviousState);
    if (!ctx->HwReady) {
        return STATUS_SUCCESS;
    }
    ctx->WantMax = FileExists(L"\\??\\C:\\topaz\\cpu.max");
    LogPrint("C:\\topaz\\cpu.max %s\n", ctx->WantMax ? "present: max level" : "absent: read only");
    if (ctx->WantMax) {
        ApplyMax(ctx);
    }
    KeInitializeEvent(&ctx->StopEvent, NotificationEvent, FALSE);
    status = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL, PollThread, ctx);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID *)&ctx->Thread, NULL);
    ZwClose(h);
    return status;
}

static NTSTATUS EvtD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    PDEVICE_CONTEXT ctx = DeviceGetContext(Device);

    UNREFERENCED_PARAMETER(TargetState);
    if (ctx->Thread != NULL) {
        KeSetEvent(&ctx->StopEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(ctx->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(ctx->Thread);
        ctx->Thread = NULL;
    }
    if (ctx->HwReady) {
        Restore(ctx);
    }
    return STATUS_SUCCESS;
}
