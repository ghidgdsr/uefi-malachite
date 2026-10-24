# malachite: the POCO X7 (MT6878)

The first MediaTek platform in this tree: the Xiaomi POCO X7 / Redmi Note 14
Pro 5G (`malachite`), with the MediaTek MT6878 (Dimensity 7300-Ultra, 4x
Cortex-A55 + 4x Cortex-A78, GICv3, PSCI over SMC).

The firmware builds a boot image that the stock unlocked LK loads and jumps to.
It brings up the panel, the one key the device tree exposes to firmware, internal
storage over the MediaTek UFS host controller, and then starts the HackiOS XNU
loader from the payload appended after the firmware volume.

## Layout

| Path | Contents |
| --- | --- |
| `Platforms/Xiaomi/malachitePkg` | platform DSC/FDF/DEC, memory map, keypad, UFS host controller, a demo application that gives the Boot Manager a boot option |
| `Silicon/MediaTek/MT6878Pkg` | `PlatformSecLib`: the register map and the early helpers SEC needs on this SoC |
| `Resources/Configs/malachite.toml` | build configuration: addresses, DTB copy, payload copy, LZ4-legacy kernel compression |
| `BootShim/BootShim.S` | EL and MMU handling, DTB copy, payload copy, the Linux-style kernel header LK expects |
| `Silicon/Silicium/SiliciumPkg` | `SimpleFbDxe` for this panel, `BootManagerLib/XnuLoader.c`, the framebuffer serial port, the PEI memory map, SEC |
| `build_uefi.py` | `dtb_copy_addr`/`dtb_copy_size`/`payload_base`, and `lz4 -l` for the kernel |

## Memory map

| Region | Address |
| --- | --- |
| UEFI firmware volume | `0x41000000`, 2 MiB |
| UEFI stack | ends at `0x41240000` |
| DTB copy | `0x41680000`, 1 MiB |
| Framebuffer (LK scanout) | `0xFD91F000`, stride 1248 |
| XNUP payload | `0x100000000` |
| GIC distributor / redistributor | `0x0C000000` / `0x0C040000` |
| Pin controller | `0x10005000` |
| UFS host controller / PHY | `0x112B0000` / `0x112A0000` |

## Boot chain

```
LK -> boot.img (BootShim + UEFI FV + XNUP payload)
   -> BootShim: mask interrupts, turn the current EL's MMU off, record
      CurrentEL/HCR_EL2/SCTLR for SEC, copy the DTB to 0x41680000, copy the
      payload to 0x100000000, enter the firmware volume at 0x41000000
   -> UEFI: display, keypad, UFS, Boot Manager
   -> BootManagerLib/XnuLoader.c: start loader.efi from the payload
   -> xnuloader (HackiOS): load XNU, build the device tree and boot_args, jump
```

`xnuloader` paints the panel white just before it starts: white with text is the
loader's console, solid white a loader that prints nowhere, and the SEC exception
colour a fault on the way in.

## Build

```sh
./build_uefi.py -d malachite -r RELEASE
```

The output is `Build/malachitePkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd`; the boot
image is `Mu-malachite.img` (Android boot header v4, kernel = `lz4 -l` of
BootShim + firmware volume). `fastboot flash boot_a Mu-malachite.img` followed by
`fastboot --set-active=a` installs it.

## Notes

- LK on this phone only accepts the legacy LZ4 frame (`lz4 -l`) for the kernel,
  and it rejects some vendor fastboot commands by wedging; keep a stock boot
  image around.
- Only one key reaches firmware: the physical Volume Up key (the device-tree node
  is misnamed `volumedown`), GPIO 54, active low. A short press is `SCAN_UP`, a
  press held past 700 ms is Enter.
- The UFS PHY and clocks are left running by LK, which read the boot image from
  that controller; `UfsMtkDxe` publishes `EDKII_UFS_HOST_CONTROLLER_PROTOCOL` and
  the generic EDK2 UFS stack does the rest.
- There is no UART. Bring-up diagnostics go to the panel; the XNU side has its
  own log monitor and saved-log slots (see the HackiOS `xnuloader`).
- USB, touch and the GPU are not implemented here.

This platform is paired with the HackiOS `xnuloader` port; see its
`docs/malachite.md` for the XNU side.
