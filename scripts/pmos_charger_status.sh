#!/bin/sh
# J606F, pmOS, read only: battery (mm8013), pmi632-charger driver view and SMB5 registers of PMI632 (SPMI sid 2) incl. the charger watchdog.
# Run as root:  sudo sh pmos_charger_status.sh      (writes nothing; debugfs regmap is only read)
date
for p in /sys/class/power_supply/*; do
	echo "== $(basename "$p")"
	for f in status capacity voltage_now current_now charge_now charge_full online charge_type usb_type health input_current_limit \
		constant_charge_current constant_charge_voltage cycle_count temp; do
		[ -r "$p/$f" ] && echo "  $f = $(cat "$p/$f" 2>/dev/null)"
	done
done
mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug
RM=""
for d in /sys/kernel/debug/regmap/*; do
	case "$(basename "$d")" in *-02|*02-*|*pmic@2*|*spmi*-02*) RM=$d; break ;; esac
done
[ -n "$RM" ] || { echo "regmap for PMI632 (sid 2) not found:"; ls /sys/kernel/debug/regmap/; exit 0; }
echo "== regmap $RM"
# address: name (smb5-reg.h, Lenovo kernel source)
grep -E "^(1006|1007|1042|1051|1107|1108|110a|110b|1306|1310|1610|1643|1648|1651|1653|1670):" "$RM/registers" | while IFS=': ' read -r a v; do
	case $a in
		1006) n="BATTERY_CHARGER_STATUS_1 (bits2:0 charger state: 0 trickle 1 pre 2 fast 3 fullon 4 taper 5 term 6 inhibit 7 disabled)" ;;
		1007) n="BATTERY_CHARGER_STATUS_2" ;;
		1042) n="CHARGING_ENABLE_CMD (bit0)" ;;
		1051) n="CHGR_CFG2" ;;
		1107) n="ICL_STATUS (x25 mA)" ;;
		1108) n="AICL_ICL_STATUS" ;;
		110a) n="AICL_STATUS" ;;
		110b) n="POWER_PATH_STATUS" ;;
		1306) n="USBIN APSD/STATUS" ;;
		1310) n="USBIN INT_RT_STS" ;;
		1610) n="MISC INT_RT_STS (wdog bark/bite bits)" ;;
		1643) n="BARK_BITE_WDOG_PET" ;;
		1648) n="MISC_SMB_EN_CMD" ;;
		1651) n="WD_CFG (bit7 AFP, bit6 bark int, bit1 timer en on plugin, bit0 wdog timer en)" ;;
		1653) n="SNARL_BARK_BITE_WD_CFG (bit7 bite disables charging, 6:4 snarl, 3:2 bark, 1:0 bite)" ;;
		1670) n="MISC_THERMREG_SRC_CFG" ;;
		*) n="" ;;
	esac
	echo "  0x$a = 0x$v  $n"
done
