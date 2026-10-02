/*
 * topaz_bl.cxx - Windows brightness slider for the topaz panel (TopazDisplay v0.3).
 *
 * Dxgkrnl / monitor.sys ask the display adapter for GUID_DEVINTERFACE_BRIGHTNESS (DxgkDdiQueryInterface)
 * when the target is an internal panel; SetBrightness(0..100) then sends DCS 0x51 to the panel
 * through DSI0 exactly like drivers/TopazBacklight v0.3: a DSI DMA command from a contiguous
 * buffer below 4 GB (the TPG FIFO path reports done but never reaches the panel), packet layout of
 * Linux dsi_cmd_dma_add, level 1..0x7FF MSB first (stock DT bl-inverted-dbv). Notes:
 * docs/NOTES_brightness.md.
 */
#include <initguid.h>
#include "BDD.hxx"

#define DSI0_PA             0x05E94000ULL
#define DSI0_SIZE           0x400
#define DSI_CTRL            0x004
#define DSI_STATUS          0x008
#define DSI_CMD_DMA_CTRL    0x03C
#define DSI_DMA_CMD_OFFSET  0x048
#define DSI_DMA_CMD_LENGTH  0x04C
#define DSI_TRIG_CTRL       0x084
#define DSI_DMA_SW_TRIGGER  0x090
#define DSI_INT_CTRL        0x110
#define DSI_TPG_CTRL        0x15C
#define CTRL_ENABLE         (1u << 0)
#define CTRL_CMD_MODE_EN    (1u << 2)
#define DMA_CTRL_LOW_POWER  (1u << 26)
#define INT_CMD_DMA_DONE    (1u << 0)
#define TRIG_DMA_SW         4u
#define BL_MAX              0x7FF

static volatile UCHAR *g_Dsi;
static PUCHAR g_DmaVa;
static PHYSICAL_ADDRESS g_DmaPa;
static UCHAR g_Brightness = 100;
static KMUTEX g_BlLock;

static ULONG Rd(ULONG Off) { return READ_REGISTER_ULONG((volatile ULONG *)(g_Dsi + Off)); }
static VOID Wr(ULONG Off, ULONG V) { WRITE_REGISTER_ULONG((volatile ULONG *)(g_Dsi + Off), V); }

static BOOLEAN DsiLongWrite(const UCHAR *Payload, ULONG Len)
{
    ULONG size = (4 + Len + 3) & ~3u, ctrl, dmaCtrl, trig, waited;
    BOOLEAN done = FALSE;

    if (g_Dsi == NULL || g_DmaVa == NULL || Len > 60) {
        return FALSE;
    }
    RtlFillMemory(g_DmaVa, 64, 0xFF);
    g_DmaVa[0] = (UCHAR)Len;
    g_DmaVa[1] = (UCHAR)(Len >> 8);
    g_DmaVa[2] = 0x39;                                  /* DCS long write, VC 0 */
    g_DmaVa[3] = 0x80 | 0x40;                           /* last packet, long */
    RtlCopyMemory(g_DmaVa + 4, Payload, Len);
    KeMemoryBarrier();

    ctrl = Rd(DSI_CTRL);
    trig = Rd(DSI_TRIG_CTRL);
    if ((trig & 7) != TRIG_DMA_SW) {
        Wr(DSI_TRIG_CTRL, (trig & ~7u) | TRIG_DMA_SW);
    }
    Wr(DSI_CTRL, ctrl | CTRL_CMD_MODE_EN | CTRL_ENABLE);
    Wr(DSI_INT_CTRL, Rd(DSI_INT_CTRL) | INT_CMD_DMA_DONE);
    Wr(DSI_TPG_CTRL, 0);
    Wr(DSI_DMA_CMD_OFFSET, g_DmaPa.LowPart);
    Wr(DSI_DMA_CMD_LENGTH, size);
    dmaCtrl = Rd(DSI_CMD_DMA_CTRL);
    Wr(DSI_CMD_DMA_CTRL, dmaCtrl & ~DMA_CTRL_LOW_POWER);
    Wr(DSI_DMA_SW_TRIGGER, 1);
    for (waited = 0; waited < 50; waited++) {
        if (Rd(DSI_INT_CTRL) & INT_CMD_DMA_DONE) {
            done = TRUE;
            break;
        }
        KeStallExecutionProcessor(1000);
    }
    Wr(DSI_INT_CTRL, Rd(DSI_INT_CTRL) | INT_CMD_DMA_DONE);
    Wr(DSI_CMD_DMA_CTRL, dmaCtrl);
    Wr(DSI_CTRL, ctrl);
    if (trig != Rd(DSI_TRIG_CTRL)) {
        Wr(DSI_TRIG_CTRL, trig);
    }
    return done;
}

static NTSTATUS SendBrightness(UCHAR Pct)
{
    UCHAR cmd[3];
    ULONG level = (ULONG)Pct * BL_MAX / 100;
    BOOLEAN done;

    if (level < 1) {
        level = 1;                                      /* 0 = black panel */
    }
    cmd[0] = 0x51;
    cmd[1] = (UCHAR)(level >> 8);
    cmd[2] = (UCHAR)level;
    KeWaitForSingleObject(&g_BlLock, Executive, KernelMode, FALSE, NULL);
    done = DsiLongWrite(cmd, sizeof(cmd));
    KeReleaseMutex(&g_BlLock, FALSE);
    LogPrint("brightness %u%% -> level 0x%03x: %s\n", Pct, level, done ? "done" : "NO DONE");
    return done ? STATUS_SUCCESS : STATUS_IO_TIMEOUT;
}

/* ---- DXGK_BRIGHTNESS_INTERFACE --------------------------------------------- */

static VOID APIENTRY BlReference(PVOID Context) { UNREFERENCED_PARAMETER(Context); }
static VOID APIENTRY BlDereference(PVOID Context) { UNREFERENCED_PARAMETER(Context); }

static NTSTATUS APIENTRY BlGetPossible(HANDLE Context, ULONG BufferSize, UCHAR *LevelCount, UCHAR *Levels)
{
    ULONG i, n = BufferSize < 101 ? BufferSize : 101;

    UNREFERENCED_PARAMETER(Context);
    if (LevelCount == NULL || Levels == NULL || n == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    for (i = 0; i < n; i++) {
        Levels[i] = (UCHAR)i;                           /* 0..100 % */
    }
    *LevelCount = (UCHAR)n;
    return STATUS_SUCCESS;
}

static NTSTATUS APIENTRY BlSet(HANDLE Context, UCHAR Brightness)
{
    UNREFERENCED_PARAMETER(Context);
    if (Brightness > 100) {
        Brightness = 100;
    }
    g_Brightness = Brightness;
    return SendBrightness(Brightness);
}

static NTSTATUS APIENTRY BlGet(HANDLE Context, UCHAR *Brightness)
{
    UNREFERENCED_PARAMETER(Context);
    if (Brightness == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *Brightness = g_Brightness;
    return STATUS_SUCCESS;
}

/* ---- called from the BDD code ---------------------------------------------- */

VOID TopazBlInit(VOID)
{
    PHYSICAL_ADDRESS pa, lo, hi, skip;

    if (g_Dsi != NULL) {
        return;
    }
    KeInitializeMutex(&g_BlLock, 0);
    pa.QuadPart = (LONGLONG)DSI0_PA;
    g_Dsi = (volatile UCHAR *)MmMapIoSpaceEx(pa, DSI0_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    lo.QuadPart = 0;
    hi.QuadPart = 0xFFFFFFFF;
    skip.QuadPart = 0;
    g_DmaVa = (PUCHAR)MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, lo, hi, skip, MmNonCached);
    if (g_DmaVa != NULL) {
        g_DmaPa = MmGetPhysicalAddress(g_DmaVa);
    }
    LogPrint("brightness: DSI0 %p (ctrl %08x), DMA buffer pa %llx\n", g_Dsi, g_Dsi ? Rd(DSI_CTRL) : 0,
             g_DmaPa.QuadPart);
}

NTSTATUS TopazBlQueryInterface(PQUERY_INTERFACE Qi)
{
    if (Qi == NULL || Qi->InterfaceType == NULL) {
        return STATUS_NOT_SUPPORTED;
    }
    if (RtlCompareMemory(Qi->InterfaceType, &GUID_DEVINTERFACE_BRIGHTNESS, sizeof(GUID)) != sizeof(GUID)) {
        LogPrint("QueryInterface: %08x-... v%u size %u: not supported\n", Qi->InterfaceType->Data1, Qi->Version,
                 Qi->Size);
        return STATUS_NOT_SUPPORTED;
    }
    if (g_Dsi == NULL || g_DmaVa == NULL || Qi->Size < sizeof(DXGK_BRIGHTNESS_INTERFACE) || Qi->Interface == NULL) {
        LogPrint("QueryInterface brightness: v%u size %u -> not available\n", Qi->Version, Qi->Size);
        return STATUS_NOT_SUPPORTED;
    }
    DXGK_BRIGHTNESS_INTERFACE *b = (DXGK_BRIGHTNESS_INTERFACE *)Qi->Interface;
    RtlZeroMemory(b, sizeof(*b));
    b->Size = sizeof(*b);
    b->Version = DXGK_BRIGHTNESS_INTERFACE_VERSION_1;
    b->Context = NULL;
    b->InterfaceReference = BlReference;
    b->InterfaceDereference = BlDereference;
    b->GetPossibleBrightness = BlGetPossible;
    b->SetBrightness = BlSet;
    b->GetBrightness = BlGet;
    LogPrint("QueryInterface brightness v%u: provided\n", Qi->Version);
    return STATUS_SUCCESS;
}

/* ---- step C2 of the GPU work (docs/P8_gpu.md) ------------------------------------
 * When the UEFI DSDT has the GPU0 device (TPZG0610, RAM-booted test image), TopazGpuW is the POST
 * display adapter and owns the panel: TopazDisplay then reports its monitor as disconnected so the
 * two drivers never scan out into the same framebuffer. The flashed UEFI has no GPU0, so normal
 * boots are unchanged. Dxgkrnl/ACPI publish the tables in HKLM\HARDWARE\ACPI (rebuilt every boot).
 * C:\topaz\td.keep forces the old behaviour. */

static BOOLEAN FindInValue(HANDLE Key)
{
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"00000000");
    static const CHAR pat[] = "TPZG0610";
    ULONG len = 0, i;
    PKEY_VALUE_PARTIAL_INFORMATION v;
    BOOLEAN found = FALSE;

    ZwQueryValueKey(Key, &name, KeyValuePartialInformation, NULL, 0, &len);
    if (len == 0 || len > 4 * 1024 * 1024) {
        return FALSE;
    }
    v = (PKEY_VALUE_PARTIAL_INFORMATION)ExAllocatePool2(POOL_FLAG_PAGED, len, 'pzTD');
    if (v == NULL) {
        return FALSE;
    }
    if (NT_SUCCESS(ZwQueryValueKey(Key, &name, KeyValuePartialInformation, v, len, &len))) {
        for (i = 0; i + sizeof(pat) - 1 <= v->DataLength; i++) {
            if (RtlCompareMemory(v->Data + i, pat, sizeof(pat) - 1) == sizeof(pat) - 1) {
                found = TRUE;
                break;
            }
        }
    }
    ExFreePoolWithTag(v, 'pzTD');
    return found;
}

static BOOLEAN SearchKey(HANDLE Parent, PCWSTR Path, ULONG Depth)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    HANDLE key;
    ULONG idx, len;
    UCHAR buf[sizeof(KEY_BASIC_INFORMATION) + 128 * sizeof(WCHAR)];
    WCHAR sub[130];
    BOOLEAN found = FALSE;

    RtlInitUnicodeString(&name, Path);
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, Parent, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&key, KEY_READ, &oa))) {
        return FALSE;
    }
    if (Depth == 0) {
        found = FindInValue(key);
    } else {
        for (idx = 0; !found; idx++) {
            PKEY_BASIC_INFORMATION bi = (PKEY_BASIC_INFORMATION)buf;
            if (!NT_SUCCESS(ZwEnumerateKey(key, idx, KeyBasicInformation, bi, sizeof(buf) - sizeof(WCHAR), &len))) {
                break;
            }
            RtlZeroMemory(sub, sizeof(sub));
            RtlCopyMemory(sub, bi->Name, min(bi->NameLength, sizeof(sub) - sizeof(WCHAR)));
            found = SearchKey(key, sub, Depth - 1);
        }
    }
    ZwClose(key);
    return found;
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
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
                                 FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0))) {
        return FALSE;
    }
    ZwClose(h);
    return TRUE;
}

BOOLEAN g_TopazGpuOwnsPanel;

VOID TopazCheckGpuOwner(VOID)
{
    /* DSDT\<OEM>\<table>\<revision>, value 00000000 = the table */
    g_TopazGpuOwnsPanel = SearchKey(NULL, L"\\Registry\\Machine\\HARDWARE\\ACPI\\DSDT", 3) &&
                          !FileExists(L"\\??\\C:\\topaz\\td.keep");
    LogPrint("GPU0 (TPZG0610) in the DSDT: %s\n", g_TopazGpuOwnsPanel ?
             "yes -> TopazGpuW owns the panel, monitor reported disconnected" : "no (or td.keep)");
}
