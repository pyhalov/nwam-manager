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
 * File:   nwam_wireless_dialog.c
 *
 * Created on May 9, 2007, 11:01 AM
 *
 */

#include <gtk/gtk.h>
#include <glade/glade.h>
#include <glib/gi18n.h>

#include <libnwamui.h>

#include "capplet-utils.h"
#include "nwam_pref_iface.h"
#include "nwam_pref_dialog.h"
#include "nwam_conn_conf_ip_panel.h"
#include "nwam_conn_stat_panel.h"
#include "nwam_net_conf_panel.h"

/* Names of Widgets in Glade file */
#define CAPPLET_DIALOG_NAME		"nwam_capplet"
#define CAPPLET_DIALOG_SHOW_COMBO	"show_combo"
#define CAPPLET_DIALOG_MAIN_NOTEBOOK	"mainview_notebook"
#define CAPPLET_DIALOG_REFRESH_BUTTON	"refresh_button"

#define NWAM_CAPPLET_DIALOG_GET_PRIVATE(o)  (G_TYPE_INSTANCE_GET_PRIVATE ((o), NWAM_TYPE_CAPPLET_DIALOG, NwamCappletDialogPrivate))

struct _NwamCappletDialogPrivate {
	/* Widget Pointers */
	GtkDialog*                  capplet_dialog;
	GtkComboBox*                show_combo;
	GtkNotebook*                main_nb;
	
        /* Panel Objects */
	NwamPrefIFace* panel[N_PANELS];
                
    /* Other Data */
    NwamuiNcp*                  active_ncp; /* currently active NCP */
    GList*                      ncp_list; /* currently active NCP */

    NwamuiNcu*                  selected_ncu;
    NwamuiNcu*                  prev_selected_ncu;
};

static void nwam_pref_init (gpointer g_iface, gpointer iface_data);
static gboolean refresh(NwamPrefIFace *iface, gpointer user_data, gboolean force);
static gboolean apply(NwamPrefIFace *iface, gpointer user_data);
static gboolean cancel(NwamPrefIFace *iface, gpointer user_data);
static gboolean help(NwamPrefIFace *iface, gpointer user_data);
static gint dialog_run(NwamPrefIFace *iface, GtkWindow *parent);
static GtkWindow* dialog_get_window(NwamPrefIFace *iface);

static void nwam_capplet_dialog_finalize(NwamCappletDialog *self);

/* Callbacks */
static void show_combo_cell_cb (GtkCellLayout *cell_layout,
  GtkCellRenderer   *renderer,
  GtkTreeModel      *model,
  GtkTreeIter       *iter,
  gpointer           data);
static gboolean show_combo_separator_cb (GtkTreeModel *model,
  GtkTreeIter *iter,
  gpointer data);

static void object_notify_cb( GObject *gobject, GParamSpec *arg1, gpointer data);
static void response_cb( GtkWidget* widget, gint repsonseid, gpointer data );
static void show_changed_cb( GtkWidget* widget, gpointer data );
static void refresh_clicked_cb( GtkButton *button, gpointer data );


/* Utility Functions */
static void show_comob_add (GtkComboBox* combo, GObject*  obj);
static void show_comob_remove (GtkComboBox* combo, GObject*  obj);
static void update_show_combo_from_ncp( GtkComboBox* combo, NwamuiNcp*  ncp ); /* Unused */


G_DEFINE_TYPE_EXTENDED (NwamCappletDialog,
                        nwam_capplet_dialog,
                        G_TYPE_OBJECT,
                        0,
                        G_IMPLEMENT_INTERFACE (NWAM_TYPE_PREF_IFACE, nwam_pref_init))

static void
nwam_pref_init (gpointer g_iface, gpointer iface_data)
{
	NwamPrefInterface *iface = (NwamPrefInterface *)g_iface;
	iface->refresh = refresh;
	iface->apply = apply;
	iface->cancel = cancel;
    iface->help = help;
    iface->dialog_run = dialog_run;
    iface->dialog_get_window = dialog_get_window;
}

static void
nwam_capplet_dialog_class_init(NwamCappletDialogClass *klass)
{
	/* Pointer to GObject Part of Class */
	GObjectClass *gobject_class = (GObjectClass*) klass;
	
	/* Override Some Function Pointers */
	gobject_class->finalize = (void (*)(GObject*)) nwam_capplet_dialog_finalize;

	g_type_class_add_private(klass, sizeof(NwamCappletDialogPrivate));
}

static void
nwam_capplet_dialog_init(NwamCappletDialog *self)
{
    GtkButton      *btn = NULL;
    NwamuiDaemon   *daemon = NULL;

    self->prv = NWAM_CAPPLET_DIALOG_GET_PRIVATE(self);
    
    /* Iniialise pointers to important widgets */
    self->prv->capplet_dialog = GTK_DIALOG(nwamui_util_glade_get_widget(CAPPLET_DIALOG_NAME));
    self->prv->show_combo = GTK_COMBO_BOX(nwamui_util_glade_get_widget(CAPPLET_DIALOG_SHOW_COMBO));
    self->prv->main_nb = GTK_NOTEBOOK(nwamui_util_glade_get_widget(CAPPLET_DIALOG_MAIN_NOTEBOOK));
        
    /* Get NCPs and Current NCP */
    daemon = nwamui_daemon_get_instance();
    self->prv->active_ncp = nwamui_daemon_get_active_ncp( daemon );
    self->prv->ncp_list = nwamui_daemon_get_ncp_list( daemon );

    self->prv->selected_ncu = NULL;
    self->prv->prev_selected_ncu = NULL;

    if ( self->prv->active_ncp == NULL && self->prv->ncp_list  != NULL ) {
        /* FIXME: Show multiple NCPs */
        GList* first_element = g_list_first( self->prv->ncp_list );


        if ( first_element != NULL && first_element->data != NULL )  {
            self->prv->active_ncp = NWAMUI_NCP(g_object_ref(G_OBJECT(first_element->data)));
        }
        
    }

    g_object_unref( daemon );
    daemon = NULL;

    /* Set title to include hostname */
    nwamui_util_window_title_append_hostname( self->prv->capplet_dialog );

    /* Construct the Notebook Panels Handling objects. */
    self->prv->panel[PANEL_CONN_STATUS] = NWAM_PREF_IFACE(nwam_conn_status_panel_new( self ));
    self->prv->panel[PANEL_NET_PREF] = NWAM_PREF_IFACE(nwam_net_conf_panel_new(self));
    self->prv->panel[PANEL_CONF_IP] = NWAM_PREF_IFACE(nwam_conf_ip_panel_new());

    /* Change Model */
	capplet_compose_combo(self->prv->show_combo,
      GTK_TYPE_TREE_STORE,
      G_TYPE_OBJECT,
      show_combo_cell_cb,
      show_combo_separator_cb,
      show_changed_cb,
      (gpointer)self,
      NULL);

	show_comob_add (self->prv->show_combo, G_OBJECT(self->prv->panel[PANEL_CONN_STATUS]));
	show_comob_add (self->prv->show_combo, G_OBJECT(self->prv->panel[PANEL_NET_PREF])); 

    g_signal_connect(self, "notify", (GCallback)object_notify_cb, NULL);
    g_signal_connect(GTK_DIALOG(self->prv->capplet_dialog), "response", (GCallback)response_cb, (gpointer)self);
    
    btn = GTK_BUTTON(nwamui_util_glade_get_widget(CAPPLET_DIALOG_REFRESH_BUTTON));
    g_signal_connect(btn, "clicked", (GCallback)refresh_clicked_cb, (gpointer)self);

    /* default select "Connection Status" */
    gtk_combo_box_set_active (GTK_COMBO_BOX(self->prv->show_combo), 0);

    /* Initial */
    nwam_pref_refresh(NWAM_PREF_IFACE(self), NULL, TRUE);
}


/**
 * nwam_capplet_dialog_new:
 * @returns: a new #NwamCappletDialog.
 *
 * Creates a new #NwamCappletDialog with an empty ncu
 **/
NwamCappletDialog*
nwam_capplet_dialog_new(void)
{
	/* capplet dialog should be single */
	static NwamCappletDialog *capplet_dialog = NULL;
	if (capplet_dialog) {
		return NWAM_CAPPLET_DIALOG (g_object_ref (G_OBJECT (capplet_dialog)));
	}
	return capplet_dialog = NWAM_CAPPLET_DIALOG(g_object_new(NWAM_TYPE_CAPPLET_DIALOG, NULL));
}

/**
 * nwam_capplet_dialog_run:
 * @nwam_capplet_dialog: a #NwamCappletDialog.
 * @returns: a GTK_DIALOG Response ID
 *
 *
 * Blocks in a recursive main loop until the dialog either emits the response signal, or is destroyed.
 **/
static gint
dialog_run(NwamPrefIFace *iface, GtkWindow *parent)
{
	NwamCappletDialog* self = NWAM_CAPPLET_DIALOG(iface);
    gint response = GTK_RESPONSE_NONE;
    
    g_assert(NWAM_IS_CAPPLET_DIALOG(self));

    if ( self->prv->capplet_dialog != NULL ) {
            response = gtk_dialog_run(GTK_DIALOG(self->prv->capplet_dialog));
            
            gtk_widget_hide(GTK_WIDGET(self->prv->capplet_dialog));
    }
    return response;
}

static GtkWindow* 
dialog_get_window(NwamPrefIFace *iface)
{
	NwamCappletDialog* self = NWAM_CAPPLET_DIALOG(iface);

    if ( self->prv->capplet_dialog ) {
        return ( GTK_WINDOW(self->prv->capplet_dialog) );
    }

    return(NULL);
}

static void
nwam_capplet_dialog_finalize(NwamCappletDialog *self)
{
	if ( self->prv->capplet_dialog != NULL )
		g_object_unref(G_OBJECT(self->prv->capplet_dialog));
	if ( self->prv->show_combo != NULL )
		g_object_unref(G_OBJECT(self->prv->show_combo));
	if ( self->prv->main_nb != NULL )
		g_object_unref(G_OBJECT(self->prv->main_nb));
	
	for (int i = 0; i < N_PANELS; i ++) {
		if (self->prv->panel[i]) {
			g_object_unref(G_OBJECT(self->prv->panel[i]));
		}
	}
	if ( self->prv->active_ncp != NULL )
		g_object_unref(G_OBJECT(self->prv->active_ncp));
	
    g_list_foreach(self->prv->ncp_list, nwamui_util_obj_unref, NULL );

	self->prv = NULL;
	
	G_OBJECT_CLASS(nwam_capplet_dialog_parent_class)->finalize(G_OBJECT(self));
}

/* Callbacks */

/**
 * refresh:
 *
 * Refresh #NwamCappletDialog with the new connections.
 * Including two static enties "Connection Status" and "Network Profile"
 * And dynamic adding connection enties.
 * FIXED ME.
 **/
static gboolean
refresh(NwamPrefIFace *iface, gpointer user_data, gboolean force)
{
    NwamCappletDialogPrivate *prv = NWAM_CAPPLET_DIALOG_GET_PRIVATE(iface);
	NwamCappletDialog* self = NWAM_CAPPLET_DIALOG(iface);
    NwamuiDaemon *daemon = nwamui_daemon_get_instance();
    GtkTreeIter iter;
    GObject *obj;
    GList* ncp_list;
    gint panel_id = (gint)user_data;

    switch(panel_id) {
    case PANEL_CONN_STATUS:
    case PANEL_NET_PREF:
        obj = g_object_ref(prv->panel[panel_id]);
        break;
    default:
        /* Remember the active object and select it later. */
        obj = capplet_combo_get_active_object(GTK_COMBO_BOX(prv->show_combo));
        break;
    }
    g_assert(obj);
    
    /* Refresh show-combo */
    ncp_list = nwamui_daemon_get_ncp_list(daemon);
    g_object_unref(daemon);

    gtk_widget_hide(GTK_WIDGET(prv->show_combo));
    for (; ncp_list;) {
        /* TODO need be more efficient. */
        show_comob_remove(prv->show_combo, G_OBJECT(ncp_list->data));
        show_comob_add(prv->show_combo, G_OBJECT(ncp_list->data));
        ncp_list = g_list_delete_link(ncp_list, ncp_list);
    }

    /* Refresh children */
    capplet_combo_set_active_object(GTK_COMBO_BOX(prv->show_combo), obj);

    gtk_widget_show(GTK_WIDGET(prv->show_combo));

    g_object_unref(obj);

/*     show_changed_cb(GTK_COMBO_BOX(prv->show_combo), (gpointer)self); */
}

static gboolean
cancel(NwamPrefIFace *iface, gpointer user_data)
{
}

static gboolean
apply(NwamPrefIFace *iface, gpointer user_data)
{
	NwamCappletDialog  *self = NWAM_CAPPLET_DIALOG(iface);
    gboolean            rval = TRUE;
    NwamuiDaemon       *daemon = NULL;
    gint                cur_idx = gtk_notebook_get_current_page (NWAM_CAPPLET_DIALOG(self)->prv->main_nb);

    daemon = nwamui_daemon_get_instance();

    /* Ensure we don't have unsaved data */
    if ( !nwam_pref_apply (NWAM_PREF_IFACE(NWAM_CAPPLET_DIALOG(self)->prv->panel[cur_idx]), NULL) ) {
        rval = FALSE;
    }

    if ( self->prv->selected_ncu ) {
        gchar      *prop_name = NULL;

        /* Validate NCU */
        if ( !nwamui_ncu_validate( NWAMUI_NCU(self->prv->selected_ncu), &prop_name ) ) {
            gchar* message = g_strdup_printf(_("An error occurred validating the current NCU.\nThe property '%s' caused this failure"), prop_name );
            nwamui_util_show_message (GTK_WINDOW(self->prv->capplet_dialog), 
                                      GTK_MESSAGE_ERROR, _("Validation Error"), message, TRUE );
            g_free(prop_name);
            rval = FALSE;
        }
    }

    if (rval && !nwamui_daemon_commit_changed_objects( daemon ) ) {
        rval = FALSE;
    }

    return( rval );
}

static gboolean
help(NwamPrefIFace *iface, gpointer user_data)
{
	NwamCappletDialog* self = NWAM_CAPPLET_DIALOG(iface);
    gint idx = gtk_notebook_get_current_page (NWAM_CAPPLET_DIALOG(self)->prv->main_nb);
    nwam_pref_help (NWAM_PREF_IFACE(NWAM_CAPPLET_DIALOG(self)->prv->panel[idx]), NULL);
}

/*
 * According to nwam_pref_iface defined apply, we should do
 * foreach instance apply it, if error, poping up error dialog and keep living
 * otherwise hiding myself.
 */
static void
response_cb( GtkWidget* widget, gint responseid, gpointer data )
{
	NwamCappletDialog* self = NWAM_CAPPLET_DIALOG(data);
    gboolean           stop_emission = FALSE;
	
	switch (responseid) {
		case GTK_RESPONSE_NONE:
			g_debug("GTK_RESPONSE_NONE");
            stop_emission = TRUE;
			break;
		case GTK_RESPONSE_DELETE_EVENT:
			g_debug("GTK_RESPONSE_DELETE_EVENT");
			break;
		case GTK_RESPONSE_OK:
			g_debug("GTK_RESPONSE_OK");
            if (nwam_pref_apply (NWAM_PREF_IFACE(self), NULL)) {
                gtk_widget_hide_all (GTK_WIDGET(self->prv->capplet_dialog));
            }
            else {
                /* TODO - report error to user */
                stop_emission = TRUE;
            }
			break;
		case GTK_RESPONSE_REJECT: /* Generated by Referesh Button */
            /* TODO - Implement refresh functionality */
			g_debug("GTK_RESPONSE_REJECT");
            nwam_pref_refresh(NWAM_PREF_IFACE(self), NULL, TRUE);
            stop_emission = TRUE;
			break;
		case GTK_RESPONSE_CANCEL:
			g_debug("GTK_RESPONSE_CANCEL");
            if (nwam_pref_cancel (NWAM_PREF_IFACE(self), NULL)) {
                gtk_widget_hide_all (GTK_WIDGET(self->prv->capplet_dialog));
            }
			break;
		case GTK_RESPONSE_HELP:
            nwam_pref_help(NWAM_PREF_IFACE(self), NULL);
            stop_emission = TRUE;
			break;
	}
    if ( stop_emission ) {
        g_signal_stop_emission_by_name(widget, "response" );
    }
}

static void
show_changed_cb( GtkWidget* widget, gpointer data )
{
	NwamCappletDialog* self = NWAM_CAPPLET_DIALOG(data);
    GType type;
    GtkTreeModel   *model = NULL;
    GtkTreeIter     iter;
	gint idx;
    GObject *obj;
    gpointer user_data = NULL;

    idx = PANEL_CONF_IP;
        
    model = gtk_combo_box_get_model(GTK_COMBO_BOX(widget));

    /* Get selected NCU and update IP Panel with this information */
    if ((obj = capplet_combo_get_active_object(GTK_COMBO_BOX(widget))) == NULL) {
        /* Caused by updating the content of show combo. */
        return;
    }

    /* Track the previously selected ncu so that we can know when to validate
     * the ncu, specifically when changing from one ncu to another.
     */
    if ( self->prv->prev_selected_ncu != NULL ) {
        g_object_unref(self->prv->prev_selected_ncu);
    }
    self->prv->prev_selected_ncu = self->prv->selected_ncu;
    self->prv->selected_ncu = NULL;

    type = G_OBJECT_TYPE(obj);
    if (type == NWAM_TYPE_CONN_STATUS_PANEL) {
        idx = 0;
    } else if (type == NWAM_TYPE_NET_CONF_PANEL) {
        user_data = g_object_ref(self->prv->active_ncp);
        idx = 1;
    } else if (type == NWAMUI_TYPE_NCU) {
        self->prv->selected_ncu = NWAMUI_NCU(g_object_ref(obj));
        user_data = obj;
        obj = G_OBJECT(self->prv->panel[PANEL_CONF_IP]);
        idx = 2;
    } else {
        g_assert_not_reached();
    }

    if ( self->prv->prev_selected_ncu != NULL &&
         self->prv->selected_ncu != self->prv->prev_selected_ncu ) {
        /* We are switching from an NCU to something else, so we should apply
         * any changes, and attempt to validate. If validation changes, then
         * we need to re-selected the previous item.
         */
        gboolean    valid = TRUE;
        gint        cur_idx = gtk_notebook_get_current_page (NWAM_CAPPLET_DIALOG(self)->prv->main_nb);
        gchar      *prop_name = NULL;

        if ( !nwam_pref_apply (NWAM_PREF_IFACE(NWAM_CAPPLET_DIALOG(self)->prv->panel[cur_idx]), NULL) ) {
            /* Don't change selection */
            valid = FALSE;
        }

        /* Validate NCU */
        if ( !nwamui_ncu_validate( NWAMUI_NCU(self->prv->prev_selected_ncu), &prop_name ) ) {
            gchar* message = g_strdup_printf(_("An error occurred validating the current NCU.\nThe property '%s' caused this failure"), prop_name );
            nwamui_util_show_message (GTK_WINDOW(self->prv->capplet_dialog), 
                                      GTK_MESSAGE_ERROR, _("Validation Error"), message, TRUE );
            g_free(prop_name);
            /* Don't change selection */
            valid = FALSE;
        }
        if ( ! valid ) {
            nwam_capplet_dialog_select_ncu(self, self->prv->prev_selected_ncu );
            /* Reset selected_ncu, otherwise we will enter a validation loop */
            self->prv->selected_ncu = NWAMUI_NCU(g_object_ref(self->prv->prev_selected_ncu));
        }
    }

	/* update the notetab according to the selected entry */
    gtk_notebook_set_current_page(self->prv->main_nb, idx);

    nwam_pref_refresh(NWAM_PREF_IFACE(obj), user_data, FALSE);

    if (user_data) {
        g_object_unref(user_data);
    }
}

static gboolean
show_combo_separator_cb (GtkTreeModel *model,
                         GtkTreeIter *iter,
                         gpointer data)
{
	gpointer item = NULL;
	gtk_tree_model_get(model, iter, 0, &item, -1);
	if (item != NULL) {
        g_object_unref(item);
        return FALSE;
    } else {
        return TRUE; /* separator */
    }
}

static void
show_combo_cell_cb (GtkCellLayout *cell_layout,
			GtkCellRenderer   *renderer,
			GtkTreeModel      *model,
			GtkTreeIter       *iter,
			gpointer           data)
{
	gpointer row_data = NULL;
	gchar *text = NULL;
	
	gtk_tree_model_get(model, iter, 0, &row_data, -1);
	
	if (row_data == NULL) {
		return;
	}
        
	g_assert (G_IS_OBJECT (row_data));
	
	if (NWAMUI_IS_NCU (row_data)) {
		text = nwamui_ncu_get_display_name(NWAMUI_NCU(row_data));
		g_object_set(renderer,
          "text", text,
          "sensitive", TRUE,
          NULL);
		g_free (text);
	} else if (NWAMUI_IS_OBJECT (row_data)) {
        gchar *markup;
		text = nwamui_object_get_name(NWAMUI_OBJECT(row_data));
        markup = g_strdup_printf("<b><i>%s</i></b>", text);
		g_object_set(renderer,
          "markup", markup,
          "sensitive", FALSE,
          NULL);
		g_free (text);
		g_free (markup);
    } else if (NWAM_IS_CONN_STATUS_PANEL (row_data)) {
		text = _("Connection Status");
		g_object_set(renderer,
          "text", text,
          "sensitive", TRUE,
          NULL);
	} else if (NWAM_IS_NET_CONF_PANEL( row_data )) {
		text = _("Network Profile");
		g_object_set(renderer,
          "text", text,
          "sensitive", TRUE,
          NULL);
	} else {
		g_assert_not_reached();
	}
    g_object_unref(row_data);
}

static void
object_notify_cb( GObject *gobject, GParamSpec *arg1, gpointer data)
{
	g_debug("NwamCappletDialog: notify %s changed", arg1->name);
}

/*
 * According to nwam_pref_iface defined refresh, we should do
 * foreach instance refresh it.
 */
static void
refresh_clicked_cb( GtkButton *button, gpointer data )
{
	NwamCappletDialog* self = NWAM_CAPPLET_DIALOG(data);
    /* Refresh self */
	nwam_pref_refresh (NWAM_PREF_IFACE(self), NULL, TRUE);
}

/*
 * Utility functions.
 */
static void
show_comob_add(GtkComboBox* combo, GObject*  obj)
{
	GtkTreeIter iter;
	GtkTreeModel *model = gtk_combo_box_get_model(combo);

    gtk_tree_store_append (GTK_TREE_STORE(model), &iter, NULL);

    if (NWAMUI_IS_NCP(obj)) {
        GList *ncu_list;
        /* Make the previous line become a separator. */
        gtk_tree_store_append (GTK_TREE_STORE(model), &iter, NULL);
        gtk_tree_store_set (GTK_TREE_STORE(model), &iter, 0, obj, -1);
        ncu_list = nwamui_ncp_get_ncu_list(NWAMUI_NCP(obj));
        for (; ncu_list;) {
            gtk_tree_store_append(GTK_TREE_STORE(model), &iter, NULL);
            gtk_tree_store_set(GTK_TREE_STORE(model), &iter, 0, ncu_list->data, -1);
            ncu_list = g_list_delete_link(ncu_list, ncu_list);
        }
    } else {
        gtk_tree_store_set (GTK_TREE_STORE(model), &iter, 0, obj, -1);
    }
}

static void
show_comob_remove(GtkComboBox* combo, GObject*  obj)
{
	GtkTreeModel *model = gtk_combo_box_get_model(combo);
	GtkTreeIter parent;
    GObject *data;

    if (capplet_model_find_object(model, obj, &parent)) {
        /* If it is NCP, remove child NCUs. */
        if (NWAMUI_IS_NCP(obj)) {
            GtkTreeIter iter;
            GtkTreePath *path;

            /* Delete the separator. */
            path = gtk_tree_model_get_path(model, &parent);
            if (gtk_tree_path_prev(path)) {
                gtk_tree_model_get_iter(model, &iter, path);
                gtk_tree_store_remove(GTK_TREE_STORE(model), &iter);
            }
            gtk_tree_path_free(path);

            /* Delete NCUs. */
            if(gtk_tree_store_remove(GTK_TREE_STORE(model), &parent))
                do {
                    gtk_tree_model_get(model, &parent, 0, &data, -1);
                    if (data)
                        g_object_unref(G_OBJECT(data));
                    else
                        break;
                } while (gtk_tree_store_remove(GTK_TREE_STORE(model), &parent));


        } else {
            /* Remove whatever it is. */
            gtk_tree_store_remove(GTK_TREE_STORE(model), &parent);
        }
    }
}

static gboolean
add_ncu_element(    GtkTreeModel *model,
                    GtkTreePath *path,
                    GtkTreeIter *iter,
                    gpointer user_data)
{
	NwamuiNcu*      ncu = NULL;
	GtkListStore*   combo_model = GTK_LIST_STORE(user_data);
    GtkTreeIter     new_iter;
    gchar *name;
	
  	gtk_tree_model_get(model, iter, 0, &ncu, -1);

    gtk_list_store_append(GTK_LIST_STORE(combo_model), &new_iter);
    gtk_list_store_set(GTK_LIST_STORE(combo_model), &new_iter, 0, ncu, -1);
    
    g_object_unref( ncu );
    
    return( FALSE ); /* FALSE = Continue Processing */
}

static void     
update_show_combo_from_ncp( GtkComboBox* combo, NwamuiNcp*  ncp )
{
	GtkTreeIter     iter;
	GtkTreeModel   *model = NULL;
	gboolean        has_next;
	
	g_return_if_fail( combo != NULL && ncp != NULL );

	/* Update list of connections in "show_combo" */
	model = gtk_combo_box_get_model(GTK_COMBO_BOX(combo));
	
	/*
	 TODO - Check if there is a better way to handle updating combo box than remove/add all items.
	 */
	/* Remove items 3 and later ( 0 = Connect Status; 1 = Network configuration; 2 = Separator ) */
	if (gtk_tree_model_get_iter_from_string(model, &iter, "3")) {
		do {
			has_next = gtk_tree_store_remove(GTK_LIST_STORE(model), &iter);
		} while (has_next);
	}
	/* Now Add Entries for NCP Enabled NCUs */
    nwamui_ncp_foreach_ncu(ncp, add_ncu_element, (gpointer)model);
            
    g_object_unref(model);
}

/*
 * Select the given NCU in the show_combo box.
 */
extern void
nwam_capplet_dialog_select_ncu(NwamCappletDialog  *self, NwamuiNcu*  ncu )
{
    GtkTreeIter     iter;
    GtkTreeModel   *model = NULL;
    gboolean        has_next;

    model = gtk_combo_box_get_model(GTK_COMBO_BOX(self->prv->show_combo));
    
    for( has_next = gtk_tree_model_get_iter_first( GTK_TREE_MODEL( model ), &iter );
         has_next;
         has_next = gtk_tree_model_iter_next( GTK_TREE_MODEL( model ), &iter ) ) {
        NwamuiNcu*   current_ncu = NULL;

        gtk_tree_model_get(GTK_TREE_MODEL(model), &iter, 0, &current_ncu, -1);
        if ( current_ncu != NULL && current_ncu == ncu ) {
            gtk_combo_box_set_active_iter(GTK_COMBO_BOX(self->prv->show_combo), &iter );
            g_object_unref( current_ncu );
            break;
        }
    }
}


