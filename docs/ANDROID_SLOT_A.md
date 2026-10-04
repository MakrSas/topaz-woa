# Flashing your own Android ROM to slot A (without breaking Windows)

The phone dual-boots by A/B slot: **slot A = Android, slot B = Mu-Silicium UEFI -> Windows**.
Any Android ROM can go to slot A as long as the rules below are kept.

## Storage layout (UFS LUN sda, 4 KiB sectors, from `~/work/topaz/gpt_backup_20261001`)

| Partition | Size | Owner |
|---|---|---|
| `boot_a`, `init_boot_a`, `vendor_boot_a`, `dtbo_a`, `vbmeta_a`, `vbmeta_system_a` | small | Android (slot A) |
| `super` | 7 GiB | Android (dynamic partitions: system, vendor, product, ...) |
| `userdata` | 80 GiB | Android data |
| `boot_b` | - | **Mu-Silicium UEFI** (Windows boot) |
| `esp` | 0.5 GiB | Windows boot (EFI system partition) |
| `win` | 146 GiB | Windows (NTFS) |

`esp` and `win` sit after `userdata` in the GPT; formatting `userdata` does not touch them.

## Allowed

- Slot A images, **always with the `_a` suffix**:
  `fastboot flash boot_a boot.img`, `init_boot_a`, `vendor_boot_a`, `dtbo_a`, `vbmeta_a`, `vbmeta_system_a`.
- `fastboot flash super super.img` (one super for both slots; Windows does not use it).
- Wiping Android data: `fastboot -w` / formatting `userdata`.
- Switching OS from fastboot:
  `fastboot --set-active=a` + `fastboot reboot` -> Android,
  `fastboot --set-active=b` + `fastboot reboot` -> Windows.
  (Only through `--set-active`: hand-editing GPT slot attributes broke slot A once, see the GPT incident notes.)

## Never

- `fastboot flash boot ...` **without a suffix** - if slot B is active it overwrites the UEFI in `boot_b`.
- The partition table: `fastboot flash partition ...`, `gpt_*.bin`, vendor `flash_all` scripts that write it.
- Firmware from the ROM's `fw/` folder (xbl, abl, ...): the UEFI depends on the current ABL
  (BOOT.XF.4.1-00361, header-v4 behaviour); another ABL may stop booting it.
- `fastboot erase` / `format` of `esp` or `win`.
- Android OTA updates: they install into the inactive slot = slot B = the UEFI. Disable system updates
  in your ROM (or never accept them).

## Recovery references

- Slot A backups (sha256-verified 2026-10-01): `~/work/topaz/backup_20261001/`
  (`boot_a`, `init_boot_a`, `vendor_boot_a`, `dtbo_a`, `vbmeta_a`, `recovery_a`).
- UEFI for `boot_b`: `Mu-topaz-v10-RELEASE.img` (see `docs/` UEFI notes), rollback image `~/work/topaz-poweroff.img`.
- GPT backups: `~/work/topaz/gpt_backup_20261001/`, last-resort restore that worked:
  `fastboot flash partition:4 ~/work/topaz/gpt_both4_20260930.bin` (only if the GPT is really broken).
- Current state (2026-10-04): `init_boot_a` holds a Magisk-patched copy (used for stock audio dumps);
  flashing your ROM's `init_boot_a` replaces it.

Before flashing a new ROM package, check its flash script for the "Never" items (or send it for review).
