# Порт UEFI на J606F: состав, карта памяти, версии, ограничения

Документ описывает **текущее** состояние порта (ноябрь 2026): как устроен UEFI, что в нём менялось от v3 до v7 и какие ограничения остаются.
История попыток и пооперационные журналы здесь не приводятся.

## 1. Основа и готовые сборки
- **Project-Aloha/mu_aloha_platforms** не содержит SM6115 (bengal / Kamorta) ни в main, ни в ветках. J606F там нет.
- **Project-Silicium/Mu-Silicium** содержит SoC-пакет `Silicon/Qualcomm/KamortaPkg` (SM6115 = Snapdragon 662) и пять устройств на нём.
  Шаблон для порта — **lime** (Redmi 9T): его набор DXE и карта памяти совпадают с нашим стоковым `xbl_a`.
  | Устройство | Codename | UEFI | Windows (по Silicium) |
  |---|---|---|---|
  | Xiaomi Redmi 9T | lime | дисплей, UFS, кнопки, USB device, mass storage | грузится: UFS, USB host, CPU, дисплей (FB) |
  | Motorola Moto G30 | caprip | дисплей, UFS, USB device | грузится: UFS, дисплей, CPU «4 Cores only» |
  | Motorola Moto G9 Power | cebu | дисплей, UFS, кнопки, USB host | нет |
  | Samsung Galaxy Tab A7 10.4 | gta4l | дисплей, UFS, USB host, PD | нет |
  | Xiaomi Poco M3 | citrus | дисплей, UFS, SD, mass storage | нет (Linux — да) |
  Предупреждение Silicium для SD662: **«Only 23H2 Windows Builds and Lower are able to Boot»**: Kryo 260 — это Cortex-A73/A53 (ARMv8.0),
  а 24H2 и новее требуют ARMv8.1 (атомарные операции LSE).
- Lenovo в Silicium представлен Tab P11 Pro (j706fPkg, SM7150) и Legion Y700 (9707fPkg). **J606F в нём отсутствовал**; наш пакет `j606fPkg` добавлен сюда.

## 2. Состав пакета `j606fPkg`
Исходники лежат в `uefi/Platforms/Lenovo/j606fPkg/`, копируются поверх дерева Mu-Silicium (см. [BUILD_UEFI](../BUILD_UEFI.md)).
- `j606f.dsc` / `j606f.fdf` — платформа и раскладка прошивки. Пакет переиспользует `KamortaPkg` и драйверы lime.
- `Include/{DXE,APRIORI,RAW}.inc` — список DXE-драйверов. Набор совпадает с `xbl_a` J606F, кроме отсутствующего у нас MiToken (Xiaomi).
- `Library/MemoryMapLib` и `Library/ConfigurationMapLib` — карта памяти и параметры конфигурации (ниже).
- `Application/J606fBoot` — меню выбора ОС (см. [menu.md](menu.md)); компилируется в состав образа.
- `uefi/Resources/Configs/j606f.toml` — формат boot-образа: header v1, ядро gzip, DTB приклеен, FD `0x5FC00000` размером `0x300000`.
- `uefi/Silicium-ACPI/…/SSDT.asl` — SSDT устройства (сейчас v6, см. [windows_drivers_plan.md](windows_drivers_plan.md)).

Бинарные части берутся **со своего планшета**: `Binaries/j606f` собирается из `xbl_a` скриптами `scripts/fv_extract.py`,
`scripts/patch_drivers.py`, `scripts/gen_binaries.py` (см. [EXTRACT_BINARIES](../EXTRACT_BINARIES.md)). В публичный репозиторий они не входят.

## 3. Карта памяти (MemoryMapLib, версия v7)
Ключевые регионы DDR (адреса в шестнадцатеричном виде):
| Регион | Адрес | Размер | Примечание |
|---|---|---|---|
| Kernel | 0x40000000 | 0x05700000 | память ядра Linux, резерв |
| HYP | 0x45700000 | 0x00600000 | |
| SMEM | 0x46000000 | 0x00200000 | |
| PIL Reserved | 0x4AB00000 | 0x09400000 | прошивки подсистем, резерв |
| **pmOS PIL (v7)** | 0x53F00000 | 0x02300000 | резерв для adsp и zap GPU, нужен Linux |
| Display Reserved | 0x5C000000 | 0x01000000 | кадровый буфер GOP, 1200×2000 |
| UEFI FD | 0x5FC00000 | 0x00300000 | сама прошивка |
| UEFI Stack | 0x5FF90000 | 0x00040000 | |
| Log Buffer | 0x5FFF7000 | 0x00008000 | не переживает холодный сброс |
| QSEE | 0x60000000 | 0x01800000 | TrustZone |
| TZApps | 0x61800000 | 0x02100000 | |
| DXE Heap | 0x63900000 | 0x04600000 | Silicium переносит кучу DXE сюда из стокового места |
| **pmOS Rsvd (v7)** | 0x89B01000 | 0x00200000 | резерв reserved-memory pmOS |

Почему в v7 добавлены два резерва: без них `RamManagerDxe` отдавал эти области как Conventional, и ядро pmOS не могло зарезервировать
память для adsp и GPU (чёрный экран). Windows от изменения не зависит.

Регионы регистров: TLMM (WEST `0x00500000`, SOUTH `0x00900000`), TCSR, IMEM и др. — из стокового `uefiplat.cfg`.
В отладочной сборке (`J606F_RAMOOPS_LOG`) добавлен регион «RAM Oops» `0xFFC00000` размером 1 МиБ под консольную зону ramoops ядра pmOS.

## 4. Версии UEFI

| Версия | Что изменилось | Проверено |
|---|---|---|
| v3 | первая RELEASE для Windows (Mu-Silicium + пакет j606f, стоковые драйверы Qualcomm) | Windows загружается до рабочего стола |
| v4 | `EnableChargerWdog = 0` в конфиге зарядника | зарядка в Windows такая же, как в pmOS |
| v5d | батарея и адаптер в AML через SPMI (компактный SSDT) | 98–100 %, статус «заряжается» |
| v6 | устройство тача `TSC0` с `_HID JHMX0001` в SSDT | тач работает (драйвер `hidtouch`) |
| v7 | резерв памяти pmOS в карте памяти; меню J606fBoot в ESP | в `boot_a`, Windows и Linux выбираются через меню |

Версия **v7** записана в `boot_a` (слот a). Слот b остаётся с pmOS через стоковый ABL и служит запасным путём загрузки.

## 5. Ограничения сборки

### 5.1 Раскладка FVMAIN: строго 0x521000
В выводе сборки строка `FVMAIN [..%Full] … total` должна показывать размер **0x521000**. Образ с 0x522000 не загружается:
он сбрасывается обратно в Windows / pmOS (причина не найдена). Свободного места в FVMAIN около 56 байт.
Поэтому:
- SSDT держим компактным (компактная версия батареи — `acpi/j606f_battery_spmi_compact.asl`);
- новые ACPI-устройства, которым нужен крупный AML, не добавляем в FVMAIN; драйверы, которым нужен SSDT, работают через вспомогательные
  устройства (например, `hidbuttons` — корневое устройство `ROOT\J606FBTN` без записи в SSDT).

Меню J606fBoot поэтому вынесено **из FV в ESP** как отдельный EFI-файл: так FVMAIN не растёт.

### 5.2 Сторожевой таймер зарядника (SMB5)
`QcomChargerDxeLA` с конфигом `EnableChargerWdog = 1` включает сторожевой таймер SMB5: регистр `0x1651` = `0x62`, `0x1653` = `0x8C`.
Бит 7 регистра `0x1653` (`BITE_WDOG_DISABLE_CHARGING`) означает, что при невыполненном «кормлении» таймера зарядка отключается.
В Windows никто таймер не кормит, поэтому зарядка прекращалась через секунды–минуты после `ExitBootServices`.
Решение — `EnableChargerWdog = 0` в `Binaries/j606f/RawFiles/QcomChargerCfg.cfg` (так же настроены kebab, enchilada, pong и gta9pwifi).
После этого `0x1651 = 00`, `0x1653 = 0C` — как в pmOS.

### 5.3 Отладочные сборки
- `J606F_MSD_ON_NO_OS` (RELEASE и DEBUG): если ОС нет, UEFI запускает USB и Mass Storage вместо выключения. Выход из режима — Громкость +.
  Mass Storage на macOS не работает (MODE SENSE зависает), с Windows-ПК мог бы работать.
- `J606F_RAMOOPS_LOG` (только DEBUG): лог UEFI дублируется в консольную зону ramoops ядра pmOS с чётностью Рида–Соломона,
  после чего делается тёплый сброс. Лог читается из pmOS из `/var/lib/systemd/pstore/console-ramoops-0`.
  Патч: `uefi/patches/silicium_j606f_diag.diff`, скрипт `scripts/patch_silicium_diag.py`.
- В DEBUG-сборке Mass Storage выключается по таймеру через 3 минуты, чтобы не ждать человека.

## 6. Что работает (состояние на ноябрь 2026)
| Подсистема | Статус | Примечание |
|---|---|---|
| UEFI в `boot_a` (v7) | ✅ | `fastboot boot` для проверок, запись в `boot_a` для постоянного использования |
| Меню Windows / Linux | ✅ | см. [menu.md](menu.md) |
| UFS | ✅ | Windows видит через ACPI `UFS0` (QCOM24A5), драйвер `storufs` из Windows |
| Экран (кадровый буфер) | ✅ | Basic Display поверх GOP, 1200×2000, портрет |
| Батарея, адаптер | ✅ | ACPI BAT0 / ADP0 по SPMI, см. [windows_drivers_plan.md](windows_drivers_plan.md) |
| Зарядка | ✅ | `EnableChargerWdog = 0` |
| Тач | ✅ | `hidtouch`, перо не реализовано |
| Кнопки | ✅ | `hidbuttons`: питание; громкость — отчёты без звука |
| Яркость из Windows по DSI | 🚧 | доказана `dsiprobe`; слайдер — драйвер `j606fdsp` в работе |
| Частоты CPU | ✅ | `j606fcpu`: 300 МГц ↔ 1804/2016 МГц, губернатор с тепловым пределом |
| Bluetooth | ✅ | стек Microsoft видит устройства; сопряжение мыши не проверено |
| ADSP (LPASS Q6) | 🚧 | запускается из Windows (READY + HANDOVER); звука и датчиков пока нет — нужен GLINK к ADSP, см. [windows_adsp.md](windows_adsp.md) |
| Wi-Fi | ✅ | настоящий адаптер Wi-Fi в Windows, WPA2-Personal, автоподключение, 802.11ac — см. [windows_wifi.md](windows_wifi.md) |
| USB host, камеры, RTC, сон, GPU | ❌ | см. [windows_gpu.md](windows_gpu.md) и [windows_drivers_plan.md](windows_drivers_plan.md) |

## 7. Ожидания по подсистемам (исходная таблица, правки владельца от 07.11)

| Подсистема | Ожидание | Почему |
|---|---|---|
| Загрузка, UFS | ✅ (как у lime/caprip) | UFS0 в DSDT Kamorta |
| Экран | ✅ только базовый framebuffer (Microsoft Basic Display), 1200x2000, портрет; поворот вручную; яркость пока не регулируется (слайдер в работе, драйвер `j606fdsp` даёт код 43) | драйвера Adreno 610/MDSS для Windows нет |
| CPU | ⚠ 4–8 ядер, частота не управляется (как оставил загрузчик) — медленно | нет PEP/драйверов питания SM6115 |
| USB-C host (клавиатура, мышь, флешка, USB-Ethernet) | ❔ у lime работает; у J606F VBUS дают FUSB302+буст gpio38 — может понадобиться хаб с питанием | URS0 в DSDT, но без драйвера Type-C |
| Тач (Himax HX83102E, SPI, zero-flash) | ✅ | полноценно работает |
| Клавиатура Keyboard Pack (I2C-HID) | ❌ | нет драйвера I2C (QUP) SM6115 |
| Wi-Fi / BT (WCN3990 через модем) | Wi-Fi ✅, BT ✅ | Bluetooth работает (стек Microsoft); Wi-Fi работает через модем: см. windows_wifi.md |
| GPU | ❌ (программная отрисовка) | Adreno 610 — драйвера нет |
| Звук, датчики, камеры | ❌ | |
| Батарея / зарядка / уровень заряда | ✅ работает | |
| Сон | ❌ | |
Итог: Windows на J606F — **демонстрация «грузится и работает с USB-мышью/клавиатурой»**, не повседневная система.
Побочная польза UEFI: можно грузить обычные aarch64-дистрибутивы/GRUB (DTB через UEFI) — к pmOS это не обязательно.
