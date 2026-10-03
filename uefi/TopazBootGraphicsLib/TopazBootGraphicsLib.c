/** @file
  tapas BootGraphicsLib (replaces MsGraphicsPkg BootGraphicsLib, which clears the screen and
  centers the Silicium logo).

  Same picture as upstream (black screen, logo centered, logo = BGRT image), plus one change:
  the console is switched to its largest text mode first, so the fallback text boot menu of
  TopazOtgDxe sits at the top of the screen instead of on the logo (GraphicsConsole centers
  the default 80x25 area).

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

/* Decode a BMP and Blt it centered on the screen. */
STATIC EFI_STATUS
DrawBmp (
  IN  EFI_GRAPHICS_OUTPUT_PROTOCOL   *Gop,
  IN  UINT8                          *Bmp,
  IN  UINTN                          BmpSize,
  OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL  **BltOut,
  OUT UINTN                          *DestX,
  OUT UINTN                          *DestY,
  OUT UINTN                          *W,
  OUT UINTN                          *H
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
  if ((Width > SizeOfX) || (Height > SizeOfY)) {
    FreePool (Blt);
    return EFI_BAD_BUFFER_SIZE;
  }

  Dx = (SizeOfX - Width) / 2;
  Dy = (SizeOfY - Height) / 2;
  Status = Gop->Blt (Gop, Blt, EfiBltBufferToVideo, 0, 0, Dx, Dy, Width, Height, Width * sizeof (*Blt));

  *BltOut = Blt;
  *DestX  = Dx;
  *DestY  = Dy;
  *W      = Width;
  *H      = Height;

  return Status;
}

EFI_STATUS
EFIAPI
DisplayBootGraphic (
  BOOT_GRAPHIC  Graphic
  )
{
  EFI_STATUS                     Status;
  UINTN                          ImageSize, Dx, Dy, W, H;
  UINT8                          *ImageData = NULL;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Blt       = NULL;
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
    /* largest text mode = full screen, so the text menu starts at the top (SetMode clears) */
    UINTN  Mode, Cols, Rows, Best = (UINTN)gST->ConOut->Mode->Mode, BestCells = 0;

    for (Mode = 0; Mode < (UINTN)gST->ConOut->Mode->MaxMode; Mode++) {
      if (!EFI_ERROR (gST->ConOut->QueryMode (gST->ConOut, Mode, &Cols, &Rows)) && (Cols * Rows > BestCells)) {
        Best      = Mode;
        BestCells = Cols * Rows;
      }
    }

    if (Best != (UINTN)gST->ConOut->Mode->Mode) {
      gST->ConOut->SetMode (gST->ConOut, Best);
    }

    gST->ConOut->EnableCursor (gST->ConOut, FALSE);
  }

  ZeroMem (&Black, sizeof (Black));
  Gop->Blt (Gop, &Black, EfiBltVideoFill, 0, 0, 0, 0, Gop->Mode->Info->HorizontalResolution,
            Gop->Mode->Info->VerticalResolution, 0);

  Status = GetBootGraphic (Graphic, &ImageSize, &ImageData);
  if (!EFI_ERROR (Status)) {
    Status = DrawBmp (Gop, ImageData, ImageSize, &Blt, &Dx, &Dy, &W, &H);
  }

  if (!EFI_ERROR (Status)) {
    if ((Graphic == BG_SYSTEM_LOGO) && (BootLogo2 != NULL)) {
      BootLogo2->SetBootLogo (BootLogo2, Blt, (INTN)Dx, (INTN)Dy, W, H);   /* BGRT */
    }

    EfiEventGroupSignal (&gLogoDisplayedEventGroup);
  }

  if (Blt != NULL) {
    FreePool (Blt);
  }

  if (ImageData != NULL) {
    FreePool (ImageData);
  }

  return Status;
}
