# Сборка UEFI (Mu-Silicium j606f)

Хост: Linux x86_64 (проверено на Ubuntu 22.04, ~40 ГБ свободно). Первая сборка — около 10 минут, инкрементальная — ~1 мин.

## 1. Окружение
```bash
sudo apt install git python3-venv clang lld llvm gcc-aarch64-linux-gnu mono-devel nasm uuid-dev acpica-tools
git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/Project-Silicium/Mu-Silicium
python3 -m venv venv && . venv/bin/activate && pip install -r Mu-Silicium/pip-requirements.txt
```
На Python 3.10 в `build_uefi.py` нет `tomllib` — поставьте `tomli` и замените импорт (`import tomli as tomllib`). Проверочная сборка
`python3 build_uefi.py -d lime -r RELEASE` должна закончиться Success.

## 2. Файлы устройства
```bash
cp -r uefi/Platforms/Lenovo        Mu-Silicium/Platforms/
cp    uefi/Resources/Configs/j606f.toml Mu-Silicium/Resources/Configs/
mkdir -p Mu-Silicium/Silicium-ACPI/Platforms/Lenovo/j606f
cp    uefi/Silicium-ACPI/Platforms/Lenovo/j606f/SSDT.asl Mu-Silicium/Silicium-ACPI/Platforms/Lenovo/j606f/
cp    Mu-Silicium/Silicium-ACPI/Platforms/Xiaomi/lime/AcpiTables.inf Mu-Silicium/Silicium-ACPI/Platforms/Lenovo/j606f/
```
Плюс со своего планшета ([EXTRACT_BINARIES.md](EXTRACT_BINARIES.md)): `Binaries/j606f/` и `Resources/DTBs/j606f.dtb`.

SSDT в `uefi/` — версия v5d (батарея и адаптер по SPMI, проверена). Варианты — в `acpi/silicium_j606f/`
(v6 добавляет устройство тача `TSC0`/`JHMX0001` для `windrv/hidtouch`, пока не испытано).

## 3. Диагностические правки (по желанию)
```bash
cd Mu-Silicium && python3 -I ../scripts/patch_silicium_diag.py .
```
Идемпотентно, сохраняет CRLF файлов Silicium. Что делает — в шапке скрипта и в `uefi/patches/silicium_j606f_diag.diff`:
- `J606F_MSD_ON_NO_OS` (RELEASE и DEBUG): без ОС — запустить USB и Mass Storage (выход — Громкость+) вместо выключения;
- `J606F_RAMOOPS_LOG` (только DEBUG): лог UEFI в консольную зону ramoops ядра pmOS (0xFFCA0000, с чётностью Рида–Соломона), тёплый сброс
  вместо выключения, тайм-аут Mass Storage 3 мин. После сброса в pmOS: `sudo cat /var/lib/systemd/pstore/console-ramoops-0`.

## 4. Сборка и запуск
```bash
. venv/bin/activate && export CLANGPDB_AARCH64_PREFIX=aarch64-linux-gnu-
cd Mu-Silicium && python3 build_uefi.py -d j606f -r RELEASE    # или -r DEBUG
# → ./Mu-j606f.img (каждая сборка его перезаписывает)
fastboot boot Mu-j606f.img                                      # только RAM
```
**Ограничение:** в выводе сборки строка `FVMAIN [..%Full] … total` должна показывать **0x521000**. Образ с 0x522000 сбрасывается до Windows
(причина не найдена) — не добавляйте в FV лишнего, AML держите компактным.

## 5. Постоянная установка (необязательно)
UEFI можно записать в `boot_a`, а pmOS оставить в слоте b: выбор ОС — `fastboot set_active a` (Windows) / `b` (pmOS).
Перед записью сохраните стоковый `boot_a`. После `set_active a` загрузчик сбрасывает флаг «successful» слота a — отметьте его
(`qbootctl -m a` из pmOS, загруженной с `slot_suffix=_a`), иначе после нескольких загрузок он откатится на b.
