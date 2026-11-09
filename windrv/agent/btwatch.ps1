# J606F: watchdog for the Bluetooth agent (btagent.ps1). Started by agent_main.cmd in Bluetooth mode instead of the agent itself.
#  - starts btagent.ps1 as a child process and starts it again whenever it exits (RESTART command, crash);
#  - heartbeat C:\j606f\out\btagent.hb (agent: "start", "rx" on every received line, "busy" while a command runs; the Mac pings every 20 s):
#    no heartbeat for 180 s -> kill the agent's process tree and start a new one (reopens the Bluetooth COM port);
#  - after contact was made in this boot, 3 restarts in a row without contact coming back and C:\j606f\btwatch.reboot present -> shutdown /r /f
#    (boot default 0: Windows comes back with a fresh stack). Without contact since boot (Mac off) it never reboots, so no reboot loops.
#  - BT UART 3 Mbps (C:\j606f\drv\bt3m.on, btbus v8) is the risky part: no contact for 10 min after start with bt3m.on present, or any
#    watchdog reboot -> bt3m.on is renamed to bt3m.off first (next boot at the proven 115200), reboot. Only once: without bt3m.on no such reboot.
# Log: C:\j606f\out\btwatch.txt.
$log = 'C:\j606f\out\btwatch.txt'
$hb = 'C:\j606f\out\btagent.hb'
function L($m) { try { Add-Content -Path $log -Value ("{0:HH:mm:ss} {1}" -f (Get-Date), $m) } catch {} }
$env:J606F_WATCHED = '1'
L "=== btwatch start (pid $PID)"
$p = $null; $contact = $false; $restarts = 0; $t0 = Get-Date
$fast = 'C:\j606f\drv\bt3m.on'
function FastOff { if (Test-Path $fast) { try { Move-Item -Force $fast 'C:\j606f\drv\bt3m.off'; L "bt3m.on -> bt3m.off (next boot 115200)" } catch { L "bt3m rename failed: $_" } } }
while ($true) {
    try {
        if (-not $p -or $p.HasExited) {
            if ($p) { L "agent exited" }
            try { [IO.File]::WriteAllText($hb, 'start') } catch {}
            $p = Start-Process powershell.exe -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File C:\j606f\btagent.ps1' -WindowStyle Hidden -PassThru
            L "agent started, pid $($p.Id)"
            Start-Sleep 15
            continue
        }
        $f = Get-Item $hb -ErrorAction SilentlyContinue
        $age = 0; $st = ''
        if ($f) { $age = ((Get-Date) - $f.LastWriteTime).TotalSeconds; try { $st = [IO.File]::ReadAllText($hb) } catch {} }
        if ($st -ne 'start' -and $age -lt 180) {
            if (-not $contact) { L "contact" }
            $contact = $true; $restarts = 0
        }
        if (-not $contact -and ((Get-Date) - $t0).TotalSeconds -ge 600 -and (Test-Path $fast)) {
            L "no contact for 10 min with bt3m.on -> 115200 and reboot"
            FastOff
            if (Test-Path 'C:\j606f\drv\btbus.boot') { Remove-Item -Force 'C:\j606f\drv\btbus.boot'; L "btbus.boot removed (start at 3 Mbps did not finish)" }
            & shutdown.exe /r /f /t 10 /c "j606f btwatch: no contact at 3 Mbps, back to 115200"
            Start-Sleep 120
            continue
        }
        if ($age -ge 180) {
            L ("no heartbeat for {0:N0} s ({1}) -> restart agent" -f $age, $st)
            & taskkill.exe /f /t /pid $p.Id 2>&1 | Out-Null
            $p = $null
            if ($contact) { $restarts++ }
            if ($contact -and $restarts -ge 3 -and (Test-Path 'C:\j606f\btwatch.reboot')) {
                L "3 restarts without contact -> reboot"
                FastOff
                & shutdown.exe /r /f /t 10 /c "j606f btwatch: Bluetooth agent lost contact"
                Start-Sleep 120
            }
            continue
        }
    } catch { L "watch error: $_" }
    Start-Sleep 15
}
