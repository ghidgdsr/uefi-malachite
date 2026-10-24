_Head:
  adr   x1, _Payload    /* Set _Entry address */
  b     _Start          /* Jump to the real code */

#if REQUIRES_KERNEL_HEADER == 1
_StackBase:
  .quad 0               /* text_offset: the stock kernel uses 0 */

_StackSize:
  .quad FD_SIZE + (_Payload - _Head) /* image_size */
  .quad 0x0a            /* flags: match the stock kernel (4K pages/MMU) */
  .quad 0               /* res2 */
  .quad 0               /* res3 */
  .quad 0               /* res4 */
  .ascii "ARM\x64"      /* ARM64 magic, must be at offset 0x38 */
  .long 0               /* res5 */
#endif

_Start:
  //
  // A Linux-style bootloader may hand off at EL1 or EL2 with its own MMU on
  // and only part of DRAM mapped; unmapped RAM (DXE heap, DTB copy target)
  // would fault on the first store. Mask interrupts and turn the current EL's
  // MMU off, so every access below is a physical, Device-nGnRnE access. SEC
  // builds its own translation tables later.
  //
  msr   daifset, #0xf

  //
  // Record what the previous stage left behind before changing anything:
  // bits [11:8] = CurrentEL, bit 1 = HCR_EL2.E2H (VHE), bit 0 = the
  // SCTLR_ELx.M of the regime this CPU is executing in. The word is stored in
  // the RAM hole between the UEFI stack (ends at 0x41240000) and the DTB copy
  // region (starts at 0x41680000), which the UEFI memory map does not
  // describe, so nothing overwrites it. SEC reads it to show this state on
  // the panel.
  //
  movz  x8, #0xA55A, lsl #48        // magic, so SEC can tell it was written
  mrs   x9, CurrentEL
  ubfx  x9, x9, #2, #2
  orr   x8, x8, x9, lsl #8

  cmp   x9, #2
  b.eq  _ProbeEl2
  cmp   x9, #3
  b.eq  _ProbeEl3
  mrs   x10, sctlr_el1
  b     _ProbeDone
_ProbeEl3:
  mrs   x10, sctlr_el3
  b     _ProbeDone
_ProbeEl2:
  mrs   x11, hcr_el2
  ubfx  x12, x11, #34, #1           // E2H
  orr   x8, x8, x12, lsl #1
  tbz   x11, #34, _ProbeEl2Plain
  mrs   x10, sctlr_el1              // VHE: the EL1&0 regime is the live one
  b     _ProbeDone
_ProbeEl2Plain:
  mrs   x10, sctlr_el2
_ProbeDone:
  and   x10, x10, #1                // M bit
  orr   x8, x8, x10

  cmp   x9, #2
  b.eq  _MmuOffEl2
  cmp   x9, #3
  b.eq  _MmuOffEl3
  mrs   x10, sctlr_el1
  bic   x10, x10, #1
  msr   sctlr_el1, x10
  //
  // Disabling the MMU keeps TLB entries. The bootloader ran with its own
  // tables and ASID 0; SEC will program ASID 0 again, so a stale entry could
  // be used with the wrong attributes once the MMU comes back on.
  //
  dsb   ishst
  tlbi  vmalle1
  dsb   ish
  isb
  b     _MmuOffDone
_MmuOffEl2:
  //
  // With HCR_EL2.E2H set (VHE) the CPU executes at EL2 in the EL1&0
  // translation regime: SCTLR_EL1.M is the live control and SCTLR_EL2 is
  // ignored. Mu's ArmMmuLib programs TCR_EL2/TTBR0_EL2/SCTLR_EL2 for EL2 and
  // does not handle VHE, so turn both regimes off and leave VHE here; EL2
  // then runs on the plain EL2 regime that Mu knows how to set up.
  //
  mrs   x11, hcr_el2
  tbz   x11, #34, _MmuOffEl2Plain
  mrs   x10, sctlr_el2
  bic   x10, x10, #1
  msr   sctlr_el2, x10
  mrs   x10, sctlr_el1
  bic   x10, x10, #1
  msr   sctlr_el1, x10
  mov   x12, #1
  lsl   x12, x12, #34
  bic   x11, x11, x12
  msr   hcr_el2, x11
  isb
  dsb   ishst
  tlbi  alle1
  tlbi  alle2
  dsb   ish
  b     _MmuOffDone
_MmuOffEl2Plain:
  mrs   x10, sctlr_el2
  bic   x10, x10, #1
  msr   sctlr_el2, x10
  dsb   ishst
  tlbi  alle2
  dsb   ish
  isb
  b     _MmuOffDone
_MmuOffEl3:
  mrs   x10, sctlr_el3
  bic   x10, x10, #1
  msr   sctlr_el3, x10
_MmuOffDone:
  dsb   sy
  isb

  // The MMU is off now: publish the probe word with a plain physical store.
  ldr   x13, =0x41240000
  str   x8, [x13]
  dsb   sy

  //
  // HackiOS: a new boot starts a new debug log ring. The frame buffer serial
  // port appends every DEBUG byte to 0x41400000 (FrameBufferSerialPortLib.c);
  // zeroing its magic here is the only reset, so a later SerialPortInitialize
  // from a DXE driver cannot throw the log away.
  //
  ldr   x13, =0x41400000
  str   wzr, [x13]
  dsb   sy

#if DTB_COPY_ADDR != 0 && DTB_COPY_SIZE != 0
  /* The firmware volume copy below may overwrite the bootloader's DTB.
     Copy the DTB to a reserved page first and hand its address to SEC in x0. */
  cbz   x0, _DtbDone
  ldr   w20, [x0]
  rev   w20, w20
  ldr   w21, =0xD00DFEED
  cmp   w20, w21
  b.ne  _DtbDone
  ldr   w20, [x0, #4]          /* totalsize */
  rev   w20, w20
  cbz   w20, _DtbDone
  ldr   w21, =DTB_COPY_SIZE
  cmp   w20, w21
  b.hi  _DtbDone
  ldr   x22, =DTB_COPY_ADDR
  mov   x23, x0
  mov   x24, x22
  mov   w25, w20
_DtbCopyLoop:
  ldrb  w26, [x23], #1
  strb  w26, [x24], #1
  subs  w25, w25, #1
  b.ne  _DtbCopyLoop
  mov   x0, x22
_DtbDone:
#endif

#if PAYLOAD_BASE != 0
  //
  // HackiOS: an "XNUP" container may follow the FD in the decompressed image.
  // Copy it first: the FD copy below writes to 0x41000000 on this device, which
  // lies inside the appended payload in the source image.
  //
  ldr   x4, =FD_SIZE
  add   x4, x1, x4
  ldr   w2, [x4]
  movz  w3, #0x4E58
  movk  w3, #0x5055, lsl #16
  cmp   w2, w3
  b.ne  _NoPayload
  ldr   w6, [x4, #4]            /* payload body size */
  add   x6, x6, #23             /* header + round up to 16 */
  and   x6, x6, #~15
  movz  x5, #(PAYLOAD_BASE & 0xffff)
  movk  x5, #((PAYLOAD_BASE >> 16) & 0xffff), lsl #16
  movk  x5, #((PAYLOAD_BASE >> 32) & 0xffff), lsl #32
  movk  x5, #((PAYLOAD_BASE >> 48) & 0xffff), lsl #48
_PayloadCopyLoop:
  ldp   x2, x3, [x4], #16
  stp   x2, x3, [x5], #16
  subs  x6, x6, #16
  b.ne  _PayloadCopyLoop
_NoPayload:
#endif

  mov   x4, x1
  ldr   x5, =FD_BASE
  cmp   x4, x5
  beq   _Entry
  ldr   x6,  =FD_SIZE
  cmp   x4, x5
  b.hi  _CopyForward

  //
  // Destination above the source (malachite: FD at 0x41000000, bootloader
  // kernel around 0x40080000). Copy backwards so writes never land on source
  // bytes that have not been read yet.
  //
  add   x4, x4, x6
  add   x5, x5, x6
_CopyBackLoop:
  ldp   x2, x3, [x4, #-16]!
  stp   x2, x3, [x5, #-16]!
  subs  x6, x6, #16
  b.ne  _CopyBackLoop
  b     _CopyDone

_CopyForward:
  ldp   x2, x3, [x4], #16
  stp   x2, x3, [x5], #16
  subs  x6, x6, #16
  b.ne  _CopyForward

_CopyDone:
  ldr   x5, =FD_BASE

_Entry:
  /* Publish the copied instructions before executing them. */
  dsb   sy
  mrs   x9, ctr_el0
  ubfx  x9, x9, #16, #4
  mov   x10, #4
  lsl   x9, x10, x9
  sub   x10, x9, #1
  bic   x8, x5, x10
  ldr   x7, =FD_SIZE
  add   x7, x5, x7

_CleanLoop:
  dc    cvac, x8
  add   x8, x8, x9
  cmp   x8, x7
  b.lo  _CleanLoop
  dsb   sy
  ic    iallu
  dsb   sy
  isb
  br    x5

_Dead:
  b     _Dead

.text
.ltorg
.align 4

_Payload:
