# Power notes: charging, sleep, Vol+ (2026-10-01)

Status of the power-related features of the Windows port and the working hypotheses behind them.
Each item is marked **verified** (seen in code, a log or on the phone) or **hypothesis** (not yet
measured). Update this file when a hypothesis is confirmed or refuted.

## 1. Charging

### Hardware (from `docs/AGENT_BRIEF_drivers.md` and the stock DT)
All on QUP0 SE1 I2C (0x4A84000, GPIO 4/5):
- **bq2589x @ 0x6A**: switching charger + OTG boost (VBUS for USB host).
- **rt1711h @ 0x4E**: Type-C port controller (TCPC), INT on GPIO 93. Needed for USB-PD.
- **sm5602 @ 0x71**: fuel gauge.
- **sc8551 / ln8000**: charge pumps (direct charging, the high-power path Xiaomi uses for 33 W).

### What runs today (verified 2026-10-01, TopazBattery v0.7, log `docs/logs/TopazBattery-v0.7.log`)
- **Type-C roles (DRP, no PD)** on rt1711h: toggling -> hub (partner Rd) = source + OTG boost,
  charger (partner Rp) = sink + charging, back to toggling on detach. Verified on the phone:
  hub -> charger -> charger -> hub, each re-plug detected in 1-2 s.
- **rt1711h was in shipping mode after UEFI/Android**: RTCTRL8 (0x9B) read 0x80 (ship_off=0),
  CC_STATUS stayed 00 even with a charger. Writing what Linux `rt1711h_init()` writes
  (RTCTRL8=0x2A, RTCTRL11=0x8F, RTCTRL14=0x0F) made CC work.
- **Charging through bq2589x at 5 V**: ICHG 1984 mA, VREG 4.40 V (nopmi fv-max), IINLIM 2000 mA
  when the source advertises Rp 3.0 A / DCP (65 W USB-C adapter: Rp 3.0 A, BC1.2 VBUS_STAT=5
  "unknown" because D+/D- are routed to the SoC via GPIO66). Measured: VBUS 4.9 V, ICHG ADC
  1.5-1.6 A, ~6 W into the battery, input-limited (IDPM_STAT=1). Before (chip defaults left by
  Android: ICHG 448 mA) it was ~1.7 W.
- Software JEITA from `qcom,nopmi-chg` on the sm5602 NTC (zones in `gauge.c`).
- Charger is a clone: REG14 PN=001 (SY6970 per Linux bq25890 driver), bq25890 register map.
- Every D0 exit sets rt1711h back to Rd/Rd so a charger always gives VBUS with no driver running;
  shutdown/restart also drops OTG.

### USB-PD 9 V (verified 2026-10-02, TopazBattery v0.8, log `docs/logs/TopazBattery-v0.8.log`)
- `drivers/TopazBattery/pd.c`: polled PD 2.0 sink on rt1711h, started when the Type-C state
  machine becomes a sink. Contract 9 V / 2 A in 249 ms after attach.
- 65 W adapter Source_Capabilities (PD 3.0): 5/9/12/15 V 3 A, 20 V 3.25 A, **PPS 3.3-11 V 5 A**.
- Measured at 9 V: VBUS 8.6-8.7 V (charger ADC), ICHG 1.95-2.0 A, ~8.2 W into the battery,
  no longer input-limited (IDPM_STAT 0). Now limited by ICHG = 2 A (bq2589x DTB `charge-current`).
- Charge pump fitted: **ln8000 @0x51** (reg00 = 0x42); sc8551 @0x66 does not answer.

### 33 W path (not done): ln8000 2:1 charge pump + PPS
DTB `ln8000_charger@51` (lionsemi,ln8000-master, IRQ GPIO 84): bat-ovp 4550 mV (alarm 4525),
bus-ovp 13000 mV (alarm 11000), bus-ocp 3750 mA (alarm 3500), tdie/tbus/tbat monitors disabled.
Vendor driver source: Xiaomi kernels, `drivers/power/supply/lionsemi/ln8000_charger.c`.
Steps: (1) read-only ln8000 register dump against the vendor register map; (2) PD 3.0 + PPS
APDO request with the pump off (verify VBUS tracking, keep-alive Request < 10 s); (3) enable the
pump at a low IBUS target with VBUS = 2*VBAT + margin, closed loop on IBUS/IBAT, all protections
set from the DTB; (4) ramp toward IBAT 5.9 A (JEITA 15-48 C) with thermal limits.

## 2. Sleep (Modern Standby)

### Verified
- `powercfg /a` on the phone (2026-10-01):
  - available: **Standby (S0 Low Power Idle), network connected** = Modern Standby;
  - S1 / S2 / S3: not supported by the firmware, and disabled anyway because S0 idle is supported;
    "Graphics" is also listed as a blocker (basic display driver);
  - hibernation not enabled; hybrid sleep and fast startup unavailable.
- The DSDT (`uefi/acpi/tapas-DSDT-xhci.dsl`) has per-CPU `_LPI` idle states (processor containers
  and ACPI0007 processors), so CPU idle states are described.

### Hypotheses
- There is **no PEP** (Qualcomm platform extension plug-in) in this port, so the platform cannot
  reach DRIPS (deepest runtime idle). "Sleep" most likely means display off with the SoC awake, and
  battery drain close to an idle, screen-off desktop.
- Check: put the phone to sleep with the power button for 30-60 min, wake it, then run
  `powercfg /sleepstudy` (HTML report: % time in DRIPS and the top blockers).

### TopazModem and sleep (verified from code)
- `TopazEvtD0Exit` stops the modem polling thread. `TopazEvtD0Entry` does not restart it
  (`g_Started`). The modem itself keeps running, so after a D0 exit there is no GLINK / rmtfs / WLAN
  service until a reboot.
- The polling loop wakes every 1 ms and keeps a CPU out of deep idle.
- Whether Modern Standby actually takes this root-enumerated device out of D0 is unverified.
- Until this is redesigned (interrupts instead of polling, resume path): **do not let the phone
  sleep while testing the modem.**

## 3. Volume up key

### Verified
- `TopazButtons` reads only PON INT_RT_STS (SID 0, 0x810): KPDPWR = Power, RESIN = Vol-. Vol+ is
  not read at all; its HID usage (Consumer Volume Increment) is already in the report descriptor.
- Stock DT (`~/work/topaz/backup/fdt.dts`): `gpio_keys/vol_up` = `<&pm6125_gpios 5 GPIO_ACTIVE_LOW>`,
  key code 115, wakeup; pinctrl `key_vol_up_default`: gpio5, function normal, input-enable,
  bias-pull-up. PM6125 GPIO controller `pinctrl@c000` -> GPIO5 = peripheral 0xC4 (SID 0).
- Not related to the missing speaker driver.

### Plan (next TopazButtons change)
Find the observer channel for PPID 0x0C4 like PON. Read the GPIO real-time status (pinctrl-spmi-gpio
`PMIC_GPIO_REG_RT_STS`-style register; confirm the offset for this GPIO subtype first). Pressed =
level low. Send Volume Increment.

Open question: is GPIO5 already configured as an input with pull-up by the UEFI/XBL? If the readout
never changes, it needs the pinctrl setup, which is a PMIC write. PMIC writes from apps are not
always allowed (PON writes hang), so check ownership before trying.
