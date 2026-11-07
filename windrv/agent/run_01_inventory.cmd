@echo off
rem J606F test 01: load the hello.sys test driver (llvm-mingw build) and take an inventory of what Windows sees on the tablet.
set OUT=C:\j606f\out
echo === test 01 %date% %time%
certutil -addstore -f Root C:\j606f\drv\j606f-test.cer
certutil -addstore -f TrustedPublisher C:\j606f\drv\j606f-test.cer
bcdedit /enum {current}
sc create j606fhello type= kernel start= demand binPath= C:\j606f\drv\hello.sys
sc start j606fhello
sc query j606fhello
type %OUT%\hello_drv.txt
echo === devices with problems
pnputil /enum-devices /problem
pnputil /enum-devices /connected > %OUT%\devices_connected.txt
pnputil /enum-devices > %OUT%\devices_all.txt
echo === wmi
powershell -NoProfile -ExecutionPolicy Bypass -Command "Get-CimInstance Win32_Battery | Format-List *; Get-CimInstance Win32_VideoController | Format-List Name,PNPDeviceID,CurrentHorizontalResolution,CurrentVerticalResolution,CurrentRefreshRate; Get-CimInstance Win32_Processor | Format-List Name,NumberOfCores,NumberOfLogicalProcessors,MaxClockSpeed,CurrentClockSpeed; Get-CimInstance Win32_DesktopMonitor | Format-List *; Get-PnpDevice -PresentOnly | Sort-Object Class | Format-Table -AutoSize Status,Class,FriendlyName,InstanceId" > %OUT%\wmi.txt 2>&1
powercfg /a
powercfg /batteryreport /output %OUT%\battery.html
systeminfo > %OUT%\systeminfo.txt
wevtutil qe System /q:"*[System[(Level=1 or Level=2)]]" /c:80 /rd:true /f:text > %OUT%\system_errors.txt
echo === done %date% %time%
