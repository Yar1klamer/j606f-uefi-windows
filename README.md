# UEFI и Windows on ARM для Lenovo Tab P11 (TB-J606F)

> 🇬🇧 English version of the documentation: the [`english`](../../tree/english) branch.

Порт UEFI-окружения **[Project Mu](https://microsoft.github.io/mu/)** на основе
**[Project-Silicium/Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium)** (платформа Qualcomm Kamorta / SM6115, шаблон — Redmi 9T «lime»)
на планшет **Lenovo Tab P11 TB-J606F** и запуск на нём **Windows 11 ARM64** — рядом с [postmarketOS](https://github.com/Yar1klamer/j606f-postmarketos).

> ⚠️ **Статус: экспериментально.** Windows 11 Pro ARM64 (22621) устанавливается из pmOS без ПК и грузится до рабочего стола через этот UEFI.
> Работают внутренняя память UFS, вывод на экран (framebuffer от загрузчика), батарея и статус зарядки. **Ввода нет** (тач — в работе,
> USB не работает), нет Wi-Fi, звука, GPU-ускорения. Для повседневного использования это пока непригодно — это задел для разработки драйверов.
> Подробности — в [STATE.md](STATE.md).

**Автор порта — Yar1klamer ([@nnshould](https://t.me/nnshould), Telegram).** Порт postmarketOS на этот же планшет —
[Yar1klamer/j606f-postmarketos](https://github.com/Yar1klamer/j606f-postmarketos).

---

## Содержание
- [Предупреждения](#предупреждения)
- [Устройство](#устройство)
- [Что работает в Windows](#что-работает-в-windows)
- [Как это устроено](#как-это-устроено)
- [Структура репозитория](#структура-репозитория)
- [Сборка UEFI](#сборка-uefi)
- [Установка Windows](#установка-windows)
- [Свои драйверы Windows](#свои-драйверы-windows)
- [Известные проблемы и планы](#известные-проблемы-и-планы)
- [Благодарности и лицензии](#благодарности-и-лицензии)

## Предупреждения
- **Никогда не пишите в `modem*`, `persist`, `fsg`, `modemst*`, `abl`, `xbl`, `tz`, `hyp`, `devcfg`, `keymaster`** и прочие загрузочные/калибровочные
  разделы. Этот порт их не трогает: UEFI грузится загрузчиком Lenovo (ABL) как обычный boot-образ — через `fastboot boot` (только в RAM) или из `boot_a`.
- Перед любым изменением разметки — **бэкап** GPT и `userdata` (см. [docs/INSTALL_WINDOWS.md](docs/INSTALL_WINDOWS.md)).
- Windows работает в режиме **тестовой подписи** (`testsigning on`, `nointegritychecks on`): собственные драйверы подписаны своим тестовым сертификатом.
  Каждый, кто собирает драйверы, создаёт **свой** сертификат (`windrv/build.sh` делает это сам) — ключ не публикуется и не передаётся.
- В репозитории **нет** бинарников Lenovo/Qualcomm (DXE-драйверы из `xbl`, DTB, прошивка тача), образов Windows/WinPE и готовых образов UEFI.
  Драйверы Qualcomm извлекаются **со своего планшета** — [docs/EXTRACT_BINARIES.md](docs/EXTRACT_BINARIES.md). Образ Windows — свой (ISO/UUP).
- Windows 11 **24H2 и новее на SD662 (ARMv8.0) не стартуют** — нужна 22H2/23H2 (22621/22631).

## Устройство

| | |
|---|---|
| Модель | Lenovo Tab P11, TB-J606F (Wi-Fi), 2020 |
| SoC | Qualcomm SM6115P «bengalp» (Snapdragon 662), soc_id 445 |
| RAM / память | 4 или 6 ГБ / UFS 64 или 128 ГБ (проверено на 6/128) |
| Экран | 11", 2000×1200, BOE + Himax HX83102P (проверено); ревизия с Novatek NT36523W не проверялась |
| Тач | Himax HX83102E (zero-flash, SPI) |
| Батарея / зарядка | 7500 мА·ч, fuel gauge MM8013, PMI632 + SMB1355 |

## Что работает в Windows

Проверено на Windows 11 Pro ARM64 22621 (ru-RU), установленной из pmOS.

| Подсистема | Статус | Примечание |
|---|---|---|
| UEFI (Mu-Silicium j606f) | ✅ | все DXE Qualcomm из стокового `xbl_a` стартуют; `fastboot boot` или запись в `boot_a` |
| UFS (внутренняя память) | ✅ | inbox `storufs` (ACPI `QCOM24A5`) |
| Загрузка Windows / рабочий стол | ✅ | OOBE пропускается через `unattend.xml` (локальная учётка) |
| Экран | 🟡 | framebuffer загрузчика (SimpleFb, 1200×2000), без регулировки яркости и без GPU |
| Батарея, адаптер, статус зарядки | ✅ | ACPI (BAT0/ADP0 + inbox cmbatt), значения читаются из PMI632 по SPMI прямо в AML; % — по OCV-таблице |
| Зарядка | ✅ | сторожевой таймер зарядника SMB5 выключен в конфиге UEFI (`EnableChargerWdog = 0`) |
| Тачскрин | 🚧 | тестовый драйвер поднимает SPI, загружает прошивку и читает координаты; HID-драйвер для ввода в Windows написан, **не испытан** |
| Кнопки | ❌ | кнопки питания в ACPI нет; в UEFI Громкость/Питание работают |
| USB (device и host) | ❌ | |
| Wi-Fi, Bluetooth, звук, GPU, камеры, датчики | ❌ | драйверов для SM6115 нет |
| Часы (RTC), сон, частоты CPU | ❌ | часы Windows стоят на 2022 году |
| Загрузка pmOS ↔ Windows | 🟡 | через слоты A/B (`fastboot set_active a` — Windows, `b` — pmOS); меню выбора в UEFI — в планах |

## Как это устроено
- **UEFI**: пакет `Platforms/Lenovo/j606fPkg` для Mu-Silicium (копия `limePkg` Redmi 9T, тот же SoC; карта памяти и набор DXE совпали со стоковым
  `xbl_a` J606F). Драйверы Qualcomm (`Binaries/j606f`) — **свои**, из `xbl_a` планшета (версии совпадают с нашими TZ/XBL), с теми же патчами,
  что Silicium применяет для lime (`scripts/patch_drivers.py`). ACPI — DSDT Kamorta из Silicium + свой SSDT (`acpi/`).
- **Отладка без экрана и без USB**: DEBUG-сборка дублирует лог в консольную зону ramoops ядра pmOS, делает тёплый сброс — лог читается в pmOS
  из `/var/lib/systemd/pstore` (`uefi/patches/silicium_j606f_diag.diff`, `scripts/patch_silicium_diag.py`).
- **Установка Windows из pmOS** (ПК с Windows не нужен): ужать pmOS, разметить ESP + NTFS, `wimlib-imagex apply` из pmOS, `bcdboot` — из
  WinPE, загруженного через этот же UEFI; `SanPolicy=4`, чтобы Windows не монтировала прочие разделы. Подробно — [docs/INSTALL_WINDOWS.md](docs/INSTALL_WINDOWS.md).
- **Разработка драйверов без ввода**: служба-агент в Windows выполняет `C:\j606f\run.cmd` и пишет логи; файлы и сценарий кладутся из pmOS (ntfs-3g),
  результаты читаются оттуда же (`scripts/pmos_win_deploy.sh`, `windrv/agent/`).

## Структура репозитория
```
uefi/                       исходники для дерева Mu-Silicium (копируются поверх клона)
  Platforms/Lenovo/j606fPkg/  пакет устройства: DSC/FDF, карта памяти, конфиг, список DXE (BSD-2-Clause-Patent)
  Resources/Configs/          j606f.toml (формат boot-образа)
  Silicium-ACPI/…/j606f/      SSDT устройства (последний проверенный — v5d, SPMI-батарея)
  patches/                    diff диагностических правок Silicium (лог в ramoops, Mass Storage без ОС)
acpi/                       исходники ACPI: батарея по SPMI (полная и компактная), история SSDT (v4…v6)
windrv/                     свои драйверы Windows ARM64 (llvm-mingw, без WDK): probe, touch, hidtouch, agent, заголовки
scripts/                    разбор xbl (fv_extract), Binaries/j606f (gen_binaries, patch_drivers), правки Silicium,
                            ужатие pmOS под Windows, выкладка файлов в Windows из pmOS, WinPE (bcdboot) и unattend.xml
docs/                       инструкции; docs/notes/ — заметки по порту; DECISIONS.md, COMMANDS.md — решения и проверенные команды
```

## Сборка UEFI
Кратко (подробно — [docs/BUILD_UEFI.md](docs/BUILD_UEFI.md)): Linux x86_64, клон Mu-Silicium, драйверы Qualcomm со своего `xbl_a`
([docs/EXTRACT_BINARIES.md](docs/EXTRACT_BINARIES.md)), затем
```bash
python3 build_uefi.py -d j606f -r RELEASE    # → Mu-j606f.img
fastboot boot Mu-j606f.img                   # только RAM, ничего не пишет
```

## Установка Windows
Без ПК, из установленного pmOS: [docs/INSTALL_WINDOWS.md](docs/INSTALL_WINDOWS.md) (разметка, wimlib, WinPE + bcdboot, SanPolicy=4, unattend).
Вариант с ПК на Windows через Mass Storage UEFI (как у lime/caprip) описан там же, но на J606F не проверялся.

## Свои драйверы Windows
`windrv/` — драйверы ядра Windows ARM64, собираемые **llvm-mingw** без WDK (свой минимальный заголовок `include/j606f_nt.h`, импорт `ntoskrnl`
через `.def`). Флаги линковки подобраны под inbox-драйверы ARM64 (иначе `STATUS_CONFLICTING_ADDRESSES`), см. `windrv/build.sh`.
- `probe/` — чтение состояния GCC/TLMM/GENI и регистров зарядки PMI632 по SPMI;
- `touch/` — тест тача: GENI SPI (CPHA 1 + CPOL 1), загрузка zero-flash прошивки HX83102E, чтение событий;
- `hidtouch/` — HID-минидрайвер тача (Touch Screen, 10 касаний) для устройства ACPI `JHMX0001` (SSDT v6) — **не испытан**;
- `agent/` — служба-агент и сценарии тестов.

Прошивка тача (`Himax_firmware.bin`) — файл Lenovo, в репозитории её нет: берётся из раздела `vendor` своего планшета и кладётся в `C:\j606f\drv\hxfw.bin`.

## Известные проблемы и планы
- **FVMAIN должен оставаться 0x521000**: образ с FVMAIN 0x522000 сбрасывается до Windows (причина не найдена) — отсюда компактный AML батареи.
- После принудительного выключения NTFS «грязная» — ntfs-3g в pmOS монтирует только на чтение; перезагружайте Windows штатно (`ntfsfix` — крайний случай).
- Тесты Windows без ввода: если планшет завис в CrashDump Qualcomm (USB `05c6:900e`), выход — удержание Питания 10–15 с.
- Планы: ввод с тача в Windows, меню выбора pmOS/Windows в UEFI, яркость (DCS 0x51), частоты CPU, USB host, RTC.

## Благодарности и лицензии
- [Project-Silicium/Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) — UEFI для Snapdragon, платформа KamortaPkg и устройство lime,
  на основе которого сделан j606fPkg; [Project Mu](https://microsoft.github.io/mu/) и [EDK II](https://github.com/tianocore/edk2).
- [Project Aloha](https://github.com/Project-Aloha/mu_aloha_platforms) — UEFI для Qualcomm-устройств, документация по WoA.
- [Renegade Project](https://renegade-project.tech) — гайды по установке Windows на Snapdragon.
- [sm61x5-mainline](https://codeberg.org/sm61x5-mainline/linux) и порт pmOS — источник сведений о железе (адреса, тактирование, тач Himax).

Лицензии — см. [LICENSE](LICENSE): `uefi/` — BSD-2-Clause-Patent (как Mu-Silicium/EDK II, заголовки апстрима сохранены);
`windrv/touch` и `windrv/hidtouch` — лицензия **не решена** (предполагается GPL-2.0-only, т.к. последовательности работы с тачем повторяют GPL-драйвер Himax; владелец решит до публикации);
остальное (`acpi/`, `windrv/` прочее, `scripts/`, `docs/`) — MIT. Windows, драйверы и прошивки Qualcomm/Lenovo принадлежат их владельцам
и здесь не распространяются.
