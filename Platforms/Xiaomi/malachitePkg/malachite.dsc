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
  PLATFORM_NAME                  = malachite
  PLATFORM_GUID                  = 7B2E5A10-6C4D-4F1B-9A2E-3D5C8B7F1A20
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/malachitePkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = malachitePkg/malachite.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 0

!include MT6878Pkg/MT6878Pkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # DDR Memory
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x40000000

  #
  # UEFI Stack
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0x41200000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Xiaomi"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"POCO X7"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"malachite"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"POCO_X7_malachite"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"POCO X7"

  #
  # Simple Frame Buffer
  #
  # The panel is 1220x2712. MediaTek aligns the scanout pitch to 32 pixels,
  # so the buffer stride is 1248 pixels (1220 -> 1248).
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1220
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|2712
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferStride|1248

[LibraryClasses]
  #
  # Memory Libraries
  #
  MemoryMapLib|malachitePkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # Input Libraries
  #
  KeypadDeviceLib|malachitePkg/Library/KeypadDeviceLib/KeypadDeviceLib.inf

  #
  # Storage Libraries (SCSI bus/disk on top of the UFS pass-thru)
  #
  UefiScsiLib|MdePkg/Library/UefiScsiLib/UefiScsiLib.inf

[Components]
  #
  # Input
  #
  SiliciumPkg/Drivers/KeypadDxe/KeypadDxe.inf
  SiliciumPkg/Drivers/KeypadDeviceDxe/KeypadDeviceDxe.inf

  #
  # Storage: UFS -> UfsPassThru -> SCSI bus -> SCSI disk (BlockIo)
  #
  malachitePkg/Drivers/UfsMtkDxe/UfsMtkDxe.inf
  MdeModulePkg/Bus/Ufs/UfsPassThruDxe/UfsPassThruDxe.inf
  MdeModulePkg/Bus/Scsi/ScsiBusDxe/ScsiBusDxe.inf
  MdeModulePkg/Bus/Scsi/ScsiDiskDxe/ScsiDiskDxe.inf

  #
  # Applications
  #
  malachitePkg/Applications/DemoApp/DemoApp.inf
