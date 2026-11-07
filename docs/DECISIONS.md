# DECISIONS — UEFI + Windows on ARM

## 2026-11-04 — база для порта
- **Mu-Silicium (Project-Silicium/Mu-Silicium), а не mu_aloha_platforms**: в Aloha нет SM6115/Kamorta ни в main, ни в ветках; в Silicium есть
  Silicon/Qualcomm/KamortaPkg, ACPI Kamorta и 5 устройств SD662 (Windows грузится на lime и caprip). Шаблон устройства — **lime** (Redmi 9T):
  его DXE-набор и карта памяти совпадают с нашим стоковым xbl_a (notes/stock_uefi.md). Решение предварительное — до подтверждения владельцем.
- Имя цели — `j606f` (`Platforms/Lenovo/j606fPkg`); в Silicium уже есть j706fPkg (Tab P11 Pro) и 9707fPkg — конфликта нет.
- Драйверы Qualcomm берём **из своего xbl_a** (а не из Device-Binaries других устройств): версия совпадает с нашим TZ/XBL. Блобы — только локально.
- Дисплей: `SimpleFbDxe` на framebuffer ABL 0x5C000000 (USE_CUSTOM_DISPLAY_DRIVER = 0), как у lime; стоковый DisplayDxe + XML панели — позже, если понадобится.
- DTB в образе — стоковый DTB №1 из стокового boot (тот, что выбирал ABL: dtb_idx=1), не mainline.
- Первый запуск — только `fastboot boot` (RAM). Запись UEFI в boot_a — после успешного запуска и решения владельца.
- Windows: целевая версия 11 23H2 ARM64 (или 10 ARM64): 24H2+ на ARMv8.0 (Kryo 260) не стартует (предупреждение Silicium для SD662).

## 2026-11-04 (ночь) — запуск и отладка без экрана
- **Канал лога UEFI — ramoops pmOS**: DEBUG-сборка дублирует лог в консольную зону ramoops (0xFFCA0000) с чётностью Рида–Соломона, как у ядра;
  без ОС — тёплый сброс → pmOS → /var/lib/systemd/pstore/console-ramoops-0. /dev/mem не годится (STRICT_DEVMEM, no-map). Только под J606F_RAMOOPS_LOG
  (TARGET=DEBUG); в RELEASE этого кода и резерва 0xFFC00000 нет.
- **Без ОС — Mass Storage, а не выключение** (J606F_MSD_ON_NO_OS, и в RELEASE): перед запуском — PlatformBootManagerOnDemandConInConnect() +
  ConnectAll, иначе UsbMsdDxe не находит EFI_USBFN_IO. В DEBUG — тайм-аут 3 мин (без человека из цикла не выйти).
- macOS Mass Storage Qualcomm не принимает (MODE SENSE зависает) — драйвер Qualcomm не правим: установку Windows вести с Windows-ПК (A) или из pmOS (B).
- Windows: образ и способ установки — решение владельца (notes/windows_plan.md); без его «да» образ не скачиваю.
- **Бэкап userdata (решение владельца 04.11):** базовый — <бэкап pmOS>/userdata-backup-20261103.simg (проверен восстановлением 03.11). Новый не снимать;
  нужен только перед действием, затирающим userdata, и только если в pmOS с тех пор появилось новое ценное. После отката из него вернуть руками wlanmdsp.mbn
  (/lib/firmware/updates/qcom/sm6115/LENOVO/J606F/, стоковый из modem_a) и ~/.config/powerdevilrc [AC][SuspendAndShutdown] AutoSuspendAction=0.
  SIGSEGV python3/powerdevil на pmOS — известно с отката 03.11, бэкап их не лечит; python3 на планшете не использовать.

## 2026-11-07 Кнопки в Windows: HID-минидрайвер с опросом SPMI на root-устройстве
- Выбрано: свой WDM HID-минидрайвер hidbuttons.sys (как hidtouch), поток опрашивает PM6125 по SPMI каждые 20 мс (PON_RT_STS 0x0810, GPIO5 RT_STS 0xC410),
  отчёты Consumer Control (громкость) + System Control (Power). Устройство — ROOT\J606FBTN\0000 (ключ Enum\Root, Service j606fbtn), появляется при загрузке.
- Не ACPI0011/PNP0C40 (Windows Button Array): им нужен GpioInt от драйвера GPIO-контроллера PMIC (qcpmicgpio/PEP), которого нет; прерывания PMIC в Windows не подняты.
- Не новое устройство в SSDT: FVMAIN UEFI заполнен на 99 % (56 байт), 0x522000 не грузится.
- В PMIC только чтение: аппаратный сброс по удержанию Питания (10–15 с), S2/PS_HOLD не затронуты.

## 2026-11-07 Меню выбора ОС: UEFI-приложение в ESP, а не в FV
- J606fBoot (efiapp/J606fBoot, компонент j606f.dsc) кладётся в ESP как \EFI\Boot\bootaa64.efi; прежний bootaa64 (bootmgfw) — рядом как bootaa64.windows.efi.
  Причина: FVMAIN заполнен на 99 % и 0x522000 не грузится; откат меню — одно копирование файла из pmOS, разделы загрузчиков не трогаются.
- pmOS из меню грузится как ядро из boot_b через EFI stub (свой gunzip — Inflate.c, без чужого кода), Windows — через bootmgfw. Последний выбор — в j606fboot.def на ESP.
- Слот a (UEFI v7) активен постоянно, слот b (pmOS через ABL) — запасной путь (`fastboot set_active b`).

## 2026-11-07 Карта памяти v7: резервировать области reserved-memory pmOS
- В MemoryMapLib j606f добавлены Reserv-записи 0x53F00000 +0x2300000 (adsp/zap GPU) и 0x89B01000 +0x200000. Без них RamManagerDxe отдавал дыры как
  Conventional, ядро pmOS не могло зарезервировать память для adsp/GPU (чёрный экран). Windows от этого не страдает (проверено: рабочий стол на v7).

## 2026-11-07 Дисплей Windows: свой display-only драйвер (вариант A) — решение владельца
- Владелец выбрал «A: драйвер + ползунок»: свой KMDOD (пример Microsoft, MS-PL) + DXGK_BRIGHTNESS_INTERFACE, яркость — DCS 0x51 по DSI0 (путь доказан dsiprobe).
  Не qcdx (вариант C): нет драйвера для A610, нужны PEP/SMMU/SCM Qualcomm (notes/windows_gpu.md).
- WDK/SDK/пример KMDOD скачаны с разрешения владельца, держатся только на хосте сборки и не публикуются; публикуется только свой код (j606f_bl.cxx, patch_kmdod.sh, build_dsp.sh).
- Подключение без INF: Service устройства ROOT\BasicDisplay\0000 = j606fdsp. Тест — в две загрузки: первая ставит Service, вторая первым делом возвращает
  BasicDisplay (любой сбой/перезагрузка возвращает рабочий Basic Display). Подмена ImagePath самой службы BasicDisplay — только с решением владельца.
- Если POST-дисплей пустой — кадровый буфер UEFI (0x5c000000, 1200×2000, pitch 4800) вручную: SimpleFbDxe и pmOS используют тот же.
