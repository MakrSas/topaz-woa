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

### What runs today (verified, `drivers/TopazBattery/gauge.c`)
- Reads the sm5602 gauge (capacity, voltage, temperature, cycles).
- Charger policy: when VBUS is present, sets bq2589x REG03 CHG_CONFIG = 1 and clears OTG_CONFIG
  (adapter -> charge, never boost into a charger). Charging state comes from REG0B CHRG_STAT.
- Nothing else: **no input current limit (IINLIM) or charge current (ICHG) writes, no USB-PD, no
  charge-pump driver.** So fast charging is not implemented.

### What the phone probably does now (hypothesis, not measured)
- bq2589x runs on its power-on defaults plus its own BC1.2 input detection:
  5 V from the adapter, input limit chosen by the detected port type (a PC port ~0.5 A, a DCP
  ~1.5-2 A), charge current at the chip default. Expected **roughly 2.5-10 W**, not 33 W.
- bq25890-class chips can do HVDCP/MaxCharge (QC2-style 9/12 V) on their own if enabled by default;
  Xiaomi's own adapters negotiate PD/PPS or a proprietary protocol, so with them the phone most
  likely stays at 5 V. Unverified.
- Exact defaults must be checked against the bq2589x datasheet, not assumed.

### How to verify
Add a bq2589x ADC readout to the TopazBattery log: start a conversion (REG02 CONV_START / CONV_RATE),
then read battery voltage, VBUS voltage, charge current and the effective input limit (REG0E..REG13
area; take the exact bits from the datasheet). One log line every 5 s while charging from (a) the PC
and (b) the stock adapter answers "how fast does it charge now".

### Path to fast charging (largest to smallest risk last)
1. **Tune bq2589x** (ICHG / IINLIM to sane values within the battery profile): quickest, still 5 V,
   about 10 W at best.
2. **USB-PD sink**: rt1711h TCPC driver + a minimal PD policy engine requesting a 9 V fixed PDO,
   then raise IINLIM: about 18 W through bq2589x. A sizeable piece of work (TCPCI registers, PD
   message layer, timers).
3. **Charge pump + PPS** (sc8551 / ln8000 direct charging): the 33 W path. Biggest and riskiest:
   needs PPS voltage tracking, thermal limits and JEITA handling like Xiaomi's stack.

**Safety:** any change to charge parameters must stay within the battery limits of the stock DT
(sm5602 battery profile, temperature table already used by TopazBattery). Never raise currents
blindly.

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
