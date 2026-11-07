# Windows на J606F: что можно «портировать» и в каком порядке (2026-11-04, после установки)

## 0. Исходные факты
- Публичных Windows-драйверов для SM6115 нет ни у Qualcomm, ни у сообщества (Silicium-гайд по драйверам для SM6115 пуст; на Redmi 9T
  работают только CPU, базовый дисплей, UFS, USB host). «Портировать» = писать своё.
- Что Windows видит из ACPI Silicium (Kamorta DSDT + наш SSDT j606f): только UFS0 (QCOM24A5, inbox storufs), SDC1 (выкл.), CPU0–7,
  URS0 (USB: dwc3 host USB0 + function UFN0). Нет GPIO (TLMM), I2C/SPI (QUP GENI), SPMI (PMIC), батареи, дисплея (кроме GOP-фреймбуфера).
- Свой SSDT мы собираем сами (Silicium-ACPI/Platforms/Lenovo/j606f/SSDT.asl → в FD) → **новые ACPI-устройства/методы добавляем без подписи драйверов**.
- testsigning on, nointegritychecks on → свои тестово-подписанные драйверы грузятся.

## 1. Зарядка
- Аппаратно PMI632 (SMB5) заряжает без драйвера ОС на настройках загрузчика: FCC 3 А, FV 4,40 В, ICL 1,5 А (у Mac фактически 950 мА, APSD = SDP)
  — проверено в pmOS 29.10 (pmos-j606f/notes/power.md).
- Риск: наш UEFI (QcomChargerDxeLA из xbl_a) в логе пишет «Enabled WDOG», QcomChargerCfg.cfg: `EnableChargerWdog = 1`. Если сторожевой таймер SMB5
  включён с `BITE_WDOG_DISABLE_CHARGING_CFG` (0x1653 бит 7) и его никто не «кормит» (0x1643), зарядка выключится через секунды–минуты после
  ExitBootServices. Тот же драйвер работает и на стоковом пути к pmOS, а pmOS заряжается → скорее всего безопасно, но **проверить**:
  1. pmOS: `sudo sh scripts/pmos_charger_status.sh` → WD_CFG 0x1651, 0x1653, статус 0x1006 (состояние зарядки), mm8013.
  2. Тест: в pmOS записать mm8013 charge_now/capacity → Windows под зарядкой 30–60 мин → pmOS снова → сравнить.
- Если таймер мешает: `EnableChargerWdog = 0` в нашем Binaries/j606f/RawFiles/QcomChargerCfg.cfg (так у kebab/enchilada/pong/gta9pwifi) → пересборка UEFI.
- Быстрая зарядка от адаптера (ICL до 3 А по DCP), SMB1355 (параллельный) — только своим драйвером/AML-записью в PMIC (позже, осторожно).

## 2. Дисплей
- Сейчас: Microsoft Basic Display Adapter поверх GOP/SimpleFB, 1200×2000, портрет, яркость как оставил загрузчик.
- GPU (Adreno 610) — драйвера нет и не будет (закрытый драйвер Qualcomm per-SoC).
- Поворот (альбомная ориентация): Basic Display поддерживает поворот в «Параметры → Дисплей → Ориентация» — **проверить у владельца**.
- Яркость: на J606F она задаётся DCS 0x51 панели (HX83102P, 12 бит), SGM37604A (I2C1 0x36) лишь включён pmi632 gpio6 (pmos notes/backlight.md).
  Варианты: (а) фиксированная яркость, выставляемая UEFI перед загрузкой (DCS через DSI-хост MDSS из нашего DXE) — просто, но без регулировки;
  (б) свой драйвер/служба: команда DSI DCS (MDSS DSI CMD DMA в видеорежиме) по горячим клавишам/ползунку; (в) регистры SGM37604A по I2C1 (если
  он в режиме I2C, а не PWM). Яркостью в Параметрах Windows управлять нельзя без собственного display-miniport — только своим инструментом.
- Выключение экрана при простое (Basic Display не гасит подсветку) — тоже через (б)/(в).

## 3. Батарея в Windows (индикатор, %, «заряжается») — ПЕРВЫЙ кандидат
Путь без подписи драйверов: **ACPI Control Method Battery** (inbox cmbatt.sys). В SSDT: `Device (BAT0) { _HID "PNP0C0A", _BIX, _BST, _STA }`
и `Device (ADP0) { _HID "ACPI0003", _PSR }`. Методы на AML читают fuel gauge **mm8013** (I2C0 = QUP0 SE0 @ 0x04A80000, адрес 0x55, SMBus-слова
LE: 0x08 напряжение мВ, 0x0A флаги (bit8 CHG, bit9 FC, bit0 DSG), 0x0E полная ёмкость мА·ч, 0x14 средний ток мА (знак), 0x2A циклы,
0x2C SoC %, 0x3C проектная ёмкость, 0x06 температура 0,1 К) через MMIO контроллера GENI I2C (OperationRegion SystemMemory):
- такты: GCC 0x0147900C голосование, биты 6 m-ahb, 7 s-ahb, 8 core, 9 core2x, 10 S0 (halt-регистры 0x0141F004/8/0C/14, 0x0141F144; бит 31 CLK_OFF);
- пины gpio0/gpio1 → функция qup0 (TLMM, конфиг пина = база тайла + 0x1000·N; тайл и номер функции — по pinctrl-sm6115.c), pull-up;
- SE: проверить GENI_FW_REVISION_RO (0x68, протокол в битах 15:8 = 3 → I2C; прошивку SE грузит XBL из qupfw);
  SER_M_CLK_CFG (0x48) и SE_I2C_SCL_COUNTERS (0x278) под 19,2 МГц/100 кГц (div 7; t_high 10, t_low 12, t_cycle 26 — как i2c-qcom-geni.c);
- транзакция «записать адрес регистра + прочитать 2 байта»: TX_TRANS_LEN=1, RX_TRANS_LEN=2, M_CMD0 = (I2C_WRITE_READ=3 << 27) | (0x55 << 9),
  слово в TX_FIFO (0x700), ждать M_IRQ_STATUS (0x610) CMD_DONE с тайм-аутом, читать RX_FIFO (0x780), очистить IRQ (0x618).
Плюсы: inbox-драйвер, ничего не подписываем, правка только в UEFI (RAM-тест через `fastboot boot`). Минусы: ошибка в AML = зависание/BSOD
ACPI_BIOS_ERROR → во всех циклах тайм-ауты, сначала проверка FW/тактов, при любой ошибке `_STA` = 0.
Альтернатива: свой драйвер battery miniclass (battc) с тем же I2C внутри — если AML окажется слишком медленным/хрупким.

## 4. Ввод
- USB host (OTG): у lime работает; у J606F VBUS даёт FUSB302 + буст gpio38 — **проверить у владельца** мышь/клавиатуру (возможно, нужен хаб с питанием).
- Клавиатура Lenovo Keyboard Pack (pogo): HID-over-I2C `mcu_pogo@60` (I2C0, irq gpio93) и `@61` (I2C1, irq gpio96), питание/детект gpio 38/41/48/66/67/74.
  Inbox hidi2c.sys подойдёт, но ему нужны **драйвер контроллера I2C (SPB) для GENI** и **драйвер GPIO (TLMM)** с прерываниями — оба писать
  самим (KMDF/SPB-framework, ~2–4 недели работы с отладкой). Тот же I2C-драйвер потом даст mm8013 нормальным драйвером.
- Тач Himax HX83102P (SPI2, zero-flash: прошивку надо заливать при каждом старте) — самый тяжёлый (свой SPI-драйвер GENI + HID-драйвер Himax
  с загрузкой прошивки). Только после I2C/GPIO.
- Кнопки громкости/питания (PMIC PON + TLMM) — через ACPI/GPIO, после драйвера GPIO.

## 5. Инфраструктура (сделана 04.11, без планшета)
- Сборка драйверов Windows ARM64 на хосте: llvm-mingw 20261020 (~/uefi-winarm/tools/llvm-mingw), свой заголовок windrv/include/j606f_nt.h
  (DDK mingw не поддерживает ARM64), импорт ntoskrnl из windrv/lib/ntoskrnl.def (llvm-dlltool), тестовая подпись osslsigncode
  (сертификат ~/uefi-winarm/windrv-cert, приватный ключ только на хосте). `sh windrv/build.sh windrv/hello` → hello.sys (пробный драйвер).
- Агент Windows (windrv/agent): служба `j606fagent` (LocalSystem, авто) → C:\j606f\agent.cmd → один раз выполняет C:\j606f\run.cmd,
  лог в C:\j606f\out. Ставится офлайн из pmOS: файлы через ntfs-3g + j606fagent.reg в SYSTEM-хайв (hivexregedit на хосте).
  Первый сценарий run_01_inventory.cmd: сертификат → загрузка hello.sys → опись устройств/батареи/видео/CPU, ошибки журнала.
- Цикл без рук: слот b (pmOS) активен → pmOS кладёт файлы → `reboot bootloader` → Mac `fastboot boot out/v3/Mu-j606f.img` → Windows
  выполняет run.cmd и (по желанию) `shutdown /r` → pmOS читает C:\j606f\out. Для этого на время разработки активным держим слот b.

## 6. Порядок работ (предложение)
1. С владельцем (10 мин): видно ли рабочий стол, поворот экрана, USB-мышь/клавиатура через OTG; вернуть слот b для отладки.
2. Зарядка: регистры + замер (п. 1). При проблеме — EnableChargerWdog = 0.
3. Агент + hello.sys (проверка всей цепочки драйверов) + опись.
4. Батарея через ACPI (п. 3): сначала DEBUG-UEFI проба состояния тактов/FW SE0/пинов в лог ramoops, потом AML.
5. Яркость/гашение экрана (п. 2), затем I2C/GPIO-драйверы → клавиатура-чехол → тач.

## 7. Готово к запуску (04.11, 13:37) и шаги, когда планшет снова доступен
Готово (на устройстве ещё не запускалось):
- `windrv/probe` → probe.sys (cf3eec2e…71a4): обе службы читают регистры зарядки PMI632 по SPMI; `j606fprobe` — только чтение GCC/TLMM/SE0; `j606fi2c` — включает I2C0 и читает mm8013. Сценарий `agent/run_02_probe.cmd`.
- `acpi/j606f_battery.asl` — черновик батареи для cmbatt (компилируется iasl без замечаний); встраивать после удачного `j606fi2c`.
- `scripts/pmos_win_deploy.sh` — выкладка из pmOS (files / pull-hive / push-hive / results), `scripts/pmos_charger_status.sh` — регистры зарядки.
Шаги:
1. Владелец: что на экране после загрузки из boot_a; поворот; мышь по OTG. Перезагрузка Windows (не «завершение работы» — иначе ntfs-3g не смонтирует) с Громкость− → fastboot.
2. `fastboot set_active b` → `fastboot reboot` → pmOS (initramfs может ждать Power). Слот b активен на время разработки, Windows — `fastboot boot out/v3/Mu-j606f.img`.
3. pmOS: `systemd-inhibit …`; `sudo sh pmos_charger_status.sh` (до); mm8013 charge_now/capacity.
4. Хайв: pull-hive → хост `hivexregedit --merge --prefix 'HKEY_LOCAL_MACHINE\SYSTEM' SYSTEM j606fagent.reg` → push-hive (с sha256 старого). Файлы: `files` с run_01.
5. `reboot bootloader` → `fastboot boot` v3 → Windows под зарядкой ~30 мин → перезагрузка → pmOS: results, сравнение заряда, pmos_charger_status.sh (после).
6. То же с run_02_probe.cmd → probe.txt. Если mm8013 читается — BAT.aml в FD (debug-сборка), тест через `fastboot boot`.

## 8. Статус 17:30
- Шаги 1–4 §7 сделаны иначе, чем планировалось: ввода в Windows нет (тач не работает, USB-C занят Mac) → чистая перезагрузка через скрипт в автозагрузке
  пользователя (windrv/agent/j606f_restart.cmd), агент поставлен в хайв (+ HiberbootEnabled=0). Зарядка: в pmOS сторожевой таймер PMI632 выключен,
  батарея 100 % после Windows. Сейчас Windows выполняет run_03 (опись + probe.sys), ждём результаты в pmOS. Подробно — STATE.md §4–5, LOG.md 17:0x–17:3x.

## 9. Статус 18:15 — зарядка и батарея через PMIC (SPMI), без I2C
- **Зарядка:** наш UEFI v3 (QcomChargerDxe, `EnableChargerWdog = 1`) оставлял включённым сторожевой таймер SMB5 (0x1651 = 0x62, 0x1653 = 0x8c, бит 7 —
  bite выключает заряд) → риск остановки зарядки в Windows. **UEFI v4** (`EnableChargerWdog = 0`): в Windows 0x1651 = 00, 0x1653 = 0c — как в pmOS (run_07).
- **mm8013 из Windows недоступен по FIFO:** QUP0 SE0 только GPI DMA (GENI_IF_DISABLE_RO = 1). §3 (AML на GENI-FIFO, acpi/j606f_battery.asl) — тупик.
- **Батарея — из PMI632:** QG (0x4800) даёт напряжение/ток батареи по SPMI (из Windows: 4423 мВ, −147 мА на зарядке, run_08; pmOS: 4402 мВ при
  mm8013 4400 мВ). % — по таблице OCV из стокового профиля (pc-temp-v1-lut, 25 °C), ёмкость 7518 мА·ч. Состояние — SMB5 0x1006 + USBIN 0x1310 бит 4.
  → acpi/j606f_battery_spmi.asl (BAT0 PNP0C0A + ADP0 ACPI0003, inbox cmbatt, только чтение PMIC) → таблица BATS в **UEFI v5** (= v4 + BATS), тест run_09.
- Ограничения: % по напряжению (без учёта IR-падения: под нагрузкой ниже, на зарядке выше; на зарядке не больше 99 %, пока не taper/terminate у 4,35 В);
  уведомлений (Notify 0x80) нет — Windows опрашивает сама; число циклов/износ — неизвестны (это только mm8013).
- Дальше: поправка на IR (OCV ≈ V + I·R, R из профиля pc-temp-z/ESR), затем дисплей (яркость), частоты CPU, USB host.
