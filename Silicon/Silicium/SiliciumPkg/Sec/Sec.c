/**
  Copyright (c) 2011-2017, ARM Limited. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/PcdLib.h>
#include <Library/DebugLib.h>
#include <Library/PrePiLib.h>
#include <Library/DebugAgentLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/SerialPortLib.h>
#include <Library/PlatformSecLib.h>
#include <Library/PrePiHobListPointerLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/TimerLib.h>
#include <Library/HobLib.h>

#include "Sec.h"

// First argument (x0) of the previous stage, saved by ModuleEntryPoint.S
extern UINT64  gSecEntryArg0;

// The library constructors ProcessLibraryConstructorList () runs for this module, in its generated order, called one by one so the screen
// shows which one a stall is in (an entry here that the generated list lacks fails to link, which is the check).
// Helper.S: clean and invalidate every data cache level by set/way (see there).
extern VOID SecCleanInvalidateDataCacheAll (VOID);

extern RETURN_STATUS EFIAPI BaseDebugLibSerialPortConstructor (VOID);
extern RETURN_STATUS EFIAPI ExtractGuidedSectionLibConstructor (VOID);
extern RETURN_STATUS EFIAPI ArmMmuBaseLibConstructor (VOID);
extern RETURN_STATUS EFIAPI LzmaDecompressLibConstructor (VOID);

// Walks the HOB list with a bound, so a zero-length or runaway entry is reported instead of looping forever in the constructors and in
// the FV decompression that follow (both search the list).
STATIC
VOID
CheckHobList (VOID)
{
  EFI_PEI_HOB_POINTERS  Hob;
  UINTN                 Count = 0;

  Hob.Raw = GetHobList ();
  while (!END_OF_HOB_LIST (Hob)) {
    if (Hob.Header->HobLength < sizeof (EFI_HOB_GENERIC_HEADER) || Count >= 16384) {
      DEBUG ((EFI_D_ERROR, "SEC: HOB list broken at entry %u: type 0x%x length %u\n", (UINT32)Count, (UINT32)Hob.Header->HobType, (UINT32)Hob.Header->HobLength));
      return;
    }

    Count++;
    Hob.Raw = GET_NEXT_HOB (Hob);
  }

  DEBUG ((EFI_D_WARN, "SEC: HOB list ok, %u entries\n", (UINT32)Count));
}

#ifndef MDEPKG_NDEBUG
STATIC
VOID
ClearFrameBuffer (VOID)
{
  EFI_MEMORY_REGION_DESCRIPTOR FrameBufferRegion;

  // Locate "Display Reserved" Memory Region
  LocateMemoryRegionByName ("Display Reserved", &FrameBufferRegion);
  LocateMemoryRegionByName ("Display_Reserved", &FrameBufferRegion);

  // Clear Memory Region
  if (FrameBufferRegion.Address != 0 && FrameBufferRegion.Length != 0) {
    ZeroMem ((VOID *)FrameBufferRegion.Address, FrameBufferRegion.Length);
  }
}

STATIC
VOID
PrintFirmwareVersion (VOID)
{
  // Clear Frame Buffer
  ClearFrameBuffer ();

  // Print UEFI Version Message
  DEBUG ((EFI_D_WARN, "\n"));
  DEBUG ((EFI_D_WARN, "Project Silicium for %a %a\n",            FixedPcdGetPtr (PcdSmbiosSystemManufacturer), FixedPcdGetPtr (PcdSmbiosSystemModel)));
  DEBUG ((EFI_D_WARN, "Firmware Version %s Build at %a on %a\n", PcdGetPtr (PcdFirmwareVersionString), __TIME__, __DATE__));
  DEBUG ((EFI_D_WARN, "\n"));
}
#endif

//
// Defined below; declared here so the memory init failure paths can paint the
// screen before returning.
//
STATIC
VOID
SecPaintFill (
  IN UINT64  Pattern
  );

STATIC
VOID
SecPaintBands (
  IN UINTN   BandShift,
  IN UINT64  PatternA,
  IN UINT64  PatternB
  );

//
// Panel colours, 0xAARRGGBB in the byte order the bootloader framebuffer scans
// out (0xFFFF0000 is red).
//
#define SEC_COLOR_BLACK   0xFF000000FF000000ULL
#define SEC_COLOR_WHITE   MAX_UINT64
#define SEC_COLOR_RED     0xFFFF0000FFFF0000ULL
#define SEC_COLOR_GREEN   0xFF00FF00FF00FF00ULL
#define SEC_COLOR_BLUE    0xFF0000FFFF0000FFULL
#define SEC_COLOR_CYAN    0xFF00FFFFFF00FFFFULL
#define SEC_COLOR_MAGENTA 0xFFFF00FFFFFF00FFULL
#define SEC_COLOR_YELLOW  0xFFFFFF00FFFFFF00ULL

STATIC
EFI_STATUS
InitializeMemory (VOID)
{
  EFI_STATUS                    Status;
  EFI_MEMORY_REGION_DESCRIPTOR  UefiMemoryRegion;
  EFI_HOB_HANDOFF_INFO_TABLE   *HobList;

  //
  // Marker: InitializeMemory entered (64 KiB red/blue bands). When this is
  // the colour left on the panel, the hang is in the memory region lookup,
  // the HOB list constructor or the call into MemoryPeim.
  //
  SecPaintBands (13, SEC_COLOR_RED, SEC_COLOR_BLUE);

  // Locate "DXE Heap" Memory Region
  LocateMemoryRegionByName ("DXE Heap", &UefiMemoryRegion);
  LocateMemoryRegionByName ("DXE_Heap", &UefiMemoryRegion);

  // Save UEFI Memory Details
  UINTN UefiMemoryBase = UefiMemoryRegion.Address;
  UINTN UefiMemorySize = UefiMemoryRegion.Length;

  // Verify UEFI Memory Base
  if (UefiMemoryBase == 0 && UefiMemorySize == 0) {
    DEBUG ((EFI_D_ERROR, "Failed to Locate \"DXE Heap\" Memory Region!\n"));
    // Marker: "DXE Heap" region lookup failed (128 KiB red/white bands).
    SecPaintBands (14, SEC_COLOR_RED, SEC_COLOR_WHITE);
    return EFI_NOT_FOUND;
  }

  // Initialize HOB List
  HobList = HobConstructor ((VOID *)UefiMemoryBase, UefiMemorySize, (VOID *)UefiMemoryBase, (VOID *)(UefiMemoryBase + UefiMemorySize));
  PrePeiSetHobList (HobList);

  //
  // Bring-up check: the HOB pointer travels in TPIDR_EL0 and every later
  // allocation reads it back. If it does not survive, or the first pool
  // allocation (the first CreateHob) does not land inside the heap, stop with
  // a distinct pattern instead of failing somewhere unrelated.
  //
  if (GetHobList () != HobList) {
    // 128 KiB magenta/white bands.
    SecPaintBands (14, SEC_COLOR_MAGENTA, SEC_COLOR_WHITE);
    return EFI_DEVICE_ERROR;
  }

  {
    VOID  *TestPool = AllocatePool (64);

    if ((UINTN)TestPool < UefiMemoryBase || (UINTN)TestPool + 64 > UefiMemoryBase + UefiMemorySize) {
      // 128 KiB cyan/white bands.
      SecPaintBands (14, SEC_COLOR_CYAN, SEC_COLOR_WHITE);
      return EFI_OUT_OF_RESOURCES;
    }
  }

  // Initialize MMU & Memory HOBs
  Status = MemoryPeim (UefiMemoryBase, UefiMemorySize);
  if (EFI_ERROR (Status)) {
    // Marker: MemoryPeim / ArmConfigureMmu failed (128 KiB green/white bands).
    SecPaintBands (14, SEC_COLOR_GREEN, SEC_COLOR_WHITE);
    return Status;
  }

  // Build Stack HOB
  BuildStackHob (FixedPcdGet64 (PcdCPUCoresStackBase), FixedPcdGet32 (PcdCPUCorePrimaryStackSize));

  return EFI_SUCCESS;
}

//
// Bring-up progress markers. Every stage paints the WHOLE bootloader
// framebuffer through the "Display Reserved" memory region (no GOP, no font),
// so this works before and after the MMU is enabled. Only the last colour
// completed stays on the panel, so the final colour names the stage the
// firmware stopped in. MemoryInitPeiLib continues the ladder inside
// MemoryPeim (blue, magenta, yellow, cyan, per-descriptor bands, green, red).
//
//   WHITE                    PlatformInitialize done, InitializeMemory not entered
//   32 KiB RED/BLUE bands    InitializeMemory entered (heap lookup, HOB list)
//   BLACK                    InitializeMemory returned (memory + MMU ready)
//   WHITE                    PlatformPeim returned
//   32 KiB WHITE/BLACK bands library constructors done
//   32 KiB YELLOW/BLACK bands FV decompressed, DXE core next
//
// Failure paths use 128 KiB bands, so they cannot be mistaken for a stage:
//   RED/WHITE     "DXE Heap" lookup failed
//   GREEN/WHITE   MemoryPeim returned an error
//   MAGENTA/WHITE PlatformPeim failed, or the HOB pointer was lost
//   CYAN/WHITE    the first pool allocation did not land in the heap
//   RED/BLACK     (64 KiB) MemoryPeim rejected the map as overlapping
//
// The exception vectors paint 64 KiB yellow/black bands for a synchronous
// fault and white/black for any other exception (see AArch64/ArchSec.c).
//
STATIC
VOID
SecFrameBuffer (
  OUT volatile UINT64  **FrameBuffer,
  OUT UINTN             *Count
  )
{
  *FrameBuffer = NULL;
  *Count       = 0;

  //
  // In a debug build the frame buffer is the debug console: painting it would
  // erase the log, so the ladder only runs in release builds.
  //
#ifdef MDEPKG_NDEBUG
  EFI_MEMORY_REGION_DESCRIPTOR  Region;

  ZeroMem (&Region, sizeof (Region));
  LocateMemoryRegionByName ("Display Reserved", &Region);
  LocateMemoryRegionByName ("Display_Reserved", &Region);
  if (Region.Address == 0 || Region.Length < sizeof (UINT64)) {
    return;
  }

  //
  // Leave the last 8 bytes alone: the frame buffer console keeps its cursor
  // there (FrameBufferSerialPortLib), and a debug build prints through it.
  //
  *FrameBuffer = (volatile UINT64 *)(UINTN)Region.Address;
  *Count       = (Region.Length / sizeof (UINT64)) - 1;
#endif
}

STATIC
VOID
SecPaintFill (
  IN UINT64  Pattern
  )
{
  volatile UINT64  *FrameBuffer;
  UINTN            Index;
  UINTN            Count;

  SecFrameBuffer (&FrameBuffer, &Count);
  if (FrameBuffer == NULL) {
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    FrameBuffer[Index] = Pattern;
  }
}

STATIC
VOID
SecPaintBands (
  IN UINTN   BandShift,
  IN UINT64  PatternA,
  IN UINT64  PatternB
  )
{
  volatile UINT64  *FrameBuffer;
  UINTN            Index;
  UINTN            Count;

  SecFrameBuffer (&FrameBuffer, &Count);
  for (Index = 0; Index < Count; Index++) {
    FrameBuffer[Index] = ((Index >> BandShift) & 1) == 0 ? PatternA : PatternB;
  }
}

STATIC
VOID
SecMain (VOID)
{
  EFI_STATUS Status;

  // Do Platform Specific Initialization
  PlatformInitialize ();

#ifndef MDEPKG_NDEBUG
  // Initialize Serial Port
  Status = SerialPortInitialize ();
  if (EFI_ERROR (Status)) {
    return;
  }

  // Print Firmware Version
  PrintFirmwareVersion ();

  // Initialize Debug Agent
  InitializeDebugAgent (DEBUG_AGENT_INIT_POSTMEM_SEC, NULL, NULL);
  SaveAndSetDebugTimerInterrupt (TRUE);
#endif

  // Initialize Memory
  Status = InitializeMemory ();
  if (EFI_ERROR (Status)) {
    return;
  }

  DEBUG ((EFI_D_WARN, "SEC: memory HOBs done\n"));

  // Marker: memory HOBs and page tables done, MMU on (black full screen).
  SecPaintFill (SEC_COLOR_BLACK);

  // Build CPU HOB
  BuildCpuHob (ArmGetPhysicalAddressBits (), PcdGet8 (PcdPrePiCpuIoSize));

  // Set the Boot Mode
  SetBootMode (BOOT_WITH_DEFAULT_SETTINGS);

  // Initialize Platform HOBs
  Status = PlatformPeim ();
  DEBUG ((EFI_D_WARN, "SEC: platform HOBs done (%r)\n", Status));
  if (EFI_ERROR (Status)) {
    // Marker: PlatformPeim failed (128 KiB magenta/white bands).
    SecPaintBands (14, SEC_COLOR_MAGENTA, SEC_COLOR_WHITE);
    return;
  }

  // Marker: platform HOBs done (white full screen).
  SecPaintFill (SEC_COLOR_WHITE);

  // Pass on what the previous stage handed over in x0 (a DT address for a
  // Linux-style boot loader), so a DXE driver can use it.
  {
    UINT64  EntryArg = gSecEntryArg0;

    BuildGuidDataHob (&gSiliciumEntryArgHobGuid, &EntryArg, sizeof (EntryArg));
  }

  CheckHobList ();

  // Run Library Cconstructors
  DEBUG ((EFI_D_WARN, "SEC: constructor debug\n"));
#ifndef MDEPKG_NDEBUG
  BaseDebugLibSerialPortConstructor ();
#endif
  DEBUG ((EFI_D_WARN, "SEC: constructor guided-section\n"));
  ExtractGuidedSectionLibConstructor ();
  DEBUG ((EFI_D_WARN, "SEC: constructor mmu\n"));
  ArmMmuBaseLibConstructor ();
  DEBUG ((EFI_D_WARN, "SEC: constructor lzma\n"));
  LzmaDecompressLibConstructor ();
  DEBUG ((EFI_D_WARN, "SEC: constructors done\n"));

  // Marker: constructors done (32 KiB white/black bands).
  SecPaintBands (12, SEC_COLOR_WHITE, SEC_COLOR_BLACK);

  CheckHobList ();

  // Decompress all FVs
  Status = DecompressFirstFv ();
  DEBUG ((EFI_D_WARN, "SEC: FV decompressed (%r)\n", Status));
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Decompress FV! Status = %r\n", Status));
    return;
  }

  // Marker: FV decompressed, DXE core is next (32 KiB yellow/black bands).
  SecPaintBands (12, SEC_COLOR_YELLOW, SEC_COLOR_BLACK);

  // Load DXE Core
  DEBUG ((EFI_D_WARN, "SEC: loading DXE core\n"));
  Status = LoadDxeCoreFromFv (NULL, 0);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Load DXE Core! Status = %r\n", Status));
  }
}

VOID
SecEntry (VOID)
{
  // The previous stage ran with its caches on. Clean and invalidate them before anything else: the UEFI writes its HOB tables and heap with
  // the MMU and caches off, and lines the previous stage left would hide those writes (or be written back over them) once the caches are on.
  SecCleanInvalidateDataCacheAll ();

  // Disable Data Cache
  ArmDisableDataCache ();

  // Invalidate Instruction Cache
  ArmInvalidateInstructionCache ();

  // Enable Instruction Cache
  ArmEnableInstructionCache ();

  // Write back and invalidate Stack & UEFI FD D-Cache. A plain invalidate would discard what the entry code just stored there
  // (live stack frames, gSecEntryArg0) if the previous stage left the data cache on.
  WriteBackInvalidateDataCacheRange ((VOID *)FixedPcdGet64 (PcdCPUCoresStackBase), FixedPcdGet32 (PcdCPUCorePrimaryStackSize));
  WriteBackInvalidateDataCacheRange ((VOID *)FixedPcdGet64 (PcdFdBaseAddress),     FixedPcdGet32 (PcdFdSize));

  // Verify Exception Vector Table
  ASSERT (((UINTN)SecVectorTable & ARM_VECTOR_TABLE_ALIGNMENT) == 0);

  // Enable new Exception Vector Table
  ArmWriteVBar ((UINTN)SecVectorTable);

  // Enter Main SEC Function
  SecMain ();
}
