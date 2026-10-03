/*
 * Xiaomi splash hand-off: TopazOtgDxe copies the framebuffer at its entry point (ABL's splash is
 * still on the panel then; GraphicsConsole clears it later in BDS) and installs this protocol.
 * tapasPkg TopazBootGraphicsLib (BDS) puts the copy back instead of the centered Silicium logo.
 */
#ifndef TOPAZ_SPLASH_H_
#define TOPAZ_SPLASH_H_

#include <Protocol/GraphicsOutput.h>

#define TOPAZ_SPLASH_PROTOCOL_GUID \
  { 0x6b1f3c52, 0x9a0e, 0x4d7b, { 0x8e, 0x21, 0x4c, 0x5a, 0x90, 0x13, 0xd7, 0x6e } }

#define TOPAZ_FB_BASE    0x5C000000UL        /* "Display Reserved" in MemoryMapLib */
#define TOPAZ_FB_WIDTH   1080
#define TOPAZ_FB_HEIGHT  2400

typedef struct {
  UINT32                         Width;
  UINT32                         Height;
  UINT32                         Lit;        /* pixels brighter than black: 0 = no splash */
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Pixels;    /* Width * Height, row stride = Width */
} TOPAZ_SPLASH;

#endif
