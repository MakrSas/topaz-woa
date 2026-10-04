/*
 * Kernel client of TopazAudio's \\.\TopazAudio interface (the same IOCTLs tools/audio/taudio.c uses):
 * MMIO in TopazAudio's allowlist, I2C on QUP0 SE1 (TopazBattery quiet windows), raw GPR packets.
 * All calls at PASSIVE_LEVEL; one handle, serialized by a mutex.
 */
#include "tspk.h"
#include "ta.h"
#include "../TopazAudio/taudio_ioctl.h"

#define GPR_DOMAIN_ADSP 2
#define GPR_DOMAIN_APPS 3

static HANDLE  g_Ta;
static KMUTEX  g_TaLock;
static BOOLEAN g_TaInit;
static ULONG   g_Token = 0x5000;
static UCHAR   g_Pkt[TAUDIO_GPR_MAX];

static NTSTATUS TaIoctl(ULONG Code, PVOID In, ULONG InLen, PVOID Out, ULONG OutLen, ULONG *Got)
{
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\??\\TopazAudio");
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    NTSTATUS status;

    if (g_Ta == NULL) {
        InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        status = ZwCreateFile(&g_Ta, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, &oa, &iosb, NULL, 0,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
        if (!NT_SUCCESS(status)) {
            g_Ta = NULL;
            return status;
        }
    }
    status = ZwDeviceIoControlFile(g_Ta, NULL, NULL, NULL, &iosb, Code, In, InLen, Out, OutLen);
    if (Got != NULL) {
        *Got = NT_SUCCESS(status) ? (ULONG)iosb.Information : 0;
    }
    return status;
}

static VOID Lock(VOID)
{
    if (!g_TaInit) {
        KeInitializeMutex(&g_TaLock, 0);
        g_TaInit = TRUE;
    }
    KeWaitForSingleObject(&g_TaLock, Executive, KernelMode, FALSE, NULL);
}

static VOID Unlock(VOID)
{
    KeReleaseMutex(&g_TaLock, FALSE);
}

VOID TaClose(VOID)
{
    if (g_Ta != NULL) {
        ZwClose(g_Ta);
        g_Ta = NULL;
    }
}

BOOLEAN TaAdspReady(VOID)
{
    TAUDIO_STATE st;
    NTSTATUS status;

    RtlZeroMemory(&st, sizeof(st));
    Lock();
    status = TaIoctl(IOCTL_TAUDIO_STATE, NULL, 0, &st, sizeof(st), NULL);
    Unlock();
    return NT_SUCCESS(status) && st.GprUp && st.SpfState == 1;
}

NTSTATUS TaWr(ULONGLONG Pa, ULONG Value)
{
    TAUDIO_MMIO m;
    NTSTATUS status;

    RtlZeroMemory(&m, sizeof(m));
    m.Op = 1;
    m.Pa = Pa;
    m.Value[0] = Value;
    Lock();
    status = TaIoctl(IOCTL_TAUDIO_MMIO, &m, sizeof(m), &m, sizeof(m), NULL);
    Unlock();
    if (!NT_SUCCESS(status)) {
        LogPrint("mmio wr %llx: %08x\r\n", Pa, status);
    }
    return status;
}

ULONG TaRd(ULONGLONG Pa)
{
    TAUDIO_MMIO m;

    RtlZeroMemory(&m, sizeof(m));
    m.Op = 0;
    m.Count = 1;
    m.Pa = Pa;
    Lock();
    if (!NT_SUCCESS(TaIoctl(IOCTL_TAUDIO_MMIO, &m, sizeof(m), &m, sizeof(m), NULL))) {
        m.Value[0] = 0xFFFFFFFF;
    }
    Unlock();
    return m.Value[0];
}

NTSTATUS TaI2cWrite(UCHAR Addr, UCHAR Reg, UCHAR Value)
{
    TAUDIO_I2C t;
    NTSTATUS status;

    RtlZeroMemory(&t, sizeof(t));
    t.Addr = Addr;
    t.WLen = 2;
    t.W[0] = Reg;
    t.W[1] = Value;
    Lock();
    status = TaIoctl(IOCTL_TAUDIO_I2C, &t, sizeof(t), &t, sizeof(t), NULL);
    Unlock();
    if (NT_SUCCESS(status) && t.Result != 0) {
        status = STATUS_IO_DEVICE_ERROR;
    }
    return status;
}

VOID TaSleep(ULONG Ms)
{
    LARGE_INTEGER t;

    t.QuadPart = -10000LL * Ms;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

/*
 * Send one GPR packet (optionally with an in-band apm_cmd_header, like `taudio apm`) and wait for the
 * reply with the same token. Returns the reply opcode in *RspOpcode and up to MaxWords payload dwords.
 * Unrelated packets (events, late replies) are dropped.
 */
NTSTATUS TaGpr(ULONG Dst, ULONG Src, ULONG Opcode, BOOLEAN ApmHdr, const ULONG *Dw, ULONG N,
               ULONG *RspOpcode, ULONG *Rsp, ULONG MaxWords, ULONG TimeoutMs)
{
    ULONG *pkt = (ULONG *)g_Pkt, k = 6, i, bytes, token, got, waited;
    NTSTATUS status;

    Lock();
    RtlZeroMemory(g_Pkt, sizeof(g_Pkt));
    if (ApmHdr) {
        pkt[k + 3] = N * 4;                        /* apm_cmd_header.payload_size */
        k += 4;
    }
    for (i = 0; i < N && k < TAUDIO_GPR_MAX / 4; i++) {
        pkt[k++] = Dw[i];
    }
    bytes = k * 4;
    token = g_Token++;
    pkt[0] = (6 << 4) | (bytes << 8);
    pkt[1] = GPR_DOMAIN_ADSP | (GPR_DOMAIN_APPS << 8);
    pkt[2] = Src;
    pkt[3] = Dst;
    pkt[4] = token;
    pkt[5] = Opcode;
    status = TaIoctl(IOCTL_TAUDIO_GPR_SEND, g_Pkt, bytes, NULL, 0, NULL);
    for (waited = 0; NT_SUCCESS(status) && waited < TimeoutMs; ) {
        status = TaIoctl(IOCTL_TAUDIO_GPR_RECV, NULL, 0, g_Pkt, sizeof(g_Pkt), &got);
        if (!NT_SUCCESS(status)) {
            break;
        }
        if (got < 24) {
            TaSleep(2);
            waited += 2;
            continue;
        }
        if (pkt[4] != token) {
            continue;
        }
        if (RspOpcode != NULL) {
            *RspOpcode = pkt[5];
        }
        {
            ULONG hlen = ((pkt[0] >> 4) & 0xF) * 4, n = (got - hlen) / 4;
            for (i = 0; Rsp != NULL && i < n && i < MaxWords; i++) {
                Rsp[i] = *(ULONG *)(g_Pkt + hlen + 4 * i);
            }
        }
        Unlock();
        return STATUS_SUCCESS;
    }
    Unlock();
    LogPrint("gpr %08x token %x: no reply (%08x)\r\n", Opcode, token, status);
    return NT_SUCCESS(status) ? STATUS_IO_TIMEOUT : status;
}

/* GPR_BASIC_RSP_RESULT {opcode, status}: 0 if the command was accepted */
NTSTATUS TaApm(ULONG Opcode, BOOLEAN ApmHdr, const ULONG *Dw, ULONG N, ULONG *Rsp, ULONG MaxWords)
{
    ULONG op = 0, r[4] = { 0 };
    NTSTATUS status = TaGpr(1, 1, Opcode, ApmHdr, Dw, N, &op, Rsp != NULL ? Rsp : r, Rsp != NULL ? MaxWords : 4, 1000);

    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (op == 0x02001005) {                        /* basic result */
        ULONG *w = Rsp != NULL ? Rsp : r;
        if (w[1] != 0) {
            LogPrint("apm %08x: status %u\r\n", Opcode, w[1]);
            return STATUS_UNSUCCESSFUL;
        }
    }
    return STATUS_SUCCESS;
}
