/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* vim:set expandtab ts=4 shiftwidth=4: */
/* 
 * Copyright 2007-2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 *
 * CDDL HEADER START
 * 
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 * 
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 * 
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 * 
 * CDDL HEADER END
 * 
 * File:   nwamui_ncp.c
 *
 */
#include <stdlib.h>

#include <glib-object.h>
#include <glib/gi18n.h>
#include <strings.h>
#include <gtk/gtkliststore.h>
#include <gtk/gtktreestore.h>

#include "libnwamui.h"

#include <errno.h>
#include <sys/dlpi.h>
#include <libdllink.h>

static NwamuiNcp       *instance        = NULL;
static gint _num_wireless = 0; /* Count wireless i/fs */

enum {
    PROP_NWAM_NCP = 1,
    PROP_PRIORITY_GROUP,
    PROP_NCU_LIST,
    PROP_NCU_TREE_STORE,
    PROP_NCU_LIST_STORE,
    PROP_ACTIVE_NCU,
    PROP_WIRELESS_LINK_NUM
};

enum {
    S_ACTIVATE_NCU,
    S_DEACTIVATE_NCU,
    ADD_NCU,
    REMOVE_NCU,
    LAST_SIGNAL
};

typedef struct {
    /* Input */
    gint64          current_prio;

    /* Output */
    guint32         num_manual_enabled;
    guint32         num_manual_online;
    guint32         num_prio_excl;
    guint32         num_prio_excl_online;
    guint32         num_prio_shared;
    guint32         num_prio_shared_online;
    guint32         num_prio_all;
    guint32         num_prio_all_online;
    NwamuiNcu      *needs_wifi_selection;
    NwamuiWifiNet  *needs_wifi_key;
} check_online_info_t;

static guint nwamui_ncp_signals[LAST_SIGNAL] = { 0, 0 };

struct _NwamuiNcpPrivate {
    nwam_ncp_handle_t nwam_ncp;
    gchar*            name;
    gboolean          nwam_ncp_modified;
    gint              wireless_link_num;

    GList*        ncu_list;
    GtkTreeStore* ncu_tree_store;
    GtkListStore* ncu_list_store;
    GList*        ncus_removed;
    GList*        ncus_added;

    GList* temp_ncu_list;

    /* Cached Priority Group */
    gint   priority_group;
};

#define NWAMUI_NCP_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE((o), NWAMUI_TYPE_NCP, NwamuiNcpPrivate))

static void nwamui_ncp_set_property ( GObject         *object,
                                      guint            prop_id,
                                      const GValue    *value,
                                      GParamSpec      *pspec);

static void nwamui_ncp_get_property ( GObject         *object,
                                      guint            prop_id,
                                      GValue          *value,
                                      GParamSpec      *pspec);

static void nwamui_ncp_finalize (     NwamuiNcp *self);

static void          nwamui_object_real_set_handle(NwamuiObject *object, const gpointer handle);
static nwam_state_t  nwamui_object_real_get_nwam_state(NwamuiObject *object, nwam_aux_state_t* aux_state_p, const gchar**aux_state_string_p);
static const gchar*  nwamui_object_real_get_name ( NwamuiObject *object );
static gboolean      nwamui_object_real_can_rename(NwamuiObject *object);
static gboolean      nwamui_object_real_set_name(NwamuiObject *object, const gchar* name);
static void          nwamui_object_real_set_active ( NwamuiObject *object, gboolean active );
static gboolean      nwamui_object_real_get_active( NwamuiObject *object );
static gboolean      nwamui_object_real_commit( NwamuiObject *object );
static void          nwamui_object_real_reload(NwamuiObject* object);
static gboolean      nwamui_object_real_destroy( NwamuiObject* object );
static gboolean      nwamui_object_real_is_modifiable(NwamuiObject *object);
static gboolean      nwamui_object_real_has_modifications(NwamuiObject* object);
static NwamuiObject* nwamui_object_real_clone(NwamuiObject *object, const gchar *name, NwamuiObject *parent);

/* Default signal handlers */
static void default_activate_ncu_signal_handler (NwamuiNcp *self, NwamuiNcu* ncu, gpointer user_data);
static void default_deactivate_ncu_signal_handler (NwamuiNcp *self, NwamuiNcu* ncu, gpointer user_data);
static void default_add_ncu_signal_handler (NwamuiNcp* ncp, NwamuiNcu* ncu, gpointer user_data);
static void default_remove_ncu_signal_handler (NwamuiNcp* ncp, NwamuiNcu* ncu, gpointer user_data);

/* Callbacks */
static int nwam_ncu_walker_cb (nwam_ncu_handle_t ncu, void *data);
static void ncu_notify_cb( GObject *gobject, GParamSpec *arg1, gpointer data);
static void row_deleted_cb (GtkTreeModel *tree_model, GtkTreePath *path, gpointer user_data);
static void row_inserted_cb (GtkTreeModel *tree_model, GtkTreePath *path, GtkTreeIter *iter, gpointer user_data);
static void rows_reordered_cb(GtkTreeModel *tree_model, GtkTreePath *path, GtkTreeIter *iter, gpointer arg3, gpointer user_data);

/* GList callbacks */
static gint find_ncu_by_device_name(gconstpointer data, gconstpointer user_data);
static gint find_first_wifi_net(gconstpointer data, gconstpointer user_data);
static void find_wireless_ncu(gpointer obj, gpointer user_data);

G_DEFINE_TYPE (NwamuiNcp, nwamui_ncp, NWAMUI_TYPE_OBJECT)

static void
nwamui_ncp_class_init (NwamuiNcpClass *klass)
{
    /* Pointer to GObject Part of Class */
    GObjectClass *gobject_class = (GObjectClass*) klass;
    NwamuiObjectClass *nwamuiobject_class = NWAMUI_OBJECT_CLASS(klass);
    
    /* Override Some Function Pointers */
    gobject_class->set_property = nwamui_ncp_set_property;
    gobject_class->get_property = nwamui_ncp_get_property;
    gobject_class->finalize = (void (*)(GObject*)) nwamui_ncp_finalize;

    klass->activate_ncu = default_activate_ncu_signal_handler;
    klass->deactivate_ncu = default_deactivate_ncu_signal_handler;
    klass->add_ncu = default_add_ncu_signal_handler;
    klass->remove_ncu = default_remove_ncu_signal_handler;

    nwamuiobject_class->set_handle = nwamui_object_real_set_handle;
    nwamuiobject_class->get_name = nwamui_object_real_get_name;
    nwamuiobject_class->set_name = nwamui_object_real_set_name;
    nwamuiobject_class->can_rename = nwamui_object_real_can_rename;
    nwamuiobject_class->get_active = nwamui_object_real_get_active;
    nwamuiobject_class->set_active = nwamui_object_real_set_active;
    nwamuiobject_class->get_nwam_state = nwamui_object_real_get_nwam_state;
    nwamuiobject_class->commit = nwamui_object_real_commit;
    nwamuiobject_class->reload = nwamui_object_real_reload;
    nwamuiobject_class->destroy = nwamui_object_real_destroy;
    nwamuiobject_class->is_modifiable = nwamui_object_real_is_modifiable;
    nwamuiobject_class->has_modifications = nwamui_object_real_has_modifications;
    nwamuiobject_class->clone = nwamui_object_real_clone;

	g_type_class_add_private(klass, sizeof(NwamuiNcpPrivate));

    /* Create some properties */
    g_object_class_install_property (gobject_class,
                                     PROP_NWAM_NCP,
                                     g_param_spec_pointer ("nwam_ncp",
                                                           _("Nwam Ncp handle"),
                                                           _("Nwam Ncp handle"),
                                                           G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE));

    g_object_class_install_property (gobject_class,
      PROP_PRIORITY_GROUP,
      g_param_spec_int64("priority_group",
        _("priority group"),
        _("priority group"),
        0,
        G_MAXINT64,
        0,
        G_PARAM_READABLE));

    g_object_class_install_property (gobject_class,
                                     PROP_NCU_LIST,
                                     g_param_spec_pointer ("ncu_list",
                                                          _("GList of NCUs in the NCP"),
                                                          _("GList of NCUs in the NCP"),
                                                           G_PARAM_READABLE));

    g_object_class_install_property (gobject_class,
                                     PROP_NCU_TREE_STORE,
                                     g_param_spec_object ("ncu_tree_store",
                                                          _("TreeStore of NCUs in the NCP"),
                                                          _("TreeStore of NCUs in the NCP"),
                                                          GTK_TYPE_TREE_STORE,
                                                          G_PARAM_READABLE));

    g_object_class_install_property (gobject_class,
                                     PROP_NCU_LIST_STORE,
                                     g_param_spec_object ("ncu_list_store",
                                                          _("ListStore of NCUs in the NCP"),
                                                          _("ListStore of NCUs in the NCP"),
                                                          GTK_TYPE_LIST_STORE,
                                                          G_PARAM_READABLE));


    g_object_class_install_property (gobject_class,
                                     PROP_WIRELESS_LINK_NUM,
                                     g_param_spec_int("wireless_link_num",
                                                      _("wireless_link_num"),
                                                      _("wireless_link_num"),
                                                      0,
                                                      G_MAXINT,
                                                      0,
                                                      G_PARAM_READABLE));

    /* Create some signals */
    nwamui_ncp_signals[S_ACTIVATE_NCU] =   
      g_signal_new ("activate_ncu",
        G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_FIRST | G_SIGNAL_ACTION,
        G_STRUCT_OFFSET (NwamuiNcpClass, activate_ncu),
        NULL, NULL,
        g_cclosure_marshal_VOID__OBJECT,
        G_TYPE_NONE,                  /* Return Type */
        1,                            /* Number of Args */
        G_TYPE_OBJECT);                  /* Types of Args */
    
    nwamui_ncp_signals[S_DEACTIVATE_NCU] =   
      g_signal_new ("deactivate_ncu",
        G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_FIRST | G_SIGNAL_ACTION,
        G_STRUCT_OFFSET (NwamuiNcpClass, deactivate_ncu),
        NULL, NULL,
        g_cclosure_marshal_VOID__OBJECT,
        G_TYPE_NONE,                  /* Return Type */
        1,                            /* Number of Args */
        G_TYPE_OBJECT);                  /* Types of Args */
    
    nwamui_ncp_signals[ADD_NCU] =   
      g_signal_new ("add_ncu",
        G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_FIRST | G_SIGNAL_ACTION,
        G_STRUCT_OFFSET (NwamuiNcpClass, add_ncu),
        NULL, NULL,
        g_cclosure_marshal_VOID__OBJECT,
        G_TYPE_NONE,                  /* Return Type */
        1,                            /* Number of Args */
        G_TYPE_OBJECT);               /* Types of Args */
    
    nwamui_ncp_signals[REMOVE_NCU] =   
      g_signal_new ("remove_ncu",
        G_TYPE_FROM_CLASS (klass),
        G_SIGNAL_RUN_FIRST | G_SIGNAL_ACTION,
        G_STRUCT_OFFSET (NwamuiNcpClass, remove_ncu),
        NULL, NULL,
        g_cclosure_marshal_VOID__OBJECT,
        G_TYPE_NONE,                  /* Return Type */
        1,                            /* Number of Args */
        G_TYPE_OBJECT);               /* Types of Args */
    
}


static void
nwamui_ncp_init ( NwamuiNcp *self)
{
    NwamuiNcpPrivate *prv = NWAMUI_NCP_GET_PRIVATE(self);
    nwam_error_t      nerr;
    int64_t           pg  = 0;

    self->prv = prv;
    
    /* Used to store the list of NCUs that are added or removed in the UI but
     * not yet committed.
     */

    /* Init pri group. */
    if ( (nerr = nwam_ncp_get_active_priority_group( &pg )) == NWAM_SUCCESS ) {
        prv->priority_group = (gint64)pg;
    } else {
        nwamui_debug("Error getting active priortiy group: %d (%s)", 
          nerr, nwam_strerror(nerr) );
    }

    prv->ncu_list_store = gtk_list_store_new ( 1, NWAMUI_TYPE_NCU);
    prv->ncu_tree_store = gtk_tree_store_new ( 1, NWAMUI_TYPE_OBJECT);

    g_signal_connect(prv->ncu_list_store, "row_deleted", G_CALLBACK(row_deleted_cb), (gpointer)self);
    g_signal_connect(prv->ncu_list_store, "row_inserted", G_CALLBACK(row_inserted_cb), (gpointer)self);
    g_signal_connect(prv->ncu_list_store, "rows_reordered", G_CALLBACK(rows_reordered_cb), (gpointer)self);
    g_signal_connect(prv->ncu_tree_store, "row_deleted", G_CALLBACK(row_deleted_cb), (gpointer)self);
    g_signal_connect(prv->ncu_tree_store, "row_inserted", G_CALLBACK(row_inserted_cb), (gpointer)self);
    g_signal_connect(prv->ncu_tree_store, "rows_reordered", G_CALLBACK(rows_reordered_cb), (gpointer)self);
}

static void
nwamui_ncp_set_property (   GObject         *object,
                            guint            prop_id,
                            const GValue    *value,
                            GParamSpec      *pspec)
{
    NwamuiNcp*      self = NWAMUI_NCP(object);
    gchar*          tmpstr = NULL;
    gint            tmpint = 0;
    nwam_error_t    nerr;
    gboolean        read_only = FALSE;

    read_only = !nwamui_object_is_modifiable(NWAMUI_OBJECT(self));

    if (read_only) {
        g_error("Attempting to modify read-only ncp %s", self->prv->name);
        return;
    }

    switch (prop_id) {
        case PROP_NWAM_NCP: {
                g_assert (self->prv->nwam_ncp == NULL);
                self->prv->nwam_ncp = g_value_get_pointer (value);
            }
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
            break;
    }
}

static void
nwamui_ncp_get_property (   GObject         *object,
                            guint            prop_id,
                            GValue          *value,
                            GParamSpec      *pspec)
{
    NwamuiNcp *self = NWAMUI_NCP(object);

    switch (prop_id) {
    case PROP_PRIORITY_GROUP:
        g_value_set_int64(value, nwamui_ncp_get_prio_group(self));
        break;
        case PROP_NCU_LIST: {
                g_value_set_pointer( value, nwamui_util_copy_obj_list( self->prv->ncu_list ) );
            }
            break;
        case PROP_NCU_TREE_STORE: {
                g_value_set_object( value, self->prv->ncu_tree_store );
            }
            break;
        case PROP_NCU_LIST_STORE: {
                g_value_set_object( value, self->prv->ncu_list_store );
            }
            break;
        case PROP_WIRELESS_LINK_NUM:
            g_value_set_int( value, nwamui_ncp_get_wireless_link_num(self));
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
            break;
    }
}

static void
nwamui_ncp_finalize (NwamuiNcp *self)
{

    g_free( self->prv->name);
    
    if ( self->prv->ncu_list != NULL ) {
        nwamui_util_free_obj_list(self->prv->ncu_list);
    }
    
    if ( self->prv->ncu_tree_store != NULL ) {
        gtk_tree_store_clear(self->prv->ncu_tree_store);
        g_object_unref(self->prv->ncu_tree_store);
    }
    
    if ( self->prv->ncu_list_store != NULL ) {
        gtk_list_store_clear(self->prv->ncu_list_store);
        g_object_unref(self->prv->ncu_list_store);
    }

    if ( self->prv->nwam_ncp != NULL ) {
        nwam_ncp_free( self->prv->nwam_ncp );
    }
    
    self->prv = NULL;

	G_OBJECT_CLASS(nwamui_ncp_parent_class)->finalize(G_OBJECT(self));
}

static nwam_state_t
nwamui_object_real_get_nwam_state(NwamuiObject *object, nwam_aux_state_t* aux_state_p, const gchar**aux_state_string_p)
{
    nwam_state_t    rstate = NWAM_STATE_UNINITIALIZED;

    g_return_val_if_fail(NWAMUI_IS_NCP( object ), rstate);

    if ( aux_state_p ) {
        *aux_state_p = NWAM_AUX_STATE_UNINITIALIZED;
    }

    if ( aux_state_string_p ) {
        *aux_state_string_p = (const gchar*)nwam_aux_state_to_string( NWAM_AUX_STATE_UNINITIALIZED );
    }

    {
        NwamuiNcp   *ncp = NWAMUI_NCP(object);
        nwam_state_t        state;
        nwam_aux_state_t    aux_state;


        /* First look at the LINK state, then the IP */
        if ( ncp->prv->nwam_ncp &&
             nwam_ncp_get_state( ncp->prv->nwam_ncp, &state, &aux_state ) == NWAM_SUCCESS ) {

            rstate = state;
            if ( aux_state_p ) {
                *aux_state_p = aux_state;
            }
            if ( aux_state_string_p ) {
                *aux_state_string_p = (const gchar*)nwam_aux_state_to_string( aux_state );
            }
        }
    }

    return(rstate);
}

extern NwamuiObject*
nwamui_ncp_new(const gchar* name )
{
    NwamuiNcp*      self = NULL;

    self = NWAMUI_NCP(g_object_new (NWAMUI_TYPE_NCP,
        "name", name,
        NULL));

    return NWAMUI_OBJECT(self);
}

/**
 * nwamui_ncp_new_with_handle
 * @returns: a #NwamuiNcp.
 *
 **/
extern NwamuiObject*
nwamui_ncp_new_with_handle (nwam_ncp_handle_t ncp)
{
    NwamuiNcp*      self = NWAMUI_NCP(g_object_new (NWAMUI_TYPE_NCP,
                                   NULL));
    nwamui_object_real_set_handle(NWAMUI_OBJECT(self), ncp);

    if (self->prv->nwam_ncp == NULL) {
        g_object_unref(self);
        self = NULL;
    }

    return NWAMUI_OBJECT( self );
}

/**
 * nwamui_object_real_clone:
 * @returns: a copy of an existing #NwamuiNcp, with the name specified.
 *
 * Creates a new #NwamuiNcp and copies properties.
 *
 **/
static NwamuiObject*
nwamui_object_real_clone(NwamuiObject *object, const gchar *name, NwamuiObject *parent)
{
    NwamuiNcp         *self    = NWAMUI_NCP(object);
    NwamuiDaemon      *daemon  = NWAMUI_DAEMON(parent);
    NwamuiObject      *new_ncp = NULL;;
    nwam_ncp_handle_t  new_ncp_h;
    nwam_error_t       nerr;

    g_return_val_if_fail(NWAMUI_IS_NCP(object), NULL);
    g_return_val_if_fail(NWAMUI_IS_DAEMON(parent), NULL);
    g_return_val_if_fail(name != NULL, NULL);

    nerr = nwam_ncp_copy (self->prv->nwam_ncp, name, &new_ncp_h);

    if ( nerr != NWAM_SUCCESS ) { 
        nwamui_warning("Failed to clone new NCP %s from existing NCP %s: %s",
          name, self->prv->name, nwam_strerror(nerr) );
        return new_ncp;
    }

    new_ncp = nwamui_ncp_new_with_handle(new_ncp_h);

    nwamui_daemon_append_object(daemon, new_ncp);

    return new_ncp;
}

static nwam_ncp_handle_t
get_nwam_ncp_handle(NwamuiNcp* self)
{
    NwamuiNcpPrivate *prv  = NWAMUI_NCP_GET_PRIVATE(self);
    nwam_ncp_handle_t ncp_handle = NULL;

    g_return_val_if_fail(NWAMUI_IS_NCP(self), ncp_handle);

    if (prv->name) {
        nwam_error_t      nerr;

        nerr = nwam_ncp_read(prv->name, 0, &ncp_handle);
        if (nerr != NWAM_SUCCESS) {
            if (nerr == NWAM_ENTITY_NOT_FOUND) {
                g_debug("Failed to read ncp information for %s error: %s", prv->name, nwam_strerror(nerr));
            } else {
                g_warning("Failed to read ncp information for %s error: %s", prv->name, nwam_strerror(nerr));
            }
            return ( NULL );
        }
    }
    return ncp_handle;
}

/**
 *  Check for new NCUs - useful after reactivation of daemon or signal for new
 *  NCUs.
 **/
static void
nwamui_object_real_reload(NwamuiObject* object)
{
    NwamuiNcpPrivate  *prv                  = NWAMUI_NCP_GET_PRIVATE(object);
    int                cb_ret               = 0;
    nwam_error_t       nerr;
    nwam_loc_handle_t  handle;

    g_return_if_fail(NWAMUI_IS_NCP(object));

    handle = get_nwam_ncp_handle(NWAMUI_NCP(object));
    if (handle) {
        if (prv->nwam_ncp) {
            nwam_ncp_free(prv->nwam_ncp);
        }
        prv->nwam_ncp = handle;
    }

    g_object_freeze_notify(G_OBJECT(object));

    /* Assumption is that anything that was newly added should be removed,
     * anything that was removed will still exist unless a commit was done.
     */
    if ( prv->ncus_added != NULL ) {
        /* Make sure they are removed from the system */
        g_list_foreach( prv->ncus_added, (GFunc)nwamui_object_destroy, NULL );
        g_list_foreach( prv->ncus_added, (GFunc)nwamui_util_obj_unref, NULL );
        g_list_free( prv->ncus_added );
        prv->ncus_added = NULL;
    }
    if ( prv->ncus_removed != NULL ) {
        g_list_foreach( prv->ncus_removed, (GFunc)nwamui_util_obj_unref, NULL );
        g_list_free( prv->ncus_removed );
        prv->ncus_removed = NULL;
    }

    g_assert(prv->nwam_ncp != NULL );

    g_object_freeze_notify(G_OBJECT(prv->ncu_tree_store));
    g_object_freeze_notify(G_OBJECT(prv->ncu_list_store));

    /*
     * Set temp_ncu_list, which is being used to find NCUs that
     * have been removed from the system.
     */
    if ( prv->ncu_list != NULL ) {
        prv->temp_ncu_list = nwamui_util_copy_obj_list( prv->ncu_list );
    }
    else {
        prv->temp_ncu_list = NULL;
    }

    _num_wireless = 0;
    nerr = nwam_ncp_walk_ncus( prv->nwam_ncp, nwam_ncu_walker_cb, (void*)object,
      NWAM_FLAG_NCU_TYPE_CLASS_ALL, &cb_ret );
    if (nerr != NWAM_SUCCESS) {
        nwamui_warning("Failed to walk ncus of ncp  %s error: %s", prv->name, nwam_strerror(nerr));
        g_list_foreach(prv->temp_ncu_list, nwamui_util_obj_unref, NULL );    
        g_list_free(prv->temp_ncu_list);
        prv->temp_ncu_list = NULL;
    }

    if ( prv->temp_ncu_list != NULL ) {
        g_debug("Found some NCUs that have been removed from the system");
        for (GList *elem = g_list_first(prv->temp_ncu_list);
             elem != NULL;
             elem = g_list_next(elem) ) {
            if ( elem->data != NULL && NWAMUI_IS_NCU(elem->data) ) {
                nwamui_ncp_remove_ncu(NWAMUI_NCP(object), NWAMUI_NCU(elem->data) );
            }
        }
        nwamui_util_free_obj_list( prv->temp_ncu_list );
        prv->temp_ncu_list = NULL;
    }
    g_object_thaw_notify(G_OBJECT(prv->ncu_tree_store));
    g_object_thaw_notify(G_OBJECT(prv->ncu_list_store));

    if ( prv->wireless_link_num != _num_wireless ) {
        prv->wireless_link_num = _num_wireless;
        g_object_notify(G_OBJECT(object), "wireless_link_num" );
    }

    g_object_thaw_notify(G_OBJECT(object));
}

extern nwam_ncp_handle_t
nwamui_ncp_get_nwam_handle( NwamuiNcp* self )
{
    return (self->prv->nwam_ncp);
}

static void
nwamui_object_real_set_handle(NwamuiObject *object, const gpointer handle)
{
    NwamuiNcpPrivate        *prv  = NWAMUI_NCP_GET_PRIVATE(object);
    const nwam_ncp_handle_t  ncp  = handle;
    const nwam_ncp_handle_t  ncp_handle;
    char*                    name = NULL;
    nwam_error_t             nerr;
    nwam_ncp_handle_t        nwam_ncp;
    
    g_return_if_fail(NWAMUI_IS_NCP(object));

    if ( (nerr = nwam_ncp_get_name (ncp, &name)) != NWAM_SUCCESS ) {
        g_debug ("Failed to get name for ncp, error: %s", nwam_strerror (nerr));
    }

    nwamui_object_set_name(object, name);

    nwam_ncp = get_nwam_ncp_handle(NWAMUI_NCP(object));
    if (nwam_ncp) {
        if (prv->nwam_ncp) {
            nwam_ncp_free(prv->nwam_ncp);
        }
        prv->nwam_ncp = nwam_ncp;

        nwamui_object_real_reload(object);
    } else {
        /* Most likely only exists in memory right now, so use handle passed
         * in as parameter.
         * in clone mode, the new handle gets from nwam_ncp_copy can't be read
         * again.
         */
        prv->nwam_ncp = ncp;
        nwamui_object_real_reload(object);
        /* prv->nwam_ncp = NULL; */
    }
    g_free(name);
}

/**
 * nwamui_object_real_get_name:
 * @returns: null-terminated C String with name of the the NCP.
 *
 **/
static const gchar*
nwamui_object_real_get_name ( NwamuiObject *object )
{
    NwamuiNcp    *self       = NWAMUI_NCP(object);
    g_return_val_if_fail (NWAMUI_IS_NCP(self), NULL); 
    
    return self->prv->name;
}

static gboolean
nwamui_object_real_can_rename(NwamuiObject *object)
{
    NwamuiNcp *self = NWAMUI_NCP(object);
    NwamuiNcpPrivate *prv = NWAMUI_NCP(object)->prv;

    g_return_val_if_fail (NWAMUI_IS_NCP(object), FALSE);
    /* if ( prv->nwam_ncp != NULL ) { */
    /*     if (nwam_ncp_can_set_name( prv->nwam_ncp )) { */
    /*         return( TRUE ); */
    /*     } */
    /* } */
    /* return FALSE; */

    return (prv->nwam_ncp == NULL);
}

static gboolean
nwamui_object_real_set_name( NwamuiObject *object, const gchar* name )
{
    NwamuiNcpPrivate *prv  = NWAMUI_NCP_GET_PRIVATE(object);
    nwam_error_t      nerr;

    g_return_val_if_fail(NWAMUI_IS_NCP(object), FALSE);
    g_return_val_if_fail(name, FALSE);

    /* Initially set name or rename. */
    if (prv->name == NULL) {
        if (prv->nwam_ncp == NULL) {
            /* prv->nwam_ncp = get_nwam_ncp_handle(NWAMUI_NCP(object)); */

            /* nerr = nwam_ncp_read (name, 0, &prv->nwam_ncp); */
            /* if (nerr == NWAM_SUCCESS) { */
            /*     /\* g_debug ("nwamui_ncp_read found nwam_ncp_handle %s", name); *\/ */
            /* } else { */
            /*     nerr = nwam_ncp_create (name, NULL, &prv->nwam_ncp); */
            /*     if (nerr != NWAM_SUCCESS) { */
            /*         g_warning("nwamui_ncp_create error creating nwam_ncp_handle %s", name); */
            /*         prv->nwam_ncp == NULL; */
            /*     } */
            /* } */

            nerr = nwam_ncp_read (name, 0, &prv->nwam_ncp);
            if (nerr == NWAM_SUCCESS) {
                /* g_debug ("nwamui_ncp_read found nwam_ncp_handle %s", name); */
            } else {
                /* New NCP */
                prv->nwam_ncp_modified = TRUE;
            }
        }
    } else {
        /* /\* we may rename here *\/ */
        /* if (prv->nwam_ncp != NULL) { */
        /*     nerr = nwam_ncp_set_name (prv->nwam_ncp, name); */
        /*     if (nerr != NWAM_SUCCESS) { */
        /*         g_debug ("nwam_ncp_set_name %s error: %s", name, nwam_strerror (nerr)); */
        /*     } */
        /* } */
        /* else { */
        /*     g_warning("Unexpected null ncp handle"); */
        /* } */

        if (prv->nwam_ncp == NULL) {
            nerr = nwam_ncp_read (name, 0, &prv->nwam_ncp);
            if (nerr == NWAM_SUCCESS) {
                /* g_debug ("nwamui_ncp_read found nwam_ncp_handle %s", name); */
                /* NCP is existed. */
                nwam_ncp_free(prv->nwam_ncp);
                prv->nwam_ncp = NULL;
                return FALSE;
            }
        }
        prv->nwam_ncp_modified = TRUE;
    }

    g_free(prv->name);
    prv->name = g_strdup(name);
    return TRUE;
}

/**
 * nwamui_ncp_is_active:
 * @nwamui_ncp: a #NwamuiNcp.
 * @returns: TRUE if the ncp is online.
 *
 **/
extern gboolean
nwamui_object_real_get_active( NwamuiObject *object )
{
    NwamuiNcp    *self       = NWAMUI_NCP(object);
    gboolean active = FALSE;
    g_return_val_if_fail( NWAMUI_IS_NCP(self), active );

    if ( self->prv->nwam_ncp ) {
        nwam_state_t        state = NWAM_STATE_OFFLINE;
        nwam_aux_state_t    aux_state = NWAM_AUX_STATE_UNINITIALIZED;

        state = nwamui_object_get_nwam_state(object, &aux_state, NULL);

        if ( state == NWAM_STATE_ONLINE ) {
            active = TRUE;
        }
    }

    return( active );
}

/** 
 * nwamui_object_real_set_active:
 * @nwamui_ncp: a #NwamuiEnv.
 * @active: Immediately activates/deactivates the ncp.
 * 
 **/ 
extern void
nwamui_object_real_set_active (NwamuiObject *object, gboolean active)
{
    NwamuiNcp    *self       = NWAMUI_NCP(object);
    /* Activate immediately */
    nwam_state_t        state = NWAM_STATE_OFFLINE;
    nwam_aux_state_t    aux_state = NWAM_AUX_STATE_UNINITIALIZED;

    g_return_if_fail (NWAMUI_IS_NCP (self));

    state = nwamui_object_get_nwam_state(object, &aux_state, NULL);

    if ( state != NWAM_STATE_ONLINE && active ) {
        nwam_error_t nerr;
        if ( (nerr = nwam_ncp_enable (self->prv->nwam_ncp)) != NWAM_SUCCESS ) {
            g_warning("Failed to enable ncp due to error: %s", nwam_strerror(nerr));
        }
    }
    else {
        g_warning("Cannot disable an NCP, enable another one to do this");
    }
}

/**
 * nwamui_object_real_is_modifiable:
 * @nwamui_ncp: a #NwamuiNcp.
 * @returns: the modifiable.
 *
 **/
extern gboolean
nwamui_object_real_is_modifiable(NwamuiObject *object)
{
    NwamuiNcp    *self       = NWAMUI_NCP(object);
    nwam_error_t  nerr;
    gboolean      modifiable = FALSE; 
    boolean_t     readonly;

    g_assert(NWAMUI_IS_NCP (self));
    if (self->prv->nwam_ncp == NULL ) {
        return TRUE;
    }

    if ((nerr = nwam_ncp_get_read_only( self->prv->nwam_ncp, &readonly )) == NWAM_SUCCESS) {
        modifiable = readonly?FALSE:TRUE;
    } else {
        g_warning("Error getting ncp read-only status: %s", nwam_strerror( nerr ) );
    }

    /* if (self->prv->name && strcmp( self->prv->name, NWAM_NCP_NAME_AUTOMATIC) == 0 ) { */
    /*     modifiable = FALSE; */
    /* } */
    /* else { */
    /*     modifiable = TRUE; */
    /* } */

    return( modifiable );
}

static gboolean
nwamui_object_real_has_modifications(NwamuiObject* object)
{
    NwamuiNcpPrivate *prv  = NWAMUI_NCP_GET_PRIVATE(object);

    return prv->nwam_ncp_modified;
}

extern gint64
nwamui_ncp_get_prio_group( NwamuiNcp* self )
{
    g_return_val_if_fail(NWAMUI_IS_NCP(self), (gint64)0);

    return self->prv->priority_group;
}

extern void
nwamui_ncp_set_prio_group( NwamuiNcp* self, gint64 new_prio ) 
{
    g_return_if_fail(NWAMUI_IS_NCP(self));

    if (self->prv->priority_group != new_prio) {
        self->prv->priority_group = new_prio;
        g_object_notify(G_OBJECT(self), "priority-group");
    }
}

static void
check_ncu_online( gpointer obj, gpointer user_data )
{
	NwamuiNcu           *ncu = NWAMUI_NCU(obj);
	check_online_info_t *info_p = (check_online_info_t*)user_data;
    nwam_state_t         state;
    nwam_aux_state_t     aux_state;
    gboolean             online = FALSE;
    nwamui_cond_activation_mode_t activation_mode;
    nwamui_cond_priority_group_mode_t prio_group_mode;


    if ( ncu == NULL || !NWAMUI_IS_NCU(ncu) ) {
        return;
    }

    activation_mode = nwamui_object_get_activation_mode(NWAMUI_OBJECT(ncu));
    state = nwamui_object_get_nwam_state( NWAMUI_OBJECT(ncu), &aux_state, NULL);

    if ( state == NWAM_STATE_ONLINE && aux_state == NWAM_AUX_STATE_UP ) {
        online = TRUE;
    }

    nwamui_debug("NCU %s: online = %s", nwamui_object_get_name(NWAMUI_OBJECT(ncu)), online?"True":"False" );

    switch (activation_mode) { 
        case NWAMUI_COND_ACTIVATION_MODE_MANUAL: {
            if ( nwamui_object_get_enabled(NWAMUI_OBJECT(ncu)) ) {
                    /* Only count if expected to be enabled. */
                    info_p->num_manual_enabled++;
                    if ( online ) {
                        info_p->num_manual_online++;
                    }
                }
            }
            break;
        case NWAMUI_COND_ACTIVATION_MODE_PRIORITIZED: {
                prio_group_mode = nwamui_ncu_get_priority_group_mode( ncu );

                if ( info_p->current_prio != nwamui_ncu_get_priority_group( ncu ) ) {
                    /* Skip objects not in current_prio group */
                    return;
                }

                switch (prio_group_mode) {
                    case NWAMUI_COND_PRIORITY_GROUP_MODE_EXCLUSIVE:
                        info_p->num_prio_excl++;
                        if ( online ) {
                            info_p->num_prio_excl_online++;
                        }
                        break;
                    case NWAMUI_COND_PRIORITY_GROUP_MODE_SHARED:
                        info_p->num_prio_shared++;
                        if ( online ) {
                            info_p->num_prio_shared_online++;
                        }
                        break;
                    case NWAMUI_COND_PRIORITY_GROUP_MODE_ALL:
                        info_p->num_prio_all++;
                        if ( online ) {
                            info_p->num_prio_all_online++;
                        }
                        break;
                }
            }
            break;
        default:
            break;
    }

    /* For link events */
    state = nwamui_ncu_get_link_nwam_state(ncu, &aux_state, NULL);

    if ( aux_state == NWAM_AUX_STATE_LINK_WIFI_NEED_SELECTION ) {
        if ( info_p->needs_wifi_selection == NULL ) {
            info_p->needs_wifi_selection = NWAMUI_NCU(g_object_ref(ncu));
        }
    }

    if ( aux_state == NWAM_AUX_STATE_LINK_WIFI_NEED_KEY) {
        if ( info_p->needs_wifi_key == NULL ) {
            info_p->needs_wifi_key = nwamui_ncu_get_wifi_info(ncu);
        }
    }

}

/**
 * nwamui_ncp_all_ncus_online:
 * @nwamui_ncp: a #NwamuiNcp.
 *
 * Sets needs_wifi_selection to point to the NCU needing selection, or NULL.
 * Sets needs_wifi_key to point to the WifiNet needing selection, or NULL.
 *
 * @returns: TRUE if all the expected NCUs in the NCP are online
 *
 **/
extern gboolean
nwamui_ncp_all_ncus_online (NwamuiNcp       *self,
                            NwamuiNcu      **needs_wifi_selection,
                            NwamuiWifiNet  **needs_wifi_key )
{
    nwam_error_t            nerr;
    gboolean                all_online = TRUE;
    check_online_info_t     info;

    if (!NWAMUI_IS_NCP (self) && self->prv->nwam_ncp == NULL ) {
        return( FALSE );
    }

    bzero((void*)&info, sizeof(check_online_info_t));
    
    info.current_prio = nwamui_ncp_get_prio_group( self );

    if ( self->prv->ncu_list == NULL ) {
        /* If there are no NCUs then something is wrong and 
         * we are not on-line 
         */
        return( FALSE );
    }

    g_list_foreach(self->prv->ncu_list, check_ncu_online, &info );

    if ( info.num_manual_enabled != info.num_manual_online ) {
        all_online = FALSE;
    }
    else if ( info.num_prio_excl > 0 && info.num_prio_excl_online == 0 ) {
        all_online = FALSE;
    }
    else if (info.num_prio_shared > 0 && (info.num_prio_shared < info.num_prio_shared_online || info.num_prio_shared_online < 1)) {
        all_online = FALSE;
    }
    else if ( info.num_prio_all != info.num_prio_all_online ) {
        all_online = FALSE;
    }

    if ( ! all_online ) {
        /* Only care about these values if we see something off-line that
         * should be on-line.
         */
        if ( needs_wifi_selection != NULL ) {
            /* No need to ref, since will already by ref-ed */
            *needs_wifi_selection = info.needs_wifi_selection;
        }
        if ( needs_wifi_key != NULL ) {
            /* No need to ref, since will already by ref-ed */
            *needs_wifi_key = info.needs_wifi_key;
        }
    }
    else {
        if ( info.needs_wifi_selection != NULL ) {
            g_object_unref(G_OBJECT(info.needs_wifi_selection));
        }
        if ( info.needs_wifi_key != NULL ) {
            g_object_unref(G_OBJECT(info.needs_wifi_key));
        }
    }

    return( all_online );
}

/**
 * nwamui_ncp_get_ncu_tree_store_store:
 * @returns: GList containing NwamuiNcu elements
 *
 **/
extern GtkTreeStore*
nwamui_ncp_get_ncu_tree_store( NwamuiNcp *self )
{
    GtkTreeStore* ncu_tree_store = NULL;
    
    if ( self == NULL ) {
        return( NULL );
    }

    g_return_val_if_fail (NWAMUI_IS_NCP(self), ncu_tree_store); 
    
    g_object_get (G_OBJECT (self),
                  "ncu_tree_store", &ncu_tree_store,
                  NULL);

    return( ncu_tree_store );
}

/**
 * nwamui_ncp_get_ncu_list:
 * @returns: GList containing NwamuiNcu elements
 *
 **/
extern GList*
nwamui_ncp_get_ncu_list( NwamuiNcp *self )
{
    g_return_val_if_fail(NWAMUI_IS_NCP(self), NULL);
    
    return nwamui_util_copy_obj_list(self->prv->ncu_list);
}

extern gint
nwamui_ncp_get_ncu_num(NwamuiNcp *self)
{
    g_return_val_if_fail (NWAMUI_IS_NCP(self), 0); 

    return g_list_length(self->prv->ncu_list);
}

extern void
nwamui_ncp_foreach_ncu_list_store( NwamuiNcp *self, GtkTreeModelForeachFunc func, gpointer user_data )
{
    NwamuiNcpPrivate *prv  = NWAMUI_NCP_GET_PRIVATE(self);
    g_return_if_fail(NWAMUI_IS_NCP(self) && prv->ncu_list_store != NULL);
    gtk_tree_model_foreach(GTK_TREE_MODEL(prv->ncu_tree_store), func, user_data );
}

extern  GList*
nwamui_ncp_find_ncu( NwamuiNcp *self, GCompareFunc func, gconstpointer data)
{
    g_return_val_if_fail(NWAMUI_IS_NCP(self), NULL);

    if (func)
        return g_list_find_custom(self->prv->ncu_list, data, func);
    else
        return g_list_find(self->prv->ncu_list, data);
}

/**
 * nwamui_ncp_get_ncu_list_store_store:
 * @returns: GList containing NwamuiNcu elements
 *
 **/
extern GtkListStore*
nwamui_ncp_get_ncu_list_store( NwamuiNcp *self )
{
    GtkListStore* ncu_list_store = NULL;
    
    if ( self == NULL ) {
        return( NULL );
    }

    g_return_val_if_fail (NWAMUI_IS_NCP(self), ncu_list_store); 
    
    g_object_get (G_OBJECT (self),
                  "ncu_list_store", &ncu_list_store,
                  NULL);

    return( ncu_list_store );
}


/**
 * nwamui_ncp_foreach_ncu
 * 
 * Calls func for each NCU in the NCP
 *
 **/
extern void
nwamui_ncp_foreach_ncu(NwamuiNcp *self, GFunc func, gpointer user_data)
{
    NwamuiNcpPrivate *prv  = NWAMUI_NCP_GET_PRIVATE(self);
    g_return_if_fail(func);
    g_list_foreach(prv->ncu_list, func, user_data);
}

/**
 * nwamui_ncp_get_ncu_by_device_name
 * 
 * Returns a pointer to an NCU given the device name.
 * Unref is needed
 *
 **/
extern  NwamuiObject*
nwamui_ncp_get_ncu_by_device_name( NwamuiNcp *self, const gchar* device_name )
{
    NwamuiNcpPrivate *prv             = NWAMUI_NCP_GET_PRIVATE(self);
    NwamuiObject     *ret_ncu         = NULL;
    gchar            *ncu_device_name = NULL;
    GList            *found_list;

    g_return_val_if_fail (device_name, ret_ncu ); 

    found_list = g_list_find_custom(self->prv->ncu_list, (gpointer)device_name, find_ncu_by_device_name);

    if (found_list) {
        ret_ncu = NWAMUI_OBJECT(g_object_ref(found_list->data));
        nwamui_debug("NCP %s found NCU %s (0x%p) OK", prv->name, device_name, found_list->data);
    } else {
        nwamui_debug("NCP %s found NCU %s FAILED", prv->name, device_name);
    }
    return ret_ncu;
}

extern void 
nwamui_ncp_remove_ncu_by_device_name( NwamuiNcp* self, const gchar* device_name ) 
{
    GtkTreeIter     iter;
    gchar          *ncu_device_name = NULL;
    gboolean        valid_iter = FALSE;
    NwamuiObject   *found_ncu = NULL;

    g_return_if_fail (device_name != NULL && strlen(device_name) > 0 );

    found_ncu = nwamui_ncp_get_ncu_by_device_name( self, device_name );

    if ( found_ncu != NULL ) {
        nwamui_ncp_remove_ncu( self, NWAMUI_NCU(found_ncu));
    }
}

extern void 
nwamui_ncp_remove_ncu( NwamuiNcp* self, NwamuiNcu* ncu ) 
{
    NwamuiNcpPrivate *prv        = NWAMUI_NCP_GET_PRIVATE(self);
    GtkTreeIter       iter;
    gboolean          valid_iter = FALSE;

    g_return_if_fail (NWAMUI_IS_NCP(self) && NWAMUI_IS_NCU(ncu) );

    g_object_freeze_notify(G_OBJECT(self));
    g_object_freeze_notify(G_OBJECT(prv->ncu_tree_store));
    g_object_freeze_notify(G_OBJECT(prv->ncu_list_store));

    for (valid_iter = gtk_tree_model_get_iter_first( GTK_TREE_MODEL(prv->ncu_tree_store), &iter);
         valid_iter;
         valid_iter = gtk_tree_model_iter_next( GTK_TREE_MODEL(prv->ncu_tree_store), &iter)) {
        NwamuiNcu      *_ncu;

        gtk_tree_model_get( GTK_TREE_MODEL(prv->ncu_tree_store), &iter, 0, &_ncu, -1);

        if ( _ncu == ncu ) {
            gtk_tree_store_remove(GTK_TREE_STORE(prv->ncu_tree_store), &iter);

            if ( nwamui_ncu_get_ncu_type( _ncu ) == NWAMUI_NCU_TYPE_WIRELESS ) {
                prv->wireless_link_num--;
                g_object_notify(G_OBJECT(self), "wireless_link_num" );
            }

            g_object_unref(_ncu);
            g_object_notify(G_OBJECT(self), "ncu_tree_store" );
            break;
        }
        g_object_unref(_ncu);
    }

    for (valid_iter = gtk_tree_model_get_iter_first( GTK_TREE_MODEL(prv->ncu_list_store), &iter);
         valid_iter;
         valid_iter = gtk_tree_model_iter_next( GTK_TREE_MODEL(prv->ncu_list_store), &iter)) {
        NwamuiNcu      *_ncu;

        gtk_tree_model_get( GTK_TREE_MODEL(prv->ncu_list_store), &iter, 0, &_ncu, -1);


        if ( _ncu == ncu ) {
            gtk_list_store_remove(GTK_LIST_STORE(prv->ncu_list_store), &iter);

            g_object_notify(G_OBJECT(self), "ncu_list_store" );
            g_object_unref(_ncu);
            break;
        }
        g_object_unref(_ncu);
    }

    prv->ncu_list = g_list_remove( prv->ncu_list, ncu );
    prv->ncus_removed = g_list_append(prv->ncus_removed, ncu );

    g_signal_emit (self,
      nwamui_ncp_signals[REMOVE_NCU],
      0, /* details */
      ncu);

    g_object_thaw_notify(G_OBJECT(prv->ncu_tree_store));
    g_object_thaw_notify(G_OBJECT(prv->ncu_list_store));
    g_object_thaw_notify(G_OBJECT(self));
}

extern void 
nwamui_ncp_add_ncu( NwamuiNcp* self, NwamuiNcu* new_ncu ) 
{
    NwamuiNcpPrivate *prv         = NWAMUI_NCP_GET_PRIVATE(self);
    GtkTreeIter       iter;
    gboolean          valid_iter  = FALSE;
    NwamuiObject     *found_ncu   = NULL;
    gchar            *device_name = NULL;
    GList            *found_list;

    g_return_if_fail (NWAMUI_IS_NCP(self) && NWAMUI_IS_NCU(new_ncu) );

    device_name = nwamui_ncu_get_device_name(new_ncu);

    if ( device_name ) {
        found_ncu = nwamui_ncp_get_ncu_by_device_name( self, device_name );
    }

    if ( found_ncu ) { 
        nwamui_warning("Tried to add existing NCU '%s' to NCP", device_name?device_name:"NULL");
        g_free(device_name);
        g_object_unref(found_ncu);
        return;
    }

    g_object_freeze_notify(G_OBJECT(self));

    /* Next check if it was previously removed in the UI! */
    found_list = g_list_find_custom(self->prv->ncus_removed, (gpointer)device_name, find_ncu_by_device_name);

    if (found_list) {
        found_ncu = NWAMUI_OBJECT(found_list->data);
        nwamui_debug("NCP %s found removed NCU %s (0x%p) OK", prv->name, device_name, found_list->data);
    }

    if (found_ncu) {
        self->prv->ncus_removed = g_list_remove(self->prv->ncus_removed, found_ncu );
        nwamui_debug("Found already removed NCU : %s, re-using...", device_name );
        nwamui_object_reload(found_ncu);
        new_ncu = NWAMUI_NCU(found_ncu);
    } else {
        if ( nwamui_ncu_get_ncu_type( new_ncu ) == NWAMUI_NCU_TYPE_WIRELESS ) {
            self->prv->wireless_link_num++;
            g_object_notify(G_OBJECT (self), "wireless_link_num" );
        }
    }

    g_object_freeze_notify(G_OBJECT(self->prv->ncu_tree_store));
    g_object_freeze_notify(G_OBJECT(self->prv->ncu_list_store));

    /* NCU isn't already in the list, so add it */
    prv->ncu_list = g_list_insert_sorted(prv->ncu_list, g_object_ref(new_ncu), (GCompareFunc)nwamui_object_sort_by_name);

    gtk_list_store_append( prv->ncu_list_store, &iter );
    gtk_list_store_set( prv->ncu_list_store, &iter, 0, new_ncu, -1 );

    gtk_tree_store_append( prv->ncu_tree_store, &iter, NULL );
    gtk_tree_store_set( prv->ncu_tree_store, &iter, 0, new_ncu, -1 );

    g_signal_connect(G_OBJECT(new_ncu), "notify",
                     (GCallback)ncu_notify_cb, (gpointer)self);


    self->prv->ncus_added = g_list_append( self->prv->ncus_added, 
                                           g_object_ref(new_ncu));

    g_free(device_name);

    g_signal_emit (self,
      nwamui_ncp_signals[ADD_NCU],
      0, /* details */
      new_ncu);

    g_object_thaw_notify(G_OBJECT(self->prv->ncu_tree_store));
    g_object_thaw_notify(G_OBJECT(self->prv->ncu_list_store));
    g_object_thaw_notify(G_OBJECT(self));
}

static gboolean
nwamui_object_real_commit( NwamuiObject *object )
{
    NwamuiNcpPrivate *prv      = NWAMUI_NCP_GET_PRIVATE(object);
    gboolean          rval     = FALSE;
    const gchar*      ncp_name = nwamui_object_get_name(object);
    nwam_error_t      nerr;

    g_return_val_if_fail (NWAMUI_IS_NCP(object), rval );

    if (prv->nwam_ncp == NULL) {
        /* This is a new added NCP */
        nerr = nwam_ncp_create (prv->name, NULL, &prv->nwam_ncp);
        if (nerr != NWAM_SUCCESS) {
            g_warning("nwamui_ncp_create error creating nwam_ncp_handle %s", prv->name);
            return FALSE;
        }
        prv->nwam_ncp_modified = FALSE;
    }

    if (nwamui_object_real_is_modifiable(object)) {
        if ( prv->ncus_removed != NULL ) {
            /* Make sure they are removed from the system */
            g_list_foreach( prv->ncus_removed, (GFunc)nwamui_object_destroy, NULL );
            g_list_foreach( prv->ncus_removed, (GFunc)nwamui_util_obj_unref, NULL );
            g_list_free( prv->ncus_removed );
            prv->ncus_removed = NULL;
        }

        if ( prv->ncus_added != NULL ) {
            g_list_foreach( prv->ncus_added, (GFunc)nwamui_object_commit, NULL );
            g_list_foreach( prv->ncus_added, (GFunc)nwamui_util_obj_unref, NULL );
            g_list_free( prv->ncus_added );
            prv->ncus_added = NULL;
        }

        for(GList* ncu_item = g_list_first(prv->ncu_list);
             rval && ncu_item != NULL;
             ncu_item = g_list_next(ncu_item)) {
            NwamuiNcu*   ncu      = NWAMUI_NCU(ncu_item->data);
            const gchar* ncu_name = nwamui_ncu_get_display_name( ncu );

            nwamui_debug("Going to commit changes for %s : %s", ncp_name, ncu_name );
            if (!nwamui_object_commit(NWAMUI_OBJECT(ncu))) {
                nwamui_debug("Commit FAILED for %s : %s", ncp_name, ncu_name );
                rval = FALSE;
                break;
            }
        }

        /* I assume commit will cause REFRESH event, so comment out this line. */
        /* nwamui_object_real_reload(object); */
    } else {
        nwamui_debug("NCP : %s is not modifiable", ncp_name );
    }

    /* if ((nerr = nwam_ncp_commit(prv->nwam_ncp, 0)) != NWAM_SUCCESS) { */
    /*     g_warning("Failed when committing NCP for %s, %s", prv->name, nwam_strerror(nerr)); */
    /*     return FALSE; */
    /* } */

    return TRUE;
}

static gboolean
nwamui_object_real_destroy( NwamuiObject *object )
{
    NwamuiNcpPrivate *prv  = NWAMUI_NCP_GET_PRIVATE(object);
    nwam_error_t    nerr;

    g_assert(NWAMUI_IS_NCP(object));

    if (prv->nwam_ncp != NULL) {

        if ((nerr = nwam_ncp_destroy(prv->nwam_ncp, 0)) != NWAM_SUCCESS) {
            g_warning("Failed when destroying NCP for %s", prv->name);
            return( FALSE );
        }
        prv->nwam_ncp = NULL;
    }

    return( TRUE );
}

/* After discussion with Alan, it makes sense that we only show devices that
 * only have a physical presence on the system - on the basis that to configure
 * anything else would have no effect. 
 *
 * Tunnels are the only possible exception, but until it is implemented by
 * Seb where tunnels have a physical link id, then this will omit them too.
 */
static gboolean
device_exists_on_system( gchar* device_name )
{
    dladm_handle_t              handle;
    datalink_id_t               linkid;
    uint32_t                    flags = 0;
    gboolean                    rval = FALSE;

    if ( device_name != NULL ) {
        if ( dladm_open( &handle ) == DLADM_STATUS_OK ) {
            /* Interfaces that exist have a mapping, but also the OPT_ACTIVE
             * flag set, this could be unset if the device was removed from
             * the system (e.g. USB / PCMCIA)
             */
            if ( dladm_name2info( handle, device_name, &linkid, &flags, NULL, NULL ) == DLADM_STATUS_OK &&
                 (flags & DLADM_OPT_ACTIVE) ) {
                rval = TRUE;
            }
            else {
                g_debug("Unable to map device '%s' to linkid", device_name );
            }

            dladm_close( handle );
        }
    }

    return( rval );
}

static nwam_ncu_type_t
get_nwam_ncu_type( nwam_ncu_handle_t ncu )
{
    nwam_error_t        nerr;
    nwam_value_type_t   nwam_type;
    nwam_value_t        nwam_data;
    uint64_t            value = 0;
    nwam_ncu_type_t     rval = NWAM_NCU_TYPE_LINK;

    if ( ncu == NULL ) {
        return( value );
    }

    if ( (nerr = nwam_ncu_get_prop_type( NWAM_NCU_PROP_TYPE, &nwam_type ) ) != NWAM_SUCCESS 
         || nwam_type != NWAM_VALUE_TYPE_UINT64 ) {
        g_warning("Unexpected type for ncu property %s - got %d\n", NWAM_NCU_PROP_TYPE, nwam_type );
        return rval;
    }

    if ( (nerr = nwam_ncu_get_prop_value (ncu, NWAM_NCU_PROP_TYPE, &nwam_data)) != NWAM_SUCCESS ) {
        g_debug("No value for ncu property %s, error = %s", NWAM_NCU_PROP_TYPE, nwam_strerror( nerr ) );
        return rval;
    }

    if ( (nerr = nwam_value_get_uint64(nwam_data, &value )) != NWAM_SUCCESS ) {
        g_debug("Unable to get uint64 value for ncu property %s, error = %s", NWAM_NCU_PROP_TYPE, nwam_strerror( nerr ) );
        return rval;
    }

    nwam_value_free(nwam_data);

    rval = (nwam_ncu_type_t)value;

    return( rval );
}

static int
nwam_ncu_walker_cb (nwam_ncu_handle_t ncu, void *data)
{
    char*               name;
    nwam_error_t        nerr;
    NwamuiObject*       new_ncu;
    GtkTreeIter         iter;
    NwamuiNcp*          ncp = NWAMUI_NCP(data);
    NwamuiNcpPrivate*   prv = ncp->prv;
    nwam_ncu_type_t     nwam_ncu_type;

    if ( (nerr = nwam_ncu_get_name (ncu, &name)) != NWAM_SUCCESS ) {
        g_debug ("Failed to get name for ncu, error: %s", nwam_strerror (nerr));
    }

    g_debug ("nwam_ncu_walker_cb '%s' 0x%p", name, ncu);

    if ( !device_exists_on_system( name ) ) {
        /* Skip device that don't have a physical equivalent */
        return( 0 );
    }

    nwam_ncu_type = get_nwam_ncu_type(ncu);
    g_debug ("nwam_ncu_walker_cb '%s' type %d 0x%p", name, nwam_ncu_type, ncu);

    if ( (new_ncu = nwamui_ncp_get_ncu_by_device_name( ncp, name )) != NULL ) {
        /* Update rather than create a new object */
        g_debug("Updating existing ncu (%s) from handle 0x%08X", name, ncu );
        nwamui_object_set_handle(new_ncu, ncu);

        /* Only count if it's a LINK class (to avoid double count) */
        if ( nwam_ncu_type == NWAM_NCU_TYPE_LINK &&
          new_ncu && nwamui_ncu_get_ncu_type(NWAMUI_NCU(new_ncu)) == NWAMUI_NCU_TYPE_WIRELESS ) {
            _num_wireless++;
        }

        /*
         * Remove from temp_ncu_list, which is being used to find NCUs that
         * have been removed from the system.
         */
        ncp->prv->temp_ncu_list = g_list_remove( ncp->prv->temp_ncu_list, new_ncu );

        g_object_unref(new_ncu);
        free(name);
        return( 0 );
    }
    else {

        g_debug("Creating a new ncu for %s from handle 0x%08X", name, ncu );
        new_ncu = nwamui_ncu_new_with_handle( NWAMUI_NCP(data), ncu);

        /* Only count if it's a LINK class (to avoid double count) */
        if ( nwam_ncu_type == NWAM_NCU_TYPE_LINK &&
          new_ncu && nwamui_ncu_get_ncu_type(NWAMUI_NCU(new_ncu)) == NWAMUI_NCU_TYPE_WIRELESS ) {
            _num_wireless++;
        }

        free(name);
    }

    /* NCU isn't already in the list, so add it */
    if ( new_ncu != NULL ) {
        prv->ncu_list = g_list_insert_sorted(prv->ncu_list, g_object_ref(new_ncu), (GCompareFunc)nwamui_object_sort_by_name);

        gtk_list_store_append( prv->ncu_list_store, &iter );
        gtk_list_store_set( prv->ncu_list_store, &iter, 0, new_ncu, -1 );

        gtk_tree_store_append( prv->ncu_tree_store, &iter, NULL );
        gtk_tree_store_set( prv->ncu_tree_store, &iter, 0, new_ncu, -1 );

        g_signal_emit (ncp,
          nwamui_ncp_signals[ADD_NCU],
          0, /* details */
          new_ncu);

        g_signal_connect(G_OBJECT(new_ncu), "notify",
                         (GCallback)ncu_notify_cb, (gpointer)ncp);

        return(0);
    }

    g_warning("Failed to create a new NCU");
    return(1);
}

static void
find_wireless_ncu( gpointer obj, gpointer user_data )
{
	NwamuiNcu           *ncu = NWAMUI_NCU(obj);
    GList              **glist_p = (GList**)user_data;

    if ( ncu == NULL || !NWAMUI_IS_NCU(ncu) ) {
        return;
    }

    if ( nwamui_ncu_get_ncu_type( ncu ) == NWAMUI_NCU_TYPE_WIRELESS ) {
        (*glist_p) = g_list_append( (*glist_p), (gpointer)g_object_ref(ncu) );
    }
}

extern GList*
nwamui_ncp_get_wireless_ncus( NwamuiNcp* self )
{
    GList*  ncu_list = NULL;

    g_return_val_if_fail( NWAMUI_IS_NCP(self), ncu_list );

    if ( self->prv->wireless_link_num > 0 ) {
        g_list_foreach( self->prv->ncu_list, find_wireless_ncu, &ncu_list );
    }

    return( ncu_list );
}

static gint
find_ncu_by_device_name(gconstpointer data, gconstpointer user_data)
{
    NwamuiNcu*  ncu             = NWAMUI_NCU(data);
    gchar      *ncu_device_name = nwamui_ncu_get_device_name( ncu );
    gchar      *device_name     = (gchar *)user_data;
    gint        found           = 1;

    g_return_val_if_fail(NWAMUI_IS_NCU(data), 1);
    g_return_val_if_fail(user_data, 1);

    if (g_ascii_strcasecmp(ncu_device_name, device_name) == 0) {
        found = 0;
    } else {
        found = 1;
    }
    g_free(ncu_device_name);
    return found;
}

static gint
find_first_wifi_net(gconstpointer data, gconstpointer user_data)
{
	NwamuiNcu  *ncu          = (NwamuiNcu *) data;
    NwamuiNcu **wireless_ncu = (NwamuiNcu **) user_data;
	
    g_return_val_if_fail(NWAMUI_IS_NCU(ncu), 1);
    g_return_val_if_fail(user_data, 1);
    
	if (nwamui_ncu_get_ncu_type(ncu) == NWAMUI_NCU_TYPE_WIRELESS ) {
        *wireless_ncu = g_object_ref(ncu);
        return 0;
	}
    return 1;
}

extern NwamuiNcu*
nwamui_ncp_get_first_wireless_ncu(NwamuiNcp *self)
{
    NwamuiNcu *wireless_ncu = NULL;

    g_return_val_if_fail(NWAMUI_IS_NCP(self), NULL);

    if (self->prv->wireless_link_num > 0) {
        nwamui_ncp_find_ncu(self, find_first_wifi_net, &wireless_ncu);
    }

    return wireless_ncu;
}

extern gint
nwamui_ncp_get_wireless_link_num( NwamuiNcp* self )
{
    g_return_val_if_fail( NWAMUI_IS_NCP(self), 0);

    return self->prv->wireless_link_num;
}

static void
freeze_thaw( gpointer obj, gpointer data ) {
    if ( obj ) {
        if ( (gboolean) data ) {
            g_object_freeze_notify(G_OBJECT(obj));
        }
        else {
            g_object_thaw_notify(G_OBJECT(obj));
        }
    }
}

extern void
nwamui_ncp_freeze_notify_ncus( NwamuiNcp* self )
{
    nwamui_ncp_foreach_ncu( self, (GFunc)freeze_thaw, (gpointer)TRUE );
}

extern void
nwamui_ncp_thaw_notify_ncus( NwamuiNcp* self )
{
    nwamui_ncp_foreach_ncu( self, (GFunc)freeze_thaw, (gpointer)FALSE );
}

/* Callbacks */

static void
ncu_notify_cb( GObject *gobject, GParamSpec *arg1, gpointer data)
{
    NwamuiNcp* self = NWAMUI_NCP(data);
    GtkTreeIter     iter;
    gboolean        valid_iter = FALSE;

    for (valid_iter = gtk_tree_model_get_iter_first( GTK_TREE_MODEL(self->prv->ncu_tree_store), &iter);
         valid_iter;
         valid_iter = gtk_tree_model_iter_next( GTK_TREE_MODEL(self->prv->ncu_tree_store), &iter)) {
        NwamuiNcu      *ncu;

        gtk_tree_model_get( GTK_TREE_MODEL(self->prv->ncu_tree_store), &iter, 0, &ncu, -1);

        if ( (gpointer)ncu == (gpointer)gobject ) {
            GtkTreePath *path;

            valid_iter = FALSE;

            path = gtk_tree_model_get_path(GTK_TREE_MODEL(self->prv->ncu_tree_store),
              &iter);
            gtk_tree_model_row_changed(GTK_TREE_MODEL(self->prv->ncu_tree_store),
              path,
              &iter);
            gtk_tree_path_free(path);
        }
        if ( ncu )
            g_object_unref(ncu);
    }
    for (valid_iter = gtk_tree_model_get_iter_first( GTK_TREE_MODEL(self->prv->ncu_list_store), &iter);
         valid_iter;
         valid_iter = gtk_tree_model_iter_next( GTK_TREE_MODEL(self->prv->ncu_list_store), &iter)) {
        NwamuiNcu      *ncu;

        gtk_tree_model_get( GTK_TREE_MODEL(self->prv->ncu_list_store), &iter, 0, &ncu, -1);

        if ( (gpointer)ncu == (gpointer)gobject ) {
            GtkTreePath *path;

            valid_iter = FALSE;

            path = gtk_tree_model_get_path(GTK_TREE_MODEL(self->prv->ncu_list_store),
              &iter);
            gtk_tree_model_row_changed(GTK_TREE_MODEL(self->prv->ncu_list_store),
              path,
              &iter);
            gtk_tree_path_free(path);
        }
        if ( ncu )
            g_object_unref(ncu);
    }
}

static void
row_deleted_cb (GtkTreeModel *model, GtkTreePath *path, gpointer user_data) 
{
    NwamuiNcp     *ncp = NWAMUI_NCP(user_data);

    if ( model == GTK_TREE_MODEL(ncp->prv->ncu_list_store )) {
        g_debug("NCU Removed from List, but not propagated.");
    }
    else if ( model == GTK_TREE_MODEL(ncp->prv->ncu_tree_store )) {
        g_debug("NCU Removed from Tree, but not propagated.");
    }
}

static void
row_inserted_cb(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer user_data)
{
    NwamuiNcp     *ncp = NWAMUI_NCP(user_data);

    if ( model == GTK_TREE_MODEL(ncp->prv->ncu_list_store )) {
        g_debug("NCU Inserted in List, but not propagated.");
    }
    else if ( model == GTK_TREE_MODEL(ncp->prv->ncu_tree_store )) {
        g_debug("NCU Inserted in Tree, but not propagated.");
    }
}

static void
rows_reordered_cb(GtkTreeModel *tree_model, GtkTreePath *path, GtkTreeIter *iter, gpointer arg3, gpointer user_data)   
{
    NwamuiNcp     *ncp = NWAMUI_NCP(user_data);
    gchar         *path_str = gtk_tree_path_to_string(path);

    g_debug("NwamuiNcp: NCU List: Rows Reordered: %s", path_str?path_str:"NULL");

    if ( tree_model == GTK_TREE_MODEL(ncp->prv->ncu_list_store )) {
        g_debug("NCU Re-ordered in List, but not propagated.");
    }
    else if ( tree_model == GTK_TREE_MODEL(ncp->prv->ncu_tree_store )) {
        g_debug("NCU Re-ordered in Tree, but not propagated.");
    }
}

static void
default_activate_ncu_signal_handler (NwamuiNcp *self, NwamuiNcu* ncu, gpointer user_data)
{
    if ( ncu != NULL ) {
        gchar* device_name = nwamui_ncu_get_device_name( ncu );

        g_debug("NCP: activate ncu %s", device_name);

        g_free( device_name );
    }
    else {
        g_debug("NCP: activate ncu NULL");
    }
}

static void
default_deactivate_ncu_signal_handler (NwamuiNcp *self, NwamuiNcu* ncu, gpointer user_data)
{
    gchar* device_name = nwamui_ncu_get_device_name( ncu );

    g_debug("NCP: deactivate ncu %s", device_name);

    g_free( device_name );
}

static void
default_add_ncu_signal_handler (NwamuiNcp* ncp, NwamuiNcu* ncu, gpointer user_data)
{
	g_debug("Create NCU %s", nwamui_ncu_get_display_name(ncu));
}

static void
default_remove_ncu_signal_handler (NwamuiNcp* ncp, NwamuiNcu* ncu, gpointer user_data)
{
	g_debug("Destroy NCU %s", nwamui_ncu_get_display_name(ncu));
}

