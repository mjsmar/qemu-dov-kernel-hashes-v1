/*
 * QEMU SPAPR Dynamic Reconfiguration Connector Implementation
 *
 * Copyright IBM Corp. 2014
 *
 * Authors:
 *  Michael Roth      <mdroth@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "hw/ppc/spapr_drc.h"
#include "qom/object.h"
#include "hw/qdev.h"
#include "qapi/visitor.h"
#include "qemu/error-report.h"

/* #define DEBUG_SPAPR_DRC */

#ifdef DEBUG_SPAPR_DRC
#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, fmt, ## __VA_ARGS__); } while (0)
#define DPRINTFN(fmt, ...) \
    do { DPRINTF(fmt, ## __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#define DPRINTFN(fmt, ...) \
    do { } while (0)
#endif

#define DRC_CONTAINER_PATH "/dr-connector"
#define DRC_INDEX_TYPE_SHIFT 28
#define DRC_INDEX_ID_MASK (~(~0 << DRC_INDEX_TYPE_SHIFT))

static sPAPRDRConnectorTypeShift get_type_shift(sPAPRDRConnectorType type)
{
    uint32_t shift = 0;

    /* make sure this isn't SPAPR_DR_CONNECTOR_TYPE_ANY, or some
     * other wonky value.
     */
    g_assert(is_power_of_2(type));

    while (type != (1 << shift)) {
        shift++;
    }
    return shift;
}

static uint32_t get_index(sPAPRDRConnector *drc)
{
    /* no set format for a drc index: it only needs to be globally
     * unique. this is how we encode the DRC type on bare-metal
     * however, so might as well do that here
     */
    return (get_type_shift(drc->type) << DRC_INDEX_TYPE_SHIFT) |
            (drc->id & DRC_INDEX_ID_MASK);
}

static int set_isolation_state(sPAPRDRConnector *drc,
                               sPAPRDRIsolationState state)
{
    sPAPRDRConnectorClass *drck = SPAPR_DR_CONNECTOR_GET_CLASS(drc);

    DPRINTFN("drc: %x, set_isolation_state: %x", get_index(drc), state);

    drc->isolation_state = state;

    if (drc->isolation_state == SPAPR_DR_ISOLATION_STATE_ISOLATED) {
        /* if we're awaiting release, but still in an unconfigured state,
         * it's likely the guest is still in the process of configuring
         * the device and is transitioning the devices to an ISOLATED
         * state as a part of that process. so we only complete the
         * removal when this transition happens for a device in a
         * configured state, as suggested by the state diagram from
         * PAPR+ 2.7, 13.4
         */
        if (drc->awaiting_release && drc->ccs.configured) {
            DPRINTFN("finalizing device removal");
            drck->detach(drc, DEVICE(drc->dev), drc->detach_cb,
                         drc->detach_cb_opaque, NULL);
        } else if (!drc->ccs.configured) {
            DPRINTFN("deferring device removal on unconfigured device\n");
        }
        drc->ccs.fdt_offset = drc->ccs.fdt_start_offset;
        drc->ccs.fdt_depth = 0;
        drc->ccs.configured = false;
    }

    return 0;
}

static int set_indicator_state(sPAPRDRConnector *drc,
                               sPAPRDRIndicatorState state)
{
    DPRINTFN("drc: %x, set_indicator_state: %x", get_index(drc), state);
    drc->indicator_state = state;
    return 0;
}

static int set_allocation_state(sPAPRDRConnector *drc,
                                sPAPRDRAllocationState state)
{
    DPRINTFN("drc: %x, set_allocation_state: %x", get_index(drc), state);
    drc->allocation_state = state;
    return 0;
}

static uint32_t get_type(sPAPRDRConnector *drc)
{
    return drc->type;
}

static const char *get_name(sPAPRDRConnector *drc)
{
    return drc->name;
}

/*
 * dr-entity-sense sensor value
 * returned via get-sensor-state RTAS calls
 * as expected by state diagram in PAPR+ 2.7, 13.4
 * based on the current allocation/indicator/power states
 * for the DR connector.
 */
static sPAPRDREntitySense entity_sense(sPAPRDRConnector *drc)
{
    if (drc->dev) {
        if (drc->type != SPAPR_DR_CONNECTOR_TYPE_PCI &&
            drc->allocation_state == SPAPR_DR_ALLOCATION_STATE_UNUSABLE) {
            /* for logical DR, we return a state of UNUSABLE
             * iff the allocation state UNUSABLE.
             * Otherwise, report the state as USABLE/PRESENT,
             * as we would for PCI.
             */
            return SPAPR_DR_ENTITY_SENSE_UNUSABLE;
        }

        /* this assumes all PCI devices are assigned to
         * a 'live insertion' power domain, where QEMU
         * manages power state automatically as opposed
         * to the guest. present, non-PCI resources are
         * unaffected by power state.
         */
        return SPAPR_DR_ENTITY_SENSE_PRESENT;
    }

    if (drc->type == SPAPR_DR_CONNECTOR_TYPE_PCI) {
        /* PCI devices, and only PCI devices, use EMPTY
         * in cases where we'd otherwise use UNUSABLE
         */
        return SPAPR_DR_ENTITY_SENSE_EMPTY;
    }
    return SPAPR_DR_ENTITY_SENSE_UNUSABLE;
}

static sPAPRDRCCResponse configure_connector_common(sPAPRDRCCState *ccs,
                            char **name, const struct fdt_property **prop,
                            int *prop_len)
{
    sPAPRDRCCResponse resp = SPAPR_DR_CC_RESPONSE_CONTINUE;
    int fdt_offset_next;

    *name = NULL;
    *prop = NULL;
    *prop_len = 0;

    if (!ccs->fdt) {
        return SPAPR_DR_CC_RESPONSE_ERROR;
    }

    while (resp == SPAPR_DR_CC_RESPONSE_CONTINUE) {
        const char *name_cur;
        uint32_t tag;
        int name_cur_len;

        tag = fdt_next_tag(ccs->fdt, ccs->fdt_offset, &fdt_offset_next);
        switch (tag) {
        case FDT_BEGIN_NODE:
            ccs->fdt_depth++;
            name_cur = fdt_get_name(ccs->fdt, ccs->fdt_offset, &name_cur_len);
            *name = g_strndup(name_cur, name_cur_len);
            resp = SPAPR_DR_CC_RESPONSE_NEXT_CHILD;
            break;
        case FDT_END_NODE:
            ccs->fdt_depth--;
            if (ccs->fdt_depth == 0) {
                resp = SPAPR_DR_CC_RESPONSE_SUCCESS;
                ccs->configured = true;
            } else {
                resp = SPAPR_DR_CC_RESPONSE_PREV_PARENT;
            }
            break;
        case FDT_PROP:
            *prop = fdt_get_property_by_offset(ccs->fdt, ccs->fdt_offset,
                                               prop_len);
            name_cur = fdt_string(ccs->fdt, fdt32_to_cpu((*prop)->nameoff));
            *name = g_strdup(name_cur);
            resp = SPAPR_DR_CC_RESPONSE_NEXT_PROPERTY;
            break;
        case FDT_END:
            resp = SPAPR_DR_CC_RESPONSE_ERROR;
            break;
        default:
            ccs->fdt_offset = fdt_offset_next;
        }
    }

    ccs->fdt_offset = fdt_offset_next;
    return resp;
}

static sPAPRDRCCResponse configure_connector(sPAPRDRConnector *drc,
                                             char **name,
                                             const struct fdt_property **prop,
                                             int *prop_len)
{
    return configure_connector_common(&drc->ccs, name, prop, prop_len);
}

static void prop_get_index(Object *obj, Visitor *v, void *opaque,
                                  const char *name, Error **errp)
{
    sPAPRDRConnector *drc = SPAPR_DR_CONNECTOR(obj);
    sPAPRDRConnectorClass *drck = SPAPR_DR_CONNECTOR_GET_CLASS(drc);
    uint32_t value = (uint32_t)drck->get_index(drc);
    visit_type_uint32(v, &value, name, errp);
}

static void prop_get_type(Object *obj, Visitor *v, void *opaque,
                          const char *name, Error **errp)
{
    sPAPRDRConnector *drc = SPAPR_DR_CONNECTOR(obj);
    sPAPRDRConnectorClass *drck = SPAPR_DR_CONNECTOR_GET_CLASS(drc);
    uint32_t value = (uint32_t)drck->get_type(drc);
    visit_type_uint32(v, &value, name, errp);
}

static char *prop_get_name(Object *obj, Error **errp)
{
    sPAPRDRConnector *drc = SPAPR_DR_CONNECTOR(obj);
    sPAPRDRConnectorClass *drck = SPAPR_DR_CONNECTOR_GET_CLASS(drc);
    return g_strdup(drck->get_name(drc));
}

static void prop_get_entity_sense(Object *obj, Visitor *v, void *opaque,
                                  const char *name, Error **errp)
{
    sPAPRDRConnector *drc = SPAPR_DR_CONNECTOR(obj);
    sPAPRDRConnectorClass *drck = SPAPR_DR_CONNECTOR_GET_CLASS(drc);
    uint32_t value = (uint32_t)drck->entity_sense(drc);
    visit_type_uint32(v, &value, name, errp);
}

static void prop_get_fdt(Object *obj, Visitor *v, void *opaque,
                        const char *name, Error **errp)
{
    sPAPRDRConnector *drc = SPAPR_DR_CONNECTOR(obj);
    sPAPRDRCCState ccs = { 0 };
    sPAPRDRCCResponse resp;

    ccs.fdt = drc->ccs.fdt;
    ccs.fdt_offset = ccs.fdt_start_offset = drc->ccs.fdt_start_offset;

    do {
        char *name = NULL;
        const struct fdt_property *prop = NULL;
        int prop_len;

        resp = configure_connector_common(&ccs, &name, &prop, &prop_len);

        switch (resp) {
        case SPAPR_DR_CC_RESPONSE_NEXT_CHILD:
            visit_start_struct(v, NULL, NULL, name, 0, NULL);
            break;
        case SPAPR_DR_CC_RESPONSE_PREV_PARENT:
            visit_end_struct(v, NULL);
            break;
        case SPAPR_DR_CC_RESPONSE_NEXT_PROPERTY: {
            int i;
            visit_start_list(v, name, NULL);
            for (i = 0; i < prop_len; i++) {
                visit_type_uint8(v, (uint8_t *)&prop->data[i], NULL, NULL);
            }
            visit_end_list(v, NULL);
            break;
        }
        default:
            resp = SPAPR_DR_CC_RESPONSE_SUCCESS;
            break;
        }

        g_free(name);
    } while (resp != SPAPR_DR_CC_RESPONSE_SUCCESS &&
             resp != SPAPR_DR_CC_RESPONSE_ERROR);

    g_assert(resp != SPAPR_DR_CC_RESPONSE_ERROR);
}

static void attach(sPAPRDRConnector *drc, DeviceState *d, void *fdt,
                   int fdt_start_offset, bool coldplug, Error **errp)
{
    DPRINTFN("drc: %x, attach", get_index(drc));

    if (drc->isolation_state != SPAPR_DR_ISOLATION_STATE_ISOLATED) {
        error_setg(errp, "an attached device is still awaiting release");
        return;
    }
    g_assert(drc->allocation_state == SPAPR_DR_ALLOCATION_STATE_UNUSABLE);
    g_assert(fdt || coldplug);

    /* NOTE: setting initial isolation state to UNISOLATED means we can't
     * detach unless guest has a userspace/kernel that moves this state
     * back to ISOLATED in response to an unplug event, or this is done
     * manually by the admin prior. if we force things while the guest
     * may be accessing the device, we can easily crash the guest, so we
     * we defer completion of removal in such cases to the reset() hook.
     */
    drc->isolation_state = SPAPR_DR_ISOLATION_STATE_UNISOLATED;
    drc->allocation_state = SPAPR_DR_ALLOCATION_STATE_USABLE;
    drc->indicator_state = SPAPR_DR_INDICATOR_STATE_ACTIVE;

    drc->dev = d;
    drc->ccs.fdt = fdt;
    drc->ccs.fdt_offset = drc->ccs.fdt_start_offset = fdt_start_offset;
    drc->ccs.fdt_depth = 0;
    drc->ccs.configured = false;

    object_property_add_link(OBJECT(drc), "device",
                             object_get_typename(OBJECT(drc->dev)),
                             (Object **)(&drc->dev),
                             NULL, 0, NULL);
}

static void detach(sPAPRDRConnector *drc, DeviceState *d,
                   spapr_drc_detach_cb *detach_cb,
                   void *detach_cb_opaque, Error **errp)
{
    DPRINTFN("drc: %x, detach", get_index(drc));

    drc->detach_cb = detach_cb;
    drc->detach_cb_opaque = detach_cb_opaque;

    if (drc->isolation_state != SPAPR_DR_ISOLATION_STATE_ISOLATED) {
        DPRINTFN("awaiting transition to isolated state before removal");
        drc->awaiting_release = true;
        return;
    }

    drc->allocation_state = SPAPR_DR_ALLOCATION_STATE_UNUSABLE;
    drc->indicator_state = SPAPR_DR_INDICATOR_STATE_INACTIVE;

    if (drc->detach_cb) {
        drc->detach_cb(drc->dev, drc->detach_cb_opaque);
    }

    drc->awaiting_release = false;
    g_free(drc->ccs.fdt);
    drc->ccs.fdt = NULL;
    drc->ccs.fdt_offset = drc->ccs.fdt_start_offset = drc->ccs.fdt_depth = 0;
    object_property_del(OBJECT(drc), "device", NULL);
    drc->dev = NULL;
    drc->detach_cb = NULL;
    drc->detach_cb_opaque = NULL;
}

static bool release_pending(sPAPRDRConnector *drc)
{
    return drc->awaiting_release;
}

static void reset(DeviceState *d)
{
    sPAPRDRConnector *drc = SPAPR_DR_CONNECTOR(d);
    sPAPRDRConnectorClass *drck = SPAPR_DR_CONNECTOR_GET_CLASS(drc);

    DPRINTFN("drc reset: %x", drck->get_index(drc));
    /* immediately upon reset we can safely assume DRCs whose devices are pending
     * removal can be safely removed, and that they will subsequently be left in
     * an ISOLATED state. move the DRC to this state in these cases (which will in
     * turn complete any pending device removals)
     */
    if (drc->awaiting_release) {
        drck->set_isolation_state(drc, SPAPR_DR_ISOLATION_STATE_ISOLATED);
        /* generally this should also finalize the removal, but if the device
         * hasn't yet been configured we normally defer removal under the
         * assumption that this transition is taking place as part of device
         * configuration. so check if we're still waiting after this, and
         * force removal if we are
         */
        if (drc->awaiting_release) {
            drck->detach(drc, DEVICE(drc->dev), drc->detach_cb,
                         drc->detach_cb_opaque, NULL);
        }
    }
}

static void realize(DeviceState *d, Error **errp)
{
    sPAPRDRConnector *drc = SPAPR_DR_CONNECTOR(d);
    sPAPRDRConnectorClass *drck = SPAPR_DR_CONNECTOR_GET_CLASS(drc);
    Object *root_container;
    char link_name[256];
    gchar *child_name;
    Error *err = NULL;

    DPRINTFN("drc realize: %x", drck->get_index(drc));
    /* NOTE: we do this as part of realize/unrealize due to the fact
     * that the guest will communicate with the DRC via RTAS calls
     * referencing the global DRC index. By unlinking the DRC
     * from DRC_CONTAINER_PATH/<drc_index> we effectively make it
     * inaccessible by the guest, since lookups rely on this path
     * existing in the composition tree
     */
    root_container = container_get(object_get_root(), DRC_CONTAINER_PATH);
    snprintf(link_name, sizeof(link_name), "%x", drck->get_index(drc));
    child_name = object_get_canonical_path_component(OBJECT(drc));
    DPRINTFN("drc child name: %s", child_name);
    object_property_add_alias(root_container, link_name,
                              drc->owner, child_name, &err);
    if (err) {
        error_report("%s", error_get_pretty(err));
        error_free(err);
        object_unref(OBJECT(drc));
    }
    DPRINTFN("drc realize complete");
}

static void unrealize(DeviceState *d, Error **errp)
{
    sPAPRDRConnector *drc = SPAPR_DR_CONNECTOR(d);
    sPAPRDRConnectorClass *drck = SPAPR_DR_CONNECTOR_GET_CLASS(drc);
    Object *root_container;
    char name[256];
    Error *err = NULL;

    DPRINTFN("drc unrealize: %x", drck->get_index(drc));
    root_container = container_get(object_get_root(), DRC_CONTAINER_PATH);
    snprintf(name, sizeof(name), "%x", drck->get_index(drc));
    object_property_del(root_container, name, &err);
    if (err) {
        error_report("%s", error_get_pretty(err));
        error_free(err);
        object_unref(OBJECT(drc));
    }
}

sPAPRDRConnector *spapr_dr_connector_new(Object *owner,
                                         sPAPRDRConnectorType type,
                                         uint32_t id)
{
    sPAPRDRConnector *drc =
        SPAPR_DR_CONNECTOR(object_new(TYPE_SPAPR_DR_CONNECTOR));

    g_assert(type);

    drc->type = type;
    drc->id = id;
    drc->owner = owner;
    object_property_add_child(owner, "dr-connector[*]", OBJECT(drc), NULL);
    object_property_set_bool(OBJECT(drc), true, "realized", NULL);

    /* human-readable name for a DRC to encode into the DT
     * description. this is mainly only used within a guest in place
     * of the unique DRC index.
     *
     * in the case of VIO/PCI devices, it corresponds to a
     * "location code" that maps a logical device/function (DRC index)
     * to a physical (or virtual in the case of VIO) location in the
     * system by chaining together the "location label" for each
     * encapsulating component.
     *
     * since this is more to do with diagnosing physical hardware
     * issues than guest compatibility, we choose location codes/DRC
     * names that adhere to the documented format, but avoid encoding
     * the entire topology information into the label/code, instead
     * just using the location codes based on the labels for the
     * endpoints (VIO/PCI adaptor connectors), which is basically
     * just "C" followed by an integer ID.
     *
     * DRC names as documented by PAPR+ v2.7, 13.5.2.4
     * location codes as documented by PAPR+ v2.7, 12.3.1.5
     */
    switch (drc->type) {
    case SPAPR_DR_CONNECTOR_TYPE_CPU:
        drc->name = g_strdup_printf("CPU %d", id);
        break;
    case SPAPR_DR_CONNECTOR_TYPE_PHB:
        drc->name = g_strdup_printf("PHB %d", id);
        break;
    case SPAPR_DR_CONNECTOR_TYPE_VIO:
    case SPAPR_DR_CONNECTOR_TYPE_PCI:
        drc->name = g_strdup_printf("C%d", id);
        break;
    case SPAPR_DR_CONNECTOR_TYPE_LMB:
        drc->name = g_strdup_printf("LMB %d", id);
        break;
    default:
        g_assert(false);
    }

    return drc;
}

static void spapr_dr_connector_instance_init(Object *obj)
{
    sPAPRDRConnector *drc = SPAPR_DR_CONNECTOR(obj);

    object_property_add_uint32_ptr(obj, "isolation-state",
                                   &drc->isolation_state, NULL);
    object_property_add_uint32_ptr(obj, "indicator-state",
                                   &drc->indicator_state, NULL);
    object_property_add_uint32_ptr(obj, "allocation-state",
                                   &drc->allocation_state, NULL);
    object_property_add_uint32_ptr(obj, "id", &drc->id, NULL);
    object_property_add(obj, "index", "uint32", prop_get_index,
                        NULL, NULL, NULL, NULL);
    object_property_add(obj, "connector_type", "uint32", prop_get_type,
                        NULL, NULL, NULL, NULL);
    object_property_add_str(obj, "name", prop_get_name, NULL, NULL);
    object_property_add(obj, "entity-sense", "uint32", prop_get_entity_sense,
                        NULL, NULL, NULL, NULL);
    object_property_add(obj, "fdt", "struct", prop_get_fdt,
                        NULL, NULL, NULL, NULL);
}

static void spapr_dr_connector_class_init(ObjectClass *k, void *data)
{
    DeviceClass *dk = DEVICE_CLASS(k);
    sPAPRDRConnectorClass *drck = SPAPR_DR_CONNECTOR_CLASS(k);

    dk->reset = reset;
    dk->realize = realize;
    dk->unrealize = unrealize;
    drck->set_isolation_state = set_isolation_state;
    drck->set_indicator_state = set_indicator_state;
    drck->set_allocation_state = set_allocation_state;
    drck->get_index = get_index;
    drck->get_type = get_type;
    drck->get_name = get_name;
    drck->entity_sense = entity_sense;
    drck->configure_connector = configure_connector;
    drck->attach = attach;
    drck->detach = detach;
    drck->release_pending = release_pending;
}

static const TypeInfo spapr_dr_connector_info = {
    .name          = TYPE_SPAPR_DR_CONNECTOR,
    .parent        = TYPE_DEVICE,
    .instance_size = sizeof(sPAPRDRConnector),
    .instance_init = spapr_dr_connector_instance_init,
    .class_size    = sizeof(sPAPRDRConnectorClass),
    .class_init    = spapr_dr_connector_class_init,
};

static void spapr_drc_register_types(void)
{
    type_register_static(&spapr_dr_connector_info);
}

type_init(spapr_drc_register_types)

/* helper functions for external users */

sPAPRDRConnector *spapr_dr_connector_by_index(uint32_t index)
{
    Object *obj;
    char name[256];

    snprintf(name, sizeof(name), "%s/%x", DRC_CONTAINER_PATH, index);
    obj = object_resolve_path(name, NULL);

    return !obj ? NULL : SPAPR_DR_CONNECTOR(obj);
}

sPAPRDRConnector *spapr_dr_connector_by_id(sPAPRDRConnectorType type,
                                           uint32_t id)
{
    return spapr_dr_connector_by_index(
            (get_type_shift(type) << DRC_INDEX_TYPE_SHIFT) |
            (id & DRC_INDEX_ID_MASK));
}
