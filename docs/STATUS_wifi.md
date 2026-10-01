# Состояние порта Wi-Fi (topaz / Redmi Note 12 4G, SM6225) — 2026-10-01

Один документ со всем, что сделано, что работает и что осталось по Wi-Fi под Windows 11 ARM64.
Связанные файлы: `docs/RESEARCH_wifi.md` (факты и протоколы), `docs/HANDOFF_wifi_windows.md`
(план порта в Windows), `docs/P4_wlan_datapath.md` (путь данных), `docs/HANDOFF_p4_htc_wmi.md`
(бриф по CE/HTC/WMI), логи `docs/logs/`.

## 1. Железо (кратко)
Wi-Fi — **WCN3990** во «встроенном» режиме: радиочип WCN3990, но прошивка WLAN исполняется на
DSP модема (MPSS), а хост общается с ней через copy engines в SoC. Ни PCIe, ни SDIO — стандартный
Windows-драйвер не за что зацепить. Открытого Windows-драйвера под этот чип нет нигде; на ноутбуках
с таким же WCN3990 (Surface Pro X) стоит закрытый бинарник Qualcomm, не переносимый. Единственный
открытый образец — Linux `ath10k` (snoc), его и переносим.

## 2. Что работает (проверено на телефоне)

Драйвер `drivers/TopazModem` (root-enumerated KMDF, один системный поток, опрос). Загружается на
холодном модеме, только если есть `C:\topaz\modem.arm` (создаёт `install.cmd`, драйвер удаляет при
старте — защита от цикла BSOD). Лог `C:\TopazModem.log`.

| Этап | Версия | Результат (лог) |
|---|---|---|
| Загрузка модема через TrustZone PAS | v0.3 | init/mem/auth = 0/0 |
| SMP2P → READY | v0.3 | slave-kernel READY за 0.25 с |
| GLINK/QRTR + pd-mapper/tftp/rmtfs | v0.3 | wlanmdsp.mbn за 0.4 с, 43 службы |
| WLFW QMI (ath10k qmi.c) | v0.3 | **WLAN FIRMWARE READY** за 4.3 с, модем держится 5.5+ мин |
| QMI WLAN_CFG + WLAN_MODE=mission | v0.5 | обе ok, 12 copy engines читаются |
| SMMU: identity-контекст для WLAN | v0.7 | **барьер пройден**, память хоста достижима железом WLAN |

### Разобранные грабли (не повторять)
- v0.1 BSOD: `KeQueryInterruptTimePrecise(NULL)` — out-параметр обязателен.
- v0.2 modem FATAL `xpu lock failed`: буфер rmtfs кончался на 4 ГиБ (32-битный конец заворачивается).
  Все буферы для TZ/железа — ниже `0xF0000000` (`PhysAlloc` в port.c это делает).
- v0.4 тихая перезагрузка: читать регистры CE можно только ПОСЛЕ WLAN_MODE=mission (раньше виснет шина).
- SMMU: глобальный bypass (S2CR type 1) железо принудительно превращает в FAULT. Решение — контекст-банк
  с выключенной трансляцией (SCTLR.M=0 = сквозной пропуск = identity), ровно как делает UEFI для своих
  потоков (cb0..3). Мы клонировали его в cb4 и навели S2CR type 0 на поток 0x1A0. Запись прижилась.

### Ключевые аппаратные факты (сверено по ath10k на s8build `~/work/wifi/linux`)
- WCN3990: target_64bit + shadow_reg_support + rri_on_ddr. chip_id 0x4130, плата → `bdwlan.bin`.
- CE-регистры: физ. `0xC800000 + 0x240000`, CE n на `+0x1000*n`, wrapper на `+0xC000`.
  Смещения: sr_base lo/hi 0x0/0x4, sr_size 0x8, dr_base lo/hi 0xC/0x10, dr_size 0x14, misc_ie 0x34,
  sr_wr_idx 0x3C, dr_wr_idx 0x40, cur_srri 0x44, cur_drri 0x48, ctrl1 0x18 (dmax[15:0], src-bswap bit17,
  dst-bswap bit18, RRI-update bit19 = 0x80000).
- RRI (mirror индексов чтения): база в wrapper `+0x4`(lo)/`+0x8`(hi) = физ. 0x24C004/0x24C008.
- Shadow src-write-idx (из-за shadow_reg): membase `0xC800000 + 0x32000 + 4*ce` для CE 0,3,4,5,7;
  dst-write-idx остаётся на CE+0x40.
- Дескриптор `ce_desc_64` = {u64 addr, u16 nbytes, u16 flags, u32 toeplitz} (16 байт).
- HTC READY (msg id 1: credit_count, credit_size, max_endpoints) приходит на control-DL-пайп = **CE2**
  (по `target_service_to_ce_map_wlan`); кадр = `htc_hdr` (8 байт) + тело.
- SMMU identity: буфер, отданный железу, достижим по своему физ. адресу. Все кольца/буферы — через
  `PhysAlloc()` (непрерывно, <4 ГиБ, write-combined), держать ниже 0xF0000000.

## 3. P4 (путь данных CE → HTC → WMI) — В РАБОТЕ, дерево сейчас НЕ СОБИРАЕТСЯ

Все предпосылки готовы (п.2). Начата реализация версии **v0.8**. На диске (не закоммичено):
изменены `Modem.h`, `Glink.c`, `Wlfw.c`, `driver.c`, `TopazModem.vcxproj`; добавлены untracked
`ce.c`, `htc.c`.

**Выбранный API** (его ждут Modem.h, Glink.c, Wlfw.c, htc.c — это канон):
- `CeStart(VOID)` — после WLAN_MODE: RRI + кольца + приёмные буферы.
- `CePoll(VOID)` — неблокирующий опрос из idle-цикла GLINK (`Glink.c:672`); отданные пакеты → `HtcRx`.
- `CeSend(UINT32 Ce, CONST VOID *Data, UINT32 Len, UINT32 TransferId)`.
- `CeSummary`, `HtcRx(Ce,Data,Len)`, `HtcSummary`, `WlfwSetStep`.
- Интеграция: `Wlfw.c:502` зовёт `CeStart()` после `WLAN ON (mission)`; `Glink.c:672` зовёт `CePoll()`.
- `htc.c` готов и согласован с этим API (HTC-заголовок, connect сервисов, OnReady/OnConnectResp, hexdump).

### ⚠️ Что сломано и как чинить
`drivers/TopazModem/ce.c` **внутренне несогласован и не компилируется** — в нём ДВА черновика:
- строки ~54–252: `CeInit/CeInitSrc/CeInitDst/CeRxPost/CeAllocRri/CeRecv` + `CeSend(Ce, Pa, Nbytes, Id)`
  — **полные рабочие хелперы колец**, но под другой верхний API;
- строки ~254–381: `CeStart/CePoll/CeSummary` + второй `CeSend(Ce, Data, Len, Id)` — **канонический API**,
  но его собственные хелперы были затёрты (ссылается на несуществующие CE_STATE/mRegs/mAttr/InitSrc и т.п.).

Дубль `CeSend` с разными сигнатурами → сборка падает.

**План починки (одним проходом, один исполнитель):** оставить рабочие хелперы верхней половины
(`CeInit`, `CeInitSrc`, `CeInitDst`, `CeRxPost`, `CeAllocRri`, `CeRecv`, глобалы `mCe/mShadow/mRri/mRing/
mRxPool`) и переписать нижнюю так, чтобы:
- `CeStart()` = маппинг CE/shadow + `CeAllocRri()` + `CeInit()`;
- `CePoll()` = по всем dest-кольцам `CeRecv()` → `HtcRx(ce, data, len)`, вернуть TRUE если что-то было;
- оставить ОДИН `CeSend(UINT32 Ce, CONST VOID *Data, UINT32 Len, UINT32 TransferId)` (канон): скопировать
  Data в PhysAlloc-буфер, положить физ. адрес в src-дескриптор, двинуть src-write-idx (shadow для 0,3,4,5,7);
- `CeSummary()` — счётчики.
Удалить второй `CeSend` и неиспользуемые имена. Затем зелёный CI, деплой на флешку, тест на телефоне.

**Первая проверяемая цель:** в логе `HTC READY` с credit_count/credit_size — это доказывает весь путь
(SMMU-банк + кольца + CE send/recv) сквозь. На этом коммит.

### Про «вторую сессию»
Фоновый агент сообщил о «параллельной сессии Claude» — это ложная тревога: он принял процесс этой же
сессии за чужой и так наложил свой черновик `ce.c` поверх своего же. Никакой второй сессии нет. Ничего
не закоммичено и не запушено, телефон/флешка не тронуты. Правит код дальше ОДИН исполнитель.

## 4. Сборка / деплой / тест
- CI: push в `display` → GitHub Actions (`.github/workflows/build.yml`, TopazModem уже внесён).
  `gh run watch <id>`, затем `gh run download <id> -n TopazModem-arm64`.
- Деплой: артефакт + прошивка на exFAT-флешку `/Volumes/Образы/topaz-woa/TopazModem/`,
  `tools/deploy/install-modem.cmd` как `install.cmd` (дерево прошивки на s8build `~/work/wifi/winfw/fw`).
  Пользователь на телефоне: `install.cmd`, затем `copy-log.cmd`, приносит `C:\TopazModem.log`.
- Поднимать `TOPAZ_MODEM_VERSION` в `driver.c` каждую сборку; каждый лог сохранять в `docs/logs/`.
- Один исполнитель на телефон за раз. Разделы/GPT/boot не трогать — это ведущий агент.

## 5. Дальше, по порядку
1. Починить `ce.c`, добиться `HTC READY` (первая цель P4).
2. WMI (wmi-tlv.c): дождаться WMI ready event (MAC, версия), послать WMI init.
3. HTT (htt*.c): RX-кольцо в памяти хоста, TX.
4. Мини-порт WiFiCx/NetAdapter поверх WMI/HTT — самый длинный этап, несколько итераций. Он и даёт
   значок Wi-Fi в Windows.

## 6. Не по Wi-Fi, но рядом (на будущее)
**Акселерометр / авто-поворот.** В Windows реально через SensorsCx. На topaz датчик не на I2C, а за
**ADSP** (`remoteproc-adsp@ab00000`, `qcom,khaje-adsp-pas`), доступ по QMI через SSC
(`qcom,fastrpc-adsp-sensors-pdr`). То есть нужен тот же приём, что для Wi-Fi: загрузить ADSP через PAS
(переиспользуется `ModemPas.c` с другим ID и прошивкой `adsp.mdt`), поднять сенсорную службу SSC по QMI,
отдать поток в SensorsCx. Транспорт (PAS+GLINK+QRTR) уже есть, новое — только протокол SSC и мини-драйвер
сенсора. Делать после Wi-Fi.
