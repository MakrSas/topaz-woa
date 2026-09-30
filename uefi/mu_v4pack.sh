#!/bin/bash
# Repack Mu-Silicium header-v1 image into header-v4 for Xiaomi topaz ABL.
# ABL here silently ignores v1 images; v4 works. Shim is PIC, so text_offset -> 0.
set -e
IN=$1; OUT=$2; T=$(mktemp -d)
python3 /home/makr/work/topaz/mkbootimg_src/unpack_bootimg.py --boot_img "$IN" --out $T >/dev/null
gzip -dc $T/kernel > $T/Image 2>/dev/null || true
python3 - "$T/Image" <<PY
import struct,sys
p=sys.argv[1]; d=bytearray(open(p,"rb").read()); d=d.ljust((len(d)+0xFFF)&~0xFFF,b"\0")
assert d[56:60]==b"ARMd"
struct.pack_into("<Q",d,8,0); struct.pack_into("<Q",d,16,len(d)); struct.pack_into("<Q",d,24,0xA)
open(p,"wb").write(d)
PY
python3 /home/makr/work/topaz/mkbootimg_src/mkbootimg.py --header_version 4 --kernel $T/Image --os_version 13.0.0 --os_patch_level 2025-01 -o "$OUT"
rm -rf $T; ls -l "$OUT"
