/*
 * QEMU SPAPR PCI Resource Allocator
 *
 * Copyright IBM Corp. 2017
 *
 * Authors:
 *  Michael Roth      <mdroth@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "hw/ppc/spapr_pci_resources.h"
//#include "qapi/error.h"
#include "qemu/error-report.h"

/*
 * This code mimics SLOF's scheme for identifying the next available
 * offset in a IO window to map a device BAR, which is to simply track
 * the offset following the most recently mapped BAR for that range and
 * size-aligning it.
 *
 * FIXME: the below should be rolled into some sort of loop construct
 * in the code that actually does the assignments until
 * verify_assigned_addr() is successful. Or maybe just warn for now to
 * maintain initial parody with SLOF? How likely/bad is this scenario?
 * FIXME: how would we even detect this? we'd need to enable the device
 * with no BARs mapped to scan for legacy vga, since doing actual BAR
 * assignment might obscure the legacy regions depending on priority
 * and it's not clear there's a way to disambiguate.
 *
 * Callers should call verify_assigned_addr() after proceeding with the
 * actual mapping to detect of there's a collision, since it's possible
 * regions may have been mapped in from elsewhere. One such example are
 * legacy vga regions, which may get mapped in under the covers as part
 * of BAR assignment / enablement for a vga device.
 *
 * We go a little bit beyond that and do some checks to ensure none of
 * our assignments overlap a region which may have been mapped in from
 * elsewhere in QEMU. We consider this non-fatal for now and proceed
 * with the mapping, since this is SLOF's behavior.
 *
 * NOTE: Using this scheme we can get fairly large gaps of unused ranges
 * due to size alignment. We could pack regions more efficiently by
 * examining any free range and using it if it's sufficiently large,
 * but there risk with relying purely on checking for region overlaps, since
 * a PAPR-compliant guest is still welcome to temporarily disable a device,
 * which would leave it's regions unmapped for a period of time, leading
 * to a potential collision if it is enabled later. Tracking our allocations
 * outside of scanning mapped MRs avoids this scenario. The upshot is that
 * this is how SLOF currently does it anyway, so this makes it easier to
 * identify potential regressions in the assignment code.
 */

typedef struct sPAPRPCIRegion {
    sPAPRPCIResourceType rtype;
    MemoryRegion mr;
    hwaddr addr;
    uint64_t size;
    char *name;
    gpointer key;
} sPAPRPCIRegion;

typedef struct sPAPRPCIRange {
    bool enabled;
    MemoryRegion *address_space;
    hwaddr start;
    uint64_t size;
    hwaddr search_start;
} sPAPRPCIRange;

struct sPAPRPCIResources {
    AddressSpace iospace_as;
    AddressSpace memspace_as;
    MemoryRegion iospace;
    MemoryRegion memspace;
    sPAPRPCIRange range[SPAPR_PCI_RESOURCE_TYPE_MAX];
    GHashTable *regions;
};

#define ALIGN(a, s) (((a) + ((s) - 1)) & ~((s) - 1))

static hwaddr find_next_aligned_region(MemoryRegion *address_space,
                                       hwaddr start, hwaddr end, uint64_t size)
{
    hwaddr addr = ALIGN(start, size);

    do {
        MemoryRegionSection mrs = memory_region_find(address_space, addr, size);
        error_report("marker 0: addr: %lx, size: %lx", addr, size);

        if (mrs.mr == NULL) {
            return addr;
        }

        error_report("overlap region %p/%s @ %lx (search at %lx), region size: >%lx<",
                     mrs.mr, mrs.mr->name,
                     mrs.offset_within_address_space - mrs.offset_within_region,
                     addr, memory_region_size(mrs.mr));

        addr = ALIGN(mrs.offset_within_address_space
                     - mrs.offset_within_region
                     + memory_region_size(mrs.mr),
                     size);

        error_report("next_addr: %lx", addr);
        memory_region_unref(mrs.mr);
    } while (addr + size < end);

    return HWADDR_MAX;
}

#define REGION_KEY_TYPE_SHIFT 30
#define REGION_KEY_TYPE_MASK (1ULL << REGION_KEY_TYPE_SHIFT)

typedef enum RegionKeyType {
    REGION_KEY_TYPE_RESERVED = 0,            /* generic reserved region */
    REGION_KEY_TYPE_BAR      = 1, /* BAR region tied to a device */
} RegionKeyType;

static gpointer region_key(RegionKeyType key_type, uint32_t id)
{
    g_assert_cmpint(id & REGION_KEY_TYPE_MASK, ==, 0);
    return GUINT_TO_POINTER((key_type << REGION_KEY_TYPE_SHIFT) | id);
}

static gpointer region_key_bar(uint32_t bdf, uint32_t bar)
{
    return region_key(REGION_KEY_TYPE_BAR, (bdf << 8) | bar);
}

static void reserve_region(sPAPRPCIResources *res,
                           sPAPRPCIResourceType rtype,
                           hwaddr addr, uint64_t size,
                           gpointer key)
{
    sPAPRPCIRegion *r;
    sPAPRPCIRange *range;

    range = &res->range[rtype];

    g_assert_cmpint(rtype, <, SPAPR_PCI_RESOURCE_TYPE_MAX);
    g_assert_true(range->enabled);
    g_assert_false(g_hash_table_contains(res->regions, key));

    r = g_new0(sPAPRPCIRegion, 1);
    r->rtype = rtype;
    r->addr = addr;
    r->size = size;
    r->key = key;
    r->name = g_strdup_printf("sPAPRPCIResource:%xh", GPOINTER_TO_UINT(r->key));

    /* FIXME: let MRs manage book-keeping/searches for now, but either
     * extricate AS from the ones QEMU uses for actually guest regions
     * or roll some simpler/locally-scoped implementation
     */
    memory_region_init_io(&r->mr, NULL, NULL, NULL, r->name, r->size);
    memory_region_add_subregion(range->address_space, r->addr, &r->mr);
    g_assert(memory_region_is_mapped(&r->mr));
    g_hash_table_insert(res->regions, key, r);
}

void spapr_pci_resources_reserve_region(sPAPRPCIResources *res,
                                        sPAPRPCIResourceType rtype,
                                        hwaddr addr, uint64_t size,
                                        uint32_t id)
{
    reserve_region(res, rtype, addr, size,
                   region_key(REGION_KEY_TYPE_RESERVED, id));
}

hwaddr spapr_pci_resources_request_bar_region(sPAPRPCIResources *res,
                                              sPAPRPCIResourceType rtype,
                                              uint64_t size, bool hotplugged,
                                              uint32_t bdf, int bar)
{
    sPAPRPCIRange *range;
    hwaddr addr, start_addr;
    gpointer key = region_key_bar(bdf, bar);

    range = &res->range[rtype];
    /*
     * slof uses a running offset for boot-time assignments. for hotplug/unplug
     * we rescan from the start of range to allow unplugged regions to be re-used
     */
    start_addr = hotplugged ? range->start : range->search_start;

    /* slof avoids zero bars */
    if (start_addr == 0) {
        start_addr = size;
    }

    addr = find_next_aligned_region(range->address_space, start_addr,
                                    range->start + range->size, size);
    if (addr == HWADDR_MAX) {
        error_report("%s: can't find free region for device %x, bar %d",
                     memory_region_name(range->address_space), bdf, bar);
        return HWADDR_MAX;
    }

    reserve_region(res, rtype, addr, size, key);
    range->search_start = addr + size;

    return addr;
}

static gboolean region_release_fn(gpointer key, gpointer value, gpointer opaque)
{
    sPAPRPCIResources *res = opaque;
    sPAPRPCIRegion *r = value;
    sPAPRPCIRange *range;

    g_assert_nonnull(r);
    g_assert_nonnull(res);

    range = &res->range[r->rtype];
    memory_region_del_subregion(range->address_space, &r->mr);
    g_free(r->name);
    g_free(r);

    return true;
}

void spapr_pci_resources_release_region(sPAPRPCIResources *res,
                                        uint32_t id)
{
    gpointer key = region_key(REGION_KEY_TYPE_RESERVED, id);
    sPAPRPCIRegion *r = g_hash_table_lookup(res->regions, key);

    g_hash_table_remove(res->regions, key);
    region_release_fn(key, r, res);
}

void spapr_pci_resources_release_bar_region(sPAPRPCIResources *res,
                                            uint32_t bdf, int bar)
{
    gpointer key = region_key_bar(bdf, bar);
    sPAPRPCIRegion *r = g_hash_table_lookup(res->regions, key);

    g_hash_table_remove(res->regions, key);
    region_release_fn(key, r, res);
}

static void init_range(sPAPRPCIRange *range, MemoryRegion *as,
                       hwaddr address_space_offset, uint64_t size)
{
    g_assert(!range->enabled);

    range->address_space = as;
    range->start = address_space_offset;
    range->size = size;
    range->search_start = range->start;
    range->enabled = true;
}

void spapr_pci_resources_add_mem_range(sPAPRPCIResources *res,
                                       sPAPRPCIResourceType rtype,
                                       hwaddr address_space_offset,
                                       uint64_t size)
{
    g_assert(rtype != SPAPR_PCI_RESOURCE_TYPE_IO);

    init_range(&res->range[rtype], &res->memspace,
                address_space_offset, size);
}

void spapr_pci_resources_add_io_range(sPAPRPCIResources *res,
                                      sPAPRPCIResourceType rtype,
                                      hwaddr address_space_offset,
                                      uint64_t size)
{
    g_assert(rtype == SPAPR_PCI_RESOURCE_TYPE_IO);

    init_range(&res->range[rtype], &res->iospace,
                address_space_offset, size);
}

void spapr_pci_resources_reset(sPAPRPCIResources *res)
{
    g_hash_table_foreach_remove(res->regions, region_release_fn, res);
}

sPAPRPCIResources *spapr_pci_resources_new(const char *phb_name,
                                           uint64_t iospace_size,
                                           uint64_t memspace_size)
{
    char name[128];
    sPAPRPCIResources *res = g_new0(sPAPRPCIResources, 1);

    snprintf(name, 128, "%s.io-assigned", phb_name);
    memory_region_init(&res->iospace, NULL, name, iospace_size);
    snprintf(name, 128, "%s.io-assigned.as", phb_name);
    address_space_init(&res->iospace_as, &res->iospace, name);

    snprintf(name, 128, "%s.mmio-assigned", phb_name);
    memory_region_init(&res->memspace, NULL, name, memspace_size);
    snprintf(name, 128, "%s.mmio-assigned.as", phb_name);
    address_space_init(&res->memspace_as, &res->memspace, name);

    res->regions = g_hash_table_new(g_direct_hash, g_direct_equal);

    return res;
}
