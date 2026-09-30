#!/usr/bin/bash
# Fake update_device.sh: topaz phone LUN sda GPT GUIDs (partitions live on the phone, not here)
DISK=39F18FC6-DCC3-7C32-2414-6DCF7D00E9C2
ESP=ABAB9ED5-1754-4030-97D9-81739C8CA67F
WIN=996389A3-450F-4E30-9821-FDD9F242EA3A
endian () { v=$1; i=${#v}; while [ $i -gt 0 ]; do i=$[$i-2]; echo -n ${v:$i:2}; done; echo; }
guid_bytes () { IFS=- read -a a <<< "$1"; echo $(endian ${a[0]})$(endian ${a[1]})$(endian ${a[2]})${a[3]}${a[4]}; }
case "$1" in *esp*) P=$ESP;; *) P=$WIN;; esac
pb=$(guid_bytes $P | sed "s/.\{2\}/&,/g;s/,$//"); db=$(guid_bytes $DISK | sed "s/.\{2\}/&,/g;s/,$//")
printf "hex:3:00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,06,00,00,00,00,00,00,00,48,00,00,00,00,00,00,00,$pb,00,00,00,00,00,00,00,00,$db,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00,00\n"
