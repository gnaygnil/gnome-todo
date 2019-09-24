/* gtd-plugin-background.c
 *
 * Copyright (C) 2017 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define G_LOG_DOMAIN "GtdPluginBackground"

#include "config.h"

#include "gtd-debug.h"
#include "gtd-plugin-background.h"

#include <glib/gi18n.h>
#include <gdk/gdk.h>
#include <gtk/gtk.h>

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/x11/gdkx.h>
#endif

#define AUTOSTART_NOTIFICATION_ID      "Gtd::BackgroundPlugin::autostart_notification"
#define AUTOSTART_NOTIFICATION_TIMEOUT 3  /* seconds */
#define MAX_BODY_LENGTH                50 /* chars */

#define DESKTOP_PORTAL_BUS_NAME "org.freedesktop.portal.Desktop"
#define DESKTOP_PORTAL_OBJECT_PATH "/org/freedesktop/portal/desktop"
#define DESKTOP_PORTAL_BACKGROUND_INTERFACE "org.freedesktop.portal.Background"

struct _GtdPluginBackground
{
  PeasExtensionBase   parent;

  GtkWidget          *preferences_panel;

  GDBusProxy         *desktop_portal;

  GSettings          *settings;
  gboolean            startup_notification;
  gboolean            show_notifications;

  gchar              *surface_handle;
  gboolean            enabled;
  gboolean            autostart;

  guint               startup_notification_timeout_id;
};

static void          on_tasklist_notified                        (GtdPluginBackground      *self);

static void          gtd_activatable_iface_init                  (GtdActivatableInterface  *iface);

G_DEFINE_DYNAMIC_TYPE_EXTENDED (GtdPluginBackground, gtd_plugin_background, PEAS_TYPE_EXTENSION_BASE,
                                0,
                                G_IMPLEMENT_INTERFACE_DYNAMIC (GTD_TYPE_ACTIVATABLE,
                                                               gtd_activatable_iface_init))

enum {
  PROP_0,
  PROP_PREFERENCES_PANEL,
  N_PROPS
};

/*
 * Auxiliary methods
 */

static inline GtkWindow*
get_window (void)
{
  GtkApplication *app = GTK_APPLICATION (g_application_get_default ());

  return GTK_WINDOW (gtk_application_get_active_window (app));
}

static void
on_request_signal_emitted_cb (GDBusProxy          *request_proxy,
                              gchar               *sender_name,
                              gchar               *signal_name,
                              GVariant            *parameters,
                              GtdPluginBackground *self)
{
  if (g_strcmp0 (signal_name, "Response") == 0)
    {
      guint response;

      g_variant_get (parameters, "u", &response);

      g_message ("Response: %u", response);
    }

  g_object_unref (request_proxy);
}

static void
update_background_portal (GtdPluginBackground *self)
{
  g_autoptr (GDBusProxy) request_proxy = NULL;
  g_autoptr (GVariant) result = NULL;
  g_autoptr (GError) error = NULL;
  gchar *request_object_path;
  GVariantBuilder builder;

  GTD_ENTRY;

  if (!self->desktop_portal)
    {
      self->desktop_portal = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                            G_DBUS_PROXY_FLAGS_NONE,
                                                            NULL,
                                                            "org.freedesktop.portal.Desktop",
                                                            "/org/freedesktop/portal/desktop",
                                                            "org.freedesktop.portal.Background",
                                                            NULL,
                                                            &error);

      if (error)
        {
          g_warning ("Failed to connect to the desktop portal: %s", error->message);
          GTD_RETURN ();
        }
    }

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&builder, "{sv}", "autostart", g_variant_new_boolean (self->autostart));
  if (self->autostart)
    {
      const gchar * commands[] = { "gnome-todo", "--gapplication-service", NULL };

      g_variant_builder_add (&builder, "{sv}", "commandline", g_variant_new_strv (commands, -1));
    }

  g_assert (self->surface_handle != NULL);

  result = g_dbus_proxy_call_sync (self->desktop_portal,
                                   "RequestBackground",
                                   g_variant_new ("(sa{sv})", self->surface_handle, &builder),
                                   G_DBUS_CALL_FLAGS_NONE,
                                   30000,
                                   NULL,
                                   &error);
  if (error)
    {
      g_warning ("Error calling D-Bus method: %s", error->message);
      GTD_RETURN ();
    }

  g_variant_get (result, "(o)", &request_object_path);
  request_proxy = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                 G_DBUS_PROXY_FLAGS_NONE,
                                                 NULL,
                                                 "org.freedesktop.portal.Desktop",
                                                 request_object_path,
                                                 "org.freedesktop.portal.Request",
                                                 NULL,
                                                 &error);

  if (error)
    {
      g_warning ("Error calling D-Bus method: %s", error->message);
      GTD_RETURN ();
    }

  g_signal_connect_object (g_steal_pointer (&request_proxy),
                           "g-signal",
                           G_CALLBACK (on_request_signal_emitted_cb),
                           self,
                           0);

  GTD_EXIT;
}

#ifdef GDK_WINDOWING_WAYLAND
static void
on_wayland_surface_handle_exported_cb (GdkSurface  *surface,
                                       const gchar *handle,
                                       gpointer     user_data)
{
  GtdPluginBackground *self = GTD_PLUGIN_BACKGROUND (user_data);

  GTD_ENTRY;

  self->surface_handle = g_strdup_printf ("wayland:%s", handle);

  update_background_portal (self);

  g_clear_pointer (&self->surface_handle, g_free);
  //gdk_wayland_surface_unexport_handle (surface);

  GTD_EXIT;
}
#endif

static void
acquire_surface_handle (GtdPluginBackground *self)
{
  GdkSurface *surface;
  GtkWindow *window;

  GTD_ENTRY;

  window = get_window ();
  surface = gtk_native_get_surface (GTK_NATIVE (window));

  g_assert (surface != NULL);

#ifdef GDK_WINDOWING_WAYLAND
  if (GDK_IS_WAYLAND_SURFACE (surface))
    {
      gdk_wayland_surface_export_handle (surface, on_wayland_surface_handle_exported_cb, self, NULL);
    }
  else
#endif
#ifdef GDK_WINDOWING_X11
  if (GDK_IS_X11_SURFACE (surface))
    {
      self->surface_handle = g_strdup_printf ("x11:%lx", gdk_x11_surface_get_xid (surface));
      update_background_portal (self);
      g_clear_pointer (&self->surface_handle, g_free);
    }
  else
#endif
    g_critical ("No valid backend");

  GTD_EXIT;
}

static void
on_window_mapped_cb (GtkWindow           *window,
                     GtdPluginBackground *self)
{
  GTD_ENTRY;

  g_signal_handlers_disconnect_by_func (window, on_window_mapped_cb, self);
  acquire_surface_handle (self);

  GTD_EXIT;
}

static void
start_update (GtdPluginBackground *self)
{
  GtkWindow *window;

  GTD_ENTRY;

  window = get_window ();
  if (gtk_widget_get_visible (GTK_WIDGET (window)))
    acquire_surface_handle (self);
  else
    g_signal_connect_object (window, "map", G_CALLBACK (on_window_mapped_cb), self, G_CONNECT_AFTER);

  GTD_EXIT;
}

static gchar*
format_notification_body (GList *tasks,
                          guint  n_tasks)
{
  GString *string, *aux;
  GList *l;
  guint current_task, length;

  aux = g_string_new ("");
  string = g_string_new ("");
  current_task = length = 0;

  for (l = tasks; l != NULL; l = l->next)
    {
      GtdTask *task = l->data;

      length += g_utf8_strlen (gtd_task_get_title (task), -1);

      if (length > MAX_BODY_LENGTH)
        break;

      /* Prepend comma */
      if (current_task > 0)
        g_string_append (aux, ", ");

      g_string_append (aux, gtd_task_get_title (task));
      current_task++;
    }

  if (current_task == 0)
    {
      /* The first task has a huge title. Let's ellipsize it */
      g_string_append (string, gtd_task_get_title (tasks->data));
      g_string_truncate (string, MAX_BODY_LENGTH - 1);
      g_string_append (string, "…");
    }
  else if (current_task < n_tasks)
    {
      /* Some tasks fit, but we need to append a text explaining there are more */
      g_string_append_printf (string,
                              g_dngettext (GETTEXT_PACKAGE,
                                           "%1$s and one more task",
                                           "%1$s and %2$d other tasks",
                                           n_tasks - current_task),
                              aux->str,
                              n_tasks - current_task);
    }
  else
    {
      /* We were able to print all task titles */
      g_string_append (string, aux->str);
    }

  g_string_free (aux, TRUE);

  return g_string_free (string, FALSE);
}

static inline gboolean
is_today (GDateTime *now,
          GDateTime *dt)
{
  return g_date_time_get_year (dt) == g_date_time_get_year (now) &&
         g_date_time_get_month (dt) == g_date_time_get_month (now) &&
         g_date_time_get_day_of_month (dt) == g_date_time_get_day_of_month (now);
}

static GList*
get_tasks_for_today (guint *n_events)
{
  g_autoptr (GDateTime) now;
  GListModel *lists;
  GtdManager *manager;
  GList *result;
  guint n_tasks;
  guint i;

  now = g_date_time_new_now_local ();
  result = NULL;
  n_tasks = 0;
  manager = gtd_manager_get_default ();
  lists = gtd_manager_get_task_lists_model (manager);

  for (i = 0; i < g_list_model_get_n_items (lists); i++)
    {
      g_autoptr (GListModel) list = NULL;
      guint j;

      list = g_list_model_get_item (lists, i);

      for (j = 0; j < g_list_model_get_n_items (list); j++)
        {
          GDateTime *due_date;
          GtdTask *task;

          task = g_list_model_get_item (list, j);

          due_date = gtd_task_get_due_date (task);

          if (!due_date || !is_today (now, due_date) || gtd_task_get_complete (task))
            continue;

          n_tasks += 1;
          result = g_list_prepend (result, task);
        }
    }

  if (n_events)
    *n_events = n_tasks;

  return result;
}

static void
send_notification (GtdPluginBackground *self)
{
  GNotification *notification;
  GApplication *app;
  GtdWindow *window;
  guint n_tasks;
  GList *tasks;
  gchar *title;
  gchar *body;

  window = GTD_WINDOW (get_window ());

  /*
   * If the user already focused To Do's window, we don't have to
   * notify about the number of tasks.
   */
  if (gtk_window_is_active (GTK_WINDOW (window)))
    return;

  /* The user don't want to be bothered with notifications */
  if (!g_settings_get_boolean (self->settings, "show-notifications"))
    return;

  app = g_application_get_default ();
  tasks = get_tasks_for_today (&n_tasks);

  /* If n_tasks == 0, tasks == NULL, thus we don't need to free it */
  if (n_tasks == 0)
    return;

  title = g_strdup_printf (g_dngettext (GETTEXT_PACKAGE,
                                        "You have %d task for today",
                                        "You have %d tasks for today",
                                        n_tasks),
                           n_tasks);

  body = format_notification_body (tasks, n_tasks);

  /* Build up the notification */
  notification = g_notification_new (title);
  g_notification_set_body (notification, body);
  g_notification_set_default_action (notification, "app.activate");

  g_application_send_notification (app, AUTOSTART_NOTIFICATION_ID, notification);

  g_clear_pointer (&tasks, g_list_free);
  g_clear_object (&notification);
}

/*
 * Callbacks
 */
static void
on_startup_changed (GSettings           *settings,
                    const gchar         *key,
                    GtdPluginBackground *self)
{
  self->autostart = g_settings_get_boolean (settings, key);

  start_update (self);
}

static gboolean
on_startup_timeout_cb (GtdPluginBackground *self)
{
  send_notification (self);

  self->startup_notification_timeout_id = 0;

  g_signal_handlers_disconnect_by_func (gtd_manager_get_default (),
                                        on_tasklist_notified,
                                        self);

  return G_SOURCE_REMOVE;
}

static void
on_tasklist_notified (GtdPluginBackground *self)
{
  /* Remove previously set timeout */
  if (self->startup_notification_timeout_id > 0)
    {
      g_source_remove (self->startup_notification_timeout_id);
      self->startup_notification_timeout_id = 0;
    }

  self->startup_notification_timeout_id = g_timeout_add_seconds (AUTOSTART_NOTIFICATION_TIMEOUT,
                                                                 (GSourceFunc) on_startup_timeout_cb,
                                                                 self);
}

static void
watch_manager_for_new_lists (GtdPluginBackground *self)
{
  GtdManager *manager = gtd_manager_get_default ();

  g_signal_connect_swapped (manager,
                            "list-added",
                            G_CALLBACK (on_tasklist_notified),
                            self);

  g_signal_connect_swapped (manager,
                            "list-changed",
                            G_CALLBACK (on_tasklist_notified),
                            self);

  g_signal_connect_swapped (manager,
                            "list-removed",
                            G_CALLBACK (on_tasklist_notified),
                            self);

  g_signal_connect_swapped (gtd_manager_get_clock (manager),
                            "day-changed",
                            G_CALLBACK (send_notification),
                            self);
}

/*
 * GtdActivatable interface implementation
 */
static void
gtd_plugin_background_activate (GtdActivatable *activatable)
{
  GtdPluginBackground *self;
  GtkWindow *window;

  self = GTD_PLUGIN_BACKGROUND (activatable);
  window = get_window ();

  self->enabled = TRUE;

  gtk_window_set_hide_on_close (window, TRUE);

  on_startup_changed (self->settings, "run-on-startup", self);
  g_signal_connect (self->settings,
                    "changed::run-on-startup",
                    G_CALLBACK (on_startup_changed),
                    self);

  /* Start watching the manager to notify the user about today's tasks */
  watch_manager_for_new_lists (self);
}

static void
gtd_plugin_background_deactivate (GtdActivatable *activatable)
{
  GtdPluginBackground *self;
  GtdManager *manager;
  GtkWindow *window;

  self = GTD_PLUGIN_BACKGROUND (activatable);
  manager = gtd_manager_get_default ();
  window = get_window ();

  self->enabled = FALSE;
  self->autostart = FALSE;

  gtk_window_set_hide_on_close (window, FALSE);

  g_signal_handlers_disconnect_by_func (self->settings,
                                        on_startup_changed,
                                        self);

  g_signal_handlers_disconnect_by_func (manager,
                                        on_tasklist_notified,
                                        self);

  g_signal_handlers_disconnect_by_func (gtd_manager_get_clock (manager),
                                        send_notification,
                                        self);

  /* Deactivate the timeout */
  if (self->startup_notification_timeout_id > 0)
    {
      g_source_remove (self->startup_notification_timeout_id);
      self->startup_notification_timeout_id = 0;
    }

  /* Deactivate auto startup */
  start_update (self);
}

static GList*
gtd_plugin_background_get_header_widgets (GtdActivatable *activatable)
{
  return NULL;
}

static GtkWidget*
gtd_plugin_background_get_preferences_panel (GtdActivatable *activatable)
{
  GtdPluginBackground *self = GTD_PLUGIN_BACKGROUND (activatable);

  return self->preferences_panel;
}

static GList*
gtd_plugin_background_get_panels (GtdActivatable *activatable)
{
  return NULL;
}

static GList*
gtd_plugin_background_get_providers (GtdActivatable *activatable)
{
  return NULL;
}

static void
gtd_activatable_iface_init (GtdActivatableInterface *iface)
{
  iface->activate = gtd_plugin_background_activate;
  iface->deactivate = gtd_plugin_background_deactivate;
  iface->get_header_widgets = gtd_plugin_background_get_header_widgets;
  iface->get_preferences_panel = gtd_plugin_background_get_preferences_panel;
  iface->get_panels = gtd_plugin_background_get_panels;
  iface->get_providers = gtd_plugin_background_get_providers;
}

static void
gtd_plugin_background_finalize (GObject *object)
{
  GtdPluginBackground *self = (GtdPluginBackground *)object;

  g_clear_object (&self->settings);

  G_OBJECT_CLASS (gtd_plugin_background_parent_class)->finalize (object);
}

static void
gtd_plugin_background_get_property (GObject    *object,
                                    guint       prop_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
  GtdPluginBackground *self = GTD_PLUGIN_BACKGROUND (object);

  switch (prop_id)
    {
    case PROP_PREFERENCES_PANEL:
      g_value_set_object (value, self->preferences_panel);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gtd_plugin_background_set_property (GObject      *object,
                                       guint         prop_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
}

static void
gtd_plugin_background_class_init (GtdPluginBackgroundClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gtd_plugin_background_finalize;
  object_class->get_property = gtd_plugin_background_get_property;
  object_class->set_property = gtd_plugin_background_set_property;

  g_object_class_override_property (object_class,
                                    PROP_PREFERENCES_PANEL,
                                    "preferences-panel");
}

static void
gtd_plugin_background_init (GtdPluginBackground *self)
{
  GtkBuilder *builder;

  /* Load the settings */
  self->settings = g_settings_new ("org.gnome.todo.plugins.background");

  /* And the preferences panel */
  builder = gtk_builder_new_from_resource ("/org/gnome/todo/ui/background/preferences.ui");

  self->preferences_panel = GTK_WIDGET (gtk_builder_get_object (builder, "main_frame"));

  g_settings_bind (self->settings,
                   "run-on-startup",
                   gtk_builder_get_object (builder, "startup_switch"),
                   "active",
                   G_SETTINGS_BIND_DEFAULT);

  g_settings_bind (self->settings,
                   "show-notifications",
                   gtk_builder_get_object (builder, "notifications_switch"),
                   "active",
                   G_SETTINGS_BIND_DEFAULT);
}

static void
gtd_plugin_background_class_finalize (GtdPluginBackgroundClass *klass)
{
}

G_MODULE_EXPORT void
gtd_plugin_background_register_types (PeasObjectModule *module)
{
  gtd_plugin_background_register_type (G_TYPE_MODULE (module));

  peas_object_module_register_extension_type (module,
                                              GTD_TYPE_ACTIVATABLE,
                                              GTD_TYPE_PLUGIN_BACKGROUND);
}
