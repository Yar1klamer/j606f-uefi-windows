#!/bin/sh
# Put the J606F Windows agent, drivers and the next test script on the Windows partition from pmOS (ntfs-3g), or move the SYSTEM hive.
# Run on the tablet as root, in ONE ssh command (the ntfs-3g mount dies with the ssh session):
#   sudo sh pmos_win_deploy.sh files DIR RUN.cmd     — DIR holds agent.cmd agent_main.cmd hello.sys probe.sys j606f-test.cer;
#                                                     RUN.cmd becomes C:\j606f\run.cmd (the agent runs it once on the next boot)
#   sudo sh pmos_win_deploy.sh tree DIR             — DIR mirrors C:\j606f (DIR/run.cmd, DIR/drv/*.sys …), copied over it
#   sudo sh pmos_win_deploy.sh pull-hive OUT         — copy C:\Windows\System32\config\SYSTEM to OUT (for hivexregedit on the host)
#   sudo sh pmos_win_deploy.sh push-hive NEW OLDSHA  — replace SYSTEM with NEW only if the current one still has sha256 OLDSHA
#   sudo sh pmos_win_deploy.sh results OUTDIR        — copy C:\j606f\out\* to OUTDIR
# Rollback: C:\j606f is ours (delete it); the hive before push-hive is kept as C:\Windows\System32\config\SYSTEM.j606f-bak.
set -eu
DEV=/dev/sda17
UUID=${WIN_NTFS_UUID:?set WIN_NTFS_UUID to "blkid -s UUID -o value" of your Windows NTFS partition}
MNT=/mnt/win
CMD=${1:-}

[ "$(blkid -s UUID -o value $DEV)" = "$UUID" ] || { echo "$DEV is not the Windows NTFS ($UUID) — stop"; exit 1; }
grep -q " $MNT " /proc/mounts && { echo "$MNT already mounted — stop"; exit 1; }
mkdir -p $MNT
# ntfs-3g refuses a hibernated volume (Windows "fast startup" shutdown). Restart Windows instead of shutting it down.
ntfs-3g $DEV $MNT || { echo "ntfs-3g mount failed (hibernated / not cleanly unmounted? restart Windows, do not shut down)"; exit 1; }
trap 'sync; umount $MNT' EXIT
HIVE=$MNT/Windows/System32/config/SYSTEM

case "$CMD" in
files)
	SRC=$2
	RUN=$3
	mkdir -p $MNT/j606f/out $MNT/j606f/drv
	cp "$SRC/agent.cmd" "$SRC/agent_main.cmd" $MNT/j606f/
	for f in "$SRC"/run_*.cmd; do if [ -f "$f" ]; then cp "$f" $MNT/j606f/; fi; done
	for f in hello.sys probe.sys j606f-test.cer; do
		if [ -f "$SRC/$f" ]; then cp "$SRC/$f" $MNT/j606f/drv/; fi
	done
	cp "$RUN" $MNT/j606f/run.cmd
	sha256sum $MNT/j606f/*.cmd $MNT/j606f/drv/*
	;;
tree)
	# DIR mirrors C:\j606f (e.g. DIR/run.cmd, DIR/drv/x.sys); copied over the existing tree
	SRC=$2
	mkdir -p $MNT/j606f/out $MNT/j606f/drv
	cp -r "$SRC/." $MNT/j606f/
	(cd "$SRC" && find . -type f | while read -r f; do sha256sum "$MNT/j606f/$f"; done)
	;;
pull-hive)
	cp $HIVE "$2"
	sha256sum $HIVE "$2"
	;;
push-hive)
	NEW=$2
	OLD=$3
	[ "$(sha256sum $HIVE | cut -d' ' -f1)" = "$OLD" ] || { echo "SYSTEM changed since pull-hive — stop"; exit 1; }
	[ -f $HIVE.j606f-bak ] || cp $HIVE $HIVE.j606f-bak
	cp "$NEW" $HIVE
	sha256sum $HIVE $HIVE.j606f-bak
	;;
results)
	mkdir -p "$2"
	cp -r $MNT/j606f/out/. "$2/"
	ls -la "$2"
	;;
*)
	echo "usage: files DIR RUN.cmd | tree DIR | pull-hive OUT | push-hive NEW OLDSHA | results OUTDIR"
	exit 1
	;;
esac
