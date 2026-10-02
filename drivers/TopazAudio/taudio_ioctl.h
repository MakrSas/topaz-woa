/*
 * TopazAudio lab interface (\\.\TopazAudio, admin only), shared by the driver and tools/audio/taudio.c.
 * Lets the speaker-path bring-up (docs/P9_audio.md, A5) run from user mode while the driver keeps
 * the ADSP, GLINK and QRTR alive: raw GPR packets, MMIO in a few ranges, I2C on QUP0 SE1, and
 * physically contiguous buffers for ADSP shared memory.
 */
#pragma once

#define TAUDIO_DEVICE_NAME   L"\\\\.\\TopazAudio"
#define TAUDIO_TYPE          0x8A5D

#define TAUDIO_IOCTL(n)      CTL_CODE (TAUDIO_TYPE, 0x800 + (n), METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_TAUDIO_STATE    TAUDIO_IOCTL (0)   /* out: TAUDIO_STATE */
#define IOCTL_TAUDIO_GPR_SEND TAUDIO_IOCTL (1)   /* in: whole GPR packet (header included) */
#define IOCTL_TAUDIO_GPR_RECV TAUDIO_IOCTL (2)   /* out: oldest received packet, 0 bytes if none */
#define IOCTL_TAUDIO_MMIO     TAUDIO_IOCTL (3)   /* in/out: TAUDIO_MMIO */
#define IOCTL_TAUDIO_I2C      TAUDIO_IOCTL (4)   /* in/out: TAUDIO_I2C */
#define IOCTL_TAUDIO_BUF      TAUDIO_IOCTL (5)   /* in/out: TAUDIO_BUF (+ data for write) */

#define TAUDIO_GPR_MAX       4096

typedef struct {
  unsigned int AdspState;      /* SMP2P slave-kernel bits, 0 if not booted */
  unsigned int GprUp;          /* adsp_apps channel up */
  unsigned int SpfState;       /* last APM SPF state (1 = ready) */
  unsigned int RxQueued;       /* GPR packets waiting in the RX ring */
  unsigned int RxDropped;
  unsigned int TxQueued;
} TAUDIO_STATE;

/* Op: 0 read32, 1 write32. Allowed: a list of known LPASS blocks (Lab.c MmioAllowed), TLMM,
   apps SMMU. Unknown LPASS pages can hang the bus (watchdog reboot): they are refused. */
typedef struct {
  unsigned int Op;
  unsigned int Count;          /* read: number of consecutive dwords (<= 64) */
  unsigned long long Pa;
  unsigned int Value[64];      /* write: Value[0]; read: results */
} TAUDIO_MMIO;

/* One I2C transaction on QUP0 SE1 inside a TopazBattery quiet window: write WLen bytes (no stop
   if RLen != 0), then read RLen bytes. Result: 0 ok, 1 NACK, 2 error, 3 timeout, 4 no window. */
typedef struct {
  unsigned char Addr, WLen, RLen, Result;
  unsigned char W[16];
  unsigned char R[16];
} TAUDIO_I2C;

/* Op: 0 alloc (Size -> Pa, Id), 1 write (Id, Offset, data after the struct), 2 read (Id, Offset,
   Size -> data after the struct), 3 free (Id). Buffers are contiguous, below 4 GiB, write-combined. */
typedef struct {
  unsigned int Op, Id, Offset, Size;
  unsigned long long Pa;
} TAUDIO_BUF;
