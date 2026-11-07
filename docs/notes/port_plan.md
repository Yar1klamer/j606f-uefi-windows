# Порт UEFI на J606F: что есть, что делать, первый эксперимент (2026-11-04)

## 1. Готовые сборки (проверено 04.11 по GitHub API и README/Status.md)
- **Project-Aloha/mu_aloha_platforms** (main, push 02.11.2026): Silicon/QC = Msm8996, Msm8998, Sdm670, Sdm845, Sm7125, Sm7150, Sm7325,
  Sm8150, Sm8250, Sm8350, Sm8450, Sm8550, Sm8850. **SM6115/bengal (Kamorta), SM4250, SM6125, SM6225 — нет.** Ветки тоже без них
  (main, mu/202511, dev/cafe, waipio/…, tp1803 и др.). J606F нет. Копия README: ref/Aloha_README.md.
- **Project-Silicium/Mu-Silicium** (main, push 01.11.2026, релиз v4.1): есть SoC-пакет **Silicon/Qualcomm/KamortaPkg** (SM6115 = Snapdragon 662)
  и 5 устройств на нём (ref/Silicium_Status.md, раздел «Snapdragon 662»):
  | Устройство | Codename | UEFI | Windows |
  |---|---|---|---|
  | Xiaomi Redmi 9T | lime | дисплей, UFS, кнопки, USB device, mass storage | **грузится**: UFS, USB host, CPU, дисплей (FB) |
  | Motorola Moto G30 | caprip | дисплей, UFS, USB device | **грузится**: UFS, дисплей, CPU «4 Cores only» |
  | Motorola Moto G9 Power | cebu | дисплей, UFS, кнопки, USB host | нет |
  | Samsung Galaxy Tab A7 10.4 | gta4l | дисплей, UFS, USB host, PD | нет |
  | Xiaomi Poco M3 | citrus | дисплей, UFS, SD, mass storage | нет (Linux — да) |
  Все пять — «Inactive». Предупреждение Silicium для SD662: **«Only 23H2 Windows Builds and Lower are able to Boot»** (Kryo 260 = Cortex-A73/A53,
  ARMv8.0; 24H2+ требует ARMv8.1 атомики). Lenovo в Silicium — только Tab P11 Pro (j706fPkg, SM7150) и Legion Y700 (9707fPkg); **J606F нет**.
- Чужих UEFI/WoA-работ именно по Lenovo Tab P11 J606F не нашёл (веб-поиск, GitHub) — это будет первый порт.

**Вывод: готовой сборки нет, но порт — небольшой.** База — Mu-Silicium (KamortaPkg + шаблон lime), а не mu_aloha_platforms.

## 2. Почему порт небольшой (проверено на наших дампах, notes/stock_uefi.md)
- Набор стоковых DXE-драйверов в нашем `xbl_a` по именам совпадает с тем, что Silicium подключает для lime (DXE.inc).
- Карта памяти и ConfigParameters нашего `uefiplat.cfg` совпадают с MemoryMapLib/ConfigurationMapLib lime (кроме переноса DXE Heap, который делает сам Silicium).
- Framebuffer ABL — 0x5C000000 (= «Display Reserved»), 1200x2000x32, его же использует simplefb в pmOS → `SimpleFbDxe` Silicium подойдёт без DisplayDxe.
- ACPI для Kamorta в Silicium-ACPI уже есть (DSDT: UFS0, SDC1, CPU0–7, URS0/USB0/UFN0; FADT/GTDT/MADT/DBG2); устройству нужен только SSDT
  (`_STA` для UFS0/URS0/SDC1 и PSUB) — ref/silicium-acpi/.
- Упаковка (Resources/Configs/<dev>.toml у lime/caprip): Android boot image **header v1**, ядро = BootShim+FD (gzip), **DTB приклеен**;
  FD base 0x5FC00000 size 0x300000. ABL J606F appended DTB при header v0/v1 уже принимал (pmOS-проект/ LOG 28.10, тест 2).

## 3. Шаги порта (оценка — рабочее время, при доступном хосте)
1. **Хост сборки** (Ubuntu, `~/j606f-build/` отдельно от ~/j606f): `git clone --recursive Mu-Silicium` (~5 ГБ!), `./setup_env.sh -p apt` (sudo apt:
   clang/llvm, python3-venv, mono?), `pip -r pip-requirements.txt` в venv. Сначала собрать **lime** без изменений — проверка окружения. 1–2 ч + скачивание.
   ⚠ 04.11 хост по ssh недоступен (timeout, как и в pmOS-проект/ LOG 04.11) — нужен владелец. Запасной вариант — сборка на самом планшете под pmOS
   (aarch64, Silicium официально не поддерживает) или Linux-VM.
2. **Binaries/j606f** из нашего `xbl_a`: для каждого DXE — `.efi` + `.depex` + бинарный `.inf` (GUID/имя из FV), структура как Binaries/lime/QcomPkg/...
   Доработать scripts/fv_extract.py (секции DXE_DEPEX 0x13, генерация INF). + RawFiles: SecParti.cfg, uefipil.cfg, QcomChargerCfg.cfg, BATTERY.PROVISION,
   Panel_boe_himax83102p_2k_vid.xml. 2–4 ч. **Только локально** (блобы).
3. **Platforms/Lenovo/j606fPkg** (копия limePkg): j606f.dsc (SMBIOS Lenovo / TB-J606F, FB 1200x2000x32, PcdSystemMemoryBase 0x40000000, стек 0x5FF90000),
   j606f.fdf, Include/{DXE,APRIORI,RAW}.inc → Binaries/j606f, MemoryMapLib/ConfigurationMapLib из нашего uefiplat.cfg, DeviceBuild.py. 1–2 ч.
4. **Resources/Configs/j606f.toml** (как lime) и **Resources/DTBs/j606f.dtb** = стоковый DTB №1 из стокового boot (raw/stock_boot_unpacked/dtb,
   смещение 0x511b1, 332 261 Б — тот, что ABL выбирает: dtb_idx=1, msm-id 445, board-id 34/3). Silicium-ACPI: Platforms/Lenovo/j606f/SSDT.asl
   (UFS0 и URS0 вкл., SDC1 выкл. — у J606F SD-карта на sdhc_2, eMMC нет). 30–60 мин.
5. Сборка `python3 build_uefi.py -d j606f` → `j606f.img` (несколько МБ). 15–30 мин на 4 ядрах.
6. Первый запуск через `fastboot boot` (ниже). Дальше — итерации (кнопки, USB, mass storage). Итого до первого экрана UEFI: **~1 рабочий день**.

## 4. Первый эксперимент — только RAM, ничего не пишет
```bash
fastboot reboot bootloader     # обязательно: fastboot boot работает 1 раз за сессию ABL
fastboot boot j606f.img
```
Слот b активен, dtbo_b стёрт → ABL возьмёт приклеенный стоковый DTB без оверлея (стоковый DTB с __symbols__ выдержал бы и оверлей).
**Ожидаем на экране:** «Booting OKAY» → заставка ABL остаётся → BootShim переносит FD на 0x5FC00000 → DXE поднимает драйверы →
на framebuffer 1200x2000 (портрет) — логотип/меню загрузки Mu (BdsDxe/UEFI-меню, навигация громкостью, Power — выбор), т.к. ОС на диске нет.
**Возможные провалы:** через ~9 с обратно в fastboot (ABL не принял образ/DTB); чёрный экран/зависание (DXE; UART нет — только логи);
перезагрузка или «QUALCOMM CrashDump Mode» (watchdog). **Выход из любого:** зажать Power 15–20 с → ABL → слот b → pmOS.
Отладка после неудачи: Log Buffer 0x5FFF7000 (не переживает холодный сброс), раздел logfs (стоковый UEFI пишет туда, читать из pmOS только чтением).

## 5. Windows: путь установки (адаптация гайдов Renegade/Silicium, не выполнять вслепую)
Страница renegade-project.tech/en/install рендерится JS, WebFetch текста не получил — схема ниже общая для этих проектов.
1. Windows 11 **23H2 (22631) ARM64** или Windows 10 ARM64 — образ владельца (UUP dump). 24H2/25H2 на SD662 не стартуют.
2. Место: Windows ≥ 25–40 ГБ во внутренней памяти. Вариант по умолчанию — **ужать pmOS**: свежий бэкап userdata → resize2fs pmOS_root (сейчас
   занято ~11 ГБ) → ужать раздел во внутренней GPT userdata → ужать userdata (sda15, последний на LUN0) во внешней GPT и создать за ним
   ESP (FAT32 ~260 МБ) + Windows (NTFS). Внутреннюю GPT после этого поправить (резервный заголовок). SD-карта для Windows не годится (нет драйвера sdhc в Windows).
3. Запись Windows: UEFI в режиме USB mass storage (UsbMsdDxe) отдаёт UFS как USB-диск → на машине с Windows: DISM /Apply-Image, bcdboot на ESP.
   На Mac этого нет — нужен ПК/VM с Windows или Linux с wimlib (`wimlib-imagex apply`) + готовый BCD. Решить с владельцем.
4. Загрузка: UEFI-образ постоянно — в **boot_a** (там сейчас нерабочий Android: userdata = pmOS), выбор ОС — `fastboot set_active a|b`
   (слот b = pmOS как сейчас). dtbo_a можно не трогать (стоковый DTB + оверлей = как в Android). Бэкап boot_a уже есть (BACKUP/partitions/boot_a.img).
5. Драйверы: для SM6115 пакета драйверов Windows **нет** (ни у Qualcomm, ни у сообщества) — только ACPI Silicium (UFS, USB, CPU).

## 6. Что реально заработает в Windows (честно)
| Подсистема | Ожидание | Почему |
|---|---|---|
| Загрузка, UFS | ✅ (как у lime/caprip) | UFS0 в DSDT Kamorta |
| Экран | ⚠ только базовый framebuffer (Microsoft Basic Display), 1200x2000, портрет; поворот вручную; яркость не регулируется | драйвера Adreno 610/MDSS для Windows нет |
| CPU | ⚠ 4–8 ядер, частота не управляется (как оставил загрузчик) — медленно | нет PEP/драйверов питания SM6115 |
| USB-C host (клавиатура, мышь, флешка, USB-Ethernet) | ❔ у lime работает; у J606F VBUS дают FUSB302+буст gpio38 — может понадобиться хаб с питанием | URS0 в DSDT, но без драйвера Type-C |
| Тач (Himax HX83102E, SPI, zero-flash) | ❌ | нет драйвера SPI/QUP и Himax для Windows; прошивку тача надо грузить |
| Клавиатура Keyboard Pack (I2C-HID) | ❌ | нет драйвера I2C (QUP) SM6115 |
| Wi-Fi / BT (WCN3990 через модем) | ❌ | нужен PIL модема + qcwlan, для SM6115 нет |
| GPU | ❌ (программная отрисовка) | Adreno 610 — драйвера нет |
| Звук, датчики, камеры | ❌ | |
| Батарея / зарядка / уровень заряда | ❌ (индикатора нет; заряжает ли PMI632 сам — не проверено) | |
| Сон | ❌ | |
Итог: Windows на J606F — **демонстрация «грузится и работает с USB-мышью/клавиатурой»**, не повседневная система.
Побочная польза UEFI: можно грузить обычные aarch64-дистрибутивы/GRUB (DTB через UEFI) — к pmOS это не обязательно.
