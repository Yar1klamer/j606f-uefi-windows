# Windows на J606F: как ставить (2026-11-04, после тестов U1–U4)

## Что уже доказано
- UEFI j606f (Mu-Silicium, KamortaPkg) грузится через `fastboot boot`: SEC → DXE → BDS, все драйверы Qualcomm из нашего xbl_a стартуют,
  UFS (Micron MT128GASAO4U21) виден, таблицы ACPI ставятся (AcpiPlatform), SimpleFb на 0x5C000000. Лог — raw/uefi_logs/u3_d2.log.
- Mass Storage UEFI поднимается (05c6:f000, SCSI BOT), LUN0 = UFS LU0 (sda, 114,9 ГиБ, сектор 4096), но **macOS с ним не работает**
  (MODE SENSE зависает, raw/uefi_logs/u4_mac_kernel_usb.log). С Windows-ПК им пользуются lime/caprip — у нас не проверено.
- Windows видит UFS через ACPI `UFS0` (_HID QCOM24A5) — так грузится lime; драйвера для тача/Wi-Fi/звука/GPU нет (notes/port_plan.md §6).

## Что нужно от владельца (блокеры)
1. **Образ Windows 11 23H2 ARM64 (сборка 22631)** — свой ISO/ESD или «да» на скачивание через UUP dump (файлы Microsoft, ~4–5 ГБ ESD +
   конвертация в ISO на хосте сборки). 24H2/25H2 на SD662 не стартуют.
2. **Чем ставить:** (A) есть ПК с Windows → классика Silicium через Mass Storage; (B) ПК нет → всё на планшете через pmOS (дольше, но без ПК).
3. **Место:** Windows ставится на LUN0 (sda) после userdata. userdata (sda15, 102,5 ГиБ) = pmOS (вложенная GPT: pmOS_boot 480 МиБ + pmOS_root ~102 ГиБ,
   занято ~11 ГБ). Варианты: ужать pmOS до ~24 ГиБ и отдать ~78 ГиБ под Windows (pmOS остаётся) или удалить pmOS целиком
   (есть BACKUP/userdata-backup-20261103.simg и релизный архив pmOS → вернуть можно).

## Разметка LUN0 (общая для A и B)
Сейчас: sda1…sda14 (ssd, **persist**, misc, keystore, frp, fpinfo, lenovocust, lenovoraw, recovery_a/b, vbmeta_system_a/b, metadata, super),
sda15 userdata — последний. Трогаем **только sda15 и место за ним**:
1. Бэкап: pmOS_root (e2image/dd по USB-сети на Mac) + внешняя GPT sda (первые и последние 6 секторов по 4К) + вложенная GPT userdata.
2. Ужать pmOS (если оставляем): из initramfs pmOS (`pmos.debug-shell`, root не смонтирован) — `e2fsck -f` + `resize2fs` pmOS_root до ~22 ГиБ →
   ужать раздел во вложенной GPT → перенести её резервный заголовок.
3. Внешняя GPT: sda15 ужать до ~24 ГиБ (`sgdisk -d 15 -n 15:<тот же старт>:+24G -t/-c/-u` с прежними type/name/PARTUUID),
   затем `esp` 512 МиБ (EF00, FAT32, сектор 4К → FAT32 требует ≥ 65525 кластеров: 512 МиБ / 4К = 131072 — ок) и `win` NTFS (0700) на остаток.
4. Каждая запись — в LOG до выполнения, с sha256 бэкапов и командой отката (`sgdisk --load-backup`).

## Вариант A — ПК с Windows (как у lime/caprip)
1. `fastboot boot out/v3/Mu-j606f.img` (без ОС UEFI сам уходит в Mass Storage; выход — Громкость+).
2. На ПК: diskpart — выбрать диск ~115 ГБ **по размеру**, `list partition`, форматировать только `esp`/`win` (созданные на шаге 3 выше; или создать их
   на ПК в свободном месте), назначить буквы S: и W:.
3. `dism /apply-image /imagefile:install.wim /index:<Pro> /applydir:W:\` → `bcdboot W:\Windows /s S: /f UEFI` →
   `bcdedit /store S:\EFI\Microsoft\Boot\BCD /set {default} testsigning on` и `nointegritychecks on` (как в гайдах Silicium) → извлечь.
4. Загрузка: `fastboot boot out/v3/Mu-j606f.img` → Silicium «Internal Storage» найдёт \EFI\Boot\bootaa64.efi на `esp`.

## Вариант B — без ПК, на самом планшете (pmOS как установщик)
1. Разметка как выше (из pmOS/initramfs). `mkfs.vfat -F32 -S 4096` для esp, `mkntfs -Q -s 4096` для win (ntfs-3g).
2. `wimlib-imagex apply install.wim <index> /dev/sdaN` (режим NTFS-3G, сохраняет ACL/атрибуты) — пакеты Alpine `wimlib`, `ntfs-3g-progs`.
3. Загрузчик: BCD нужно создать без bcdboot. Надёжный путь — WinPE из того же ISO: положить на esp файлы загрузки ISO (\EFI, \boot, \sources\boot.wim,
   их BCD универсальный: ramdisk=[boot]\sources\boot.wim), в boot.wim (индекс 2) через `wimlib-imagex update` добавить `winpeshl.ini` + скрипт:
   diskpart (буквы по меткам томов) → `bcdboot W:\Windows /s S: /f UEFI` → bcdedit testsigning/nointegritychecks → лог на esp → `wpeutil reboot`.
   Затем убрать файлы WinPE с esp. Всё без клавиатуры; результат читается из pmOS (лог на esp).
4. Загрузка Windows: как в A.4. Первый запуск (OOBE) требует ввода: USB-клавиатура/мышь через OTG (VBUS у J606F — FUSB302 + буст gpio38,
   может понадобиться хаб с питанием) или `unattend.xml` с локальной учёткой и пропуском OOBE (кладётся в W:\Windows\Panther\ до первой загрузки).

## Постоянная установка UEFI
После того как Windows загрузится через `fastboot boot`: записать UEFI (RELEASE) в **boot_a**, переключать ОС `fastboot set_active a|b`
(b = pmOS). boot_a сейчас — сток+Magisk, бэкап <бэкап разделов>/boot_a.img. Запись — только после LOG и с sha256.
