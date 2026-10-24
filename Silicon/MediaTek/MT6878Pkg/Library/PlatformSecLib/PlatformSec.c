/**
  Copyright (c) 2011-2012, ARM Limited. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/PlatformSecLib.h>

#include "PlatformRegisters.h"

//
// Bring-up probe left by the BootShim before it touches the MMU: the RAM hole
// between the UEFI stack (ends at 0x41240000) and the DTB copy region (starts
// at 0x41680000), which the UEFI memory map does not describe.
//
#define SEC_BOOT_STATE_ADDR   0x41240000ULL
#define SEC_BOOT_STATE_MAGIC  0xA55A000000000000ULL
#define SEC_BOOT_STATE_MASK   0xFFFF000000000000ULL

//
// Colours are written as 0xAARRGGBB with the byte order the panel scans out.
//
#define SEC_COLOR_BLACK   0xFF000000
#define SEC_COLOR_WHITE   0xFFFFFFFF
#define SEC_COLOR_RED     0xFFFF0000
#define SEC_COLOR_GREEN   0xFF00FF00
#define SEC_COLOR_BLUE    0xFF0000FF
#define SEC_COLOR_CYAN    0xFF00FFFF
#define SEC_COLOR_MAGENTA 0xFFFF00FF
#define SEC_COLOR_YELLOW  0xFFFFFF00

VOID
DisableWatchDogTimer ()
{
  EFI_STATUS                   Status;
  EFI_MEMORY_REGION_DESCRIPTOR WDogRegion;

  // Locate "WatchDog Timer" Memory Region
  Status = LocateMemoryRegionByName ("WatchDog Timer", &WDogRegion);
  if (EFI_ERROR (Status)) {
    return;
  }

  // Disable WatchDog Timer
  MmioWrite32 (WDogRegion.Address, WDT_MODE_KEY);
}

//
// Fill the whole bootloader framebuffer with one colour, straight through the
// "Display Reserved" memory region (no GOP, no font).
//
STATIC
VOID
PaintScreen (
  IN UINT32  Color
  )
{
  EFI_MEMORY_REGION_DESCRIPTOR  Region;
  volatile UINT64              *FrameBuffer;
  UINTN                         Index;
  UINTN                         Count;
  UINT64                        Pattern;

  ZeroMem (&Region, sizeof (Region));
  LocateMemoryRegionByName ("Display Reserved", &Region);
  LocateMemoryRegionByName ("Display_Reserved", &Region);
  if (Region.Address == 0 || Region.Length < sizeof (UINT64)) {
    return;
  }

  Pattern     = ((UINT64)Color << 32) | Color;
  FrameBuffer = (volatile UINT64 *)(UINTN)Region.Address;
  Count       = Region.Length / sizeof (UINT64);
  for (Index = 0; Index < Count; Index++) {
    FrameBuffer[Index] = Pattern;
  }
}

STATIC
VOID
HoldScreen (
  VOID
  )
{
  UINT64  Count = 0x30000000ULL;

  // A plain counting loop: the generic timer may not be running yet, and the
  // BootShim has interrupts masked, so no timer can tell us how long we held.
  while (Count-- != 0) {
    __asm__ volatile ("nop");
  }
}

VOID
PlatformInitialize ()
{
  UINT64  BootState;
  UINT64  CurrentEl;
  UINT64  Vhe;
  UINT64  MmuOn;
  UINT32  Color;
  UINTN   Blink;

  // Disable WatchDog Timer
  DisableWatchDogTimer ();

  //
  // Show what the previous stage handed over: the exception level, whether
  // EL2 was running with VHE (HCR_EL2.E2H) and whether that regime's MMU was
  // on when the BootShim took control. The BootShim recorded this word before
  // it disabled anything.
  //
  //   WHITE    EL1, LK's MMU was ON  (normal)     GREEN  EL1, MMU was OFF
  //   CYAN     EL2, MMU was ON                   YELLOW EL2, MMU was OFF
  //   MAGENTA  EL2 with VHE, MMU was ON          BLUE   EL2 with VHE, MMU OFF
  //   RED      EL3                               BLACK  no probe word
  //
  BootState = *(volatile UINT64 *)(UINTN)SEC_BOOT_STATE_ADDR;
  CurrentEl = (BootState >> 8) & 0xF;
  Vhe       = (BootState >> 1) & 1;
  MmuOn     = BootState & 1;

  if ((BootState & SEC_BOOT_STATE_MASK) != SEC_BOOT_STATE_MAGIC) {
    Color = SEC_COLOR_BLACK;
  } else if (CurrentEl == 1) {
    Color = MmuOn ? SEC_COLOR_WHITE : SEC_COLOR_GREEN;
  } else if (CurrentEl == 2) {
    if (Vhe != 0) {
      Color = MmuOn ? SEC_COLOR_MAGENTA : SEC_COLOR_BLUE;
    } else {
      Color = MmuOn ? SEC_COLOR_CYAN : SEC_COLOR_YELLOW;
    }
  } else if (CurrentEl == 3) {
    Color = SEC_COLOR_RED;
  } else {
    Color = SEC_COLOR_BLACK;
  }

  // Blink it three times, so the opening colour cannot be confused with the
  // static colour the stage ladder leaves behind later.
  for (Blink = 0; Blink < 3; Blink++) {
    PaintScreen (Color);
    HoldScreen ();
    PaintScreen (SEC_COLOR_BLACK);
    HoldScreen ();
  }

  // Neutral colour the SEC stage ladder starts from.
  PaintScreen (SEC_COLOR_WHITE);
}
