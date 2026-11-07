#!/usr/bin/env python3
"""Патчи стоковых DXE J606F — те же, что Project Silicium применяет к драйверам lime (Device-Binaries/lime/README.md):
  ButtonsDxe — Power даёт CHAR_CARRIAGE_RETURN (UnicodeChar 0x0D) вместо SCAN_SUSPEND (ScanCode 0x102): «mov wN,#0x102; strh wN,[sp,#o]»
               → «mov wN,#0xd; strh wN,[sp,#o+2]» (EFI_INPUT_KEY: ScanCode u16, затем UnicodeChar u16);
  UsbMsdDxe  — mass storage «несъёмный»: «orr wN,wzr,#0x80» перед «strb wN,[xM,#1]» (байт RMB ответа INQUIRY) → «mov wN,#0»;
  ClockDxe   — дисплей не гаснет: обнулить 6 qword NPA-описания ресурса «/clk/mmrt» в ClockBSP.
Каждый шаблон обязан найтись ровно один раз, иначе ошибка. Запуск: python3 -I patch_drivers.py <каталог с ClockDxe.efi ...> <выходной каталог>"""
import os, struct, sys

src, dst = sys.argv[1], sys.argv[2]
os.makedirs(dst, exist_ok=True)


def insns(d):
    return [(o, struct.unpack_from("<I", d, o)[0]) for o in range(0, len(d) - 3, 4)]


def text_rva_equals_offset(d):
    pe = struct.unpack_from("<I", d, 0x3c)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    opt = struct.unpack_from("<H", d, pe + 20)[0]
    sh = pe + 24 + opt
    for i in range(nsec):
        va, raw = struct.unpack_from("<I", d, sh + i * 40 + 12)[0], struct.unpack_from("<I", d, sh + i * 40 + 20)[0]
        if va != raw:
            return False
    return True


def one(lst, what):
    if len(lst) != 1:
        sys.exit(f"{what}: найдено {len(lst)} совпадений, ожидалось 1")
    return lst[0]


def buttons(d):
    d = bytearray(d)
    ins = insns(d)
    hits = []
    for i, (o, w) in enumerate(ins[:-1]):
        if (w & 0xFFFFFFE0) == 0x52802040:  # movz wN,#0x102
            rd = w & 0x1F
            w2 = ins[i + 1][1]
            # strh wN,[sp,#imm] (unsigned offset): 0x79000000 | imm12<<10 | 31<<5 | rd
            if (w2 & 0xFFC003FF) == (0x790003E0 | rd):
                hits.append((o, rd, w2))
    o, rd, w2 = one(hits, "ButtonsDxe mov #0x102/strh")
    imm12 = (w2 >> 10) & 0xFFF
    struct.pack_into("<I", d, o, 0x52800000 | (0xD << 5) | rd)
    struct.pack_into("<I", d, o + 4, (w2 & ~(0xFFF << 10)) | ((imm12 + 1) << 10))
    return bytes(d), f"@0x{o:x}: mov w{rd},#0xd; strh -> [sp,#0x{(imm12 + 1) * 2:x}]"


def usbmsd(d):
    d = bytearray(d)
    ins = insns(d)
    hits = []
    for i, (o, w) in enumerate(ins):
        if (w & 0xFFFFFFE0) == 0x321903E0:  # orr wN,wzr,#0x80
            rd = w & 0x1F
            for o2, w2 in ins[i + 1:i + 6]:
                # strb wN,[xM,#1]: 0x39000000 | 1<<10 | rn<<5 | rd
                if (w2 & 0xFFFFFC1F) == (0x39000400 | rd):
                    hits.append((o, rd)); break
    o, rd = one(hits, "UsbMsdDxe orr #0x80/strb [,#1]")
    struct.pack_into("<I", d, o, 0x52800000 | rd)
    return bytes(d), f"@0x{o:x}: mov w{rd},#0"


def clock(d):
    if not text_rva_equals_offset(d):
        sys.exit("ClockDxe: RVA != смещение в файле, патч по RVA не применим")
    d = bytearray(d)
    s = one([i for i in range(len(d)) if d.startswith(b"/clk/mmrt\0", i)], "ClockDxe строка /clk/mmrt")
    refs = [o for o in range(0, len(d) - 7, 8) if struct.unpack_from("<Q", d, o)[0] == s]
    o = one(refs, "ClockDxe указатель на /clk/mmrt")
    q = [struct.unpack_from("<Q", d, o + 8 * k)[0] for k in range(7)]
    rpm = d[q[2]:d.index(b"\0", q[2])] if 0 < q[2] < len(d) else b""
    if rpm != b"/protocol/rpm/rpm" or q[6] != 0xFFFFFFFF:
        sys.exit(f"ClockDxe: структура не как у lime: {[hex(x) for x in q]} {rpm}")
    d[o:o + 48] = bytes(48)
    return bytes(d), f"@0x{o:x}: 6 qword обнулены (/clk/mmrt @0x{s:x})"


for name, fn in (("ButtonsDxe", buttons), ("UsbMsdDxe", usbmsd), ("ClockDxe", clock)):
    data = open(os.path.join(src, name + ".efi"), "rb").read()
    out, msg = fn(data)
    diff = sum(a != b for a, b in zip(data, out))
    open(os.path.join(dst, name + ".patched.efi"), "wb").write(out)
    print(f"{name}: {msg}; изменено байт: {diff}")
