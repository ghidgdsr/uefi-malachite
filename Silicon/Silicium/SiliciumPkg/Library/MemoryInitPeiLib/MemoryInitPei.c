/**
  Copyright (c) 2011-2015, ARM Limited. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/PcdLib.h>
#include <Library/SortLib.h>
#include <Library/ArmMmuLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MemoryMapLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>

VOID
BuildMemoryTypeInformationHob ();

//
// Bring-up ladder: paint the whole panel before each risky step, straight
// through the "Display Reserved" memory region, and hold it long enough to be
// seen. The colour left behind names the last step completed. Sec.c continues
// the ladder once MemoryPeim returns (black = success, 128 KiB bands = error).
//
#define PEI_COLOR_BLUE    0xFF0000FFFF0000FFULL
#define PEI_COLOR_CYAN    0xFF00FFFFFF00FFFFULL
#define PEI_COLOR_YELLOW  0xFFFFFF00FFFFFF00ULL
#define PEI_COLOR_GREEN   0xFF00FF00FF00FF00ULL
#define PEI_COLOR_RED     0xFFFF0000FFFF0000ULL
#define PEI_COLOR_MAGENTA 0xFFFF00FFFFFF00FFULL
#define PEI_COLOR_WHITE   MAX_UINT64
#define PEI_COLOR_BLACK   0xFF000000FF000000ULL

STATIC
VOID
PaintProgress (
  IN UINT64  Pattern
  )
{
#ifdef MDEPKG_NDEBUG
  EFI_MEMORY_REGION_DESCRIPTOR  Region;
  volatile UINT64              *FrameBuffer;
  UINTN                         Index;
  UINTN                         Count;

  Region.Address = 0;
  Region.Length  = 0;
  LocateMemoryRegionByName ("Display Reserved", &Region);
  LocateMemoryRegionByName ("Display_Reserved", &Region);
  if (Region.Address == 0 || Region.Length < sizeof (UINT64)) {
    return;
  }

  // The frame buffer console keeps its cursor in the last 8 bytes; leave it.
  FrameBuffer = (volatile UINT64 *)(UINTN)Region.Address;
  Count       = (Region.Length / sizeof (UINT64)) - 1;
  for (Index = 0; Index < Count; Index++) {
    FrameBuffer[Index] = Pattern;
  }
#else
  // Debug builds print their log to this frame buffer; do not erase it.
  (VOID)Pattern;
#endif
}

//
// Same, as alternating bands of two patterns. Index counts 64-bit cells, so a
// shift of N gives bands of 2^N * 8 bytes.
//
STATIC
VOID
PaintProgressBands (
  IN UINTN   BandShift,
  IN UINT64  PatternA,
  IN UINT64  PatternB
  )
{
#ifdef MDEPKG_NDEBUG
  EFI_MEMORY_REGION_DESCRIPTOR  Region;
  volatile UINT64              *FrameBuffer;
  UINTN                         Index;
  UINTN                         Count;

  Region.Address = 0;
  Region.Length  = 0;
  LocateMemoryRegionByName ("Display Reserved", &Region);
  LocateMemoryRegionByName ("Display_Reserved", &Region);
  if (Region.Address == 0 || Region.Length < sizeof (UINT64)) {
    return;
  }

  // The frame buffer console keeps its cursor in the last 8 bytes; leave it.
  FrameBuffer = (volatile UINT64 *)(UINTN)Region.Address;
  Count       = (Region.Length / sizeof (UINT64)) - 1;
  for (Index = 0; Index < Count; Index++) {
    FrameBuffer[Index] = ((Index >> BandShift) & 1) == 0 ? PatternA : PatternB;
  }
#else
  (VOID)BandShift;
  (VOID)PatternA;
  (VOID)PatternB;
#endif
}

STATIC
VOID
HoldProgress (
  VOID
  )
{
#ifdef MDEPKG_NDEBUG
  UINT64  Count = 0x20000000ULL;

  // A counting loop: no timer setup is assumed this early.
  while (Count-- != 0) {
    __asm__ volatile ("nop");
  }
#endif
}

//
// One colour per descriptor handled in the HOB loop, so the bands left on the
// panel name the descriptor whose handling stopped.
//
STATIC CONST UINT64  mHobPaintColors[] = {
  PEI_COLOR_BLUE,
  PEI_COLOR_CYAN,
  PEI_COLOR_YELLOW,
  PEI_COLOR_GREEN,
  PEI_COLOR_RED,
  PEI_COLOR_MAGENTA,
  PEI_COLOR_WHITE
};

STATIC
INTN
EFIAPI
CompareMemoryRegions (
  IN CONST VOID *P1,
  IN CONST VOID *P2)
{
  // Populate Memory Region Structure
  CONST EFI_MEMORY_REGION_DESCRIPTOR *Region1 = (EFI_MEMORY_REGION_DESCRIPTOR *)P1;
  CONST EFI_MEMORY_REGION_DESCRIPTOR *Region2 = (EFI_MEMORY_REGION_DESCRIPTOR *)P2;

  // Compare Base Addresses
  if (Region1->Address < Region2->Address) {
    return -1;
  }

  // Compare Base Addresses
  if (Region1->Address > Region2->Address) {
    return 1;
  }

  return 0;
}

STATIC
VOID
ValidateMemoryMap (
  IN OUT EFI_MEMORY_REGION_DESCRIPTOR *MemoryDescriptor,
  IN     UINT8                         MemoryDescriptorCount)
{
  BOOLEAN IsInvalid = FALSE;

  // Verify Memory Region Count
  ASSERT (MemoryDescriptorCount - 1 < MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT);

  // Sort Memory Regions
  PerformQuickSort (MemoryDescriptor, MemoryDescriptorCount, sizeof (EFI_MEMORY_REGION_DESCRIPTOR), CompareMemoryRegions);

  // Marker: sort done (yellow full screen). The sort allocates its scratch
  // buffer from the HOB list, so a hang on yellow means the first CreateHob.
  PaintProgress (PEI_COLOR_YELLOW);
  HoldProgress ();

  // Go thru each Memory Region
  for (UINT8 i = 0; i < MemoryDescriptorCount - 1; i++) {
    // Get Current & Next Memory Region
    EFI_MEMORY_REGION_DESCRIPTOR *CurrentRegion = &MemoryDescriptor[i];
    EFI_MEMORY_REGION_DESCRIPTOR *NextRegion    = &MemoryDescriptor[i + 1];

    // Calculate Current End Address
    EFI_PHYSICAL_ADDRESS EndAddress = CurrentRegion->Address + CurrentRegion->Length;

    // Skip Register Regions
    if (CurrentRegion->Address < FixedPcdGet64 (PcdSystemMemoryBase)) {
      continue;
    }

    // Check for Overlap
    if (EndAddress > NextRegion->Address) {
      DEBUG ((EFI_D_ERROR, "\"%a\" (0x%llx) and \"%a\" (0x%llx) Overlap!\n", CurrentRegion->Name, CurrentRegion->Address, NextRegion->Name, NextRegion->Address));

      // Set Memory Map State
      IsInvalid = TRUE;
    }
  }

  // Marker: overlap check done (cyan full screen).
  PaintProgress (PEI_COLOR_CYAN);
  HoldProgress ();

  // Check Memory Map State
  if (IsInvalid) {
    // Marker: the map was rejected as overlapping (64 KiB red/black bands).
    PaintProgressBands (13, PEI_COLOR_RED, PEI_COLOR_BLACK);
    CpuDeadLoop ();
  }
}

STATIC
VOID
AddHob (IN EFI_MEMORY_REGION_DESCRIPTOR MemoryRegion)
{
  // Save HOB Option
  EFI_MEMORY_HOB_OPTION HobOption = MemoryRegion.HobOption;

  // Build Resource Descriptor HOB
  if (HobOption == AddMem || HobOption == AddDev || HobOption == HobOnlyNoCacheSetting) {
    BuildResourceDescriptorHob (MemoryRegion.ResourceType, MemoryRegion.ResourceAttribute, MemoryRegion.Address, MemoryRegion.Length);
  }

  // Build Memory Allocation HOB
  if (MemoryRegion.ResourceType == EFI_RESOURCE_SYSTEM_MEMORY || MemoryRegion.MemoryType == EfiRuntimeServicesData) {
    BuildMemoryAllocationHob (MemoryRegion.Address, MemoryRegion.Length, MemoryRegion.MemoryType);
  }
}

EFI_STATUS
EFIAPI
MemoryPeim (
  IN EFI_PHYSICAL_ADDRESS UefiMemoryBase,
  IN UINT64               UefiMemorySize)
{
  EFI_STATUS                    Status                                              = EFI_SUCCESS;
  ARM_MEMORY_REGION_DESCRIPTOR  MemoryTable[MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT] = {0};
  EFI_MEMORY_REGION_DESCRIPTOR *MemoryDescriptor                                    = NULL;
  VOID                         *TranslationTableBase                                = NULL;
  UINTN                         TranslationTableSize                                = 0;
  UINT8                         MemoryDescriptorCount                               = 0;
  UINTN                         Index                                               = 0;

  // Marker: MemoryPeim entered (blue full screen).
  PaintProgress (PEI_COLOR_BLUE);
  HoldProgress ();

  // Show Entry Message
  DEBUG ((EFI_D_WARN, "MMU In\r"));

  // Get Memory Map
  GetMemoryMap (&MemoryDescriptor, &MemoryDescriptorCount);

  DEBUG ((EFI_D_WARN, "MMU: %u regions\n", (UINT32)MemoryDescriptorCount));

  // Marker: memory map fetched (magenta full screen).
  PaintProgress (PEI_COLOR_MAGENTA);
  HoldProgress ();

  // Validate Memory Map
  ValidateMemoryMap (MemoryDescriptor, MemoryDescriptorCount);
  DEBUG ((EFI_D_WARN, "MMU: map valid\n"));

  // Go thru each Memory Region
  for (UINT8 i = 0; i < MemoryDescriptorCount; i++) {
    //
    // Marker: about to handle descriptor i (16 KiB colour/black bands). The
    // last bands left name the descriptor whose handling stopped; i >= 7 is
    // black/white, since black-on-black would be invisible.
    //
    if (i < ARRAY_SIZE (mHobPaintColors)) {
      PaintProgressBands (11, mHobPaintColors[i], PEI_COLOR_BLACK);
    } else {
      PaintProgressBands (11, PEI_COLOR_BLACK, PEI_COLOR_WHITE);
    }

    HoldProgress ();

    // Add HOB
    AddHob (MemoryDescriptor[i]);

    // Verify Memory Region HOB Option
    if (MemoryDescriptor[i].HobOption == HobOnlyNoCacheSetting) {
      continue;
    }

    // Save Attributes
    MemoryTable[Index].PhysicalBase = MemoryDescriptor[i].Address;
    MemoryTable[Index].VirtualBase  = MemoryDescriptor[i].Address;
    MemoryTable[Index].Length       = MemoryDescriptor[i].Length;
    MemoryTable[Index].Attributes   = MemoryDescriptor[i].ArmAttributes;

    // Increase Memory Table Index
    Index++;
  }

  DEBUG ((EFI_D_WARN, "MMU: HOBs built, enabling\n"));

  // Marker: HOBs and MMU table built, ArmConfigureMmu next (green).
  PaintProgress (PEI_COLOR_GREEN);
  HoldProgress ();

  // Configure MMU
  Status = ArmConfigureMmu (MemoryTable, &TranslationTableBase, &TranslationTableSize);
  DEBUG ((EFI_D_WARN, "MMU: enabled\n"));
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Configure MMU! Status = %r\n", Status));
    return Status;
  }

  // Marker: ArmConfigureMmu returned, the MMU is on (red full screen). If the
  // panel stays green, the fault is inside ArmConfigureMmu.
  PaintProgress (PEI_COLOR_RED);
  HoldProgress ();

  // Build Memory Type Info HOB
  if (FeaturePcdGet (PcdPrePiProduceMemoryTypeInformationHob)) {
    BuildMemoryTypeInformationHob ();
  }

  return EFI_SUCCESS;
}
