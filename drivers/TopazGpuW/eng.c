/*
 * Step C2: the CPU "engine" of TopazGpuW. Dxgkrnl's DMA buffers (paging buffers from VidMM,
 * present/blt buffers from DxgkDdiPresent) carry TG_CMD records in their private data; SubmitCommand
 * (DISPATCH_LEVEL) copies the records into a FIFO and a passive worker thread executes them with the
 * CPU (blt, fill, aperture map/unmap), then reports the fence. One thread = submission order = fence
 * order. The same thread also refreshes the panel from the scanned-out primary (disp.c).
 * Rendering by the Adreno itself still goes through the msm escapes (msm.c), not through here.
 */
#include "tgpu.h"

typedef struct _ENG_ITEM {
    LIST_ENTRY Link;
    ULONG      Fence;
    ULONG      Bytes;
    UCHAR      Cmds[1];
} ENG_ITEM;

static LIST_ENTRY    g_Queue;
static KSPIN_LOCK    g_QueueLock;
static KEVENT        g_Kick, g_Stop;
static PKTHREAD      g_Thread;
static TG_FENCE_DONE g_Done;
static PVOID         g_DoneCtx;
static volatile LONG g_ScanKick;

/* CPU view of a present operand: its BO / own aperture mapping, else the aperture address (v0.30) */
static PUCHAR View(TGPU_ALLOCATION *Al, ULONG Seg, ULONGLONG Addr, PMDL *Tmp)
{
    PUCHAR p = AllocPixels(Al);

    *Tmp = NULL;
    if (p == NULL && Al != NULL && Seg == 1) {
        p = (PUCHAR)GartMapVa(Addr, Al->Size, Tmp);
    }
    return p;
}

static VOID Blt(const TG_CMD *C)
{
    PMDL ms, md;
    PUCHAR s = View(C->Src, C->SrcSeg, C->SrcAddr, &ms), d = View(C->Dst, C->DstSeg, C->DstAddr, &md);
    ULONG i, n = C->NumRects ? C->NumRects : 1;
    LONG dx = C->SrcRect.left - C->DstRect.left, dy = C->SrcRect.top - C->DstRect.top;
    static ULONG fails;

    if (s == NULL || d == NULL) {
        if (++fails <= 10) {
            LogPrint("eng: blt without CPU view (src %p/%p seg %u %llx, dst %p/%p seg %u %llx)\n", C->Src, s, C->SrcSeg,
                     C->SrcAddr, C->Dst, d, C->DstSeg, C->DstAddr);
        }
        GartUnmapVa(s, ms);
        GartUnmapVa(d, md);
        return;
    }
    if (C->Rotation == D3DKMDT_VPPR_ROTATE90 || C->Rotation == D3DKMDT_VPPR_ROTATE180 ||
        C->Rotation == D3DKMDT_VPPR_ROTATE270) {
        /* v0.48: Present with Flags.Rotate - rects are in source (unrotated) coordinates; every source pixel
           (x,y) of the W x H source goes to its rotated position in the physical-orientation destination */
        LONG W = (LONG)C->Src->Desc.width, H = (LONG)C->Src->Desc.height;
        LONG DW = (LONG)C->Dst->Desc.width, DH = (LONG)C->Dst->Desc.height;
        for (i = 0; i < n; i++) {
            RECT r = C->NumRects ? C->Rects[i] : C->SrcRect;
            LONG x, y;
            if (r.left < 0) r.left = 0;
            if (r.top < 0) r.top = 0;
            if (r.right > W) r.right = W;
            if (r.bottom > H) r.bottom = H;
            for (y = r.top; y < r.bottom; y++) {
                const ULONG *srow = (const ULONG *)(s + (SIZE_T)y * C->Src->Desc.pitch);
                for (x = r.left; x < r.right; x++) {
                    LONG ox, oy;
                    if (C->Rotation == D3DKMDT_VPPR_ROTATE90) {
                        ox = H - 1 - y; oy = x;
                    } else if (C->Rotation == D3DKMDT_VPPR_ROTATE270) {
                        ox = y; oy = W - 1 - x;
                    } else {
                        ox = W - 1 - x; oy = H - 1 - y;
                    }
                    if (ox >= 0 && oy >= 0 && ox < DW && oy < DH) {
                        *(ULONG *)(d + (SIZE_T)oy * C->Dst->Desc.pitch + (SIZE_T)ox * 4) = srow[x];
                    }
                }
            }
        }
        GartUnmapVa(s, ms);
        GartUnmapVa(d, md);
        {
            RECT all = { 0, 0, DW, DH };
            DispPresentRects(C->Dst, &all, 1);
        }
        return;
    }
    for (i = 0; i < n; i++) {
        RECT r = C->NumRects ? C->Rects[i] : C->DstRect;
        LONG y, w;

        /* clip against both surfaces (1:1 copies only; stretch is not reported in the caps) */
        if (r.left < 0) r.left = 0;
        if (r.top < 0) r.top = 0;
        if (r.right > (LONG)C->Dst->Desc.width) r.right = C->Dst->Desc.width;
        if (r.bottom > (LONG)C->Dst->Desc.height) r.bottom = C->Dst->Desc.height;
        if (r.left + dx < 0) r.left = -dx;
        if (r.top + dy < 0) r.top = -dy;
        if (r.right + dx > (LONG)C->Src->Desc.width) r.right = C->Src->Desc.width - dx;
        if (r.bottom + dy > (LONG)C->Src->Desc.height) r.bottom = C->Src->Desc.height - dy;
        w = r.right - r.left;
        if (w <= 0 || r.bottom <= r.top) {
            continue;
        }
        for (y = r.top; y < r.bottom; y++) {
            RtlCopyMemory(d + (SIZE_T)y * C->Dst->Desc.pitch + (SIZE_T)r.left * 4,
                          s + (SIZE_T)(y + dy) * C->Src->Desc.pitch + (SIZE_T)(r.left + dx) * 4, (SIZE_T)w * 4);
        }
    }
    GartUnmapVa(s, ms);
    GartUnmapVa(d, md);
    DispPresentRects(C->Dst, C->NumRects ? C->Rects : &C->DstRect, n);
}

static VOID Fill(const TG_CMD *C)
{
    PMDL md;
    PUCHAR d = View(C->Dst, C->DstSeg, C->DstAddr, &md);
    ULONG i, n = C->NumRects ? C->NumRects : 1;

    if (d == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        RECT r = C->NumRects ? C->Rects[i] : C->DstRect;
        LONG x, y;
        if (r.left < 0) r.left = 0;
        if (r.top < 0) r.top = 0;
        if (r.right > (LONG)C->Dst->Desc.width) r.right = C->Dst->Desc.width;
        if (r.bottom > (LONG)C->Dst->Desc.height) r.bottom = C->Dst->Desc.height;
        for (y = r.top; y < r.bottom; y++) {
            PULONG row = (PULONG)(d + (SIZE_T)y * C->Dst->Desc.pitch);
            for (x = r.left; x < r.right; x++) {
                row[x] = C->Color;
            }
        }
    }
    GartUnmapVa(d, md);
    DispPresentRects(C->Dst, C->NumRects ? C->Rects : &C->DstRect, n);
}

static VOID PagingFill(const TG_CMD *C)
{
    PULONG d = (PULONG)AllocPixels(C->Dst);
    SIZE_T i, n = C->Bytes / 4;

    if (d == NULL) {
        return;
    }
    if (n * 4 > C->Dst->Size) {
        n = C->Dst->Size / 4;
    }
    for (i = 0; i < n; i++) {
        d[i] = C->Color;
    }
}

static VOID Execute(const ENG_ITEM *It)
{
    ULONG off;

    for (off = 0; off + sizeof(TG_CMD) <= It->Bytes; off += sizeof(TG_CMD)) {
        const TG_CMD *c = (const TG_CMD *)(It->Cmds + off);
        switch (c->Op) {
        case TG_CMD_BLT:
            Blt(c);
            break;
        case TG_CMD_FILL:
            Fill(c);
            break;
        case TG_CMD_PG_FILL:
            PagingFill(c);
            break;
        case TG_CMD_PG_MAP:
            AllocMapAperture(c->Dst, c->Mdl, c->MdlOffset, c->Pages);
            break;
        case TG_CMD_PG_UNMAP:
            AllocUnmapAperture(c->Dst);
            break;
        default:
            break;
        }
    }
}

static KSTART_ROUTINE EngThread;
static VOID EngThread(PVOID Ctx)
{
    PVOID objs[2] = { &g_Kick, &g_Stop };
    KIRQL irql;
    NTSTATUS st;

    UNREFERENCED_PARAMETER(Ctx);
    for (;;) {
        st = KeWaitForMultipleObjects(2, objs, WaitAny, Executive, KernelMode, FALSE, NULL, NULL);
        for (;;) {
            ENG_ITEM *it = NULL;
            KeAcquireSpinLock(&g_QueueLock, &irql);
            if (!IsListEmpty(&g_Queue)) {
                it = CONTAINING_RECORD(RemoveHeadList(&g_Queue), ENG_ITEM, Link);
            }
            KeReleaseSpinLock(&g_QueueLock, irql);
            if (it == NULL) {
                break;
            }
            Execute(it);
            g_Done(g_DoneCtx, it->Fence);
            ExFreePoolWithTag(it, TGPU_POOL_TAG);
        }
        if (InterlockedExchange(&g_ScanKick, 0)) {
            DispScanoutWork();
        }
        if (st == STATUS_WAIT_1) {
            break;
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS EngStart(TG_FENCE_DONE Done, PVOID Ctx)
{
    HANDLE th;
    NTSTATUS st;

    if (g_Thread != NULL) {
        return STATUS_SUCCESS;
    }
    InitializeListHead(&g_Queue);
    KeInitializeSpinLock(&g_QueueLock);
    KeInitializeEvent(&g_Kick, SynchronizationEvent, FALSE);
    KeInitializeEvent(&g_Stop, NotificationEvent, FALSE);
    g_Done = Done;
    g_DoneCtx = Ctx;
    st = PsCreateSystemThread(&th, THREAD_ALL_ACCESS, NULL, NULL, NULL, EngThread, NULL);
    if (!NT_SUCCESS(st)) {
        return st;
    }
    st = ObReferenceObjectByHandle(th, SYNCHRONIZE, *PsThreadType, KernelMode, (PVOID *)&g_Thread, NULL);
    ZwClose(th);
    if (!NT_SUCCESS(st)) {
        g_Thread = NULL;
    }
    return st;
}

VOID EngStop(VOID)
{
    if (g_Thread == NULL) {
        return;
    }
    KeSetEvent(&g_Stop, IO_NO_INCREMENT, FALSE);
    KeWaitForSingleObject(g_Thread, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(g_Thread);
    g_Thread = NULL;
    while (!IsListEmpty(&g_Queue)) {
        ExFreePoolWithTag(CONTAINING_RECORD(RemoveHeadList(&g_Queue), ENG_ITEM, Link), TGPU_POOL_TAG);
    }
}

NTSTATUS EngSubmit(const VOID *Cmds, ULONG Bytes, ULONG Fence)
{
    ENG_ITEM *it;
    KIRQL irql;

    if (g_Thread == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    it = (ENG_ITEM *)ExAllocatePool2(POOL_FLAG_NON_PAGED, FIELD_OFFSET(ENG_ITEM, Cmds) + Bytes + 1, TGPU_POOL_TAG);
    if (it == NULL) {
        return STATUS_NO_MEMORY;
    }
    it->Fence = Fence;
    it->Bytes = Bytes;
    if (Bytes != 0) {
        RtlCopyMemory(it->Cmds, Cmds, Bytes);
    }
    KeAcquireSpinLock(&g_QueueLock, &irql);
    InsertTailList(&g_Queue, &it->Link);
    KeReleaseSpinLock(&g_QueueLock, irql);
    KeSetEvent(&g_Kick, IO_NO_INCREMENT, FALSE);
    return STATUS_SUCCESS;
}

/* disp.c: the scanned-out primary changed (called from the vsync DPC) */
VOID EngKickScanout(VOID)
{
    if (g_Thread != NULL) {
        InterlockedExchange(&g_ScanKick, 1);
        KeSetEvent(&g_Kick, IO_NO_INCREMENT, FALSE);
    }
}
