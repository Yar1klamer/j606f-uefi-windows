@echo off
rem J606F test 03: inventory (run_01) + probe (run_02), then restart; slot b is active, so the tablet comes back to pmOS by itself.
powercfg /h off > C:\j606f\out\powercfg_h_off.log 2>&1
call C:\j606f\run_01_inventory.cmd > C:\j606f\out\run01.log 2>&1
call C:\j606f\run_02_probe.cmd > C:\j606f\out\run02.log 2>&1
shutdown /r /t 20 /c "j606f tests done"
