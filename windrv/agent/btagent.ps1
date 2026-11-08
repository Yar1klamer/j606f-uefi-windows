# J606F Bluetooth serial agent: remote shell for the Mac over Bluetooth SPP, so tests do not need a reboot into pmOS.
# Tries every COM port (the outgoing "Standard Serial over Bluetooth link" port that Windows creates after pairing with the Mac connects to the
# Mac's /dev/tty.Bluetooth-Incoming-Port; an incoming port also works). On open it sends "HELLO j606f"; then line protocol, LF:
#   PING                         -> PONG, then <EOT>END 0
#   CMD <command line>           -> output of cmd /c (UTF-8), then  <EOT>END <exit code>
#   PS <powershell>              -> output, then <EOT>END 0
#   PUT <path> <bytes>           -> base64 lines until a line "." ; writes the file; -> <EOT>END <bytes written>
#   PUTA <path> <bytes>          -> same, appends (btsh.py sends big files in 256 KiB pieces, a lost piece is sent again)
#   GET <path>                   -> base64 lines (76 chars), then <EOT>END <bytes>
#   BYE                          -> closes the port and waits for the next connection
#   RESTART                      -> starts C:\j606f\btagent.ps1 anew (after an update) and exits
# Log: C:\j606f\out\btagent.txt (commands only, no output). No network, nothing listens on IP.
# Heartbeat: C:\j606f\out\btagent.hb is rewritten on every received line ("rx") and every 5 s while a command runs ("busy");
# btwatch.ps1 restarts this agent when it goes stale (08.11: an endless wait inside PUT stopped all work until a manual reboot).
# CMD and PS run in a child process with a time limit (default 600 s, "CMDT <s> ..." / "PST <s> ..."), so a hung command cannot hang the agent.
$ErrorActionPreference = 'Continue'
$log = 'C:\j606f\out\btagent.txt'
$EOT = [char]4
$hb = 'C:\j606f\out\btagent.hb'
function L($m) { try { Add-Content -Path $log -Value ("{0:HH:mm:ss} {1}" -f (Get-Date), $m) } catch {} }
function Beat($s) { try { [IO.File]::WriteAllText($hb, $s) } catch {} }

function ReadOut($o, $e) {
    $t = ''
    foreach ($f in @($o, $e)) { try { $x = [IO.File]::ReadAllText($f, [Text.Encoding]::UTF8); if (-not $x.StartsWith('#< CLIXML')) { $t += $x } } catch {} }   # CLIXML = powershell host progress noise
    return $t
}
# child process with a time limit; heartbeat while it runs; the whole process tree is killed on timeout
function RunProc($exe, $argline, $limit) {
    $o = Join-Path $env:TEMP 'btagent.out'; $e = Join-Path $env:TEMP 'btagent.err'
    Remove-Item $o, $e -ErrorAction SilentlyContinue
    $p = Start-Process $exe -ArgumentList $argline -NoNewWindow -PassThru -RedirectStandardOutput $o -RedirectStandardError $e
    $null = $p.Handle
    $t = 0
    while (-not $p.WaitForExit(5000)) {
        $t += 5; Beat 'busy'
        if ($t -ge $limit) {
            & taskkill.exe /f /t /pid $p.Id 2>&1 | Out-Null
            L "timeout after $limit s, killed"
            return @(("TIMEOUT after $limit s, process killed`n" + (ReadOut $o $e)), -999)
        }
    }
    $p.WaitForExit()
    return @((ReadOut $o $e), $p.ExitCode)
}
function Limit([ref]$c, $prefix) {
    # "<prefix>T <seconds> <rest>" -> rest, seconds; else 600
    $s = $c.Value; $i = $s.IndexOf(' ')
    if ($i -gt 0) { $n = 0; if ([int]::TryParse($s.Substring(0, $i), [ref]$n)) { $c.Value = $s.Substring($i + 1); return $n } }
    return 600
}

function Send($sp, [string]$s) { $sp.Write($s.Replace("`r`n", "`n")) }
function Done($sp, $rc) { Send $sp ("`n" + $EOT + "END $rc`n") }

function Serve($sp) {
    Send $sp "HELLO j606f $env:COMPUTERNAME`n"
    $idle = 0
    while ($sp.IsOpen) {
        try { $line = $sp.ReadLine(); $idle = 0; Beat 'rx' } catch [System.TimeoutException] {
            # keepalive: a dead Bluetooth link does not end ReadLine, a write to it fails -> reopen (bridge restarts, 08.11)
            $idle += 2
            if ($idle -ge 30) {
                $idle = 0
                try { $sp.Write([string][char]0) } catch { L "keepalive failed: $($_.Exception.Message)"; return }
            }
            continue
        } catch { L "read error: $_"; return }
        $line = $line.TrimEnd("`r")
        if ($line -eq '') { continue }
        if ($line -eq 'PING') { Send $sp 'PONG'; Done $sp 0; continue }
        if ($line -eq 'BYE') { L 'bye'; return }
        if ($line -eq 'RESTART') {
            # start the (updated) script anew and end this process; the new one opens the port after this one has closed it
            L 'restart'
            # under btwatch.ps1 the watchdog starts the new agent; standalone, start it ourselves
            if ($env:J606F_WATCHED -ne '1') {
                Start-Process powershell -WindowStyle Hidden -ArgumentList '-NoProfile -ExecutionPolicy Bypass -Command "Start-Sleep 5; & C:\j606f\btagent.ps1"'
            }
            Done $sp 0
            Start-Sleep 1
            try { $sp.Close() } catch {}
            [Environment]::Exit(0)
        }
        if ($line.StartsWith('CMD ') -or $line.StartsWith('CMDT ')) {
            $c = $line.Substring($line.IndexOf(' ') + 1); $lim = 600
            if ($line.StartsWith('CMDT ')) { $lim = Limit ([ref]$c) 'CMD' }
            L "cmd: $c"
            $r = RunProc 'cmd.exe' "/d /c chcp 65001 >nul & $c" $lim
            Send $sp $r[0]; Done $sp $r[1]; continue
        }
        if ($line.StartsWith('PS ') -or $line.StartsWith('PST ')) {
            $c = $line.Substring($line.IndexOf(' ') + 1); $lim = 600
            if ($line.StartsWith('PST ')) { $lim = Limit ([ref]$c) 'PS' }
            L "ps: $c"
            $script = "[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding `$false; `$ProgressPreference = 'SilentlyContinue'; & { $c } 2>&1 | Out-String -Width 200"
            $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($script))
            $r = RunProc 'powershell.exe' "-NoProfile -ExecutionPolicy Bypass -EncodedCommand $enc" $lim
            Send $sp $r[0]; Done $sp $r[1]; continue
        }
        if ($line.StartsWith('PUT ') -or $line.StartsWith('PUTA ')) {
            $append = $line.StartsWith('PUTA '); $a = $line.Substring($line.IndexOf(' ') + 1); $i = $a.LastIndexOf(' '); $path = $a.Substring(0, $i); $n = [int]$a.Substring($i + 1)
            L "put: $path ($n)"
            $sb = New-Object System.Text.StringBuilder
            $quiet = 0; $ok = $false
            while ($quiet -lt 30) {                     # 30 s without data -> give up (was an endless wait, 08.11)
                try { $l = $sp.ReadLine().TrimEnd("`r"); $quiet = 0; Beat 'rx' } catch [System.TimeoutException] { $quiet += 2; continue } catch { L "put read error: $_"; return }
                if ($l -eq '.') { $ok = $true; break }
                [void]$sb.Append($l)
            }
            if (-not $ok) { L "put: no data for 30 s, aborted ($($sb.Length) chars)"; Send $sp "put aborted: no data for 30 s"; Done $sp -1; continue }
            try {
                $b = [Convert]::FromBase64String($sb.ToString())
                if ($b.Length -ne $n) { Send $sp "size mismatch: got $($b.Length)"; Done $sp -1; continue }
                if ($append) { $fs = [IO.File]::Open($path, 'Append', 'Write'); try { $fs.Write($b, 0, $b.Length) } finally { $fs.Close() } } else { [IO.File]::WriteAllBytes($path, $b) }
                Done $sp $b.Length
            } catch { Send $sp "ERROR: $_"; Done $sp -1 }
            continue
        }
        if ($line.StartsWith('GET ')) {
            $path = $line.Substring(4); L "get: $path"
            try {
                $b = [IO.File]::ReadAllBytes($path); $s = [Convert]::ToBase64String($b)
                for ($o = 0; $o -lt $s.Length; $o += 76) { Send $sp ($s.Substring($o, [Math]::Min(76, $s.Length - $o)) + "`n") }
                Done $sp $b.Length
            } catch { Send $sp "ERROR: $_"; Done $sp -1 }
            continue
        }
        Send $sp "unknown: $line"; Done $sp -1
    }
}

try { [Console]::OutputEncoding = New-Object System.Text.UTF8Encoding $false } catch {}
L "=== btagent start (pid $PID)"
Beat 'start'
$failed = @{}
while ($true) {
    $ports = [System.IO.Ports.SerialPort]::GetPortNames()
    foreach ($n in $ports) {
        $sp = New-Object System.IO.Ports.SerialPort $n, 115200
        $sp.NewLine = "`n"; $sp.ReadTimeout = 2000; $sp.WriteTimeout = 20000; $sp.Encoding = New-Object System.Text.UTF8Encoding $false
        try { $sp.Open() } catch { if (-not $failed[$n]) { L "open $n failed: $($_.Exception.Message)"; $failed[$n] = 1 }; continue }
        $failed[$n] = 0
        L "open $n ok"
        try { Serve $sp } catch { L "serve $n error: $_" }
        try { $sp.Close() } catch {}
        L "closed $n"
    }
    Start-Sleep -Seconds 5
}
