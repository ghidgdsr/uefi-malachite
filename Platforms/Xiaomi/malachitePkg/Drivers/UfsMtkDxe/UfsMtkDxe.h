/** @file
  MediaTek MT6878 UFS host controller (MMIO) header.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _UFS_MTK_DXE_H_
#define _UFS_MTK_DXE_H_

#include <Uefi.h>
#include <Protocol/UfsHostController.h>

//
// The controller and PHY from the device tree:
//   ufshci@112b0000, compatible "mediatek,mt8183-ufshci", reg 0x2300
//   ufsphy@112a0000, compatible "mediatek,mt8183-ufsphy"
//
#define UFS_MTK_HC_BASE   0x112B0000ULL
#define UFS_MTK_PHY_BASE  0x112A0000ULL
//
// ufscfg_ao@112b8000 and ufscfg_pdn@112bb000 (mt6878.dts): UFS clock gates and
// resets. Gate set 0x08 / clr 0x0c / status 0x04; reset assert 0x48, deassert
// 0x4c (pdn bits 0..2 = unipro/crypto/hci; ao bit 8 = mphy).
//
#define UFS_MTK_AO_BASE   0x112B8000ULL
#define UFS_MTK_PDN_BASE  0x112BB000ULL

#define UFS_MTK_PRIVATE_SIGNATURE  SIGNATURE_32 ('U', 'F', 'S', 'M')

typedef struct {
  UINT32                                Signature;
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL    UfsHcProtocol;
} UFS_MTK_PRIVATE_DATA;

#endif
