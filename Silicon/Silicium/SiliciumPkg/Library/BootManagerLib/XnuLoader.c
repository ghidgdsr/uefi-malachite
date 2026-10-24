/**
  Start the HackiOS XNU loader from the "XNUP" payload (malachite bring-up).

  The boot shim copies the container appended after the UEFI FD to
  XNU_PAYLOAD_BASE. If it carries "loader.efi", that is the real XNU loader:
  start it straight from memory, so the loader can be iterated on without
  rebuilding this firmware.

  The panel is painted white just before the loader starts: white with text is
  the loader's console, solid white is a loader that prints nowhere, and the
  SEC exception colour (yellow/black) is a fault on the way in.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include "XnuLoader.h"

#define XNU_FB_BASE   0xFD91F000
#define XNU_FB_SIZE   0x026E0000

//
// The loader leaves its log here before returning (xnuloader/loader.c, LOG_MBOX_*).
//
#define XNU_LOG_MBOX     (XNU_PAYLOAD_BASE + XNU_PAYLOAD_MAX_SIZE - 0x80000ULL)
#define XNU_LOG_MAGIC    0x474F4C58u   // "XLOG"
#define XNU_LOG_LIMIT    0x7F000u
#define XNU_LOG_FAULT_OFF 0x7F800u     // u32: 1 = a previous boot's fault log

//
// The framebuffer serial port keeps every DEBUG byte in a ring inside the
// reserved RAM hole (FrameBufferSerialPortLib.c); the loader log clears the
// panel, so print its tail first and the UFS/SCSI driver messages survive.
//
#define UEFI_DEBUG_LOG_BASE    0x41400000ULL
#define UEFI_DEBUG_LOG_MAGIC   0x47424455u   // "UDBG"
#define UEFI_DEBUG_LOG_SIZE    0x00080000u
#define UEFI_DEBUG_LOG_MARK    0x414FF000ULL  // see FrameBufferSerialPortLib.c
#define UEFI_DEBUG_TAIL_SIZE   512u

typedef struct {
  UINT32  Magic;
  UINT32  Head;
  UINT32  Total;
  UINT32  Wrapped;
  UINT8   Data[UEFI_DEBUG_LOG_SIZE];
} UEFI_DEBUG_LOG_RING;

STATIC
UINT32
CopyDebugLogTail (
  OUT CHAR8   *Out,
  IN  UINT32   OutSize
  )
{
  volatile UEFI_DEBUG_LOG_RING  *Ring;
  UINT32                         Take;
  UINT32                         Start;

  Ring = (volatile UEFI_DEBUG_LOG_RING *)(UINTN)UEFI_DEBUG_LOG_BASE;
  if (Ring->Magic != UEFI_DEBUG_LOG_MAGIC) {
    return 0;
  }

  Take = Ring->Wrapped ? UEFI_DEBUG_LOG_SIZE : Ring->Total;
  if (Take > UEFI_DEBUG_LOG_SIZE) {
    Take = UEFI_DEBUG_LOG_SIZE;
  }

  if (Take > OutSize) {
    Take = OutSize;
  }

  if (Take == 0) {
    return 0;
  }

  Start = (Ring->Head + UEFI_DEBUG_LOG_SIZE - Take) % UEFI_DEBUG_LOG_SIZE;
  for (UINT32 i = 0; i < Take; i++) {
    Out[i] = Ring->Data[(Start + i) % UEFI_DEBUG_LOG_SIZE];
  }

  return Take;
}

//
// The frame buffer console keeps its cursor in the last 8 bytes of the Display
// Reserved region: {XPos, YPos} in character cells.
//
#define XNU_FB_BASE   0xFD91F000
#define XNU_FB_SIZE   0x026E0000

STATIC
VOID
PrintLogChunked (
  IN CHAR8  *Text,
  IN UINT32  Len
  )
{
  CHAR8  Chunk[201];
  UINT32 Offset;

  //
  // BaseDebugLibSerialPort formats into a 256-byte buffer; anything longer is
  // silently cut. Feed it 200 characters at a time instead.
  //
  for (Offset = 0; Offset < Len; Offset += 200) {
    UINT32 Count = Len - Offset;

    if (Count > 200) {
      Count = 200;
    }

    CopyMem (Chunk, Text + Offset, Count);
    Chunk[Count] = 0;
    DEBUG ((EFI_D_ERROR, "%a", Chunk));
  }
}

STATIC
VOID
PrintLoaderLog (VOID)
{
  volatile UINT32              *Header      = (volatile UINT32 *)(UINTN)XNU_LOG_MBOX;
  volatile UEFI_DEBUG_LOG_RING *DebugRing   = (volatile UEFI_DEBUG_LOG_RING *)(UINTN)UEFI_DEBUG_LOG_BASE;
  volatile UINT32              *Mark        = (volatile UINT32 *)(UINTN)UEFI_DEBUG_LOG_MARK;
  CHAR8                        *Text;
  CHAR8                         DebugTail[UEFI_DEBUG_TAIL_SIZE];
  UINT32                        DebugLen;
  UINT32                        DebugTotal;
  UINT32                        DebugWrapped;
  BOOLEAN                       HaveLog;

  //
  // Snapshot before printing: every DEBUG call below appends to the ring.
  //
  DebugTotal   = DebugRing->Total;
  DebugWrapped = DebugRing->Wrapped;
  DebugLen     = CopyDebugLogTail (DebugTail, sizeof (DebugTail));

  HaveLog = (Header[0] == XNU_LOG_MAGIC) && (Header[1] <= XNU_LOG_LIMIT);
  if (!HaveLog) {
    DEBUG ((EFI_D_ERROR, "XNU: no loader log mailbox (magic %08x size %x)\n", Header[0], Header[1]));
  }

  Text = (CHAR8 *)(UINTN)(XNU_LOG_MBOX + 8);

  {
    volatile UINT32 *Cursor = (volatile UINT32 *)(UINTN)(XNU_FB_BASE + XNU_FB_SIZE - 8);

    DEBUG ((EFI_D_ERROR, "\f"));
    Cursor[0] = 0;
    Cursor[1] = 8;
  }

  //
  // The loader log first and the firmware's own log last: the failing
  // driver's last lines are what is being looked for, and they stay on the
  // panel even when the two together are taller than it.
  //
  if (HaveLog) {
    DEBUG ((EFI_D_ERROR, "== XNULOADER LOG ==\n"));
    PrintLogChunked (Text, Header[1]);
    DEBUG ((EFI_D_ERROR, "\n== END ==\n"));
  }

  DEBUG ((
    EFI_D_ERROR,
    "dbg: ring total=%d wrapped=%d, serial-init=%d at %08x, ring-init=%d\n",
    DebugTotal,
    DebugWrapped,
    Mark[0],
    Mark[1],
    Mark[2]
    ));

  if (DebugLen != 0) {
    DEBUG ((EFI_D_ERROR, "== UEFI DEBUG (last %d bytes) ==\n", DebugLen));
    PrintLogChunked (DebugTail, DebugLen);
    DEBUG ((EFI_D_ERROR, "\n"));
  }
}

STATIC
VOID
StartPayloadLoader (VOID)
{
  XNU_PAYLOAD_HEADER *Header = (XNU_PAYLOAD_HEADER *)(UINTN)XNU_PAYLOAD_BASE;
  UINT8              *Cursor;
  UINT8              *End;

  if (Header->Magic != XNU_PAYLOAD_MAGIC || Header->Size > XNU_PAYLOAD_MAX_SIZE) {
    DEBUG ((EFI_D_ERROR, "XNU: no payload at %lx (magic %08x)\n", XNU_PAYLOAD_BASE, Header->Magic));
    return;
  }

  Cursor = (UINT8 *)(Header + 1);
  End    = Cursor + Header->Size;

  while (Cursor + sizeof (XNU_PAYLOAD_ENTRY) <= End) {
    XNU_PAYLOAD_ENTRY *Entry = (XNU_PAYLOAD_ENTRY *)Cursor;
    EFI_HANDLE         Handle;

    if (Entry->Name[0] == 0) {
      break;
    }

    DEBUG ((EFI_D_ERROR, "XNU: payload entry '%a' size %x\n", Entry->Name, Entry->Size));

    if (AsciiStrnCmp (Entry->Name, "loader.efi", sizeof (Entry->Name)) == 0) {
      EFI_STATUS Status;

      DEBUG ((EFI_D_ERROR, "XNU: loader.efi found, %x bytes at %p\n", Entry->Size, Entry + 1));

      Status = gBS->LoadImage (FALSE, gImageHandle, NULL, Entry + 1, Entry->Size, &Handle);
      DEBUG ((EFI_D_ERROR, "XNU: LoadImage = %r (handle %p)\n", Status, Handle));
      if (!EFI_ERROR (Status)) {
        Status = gBS->StartImage (Handle, NULL, NULL);
        DEBUG ((EFI_D_ERROR, "XNU: StartImage = %r\n", Status));
        PrintLoaderLog ();
      }
      return;
    }

    Cursor += sizeof (XNU_PAYLOAD_ENTRY) + ALIGN_VALUE (Entry->Size, 16);
  }

  DEBUG ((EFI_D_ERROR, "XNU: no loader.efi in the payload\n"));
}

VOID
XnuLoaderMain ()
{
  volatile UINT32 *Header = (volatile UINT32 *)(UINTN)XNU_LOG_MBOX;

  //
  // A previous boot may have left its log here; print it (in 200-character
  // chunks: DebugLib cuts any single message at 255) and stop, so the panel
  // keeps it. The flag is not required: after a reset the log is the signal.
  //
  if (Header[0] == XNU_LOG_MAGIC && Header[1] > 0 && Header[1] <= XNU_LOG_LIMIT) {
    volatile UINT32 *Cursor = (volatile UINT32 *)(UINTN)(XNU_FB_BASE + XNU_FB_SIZE - 8);

    // Consume the mailbox: a log is printed once, so what is seen is always the
    // latest boot's, never a stale copy that survived the reset.
    Header[0] = 0;

    // \f clears the panel; start below the punch hole so the panel's camera cutout
    // does not cover the first lines.
    DEBUG ((EFI_D_ERROR, "\f"));
    Cursor[0] = 0;
    Cursor[1] = 8;

    DEBUG ((EFI_D_ERROR, "== PREVIOUS BOOT LOG ==\n"));
    PrintLogChunked ((CHAR8 *)(UINTN)(XNU_LOG_MBOX + 8), Header[1]);
    DEBUG ((EFI_D_ERROR, "\n== END ==\n"));
    CpuDeadLoop ();
  }

  //
  // The loader may spend minutes reading the system image from UFS before it
  // starts the kernel; the firmware watchdog must not reset the device in the
  // middle of that transfer.
  //
  gBS->SetWatchdogTimer (0, 0, 0, NULL);

  //
  // Returns when the payload has no loader (the BDS demo path then still runs).
  //
  StartPayloadLoader ();

  //
  // Bring-up probe: did the bootloader leave the USB blocks clocked and
  // powered? A register that reads back as 0 or all ones, or a hang here, says
  // no and the USB port needs clock/power/PHY work before any driver.
  //
  DEBUG ((
    EFI_D_ERROR,
    "USB probe: xhci[0]=%08x xhci[1]=%08x mtu3[0]=%08x mtu3[1]=%08x mtu3[2]=%08x ippc=%08x phy=%08x\n",
    MmioRead32 (0x11200000),
    MmioRead32 (0x11200004),
    MmioRead32 (0x11201000),
    MmioRead32 (0x11201004),
    MmioRead32 (0x11201008),
    MmioRead32 (0x11203E00),
    MmioRead32 (0x11F40000)
    ));

  CpuDeadLoop ();
}
