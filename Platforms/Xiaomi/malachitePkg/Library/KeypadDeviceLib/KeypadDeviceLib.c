/** @file
  Keypad device library for the POCO X7 (malachite, MT6878).

  The device tree exposes one physical key to firmware (it is the Volume Up key
  on the hardware even though the node is named volumedown), wired to
  GPIO 54 of the MediaTek pin controller (0x10005000) as active low with the
  pad pulled up (/gpio-keys/volumedown, /soc/pinctrl/key-default
  pinmux = <0x3600>).

  MediaTek GPIO register layout (same as GpioImplLib): pins are grouped in
  banks of 32, each bank 0x10 bytes apart; the data-in register block starts
  at 0x200. For pin 54 that is 0x210, bit 22.

  A short press reports SCAN_UP (which is also what the Mu Boot Manager listens
  for to open the menu); a
  press held for longer than 700 ms reports Enter instead, so the menu can be
  driven with the single key the firmware can see.
**/

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/KeypadDeviceLib.h>

#define MTK_PINCTRL_BASE      0x10005000U
#define MTK_GPIO_DATA_IN_REG(Pin) \
  (MTK_PINCTRL_BASE + 0x200U + (((Pin) / 32U) * 0x10U))

#define VOLUME_DOWN_PIN       54U
#define VOLUME_DOWN_BIT       (1U << (VOLUME_DOWN_PIN % 32U))
#define LONG_PRESS_NS         700000000ULL

STATIC BOOLEAN  mKeyDown;
STATIC UINT64   mKeyDownNs;
STATIC BOOLEAN  mLongPressSent;

STATIC
BOOLEAN
VolumeDownPressed (
  OUT UINT32  *RawValue OPTIONAL
  )
{
  UINT32  Value;

  Value = MmioRead32 (MTK_GPIO_DATA_IN_REG (VOLUME_DOWN_PIN));
  if (RawValue != NULL) {
    *RawValue = Value;
  }

  // Active low, the pad is pulled up, so a pressed key reads 0.
  return (Value & VOLUME_DOWN_BIT) == 0;
}

EFI_STATUS
EFIAPI
KeypadDeviceReset (
  IN KEYPAD_DEVICE_PROTOCOL  *This
  )
{
  mKeyDown       = FALSE;
  mKeyDownNs     = 0;
  mLongPressSent = FALSE;

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
KeypadDeviceGetKeys (
  IN KEYPAD_DEVICE_PROTOCOL  *This,
  IN KEYPAD_RETURN_API       *KeypadReturnApi,
  IN UINT64                   Delta
  )
{
  EFI_KEY_DATA  KeyData;
  UINT32        RawValue;
  BOOLEAN       Pressed;

  Pressed = VolumeDownPressed (&RawValue);

  if (Pressed != mKeyDown) {
    DEBUG ((EFI_D_ERROR, "KEYPAD: volume up %a (DIN=0x%08x)\n", Pressed ? "pressed" : "released", RawValue));
  }

  if (Pressed && !mKeyDown) {
    mKeyDown       = TRUE;
    mKeyDownNs     = 0;
    mLongPressSent = FALSE;
  } else if (!Pressed && mKeyDown) {
    mKeyDown = FALSE;

    // A short press reports Volume Up.
    if (!mLongPressSent) {
      ZeroMem (&KeyData, sizeof (KeyData));
      KeyData.Key.ScanCode = SCAN_UP;
      KeypadReturnApi->PushEfikeyBufTail (KeypadReturnApi, &KeyData);
      DEBUG ((EFI_D_ERROR, "KEYPAD: push SCAN_UP\n"));
    }
  }

  if (Pressed && mKeyDown) {
    mKeyDownNs += Delta;

    // Holding the key reports Enter, so the one visible key can select too.
    if (!mLongPressSent && (mKeyDownNs >= LONG_PRESS_NS)) {
      ZeroMem (&KeyData, sizeof (KeyData));
      KeyData.Key.UnicodeChar = CHAR_CARRIAGE_RETURN;
      KeypadReturnApi->PushEfikeyBufTail (KeypadReturnApi, &KeyData);
      mLongPressSent = TRUE;
      DEBUG ((EFI_D_ERROR, "KEYPAD: push ENTER (long press)\n"));
    }
  }

  return EFI_SUCCESS;
}
