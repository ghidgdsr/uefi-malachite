#ifndef _XNU_LOADER_H_
#define _XNU_LOADER_H_

//
// malachite (MT6878): the boot shim copies everything appended after the UEFI FD
// to XNU_PAYLOAD_BASE, which the platform memory map marks reserved. Kept in
// sync with the Makefile's PAYLOAD_BASE and xnuloader/loader.c's PAYLOAD_BASE.
//
#define XNU_PAYLOAD_BASE      0x100000000ULL
#define XNU_PAYLOAD_MAX_SIZE  0x04000000ULL
#define XNU_PAYLOAD_MAGIC     0x50554E58 // "XNUP"

typedef struct {
  UINT32 Magic;
  UINT32 Size;
} XNU_PAYLOAD_HEADER;

// The header is followed by entries; an empty name ends the list.
typedef struct {
  CHAR8  Name[16];
  UINT32 Size;
  UINT32 Reserved;
  // UINT8 Data[Size], padded to 16 bytes
} XNU_PAYLOAD_ENTRY;

VOID
XnuLoaderMain ();

#endif /* _XNU_LOADER_H_ */
