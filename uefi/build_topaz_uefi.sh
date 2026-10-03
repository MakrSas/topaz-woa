#!/bin/bash
# Build the working topaz UEFI (USB host + Windows boot) from a Mu-Silicium checkout.
# Run on the build laptop:  uefi/build_topaz_uefi.sh ~/work/Mu-Silicium out.img [RELEASE|DEBUG]
# Known-good pins: Mu-Silicium e5f9ec06, Mu_Basecore bb557081, Silicium-ACPI 9abca7f9, Binaries 203fb36b.
# Result is boot header v4; test with `fastboot boot out.img` (never flash untested images).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
MU=${1:?Mu-Silicium path}; OUT=${2:?output image}; TARGET=${3:-RELEASE}
cd "$MU"

apply() { # repo-relative patch, git dir
    if git -C "$2" apply --reverse --check "$1" 2>/dev/null; then echo "already applied: $1"
    else git -C "$2" apply "$1" && echo "applied: $1"; fi
}
apply "$HERE/patches/mu-memorymap-topaz.patch" .           # required: reserved AOP/CDSP/TZ/PIL RAM
apply "$HERE/patches/mu-fat-fixes.patch" Mu_Basecore        # FAT: reject oversized volumes, skip FAT12
apply "$HERE/patches/mu-topazotg.diff" .                    # TopazOtgDxe in dsc + DXE.inc

mkdir -p Platforms/Xiaomi/tapasPkg/Drivers/TopazOtgDxe
cp "$HERE"/TopazOtgDxe/*.{c,h,inf} Platforms/Xiaomi/tapasPkg/Drivers/TopazOtgDxe/
# upstream boot picture + full-screen text mode (TopazBootGraphicsLib)
mkdir -p Platforms/Xiaomi/tapasPkg/Library/TopazBootGraphicsLib
rm -f Platforms/Xiaomi/tapasPkg/Library/TopazBootGraphicsLib/*.bmp
cp "$HERE"/TopazBootGraphicsLib/*.{c,inf} Platforms/Xiaomi/tapasPkg/Library/TopazBootGraphicsLib/
sed -i '/FILE FREEFORM = 9A4C2E17-5B3D-4F81-A60E-7D12C9483B5F/,/^  }/d' Platforms/Xiaomi/tapasPkg/tapas.fdf   # old MiLogo.bmp
# WinRE-look touch menu bitmaps (uefi/winre/mkwinre.py, needs python3-pil), one FREEFORM file, RAW section order = WinRe.c
W=Platforms/Xiaomi/tapasPkg/Drivers/TopazOtgDxe/winre
rm -rf $W
/usr/bin/python3 "$HERE/winre/mkwinre.py" $W
sed -i '/FILE FREEFORM = 5D0B7A63-2C84-4E19-B73F-910A6E52C428/,/^  }/d' Platforms/Xiaomi/tapasPkg/tapas.fdf   # rebuilt: the asset count changes
secs=$(for f in $W/winre_*.bmp; do printf '\\n    SECTION RAW = tapasPkg/Drivers/TopazOtgDxe/winre/%s' "$(basename $f)"; done)
sed -i "s#^  !include QcomPkg/Extra.fdf.inc#&\\n\\n  FILE FREEFORM = 5D0B7A63-2C84-4E19-B73F-910A6E52C428 {$secs\\n  }#" Platforms/Xiaomi/tapasPkg/tapas.fdf
sed -i '/FILE FREEFORM = 2F6D81C4-7E19-4B3A-9C52-0BE46A17D893/,/^  }/d' Platforms/Xiaomi/tapasPkg/tapas.fdf   # old SiliciumText.bmp
rm -f Platforms/Xiaomi/tapasPkg/Library/TopazBootGraphicsLib/SiliciumText.bmp
grep -q TopazBootGraphicsLib Platforms/Xiaomi/tapasPkg/tapas.dsc ||
    sed -i 's#^  ConfigurationMapLib|.*$#&\n  BootGraphicsLib|tapasPkg/Library/TopazBootGraphicsLib/TopazBootGraphicsLib.inf#' Platforms/Xiaomi/tapasPkg/tapas.dsc

A=Silicium-ACPI/Platforms/Xiaomi/tapas
[ -f $A/DSDT.aml.orig ] || cp $A/DSDT.aml $A/DSDT.aml.orig
cp "$HERE/acpi/tapas-DSDT-xhci.aml" $A/DSDT.aml             # URS0 -> plain PNP0D10 XHCI, GPU0 (GPUE switch)

# DXE.inc / APRIORI.inc must have the Qualcomm USB DXEs enabled (upstream default; do NOT use mu-nousb.diff)
if grep -q '^#NOUSB' Platforms/Xiaomi/tapasPkg/Include/DXE.inc; then
    echo "DXE.inc has USB disabled (#NOUSB) - restore it first" >&2; exit 1
fi

export CLANGPDB_AARCH64_PREFIX=${CLANGPDB_AARCH64_PREFIX:-aarch64-linux-gnu-}
python3 build_uefi.py -d tapas -r "$TARGET"
"$HERE/mu_v4pack.sh" Mu-tapas.img "$OUT"
