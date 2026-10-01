#!/bin/bash
# Switch the default OS of the phone (run on the build laptop, phone in fastboot or Android/TWRP).
#   topaz-boot.sh android   -> slot a (HyperOS, TWRP in recovery_a), stays until switched back
#   topaz-boot.sh windows   -> slot b (boot_b = Mu UEFI -> Windows), the default
# Uses only ABL's own `fastboot set_active` (swaps partition type GUIDs + attributes together).
# Never switch slots by editing GPT attributes alone - see docs/AGENT_BRIEF_drivers.md 2c.
set -e
case "$1" in
  android) SLOT=a ;;
  windows) SLOT=b ;;
  *) echo "usage: $0 android|windows" >&2; exit 1 ;;
esac
if ! fastboot devices | grep -q fastboot; then
  adb reboot bootloader
  for i in $(seq 1 60); do fastboot devices | grep -q fastboot && break; sleep 1; done
fi
[ "$(fastboot getvar is-userspace 2>&1 | awk '/is-userspace/{print $2}')" = "no" ] || { echo "not in bootloader fastboot" >&2; exit 1; }
fastboot set_active $SLOT
fastboot reboot
