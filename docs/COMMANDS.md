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
# хост: ~/uefi-winarm; правки дерева — scripts/patch_silicium_diag.py (идемпотентно), порт — port/ → Mu-Silicium/
cd ~/uefi-winarm/Mu-Silicium && python3 -I ../patch_silicium_diag.py .
. ../venv/bin/activate && export CLANGPDB_AARCH64_PREFIX=aarch64-linux-gnu-
python3 build_uefi.py -d j606f -r RELEASE   # или -r DEBUG (лог в ramoops, тёплый сброс, MSD 3 мин) → Mu-j606f.img
```

## Запуск UEFI (только RAM) и лог
```bash
ssh user@172.16.42.1 'sudo reboot bootloader'          # из pmOS; или кнопками
fastboot devices              # дождаться, затем ~3 с паузы (иначе usb_write failed)
fastboot boot out/d4/Mu-j606f.img
# DEBUG: ~40 с до pmOS (+3 мин, если стартовал MSD); затем сразу забрать лог:
ssh user@172.16.42.1 'sudo cat /var/lib/systemd/pstore/console-ramoops-0' > raw/uefi_logs/<тест>.log
```
Журнал ядра macOS про USB-накопитель: `/usr/bin/log show --last 20m --style compact --predicate 'process == "kernel"' | grep -i -E "usb|scsi"`
(`log` в zsh — встроенная команда, нужен полный путь).

## Windows: цикл разработки без ввода (с 2026-11-04)
```sh
# pmOS (слот b активен): выложить файлы и сценарий (перезаписывает существующие файлы в C:\j606f)
tar -cf - -C raw payload_X | ssh user@172.16.42.1 'mkdir -p /tmp/j606f && tar -xf - -C /tmp/j606f && sudo sh /tmp/pmos_win_deploy.sh files /tmp/j606f/payload_X /tmp/j606f/payload_X/run_XX.cmd'
ssh user@172.16.42.1 'sudo reboot bootloader'
fastboot boot out/v3/Mu-j606f.img          # Windows: агент выполнит run.cmd и (если в сценарии shutdown /r) вернётся в pmOS
ssh user@172.16.42.1 'sudo sh /tmp/pmos_win_deploy.sh results /tmp/win_out' && scp -r user@172.16.42.1:/tmp/win_out raw/win_runXX
# хайв: pull-hive → хост hivexregedit --merge --prefix 'HKEY_LOCAL_MACHINE\SYSTEM' SYSTEM X.reg → push-hive NEW OLDSHA (только чистый хайв: seq1 = seq2)
```
