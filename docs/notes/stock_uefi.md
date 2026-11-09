# Стоковый UEFI J606F (XBL, BOOT.XF.4.1-00187-KAMORTALAZ-2)

Источник: `<бэкап разделов>/xbl_a.img` (3 670 016 Б; дамп, только чтение).
Разбор: `scripts/fv_extract.py` (свой, без внешних пакетов: PI FV + LZMA + gzip-секция Qualcomm GUID 1D301FE9-BE79-4353-91C2-D23BC959AE0C).
Результат: около 100 файлов FFS (образы DXE-драйверов, конфиги, XML панелей), разбор не публикуется.
**Блобы Qualcomm/Lenovo: только приватно, никогда в публичный репозиторий.**

## Где что лежит
- `xbl_a` — ELF; внешний FV @0x74520 (len 0x212e00, erase polarity 1): PAD, SEC (0x3e018), `uefiplat.cfg`, FV_IMAGE (gzip) →
  внутренний FV: DxeCore + 90+ DXE-драйверов + конфиги + XML панелей + картинки.
- `xbl_b` — тот же набор файлов (список совпадает).
- `imagefv_a` — только картинки (lenovo_logo.bmp, battery_symbol_*.bmp).
- `abl_a` — LinuxLoader (ABL) — сам является UEFI-приложением в этом же UEFI.

## Драйверы (внутренний FV)
DxeCore, ArmCpuDxe, RuntimeDxe, SecurityStubDxe, SecRSADxe, ASN1X509Dxe, ScmDxe, TzDxe, VerifiedBootDxe, WatchdogTimer, CapsuleRuntimeDxe,
RscRtDxe, SCHandlerRtDxe, VariableDxe, EmbeddedMonotonicCounter, SimpleTextInOutSerial, ResetRuntimeDxe, RealTimeClock, MetronomeDxe,
PrintDxe, DevicePathDxe, ConPlatformDxe, ConSplitterDxe, GraphicsConsoleDxe, HiiDatabase, FontDxe, EnvDxe, QcomWDogDxe, QcomMpmTimerDxe,
ArmGicDxe, ArmTimerDxe, ChipInfo, GlinkDxe, SmemDxe, ULogDxe, NpaDxe, DiskIoDxe, PartitionDxe, Fat, EnglishDxe, FvSimpleFileSystem,
DALSys, VcsDxe, ClockDxe, HALIOMMU, HWIODxeDriver, GpiDxe, I2C, SPMI, SdccDxe, UFSDxe, DALTLMM, MinidumpTADxe, **DisplayDxe**,
PlatformInfoDxeDriver, ButtonsDxe, PmicDxe, ChargerExDxe, QcomChargerDxeLA, UsbPwrCtrlDxe, AdcDxe, TsensDxe, DDRInfoDxe,
UsbfnDwc3Dxe, UsbMsdDxe, UsbDeviceDxe, UsbConfigDxe, HashDxe, CipherDxe, MacDxe, RngDxe, QcomBds, PILDxe, QcomChargerApp, ShmBridgeDxe.
Это **тот же набор, что Mu-Silicium подключает для Xiaomi Redmi 9T (lime, тоже Kamorta)** — см. ref/silicium-lime/Platforms/Xiaomi/limePkg/Include/DXE.inc.

## Конфиги
- `uefiplat.cfg`: карта памяти DDR и регистров — **совпадает с lime** (Kernel 0x40000000/0x5700000, HYP, SMEM 0x46000000, PIL 0x4AB00000/0x9400000,
  DXE Heap 0x53F00000/0x2800000 в стоке — Silicium переносит его на 0x63900000/0x4600000 после TZApps, Display Reserved **0x5C000000/16 МиБ**,
  UEFI FD 0x5FC00000/0x300000, стек 0x5FF90000, Log Buffer 0x5FFF7000, QSEE/TZApps 0x60000000/0x61800000).
  ConfigParameters тоже как в ConfigurationMapLib lime (NumCpus 8, SecurityFlag 0xC4, TzAppsRegn 0x61800000/0x2100000, ShmBridge 0xA00000…).
  DefaultBDSBootApp = "LinuxLoader", SpecialLogPartition = "LOGFS:\".
- `uefipil.cfg`: загрузка MPSS/ADSP/CDSP из core_nhlos_a (\image\modem, adsp …), регионы как в DT.
- `SecParti.cfg`, `QcomChargerCfg.cfg`, `BATTERY.PROVISION`, `BDS_Menu.cfg`.
- XML панелей: **`Panel_boe_himax83102p_2k_vid.xml`** (наша: 1200x2000, HFP 60/HBP 49/HSW 8, VFP 80/VBP 38/VSW 8, 4 линии, video) и
  `Panel_boe_nt36523w_2k_vid.xml` (второй SKU), плюс три truly-панели от референса.
