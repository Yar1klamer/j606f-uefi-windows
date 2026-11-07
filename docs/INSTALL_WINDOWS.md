# Установка Windows 11 ARM64 из pmOS (без ПК)

Проверено один раз на TB-J606F 6/128 с установленной pmOS (userdata = вложенная GPT: pmOS_boot + pmOS_root, сектор UFS 4096 Б).
Итог: pmOS ужата до 24 ГиБ, за ней ESP 512 МиБ и NTFS ~78 ГиБ под Windows. **Это меняет разметку LUN0 — сначала бэкап.**

Что нужно: собранный UEFI ([BUILD_UEFI.md](BUILD_UEFI.md)), образ **Windows 11 22H2/23H2 ARM64** (свой ISO или собранный через UUP dump;
24H2+ на SD662 не стартует), Linux-машина рядом (для `wimlib`/`hivex`), USB-сеть с pmOS (`ssh user@172.16.42.1`).

## 0. Бэкап
- Вся userdata: `fastboot`-совместимый sparse-образ (как в репозитории pmOS, `scripts/backup_userdata.sh`) — откат `fastboot flash userdata …`.
- GPT LUN0 и вложенная GPT userdata: `sgdisk --backup=sda.sgdisk /dev/sda`, то же для петли userdata; первые 6 и последние 5 секторов.

## 1. Ужать pmOS и разметить место
`scripts/shrink_pmos_for_windows.sh` — для 128-ГБ модели (все размеры и секторы в шапке скрипта; для другой модели пересчитайте).
PARTUUID берутся с вашего планшета и передаются через переменные `ROOT_PARTUUID`, `UD_PARTUUID`.
1. Загрузить pmOS с `pmos.debug-shell` в cmdline (оболочка initramfs до монтирования root; `telnet 172.16.42.1 23`), только через `fastboot boot`.
2. `sh shrink_pmos_for_windows.sh` — проверки и план без записи; `--write` — e2fsck, resize2fs, вложенная GPT, затем внешняя:
   `userdata` 24 ГиБ (тот же type/PARTUUID/имя), `esp` 512 МиБ (EF00), `win` — остаток (0700).
3. Форматирование: `mkfs.vfat -F 32 -S 4096 -n ESP /dev/sda16`, `mkntfs -Q -s 4096 -L Windows /dev/sda17`
   (пакеты Alpine: `sgdisk`, `wimlib`, `ntfs-3g-progs`, `dosfstools`).

Откат разметки: `sgdisk --load-backup=sda.sgdisk /dev/sda` (+ вложенная таблица); данные — из бэкапа userdata.

## 2. Применить install.wim
На Linux-машине экспортировать нужный индекс в pipable WIM и подать его потоком на планшет (на планшете мало места):
```sh
wimlib-imagex export install.wim 1 install_pipable.wim --pipable
cat install_pipable.wim | ssh user@172.16.42.1 'sudo wimlib-imagex apply - 1 /dev/sda17'
```
Предупреждение «Ignoring extended attributes» в режиме NTFS-3G — ожидаемо, Windows с этим грузится.
`scripts/winpe/unattend.xml` (локальная учётка `User` без пароля, пропуск OOBE, `SanPolicy=4`) → `W:\Windows\Panther\unattend.xml`.
Язык, часовой пояс и имя компьютера в нём поменяйте на свои.

## 3. Защитить прочие разделы: SanPolicy=4
Windows видит все LUN UFS, в т.ч. `modem_b` (FAT). Чтобы она ничего не монтировала, кроме своего диска, — офлайн в хайве SYSTEM
`ControlSet001\Services\partmgr\Parameters\SanPolicy = 4`: скопировать `W:\Windows\System32\config\SYSTEM` на Linux,
`hivexregedit --merge --prefix 'HKEY_LOCAL_MACHINE\SYSTEM' SYSTEM sanpolicy.reg`, вернуть. Править только «чистый» хайв
(в заголовке последовательности seq1 = seq2, смещения 4 и 8). После установки сверьте sha256 `modem_*` с бэкапом.

## 4. Загрузчик: WinPE + bcdboot
bcdboot есть только в Windows, поэтому один раз грузится WinPE с ESP:
1. Из того же ISO на ESP: `efi/`, `boot/boot.sdi`; `sources/boot.wim` — индекс WinPE с добавленными
   `scripts/winpe/winpeshl.ini` и `scripts/winpe/j606f.cmd` (`wimlib-imagex update`, затем `--boot`).
2. `fastboot boot Mu-j606f.img` → UEFI находит `\EFI\Boot\bootaa64.efi` → WinPE → `j606f.cmd`: находит `\Windows`, удаляет файлы WinPE с ESP,
   `bcdboot <диск>:\Windows /s S: /f UEFI`, `testsigning on`, `nointegritychecks on`, лог `S:\j606f_pe.log`, `wpeutil reboot`.
3. Проверить лог из pmOS (ESP монтируется как vfat).

## 5. Первая загрузка
`fastboot boot Mu-j606f.img` → Windows Setup (specialize) → перезагрузка в pmOS (UEFI был только в RAM) → ещё раз `fastboot boot` → OOBE по unattend
→ рабочий стол. Ввода в Windows нет: всё, что нужно сделать внутри, делается через агент (`windrv/agent/`, `scripts/pmos_win_deploy.sh`).

Особенности:
- Выключайте Windows **перезагрузкой**: после «Завершения работы» с быстрым запуском NTFS спящая и ntfs-3g её не смонтирует
  (агент ставит `HiberbootEnabled=0` и `powercfg /h off`).
- `scripts/pmos_win_deploy.sh` проверяет UUID NTFS — задайте `WIN_NTFS_UUID` (`blkid -s UUID -o value /dev/sda17`).

## Вариант с ПК на Windows (не проверен на J606F)
Без ОС UEFI сам поднимает USB Mass Storage (LUN0 UFS). С ПК на Windows: diskpart — диск ~115 ГБ **по размеру**, только разделы `esp`/`win`,
`dism /apply-image`, `bcdboot W:\Windows /s S: /f UEFI`, `bcdedit … testsigning on / nointegritychecks on` — как в гайдах Silicium для lime.
macOS с этим Mass Storage не работает (MODE SENSE зависает).
