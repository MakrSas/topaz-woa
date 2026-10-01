/*
 * Battery class (battc.sys) miniport callbacks. All values come from the snapshot
 * the poll thread keeps current; callbacks never touch the I2C bus.
 * Capacities are reported in mWh (absolute), converted from mAh at the nominal voltage.
 */
#include "driver.h"

static BCLASS_QUERY_TAG_CALLBACK          BattQueryTag;
static BCLASS_QUERY_INFORMATION_CALLBACK  BattQueryInformation;
static BCLASS_SET_INFORMATION_CALLBACK    BattSetInformation;
static BCLASS_QUERY_STATUS_CALLBACK       BattQueryStatus;
static BCLASS_SET_STATUS_NOTIFY_CALLBACK  BattSetStatusNotify;
static BCLASS_DISABLE_STATUS_NOTIFY_CALLBACK BattDisableStatusNotify;

static VOID GetSnap(PDEVICE_CONTEXT Ctx, PBATT_SNAPSHOT Out)
{
    KIRQL irql;

    KeAcquireSpinLock(&Ctx->SnapLock, &irql);
    *Out = Ctx->Snap;
    KeReleaseSpinLock(&Ctx->SnapLock, irql);
}

static ULONG MahToMwh(ULONG Mah)
{
    return Mah * BATT_NOMINAL_MV / 1000;
}

NTSTATUS BattClassInit(PDEVICE_CONTEXT Ctx)
{
    BATTERY_MINIPORT_INFO_V1_1 info;
    NTSTATUS status;

    RtlZeroMemory(&info, sizeof(info));
    info.MajorVersion = BATTERY_CLASS_MAJOR_VERSION;
    info.MinorVersion = BATTERY_CLASS_MINOR_VERSION_1;
    info.Context = Ctx;
    info.QueryTag = BattQueryTag;
    info.QueryInformation = BattQueryInformation;
    info.SetInformation = BattSetInformation;
    info.QueryStatus = BattQueryStatus;
    info.SetStatusNotify = BattSetStatusNotify;
    info.DisableStatusNotify = BattDisableStatusNotify;
    info.Pdo = WdfDeviceWdmGetPhysicalDevice(Ctx->Device);
    info.DeviceName = NULL;
    info.Fdo = WdfDeviceWdmGetDeviceObject(Ctx->Device);

    WdfWaitLockAcquire(Ctx->ClassInitLock, NULL);
    status = BatteryClassInitializeDevice((PBATTERY_MINIPORT_INFO)&info, &Ctx->ClassHandle);
    if (!NT_SUCCESS(status)) {
        Ctx->ClassHandle = NULL;
    }
    WdfWaitLockRelease(Ctx->ClassInitLock);
    return status;
}

VOID BattClassUnload(PDEVICE_CONTEXT Ctx)
{
    WdfWaitLockAcquire(Ctx->ClassInitLock, NULL);
    if (Ctx->ClassHandle != NULL) {
        BatteryClassUnload(Ctx->ClassHandle);
        Ctx->ClassHandle = NULL;
    }
    WdfWaitLockRelease(Ctx->ClassInitLock);
}

static NTSTATUS BattQueryTag(PVOID Context, PULONG BatteryTag)
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;

    *BatteryTag = ctx->HwReady ? TOPAZ_BATTERY_TAG : BATTERY_TAG_INVALID;
    return ctx->HwReady ? STATUS_SUCCESS : STATUS_NO_SUCH_DEVICE;
}

static NTSTATUS CopyOut(PVOID Buffer, ULONG BufferLength, PULONG ReturnedLength, const VOID *Data, ULONG Size)
{
    *ReturnedLength = Size;
    if (Buffer == NULL || BufferLength < Size) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlCopyMemory(Buffer, Data, Size);
    return STATUS_SUCCESS;
}

static NTSTATUS BattQueryInformation(PVOID Context, ULONG BatteryTag, BATTERY_QUERY_INFORMATION_LEVEL Level,
                                     LONG AtRate, PVOID Buffer, ULONG BufferLength, PULONG ReturnedLength)
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;
    BATT_SNAPSHOT s;

    *ReturnedLength = 0;
    if (BatteryTag != TOPAZ_BATTERY_TAG) {
        return STATUS_NO_SUCH_DEVICE;
    }
    GetSnap(ctx, &s);

    switch (Level) {
    case BatteryInformation: {
        BATTERY_INFORMATION bi;
        RtlZeroMemory(&bi, sizeof(bi));
        bi.Capabilities = BATTERY_SYSTEM_BATTERY;
        bi.Technology = 1;                                   /* rechargeable */
        RtlCopyMemory(bi.Chemistry, "LiP ", 4);
        bi.DesignedCapacity = MahToMwh(BATT_DESIGN_MAH);
        bi.FullChargedCapacity = MahToMwh(s.FullMah);
        bi.DefaultAlert1 = bi.FullChargedCapacity * 7 / 100;  /* low */
        bi.DefaultAlert2 = bi.FullChargedCapacity * 4 / 100;  /* critical */
        bi.CriticalBias = 0;
        bi.CycleCount = s.Cycles;
        return CopyOut(Buffer, BufferLength, ReturnedLength, &bi, sizeof(bi));
    }
    case BatteryTemperature: {
        ULONG tenthsK = (ULONG)(s.TempTenthsC + 2731);
        return CopyOut(Buffer, BufferLength, ReturnedLength, &tenthsK, sizeof(tenthsK));
    }
    case BatteryEstimatedTime: {
        ULONG secs = BATTERY_UNKNOWN_TIME;
        LONG rateMw = AtRate != 0 ? AtRate : (LONG)((LONGLONG)s.CurrentMa * (LONG)s.VoltageMv / 1000);
        if (!s.OnLine && rateMw < -50) {
            ULONG capMwh = MahToMwh(s.FullMah) * s.SocTenths / 1000;
            secs = (ULONG)((ULONGLONG)capMwh * 3600 / (ULONG)(-rateMw));
        }
        return CopyOut(Buffer, BufferLength, ReturnedLength, &secs, sizeof(secs));
    }
    case BatteryDeviceName: {
        static const WCHAR name[] = L"Redmi Note 12 Battery";
        return CopyOut(Buffer, BufferLength, ReturnedLength, name, sizeof(name));
    }
    case BatteryManufactureName: {
        static const WCHAR name[] = L"Xiaomi (sm5602)";
        return CopyOut(Buffer, BufferLength, ReturnedLength, name, sizeof(name));
    }
    case BatteryUniqueID: {
        static const WCHAR id[] = L"topaz-batt0";
        return CopyOut(Buffer, BufferLength, ReturnedLength, id, sizeof(id));
    }
    default:
        return STATUS_INVALID_DEVICE_REQUEST;
    }
}

static NTSTATUS BattQueryStatus(PVOID Context, ULONG BatteryTag, PBATTERY_STATUS BatteryStatus)
{
    PDEVICE_CONTEXT ctx = (PDEVICE_CONTEXT)Context;
    BATT_SNAPSHOT s;
    LONG rateMw;

    if (BatteryTag != TOPAZ_BATTERY_TAG) {
        return STATUS_NO_SUCH_DEVICE;
    }
    GetSnap(ctx, &s);
    if (!s.Valid) {
        BatteryStatus->PowerState = BATTERY_POWER_ON_LINE;
        BatteryStatus->Capacity = BATTERY_UNKNOWN_CAPACITY;
        BatteryStatus->Voltage = BATTERY_UNKNOWN_VOLTAGE;
        BatteryStatus->Rate = BATTERY_UNKNOWN_RATE;
        return STATUS_SUCCESS;
    }

    rateMw = (LONG)((LONGLONG)s.CurrentMa * (LONG)s.VoltageMv / 1000);
    BatteryStatus->PowerState = 0;
    if (s.OnLine) {
        BatteryStatus->PowerState |= BATTERY_POWER_ON_LINE;
    }
    if (s.Charging) {
        BatteryStatus->PowerState |= BATTERY_CHARGING;
    } else if (!s.OnLine) {
        BatteryStatus->PowerState |= BATTERY_DISCHARGING;
    }
    if (!s.OnLine && s.SocTenths <= 30) {
        BatteryStatus->PowerState |= BATTERY_CRITICAL;
    }
    BatteryStatus->Capacity = MahToMwh(s.FullMah) * s.SocTenths / 1000;
    BatteryStatus->Voltage = s.VoltageMv;
    BatteryStatus->Rate = s.ChargeDone ? 0 : rateMw;
    return STATUS_SUCCESS;
}

static NTSTATUS BattSetInformation(PVOID Context, ULONG BatteryTag, BATTERY_SET_INFORMATION_LEVEL Level, PVOID Buffer)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(BatteryTag);
    UNREFERENCED_PARAMETER(Buffer);
    LogPrint("SetInformation level %u (not supported)\n", (ULONG)Level);
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS BattSetStatusNotify(PVOID Context, ULONG BatteryTag, PBATTERY_NOTIFY BatteryNotify)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(BatteryNotify);
    /* The poll thread notifies on every power-state or whole-percent change. */
    return BatteryTag == TOPAZ_BATTERY_TAG ? STATUS_SUCCESS : STATUS_NO_SUCH_DEVICE;
}

static NTSTATUS BattDisableStatusNotify(PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    return STATUS_SUCCESS;
}
