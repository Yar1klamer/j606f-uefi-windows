/** @file
  J606F boot menu (UEFI application, \EFI\Boot\bootaa64.efi on the esp partition, started by Mu-Silicium "Internal Storage").

  Windows: chain-loads \EFI\Microsoft\Boot\bootmgfw.efi from the same partition.
  postmarketOS: reads the Android boot image (header v2) of partition boot_b, unpacks the gzip'ed arm64 kernel (EFI stub), installs the
  image's DTB section as the FDT configuration table and the ramdisk through LoadFile2 (LINUX_EFI_INITRD_MEDIA_GUID), and starts the
  kernel with the image's command line. Nothing is written anywhere.

  Keys: Volume Up / Down select, Power (Enter) boots; no key for the timeout -> default entry.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/DevicePathLib.h>
#include <Library/PrintLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/BlockIo.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/LoadFile2.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/HiiFont.h>
#include <Guid/FileInfo.h>
#include <Guid/LinuxEfiInitrdMedia.h>
#include <Guid/Fdt.h>
#include "Inflate.h"

#define MENU_TIMEOUT_S  5
#define DEFAULT_ENTRY   0       // 0 Windows, 1 postmarketOS
#define BOOT_PART       L"boot_b"
#define EXTRA_ARGS      " efi=noruntime"

STATIC CONST CHAR16  *mEntries[] = { L"Windows 11", L"postmarketOS" };
STATIC CONST CHAR16  *mSub[]     = { L"Windows on ARM, UEFI", L"Linux from boot_b" };

// ---- initrd through LoadFile2 (as EmbeddedPkg AndroidBootImgLib) ----
#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH          VendorMediaNode;
  EFI_DEVICE_PATH_PROTOCOL    EndNode;
} RAMDISK_DEVICE_PATH;
#pragma pack()

STATIC CONST RAMDISK_DEVICE_PATH  mRamdiskDevicePath = {
  {
    { MEDIA_DEVICE_PATH, MEDIA_VENDOR_DP, { sizeof (VENDOR_DEVICE_PATH), 0 } },
    LINUX_EFI_INITRD_MEDIA_GUID
  },
  { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 } }
};

STATIC VOID   *mRamdisk;
STATIC UINTN  mRamdiskSize;

STATIC
EFI_STATUS
EFIAPI
RamdiskLoadFile2 (
  IN EFI_LOAD_FILE2_PROTOCOL   *This,
  IN EFI_DEVICE_PATH_PROTOCOL  *FilePath,
  IN BOOLEAN                   BootPolicy,
  IN OUT UINTN                 *BufferSize,
  IN VOID                      *Buffer OPTIONAL
  )
{
  if ((This == NULL) || (BufferSize == NULL) || (FilePath == NULL) || !IsDevicePathValid (FilePath, 0)) {
    return EFI_INVALID_PARAMETER;
  }
  if (BootPolicy) {
    return EFI_UNSUPPORTED;
  }
  if ((Buffer == NULL) || (*BufferSize < mRamdiskSize)) {
    *BufferSize = mRamdiskSize;
    return EFI_BUFFER_TOO_SMALL;
  }
  CopyMem (Buffer, mRamdisk, mRamdiskSize);
  *BufferSize = mRamdiskSize;
  return EFI_SUCCESS;
}

STATIC EFI_LOAD_FILE2_PROTOCOL  mRamdiskLoadFile2 = { RamdiskLoadFile2 };

// ---- log on the esp: \EFI\Boot\j606fboot.log (overwritten on every start; read it from pmOS) ----
STATIC EFI_FILE_PROTOCOL  *mRoot;
STATIC CHAR8              mLog[8192];
STATIC UINTN              mLogLen;

STATIC
VOID
LogFlush (
  VOID
  )
{
  EFI_FILE_PROTOCOL  *F;
  UINTN              N = mLogLen;

  if ((mRoot == NULL) || EFI_ERROR (mRoot->Open (mRoot, &F, L"\\EFI\\Boot\\j606fboot.log",
                                                  EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0)))
  {
    return;
  }
  F->SetPosition (F, 0);
  F->Write (F, &N, mLog);
  F->Flush (F);
  F->Close (F);
}

// Print to the screen and to the log buffer
STATIC
VOID
EFIAPI
Say (
  IN CONST CHAR16  *Fmt,
  ...
  )
{
  VA_LIST  Ap;
  CHAR16   Line[512];
  UINTN    I;

  VA_START (Ap, Fmt);
  UnicodeVSPrint (Line, sizeof (Line), Fmt, Ap);
  VA_END (Ap);
  Print (L"%s", Line);
  for (I = 0; Line[I] != 0 && mLogLen < sizeof (mLog) - 2; I++) {
    if (Line[I] == L'\n') {
      mLog[mLogLen++] = '\r';
    }
    mLog[mLogLen++] = Line[I] < 0x80 ? (CHAR8)Line[I] : '?';
  }
}

STATIC
VOID
OpenEsp (
  IN EFI_HANDLE  ImageHandle
  )
{
  EFI_LOADED_IMAGE_PROTOCOL        *Self;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;

  if (!EFI_ERROR (gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Self)) &&
      !EFI_ERROR (gBS->HandleProtocol (Self->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs)))
  {
    if (EFI_ERROR (Fs->OpenVolume (Fs, &mRoot))) {
      mRoot = NULL;
    }
  }
}

// ---- helpers ----
STATIC
VOID
Fail (
  IN CONST CHAR16  *What,
  IN EFI_STATUS    Status
  )
{
  Say (L"\n  ERROR: %s (%r)\n  Windows will start in 10 s.\n", What, Status);
  LogFlush ();
  gBS->Stall (10 * 1000 * 1000);
}

STATIC
UINT32
Le32 (
  IN CONST UINT8  *P
  )
{
  return (UINT32)P[0] | ((UINT32)P[1] << 8) | ((UINT32)P[2] << 16) | ((UINT32)P[3] << 24);
}

STATIC
UINTN
Pages (
  IN UINTN  Size,
  IN UINTN  PageSize
  )
{
  return (Size + PageSize - 1) / PageSize;
}

STATIC
EFI_STATUS
FindPartition (
  IN  CONST CHAR16           *Name,
  OUT EFI_BLOCK_IO_PROTOCOL  **BlockIo
  )
{
  EFI_HANDLE                   *Handles;
  UINTN                        Count, Index;
  EFI_STATUS                   Status;
  EFI_PARTITION_INFO_PROTOCOL  *Info;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiPartitionInfoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = EFI_NOT_FOUND;
  for (Index = 0; Index < Count; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiPartitionInfoProtocolGuid, (VOID **)&Info))) {
      continue;
    }
    if ((Info->Type != PARTITION_TYPE_GPT) || (StrnCmp (Info->Info.Gpt.PartitionName, Name, 36) != 0)) {
      continue;
    }
    Status = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID **)BlockIo);
    break;
  }
  FreePool (Handles);
  return Status;
}

// Reads Size bytes from the start of the partition into a page buffer (rounded up to the block size).
STATIC
EFI_STATUS
ReadPart (
  IN  EFI_BLOCK_IO_PROTOCOL  *BlockIo,
  IN  UINTN                  Size,
  OUT UINT8                  **Buffer
  )
{
  UINTN       Bs   = BlockIo->Media->BlockSize;
  UINTN       Len  = (Size + Bs - 1) / Bs * Bs;
  EFI_STATUS  Status;

  if (Len > MultU64x32 (BlockIo->Media->LastBlock + 1, (UINT32)Bs)) {
    return EFI_BAD_BUFFER_SIZE;
  }
  *Buffer = AllocatePages (EFI_SIZE_TO_PAGES (Len));
  if (*Buffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Status = BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId, 0, Len, *Buffer);
  if (EFI_ERROR (Status)) {
    FreePages (*Buffer, EFI_SIZE_TO_PAGES (Len));
    *Buffer = NULL;
  }
  return Status;
}


// ---- last choice: \EFI\Boot\j606fboot.def ('0' Windows, '1' postmarketOS) ----
STATIC
UINTN
LoadDefault (
  VOID
  )
{
  EFI_FILE_PROTOCOL  *F;
  CHAR8              C = 0;
  UINTN              N = 1;

  if ((mRoot == NULL) || EFI_ERROR (mRoot->Open (mRoot, &F, L"\\EFI\\Boot\\j606fboot.def", EFI_FILE_MODE_READ, 0))) {
    return DEFAULT_ENTRY;
  }
  F->Read (F, &N, &C);
  F->Close (F);
  return (N == 1 && C == '1') ? 1 : 0;
}

STATIC
VOID
SaveDefault (
  IN UINTN  Sel
  )
{
  EFI_FILE_PROTOCOL  *F;
  CHAR8              C = Sel ? '1' : '0';
  UINTN              N = 1;

  if ((mRoot == NULL) || EFI_ERROR (mRoot->Open (mRoot, &F, L"\\EFI\\Boot\\j606fboot.def",
                                                  EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0)))
  {
    return;
  }
  F->SetPosition (F, 0);
  F->Write (F, &N, &C);
  F->Flush (F);
  F->Close (F);
}

// pmOS reboot mark (device-lenovo-j606f j606f-reboot-flag.sh): "J606F-REBOOT" at 0xF0000 of misc. The initramfs charger hook
// consumes it and boots normally instead of showing "Hold Power to boot" (a reboot reads as a cable power-on in the PMIC).
STATIC
VOID
SetRebootMark (
  VOID
  )
{
  EFI_BLOCK_IO_PROTOCOL  *Bio;
  UINT8                  *Buf;
  UINTN                  Bs;
  EFI_LBA                Lba;

  if (EFI_ERROR (FindPartition (L"misc", &Bio))) {
    Say (L"  misc not found: no reboot mark\n");
    return;
  }
  Bs = Bio->Media->BlockSize;
  if ((Bs == 0) || (0xF0000 % Bs) != 0) {
    return;
  }
  Lba = 0xF0000 / Bs;
  Buf = AllocatePages (EFI_SIZE_TO_PAGES (Bs));
  if (Buf == NULL) {
    return;
  }
  if (!EFI_ERROR (Bio->ReadBlocks (Bio, Bio->Media->MediaId, Lba, Bs, Buf))) {
    CopyMem (Buf, "J606F-REBOOT", 12);
    if (!EFI_ERROR (Bio->WriteBlocks (Bio, Bio->Media->MediaId, Lba, Bs, Buf))) {
      Bio->FlushBlocks (Bio);
      Say (L"  reboot mark set (no charger screen)\n");
    }
  }
  FreePages (Buf, EFI_SIZE_TO_PAGES (Bs));
}

// ---- graphical menu (GOP + HII system font glyphs, scaled) ----
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL  *mGop;
STATIC EFI_HII_FONT_PROTOCOL         *mFont;

#define RGB(r, g, b)  { (b), (g), (r), 0 }
STATIC CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  cBg      = RGB (0x0e, 0x11, 0x16);
STATIC CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  cCard    = RGB (0x1d, 0x22, 0x2b);
STATIC CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  cBorder  = RGB (0x3a, 0x42, 0x50);
STATIC CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  cWin     = RGB (0x00, 0x78, 0xd4);
STATIC CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  cPmos    = RGB (0x00, 0x9a, 0x6e);
STATIC CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  cText    = RGB (0xf2, 0xf4, 0xf7);
STATIC CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  cDim     = RGB (0x8a, 0x93, 0xa3);

STATIC
VOID
Fill (
  IN UINTN                                X,
  IN UINTN                                Y,
  IN UINTN                                W,
  IN UINTN                                H,
  IN CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *C
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  P = *C;

  mGop->Blt (mGop, &P, EfiBltVideoFill, 0, 0, X, Y, W, H, 0);
}

// One pass over the string with the HII system font (8 x 19 glyphs): measure only (Draw = FALSE) or draw at X, Y scaled S times.
STATIC
UINTN
TextPass (
  IN UINTN                                X,
  IN UINTN                                Y,
  IN UINTN                                S,
  IN CONST CHAR16                         *Str,
  IN CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Fg,
  IN BOOLEAN                              Draw
  )
{
  UINTN                          I, Gx, Gy, W = 0;
  EFI_IMAGE_OUTPUT               *G;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Px;

  for (I = 0; Str[I] != 0; I++) {
    G = NULL;
    if (EFI_ERROR (mFont->GetGlyph (mFont, Str[I], NULL, &G, NULL)) || (G == NULL)) {
      W += 8 * S;
      continue;
    }
    if (Draw) {
      for (Gy = 0; Gy < G->Height; Gy++) {
        for (Gx = 0; Gx < G->Width; Gx++) {
          Px = &G->Image.Bitmap[Gy * G->Width + Gx];
          if ((UINTN)Px->Red + Px->Green + Px->Blue > 0x180) {
            Fill (X + W + Gx * S, Y + Gy * S, S, S, Fg);
          }
        }
      }
    }
    W += G->Width * S;
    if (G->Image.Bitmap != NULL) {
      FreePool (G->Image.Bitmap);
    }
    FreePool (G);
  }
  return W;
}

// Text scaled S times; Center = TRUE: X is the horizontal center. Returns the width.
STATIC
UINTN
Text (
  IN UINTN                                X,
  IN UINTN                                Y,
  IN UINTN                                S,
  IN CONST CHAR16                         *Str,
  IN CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL  *Fg,
  IN BOOLEAN                              Center
  )
{
  UINTN  W = TextPass (0, 0, S, Str, Fg, FALSE);

  return TextPass (Center ? (X > W / 2 ? X - W / 2 : 0) : X, Y, S, Str, Fg, TRUE);
}

STATIC
VOID
Card (
  IN UINTN    Idx,
  IN BOOLEAN  Sel,
  IN UINTN    Y,
  IN UINTN    H
  )
{
  UINTN                                  Sw = mGop->Mode->Info->HorizontalResolution;
  UINTN                                  X = Sw / 10, W = Sw - 2 * (Sw / 10), B = Sel ? 8 : 3;
  CONST EFI_GRAPHICS_OUTPUT_BLT_PIXEL    *Acc = Idx == 0 ? &cWin : &cPmos;

  Fill (X, Y, W, H, Sel ? Acc : &cBorder);
  Fill (X + B, Y + B, W - 2 * B, H - 2 * B, Sel ? Acc : &cCard);
  Fill (X + B, Y + B, 24, H - 2 * B, Sel ? &cText : Acc);                    // colour strip on the left
  Text (Sw / 2, Y + H / 2 - 19 * 4 / 2 - 34, 4, mEntries[Idx], &cText, TRUE);
  Text (Sw / 2, Y + H / 2 + 34, 2, mSub[Idx], Sel ? &cText : &cDim, TRUE);
}

STATIC
VOID
GDraw (
  IN UINTN  Sel,
  IN INTN   Left
  )
{
  UINTN   Sw = mGop->Mode->Info->HorizontalResolution, Sh = mGop->Mode->Info->VerticalResolution;
  UINTN   Ch = Sh / 7, Y0 = Sh * 34 / 100;
  CHAR16  Line[80];

  Fill (0, 0, Sw, Sh * 30 / 100, &cBg);
  Text (Sw / 2, Sh * 12 / 100, 7, L"J606F", &cText, TRUE);
  Text (Sw / 2, Sh * 12 / 100 + 19 * 7 + 30, 2, L"Lenovo Tab P11  -  choose a system", &cDim, TRUE);
  Card (0, Sel == 0, Y0, Ch);
  Card (1, Sel == 1, Y0 + Ch + Sh / 30, Ch);
  Fill (0, Sh * 78 / 100, Sw, Sh * 22 / 100, &cBg);
  Text (Sw / 2, Sh * 80 / 100, 2, L"Volume Up / Down  -  select", &cDim, TRUE);
  Text (Sw / 2, Sh * 80 / 100 + 60, 2, L"Power  -  boot", &cDim, TRUE);
  if (Left >= 0) {
    UnicodeSPrint (Line, sizeof (Line), L"Starting %s in %d s", mEntries[Sel], Left);
  } else {
    UnicodeSPrint (Line, sizeof (Line), L" ");
  }
  Fill (0, Sh * 87 / 100, Sw, 19 * 3 + 20, &cBg);
  Text (Sw / 2, Sh * 88 / 100, 3, Line, &cText, TRUE);
}

STATIC
BOOLEAN
GInit (
  VOID
  )
{
  if (EFI_ERROR (gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID **)&mGop)) &&
      EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&mGop)))
  {
    return FALSE;
  }
  if (EFI_ERROR (gBS->LocateProtocol (&gEfiHiiFontProtocolGuid, NULL, (VOID **)&mFont))) {
    return FALSE;
  }
  return mGop->Mode->Info->HorizontalResolution >= 600 && mGop->Mode->Info->VerticalResolution >= 600;
}

// ---- postmarketOS ----
STATIC
EFI_STATUS
BootPmos (
  IN EFI_HANDLE  ImageHandle
  )
{
  EFI_BLOCK_IO_PROTOCOL      *BlockIo;
  EFI_LOADED_IMAGE_PROTOCOL  *Loaded;
  EFI_HANDLE                 RamdiskHandle = NULL, KernelHandle = NULL;
  EFI_STATUS                 Status;
  UINT8                      *Hdr, *Img, *Kernel, *Fdt;
  UINT32                     KSize, RSize, SSize, PageSize, Version, RecSize = 0, DtbSize = 0;
  UINTN                      Total, KOff, ROff, DOff, KOut;
  UINT64                     Out;
  CHAR8                      Args[512 + 1024 + 64];
  CHAR16                     *Args16;
  UINTN                      ArgsLen, I;
  int                        Rc;

  Say (L"\n  postmarketOS: reading %s ...\n", BOOT_PART);
  Status = FindPartition (BOOT_PART, &BlockIo);
  if (EFI_ERROR (Status)) {
    Fail (L"partition boot_b not found", Status);
    return Status;
  }
  Status = ReadPart (BlockIo, 4096, &Hdr);
  if (EFI_ERROR (Status)) {
    Fail (L"read boot_b header", Status);
    return Status;
  }
  if (CompareMem (Hdr, "ANDROID!", 8) != 0) {
    Fail (L"boot_b holds no Android boot image", EFI_VOLUME_CORRUPTED);
    return EFI_VOLUME_CORRUPTED;
  }
  KSize    = Le32 (Hdr + 8);
  RSize    = Le32 (Hdr + 16);
  SSize    = Le32 (Hdr + 24);
  PageSize = Le32 (Hdr + 36);
  Version  = Le32 (Hdr + 40);
  if ((PageSize < 2048) || (PageSize > 65536) || (Version > 2)) {
    Fail (L"unsupported boot image (page size / header version)", EFI_UNSUPPORTED);
    return EFI_UNSUPPORTED;
  }
  if (Version >= 1) {
    RecSize = Le32 (Hdr + 1632);
  }
  if (Version >= 2) {
    DtbSize = Le32 (Hdr + 1648);
  }
  // command line: cmdline[512] @64 + extra_cmdline[1024] @608 + ours
  AsciiStrnCpyS (Args, sizeof (Args), (CHAR8 *)Hdr + 64, 511);
  if (Hdr[608] != 0) {
    AsciiStrnCatS (Args, sizeof (Args), (CHAR8 *)Hdr + 608, 1023);
  }
  AsciiStrCatS (Args, sizeof (Args), EXTRA_ARGS);
  Say (L"  header v%u, page %u, kernel %u, ramdisk %u, dtb %u bytes\n", Version, PageSize, KSize, RSize, DtbSize);
  if (DtbSize == 0) {
    Fail (L"boot image has no DTB section", EFI_UNSUPPORTED);
    return EFI_UNSUPPORTED;
  }
  KOff  = PageSize;
  ROff  = KOff + Pages (KSize, PageSize) * PageSize;
  DOff  = ROff + (Pages (RSize, PageSize) + Pages (SSize, PageSize) + Pages (RecSize, PageSize)) * PageSize;
  Total = DOff + Pages (DtbSize, PageSize) * PageSize;
  Status = ReadPart (BlockIo, Total, &Img);
  if (EFI_ERROR (Status)) {
    Fail (L"read boot_b", Status);
    return Status;
  }

  // kernel: gzip -> Image (PE/COFF EFI stub)
  Out = GzipSize (Img + KOff, KSize);
  if ((Out < 0x10000) || (Out > 0x8000000)) {
    Fail (L"kernel is not gzip or has a bad size", EFI_UNSUPPORTED);
    return EFI_UNSUPPORTED;
  }
  Kernel = AllocatePages (EFI_SIZE_TO_PAGES ((UINTN)Out));
  if (Kernel == NULL) {
    Fail (L"no memory for the kernel", EFI_OUT_OF_RESOURCES);
    return EFI_OUT_OF_RESOURCES;
  }
  Say (L"  unpacking the kernel (%lu bytes) ...\n", Out);
  Rc = GunzipBuf (Kernel, &Out, Img + KOff, KSize);
  if ((Rc != 0) || (Crc32 (Kernel, Out) != Le32 (Img + KOff + KSize - 8))) {
    Say (L"  inflate rc %d, %lu bytes\n", Rc, Out);
    Fail (L"kernel unpack / CRC failed", EFI_CRC_ERROR);
    return EFI_CRC_ERROR;
  }
  KOut = (UINTN)Out;
  if ((Kernel[0] != 'M') || (Kernel[1] != 'Z')) {
    Fail (L"kernel has no EFI stub (no MZ header)", EFI_UNSUPPORTED);
    return EFI_UNSUPPORTED;
  }

  // DTB -> FDT configuration table (the EFI stub copies it and adds /chosen itself)
  Fdt = AllocateCopyPool (DtbSize, Img + DOff);
  if ((Fdt == NULL) || (Le32 (Fdt) != 0xedfe0dd0)) {
    Fail (L"DTB section is not a flattened device tree", EFI_VOLUME_CORRUPTED);
    return EFI_VOLUME_CORRUPTED;
  }
  Status = gBS->InstallConfigurationTable (&gFdtTableGuid, Fdt);
  if (EFI_ERROR (Status)) {
    Fail (L"install FDT", Status);
    return Status;
  }

  // ramdisk -> LoadFile2
  mRamdisk     = Img + ROff;
  mRamdiskSize = RSize;
  Status       = gBS->InstallMultipleProtocolInterfaces (
                        &RamdiskHandle,
                        &gEfiLoadFile2ProtocolGuid,
                        &mRamdiskLoadFile2,
                        &gEfiDevicePathProtocolGuid,
                        &mRamdiskDevicePath,
                        NULL
                        );
  if (EFI_ERROR (Status)) {
    Fail (L"install initrd LoadFile2", Status);
    return Status;
  }

  Status = gBS->LoadImage (FALSE, ImageHandle, NULL, Kernel, KOut, &KernelHandle);
  if (EFI_ERROR (Status)) {
    Fail (L"LoadImage(kernel)", Status);
    return Status;
  }
  ArgsLen = AsciiStrLen (Args);
  Args16  = AllocatePool ((ArgsLen + 1) * sizeof (CHAR16));
  if (Args16 == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  for (I = 0; I <= ArgsLen; I++) {
    Args16[I] = (CHAR16)(UINT8)Args[I];
  }
  gBS->HandleProtocol (KernelHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
  Loaded->LoadOptions     = Args16;
  Loaded->LoadOptionsSize = (UINT32)((ArgsLen + 1) * sizeof (CHAR16));
  SetRebootMark ();
  Say (L"  starting Linux: %a\n", Args);
  LogFlush ();
  Status = gBS->StartImage (KernelHandle, NULL, NULL);
  // only on failure
  gBS->InstallConfigurationTable (&gFdtTableGuid, NULL);
  Fail (L"kernel returned", Status);
  return Status;
}

// ---- Windows ----
STATIC
EFI_STATUS
BootWindows (
  IN EFI_HANDLE  ImageHandle
  )
{
  EFI_LOADED_IMAGE_PROTOCOL  *Self;
  EFI_DEVICE_PATH_PROTOCOL   *Path;
  EFI_HANDLE                 Handle;
  EFI_STATUS                 Status;

  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Self);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Path = FileDevicePath (Self->DeviceHandle, L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi");
  if (Path == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Status = gBS->LoadImage (FALSE, ImageHandle, Path, NULL, 0, &Handle);
  if (EFI_ERROR (Status)) {
    Print (L"\n  ERROR: cannot load \\EFI\\Microsoft\\Boot\\bootmgfw.efi (%r)\n", Status);
    gBS->Stall (5 * 1000 * 1000);
    return Status;
  }
  return gBS->StartImage (Handle, NULL, NULL);
}

// ---- menu ----
STATIC
VOID
DrawMenu (
  IN UINTN  Sel,
  IN INTN   Left
  )
{
  UINTN  I;

  gST->ConOut->SetCursorPosition (gST->ConOut, 0, 2);
  for (I = 0; I < ARRAY_SIZE (mEntries); I++) {
    gST->ConOut->SetAttribute (gST->ConOut, I == Sel ? EFI_TEXT_ATTR (EFI_BLACK, EFI_LIGHTGRAY) : EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));
    Print (L"  %s %s  \n", I == Sel ? L">" : L" ", mEntries[I]);
  }
  gST->ConOut->SetAttribute (gST->ConOut, EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK));
  if (Left >= 0) {
    Print (L"\n  Volume Up/Down: choose, Power: boot. Auto boot in %d s   \n", Left);
  } else {
    Print (L"\n  Volume Up/Down: choose, Power: boot.                     \n");
  }
}

EFI_STATUS
EFIAPI
J606fBootMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_EVENT      Timer, Events[2];
  EFI_INPUT_KEY  Key;
  UINTN          Sel, Which;
  BOOLEAN        Gfx;
  INTN           Left = MENU_TIMEOUT_S * 10;     // 100 ms ticks; < 0 after the first key (no timeout)

  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  OpenEsp (ImageHandle);
  Sel = LoadDefault ();
  gST->ConOut->ClearScreen (gST->ConOut);
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);
  Gfx = GInit ();
  if (Gfx) {
    Fill (0, 0, mGop->Mode->Info->HorizontalResolution, mGop->Mode->Info->VerticalResolution, &cBg);
  }
  if (!Gfx) {
    Print (L"  J606F boot menu\n");
  }
  gST->ConIn->Reset (gST->ConIn, FALSE);
  gBS->CreateEvent (EVT_TIMER, 0, NULL, NULL, &Timer);
  gBS->SetTimer (Timer, TimerPeriodic, 1000 * 1000);    // 100 ms
  Events[0] = gST->ConIn->WaitForKey;
  Events[1] = Timer;
  Gfx ? GDraw (Sel, (Left + 9) / 10) : DrawMenu (Sel, (Left + 9) / 10);
  for (;;) {
    gBS->WaitForEvent (2, Events, &Which);
    if (Which == 1) {
      if (Left < 0) {
        continue;
      }
      if (--Left == 0) {
        break;
      }
      if (Left % 10 == 0) {
        Gfx ? GDraw (Sel, Left / 10) : DrawMenu (Sel, Left / 10);
      }
      continue;
    }
    if (EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) {
      continue;
    }
    Left = -1;
    if ((Key.ScanCode == SCAN_UP) || (Key.ScanCode == SCAN_DOWN) || (Key.ScanCode == SCAN_PAGE_UP) || (Key.ScanCode == SCAN_PAGE_DOWN)) {
      Sel = (Sel + 1) % ARRAY_SIZE (mEntries);
      Gfx ? GDraw (Sel, -1) : DrawMenu (Sel, -1);
      continue;
    }
    if ((Key.UnicodeChar == CHAR_CARRIAGE_RETURN) || (Key.UnicodeChar == L' ') || (Key.ScanCode == SCAN_SUSPEND)) {
      break;
    }
    // any other key: log its code (to learn the key map of this device)
    if (Gfx) {
      continue;
    }
    gST->ConOut->SetCursorPosition (gST->ConOut, 0, 8);
    Print (L"  key: scan %04x char %04x            \n", Key.ScanCode, Key.UnicodeChar);
  }
  gBS->SetTimer (Timer, TimerCancel, 0);
  gBS->CloseEvent (Timer);
  gST->ConOut->ClearScreen (gST->ConOut);
  if (Gfx) {
    CHAR16  Msg[64];
    UINTN   Sh = mGop->Mode->Info->VerticalResolution;
    UnicodeSPrint (Msg, sizeof (Msg), L"Starting %s ...", mEntries[Sel]);
    Fill (0, Sh / 2 - 40, mGop->Mode->Info->HorizontalResolution, 19 * 3 + 80, &cBg);
    Text (mGop->Mode->Info->HorizontalResolution / 2, Sh / 2, 3, Msg, Sel ? &cPmos : &cWin, TRUE);
  }
  SaveDefault (Sel);
  Say (L"  J606F boot menu: %s\n", mEntries[Sel]);
  LogFlush ();
  if (Sel == 1) {
    BootPmos (ImageHandle);
  }
  return BootWindows (ImageHandle);
}
