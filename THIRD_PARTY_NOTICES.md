# 第三方來源聲明

本文件保留教材引用片段及補丁所涉及原檔的開頭聲明。版本以 sources.lock.json 的 commit 為準。
文件沒有個別聲明時，應連同上游授權規則閱讀；本文件不改變其授權。

授權範圍見 [LICENSING.md](LICENSING.md)。

## edk2/BaseTools/Conf/tools_def.template

```text
此檔案開頭沒有個別授權聲明；適用上游專案的授權規則。
```

## edk2/MdeModulePkg/Bus/Pci/PciBusDxe/PciEnumeratorSupport.c

```text
/** @file
  PCI emumeration support functions implementation for PCI Bus module.

Copyright (c) 2006 - 2021, Intel Corporation. All rights reserved.<BR>
(C) Copyright 2015 Hewlett Packard Enterprise Development LP<BR>
Copyright (C) 2023 Advanced Micro Devices, Inc. All rights reserved.<BR>
SPDX-License-Identifier: BSD-2-Clause-Patent

**/
```

## edk2/MdeModulePkg/Bus/Pci/PciBusDxe/PciIo.c

```text
/** @file
  EFI PCI IO protocol functions implementation for PCI Bus module.

Copyright (c) 2006 - 2019, Intel Corporation. All rights reserved.<BR>
SPDX-License-Identifier: BSD-2-Clause-Patent

**/
```

## edk2/MdeModulePkg/MdeModulePkg.dec

```text
此檔案開頭沒有個別授權聲明；適用上游專案的授權規則。
```

## edk2/OvmfPkg/OvmfPkgX64.dsc

```text
此檔案開頭沒有個別授權聲明；適用上游專案的授權規則。
```

## edk2/UefiCpuPkg/Library/CpuExceptionHandlerLib/X64/ExceptionHandlerAsm.nasm

```text
;------------------------------------------------------------------------------ ;
; Copyright (c) 2012 - 2022, Intel Corporation. All rights reserved.<BR>
; SPDX-License-Identifier: BSD-2-Clause-Patent
;
; Module Name:
;
;   ExceptionHandlerAsm.Asm
;
; Abstract:
;
;   x64 CPU Exception Handler
;
; Notes:
;
;------------------------------------------------------------------------------
```

## linux/drivers/firmware/efi/libstub/efi-stub-helper.c

```text
// SPDX-License-Identifier: GPL-2.0
/*
 * Helper functions used by the EFI stub on multiple
 * architectures. This should be #included by the EFI stub
 * implementation files.
 *
 * Copyright 2011 Intel Corporation; author Matt Fleming
 */
```

## linux/drivers/iommu/intel/iommu.c

```text
// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright © 2006-2014 Intel Corporation.
 *
 * Authors: David Woodhouse <dwmw2@infradead.org>,
 *          Ashok Raj <ashok.raj@intel.com>,
 *          Shaohua Li <shaohua.li@intel.com>,
 *          Anil S Keshavamurthy <anil.s.keshavamurthy@intel.com>,
 *          Fenghua Yu <fenghua.yu@intel.com>
 *          Joerg Roedel <jroedel@suse.de>
 */
```

## linux/drivers/pci/iov.c

```text
// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Express I/O Virtualization (IOV) support
 *   Single Root IOV 1.0
 *   Address Translation Service 1.0
 *
 * Copyright (C) 2009 Intel Corporation, Yu Zhao <yu.zhao@intel.com>
 */
```

## linux/drivers/vfio/Kconfig

```text
# SPDX-License-Identifier: GPL-2.0-only
```

## linux/drivers/vfio/pci/Kconfig

```text
# SPDX-License-Identifier: GPL-2.0-only
```

## linux/drivers/vfio/pci/vfio_pci_core.c

```text
// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 Red Hat, Inc.  All rights reserved.
 *     Author: Alex Williamson <alex.williamson@redhat.com>
 *
 * Derived from original vfio:
 * Copyright 2010 Cisco Systems, Inc.  All rights reserved.
 * Author: Tom Lyon, pugs@cisco.com
 */
```

## linux/drivers/vfio/vfio_iommu_type1.c

```text
// SPDX-License-Identifier: GPL-2.0-only
/*
 * VFIO: IOMMU DMA mapping support for Type1 IOMMU
 *
 * Copyright (C) 2012 Red Hat, Inc.  All rights reserved.
 *     Author: Alex Williamson <alex.williamson@redhat.com>
 *
 * Derived from original vfio:
 * Copyright 2010 Cisco Systems, Inc.  All rights reserved.
 * Author: Tom Lyon, pugs@cisco.com
 *
 * We arbitrarily define a Type1 IOMMU as one matching the below code.
 * It could be called the x86 IOMMU as it's designed for AMD-Vi & Intel
 * VT-d, but that makes it harder to re-use as theoretically anyone
 * implementing a similar IOMMU could make use of this.  We expect the
 * IOMMU to support the IOMMU API and have few to no restrictions around
 * the IOVA range that can be mapped.  The Type1 IOMMU is currently
 * optimized for relatively static mappings of a userspace process with
 * userspace pages pinned into memory.  We also assume devices and IOMMU
 * domains are PCI based as the IOMMU API is still centered around a
 * device/bus interface rather than a group interface.
 */
```

## linux/fs/Makefile

```text
# SPDX-License-Identifier: GPL-2.0
```

## linux/include/linux/pci.h

```text
/* SPDX-License-Identifier: GPL-2.0 */
/*
 *	pci.h
 *
 *	PCI defines and function prototypes
 *	Copyright 1994, Drew Eckhardt
 *	Copyright 1997--1999 Martin Mares <mj@ucw.cz>
 *
 *	PCI Express ASPM defines and function prototypes
 *	Copyright (c) 2007 Intel Corp.
 *		Zhang Yanmin (yanmin.zhang@intel.com)
 *		Shaohua Li (shaohua.li@intel.com)
 *
 *	For more information, please consult the following manuals (look at
 *	http://www.pcisig.com/ for how to get them):
 *
 *	PCI BIOS Specification
 *	PCI Local Bus Specification
 *	PCI to PCI Bridge Specification
 *	PCI Express Specification
 *	PCI System Design Guide
 */
```

## linux/include/uapi/linux/vfio.h

```text
/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * VFIO API definition
 *
 * Copyright (C) 2012 Red Hat, Inc.  All rights reserved.
 *     Author: Alex Williamson <alex.williamson@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
```

## linux/lib/Kconfig.kasan

```text
# SPDX-License-Identifier: GPL-2.0-only

# This config refers to the generic KASAN mode.
```

## linux/lib/Kconfig.kcsan

```text
# SPDX-License-Identifier: GPL-2.0-only
```

## qemu/hw/net/igbvf.c

```text
/*
 * QEMU Intel 82576 SR/IOV Ethernet Controller Emulation
 *
 * Datasheet:
 * https://www.intel.com/content/dam/www/public/us/en/documents/datasheets/82576eg-gbe-datasheet.pdf
 *
 * Copyright (c) 2020-2023 Red Hat, Inc.
 * Copyright (c) 2015 Ravello Systems LTD (http://ravellosystems.com)
 * Developed by Daynix Computing LTD (http://www.daynix.com)
 *
 * Authors:
 * Akihiko Odaki <akihiko.odaki@daynix.com>
 * Gal Hammmer <gal.hammer@sap.com>
 * Marcel Apfelbaum <marcel.apfelbaum@gmail.com>
 * Dmitry Fleytman <dmitry@daynix.com>
 * Leonid Bloch <leonid@daynix.com>
 * Yan Vugenfirer <yan@daynix.com>
 *
 * Based on work done by:
 * Nir Peleg, Tutis Systems Ltd. for Qumranet Inc.
 * Copyright (c) 2008 Qumranet
 * Based on work done by:
 * Copyright (c) 2007 Dan Aloni
 * Copyright (c) 2004 Antony T Curtis
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
```

## qemu/hw/pci/msix.c

```text
/*
 * MSI-X device support
 *
 * This module includes support for MSI-X in pci devices.
 *
 * Author: Michael S. Tsirkin <mst@redhat.com>
 *
 *  Copyright (c) 2009, Red Hat Inc, Michael S. Tsirkin (mst@redhat.com)
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */
```

## qemu/hw/pci/pcie.c

```text
/*
 * pcie.c
 *
 * Copyright (c) 2010 Isaku Yamahata <yamahata at valinux co jp>
 *                    VA Linux Systems Japan K.K.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */
```

## qemu/hw/pci/pcie_sriov.c

```text
/*
 * pcie_sriov.c:
 *
 * Implementation of SR/IOV emulation support.
 *
 * Copyright (c) 2015-2017 Knut Omang <knut.omang@oracle.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */
```

## qemu/include/hw/pci/pci_device.h

```text
此檔案開頭沒有個別授權聲明；適用上游專案的授權規則。
```

## qemu/include/hw/pci/pcie_sriov.h

```text
/*
 * pcie_sriov.h:
 *
 * Implementation of SR/IOV emulation support.
 *
 * Copyright (c) 2015 Knut Omang <knut.omang@oracle.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */
```

## qemu/pythondeps.toml

```text
# This file describes Python package requirements to be
# installed in the pyvenv Python virtual environment.
```
