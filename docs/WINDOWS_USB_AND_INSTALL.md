# Windows on topaz: USB host boot + manual driver install

## Boot with USB (keyboard/mouse/flash via hub)
1. Phone in bootloader fastboot (`fastboot getvar is-userspace` -> `no`), cable to the laptop.
2. `ssh s8build fastboot boot ~/work/win/uefi/Mu-topaz-v4-GOOD-usb-host-20261001.img`
   (= `Mu-topaz-v4-OTG3-RELEASE.img`, sha256 `dba80b96…bfcdfb`; rebuild with `uefi/build_topaz_uefi.sh`)
3. Wait for `>>> UNPLUG PC CABLE, PLUG THE HUB NOW (60 s) <<<` on screen, then swap the
   laptop cable for the hub. `OTG ON` = 5 V on the port; boot continues after 3 s.
   (The charger refuses OTG while the PC powers VBUS, so the swap must happen at this prompt.)
4. Pick Windows in Boot Manager (power button works there).
5. Shut down via Start -> Shut down (keeps NTFS clean). Reboot lands in Android/fastboot:
   the UEFI is RAM-only, start again from step 1.

## Manual driver install (from a flash drive)
Package = CI artifact `TopazTouch-arm64` + `install.cmd` (copied to the flash as `topaz-woa\TopazTouch`).
1. Set the correct date in Windows (it boots at 2022-05-07; certs from before this fix fail with 0x800B0101).
2. Right-click `install.cmd` -> Run as administrator. It adds `topaz-woa-test.cer` to Root +
   TrustedPublisher, removes an old `Root\TopazTouch` and runs `devcon install TopazTouch.inf Root\TopazTouch`.
   Output: `install.log` next to the script.
3. Accept the "install driver from topaz-woa" prompt.
4. Driver log: `C:\TopazTouch.log` (copy it to the flash and hand it back for analysis).

Test signing is already on in BCD.
