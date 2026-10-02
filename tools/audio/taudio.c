/*
 * taudio - user-mode side of the TopazAudio lab interface (drivers/TopazAudio/taudio_ioctl.h).
 * Run elevated on the phone (ssh topaz-win). Numbers accept 0x.. hex or decimal.
 *
 *   taudio state                                  ADSP / GPR / ring state
 *   taudio recv [ms]                              print received GPR packets for ms (default 500)
 *   taudio cmd <dst> <src> <opcode> [dw..]        GPR packet, payload = dwords, then recv 500 ms
 *   taudio apm <dst> <src> <opcode> [dw..]        same with an in-band apm_cmd_header first
 *   taudio prm-hw <id> [rel]                      PRM HW core vote (1 LPASS macro, 2 DCODEC, 3 LPR)
 *   taudio prm-clk <id> <hz> [attr] [root]        PRM clock request (hz 0 = release)
 *   taudio rd <pa> [count]                        MMIO read32 (LPASS / TLMM / apps SMMU only)
 *   taudio wr <pa> <value>                        MMIO write32
 *   taudio i2c <addr> [wbytes..] [rN]             QUP0 SE1 transaction (write, then read N)
 *   taudio pmic <sid> <addr> [count]              PMIC read (SPMI observer), addr = periph<<8|reg
 */
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../drivers/TopazAudio/taudio_ioctl.h"

#define GPR_DOMAIN_ADSP 2
#define GPR_DOMAIN_APPS 3
#define GPR_PRM_IID     2
#define PRM_CMD_REQUEST_HW_RSC      0x0100100F
#define PRM_CMD_RELEASE_HW_RSC      0x01001010
#define PARAM_ID_RSC_HW_CORE        0x08001032
#define PARAM_ID_RSC_AUDIO_HW_CLK   0x0800102C
#define PARAM_ID_RSC_CPU_LPR        0x08001A6E

static HANDLE g_Dev = INVALID_HANDLE_VALUE;
static unsigned g_Token = 0x7000;

static unsigned Num(const char *s)
{
    return (unsigned)strtoul(s, NULL, 0);
}

static BOOL Ioctl(DWORD code, void *in, DWORD inLen, void *out, DWORD outLen, DWORD *got)
{
    DWORD n = 0;
    BOOL ok = DeviceIoControl(g_Dev, code, in, inLen, out, outLen, &n, NULL);
    if (got != NULL) {
        *got = n;
    }
    if (!ok) {
        printf("ioctl %08lx failed: %lu\n", code, GetLastError());
    }
    return ok;
}

static const char *OpName(unsigned op)
{
    switch (op) {
    case 0x02001005: return "GPR_BASIC_RSP_RESULT";
    case 0x02001006: return "GPR_BASIC_EVT_ACCEPTED";
    case 0x02001002: return "PRM_CMD_RSP_REQUEST_HW_RSC";
    case 0x02001003: return "PRM_CMD_RSP_RELEASE_HW_RSC";
    case 0x02001007: return "APM_CMD_RSP_GET_SPF_STATE";
    case 0x02001000: return "APM_CMD_RSP_GET_CFG";
    case 0x03001000: return "APM_EVENT_MODULE_TO_CLIENT";
    default:         return "";
    }
}

static void PrintPkt(const unsigned char *p, DWORD len)
{
    const unsigned *w = (const unsigned *)p;
    unsigned hlen = ((w[0] >> 4) & 0xF) * 4, i;

    if (len < 24) {
        printf("  short packet %lu B\n", len);
        return;
    }
    printf("  <- %lu B dom %u->%u port %x->%x token %x opcode %08x %s\n", len, p[5], p[4], w[2], w[3], w[4], w[5],
           OpName(w[5]));
    printf("     ");
    for (i = hlen; i + 4 <= len && i < hlen + 4 * 32; i += 4) {
        printf(" %08x", *(const unsigned *)(p + i));
    }
    printf("\n");
}

static void Recv(DWORD ms)
{
    static unsigned char buf[TAUDIO_GPR_MAX];
    DWORD t0 = GetTickCount(), got;

    do {
        while (Ioctl(IOCTL_TAUDIO_GPR_RECV, NULL, 0, buf, sizeof(buf), &got) && got != 0) {
            PrintPkt(buf, got);
        }
        Sleep(20);
    } while (GetTickCount() - t0 < ms);
}

/* GPR header + optional apm_cmd_header + dwords */
static void SendGpr(unsigned dst, unsigned src, unsigned opcode, BOOL apmHdr, const unsigned *dw, unsigned n)
{
    unsigned pkt[TAUDIO_GPR_MAX / 4], k = 6, i;
    unsigned bytes;

    memset(pkt, 0, sizeof(pkt));
    if (apmHdr) {
        pkt[k + 3] = n * 4;                   /* apm_cmd_header.payload_size */
        k += 4;
    }
    for (i = 0; i < n && k < TAUDIO_GPR_MAX / 4; i++) {
        pkt[k++] = dw[i];
    }
    bytes = k * 4;
    pkt[0] = 0 | (6 << 4) | (bytes << 8);
    pkt[1] = GPR_DOMAIN_ADSP | (GPR_DOMAIN_APPS << 8);
    pkt[2] = src;
    pkt[3] = dst;
    pkt[4] = g_Token++;
    pkt[5] = opcode;
    printf("  -> %u B port %x->%x token %x opcode %08x\n", bytes, src, dst, pkt[4], opcode);
    Ioctl(IOCTL_TAUDIO_GPR_SEND, pkt, bytes, NULL, 0, NULL);
    Recv(500);
}

static int Usage(void)
{
    printf("taudio state | recv [ms] | cmd|apm <dst> <src> <opcode> [dw..] | prm-hw <id> [rel] |\n"
           "       prm-clk <id> <hz> [attr] [root] | rd <pa> [count] | wr <pa> <val> | i2c <addr> [w..] [rN]\n");
    return 2;
}

int main(int argc, char **argv)
{
    const char *c;
    unsigned dw[256], n = 0, i;

    if (argc < 2) {
        return Usage();
    }
    g_Dev = CreateFileW(TAUDIO_DEVICE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (g_Dev == INVALID_HANDLE_VALUE) {
        printf("cannot open \\\\.\\TopazAudio: %lu\n", GetLastError());
        return 1;
    }
    c = argv[1];
    if (strcmp(c, "state") == 0) {
        TAUDIO_STATE s;
        if (Ioctl(IOCTL_TAUDIO_STATE, NULL, 0, &s, sizeof(s), NULL)) {
            printf("adsp slave-kernel %08x, gpr %s, spf state %d, rx queued %u dropped %u, tx queued %u\n", s.AdspState,
                   s.GprUp ? "UP" : "down", (int)s.SpfState, s.RxQueued, s.RxDropped, s.TxQueued);
        }
    } else if (strcmp(c, "recv") == 0) {
        Recv(argc > 2 ? Num(argv[2]) : 500);
    } else if ((strcmp(c, "cmd") == 0 || strcmp(c, "apm") == 0) && argc >= 5) {
        for (i = 5; i < (unsigned)argc && n < 256; i++) {
            dw[n++] = Num(argv[i]);
        }
        SendGpr(Num(argv[2]), Num(argv[3]), Num(argv[4]), c[0] == 'a', dw, n);
    } else if (strcmp(c, "prm-hw") == 0 && argc >= 3) {
        unsigned id = Num(argv[2]);
        BOOL rel = argc > 3 && strcmp(argv[3], "rel") == 0;
        /* apm_module_param_data {iid, param_id, size, error} + hw_clk_id / lpr_state */
        dw[0] = GPR_PRM_IID;
        dw[1] = (id == 3) ? PARAM_ID_RSC_CPU_LPR : PARAM_ID_RSC_HW_CORE;
        dw[2] = 4;
        dw[3] = 0;
        dw[4] = (id == 3) ? 1 : id;
        SendGpr(GPR_PRM_IID, GPR_PRM_IID, rel ? PRM_CMD_RELEASE_HW_RSC : PRM_CMD_REQUEST_HW_RSC, TRUE, dw, 5);
    } else if (strcmp(c, "prm-clk") == 0 && argc >= 4) {
        unsigned hz = Num(argv[3]);
        dw[0] = GPR_PRM_IID;
        dw[1] = PARAM_ID_RSC_AUDIO_HW_CLK;
        dw[3] = 0;
        dw[4] = 1;                            /* num_clk_id */
        dw[5] = Num(argv[2]);
        if (hz != 0) {
            dw[2] = 20;                       /* num + audio_hw_clk_cfg */
            dw[6] = hz;
            dw[7] = argc > 4 ? Num(argv[4]) : 1;   /* attr: COUPLE_NO */
            dw[8] = argc > 5 ? Num(argv[5]) : 0;   /* root: default */
            SendGpr(GPR_PRM_IID, GPR_PRM_IID, PRM_CMD_REQUEST_HW_RSC, TRUE, dw, 9);
        } else {
            dw[2] = 8;                        /* num + audio_hw_clk_rel_cfg */
            SendGpr(GPR_PRM_IID, GPR_PRM_IID, PRM_CMD_RELEASE_HW_RSC, TRUE, dw, 6);
        }
    } else if ((strcmp(c, "rd") == 0 || strcmp(c, "wr") == 0) && argc >= 3) {
        TAUDIO_MMIO m;
        memset(&m, 0, sizeof(m));
        m.Pa = strtoull(argv[2], NULL, 0);
        m.Op = (c[0] == 'w');
        if (m.Op) {
            if (argc < 4) {
                return Usage();
            }
            m.Value[0] = Num(argv[3]);
        } else {
            m.Count = argc > 3 ? Num(argv[3]) : 1;
        }
        if (Ioctl(IOCTL_TAUDIO_MMIO, &m, sizeof(m), &m, sizeof(m), NULL)) {
            if (m.Op) {
                printf("  %08llx <- %08x\n", m.Pa, m.Value[0]);
            } else {
                for (i = 0; i < m.Count; i++) {
                    if ((i % 4) == 0) {
                        printf("%s  %08llx:", i ? "\n" : "", m.Pa + 4ull * i);
                    }
                    printf(" %08x", m.Value[i]);
                }
                printf("\n");
            }
        }
    } else if (strcmp(c, "i2c") == 0 && argc >= 3) {
        TAUDIO_I2C t;
        static const char *res[] = { "ok", "NACK", "error", "timeout", "no bus window" };
        memset(&t, 0, sizeof(t));
        t.Addr = (unsigned char)Num(argv[2]);
        for (i = 3; i < (unsigned)argc; i++) {
            if (argv[i][0] == 'r') {
                t.RLen = (unsigned char)Num(argv[i] + 1);
            } else if (t.WLen < 16) {
                t.W[t.WLen++] = (unsigned char)Num(argv[i]);
            }
        }
        if (Ioctl(IOCTL_TAUDIO_I2C, &t, sizeof(t), &t, sizeof(t), NULL)) {
            printf("  i2c %02x: %s", t.Addr, t.Result <= 4 ? res[t.Result] : "?");
            for (i = 0; t.Result == 0 && i < t.RLen; i++) {
                printf(" %02x", t.R[i]);
            }
            printf("\n");
        }
    } else if (strcmp(c, "pmic") == 0 && argc >= 4) {
        TAUDIO_PMIC p;
        memset(&p, 0, sizeof(p));
        p.Sid = Num(argv[2]);
        p.Addr = Num(argv[3]);
        p.Count = argc > 4 ? Num(argv[4]) : 1;
        if (Ioctl(IOCTL_TAUDIO_PMIC, &p, sizeof(p), &p, sizeof(p), NULL)) {
            printf("  pmic sid %u %04x:", p.Sid, p.Addr);
            for (i = 0; i < p.Count && i < 32; i++) {
                printf(p.Value[i] > 0xFF ? " ??" : " %02x", p.Value[i]);
            }
            printf("\n");
        }
    } else {
        return Usage();
    }
    CloseHandle(g_Dev);
    return 0;
}
