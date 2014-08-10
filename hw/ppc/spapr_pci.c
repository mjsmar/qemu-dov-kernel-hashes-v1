/*
 * QEMU sPAPR PCI host originated from Uninorth PCI host
 *
 * Copyright (c) 2011 Alexey Kardashevskiy, IBM Corporation.
 * Copyright (C) 2011 David Gibson, IBM Corporation.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "hw/hw.h"
#include "hw/pci/pci.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
#include "hw/pci/pci_host.h"
#include "hw/ppc/spapr.h"
#include "hw/pci-host/spapr.h"
#include "exec/address-spaces.h"
#include <libfdt.h>
#include "trace.h"
#include "qemu/error-report.h"
#include "qapi/qmp/qerror.h"

#include "hw/pci/pci_bus.h"

/* Copied from the kernel arch/powerpc/platforms/pseries/msi.c */
#define RTAS_QUERY_FN           0
#define RTAS_CHANGE_FN          1
#define RTAS_RESET_FN           2
#define RTAS_CHANGE_MSI_FN      3
#define RTAS_CHANGE_MSIX_FN     4

/* Interrupt types to return on RTAS_CHANGE_* */
#define RTAS_TYPE_MSI           1
#define RTAS_TYPE_MSIX          2

#define FDT_MAX_SIZE            0x10000
#define _FDT(exp) \
    do { \
        int ret = (exp);                                           \
        if (ret < 0) {                                             \
            return ret;                                            \
        }                                                          \
    } while (0)

#include "hw/ppc/spapr_drc.h"

#define FDT_MAX_SIZE            0x10000
#define _FDT(exp) \
    do { \
        int ret = (exp);                                           \
        if (ret < 0) {                                             \
            return ret;                                            \
        }                                                          \
    } while (0)

static sPAPRPHBState *find_phb(sPAPREnvironment *spapr, uint64_t buid)
{
    sPAPRPHBState *sphb;

    QLIST_FOREACH(sphb, &spapr->phbs, list) {
        if (sphb->buid != buid) {
            continue;
        }
        return sphb;
    }

    return NULL;
}

static PCIDevice *find_dev(sPAPREnvironment *spapr, uint64_t buid,
                           uint32_t config_addr)
{
    sPAPRPHBState *sphb = find_phb(spapr, buid);
    PCIHostState *phb = PCI_HOST_BRIDGE(sphb);
    int bus_num = (config_addr >> 16) & 0xFF;
    int devfn = (config_addr >> 8) & 0xFF;

    if (!phb) {
        return NULL;
    }

    return pci_find_device(phb->bus, bus_num, devfn);
}

static uint32_t rtas_pci_cfgaddr(uint32_t arg)
{
    /* This handles the encoding of extended config space addresses */
    return ((arg >> 20) & 0xf00) | (arg & 0xff);
}

static void finish_read_pci_config(sPAPREnvironment *spapr, uint64_t buid,
                                   uint32_t addr, uint32_t size,
                                   target_ulong rets)
{
    PCIDevice *pci_dev;
    uint32_t val;

    if ((size != 1) && (size != 2) && (size != 4)) {
        /* access must be 1, 2 or 4 bytes */
        rtas_st(rets, 0, RTAS_OUT_HW_ERROR);
        return;
    }

    pci_dev = find_dev(spapr, buid, addr);
    addr = rtas_pci_cfgaddr(addr);

    if (!pci_dev || (addr % size) || (addr >= pci_config_size(pci_dev))) {
        /* Access must be to a valid device, within bounds and
         * naturally aligned */
        rtas_st(rets, 0, RTAS_OUT_HW_ERROR);
        return;
    }

    val = pci_host_config_read_common(pci_dev, addr,
                                      pci_config_size(pci_dev), size);

    rtas_st(rets, 0, RTAS_OUT_SUCCESS);
    rtas_st(rets, 1, val);
}

static void rtas_ibm_read_pci_config(PowerPCCPU *cpu, sPAPREnvironment *spapr,
                                     uint32_t token, uint32_t nargs,
                                     target_ulong args,
                                     uint32_t nret, target_ulong rets)
{
    uint64_t buid;
    uint32_t size, addr;

    if ((nargs != 4) || (nret != 2)) {
        rtas_st(rets, 0, RTAS_OUT_HW_ERROR);
        return;
    }

    buid = ((uint64_t)rtas_ld(args, 1) << 32) | rtas_ld(args, 2);
    size = rtas_ld(args, 3);
    addr = rtas_ld(args, 0);

    finish_read_pci_config(spapr, buid, addr, size, rets);
}

static void rtas_read_pci_config(PowerPCCPU *cpu, sPAPREnvironment *spapr,
                                 uint32_t token, uint32_t nargs,
                                 target_ulong args,
                                 uint32_t nret, target_ulong rets)
{
    uint32_t size, addr;

    if ((nargs != 2) || (nret != 2)) {
        rtas_st(rets, 0, RTAS_OUT_HW_ERROR);
        return;
    }

    size = rtas_ld(args, 1);
    addr = rtas_ld(args, 0);

    finish_read_pci_config(spapr, 0, addr, size, rets);
}

static void finish_write_pci_config(sPAPREnvironment *spapr, uint64_t buid,
                                    uint32_t addr, uint32_t size,
                                    uint32_t val, target_ulong rets)
{
    PCIDevice *pci_dev;

    if ((size != 1) && (size != 2) && (size != 4)) {
        /* access must be 1, 2 or 4 bytes */
        rtas_st(rets, 0, RTAS_OUT_HW_ERROR);
        return;
    }

    pci_dev = find_dev(spapr, buid, addr);
    addr = rtas_pci_cfgaddr(addr);

    if (!pci_dev || (addr % size) || (addr >= pci_config_size(pci_dev))) {
        /* Access must be to a valid device, within bounds and
         * naturally aligned */
        rtas_st(rets, 0, RTAS_OUT_HW_ERROR);
        return;
    }

    pci_host_config_write_common(pci_dev, addr, pci_config_size(pci_dev),
                                 val, size);

    rtas_st(rets, 0, RTAS_OUT_SUCCESS);
}

static void rtas_ibm_write_pci_config(PowerPCCPU *cpu, sPAPREnvironment *spapr,
                                      uint32_t token, uint32_t nargs,
                                      target_ulong args,
                                      uint32_t nret, target_ulong rets)
{
    uint64_t buid;
    uint32_t val, size, addr;

    if ((nargs != 5) || (nret != 1)) {
        rtas_st(rets, 0, RTAS_OUT_HW_ERROR);
        return;
    }

    buid = ((uint64_t)rtas_ld(args, 1) << 32) | rtas_ld(args, 2);
    val = rtas_ld(args, 4);
    size = rtas_ld(args, 3);
    addr = rtas_ld(args, 0);

    finish_write_pci_config(spapr, buid, addr, size, val, rets);
}

static void rtas_write_pci_config(PowerPCCPU *cpu, sPAPREnvironment *spapr,
                                  uint32_t token, uint32_t nargs,
                                  target_ulong args,
                                  uint32_t nret, target_ulong rets)
{
    uint32_t val, size, addr;

    if ((nargs != 3) || (nret != 1)) {
        rtas_st(rets, 0, RTAS_OUT_HW_ERROR);
        return;
    }


    val = rtas_ld(args, 2);
    size = rtas_ld(args, 1);
    addr = rtas_ld(args, 0);

    finish_write_pci_config(spapr, 0, addr, size, val, rets);
}

/*
 * Set MSI/MSIX message data.
 * This is required for msi_notify()/msix_notify() which
 * will write at the addresses via spapr_msi_write().
 *
 * If hwaddr == 0, all entries will have .data == first_irq i.e.
 * table will be reset.
 */
static void spapr_msi_setmsg(PCIDevice *pdev, hwaddr addr, bool msix,
                             unsigned first_irq, unsigned req_num)
{
    unsigned i;
    MSIMessage msg = { .address = addr, .data = first_irq };

    if (!msix) {
        msi_set_message(pdev, msg);
        trace_spapr_pci_msi_setup(pdev->name, 0, msg.address);
        return;
    }

    for (i = 0; i < req_num; ++i) {
        msix_set_message(pdev, i, msg);
        trace_spapr_pci_msi_setup(pdev->name, i, msg.address);
        if (addr) {
            ++msg.data;
        }
    }
}

static void rtas_ibm_change_msi(PowerPCCPU *cpu, sPAPREnvironment *spapr,
                                uint32_t token, uint32_t nargs,
                                target_ulong args, uint32_t nret,
                                target_ulong rets)
{
    uint32_t config_addr = rtas_ld(args, 0);
    uint64_t buid = ((uint64_t)rtas_ld(args, 1) << 32) | rtas_ld(args, 2);
    unsigned int func = rtas_ld(args, 3);
    unsigned int req_num = rtas_ld(args, 4); /* 0 == remove all */
    unsigned int seq_num = rtas_ld(args, 5);
    unsigned int ret_intr_type;
    unsigned int irq, max_irqs = 0, num = 0;
    sPAPRPHBState *phb = NULL;
    PCIDevice *pdev = NULL;
    spapr_pci_msi *msi;
    int *config_addr_key;

    switch (func) {
    case RTAS_CHANGE_MSI_FN:
    case RTAS_CHANGE_FN:
        ret_intr_type = RTAS_TYPE_MSI;
        break;
    case RTAS_CHANGE_MSIX_FN:
        ret_intr_type = RTAS_TYPE_MSIX;
        break;
    default:
        error_report("rtas_ibm_change_msi(%u) is not implemented", func);
        rtas_st(rets, 0, RTAS_OUT_PARAM_ERROR);
        return;
    }

    /* Fins sPAPRPHBState */
    phb = find_phb(spapr, buid);
    if (phb) {
        pdev = find_dev(spapr, buid, config_addr);
    }
    if (!phb || !pdev) {
        rtas_st(rets, 0, RTAS_OUT_PARAM_ERROR);
        return;
    }

    /* Releasing MSIs */
    if (!req_num) {
        msi = (spapr_pci_msi *) g_hash_table_lookup(phb->msi, &config_addr);
        if (!msi) {
            trace_spapr_pci_msi("Releasing wrong config", config_addr);
            rtas_st(rets, 0, RTAS_OUT_HW_ERROR);
            return;
        }

        xics_free(spapr->icp, msi->first_irq, msi->num);
        if (msi_present(pdev)) {
            spapr_msi_setmsg(pdev, 0, false, 0, num);
        }
        if (msix_present(pdev)) {
            spapr_msi_setmsg(pdev, 0, true, 0, num);
        }
        g_hash_table_remove(phb->msi, &config_addr);

        trace_spapr_pci_msi("Released MSIs", config_addr);
        rtas_st(rets, 0, RTAS_OUT_SUCCESS);
        rtas_st(rets, 1, 0);
        return;
    }

    /* Enabling MSI */

    /* Check if the device supports as many IRQs as requested */
    if (ret_intr_type == RTAS_TYPE_MSI) {
        max_irqs = msi_nr_vectors_allocated(pdev);
    } else if (ret_intr_type == RTAS_TYPE_MSIX) {
        max_irqs = pdev->msix_entries_nr;
    }
    if (!max_irqs) {
        error_report("Requested interrupt type %d is not enabled for device %x",
                     ret_intr_type, config_addr);
        rtas_st(rets, 0, -1); /* Hardware error */
        return;
    }
    /* Correct the number if the guest asked for too many */
    if (req_num > max_irqs) {
        trace_spapr_pci_msi_retry(config_addr, req_num, max_irqs);
        req_num = max_irqs;
        irq = 0; /* to avoid misleading trace */
        goto out;
    }

    /* Allocate MSIs */
    irq = xics_alloc_block(spapr->icp, 0, req_num, false,
                           ret_intr_type == RTAS_TYPE_MSI);
    if (!irq) {
        error_report("Cannot allocate MSIs for device %x", config_addr);
        rtas_st(rets, 0, RTAS_OUT_HW_ERROR);
        return;
    }

    /* Setup MSI/MSIX vectors in the device (via cfgspace or MSIX BAR) */
    spapr_msi_setmsg(pdev, SPAPR_PCI_MSI_WINDOW, ret_intr_type == RTAS_TYPE_MSIX,
                     irq, req_num);

    /* Add MSI device to cache */
    msi = g_new(spapr_pci_msi, 1);
    msi->first_irq = irq;
    msi->num = req_num;
    config_addr_key = g_new(int, 1);
    *config_addr_key = config_addr;
    g_hash_table_insert(phb->msi, config_addr_key, msi);

out:
    rtas_st(rets, 0, RTAS_OUT_SUCCESS);
    rtas_st(rets, 1, req_num);
    rtas_st(rets, 2, ++seq_num);
    rtas_st(rets, 3, ret_intr_type);

    trace_spapr_pci_rtas_ibm_change_msi(config_addr, func, req_num, irq);
}

static void rtas_ibm_query_interrupt_source_number(PowerPCCPU *cpu,
                                                   sPAPREnvironment *spapr,
                                                   uint32_t token,
                                                   uint32_t nargs,
                                                   target_ulong args,
                                                   uint32_t nret,
                                                   target_ulong rets)
{
    uint32_t config_addr = rtas_ld(args, 0);
    uint64_t buid = ((uint64_t)rtas_ld(args, 1) << 32) | rtas_ld(args, 2);
    unsigned int intr_src_num = -1, ioa_intr_num = rtas_ld(args, 3);
    sPAPRPHBState *phb = NULL;
    PCIDevice *pdev = NULL;
    spapr_pci_msi *msi;

    /* Find sPAPRPHBState */
    phb = find_phb(spapr, buid);
    if (phb) {
        pdev = find_dev(spapr, buid, config_addr);
    }
    if (!phb || !pdev) {
        rtas_st(rets, 0, RTAS_OUT_PARAM_ERROR);
        return;
    }

    /* Find device descriptor and start IRQ */
    msi = (spapr_pci_msi *) g_hash_table_lookup(phb->msi, &config_addr);
    if (!msi || !msi->first_irq || !msi->num || (ioa_intr_num >= msi->num)) {
        trace_spapr_pci_msi("Failed to return vector", config_addr);
        rtas_st(rets, 0, RTAS_OUT_HW_ERROR);
        return;
    }
    intr_src_num = msi->first_irq + ioa_intr_num;
    trace_spapr_pci_rtas_ibm_query_interrupt_source_number(ioa_intr_num,
                                                           intr_src_num);

    rtas_st(rets, 0, RTAS_OUT_SUCCESS);
    rtas_st(rets, 1, intr_src_num);
    rtas_st(rets, 2, 1);/* 0 == level; 1 == edge */
}

static int pci_spapr_swizzle(int slot, int pin)
{
    return (slot + pin) % PCI_NUM_PINS;
}

static int pci_spapr_map_irq(PCIDevice *pci_dev, int irq_num)
{
    /*
     * Here we need to convert pci_dev + irq_num to some unique value
     * which is less than number of IRQs on the specific bus (4).  We
     * use standard PCI swizzling, that is (slot number + pin number)
     * % 4.
     */
    return pci_spapr_swizzle(PCI_SLOT(pci_dev->devfn), irq_num);
}

static void pci_spapr_set_irq(void *opaque, int irq_num, int level)
{
    /*
     * Here we use the number returned by pci_spapr_map_irq to find a
     * corresponding qemu_irq.
     */
    sPAPRPHBState *phb = opaque;

    trace_spapr_pci_lsi_set(phb->dtbusname, irq_num, phb->lsi_table[irq_num].irq);
    qemu_set_irq(spapr_phb_lsi_qirq(phb, irq_num), level);
}

static PCIINTxRoute spapr_route_intx_pin_to_irq(void *opaque, int pin)
{
    sPAPRPHBState *sphb = SPAPR_PCI_HOST_BRIDGE(opaque);
    PCIINTxRoute route;

    route.mode = PCI_INTX_ENABLED;
    route.irq = sphb->lsi_table[pin].irq;

    return route;
}

/*
 * MSI/MSIX memory region implementation.
 * The handler handles both MSI and MSIX.
 * For MSI-X, the vector number is encoded as a part of the address,
 * data is set to 0.
 * For MSI, the vector number is encoded in least bits in data.
 */
static void spapr_msi_write(void *opaque, hwaddr addr,
                            uint64_t data, unsigned size)
{
    uint32_t irq = data;

    trace_spapr_pci_msi_write(addr, data, irq);

    qemu_irq_pulse(xics_get_qirq(spapr->icp, irq));
}

static const MemoryRegionOps spapr_msi_ops = {
    /* There is no .read as the read result is undefined by PCI spec */
    .read = NULL,
    .write = spapr_msi_write,
    .endianness = DEVICE_LITTLE_ENDIAN
};

/*
 * PHB PCI device
 */
static AddressSpace *spapr_pci_dma_iommu(PCIBus *bus, void *opaque, int devfn)
{
    sPAPRPHBState *phb = opaque;

    return &phb->iommu_as;
}

/* for 'reg'/'assigned-addresses' OF properties */
#define RESOURCE_CELLS_SIZE 2
#define RESOURCE_CELLS_ADDRESS 3
#define RESOURCE_CELLS_TOTAL \
    (RESOURCE_CELLS_SIZE + RESOURCE_CELLS_ADDRESS)

static void fill_resource_props(PCIDevice *d, int bus_num,
                                uint32_t *reg, int *reg_size,
                                uint32_t *assigned, int *assigned_size)
{
    uint32_t *reg_row, *assigned_row;
    uint32_t dev_id = ((bus_num << 8) |
                        (PCI_SLOT(d->devfn) << 3) | PCI_FUNC(d->devfn));
    int i, idx = 0;

    reg[0] = cpu_to_be32(dev_id << 8);

    for (i = 0; i < PCI_NUM_REGIONS; i++) {
        if (!d->io_regions[i].size) {
            continue;
        }
        reg_row = &reg[(idx + 1) * RESOURCE_CELLS_TOTAL];
        assigned_row = &assigned[idx * RESOURCE_CELLS_TOTAL];
        reg_row[0] = cpu_to_be32((dev_id << 8) | (pci_bar(d, i) & 0xff));
        if (d->io_regions[i].type & PCI_BASE_ADDRESS_SPACE_IO) {
            reg_row[0] |= cpu_to_be32(0x01000000);
        } else {
            reg_row[0] |= cpu_to_be32(0x02000000);
        }
        assigned_row[0] = cpu_to_be32(reg_row[0] | 0x80000000);
        assigned_row[3] = reg_row[3] = cpu_to_be32(d->io_regions[i].size >> 32);
        assigned_row[4] = reg_row[4] = cpu_to_be32(d->io_regions[i].size);
        assigned_row[1] = cpu_to_be32(d->io_regions[i].addr >> 32);
        assigned_row[2] = cpu_to_be32(d->io_regions[i].addr);
        idx++;
    }

    *reg_size = (idx + 1) * RESOURCE_CELLS_TOTAL * sizeof(uint32_t);
    *assigned_size = idx * RESOURCE_CELLS_TOTAL * sizeof(uint32_t);
}

static int spapr_populate_pci_child_dt(PCIDevice *dev, void *fdt, int offset,
                                       int phb_index, int drc_index)
{
    int slot = PCI_SLOT(dev->devfn);
    char slotname[16];
    bool is_bridge = 1;
    uint32_t reg[RESOURCE_CELLS_TOTAL * 8] = { 0 };
    uint32_t assigned[RESOURCE_CELLS_TOTAL * 8] = { 0 };
    int pci_status, reg_size, assigned_size;

    if (pci_default_read_config(dev, PCI_HEADER_TYPE, 1) ==
        PCI_HEADER_TYPE_NORMAL) {
        is_bridge = 0;
    }

    _FDT(fdt_setprop_cell(fdt, offset, "vendor-id",
                          pci_default_read_config(dev, PCI_VENDOR_ID, 2)));
    _FDT(fdt_setprop_cell(fdt, offset, "device-id",
                          pci_default_read_config(dev, PCI_DEVICE_ID, 2)));
    _FDT(fdt_setprop_cell(fdt, offset, "revision-id",
                          pci_default_read_config(dev, PCI_REVISION_ID, 1)));
    _FDT(fdt_setprop_cell(fdt, offset, "class-code",
                          pci_default_read_config(dev, PCI_CLASS_DEVICE, 2) << 8));

    _FDT(fdt_setprop_cell(fdt, offset, "interrupts",
                          pci_default_read_config(dev, PCI_INTERRUPT_PIN, 1)));

    /* if this device is NOT a bridge */
    if (!is_bridge) {
        _FDT(fdt_setprop_cell(fdt, offset, "min-grant",
            pci_default_read_config(dev, PCI_MIN_GNT, 1)));
        _FDT(fdt_setprop_cell(fdt, offset, "max-latency",
            pci_default_read_config(dev, PCI_MAX_LAT, 1)));
        _FDT(fdt_setprop_cell(fdt, offset, "subsystem-id",
            pci_default_read_config(dev, PCI_SUBSYSTEM_ID, 2)));
        _FDT(fdt_setprop_cell(fdt, offset, "subsystem-vendor-id",
            pci_default_read_config(dev, PCI_SUBSYSTEM_VENDOR_ID, 2)));
    }

    _FDT(fdt_setprop_cell(fdt, offset, "cache-line-size",
        pci_default_read_config(dev, PCI_CACHE_LINE_SIZE, 1)));

    /* the following fdt cells are masked off the pci status register */
    pci_status = pci_default_read_config(dev, PCI_STATUS, 2);
    _FDT(fdt_setprop_cell(fdt, offset, "devsel-speed",
                          PCI_STATUS_DEVSEL_MASK & pci_status));
    _FDT(fdt_setprop_cell(fdt, offset, "fast-back-to-back",
                          PCI_STATUS_FAST_BACK & pci_status));
    _FDT(fdt_setprop_cell(fdt, offset, "66mhz-capable",
                          PCI_STATUS_66MHZ & pci_status));
    _FDT(fdt_setprop_cell(fdt, offset, "udf-supported",
                          PCI_STATUS_UDF & pci_status));

    _FDT(fdt_setprop_string(fdt, offset, "name", "pci"));
    sprintf(slotname, "Slot %d", slot + phb_index * PCI_SLOT_MAX);
    _FDT(fdt_setprop(fdt, offset, "ibm,loc-code", slotname, strlen(slotname)));
    _FDT(fdt_setprop_cell(fdt, offset, "ibm,my-drc-index", drc_index));

    _FDT(fdt_setprop_cell(fdt, offset, "#address-cells",
                          RESOURCE_CELLS_ADDRESS));
    _FDT(fdt_setprop_cell(fdt, offset, "#size-cells",
                          RESOURCE_CELLS_SIZE));
    _FDT(fdt_setprop_cell(fdt, offset, "ibm,req#msi-x",
                          RESOURCE_CELLS_SIZE));
    fill_resource_props(dev, phb_index, reg, &reg_size,
                        assigned, &assigned_size);
    _FDT(fdt_setprop(fdt, offset, "reg", reg, reg_size));
    _FDT(fdt_setprop(fdt, offset, "assigned-addresses",
                     assigned, assigned_size));

    return 0;
}

/*
static void spapr_drc_attach(PCIDevice *d, void *opaque)
{
    sPAPRDRConnector *drc =
        spapr_dr_connector_by_id(SPAPR_DR_CONNECTOR_TYPE_PCI, d->devfn);
    sPAPRDRConnectorClass *drck = SPAPR_DR_CONNECTOR_GET_CLASS(drc);

    drck->attach(drc, DEVICE(d), NULL, false);
}
*/

/* create OF node for pci device and required OF DT properties */
static void *spapr_create_pci_child_dt(sPAPRPHBState *phb, PCIDevice *dev,
                                       int drc_index, int *dt_offset)
{
    void *fdt_orig, *fdt;
    int offset, ret;
    int slot = PCI_SLOT(dev->devfn);
    char nodename[512];

    fdt_orig = g_malloc0(FDT_MAX_SIZE);
    offset = fdt_create(fdt_orig, FDT_MAX_SIZE);
    fdt_begin_node(fdt_orig, "");
    fdt_end_node(fdt_orig);
    fdt_finish(fdt_orig);

    fdt = g_malloc0(FDT_MAX_SIZE);
    fdt_open_into(fdt_orig, fdt, FDT_MAX_SIZE);
    sprintf(nodename, "pci@%d", slot);
    offset = fdt_add_subnode(fdt, 0, nodename);
    ret = spapr_populate_pci_child_dt(dev, fdt, offset, phb->index, drc_index);
    g_assert(!ret);
    g_free(fdt_orig);

    *dt_offset = offset;
    return fdt;
}

static void spapr_device_hotplug_add(sPAPRPHBState *phb, PCIDevice *pdev)
{
    sPAPRDRConnector *drc =
        spapr_dr_connector_by_id(SPAPR_DR_CONNECTOR_TYPE_PCI,
                                 pdev->devfn);
    sPAPRDRConnectorClass *drck = SPAPR_DR_CONNECTOR_GET_CLASS(drc);
    DeviceState *dev = DEVICE(pdev);
    int drc_index = drck->get_index(drc);
    void *fdt = NULL;
    int fdt_start_offset = 0;

    /* boot-time devices get their device tree node created by SLOF, but for
     * hotplugged devices we need QEMU to generate it so the guest can fetch
     * it via RTAS
     */
    if (dev->hotplugged) {
        fdt = spapr_create_pci_child_dt(phb, pdev, drc_index,
                                        &fdt_start_offset);
    }
    drck->attach(drc, DEVICE(pdev), fdt, fdt_start_offset, !dev->hotplugged);
}

static void spapr_device_hotplug_remove_cb(DeviceState *dev, void *opaque)
{
    g_warning("doing device cleanup");
    object_unparent(OBJECT(dev));
    /* add qmp event */
}

static void spapr_device_hotplug_remove(sPAPRPHBState *phb, PCIDevice *dev)
{
    sPAPRDRConnector *drc =
        spapr_dr_connector_by_id(SPAPR_DR_CONNECTOR_TYPE_PCI,
                                 dev->devfn);
    sPAPRDRConnectorClass *drck = SPAPR_DR_CONNECTOR_GET_CLASS(drc);

    g_warning("doing detach");
    drck->detach(drc, DEVICE(dev), spapr_device_hotplug_remove_cb, phb);
}

static void spapr_phb_hot_plug(HotplugHandler *plug_handler,
                               DeviceState *plugged_dev, Error **errp)
{
    sPAPRPHBState *phb = SPAPR_PCI_HOST_BRIDGE(DEVICE(plug_handler));
    PCIDevice *pdev = PCI_DEVICE(plugged_dev);

    /* if DR is disabled we don't need to do anything in the case of
     * hotplug or coldplug callbacks
     */
    if (!phb->dr_enabled) {
        /* if this is a hotplug operation initiated by the user
         * we need to let them know it's not enabled
         */
        if (plugged_dev->hotplugged) {
            error_set(errp, QERR_BUS_NO_HOTPLUG,
                      object_get_typename(OBJECT(phb)));
        }
        return;
    }
    spapr_device_hotplug_add(phb, pdev);
}

static void spapr_phb_hot_unplug(HotplugHandler *plug_handler,
                                 DeviceState *plugged_dev, Error **errp)
{
    sPAPRPHBState *phb = SPAPR_PCI_HOST_BRIDGE(DEVICE(plug_handler));
    PCIDevice *pdev = PCI_DEVICE(plugged_dev);

    if (!phb->dr_enabled) {
        error_set(errp, QERR_BUS_NO_HOTPLUG,
                  object_get_typename(OBJECT(phb)));
        return;
    }

    spapr_device_hotplug_remove(phb, pdev);
}

static void spapr_phb_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *s = SYS_BUS_DEVICE(dev);
    sPAPRPHBState *sphb = SPAPR_PCI_HOST_BRIDGE(s);
    PCIHostState *phb = PCI_HOST_BRIDGE(s);
    sPAPRPHBClass *info = SPAPR_PCI_HOST_BRIDGE_GET_CLASS(s);
    char *namebuf;
    int i;
    PCIBus *bus;
    uint64_t msi_window_size = 4096;

    if (sphb->index != -1) {
        hwaddr windows_base;

        if ((sphb->buid != -1) || (sphb->dma_liobn != -1)
            || (sphb->mem_win_addr != -1)
            || (sphb->io_win_addr != -1)) {
            error_setg(errp, "Either \"index\" or other parameters must"
                       " be specified for PAPR PHB, not both");
            return;
        }

        sphb->buid = SPAPR_PCI_BASE_BUID + sphb->index;
        sphb->dma_liobn = SPAPR_PCI_BASE_LIOBN + sphb->index;

        windows_base = SPAPR_PCI_WINDOW_BASE
            + sphb->index * SPAPR_PCI_WINDOW_SPACING;
        sphb->mem_win_addr = windows_base + SPAPR_PCI_MMIO_WIN_OFF;
        sphb->io_win_addr = windows_base + SPAPR_PCI_IO_WIN_OFF;
    }

    if (sphb->buid == -1) {
        error_setg(errp, "BUID not specified for PHB");
        return;
    }

    if (sphb->dma_liobn == -1) {
        error_setg(errp, "LIOBN not specified for PHB");
        return;
    }

    if (sphb->mem_win_addr == -1) {
        error_setg(errp, "Memory window address not specified for PHB");
        return;
    }

    if (sphb->io_win_addr == -1) {
        error_setg(errp, "IO window address not specified for PHB");
        return;
    }

    if (find_phb(spapr, sphb->buid)) {
        error_setg(errp, "PCI host bridges must have unique BUIDs");
        return;
    }

    sphb->dtbusname = g_strdup_printf("pci@%" PRIx64, sphb->buid);

    namebuf = alloca(strlen(sphb->dtbusname) + 32);

    /* Initialize memory regions */
    sprintf(namebuf, "%s.mmio", sphb->dtbusname);
    memory_region_init(&sphb->memspace, OBJECT(sphb), namebuf, UINT64_MAX);

    sprintf(namebuf, "%s.mmio-alias", sphb->dtbusname);
    memory_region_init_alias(&sphb->memwindow, OBJECT(sphb),
                             namebuf, &sphb->memspace,
                             SPAPR_PCI_MEM_WIN_BUS_OFFSET, sphb->mem_win_size);
    memory_region_add_subregion(get_system_memory(), sphb->mem_win_addr,
                                &sphb->memwindow);

    /* Initialize IO regions */
    sprintf(namebuf, "%s.io", sphb->dtbusname);
    memory_region_init(&sphb->iospace, OBJECT(sphb),
                       namebuf, SPAPR_PCI_IO_WIN_SIZE);

    sprintf(namebuf, "%s.io-alias", sphb->dtbusname);
    memory_region_init_alias(&sphb->iowindow, OBJECT(sphb), namebuf,
                             &sphb->iospace, 0, SPAPR_PCI_IO_WIN_SIZE);
    memory_region_add_subregion(get_system_memory(), sphb->io_win_addr,
                                &sphb->iowindow);

    bus = pci_register_bus(dev, NULL,
                           pci_spapr_set_irq, pci_spapr_map_irq, sphb,
                           &sphb->memspace, &sphb->iospace,
                           PCI_DEVFN(0, 0), PCI_NUM_PINS, TYPE_PCI_BUS);
    phb->bus = bus;
    qbus_set_hotplug_handler(BUS(phb->bus), DEVICE(sphb), NULL);

    /*
     * Initialize PHB address space.
     * By default there will be at least one subregion for default
     * 32bit DMA window.
     * Later the guest might want to create another DMA window
     * which will become another memory subregion.
     */
    sprintf(namebuf, "%s.iommu-root", sphb->dtbusname);

    memory_region_init(&sphb->iommu_root, OBJECT(sphb),
                       namebuf, UINT64_MAX);
    address_space_init(&sphb->iommu_as, &sphb->iommu_root,
                       sphb->dtbusname);

    /*
     * As MSI/MSIX interrupts trigger by writing at MSI/MSIX vectors,
     * we need to allocate some memory to catch those writes coming
     * from msi_notify()/msix_notify().
     * As MSIMessage:addr is going to be the same and MSIMessage:data
     * is going to be a VIRQ number, 4 bytes of the MSI MR will only
     * be used.
     *
     * For KVM we want to ensure that this memory is a full page so that
     * our memory slot is of page size granularity.
     */
#ifdef CONFIG_KVM
    if (kvm_enabled()) {
        msi_window_size = getpagesize();
    }
#endif

    memory_region_init_io(&sphb->msiwindow, NULL, &spapr_msi_ops, spapr,
                          "msi", msi_window_size);
    memory_region_add_subregion(&sphb->iommu_root, SPAPR_PCI_MSI_WINDOW,
                                &sphb->msiwindow);

    pci_setup_iommu(bus, spapr_pci_dma_iommu, sphb);

    pci_bus_set_route_irq_fn(bus, spapr_route_intx_pin_to_irq);

    QLIST_INSERT_HEAD(&spapr->phbs, sphb, list);

    /* Initialize the LSI table */
    for (i = 0; i < PCI_NUM_PINS; i++) {
        uint32_t irq;

        irq = xics_alloc_block(spapr->icp, 0, 1, true, false);
        if (!irq) {
            error_setg(errp, "spapr_allocate_lsi failed");
            return;
        }

        sphb->lsi_table[i].irq = irq;
    }

    /* Associate PHB with a DR connector (to enable sensor state reporting,
     * and eventually unplug), and allocate connectors for child PCI devices
     */
    if (sphb->dr_enabled) {
        /* TODO: it's not mandatory that a PHB be associated with a
         * dr-connector except to support PHB hotplug. so introduce
         * this as part of PHB hotplug support as opposed to making
         * it a dependency for PCI hotplug
         */
#if 0
        for (i = 0; i < SPAPR_DRC_MAX_PHB; i++) {
            sPAPRDRConnector *drc;
            sPAPRDRConnectorClass *drck;

            drc = spapr_dr_connector_by_id(SPAPR_DR_CONNECTOR_TYPE_PHB, i);
            if (drc) {
                drck = SPAPR_DR_CONNECTOR_GET_CLASS(drc);
                if (drck->entity_sense(drc) == SPAPR_DR_ENTITY_SENSE_EMPTY) {
                    drck->attach(drc, DEVICE(sphb), NULL, 0, true);
                    break;
                }
            }
        }

        if (i == SPAPR_DRC_MAX_PHB) {
            error_setg(errp, "Unable to find free DR connector for PHB");
            return;
        }
#endif

        for (i = 0; i < PCI_SLOT_MAX; i++) {
            spapr_dr_connector_new(OBJECT(phb),
                                   SPAPR_DR_CONNECTOR_TYPE_PCI,
                                   (sphb->index << 8) | (i << 3));
        }
    }

    if (!info->finish_realize) {
        error_setg(errp, "finish_realize not defined");
        return;
    }

    info->finish_realize(sphb, errp);

    sphb->msi = g_hash_table_new_full(g_int_hash, g_int_equal, g_free, g_free);
}

static void spapr_phb_finish_realize(sPAPRPHBState *sphb, Error **errp)
{
    sPAPRTCETable *tcet;

    tcet = spapr_tce_new_table(DEVICE(sphb), sphb->dma_liobn,
                               0,
                               SPAPR_TCE_PAGE_SHIFT,
                               0x40000000 >> SPAPR_TCE_PAGE_SHIFT, false);
    if (!tcet) {
        error_setg(errp, "Unable to create TCE table for %s",
                   sphb->dtbusname);
        return ;
    }

    /* Register default 32bit DMA window */
    memory_region_add_subregion(&sphb->iommu_root, 0,
                                spapr_tce_get_iommu(tcet));
}

static int spapr_phb_children_reset(Object *child, void *opaque)
{
    DeviceState *dev = (DeviceState *) object_dynamic_cast(child, TYPE_DEVICE);

    if (dev) {
        device_reset(dev);
    }

    return 0;
}

static void spapr_phb_reset(DeviceState *qdev)
{
    /* Reset the IOMMU state */
    object_child_foreach(OBJECT(qdev), spapr_phb_children_reset, NULL);
}

static Property spapr_phb_properties[] = {
    DEFINE_PROP_INT32("index", sPAPRPHBState, index, -1),
    DEFINE_PROP_UINT64("buid", sPAPRPHBState, buid, -1),
    DEFINE_PROP_UINT32("liobn", sPAPRPHBState, dma_liobn, -1),
    DEFINE_PROP_UINT64("mem_win_addr", sPAPRPHBState, mem_win_addr, -1),
    DEFINE_PROP_UINT64("mem_win_size", sPAPRPHBState, mem_win_size,
                       SPAPR_PCI_MMIO_WIN_SIZE),
    DEFINE_PROP_UINT64("io_win_addr", sPAPRPHBState, io_win_addr, -1),
    DEFINE_PROP_UINT64("io_win_size", sPAPRPHBState, io_win_size,
                       SPAPR_PCI_IO_WIN_SIZE),
    DEFINE_PROP_BOOL("dynamic-reconfiguration", sPAPRPHBState, dr_enabled,
                     true),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vmstate_spapr_pci_lsi = {
    .name = "spapr_pci/lsi",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_EQUAL(irq, struct spapr_pci_lsi),

        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_spapr_pci_msi = {
    .name = "spapr_pci/msi",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField []) {
        VMSTATE_UINT32(key, spapr_pci_msi_mig),
        VMSTATE_UINT32(value.first_irq, spapr_pci_msi_mig),
        VMSTATE_UINT32(value.num, spapr_pci_msi_mig),
        VMSTATE_END_OF_LIST()
    },
};

static void spapr_pci_fill_msi_devs(gpointer key, gpointer value,
                                    gpointer opaque)
{
    sPAPRPHBState *sphb = opaque;

    sphb->msi_devs[sphb->msi_devs_num].key = *(uint32_t *)key;
    sphb->msi_devs[sphb->msi_devs_num].value = *(spapr_pci_msi *)value;
    sphb->msi_devs_num++;
}

static void spapr_pci_pre_save(void *opaque)
{
    sPAPRPHBState *sphb = opaque;
    int msi_devs_num;

    if (sphb->msi_devs) {
        g_free(sphb->msi_devs);
        sphb->msi_devs = NULL;
    }
    sphb->msi_devs_num = 0;
    msi_devs_num = g_hash_table_size(sphb->msi);
    if (!msi_devs_num) {
        return;
    }
    sphb->msi_devs = g_malloc(msi_devs_num * sizeof(spapr_pci_msi_mig));

    g_hash_table_foreach(sphb->msi, spapr_pci_fill_msi_devs, sphb);
    assert(sphb->msi_devs_num == msi_devs_num);
}

static int spapr_pci_post_load(void *opaque, int version_id)
{
    sPAPRPHBState *sphb = opaque;
    gpointer key, value;
    int i;

    for (i = 0; i < sphb->msi_devs_num; ++i) {
        key = g_memdup(&sphb->msi_devs[i].key,
                       sizeof(sphb->msi_devs[i].key));
        value = g_memdup(&sphb->msi_devs[i].value,
                         sizeof(sphb->msi_devs[i].value));
        g_hash_table_insert(sphb->msi, key, value);
    }
    if (sphb->msi_devs) {
        g_free(sphb->msi_devs);
        sphb->msi_devs = NULL;
    }
    sphb->msi_devs_num = 0;

    return 0;
}

static const VMStateDescription vmstate_spapr_pci = {
    .name = "spapr_pci",
    .version_id = 2,
    .minimum_version_id = 2,
    .pre_save = spapr_pci_pre_save,
    .post_load = spapr_pci_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT64_EQUAL(buid, sPAPRPHBState),
        VMSTATE_UINT32_EQUAL(dma_liobn, sPAPRPHBState),
        VMSTATE_UINT64_EQUAL(mem_win_addr, sPAPRPHBState),
        VMSTATE_UINT64_EQUAL(mem_win_size, sPAPRPHBState),
        VMSTATE_UINT64_EQUAL(io_win_addr, sPAPRPHBState),
        VMSTATE_UINT64_EQUAL(io_win_size, sPAPRPHBState),
        VMSTATE_STRUCT_ARRAY(lsi_table, sPAPRPHBState, PCI_NUM_PINS, 0,
                             vmstate_spapr_pci_lsi, struct spapr_pci_lsi),
        VMSTATE_INT32(msi_devs_num, sPAPRPHBState),
        VMSTATE_STRUCT_VARRAY_ALLOC(msi_devs, sPAPRPHBState, msi_devs_num, 0,
                                    vmstate_spapr_pci_msi, spapr_pci_msi_mig),
        VMSTATE_END_OF_LIST()
    },
};

static const char *spapr_phb_root_bus_path(PCIHostState *host_bridge,
                                           PCIBus *rootbus)
{
    sPAPRPHBState *sphb = SPAPR_PCI_HOST_BRIDGE(host_bridge);

    return sphb->dtbusname;
}

static void spapr_phb_class_init(ObjectClass *klass, void *data)
{
    PCIHostBridgeClass *hc = PCI_HOST_BRIDGE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);
    sPAPRPHBClass *spc = SPAPR_PCI_HOST_BRIDGE_CLASS(klass);
    HotplugHandlerClass *hp = HOTPLUG_HANDLER_CLASS(klass);

    hc->root_bus_path = spapr_phb_root_bus_path;
    dc->realize = spapr_phb_realize;
    dc->props = spapr_phb_properties;
    dc->reset = spapr_phb_reset;
    dc->vmsd = &vmstate_spapr_pci;
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
    dc->cannot_instantiate_with_device_add_yet = false;
    spc->finish_realize = spapr_phb_finish_realize;
    hp->plug = spapr_phb_hot_plug;
    hp->unplug = spapr_phb_hot_unplug;
}

static const TypeInfo spapr_phb_info = {
    .name          = TYPE_SPAPR_PCI_HOST_BRIDGE,
    .parent        = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(sPAPRPHBState),
    .class_init    = spapr_phb_class_init,
    .class_size    = sizeof(sPAPRPHBClass),
    .interfaces    = (InterfaceInfo[]) {
        { TYPE_HOTPLUG_HANDLER },
        { }
    }
};

PCIHostState *spapr_create_phb(sPAPREnvironment *spapr, int index)
{
    DeviceState *dev;

    dev = qdev_create(NULL, TYPE_SPAPR_PCI_HOST_BRIDGE);
    qdev_prop_set_uint32(dev, "index", index);
    qdev_init_nofail(dev);

    return PCI_HOST_BRIDGE(dev);
}

/* Macros to operate with address in OF binding to PCI */
#define b_x(x, p, l)    (((x) & ((1<<(l))-1)) << (p))
#define b_n(x)          b_x((x), 31, 1) /* 0 if relocatable */
#define b_p(x)          b_x((x), 30, 1) /* 1 if prefetchable */
#define b_t(x)          b_x((x), 29, 1) /* 1 if the address is aliased */
#define b_ss(x)         b_x((x), 24, 2) /* the space code */
#define b_bbbbbbbb(x)   b_x((x), 16, 8) /* bus number */
#define b_ddddd(x)      b_x((x), 11, 5) /* device number */
#define b_fff(x)        b_x((x), 8, 3)  /* function number */
#define b_rrrrrrrr(x)   b_x((x), 0, 8)  /* register number */

typedef struct sPAPRTCEDT {
    void *fdt;
    int node_off;
} sPAPRTCEDT;

static int spapr_phb_children_dt(Object *child, void *opaque)
{
    sPAPRTCEDT *p = opaque;
    sPAPRTCETable *tcet;

    tcet = (sPAPRTCETable *) object_dynamic_cast(child, TYPE_SPAPR_TCE_TABLE);
    if (!tcet) {
        return 0;
    }

    spapr_dma_dt(p->fdt, p->node_off, "ibm,dma-window",
                 tcet->liobn, tcet->bus_offset,
                 tcet->nb_table << tcet->page_shift);
    /* Stop after the first window */

    return 1;
}

static void spapr_create_drc_phb_dt_entries(void *fdt, int bus_off,
                                            int phb_index)
{
    char char_buf[1024];
    uint32_t int_buf[PCI_SLOT_MAX + 1];
    uint32_t *entries;
    int i, ret, offset;
    sPAPRDRConnector *drc;
    sPAPRDRConnectorClass *drck;

    /* ibm,drc-indexes */
    memset(int_buf, 0 , sizeof(int_buf));
    int_buf[0] = PCI_SLOT_MAX;

    for (i = 1; i < PCI_SLOT_MAX; i++) {
        //int_buf[i] = SPAPR_DRC_DEV_ID_BASE + (phb_index << 8) + ((i - 1) << 3);
        drc = spapr_dr_connector_by_id(SPAPR_DR_CONNECTOR_TYPE_PCI,
                                       (phb_index << 8) | (i << 3));
        g_assert(drc);
        drck = SPAPR_DR_CONNECTOR_GET_CLASS(drc);
        int_buf[i-1] = drck->get_index(drc);
    }

    ret = fdt_setprop(fdt, bus_off, "ibm,drc-indexes", int_buf,
                      sizeof(int_buf));
    if (ret) {
        fprintf(stderr, "error adding 'ibm,drc-indexes' field for PHB FDT");
    }

    /* ibm,drc-power-domains */
    memset(int_buf, 0, sizeof(int_buf));
    int_buf[0] = PCI_SLOT_MAX;

    for (i = 1; i <= PCI_SLOT_MAX; i++) {
        int_buf[i] = 0xffffffff;
    }

    ret = fdt_setprop(fdt, bus_off, "ibm,drc-power-domains", int_buf,
                      sizeof(int_buf));
    if (ret) {
        fprintf(stderr,
                "error adding 'ibm,drc-power-domains' field for PHB FDT");
    }

    /* ibm,drc-names */
    memset(char_buf, 0, sizeof(char_buf));
    entries = (uint32_t *)&char_buf[0];
    *entries = PCI_SLOT_MAX;
    offset = sizeof(*entries);

    for (i = 1; i <= PCI_SLOT_MAX; i++) {
        offset += sprintf(char_buf + offset, "Slot %d",
                          (phb_index * PCI_SLOT_MAX) + i - 1);
        char_buf[offset++] = '\0';
    }

    ret = fdt_setprop(fdt, bus_off, "ibm,drc-names", char_buf, offset);
    if (ret) {
        fprintf(stderr, "error adding 'ibm,drc-names' field for PHB FDT");
    }

    /* ibm,drc-types */
    memset(char_buf, 0, sizeof(char_buf));
    entries = (uint32_t *)&char_buf[0];
    *entries = PCI_SLOT_MAX;
    offset = sizeof(*entries);

    for (i = 0; i < PCI_SLOT_MAX; i++) {
        offset += sprintf(char_buf + offset, "28");
        char_buf[offset++] = '\0';
    }

    ret = fdt_setprop(fdt, bus_off, "ibm,drc-types", char_buf, offset);
    if (ret) {
        fprintf(stderr, "error adding 'ibm,drc-types' field for PHB FDT");
    }

    /* we want the initial indicator state to be 0 - "empty", when we
     * hot-plug an adaptor in the slot, we need to set the indicator
     * to 1 - "present."
     */

    /* ibm,indicator-9003 */
    memset(int_buf, 0, sizeof(int_buf));
    int_buf[0] = PCI_SLOT_MAX;

    ret = fdt_setprop(fdt, bus_off, "ibm,indicator-9003", int_buf,
                      sizeof(int_buf));
    if (ret) {
        fprintf(stderr, "error adding 'ibm,indicator-9003' field for PHB FDT");
    }

    /* ibm,sensor-9003 */
    memset(int_buf, 0, sizeof(int_buf));
    int_buf[0] = PCI_SLOT_MAX;

    ret = fdt_setprop(fdt, bus_off, "ibm,sensor-9003", int_buf,
                      sizeof(int_buf));
    if (ret) {
        fprintf(stderr, "error adding 'ibm,sensor-9003' field for PHB FDT");
    }
}

int spapr_populate_pci_dt(sPAPRPHBState *phb,
                          uint32_t xics_phandle,
                          void *fdt)
{
    int bus_off, i, j;
    char nodename[256];
    uint32_t bus_range[] = { cpu_to_be32(0), cpu_to_be32(0xff) };
    struct {
        uint32_t hi;
        uint64_t child;
        uint64_t parent;
        uint64_t size;
    } QEMU_PACKED ranges[] = {
        {
            cpu_to_be32(b_ss(1)), cpu_to_be64(0),
            cpu_to_be64(phb->io_win_addr),
            cpu_to_be64(memory_region_size(&phb->iospace)),
        },
        {
            cpu_to_be32(b_ss(2)), cpu_to_be64(SPAPR_PCI_MEM_WIN_BUS_OFFSET),
            cpu_to_be64(phb->mem_win_addr),
            cpu_to_be64(memory_region_size(&phb->memwindow)),
        },
    };
    uint64_t bus_reg[] = { cpu_to_be64(phb->buid), 0 };
    uint32_t interrupt_map_mask[] = {
        cpu_to_be32(b_ddddd(-1)|b_fff(0)), 0x0, 0x0, cpu_to_be32(-1)};
    uint32_t interrupt_map[PCI_SLOT_MAX * PCI_NUM_PINS][7];
    sPAPRDRConnector *drc;

    /* Start populating the FDT */
    sprintf(nodename, "pci@%" PRIx64, phb->buid);
    bus_off = fdt_add_subnode(fdt, 0, nodename);
    if (bus_off < 0) {
        return bus_off;
    }

    /* Write PHB properties */
    _FDT(fdt_setprop_string(fdt, bus_off, "device_type", "pci"));
    _FDT(fdt_setprop_string(fdt, bus_off, "compatible", "IBM,Logical_PHB"));
    _FDT(fdt_setprop_cell(fdt, bus_off, "#address-cells", 0x3));
    _FDT(fdt_setprop_cell(fdt, bus_off, "#size-cells", 0x2));
    _FDT(fdt_setprop_cell(fdt, bus_off, "#interrupt-cells", 0x1));
    _FDT(fdt_setprop(fdt, bus_off, "used-by-rtas", NULL, 0));
    _FDT(fdt_setprop(fdt, bus_off, "bus-range", &bus_range, sizeof(bus_range)));
    _FDT(fdt_setprop(fdt, bus_off, "ranges", &ranges, sizeof(ranges)));
    _FDT(fdt_setprop(fdt, bus_off, "reg", &bus_reg, sizeof(bus_reg)));
    _FDT(fdt_setprop_cell(fdt, bus_off, "ibm,pci-config-space-type", 0x1));
    _FDT(fdt_setprop_cell(fdt, bus_off, "ibm,pe-total-#msi", XICS_IRQS));

    /* Build the interrupt-map, this must matches what is done
     * in pci_spapr_map_irq
     */
    _FDT(fdt_setprop(fdt, bus_off, "interrupt-map-mask",
                     &interrupt_map_mask, sizeof(interrupt_map_mask)));
    for (i = 0; i < PCI_SLOT_MAX; i++) {
        for (j = 0; j < PCI_NUM_PINS; j++) {
            uint32_t *irqmap = interrupt_map[i*PCI_NUM_PINS + j];
            int lsi_num = pci_spapr_swizzle(i, j);

            irqmap[0] = cpu_to_be32(b_ddddd(i)|b_fff(0));
            irqmap[1] = 0;
            irqmap[2] = 0;
            irqmap[3] = cpu_to_be32(j+1);
            irqmap[4] = cpu_to_be32(xics_phandle);
            irqmap[5] = cpu_to_be32(phb->lsi_table[lsi_num].irq);
            irqmap[6] = cpu_to_be32(0x8);
        }
    }
    /* Write interrupt map */
    _FDT(fdt_setprop(fdt, bus_off, "interrupt-map", &interrupt_map,
                     sizeof(interrupt_map)));

    object_child_foreach(OBJECT(phb), spapr_phb_children_dt,
                         &((sPAPRTCEDT){ .fdt = fdt, .node_off = bus_off }));

    drc = spapr_dr_connector_by_id(SPAPR_DR_CONNECTOR_TYPE_PHB, phb->index);
    if (drc) {
        sPAPRDRConnectorClass *drck = SPAPR_DR_CONNECTOR_GET_CLASS(drc);
        uint32_t drc_index = drck->get_index(drc);

        _FDT(fdt_setprop(fdt, bus_off, "ibm,my-drc-index", &drc_index,
                         sizeof(drc_index)));
    }
    if (phb->dr_enabled) {
        spapr_create_drc_phb_dt_entries(fdt, bus_off, phb->index);
    }

    return 0;
}

void spapr_pci_rtas_init(void)
{
    spapr_rtas_register(RTAS_READ_PCI_CONFIG, "read-pci-config",
                        rtas_read_pci_config);
    spapr_rtas_register(RTAS_WRITE_PCI_CONFIG, "write-pci-config",
                        rtas_write_pci_config);
    spapr_rtas_register(RTAS_IBM_READ_PCI_CONFIG, "ibm,read-pci-config",
                        rtas_ibm_read_pci_config);
    spapr_rtas_register(RTAS_IBM_WRITE_PCI_CONFIG, "ibm,write-pci-config",
                        rtas_ibm_write_pci_config);
    if (msi_supported) {
        spapr_rtas_register(RTAS_IBM_QUERY_INTERRUPT_SOURCE_NUMBER,
                            "ibm,query-interrupt-source-number",
                            rtas_ibm_query_interrupt_source_number);
        spapr_rtas_register(RTAS_IBM_CHANGE_MSI, "ibm,change-msi",
                            rtas_ibm_change_msi);
    }
}

static void spapr_pci_register_types(void)
{
    type_register_static(&spapr_phb_info);
}

type_init(spapr_pci_register_types)
