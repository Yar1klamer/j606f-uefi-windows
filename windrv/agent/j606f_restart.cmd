@echo off
rem J606F: one clean restart without input (no touch in Windows), from User's Startup folder; deletes itself.
rem ntfs-3g on the tablet cannot create new files on this volume (free MFT records unreadable), so Windows creates
rem empty placeholders here that pmOS later overwrites.
if exist C:\j606f\out\restart.done goto gone
mkdir C:\j606f
mkdir C:\j606f\out
mkdir C:\j606f\drv
for %%f in (agent.cmd agent_main.cmd run.cmd run_01_inventory.cmd run_02_probe.cmd run_03_all.cmd spare1.cmd spare2.cmd spare3.cmd spare4.cmd) do type nul > C:\j606f\%%f
for %%f in (hello.sys probe.sys j606f-test.cer spare1.sys spare2.sys spare3.sys spare4.sys) do type nul > C:\j606f\drv\%%f
for %%f in (spare1.txt spare2.txt spare3.txt) do type nul > C:\j606f\out\%%f
echo restart %date% %time% > C:\j606f\out\restart.done
dir /s C:\j606f > C:\j606f\out\restart_dir.txt 2>&1
whoami /all >> C:\j606f\out\restart_dir.txt 2>&1
shutdown /r /t 60 /c "j606f: clean restart"
:gone
del "%~f0"
