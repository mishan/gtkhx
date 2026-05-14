/*
 * tray.h — Linux system-tray integration via StatusNotifierItem.
 *
 * Build context: GtkStatusIcon is gone in GTK 4. The de-facto modern
 * replacement is the StatusNotifierItem (SNI) D-Bus protocol — what
 * KDE Plasma, Cinnamon, MATE, XFCE, and (via the AppIndicator
 * extension) GNOME Shell all consume.
 *
 * We implement SNI directly via GDBus instead of depending on
 * libayatana-appindicator, for two reasons:
 *
 *   1. Zero new build deps. GDBus comes with GIO which we already
 *      link transitively through GTK 4.
 *   2. libayatana-appindicator's public menu API still takes a
 *      GtkMenu (GTK 3); using it from a GTK 4 process means linking
 *      both toolkits or accepting a fragile interop story.
 *
 * The companion DBusMenu protocol (com.canonical.dbusmenu) is also
 * implemented inline here so the right-click menu items
 * (Show/Hide, Connect…, Disconnect, Quit) render natively in the
 * compositor's tray host.
 *
 * Public surface: lifecycle (init / shutdown), runtime enable/disable
 * driven by the TRAY pref, and a small notification API the rest of
 * GtkHx uses to react to connection-state changes (so the menu's
 * Connect / Disconnect items enable/disable in lockstep with the
 * toolbar buttons).
 */

#ifndef HX_TRAY_H
#define HX_TRAY_H

#include <gtk/gtk.h>

/* Allocate the SNI object on the session bus and register with the
 * watcher. Idempotent; safe to call before a host is around (we
 * stay registered and become visible when one shows up). Pulls the
 * initial enabled state from gtkhx_prefs.tray. Call once at app
 * startup, after the GtkApplication has activated. */
extern void gtkhx_tray_init (GtkApplication *app);

/* Unregister and free everything. Safe to call multiple times. */
extern void gtkhx_tray_shutdown (void);

/* Pref toggle (Settings → General → "Show tray icon"). Switching ON
 * registers; switching OFF unregisters but keeps the rest of the
 * module alive so it can be re-enabled without re-initialising. */
extern void gtkhx_tray_set_enabled (gboolean enabled);
extern gboolean gtkhx_tray_is_enabled (void);

/* Has a tray host (KDE / GNOME-AppIndicator-extension / waybar / etc.)
 * registered itself with StatusNotifierWatcher? When FALSE there's
 * nowhere for our icon to render — useful for the toolbar's
 * hide-to-tray decision so we don't disappear into nothing. */
extern gboolean gtkhx_tray_host_available (void);

/* Show / hide / toggle every GtkApplication-owned window in one shot.
 * Used by the SNI Activate callback (left click on the tray icon)
 * and the menu's "Show GtkHx" / "Hide GtkHx" items. On show, calls
 * gtk_window_present() on each window — bringing-to-current-
 * workspace is then up to the compositor (KDE / GNOME via xdg-
 * activation honour it; on a minimal Wayland compositor it's
 * best-effort). */
extern void gtkhx_tray_show_all_windows (void);
extern void gtkhx_tray_hide_all_windows (void);
extern void gtkhx_tray_toggle_windows (void);
extern gboolean gtkhx_tray_any_window_visible (void);

/* Drive the Connect…/Disconnect menu items' sensitivity from the
 * connection-state-changed signal. */
extern void gtkhx_tray_set_connected (gboolean connected);

#endif /* HX_TRAY_H */
