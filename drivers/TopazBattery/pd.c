/*
 * Minimal USB Power Delivery 2.0 sink on the rt1711h (TCPCI), polled from the battery thread.
 *
 * Only what a phone charging at a fixed voltage needs: receive Source_Capabilities, send a
 * Request for the 9 V fixed PDO (5 V if there is none or the battery is warm), wait for Accept
 * and PS_RDY. Get_Sink_Cap is answered, swaps/Get_Source_Cap are rejected, Soft_Reset is
 * accepted, VDMs are ignored. GoodCRC and retries are done by the TCPC.
 * Register use follows Linux tcpci.c / tcpci_rt1711h.c; message layout follows the PD 2.0 spec.
 *
 * Timing: a Request must follow Source_Capabilities within tSenderResponse (24 ms), so the
 * negotiation runs in a busy loop (PdRun) and log lines are buffered until it ends (the log
 * file is write-through and too slow to write in between).
 */
#include "driver.h"

/* PD message types (PD 2.0) */
#define PD_CTRL_GOODCRC        1
#define PD_CTRL_GOTOMIN        2
#define PD_CTRL_ACCEPT         3
#define PD_CTRL_REJECT         4
#define PD_CTRL_PING           5
#define PD_CTRL_PS_RDY         6
#define PD_CTRL_GET_SRC_CAP    7
#define PD_CTRL_GET_SNK_CAP    8
#define PD_CTRL_DR_SWAP        9
#define PD_CTRL_PR_SWAP        10
#define PD_CTRL_VCONN_SWAP     11
#define PD_CTRL_WAIT           12
#define PD_CTRL_SOFT_RESET     13
#define PD_DATA_SRC_CAP        1
#define PD_DATA_REQUEST        2
#define PD_DATA_SNK_CAP        4
#define PD_DATA_VDM            15

#define PD_HDR_TYPE(h)         ((h) & 0x1F)
#define PD_HDR_REV(h)          (((h) >> 6) & 3)
#define PD_HDR_ID(h)           (((h) >> 9) & 7)
#define PD_HDR_CNT(h)          (((h) >> 12) & 7)
#define PD_HDR_EXT(h)          (((h) >> 15) & 1)
/* our header: UFP, spec rev 2.0, power role sink */
#define PD_HDR(type, id, cnt)  ((USHORT)((type) | (1u << 6) | (((id) & 7u) << 9) | (((cnt) & 7u) << 12)))

#define PDO_TYPE(p)            ((p) >> 30)          /* 0 fixed, 1 battery, 2 variable, 3 APDO */
#define PDO_FIXED_MV(p)        ((((p) >> 10) & 0x3FF) * 50)
#define PDO_FIXED_MA(p)        (((p) & 0x3FF) * 10)
#define APDO_PPS(p)            ((((p) >> 28) & 3) == 0)
#define APDO_MAX_MV(p)         ((((p) >> 17) & 0xFF) * 100)
#define APDO_MIN_MV(p)         ((((p) >> 8) & 0xFF) * 100)
#define APDO_MA(p)             (((p) & 0x7F) * 50)

/* TCPCI */
#define TCPC_REG_MSG_HDR_INFO  0x2E
#define TCPC_MSG_HDR_SNK_UFP_20 0x02      /* data role UFP, power role sink, GoodCRC rev 2.0 */
#define TCPC_REG_RX_DETECT     0x2F
#define TCPC_RX_DETECT_SOP_HR  0x21      /* SOP + hard reset */
#define TCPC_REG_RX_BYTE_CNT   0x30
#define TCPC_REG_RX_FRAME_TYPE 0x31
#define TCPC_REG_RX_HDR        0x32
#define TCPC_REG_RX_DATA       0x34
#define TCPC_REG_TRANSMIT      0x50
#define TCPC_TRANSMIT_SOP_R3   0x30      /* SOP, nRetryCount 3 (PD 2.0) */
#define TCPC_REG_TX_BYTE_CNT   0x51
#define TCPC_REG_TX_HDR        0x52
#define TCPC_REG_TX_DATA       0x54
#define TCPC_ALERT_RX_STATUS   0x0004
#define TCPC_ALERT_RX_HARD_RST 0x0008
#define TCPC_ALERT_TX_FAILED   0x0010
#define TCPC_ALERT_TX_DISCARD  0x0020
#define TCPC_ALERT_TX_SUCCESS  0x0040
#define TCPC_ALERT_TX_ANY      (TCPC_ALERT_TX_FAILED | TCPC_ALERT_TX_DISCARD | TCPC_ALERT_TX_SUCCESS)
/* rt1711h BMC receiver threshold (Linux rt1711h_init_cc_params) */
#define RT1711H_RTCTRL4        0x93      /* bit0 RXDZSEL */
#define RT1711H_RTCTRL18       0xAF      /* bit0 BMCIO_RXDZEN */

/* What we ask for. The bq2589x DTB node limits the input to 2 A; 9 V x 2 A = 18 W. */
#define PD_WANT_MV             9000
#define PD_MAX_MA              2000
#define PD_WARM_TENTHS         450       /* battery >= 45.0 degC at negotiation: stay at 5 V */

static const PCSTR g_PdNames[] = { "off", "wait-caps", "wait-accept", "wait-ps-rdy", "ready", "no-pd" };

/* ---- buffered log (the log file is write-through: too slow inside the negotiation) ---------- */

#define PD_LOG_LINES 24
#define PD_LOG_LEN   200
static CHAR  g_PdLog[PD_LOG_LINES][PD_LOG_LEN];
static ULONG g_PdLogN, g_PdLogLost;
static ULONGLONG g_PdT0;

static VOID PdLog(_In_z_ _Printf_format_string_ PCSTR Fmt, ...)
{
    va_list ap;
    ULONG ms = (ULONG)((KeQueryInterruptTime() - g_PdT0) / 10000);

    if (g_PdLogN >= PD_LOG_LINES) {
        g_PdLogLost++;
        return;
    }
    RtlStringCbPrintfA(g_PdLog[g_PdLogN], PD_LOG_LEN, "pd +%ums: ", ms);
    {
        size_t used = 0;
        RtlStringCbLengthA(g_PdLog[g_PdLogN], PD_LOG_LEN, &used);
        va_start(ap, Fmt);
        RtlStringCbVPrintfA(g_PdLog[g_PdLogN] + used, PD_LOG_LEN - used, Fmt, ap);
        va_end(ap);
    }
    g_PdLogN++;
}

static VOID PdLogFlush(VOID)
{
    ULONG i;

    for (i = 0; i < g_PdLogN; i++) {
        LogPrint("%s\n", g_PdLog[i]);
    }
    if (g_PdLogLost != 0) {
        LogPrint("pd: %u log lines dropped\n", g_PdLogLost);
    }
    g_PdLogN = 0;
    g_PdLogLost = 0;
}

/* ---- TCPC access ----------------------------------------------------------------------------- */

static NTSTATUS TcpcWrite16(PDEVICE_CONTEXT Ctx, UCHAR Reg, USHORT Val)
{
    UCHAR b[3] = { Reg, (UCHAR)Val, (UCHAR)(Val >> 8) };
    return GeniI2cWrite(&Ctx->Bus, TCPC_ADDR, b, sizeof(b), TRUE);
}

static NTSTATUS TcpcReadAlert(PDEVICE_CONTEXT Ctx, PUSHORT Alert)
{
    /* rt1711h auto-idles: retry once */
    NTSTATUS s = I2cReadWord(&Ctx->Bus, TCPC_ADDR, TCPC_REG_ALERT, Alert);
    if (!NT_SUCCESS(s)) {
        s = I2cReadWord(&Ctx->Bus, TCPC_ADDR, TCPC_REG_ALERT, Alert);
    }
    return s;
}

static VOID RmwBit0(PDEVICE_CONTEXT Ctx, UCHAR Reg, BOOLEAN Set)
{
    UCHAR v = 0;
    if (NT_SUCCESS(I2cReadByte(&Ctx->Bus, TCPC_ADDR, Reg, &v))) {
        I2cWriteByte(&Ctx->Bus, TCPC_ADDR, Reg, Set ? (UCHAR)(v | 1) : (UCHAR)(v & ~1));
    }
}

/* Sends one SOP message and waits for the TCPC's verdict (GoodCRC received or retries failed). */
static NTSTATUS PdTx(PDEVICE_CONTEXT Ctx, ULONG Type, ULONG Cnt, const ULONG *Obj)
{
    PPD_PORT pd = &Ctx->Pd;
    USHORT hdr = PD_HDR(Type, pd->TxId, Cnt), alert = 0;
    UCHAR data[1 + 7 * 4];
    ULONGLONG end;
    NTSTATUS s;

    TcpcWrite16(Ctx, TCPC_REG_ALERT, TCPC_ALERT_TX_ANY);
    s = I2cWriteByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_TX_BYTE_CNT, (UCHAR)(2 + 4 * Cnt));
    if (NT_SUCCESS(s)) s = TcpcWrite16(Ctx, TCPC_REG_TX_HDR, hdr);
    if (NT_SUCCESS(s) && Cnt != 0) {
        data[0] = TCPC_REG_TX_DATA;
        RtlCopyMemory(&data[1], Obj, 4 * Cnt);              /* little endian, as on the wire */
        s = GeniI2cWrite(&Ctx->Bus, TCPC_ADDR, data, 1 + 4 * Cnt, TRUE);
    }
    if (NT_SUCCESS(s)) s = I2cWriteByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_TRANSMIT, TCPC_TRANSMIT_SOP_R3);
    if (!NT_SUCCESS(s)) {
        PdLog("tx type %u: i2c error %08x", Type, s);
        return s;
    }
    end = KeQueryInterruptTime() + 20 * 10000;              /* 3 retries take ~4 ms */
    do {
        if (NT_SUCCESS(TcpcReadAlert(Ctx, &alert)) && (alert & TCPC_ALERT_TX_ANY)) {
            break;
        }
        KeStallExecutionProcessor(100);
    } while (KeQueryInterruptTime() < end);
    TcpcWrite16(Ctx, TCPC_REG_ALERT, (USHORT)(alert & TCPC_ALERT_TX_ANY));
    if (alert & TCPC_ALERT_TX_SUCCESS) {
        pd->TxId = (pd->TxId + 1) & 7;
        return STATUS_SUCCESS;
    }
    PdLog("tx type %u cnt %u id %u: %s (alert %04x)", Type, Cnt, pd->TxId,
          (alert & TCPC_ALERT_TX_DISCARD) ? "discarded" : (alert & TCPC_ALERT_TX_FAILED) ? "no GoodCRC" : "timeout",
          alert);
    return STATUS_IO_DEVICE_ERROR;
}

static VOID PdSetState(PDEVICE_CONTEXT Ctx, ULONG State)
{
    if (Ctx->Pd.State != State) {
        PdLog("state %s -> %s", g_PdNames[Ctx->Pd.State], g_PdNames[State]);
        Ctx->Pd.State = State;
        Ctx->Pd.StateSince = KeQueryInterruptTime();
    }
}

/* Both sides' message counters start over (attach, hard reset, soft reset). */
static VOID PdResetProtocol(PDEVICE_CONTEXT Ctx)
{
    Ctx->Pd.TxId = 0;
    Ctx->Pd.RxId = 0xFF;
}

/* ---- policy ------------------------------------------------------------------------------- */

static VOID PdOnSourceCaps(PDEVICE_CONTEXT Ctx, const ULONG *Pdo, ULONG Cnt, ULONG Rev)
{
    PPD_PORT pd = &Ctx->Pd;
    ULONG i, pos = 1, mv = 5000, ma, rdo;
    BOOLEAN warm;
    LONG temp;
    KIRQL irql;

    KeAcquireSpinLock(&Ctx->SnapLock, &irql);
    temp = Ctx->Snap.TempTenthsC;
    warm = !Ctx->Snap.Valid || temp >= PD_WARM_TENTHS;
    KeReleaseSpinLock(&Ctx->SnapLock, irql);

    ma = PDO_FIXED_MA(Pdo[0]);
    if (!warm) {
        for (i = 0; i < Cnt; i++) {
            if (PDO_TYPE(Pdo[i]) == 0 && PDO_FIXED_MV(Pdo[i]) == PD_WANT_MV && PDO_FIXED_MA(Pdo[i]) >= 1000) {
                pos = i + 1;
                mv = PD_WANT_MV;
                ma = PDO_FIXED_MA(Pdo[i]);
                break;
            }
        }
    }
    ma = min(ma, PD_MAX_MA);
    /* fixed RDO: object position, no USB suspend, operating = max current */
    rdo = (pos << 28) | (1u << 24) | ((ma / 10) << 10) | (ma / 10);
    pd->ReqMv = mv;
    pd->ReqMa = ma;
    if (NT_SUCCESS(PdTx(Ctx, PD_DATA_REQUEST, 1, &rdo))) {
        PdSetState(Ctx, PD_ST_WAIT_ACCEPT);
    } else {
        PdSetState(Ctx, PD_ST_WAIT_CAPS);
    }

    /* logging only after the Request is out (tSenderResponse) */
    PdLog("Source_Capabilities rev %u.0, %u PDOs%s:", Rev + 1, Cnt, warm ? " (battery warm or unknown: 5 V only)" : "");
    for (i = 0; i < Cnt; i++) {
        ULONG p = Pdo[i];
        switch (PDO_TYPE(p)) {
        case 0:  PdLog("  PDO%u fixed %umV %umA (%08x)", i + 1, PDO_FIXED_MV(p), PDO_FIXED_MA(p), p); break;
        case 3:  PdLog("  PDO%u %s %u-%umV %umA (%08x)", i + 1, APDO_PPS(p) ? "PPS" : "APDO?", APDO_MIN_MV(p),
                       APDO_MAX_MV(p), APDO_MA(p), p); break;
        default: PdLog("  PDO%u type %u (%08x)", i + 1, PDO_TYPE(p), p); break;
        }
    }
    PdLog("Request PDO%u %umV %umA rdo=%08x temp=%d.%dC", pos, mv, ma, rdo, temp / 10,
          (temp < 0 ? -temp : temp) % 10);
}

static VOID PdOnMessage(PDEVICE_CONTEXT Ctx, USHORT Hdr, const ULONG *Obj)
{
    PPD_PORT pd = &Ctx->Pd;
    ULONG type = PD_HDR_TYPE(Hdr), cnt = PD_HDR_CNT(Hdr), id = PD_HDR_ID(Hdr);

    if (cnt == 0 && type == PD_CTRL_SOFT_RESET) {
        PdResetProtocol(Ctx);
        PdTx(Ctx, PD_CTRL_ACCEPT, 0, NULL);
        PdLog("Soft_Reset -> Accept, waiting for capabilities");
        pd->RxId = id;
        PdSetState(Ctx, PD_ST_WAIT_CAPS);
        return;
    }
    if (id == pd->RxId) {
        PdLog("duplicate message id %u type %u cnt %u ignored", id, type, cnt);
        return;
    }
    pd->RxId = id;

    if (PD_HDR_EXT(Hdr)) {
        PdLog("extended message type %u ignored", type);
        return;
    }
    if (cnt != 0) {
        switch (type) {
        case PD_DATA_SRC_CAP:
            pd->Rx++;
            PdOnSourceCaps(Ctx, Obj, cnt, PD_HDR_REV(Hdr));
            return;
        case PD_DATA_VDM:
            PdLog("VDM %08x ignored", Obj[0]);
            return;
        default:
            PdLog("data message type %u cnt %u ignored", type, cnt);
            return;
        }
    }
    switch (type) {
    case PD_CTRL_ACCEPT:
        if (pd->State == PD_ST_WAIT_ACCEPT) {
            PdSetState(Ctx, PD_ST_WAIT_PS_RDY);
        }
        PdLog("Accept");
        break;
    case PD_CTRL_REJECT:
    case PD_CTRL_WAIT:
        PdLog("%s: keeping the previous contract", type == PD_CTRL_REJECT ? "Reject" : "Wait");
        PdSetState(Ctx, pd->ContractMv != 0 ? PD_ST_READY : PD_ST_WAIT_CAPS);
        break;
    case PD_CTRL_PS_RDY:
        if (pd->State == PD_ST_WAIT_PS_RDY) {
            pd->ContractMv = pd->ReqMv;
            pd->ContractMa = pd->ReqMa;
            PdSetState(Ctx, PD_ST_READY);
            PdLog("PS_RDY: contract %umV %umA", pd->ContractMv, pd->ContractMa);
        } else {
            PdLog("PS_RDY in state %s ignored", g_PdNames[pd->State]);
        }
        break;
    case PD_CTRL_GET_SNK_CAP: {
        /* 5 V 2 A (higher capability: we prefer more), 9 V 2 A */
        ULONG snk[2] = { (1u << 28) | (100u << 10) | (PD_MAX_MA / 10), (180u << 10) | (PD_MAX_MA / 10) };
        PdTx(Ctx, PD_DATA_SNK_CAP, 2, snk);
        PdLog("Get_Sink_Cap -> Sink_Capabilities 5V/9V 2A");
        break;
    }
    case PD_CTRL_GET_SRC_CAP:
    case PD_CTRL_DR_SWAP:
    case PD_CTRL_PR_SWAP:
    case PD_CTRL_VCONN_SWAP:
        PdTx(Ctx, PD_CTRL_REJECT, 0, NULL);
        PdLog("control %u -> Reject", type);
        break;
    default:
        PdLog("control %u ignored", type);
        break;
    }
}

/* Drains the RX buffer and handles a hard reset. Returns TRUE if anything happened. */
static BOOLEAN PdPoll(PDEVICE_CONTEXT Ctx)
{
    USHORT alert = 0, hdr = 0;
    UCHAR cnt = 0, frame = 0;
    ULONG obj[7];

    if (!NT_SUCCESS(TcpcReadAlert(Ctx, &alert))) {
        return FALSE;
    }
    if (alert & TCPC_ALERT_RX_HARD_RST) {
        TcpcWrite16(Ctx, TCPC_REG_ALERT, TCPC_ALERT_RX_HARD_RST);
        PdResetProtocol(Ctx);
        Ctx->Pd.ContractMv = 0;
        Ctx->Pd.ContractMa = 0;
        Ctx->Pd.HardResets++;
        PdLog("Hard_Reset received: back to 5 V, waiting for capabilities");
        PdSetState(Ctx, PD_ST_WAIT_CAPS);
        return TRUE;
    }
    if (!(alert & TCPC_ALERT_RX_STATUS)) {
        return FALSE;
    }
    RtlZeroMemory(obj, sizeof(obj));
    I2cReadByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_RX_BYTE_CNT, &cnt);
    I2cReadWord(&Ctx->Bus, TCPC_ADDR, TCPC_REG_RX_HDR, &hdr);
    if (cnt > 3) {
        ULONG n = min((ULONG)cnt - 3, (ULONG)sizeof(obj));
        GeniI2cReadReg(&Ctx->Bus, TCPC_ADDR, TCPC_REG_RX_DATA, (UCHAR *)obj, n);
    }
    I2cReadByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_RX_FRAME_TYPE, &frame);
    TcpcWrite16(Ctx, TCPC_REG_ALERT, TCPC_ALERT_RX_STATUS);    /* read complete */
    if ((frame & 7) != 0) {
        PdLog("frame type %u ignored", frame & 7);
        return TRUE;
    }
    if (PD_HDR_CNT(hdr) * 4 + 3 > cnt) {
        PdLog("short message: byte_cnt %u header %04x", cnt, hdr);
        return TRUE;
    }
    PdOnMessage(Ctx, hdr, obj);
    return TRUE;
}

/*
 * Busy loop for up to MaxMs: answers within the PD deadlines. Ends early 1.5 s after a contract
 * (time for the source's Get_Sink_Cap / Discover Identity), or when no capabilities came in 3 s.
 */
static VOID PdRun(PDEVICE_CONTEXT Ctx, ULONG MaxMs)
{
    PPD_PORT pd = &Ctx->Pd;
    ULONGLONG start = KeQueryInterruptTime(), now;

    for (;;) {
        if (!PdPoll(Ctx)) {
            KeStallExecutionProcessor(200);
        }
        now = KeQueryInterruptTime();
        if (pd->State == PD_ST_WAIT_ACCEPT && now - pd->StateSince > 50 * 10000) {
            PdLog("no Accept in 50 ms");
            PdSetState(Ctx, PD_ST_WAIT_CAPS);
        } else if (pd->State == PD_ST_WAIT_PS_RDY && now - pd->StateSince > 700 * 10000) {
            PdLog("no PS_RDY in 700 ms");
            PdSetState(Ctx, PD_ST_WAIT_CAPS);
        }
        if (pd->State == PD_ST_READY && now - pd->StateSince > 1500 * 10000) {
            break;
        }
        if (pd->State == PD_ST_WAIT_CAPS && pd->Rx == 0 && now - start > 3000 * 10000) {
            PdLog("no Source_Capabilities in 3 s: not a PD source (or it gave up)");
            PdSetState(Ctx, PD_ST_NO_PD);
            break;
        }
        if (now - start > (ULONGLONG)MaxMs * 10000) {
            break;
        }
    }
}

/* ---- entry points from the Type-C state machine (gauge.c) -------------------------------- */

/* Called right after the port became a sink. Cc = CC_STATUS at attach. */
VOID PdAttach(PDEVICE_CONTEXT Ctx, UCHAR Cc)
{
    PPD_PORT pd = &Ctx->Pd;
    BOOLEAN cc2 = TCPC_CC1(Cc) == 0 && TCPC_CC2(Cc) != 0;
    ULONG rp = cc2 ? TCPC_CC2(Cc) : TCPC_CC1(Cc);
    NTSTATUS s = STATUS_SUCCESS;

    g_PdT0 = KeQueryInterruptTime();
    RtlZeroMemory(pd, sizeof(*pd));
    PdResetProtocol(Ctx);

    /* polarity: talk on the CC with Rp, open the other one (Linux tcpci_set_polarity) */
    s |= I2cWriteByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_ROLE_CTRL, cc2 ? 0x0B : 0x0E);
    s |= I2cWriteByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_TCPC_CTRL, cc2 ? 1 : 0);
    /* BMC receiver threshold for Rp 1.5/3.0 (rt1711h: RXDZEN=1, RXDZSEL=0) */
    RmwBit0(Ctx, RT1711H_RTCTRL18, rp >= 2);
    RmwBit0(Ctx, RT1711H_RTCTRL4, rp < 2);
    s |= I2cWriteByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_MSG_HDR_INFO, TCPC_MSG_HDR_SNK_UFP_20);
    s |= TcpcWrite16(Ctx, TCPC_REG_ALERT, 0xFFFF);
    s |= I2cWriteByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_RX_DETECT, TCPC_RX_DETECT_SOP_HR);
    PdLog("attach: cc_status=%02x active cc%u rp=%u, rx on (%08x)", Cc, cc2 ? 2 : 1, rp, s);
    if (!NT_SUCCESS(s)) {
        PdSetState(Ctx, PD_ST_NO_PD);
        PdLogFlush();
        return;
    }
    PdSetState(Ctx, PD_ST_WAIT_CAPS);
    PdRun(Ctx, 4000);
    PdLogFlush();
}

/* Called every Type-C step while we are a sink: handles late messages (re-sent capabilities). */
VOID PdService(PDEVICE_CONTEXT Ctx)
{
    USHORT alert = 0;

    if (Ctx->Pd.State == PD_ST_OFF) {
        return;
    }
    if (NT_SUCCESS(TcpcReadAlert(Ctx, &alert)) && (alert & (TCPC_ALERT_RX_STATUS | TCPC_ALERT_RX_HARD_RST))) {
        g_PdT0 = KeQueryInterruptTime();
        PdLog("service: alert %04x in state %s", alert, g_PdNames[Ctx->Pd.State]);
        PdRun(Ctx, 3000);
        PdLogFlush();
    }
}

/* Detach / leaving D0: no PD reception, default terminations and receiver settings. */
VOID PdDetach(PDEVICE_CONTEXT Ctx)
{
    if (Ctx->Pd.State != PD_ST_OFF) {
        LogPrint("pd: detach (state %s, contract %umV %umA, hard resets %u)\n", g_PdNames[Ctx->Pd.State],
                 Ctx->Pd.ContractMv, Ctx->Pd.ContractMa, Ctx->Pd.HardResets);
    }
    I2cWriteByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_RX_DETECT, 0);
    I2cWriteByte(&Ctx->Bus, TCPC_ADDR, TCPC_REG_TCPC_CTRL, 0);
    RmwBit0(Ctx, RT1711H_RTCTRL18, FALSE);
    RmwBit0(Ctx, RT1711H_RTCTRL4, TRUE);
    RtlZeroMemory(&Ctx->Pd, sizeof(Ctx->Pd));
}

PCSTR PdStateName(PDEVICE_CONTEXT Ctx)
{
    return g_PdNames[Ctx->Pd.State];
}
