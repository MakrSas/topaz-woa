/** @file
  tapas BootGraphicsLib (replaces MsGraphicsPkg BootGraphicsLib, which clears the screen and
  centers the Silicium logo).

  BG_SYSTEM_LOGO: put back the Xiaomi splash that TopazOtgDxe copied from the framebuffer at
  its entry point (GraphicsConsole cleared the panel in between), blank the bottom band where
  the splash says "Powered by Android" and draw the Silicium logo there. The Xiaomi logo
  (bounding box of the lit pixels above the band) becomes the BGRT image, so Windows Boot
  Manager shows it too. Without a saved splash: black screen + Silicium logo at the bottom.
  Other graphics (no OS, battery, ...) keep the upstream black + centered behaviour.

  Copyright (c) 2011 - 2018, Intel Corporation. All rights reserved.<BR>
  Copyright (C) Microsoft Corporation. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/BootLogo2.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BootGraphicsLib.h>
#include <Library/BootGraphicsProviderLib.h>
#include <Library/BmpSupportLib.h>
#include <Library/UefiLib.h>
#include "../../Drivers/TopazOtgDxe/TopazSplash.h"

#define BAND_TOP_PCT    75     /* "Powered by Android" lives below 3/4 of the height */
#define LOGO_BOTTOM_GAP 150    /* pixels between the Silicium logo and the bottom edge */

STATIC EFI_GUID  mSplashGuid = TOPAZ_SPLASH_PROTOCOL_GUID;

STATIC BOOLEAN
IsLit (
  IN EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *P
  )
{
  return (P->Red | P->Green | P->Blue) >= 0x20;
}

/* BGRT: crop the lit area of the splash above the band. */
STATIC VOID
SetBgrtFromSplash (
  IN EDKII_BOOT_LOGO2_PROTOCOL  *BootLogo2,
  IN TOPAZ_SPLASH               *S,
  IN UINTN                      BandTop
  )
{
  UINTN                          x, y, x0 = S->Width, y0 = BandTop, x1 = 0, y1 = 0, w, h;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *crop;

  for (y = 0; y < BandTop; y++) {
    for (x = 0; x < S->Width; x++) {
      if (IsLit (&S->Pixels[y * S->Width + x])) {
        x0 = MIN (x0, x);
        x1 = MAX (x1, x);
        y0 = MIN (y0, y);
        y1 = MAX (y1, y);
      }
    }
  }

  if ((x1 < x0) || (y1 < y0)) {
    return;
  }

  w    = x1 - x0 + 1;
  h    = y1 - y0 + 1;
  crop = AllocatePool (w * h * sizeof (*crop));
  if (crop == NULL) {
    return;
  }

  for (y = 0; y < h; y++) {
    CopyMem (&crop[y * w], &S->Pixels[(y0 + y) * S->Width + x0], w * sizeof (*crop));
  }

  BootLogo2->SetBootLogo (BootLogo2, crop, (INTN)x0, (INTN)y0, w, h);
  FreePool (crop);
}

EFI_STATUS
EFIAPI
DisplayBootGraphic (
  BOOT_GRAPHIC  Graphic
  )
{
  EFI_STATUS                     Status;
  UINTN                          Height, Width, SizeOfX, SizeOfY, BltSize, ImageSize, BandTop;
  INTN                           DestX, DestY;
  UINT8                          *ImageData = NULL;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Blt       = NULL;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Black;
  EFI_GRAPHICS_OUTPUT_PROTOCOL   *Gop;
  EDKII_BOOT_LOGO2_PROTOCOL      *BootLogo2 = NULL;
  TOPAZ_SPLASH                   *Splash    = NULL;
  BOOLEAN                        UseSplash;

  Status = gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID **)&Gop);
  if (EFI_ERROR (Status)) {
    Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (EFI_ERROR (gBS->LocateProtocol (&gEdkiiBootLogo2ProtocolGuid, NULL, (VOID **)&BootLogo2))) {
    BootLogo2 = NULL;
  }

  if (gST->ConOut != NULL) {
    gST->ConOut->EnableCursor (gST->ConOut, FALSE);
  }

  SizeOfX = Gop->Mode->Info->HorizontalResolution;
  SizeOfY = Gop->Mode->Info->VerticalResolution;
  BandTop = SizeOfY * BAND_TOP_PCT / 100;

  if (EFI_ERROR (gBS->LocateProtocol (&mSplashGuid, NULL, (VOID **)&Splash))) {
    Splash = NULL;
  }

  UseSplash = (Graphic == BG_SYSTEM_LOGO) && (Splash != NULL) && (Splash->Lit != 0) &&
              (Splash->Width == SizeOfX) && (Splash->Height == SizeOfY);

  ZeroMem (&Black, sizeof (Black));
  if (UseSplash) {
    Gop->Blt (Gop, Splash->Pixels, EfiBltBufferToVideo, 0, 0, 0, 0, SizeOfX, BandTop, SizeOfX * sizeof (*Blt));
    Gop->Blt (Gop, &Black, EfiBltVideoFill, 0, 0, 0, BandTop, SizeOfX, SizeOfY - BandTop, 0);
  } else {
    Gop->Blt (Gop, &Black, EfiBltVideoFill, 0, 0, 0, 0, SizeOfX, SizeOfY, 0);
  }

  Status = GetBootGraphic (Graphic, &ImageSize, &ImageData);
  if (!EFI_ERROR (Status)) {
    Status = TranslateBmpToGopBlt (ImageData, ImageSize, &Blt, &BltSize, &Height, &Width);
  }

  if (EFI_ERROR (Status) || (Width > SizeOfX) || (Height + LOGO_BOTTOM_GAP > SizeOfY)) {
    DEBUG ((DEBUG_ERROR, "%a: logo %r\n", __FUNCTION__, Status));
    goto CleanUp;
  }

  DestX = (SizeOfX - Width) / 2;
  DestY = (Graphic == BG_SYSTEM_LOGO) ? (INTN)(SizeOfY - LOGO_BOTTOM_GAP - Height) : (INTN)(SizeOfY - Height) / 2;
  Status = Gop->Blt (Gop, Blt, EfiBltBufferToVideo, 0, 0, (UINTN)DestX, (UINTN)DestY, Width, Height, Width * sizeof (*Blt));

  if ((Graphic == BG_SYSTEM_LOGO) && (BootLogo2 != NULL)) {
    if (UseSplash) {
      SetBgrtFromSplash (BootLogo2, Splash, BandTop);
    } else {
      BootLogo2->SetBootLogo (BootLogo2, Blt, DestX, DestY, Width, Height);
    }
  }

  EfiEventGroupSignal (&gLogoDisplayedEventGroup);
  Status = EFI_SUCCESS;

CleanUp:
  if (Blt != NULL) {
    FreePool (Blt);
  }

  if (ImageData != NULL) {
    FreePool (ImageData);
  }

  return Status;
}
