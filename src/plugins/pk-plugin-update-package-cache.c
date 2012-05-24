/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2012 Matthias Klumpp <matthias@tenstral.net>
 * Copyright (C) 2011 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <config.h>
#include <gio/gio.h>
#include <pk-plugin.h>
#include <packagekit-glib2/pk-package.h>
#include <packagekit-glib2/pk-package-sack-sync.h>
#include <packagekit-glib2/pk-debug.h>

#include "pk-package-cache.h"

struct PkPluginPrivate {
	GPtrArray		*pkgs;
	GMainLoop		*loop;
};

/**
 * pk_package_sack_find_by_id:
 */
static PkPackage *
pk_plugin_find_package_by_id (PkPlugin *plugin, const gchar *package_id)
{
	PkPackage *package_tmp;
	const gchar *id;
	PkPackage *package = NULL;
	guint i;
	guint len;

	len = plugin->priv->pkgs->len;
	for (i=0; i<len; i++) {
		package_tmp = g_ptr_array_index (plugin->priv->pkgs, i);
		id = pk_package_get_id (package_tmp);
		if (g_strcmp0 (package_id, id) == 0) {
			package = g_object_ref (package_tmp);
			break;
		}
	}

	return package;
}

/**
 * pk_plugin_get_description:
 */
const gchar *
pk_plugin_get_description (void)
{
	return "Maintains a database of all packages for fast read-only access to package information";
}

/**
 * pk_plugin_initialize:
 */
void
pk_plugin_initialize (PkPlugin *plugin)
{
	/* create private area */
	plugin->priv = PK_TRANSACTION_PLUGIN_GET_PRIVATE (PkPluginPrivate);
	plugin->priv->loop = g_main_loop_new (NULL, FALSE);
	plugin->priv->pkgs = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);

	/* use logging */
	pk_debug_add_log_domain (G_LOG_DOMAIN);
	pk_debug_add_log_domain ("PkPkgCache");
}

/**
 * pk_plugin_destroy:
 */
void
pk_plugin_destroy (PkPlugin *plugin)
{
	g_main_loop_unref (plugin->priv->loop);
	g_ptr_array_unref (plugin->priv->pkgs);
}

/**
 * pk_plugin_package_cb:
 **/
static void
pk_plugin_package_cb (PkBackend *backend,
		      PkPackage *package,
		      PkPlugin *plugin)
{
	g_ptr_array_add (plugin->priv->pkgs, g_object_ref (package));
}

/**
 * pk_plugin_details_cb:
 **/
static void
pk_plugin_details_cb (PkBackend *backend,
			PkDetails *item,
			PkPlugin *plugin)
{
	gchar *package_id;
	gchar *description;
	gchar *license;
	gchar *url;
	guint64 size;
	PkGroupEnum group;
	PkPackage *package;

	/* get data */
	g_object_get (item,
		      "package-id", &package_id,
		      "group", &group,
		      "description", &description,
		      "license", &license,
		      "url", &url,
		      "size", &size,
		      NULL);

	/* get package, and set data */
	package = pk_plugin_find_package_by_id (plugin, package_id);
	if (package == NULL) {
		g_warning ("failed to find %s", package_id);
		goto out;
	}

	/* set data */
	g_object_set (package,
		      "license", license,
		      "group", group,
		      "description", description,
		      "url", url,
		      "size", size,
		      NULL);
	g_object_unref (package);

out:
	g_free (package_id);
	g_free (description);
	g_free (license);
	g_free (url);
}

/**
 * pk_plugin_finished_cb:
 **/
static void
pk_plugin_finished_cb (PkBackend *backend,
		       PkExitEnum exit_enum,
		       PkPlugin *plugin)
{
	if (!g_main_loop_is_running (plugin->priv->loop))
		return;
	g_main_loop_quit (plugin->priv->loop);
}

/**
 * pk_plugin_package_array_to_string:
 **/
static gchar *
pk_plugin_package_array_to_string (GPtrArray *array)
{
	guint i;
	PkPackage *package;
	GString *string;
	PkInfoEnum info;
	gchar *package_id;
	gchar *summary;

	string = g_string_new ("");
	for (i=0; i<array->len; i++) {
		package = g_ptr_array_index (array, i);
		g_object_get (package,
			      "info", &info,
			      "package-id", &package_id,
			      "summary", &summary,
			      NULL);
		g_string_append_printf (string, "%s\t%s\t%s\n",
					pk_info_enum_to_string (info),
					package_id,
					summary);
		g_free (package_id);
		g_free (summary);
	}

	/* remove trailing newline */
	if (string->len != 0)
		g_string_set_size (string, string->len-1);
	return g_string_free (string, FALSE);
}

/**
 * pk_package_sack_get_package_ids:
 **/
static gchar **
pk_plugin_get_package_ids (PkPlugin *plugin)
{
	const gchar *id;
	gchar **package_ids;
	const GPtrArray *array;
	PkPackage *package;
	guint i;

	/* create array of package_ids */
	array = plugin->priv->pkgs;
	package_ids = g_new0 (gchar *, array->len+1);
	for (i=0; i<array->len; i++) {
		package = g_ptr_array_index (array, i);
		id = pk_package_get_id (package);
		package_ids[i] = g_strdup (id);
	}

	return package_ids;
}

/**
 * pk_plugin_transaction_finished_end:
 */
void
pk_plugin_transaction_finished_end (PkPlugin *plugin,
				    PkTransaction *transaction)
{
	gboolean ret;
	GError *error = NULL;
	guint finished_sig_id = 0;
	guint package_sig_id = 0;
	guint details_sig_id = 0;
	PkConf *conf;
	PkRoleEnum role;

	PkPackageCache *cache = NULL;
	gchar **package_ids;
	PkPackage *package;
	uint i;
	gchar *data = NULL;
	PkPluginPrivate *priv = plugin->priv;

	/* check the config file */
	conf = pk_transaction_get_conf (transaction);
	ret = pk_conf_get_bool (conf, "UpdatePackageCache");
	if (!ret)
		goto out;

	/* check the role */
	role = pk_transaction_get_role (transaction);
	if (role != PK_ROLE_ENUM_REFRESH_CACHE)
		goto out;

	/* check we can do the action */
	if (!pk_backend_is_implemented (plugin->backend,
	    PK_ROLE_ENUM_GET_PACKAGES)) {
		g_debug ("cannot get packages");
		goto out;
	}

	/* connect to backend */
	finished_sig_id = g_signal_connect (plugin->backend, "finished",
					G_CALLBACK (pk_plugin_finished_cb), plugin);
	package_sig_id = g_signal_connect (plugin->backend, "package",
				       G_CALLBACK (pk_plugin_package_cb), plugin);
	details_sig_id = g_signal_connect (plugin->backend, "details",
				       G_CALLBACK (pk_plugin_details_cb), plugin);

	g_debug ("plugin: recreating package database");

	/* clear old package list */
	if (plugin->priv->pkgs->len > 0)
		g_ptr_array_set_size (plugin->priv->pkgs, 0);

	/* update UI */
	pk_backend_set_status (plugin->backend,
			       PK_STATUS_ENUM_GENERATE_PACKAGE_LIST);
	pk_backend_set_percentage (plugin->backend, 101);

	/* get the new package list */
	pk_backend_reset (plugin->backend);
	pk_backend_get_packages (plugin->backend, PK_FILTER_ENUM_NONE);

	/* wait for finished */
	g_main_loop_run (priv->loop);

	/* update UI */
	pk_backend_set_percentage (plugin->backend, 90);

	/* fetch package details too */
	package_ids = pk_plugin_get_package_ids (plugin);
	pk_backend_get_details (plugin->backend, package_ids);
	g_strfreev (package_ids);

	/* open the package-cache */
	cache = pk_package_cache_new ();
	pk_package_cache_set_filename (cache, PK_SYSTEM_PACKAGE_CACHE_FILENAME, NULL);
	ret = pk_package_cache_open (cache, FALSE, &error);
	if (!ret) {
		g_warning ("%s: %s\n", "Failed to open cache", error->message);
		g_error_free (error);
		goto out;
	}

	/* add packages to cache */
	for (i=0; i<priv->pkgs->len; i++) {
		package = g_ptr_array_index (priv->pkgs, i);
		ret = pk_package_cache_add_package (cache, package, &error);
		if (!ret) {
			g_warning ("%s: %s\n", "Couldn't update cache", error->message);
			g_error_free (error);
			goto out;
		}
	}

	/* convert to a file and save the package list - we require this for backward-compatibility */
	ret = pk_conf_get_bool (conf, "UpdatePackageList");
	if (ret) {
		data = pk_plugin_package_array_to_string (priv->pkgs);

		ret = g_file_set_contents (PK_SYSTEM_PACKAGE_LIST_FILENAME,
					data, -1, &error);
		if (!ret) {
			g_warning ("failed to save to file: %s",
				error->message);
			g_error_free (error);
		}
	}

	/* update UI (finished) */
	pk_backend_set_percentage (plugin->backend, 100);
	pk_backend_set_status (plugin->backend, PK_STATUS_ENUM_FINISHED);

out:
	if (finished_sig_id != 0) {
		g_signal_handler_disconnect (plugin->backend, finished_sig_id);
		g_signal_handler_disconnect (plugin->backend, package_sig_id);
		g_signal_handler_disconnect (plugin->backend, details_sig_id);
	}

	if (cache != NULL) {
		ret = pk_package_cache_close (cache, FALSE, &error);
		if (!ret) {
			g_warning ("%s: %s\n", "Failed to close cache", error->message);
			g_error_free (error);
		}
		g_object_unref (cache);
	}
}