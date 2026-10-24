/** @file
  Minimal UEFI application for the malachite bring-up.

  The Boot Manager menu application refuses to open when the platform has no
  boot options at all, so this application gives it one: a plain UEFI screen
  that proves the console, the input device and the firmware stack work.
**/

#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_INPUT_KEY  Key;
  EFI_STATUS     Status;

  gST->ConOut->SetAttribute (gST->ConOut, EFI_WHITE | EFI_BACKGROUND_BLUE);
  gST->ConOut->ClearScreen (gST->ConOut);

  gST->ConOut->SetCursorPosition (gST->ConOut, 4, 4);
  gST->ConOut->OutputString (gST->ConOut, L"UniXNU UEFI for POCO X7 (malachite)\r\n");
  gST->ConOut->SetCursorPosition (gST->ConOut, 4, 6);
  gST->ConOut->OutputString (gST->ConOut, L"The firmware booted and this is a UEFI application.\r\n");
  gST->ConOut->SetCursorPosition (gST->ConOut, 4, 8);
  gST->ConOut->OutputString (gST->ConOut, L"Press Volume Up (short press) to return.\r\n");

  if (gST->ConIn == NULL) {
    DEBUG ((EFI_D_ERROR, "DEMO: no ConIn, returning\n"));
    return EFI_SUCCESS;
  }

  do {
    Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
    gBS->Stall (100000);
  } while (EFI_ERROR (Status));

  return EFI_SUCCESS;
}
