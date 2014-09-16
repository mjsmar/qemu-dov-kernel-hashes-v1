/*
 * QEMU SPAPR Dynamic Resource Connector Implementation
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

#define DRC_CONTAINER_PATH "/dr-connector"
#define DRC_INDEX_TYPE_SHIFT 28

static int set_isolation_state(sPAPRDRConnector *drc,
                               sPAPRDRIsolationState state)
{
    sPAPRDRConnectorClass *drck = SPAPR_DR_CONNECTOR_GET_CLASS(drc);

    g_warning("set_isolation_state");
    drc->isolation_state = state;
    if (drc->awaiting_release &&
        drc->isolation_state == SPAPR_DR_ISOLATION_STATE_ISOLATED) {
        drck->detach(drc, DEVICE(drc->dev), drc->detach_cb,
                     drc->detach_cb_opaque);
    }
    return 0;
}

static int set_indicator_state(sPAPRDRConnector *drc,
                               sPAPRDRIndicatorState state)
{
    g_warning("set_indicator_state");
    drc->indicator_state = state;
    return 0;
}

static int set_allocation_state(sPAPRDRConnector *drc,
                                sPAPRDRAllocationState state)
{
    g_warning("set_allocation_state");
    drc->indicator_state = state;
    return 0;
}

static uint32_t get_index(sPAPRDRConnector *drc)
{
    /* no set format for a drc index: it only needs to be globally
     * unique. this is how we encode the DRC type on bare-metal
     * however, so might as well do that here
     */
    return drc->type << DRC_INDEX_TYPE_SHIFT | drc->id;
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
    /* FIXME: check this logic again. accomadate _UNUSABLE for logical dr,
     * isolation state almost surely should affect our response
     */
    if (drc->dev) {
        return SPAPR_DR_ENTITY_SENSE_PRESENT;
    }

    return SPAPR_DR_ENTITY_SENSE_EMPTY;
}

static sPAPRDRCCResponse configure_connector_common(sPAPRDRCCState *ccs,
                            char **prop_name, const struct fdt_property **prop,
                            int *prop_len)
{
    sPAPRDRCCResponse resp = SPAPR_DR_CC_RESPONSE_CONTINUE;
    int fdt_offset_next;

    *prop_name = NULL;
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
            *prop_name = g_strndup(name_cur, name_cur_len);
            resp = SPAPR_DR_CC_RESPONSE_NEXT_CHILD;
            break;
        case FDT_END_NODE:
            ccs->fdt_depth--;
            if (ccs->fdt_depth == 0) {
                resp = SPAPR_DR_CC_RESPONSE_SUCCESS;
            } else {
                resp = SPAPR_DR_CC_RESPONSE_PREV_PARENT;
            }
            break;
        case FDT_PROP:
            *prop = fdt_get_property_by_offset(ccs->fdt, ccs->fdt_offset,
                                               prop_len);
            name_cur = fdt_string(ccs->fdt, fdt32_to_cpu((*prop)->nameoff));
            *prop_name = g_strdup(name_cur);
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
    printf("configure_connector: returning %i, offset: %i\n", resp, ccs->fdt_offset);
    return resp;
}

static sPAPRDRCCResponse configure_connector(sPAPRDRConnector *drc,
                                             char **prop_name,
                                             const struct fdt_property **prop,
                                             int *prop_len)
{
    return configure_connector_common(&drc->ccs, prop_name, prop, prop_len);
}

static void prop_get_index(Object *obj, Visitor *v, void *opaque,
                                  const char *name, Error **errp)
{
    sPAPRDRConnector *drc = SPAPR_DR_CONNECTOR(obj);
    sPAPRDRConnectorClass *drck = SPAPR_DR_CONNECTOR_GET_CLASS(drc);
    uint32_t value = (uint32_t)drck->get_index(drc);
    visit_type_uint32(v, &value, name, errp);
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
        char *prop_name = NULL;
        const struct fdt_property *prop = NULL;
        int prop_len;

        resp = configure_connector_common(&ccs, &prop_name, &prop, &prop_len);

        switch (resp) {
        case SPAPR_DR_CC_RESPONSE_NEXT_CHILD:
            visit_start_struct(v, NULL, NULL, prop_name, 0, NULL);
            break;
        case SPAPR_DR_CC_RESPONSE_PREV_PARENT:
            visit_end_struct(v, NULL);
            break;
        case SPAPR_DR_CC_RESPONSE_NEXT_PROPERTY: {
            int i;
            visit_start_list(v, prop_name, NULL);
            for (i = 0; i < prop_len; i++) {
                g_warning("prop: %s, i: %d", prop_name, i);
                visit_type_uint8(v, (uint8_t *)&prop->data[i], NULL, NULL);
            }
            visit_end_list(v, NULL);
            break;
        }
        default:
            g_warning("unhandled response: resp");
            resp = SPAPR_DR_CC_RESPONSE_SUCCESS;
            break;
        }

        g_free(prop_name);
    } while (resp != SPAPR_DR_CC_RESPONSE_SUCCESS &&
             resp != SPAPR_DR_CC_RESPONSE_ERROR);
}

static void attach(sPAPRDRConnector *drc, DeviceState *d, void *fdt,
                   int fdt_start_offset, bool coldplug)
{
    g_warning("attach");

    g_assert(drc->isolation_state == SPAPR_DR_ISOLATION_STATE_ISOLATED);
    g_assert(drc->allocation_state == SPAPR_DR_ALLOCATION_STATE_UNUSABLE);
    g_assert(drc->indicator_state == SPAPR_DR_INDICATOR_STATE_INACTIVE);
    g_assert(fdt || coldplug);

    /* NOTE:this means we can't detach unless guest has a userspace/kernel
     * that moves this state back to ISOLATED in response to an unplug
     * event (or manually by the admin prior to unplug). if we force things
     * we can easily crash the guest, so play it safe for now.
     *
     * as a potential improvement we can queue up an unplug request and
     * satisfy it during reset when we know the device won't be in use,
     * and beyond that we could look at implementing a timeout or flag to
     * handle the forced removal, but since all these are essentially
     * work-arounds for guests that don't support hotplug in the first
     * place, we can set that aside for now.
     */
    drc->isolation_state = SPAPR_DR_ISOLATION_STATE_UNISOLATED;
    drc->allocation_state = SPAPR_DR_ALLOCATION_STATE_USABLE;
    drc->indicator_state = SPAPR_DR_INDICATOR_STATE_ACTIVE;

    drc->dev = d;
    drc->ccs.fdt = fdt;
    drc->ccs.fdt_offset = drc->ccs.fdt_start_offset = fdt_start_offset;
    drc->ccs.fdt_depth = 0;

    object_property_add_link(OBJECT(drc), "device",
                             object_get_typename(OBJECT(drc->dev)),
                             (Object **)(&drc->dev),
                             NULL, 0, NULL);
}

static void detach(sPAPRDRConnector *drc, DeviceState *d,
                   spapr_drc_detach_cb *detach_cb,
                   void *detach_cb_opaque)
{
    g_warning("detach");

    drc->detach_cb = detach_cb;
    drc->detach_cb_opaque = detach_cb_opaque;

    if (drc->isolation_state != SPAPR_DR_ISOLATION_STATE_ISOLATED) {
        g_warning("awaiting transition to isolated state before removal");
        drc->awaiting_release = true;
        return;
    }

    drc->allocation_state = SPAPR_DR_ALLOCATION_STATE_UNUSABLE;
    drc->indicator_state = SPAPR_DR_INDICATOR_STATE_INACTIVE;

    if (drc->detach_cb) {
        drc->detach_cb(drc->dev, drc->detach_cb_opaque);
    }

    /* TODO: reset all fields */
    drc->awaiting_release = false;
    g_free(drc->ccs.fdt);
    drc->ccs.fdt = NULL;
    drc->ccs.fdt_offset = drc->ccs.fdt_start_offset = drc->ccs.fdt_depth = 0;
    object_property_del(OBJECT(drc), "device", NULL);
    drc->dev = NULL;
    drc->detach_cb = NULL;
    drc->detach_cb_opaque = NULL;
}

sPAPRDRConnector *spapr_dr_connector_by_index(uint32_t index)
{
    Object *obj;
    char name[256];

    /* FIXME: should use qdev_get_machine() for root */
    snprintf(name, sizeof(name), "/machine/%s/%x", DRC_CONTAINER_PATH, index);
    obj = object_resolve_path(name, NULL);

    return !obj ? NULL : SPAPR_DR_CONNECTOR(obj);
}

sPAPRDRConnector *spapr_dr_connector_by_id(sPAPRDRConnectorType type,
                                           uint32_t id)
{
    return spapr_dr_connector_by_index(type << DRC_INDEX_TYPE_SHIFT | id);
}

sPAPRDRConnector *spapr_dr_connector_new(sPAPRDRConnectorType type,
                                         uint32_t id)
{
    sPAPRDRConnector *drc =
        SPAPR_DR_CONNECTOR(object_new(TYPE_SPAPR_DR_CONNECTOR));
    sPAPRDRConnectorClass *drck = SPAPR_DR_CONNECTOR_GET_CLASS(drc);
    char name[256];
    Error *err = NULL;
    Object *root_container;

    g_assert(type);

    drc->type = type;
    drc->id = id;
    snprintf(name, sizeof(name), "%x", drck->get_index(drc));

    root_container = container_get(qdev_get_machine(), DRC_CONTAINER_PATH);
    object_property_add_child(root_container, name, OBJECT(drc), &err);
    if (err) {
        error_report("%s", error_get_pretty(err));
        error_free(err);
        object_unref(OBJECT(drc));
    }

    g_warning("created: %s", name);

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
    object_property_add(obj, "index", "uint32", prop_get_index,
                        NULL, NULL, NULL, NULL);
    object_property_add(obj, "entity-sense", "uint32", prop_get_entity_sense,
                        NULL, NULL, NULL, NULL);
    object_property_add(obj, "fdt", "struct", prop_get_fdt,
                        NULL, NULL, NULL, NULL);
}

static void spapr_dr_connector_class_init(ObjectClass *k, void *data)
{
    sPAPRDRConnectorClass *drck = SPAPR_DR_CONNECTOR_CLASS(k);
    drck->set_isolation_state = set_isolation_state;
    drck->set_indicator_state = set_indicator_state;
    drck->set_allocation_state = set_allocation_state;
    drck->get_index = get_index;
    drck->entity_sense = entity_sense;
    drck->configure_connector = configure_connector;
    drck->attach = attach;
    drck->detach = detach; }

static const TypeInfo spapr_dr_connector_info = {
    .name          = TYPE_SPAPR_DR_CONNECTOR,
    .parent        = TYPE_OBJECT,
    .instance_size = sizeof(sPAPRDRConnector),
    .instance_init = spapr_dr_connector_instance_init,
    .class_size    = sizeof(sPAPRDRConnectorClass),
    .class_init    = spapr_dr_connector_class_init,
/*
    .interfaces    = (InterfaceInfo[]) {
        { }
    }
*/
};

static void spapr_drc_register_types(void)
{
    type_register_static(&spapr_dr_connector_info);
}

type_init(spapr_drc_register_types)

