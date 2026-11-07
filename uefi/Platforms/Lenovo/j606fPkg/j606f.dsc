##
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = j606f
  PLATFORM_GUID                  = 5E8B3C1A-6F2D-4B7E-9A31-0606F1A2B3C4
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/j606fPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = j606fPkg/j606f.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 0

!include KamortaPkg/KamortaPkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # DDR Memory
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x40000000

  #
  # UEFI Stack
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0x5FF90000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Lenovo"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"Tab P11"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"TB-J606F"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"Lenovo_Tab_P11_TB-J606F"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"Tab P11"

  #
  # Simple Frame Buffer
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1200
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|2000
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

  #
  # Platform PEI
  #
  gQcomPkgTokenSpaceGuid.PcdPlatformType|"LA"

[LibraryClasses]
  #
  # Memory Libraries
  #
  MemoryMapLib|j606fPkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # QCOM Libraries
  #
  ConfigurationMapLib|j606fPkg/Library/ConfigurationMapLib/ConfigurationMapLib.inf

[Components]
  j606fPkg/Application/J606fBoot/J606fBoot.inf
  #
  # ACPI Tables
  #
  j606f/AcpiTables.inf

!if $(TARGET) == DEBUG
[PcdsFixedAtBuild]
  # uefi-winarm: ASSERT печатается, но не останавливает (без экрана зависание не увидеть)
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x07
!endif

[BuildOptions]
  # uefi-winarm: без ОС — режим Mass Storage вместо выключения (проверка без экрана, установка Windows);
  # в DEBUG ещё лог в ramoops pmOS и тёплый сброс вместо выключения (scripts/patch_silicium_diag.py)
!if $(TARGET) == DEBUG
  *_*_*_CC_FLAGS = -DJ606F_MSD_ON_NO_OS -DJ606F_RAMOOPS_LOG
!else
  *_*_*_CC_FLAGS = -DJ606F_MSD_ON_NO_OS
!endif
