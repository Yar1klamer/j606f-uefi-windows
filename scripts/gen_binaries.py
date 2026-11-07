#!/usr/bin/env python3
"""Собирает Binaries/j606f для Mu-Silicium по образцу Binaries/lime: для каждого .inf lime ищет драйвер J606F с тем же FILE_GUID
(или тем же BASE_NAME) в manifest.json стокового xbl_a (scripts/fv_extract.py) и кладёт наши .efi/.te/.depex под теми же путями и именами.
Патченные драйверы (scripts/patch_drivers.py) подставляются вместо *.patched.efi. INF переписываются под наш набор секций.
RawFiles: наши cfg/xml/bmp из xbl_a. Запуск:
  python3 -I gen_binaries.py <Binaries/lime> <raw/xbl_a_fv> <raw/drv_patched> <Binaries/j606f>"""
import json, os, re, shutil, sys

lime, fv, patched, out = sys.argv[1:5]
man = json.load(open(os.path.join(fv, "manifest.json")))
by_guid = {r["guid"].upper(): r for r in man}
by_name = {r["name"]: r for r in man if r["name"]}
report = {"ok": [], "missing": [], "renamed": []}

for root, _, files in os.walk(lime):
    for f in files:
        if not f.endswith(".inf"):
            continue
        rel = os.path.relpath(root, lime)
        if rel.startswith("RawFiles"):
            continue
        text = open(os.path.join(root, f)).read()
        guid = re.search(r"FILE_GUID\s*=\s*([0-9A-Fa-f-]+)", text).group(1).upper()
        base = re.search(r"BASE_NAME\s*=\s*(\S+)", text).group(1)
        rec = by_guid.get(guid) or by_name.get(base)
        if rec is None:
            report["missing"].append(f"{rel}/{f} ({base} {guid})")
            continue
        if rec["guid"].upper() != guid:
            report["renamed"].append(f"{rel}/{f}: GUID {guid} -> {rec['guid']}")
            text = text.replace(re.search(r"FILE_GUID\s*=\s*\S+", text).group(0), f"FILE_GUID                      = {rec['guid']}")
        dst = os.path.join(out, rel)
        os.makedirs(dst, exist_ok=True)
        lines = []
        for line in text.splitlines():
            m = re.match(r"\s*(PE32|TE|DXE_DEPEX|RAW)\|([^|]+)\|", line)
            if not m:
                lines.append(line)
                continue
            kind, name = m.groups()
            key = {"PE32": "efi", "TE": "te", "DXE_DEPEX": "depex", "RAW": "raw"}[kind]
            if name.endswith(".patched.efi"):
                srcf = os.path.join(patched, rec["name"] + ".patched.efi")
                if not os.path.exists(srcf):
                    sys.exit(f"нет патченного {srcf}")
            elif key in rec["files"]:
                srcf = os.path.join(fv, rec["files"][key])
            else:
                report["renamed"].append(f"{rel}/{f}: у J606F нет секции {kind}, строка убрана")
                continue
            shutil.copyfile(srcf, os.path.join(dst, name))
            lines.append(line)
        # у J606F есть DEPEX, а в INF lime его нет
        if "depex" in rec["files"] and not any("DXE_DEPEX|" in l for l in lines):
            name = base + ".depex"
            shutil.copyfile(os.path.join(fv, rec["files"]["depex"]), os.path.join(dst, name))
            i = next(k for k, l in enumerate(lines) if l.strip().startswith("[Binaries"))
            lines.insert(i + 1, f"  DXE_DEPEX|{name}|*")
            report["renamed"].append(f"{rel}/{f}: добавлен DXE_DEPEX")
        open(os.path.join(dst, f), "w").write("\n".join(lines) + "\n")
        report["ok"].append(f"{rel}/{f} <- {rec['name']}")

raw_out = os.path.join(out, "RawFiles")
os.makedirs(raw_out, exist_ok=True)
for r in man:
    if r["type"] == "FREEFORM" and r["name"] and "raw" in r["files"]:
        shutil.copyfile(os.path.join(fv, r["files"]["raw"]), os.path.join(raw_out, r["name"]))

for k in ("missing", "renamed"):
    print(f"## {k}: {len(report[k])}")
    for x in report[k]:
        print("  ", x)
print(f"## ok: {len(report['ok'])}")
