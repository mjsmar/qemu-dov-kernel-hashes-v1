/*
 * QEMU SPAPR PCI Resource Allocator
 *
 * Copyright IBM Corp. 20167
 *
 * Authors:
 *  Michael Roth      <mdroth@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#ifndef _SPAPR_PCI_RESOURCES_H
#define _SPAPR_PCI_RESOURCES_H

#include "qemu/osdep.h"
#include "cpu.h"

typedef enum {
    SPAPR_PCI_RESOURCE_TYPE_IO = 0,
    SPAPR_PCI_RESOURCE_TYPE_MMIO32,
    SPAPR_PCI_RESOURCE_TYPE_MMIO64,
    SPAPR_PCI_RESOURCE_TYPE_MEM32,
    SPAPR_PCI_RESOURCE_TYPE_MEM64,
    SPAPR_PCI_RESOURCE_TYPE_MAX,
} sPAPRPCIResourceType;

typedef struct sPAPRPCIResources sPAPRPCIResources;

void spapr_pci_resources_reserve_region(sPAPRPCIResources *res,
                                        sPAPRPCIResourceType rtype,
                                        hwaddr addr, uint64_t size,
                                        uint32_t id);
hwaddr spapr_pci_resources_request_bar_region(sPAPRPCIResources *res,
                                              sPAPRPCIResourceType rtype,
                                              uint64_t size, bool hotplugged,
                                              uint32_t bdf, int bar);
void spapr_pci_resources_release_region(sPAPRPCIResources *res,
                                        uint32_t id);
void spapr_pci_resources_release_bar_region(sPAPRPCIResources *res,
                                            uint32_t bdf, int bar);
void spapr_pci_resources_add_mem_range(sPAPRPCIResources *res,
                                       sPAPRPCIResourceType rtype,
                                       hwaddr address_space_offset,
                                       uint64_t size);
void spapr_pci_resources_add_io_range(sPAPRPCIResources *res,
                                      sPAPRPCIResourceType rtype,
                                      hwaddr address_space_offset,
                                      uint64_t size);
void spapr_pci_resources_reset(sPAPRPCIResources *res);
sPAPRPCIResources *spapr_pci_resources_new(const char *phb_name,
                                           uint64_t iospace_size,
                                           uint64_t memspace_size);

#endif /*_SPAPR_PCI_RESOURCES_H */
