#!/bin/sh
# One unattended Windows test cycle from the Mac (tablet in pmOS, reachable over ssh as user@TABLET):
#   sh scripts/mac_win_cycle.sh raw/payload_runNN
# 1. copies scripts/pmos_win_deploy.sh and the payload to the tablet's /tmp (lost on every pmOS reboot — copied every time);
# 2. renames C:\j606f\run.done.cmd -> run.cmd (ntfs-3g can not create files, only rename/overwrite), then "tree" copies the payload over it;
# 3. checks that run.cmd on the NTFS has the payload's sha256; only then sets the ESP boot default to Windows ('0') and reboots.
# Any failure stops before the reboot. The payload's run.cmd must write '1' to the ESP first thing (back to pmOS).
set -eu
T=${TABLET:-user@<tablet-ip>}
P=$1
N=$(basename "$P")
SHA=$(shasum -a 256 "$P/run.cmd" | cut -c1-64)
cd "$(dirname "$0")/.."
scp -q scripts/pmos_win_deploy.sh "$T:/tmp/pmos_win_deploy.sh"
ssh "$T" "rm -rf /tmp/$N"
scp -q -r "$P" "$T:/tmp/$N"
ssh "$T" "test -f /tmp/pmos_win_deploy.sh && test -f /tmp/$N/run.cmd"
ssh "$T" "sudo sh -c 'ntfs-3g /dev/sda17 /mnt/win && { test -f /mnt/win/j606f/run.cmd || mv /mnt/win/j606f/run.done.cmd /mnt/win/j606f/run.cmd; }; r=\$?; umount /mnt/win; exit \$r'"
ssh "$T" "sudo sh /tmp/pmos_win_deploy.sh tree /tmp/$N" > "raw/deploy_${N#payload_}.log" 2>&1
cat "raw/deploy_${N#payload_}.log"
ssh "$T" "sudo sh -c 'ntfs-3g -o ro /dev/sda17 /mnt/win && sha256sum /mnt/win/j606f/run.cmd; umount /mnt/win'" | grep -q "^$SHA " || { echo "run.cmd on NTFS is not $SHA — stop"; exit 1; }
ssh "$T" "sudo sh -c 'mount /dev/sda16 /mnt/esp && printf 0 > /mnt/esp/EFI/Boot/j606fboot.def && sync && cat /mnt/esp/EFI/Boot/j606fboot.def; umount /mnt/esp'"
echo
ssh "$T" "sudo reboot" || true
echo "rebooting into Windows: $N (run.cmd $SHA)"
