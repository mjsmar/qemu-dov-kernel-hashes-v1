/*
 * QContext: QEMU event loop context class
 *
 * Copyright IBM Corp. 2013
 *
 * Authors:
 *  Michael Roth      <mdroth@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#include <stdio.h>
#include "qom/object.h"
#include "qemu/module.h"
#include "qcontext/qcontext.h"
#include "qapi/error.h"
#include "string.h"
#include "qemu/osdep.h"
#include "qapi/visitor.h"

/* TODO: this is for compatibility with -object, but really these
 * should probably live in /qcontexts or something
 */
#define QCONTEXT_ROOT_CONTAINER "/objects"

/* QContext property accessors */

static char *qcontext_get_id(Object *obj, Error **errp)
{
    QContext *ctx = QCONTEXT(obj);

    return ctx->id ? g_strdup(ctx->id) : NULL;
}

static void qcontext_set_id(Object *obj, const char *id, Error **errp)
{
    QContext *ctx = QCONTEXT(obj);
    QContextClass *ctxk = QCONTEXT_GET_CLASS(ctx);
    Object *root_container = container_get(object_get_root(),
                                           QCONTEXT_ROOT_CONTAINER);

    if (id) {
        object_property_add_child(root_container, id, OBJECT(ctx), errp);
        ctx->id = g_strdup(id);
    } else {
        ctx->id = object_property_add_unnamed_child(root_container,
                                                    OBJECT(ctx), errp);
    }

    if (ctxk->set_id_hook) {
        ctxk->set_id_hook(ctx, id, errp);
    }
}

static char *qcontext_get_threaded(Object *obj, Error **errp)
{
    QContext *ctx = QCONTEXT(obj);

    return ctx->threaded ? g_strdup("yes") : g_strdup("no");
}

static void qcontext_set_threaded(Object *obj, const char *threaded,
                                  Error **errp)
{
    QContext *ctx = QCONTEXT(obj);

    if (strcmp(threaded, "yes") == 0) {
        if (ctx->threaded == false) {
            qcontext_create_thread(ctx);
        }
        ctx->threaded = true;
        ctx->should_run = true;
    } else if (strcmp(threaded, "no") == 0) {
        ctx->threaded = false;
        ctx->should_run = false;
    } else {
        error_setg(errp,
                   "invalid value for \"threaded\","
                   " must specify \"yes\" or \"no\"");
    }
}

static void qcontext_get_thread_id(Object *obj, Visitor *v, void *opaque,
                                   const char *name, Error **errp)
{
    QContext *ctx = QCONTEXT(obj);
    int64_t value = ctx->thread_id;

    visit_type_int(v, &value, name, errp);
}

/* QOM interfaces */

static void qcontext_initfn(Object *obj)
{
    QContext *ctx = QCONTEXT(obj);
    /* note: controlling these as properties is somewhat awkward. these are
     * really static initialization parameters, but we do it this way so we
     * can instantiate from the command-line via -object.
     */
    object_property_add_str(obj, "id",
                            qcontext_get_id,
                            qcontext_set_id,
                            NULL);
    object_property_add_str(obj, "threaded",
                            qcontext_get_threaded,
                            qcontext_set_threaded,
                            NULL);
    object_property_add(obj, "thread_id", "int",
                        qcontext_get_thread_id, NULL, NULL,
                        NULL, NULL);

    ctx->named_sources = g_hash_table_new(g_str_hash, g_str_equal);
    ctx->threaded = true;
}

static void qcontext_init_completionfn(Object *obj)
{
    QContext *ctx = QCONTEXT(obj);
    gchar *path, *id;

    /* this means we were created via -object. Figure out
     * our 'id' by looking at our path in the QOM tree and
     * Update our internal structures to reflect this.
     */
    if (!ctx->id) {
        path = object_get_canonical_path(obj);
        id = g_strrstr(path, "/") + 1;
        ctx->id = g_strdup(id);
        g_free(path);
    }

    if (strcmp(ctx->id, QEMU_QCONTEXT_MAIN) == 0) {
        ctx->gmctx = g_main_context_default();
    } else {
        ctx->gmctx = g_main_context_new();
    }

    if (ctx->threaded) {
        ctx->thread_id = -1;
        qcontext_create_thread(ctx);
    } else {
        ctx->thread_id = qemu_get_thread_id();
    }
}

static void qcontext_finalizefn(Object *obj)
{
    QContext *ctx = QCONTEXT(obj);

    if (ctx->threaded) {
        qcontext_stop_thread(ctx);
    }

    if (strcmp(ctx->id, QEMU_QCONTEXT_MAIN) != 0) {
        g_main_context_unref(ctx->gmctx);
    }

    g_free(ctx->id);
    g_hash_table_unref(ctx->named_sources);
}

static void qcontext_class_initfn(ObjectClass *class, void *data)
{
}

static const TypeInfo qcontext_type_info = {
    .name = TYPE_QCONTEXT,
    .parent = TYPE_OBJECT,
    .instance_size = sizeof(QContext),
    .instance_init = qcontext_initfn,
    .instance_init_completion = qcontext_init_completionfn,
    .instance_finalize = qcontext_finalizefn,
    .class_size = sizeof(QContextClass),
    .class_init = qcontext_class_initfn,
    .abstract = false
};

void qcontext_register_types(void)
{
    type_register_static(&qcontext_type_info);
}

type_init(qcontext_register_types)

/* Helper functions for working with QContexts */

QContext *qcontext_find_by_name(const char *name, Error **errp)
{
    char path[256];

    sprintf(path, "%s/%s", QCONTEXT_ROOT_CONTAINER, name);
    return QCONTEXT(object_resolve_path_type(path, TYPE_QCONTEXT, NULL));
}

void qcontext_notify(QContext *ctx)
{
    g_main_context_wakeup(ctx->gmctx);
}

static void *qcontext_thread_fn(void *opaque)
{
    QContext *ctx = opaque;
    ctx->thread_id = qemu_get_thread_id();
    while (ctx->should_run) {
        qcontext_iterate(ctx, true);
    }
    return NULL;
}

void qcontext_create_thread(QContext *ctx)
{
    qemu_thread_create(&ctx->thread, qcontext_thread_fn,
                       ctx, QEMU_THREAD_JOINABLE);
}

void qcontext_stop_thread(QContext *ctx)
{
    ctx->should_run = false;
    qcontext_notify(ctx);
    qemu_thread_join(&ctx->thread);
    ctx->threaded = false;
}

GMainContext *qcontext_get_context(QContext *ctx)
{
    return ctx->gmctx;
}

QContext *qcontext_new(const char *id, bool threaded, Error **errp)
{
    QContext *ctx = QCONTEXT(object_new(TYPE_QCONTEXT));

    object_property_set_str(OBJECT(ctx), id, "id", errp);
    if (error_is_set(errp)) {
        object_unref(OBJECT(ctx));
        return NULL;
    }

    object_property_set_str(OBJECT(ctx),
                            threaded ? "yes" : "no",
                            "threaded", errp);
    if (error_is_set(errp)) {
        object_unref(OBJECT(ctx));
        return NULL;
    }

    object_init_completion(OBJECT(ctx));

    return ctx;
}

/* GLib unfortunately doesn't provide a way to locate a GSource by name.
 * This ends up be very useful for adding "utility" GSources to a
 * GMainContext to act as a backend for things like IOHandlers, Slirp,
 * etc. So to provide this we maintain a hash of named/tracked GSource
 * as part of the QContext encapsulating the GMainContext we've attached
 * the GSource to.
 *
 * This must only be used for GSource that have already been attached to
 * the GMainContext associated with the QContext. If the GSource is
 * subsequently detached, we should also remove the mapping via
 * qcontext_remove_named_source().
 */
void qcontext_attach_source(QContext *ctx, GSource *source,
                            const char *name)
{
    g_assert(source);
    g_source_attach(source, qcontext_get_context(ctx));
    if (name) {
        g_assert(qcontext_find_source_by_name(ctx, name) == NULL);
        g_hash_table_insert(ctx->named_sources, g_strdup(name), source);
    }
}

static gboolean match_source_value(gpointer stored_key,
                                   gpointer stored_value,
                                   gpointer desired_value)
{
    return stored_value == desired_value;
}

/* GLib doesn't provide a 'detach' function for GSources added to
 * non-default contexts. As a result we have the same limitations
 * here and cannot provide a nice counterpart to 'attach', only
 * 'destroy'.
 */
void qcontext_destroy_source(QContext *ctx, GSource *source)
{
    g_hash_table_foreach_remove(ctx->named_sources, match_source_value, source);
    g_source_destroy(source);
}

GSource *qcontext_find_source_by_name(QContext *ctx, const char *name)
{
    return g_hash_table_lookup(ctx->named_sources, name);
}

gboolean qcontext_prepare(QContext *ctx, gint *priority)
{
    return g_main_context_prepare(ctx->gmctx, priority);
}

gint qcontext_append_query(QContext *ctx, gint max_priority, gint *timeout,
                           GArray *pfd_array)
{
    gint current_count = ctx->last_pfd_count;
    gint estimated_count;

    guint start_offset = pfd_array->len;

    do {
        GPollFD *pfds;
        estimated_count = current_count;
        g_array_set_size(pfd_array, start_offset + estimated_count);
        pfds = &g_array_index(pfd_array, GPollFD, start_offset);
        current_count = g_main_context_query(ctx->gmctx, max_priority, timeout, pfds,
                                             estimated_count);
    } while (estimated_count < current_count);

    if (estimated_count > current_count) {
        g_array_set_size(pfd_array, start_offset + current_count);
    }

    /* remember prior pfd count to reduce unecessary iterations */
    ctx->last_pfd_count = current_count;

    return current_count;
}

gint qcontext_check(QContext *ctx, gint max_priority, GPollFD *fds, gint n_fds)
{
    return g_main_context_check(ctx->gmctx, max_priority, fds, n_fds);
}

void qcontext_dispatch(QContext *ctx)
{
    g_main_context_dispatch(ctx->gmctx);
}

gboolean qcontext_iterate(QContext *ctx, gboolean blocking)
{
    return g_main_context_iteration(ctx->gmctx, blocking);
}
