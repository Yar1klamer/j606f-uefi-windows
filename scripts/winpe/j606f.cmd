@echo off
rem J606F: WinPE on the ESP runs bcdboot for the applied Windows and reboots. Log: S:\j606f_pe.log (read from pmOS)
wpeinit
mountvol S: /S
set LOG=S:\j606f_pe.log
echo [j606f] start %date% %time% > %LOG%
mountvol >> %LOG% 2>&1
set WIN=
for %%d in (C D E F G H I J K L M N O P Q R T U V W Y Z) do if exist %%d:\Windows\System32\config\SYSTEM set WIN=%%d:
echo WIN=%WIN% >> %LOG%
if "%WIN%"=="" goto fail
rem WinPE boot files (EFI, boot, sources) are no longer needed: bootmgr and boot.wim are already in RAM
rd /s /q S:\EFI >> %LOG% 2>&1
rd /s /q S:\boot >> %LOG% 2>&1
rd /s /q S:\sources >> %LOG% 2>&1
bcdboot %WIN%\Windows /s S: /f UEFI /l ru-ru >> %LOG% 2>&1
echo bcdboot rc=%errorlevel% >> %LOG%
bcdedit /store S:\EFI\Microsoft\Boot\BCD /set {default} testsigning on >> %LOG% 2>&1
bcdedit /store S:\EFI\Microsoft\Boot\BCD /set {default} nointegritychecks on >> %LOG% 2>&1
bcdedit /store S:\EFI\Microsoft\Boot\BCD /set {bootmgr} timeout 3 >> %LOG% 2>&1
bcdedit /store S:\EFI\Microsoft\Boot\BCD /enum all >> %LOG% 2>&1
dir /s S:\ >> %LOG% 2>&1
echo [j606f] done %date% %time% >> %LOG%
goto end
:fail
echo [j606f] Windows not found >> %LOG%
echo list disk > X:\dp.txt
echo list volume >> X:\dp.txt
diskpart /s X:\dp.txt >> %LOG% 2>&1
:end
wpeutil reboot
