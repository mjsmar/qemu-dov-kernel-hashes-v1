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

#include "hw/pci/pci_bus.h"

/* #define DEBUG_SPAPR */

#ifdef DEBUG_SPAPR
#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

/* Copied from the kernel arch/powerpc/platforms/pseries/msi.c */
#define RTAS_QUERY_FN           0
#define RTAS_CHANGE_FN          1
#define RTAS_RESET_FN           2
#define RTAS_CHANGE_MSI_FN      3
#define RTAS_CHANGE_MSIX_FN     4

/* Interrupt types to return on RTAS_CHANGE_* */
#define RTAS_TYPE_MSI           1
#define RTAS_TYPE_MSIX          2

/* For set-indicator RTAS interface */
#define INDICATOR_ISOLATION_MASK            0x0001   /* 9001 one bit */
#define INDICATOR_GLOBAL_INTERRUPT_MASK     0x0002   /* 9005 one bit */
#define INDICATOR_ERROR_LOG_MASK            0x0004   /* 9006 one bit */
#define INDICATOR_IDENTIFY_MASK             0x0008   /* 9007 one bit */
#define INDICATOR_RESET_MASK                0x0010   /* 9009 one bit */
#define INDICATOR_DR_MASK                   0x00e0   /* 9002 three bits */
#define INDICATOR_ALLOCATION_MASK           0x0300   /* 9003 two bits */
#define INDICATOR_EPOW_MASK                 0x1c00   /* 9 three bits */
#define INDICATOR_ENTITY_SENSE_MASK         0xe000   /* 9003 three bits */

#define INDICATOR_ISOLATION_SHIFT           0x00     /* bit 0 */
#define INDICATOR_GLOBAL_INTERRUPT_SHIFT    0x01     /* bit 1 */
#define INDICATOR_ERROR_LOG_SHIFT           0x02     /* bit 2 */
#define INDICATOR_IDENTIFY_SHIFT            0x03     /* bit 3 */
#define INDICATOR_RESET_SHIFT               0x04     /* bit 4 */
#define INDICATOR_DR_SHIFT                  0x05     /* bits 5-7 */
#define INDICATOR_ALLOCATION_SHIFT          0x08     /* bits 8-9 */
#define INDICATOR_EPOW_SHIFT                0x0a     /* bits 10-12 */
#define INDICATOR_ENTITY_SENSE_SHIFT        0x0d     /* bits 13-15 */

#define INDICATOR_ENTITY_SENSE_EMPTY    0
#define INDICATOR_ENTITY_SENSE_PRESENT  1

#define DECODE_DRC_STATE(state, m, s)                  \
    ((((uint32_t)(state) & (uint32_t)(m))) >> (s))

#define ENCODE_DRC_STATE(val, m, s) \
    (((uint32_t)(val) << (s)) & (uint32_t)(m))

#define FDT_MAX_SIZE            0x10000
#define _FDT(exp) \
    do { \
        int ret = (exp);                                           \
        if (ret < 0) {                                             \
            return ret;                                            \
        }                                                          \
    } while (0)

static void spapr_drc_state_reset(sPAPRDrcEntry *drc_entry);

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
 * Find an entry with config_addr or returns the empty one if not found AND
 * alloc_new is set.
 * At the moment the msi_table entries are never released so there is
 * no point to look till the end of the list if we need to find the free entry.
 */
static int spapr_msicfg_find(sPAPRPHBState *phb, uint32_t config_addr,
                             bool alloc_new)
{
    int i;

    for (i = 0; i < SPAPR_MSIX_MAX_DEVS; ++i) {
        if (!phb->msi_table[i].nvec) {
            break;
        }
        if (phb->msi_table[i].config_addr == config_addr) {
            return i;
        }
    }
    if ((i < SPAPR_MSIX_MAX_DEVS) && alloc_new) {
        trace_spapr_pci_msi("Allocating new MSI config", i, config_addr);
        return i;
    }

    return -1;
}

/*
 * Set MSI/MSIX message data.
 * This is required for msi_notify()/msix_notify() which
 * will write at the addresses via spapr_msi_write().
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

    for (i = 0; i < req_num; ++i, ++msg.data) {
        msix_set_message(pdev, i, msg);
        trace_spapr_pci_msi_setup(pdev->name, i, msg.address);
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
    int ndev, irq, max_irqs = 0;
    sPAPRPHBState *phb = NULL;
    PCIDevice *pdev = NULL;

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
        ndev = spapr_msicfg_find(phb, config_addr, false);
        if (ndev < 0) {
            trace_spapr_pci_msi("MSI has not been enabled", -1, config_addr);
            rtas_st(rets, 0, RTAS_OUT_HW_ERROR);
            return;
        }
        trace_spapr_pci_msi("Released MSIs", ndev, config_addr);
        rtas_st(rets, 0, RTAS_OUT_SUCCESS);
        rtas_st(rets, 1, 0);
        return;
    }

    /* Enabling MSI */

    /* Find a device number in the map to add or reuse the existing one */
    ndev = spapr_msicfg_find(phb, config_addr, true);
    if (ndev >= SPAPR_MSIX_MAX_DEVS || ndev < 0) {
        error_report("No free entry for a new MSI device");
        rtas_st(rets, 0, RTAS_OUT_HW_ERROR);
        return;
    }
    trace_spapr_pci_msi("Configuring MSI", ndev, config_addr);

    /* Check if the device supports as many IRQs as requested */
    if (ret_intr_type == RTAS_TYPE_MSI) {
        max_irqs = msi_nr_vectors_allocated(pdev);
    } else if (ret_intr_type == RTAS_TYPE_MSIX) {
        max_irqs = pdev->msix_entries_nr;
    }
    if (!max_irqs) {
        error_report("Requested interrupt type %d is not enabled for device#%d",
                     ret_intr_type, ndev);
        rtas_st(rets, 0, -1); /* Hardware error */
        return;
    }
    /* Correct the number if the guest asked for too many */
    if (req_num > max_irqs) {
        req_num = max_irqs;
    }

    /* Check if there is an old config and MSI number has not changed */
    if (phb->msi_table[ndev].nvec && (req_num != phb->msi_table[ndev].nvec)) {
        /* Unexpected behaviour */
        error_report("Cannot reuse MSI config for device#%d", ndev);
        rtas_st(rets, 0, RTAS_OUT_HW_ERROR);
        return;
    }

    /* There is no cached config, allocate MSIs */
    if (!phb->msi_table[ndev].nvec) {
        irq = spapr_allocate_irq_block(req_num, false,
                                       ret_intr_type == RTAS_TYPE_MSI);
        if (irq < 0) {
            error_report("Cannot allocate MSIs for device#%d", ndev);
            rtas_st(rets, 0, RTAS_OUT_HW_ERROR);
            return;
        }
        phb->msi_table[ndev].irq = irq;
        phb->msi_table[ndev].nvec = req_num;
        phb->msi_table[ndev].config_addr = config_addr;
    }

    /* Setup MSI/MSIX vectors in the device (via cfgspace or MSIX BAR) */
    spapr_msi_setmsg(pdev, spapr->msi_win_addr, ret_intr_type == RTAS_TYPE_MSIX,
                     phb->msi_table[ndev].irq, req_num);

    rtas_st(rets, 0, RTAS_OUT_SUCCESS);
    rtas_st(rets, 1, req_num);
    rtas_st(rets, 2, ++seq_num);
    rtas_st(rets, 3, ret_intr_type);

    trace_spapr_pci_rtas_ibm_change_msi(func, req_num);
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
    int ndev;
    sPAPRPHBState *phb = NULL;

    /* Fins sPAPRPHBState */
    phb = find_phb(spapr, buid);
    if (!phb) {
        rtas_st(rets, 0, RTAS_OUT_PARAM_ERROR);
        return;
    }

    /* Find device descriptor and start IRQ */
    ndev = spapr_msicfg_find(phb, config_addr, false);
    if (ndev < 0) {
        trace_spapr_pci_msi("MSI has not been enabled", -1, config_addr);
        rtas_st(rets, 0, RTAS_OUT_HW_ERROR);
        return;
    }

    intr_src_num = phb->msi_table[ndev].irq + ioa_intr_num;
    trace_spapr_pci_rtas_ibm_query_interrupt_source_number(ioa_intr_num,
                                                           intr_src_num);

    rtas_st(rets, 0, RTAS_OUT_SUCCESS);
    rtas_st(rets, 1, intr_src_num);
    rtas_st(rets, 2, 1);/* 0 == level; 1 == edge */
}

static void rtas_set_indicator(PowerPCCPU *cpu, sPAPREnvironment *spapr,
                               uint32_t token, uint32_t nargs,
                               target_ulong args, uint32_t nret,
                               target_ulong rets)
{
    uint32_t indicator = rtas_ld(args, 0);
    uint32_t drc_index = rtas_ld(args, 1);
    uint32_t indicator_state = rtas_ld(args, 2);
    uint32_t encoded = 0, shift = 0, mask = 0;
    uint32_t *pind;
    sPAPRDrcEntry *drc_entry = NULL;

    if (drc_index == 0) { /* platform indicator */
        pind = &spapr->state;
    } else {
        drc_entry = spapr_find_drc_entry(drc_index);
        if (!drc_entry) {
            DPRINTF("rtas_set_indicator: unable to find drc_entry for %x",
                    drc_index);
            rtas_st(rets, 0, RTAS_OUT_PARAM_ERROR);
            return;
        }
        pind = &drc_entry->state;
    }

    switch (indicator) {
    case 9:  /* EPOW */
        shift = INDICATOR_EPOW_SHIFT;
        mask = INDICATOR_EPOW_MASK;
        break;
    case 9001: /* Isolation state */
        /* encode the new value into the correct bit field */
        shift = INDICATOR_ISOLATION_SHIFT;
        mask = INDICATOR_ISOLATION_MASK;
        if (drc_entry) {
            /* transition from unisolated to isolated for a hotplug slot
             * entails completion of guest-side device unplug/cleanup, so
             * we can now safely remove the device if qemu is waiting for
             * it to be released
             */
            if (DECODE_DRC_STATE(*pind, mask, shift) != indicator_state) {
                if (indicator_state == 0 && drc_entry->awaiting_release) {
                    /* device_del has been called and host is waiting
                     * for guest to release/isolate device, go ahead
                     * and remove it now
                     */
                    spapr_drc_state_reset(drc_entry);
                }
            }
        }
        break;
    case 9002: /* DR */
        shift = INDICATOR_DR_SHIFT;
        mask = INDICATOR_DR_MASK;
        break;
    case 9003: /* Allocation State */
        shift = INDICATOR_ALLOCATION_SHIFT;
        mask = INDICATOR_ALLOCATION_MASK;
        break;
    case 9005: /* global interrupt */
        shift = INDICATOR_GLOBAL_INTERRUPT_SHIFT;
        mask = INDICATOR_GLOBAL_INTERRUPT_MASK;
        break;
    case 9006: /* error log */
        shift = INDICATOR_ERROR_LOG_SHIFT;
        mask = INDICATOR_ERROR_LOG_MASK;
        break;
    case 9007: /* identify */
        shift = INDICATOR_IDENTIFY_SHIFT;
        mask = INDICATOR_IDENTIFY_MASK;
        break;
    case 9009: /* reset */
        shift = INDICATOR_RESET_SHIFT;
        mask = INDICATOR_RESET_MASK;
        break;
    default:
        DPRINTF("rtas_set_indicator: indicator not implemented: %d",
                indicator);
        rtas_st(rets, 0, RTAS_OUT_PARAM_ERROR);
        return;
    }

    encoded = ENCODE_DRC_STATE(indicator_state, mask, shift);
    /* clear the current indicator value */
    *pind &= ~mask;
    /* set the new value */
    *pind |= encoded;
    rtas_st(rets, 0, RTAS_OUT_SUCCESS);
}

static void rtas_set_power_level(PowerPCCPU *cpu, sPAPREnvironment *spapr,
                                 uint32_t token, uint32_t nargs,
                                 target_ulong args, uint32_t nret,
                                 target_ulong rets)
{
    /* we currently only use a single, "live insert" powerdomain for
     * hotplugged/dlpar'd resources, so the power is always live/full (100)
     */
    rtas_st(rets, 0, RTAS_OUT_SUCCESS);
    rtas_st(rets, 1, 100);
}

static void rtas_get_power_level(PowerPCCPU *cpu, sPAPREnvironment *spapr,
                                  uint32_t token, uint32_t nargs,
                                  target_ulong args, uint32_t nret,
                                  target_ulong rets)
{
    rtas_st(rets, 0, RTAS_OUT_SUCCESS);
    rtas_st(rets, 1, 100);
}

static void rtas_get_sensor_state(PowerPCCPU *cpu, sPAPREnvironment *spapr,
                                  uint32_t token, uint32_t nargs,
                                  target_ulong args, uint32_t nret,
                                  target_ulong rets)
{
    uint32_t sensor = rtas_ld(args, 0);
    uint32_t drc_index = rtas_ld(args, 1);
    uint32_t sensor_state = 0, decoded = 0;
    uint32_t shift = 0, mask = 0;
    sPAPRDrcEntry *drc_entry = NULL;

    if (drc_index == 0) {  /* platform state sensor/indicator */
        sensor_state = spapr->state;
    } else { /* we should have a drc entry */
        drc_entry = spapr_find_drc_entry(drc_index);
        if (!drc_entry) {
            DPRINTF("unable to find DRC entry for index %x", drc_index);
            sensor_state = 0; /* empty */
            rtas_st(rets, 0, RTAS_OUT_PARAM_ERROR);
            return;
        }
        sensor_state = drc_entry->state;
    }
    switch (sensor) {
    case 9:  /* EPOW */
        shift = INDICATOR_EPOW_SHIFT;
        mask = INDICATOR_EPOW_MASK;
        break;
    case 9001: /* Isolation state */
        /* encode the new value into the correct bit field */
        shift = INDICATOR_ISOLATION_SHIFT;
        mask = INDICATOR_ISOLATION_MASK;
        break;
    case 9002: /* DR */
        shift = INDICATOR_DR_SHIFT;
        mask = INDICATOR_DR_MASK;
        break;
    case 9003: /* entity sense */
        shift = INDICATOR_ENTITY_SENSE_SHIFT;
        mask = INDICATOR_ENTITY_SENSE_MASK;
        break;
    case 9005: /* global interrupt */
        shift = INDICATOR_GLOBAL_INTERRUPT_SHIFT;
        mask = INDICATOR_GLOBAL_INTERRUPT_MASK;
        break;
    case 9006: /* error log */
        shift = INDICATOR_ERROR_LOG_SHIFT;
        mask = INDICATOR_ERROR_LOG_MASK;
        break;
    case 9007: /* identify */
        shift = INDICATOR_IDENTIFY_SHIFT;
        mask = INDICATOR_IDENTIFY_MASK;
        break;
    case 9009: /* reset */
        shift = INDICATOR_RESET_SHIFT;
        mask = INDICATOR_RESET_MASK;
        break;
    default:
        DPRINTF("rtas_get_sensor_state: sensor not implemented: %d",
                sensor);
        rtas_st(rets, 0, RTAS_OUT_PARAM_ERROR);
        return;
    }

    decoded = DECODE_DRC_STATE(sensor_state, mask, shift);
    rtas_st(rets, 0, RTAS_OUT_SUCCESS);
    rtas_st(rets, 1, decoded);
}

/* configure-connector work area offsets, int32_t units */
#define CC_IDX_NODE_NAME_OFFSET 2
#define CC_IDX_PROP_NAME_OFFSET 2
#define CC_IDX_PROP_LEN 3
#define CC_IDX_PROP_DATA_OFFSET 4

#define CC_VAL_DATA_OFFSET ((CC_IDX_PROP_DATA_OFFSET + 1) * 4)
#define CC_RET_NEXT_SIB 1
#define CC_RET_NEXT_CHILD 2
#define CC_RET_NEXT_PROPERTY 3
#define CC_RET_PREV_PARENT 4
#define CC_RET_ERROR RTAS_OUT_HW_ERROR
#define CC_RET_SUCCESS RTAS_OUT_SUCCESS

static void rtas_ibm_configure_connector(PowerPCCPU *cpu,
                                         sPAPREnvironment *spapr,
                                         uint32_t token, uint32_t nargs,
                                         target_ulong args, uint32_t nret,
                                         target_ulong rets)
{
    uint64_t wa_addr = ((uint64_t)rtas_ld(args, 1) << 32) | rtas_ld(args, 0);
    sPAPRDrcEntry *drc_entry = NULL;
    sPAPRConfigureConnectorState *ccs;
    void *wa_buf;
    int32_t *wa_buf_int;
    hwaddr map_len = 0x1024;
    uint32_t drc_index;
    int rc = 0, next_offset, tag, prop_len, node_name_len;
    const struct fdt_property *prop;
    const char *node_name, *prop_name;

    wa_buf = cpu_physical_memory_map(wa_addr, &map_len, 1);
    if (!wa_buf) {
        rc = CC_RET_ERROR;
        goto error_exit;
    }
    wa_buf_int = wa_buf;

    drc_index = *(uint32_t *)wa_buf;
    drc_entry = spapr_find_drc_entry(drc_index);
    if (!drc_entry) {
        rc = -1;
        goto error_exit;
    }

    ccs = &drc_entry->cc_state;
    if (ccs->state == CC_STATE_PENDING) {
        /* fdt should've been been attached to drc_entry during
         * realize/hotplug
         */
        g_assert(ccs->fdt);
        ccs->depth = 0;
        ccs->offset = ccs->offset_start;
        ccs->state = CC_STATE_ACTIVE;
    }

    if (ccs->state == CC_STATE_IDLE) {
        rc = -1;
        goto error_exit;
    }

retry:
    tag = fdt_next_tag(ccs->fdt, ccs->offset, &next_offset);

    switch (tag) {
    case FDT_BEGIN_NODE:
        ccs->depth++;
        node_name = fdt_get_name(ccs->fdt, ccs->offset, &node_name_len);
        wa_buf_int[CC_IDX_NODE_NAME_OFFSET] = CC_VAL_DATA_OFFSET;
        strcpy(wa_buf + wa_buf_int[CC_IDX_NODE_NAME_OFFSET], node_name);
        rc = CC_RET_NEXT_CHILD;
        break;
    case FDT_END_NODE:
        ccs->depth--;
        if (ccs->depth == 0) {
            /* reached the end of top-level node, declare success */
            ccs->state = CC_STATE_PENDING;
            rc = CC_RET_SUCCESS;
        } else {
            rc = CC_RET_PREV_PARENT;
        }
        break;
    case FDT_PROP:
        prop = fdt_get_property_by_offset(ccs->fdt, ccs->offset, &prop_len);
        prop_name = fdt_string(ccs->fdt, fdt32_to_cpu(prop->nameoff));
        wa_buf_int[CC_IDX_PROP_NAME_OFFSET] = CC_VAL_DATA_OFFSET;
        wa_buf_int[CC_IDX_PROP_LEN] = prop_len;
        wa_buf_int[CC_IDX_PROP_DATA_OFFSET] =
            CC_VAL_DATA_OFFSET + strlen(prop_name) + 1;
        strcpy(wa_buf + wa_buf_int[CC_IDX_PROP_NAME_OFFSET], prop_name);
        memcpy(wa_buf + wa_buf_int[CC_IDX_PROP_DATA_OFFSET],
               prop->data, prop_len);
        rc = CC_RET_NEXT_PROPERTY;
        break;
    case FDT_END:
        rc = CC_RET_ERROR;
        break;
    default:
        ccs->offset = next_offset;
        goto retry;
    }

    ccs->offset = next_offset;

error_exit:
    cpu_physical_memory_unmap(wa_buf, 0x1024, 1, 0x1024);
    rtas_st(rets, 0, rc);
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

void spapr_pci_msi_init(sPAPREnvironment *spapr, hwaddr addr)
{
    uint64_t window_size = 4096;

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
        window_size = getpagesize();
    }
#endif

    spapr->msi_win_addr = addr;
    memory_region_init_io(&spapr->msiwindow, NULL, &spapr_msi_ops, spapr,
                          "msi", window_size);
    memory_region_add_subregion(get_system_memory(), spapr->msi_win_addr,
                                &spapr->msiwindow);
}

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

static hwaddr spapr_find_bar_addr(sPAPRPHBState *phb, PCIIORegion *r)
{
    MemoryRegionSection mrs = { 0 };
    hwaddr search_addr;
    hwaddr size = r->size;
    hwaddr addr_mask = ~(size - 1);
    hwaddr increment = size;
    hwaddr limit;

    if (!(r->type & PCI_BASE_ADDRESS_SPACE_IO)) {
        /* beginning portion of mmio address space for bus does not get
         * mapped into system memory, so calculate addr starting at the
         * corresponding offset into mmio as.
         */
        search_addr = (SPAPR_PCI_MEM_WIN_BUS_OFFSET + increment) & addr_mask;
    } else {
        search_addr = increment;
    }
    limit = memory_region_size(r->address_space);

    do {
        mrs = memory_region_find_subregion(r->address_space, search_addr, size);
        if (mrs.mr) {
            hwaddr mr_last_addr;
            mr_last_addr = mrs.mr->addr + memory_region_size(mrs.mr) - 1;
            search_addr = (mr_last_addr + 1) & addr_mask;
            if (search_addr <= mr_last_addr) {
                search_addr += increment;
            }
            /* this memory region overlaps, unref and continue searching */
            memory_region_unref(mrs.mr);
        }
    } while (int128_nz(mrs.size) && search_addr + size <= limit);

    if (search_addr + size >= limit) {
        return PCI_BAR_UNMAPPED;
    }

    return search_addr;
}

static int spapr_map_bars(sPAPRPHBState *phb, PCIDevice *dev)
{
    PCIIORegion *r;
    int i, ret = -1;

    for (i = 0; i < PCI_NUM_REGIONS; i++) {
        uint32_t bar_address = pci_bar(dev, i);
        uint32_t bar_value;
        uint16_t cmd_value = pci_default_read_config(dev, PCI_COMMAND, 2);
        hwaddr addr;

        r = &dev->io_regions[i];

        /* this region isn't registered */
        if (!r->size) {
            continue;
        }

        /* find a hw addr we can map */
        addr = spapr_find_bar_addr(phb, r);
        if (addr == PCI_BAR_UNMAPPED) {
            /* we can't find a free range within address space for this BAR */
            fprintf(stderr,
                    "Unable to map BAR %d, no free range available\n", i);
            return -1;
        }
        /* we can probably map this region into memory if there is not
         * a race condition with some other allocator. write the address
         * to the device BAR which will force a call to pci_update_mappings
         */
        if (r->type & PCI_BASE_ADDRESS_SPACE_IO) {
            pci_default_write_config(dev, PCI_COMMAND,
                                     cmd_value | PCI_COMMAND_IO, 2);
        } else {
            pci_default_write_config(dev, PCI_COMMAND,
                                     cmd_value | PCI_COMMAND_MEMORY, 2);
        }

        bar_value = addr;

        if (i == PCI_ROM_SLOT) {
            bar_value |= PCI_ROM_ADDRESS_ENABLE;
        }
        /* write the new bar value */
        pci_default_write_config(dev, bar_address, bar_value, 4);

        /* if this is a 64-bit BAR, we need to also write the
         * upper 32 bit value.
         */
        if (r->type & PCI_BASE_ADDRESS_MEM_TYPE_64) {
            bar_value = (addr >> 32) & 0xffffffffUL;
            pci_default_write_config(dev, bar_address + 4, bar_value, 4);
        }
        ret = 0;
    }
    return ret;
}

static int spapr_populate_pci_child_dt(PCIDevice *dev, void *fdt, int offset,
                                       int phb_index)
{
    int slot = PCI_SLOT(dev->devfn);
    char slotname[16];
    bool is_bridge = 1;
    sPAPRDrcEntry *drc_entry, *drc_entry_slot;
    uint32_t reg[RESOURCE_CELLS_TOTAL * 8] = { 0 };
    uint32_t assigned[RESOURCE_CELLS_TOTAL * 8] = { 0 };
    int reg_size, assigned_size;

    drc_entry = spapr_phb_to_drc_entry(phb_index + SPAPR_PCI_BASE_BUID);
    g_assert(drc_entry);
    drc_entry_slot = &drc_entry->child_entries[slot];

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
    int pci_status = pci_default_read_config(dev, PCI_STATUS, 2);
    _FDT(fdt_setprop_cell(fdt, offset, "devsel-speed",
                          PCI_STATUS_DEVSEL_MASK & pci_status));
    _FDT(fdt_setprop_cell(fdt, offset, "fast-back-to-back",
                          PCI_STATUS_FAST_BACK & pci_status));
    _FDT(fdt_setprop_cell(fdt, offset, "66mhz-capable",
                          PCI_STATUS_66MHZ & pci_status));
    _FDT(fdt_setprop_cell(fdt, offset, "udf-supported",
                          PCI_STATUS_UDF & pci_status));

    _FDT(fdt_setprop_string(fdt, offset, "name", "pci"));
    sprintf(slotname, "Slot %d", slot + phb_index * 32);
    _FDT(fdt_setprop(fdt, offset, "ibm,loc-code", slotname, strlen(slotname)));
    _FDT(fdt_setprop_cell(fdt, offset, "ibm,my-drc-index",
                          drc_entry_slot->drc_index));

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

static int spapr_device_hotplug_add(DeviceState *qdev, PCIDevice *dev)
{
    sPAPRPHBState *phb = SPAPR_PCI_HOST_BRIDGE(qdev);
    sPAPRDrcEntry *drc_entry, *drc_entry_slot;
    sPAPRConfigureConnectorState *ccs;
    int slot = PCI_SLOT(dev->devfn);
    int offset, ret;
    void *fdt_orig, *fdt;
    char nodename[512];
    uint32_t encoded = ENCODE_DRC_STATE(INDICATOR_ENTITY_SENSE_PRESENT,
                                        INDICATOR_ENTITY_SENSE_MASK,
                                        INDICATOR_ENTITY_SENSE_SHIFT);

    drc_entry = spapr_phb_to_drc_entry(phb->buid);
    g_assert(drc_entry);
    drc_entry_slot = &drc_entry->child_entries[slot];

    drc_entry->state &= ~(uint32_t)INDICATOR_ENTITY_SENSE_MASK;
    drc_entry->state |= encoded; /* DR entity present */
    drc_entry_slot->state &= ~(uint32_t)INDICATOR_ENTITY_SENSE_MASK;
    drc_entry_slot->state |= encoded; /* and the slot */

    /* reliable unplug requires we wait for a transition from
     * UNISOLATED->ISOLATED prior to device removal/deletion.
     * However, slots populated by devices at boot-time will not
     * have ever been set by guest tools to an UNISOLATED/populated
     * state, so set this manually in the case of coldplug devices
     */
    if (DEVICE(dev)->hotplugged) {
        /* need to allocate memory region for device BARs */
        spapr_map_bars(phb, dev);
    } else {
        drc_entry_slot->state |= ENCODE_DRC_STATE(1,
                                                  INDICATOR_ISOLATION_MASK,
                                                  INDICATOR_ISOLATION_SHIFT);
    }

    /* add OF node for pci device and required OF DT properties */
    fdt_orig = g_malloc0(FDT_MAX_SIZE);
    offset = fdt_create(fdt_orig, FDT_MAX_SIZE);
    fdt_begin_node(fdt_orig, "");
    fdt_end_node(fdt_orig);
    fdt_finish(fdt_orig);

    fdt = g_malloc0(FDT_MAX_SIZE);
    fdt_open_into(fdt_orig, fdt, FDT_MAX_SIZE);
    sprintf(nodename, "pci@%d", slot);
    offset = fdt_add_subnode(fdt, 0, nodename);
    ret = spapr_populate_pci_child_dt(dev, fdt, offset, phb->index);
    g_assert(!ret);
    g_free(fdt_orig);

    /* hold on to node, configure_connector will pass it to the guest later */
    ccs = &drc_entry_slot->cc_state;
    ccs->fdt = fdt;
    ccs->offset_start = offset;
    ccs->state = CC_STATE_PENDING;
    ccs->dev = dev;

    return 0;
}

/* check whether guest has released/isolated device */
static bool spapr_drc_state_is_releasable(sPAPRDrcEntry *drc_entry)
{
    return !DECODE_DRC_STATE(drc_entry->state,
                             INDICATOR_ISOLATION_MASK,
                             INDICATOR_ISOLATION_SHIFT);
}

/* finalize device unplug/deletion */
static void spapr_drc_state_reset(sPAPRDrcEntry *drc_entry)
{
    sPAPRConfigureConnectorState *ccs = &drc_entry->cc_state;
    uint32_t sense_empty = ENCODE_DRC_STATE(INDICATOR_ENTITY_SENSE_EMPTY,
                                            INDICATOR_ENTITY_SENSE_MASK,
                                            INDICATOR_ENTITY_SENSE_SHIFT);

    g_free(ccs->fdt);
    ccs->fdt = NULL;
    object_unparent(OBJECT(ccs->dev));
    ccs->dev = NULL;
    ccs->state = CC_STATE_IDLE;
    drc_entry->state &= ~INDICATOR_ENTITY_SENSE_MASK;
    drc_entry->state |= sense_empty;
    drc_entry->awaiting_release = false;
}

static void spapr_device_hotplug_remove(DeviceState *qdev, PCIDevice *dev)
{
    sPAPRPHBState *phb = SPAPR_PCI_HOST_BRIDGE(qdev);
    sPAPRDrcEntry *drc_entry, *drc_entry_slot;
    sPAPRConfigureConnectorState *ccs;
    int slot = PCI_SLOT(dev->devfn);

    drc_entry = spapr_phb_to_drc_entry(phb->buid);
    g_assert(drc_entry);
    drc_entry_slot = &drc_entry->child_entries[slot];
    ccs = &drc_entry_slot->cc_state;
    /* shouldn't be removing devices we haven't created an fdt for */
    g_assert(ccs->state != CC_STATE_IDLE);
    /* if the device has already been released/isolated by guest, go ahead
     * and remove it now. Otherwise, flag it as pending guest release so it
     * can be removed later
     */
    if (spapr_drc_state_is_releasable(drc_entry_slot)) {
        spapr_drc_state_reset(drc_entry_slot);
    } else {
        if (drc_entry_slot->awaiting_release) {
            fprintf(stderr, "waiting for guest to release the device");
        } else {
            drc_entry_slot->awaiting_release = true;
        }
    }
}

static void spapr_phb_hot_plug(HotplugHandler *plug_handler,
                               DeviceState *plugged_dev, Error **errp)
{
    int slot = PCI_SLOT(PCI_DEVICE(plugged_dev)->devfn);

    spapr_device_hotplug_add(DEVICE(plug_handler), PCI_DEVICE(plugged_dev));
    if (plugged_dev->hotplugged) {
        spapr_pci_hotplug_add_event(DEVICE(plug_handler), slot);
    }
}

static void spapr_phb_hot_unplug(HotplugHandler *plug_handler,
                                 DeviceState *plugged_dev, Error **errp)
{
    int slot = PCI_SLOT(PCI_DEVICE(plugged_dev)->devfn);

    spapr_device_hotplug_remove(DEVICE(plug_handler), PCI_DEVICE(plugged_dev));
    spapr_pci_hotplug_remove_event(DEVICE(plug_handler), slot);
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
        spapr_add_phb_to_drc_table(sphb->buid, 2 /* Unusable */);
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

    pci_setup_iommu(bus, spapr_pci_dma_iommu, sphb);

    pci_bus_set_route_irq_fn(bus, spapr_route_intx_pin_to_irq);

    QLIST_INSERT_HEAD(&spapr->phbs, sphb, list);

    /* Initialize the LSI table */
    for (i = 0; i < PCI_NUM_PINS; i++) {
        uint32_t irq;

        irq = spapr_allocate_lsi(0);
        if (!irq) {
            error_setg(errp, "spapr_allocate_lsi failed");
            return;
        }

        sphb->lsi_table[i].irq = irq;
    }

    /* make sure the platform EPOW sensor is initialized - the
     * guest will probe it when there is a hotplug event.
     */
    spapr->state &= ~(uint32_t)INDICATOR_EPOW_MASK;
    spapr->state |= ENCODE_DRC_STATE(0,
                                     INDICATOR_EPOW_MASK,
                                     INDICATOR_EPOW_SHIFT);

    if (!info->finish_realize) {
        error_setg(errp, "finish_realize not defined");
        return;
    }

    info->finish_realize(sphb, errp);
}

static void spapr_phb_finish_realize(sPAPRPHBState *sphb, Error **errp)
{
    sPAPRTCETable *tcet;

    tcet = spapr_tce_new_table(DEVICE(sphb), sphb->dma_liobn,
                               0,
                               SPAPR_TCE_PAGE_SHIFT,
                               0x40000000 >> SPAPR_TCE_PAGE_SHIFT);
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
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vmstate_spapr_pci_lsi = {
    .name = "spapr_pci/lsi",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField []) {
        VMSTATE_UINT32_EQUAL(irq, struct spapr_pci_lsi),

        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_spapr_pci_msi = {
    .name = "spapr_pci/lsi",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField []) {
        VMSTATE_UINT32(config_addr, struct spapr_pci_msi),
        VMSTATE_UINT32(irq, struct spapr_pci_msi),
        VMSTATE_UINT32(nvec, struct spapr_pci_msi),

        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_spapr_pci = {
    .name = "spapr_pci",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields      = (VMStateField []) {
        VMSTATE_UINT64_EQUAL(buid, sPAPRPHBState),
        VMSTATE_UINT32_EQUAL(dma_liobn, sPAPRPHBState),
        VMSTATE_UINT64_EQUAL(mem_win_addr, sPAPRPHBState),
        VMSTATE_UINT64_EQUAL(mem_win_size, sPAPRPHBState),
        VMSTATE_UINT64_EQUAL(io_win_addr, sPAPRPHBState),
        VMSTATE_UINT64_EQUAL(io_win_size, sPAPRPHBState),
        VMSTATE_STRUCT_ARRAY(lsi_table, sPAPRPHBState, PCI_NUM_PINS, 0,
                             vmstate_spapr_pci_lsi, struct spapr_pci_lsi),
        VMSTATE_STRUCT_ARRAY(msi_table, sPAPRPHBState, SPAPR_MSIX_MAX_DEVS, 0,
                             vmstate_spapr_pci_msi, struct spapr_pci_msi),

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

static void spapr_create_drc_phb_dt_entries(void *fdt, int bus_off, int phb_index)
{
    char char_buf[1024];
    uint32_t int_buf[SPAPR_DRC_PHB_SLOT_MAX + 1];
    uint32_t *entries;
    int i, ret, offset;

    /* ibm,drc-indexes */
    memset(int_buf, 0 , sizeof(int_buf));
    int_buf[0] = SPAPR_DRC_PHB_SLOT_MAX;

    for (i = 1; i <= SPAPR_DRC_PHB_SLOT_MAX; i++) {
        int_buf[i] = SPAPR_DRC_DEV_ID_BASE + (phb_index << 8) + ((i - 1) << 3);
    }

    ret = fdt_setprop(fdt, bus_off, "ibm,drc-indexes", int_buf,
                      sizeof(int_buf));
    if (ret) {
        fprintf(stderr, "error adding 'ibm,drc-indexes' field for PHB FDT");
    }

    /* ibm,drc-power-domains */
    memset(int_buf, 0, sizeof(int_buf));
    int_buf[0] = SPAPR_DRC_PHB_SLOT_MAX;

    for (i = 1; i <= SPAPR_DRC_PHB_SLOT_MAX; i++) {
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
    *entries = SPAPR_DRC_PHB_SLOT_MAX;
    offset = sizeof(*entries);

    for (i = 1; i <= SPAPR_DRC_PHB_SLOT_MAX; i++) {
        offset += sprintf(char_buf + offset, "Slot %d",
                          (phb_index * SPAPR_DRC_PHB_SLOT_MAX) + i - 1);
        char_buf[offset++] = '\0';
    }

    ret = fdt_setprop(fdt, bus_off, "ibm,drc-names", char_buf, offset);
    if (ret) {
        fprintf(stderr, "error adding 'ibm,drc-names' field for PHB FDT");
    }

    /* ibm,drc-types */
    memset(char_buf, 0, sizeof(char_buf));
    entries = (uint32_t *)&char_buf[0];
    *entries = SPAPR_DRC_PHB_SLOT_MAX;
    offset = sizeof(*entries);

    for (i = 0; i < SPAPR_DRC_PHB_SLOT_MAX; i++) {
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
    int_buf[0] = SPAPR_DRC_PHB_SLOT_MAX;

    ret = fdt_setprop(fdt, bus_off, "ibm,indicator-9003", int_buf,
                      sizeof(int_buf));
    if (ret) {
        fprintf(stderr, "error adding 'ibm,indicator-9003' field for PHB FDT");
    }

    /* ibm,sensor-9003 */
    memset(int_buf, 0, sizeof(int_buf));
    int_buf[0] = SPAPR_DRC_PHB_SLOT_MAX;

    ret = fdt_setprop(fdt, bus_off, "ibm,sensor-9003", int_buf,
                      sizeof(int_buf));
    if (ret) {
        fprintf(stderr, "error adding 'ibm,sensor-9003' field for PHB FDT");
    }
}

int spapr_populate_pci_dt(sPAPRPHBState *phb,
                          uint32_t xics_phandle,
                          uint32_t drc_index,
                          void *fdt)
{
    int bus_off, i, j;
    char nodename[256];
    uint32_t bus_range[] = { cpu_to_be32(0), cpu_to_be32(0xff) };
    uint16_t nmsi = 64;
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
    _FDT(fdt_setprop(fdt, bus_off, "ibm,pe-total-#msi", &nmsi, sizeof(nmsi)));

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

    spapr_create_drc_phb_dt_entries(fdt, bus_off, phb->index);
    if (drc_index) {
        _FDT(fdt_setprop(fdt, bus_off, "ibm,my-drc-index", &drc_index,
                         sizeof(drc_index)));
    }

    return 0;
}

void spapr_pci_rtas_init(void)
{
    spapr_rtas_register("read-pci-config", rtas_read_pci_config);
    spapr_rtas_register("write-pci-config", rtas_write_pci_config);
    spapr_rtas_register("ibm,read-pci-config", rtas_ibm_read_pci_config);
    spapr_rtas_register("ibm,write-pci-config", rtas_ibm_write_pci_config);
    if (msi_supported) {
        spapr_rtas_register("ibm,query-interrupt-source-number",
                            rtas_ibm_query_interrupt_source_number);
        spapr_rtas_register("ibm,change-msi", rtas_ibm_change_msi);
    }
    spapr_rtas_register("set-indicator", rtas_set_indicator);
    spapr_rtas_register("set-power-level", rtas_set_power_level);
    spapr_rtas_register("get-power-level", rtas_get_power_level);
    spapr_rtas_register("get-sensor-state", rtas_get_sensor_state);
    spapr_rtas_register("ibm,configure-connector",
                        rtas_ibm_configure_connector);
}

static void spapr_pci_register_types(void)
{
    type_register_static(&spapr_phb_info);
}

type_init(spapr_pci_register_types)
