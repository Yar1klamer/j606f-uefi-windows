#!/usr/bin/env python3
"""Дописывает аргументы в cmdline Android boot.img (header v0–v2: поле cmdline 512 Б по смещению 64, extra_cmdline 1024 Б по 0x240).
Остальное не меняет (ABL J606F разблокирован, id-хеш не проверяет). Запуск: python3 -I bootimg_add_cmdline.py IN OUT "pmos.debug-shell" """
import struct, sys

src, dst, extra = sys.argv[1], sys.argv[2], sys.argv[3]
d = bytearray(open(src, "rb").read())
if d[:8] != b"ANDROID!":
    sys.exit("не Android boot image")
version = struct.unpack_from("<I", d, 40)[0]
if version > 2:
    sys.exit(f"header v{version} не поддержан")
cmd = d[64:64 + 512].split(b"\0")[0]
new = (cmd + b" " + extra.encode()).strip()
if len(new) >= 512:
    sys.exit("cmdline не помещается в 512 Б (extra_cmdline не трогаем)")
d[64:64 + 512] = new.ljust(512, b"\0")
open(dst, "wb").write(d)
print(f"header v{version}; cmdline:\n  было: {cmd.decode()}\n  стало: {new.decode()}")
