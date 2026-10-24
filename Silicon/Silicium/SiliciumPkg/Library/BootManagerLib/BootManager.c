/**
  Copyright (C) DuoWoA authors. All rights reserved.
  Copyright (C) Microsoft Corporation. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/PcdLib.h>
#include <Library/DebugLib.h>
#include <Library/AcpiTableUpdateLib.h>
#include <Library/DeviceBootManagerLib.h>
#include <Library/PlatformBootManagerLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/SecureBootHandlerLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MsBootOptionsLib.h>
#include <Library/BootGraphicsLib.h>
#include <Library/BootGraphics.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiLib.h>
#include <Library/DevicePathLib.h>

#include <Protocol/LoadedImage.h>

#include <Configuration/BootDevices.h>

#include "BootManager.h"
#include "XnuLoader.h"

//
// Provided by MsBootOptionsLib (SiliciumPkg). Declared here to register the
// platform's own boot option without growing that library's public surface.
//
EFI_STATUS
CreateFvBootOption (
  IN     EFI_GUID                     *FileGuid,
  IN     CHAR16                       *Description,
  IN OUT EFI_BOOT_MANAGER_LOAD_OPTION *BootOption,
  IN     UINT32                        Attributes,
  IN     UINT8                        *OptionalData,
  IN     UINT32                        OptionalDataSize OPTIONAL
  );

//
// Global Variables
//
STATIC UINTN XPos = 0;
STATIC UINTN YPos = 0;

//
// BDS Combo Message
//
STATIC CONST CHAR8 *ComboMessage = "[Volume Up] Boot Manager";

VOID
EFIAPI
PlatformBootManagerBeforeConsole ()
{
  // Signal End Of DXE
  EfiEventGroupSignal (&gEfiEndOfDxeEventGroupGuid);

  // Dispatch Deferred Images
  EfiBootManagerDispatchDeferredImages ();

  // Register Boot Options
  MsBootOptionsLibRegisterDefaultBootOptions ();

  //
  // The Boot Manager menu application refuses to start when the platform has
  // no boot options, so register this platform's demo application: the menu
  // then has an entry to list, and the same image proves the UEFI console.
  //
  {
    STATIC EFI_GUID  DemoAppFileGuid = {
      0x7E3B1C5A, 0x9D24, 0x4F8B, { 0xB6, 0xE2, 0x0A, 0x5C, 0x9D, 0x3F, 0x1E, 0x77 }
    };
    EFI_BOOT_MANAGER_LOAD_OPTION  DemoOption;
    EFI_STATUS                    Status;

    Status = CreateFvBootOption (&DemoAppFileGuid, L"UniXNU Demo", &DemoOption, LOAD_OPTION_ACTIVE, NULL, 0);
    DEBUG ((EFI_D_ERROR, "BDS: create demo boot option = %r\n", Status));
    if (!EFI_ERROR (Status)) {
      Status = EfiBootManagerLoadOptionToVariable (&DemoOption);
      DEBUG ((EFI_D_ERROR, "BDS: store demo boot option = %r\n", Status));
      EfiBootManagerFreeLoadOption (&DemoOption);
    }
  }

  //
  // Register Console Variables
  //
  EfiBootManagerUpdateConsoleVariable (ConIn,  (EFI_DEVICE_PATH_PROTOCOL *)&KeypadDevicePath,  NULL);
  EfiBootManagerUpdateConsoleVariable (ConOut, (EFI_DEVICE_PATH_PROTOCOL *)&DisplayDevicePath, NULL);
  EfiBootManagerUpdateConsoleVariable (ErrOut, (EFI_DEVICE_PATH_PROTOCOL *)&DisplayDevicePath, NULL);

  // Execute Secondary Before Console
  DeviceBootManagerBeforeConsole (NULL, NULL);
}

VOID
PostGopSetup (OUT EFI_GRAPHICS_OUTPUT_PROTOCOL **GopProtocol)
{
  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *LocalGopProtocol;

  // Verify Parameters
  if (GopProtocol == NULL) {
    return;
  }

  // Locate GOP Protocol
  Status = gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID *)&LocalGopProtocol);
  if (EFI_ERROR (Status) && Status != EFI_UNSUPPORTED) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Locate GOP Protocol from Handle! Status = %r\n", __FUNCTION__, Status));
    return;
  }

  // Check Status
  if (Status == EFI_UNSUPPORTED) {
    // Locate GOP Protocol
    Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID *)&LocalGopProtocol);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "%a: Failed to Locate GOP Protocol! Status = %r\n", __FUNCTION__, Status));
      return;
    }

    // Install GOP Protocol
    Status = gBS->InstallProtocolInterface (&gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, EFI_NATIVE_INTERFACE, LocalGopProtocol);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "%a: Failed to Install GOP Protocol onto \"ConsoleOutHandle\"! Status = %r\n", __FUNCTION__, Status));
    }
  }

  // Pass GOP Protocol
  *GopProtocol = LocalGopProtocol;
}

VOID
EFIAPI
PlatformBootManagerAfterConsole ()
{
  EFI_STATUS                    Status      = EFI_SUCCESS;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *GopProtocol = NULL;

  // Post Setup GOP
  PostGopSetup (&GopProtocol);

  DEBUG ((EFI_D_ERROR, "BDS: ConIn=%p ConOut=%p GOP=%p\n", gST->ConIn, gST->ConOut, GopProtocol));

  //
  // HackiOS: BDS would connect the driver-model drivers when it boots an
  // option, which is after this hook. The loader needs the storage stack
  // (UfsPassThru -> ScsiBus -> ScsiDisk, all driver-model drivers) bound and
  // running before it looks for BlockIo, so connect everything now.
  //
  EfiBootManagerConnectAll ();
  DEBUG ((EFI_D_ERROR, "BDS: ConnectAll done\n"));

  //
  // HackiOS: hand over to the XNU loader in the payload (does not return when
  // it is there). Here rather than in PriorityBoot: the console is connected
  // by now, so the loader's output has somewhere to go.
  //
  XnuLoaderMain ();

  // Display Boot Logo
  Status = DisplayBootGraphic (BG_SYSTEM_LOGO);
  DEBUG ((EFI_D_ERROR, "BDS: DisplayBootGraphic = %r\n", Status));
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Display Boot Logo! Status = %r\n", __FUNCTION__, Status));
  }

  // Register Key Callback
  Status = RegisterKeyCallback ((EFI_DEVICE_PATH_PROTOCOL *)&KeypadDevicePath);
  DEBUG ((EFI_D_ERROR, "BDS: RegisterKeyCallback = %r\n", Status));
  if (!EFI_ERROR (Status) && GopProtocol != NULL) {
    // Set Screen Resolution
    UINT32 ScreenWidth  = GopProtocol->Mode->Info->HorizontalResolution;
    UINT32 ScreenHeight = GopProtocol->Mode->Info->VerticalResolution;

    // Set Combo Message Position
    XPos = (ScreenWidth - AsciiStrLen (ComboMessage) * EFI_GLYPH_WIDTH) / 2;
    YPos = ScreenHeight * 48 / 50;
  }

  // Execute Secondary After Console
  DeviceBootManagerAfterConsole ();
}

VOID
EFIAPI
PlatformBootManagerOnDemandConInConnect ()
{
  // Execute Secondary On Demand
  DeviceBootManagerOnDemandConInConnect ();
}

VOID
EFIAPI
PlatformBootManagerProcessBootCompletion (OUT EFI_BOOT_MANAGER_LOAD_OPTION *BootOption)
{
  return;
}

VOID
EFIAPI
PlatformBootManagerPriorityBoot (IN OUT UINT16 **BootNext)
{
  STATIC EFI_GUID                          DemoAppFileGuid = {
    0x7E3B1C5A, 0x9D24, 0x4F8B, { 0xB6, 0xE2, 0x0A, 0x5C, 0x9D, 0x3F, 0x1E, 0x77 }
  };
  EFI_STATUS                               Status;
  EFI_LOADED_IMAGE_PROTOCOL               *LoadedImage;
  EFI_DEVICE_PATH_PROTOCOL                *FvPath;
  EFI_DEVICE_PATH_PROTOCOL                *FilePath;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH        FileNode;
  EFI_HANDLE                               DemoImage;
  UINTN                                    ExitDataSize;
  CHAR16                                  *ExitData;

  // Unregister Key Callback
  UnregisterKeyCallback ();

  (VOID)EnterBootManager ();

  //
  // Bring-up: start the platform demo application straight from the firmware
  // volume. BDS's boot-option path needs a variable store and an option list;
  // this path needs only the FV that holds BdsDxe itself.
  //
  Status = gBS->HandleProtocol (gImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&LoadedImage);
  if (EFI_ERROR (Status)) {
    return;
  }

  FvPath = DevicePathFromHandle (LoadedImage->DeviceHandle);
  if (FvPath == NULL) {
    return;
  }

  EfiInitializeFwVolDevicepathNode (&FileNode, &DemoAppFileGuid);
  FilePath = AppendDevicePathNode (FvPath, (EFI_DEVICE_PATH_PROTOCOL *)&FileNode);
  if (FilePath == NULL) {
    return;
  }

  Status = gBS->LoadImage (FALSE, gImageHandle, FilePath, NULL, 0, &DemoImage);
  DEBUG ((EFI_D_ERROR, "BDS: LoadImage(demo) = %r\n", Status));
  if (!EFI_ERROR (Status)) {
    Status = gBS->StartImage (DemoImage, &ExitDataSize, &ExitData);
    DEBUG ((EFI_D_ERROR, "BDS: StartImage(demo) = %r\n", Status));
    gBS->UnloadImage (DemoImage);
  }

  FreePool (FilePath);

  //
  // Leave a colour on the panel so the result is visible without a log:
  // green = the demo ran and returned, red = it could not be loaded or started.
  //
  {
    volatile UINT64  *FrameBuffer;
    UINTN             Index;
    UINTN             Count;
    UINT64            Pattern;

    Pattern     = EFI_ERROR (Status) ? 0xFFFF0000FFFF0000ULL : 0xFF00FF00FF00FF00ULL;
    FrameBuffer = (volatile UINT64 *)(UINTN)0xFD91F000;
    Count       = (0x026E0000 / sizeof (UINT64)) - 1;
    for (Index = 0; Index < Count; Index++) {
      FrameBuffer[Index] = Pattern;
    }
  }
}

VOID
EFIAPI
PlatformBootManagerWaitCallback (IN UINT16 TimeoutRemain)
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Color;

  // Verify Combo Message Position
  if (XPos == 0 && YPos == 0) {
    return;
  }

  // Get Timeout Time
  UINT16 Timeout = PcdGet16 (PcdPlatformBootTimeOut);

  // Check Remaining Timeout
  if (TimeoutRemain == Timeout || TimeoutRemain == 0) {
    // Set Draw Color
    UINT8 DrawColor = (TimeoutRemain == Timeout) ? 0xFF : 0x00;

    // Set GOP Draw Color
    Color.Red = Color.Green = Color.Blue = DrawColor;

    // Print Combo Message
    AsciiPrintXY (XPos, YPos, &Color, NULL, ComboMessage);
  }
}

VOID
EFIAPI
PlatformBootManagerUnableToBoot ()
{
  EFI_STATUS Status;

  // Execute Secondary Unable To Boot
  DeviceBootManagerUnableToBoot ();

  // Display No Boot OS Logo
  Status = DisplayBootGraphic (BG_NO_BOOT_OS);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "%a: Failed to Display No Boot OS Logo! Status = %r\n", __FUNCTION__, Status));
    Print (L"No Operating System Found! Please make sure that an OS is Installed.\n");
  }

  // Wait 10s
  gBS->Stall (10000000);

  // Shutdown
  gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);

  // Do Cpu Dead Loop
  CpuDeadLoop ();
}

VOID
EFIAPI
PlatformBootManagerBdsEntry ()
{
  EFI_STATUS Status;

  // Setup Secure Boot
  Status = SetupSecureBoot ();
  ASSERT_EFI_ERROR (Status);

  // Update ACPI Tables
  UpdateAcpiTables ();

  // Execute Secondary BDS Entry
  DeviceBootManagerBdsEntry ();
}
