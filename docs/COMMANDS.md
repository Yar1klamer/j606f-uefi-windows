# COMMANDS — проверенные команды (UEFI + WoA)

Fastboot: `fastboot`. Python для разбора — `python3 -I`.

## Разбор стокового UEFI (проверено 04.11)
```bash
python3 -I scripts/fv_extract.py <бэкап разделов>/xbl_a.img                    # список FV/файлов
python3 -I scripts/fv_extract.py <бэкап разделов>/xbl_a.img --out raw/xbl_a_fv # извлечь (.efi/.te/.raw, cfg, xml)
```

## Чтение состояния планшета (без записи)
```bash
fastboot devices
fastboot getvar current-slot
```

## Публичное API GitHub (без авторизации, только чтение)
```bash
curl -s "https://api.github.com/repos/Project-Silicium/Mu-Silicium/git/trees/main?recursive=1"
curl -s "https://api.github.com/repos/Project-Aloha/mu_aloha_platforms/contents/Silicon/QC?ref=main"
```

## Сборка UEFI на хосте (проверено 04.11)
```bash
# хост: ~/j606f-build; правки дерева — scripts/patch_silicium_diag.py (идемпотентно), порт — port/ → Mu-Silicium/
cd ~/j606f-build/Mu-Silicium && python3 -I ../patch_silicium_diag.py .
. ../venv/bin/activate && export CLANGPDB_AARCH64_PREFIX=aarch64-linux-gnu-
python3 build_uefi.py -d j606f -r RELEASE   # или -r DEBUG (лог в ramoops, тёплый сброс, MSD 3 мин) → Mu-j606f.img
```

## Запуск UEFI (только RAM) и лог
```bash
ssh user@<tablet-ip> 'sudo reboot bootloader'          # из pmOS; или кнопками
fastboot devices              # дождаться, затем ~3 с паузы (иначе usb_write failed)
fastboot boot out/d4/Mu-j606f.img
# DEBUG: ~40 с до pmOS (+3 мин, если стартовал MSD); затем сразу забрать лог:
ssh user@<tablet-ip> 'sudo cat /var/lib/systemd/pstore/console-ramoops-0' > raw/uefi_logs/<тест>.log
```
Журнал ядра macOS про USB-накопитель: `/usr/bin/log show --last 20m --style compact --predicate 'process == "kernel"' | grep -i -E "usb|scsi"`
(`log` в zsh — встроенная команда, нужен полный путь).

## Windows: цикл разработки без ввода (с 2026-11-04)
```sh
# pmOS (слот b активен): выложить файлы и сценарий (перезаписывает существующие файлы в C:\j606f)
tar -cf - -C raw payload_X | ssh user@<tablet-ip> 'mkdir -p /tmp/j606f && tar -xf - -C /tmp/j606f && sudo sh /tmp/pmos_win_deploy.sh files /tmp/j606f/payload_X /tmp/j606f/payload_X/run_XX.cmd'
ssh user@<tablet-ip> 'sudo reboot bootloader'
fastboot boot out/v3/Mu-j606f.img          # Windows: агент выполнит run.cmd и (если в сценарии shutdown /r) вернётся в pmOS
ssh user@<tablet-ip> 'sudo sh /tmp/pmos_win_deploy.sh results /tmp/win_out' && scp -r user@<tablet-ip>:/tmp/win_out raw/win_runXX
# хайв: pull-hive → хост hivexregedit --merge --prefix 'HKEY_LOCAL_MACHINE\SYSTEM' SYSTEM X.reg → push-hive NEW OLDSHA (только чистый хайв: seq1 = seq2)
```

## Windows: цикл через меню UEFI (с 2026-11-07, слот a = v7; проверено run_20…run_25)
`$BUILD` — хост сборки (адрес — в приватных заметках, не в репозитории). Планшет в pmOS: `ssh user@<tablet-ip>`.
```sh
# 1. Новые файлы ntfs-3g не создаёт → сначала переименовать заготовку (mv), потом tree-копия поверх; всё в одной ssh-команде на каждый шаг
scp -q -r raw/payload_runXX scripts/pmos_win_deploy.sh user@<tablet-ip>:/tmp/
ssh user@<tablet-ip> 'sudo sh -c "ntfs-3g /dev/sda17 /mnt/win && mv /mnt/win/j606f/run.done.cmd /mnt/win/j606f/run.cmd; umount /mnt/win"'
ssh user@<tablet-ip> 'sudo sh /tmp/pmos_win_deploy.sh tree /tmp/payload_runXX' | tee raw/deploy_runXX.log   # печатает sha256 записанного
# 2. Каждый run.cmd первым делом ставит меню по умолчанию на pmOS:
#    mountvol S: /s & < nul set /p "=1" > S:\EFI\Boot\j606fboot.def & mountvol S: /d
#    следующий сценарий в цепочке: copy /y C:\j606f\runNNb.cmd C:\j606f\run.cmd (перед shutdown /r)
# 3. Владелец перезагружает и выбирает Windows в меню (Громкость ±, Питание). Агент: run.cmd → run.running.cmd → run.done.cmd, журнал out\agent.log
# 4. Ждать возврата pmOS (фоном): пока ssh не ответит
for i in $(seq 1 150); do ssh -o ConnectTimeout=4 -o BatchMode=yes user@<tablet-ip> uptime 2>/dev/null && break; sleep 6; done
# 5. Результаты — только чтение (NTFS после сброса грязная): скопировать нужные файлы в /tmp и забрать scp
ssh user@<tablet-ip> 'sudo sh -c "ntfs-3g -o ro /dev/sda17 /mnt/win; cp /mnt/win/j606f/out/X.txt /tmp/; umount /mnt/win; chmod a+r /tmp/X.txt"'
scp user@<tablet-ip>:/tmp/X.txt raw/win_runXX/
iconv -f CP866 -t UTF-8 X.txt   # вывод консольных утилит Windows (часть строк — CP1251)
```
- Бэкап хайва перед изменениями реестра: `ssh … 'sudo sh -c "ntfs-3g -o ro /dev/sda17 /mnt/win; cat /mnt/win/Windows/System32/config/SYSTEM > /tmp/S; umount /mnt/win"'`,
  scp → raw/hive/SYSTEM.YYYYMMDD-pre-runXX; чистый ли: `python3 -I -c "import struct;print(struct.unpack('<II',open(F,'rb').read(12)[4:12]))"` (два числа равны).
- NTFS грязная (ntfs-3g rw отказывает): владелец загружает Windows один раз и ничего не нажимает — devmode-агент сам перезагрузит в pmOS через 300 с.
- Состояние загрузки (только чтение): `sudo qbootctl`; `sudo mount -o ro /dev/sda16 /mnt/esp; ls -la /mnt/esp/EFI/Boot; cat /mnt/esp/EFI/Boot/j606fboot.def`.

## Сборка драйверов Windows
```sh
# legacy/WDM (hello, probe, hidtouch, hidbuttons, dsiprobe): на $BUILD, llvm-mingw + тестовая подпись
ssh $BUILD 'cd ~/j606f-build && sh windrv/build.sh windrv/hidbuttons'          # → windrv/hidbuttons/hidbuttons.sys, печатает sha256
# KMDOD (j606fdsp): исходники примера и WDK — только на $BUILD (~/j606f-build/windrv-wddm/kmdod, ~/j606f-build/dl/wdk)
scp windrv/j606fdsp/{patch_kmdod.sh,j606f_bl.cxx} $BUILD:j606f-build/windrv-wddm/kmdod/
ssh $BUILD 'sh ~/j606f-build/windrv-wddm/build_dsp.sh'                        # патч примера → clang-cl → lld-link /driver → osslsigncode
scp $BUILD:j606f-build/windrv-wddm/kmdod/j606fdsp.sys windrv/j606fdsp/
```

## UEFI в boot_a (проверено 07.11 для v6 и v7)
Сначала `fastboot boot out/vN/Mu-j606f.img` и проверка обеих ОС из меню. Затем из pmOS: бэкап раздела
(`sudo dd if=/dev/disk/by-partlabel/boot_a of=/home/user/boot_a-vOLD.img` + sha256), запись образа `dd … of=/dev/disk/by-partlabel/boot_a conv=fsync`,
сверка `cmp -n <размер образа>`; затем `fastboot set_active a` (или qbootctl). Запасной путь — `fastboot set_active b` (pmOS через ABL).
