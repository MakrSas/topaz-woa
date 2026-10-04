/* ta.c: TopazAudio (\\.\TopazAudio) client, PASSIVE_LEVEL only */
#pragma once

BOOLEAN  TaAdspReady(VOID);
VOID     TaClose(VOID);
NTSTATUS TaWr(ULONGLONG Pa, ULONG Value);
ULONG    TaRd(ULONGLONG Pa);
NTSTATUS TaI2cWrite(UCHAR Addr, UCHAR Reg, UCHAR Value);
VOID     TaSleep(ULONG Ms);
NTSTATUS TaGpr(ULONG Dst, ULONG Src, ULONG Opcode, BOOLEAN ApmHdr, const ULONG *Dw, ULONG N,
               ULONG *RspOpcode, ULONG *Rsp, ULONG MaxWords, ULONG TimeoutMs);
NTSTATUS TaApm(ULONG Opcode, BOOLEAN ApmHdr, const ULONG *Dw, ULONG N, ULONG *Rsp, ULONG MaxWords);
