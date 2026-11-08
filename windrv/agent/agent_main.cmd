@echo off
rem J606F agent: runs C:\j606f\run.cmd once (renamed to run.done.cmd) and logs to C:\j606f\out. Files are put here from pmOS (ntfs-3g).
rem Development mode (C:\j606f\devmode exists): with nothing to run, restart after 5 minutes so the tablet goes back to pmOS without input.
rem Bluetooth mode (C:\j606f\btmode exists, 08.11): with nothing to run, start the Bluetooth serial agent (btagent.ps1) and stay in Windows -
rem the Mac works over Bluetooth (scripts/btsh.py), a crash reboots into Windows again (boot default 0) and the agent comes back by itself.
set OUT=C:\j606f\out
echo [agent] boot %date% %time% >> %OUT%\agent.log
ping -n 30 127.0.0.1 > nul
if not exist C:\j606f\run.cmd goto idle
move /y C:\j606f\run.cmd C:\j606f\run.running.cmd > nul
echo [agent] run.cmd start %date% %time% >> %OUT%\agent.log
call C:\j606f\run.running.cmd > %OUT%\run.log 2>&1
move /y C:\j606f\run.running.cmd C:\j606f\run.done.cmd > nul
echo [agent] run.cmd done %date% %time% >> %OUT%\agent.log
goto end
:idle
if exist C:\j606f\btmode if exist C:\j606f\btagent.ps1 goto btmode
if not exist C:\j606f\devmode goto end
echo [agent] devmode, no run.cmd: restart in 300 s %date% %time% >> %OUT%\agent.log
rem the menu default goes back to pmOS first, otherwise a Windows default loops through this restart
mountvol S: /s
< nul set /p "=1" > S:\EFI\Boot\j606fboot.def
mountvol S: /d
shutdown /r /t 300 /c "j606f devmode: nothing to run, back to pmOS in 5 min"
:btmode
echo [agent] btmode: Bluetooth agent %date% %time% >> %OUT%\agent.log
rem btwatch.ps1 (08.11): watchdog that keeps btagent.ps1 alive (heartbeat, restart, reboot as the last resort)
if exist C:\j606f\btwatch.ps1 powershell -NoProfile -ExecutionPolicy Bypass -File C:\j606f\btwatch.ps1
if not exist C:\j606f\btwatch.ps1 powershell -NoProfile -ExecutionPolicy Bypass -File C:\j606f\btagent.ps1
echo [agent] btmode: agent ended %date% %time% >> %OUT%\agent.log
:end
