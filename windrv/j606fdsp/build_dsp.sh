#!/bin/sh
# Build j606fdsp.sys (MS KMDOD sample + j606f_bl.cxx) with clang-cl + lld-link against the WDK 10.0.28000 ARM64 NuGet headers/libs
# (~/j606f-build/dl/wdk, local only). Section flags as an inbox driver (see windrv/build.sh: NOT_PAGED .text/.rdata/.data, no TSAWARE).
set -eu
cd ~/j606f-build/windrv-wddm/kmdod
L=~/j606f-build/dl/wdk/wdk/c/Lib/10.0.28000.0/km/ARM64
T=~/j606f-build/tools/llvm-mingw/bin
CERT=~/j606f-build/windrv-cert
sh patch_kmdod.sh >/dev/null
rm -f *.obj
for f in *.cxx; do ./cc.sh /Fo$f.obj $f 2>&1 | grep -E ": error" && exit 1 || true; done
$T/lld-link /nologo /driver /subsystem:native,10.0 /osversion:10.0 /entry:GsDriverEntry /machine:arm64 /nodefaultlib /release /tsaware:no \
	/section:.text,ERP /section:.rdata,RP /section:.data,RWP /section:.pdata,RP /section:INIT,ERD \
	/out:j606fdsp.unsigned.sys *.obj $L/bufferoverflowfastfailk.lib $L/ntoskrnl.lib $L/hal.lib $L/displib.lib $L/libcntpr.lib $L/ntstrsafe.lib
rm -f j606fdsp.sys
osslsigncode sign -certs $CERT/test.crt -key $CERT/test.key -h sha256 -n "J606F display (KMDOD)" -in j606fdsp.unsigned.sys -out j606fdsp.sys >/dev/null
sha256sum j606fdsp.sys
