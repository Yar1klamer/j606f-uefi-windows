#!/bin/sh
# Ужать pmOS (userdata, вложенная GPT, сектор 4096) до 24 ГиБ и разметить место за ним под Windows (ESP + NTFS).
# Запуск: в debug-shell initramfs pmOS (pmos.debug-shell, root не смонтирован), root-раздел — источник sgdisk (chroot).
#   sh shrink_pmos_for_windows.sh            — проверки + план, без записи
#   sh shrink_pmos_for_windows.sh --write    — выполнить
# Откат разметки: sgdisk --load-backup (<бэкап GPT>/*.sgdisk), данные — <бэкап pmOS>/userdata-backup-20261103.simg.
set -eu
WRITE=${1:-}
UD=/dev/sda15
UD_BYTES=110004875264        # sda15 сейчас
UD_START=3263272             # внешняя GPT, сектора 4К
UD_NEW=6291456               # 24 ГиБ в секторах 4К
UD_END=$((UD_START + UD_NEW - 1))        # 9554727
ESP_START=$((UD_END + 1))                # 9554728
ESP_END=$((ESP_START + 131072 - 1))      # 9685799 (512 МиБ)
WIN_START=$((ESP_END + 1))               # 9685800
WIN_END=30119930                         # последний используемый сектор sda
ROOT_START=124928
# sgdisk -e зеркалит зазор начала (первый используемый = 24) → последний используемый = 6291456 - 24 = 6291432; p2 кончаем на нём («0»)
ROOT_LAST=$((UD_NEW - 24))                  # 6291432 (проверяется после sgdisk)
ROOT_BLOCKS=6166016                         # ext4 по 4К, чуть меньше раздела (6166505); initramfs потом сам дорастит fs до раздела
ROOT_TYPE=B921B045-1DF0-41C3-AF44-4C6F280D3FAE
ROOT_PARTUUID=${ROOT_PARTUUID:?set ROOT_PARTUUID to the PARTUUID of the pmOS root inside userdata (sgdisk -i 2 on the loop device)}
UD_TYPE=1B81E7E6-F50D-419B-A739-2AEEF8DA3335
UD_PARTUUID=${UD_PARTUUID:?set UD_PARTUUID to the PARTUUID of userdata (sgdisk -i 15 /dev/sda)}

say() { echo "== $*"; }

[ "$(blockdev --getsize64 $UD)" = "$UD_BYTES" ] || { echo "размер $UD не $UD_BYTES — уже ужат? стоп"; exit 1; }
[ "$(cat /sys/class/block/sda15/start)" = "$((UD_START * 8))" ] || { echo "старт sda15 не тот — стоп"; exit 1; }
grep -q " /mnt " /proc/mounts && { echo "/mnt занят — стоп"; exit 1; }

L=$(losetup --show -f -P --sector-size 4096 $UD)
say "вложенный диск $L"
blkid ${L}p2
say "e2fsck -f -y ${L}p2"
e2fsck -f -y ${L}p2 || [ $? -le 1 ]
dumpe2fs -h ${L}p2 2>/dev/null | grep -E "Block count|Free blocks|Block size" || true
if [ "$WRITE" != "--write" ]; then
	say "план: resize2fs ${L}p2 $ROOT_BLOCKS; вложенная p2 $ROOT_START..$ROOT_LAST; sda15 $UD_START..$UD_END; esp $ESP_START..$ESP_END; win $WIN_START..$WIN_END"
	losetup -d $L
	exit 0
fi

say "resize2fs ${L}p2 $ROOT_BLOCKS"
resize2fs ${L}p2 $ROOT_BLOCKS
e2fsck -f -n ${L}p2

# sgdisk есть только в root pmOS: монтируем его только для чтения и работаем через chroot
mkdir -p /mnt
mount -o ro ${L}p2 /mnt
mount --bind /dev /mnt/dev
mount -t proc proc /mnt/proc
mount -t sysfs sys /mnt/sys
SG="chroot /mnt sgdisk"

# вложенная GPT: ограниченная 24 ГиБ петля, резервная GPT в конец, p2 до последнего сектора (initramfs потом сам ничего не растянет)
L2=$(losetup --show -f --sector-size 4096 --sizelimit $((UD_NEW * 4096)) $UD)
say "вложенная GPT на $L2"
# p2 сейчас вылезает за 24 ГиБ, поэтому сначала удалить и перенести резервную GPT, потом создать p2 (одним вызовом sgdisk не выходит)
$SG -d 2 -e $L2
$SG -n 2:$ROOT_START:0 -t 2:$ROOT_TYPE -u 2:$ROOT_PARTUUID -c 2:primary $L2
END2=$($SG -i 2 $L2 | sed -n 's/^Last sector: \([0-9]*\).*/\1/p')
[ "$END2" = "$ROOT_LAST" ] || { echo "p2 кончается на $END2, ждал $ROOT_LAST — стоп до внешней GPT"; exit 1; }
$SG -v $L2
$SG -p $L2
losetup -d $L2

say "внешняя GPT sda"
$SG -d 15 -n 15:$UD_START:$UD_END -t 15:$UD_TYPE -u 15:$UD_PARTUUID -c 15:userdata \
	-n 16:$ESP_START:$ESP_END -t 16:EF00 -c 16:esp \
	-n 17:$WIN_START:$WIN_END -t 17:0700 -c 17:win /dev/sda
$SG -v /dev/sda
$SG -p /dev/sda

umount /mnt/sys /mnt/proc /mnt/dev /mnt
losetup -d $L
sync
say "готово"
