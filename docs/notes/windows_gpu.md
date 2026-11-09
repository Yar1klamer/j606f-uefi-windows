# GPU Adreno 610 и вывод изображения в Windows

Почему в Windows нет ускорения GPU и что возможно вместо него. Раздел 5 — текущее состояние работы над дисплейным драйвером.

## 1. Что работает сейчас
- Windows рисует через **Microsoft Basic Display Driver** (`ROOT\BasicDisplay\0000`): кадровый буфер GOP из UEFI, 1200×2000.
- 3D-графика идёт через WARP (программная отрисовка на CPU). Смены режима и управления яркостью нет.
- Яркость остаётся такой, какую выставил загрузчик. В pmOS яркость задаётся DCS `0x51` (12 бит) по DSI в TCON панели;
  подсветку включает SGM37604A через gpio6 PMI632.

## 2. Существующие драйверы Adreno в Windows (qcdx*)
| Пакет | SoC | GPU | Где встречается |
|---|---|---|---|
| qcdx850 | SDM850 (≈ SDM845) | A630 | Lenovo Yoga C630, Samsung Galaxy Book 2 |
| qcdx8180 | SC8180X (8cx) | A680 | Surface Pro X, Galaxy Book S |
| qcdx7180 | SC7180 (7c) | A618 | ноутбуки на 7c |
| qcdx8150 / qcdx8350 | SM8150 / SM8350 | A640 / A660 | Surface Duo, WOA-Project (windows_qcom_platforms) |
| qcdx8280, qcdx8380 | SC8280XP, X Elite | A690, X1 | Snapdragon X, официально от Qualcomm |

Для **SM6115 / SM6125 / A610 / A612** драйвера Windows нет ни у Qualcomm, ни в сообществе WoA. У Mu-Silicium для всех устройств,
где Windows загружается, статус GPU — «нет» (Status.md).

## 3. Почему GPU не заработает
- **PEP (qcpep) и PoFx.** Драйверы qcdx включают GPU через PEP Qualcomm: GDSC, тактирование, голосование за шину.
  PEP рассчитан на **RPMh** (SDM845 и новее). У SM6115 старый **SMD RPM**: наш патч 0009 голосует за DDR через bimc/RPM,
  поэтому чужой PEP не подойдёт.
- **SMMU.** У Adreno свой SMMU (adreno_smmu). Нужен драйвер SMMU Qualcomm и описание в IORT/ACPI, которых в нашем UEFI нет.
- **ACPI-описание GPU.** Нужны ресурсы (регистры `0x5900000`, прерывания, GPUCC `0x5990000`), зависимости от PEP, SMMU и RPM,
  таблица частот. Это писать с нуля, а в FVMAIN свободно около 56 байт (см. [port_plan.md](port_plan.md), раздел 5.1).
- **Микрокод.** `a630_sqe.fw` (общий для A6xx) и zap-шейдер `a610_zap.mbn` через TrustZone. В Windows для этого нужен драйвер
  qcscm/TrEE, которого тоже нет.
- **Таблица чипов.** Идентификатор A610 (6.1.0) не числится ни в одном известном qcdx. Даже с готовыми PEP и SMMU драйвер,
  скорее всего, откажется работать или упадёт в коде GMU (у A610 GMU нет).

**Итог.** Запуск qcdx на A610 означает повторить работу Qualcomm над BSP (PEP для RPM, SMMU, SCM, ACPI) без исходников и с драйвером,
который этот GPU не знает. Шанс близок к нулю, трудоёмкость — месяцы, без гарантии результата.

## 4. Правовая сторона
qcdx, qcpep, qcsmmu и прочие пакеты — проприетарные пакеты Qualcomm и OEM. В публичный репозиторий они не попадают.
Их можно держать только локально и только из законного источника (Windows Update или пакет OEM для купленного устройства).
В репозиторий входят инструкции и собственный код.

## 5. Варианты и выбор
| Вариант | Что даёт | Трудоёмкость | Риск |
|---|---|---|---|
| **A. Свой display-only драйвер (KMDOD)** | ползунок яркости в Windows; «выключить дисплей» гасит подсветку; 3D через WARP | 2–4 сессии | средний: DSI-команды при работающем видеопотоке; при ошибке — чёрный экран до перезагрузки |
| **B. Только яркость без нового дисплейного драйвера** | регулировка яркости кнопками (через `hidbuttons`) без смены Basic Display | 1 сессия | низкий |
| **C. Настоящий GPU (qcdx)** | 3D-ускорение | месяцы, без гарантии | очень высокий — не рекомендуется |

Выбран вариант **A** (решение владельца от 07.11): ползунок яркости в Windows.

## 6. Состояние варианта A (10.11)
- Яркость по DSI из Windows доказана (`dsiprobe`): уровни переключаются, изображение не пропадает.
- Свой драйвер `j606fdsp` — на основе примера Microsoft KMDOD (MS-PL). Свои файлы — `j606f_bl.cxx` (интерфейс яркости и DSI),
  `patch_kmdod.sh` (вставки в пример), `build_dsp.sh` (сборка). Пример и WDK на машину сборки не выкладываются.
- Устройство дисплея — ACPI `JDSP0001` в UEFI v8. Драйвер выводит рабочий стол на встроенной панели, ползунок яркости работает (подтвердил владелец).
- Ограничение: 3D по-прежнему через WARP, аппаратного ускорения GPU нет.

## 7. Источники
- Qualcomm, драйвер Adreno для Windows (только Snapdragon X): https://softwarecenter.qualcomm.com/catalog/item/Windows_Graphics_Driver
- WOA-Project windows_qcom_platforms: https://github.com/WOA-Project/windows_qcom_platforms
- Mu-Silicium, Status.md: https://github.com/Project-Silicium/Mu-Silicium/blob/main/Status.md
- Adreno SMMU в Linux (arm-smmu-qcom): https://www.mail-archive.com/linux-kernel@vger.kernel.org/msg2295889.html
- Пример Microsoft KMDOD: репозиторий Windows-driver-samples, каталог video/KMDOD (MS-PL).
