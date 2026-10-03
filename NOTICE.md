# Third-party notices

## ntdevlabs/exynos9810-woa (NTDEV) — WinRE-look boot menu

The touch boot menu of the UEFI (`uefi/TopazOtgDxe/WinRe.c`, bitmaps from `uefi/winre/mkwinre.py`)
copies the Windows-Recovery-Environment look of **NTDEV's**
[exynos9810-woa](https://github.com/ntdevlabs/exynos9810-woa) (`tools/twrp-winre`,
commit `6e2cacf15f6b99b198ed12ad242da69eb2259bac`):

- the layout of the *Choose an option* page (tile size, icon / text positions, font sizes,
  text colours) from `tools/twrp-winre/build.py`;
- the icons `winre_ic_continue/recovery/poweroff/back.png` (original artwork of that
  project), copied unchanged to `uefi/winre/third_party/exynos9810-woa/icons/` (the graphics
  card icon is our own drawing in `mkwinre.py`, in the same stroke style);
- the fonts `winre-light/semilight/regular.ttf`, copied unchanged to
  `uefi/winre/third_party/exynos9810-woa/fonts/`.

Code and artwork: Copyright (c) 2026, exynos9810-woa contributors, BSD-2-Clause-Patent —
full text in `uefi/winre/third_party/exynos9810-woa/LICENSE`.

Fonts: compiled by exynos9810-woa from Microsoft's open-source **Selawik**
(https://github.com/microsoft/Selawik), Copyright 2015 Microsoft Corporation, Reserved Font
Name "Selawik", renamed "S9WoA Sans" as Modified Versions. SIL Open Font License 1.1 —
`uefi/winre/third_party/exynos9810-woa/fonts/OFL.txt` (and that folder's README).

No Segoe UI or other Microsoft font files are used.

## Mu-Silicium (Project Silicium)

The UEFI is built from [Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium);
`uefi/TopazBootGraphicsLib` is based on MsGraphicsPkg BootGraphicsLib (Microsoft, Intel,
BSD-2-Clause-Patent).
