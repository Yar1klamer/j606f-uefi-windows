#!/bin/sh
# Build the NDIS miniport with the WDK headers on the Linux build host (WDK stays there, only our source is published).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
W=~/j606f-build/dl/wdk/wdk/c
S=~/j606f-build/dl/wdk/sdk/c
V=10.0.28000.0
T=${LLVM_MINGW:-~/j606f-build/tools/llvm-mingw}
CERT=~/j606f-build/windrv-cert
mkdir -p "$HERE/out/inc"
ln -sf "$S/Include/$V/shared/wlantypes.h" "$HERE/out/inc/WlanTypes.h"   # the header uses another letter case on this case-sensitive host
"$T/bin/clang" --target=aarch64-pc-windows-msvc -fms-extensions -fms-compatibility -O2 -w -c -ffreestanding -fno-stack-protector -fno-builtin \
	-D_ARM64_ -DARM64 -D_WIN64 -D_KERNEL_MODE -DNTDDI_VERSION=0x0A000000 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 \
	-isystem "$HERE/out/inc" -isystem "$W/Include/$V/km" -isystem "$W/Include/$V/km/crt" -isystem "$W/Include/$V/shared" -isystem "$S/Include/$V/shared" -isystem "$S/Include/$V/um" \
	-o "$HERE/out/j606fnet.obj" "$HERE/j606fnet.c"
"$T/bin/lld-link" /nologo /machine:arm64 /driver /subsystem:native,10.0 /entry:DriverEntry /nodefaultlib /osversion:10.0 /tsaware:no /dynamicbase /nxcompat \
	/section:.text,ERP /section:.rdata,RP /section:.data,RWP /base:0x1c0000000 \
	/out:"$HERE/out/j606fnet.unsigned.sys" "$HERE/out/j606fnet.obj" "$W/Lib/$V/km/ARM64/ndis.lib" "$W/Lib/$V/km/ARM64/ntoskrnl.lib" "$W/Lib/$V/km/ARM64/hal.lib" "$W/Lib/$V/km/ARM64/bufferoverflowfastfailk.lib"
rm -f "$HERE/out/j606fnet.sys"
osslsigncode sign -certs "$CERT/test.crt" -key "$CERT/test.key" -h sha256 -n "J606F j606fnet" -in "$HERE/out/j606fnet.unsigned.sys" -out "$HERE/out/j606fnet.sys" >/dev/null
sha256sum "$HERE/out/j606fnet.sys"
