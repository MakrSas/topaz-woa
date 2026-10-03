/*
 * WDDM 1.3 DDIs of TopazGpuW. Step A: GPU work of the freedreno UMD goes through DxgkDdiEscape
 * (msm ioctls). Step C2 (v0.28): also the display adapter of the panel - 1 VidPN source/target
 * (disp.c), allocations with a CPU view (UMD BO or VidMM aperture backing), Present/Blt and paging
 * fills executed by the CPU engine (eng.c). Shape follows viogpu3d (virtio-win PR #943, BSD-3:
 * same Mesa d3d10umd model, aperture segment, MMIO flips + timer vsync) and the WDK samples.
 */
#include "tgpu.h"
#include <initguid.h>
#include <wdmguid.h>
#include <devpkey.h>

typedef struct _TGPU_ADAPTER {
    PDEVICE_OBJECT       Pdo;
    DXGKRNL_INTERFACE    Dxgk;
    BOOLEAN              Started;
    ULONG                LastSubmittedFence;
    ULONG                LastCompletedFence;
} TGPU_ADAPTER;

typedef struct _TGPU_DEVICE {
    TGPU_ADAPTER *Adapter;
    HANDLE        hRtDevice;
} TGPU_DEVICE;

typedef struct _TGPU_CONTEXT {
    TGPU_DEVICE *Device;
} TGPU_CONTEXT;


static TGPU_ADAPTER *g_Adapter;

/* ---------------------------------------------------------------- hang guard
 * C:\topaz\gpuw.guard is created at StartDevice and deleted after 60 s of normal life. If it is
 * still there at the next StartDevice, the previous start hung the system: refuse to start. */

#define GUARD_PATH L"\\??\\C:\\topaz\\gpuw.guard"

static BOOLEAN GuardFile(BOOLEAN Create, BOOLEAN Delete)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    NTSTATUS st;

    RtlInitUnicodeString(&name, GUARD_PATH);
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    st = ZwCreateFile(&h, (Delete ? DELETE : FILE_READ_ATTRIBUTES) | SYNCHRONIZE, &oa, &iosb, NULL,
                      FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_DELETE, Create ? FILE_OPEN_IF : FILE_OPEN,
                      (Delete ? FILE_DELETE_ON_CLOSE : 0) | FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
                      NULL, 0);
    if (!NT_SUCCESS(st)) {
        return FALSE;
    }
    ZwClose(h);
    return TRUE;
}

/* v0.19: the thread must never outlive the driver image. v0.17/v0.18 slept 60 s blindly; a driver
   update within that minute unloaded the image under the sleeping thread -> bugcheck 0xCE. Now it
   waits on g_GuardStop, and StopDevice/Unload signal it and wait for the thread to exit. */
static KEVENT g_GuardStop;
static PKTHREAD g_GuardThread;

static KSTART_ROUTINE GuardThread;
static VOID GuardThread(PVOID Ctx)
{
    LARGE_INTEGER d;
    NTSTATUS st;
    UNREFERENCED_PARAMETER(Ctx);
    d.QuadPart = -10000000LL * 60;
    st = KeWaitForSingleObject(&g_GuardStop, Executive, KernelMode, FALSE, &d);
    /* alive 60 s, or stopped cleanly before that: either way the start did not hang the system */
    GuardFile(FALSE, TRUE);
    LogPrint("guard: %s (Dxgkrnl fence %u), guard file removed\n", st == STATUS_TIMEOUT ? "alive 60 s" : "stopped",
             g_Adapter != NULL ? g_Adapter->LastCompletedFence : 0);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID GuardStart(VOID)
{
    HANDLE th;

    KeInitializeEvent(&g_GuardStop, NotificationEvent, FALSE);
    if (NT_SUCCESS(PsCreateSystemThread(&th, THREAD_ALL_ACCESS, NULL, NULL, NULL, GuardThread, NULL))) {
        if (!NT_SUCCESS(ObReferenceObjectByHandle(th, SYNCHRONIZE, *PsThreadType, KernelMode,
                                                  (PVOID *)&g_GuardThread, NULL))) {
            g_GuardThread = NULL;
        }
        ZwClose(th);
    }
}

static VOID GuardStop(VOID)
{
    if (g_GuardThread != NULL) {
        KeSetEvent(&g_GuardStop, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(g_GuardThread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(g_GuardThread);
        g_GuardThread = NULL;
    }
}

/* ---------------------------------------------------------------- completion of Dxgkrnl DMA buffers */

typedef struct _NOTIFY_CTX {
    TGPU_ADAPTER *Adapter;
    ULONG         Fence;
} NOTIFY_CTX;

static BOOLEAN NotifyRoutine(PVOID Ctx)
{
    NOTIFY_CTX *n = (NOTIFY_CTX *)Ctx;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA d;

    RtlZeroMemory(&d, sizeof(d));
    d.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
    d.DmaCompleted.SubmissionFenceId = n->Fence;
    d.DmaCompleted.NodeOrdinal = 0;
    d.DmaCompleted.EngineOrdinal = 0;
    n->Adapter->Dxgk.DxgkCbNotifyInterrupt(n->Adapter->Dxgk.DeviceHandle, &d);
    n->Adapter->LastCompletedFence = n->Fence;
    n->Adapter->Dxgk.DxgkCbQueueDpc(n->Adapter->Dxgk.DeviceHandle);
    return TRUE;
}

static VOID CompleteFence(TGPU_ADAPTER *A, ULONG Fence)
{
    NOTIFY_CTX n = { A, Fence };
    BOOLEAN ret = FALSE;
    NTSTATUS st = A->Dxgk.DxgkCbSynchronizeExecution(A->Dxgk.DeviceHandle, NotifyRoutine, &n, 0, &ret);
    if (!NT_SUCCESS(st) || Fence < 64) {
        LogPrint("  CompleteFence %u: SynchronizeExecution %08x ret %u\n", Fence, st, ret);
    }
}

static VOID FenceDone(PVOID Ctx, ULONG Fence)
{
    CompleteFence((TGPU_ADAPTER *)Ctx, Fence);
}

/* ---------------------------------------------------------------- adapter */

/* The root-enumerated device has no resources; give it the GPU interrupt (GIC SPI 177 = GSIV 209,
   level) by writing its LogConf\BasicConfigVector (IO_RESOURCE_REQUIREMENTS_LIST) from the kernel:
   Enum\ is not writable from user mode and the INF LogConfigOverride is ignored. Takes effect at
   the next start of the device. */
static VOID EnsureIrqRequirement(PDEVICE_OBJECT Pdo)
{
    WCHAR inst[128], path[256];
    ULONG len = 0;
    DEVPROPTYPE type;
    UNICODE_STRING name, value;
    OBJECT_ATTRIBUTES oa;
    HANDLE key;
    UCHAR b[72];
    ULONG got = 0;
    NTSTATUS st;

    st = IoGetDevicePropertyData(Pdo, &DEVPKEY_Device_InstanceId, 0, 0, sizeof(inst) - sizeof(WCHAR), inst, &len, &type);
    if (!NT_SUCCESS(st)) {
        LogPrint("IRQ requirement: instance id %08x\n", st);
        return;
    }
    inst[len / sizeof(WCHAR)] = 0;
    RtlStringCbPrintfW(path, sizeof(path), L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Enum\\%ws\\LogConf", inst);
    RtlInitUnicodeString(&name, path);
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    st = ZwCreateKey(&key, KEY_ALL_ACCESS, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, NULL);
    if (!NT_SUCCESS(st)) {
        LogPrint("IRQ requirement: open %ws %08x\n", path, st);
        return;
    }
    RtlInitUnicodeString(&value, L"BasicConfigVector");
    if (NT_SUCCESS(ZwQueryValueKey(key, &value, KeyValuePartialInformation, NULL, 0, &got)) || got > 16) {
        LogPrint("IRQ requirement: already present (%u)\n", got);
        ZwClose(key);
        return;
    }
    RtlZeroMemory(b, sizeof(b));
    *(ULONG *)(b + 0) = sizeof(b);                       /* ListSize, Internal, bus 0 */
    *(ULONG *)(b + 28) = 1;                              /* AlternativeLists */
    *(USHORT *)(b + 32) = 1;                             /* Version */
    *(USHORT *)(b + 34) = 1;                             /* Revision */
    *(ULONG *)(b + 36) = 1;                              /* Count */
    b[41] = CmResourceTypeInterrupt;
    b[42] = CmResourceShareDeviceExclusive;
    *(USHORT *)(b + 44) = CM_RESOURCE_INTERRUPT_LEVEL_SENSITIVE;
    *(ULONG *)(b + 48) = 209;                            /* MinimumVector */
    *(ULONG *)(b + 52) = 209;                            /* MaximumVector */
    st = ZwSetValueKey(key, &value, 0, REG_RESOURCE_REQUIREMENTS_LIST, b, sizeof(b));
    LogPrint("IRQ requirement: %ws BasicConfigVector written %08x (applies at the next start)\n", inst, st);
    ZwClose(key);
}

static NTSTATUS TgAddDevice(const PDEVICE_OBJECT Pdo, PVOID *Ctx)
{
    /* v0.10: with this IRQ requirement Dxgkrnl failed the start (c0000034) before StartDevice */
    UNREFERENCED_PARAMETER(EnsureIrqRequirement);
    TGPU_ADAPTER *a = (TGPU_ADAPTER *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*a), TGPU_POOL_TAG);
    if (a == NULL) {
        return STATUS_NO_MEMORY;
    }
    a->Pdo = Pdo;
    *Ctx = a;
    g_Adapter = a;
    LogPrint("AddDevice\n");
    return STATUS_SUCCESS;
}

static NTSTATUS TgStartDevice(const PVOID Ctx, PDXGK_START_INFO StartInfo, PDXGKRNL_INTERFACE Dxgk,
                              PULONG NumSources, PULONG NumChildren)
{
    TGPU_ADAPTER *a = (TGPU_ADAPTER *)Ctx;

    UNREFERENCED_PARAMETER(StartInfo);
    if (GuardFile(FALSE, FALSE)) {
        LogPrint("StartDevice: C:\\topaz\\gpuw.guard exists (previous start hung?) -> refusing to start\n");
        return STATUS_UNSUCCESSFUL;
    }
    GuardFile(TRUE, FALSE);
    GuardStart();
    RtlCopyMemory(&a->Dxgk, Dxgk, sizeof(a->Dxgk));
    {
        DXGK_DEVICE_INFO di;
        NTSTATUS st = a->Dxgk.DxgkCbGetDeviceInformation(a->Dxgk.DeviceHandle, &di);
        LogPrint("GetDeviceInformation: %08x\n", st);
        if (NT_SUCCESS(st) && di.TranslatedResourceList != NULL) {
            PCM_PARTIAL_RESOURCE_LIST pl = &di.TranslatedResourceList->List[0].PartialResourceList;
            ULONG i;
            for (i = 0; di.TranslatedResourceList->Count > 0 && i < pl->Count; i++) {
                PCM_PARTIAL_RESOURCE_DESCRIPTOR r = &pl->PartialDescriptors[i];
                LogPrint("  resource %u: type %u flags %x %08x %08x %08x\n", i, r->Type, r->Flags,
                         r->u.Generic.Start.LowPart, r->u.Generic.Start.HighPart, r->u.Generic.Length);
            }
        }
    }
    if (!NT_SUCCESS(EngStart(FenceDone, a))) {
        LogPrint("StartDevice: engine thread failed\n");
        return STATUS_UNSUCCESSFUL;
    }
    DispStart(&a->Dxgk);
    *NumSources = 1;                                     /* step C2: the panel */
    *NumChildren = 1;
    a->Started = TRUE;
    LogPrint("StartDevice: display adapter (1 source, 1 child), GPU starts on the first escape\n");
    return STATUS_SUCCESS;
}

static NTSTATUS TgStopDevice(const PVOID Ctx)
{
    UNREFERENCED_PARAMETER(Ctx);
    LogPrint("StopDevice\n");
    DispStop();
    EngStop();
    GuardStop();
    MsmCleanup();
    HwStop();
    return STATUS_SUCCESS;
}

static NTSTATUS TgRemoveDevice(const PVOID Ctx)
{
    ExFreePoolWithTag(Ctx, TGPU_POOL_TAG);
    g_Adapter = NULL;
    return STATUS_SUCCESS;
}

static NTSTATUS TgDispatchIoRequest(const PVOID Ctx, ULONG Src, PVIDEO_REQUEST_PACKET Vrp)
{
    LogPrint("%s\n", "TgDispatchIoRequest");
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(Src);
    UNREFERENCED_PARAMETER(Vrp);
    return STATUS_NOT_SUPPORTED;
}

static volatile LONG g_Isr;
static BOOLEAN TgInterruptRoutine(const PVOID Ctx, ULONG Msg)
{
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(Msg);
    InterlockedIncrement(&g_Isr);
    return FALSE;                                        /* GPU interrupts are still masked */
}

static VOID TgDpcRoutine(const PVOID Ctx)
{
    TGPU_ADAPTER *a = (TGPU_ADAPTER *)Ctx;
    a->Dxgk.DxgkCbNotifyDpc(a->Dxgk.DeviceHandle);
}

static NTSTATUS TgSetPowerState(const PVOID Ctx, ULONG Uid, DEVICE_POWER_STATE Ps, POWER_ACTION Action)
{
    LogPrint("%s\n", "TgSetPowerState");
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(Uid);
    UNREFERENCED_PARAMETER(Ps);
    UNREFERENCED_PARAMETER(Action);
    return STATUS_SUCCESS;
}

static VOID TgResetDevice(const PVOID Ctx)
{
    UNREFERENCED_PARAMETER(Ctx);
}

static VOID TgUnload(VOID)
{
    GuardStop();
    LogClose();
}

static NTSTATUS TgQueryInterface(const PVOID Ctx, PQUERY_INTERFACE Qi)
{
    UNREFERENCED_PARAMETER(Ctx);
    return DispBrightnessQueryInterface(Qi);
}

static NTSTATUS QueryAdapterInfoInner(const DXGKARG_QUERYADAPTERINFO *Q);

static NTSTATUS APIENTRY TgQueryAdapterInfo(const HANDLE hAdapter, const DXGKARG_QUERYADAPTERINFO *Q)
{
    NTSTATUS st;
    UNREFERENCED_PARAMETER(hAdapter);
    st = QueryAdapterInfoInner(Q);
    LogPrint("QueryAdapterInfo type %u in %u out %u -> %08x\n", Q->Type, Q->InputDataSize, Q->OutputDataSize, st);
    return st;
}

static NTSTATUS QueryAdapterInfoInner(const DXGKARG_QUERYADAPTERINFO *Q)
{
    switch (Q->Type) {
    case DXGKQAITYPE_DRIVERCAPS: {
        DXGK_DRIVERCAPS *c = (DXGK_DRIVERCAPS *)Q->pOutputData;
        /* WDDM 1.3 caps are shorter than the current header's struct (552 bytes on 22621) */
        if (Q->OutputDataSize < FIELD_OFFSET(DXGK_DRIVERCAPS, SupportSmoothRotation)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        RtlZeroMemory(c, Q->OutputDataSize);
        c->WDDMVersion = DXGKDDI_WDDMv1_3;
        c->HighestAcceptableAddress.QuadPart = (LONGLONG)-1;
        c->MaxAllocationListSlotId = 16;
        /* the CPU engine never preempts; v0.35: PreemptionAware with granularity NONE as viogpu3d
           (it worked with DWM on 22621; DWM's flips failed in the runtime with PreemptionAware 0) */
        c->SchedulingCaps.MultiEngineAware = 1;
        c->SchedulingCaps.PreemptionAware = 1;
        c->SupportDirectFlip = 1;
        c->GpuEngineTopology.NbAsymetricProcessingNodes = 1;
        c->SupportNonVGA = TRUE;
        /* C2: SetVidPnSourceAddress = MMIO flip latched at the next (timer) vsync, like viogpu3d */
        c->FlipCaps.FlipOnVSyncMmIo = 1;
        c->MaxQueuedFlipOnVSync = 1;
        /* v0.29: as viogpu3d - without it no primary could be placed in the aperture segment and every
           mode set ended in CommitVidPn with 0 paths (SetDisplayConfig 1610/31, driver never asked) */
        c->MemoryManagementCaps.SectionBackedPrimary = 1;
        c->PresentationCaps.NoScreenToScreenBlt = 1;
        c->PresentationCaps.NoOverlapScreenBlt = 1;
        c->PresentationCaps.AlignmentShift = 2;
        c->PresentationCaps.MaxTextureWidthShift = 2;
        c->PresentationCaps.MaxTextureHeightShift = 2;
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_QUERYSEGMENT3: {
        DXGK_QUERYSEGMENTOUT3 *s = (DXGK_QUERYSEGMENTOUT3 *)Q->pOutputData;
        if (Q->OutputDataSize < sizeof(*s)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        if (s->pSegmentDescriptor == NULL) {
            s->NbSegment = 1;
        } else {
            DXGK_SEGMENTDESCRIPTOR3 *d = s->pSegmentDescriptor;
            RtlZeroMemory(&d[0], sizeof(d[0]));
            s->PagingBufferPrivateDataSize = 16 * sizeof(TG_CMD);
            s->PagingBufferSegmentId = 1;
            s->PagingBufferSize = 16 * PAGE_SIZE;
            /* VidMM's view only: real backing and GPU VAs belong to msm.c */
            d[0].BaseAddress.QuadPart = 0xC0000000;
            d[0].Size = 1024ull * 1024 * 1024;
            d[0].CommitLimit = 1024ull * 1024 * 1024;
            d[0].Flags.Aperture = TRUE;
            d[0].Flags.CacheCoherent = TRUE;
            d[0].Flags.DirectFlip = TRUE;                /* v0.35: as viogpu3d */
        }
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_HISTORYBUFFERPRECISION: {
        /* v0.27: Dxgkrnl asks this of every adapter; STATUS_NOT_SUPPORTED is an "invalid NTSTATUS"
           for it and the adapter got reset at start (ADAPTER_WAS_RESET c01e0004, ETW) */
        DXGKARG_HISTORYBUFFERPRECISION *h = (DXGKARG_HISTORYBUFFERPRECISION *)Q->pOutputData;
        if (Q->OutputDataSize < sizeof(*h)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        h->PrecisionBits = 64;
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_UMDRIVERPRIVATE: {
        if (Q->OutputDataSize < sizeof(ULONG)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        *(PULONG)Q->pOutputData = TOPAZGPU_ESC_MAGIC;
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

static NTSTATUS APIENTRY TgEscape(const HANDLE hAdapter, const DXGKARG_ESCAPE *E)
{
    UNREFERENCED_PARAMETER(hAdapter);
    if (E->PrivateDriverDataSize < sizeof(struct topazgpu_escape) || E->pPrivateDriverData == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    return MsmEscape((struct topazgpu_escape *)E->pPrivateDriverData, E->hDevice);
}

/* ---------------------------------------------------------------- devices, contexts, allocations */

static NTSTATUS APIENTRY TgCreateDevice(const HANDLE hAdapter, DXGKARG_CREATEDEVICE *A)
{
    LogPrint("%s\n", "TgCreateDevice");
    TGPU_DEVICE *d = (TGPU_DEVICE *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*d), TGPU_POOL_TAG);
    if (d == NULL) {
        return STATUS_NO_MEMORY;
    }
    d->Adapter = (TGPU_ADAPTER *)hAdapter;
    d->hRtDevice = A->hDevice;
    A->hDevice = d;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgDestroyDevice(const HANDLE hDevice)
{
    MsmReleaseOwner(hDevice);
    ExFreePoolWithTag(hDevice, TGPU_POOL_TAG);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgCreateContext(const HANDLE hDevice, DXGKARG_CREATECONTEXT *A)
{
    LogPrint("%s\n", "TgCreateContext");
    TGPU_CONTEXT *c = (TGPU_CONTEXT *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*c), TGPU_POOL_TAG);
    if (c == NULL) {
        return STATUS_NO_MEMORY;
    }
    c->Device = (TGPU_DEVICE *)hDevice;
    A->hContext = c;
    A->ContextInfo.DmaBufferSize = 16 * PAGE_SIZE;
    A->ContextInfo.DmaBufferPrivateDataSize = 4 * sizeof(TG_CMD);
    A->ContextInfo.DmaBufferSegmentSet = 1;
    A->ContextInfo.AllocationListSize = 64;
    A->ContextInfo.PatchLocationListSize = 64;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgDestroyContext(const HANDLE hContext)
{
    ExFreePoolWithTag(hContext, TGPU_POOL_TAG);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- aperture page table (v0.30) */

#define GART_PAGES ((ULONG)(TGPU_APERTURE_SIZE / PAGE_SIZE))
static PPFN_NUMBER g_Gart;                               /* 0 = not mapped */

static VOID GartMap(SIZE_T OffsetInPages, PMDL Mdl, ULONG MdlOffset, SIZE_T Pages)
{
    PPFN_NUMBER pfn;
    SIZE_T i;

    if (g_Gart == NULL) {
        g_Gart = (PPFN_NUMBER)ExAllocatePool2(POOL_FLAG_NON_PAGED, GART_PAGES * sizeof(PFN_NUMBER), TGPU_POOL_TAG);
        if (g_Gart == NULL) {
            return;
        }
    }
    if (Mdl == NULL || OffsetInPages + Pages > GART_PAGES) {
        return;
    }
    pfn = MmGetMdlPfnArray(Mdl);
    for (i = 0; i < Pages; i++) {
        g_Gart[OffsetInPages + i] = pfn[MdlOffset + i];
    }
}

static VOID GartUnmap(SIZE_T OffsetInPages, SIZE_T Pages)
{
    if (g_Gart != NULL && OffsetInPages + Pages <= GART_PAGES) {
        RtlZeroMemory(&g_Gart[OffsetInPages], Pages * sizeof(PFN_NUMBER));
    }
}

PVOID GartMapVa(ULONGLONG Addr, SIZE_T Bytes, PMDL *Mdl)
{
    SIZE_T first, pages, i;
    PMDL m;
    PPFN_NUMBER pfn;
    PVOID va;

    *Mdl = NULL;
    if (g_Gart == NULL || Addr < TGPU_APERTURE_BASE || (Addr & (PAGE_SIZE - 1)) != 0 || Bytes == 0) {
        return NULL;
    }
    first = (SIZE_T)((Addr - TGPU_APERTURE_BASE) / PAGE_SIZE);
    pages = ROUND_TO_PAGES(Bytes) / PAGE_SIZE;
    if (first + pages > GART_PAGES) {
        return NULL;
    }
    m = (PMDL)ExAllocatePool2(POOL_FLAG_NON_PAGED, MmSizeOfMdl(NULL, pages * PAGE_SIZE), TGPU_POOL_TAG);
    if (m == NULL) {
        return NULL;
    }
    MmInitializeMdl(m, NULL, pages * PAGE_SIZE);
    pfn = MmGetMdlPfnArray(m);
    for (i = 0; i < pages; i++) {
        pfn[i] = g_Gart[first + i];
        if (pfn[i] == 0) {
            ExFreePoolWithTag(m, TGPU_POOL_TAG);
            return NULL;
        }
    }
    m->MdlFlags |= MDL_PAGES_LOCKED;
    va = MmMapLockedPagesSpecifyCache(m, KernelMode, MmCached, NULL, FALSE, NormalPagePriority | MdlMappingNoExecute);
    if (va == NULL) {
        ExFreePoolWithTag(m, TGPU_POOL_TAG);
        return NULL;
    }
    *Mdl = m;
    return va;
}

VOID GartUnmapVa(PVOID Va, PMDL Mdl)
{
    if (Va != NULL && Mdl != NULL) {
        MmUnmapLockedPages(Va, Mdl);
        ExFreePoolWithTag(Mdl, TGPU_POOL_TAG);
    }
}

/* ---------------------------------------------------------------- allocations (step C2)
 * Private data = struct topazgpu_alloc (topazgpu_escape.h). Allocations of the UMD name the BO
 * with their pixels; standard allocations of Dxgkrnl (shared primary, shadow, staging, GDI) are
 * read and written by the CPU through VidMM's aperture backing, mapped at MAP_APERTURE_SEGMENT. */

PUCHAR AllocPixels(TGPU_ALLOCATION *Al)
{
    if (Al == NULL) {
        return NULL;
    }
    if (Al->Bo != NULL) {
        return Al->Bo->KernelVa != NULL ? (PUCHAR)Al->Bo->KernelVa + Al->Desc.bo_offset : NULL;
    }
    return (PUCHAR)Al->ApVa;
}

VOID AllocMapAperture(TGPU_ALLOCATION *Al, PMDL Mdl, ULONG MdlOffset, SIZE_T Pages)
{
    PMDL m = Mdl;
    PVOID va;

    if (Al == NULL || Mdl == NULL || Al->Bo != NULL) {
        return;                                          /* UMD allocations use their BO */
    }
    AllocUnmapAperture(Al);
    if (MdlOffset != 0) {
        m = IoAllocateMdl((PUCHAR)MmGetMdlVirtualAddress(Mdl) + (SIZE_T)MdlOffset * PAGE_SIZE,
                          (ULONG)(Pages * PAGE_SIZE), FALSE, FALSE, NULL);
        if (m == NULL) {
            return;
        }
        IoBuildPartialMdl(Mdl, m, (PUCHAR)MmGetMdlVirtualAddress(Mdl) + (SIZE_T)MdlOffset * PAGE_SIZE,
                          (ULONG)(Pages * PAGE_SIZE));
        Al->ApPartial = m;
    }
    va = MmMapLockedPagesSpecifyCache(m, KernelMode, MmCached, NULL, FALSE, NormalPagePriority | MdlMappingNoExecute);
    if (va == NULL) {
        LogPrint("alloc %p: aperture mapping of %u pages failed\n", Al, (ULONG)Pages);
        if (Al->ApPartial != NULL) {
            IoFreeMdl(Al->ApPartial);
            Al->ApPartial = NULL;
        }
        return;
    }
    Al->ApMdl = m;
    Al->ApVa = va;
    Al->ApBytes = Pages * PAGE_SIZE;
}

VOID AllocUnmapAperture(TGPU_ALLOCATION *Al)
{
    if (Al == NULL || Al->ApVa == NULL) {
        return;
    }
    MmUnmapLockedPages(Al->ApVa, Al->ApMdl);
    if (Al->ApPartial != NULL) {
        IoFreeMdl(Al->ApPartial);
    }
    Al->ApVa = NULL;
    Al->ApMdl = NULL;
    Al->ApPartial = NULL;
    Al->ApBytes = 0;
}

VOID AllocRelease(TGPU_ALLOCATION *Al)
{
    if (InterlockedDecrement(&Al->Refs) != 0) {
        return;
    }
    AllocUnmapAperture(Al);
    if (Al->Bo != NULL) {
        MsmBoRelease(Al->Bo);
    }
    ExFreePoolWithTag(Al, TGPU_POOL_TAG);
}

static const struct topazgpu_alloc *FindDesc(const VOID *Priv, UINT Size)
{
    const struct topazgpu_alloc *d = (const struct topazgpu_alloc *)Priv;
    return (d != NULL && Size >= sizeof(*d) && d->magic == TOPAZGPU_ALLOC_MAGIC) ? d : NULL;
}

static NTSTATUS APIENTRY TgCreateAllocation(const HANDLE hAdapter, DXGKARG_CREATEALLOCATION *A)
{
    ULONG i;
    static ULONG count;

    UNREFERENCED_PARAMETER(hAdapter);
    for (i = 0; i < A->NumAllocations; i++) {
        DXGK_ALLOCATIONINFO *ai = &A->pAllocationInfo[i];
        const struct topazgpu_alloc *d = FindDesc(ai->pPrivateDriverData, ai->PrivateDriverDataSize);
        TGPU_ALLOCATION *al;

        if (d == NULL) {
            d = FindDesc(A->pPrivateDriverData, A->PrivateDriverDataSize);
        }
        al = (TGPU_ALLOCATION *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*al), TGPU_POOL_TAG);
        if (al == NULL) {
            return STATUS_NO_MEMORY;
        }
        al->Refs = 1;
        if (d != NULL) {
            al->Desc = *d;
        } else {
            /* foreign private data (UMD resource without a BO): a page of bookkeeping as in step A */
            al->Desc.magic = TOPAZGPU_ALLOC_MAGIC;
            al->Desc.size = PAGE_SIZE;
        }
        if (al->Desc.size == 0) {
            al->Desc.size = al->Desc.pitch * al->Desc.height;
        }
        al->Size = ROUND_TO_PAGES(al->Desc.size != 0 ? al->Desc.size : PAGE_SIZE);
        /* v0.32: GDI texture surfaces (window redirection) get a BO so DWM on the UMD can sample them;
           CDD fills them with Present blts, which write into the BO */
        if (al->Desc.bo == 0 && al->Desc.kind == TOPAZGPU_ALLOC_GDI && al->Desc.reserved[0] == D3DKMDT_GDISURFACE_TEXTURE &&
            d != NULL && ai->pPrivateDriverData != NULL && ai->PrivateDriverDataSize >= sizeof(*d)) {
            al->Bo = MsmBoCreate(al->Size);
            if (al->Bo != NULL) {
                al->Desc.bo = al->Bo->Handle;
                al->Desc.bo_offset = 0;
                ((struct topazgpu_alloc *)ai->pPrivateDriverData)->bo = al->Desc.bo;
            }
        } else if (al->Desc.bo != 0) {
            al->Bo = MsmBoAcquire(al->Desc.bo);
            if (al->Bo == NULL || al->Bo->Size < al->Desc.bo_offset + al->Desc.size) {
                LogPrint("CreateAllocation: bo %u missing/too small\n", al->Desc.bo);
                if (al->Bo != NULL) {
                    MsmBoRelease(al->Bo);
                }
                ExFreePoolWithTag(al, TGPU_POOL_TAG);
                return STATUS_INVALID_PARAMETER;
            }
        }
        ai->hAllocation = al;
        ai->Alignment = 0;
        ai->Size = al->Size;
        ai->PitchAlignedSize = 0;
        ai->HintedBank.Value = 0;
        ai->PreferredSegment.Value = 0;
        ai->PreferredSegment.SegmentId0 = 1;
        ai->SupportedReadSegmentSet = 1;
        ai->SupportedWriteSegmentSet = 1;
        ai->EvictionSegmentSet = 1;                      /* v0.35: as viogpu3d */
        ai->MaximumRenamingListLength = 0;
        /* v0.35: Dxgkrnl "KMD should set a non-zero initial priority for allocations" (ETW) */
        ai->AllocationPriority = D3DDDI_ALLOCATIONPRIORITY_NORMAL;
        ai->Flags.Value = 0;
        ai->Flags.CpuVisible = 1;
        if (++count <= 40 || d == NULL) {
            LogPrint("CreateAllocation %p: kind %u %ux%u pitch %u fmt %u bo %u size %u%s%s\n", al, al->Desc.kind,
                     al->Desc.width, al->Desc.height, al->Desc.pitch, al->Desc.format, al->Desc.bo, (ULONG)al->Size,
                     (al->Desc.flags & TOPAZGPU_ALLOC_F_PRIMARY) ? " primary" : "", d == NULL ? " (no desc)" : "");
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgDestroyAllocation(const HANDLE hAdapter, const DXGKARG_DESTROYALLOCATION *A)
{
    ULONG i;

    UNREFERENCED_PARAMETER(hAdapter);
    for (i = 0; i < A->NumAllocations; i++) {
        TGPU_ALLOCATION *al = (TGPU_ALLOCATION *)A->pAllocationList[i];
        DispAllocDestroyed(al);
        AllocRelease(al);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgDescribeAllocation(const HANDLE hAdapter, DXGKARG_DESCRIBEALLOCATION *A)
{
    TGPU_ALLOCATION *al = (TGPU_ALLOCATION *)A->hAllocation;

    UNREFERENCED_PARAMETER(hAdapter);
    A->Width = al->Desc.width;
    A->Height = al->Desc.height;
    A->Format = al->Desc.format != 0 ? (D3DDDIFORMAT)al->Desc.format : D3DDDIFMT_X8R8G8B8;
    A->MultisampleMethod.NumSamples = 0;
    A->MultisampleMethod.NumQualityLevels = 0;
    A->RefreshRate.Numerator = 60;
    A->RefreshRate.Denominator = 1;
    A->PrivateDriverFormatAttribute = 0;
    A->Rotation = D3DDDI_ROTATION_IDENTITY;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgGetStandardAllocationDriverData(const HANDLE hAdapter,
                                                           DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *A)
{
    struct topazgpu_alloc d;

    UNREFERENCED_PARAMETER(hAdapter);
    RtlZeroMemory(&d, sizeof(d));
    d.magic = TOPAZGPU_ALLOC_MAGIC;
    d.kind = A->StandardAllocationType;
    switch (A->StandardAllocationType) {
    case D3DKMDT_STANDARDALLOCATION_SHAREDPRIMARYSURFACE:
        d.width = A->pCreateSharedPrimarySurfaceData->Width;
        d.height = A->pCreateSharedPrimarySurfaceData->Height;
        d.format = A->pCreateSharedPrimarySurfaceData->Format;
        d.vidpn = A->pCreateSharedPrimarySurfaceData->VidPnSourceId;
        d.flags = TOPAZGPU_ALLOC_F_PRIMARY;
        break;
    case D3DKMDT_STANDARDALLOCATION_SHADOWSURFACE:
        d.width = A->pCreateShadowSurfaceData->Width;
        d.height = A->pCreateShadowSurfaceData->Height;
        d.format = A->pCreateShadowSurfaceData->Format;
        A->pCreateShadowSurfaceData->Pitch = d.width * 4;
        break;
    case D3DKMDT_STANDARDALLOCATION_STAGINGSURFACE:
        d.width = A->pCreateStagingSurfaceData->Width;
        d.height = A->pCreateStagingSurfaceData->Height;
        d.format = D3DDDIFMT_X8R8G8B8;
        A->pCreateStagingSurfaceData->Pitch = d.width * 4;
        break;
    case D3DKMDT_STANDARDALLOCATION_GDISURFACE:
        d.width = A->pCreateGdiSurfaceData->Width;
        d.height = A->pCreateGdiSurfaceData->Height;
        d.format = A->pCreateGdiSurfaceData->Format;
        d.reserved[0] = A->pCreateGdiSurfaceData->Type;  /* D3DKMDT_GDISURFACETYPE */
        A->pCreateGdiSurfaceData->Pitch = d.width * 4;
        break;
    default:
        LogPrint("GetStandardAllocationDriverData: type %u not supported\n", A->StandardAllocationType);
        return STATUS_NOT_SUPPORTED;
    }
    d.pitch = d.width * 4;
    d.size = d.pitch * d.height;
    if (A->pAllocationPrivateDriverData != NULL) {
        RtlCopyMemory(A->pAllocationPrivateDriverData, &d, sizeof(d));
    }
    A->AllocationPrivateDriverDataSize = sizeof(d);
    A->ResourcePrivateDriverDataSize = 0;
    LogPrint("GetStandardAllocationDriverData: type %u %ux%u fmt %u%s\n", d.kind, d.width, d.height, d.format,
             A->pAllocationPrivateDriverData != NULL ? "" : " (size query)");
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgOpenAllocation(const HANDLE hDevice, const DXGKARG_OPENALLOCATION *A)
{
    ULONG i;
    TGPU_DEVICE *d = (TGPU_DEVICE *)hDevice;
    for (i = 0; i < A->NumAllocations; i++) {
        DXGKARGCB_GETHANDLEDATA g;
        g.hObject = A->pOpenAllocation[i].hAllocation;
        g.Type = DXGK_HANDLE_ALLOCATION;
        g.Flags.Value = 0;
        A->pOpenAllocation[i].hDeviceSpecificAllocation = d->Adapter->Dxgk.DxgkCbGetHandleData(&g);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgCloseAllocation(const HANDLE hDevice, const DXGKARG_CLOSEALLOCATION *A)
{
    UNREFERENCED_PARAMETER(hDevice);
    UNREFERENCED_PARAMETER(A);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- scheduling (no Dxgkrnl-driven GPU work yet) */

static NTSTATUS APIENTRY TgRender(const HANDLE hContext, DXGKARG_RENDER *A)
{
    UNREFERENCED_PARAMETER(hContext);
    A->pDmaBuffer = (PUCHAR)A->pDmaBuffer + A->CommandLength;
    return STATUS_SUCCESS;
}

/* C2: one TG_CMD in the private data, 4 bytes in the DMA buffer (an empty DMA buffer is completed
   by VidSch without SubmitCommand). Flips need no command: the MMIO flip is SetVidPnSourceAddress. */
static NTSTATUS APIENTRY TgPresent(const HANDLE hContext, DXGKARG_PRESENT *A)
{
    TG_CMD *c = (TG_CMD *)A->pDmaBufferPrivateData;
    const DXGK_ALLOCATIONLIST *src = &A->pAllocationList[DXGK_PRESENT_SOURCE_INDEX];
    const DXGK_ALLOCATIONLIST *dst = &A->pAllocationList[DXGK_PRESENT_DESTINATION_INDEX];
    ULONG i;
    static ULONG count;

    UNREFERENCED_PARAMETER(hContext);
    /* v0.36: flips need no command (the MMIO flip is SetVidPnSourceAddress, as viogpu3d). Before, Dxgkrnl
       handed flips a DMA buffer without private data and we answered STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER
       (0xC01E0001) forever: every DWM flip failed in pfnPresentCb with E_FAIL. */
    if (A->Flags.Flip) {
        if (++count <= 400) {
            LogPrint("Present: flip src %p dma %u priv %u\n", src->hDeviceSpecificAllocation, A->DmaSize,
                     A->DmaBufferPrivateDataSize);
        }
        return STATUS_SUCCESS;
    }
    if (A->DmaSize < 4 || A->DmaBufferPrivateDataSize < sizeof(TG_CMD)) {
        LogPrint("Present: flags %x dma %u priv %u -> insufficient\n", A->Flags.Value, A->DmaSize,
                 A->DmaBufferPrivateDataSize);
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    }
    RtlZeroMemory(c, sizeof(*c));
    c->Op = TG_CMD_NOP;
    if ((A->Flags.Blt || A->Flags.ColorFill) && dst->hDeviceSpecificAllocation != NULL) {
        c->Op = A->Flags.Blt ? TG_CMD_BLT : TG_CMD_FILL;
        c->Src = (TGPU_ALLOCATION *)src->hDeviceSpecificAllocation;
        c->Dst = (TGPU_ALLOCATION *)dst->hDeviceSpecificAllocation;
        c->SrcRect = A->SrcRect;
        c->DstRect = A->DstRect;
        c->Color = A->Color;
        if (A->SubRectCnt > 0 && A->SubRectCnt <= TG_CMD_MAX_RECTS) {
            c->NumRects = A->SubRectCnt;
            for (i = 0; i < A->SubRectCnt; i++) {
                c->Rects[i] = A->pDstSubRects[i];
            }
        } else if (A->SubRectCnt > TG_CMD_MAX_RECTS) {
            /* more rects than fit: copy the bounding box (the source has the whole frame) */
            RECT b = A->pDstSubRects[0];
            for (i = 1; i < A->SubRectCnt; i++) {
                b.left = min(b.left, A->pDstSubRects[i].left);
                b.top = min(b.top, A->pDstSubRects[i].top);
                b.right = max(b.right, A->pDstSubRects[i].right);
                b.bottom = max(b.bottom, A->pDstSubRects[i].bottom);
            }
            c->NumRects = 1;
            c->Rects[0] = b;
        }
        if (c->Op == TG_CMD_BLT && c->Src == NULL) {
            c->Op = TG_CMD_NOP;
        }
    }
    if (++count <= 30 || (A->Flags.Value != 1 && count < 400)) {   /* v0.34: every flip/non-blt present */
        LogPrint("Present: flags %x src %p dst %p src (%d,%d)-(%d,%d) dst (%d,%d)-(%d,%d) rects %u\n", A->Flags.Value,
                 src->hDeviceSpecificAllocation, dst->hDeviceSpecificAllocation, A->SrcRect.left, A->SrcRect.top,
                 A->SrcRect.right, A->SrcRect.bottom, A->DstRect.left, A->DstRect.top, A->DstRect.right,
                 A->DstRect.bottom, A->SubRectCnt);
    }
    /* v0.31: patch locations for both operands, as viogpu3d: without them VidMM left CDD's shadow surface
       in system memory (segment 0) and the blt had no source */
    if (c->Op != TG_CMD_NOP && A->pPatchLocationListOut != NULL && A->PatchLocationListOutSize >= 2) {
        D3DDDI_PATCHLOCATIONLIST *pl = A->pPatchLocationListOut;
        RtlZeroMemory(pl, 2 * sizeof(*pl));
        pl[0].AllocationIndex = DXGK_PRESENT_SOURCE_INDEX;
        pl[0].SlotId = 1;
        pl[0].DriverId = 1;
        pl[1].AllocationIndex = DXGK_PRESENT_DESTINATION_INDEX;
        pl[1].SlotId = 2;
        pl[1].DriverId = 2;
        A->pPatchLocationListOut = pl + 2;
        if (c->Op == TG_CMD_FILL) {
            pl[0] = pl[1];
            A->pPatchLocationListOut = pl + 1;
        }
    }
    *(PULONG)A->pDmaBuffer = c->Op;
    A->pDmaBuffer = (PUCHAR)A->pDmaBuffer + 4;
    A->pDmaBufferPrivateData = c + 1;
    return STATUS_SUCCESS;
}

/* v0.30: final placement of the present source/destination into our commands */
static NTSTATUS APIENTRY TgPatch(const HANDLE hAdapter, const DXGKARG_PATCH *A)
{
    ULONG off;
    static ULONG count;

    UNREFERENCED_PARAMETER(hAdapter);
    if (A->pDmaBufferPrivateData == NULL || A->pAllocationList == NULL) {
        return STATUS_SUCCESS;
    }
    for (off = A->DmaBufferPrivateDataSubmissionStartOffset;
         off + sizeof(TG_CMD) <= A->DmaBufferPrivateDataSubmissionEndOffset; off += sizeof(TG_CMD)) {
        TG_CMD *c = (TG_CMD *)((PUCHAR)A->pDmaBufferPrivateData + off);
        if (c->Op != TG_CMD_BLT && c->Op != TG_CMD_FILL) {
            continue;
        }
        if (A->AllocationListSize > DXGK_PRESENT_DESTINATION_INDEX) {
            c->SrcSeg = A->pAllocationList[DXGK_PRESENT_SOURCE_INDEX].SegmentId;
            c->SrcAddr = (ULONGLONG)A->pAllocationList[DXGK_PRESENT_SOURCE_INDEX].PhysicalAddress.QuadPart;
            c->DstSeg = A->pAllocationList[DXGK_PRESENT_DESTINATION_INDEX].SegmentId;
            c->DstAddr = (ULONGLONG)A->pAllocationList[DXGK_PRESENT_DESTINATION_INDEX].PhysicalAddress.QuadPart;
        }
        if (++count <= 10 || (c->Op == TG_CMD_BLT && c->SrcSeg == 0 && count < 40)) {
            LogPrint("Patch: op %u src seg %u %llx dst seg %u %llx (list %u)\n", c->Op, c->SrcSeg, c->SrcAddr, c->DstSeg,
                     c->DstAddr, A->AllocationListSize);
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgSubmitCommand(const HANDLE hAdapter, const DXGKARG_SUBMITCOMMAND *A)
{
    TGPU_ADAPTER *a = (TGPU_ADAPTER *)hAdapter;
    const UCHAR *cmds = NULL;
    ULONG bytes = 0;
    static ULONG count;

    if (A->pDmaBufferPrivateData != NULL &&
        A->DmaBufferPrivateDataSubmissionEndOffset > A->DmaBufferPrivateDataSubmissionStartOffset) {
        cmds = (const UCHAR *)A->pDmaBufferPrivateData + A->DmaBufferPrivateDataSubmissionStartOffset;
        bytes = A->DmaBufferPrivateDataSubmissionEndOffset - A->DmaBufferPrivateDataSubmissionStartOffset;
    }
    if (++count <= 40) {
        LogPrint("SubmitCommand: fence %u flags %x dma %u priv %u (irql %u)\n", A->SubmissionFenceId, A->Flags.Value,
                 A->DmaBufferSubmissionEndOffset - A->DmaBufferSubmissionStartOffset, bytes, KeGetCurrentIrql());
    }
    a->LastSubmittedFence = A->SubmissionFenceId;
    if (!NT_SUCCESS(EngSubmit(cmds, bytes, A->SubmissionFenceId))) {
        CompleteFence(a, A->SubmissionFenceId);          /* engine gone: complete without work */
    }
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgPreemptCommand(const HANDLE hAdapter, const DXGKARG_PREEMPTCOMMAND *A)
{
    LogPrint("PreemptCommand: fence %u\n", A->PreemptionFenceId);
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(A);
    return STATUS_SUCCESS;
}

/* C2: aperture map/unmap act at once (as viogpu3d: VidMM evicts only idle allocations); FILL becomes
   an engine command. Transfers between the aperture and system memory would copy a page set onto
   itself (the aperture is backed by the same system pages): nothing to do. */
static NTSTATUS APIENTRY TgBuildPagingBuffer(const HANDLE hAdapter, DXGKARG_BUILDPAGINGBUFFER *A)
{
    static ULONG count;
    TG_CMD *c;

    UNREFERENCED_PARAMETER(hAdapter);
    switch (A->Operation) {
    case DXGK_OPERATION_MAP_APERTURE_SEGMENT:
        GartMap(A->MapApertureSegment.OffsetInPages, A->MapApertureSegment.pMdl, A->MapApertureSegment.MdlOffset,
                A->MapApertureSegment.NumberOfPages);
        AllocMapAperture((TGPU_ALLOCATION *)A->MapApertureSegment.hAllocation, A->MapApertureSegment.pMdl,
                         A->MapApertureSegment.MdlOffset, A->MapApertureSegment.NumberOfPages);
        break;
    case DXGK_OPERATION_UNMAP_APERTURE_SEGMENT:
        GartUnmap(A->UnmapApertureSegment.OffsetInPages, A->UnmapApertureSegment.NumberOfPages);
        AllocUnmapAperture((TGPU_ALLOCATION *)A->UnmapApertureSegment.hAllocation);
        break;
    case DXGK_OPERATION_FILL:
        if (A->Fill.hAllocation == NULL) {
            break;
        }
        if (A->DmaSize < 4 || A->DmaBufferPrivateDataSize < sizeof(TG_CMD)) {
            return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
        }
        c = (TG_CMD *)A->pDmaBufferPrivateData;
        RtlZeroMemory(c, sizeof(*c));
        c->Op = TG_CMD_PG_FILL;
        c->Dst = (TGPU_ALLOCATION *)A->Fill.hAllocation;
        c->Bytes = A->Fill.FillSize;
        c->Color = A->Fill.FillPattern;
        *(PULONG)A->pDmaBuffer = c->Op;
        A->pDmaBuffer = (PUCHAR)A->pDmaBuffer + 4;
        A->pDmaBufferPrivateData = c + 1;
        break;
    default:
        break;
    }
    if (++count <= 60) {
        LogPrint("BuildPagingBuffer: op %u ap page %llx pages %llx alloc %p\n", A->Operation,
                 A->Operation == DXGK_OPERATION_MAP_APERTURE_SEGMENT ? (ULONGLONG)A->MapApertureSegment.OffsetInPages : 0ull,
                 A->Operation == DXGK_OPERATION_MAP_APERTURE_SEGMENT ? (ULONGLONG)A->MapApertureSegment.NumberOfPages : 0ull,
                 A->Operation == DXGK_OPERATION_MAP_APERTURE_SEGMENT ? A->MapApertureSegment.hAllocation :
                 A->Operation == DXGK_OPERATION_UNMAP_APERTURE_SEGMENT ? A->UnmapApertureSegment.hAllocation :
                 A->Operation == DXGK_OPERATION_FILL ? A->Fill.hAllocation : NULL);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgResetFromTimeout(const HANDLE hAdapter)
{
    LogPrint("%s\n", "TgResetFromTimeout");
    UNREFERENCED_PARAMETER(hAdapter);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgRestartFromTimeout(const HANDLE hAdapter)
{
    UNREFERENCED_PARAMETER(hAdapter);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgCollectDbgInfo(const HANDLE hAdapter, const DXGKARG_COLLECTDBGINFO *A)
{
    LogPrint("%s\n", "TgCollectDbgInfo");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(A);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgQueryCurrentFence(const HANDLE hAdapter, DXGKARG_QUERYCURRENTFENCE *A)
{
    TGPU_ADAPTER *a = (TGPU_ADAPTER *)hAdapter;
    A->CurrentFence = a->LastCompletedFence;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgControlInterrupt(const HANDLE hAdapter, const DXGK_INTERRUPT_TYPE Type, BOOLEAN Enable)
{
    UNREFERENCED_PARAMETER(hAdapter);
    LogPrint("ControlInterrupt: type %u enable %u\n", Type, Enable);
    if (Type == DXGK_INTERRUPT_CRTC_VSYNC) {
        DispVsyncEnable(Enable);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgGetNodeMetadata(const HANDLE hAdapter, UINT NodeOrdinal, DXGKARG_GETNODEMETADATA *A)
{
    LogPrint("%s\n", "TgGetNodeMetadata");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(NodeOrdinal);
    RtlZeroMemory(A, sizeof(*A));
    A->EngineType = DXGK_ENGINE_TYPE_3D;
    RtlStringCbCopyW(A->FriendlyName, sizeof(A->FriendlyName), L"Adreno 610 3D");
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgCancelCommand(const HANDLE hAdapter, const DXGKARG_CANCELCOMMAND *A)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(A);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgQueryEngineStatus(const HANDLE hAdapter, DXGKARG_QUERYENGINESTATUS *A)
{
    LogPrint("%s\n", "TgQueryEngineStatus");
    UNREFERENCED_PARAMETER(hAdapter);
    A->EngineStatus.Responsive = 1;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgResetEngine(const HANDLE hAdapter, DXGKARG_RESETENGINE *A)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(A);
    return STATUS_SUCCESS;
}


/* ---------------------------------------------------------------- display DDIs (disp.c) */

static NTSTATUS TgQueryChildRelations(const PVOID Ctx, PDXGK_CHILD_DESCRIPTOR Rel, ULONG Size)
{
    UNREFERENCED_PARAMETER(Ctx);
    return DispQueryChildRelations(Rel, Size);
}

static NTSTATUS TgQueryChildStatus(const PVOID Ctx, PDXGK_CHILD_STATUS St, BOOLEAN NonDestructive)
{
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(NonDestructive);
    return DispQueryChildStatus(St);
}

static NTSTATUS TgQueryDeviceDescriptor(const PVOID Ctx, ULONG Uid, PDXGK_DEVICE_DESCRIPTOR Desc)
{
    UNREFERENCED_PARAMETER(Ctx);
    return DispQueryDeviceDescriptor(Uid, Desc);
}

static NTSTATUS APIENTRY TgIsSupportedVidPn(const HANDLE hAdapter, DXGKARG_ISSUPPORTEDVIDPN *A)
{
    UNREFERENCED_PARAMETER(hAdapter);
    A->IsVidPnSupported = TRUE;                          /* one source, one target: any topology of them */
    LogPrint("IsSupportedVidPn %p -> yes\n", A->hDesiredVidPn);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgRecommendFunctionalVidPn(const HANDLE hAdapter, const DXGKARG_RECOMMENDFUNCTIONALVIDPN *const A)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(A);
    LogPrint("RecommendFunctionalVidPn -> none\n");
    return STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;
}

static NTSTATUS APIENTRY TgRecommendVidPnTopology(const HANDLE hAdapter, const DXGKARG_RECOMMENDVIDPNTOPOLOGY *const A)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(A);
    return STATUS_GRAPHICS_NO_RECOMMENDED_VIDPN_TOPOLOGY;
}

static NTSTATUS APIENTRY TgRecommendMonitorModes(const HANDLE hAdapter, const DXGKARG_RECOMMENDMONITORMODES *const A)
{
    UNREFERENCED_PARAMETER(hAdapter);
    LogPrint("RecommendMonitorModes target %u\n", A->VideoPresentTargetId);
    return DispRecommendMonitorModes(A);
}

static NTSTATUS APIENTRY TgEnumVidPnCofuncModality(const HANDLE hAdapter, const DXGKARG_ENUMVIDPNCOFUNCMODALITY *const A)
{
    LogPrint("EnumVidPnCofuncModality pivot %u\n", A->EnumPivotType);
    return DispEnumCofuncModality(&((TGPU_ADAPTER *)hAdapter)->Dxgk, A);
}

static NTSTATUS APIENTRY TgCommitVidPn(const HANDLE hAdapter, const DXGKARG_COMMITVIDPN *const A)
{
    return DispCommitVidPn(&((TGPU_ADAPTER *)hAdapter)->Dxgk, A);
}

static NTSTATUS APIENTRY TgSetVidPnSourceAddress(const HANDLE hAdapter, const DXGKARG_SETVIDPNSOURCEADDRESS *A)
{
    UNREFERENCED_PARAMETER(hAdapter);
    return DispSetSourceAddress(A);
}

static NTSTATUS APIENTRY TgSetVidPnSourceVisibility(const HANDLE hAdapter, const DXGKARG_SETVIDPNSOURCEVISIBILITY *A)
{
    UNREFERENCED_PARAMETER(hAdapter);
    return DispSetVisibility(A);
}

static NTSTATUS APIENTRY TgUpdateActiveVidPnPresentPath(const HANDLE hAdapter,
                                                        const DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *const A)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(A);
    return STATUS_SUCCESS;
}

#define VIDPN_STUB(Name, ArgType) \
    static NTSTATUS APIENTRY Name(const HANDLE hAdapter, ArgType A) \
    { UNREFERENCED_PARAMETER(hAdapter); UNREFERENCED_PARAMETER(A); return STATUS_SUCCESS; }

VIDPN_STUB(TgStopCapture, const DXGKARG_STOPCAPTURE *)
VIDPN_STUB(TgSetPalette, const DXGKARG_SETPALETTE *)
VIDPN_STUB(TgSetPointerPosition, const DXGKARG_SETPOINTERPOSITION *)
VIDPN_STUB(TgSetPointerShape, const DXGKARG_SETPOINTERSHAPE *)

static NTSTATUS APIENTRY TgGetScanLine(const HANDLE hAdapter, DXGKARG_GETSCANLINE *A)
{
    UNREFERENCED_PARAMETER(hAdapter);
    return DispGetScanLine(A);
}

static NTSTATUS APIENTRY TgQueryVidPnHWCapability(const HANDLE hAdapter, DXGKARG_QUERYVIDPNHWCAPABILITY *A)
{
    UNREFERENCED_PARAMETER(hAdapter);
    RtlZeroMemory(&A->VidPnHWCaps, sizeof(A->VidPnHWCaps));
    return STATUS_SUCCESS;
}

static NTSTATUS TgStopDeviceAndReleasePostDisplayOwnership(PVOID Ctx, D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                           PDXGK_DISPLAY_INFORMATION Info)
{
    UNREFERENCED_PARAMETER(Ctx);
    LogPrint("StopDeviceAndReleasePostDisplayOwnership: target %u\n", TargetId);
    DispGetPostInfo(Info);
    DispStop();
    EngStop();
    return STATUS_SUCCESS;
}

static NTSTATUS TgSystemDisplayEnable(PVOID Ctx, D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                      PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags, UINT *Width, UINT *Height,
                                      D3DDDIFORMAT *Format)
{
    DXGK_DISPLAY_INFORMATION i;

    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(TargetId);
    UNREFERENCED_PARAMETER(Flags);
    DispGetPostInfo(&i);
    *Width = i.Width;
    *Height = i.Height;
    *Format = D3DDDIFMT_X8R8G8B8;
    return STATUS_SUCCESS;
}

static VOID TgSystemDisplayWrite(PVOID Ctx, PVOID Src, UINT W, UINT H, UINT Stride, UINT X, UINT Y)
{
    UNREFERENCED_PARAMETER(Ctx);
    DispSystemWrite(Src, W, H, Stride, X, Y);
}

/* ---------------------------------------------------------------- entry */

DRIVER_INITIALIZE DriverEntry;

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    DRIVER_INITIALIZATION_DATA d;
    NTSTATUS st;

    LogOpen();
    LogPrint("==== TopazGpuW " TGPU_VERSION " (display + msm escapes, step C2, 1 ms high-res vsync, gpu.600 + gpu.mhz options) ====\n");
    MsmInit();
    RtlZeroMemory(&d, sizeof(d));
    d.Version = DXGKDDI_INTERFACE_VERSION_WDDM1_3;
    d.DxgkDdiAddDevice = TgAddDevice;
    d.DxgkDdiStartDevice = TgStartDevice;
    d.DxgkDdiStopDevice = TgStopDevice;
    d.DxgkDdiRemoveDevice = TgRemoveDevice;
    d.DxgkDdiDispatchIoRequest = TgDispatchIoRequest;
    d.DxgkDdiInterruptRoutine = TgInterruptRoutine;
    d.DxgkDdiDpcRoutine = TgDpcRoutine;
    /* step C2: display DDIs live in disp.c (1 source, 1 target: the panel) */
    d.DxgkDdiQueryChildRelations = TgQueryChildRelations;
    d.DxgkDdiQueryChildStatus = TgQueryChildStatus;
    d.DxgkDdiQueryDeviceDescriptor = TgQueryDeviceDescriptor;
    d.DxgkDdiIsSupportedVidPn = TgIsSupportedVidPn;
    d.DxgkDdiRecommendFunctionalVidPn = TgRecommendFunctionalVidPn;
    d.DxgkDdiEnumVidPnCofuncModality = TgEnumVidPnCofuncModality;
    d.DxgkDdiSetVidPnSourceAddress = TgSetVidPnSourceAddress;
    d.DxgkDdiSetVidPnSourceVisibility = TgSetVidPnSourceVisibility;
    d.DxgkDdiCommitVidPn = TgCommitVidPn;
    d.DxgkDdiUpdateActiveVidPnPresentPath = TgUpdateActiveVidPnPresentPath;
    d.DxgkDdiRecommendMonitorModes = TgRecommendMonitorModes;
    d.DxgkDdiRecommendVidPnTopology = TgRecommendVidPnTopology;
    d.DxgkDdiGetScanLine = TgGetScanLine;
    d.DxgkDdiStopCapture = TgStopCapture;
    d.DxgkDdiSetPalette = TgSetPalette;
    d.DxgkDdiSetPointerPosition = TgSetPointerPosition;
    d.DxgkDdiSetPointerShape = TgSetPointerShape;
    d.DxgkDdiQueryVidPnHWCapability = TgQueryVidPnHWCapability;
    d.DxgkDdiStopDeviceAndReleasePostDisplayOwnership = TgStopDeviceAndReleasePostDisplayOwnership;
    d.DxgkDdiSystemDisplayEnable = TgSystemDisplayEnable;
    d.DxgkDdiSystemDisplayWrite = TgSystemDisplayWrite;
    d.DxgkDdiSetPowerState = TgSetPowerState;
    d.DxgkDdiResetDevice = TgResetDevice;
    d.DxgkDdiUnload = TgUnload;
    d.DxgkDdiQueryInterface = TgQueryInterface;
    d.DxgkDdiQueryAdapterInfo = TgQueryAdapterInfo;
    d.DxgkDdiEscape = TgEscape;
    d.DxgkDdiCreateDevice = TgCreateDevice;
    d.DxgkDdiDestroyDevice = TgDestroyDevice;
    d.DxgkDdiCreateContext = TgCreateContext;
    d.DxgkDdiDestroyContext = TgDestroyContext;
    d.DxgkDdiCreateAllocation = TgCreateAllocation;
    d.DxgkDdiDestroyAllocation = TgDestroyAllocation;
    d.DxgkDdiDescribeAllocation = TgDescribeAllocation;
    d.DxgkDdiGetStandardAllocationDriverData = TgGetStandardAllocationDriverData;
    d.DxgkDdiOpenAllocation = TgOpenAllocation;
    d.DxgkDdiCloseAllocation = TgCloseAllocation;
    d.DxgkDdiRender = TgRender;
    d.DxgkDdiPresent = TgPresent;
    d.DxgkDdiPatch = TgPatch;
    d.DxgkDdiSubmitCommand = TgSubmitCommand;
    d.DxgkDdiPreemptCommand = TgPreemptCommand;
    d.DxgkDdiBuildPagingBuffer = TgBuildPagingBuffer;
    d.DxgkDdiResetFromTimeout = TgResetFromTimeout;
    d.DxgkDdiRestartFromTimeout = TgRestartFromTimeout;
    d.DxgkDdiCollectDbgInfo = TgCollectDbgInfo;
    d.DxgkDdiQueryCurrentFence = TgQueryCurrentFence;
    d.DxgkDdiControlInterrupt = TgControlInterrupt;
    d.DxgkDdiGetNodeMetadata = TgGetNodeMetadata;
    d.DxgkDdiCancelCommand = TgCancelCommand;
    d.DxgkDdiQueryEngineStatus = TgQueryEngineStatus;
    d.DxgkDdiResetEngine = TgResetEngine;
    st = DxgkInitialize(DriverObject, RegistryPath, &d);
    LogPrint("DxgkInitialize: %08x\n", st);
    return st;
}
