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
