/*
 * main.c
 * Copyright (C) 2015 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
 *
 * gnome-todo is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * gnome-todo is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "gtd-application.h"

#include <config.h>
#include <glib/gi18n.h>

gint
main (gint    argc,
      gchar **argv)
{
  g_autoptr (GtdApplication) app = NULL;
  g_autofree gchar *program_name = NULL;

  bindtextdomain (GETTEXT_PACKAGE, PACKAGE_LOCALE_DIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  program_name = g_strconcat (_("To Do"), NAME_SUFFIX, NULL);
  g_set_application_name (program_name);

  app = gtd_application_new ();
  g_application_set_default (G_APPLICATION (app));

  return g_application_run (G_APPLICATION (app), argc, argv);
}

