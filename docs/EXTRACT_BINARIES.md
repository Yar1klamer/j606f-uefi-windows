# Драйверы Qualcomm и DTB — со своего планшета

В репозитории нет ни одного бинарника Lenovo/Qualcomm. Для сборки UEFI нужны DXE-драйверы и конфиги из **стокового `xbl_a` вашего же планшета**
(версии совпадают с вашими TZ/XBL) и стоковый DTB. Все шаги ниже — **только чтение** разделов.

## 1. Снять дампы (только чтение)
Из pmOS (или любого Linux на планшете с root):
```sh
# имена разделов: ls -l /dev/disk/by-partlabel/
sudo dd if=/dev/disk/by-partlabel/xbl_a  of=xbl_a.img  bs=4096
sudo dd if=/dev/disk/by-partlabel/boot_a of=boot_a.img bs=4096   # стоковый boot (для DTB), если в boot_a ещё Android
sha256sum xbl_a.img boot_a.img
```
**Не записывайте в эти разделы ничего.** `xbl`/`abl` испорченные — это кирпич, восстанавливаемый только через EDL.

## 2. Разобрать xbl_a
```sh
python3 -I scripts/fv_extract.py xbl_a.img                 # список FV и файлов
python3 -I scripts/fv_extract.py xbl_a.img --out xbl_a_fv  # .efi/.te/.depex/.raw + manifest.json
```
`fv_extract.py` — свой разборщик PI Firmware Volume без внешних пакетов (LZMA и gzip-секция Qualcomm `1D301FE9-…`).
На J606F: внешний FV на смещении 0x74520, внутренний — в gzip-секции, всего ~160 файлов.

## 3. Патчи драйверов (как у Silicium для lime)
```sh
python3 -I scripts/patch_drivers.py xbl_a_fv drv_patched
```
- `ButtonsDxe` — кнопка Питание даёт Enter (вместо SCAN_SUSPEND);
- `UsbMsdDxe` — Mass Storage «несъёмный» (RMB = 0);
- `ClockDxe` — убран ресурс `/clk/mmrt`, иначе экран гаснет.
Каждый шаблон должен найтись ровно один раз, иначе скрипт останавливается (другая версия драйвера — проверьте вручную).

## 4. Собрать Binaries/j606f
```sh
python3 -I scripts/gen_binaries.py <Mu-Silicium>/Binaries/lime xbl_a_fv drv_patched <Mu-Silicium>/Binaries/j606f
```
INF lime сопоставляются вашим драйверам по FILE_GUID (на J606F совпали все, кроме MiToken Xiaomi — он не нужен).
Затем в `Binaries/j606f/RawFiles/QcomChargerCfg.cfg` замените `EnableChargerWdog = 1` на `EnableChargerWdog = 0` — иначе в Windows
сторожевой таймер зарядника SMB5 остаётся включённым и зарядка останавливается.

## 5. DTB
`Resources/DTBs/j606f.dtb` — стоковый DTB из `boot_a.img` (секция DTB boot-образа), тот, который выбирает загрузчик Lenovo для вашей ревизии
(на ревизии Himax — DTB с индексом 1). Достаточно разбить секцию по сигнатуре `d0 0d fe ed` и взять нужный; проверить можно по `model`/`qcom,board-id`.
Mainline-DTB сюда не подходит.

Всё полученное здесь — **только для себя**: не публикуйте и не прикладывайте к релизам.
