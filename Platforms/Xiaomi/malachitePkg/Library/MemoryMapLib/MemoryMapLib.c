#include <Library/MemoryMapLib.h>

STATIC
EFI_MEMORY_REGION_DESCRIPTOR
gMemoryDescriptor[] = {
  // Name, Address, Length, HobOption, ResourceType, ResourceAttribute, MemoryType, ArmAttribute

  // DDR Regions
  {"UEFI FD",            0x41000000, 0x00200000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"UEFI Stack",         0x41200000, 0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  //
  // The hole between the stack and the DTB copy holds the BootShim's probe word
  // and is deliberately not allocatable, but it must be mapped: the segment
  // loader's stack sits at the very top of the UEFI stack and its cache clean
  // reaches one page past 0x41240000.
  //
  // 0x41500000..0x41600000 in the middle of it is the UFS DMA pool: the UFS
  // driver's command lists and UPIUs are EDK2 "common buffers" that the CPU
  // fills after the buffer is mapped, so they must be non-cacheable (normal
  // non-cacheable, not device: the structs are not accessed with widening
  // alignment guarantees). See UfsMtkDxe's UfsMtkDmaAlloc.
  //
  {"RAM Hole",           0x41240000, 0x002C0000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"UFS DMA",            0x41500000, 0x00100000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, NS_UNCACHED_UNBUFFERED},
  {"RAM Hole Top",       0x41600000, 0x00080000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"DTB Copy",           0x41680000, 0x00100000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK},
  //
  // The 60 MiB heap must start above the 1 MiB DTB copy: from 0x41280000 it
  // would run to 0x44E80000 and swallow the DTB region, which
  // ValidateMemoryMap rejects as an overlap and turns into a CpuDeadLoop.
  //
  {"DXE Heap",           0x41800000, 0x03C00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},

  //
  // malachite's bootloader places the display scanout buffer at the top of the
  // first 4 GiB, not at the top of the 12 GiB DRAM. The runtime DT reports:
  //   /chosen/atag,videolfb-fb_base_l = 0xFD91F000
  //   /chosen/atag,videolfb-vramSize  = 0x026E0000
  // This region must be mapped, otherwise the first GOP write faults and the
  // bootloader resets the device (black screen + preloader retry loop).
  //
  {"Display Reserved",   0xFD91F000, 0x026E0000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH},

  //
  // HackiOS: where the boot shim puts the XNUP container (loader.efi, kernel,
  // root image). Above the 4 GiB the stock reserved ranges use, below the
  // 12 GiB top. Keep in sync with BootShim's PAYLOAD_BASE and XnuLoader.h.
  //
  {"XNU Payload",        0x100000000, 0x04000000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},

  //
  // HackiOS: the rest of the high DDR is the kernel's to use. MediaTek's
  // reserved-memory ranges in the stock DT are all below 4 GiB (alloc-ranges
  // end at 0xF0000000), and the top 1 GiB of the 12 GiB is kept out.
  //
  {"System RAM",         0x104000000, 0x1BC000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},

  // Register Regions
  {"GIC Distributor",    0x0C000000, 0x00040000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"GIC Redistributors", 0x0C040000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"WatchDog Timer",     0x1C00A000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  //
  // USB3: xHCI at 0x11200000, mtu3 device controller (mac 0x11201000 + ippc
  // 0x11203E00) and the USB2 PHY at 0x11F40000 (malachite.dts: usb0@11201000,
  // xhci0@11200000, usb-phy0@11f40000).
  //
  {"USB3 xHCI",          0x11200000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"USB3 mtu3",          0x11201000, 0x00003000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"USB2 PHY",           0x11F40000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  //
  // MediaTek pin controller: the Volume Down key is GPIO 54 on this block,
  // so the keypad library reads its data-in register.
  //
  {"Pinctrl",            0x10005000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  //
  // UFS: the host controller the generic UFS stack talks to, and its PHY.
  // The bootloader leaves both running (it read this image from that disk).
  // The DT says the controller's reg is 0x2300 bytes long; ArmMmuLib rejects
  // any mapping whose start or length is not 4 KiB aligned, so round it up to
  // the next page. The extra 0xD00 bytes are unused register space.
  //
  {"UFS HC",             0x112B0000, 0x00003000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"UFS PHY",            0x112A0000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  //
  // UFS clock gates and resets: ufscfg_ao@112b8000 and ufscfg_pdn@112bb000
  // (mt6878.dts). Gate set 0x08 / clr 0x0c / status 0x04, reset assert 0x48
  // and deassert 0x4c. The MTK UFS power-up in UfsMtkDxe writes them.
  //
  {"UFS CFG AO",         0x112B8000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"UFS CFG PDN",        0x112BB000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE}
};

VOID
GetMemoryMap (
  OUT EFI_MEMORY_REGION_DESCRIPTOR **MemoryDescriptor,
  OUT UINT8                         *MemoryDescriptorCount)
{
  // Pass Data
  *MemoryDescriptor      = gMemoryDescriptor;
  *MemoryDescriptorCount = ARRAY_SIZE (gMemoryDescriptor);
}
