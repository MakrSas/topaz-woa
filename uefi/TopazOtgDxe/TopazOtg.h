/* Shared between TopazOtgDxe.c (I2C, menu) and WinRe.c (WinRE-look menu, touch). */
#ifndef TOPAZ_OTG_H_
#define TOPAZ_OTG_H_

#include <Uefi.h>

#define TOPAZ_SE1_BASE  0x04A84000UL   /* QUP0 SE1: bq2589x@6a, rt1711h@4e */
#define TOPAZ_SE2_BASE  0x04A88000UL   /* QUP0 SE2: focaltech@38 */

typedef struct {
  UINTN   Base;
  UINT32  CbcrOff;     /* GCC offsets of the SE clock branch / RCG */
  UINT32  RcgOff;
  UINT32  VoteBit;     /* bit in GCC_APCS_CLOCK_BRANCH_ENA_VOTE */
} TOPAZ_SE;

extern CONST TOPAZ_SE  gTopazSe1;
extern CONST TOPAZ_SE  gTopazSe2;

VOID       LogAdd (CONST CHAR8 *Fmt, ...);
#define LOG(...)  LogAdd (__VA_ARGS__)

EFI_STATUS GeniOpen (CONST TOPAZ_SE *Se);
EFI_STATUS I2cWrite (UINT8 Addr, CONST UINT8 *Buf, UINT32 Len, BOOLEAN Stop);
EFI_STATUS I2cRead (UINT8 Addr, UINT8 *Buf, UINT32 Len);

/* WinRE-look menu choices (same order as the text menu) */
enum { WINRE_WINDOWS, WINRE_WINDOWS_NOGPU, WINRE_FASTBOOT, WINRE_POWEROFF, WINRE_BACK, WINRE_UNAVAILABLE };

/* Show the WinRE-look "Choose an option" page; touch + volume/power keys. Leaves SE1 selected.
   TimeoutSec != 0: count down and return WINRE_WINDOWS unless touched / a key is pressed. */
UINTN WinReMenu (UINTN TimeoutSec);

#endif
