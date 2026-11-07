# Windows на J606F: как она установлена и как работает загрузка

Документ описывает итоговую схему: какая Windows стоит на планшете, где лежат её разделы, как она загружается и почему выбран такой путь.
Пошаговые команды для повторения — в [INSTALL_WINDOWS.md](../INSTALL_WINDOWS.md).

## 1. Итог
- Установлена **Windows 11 Pro ARM64, сборка 22621 (22H2), язык ru-RU**. Образ собран из ISO 22621 (UUP dump), без обновлений на Linux.
- Установка выполнена **из установленного postmarketOS на том же устройстве, без ПК с Windows**.
- Загрузка выбирается меню J606fBoot ([menu.md](menu.md)). Windows грузится через `bootmgfw.efi` из ESP.
- Windows 24H2 и новее на SD662 не стартуют (ARMv8.0), поэтому выбрана 22H2.

## 2. Разметка LUN0 (UFS) после установки
| Раздел | Размер | Тип | Назначение |
|---|---|---|---|
| sda1…sda14 | — | Qualcomm / Lenovo | не трогались: persist, modem, fsg, keystore и др. |
| sda15 `userdata` | 24 ГиБ | Linux | pmOS (вложенная GPT: pmOS_boot и pmOS_root, ~22,7 ГиБ) |
| sda16 `esp` | 512 МиБ | EF00, FAT32 | загрузчики UEFI, меню, BCD Windows, журналы меню и WinPE |
| sda17 `win` | 77,9 ГиБ | 0700, NTFS | Windows (`C:`) |

Пространство для Windows получено **ужатием pmOS**: `resize2fs` уменьшил корневую файловую систему pmOS, после чего вложенная и внешняя GPT
сдвинуты. Скрипт — `scripts/shrink_pmos_for_windows.sh`. Перед этим делается бэкап: образ `userdata` и таблицы разделов.

## 3. Как попала Windows на устройство
1. **Развёртывание.** Образ `install.wim` (индекс Pro) передаётся потоком `wimlib-imagex apply` в NTFS-раздел `sda17` в режиме NTFS-3G.
   Предупреждения о расширенных атрибутах (`$CI.CATALOGHINT` и др.) безвредны.
2. **Защита прочих разделов.** В офлайн-хайве SYSTEM задано `SanPolicy = 4` (`partmgr`). Тогда Windows не монтирует ни один внутренний диск
   кроме своего. `modem_b` проверен по хешу после установки: не изменён.
3. **Загрузчик.** `bcdboot` выполняется в **WinPE**, который загружается тем же UEFI с ESP. `scripts/winpe/j606f.cmd` находит `\Windows`,
   создаёт BCD на ESP, выставляет `testsigning on` и `nointegritychecks on`, пишет лог `S:\j606f_pe.log` и перезагружает устройство.
   Файлы WinPE после этого удаляются с ESP.
4. **Первая загрузка.** `scripts/winpe/unattend.xml` кладётся в `W:\Windows\Panther`: язык ru-RU, локальная учётная запись `User` без пароля,
   автовход, пропуск OOBE, `SanPolicy = 4` в фазе specialize.
5. **Постоянный выбор ОС.** Меню J606fBoot находится в ESP как `bootaa64.efi`, исходный загрузчик — рядом как `bootaa64.windows.efi`.

## 4. Как загружается Windows сейчас
- UEFI v7 в `boot_a` → меню в ESP → `bootmgfw.efi` → `winload.efi` → Windows.
- Выбор по умолчанию хранится в `\EFI\Boot\j606fboot.def`. Управление: Громкость ± выбирает, Питание загружает.
- Для возврата в pmOS: перезагрузка Windows → меню → Linux, или `fastboot set_active b` для загрузки через стоковый ABL.

## 5. Что важно помнить при обслуживании
- **Выключать Windows только перезагрузкой.** Быстрый запуск (hiberboot) оставляет NTFS в спящем состоянии, и ntfs-3g из pmOS её не монтирует.
  Агент выставляет `HiberbootEnabled = 0` и `powercfg /h off`.
- После принудительного выключения NTFS «грязная»: ntfs-3g монтирует её только для чтения. Лучше одна штатная загрузка Windows;
  крайний случай — `ntfsfix`.
- Хайв SYSTEM менять только в «чистом» состоянии (seq1 = seq2 в заголовке), иначе Windows проиграет журнал поверх изменений.
- Часы Windows стоят на 2022 годе: RTC не выставляется. Это ограничение UEFI, а не Windows.
- Сон и USB device-mode в Windows не работают; планшет не виден с Mac по USB.

## 6. Что не работает в Windows
Звук, Wi-Fi, Bluetooth, GPU-ускорение, камеры, датчики, USB host, перо, яркость с ползунком, часы, сон. Подробно — в
[windows_drivers_plan.md](windows_drivers_plan.md) и [windows_gpu.md](windows_gpu.md).
