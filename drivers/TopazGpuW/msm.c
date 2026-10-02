/*
 * The Linux msm DRM ioctls on top of DxgkDdiEscape (topazgpu_escape.h). One global BO table:
 * handle == flink name (global), so shared BOs are opened by name from any process. Escapes run at
 * PASSIVE_LEVEL in the context of the calling process, so user pointers in GEM_SUBMIT are read
 * directly (probed) and MMAP maps into the caller.
 */
#include "tgpu.h"

#define MSM_VERSION_MINOR   9                   /* FD_VERSION_VA_SIZE */
#define BO_MAX              65536

static KMUTEX g_BoLock;          /* keeps IRQL at PASSIVE (logging, user mappings) */
static LIST_ENTRY g_Bos;
static TGPU_BO *g_BoTable[BO_MAX];
static ULONG g_NextHandle = 1;

VOID HwInit(VOID);

VOID MsmInit(VOID)
{
    KeInitializeMutex(&g_BoLock, 0);
    InitializeListHead(&g_Bos);
    HwInit();
}

static TGPU_BO *BoGet(ULONG Handle)
{
    return (Handle > 0 && Handle < BO_MAX) ? g_BoTable[Handle] : NULL;
}

static VOID BoUnmapAll(TGPU_BO *Bo, PEPROCESS OnlyProcess)
{
    PLIST_ENTRY e, next;
    TGPU_MAP *m;

    for (e = Bo->Maps.Flink; e != &Bo->Maps; e = next) {
        next = e->Flink;
        m = CONTAINING_RECORD(e, TGPU_MAP, Link);
        if (OnlyProcess != NULL && m->Process != OnlyProcess) {
            continue;
        }
        if (m->Process == PsGetCurrentProcess()) {
            MmUnmapLockedPages(m->UserVa, Bo->Mdl);
        }                                       /* other processes: their mapping dies with them */
        RemoveEntryList(&m->Link);
        ExFreePoolWithTag(m, TGPU_POOL_TAG);
    }
}

static VOID BoFree(TGPU_BO *Bo)
{
    HwWaitFence(Bo->LastFence, 2000);
    BoUnmapAll(Bo, NULL);
    TgFreePages(Bo->Mdl, Bo->KernelVa, Bo->Size, Bo->Iova);
    g_BoTable[Bo->Handle] = NULL;
    RemoveEntryList(&Bo->Link);
    ExFreePoolWithTag(Bo, TGPU_POOL_TAG);
}

static NTSTATUS GemNew(struct drm_msm_gem_new *A)
{
    PHYSICAL_ADDRESS lo, hi, skip;
    TGPU_BO *bo;
    ULONG h;

    if (A->size == 0 || A->size > 0x40000000ull) {
        return STATUS_INVALID_PARAMETER;
    }
    bo = (TGPU_BO *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*bo), TGPU_POOL_TAG);
    if (bo == NULL) {
        return STATUS_NO_MEMORY;
    }
    InitializeListHead(&bo->Maps);
    bo->Size = ROUND_TO_PAGES((SIZE_T)A->size);
    bo->Flags = A->flags;
    bo->Refs = 1;
    lo.QuadPart = 0;
    hi.QuadPart = 0xEFFFFFFF;                          /* v0.14: walks/fetches above 4 GB faulted */
    skip.QuadPart = 0;
    UNREFERENCED_PARAMETER(lo);
    UNREFERENCED_PARAMETER(hi);
    UNREFERENCED_PARAMETER(skip);
    bo->Mdl = TgAllocPages(bo->Size, &bo->Iova, &bo->KernelVa);
    if (bo->Mdl == NULL) {
        ExFreePoolWithTag(bo, TGPU_POOL_TAG);
        return STATUS_NO_MEMORY;
    }
    for (h = g_NextHandle; h < BO_MAX && g_BoTable[h] != NULL; h++) {
    }
    if (h == BO_MAX) {
        for (h = 1; h < BO_MAX && g_BoTable[h] != NULL; h++) {
        }
    }
    if (h == BO_MAX) {
        bo->Handle = 0;
        TgFreePages(bo->Mdl, bo->KernelVa, bo->Size, bo->Iova);
        ExFreePoolWithTag(bo, TGPU_POOL_TAG);
        return STATUS_NO_MEMORY;
    }
    g_NextHandle = h + 1;
    bo->Handle = h;
    g_BoTable[h] = bo;
    InsertTailList(&g_Bos, &bo->Link);
    A->handle = h;
    return STATUS_SUCCESS;
}

static NTSTATUS GemInfo(struct drm_msm_gem_info *A)
{
    TGPU_BO *bo = BoGet(A->handle);

    if (bo == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    switch (A->info) {
    case MSM_INFO_GET_OFFSET:
        A->value = (ULONGLONG)bo->Handle << PAGE_SHIFT;  /* mmap token */
        return STATUS_SUCCESS;
    case MSM_INFO_GET_IOVA:
        A->value = bo->Iova;
        return STATUS_SUCCESS;
    case MSM_INFO_GET_FLAGS:
        A->value = bo->Flags;
        return STATUS_SUCCESS;
    case MSM_INFO_SET_NAME:
    case MSM_INFO_SET_METADATA:
        return STATUS_SUCCESS;                           /* debug names / metadata ignored */
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

static NTSTATUS Mmap(struct topazgpu_mmap *A)
{
    TGPU_BO *bo = BoGet((ULONG)(A->offset >> PAGE_SHIFT));
    TGPU_MAP *m;
    PVOID va = NULL;

    if (bo == NULL) {
        return STATUS_INVALID_HANDLE;
    }
    m = (TGPU_MAP *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*m), TGPU_POOL_TAG);
    if (m == NULL) {
        return STATUS_NO_MEMORY;
    }
    __try {
        va = MmMapLockedPagesSpecifyCache(bo->Mdl, UserMode, MmWriteCombined, NULL, FALSE,
                                          NormalPagePriority | MdlMappingNoExecute);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        va = NULL;
    }
    if (va == NULL) {
        ExFreePoolWithTag(m, TGPU_POOL_TAG);
        return STATUS_NO_MEMORY;
    }
    m->Process = PsGetCurrentProcess();
    m->UserVa = va;
    InsertTailList(&bo->Maps, &m->Link);
    A->addr = (ULONGLONG)(ULONG_PTR)va;
    return STATUS_SUCCESS;
}

/* v0.18 hang dump: dwords of the BO holding Iova (identity mapping: the BO's kernel VA covers it) */
VOID MsmDumpIova(PCSTR Tag, ULONGLONG Iova, ULONG Before, ULONG After)
{
    PLIST_ENTRY eb;
    TGPU_BO *bo;
    ULONGLONG lo, hi, a;
    PULONG p;

    KeWaitForSingleObject(&g_BoLock, Executive, KernelMode, FALSE, NULL);    /* recursive KMUTEX */
    for (eb = g_Bos.Flink; eb != &g_Bos; eb = eb->Flink) {
        bo = CONTAINING_RECORD(eb, TGPU_BO, Link);
        if (bo->KernelVa == NULL || Iova < bo->Iova || Iova >= bo->Iova + bo->Size) {
            continue;
        }
        lo = (Iova & ~3ull) - 4ull * Before;
        lo = lo < bo->Iova ? bo->Iova : lo;
        hi = (Iova & ~3ull) + 4ull * After;
        hi = hi > bo->Iova + bo->Size ? bo->Iova + bo->Size : hi;
        LogPrint("  %s %llx in bo %u (iova %llx size %llx):\n", Tag, Iova, bo->Handle, bo->Iova, (ULONGLONG)bo->Size);
        for (a = lo; a < hi; a += 32) {
            p = (PULONG)((PUCHAR)bo->KernelVa + (a - bo->Iova));
            LogPrint("    %llx%s %08x %08x %08x %08x %08x %08x %08x %08x\n", a, (Iova >= a && Iova < a + 32) ? "*" : ":",
                     p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        }
        KeReleaseMutex(&g_BoLock, FALSE);
        return;
    }
    KeReleaseMutex(&g_BoLock, FALSE);
    LogPrint("  %s %llx: no BO\n", Tag, Iova);
}

static NTSTATUS Munmap(struct topazgpu_mmap *A)
{
    PLIST_ENTRY eb, em;
    TGPU_BO *bo;
    TGPU_MAP *m;

    for (eb = g_Bos.Flink; eb != &g_Bos; eb = eb->Flink) {
        bo = CONTAINING_RECORD(eb, TGPU_BO, Link);
        for (em = bo->Maps.Flink; em != &bo->Maps; em = em->Flink) {
            m = CONTAINING_RECORD(em, TGPU_MAP, Link);
            if (m->Process == PsGetCurrentProcess() && (ULONGLONG)(ULONG_PTR)m->UserVa == A->addr) {
                MmUnmapLockedPages(m->UserVa, bo->Mdl);
                RemoveEntryList(&m->Link);
                ExFreePoolWithTag(m, TGPU_POOL_TAG);
                return STATUS_SUCCESS;
            }
        }
    }
    return STATUS_NOT_FOUND;
}

static NTSTATUS Submit(struct drm_msm_gem_submit *A)
{
    struct drm_msm_gem_submit_bo *bos = NULL;
    struct drm_msm_gem_submit_cmd *cmds = NULL;
    ULONGLONG ib[64];
    ULONG dw[64], i, fence = 0;
    NTSTATUS st = STATUS_SUCCESS;
    TGPU_BO *bo;

    if (A->nr_cmds == 0 || A->nr_cmds > 64 || A->nr_bos > 65536) {
        return STATUS_INVALID_PARAMETER;
    }
    bos = (struct drm_msm_gem_submit_bo *)ExAllocatePool2(POOL_FLAG_NON_PAGED,
        sizeof(*bos) * (A->nr_bos + 1), TGPU_POOL_TAG);
    cmds = (struct drm_msm_gem_submit_cmd *)ExAllocatePool2(POOL_FLAG_NON_PAGED,
        sizeof(*cmds) * A->nr_cmds, TGPU_POOL_TAG);
    if (bos == NULL || cmds == NULL) {
        st = STATUS_NO_MEMORY;
        goto out;
    }
    __try {
        ProbeForRead((PVOID)(ULONG_PTR)A->bos, sizeof(*bos) * A->nr_bos, 4);
        RtlCopyMemory(bos, (PVOID)(ULONG_PTR)A->bos, sizeof(*bos) * A->nr_bos);
        ProbeForRead((PVOID)(ULONG_PTR)A->cmds, sizeof(*cmds) * A->nr_cmds, 4);
        RtlCopyMemory(cmds, (PVOID)(ULONG_PTR)A->cmds, sizeof(*cmds) * A->nr_cmds);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        st = STATUS_ACCESS_VIOLATION;
        goto out;
    }
    for (i = 0; i < A->nr_bos; i++) {
        if (BoGet(bos[i].handle) == NULL) {
            st = STATUS_INVALID_HANDLE;
            goto out;
        }
    }
    for (i = 0; i < A->nr_cmds; i++) {
        if (cmds[i].submit_idx >= A->nr_bos) {
            st = STATUS_INVALID_PARAMETER;
            goto out;
        }
        bo = BoGet(bos[cmds[i].submit_idx].handle);
        if ((ULONGLONG)cmds[i].submit_offset + cmds[i].size > bo->Size) {
            st = STATUS_INVALID_PARAMETER;
            goto out;
        }
        ib[i] = bo->Iova + cmds[i].submit_offset;
        dw[i] = cmds[i].size / 4;
    }
    st = HwSubmit(ib, dw, A->nr_cmds, &fence);
    if (NT_SUCCESS(st)) {
        for (i = 0; i < A->nr_bos; i++) {
            BoGet(bos[i].handle)->LastFence = fence;
        }
        A->fence = fence;
        A->fence_fd = -1;
    }
out:
    if (bos != NULL) {
        ExFreePoolWithTag(bos, TGPU_POOL_TAG);
    }
    if (cmds != NULL) {
        ExFreePoolWithTag(cmds, TGPU_POOL_TAG);
    }
    return st;
}

static NTSTATUS GetParam(struct drm_msm_param *A)
{
    switch (A->param) {
    case MSM_PARAM_GPU_ID:      A->value = 610; break;
    case MSM_PARAM_GMEM_SIZE:   A->value = 0x21000; break;           /* 128K + 4K */
    case MSM_PARAM_CHIP_ID:     A->value = 0x06010001; break;
    case MSM_PARAM_MAX_FREQ:    A->value = 300000000; break;
    case MSM_PARAM_TIMESTAMP:   A->value = HwTimestamp(); break;
    case MSM_PARAM_GMEM_BASE:   A->value = 0x100000; break;
    case MSM_PARAM_PRIORITIES:  A->value = 1; break;
    case MSM_PARAM_PP_PGTABLE:  A->value = 0; break;
    case MSM_PARAM_FAULTS:      A->value = 0; break;
    case MSM_PARAM_SUSPENDS:    A->value = 0; break;
    case MSM_PARAM_VA_START:    A->value = TGPU_VA_USER_START; break;
    case MSM_PARAM_VA_SIZE:     A->value = TGPU_VA_KERNEL - TGPU_VA_USER_START; break;
    default:
        return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

/* errno-style results for the shim (it hands e->ret back to freedreno) */
static int Errno(NTSTATUS St)
{
    switch (St) {
    case STATUS_SUCCESS:            return 0;
    case STATUS_INVALID_HANDLE:     return -2;   /* ENOENT */
    case STATUS_NO_MEMORY:          return -12;  /* ENOMEM */
    case STATUS_ACCESS_VIOLATION:   return -14;  /* EFAULT */
    case STATUS_DEVICE_BUSY:        return -16;  /* EBUSY */
    case STATUS_TIMEOUT:            return -62;  /* ETIME */
    case STATUS_DEVICE_HARDWARE_ERROR: return -5; /* EIO: GPU wedged */
    case STATUS_NOT_SUPPORTED:      return -38;  /* ENOSYS */
    default:                        return -22;  /* EINVAL */
    }
}

NTSTATUS MsmEscape(struct topazgpu_escape *E)
{
    NTSTATUS st = STATUS_SUCCESS;
    PVOID d = E->data;
    TGPU_BO *bo;

    if (E->magic != TOPAZGPU_ESC_MAGIC || E->size > TOPAZGPU_ESC_MAX_DATA) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!HwStart()) {
        E->ret = -19;                                    /* ENODEV */
        return STATUS_SUCCESS;
    }
    KeWaitForSingleObject(&g_BoLock, Executive, KernelMode, FALSE, NULL);
    switch (E->nr) {
    case TOPAZGPU_NR_VERSION: {
        struct topazgpu_version *v = (struct topazgpu_version *)d;
        v->major = 1;
        v->minor = MSM_VERSION_MINOR;
        v->patch = 0;
        RtlStringCbCopyA(v->name, sizeof(v->name), "msm");
        break;
    }
    case DRM_COMMAND_BASE + DRM_MSM_GET_PARAM:
        st = GetParam((struct drm_msm_param *)d);
        break;
    case DRM_COMMAND_BASE + DRM_MSM_SET_PARAM:
        break;
    case DRM_COMMAND_BASE + DRM_MSM_GEM_NEW:
        st = GemNew((struct drm_msm_gem_new *)d);
        break;
    case DRM_COMMAND_BASE + DRM_MSM_GEM_INFO:
        st = GemInfo((struct drm_msm_gem_info *)d);
        break;
    case DRM_COMMAND_BASE + DRM_MSM_GEM_CPU_PREP: {
        struct drm_msm_gem_cpu_prep *p = (struct drm_msm_gem_cpu_prep *)d;
        bo = BoGet(p->handle);
        if (bo == NULL) {
            st = STATUS_INVALID_HANDLE;
        } else if (p->op & MSM_PREP_NOSYNC) {
            st = ((LONG)(HwCompletedFence() - bo->LastFence) >= 0) ? STATUS_SUCCESS : STATUS_DEVICE_BUSY;
        } else {
            KeReleaseMutex(&g_BoLock, FALSE);
            st = HwWaitFence(bo->LastFence, 5000) ? STATUS_SUCCESS : HwWedged() ? STATUS_DEVICE_HARDWARE_ERROR : STATUS_TIMEOUT;
            KeWaitForSingleObject(&g_BoLock, Executive, KernelMode, FALSE, NULL);
        }
        break;
    }
    case DRM_COMMAND_BASE + DRM_MSM_GEM_CPU_FINI:
        break;
    case DRM_COMMAND_BASE + DRM_MSM_GEM_MADVISE:
        ((struct drm_msm_gem_madvise *)d)->retained = 1;
        break;
    case DRM_COMMAND_BASE + DRM_MSM_GEM_SUBMIT:
        st = Submit((struct drm_msm_gem_submit *)d);
        break;
    case DRM_COMMAND_BASE + DRM_MSM_WAIT_FENCE: {
        ULONG f = ((struct drm_msm_wait_fence *)d)->fence;
        KeReleaseMutex(&g_BoLock, FALSE);
        st = HwWaitFence(f, 5000) ? STATUS_SUCCESS : HwWedged() ? STATUS_DEVICE_HARDWARE_ERROR : STATUS_TIMEOUT;
        KeWaitForSingleObject(&g_BoLock, Executive, KernelMode, FALSE, NULL);
        break;
    }
    case DRM_COMMAND_BASE + DRM_MSM_SUBMITQUEUE_NEW:
        ((struct drm_msm_submitqueue *)d)->id = 1;
        break;
    case DRM_COMMAND_BASE + DRM_MSM_SUBMITQUEUE_CLOSE:
        break;
    case DRM_COMMAND_BASE + DRM_MSM_SUBMITQUEUE_QUERY: {
        struct drm_msm_submitqueue_query *q = (struct drm_msm_submitqueue_query *)d;
        q->len = 0;                                      /* no faults (data pointer left untouched) */
        break;
    }
    case TOPAZGPU_NR_GEM_CLOSE:
        bo = BoGet(((struct drm_gem_close *)d)->handle);
        if (bo == NULL) {
            st = STATUS_INVALID_HANDLE;
        } else if (--bo->Refs == 0) {
            BoFree(bo);
        } else {
            BoUnmapAll(bo, PsGetCurrentProcess());
        }
        break;
    case TOPAZGPU_NR_GEM_FLINK: {
        struct drm_gem_flink *f = (struct drm_gem_flink *)d;
        st = BoGet(f->handle) != NULL ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
        f->name = f->handle;
        break;
    }
    case TOPAZGPU_NR_GEM_OPEN: {
        struct drm_gem_open *o = (struct drm_gem_open *)d;
        bo = BoGet(o->name);
        if (bo == NULL) {
            st = STATUS_INVALID_HANDLE;
        } else {
            bo->Refs++;
            o->handle = bo->Handle;
            o->size = bo->Size;
        }
        break;
    }
    case TOPAZGPU_NR_MMAP:
        st = Mmap((struct topazgpu_mmap *)d);
        break;
    case TOPAZGPU_NR_MUNMAP:
        st = Munmap((struct topazgpu_mmap *)d);
        break;
    default:
        LogPrint("escape: unknown nr %x\n", E->nr);
        st = STATUS_NOT_SUPPORTED;
        break;
    }
    KeReleaseMutex(&g_BoLock, FALSE);
    E->ret = Errno(st);
    return STATUS_SUCCESS;
}

VOID MsmCleanup(VOID)
{
    while (!IsListEmpty(&g_Bos)) {
        BoFree(CONTAINING_RECORD(g_Bos.Flink, TGPU_BO, Link));
    }
}
