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
cp "$HERE"/TopazOtgDxe/TopazOtgDxe.{c,inf} Platforms/Xiaomi/tapasPkg/Drivers/TopazOtgDxe/

A=Silicium-ACPI/Platforms/Xiaomi/tapas
[ -f $A/DSDT.aml.orig ] || cp $A/DSDT.aml $A/DSDT.aml.orig
cp "$HERE/acpi/tapas-DSDT-xhci.aml" $A/DSDT.aml             # URS0 -> plain PNP0D10 XHCI

# DXE.inc / APRIORI.inc must have the Qualcomm USB DXEs enabled (upstream default; do NOT use mu-nousb.diff)
if grep -q '^#NOUSB' Platforms/Xiaomi/tapasPkg/Include/DXE.inc; then
    echo "DXE.inc has USB disabled (#NOUSB) - restore it first" >&2; exit 1
fi

export CLANGPDB_AARCH64_PREFIX=${CLANGPDB_AARCH64_PREFIX:-aarch64-linux-gnu-}
python3 build_uefi.py -d tapas -r "$TARGET"
"$HERE/mu_v4pack.sh" Mu-tapas.img "$OUT"
