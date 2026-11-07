#!/usr/bin/env python3
"""Минимальный разборщик UEFI Firmware Volume (PI spec) для дампов xbl/imagefv/abl.
Ищет все _FVH в файле, обходит FFS-файлы, распаковывает LZMA (EE4E5898…) и gzip Qualcomm (1D301FE9…) и вложенные FV,
печатает GUID/тип/имя (UI-секция) и, с --out DIR, сохраняет PE32/TE/DEPEX/RAW секции как <имя>.<ext>\nи manifest.json (GUID, тип FFS, имя, файлы).
Только чтение входного файла. Запуск: python3 -I fv_extract.py IMG [--out DIR]"""
import gzip, json, lzma, os, struct, sys, uuid

LZMA_GUID = uuid.UUID("EE4E5898-3914-4259-9D6E-DC7BD79403CF")
GZIP_GUID = uuid.UUID("1D301FE9-BE79-4353-91C2-D23BC959AE0C")  # Qualcomm: gzip-сжатый FV в xbl
FTYPES = {1: "RAW", 2: "FREEFORM", 3: "SEC", 4: "PEI_CORE", 5: "DXE_CORE", 6: "PEIM", 7: "DRIVER",
          8: "COMBINED", 9: "APPLICATION", 0xA: "MM", 0xB: "FV_IMAGE", 0xF0: "PAD"}
out = None
found = []


def g(b):
    return str(uuid.UUID(bytes_le=bytes(b[:16]))).upper()


def sections(data, depth, ctx):
    off = 0
    while off + 4 <= len(data):
        size = int.from_bytes(data[off:off + 3], "little")
        typ = data[off + 3]
        hdr = 4
        if size == 0xFFFFFF:
            size = struct.unpack_from("<I", data, off + 4)[0]; hdr = 8
        if size < hdr or off + size > len(data):
            break
        body = data[off + hdr:off + size]
        if typ == 0x15:  # USER_INTERFACE
            ctx["name"] = body.decode("utf-16-le", "replace").rstrip("\0")
        elif typ == 0x02:  # GUID_DEFINED
            sg = uuid.UUID(bytes_le=bytes(body[:16]))
            doff = struct.unpack_from("<H", body, 16)[0] - hdr
            payload = body[doff:]
            if sg == LZMA_GUID:
                try:
                    sections(lzma.decompress(payload, format=lzma.FORMAT_ALONE), depth + 1, ctx)
                except lzma.LZMAError as e:
                    print("  " * depth + f"  LZMA error {e}")
            elif sg == GZIP_GUID:
                sections(gzip.decompress(payload), depth + 1, ctx)
            else:
                sections(payload, depth + 1, ctx)
        elif typ == 0x17:  # FV image
            parse_fv(body, depth + 1)
        elif typ in (0x10, 0x12, 0x13, 0x19):  # PE32, TE, DXE_DEPEX, RAW
            ctx.setdefault("blobs", []).append(({0x10: "efi", 0x12: "te", 0x13: "depex", 0x19: "raw"}[typ], body))
        off = (off + size + 3) & ~3


def parse_fv(fv, depth=0):
    if fv[40:44] != b"_FVH":
        return
    fvlen = struct.unpack_from("<Q", fv, 32)[0]
    hlen = struct.unpack_from("<H", fv, 48)[0]
    print("  " * depth + f"FV {g(fv[16:32])} len 0x{fvlen:x}")
    off = hlen
    ext = struct.unpack_from("<H", fv, 52)[0]
    if ext:
        off = ext + struct.unpack_from("<I", fv, ext + 16)[0]
    off = (off + 7) & ~7
    while off + 24 <= min(len(fv), fvlen):
        fg = fv[off:off + 16]
        if fv[off:off + 24] == b"\xff" * 24:  # свободное место (erase polarity 1)
            break
        ftype = fv[off + 18]
        attr = fv[off + 19]
        size = int.from_bytes(fv[off + 20:off + 23], "little")
        hdr = 24
        if attr & 0x01 and size == 0:  # FFS_ATTRIB_LARGE_FILE
            size = struct.unpack_from("<Q", fv, off + 24)[0]; hdr = 32
        if size < hdr:
            break
        body = fv[off + hdr:off + size]
        ctx = {}
        if ftype not in (0xF0, 0x01):
            sections(body, depth + 1, ctx)
        elif ftype == 0x01:
            ctx["blobs"] = [("raw", body)]
        name = ctx.get("name", "")
        print("  " * depth + f"  {g(fg)} {FTYPES.get(ftype, hex(ftype)):12} {name}")
        rec = {"guid": g(fg), "type": FTYPES.get(ftype, hex(ftype)), "name": name, "files": {}}
        found.append(rec)
        if out and ctx.get("blobs"):
            for i, (ext_, blob) in enumerate(ctx["blobs"]):
                base = (name or g(fg)).replace("/", "_")
                fn = base + (f".{i}" if i else "") + ("" if ext_ == "raw" and "." in base else "." + ext_)
                with open(os.path.join(out, fn), "wb") as f:
                    f.write(blob)
                rec["files"][ext_] = fn
        off = (off + size + 7) & ~7


def main():
    global out
    img = open(sys.argv[1], "rb").read()
    if "--out" in sys.argv:
        out = sys.argv[sys.argv.index("--out") + 1]; os.makedirs(out, exist_ok=True)
    pos = 0
    while True:
        i = img.find(b"_FVH", pos)
        if i < 0:
            break
        start = i - 40
        if start >= 0:
            print(f"# _FVH @ file offset 0x{start:x}")
            parse_fv(img[start:])
        pos = i + 4
    print(f"# files: {len(found)}")
    if out:
        with open(os.path.join(out, "manifest.json"), "w") as f:
            json.dump(found, f, indent=1)


main()
