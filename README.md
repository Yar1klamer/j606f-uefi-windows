# UEFI и Windows on ARM для Lenovo Tab P11 (TB-J606F)

> 🇬🇧 English version of the documentation: the [`english`](../../tree/english) branch (пока не создана — см. [STATE.md](STATE.md)).

Порт UEFI-окружения **[Project Mu](https://microsoft.github.io/mu/)** на основе
**[Project-Silicium/Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium)** (платформа Qualcomm Kamorta / SM6115, шаблон устройства — Redmi 9T «lime»)
на планшет **Lenovo Tab P11 TB-J606F** (Wi-Fi) и запуск на нём **Windows 11 ARM64**. Тот же планшет можно загрузить в [postmarketOS](https://github.com/Yar1klamer/j606f-postmarketos): UEFI-меню выбирает Windows или Linux.

> ⚠️ **Статус: экспериментально.** Windows 11 Pro ARM64 (сборка 22621) ставится из установленной postmarketOS без ПК и грузится до рабочего стола.
> Это не повседневная система: нет звука, Wi-Fi, Bluetooth, ускорения GPU, USB-host, камер и датчиков. Подробнее — [STATE.md](STATE.md).

**Автор порта — Yar1klamer ([@nnshould](https://t.me/nnshould), Telegram).** Вопросы — туда.

---

## Содержание
- [Предупреждения](#предупреждения)
- [Устройство](#устройство)
- [Что работает и что нет](#что-работает-и-что-нет)
- [Как устроено](#как-устроено)
- [Структура репозитория](#структура-репозитория)
- [Сборка UEFI](#сборка-uefi)
- [Меню выбора ОС (J606fBoot)](#меню-выбора-ос-j606fboot)
- [Установка Windows](#установка-windows)
- [Драйверы Windows](#драйверы-windows)
- [Версии UEFI](#версии-uefi)
- [Благодарности и лицензии](#благодарности-и-лицензии)

## Предупреждения
- **Не записывайте в `modem*`, `persist`, `fsg`, `modemst*`, `abl`, `xbl`, `tz`, `hyp`, `devcfg`, `keymaster`** и прочие загрузочные и калибровочные разделы.
  UEFI грузится через `fastboot boot` (только в RAM, ничего не пишет) или из `boot_a`. Прошивки загрузчика Lenovo не меняются.
- Перед любым изменением разметки UFS сделайте бэкап GPT и `userdata` (см. [docs/INSTALL_WINDOWS.md](docs/INSTALL_WINDOWS.md)).
- Windows работает в режиме **тестовой подписи** (`testsigning on`, `nointegritychecks on`). Драйверы подписываются **вашим** тестовым сертификатом,
  который создаёт `windrv/build.sh`. Ключ и сертификат в репозитории не публикуются.
- В репозитории **нет** бинарников Lenovo и Qualcomm (DXE из `xbl`, DTB, прошивка тача), образов Windows, WinPE и UUP, готовых образов UEFI и `.sys`.
  Всё это берётся со своего планшета и своей установки — см. [docs/EXTRACT_BINARIES.md](docs/EXTRACT_BINARIES.md).
- **Windows 11 24H2 и новее на SD662 (ARMv8.0) не стартуют**: нужна 22H2 или 23H2 (сборка 22621 или 22631).

## Устройство

| | |
|---|---|
| Модель | Lenovo Tab P11, TB-J606F (Wi-Fi, без LTE), 2020 |
| SoC | Qualcomm SM6115P «bengalp» (Snapdragon 662), soc_id 445 |
| RAM / память | 4 или 6 ГБ / UFS 64 или 128 ГБ (проверено на 6/128) |
| Экран | 11", 2000×1200, BOE + Himax HX83102P (проверено); ревизия с Novatek NT36523W не проверялась |
| Тач | Himax HX83102E (zero-flash, SPI) |
| Батарея и зарядка | 7500 мА·ч, fuel gauge MM8013, зарядник PMI632 + SMB1355 |

## Что работает и что нет

Проверено на Windows 11 Pro ARM64 22621 (ru-RU), установленной на внутреннюю память.

| Подсистема | Статус | Примечание |
|---|---|---|
| UEFI (Mu-Silicium, j606f) | ✅ | v7 в `boot_a`; `fastboot boot` для проб |
| Меню Windows / Linux | ✅ | см. [Меню выбора ОС](#меню-выбора-ос-j606fboot) |
| UFS (внутренняя память) | ✅ | inbox-драйвер `storufs` (ACPI `QCOM24A5`) |
| Рабочий стол Windows | ✅ | на кадровом буфере UEFI (Basic Display); OOBE проходит через `unattend.xml` |
| Батарея, адаптер, статус зарядки | ✅ | ACPI (BAT0 и ADP0) и inbox `cmbatt`; напряжение и состояние читаются из PMI632 по SPMI |
| Зарядка | ✅ | сторожевой таймер зарядника SMB5 выключен (`EnableChargerWdog = 0`) |
| Тачскрин | ✅ | свой HID-драйвер: 10 касаний, работает и на экране входа; **перо не сделано** |
| Кнопка питания | ✅ | свой HID-драйвер, опрос PMIC по SPMI |
| Кнопки громкости | 🟡 | отчёты уходят в Windows; звука (аудиодрайвера) нет, индикатора громкости не будет |
| Яркость экрана | 🚧 | яркость панели из Windows по DSI (DCS 0x51) доказана; свой дисплейный драйвер в работе, пока не запускается — см. [Драйверы Windows](#драйверы-windows) |
| Звук, Wi-Fi, Bluetooth, GPU-ускорение | ❌ | драйверов для SM6115 в Windows нет |
| USB (host) | ❌ | |
| Камеры, датчики | ❌ | |
| Часы (RTC) | ❌ | часы Windows стоят на 2022 году |
| Сон, частоты CPU | ❌ | |

## Как устроено
- **UEFI** — пакет `uefi/Platforms/Lenovo/j606fPkg` для Mu-Silicium (копия `limePkg` Redmi 9T, тот же SoC; карта памяти и набор DXE
  совпали со стоковым загрузчиком J606F). Драйверы Qualcomm (`Binaries/j606f`) берутся **из своего** `xbl_a`, с теми же патчами, что Silicium
  применяет для lime (`scripts/patch_drivers.py`). ACPI — DSDT Kamorta из Silicium и свой SSDT (`acpi/`).
- **Меню** `J606fBoot` — отдельное UEFI-приложение в ESP. Оно выбирает Windows (через свой `bootmgfw`) или Linux (ядро из раздела загрузки второй ОС через EFI stub).
- **Windows ставится на устройство без ПК**: разметка, `wimlib-imagex apply`, загрузка WinPE через UEFI, `bcdboot`, `SanPolicy=4` для защиты прочих разделов.
- **Драйверы Windows** собираются `llvm-mingw` без WDK, подписываются тестовым сертификатом и ставятся без INF (через `Service` в реестре).

## Структура репозитория
```
uefi/
  Platforms/Lenovo/j606fPkg/    пакет устройства: DSC/FDF, карта памяти, список DXE, Application/J606fBoot (меню)
  Resources/Configs/            j606f.toml (формат boot-образа)
  Silicium-ACPI/…/j606f/        SSDT устройства (v6: тач, устройство TSC0)
  patches/                      диагностические правки Silicium (лог в ramoops, Mass Storage без ОС)
acpi/                           ACPI: батарея по SPMI (полная и компактная), история SSDT v4…v6
windrv/                         свои драйверы Windows ARM64: hidtouch, hidbuttons, dsiprobe, j606fdsp (в работе), probe, touch, agent
scripts/                        разбор xbl (fv_extract), Binaries/j606f (gen_binaries, patch_drivers), правки Silicium,
                                ужатие разметки под Windows, выкладка файлов в Windows, WinPE (bcdboot), unattend.xml
docs/                           инструкции по сборке, установке и драйверам; docs/notes/ — заметки по порту
```
Каталог `windrv/` — рабочий: в нём есть устаревшие пробные драйверы (`touch`, `probe`), они нужны для истории.

## Сборка UEFI
Кратко — [docs/BUILD_UEFI.md](docs/BUILD_UEFI.md). Нужны Linux x86_64, клон Mu-Silicium, драйверы Qualcomm со своего `xbl_a`:
```bash
python3 build_uefi.py -d j606f -r RELEASE    # → Mu-j606f.img (меню J606fBoot собирается в этом же дереве)
fastboot boot Mu-j606f.img                   # только RAM, ничего не пишет
```
Строка `FVMAIN` в выводе сборки должна показывать `0x521000`: при `0x522000` образ не грузится.

## Меню выбора ОС (J606fBoot)
Меню — UEFI-приложение в ESP (FAT32, раздел `esp`), которое загружается вместо `bootaa64.efi`.

**Что положить:**
1. Сохранить исходный загрузчик Windows из ESP: `EFI/Boot/bootaa64.efi` → `EFI/Boot/bootaa64.windows.efi` (это `bootmgfw`).
2. Скопировать собранный `J606fBoot.efi` в `EFI/Boot/bootaa64.efi`. Файл появляется в каталоге сборки UEFI после `build_uefi.py` (найти: `find Build -name J606fBoot.efi`).
3. Если нужен другой выбор по умолчанию, в `EFI/Boot/j606fboot.def` записать `1` (Linux) или `0` (Windows).

**Управление:** 5 секунд ждать; Громкость + и − — выбор; Питание — загрузка. Последний выбор запоминается в `j606fboot.def`.

**Откат:** вернуть `bootaa64.windows.efi` в `bootaa64.efi` (тогда ESP снова грузит Windows напрямую, меню не будет).

## Установка Windows
Из установленной Linux на том же UFS, без ПК: [docs/INSTALL_WINDOWS.md](docs/INSTALL_WINDOWS.md) — разметка, wimlib, WinPE и `bcdboot`, `SanPolicy=4`, `unattend.xml`.
Вариант с ПК на Windows через Mass Storage UEFI описан там же, на J606F он не проверялся.

Образ Windows — свой (ISO или UUP-сборка). В репозитории образов нет.

## Драйверы Windows
Все драйверы — свои, с тестовой подписью. Общая сборка: `windrv/build.sh <каталог>` (llvm-mingw, без WDK).
- **`hidtouch`** — HID-минидрайвер тача (Touch Screen, 10 касаний) для устройства ACPI `JHMX0001`; нужна прошивка HX83102E со своего планшета (`C:\j606f\drv\hxfw.bin`).
- **`hidbuttons`** — кнопка Питания и громкости, опрос PMIC по SPMI, HID Consumer и System Control.
- **`dsiprobe`** — проба: яркость панели по DSI (DCS 0x51) из Windows.
- **`j606fdsp`** — **в работе, не работает**: свой display-only драйвер (вариант KMDOD) с ползунком яркости.
  - В репозиторий входят только свои файлы: `j606f_bl.cxx`, `patch_kmdod.sh`, `build_dsp.sh`.
  - Пример Microsoft KMDOD (MS-PL), WDK и SDK **не входят**. Скачайте их сами: пример — из репозитория `Windows-driver-samples`, каталог `video/KMDOD`; WDK — с сайта Microsoft.
  - `patch_kmdod.sh` вставляет короткие куски в файлы примера (MS-PL), сам пример не распространяется.
- **`probe`**, **`touch`**, **`agent`**, **`ldtest`**, **`hello`** — диагностика и служба-агент для тестов без ввода.

Драйвер тача (`touch`, `hidtouch`) и кнопок — см. [лицензии](#благодарности-и-лицензии).

## Версии UEFI

| Версия | Что изменилось | Статус |
|---|---|---|
| v3 | первая RELEASE для Windows | проверена |
| v4 | `EnableChargerWdog = 0` | зарядка как в Linux |
| v5d | батарея по SPMI в AML | проверена, 98–100 % |
| v6 | устройство тача `TSC0` (`JHMX0001`) в SSDT | тач работает |
| v7 | карта памяти: резерв reserved-memory для GPU и ADSP Linux; меню J606fBoot | в `boot_a`, проверена |

## Благодарности и лицензии
- [Project-Silicium/Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) — UEFI для Snapdragon, платформа KamortaPkg и устройство lime; [Project Mu](https://microsoft.github.io/mu/) и [EDK II](https://github.com/tianocore/edk2).
- [Project Aloha](https://github.com/Project-Aloha/mu_aloha_platforms) — UEFI для Qualcomm-устройств.
- [Renegade Project](https://renegade-project.tech) — гайды по установке Windows на Snapdragon.
- Сведения о железе — из исходников Linux для SM6115 (`sm61x5-mainline`) и публичного порта Linux на этот планшет.
- Пример Microsoft KMDOD — [Windows-driver-samples](https://github.com/microsoft/Windows-driver-samples), лицензия MS-PL; не входит в репозиторий.

Лицензии по частям — в [LICENSE](LICENSE). Коротко:
- `uefi/` — BSD-2-Clause-Patent;
- `windrv/j606fdsp/` — MIT; сам пример KMDOD (MS-PL) не входит, файлы патча вставляются в него на вашей машине;
- `windrv/touch/` и `windrv/hidtouch/` — GPL-2.0-only;
- остальное (`acpi/`, прочий `windrv/`, `scripts/`, `docs/`) — MIT.

Windows, WinPE, драйверы Qualcomm и Lenovo, прошивка тача — собственность их владельцев, в репозитории их нет.
