/*
 * tray.c — StatusNotifierItem + DBusMenu, hand-rolled over GDBus.
 *
 * See tray.h for the build-context rationale (why hand-rolled
 * instead of libayatana-appindicator).
 *
 * Protocol layout:
 *
 *   - We own a unique well-known bus name
 *     "org.kde.StatusNotifierItem-<pid>-1" on the session bus.
 *   - At /StatusNotifierItem we expose org.kde.StatusNotifierItem,
 *     publishing IconName / Title / Status / Category / ItemIsMenu /
 *     Menu properties and Activate / SecondaryActivate / ContextMenu /
 *     Scroll methods. The watcher (org.kde.StatusNotifierWatcher) is
 *     the rendezvous: we call its RegisterStatusNotifierItem() with
 *     our bus name; the tray host (kded, plasma-panel, gnome-shell-
 *     extension-appindicator, waybar's tray module, etc.) is its
 *     client.
 *   - At /MenuBar we expose com.canonical.dbusmenu, a tiny tree of
 *     four items (Show/Hide, Connect, Disconnect, Quit) whose
 *     properties we serve via GetLayout / GetGroupProperties and
 *     whose clicks we receive via Event.
 *
 * Reference: https://www.freedesktop.org/wiki/Specifications/StatusNotifierItem/
 *            https://github.com/AyatanaIndicators/libdbusmenu/blob/master/libdbusmenu-glib/dbus-menu.xml
 *
 * Lifetime: the SNI object is registered when the TRAY pref is on
 * AND we successfully name-own. If the watcher isn't around at init
 * time we still own the name and publish the object; the watcher
 * (when it appears) can discover us, or the user can re-trigger by
 * toggling the pref. Watcher-discovery via NameOwnerChanged would be
 * nicer but adds complexity and matters only on session-start race
 * orderings.
 */

#include "config.h"
#include <gtk/gtk.h>
#include <gio/gio.h>
#include "tray.h"
#include "session.h"
#include "prefs.h"
#include "connect.h"
#include "toolbar.h"

/* ---- D-Bus introspection XML ---------------------------------------- */

/* StatusNotifierItem — minimum spec subset we care about. Older /
 * larger SNI hosts ignore properties they don't recognise, so
 * keeping this lean is harmless. */
static const char SNI_INTROSPECTION_XML[]
    = "<node>"
      "  <interface name='org.kde.StatusNotifierItem'>"
      "    <property name='Category'      type='s'    access='read'/>"
      "    <property name='Id'            type='s'    access='read'/>"
      "    <property name='Title'         type='s'    access='read'/>"
      "    <property name='Status'        type='s'    access='read'/>"
      "    <property name='IconName'      type='s'    access='read'/>"
      "    <property name='IconThemePath' type='s'    access='read'/>"
      "    <property name='ItemIsMenu'    type='b'    access='read'/>"
      "    <property name='Menu'          type='o'    access='read'/>"
      "    <method name='Activate'>"
      "      <arg name='x' type='i' direction='in'/>"
      "      <arg name='y' type='i' direction='in'/>"
      "    </method>"
      "    <method name='SecondaryActivate'>"
      "      <arg name='x' type='i' direction='in'/>"
      "      <arg name='y' type='i' direction='in'/>"
      "    </method>"
      "    <method name='ContextMenu'>"
      "      <arg name='x' type='i' direction='in'/>"
      "      <arg name='y' type='i' direction='in'/>"
      "    </method>"
      "    <method name='Scroll'>"
      "      <arg name='delta'       type='i' direction='in'/>"
      "      <arg name='orientation' type='s' direction='in'/>"
      "    </method>"
      "    <signal name='NewTitle'/>"
      "    <signal name='NewIcon'/>"
      "    <signal name='NewStatus'>"
      "      <arg name='status' type='s'/>"
      "    </signal>"
      "  </interface>"
      "</node>";

/* DBusMenu — also a subset. GetLayout returns the full menu tree
 * via a recursive variant tuple; we only have one root with four
 * leaf children, so the implementation is small. */
static const char DBUSMENU_INTROSPECTION_XML[]
    = "<node>"
      "  <interface name='com.canonical.dbusmenu'>"
      "    <property name='Version' type='u' access='read'/>"
      "    <property name='Status'  type='s' access='read'/>"
      "    <property name='TextDirection' type='s' access='read'/>"
      "    <property name='IconThemePath' type='as' access='read'/>"
      "    <method name='GetLayout'>"
      "      <arg name='parentId'           type='i'        direction='in'/>"
      "      <arg name='recursionDepth'     type='i'        direction='in'/>"
      "      <arg name='propertyNames'      type='as'       direction='in'/>"
      "      <arg name='revision'           type='u'        direction='out'/>"
      "      <arg name='layout'             type='(ia{sv}av)' direction='out'/>"
      "    </method>"
      "    <method name='GetGroupProperties'>"
      "      <arg name='ids'                type='ai'       direction='in'/>"
      "      <arg name='propertyNames'      type='as'       direction='in'/>"
      "      <arg name='properties'         type='a(ia{sv})' direction='out'/>"
      "    </method>"
      "    <method name='GetProperty'>"
      "      <arg name='id'                 type='i'        direction='in'/>"
      "      <arg name='name'               type='s'        direction='in'/>"
      "      <arg name='value'              type='v'        direction='out'/>"
      "    </method>"
      "    <method name='Event'>"
      "      <arg name='id'                 type='i'        direction='in'/>"
      "      <arg name='eventId'            type='s'        direction='in'/>"
      "      <arg name='data'               type='v'        direction='in'/>"
      "      <arg name='timestamp'          type='u'        direction='in'/>"
      "    </method>"
      "    <method name='AboutToShow'>"
      "      <arg name='id'                 type='i'        direction='in'/>"
      "      <arg name='needUpdate'         type='b'        direction='out'/>"
      "    </method>"
      "    <signal name='ItemsPropertiesUpdated'>"
      "      <arg name='updatedProps' type='a(ia{sv})'/>"
      "      <arg name='removedProps' type='a(ias)'/>"
      "    </signal>"
      "    <signal name='LayoutUpdated'>"
      "      <arg name='revision' type='u'/>"
      "      <arg name='parent'   type='i'/>"
      "    </signal>"
      "  </interface>"
      "</node>";

/* ---- Menu item IDs --------------------------------------------------- */

#define MENU_ROOT 0
#define MENU_SHOWHIDE 1
#define MENU_CONNECT 2
#define MENU_DISCONNECT 3
#define MENU_QUIT 4

/* ---- Module state ---------------------------------------------------- */

static GtkApplication *tray_app;
static gboolean tray_enabled;
static gboolean tray_host_present;
static gboolean tray_connected;

/* GDBus bookkeeping */
static GDBusConnection *tray_bus;
static guint tray_name_id;     /* g_bus_own_name return */
static guint tray_sni_reg_id;  /* SNI object registration */
static guint tray_menu_reg_id; /* DBusMenu object registration */
static char *tray_bus_name;    /* "org.kde.StatusNotifierItem-<pid>-1" */
static GDBusNodeInfo *tray_sni_node;
static GDBusNodeInfo *tray_menu_node;
static guint tray_menu_revision = 1;

/* Watcher proxy + signal handler — when a new host shows up we
 * re-register, when the watcher itself appears/disappears we track
 * tray_host_present. */
static GDBusProxy *tray_watcher_proxy;
static gulong tray_watcher_owner_sigid;
static gulong tray_watcher_host_registered_sigid;

/* ---- Forward decls --------------------------------------------------- */

static void tray_activate_register (void);
static void tray_unregister (void);
static void tray_watcher_setup (void);
static void tray_watcher_teardown (void);
static void on_watcher_signal (GDBusProxy *p, const char *sender,
                               const char *signal, GVariant *params,
                               gpointer u);
static void on_watcher_owner (GDBusProxy *p, GParamSpec *pspec, gpointer u);
/* Our menu shape is fixed (Show/Hide, Connect, Disconnect, Quit) for
 * the life of the process, so we never emit LayoutUpdated — only
 * per-item ItemsPropertiesUpdated as Show/Hide flips or the
 * connection state changes the enabled-ness of Connect/Disconnect. */
static void emit_menu_props_updated (int id);

/* ---- Tiny helpers: window enumeration ------------------------------- */

/* The connect dialog / about / preferences are AdwDialog windows
 * (not toplevels under app->windows on GTK 4 ≥ 4.10). For
 * gtk_application_get_windows() the toolbar / chat / news / users /
 * tasks / etc. all qualify. We hide them with gtk_widget_set_visible
 * (preserves window position on hide, restores on show). */

static GList *
tray_managed_windows (void)
{
    if (!tray_app) {
        return NULL;
    }
    return g_list_copy (gtk_application_get_windows (tray_app));
}

gboolean
gtkhx_tray_any_window_visible (void)
{
    GList *wins, *l;
    gboolean any = FALSE;

    wins = tray_managed_windows ();
    for (l = wins; l; l = l->next) {
        if (gtk_widget_get_visible (GTK_WIDGET (l->data))) {
            any = TRUE;
            break;
        }
    }
    g_list_free (wins);
    return any;
}

void
gtkhx_tray_show_all_windows (void)
{
    GList *wins, *l;

    wins = tray_managed_windows ();
    for (l = wins; l; l = l->next) {
        GtkWidget *w = GTK_WIDGET (l->data);
        gtk_widget_set_visible (w, TRUE);
        /* gtk_window_present requests user attention; on
		 * compositors with xdg-activation this brings the
		 * window onto the current workspace and raises it. */
        gtk_window_present (GTK_WINDOW (w));
    }
    g_list_free (wins);
    emit_menu_props_updated (MENU_SHOWHIDE);
}

void
gtkhx_tray_hide_all_windows (void)
{
    GList *wins, *l;

    wins = tray_managed_windows ();
    for (l = wins; l; l = l->next) {
        gtk_widget_set_visible (GTK_WIDGET (l->data), FALSE);
    }
    g_list_free (wins);
    emit_menu_props_updated (MENU_SHOWHIDE);
}

void
gtkhx_tray_toggle_windows (void)
{
    if (gtkhx_tray_any_window_visible ()) {
        gtkhx_tray_hide_all_windows ();
    } else {
        gtkhx_tray_show_all_windows ();
    }
}

/* ---- StatusNotifierItem method / property handlers ------------------ */

static void
sni_method_call (GDBusConnection *conn, const char *sender,
                 const char *object_path, const char *interface,
                 const char *method, GVariant *params,
                 GDBusMethodInvocation *invocation, gpointer user_data)
{
    (void)conn;
    (void)sender;
    (void)object_path;
    (void)interface;
    (void)params;
    (void)user_data;

    if (g_strcmp0 (method, "Activate") == 0) {
        gtkhx_tray_toggle_windows ();
        g_dbus_method_invocation_return_value (invocation, NULL);
        return;
    }
    if (g_strcmp0 (method, "SecondaryActivate") == 0
        || g_strcmp0 (method, "ContextMenu") == 0
        || g_strcmp0 (method, "Scroll") == 0) {
        /* No-op: ContextMenu is served by the host via our Menu
		 * property pointing at /MenuBar. SecondaryActivate and
		 * Scroll have no useful meaning for us. */
        g_dbus_method_invocation_return_value (invocation, NULL);
        return;
    }
    g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                           G_DBUS_ERROR_UNKNOWN_METHOD,
                                           "Unknown method %s", method);
}

static GVariant *
sni_get_property (GDBusConnection *conn, const char *sender,
                  const char *object_path, const char *interface,
                  const char *property, GError **error, gpointer user_data)
{
    (void)conn;
    (void)sender;
    (void)object_path;
    (void)interface;
    (void)user_data;

    if (g_strcmp0 (property, "Category") == 0) {
        return g_variant_new_string ("Communications");
    }
    if (g_strcmp0 (property, "Id") == 0) {
        return g_variant_new_string ("com.nasledov.gtkhx");
    }
    if (g_strcmp0 (property, "Title") == 0) {
        return g_variant_new_string ("GtkHx");
    }
    if (g_strcmp0 (property, "Status") == 0) {
        return g_variant_new_string ("Active");
    }
    if (g_strcmp0 (property, "IconName") == 0) {
        /* Falls back to the bundled gresource icon. Hosts query
		 * the icon theme using this name; we registered the
		 * gresource icon path under the same name at startup
		 * (gtkhx.c::gtkhx_activate). */
        return g_variant_new_string ("com.nasledov.gtkhx");
    }
    if (g_strcmp0 (property, "IconThemePath") == 0) {
        return g_variant_new_string ("");
    }
    if (g_strcmp0 (property, "ItemIsMenu") == 0) {
        return g_variant_new_boolean (FALSE);
    }
    if (g_strcmp0 (property, "Menu") == 0) {
        return g_variant_new_object_path ("/MenuBar");
    }

    g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
                 "No such property %s", property);
    return NULL;
}

static const GDBusInterfaceVTable sni_vtable
    = { sni_method_call,
        sni_get_property,
        NULL, /* set_property — all read-only */
        { NULL } };

/* ---- DBusMenu: build the static menu tree --------------------------- */

/* Per-item property table. id 0 is the root (no label, just children);
 * 1..4 are the user-visible items. The "enabled" / "visible" /
 * "label" trio is the dbusmenu spec's minimum set; toolkits know
 * how to render off of just those. */
static GVariant *
menu_item_properties (int id)
{
    GVariantBuilder b;
    const char *label = NULL;
    gboolean enabled = TRUE;
    gboolean visible = TRUE;
    const char *type = NULL;
    gboolean children_as_submenu = FALSE;

    switch (id) {
    case MENU_ROOT:
        children_as_submenu = TRUE;
        break;
    case MENU_SHOWHIDE:
        label = gtkhx_tray_any_window_visible () ? "Hide GtkHx" : "Show GtkHx";
        break;
    case MENU_CONNECT:
        label = "Connect…";
        enabled = !tray_connected;
        break;
    case MENU_DISCONNECT:
        label = "Disconnect";
        enabled = tray_connected;
        break;
    case MENU_QUIT:
        label = "Quit";
        break;
    default:
        break;
    }

    g_variant_builder_init (&b, G_VARIANT_TYPE ("a{sv}"));
    if (label) {
        g_variant_builder_add (&b, "{sv}", "label",
                               g_variant_new_string (label));
    }
    g_variant_builder_add (&b, "{sv}", "enabled",
                           g_variant_new_boolean (enabled));
    g_variant_builder_add (&b, "{sv}", "visible",
                           g_variant_new_boolean (visible));
    if (type) {
        g_variant_builder_add (&b, "{sv}", "type", g_variant_new_string (type));
    }
    if (children_as_submenu) {
        g_variant_builder_add (&b, "{sv}", "children-display",
                               g_variant_new_string ("submenu"));
    }
    return g_variant_builder_end (&b);
}

/* Build the "(ia{sv}av)" recursive layout for the given root id. We
 * only have one nesting level (root → leaves) so the recursion stops
 * at depth 1. */
static GVariant *
menu_build_layout (int id, int depth)
{
    GVariantBuilder children;

    g_variant_builder_init (&children, G_VARIANT_TYPE ("av"));

    if (id == MENU_ROOT && depth != 0) {
        int kids[]
            = { MENU_SHOWHIDE, MENU_CONNECT, MENU_DISCONNECT, MENU_QUIT };
        gsize i;
        int sub_depth = (depth < 0) ? -1 : depth - 1;
        for (i = 0; i < G_N_ELEMENTS (kids); i++) {
            g_variant_builder_add_value (
                &children,
                g_variant_new_variant (menu_build_layout (kids[i], sub_depth)));
        }
    }

    return g_variant_new ("(i@a{sv}av)", id, menu_item_properties (id),
                          &children);
}

/* ---- DBusMenu method handlers --------------------------------------- */

static void
menu_method_call (GDBusConnection *conn, const char *sender,
                  const char *object_path, const char *interface,
                  const char *method, GVariant *params,
                  GDBusMethodInvocation *invocation, gpointer user_data)
{
    (void)conn;
    (void)sender;
    (void)object_path;
    (void)interface;
    (void)user_data;

    if (g_strcmp0 (method, "GetLayout") == 0) {
        gint32 parent_id, recursion_depth;
        GVariant *layout;

        g_variant_get (params, "(ii^a&s)", &parent_id, &recursion_depth, NULL);
        layout = menu_build_layout (parent_id, recursion_depth);
        g_dbus_method_invocation_return_value (
            invocation,
            g_variant_new ("(u@(ia{sv}av))", tray_menu_revision, layout));
        return;
    }

    if (g_strcmp0 (method, "GetGroupProperties") == 0) {
        GVariantIter *iter;
        gint32 id;
        GVariantBuilder out;

        g_variant_builder_init (&out, G_VARIANT_TYPE ("a(ia{sv})"));
        g_variant_get (params, "(ai^a&s)", &iter, NULL);
        while (g_variant_iter_loop (iter, "i", &id)) {
            g_variant_builder_add (&out, "(i@a{sv})", id,
                                   menu_item_properties (id));
        }
        g_variant_iter_free (iter);

        g_dbus_method_invocation_return_value (
            invocation, g_variant_new ("(a(ia{sv}))", &out));
        return;
    }

    if (g_strcmp0 (method, "GetProperty") == 0) {
        gint32 id;
        const char *name;
        GVariant *props, *value;
        GVariantIter iter;
        const char *k;
        GVariant *v;

        g_variant_get (params, "(i&s)", &id, &name);
        props = menu_item_properties (id);
        g_variant_iter_init (&iter, props);
        value = NULL;
        while (g_variant_iter_next (&iter, "{&sv}", &k, &v)) {
            if (g_strcmp0 (k, name) == 0) {
                value = v;
                break;
            }
            g_variant_unref (v);
        }
        g_variant_unref (props);
        if (!value) {
            value = g_variant_new_string ("");
        }
        g_dbus_method_invocation_return_value (invocation,
                                               g_variant_new ("(v)", value));
        return;
    }

    if (g_strcmp0 (method, "Event") == 0) {
        gint32 id;
        const char *event_id;

        /* Spec: Event(IN i id, IN s eventId, IN v data,
		 * IN u timestamp). The previous '(i&sva{sv}u)' had a
		 * stray a{sv} between the variant and the timestamp;
		 * g_variant_get hit the type mismatch and bailed before
		 * extracting event_id, so every click was silently
		 * ignored. */
        g_variant_get (params, "(i&svu)", &id, &event_id, NULL, NULL);
        if (g_strcmp0 (event_id, "clicked") == 0) {
            switch (id) {
            case MENU_SHOWHIDE:
                gtkhx_tray_toggle_windows ();
                break;
            case MENU_CONNECT:
                gtkhx_tray_show_all_windows ();
                create_connect_window (NULL, hx_active_session ());
                break;
            case MENU_DISCONNECT:
                disconnect_clicked ();
                break;
            case MENU_QUIT:
                if (tray_app) {
                    g_application_quit (G_APPLICATION (tray_app));
                }
                break;
            }
        }
        g_dbus_method_invocation_return_value (invocation, NULL);
        return;
    }

    if (g_strcmp0 (method, "AboutToShow") == 0) {
        /* needUpdate=false — properties update via the
		 * ItemsPropertiesUpdated signal as state changes, so the
		 * host never has to refetch. */
        g_dbus_method_invocation_return_value (invocation,
                                               g_variant_new ("(b)", FALSE));
        return;
    }

    g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                           G_DBUS_ERROR_UNKNOWN_METHOD,
                                           "Unknown method %s", method);
}

static GVariant *
menu_get_property (GDBusConnection *conn, const char *sender,
                   const char *object_path, const char *interface,
                   const char *property, GError **error, gpointer user_data)
{
    (void)conn;
    (void)sender;
    (void)object_path;
    (void)interface;
    (void)user_data;

    if (g_strcmp0 (property, "Version") == 0) {
        return g_variant_new_uint32 (3);
    }
    if (g_strcmp0 (property, "Status") == 0) {
        return g_variant_new_string ("normal");
    }
    if (g_strcmp0 (property, "TextDirection") == 0) {
        return g_variant_new_string ("ltr");
    }
    if (g_strcmp0 (property, "IconThemePath") == 0) {
        const char *empty[] = { NULL };
        return g_variant_new_strv (empty, 0);
    }
    g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
                 "No such property %s", property);
    return NULL;
}

static const GDBusInterfaceVTable menu_vtable
    = { menu_method_call, menu_get_property, NULL, { NULL } };

/* ---- Signal emitters ------------------------------------------------ */

static void
emit_menu_props_updated (int id)
{
    GVariantBuilder updated, removed;

    if (!tray_bus) {
        return;
    }

    g_variant_builder_init (&updated, G_VARIANT_TYPE ("a(ia{sv})"));
    g_variant_builder_add (&updated, "(i@a{sv})", id,
                           menu_item_properties (id));
    g_variant_builder_init (&removed, G_VARIANT_TYPE ("a(ias)"));

    g_dbus_connection_emit_signal (
        tray_bus, NULL, "/MenuBar", "com.canonical.dbusmenu",
        "ItemsPropertiesUpdated",
        g_variant_new ("(a(ia{sv})a(ias))", &updated, &removed), NULL);
}

/* ---- Watcher registration ------------------------------------------- */

static void
on_register_done (GObject *src, GAsyncResult *res, gpointer u)
{
    GError *err = NULL;
    GVariant *r;

    (void)u;
    r = g_dbus_proxy_call_finish (G_DBUS_PROXY (src), res, &err);
    if (err) {
        /* Watcher not around — common on Wayland GNOME without
		 * the AppIndicator extension. Stay registered on the
		 * bus; the watcher can pick us up later if it shows up
		 * (its IsStatusNotifierHostRegistered notifications will
		 * trigger our re-register path). */
        tray_host_present = FALSE;
        g_clear_error (&err);
        return;
    }
    tray_host_present = TRUE;
    g_clear_pointer (&r, g_variant_unref);
}

static void
tray_register_with_watcher (void)
{
    if (!tray_watcher_proxy || !tray_bus_name) {
        return;
    }
    g_dbus_proxy_call (tray_watcher_proxy, "RegisterStatusNotifierItem",
                       g_variant_new ("(s)", tray_bus_name),
                       G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_register_done,
                       NULL);
}

static void
on_watcher_proxy_ready (GObject *src, GAsyncResult *res, gpointer u)
{
    GError *err = NULL;

    (void)src;
    (void)u;
    tray_watcher_proxy = g_dbus_proxy_new_for_bus_finish (res, &err);
    if (err) {
        g_clear_error (&err);
        return;
    }

    tray_watcher_owner_sigid
        = g_signal_connect (tray_watcher_proxy, "notify::g-name-owner",
                            G_CALLBACK (on_watcher_owner), NULL);
    tray_watcher_host_registered_sigid = g_signal_connect (
        tray_watcher_proxy, "g-signal", G_CALLBACK (on_watcher_signal), NULL);

    /* If the proxy already has an owner, the watcher is live —
	 * register. */
    {
        char *owner = g_dbus_proxy_get_name_owner (tray_watcher_proxy);
        if (owner) {
            g_free (owner);
            tray_register_with_watcher ();
        }
    }
}

static void
on_watcher_signal (GDBusProxy *p, const char *sender, const char *signal,
                   GVariant *params, gpointer u)
{
    (void)p;
    (void)sender;
    (void)params;
    (void)u;
    if (g_strcmp0 (signal, "StatusNotifierHostRegistered") == 0) {
        /* A host just appeared — re-register in case we beat
		 * it to the punch at startup. */
        tray_host_present = TRUE;
        tray_register_with_watcher ();
    }
    if (g_strcmp0 (signal, "StatusNotifierHostUnregistered") == 0) {
        tray_host_present = FALSE;
    }
}

static void
on_watcher_owner (GDBusProxy *p, GParamSpec *pspec, gpointer u)
{
    char *owner;

    (void)pspec;
    (void)u;
    owner = g_dbus_proxy_get_name_owner (p);
    if (owner) {
        g_free (owner);
        tray_register_with_watcher ();
    } else {
        tray_host_present = FALSE;
    }
}

static void
tray_watcher_setup (void)
{
    if (tray_watcher_proxy) {
        return;
    }
    g_dbus_proxy_new_for_bus (
        G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL,
        "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
        "org.kde.StatusNotifierWatcher", NULL, on_watcher_proxy_ready, NULL);
}

static void
tray_watcher_teardown (void)
{
    if (tray_watcher_proxy && tray_watcher_owner_sigid) {
        g_signal_handler_disconnect (tray_watcher_proxy,
                                     tray_watcher_owner_sigid);
    }
    if (tray_watcher_proxy && tray_watcher_host_registered_sigid) {
        g_signal_handler_disconnect (tray_watcher_proxy,
                                     tray_watcher_host_registered_sigid);
    }
    tray_watcher_owner_sigid = 0;
    tray_watcher_host_registered_sigid = 0;
    g_clear_object (&tray_watcher_proxy);
}

/* ---- Object registration -------------------------------------------- */

static void
on_bus_acquired (GDBusConnection *conn, const char *name, gpointer u)
{
    GError *err = NULL;
    (void)name;
    (void)u;

    tray_bus = g_object_ref (conn);

    tray_sni_node = g_dbus_node_info_new_for_xml (SNI_INTROSPECTION_XML, NULL);
    tray_menu_node
        = g_dbus_node_info_new_for_xml (DBUSMENU_INTROSPECTION_XML, NULL);

    tray_sni_reg_id = g_dbus_connection_register_object (
        conn, "/StatusNotifierItem", tray_sni_node->interfaces[0], &sni_vtable,
        NULL, NULL, &err);
    if (err) {
        g_warning ("tray: SNI register failed: %s", err->message);
        g_clear_error (&err);
    }

    tray_menu_reg_id = g_dbus_connection_register_object (
        conn, "/MenuBar", tray_menu_node->interfaces[0], &menu_vtable, NULL,
        NULL, &err);
    if (err) {
        g_warning ("tray: menu register failed: %s", err->message);
        g_clear_error (&err);
    }
}

static void
on_name_acquired (GDBusConnection *conn, const char *name, gpointer u)
{
    (void)conn;
    (void)name;
    (void)u;
    tray_watcher_setup ();
}

static void
on_name_lost (GDBusConnection *conn, const char *name, gpointer u)
{
    (void)name;
    (void)u;
    if (!conn) {
        /* Couldn't connect to session bus at all. Nothing to
		 * do; user environment probably doesn't have one. */
        return;
    }
}

static void
tray_activate_register (void)
{
    if (tray_name_id) {
        return;
    }
    g_free (tray_bus_name);
    tray_bus_name
        = g_strdup_printf ("org.kde.StatusNotifierItem-%d-1", getpid ());
    tray_name_id = g_bus_own_name (G_BUS_TYPE_SESSION, tray_bus_name,
                                   G_BUS_NAME_OWNER_FLAGS_NONE, on_bus_acquired,
                                   on_name_acquired, on_name_lost, NULL, NULL);
}

static void
tray_unregister (void)
{
    if (tray_bus && tray_sni_reg_id) {
        g_dbus_connection_unregister_object (tray_bus, tray_sni_reg_id);
        tray_sni_reg_id = 0;
    }
    if (tray_bus && tray_menu_reg_id) {
        g_dbus_connection_unregister_object (tray_bus, tray_menu_reg_id);
        tray_menu_reg_id = 0;
    }
    g_clear_pointer (&tray_sni_node, g_dbus_node_info_unref);
    g_clear_pointer (&tray_menu_node, g_dbus_node_info_unref);
    if (tray_name_id) {
        g_bus_unown_name (tray_name_id);
        tray_name_id = 0;
    }
    g_clear_object (&tray_bus);
    g_clear_pointer (&tray_bus_name, g_free);
    tray_watcher_teardown ();
    tray_host_present = FALSE;
}

/* ---- Public API ----------------------------------------------------- */

void
gtkhx_tray_init (GtkApplication *app)
{
    if (tray_app) {
        return;
    }
    tray_app = app;
    /* Pull the initial pref straight out of gtkhx_prefs here so the
	 * caller doesn't have to remember to do it. Subsequent changes
	 * arrive via gtkhx_tray_set_enabled() from the changed_tray
	 * cfgvar callback. */
    tray_enabled = gtkhx_prefs.tray ? TRUE : FALSE;
    if (tray_enabled) {
        tray_activate_register ();
    }
}

void
gtkhx_tray_shutdown (void)
{
    tray_unregister ();
    tray_app = NULL;
}

void
gtkhx_tray_set_enabled (gboolean enabled)
{
    if (tray_enabled == enabled) {
        return;
    }
    tray_enabled = enabled;
    if (!tray_app) {
        return;
    }
    if (enabled) {
        tray_activate_register ();
    } else {
        tray_unregister ();
    }
}

gboolean
gtkhx_tray_is_enabled (void)
{
    return tray_enabled;
}

gboolean
gtkhx_tray_host_available (void)
{
    return tray_host_present;
}

void
gtkhx_tray_set_connected (gboolean connected)
{
    if (tray_connected == connected) {
        return;
    }
    tray_connected = connected;
    emit_menu_props_updated (MENU_CONNECT);
    emit_menu_props_updated (MENU_DISCONNECT);
}
