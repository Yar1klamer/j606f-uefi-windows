@echo off
rem J606F test 02: probe.sys (llvm-mingw). Read-only pass first (GCC/TLMM/SE0 state left by UEFI), then the active pass
rem (QUP0 clocks + SE0 as Linux i2c-qcom-geni, reads the mm8013 fuel gauge). The mode comes from the service name. Log: out\probe.txt.
set OUT=C:\j606f\out
echo === test 02 %date% %time%
certutil -addstore -f Root C:\j606f\drv\j606f-test.cer
certutil -addstore -f TrustedPublisher C:\j606f\drv\j606f-test.cer
copy /y C:\j606f\drv\probe.sys C:\j606f\drv\probei2c.sys
sc create j606fprobe type= kernel start= demand binPath= C:\j606f\drv\probe.sys
sc start j606fprobe
sc stop j606fprobe
sc create j606fi2c type= kernel start= demand binPath= C:\j606f\drv\probei2c.sys
sc start j606fi2c
sc stop j606fi2c
type %OUT%\probe.txt
echo === done %date% %time%
