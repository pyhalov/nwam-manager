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
 */
#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#include "libnwamui.h"
#include "nwam_tree_view.h"

#include "status_icon.h"
#include "notify.h"

#define DETECT_ICON_EMBEDDING_INTERVAL 2 /* In seconds */

/* Command-line options */
static gboolean debug = FALSE;
static gboolean notify_reuse = FALSE;
static gboolean notify_create_always = FALSE;
static gboolean notify_create_nostatus = FALSE;

static GOptionEntry option_entries[] = {
    {"debug", 'D', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &debug, N_("Enable debugging messages"), NULL },
    {"notify-reuse", 'a', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &notify_reuse, N_("Always re-use notification message"), NULL },
    {"notify-create-always", 'a', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &notify_create_always, N_("Always create notification message, rather than re-use"), NULL },
    {"notify-create-always-nostatus", 'n', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &notify_create_nostatus, N_("Always create notification message, rather than re-use, and don't link to status icon"), NULL },
    {NULL}
};

static void 
cleanup_and_exit(int sig, siginfo_t *sip, void *data)
{
    gtk_main_quit ();
}

static gboolean
init_wait_for_embedding_idle(gpointer data)
{
    GtkStatusIcon* status_icon = GTK_STATUS_ICON(data);

    if (gtk_status_icon_is_embedded(status_icon)) {
        nwam_status_icon_run(NWAM_STATUS_ICON(status_icon));
        return FALSE;
    }
    return TRUE;
}

static gboolean
init_wait_for_embedding(gpointer data)
{
    /* Run it first because we may avoid the first interval. */
    if (init_wait_for_embedding_idle(data)) {
        g_timeout_add_seconds_full(G_PRIORITY_DEFAULT,
          DETECT_ICON_EMBEDDING_INTERVAL,
          init_wait_for_embedding_idle,
          g_object_ref(data),
          g_object_unref);
    }
    return( TRUE );
}

int
main( int argc, char* argv[] )
{
    GnomeProgram *program;
    GOptionContext *option_context;
    GnomeClient *client;
    struct sigaction act;
    NwamuiProf* prof;
    GtkStatusIcon *status_icon = NULL;

    option_context = g_option_context_new (PACKAGE);

    bindtextdomain (GETTEXT_PACKAGE, NWAM_MANAGER_LOCALEDIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);
    g_option_context_add_main_entries(option_context, option_entries, GETTEXT_PACKAGE);

    program = gnome_program_init (PACKAGE, VERSION, LIBGNOMEUI_MODULE,
                                  argc, argv,
                                  GNOME_PARAM_APP_DATADIR, NWAM_MANAGER_DATADIR,
                                  GNOME_PARAM_GOPTION_CONTEXT, option_context,
                                  GNOME_PARAM_NONE);


    gtk_icon_theme_append_search_path (gtk_icon_theme_get_default (),
                           NWAM_MANAGER_DATADIR G_DIR_SEPARATOR_S "icons");


    /* Setup log handler to trap debug messages */
    nwamui_util_default_log_handler_init();

    nwamui_util_set_debug_mode( debug );

    if ( notify_reuse ) {
        notify_notification_set_notification_style( NOTIFICATION_STYLE_REUSE );
    }
    if ( notify_create_always ) {
        notify_notification_set_notification_style( NOTIFICATION_STYLE_CREATE_ALWAYS );
    }

    if ( notify_create_nostatus ) {
        notify_notification_set_notification_style( NOTIFICATION_STYLE_CREATE_ALWAYS_NO_STATUS_ICON );
    }

    act.sa_sigaction = cleanup_and_exit;
    sigemptyset (&act.sa_mask);
    act.sa_flags = SA_SIGINFO;
    sigaction (SIGINT, &act, NULL);
    sigaction (SIGPIPE, &act, NULL);
    sigaction (SIGKILL, &act, NULL);
    sigaction (SIGTERM, &act, NULL);

#if 0
    if (!nwamui_prof_check_ui_auth(nwamui_prof_get_instance_noref(), UI_AUTH_LEAST)) {
        g_warning("User doesn't have the enough authorisations to run nwam-manager.");
        exit(0);
    }
#endif

    gtk_init( &argc, &argv );

    if ( !nwamui_util_is_debug_mode() ) {
	GApplication *app = NULL;
	GError *error = NULL;

	app = g_application_new("com.sun.nwam-manager", 0);
	g_application_register (G_APPLICATION (app), NULL, &error);
	if (error != NULL) {
	    g_warning ("Unable to register GApplication: %s", error->message);
	    g_error_free (error);
	    error = NULL;
	}
	if (g_application_get_is_remote (G_APPLICATION (app))) {
            nwamui_util_show_message( NULL, GTK_MESSAGE_INFO, _("NWAM Manager"),
                                      _("\nAnother instance is running.\nThis instance will exit now."), TRUE );
            g_debug("Another instance is running, exiting.");
	    g_object_unref (app);
	    exit(0);
	}
	g_object_unref (app);
    }
    else {
        g_debug("Uniqueness is disabled while in debug mode.");
    }

    NWAM_TYPE_TREE_VIEW; /* Dummy to cause NwamTreeView to be loaded for GtkBuilder to find symbol */

    
    /* Setup function to be called after the mainloop has been created
     * this is to avoid confusion when calling gtk_main_iteration to get to
     * the point where the status icon's embedded flag is correctly set
     */
    status_icon = nwam_status_icon_new();
    gtk_init_add(init_wait_for_embedding, (gpointer)status_icon);
    if ( nwamui_util_is_debug_mode() ) {
        g_message("Show status icon initially on debug mode.");
    }

    gtk_main();

    g_debug ("exiting...");

    g_object_unref(status_icon);
    g_object_unref (G_OBJECT (program));
    
    return 0;
}
