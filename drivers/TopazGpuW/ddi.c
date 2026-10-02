/*
 * WDDM 1.3 DDIs of TopazGpuW, step A: a render-only adapter (no VidPN sources, no children).
 * The real work is in DxgkDdiEscape (msm ioctls for the freedreno UMD). Allocations/contexts are
 * minimal bookkeeping; DMA buffers submitted by Dxgkrnl (paging) complete immediately.
 * Shape follows viogpu3d (virtio-win PR #943, BSD-3) and the WDK samples.
 */
#include "tgpu.h"

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

typedef struct _TGPU_ALLOCATION {
    SIZE_T Size;
} TGPU_ALLOCATION;

static TGPU_ADAPTER *g_Adapter;

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
    BOOLEAN ret;
    A->Dxgk.DxgkCbSynchronizeExecution(A->Dxgk.DeviceHandle, NotifyRoutine, &n, 0, &ret);
}

/* ---------------------------------------------------------------- adapter */

static NTSTATUS TgAddDevice(const PDEVICE_OBJECT Pdo, PVOID *Ctx)
{
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
    RtlCopyMemory(&a->Dxgk, Dxgk, sizeof(a->Dxgk));
    *NumSources = 0;                                     /* render-only (step A) */
    *NumChildren = 0;
    a->Started = TRUE;
    LogPrint("StartDevice: render-only, GPU starts on the first escape\n");
    return STATUS_SUCCESS;
}

static NTSTATUS TgStopDevice(const PVOID Ctx)
{
    UNREFERENCED_PARAMETER(Ctx);
    LogPrint("StopDevice\n");
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

static BOOLEAN TgInterruptRoutine(const PVOID Ctx, ULONG Msg)
{
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(Msg);
    return FALSE;
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
    LogClose();
}

static NTSTATUS TgQueryInterface(const PVOID Ctx, PQUERY_INTERFACE Qi)
{
    LogPrint("%s\n", "TgQueryInterface");
    UNREFERENCED_PARAMETER(Ctx);
    UNREFERENCED_PARAMETER(Qi);
    return STATUS_NOT_SUPPORTED;
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
        if (Q->OutputDataSize < sizeof(*c)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        RtlZeroMemory(c, Q->OutputDataSize);
        c->WDDMVersion = DXGKDDI_WDDMv1_3;
        c->HighestAcceptableAddress.QuadPart = (LONGLONG)-1;
        c->MaxAllocationListSlotId = 16;
        c->SchedulingCaps.MultiEngineAware = 1;
        c->SchedulingCaps.PreemptionAware = 1;
        c->PreemptionCaps.GraphicsPreemptionGranularity = D3DKMDT_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY;
        c->PreemptionCaps.ComputePreemptionGranularity = D3DKMDT_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY;
        c->GpuEngineTopology.NbAsymetricProcessingNodes = 1;
        c->SupportNonVGA = TRUE;
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
            s->PagingBufferPrivateDataSize = 0;
            s->PagingBufferSegmentId = 1;
            s->PagingBufferSize = 16 * PAGE_SIZE;
            /* VidMM's view only: real backing and GPU VAs belong to msm.c */
            d[0].BaseAddress.QuadPart = 0xC0000000;
            d[0].Size = 1024ull * 1024 * 1024;
            d[0].CommitLimit = 1024ull * 1024 * 1024;
            d[0].Flags.Aperture = TRUE;
            d[0].Flags.CacheCoherent = TRUE;
        }
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
    return MsmEscape((struct topazgpu_escape *)E->pPrivateDriverData);
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

static NTSTATUS APIENTRY TgCreateAllocation(const HANDLE hAdapter, DXGKARG_CREATEALLOCATION *A)
{
    LogPrint("%s\n", "TgCreateAllocation");
    ULONG i;
    UNREFERENCED_PARAMETER(hAdapter);
    for (i = 0; i < A->NumAllocations; i++) {
        DXGK_ALLOCATIONINFO *ai = &A->pAllocationInfo[i];
        TGPU_ALLOCATION *al = (TGPU_ALLOCATION *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*al), TGPU_POOL_TAG);
        if (al == NULL) {
            return STATUS_NO_MEMORY;
        }
        al->Size = PAGE_SIZE;
        ai->hAllocation = al;
        ai->Alignment = 0;
        ai->Size = PAGE_SIZE;
        ai->PitchAlignedSize = 0;
        ai->HintedBank.Value = 0;
        ai->PreferredSegment.Value = 0;
        ai->SupportedReadSegmentSet = 1;
        ai->SupportedWriteSegmentSet = 1;
        ai->EvictionSegmentSet = 0;
        ai->MaximumRenamingListLength = 0;
        ai->Flags.Value = 0;
        ai->Flags.CpuVisible = 1;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgDestroyAllocation(const HANDLE hAdapter, const DXGKARG_DESTROYALLOCATION *A)
{
    ULONG i;
    UNREFERENCED_PARAMETER(hAdapter);
    for (i = 0; i < A->NumAllocations; i++) {
        ExFreePoolWithTag(A->pAllocationList[i], TGPU_POOL_TAG);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgDescribeAllocation(const HANDLE hAdapter, DXGKARG_DESCRIBEALLOCATION *A)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(A);
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS APIENTRY TgGetStandardAllocationDriverData(const HANDLE hAdapter,
                                                           DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *A)
{
    UNREFERENCED_PARAMETER(hAdapter);
    A->AllocationPrivateDriverDataSize = 0;
    A->ResourcePrivateDriverDataSize = 0;
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

static NTSTATUS APIENTRY TgPresent(const HANDLE hContext, DXGKARG_PRESENT *A)
{
    UNREFERENCED_PARAMETER(hContext);
    UNREFERENCED_PARAMETER(A);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgPatch(const HANDLE hAdapter, const DXGKARG_PATCH *A)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(A);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgSubmitCommand(const HANDLE hAdapter, const DXGKARG_SUBMITCOMMAND *A)
{
    LogPrint("%s\n", "TgSubmitCommand");
    TGPU_ADAPTER *a = (TGPU_ADAPTER *)hAdapter;
    a->LastSubmittedFence = A->SubmissionFenceId;
    CompleteFence(a, A->SubmissionFenceId);              /* nothing to execute: done at once */
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgPreemptCommand(const HANDLE hAdapter, const DXGKARG_PREEMPTCOMMAND *A)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(A);
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY TgBuildPagingBuffer(const HANDLE hAdapter, DXGKARG_BUILDPAGINGBUFFER *A)
{
    LogPrint("%s\n", "TgBuildPagingBuffer");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(A);
    return STATUS_SUCCESS;                               /* VidMM placement is a bookkeeping fiction */
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
    LogPrint("%s\n", "TgControlInterrupt");
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(Type);
    UNREFERENCED_PARAMETER(Enable);
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

/* ---------------------------------------------------------------- entry */

DRIVER_INITIALIZE DriverEntry;

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    DRIVER_INITIALIZATION_DATA d;
    NTSTATUS st;

    LogOpen();
    LogPrint("==== TopazGpuW " TGPU_VERSION " (render-only, msm escapes) ====\n");
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
    /* render-only: every display DDI must stay NULL (v0.2: with the child DDIs set, Dxgkrnl stopped
       the adapter right after StartDevice) */
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
