#!/usr/bin/env python3
"""Диагностические правки дерева Mu-Silicium для J606F (всё под #ifdef J606F_RAMOOPS_LOG, включается флагом в j606f.dsc):
  FrameBufferSerialPortLib — лог DEBUG дублируется в консольную зону ramoops ядра pmOS (0xFFCA0000, данных 232980 Б) с чётностью
                             Рида–Соломона, как у ядра (без неё ядро «исправляет» заголовок обратно) → после тёплого сброса console-ramoops-0;
  Sec.c                    — в начале SecMain сбрасывает сигнатуру зоны (лог каждой загрузки с нуля);
  BootManager.c            — (всегда, J606F_MSD_ON_NO_OS) перед Mass Storage запускает контроллер USB и ConnectAll;
                             (диагностика) печатает загрузочные опции и делает тёплый сброс вместо выключения;
  MassStorage.c            — печатает QueryMaxLun и результат поиска каждого носителя; выход из MSD через 3 мин.
Повторный запуск ничего не меняет. Запуск: python3 -I patch_silicium_diag.py <Mu-Silicium>"""
import os, sys

root = sys.argv[1]


def patch(rel, old, new, marker):
    p = os.path.join(root, rel)
    s = open(p, newline="").read()
    if "\r\n" in s:  # файлы Silicium частично с CRLF — сохраняем их окончания строк
        old, new = old.replace("\n", "\r\n"), new.replace("\n", "\r\n")
    if marker in s:
        print(f"{rel}: уже есть")
        return
    if s.count(old) != 1:
        sys.exit(f"{rel}: шаблон найден {s.count(old)} раз, ожидалось 1")
    open(p, "w", newline="").write(s.replace(old, new))
    print(f"{rel}: исправлен")


FB = "Silicon/Silicium/SiliciumPkg/Library/FrameBufferSerialPortLib/FrameBufferSerialPortLib.c"
patch(FB, """UINTN
EFIAPI
SerialPortWrite (""", '#ifdef J606F_RAMOOPS_LOG\n// uefi-winarm (J606F): mirror the log into the console zone of the pmOS ramoops region (0xFFC00000, 1 MiB: 640 KiB dmesg records,\n// then 256 KiB console). After a warm reset pmOS shows it as /sys/fs/pstore/console-ramoops-0. The zone has ECC 16 (RS over 128-byte\n// blocks + the 12-byte header): without valid parity the kernel "corrects" our header back to its old values, so parity is written too.\n#define J606F_RAMOOPS_CONSOLE  0xFFCA0000\n#define J606F_RAMOOPS_SIG      0x43474244\n#define J606F_RAMOOPS_DATA     232980\n#define J606F_RAMOOPS_BLOCKS   ((J606F_RAMOOPS_DATA + 127) / 128)\n\nSTATIC CONST UINT8 mJ606fRsAlpha[256] = {1,2,4,8,16,32,64,128,29,58,116,232,205,135,19,38,76,152,45,90,180,117,234,201,143,3,6,12,24,48,96,192,157,39,78,156,37,74,148,53,106,212,181,119,238,193,159,35,70,140,5,10,20,40,80,160,93,186,105,210,185,111,222,161,95,190,97,194,153,47,94,188,101,202,137,15,30,60,120,240,253,231,211,187,107,214,177,127,254,225,223,163,91,182,113,226,217,175,67,134,17,34,68,136,13,26,52,104,208,189,103,206,129,31,62,124,248,237,199,147,59,118,236,197,151,51,102,204,133,23,46,92,184,109,218,169,79,158,33,66,132,21,42,84,168,77,154,41,82,164,85,170,73,146,57,114,228,213,183,115,230,209,191,99,198,145,63,126,252,229,215,179,123,246,241,255,227,219,171,75,150,49,98,196,149,55,110,220,165,87,174,65,130,25,50,100,200,141,7,14,28,56,112,224,221,167,83,166,81,162,89,178,121,242,249,239,195,155,43,86,172,69,138,9,18,36,72,144,61,122,244,245,247,243,251,235,203,139,11,22,44,88,176,125,250,233,207,131,27,54,108,216,173,71,142,0};\nSTATIC CONST UINT8 mJ606fRsIndex[256] = {255,0,1,25,2,50,26,198,3,223,51,238,27,104,199,75,4,100,224,14,52,141,239,129,28,193,105,248,200,8,76,113,5,138,101,47,225,36,15,33,53,147,142,218,240,18,130,69,29,181,194,125,106,39,249,185,201,154,9,120,77,228,114,166,6,191,139,98,102,221,48,253,226,152,37,179,16,145,34,136,54,208,148,206,143,150,219,189,241,210,19,92,131,56,70,64,30,66,182,163,195,72,126,110,107,58,40,84,250,133,186,61,202,94,155,159,10,21,121,43,78,212,229,172,115,243,167,87,7,112,192,247,140,128,99,13,103,74,222,237,49,197,254,24,227,165,153,119,38,184,180,124,17,68,146,217,35,32,137,46,55,63,209,91,149,188,207,205,144,135,151,178,220,252,190,97,242,86,211,171,20,42,93,158,132,60,57,83,71,109,65,162,31,45,67,216,183,123,164,118,196,23,73,236,127,12,111,246,108,161,59,82,41,157,85,170,251,96,134,177,187,204,62,90,203,89,95,176,156,169,160,81,11,245,22,235,122,117,44,215,79,174,213,233,230,231,173,232,116,214,244,234,168,80,88,175};\nSTATIC CONST UINT8 mJ606fRsGen[17] = {120,225,194,182,169,147,191,91,3,76,161,102,109,107,104,120,0};\n\n// Reed-Solomon parity as the kernel\'s encode_rs8 (init_rs (8, 0x11d, 0, 1, 16)) used by pstore/ram ECC\nSTATIC\nVOID\nJ606fRsEncode (\n  IN  volatile UINT8 *Data,\n  IN  UINTN           Length,\n  OUT volatile UINT8 *Parity)\n{\n  UINT8 Par[16] = {0};\n\n  for (UINTN i = 0; i < Length; i++) {\n    UINT8 Feedback = mJ606fRsIndex[Data[i] ^ Par[0]];\n\n    if (Feedback != 255) {\n      for (UINTN j = 1; j < 16; j++) {\n        Par[j] ^= mJ606fRsAlpha[(Feedback + mJ606fRsGen[16 - j]) % 255];\n      }\n    }\n\n    for (UINTN j = 0; j < 15; j++) {\n      Par[j] = Par[j + 1];\n    }\n\n    Par[15] = (Feedback != 255) ? mJ606fRsAlpha[(Feedback + mJ606fRsGen[0]) % 255] : 0;\n  }\n\n  for (UINTN i = 0; i < 16; i++) {\n    Parity[i] = Par[i];\n  }\n}\n\nSTATIC\nVOID\nJ606fRamoopsEncodeBlock (IN UINT32 Block)\n{\n  volatile UINT8 *Data   = (volatile UINT8 *)(J606F_RAMOOPS_CONSOLE + 12);\n  volatile UINT8 *Parity = Data + J606F_RAMOOPS_DATA;\n  UINT32          Length = J606F_RAMOOPS_DATA - Block * 128;\n\n  J606fRsEncode (Data + Block * 128, (Length < 128) ? Length : 128, Parity + Block * 16);\n}\n\nSTATIC\nVOID\nWriteRamoops (\n  IN UINT8 *Buffer,\n  IN UINTN  NumberOfBytes)\n{\n  volatile UINT32 *Header = (volatile UINT32 *)J606F_RAMOOPS_CONSOLE;\n  volatile UINT8  *Data   = (volatile UINT8 *)(J606F_RAMOOPS_CONSOLE + 12);\n\n  if (NumberOfBytes == 0) {\n    return;\n  }\n\n  // Header: Sig, Start (next write position), Size (valid bytes)\n  if (Header[0] != J606F_RAMOOPS_SIG || Header[1] >= J606F_RAMOOPS_DATA || Header[2] > J606F_RAMOOPS_DATA) {\n    Header[1] = 0;\n    Header[2] = 0;\n    Header[0] = J606F_RAMOOPS_SIG;\n  }\n\n  UINT32 First = Header[1];\n  UINT32 Start = First;\n  UINT32 Size  = Header[2];\n\n  for (UINTN i = 0; i < NumberOfBytes; i++) {\n    Data[Start] = Buffer[i];\n    Start       = (Start + 1) % J606F_RAMOOPS_DATA;\n    if (Size < J606F_RAMOOPS_DATA) {\n      Size++;\n    }\n  }\n\n  Header[1] = Start;\n  Header[2] = Size;\n\n  // Parity of every touched data block, then of the header\n  if (NumberOfBytes >= J606F_RAMOOPS_DATA) {\n    for (UINT32 Block = 0; Block < J606F_RAMOOPS_BLOCKS; Block++) {\n      J606fRamoopsEncodeBlock (Block);\n    }\n  } else {\n    UINT32 Block = First / 128;\n    UINT32 Last  = ((First + NumberOfBytes - 1) % J606F_RAMOOPS_DATA) / 128;\n\n    while (TRUE) {\n      J606fRamoopsEncodeBlock (Block);\n      if (Block == Last) {\n        break;\n      }\n\n      Block = (Block + 1) % J606F_RAMOOPS_BLOCKS;\n    }\n  }\n\n  J606fRsEncode ((volatile UINT8 *)Header, 12, Data + J606F_RAMOOPS_DATA + J606F_RAMOOPS_BLOCKS * 16);\n}\n#endif\n\nUINTN\nEFIAPI\nSerialPortWrite (', "J606fRsEncode")

patch(FB, """  UINT64               FbLength;

  // Get Frame Buffer Memory
  Status = GetFrameBufferMemory (&FbBase, &FbLength);""", """  UINT64               FbLength;

#ifdef J606F_RAMOOPS_LOG
  WriteRamoops (Buffer, NumberOfBytes);
#endif

  // Get Frame Buffer Memory
  Status = GetFrameBufferMemory (&FbBase, &FbLength);""", "  WriteRamoops (Buffer, NumberOfBytes);")

patch("Silicon/Silicium/SiliciumPkg/Sec/Sec.c", """  // Do Platform Specific Initialization
  PlatformInitialize ();
""", """#ifdef J606F_RAMOOPS_LOG
  // uefi-winarm (J606F): start a fresh log in the ramoops console zone
  *(volatile UINT32 *)0xFFCA0000 = 0;
#endif

  // Do Platform Specific Initialization
  PlatformInitialize ();
""", "J606F_RAMOOPS_LOG")

BM = "Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/BootManager.c"
# не диагностика, а часть J606F_MSD_ON_NO_OS: без этого UsbMsdDxe не находит EFI_USBFN_IO (лог U3: «Failed to located USB FN IO protocol»)
patch(BM, """    UINTN                         OptionCount;

    Options = EfiBootManagerGetLoadOptions (&OptionCount, LoadOptionTypeBoot);""", """    UINTN                         OptionCount;

    // The USB controller is started on demand (boot menu); start it and connect its drivers so UsbMsdDxe finds the USBFN IO protocol
    PlatformBootManagerOnDemandConInConnect ();
    EfiBootManagerConnectAll ();

    Options = EfiBootManagerGetLoadOptions (&OptionCount, LoadOptionTypeBoot);""", "connect its drivers so UsbMsdDxe")

patch(BM, """    Options = EfiBootManagerGetLoadOptions (&OptionCount, LoadOptionTypeBoot);
    for (UINTN i = 0; i < OptionCount; i++) {
      if (Options[i].Description""", """    Options = EfiBootManagerGetLoadOptions (&OptionCount, LoadOptionTypeBoot);
#ifdef J606F_RAMOOPS_LOG
    DEBUG ((EFI_D_ERROR, "J606F: no OS, %u boot options\\n", OptionCount));
    for (UINTN i = 0; i < OptionCount; i++) {
      DEBUG ((EFI_D_ERROR, "J606F: Boot%04x \\"%s\\" attr 0x%x\\n", Options[i].OptionNumber, Options[i].Description, Options[i].Attributes));
    }
#endif
    for (UINTN i = 0; i < OptionCount; i++) {
      if (Options[i].Description""", "J606F: no OS")

patch(BM, """        EfiBootManagerBoot (&Options[i]);
        break;""", """        EfiBootManagerBoot (&Options[i]);
#ifdef J606F_RAMOOPS_LOG
        DEBUG ((EFI_D_ERROR, "J606F: Mass Storage returned %r\\n", Options[i].Status));
#endif
        break;""", "Mass Storage returned")

patch(BM, """  // Shutdown
  gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);""", """#ifdef J606F_RAMOOPS_LOG
  // uefi-winarm (J606F): warm reset keeps the ramoops log for pmOS
  DEBUG ((EFI_D_ERROR, "J606F: warm reset\\n"));
  gRT->ResetSystem (EfiResetWarm, EFI_SUCCESS, 0, NULL);
#endif

  // Shutdown
  gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);""", "keeps the ramoops log")

MS = "Silicon/Qualcomm/QcomPkg/Applications/MassStorage/MassStorage.c"
patch(MS, """    AssignableMsdLuns = 2;
  }
""", """    AssignableMsdLuns = 2;
  }

#ifdef J606F_RAMOOPS_LOG
  DEBUG ((EFI_D_ERROR, "J606F: QueryMaxLun %r, AssignableMsdLuns %u\\n", Status, AssignableMsdLuns));
#endif
""", "J606F: QueryMaxLun")

patch(MS, """    Status = gBS->LocateDevicePath (&gEfiBlockIoProtocolGuid, &StorageDevicePath, &ProtocolHandle);
    if (EFI_ERROR (Status)) {""", """    Status = gBS->LocateDevicePath (&gEfiBlockIoProtocolGuid, &StorageDevicePath, &ProtocolHandle);
#ifdef J606F_RAMOOPS_LOG
    DEBUG ((EFI_D_ERROR, "J606F: storage %u (%g): %r\\n", i, &StorageGuid[i], Status));
#endif
    if (EFI_ERROR (Status)) {""", "J606F: storage")

patch(MS, """    Status = mUsbMsdProtocol->AssignBlkIoHandle (mUsbMsdProtocol, BlkIoProtocol, AssignedMsdLuns);
    if (!EFI_ERROR (Status)) {""", """    Status = mUsbMsdProtocol->AssignBlkIoHandle (mUsbMsdProtocol, BlkIoProtocol, AssignedMsdLuns);
#ifdef J606F_RAMOOPS_LOG
    DEBUG ((EFI_D_ERROR, "J606F: AssignBlkIoHandle LUN %u: %r (media %u, last LBA 0x%lx)\\n", AssignedMsdLuns, Status,
            BlkIoProtocol->Media->MediaId, BlkIoProtocol->Media->LastBlock));
#endif
    if (!EFI_ERROR (Status)) {""", "J606F: AssignBlkIoHandle")

patch(MS, """  // Start USB Device
  Status = mUsbMsdProtocol->StartDevice (mUsbMsdProtocol);""", """  // Start USB Device
  Status = mUsbMsdProtocol->StartDevice (mUsbMsdProtocol);
#ifdef J606F_RAMOOPS_LOG
  DEBUG ((EFI_D_ERROR, "J606F: StartDevice %r\\n", Status));
#endif""", "J606F: StartDevice")

# тайм-аут MSD в диагностических сборках: без человека выйти из цикла по Громкость+ некому (U4)
patch(MS, """  while (TRUE) {
    // Handle USB Events
    mUsbMsdProtocol->EventHandler (mUsbMsdProtocol);
""", """#ifdef J606F_RAMOOPS_LOG
  // uefi-winarm (J606F): leave Mass Storage after 3 minutes in diagnostic builds (nobody to press Volume Up)
  EFI_EVENT TimeoutEvent = NULL;
  gBS->CreateEvent (EVT_TIMER, 0, NULL, NULL, &TimeoutEvent);
  gBS->SetTimer (TimeoutEvent, TimerRelative, 180ULL * 10000000);
#endif

  while (TRUE) {
#ifdef J606F_RAMOOPS_LOG
    if (TimeoutEvent != NULL && gBS->CheckEvent (TimeoutEvent) == EFI_SUCCESS) {
      DEBUG ((EFI_D_ERROR, "J606F: Mass Storage timeout\\n"));
      break;
    }
#endif

    // Handle USB Events
    mUsbMsdProtocol->EventHandler (mUsbMsdProtocol);
""", "Mass Storage timeout")
