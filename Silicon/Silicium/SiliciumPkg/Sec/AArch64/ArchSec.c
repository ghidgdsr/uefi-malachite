/**
  Copyright (c) 2012-2013, ARM Limited. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>

#include "Sec.h"

//
// Bring-up: the release build has no serial console, so an exception would be
// a silent hang. Paint the bootloader frame buffer before dead-looping, so a
// fault can be told apart from a plain spin, and by its bands from every
// stage colour. No library calls here on purpose: whatever faulted may still
// be broken, and this handler must not fault again.
//
#define SEC_EXCEPTION_FB_BASE  0xFD91F000
#define SEC_EXCEPTION_FB_SIZE  0x026E0000

STATIC
VOID
SecPaintException (
  IN UINT32  Type
  )
{
  volatile UINT64  *FrameBuffer;
  UINTN             Index;
  UINT64            PatternA;
  UINT64            PatternB;

  // Synchronous faults (data/instruction aborts, undef) on yellow/black,
  // every other exception on white/black; 64 KiB bands.
  if (Type == EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS) {
    PatternA = 0xFFFFFF00FFFFFF00ULL;
    PatternB = 0xFF000000FF000000ULL;
  } else {
    PatternA = MAX_UINT64;
    PatternB = 0xFF000000FF000000ULL;
  }

  FrameBuffer = (volatile UINT64 *)(UINTN)SEC_EXCEPTION_FB_BASE;
  for (Index = 0; Index < ((SEC_EXCEPTION_FB_SIZE / sizeof (UINT64)) - 1); Index++) {
    FrameBuffer[Index] = ((Index >> 13) & 1) == 0 ? PatternA : PatternB;
  }
}

VOID
SecCommonExceptionEntry (
  IN UINT32 Type,
  IN UINTN  LR)
{
  CHAR8 Buffer[100];
  UINTN CharCount;

  SecPaintException (Type);

  // Set Exception Message
  switch (Type) {
    case EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS:
      CharCount = AsciiSPrint (Buffer, sizeof (Buffer), "Synchronous Exception at 0x%llx\n", LR);
      break;

    case EXCEPT_AARCH64_IRQ:
      CharCount = AsciiSPrint (Buffer, sizeof (Buffer), "IRQ Exception at 0x%llx\n", LR);
      break;

    case EXCEPT_AARCH64_FIQ:
      CharCount = AsciiSPrint (Buffer, sizeof (Buffer), "FIQ Exception at 0x%llx\n", LR);
      break;

    case EXCEPT_AARCH64_SERROR:
      CharCount = AsciiSPrint (Buffer, sizeof (Buffer), "SError/Abort Exception at 0x%llx\n", LR);
      break;

    default:
      CharCount = AsciiSPrint (Buffer, sizeof (Buffer), "Unknown Exception at 0x%llx\n", LR);
      break;
  }

  // Show Exception Message
  SerialPortWrite ((UINT8 *)Buffer, CharCount);

  // CPU Dead Loop
  CpuDeadLoop ();
}
