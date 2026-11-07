#!/bin/sh
# Loader experiment: hellov.c built with different link options + ntload.exe. Run on the build host: sh build_ldtest.sh
set -eu
T=${LLVM_MINGW:-~/j606f-build/tools/llvm-mingw}
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(cd "$HERE/.." && pwd)
CERT=~/j606f-build/windrv-cert
O=$HERE/out
mkdir -p "$O" "$W/lib/out"
"$T/bin/llvm-dlltool" -m arm64 -d "$W/lib/ntoskrnl.def" -l "$W/lib/out/libntoskrnl.a"
CC="$T/bin/aarch64-w64-mingw32-clang -O2 -Wall -ffreestanding -fno-builtin -fno-stack-protector -fno-exceptions -D_KERNEL_MODE -I$W/include"
BASE="-nostdlib -Wl,--subsystem,native -Wl,-e,DriverEntry -Wl,--dynamicbase -Wl,--nxcompat"
build() {  # name, extra link flags
	$CC -DVARIANT="\"$1\"" $BASE $2 -o "$O/$1.unsigned.sys" "$HERE/hellov.c" -L"$W/lib/out" -lntoskrnl
	rm -f "$O/$1.sys"
	osslsigncode sign -certs "$CERT/test.crt" -key "$CERT/test.key" -h sha256 -n "J606F $1" -in "$O/$1.unsigned.sys" -out "$O/$1.sys" >/dev/null
}
NP="-Xlinker -Xlink=-section:.text,ERP -Xlinker -Xlink=-section:.rdata,RP -Xlinker -Xlink=-section:.data,RWP"
V10="-Wl,--major-os-version,10 -Wl,--minor-os-version,0 -Wl,--major-subsystem-version,10 -Wl,--minor-subsystem-version,0"
# 04.11 run_04: hva..hvf (base 0x140000000 / 0x10000, alignments, -driver variants) all failed: C0000018 / C000007B (base 0x10000)
build hvg "-Wl,--image-base,0x1c0000000 -Wl,-Xlink=-driver"
build hvh "-Wl,--image-base,0x1c0000000 -Wl,-Xlink=-driver $NP"
build hvi "-Wl,--image-base,0x1c0000000 -Wl,-Xlink=-driver $V10"
build hvj "-Wl,--image-base,0x140000000 -Wl,-Xlink=-driver $NP $V10 -Wl,-Xlink=-tsaware:no"
build hvk "-Wl,--image-base,0x1c0000000 -Wl,-Xlink=-driver $NP $V10 -Wl,-Xlink=-tsaware:no"
$T/bin/aarch64-w64-mingw32-clang -O2 -Wall -municode -o "$O/ntload.exe" "$HERE/ntload.c" -lntdll
for f in hvg hvh hvi hvj hvk; do
	echo "== $f"; "$T/bin/llvm-objdump" -h "$O/$f.unsigned.sys" | tail -n +5
	"$T/bin/llvm-readobj" --coff-basereloc "$O/$f.unsigned.sys" | grep -c "Type:" || true
done
sha256sum "$O"/*.sys "$O/ntload.exe" | grep -v unsigned
