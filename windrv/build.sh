#!/bin/sh
# Build and test-sign J606F Windows ARM64 kernel drivers on the Linux build host.
# Link flags as an inbox ARM64 driver (null.sys): without NOT_PAGED sections + OS/subsystem 10.0 + no TSAWARE the kernel refuses the
# image with C0000018 STATUS_CONFLICTING_ADDRESSES (tests run_04/run_05, 2026-11-04).
#   sh build.sh <driver dir>     → <dir>/out/<name>.sys (signed with the test certificate in ~/uefi-winarm/windrv-cert)
set -eu
T=${LLVM_MINGW:-$HOME/uefi-winarm/tools/llvm-mingw}
HERE=$(cd "$(dirname "$0")" && pwd)
D=$(cd "$1" && pwd)
NAME=$(basename "$D")
CERT=$HOME/uefi-winarm/windrv-cert
mkdir -p "$HERE/lib/out" "$D/out"
for def in "$HERE"/lib/*.def; do
	"$T/bin/llvm-dlltool" -m arm64 -d "$def" -l "$HERE/lib/out/lib$(basename "$def" .def).a"
done
"$T/bin/aarch64-w64-mingw32-clang" -O2 -Wall -ffreestanding -fno-builtin -fno-stack-protector -fno-exceptions \
	-D_KERNEL_MODE -DNTDDI_VERSION=0x0A000000 -D_WIN32_WINNT=0x0A00 \
	-I"$HERE/include" \
	-nostdlib -Wl,--subsystem,native -Wl,-e,DriverEntry -Wl,--image-base,0x1c0000000 \
	-Wl,--dynamicbase -Wl,--nxcompat -Wl,-Xlink=-driver \
	-Xlinker -Xlink=-section:.text,ERP -Xlinker -Xlink=-section:.rdata,RP -Xlinker -Xlink=-section:.data,RWP -Xlinker -Xlink=-tsaware:no \
	-Wl,--major-os-version,10 -Wl,--minor-os-version,0 -Wl,--major-subsystem-version,10 -Wl,--minor-subsystem-version,0 \
	-o "$D/out/$NAME.unsigned.sys" "$D"/*.c -L"$HERE/lib/out" -lntoskrnl -lhidclass
if [ ! -f "$CERT/test.key" ]; then
	mkdir -p "$CERT" && chmod 700 "$CERT"
	openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj "/CN=J606F Test Driver Signing" \
		-addext "extendedKeyUsage=codeSigning" -addext "keyUsage=digitalSignature" \
		-keyout "$CERT/test.key" -out "$CERT/test.crt" 2>/dev/null
	openssl x509 -in "$CERT/test.crt" -outform DER -out "$CERT/test.cer"
fi
rm -f "$D/out/$NAME.sys"
osslsigncode sign -certs "$CERT/test.crt" -key "$CERT/test.key" -h sha256 -n "J606F $NAME" \
	-in "$D/out/$NAME.unsigned.sys" -out "$D/out/$NAME.sys" >/dev/null
cp "$CERT/test.cer" "$D/out/j606f-test.cer"
sha256sum "$D/out/$NAME.sys" "$D/out/j606f-test.cer"
