# GPU (Adreno 610) и вывод изображения в Windows — исследование (2026-11-07)

Только исследование: на планшет ничего не записывалось. Решение — за владельцем (вопрос в конце).

## Что сейчас
- Windows рисует через **Microsoft Basic Display Driver** (`ROOT\BasicDisplay\0000`, basicdisplay.inf, raw/win_run03/devices_all.txt):
  кадровый буфер GOP из UEFI, без 3D-ускорения (всё рисует процессор через WARP), без управления яркостью, без смены режима.
- Яркость остаётся той, что выставили UEFI/загрузчик. В pmOS яркость = DCS 0x51 (12 бит) по DSI в TCON панели, SGM37604A включён pmi632 gpio6
  (../pmos-j606f/notes/backlight.md).

## 1. Существующие Windows-драйверы Adreno (qcdx*)
| Пакет | SoC | GPU | Где встречается |
|---|---|---|---|
| qcdx850 | SDM850 (≈ SDM845) | A630 | Lenovo Yoga C630, Samsung Galaxy Book 2; Renegade (SDM845-телефоны) |
| qcdx8180 | SC8180X (8cx) | A680 | Surface Pro X, Galaxy Book S |
| qcdx7180 | SC7180 (7c) | A618 | ноутбуки на 7c |
| qcdx8150 / qcdx8350 | SM8150 / SM8350 | A640 / A660 | Surface Duo / Duo 2, WOA-Project QRD/HDK (windows_qcom_platforms) |
| qcdx8280, qcdx8380 … | SC8280XP, X Elite | A690, X1 | Snapdragon X — официально у Qualcomm |

- Для **SM6115 / SM6125 / A610 / A612 Windows-драйвера нет** ни у Qualcomm, ни в WoA-сообществе. У Mu-Silicium для всех устройств, где Windows грузится,
  в статусе GPU стоит ❌ (Status.md).
- Ближайший по архитектуре — A618 (7c, qcdx7180), но у него есть **GMU**, а A610 — GPU без GMU (в Linux — «gmu wrapper»), питание и частоты идут
  через GPUCC + RPM.

## 2. Что нужно qcdx-драйверу и чего у нас нет
- **PEP (qcpep) / PoFx:** все qcdx запускают GPU через PEP Qualcomm: включение GDSC, тактирование, голосование за шину. На SM6115 у нас PEP нет,
  и даже при наличии он рассчитан на **RPMh** (845/850 и новее). У SM6115 старый **SMD RPM** (наш патч 0009 голосует за DDR через bimc/RPM), так что чужой PEP не подходит.
- **SMMU (qcsmmu):** у Adreno свой SMMU (adreno_smmu). В Windows нужен драйвер SMMU от Qualcomm плюс описание в IORT/ACPI. Сейчас в нашем UEFI этого нет.
- **ACPI:** узел GPU с ресурсами (5900000, прерывания, GPUCC 5990000), зависимости от PEP/SMMU/RPM и таблицы частот. Всё это надо писать с нуля,
  а в FVMAIN свободно всего 56 байт.
- **Микрокод:** a630_sqe.fw (общий для A6xx) и zap-шейдер a610_zap.mbn через TrustZone (в Windows — через драйвер qcscm/TrEE, которого тоже нет).
- **Таблица чипов в самом драйвере:** chip id A610 (6.1.0) в известных qcdx не числится. Даже с готовыми PEP/SMMU драйвер, скорее всего, откажется работать
  или сломается на коде для GMU.

**Итог:** запустить qcdx на A610 — это фактически повторить работу Qualcomm над BSP (PEP для RPM, SMMU, SCM, ACPI) без исходников, да ещё с драйвером,
который этот GPU не знает. **Реалистичность — около нуля**, трудоёмкость — месяцы, и без гарантии результата.

## 3. Юридическая сторона
- qcdx*, qcpep, qcsmmu и прочее — проприетарные пакеты Qualcomm/OEM. **Не публикуются никогда.** Их можно держать только локально у владельца
  и только из легального источника (Windows Update, пакет OEM для купленного устройства).
- В публичный репозиторий — только инструкции и свой код (наши драйверы, ASL).

## 4. Варианты
| Вариант | Что даёт | Трудоёмкость | Риск |
|---|---|---|---|
| **A. Свой display-only драйвер (KMDOD)** на основе открытого примера Microsoft «Kernel Mode Display-Only Driver» (MIT): тот же кадровый буфер GOP + интерфейс яркости DXGK | ползунок яркости в Windows (Центр уведомлений / «Параметры»), Power → «выключить дисплей» реально гасит подсветку; 3D по-прежнему через WARP | 2–4 сессии: драйвер (llvm-mingw, displib/dxgkrnl импорты), DCS 0x51 через командный движок MDSS DSI (как в pmOS-драйвере панели), выключение — DCS 0x28/0x10 + pmi632 gpio6 | средний: DSI-команды при работающем видеопотоке UEFI надо отлаживать; при ошибке — чёрный экран до перезагрузки (аварийный выход тот же) |
| **B. Только яркость, без нового дисплейного драйвера:** маленький драйвер/служба, яркость кнопками или командой (например, Громкость± при зажатом Питании, через hidbuttons) | регулировка яркости и выключение подсветки без смены Basic Display | 1 сессия | низкий |
| **C. Настоящий GPU (qcdx)** | 3D-ускорение | месяцы, без гарантии; нужны чужие проприетарные пакеты | очень высокий (см. п. 2) — не рекомендую |

Рекомендация: **A**, если нужен нормальный ползунок яркости в Windows; **B**, если хватит регулировки кнопками. **C** — нет.

## Источники
- Qualcomm, Windows Adreno GPU driver (только Snapdragon X): https://softwarecenter.qualcomm.com/catalog/item/Windows_Graphics_Driver
- WOA-Project windows_qcom_platforms (855/865/888/G3x): https://github.com/WOA-Project/windows_qcom_platforms
- Mu-Silicium Status.md (GPU ❌ на всех Windows-устройствах): https://github.com/Project-Silicium/Mu-Silicium/blob/main/Status.md
- Adreno 630 (qcdx850) для Windows, сборка сообщества: https://github.com/aleksartx7/adreno-630-gpu-driver
- Adreno SMMU в Linux (arm-smmu-qcom): https://www.mail-archive.com/linux-kernel@vger.kernel.org/msg2295889.html
- Пример Microsoft KMDOD: репозиторий Windows-driver-samples, video/KMDOD (лицензия MIT).

## 5. Ход варианта A (решение владельца 2026-11-07: «A: драйвер + ползунок»)
- Яркость по DSI из Windows доказана: dsiprobe.sys, run_22 — 4095/600/100/2000, изображение не пропадает (LOG 07.11).
- WDK 10.0.28000.2526 ARM64 + SDK (nuget.org) и пример KMDOD (Windows-driver-samples, MS-PL) — только на хосте сборки, не публикуются.
  Своё: windrv/j606fdsp/j606f_bl.cxx (интерфейс яркости, DSI), patch_kmdod.sh (правки примера), build_dsp.sh.
- Подключение: ROOT\BasicDisplay\0000 Service = j606fdsp (действует со следующей загрузки), тест в две загрузки, вторая сразу возвращает BasicDisplay.
- run_24 (9fc42082…): DriverEntry, QueryInterface → StartDevice не сообщил успеха — `DxgkCbAcquirePostDisplayOwnership` даёт пустой DispInfo не-BasicDisplay драйверу.
- run_25 (3feea6a9…): при пустом DispInfo — кадровый буфер UEFI вручную (0x5c000000, 1200×2000, pitch 4800, A8R8G8B8) → StartDevice успешен, затем
  ROOT\BasicDisplay\0000 код 43 (CM_PROB_FAILED_POST_START), крутилка. Дальше — журнал во всех DDI (STATE §4 п. 1).
