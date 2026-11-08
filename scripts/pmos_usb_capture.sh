#!/bin/sh
# Только чтение: ждёт роль host на Type-C (до 15 мин), снимает состояние USB в /tmp/usbcap.
out=/tmp/usbcap; mkdir -p $out
snap() { # $1 = метка
  d=$out/$1; mkdir -p $d
  cat /sys/class/typec/port0/data_role /sys/class/typec/port0/power_role > $d/typec.txt 2>&1
  ls /sys/class/typec/ >> $d/typec.txt 2>&1
  cat /sys/class/usb_role/*/role > $d/role.txt 2>&1
  cat /sys/kernel/debug/usb/4e00000.usb/mode /sys/kernel/debug/usb/4e00000.usb/regdump > $d/dwc3_regdump.txt 2>&1
  for p in /sys/kernel/debug/usb/xhci/*/ports/port*; do echo "$p $(cat $p/portsc 2>&1)"; done > $d/portsc.txt 2>&1
  cat /sys/kernel/debug/usb/tcpm-0-0022 > $d/tcpm.txt 2>&1
  grep -i -E "gpio38|gpio97" /sys/kernel/debug/gpio > $d/gpio.txt 2>&1
  for s in /sys/class/power_supply/*; do echo "$s online=$(cat $s/online 2>/dev/null) type=$(cat $s/type 2>/dev/null)"; done > $d/psy.txt
  lsusb > $d/lsusb.txt 2>&1; lsusb -t >> $d/lsusb.txt 2>&1
  dmesg | tail -150 > $d/dmesg.txt
  cat /sys/kernel/debug/regulator/regulator_summary > $d/regulators.txt 2>&1
  cat /sys/kernel/debug/usb/devices > $d/usbdevices.txt 2>&1
}
snap before
i=0
while [ $i -lt 900 ]; do
  r=$(cat /sys/class/typec/port0/data_role 2>/dev/null)
  case "$r" in *"[host]"*) sleep 15; snap host; echo done > $out/DONE; exit 0;; esac
  sleep 2; i=$((i+2))
done
snap timeout; echo timeout > $out/DONE
