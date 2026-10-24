/** @file
  MediaTek MT6878 UFS host controller (MMIO) for the generic EDK2 UFS stack.

  The controller is a standard UFSHCI at a fixed MMIO address; EDK2's
  UfsPassThruDxe does the UFS protocol work once this driver publishes
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL. The PHY and clocks are assumed to be
  left running by the bootloader (LK reads the boot image from this UFS).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "UfsMtkDxe.h"

#include <Library/ArmSmcLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <IndustryStandard/UfsHci.h>
#include <Protocol/UfsHostControllerPlatform.h>

STATIC EFI_GUID  mUfsMtkVendorGuid = { 0x8f3a7c21, 0x4b6d, 0x4e59, { 0x9a, 0x1c, 0x2d, 0x3e, 0x4f, 0x50, 0x61, 0x72 } };

//
// DMA pool: "UFS DMA" in MemoryMapLib, normal non-cacheable. Every buffer the
// UFS stack allocates is a common buffer (transfer list, task list, command
// and response UPIUs), and the driver writes it after mapping, so cached
// memory is never seen by the controller. Allocations are page granular.
//
#define UFS_MTK_DMA_BASE   0x41500000ULL
#define UFS_MTK_DMA_PAGES  256
STATIC UINT64  mUfsMtkDmaUsed[UFS_MTK_DMA_PAGES / 64];

STATIC
EFI_STATUS
UfsMtkDmaAlloc (
  IN  UINTN  Pages,
  OUT VOID   **HostAddress
  )
{
  UINTN  Run;

  if ((Pages == 0) || (Pages > UFS_MTK_DMA_PAGES)) {
    return EFI_INVALID_PARAMETER;
  }

  Run = 0;
  for (UINTN i = 0; i < UFS_MTK_DMA_PAGES; i++) {
    if ((mUfsMtkDmaUsed[i / 64] & (1ULL << (i % 64))) != 0) {
      Run = 0;
      continue;
    }

    if (++Run == Pages) {
      UINTN  Start = i + 1 - Pages;

      for (UINTN j = Start; j <= i; j++) {
        mUfsMtkDmaUsed[j / 64] |= 1ULL << (j % 64);
      }

      *HostAddress = (VOID *)(UINTN)(UFS_MTK_DMA_BASE + Start * EFI_PAGE_SIZE);
      ZeroMem (*HostAddress, Pages * EFI_PAGE_SIZE);
      return EFI_SUCCESS;
    }
  }

  return EFI_OUT_OF_RESOURCES;
}

STATIC
EFI_STATUS
UfsMtkDmaFree (
  IN UINTN  Pages,
  IN VOID   *HostAddress
  )
{
  UINTN  Address;
  UINTN  Start;

  Address = (UINTN)HostAddress;
  if ((Address < UFS_MTK_DMA_BASE) ||
      (Address + Pages * EFI_PAGE_SIZE > UFS_MTK_DMA_BASE + UFS_MTK_DMA_PAGES * EFI_PAGE_SIZE) ||
      (((Address - UFS_MTK_DMA_BASE) % EFI_PAGE_SIZE) != 0))
  {
    return EFI_INVALID_PARAMETER;
  }

  Start = (Address - UFS_MTK_DMA_BASE) / EFI_PAGE_SIZE;
  for (UINTN j = Start; j < Start + Pages; j++) {
    mUfsMtkDmaUsed[j / 64] &= ~(1ULL << (j % 64));
  }

  return EFI_SUCCESS;
}

STATIC
UINT64
UfsMtkSmc (
  IN UINT64  Cmd,
  IN UINT64  V1,
  IN UINT64  V2
  )
{
  ARM_SMC_ARGS  Args;

  ZeroMem (&Args, sizeof (Args));
  Args.Arg0 = 0x82000276;   // MTK_SIP_UFS_CONTROL
  Args.Arg1 = Cmd;
  Args.Arg2 = V1;
  Args.Arg3 = V2;
  ArmCallSmc (&Args);

  return Args.Arg0;
}

//
// One line per power-up step: which step turns the UFSHCI register block into
// the constant 3 that a powered-off block returns is exactly what is needed.
//
STATIC
VOID
UfsMtkStage (
  IN CONST CHAR8  *Name
  )
{
  DEBUG ((
    EFI_D_ERROR,
    "UfsMtk: %a CAP=%08x VER=%08x IS=%08x HCS=%08x HCE=%08x phy8c=%08x\n",
    Name,
    MmioRead32 ((UINTN)UFS_MTK_HC_BASE + 0x00),
    MmioRead32 ((UINTN)UFS_MTK_HC_BASE + 0x08),
    MmioRead32 ((UINTN)UFS_MTK_HC_BASE + 0x20),
    MmioRead32 ((UINTN)UFS_MTK_HC_BASE + 0x30),
    MmioRead32 ((UINTN)UFS_MTK_HC_BASE + 0x34),
    MmioRead32 ((UINTN)UFS_MTK_PHY_BASE + 0x008C)
    ));
}

STATIC
VOID
UfsMtkPowerUp (
  VOID
  )
{
  UINT32  Reg;
  UINT32  Ack;
  UINT64  SmcMtcmos;
  UINT64  SmcSram;
  UINT64  SmcDev;
  UINT64  SmcRst0;
  UINT64  SmcRst1;

  UfsMtkStage ("entry");

  //
  // The controller and its MPHY are powered through EL3. This firmware
  // answers the newer command set (MTCMOS_CTRL, bit 9, x2 = on); an unknown
  // command would come back non-zero.
  //
  SmcMtcmos = UfsMtkSmc (BIT9, 1, 0);
  SmcSram   = UfsMtkSmc (BIT5, 1, 0);
  //
  // Device rails (VCC/VCCQ) and a reset pulse: the bootloader hands the
  // device over powered down, and the link startup sees no device otherwise.
  //
  SmcDev = UfsMtkSmc (BIT7, 1, 0x310);
  gBS->Stall (2000);
  SmcRst0 = UfsMtkSmc (BIT1, 0, 0);
  gBS->Stall (20);
  SmcRst1 = UfsMtkSmc (BIT1, 1, 0);
  gBS->Stall (12000);
  DEBUG ((
    EFI_D_ERROR,
    "UfsMtk: smc mtcmos=%x sram=%x dev=%x rst=%x/%x\n",
    SmcMtcmos,
    SmcSram,
    SmcDev,
    SmcRst0,
    SmcRst1
    ));
  UfsMtkStage ("mtcmos");

  //
  // Do not touch the ufscfg clock gates: at entry their status is 0 and the
  // controller already answers, while writing 0x08 (the set register on
  // mt8189) makes every UFSHCI register read back the CAP value -- on this
  // block 1 means gated, so the bootloader had already left the clocks on.
  //

  //
  // Release the resets the way the vendor driver does, but without asserting
  // them first: power-on leaves them asserted, and the power cycle already
  // reset the controller. unipro, crypto, hci, then mphy.
  //
  MmioWrite32 ((UINTN)UFS_MTK_PDN_BASE + 0x4C, BIT0);
  gBS->Stall (20);
  MmioWrite32 ((UINTN)UFS_MTK_PDN_BASE + 0x4C, BIT1);
  gBS->Stall (20);
  MmioWrite32 ((UINTN)UFS_MTK_PDN_BASE + 0x4C, BIT2);
  gBS->Stall (20);
  MmioWrite32 ((UINTN)UFS_MTK_AO_BASE + 0x4C, BIT8);
  gBS->Stall (200);
  UfsMtkStage ("resets");

  //
  // PHY power-on (mt8183): PLL out of isolation, CDR out of isolation, RX
  // squelch on, release the DIFZ force.
  //
  Reg  = MmioRead32 ((UINTN)UFS_MTK_PHY_BASE + 0x008C);
  Reg |= BIT11;
  Reg &= ~(BIT10 | BIT9 | BIT8);
  MmioWrite32 ((UINTN)UFS_MTK_PHY_BASE + 0x008C, Reg);

  Reg  = MmioRead32 ((UINTN)UFS_MTK_PHY_BASE + 0xB044);
  Reg |= BIT18;
  Reg &= ~(BIT20 | BIT19 | BIT17);
  MmioWrite32 ((UINTN)UFS_MTK_PHY_BASE + 0xB044, Reg);

  Reg  = MmioRead32 ((UINTN)UFS_MTK_PHY_BASE + 0xA0AC);
  Reg |= BIT1;
  Reg &= ~BIT0;
  MmioWrite32 ((UINTN)UFS_MTK_PHY_BASE + 0xA0AC, Reg);

  gBS->Stall (10);

  Reg  = MmioRead32 ((UINTN)UFS_MTK_PHY_BASE + 0xA09C);
  Reg &= ~BIT18;
  MmioWrite32 ((UINTN)UFS_MTK_PHY_BASE + 0xA09C, Reg);

  gBS->Stall (10);
  UfsMtkStage ("phy");

  //
  // Reference clock: REFCLK_REQUEST (bit 0) of the vendor register at 0x144,
  // then wait for REFCLK_ACK (bit 1). No ack means the 26 MHz reference is
  // not running, and the PHY PLL cannot lock.
  //
  Reg = MmioRead32 ((UINTN)UFS_MTK_HC_BASE + 0x144);
  MmioWrite32 ((UINTN)UFS_MTK_HC_BASE + 0x144, Reg | BIT0);

  Ack = 0;
  for (UINT32 i = 0; i < 300; i++) {
    Reg = MmioRead32 ((UINTN)UFS_MTK_HC_BASE + 0x144);
    Ack = ((Reg & BIT1) >> 1) == (Reg & BIT0);
    if (Ack != 0) {
      break;
    }

    gBS->Stall (10);
  }

  //
  // MTK vendor control: XOUFS enable (the vendor driver sets bit 7 here).
  //
  Reg  = MmioRead32 ((UINTN)UFS_MTK_HC_BASE + 0x140);
  MmioWrite32 ((UINTN)UFS_MTK_HC_BASE + 0x140, Reg | 0x80);

  DEBUG ((
    EFI_D_ERROR,
    "UfsMtk: refclk=%08x ack=%x ao sta=%08x pdn sta=%08x\n",
    MmioRead32 ((UINTN)UFS_MTK_HC_BASE + 0x144),
    Ack,
    MmioRead32 ((UINTN)UFS_MTK_AO_BASE + 0x04),
    MmioRead32 ((UINTN)UFS_MTK_PDN_BASE + 0x04)
    ));
  UfsMtkStage ("done");
}

STATIC
EFI_STATUS
UfsMtkDmeSet (
  IN OUT EDKII_UFS_HC_DRIVER_INTERFACE  *Ifc,
  IN     UINT32                          AttrSel,
  IN     UINT32                          Value
  )
{
  EDKII_UIC_COMMAND  Cmd;

  ZeroMem (&Cmd, sizeof (Cmd));
  Cmd.Opcode = UfsUicDmeSet;
  Cmd.Arg1   = AttrSel;
  Cmd.Arg3   = Value;
  return Ifc->UfsExecUicCommand (Ifc, &Cmd);
}

STATIC
EFI_STATUS
UfsMtkDmeGet (
  IN OUT EDKII_UFS_HC_DRIVER_INTERFACE  *Ifc,
  IN     UINT32                          AttrSel,
  OUT    UINT32                         *Value
  )
{
  EFI_STATUS  Status;
  EDKII_UIC_COMMAND  Cmd;

  ZeroMem (&Cmd, sizeof (Cmd));
  Cmd.Opcode = UfsUicDmeGet;
  Cmd.Arg1   = AttrSel;
  Status     = Ifc->UfsExecUicCommand (Ifc, &Cmd);
  *Value     = Cmd.Arg3;
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
UfsMtkPlatformCallback (
  IN     EFI_HANDLE                            ControllerHandle,
  IN     EDKII_UFS_HC_PLATFORM_CALLBACK_PHASE  CallbackPhase,
  IN OUT VOID                                  *CallbackData
  )
{
  if (CallbackPhase == EdkiiUfsHcPreHce) {
    UfsMtkPowerUp ();
  }

  if (CallbackPhase == EdkiiUfsHcPreLinkStartup) {
    EDKII_UFS_HC_DRIVER_INTERFACE  *Ifc;
    EFI_STATUS                      S1;
    EFI_STATUS                      S2;
    EFI_STATUS                      S3;
    EFI_STATUS                      S4;
    UINT32                          Power;

    Ifc   = CallbackData;
    Power = 0;
    //
    // The vendor driver's pre-link: leave UniPro power-down, disable TX lane
    // clock gating on the host, and clear the deep-stall bit. Log failures
    // and still let the link startup run; without these the PA layer rejects
    // the link (UECPA 0x10).
    //
    S1 = UfsMtkDmeSet (Ifc, 0xD0A8u << 16, 0);                    // VS_UNIPROPOWERDOWNCONTROL
    S2 = UfsMtkDmeSet (Ifc, 0x155Eu << 16, 0);                    // PA_LOCAL_TX_LCC_ENABLE
    S3 = UfsMtkDmeGet (Ifc, 0xD0A6u << 16, &Power);               // VS_SAVEPOWERCONTROL
    S4 = UfsMtkDmeSet (Ifc, 0xD0A6u << 16, Power & ~(UINT32)BIT6);
    DEBUG ((
      EFI_D_ERROR,
      "UfsMtk: prelink unipro=%r lcc=%r get=%r(%x) set=%r\n",
      S1,
      S2,
      S3,
      Power,
      S4
      ));
  }

  return EFI_SUCCESS;
}

STATIC EDKII_UFS_HC_PLATFORM_PROTOCOL  mUfsMtkPlatformProtocol = {
  EDKII_UFS_HC_PLATFORM_PROTOCOL_VERSION,
  NULL,
  UfsMtkPlatformCallback,
  EdkiiUfsCardRefClkFreqObsolete,
  FALSE,
  FALSE
};

STATIC
EFI_STATUS
EFIAPI
UfsMtkGetMmioBar (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  OUT UINTN                               *MmioBar
  );

STATIC
EFI_STATUS
EFIAPI
UfsMtkAllocateBuffer (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN  EFI_ALLOCATE_TYPE                   Type,
  IN  EFI_MEMORY_TYPE                     MemoryType,
  IN  UINTN                               Pages,
  OUT VOID                                **HostAddress,
  IN  UINT64                              Attributes
  );

STATIC
EFI_STATUS
EFIAPI
UfsMtkFreeBuffer (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN UINTN                               Pages,
  IN VOID                                *HostAddress
  );

STATIC
EFI_STATUS
EFIAPI
UfsMtkMap (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL   *This,
  IN     EDKII_UFS_HOST_CONTROLLER_OPERATION  Operation,
  IN     VOID                                 *HostAddress,
  IN OUT UINTN                                *NumberOfBytes,
  OUT    EFI_PHYSICAL_ADDRESS                 *DeviceAddress,
  OUT    VOID                                 **Mapping
  );

STATIC
EFI_STATUS
EFIAPI
UfsMtkUnmap (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN VOID                                *Mapping
  );

STATIC
EFI_STATUS
EFIAPI
UfsMtkFlush (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This
  );

STATIC
EFI_STATUS
EFIAPI
UfsMtkMmioRead (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL        *This,
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH  Width,
  IN     UINT64                                    Offset,
  IN     UINTN                                     Count,
  IN OUT VOID                                      *Buffer
  );

STATIC
EFI_STATUS
EFIAPI
UfsMtkMmioWrite (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL        *This,
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH  Width,
  IN     UINT64                                    Offset,
  IN     UINTN                                     Count,
  IN OUT VOID                                      *Buffer
  );

UFS_MTK_PRIVATE_DATA  mUfsMtkPrivate = {
  UFS_MTK_PRIVATE_SIGNATURE,
  {
    UfsMtkGetMmioBar,
    UfsMtkAllocateBuffer,
    UfsMtkFreeBuffer,
    UfsMtkMap,
    UfsMtkUnmap,
    UfsMtkFlush,
    UfsMtkMmioRead,
    UfsMtkMmioWrite
  }
};

STATIC
EFI_STATUS
EFIAPI
UfsMtkGetMmioBar (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  OUT UINTN                               *MmioBar
  )
{
  if ((This == NULL) || (MmioBar == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *MmioBar = (UINTN)UFS_MTK_HC_BASE;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
UfsMtkAllocateBuffer (
  IN  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN  EFI_ALLOCATE_TYPE                   Type,
  IN  EFI_MEMORY_TYPE                     MemoryType,
  IN  UINTN                               Pages,
  OUT VOID                                **HostAddress,
  IN  UINT64                              Attributes
  )
{
  if ((This == NULL) || (HostAddress == NULL) || (Pages == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  if (UfsMtkDmaAlloc (Pages, HostAddress) != EFI_SUCCESS) {
    return EFI_OUT_OF_RESOURCES;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
UfsMtkFreeBuffer (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN UINTN                               Pages,
  IN VOID                                *HostAddress
  )
{
  if ((This == NULL) || (HostAddress == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  return UfsMtkDmaFree (Pages, HostAddress);
}

STATIC
EFI_STATUS
EFIAPI
UfsMtkMap (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL   *This,
  IN     EDKII_UFS_HOST_CONTROLLER_OPERATION  Operation,
  IN     VOID                                 *HostAddress,
  IN OUT UINTN                                *NumberOfBytes,
  OUT    EFI_PHYSICAL_ADDRESS                 *DeviceAddress,
  OUT    VOID                                 **Mapping
  )
{
  if ((This == NULL) || (HostAddress == NULL) || (NumberOfBytes == NULL) ||
      (DeviceAddress == NULL) || (Mapping == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Identity mapping: the firmware's page tables map physical == virtual, and
  // UFS on this SoC is behind the coherent interconnect. The cache maintenance
  // below costs little and covers a non-coherent port.
  //
  switch (Operation) {
    case EdkiiUfsHcOperationBusMasterRead:
      WriteBackDataCacheRange (HostAddress, *NumberOfBytes);
      break;
    case EdkiiUfsHcOperationBusMasterWrite:
      InvalidateDataCacheRange (HostAddress, *NumberOfBytes);
      break;
    case EdkiiUfsHcOperationBusMasterCommonBuffer:
      WriteBackInvalidateDataCacheRange (HostAddress, *NumberOfBytes);
      break;
    default:
      break;
  }

  *DeviceAddress = (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress;
  *Mapping       = NULL;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
UfsMtkUnmap (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This,
  IN VOID                                *Mapping
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
UfsMtkFlush (
  IN EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *This
  )
{
  // A read of a posted register is enough to know the writes have left the CPU.
  (VOID)MmioRead32 ((UINTN)UFS_MTK_HC_BASE + 0x08);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
UfsMtkMmioRead (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL        *This,
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH  Width,
  IN     UINT64                                    Offset,
  IN     UINTN                                     Count,
  IN OUT VOID                                      *Buffer
  )
{
  UINT8  *Data;

  if ((This == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Data = Buffer;
  for (UINTN i = 0; i < Count; i++) {
    UINTN Address = (UINTN)UFS_MTK_HC_BASE + (UINTN)Offset + i * (1U << Width);
    switch (Width) {
      case EfiUfsHcWidthUint8:
        *(UINT8 *)Data = MmioRead8 (Address);
        Data          += 1;
        break;
      case EfiUfsHcWidthUint16:
        *(UINT16 *)Data = MmioRead16 (Address);
        Data           += 2;
        break;
      case EfiUfsHcWidthUint32:
        *(UINT32 *)Data = MmioRead32 (Address);
        Data           += 4;
        break;
      case EfiUfsHcWidthUint64:
        *(UINT64 *)Data = MmioRead64 (Address);
        Data           += 8;
        break;
      default:
        return EFI_UNSUPPORTED;
    }
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
UfsMtkMmioWrite (
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL        *This,
  IN     EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH  Width,
  IN     UINT64                                    Offset,
  IN     UINTN                                     Count,
  IN OUT VOID                                      *Buffer
  )
{
  UINT8  *Data;

  if ((This == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Data = Buffer;
  for (UINTN i = 0; i < Count; i++) {
    UINTN Address = (UINTN)UFS_MTK_HC_BASE + (UINTN)Offset + i * (1U << Width);
    switch (Width) {
      case EfiUfsHcWidthUint8:
        MmioWrite8 (Address, *(UINT8 *)Data);
        Data += 1;
        break;
      case EfiUfsHcWidthUint16:
        MmioWrite16 (Address, *(UINT16 *)Data);
        Data += 2;
        break;
      case EfiUfsHcWidthUint32:
        MmioWrite32 (Address, *(UINT32 *)Data);
        Data += 4;
        break;
      case EfiUfsHcWidthUint64:
        MmioWrite64 (Address, *(UINT64 *)Data);
        Data += 8;
        break;
      default:
        return EFI_UNSUPPORTED;
    }
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
UfsMtkDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS           Status;
  EFI_HANDLE           Handle;
  VENDOR_DEVICE_PATH   *VendorPath;
  UINT32               Cap;
  UINT32               Ver;

  Handle = NULL;

  Cap = MmioRead32 ((UINTN)UFS_MTK_HC_BASE + 0x00);
  Ver = MmioRead32 ((UINTN)UFS_MTK_HC_BASE + 0x08);
  //
  // HCS/HCE/IS as the bootloader left them: this is the state the generic UFS
  // stack starts from, before it resets the controller.
  //
  DEBUG ((
    EFI_D_ERROR,
    "UfsMtk: HC %x CAP=%08x VER=%08x HCS=%08x HCE=%08x IS=%08x\n",
    UFS_MTK_HC_BASE,
    Cap,
    Ver,
    MmioRead32 ((UINTN)UFS_MTK_HC_BASE + 0x30),
    MmioRead32 ((UINTN)UFS_MTK_HC_BASE + 0x34),
    MmioRead32 ((UINTN)UFS_MTK_HC_BASE + 0x20)
    ));

  if (Cap == 0xFFFFFFFFU || Ver == 0xFFFFFFFFU) {
    DEBUG ((EFI_D_ERROR, "UfsMtk: no controller (clock or PHY off?)\n"));
    return EFI_SUCCESS;
  }

  //
  // The generic UFS stack locates this protocol and calls back into it right
  // before it enables the host controller: the place the vendor driver does
  // the MTK clock, reset and MPHY power-up.
  //
  {
    EFI_HANDLE  PlatformHandle;

    PlatformHandle = NULL;
    Status         = gBS->InstallProtocolInterface (
                            &PlatformHandle,
                            &gEdkiiUfsHcPlatformProtocolGuid,
                            EFI_NATIVE_INTERFACE,
                            &mUfsMtkPlatformProtocol
                            );
    DEBUG ((EFI_D_ERROR, "UfsMtk: platform protocol = %r\n", Status));
  }

  //
  // A device path must end with an end-of-device-path node: DxeCore validates
  // the path with IsDevicePathValid before installing it, and an unterminated
  // vendor node is rejected (the GetDevicePathSize ASSERT in Handle.c).
  //
  VendorPath = AllocateZeroPool (sizeof (VENDOR_DEVICE_PATH) + sizeof (EFI_DEVICE_PATH_PROTOCOL));
  if (VendorPath == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  VendorPath->Header.Type    = HARDWARE_DEVICE_PATH;
  VendorPath->Header.SubType = HW_VENDOR_DP;
  VendorPath->Header.Length[0] = (UINT8)sizeof (VENDOR_DEVICE_PATH);
  VendorPath->Header.Length[1] = (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8);
  CopyGuid (&VendorPath->Guid, &mUfsMtkVendorGuid);
  SetDevicePathEndNode ((EFI_DEVICE_PATH_PROTOCOL *)(VendorPath + 1));

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid,
                  VendorPath,
                  &gEdkiiUfsHostControllerProtocolGuid,
                  &mUfsMtkPrivate.UfsHcProtocol,
                  NULL
                  );
  DEBUG ((EFI_D_ERROR, "UfsMtk: protocol install = %r (handle %p)\n", Status, Handle));

  return Status;
}
