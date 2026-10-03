/** @file
  tapas BootGraphicsLib (replaces MsGraphicsPkg BootGraphicsLib, which clears the screen and
  centers the Silicium logo).

  BG_SYSTEM_LOGO: black screen, the Mi logo (MiLogo.bmp, a FREEFORM file in FvMain) centered
  like the Xiaomi splash, and "Project Silicium" (SiliciumText.bmp; PcdLogoFile if missing) at
  the bottom where the splash says "Powered by Android". The Mi logo is also the BGRT image, so Windows Boot Manager shows it.
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
#include <Library/DxeServicesLib.h>

/* MiLogo.bmp and SiliciumText.bmp, added to FvMain by uefi/build_topaz_uefi.sh */
#define TOPAZ_MI_LOGO_GUID \
  { 0x9a4c2e17, 0x5b3d, 0x4f81, { 0xa6, 0x0e, 0x7d, 0x12, 0xc9, 0x48, 0x3b, 0x5f } }
#define TOPAZ_SILICIUM_TEXT_GUID \
  { 0x2f6d81c4, 0x7e19, 0x4b3a, { 0x9c, 0x52, 0x0b, 0xe4, 0x6a, 0x17, 0xd8, 0x93 } }

#define LOGO_BOTTOM_GAP  150   /* pixels between the Silicium text and the bottom edge */

STATIC EFI_GUID  mMiLogoGuid       = TOPAZ_MI_LOGO_GUID;
STATIC EFI_GUID  mSiliciumTextGuid = TOPAZ_SILICIUM_TEXT_GUID;

/* Decode a BMP and Blt it horizontally centered; BottomGap < 0 = vertically centered, else
   the image ends BottomGap pixels above the bottom edge. */
STATIC EFI_STATUS
DrawBmp (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL   *Gop,
  IN  UINT8                          *Bmp,
  IN  UINTN                          BmpSize,
  IN  INTN                           BottomGap,
  OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL  **BltOut OPTIONAL,
  OUT UINTN                          *DestX OPTIONAL,
  OUT UINTN                          *DestY OPTIONAL,
  OUT UINTN                          *W OPTIONAL,
  OUT UINTN                          *H OPTIONAL
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Blt = NULL;
  UINTN                          BltSize, Width, Height, SizeOfX, SizeOfY, Dx, Dy;
  EFI_STATUS                     Status;

  Status = TranslateBmpToGopBlt (Bmp, BmpSize, &Blt, &BltSize, &Height, &Width);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  SizeOfX = Gop->Mode->Info->HorizontalResolution;
  SizeOfY = Gop->Mode->Info->VerticalResolution;
  if ((Width > SizeOfX) || (Height + MAX (BottomGap, 0) > SizeOfY)) {
    FreePool (Blt);
    return EFI_BAD_BUFFER_SIZE;
  }

  Dx = (SizeOfX - Width) / 2;
  Dy = (BottomGap < 0) ? (SizeOfY - Height) / 2 : SizeOfY - Height - (UINTN)BottomGap;
  Status = Gop->Blt (Gop, Blt, EfiBltBufferToVideo, 0, 0, Dx, Dy, Width, Height, Width * sizeof (*Blt));

  if (BltOut != NULL) {
    *BltOut = Blt;
    *DestX  = Dx;
    *DestY  = Dy;
    *W      = Width;
    *H      = Height;
  } else {
    FreePool (Blt);
  }

  return Status;
}

EFI_STATUS
EFIAPI
DisplayBootGraphic (
  BOOT_GRAPHIC  Graphic
  )
{
  EFI_STATUS                     Status;
  UINTN                          ImageSize, MiSize, SizeOfY, Dx, Dy, W, H;
  UINT8                          *ImageData = NULL;
  UINT8                          *MiData    = NULL;
  UINT8                          *TextData  = NULL;
  UINTN                          TextSize;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *MiBlt     = NULL;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  Black;
  EFI_GRAPHICS_OUTPUT_PROTOCOL   *Gop;
  EDKII_BOOT_LOGO2_PROTOCOL      *BootLogo2 = NULL;

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

  SizeOfY = Gop->Mode->Info->VerticalResolution;
  ZeroMem (&Black, sizeof (Black));
  Gop->Blt (Gop, &Black, EfiBltVideoFill, 0, 0, 0, 0, Gop->Mode->Info->HorizontalResolution, SizeOfY, 0);

  Status = GetBootGraphic (Graphic, &ImageSize, &ImageData);
  if (EFI_ERROR (Status)) {
    goto CleanUp;
  }

  if (Graphic != BG_SYSTEM_LOGO) {
    Status = DrawBmp (Gop, ImageData, ImageSize, -1, NULL, NULL, NULL, NULL, NULL);
    goto CleanUp;
  }

  /* "Project Silicium" at the bottom, where the Xiaomi splash says "Powered by Android" */
  if (!EFI_ERROR (GetSectionFromAnyFv (&mSiliciumTextGuid, EFI_SECTION_RAW, 0, (VOID **)&TextData, &TextSize))) {
    DrawBmp (Gop, TextData, TextSize, LOGO_BOTTOM_GAP, NULL, NULL, NULL, NULL, NULL);
  } else {
    DrawBmp (Gop, ImageData, ImageSize, LOGO_BOTTOM_GAP, NULL, NULL, NULL, NULL, NULL);
  }

  if (!EFI_ERROR (GetSectionFromAnyFv (&mMiLogoGuid, EFI_SECTION_RAW, 0, (VOID **)&MiData, &MiSize))) {
    Status = DrawBmp (Gop, MiData, MiSize, -1, &MiBlt, &Dx, &Dy, &W, &H);
    if (!EFI_ERROR (Status) && (BootLogo2 != NULL)) {
      BootLogo2->SetBootLogo (BootLogo2, MiBlt, (INTN)Dx, (INTN)Dy, W, H);   /* BGRT */
    }
  }

  EfiEventGroupSignal (&gLogoDisplayedEventGroup);
  Status = EFI_SUCCESS;

CleanUp:
  if (MiBlt != NULL) {
    FreePool (MiBlt);
  }

  if (TextData != NULL) {
    FreePool (TextData);
  }

  if (MiData != NULL) {
    FreePool (MiData);
  }

  if (ImageData != NULL) {
    FreePool (ImageData);
  }

  return Status;
}
