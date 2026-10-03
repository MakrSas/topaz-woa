/*
 * WinRE-look "Choose an option" page for the topaz boot menu, with touch.
 *
 * The page is a set of pre-rendered 8-bit grey bitmaps (uefi/winre/mkwinre.py, RAW sections
 * of one FREEFORM file in FvMain). Grey = coverage: each bitmap is blended white over the
 * tile colour (black); focus is a white outline drawn in code.
 * Look, layout, icons and fonts: ntdevlabs/exynos9810-woa tools/twrp-winre (see NOTICE.md).
 *
 * Touch: FocalTech FT5452 @0x38 on QUP0 SE2, polled like drivers/TopazTouch (no firmware
 * download needed; reset pulse only if it does not answer). Keys: Vol+/Vol- move, Power selects.
 */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/BmpSupportLib.h>
#include <Protocol/GraphicsOutput.h>
#include "TopazOtg.h"

/* FREEFORM file with the RAW sections, see build_topaz_uefi.sh */
STATIC EFI_GUID  mWinReGuid = { 0x5d0b7a63, 0x2c84, 0x4e19, { 0xb7, 0x3f, 0x91, 0x0a, 0x6e, 0x52, 0xc4, 0x28 } };

enum { ASSET_HEADER, ASSET_TILE0, ASSET_BACK = ASSET_TILE0 + 4, ASSET_HINT, ASSET_COUNT };

#define TILE_COUNT   4
#define FOCUS_BACK   TILE_COUNT          /* focus index of the back arrow */

/* exynos9810-woa layout (1080 x 1920 theme), rows spread for the 2400 px panel */
#define MARGIN_X     108
#define HEADER_Y     420
#define ROW0_Y       700
#define ROW_PITCH    270
#define BACK_BOX_X   60
#define BACK_BOX_Y   205
#define BACK_BOX     120
#define HINT_BOTTOM  260
#define OUTLINE      3                   /* focus: white outline, black inside */

typedef struct {
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Px;
  UINTN                          W, H;
} ASSET;

STATIC ASSET                         mAsset[ASSET_COUNT];
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL  *mGop;

/* ---- drawing ----------------------------------------------------------------- */

STATIC EFI_STATUS LoadAssets(VOID)
{
  UINTN i, size, bltSize;
  VOID  *bmp;

  for (i = 0; i < ASSET_COUNT; i++) {
    if (mAsset[i].Px != NULL) {
      continue;
    }
    bmp = NULL;
    if (EFI_ERROR (GetSectionFromAnyFv (&mWinReGuid, EFI_SECTION_RAW, i, &bmp, &size)) ||
        EFI_ERROR (TranslateBmpToGopBlt (bmp, size, &mAsset[i].Px, &bltSize, &mAsset[i].H, &mAsset[i].W))) {
      LOG ("winre: asset %u missing\n", (UINT32)i);
      if (bmp != NULL) {
        FreePool (bmp);
      }
      return EFI_NOT_FOUND;
    }
    FreePool (bmp);
  }
  return EFI_SUCCESS;
}

STATIC VOID Fill(UINTN X, UINTN Y, UINTN W, UINTN H, UINT8 Grey)
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL c = { Grey, Grey, Grey, 0 };

  mGop->Blt (mGop, &c, EfiBltVideoFill, 0, 0, X, Y, W, H, 0);
}

/* white over Bg, weighted by the asset's grey */
STATIC VOID Draw(UINTN Id, UINTN X, UINTN Y, UINT8 Bg)
{
  ASSET *a = &mAsset[Id];
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *tmp;
  UINTN i, n = a->W * a->H;
  UINT8 v;

  tmp = AllocatePool (n * sizeof (*tmp));
  if (tmp == NULL) {
    return;
  }
  for (i = 0; i < n; i++) {
    v = (UINT8)(Bg + ((255 - Bg) * a->Px[i].Red) / 255);
    tmp[i].Blue = tmp[i].Green = tmp[i].Red = v;
    tmp[i].Reserved = 0;
  }
  mGop->Blt (mGop, tmp, EfiBltBufferToVideo, 0, 0, X, Y, a->W, a->H, a->W * sizeof (*tmp));
  FreePool (tmp);
}

STATIC VOID Frame(UINTN X, UINTN Y, UINTN W, UINTN H, UINT8 Grey)
{
  Fill (X, Y, W, OUTLINE, Grey);
  Fill (X, Y + H - OUTLINE, W, OUTLINE, Grey);
  Fill (X, Y, OUTLINE, H, Grey);
  Fill (X + W - OUTLINE, Y, OUTLINE, H, Grey);
}

/* focus = tile / back arrow with a white outline (WinRE keyboard focus), else plain */
STATIC VOID DrawFocus(UINTN F, BOOLEAN On)
{
  if (F == FOCUS_BACK) {
    Draw (ASSET_BACK, BACK_BOX_X + (BACK_BOX - mAsset[ASSET_BACK].W) / 2,
          BACK_BOX_Y + (BACK_BOX - mAsset[ASSET_BACK].H) / 2, 0);
    Frame (BACK_BOX_X, BACK_BOX_Y, BACK_BOX, BACK_BOX, On ? 255 : 0);
  } else if (F < TILE_COUNT) {
    Draw (ASSET_TILE0 + F, MARGIN_X, ROW0_Y + F * ROW_PITCH, 0);
    Frame (MARGIN_X, ROW0_Y + F * ROW_PITCH, mAsset[ASSET_TILE0].W, mAsset[ASSET_TILE0].H, On ? 255 : 0);
  }
}

STATIC VOID DrawPage(UINTN Focus)
{
  UINTN sx = mGop->Mode->Info->HorizontalResolution, sy = mGop->Mode->Info->VerticalResolution, t;

  Fill (0, 0, sx, sy, 0);
  Draw (ASSET_HEADER, MARGIN_X, HEADER_Y, 0);
  DrawFocus (FOCUS_BACK, Focus == FOCUS_BACK);
  for (t = 0; t < TILE_COUNT; t++) {
    DrawFocus (t, t == Focus);
  }
  Draw (ASSET_HINT, (sx - mAsset[ASSET_HINT].W) / 2, sy - HINT_BOTTOM, 0);
}

/* focus index under a screen point, or MAX_UINTN */
STATIC UINTN HitTest(UINTN X, UINTN Y)
{
  UINTN t;

  if (X >= BACK_BOX_X - 30 && X < BACK_BOX_X + BACK_BOX + 30 && Y >= BACK_BOX_Y - 30 && Y < BACK_BOX_Y + BACK_BOX + 30) {
    return FOCUS_BACK;
  }
  for (t = 0; t < TILE_COUNT; t++) {
    UINTN y0 = ROW0_Y + t * ROW_PITCH;
    if (X >= MARGIN_X && X < MARGIN_X + mAsset[ASSET_TILE0].W && Y >= y0 && Y < y0 + mAsset[ASSET_TILE0].H) {
      return t;
    }
  }
  return MAX_UINTN;
}

/* ---- touch (drivers/TopazTouch/hw.h) ----------------------------------------- */

#define TLMM_PIN(tile, pin)  (0x00400000UL + (tile) + (pin) * 0x1000UL)
#define TLMM_WEST            0x00100000UL
#define TLMM_EAST            0x00900000UL
#define TLMM_CTL(func, pull, ma, oe)  (((pull) & 3) | ((func) << 2) | ((((ma) / 2) - 1) << 6) | ((oe) ? (1u << 9) : 0))
#define TS_ADDR              0x38
#define TS_SDA               6     /* WEST, func 1 = qup2 */
#define TS_SCL               7
#define TS_IRQ               80    /* WEST, active low */
#define TS_RESET             86    /* WEST, active low */
#define TS_AVDD              36    /* EAST, output high */
#define TS_RAW_X             1350  /* FT5452 fw 0x36 reports 1.25x the panel */
#define TS_RAW_Y             3000
#define TS_DATA_LEN          (3 + 6 * 10)

STATIC BOOLEAN mTouch;

STATIC EFI_STATUS TsReg(UINT8 Reg, UINT8 *Buf, UINT32 Len)
{
  EFI_STATUS s = I2cWrite (TS_ADDR, &Reg, 1, FALSE);

  return EFI_ERROR (s) ? s : I2cRead (TS_ADDR, Buf, Len);
}

STATIC VOID TouchInit(VOID)
{
  EFI_STATUS s;
  UINT8 id = 0;

  MmioWrite32 (TLMM_PIN (TLMM_WEST, TS_SDA), TLMM_CTL (1, 0, 2, FALSE));
  MmioWrite32 (TLMM_PIN (TLMM_WEST, TS_SCL), TLMM_CTL (1, 0, 2, FALSE));
  MmioWrite32 (TLMM_PIN (TLMM_WEST, TS_IRQ), TLMM_CTL (0, 3, 8, FALSE));
  MmioWrite32 (TLMM_PIN (TLMM_EAST, TS_AVDD) + 4, 2);
  MmioWrite32 (TLMM_PIN (TLMM_EAST, TS_AVDD), TLMM_CTL (0, 3, 8, TRUE));

  s = GeniOpen (&gTopazSe2);
  if (!EFI_ERROR (s)) {
    s = TsReg (0xA3, &id, 1);
    if (EFI_ERROR (s)) {
      /* not running: reset pulse (ts_reset_active), then give the firmware time to boot */
      MmioWrite32 (TLMM_PIN (TLMM_WEST, TS_RESET) + 4, 0);
      MmioWrite32 (TLMM_PIN (TLMM_WEST, TS_RESET), TLMM_CTL (0, 3, 8, TRUE));
      gBS->Stall (20 * 1000);
      MmioWrite32 (TLMM_PIN (TLMM_WEST, TS_RESET) + 4, 2);
      gBS->Stall (250 * 1000);
      s = TsReg (0xA3, &id, 1);
    }
  }
  mTouch = !EFI_ERROR (s);
  LOG ("winre: touch %r chip_id=%02x\n", s, id);
}

/* TRUE + panel coordinates while a finger is down */
STATIC BOOLEAN TouchPoll(UINTN *X, UINTN *Y)
{
  UINT8 b[TS_DATA_LEN];
  UINT8 *p = b + 3;
  UINTN x, y;

  if (!mTouch || EFI_ERROR (TsReg (0x00, b, sizeof (b)))) {
    return FALSE;
  }
  if ((b[2] & 0x0F) == 0 || (p[0] >> 6) == 1 || (p[0] >> 6) == 3) {   /* no points / up / none */
    return FALSE;
  }
  x = ((UINTN)(p[0] & 0x0F) << 8) | p[1];
  y = ((UINTN)(p[2] & 0x0F) << 8) | p[3];
  *X = MIN (x * mGop->Mode->Info->HorizontalResolution / TS_RAW_X, mGop->Mode->Info->HorizontalResolution - 1);
  *Y = MIN (y * mGop->Mode->Info->VerticalResolution / TS_RAW_Y, mGop->Mode->Info->VerticalResolution - 1);
  return TRUE;
}

/* ---- page loop ---------------------------------------------------------------- */

UINTN WinReMenu(VOID)
{
  EFI_INPUT_KEY key;
  UINTN focus = 0, pressed = MAX_UINTN, x = 0, y = 0, hit, result;
  BOOLEAN down;

  if (EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&mGop)) ||
      EFI_ERROR (LoadAssets ())) {
    return WINRE_UNAVAILABLE;
  }
  TouchInit ();
  DrawPage (focus);
  while (gST->ConIn != NULL && !EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &key))) {
    /* drop the key that opened the page */
  }

  for (;;) {
    if (gST->ConIn != NULL && !EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &key))) {
      if (key.ScanCode == SCAN_UP || key.ScanCode == SCAN_DOWN) {
        DrawFocus (focus, FALSE);
        focus = (focus + (key.ScanCode == SCAN_UP ? FOCUS_BACK : 1)) % (FOCUS_BACK + 1);
        DrawFocus (focus, TRUE);
      } else {
        result = focus;
        break;
      }
    }

    down = TouchPoll (&x, &y);
    hit  = down ? HitTest (x, y) : MAX_UINTN;
    if (down && pressed == MAX_UINTN && hit != MAX_UINTN) {
      pressed = hit;                                     /* finger down on an option */
      DrawFocus (focus, FALSE);
      DrawFocus (pressed, TRUE);
    } else if (down && pressed != MAX_UINTN && hit != pressed) {
      DrawFocus (pressed, FALSE);                            /* slid off: cancel */
      pressed = MAX_UINTN;
      DrawFocus (focus, TRUE);
    } else if (!down && pressed != MAX_UINTN) {
      result = pressed;                                  /* lifted on it: choose */
      break;
    }
    gBS->Stall (15 * 1000);
  }

  LOG ("winre: choice %u\n", (UINT32)result);
  GeniOpen (&gTopazSe1);
  return result == FOCUS_BACK ? WINRE_BACK : result;
}
