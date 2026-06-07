/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * chat_tabs.c — implementation of the Chat panel's internal tab
 * strip.
 *
 * See chat_tabs.h for the API rationale.
 */

#include "config.h"

#include "chat_tabs.h"
#include "panel_registry.h"
#include "hx_panel.h"

#include <libpanel.h>

/* The singleton tab view. NULL until gtkhx_chat_tabs_init() runs
 * (called from create_chat_window). */
static AdwTabView *the_tab_view = NULL;

/* Index tables: cid → AdwTabPage* and uid → AdwTabPage*. Borrowed
 * pointers: AdwTabView owns the page; we just need the lookup. The
 * pages are removed from these tables in the close-page handler
 * before the page is destroyed. */
static GHashTable *pchat_tabs = NULL;  /* guint32 cid → AdwTabPage* */
static GHashTable *msg_tabs   = NULL;  /* guint16 uid → AdwTabPage* */

/* User-installed teardown handlers. NULL until set by chat.c / msg.c. */
static ChatTabsClosePchatFunc on_close_pchat = NULL;
static ChatTabsCloseMsgFunc   on_close_msg   = NULL;

/* Per-page identification. We stash these via g_object_set_data on
 * the AdwTabPage so close-page can recover the cid/uid in O(1).
 *
 *   "chat-tab-kind" → GINT_TO_POINTER (one of the values below).
 *   "chat-tab-id"   → GUINT_TO_POINTER (cid for pchat, uid for msg).
 *
 * Public-chat pages carry kind=PUBLIC and id=0; the close-page
 * handler sees those and bails out (it's pinned so close shouldn't
 * fire anyway, but the early-return is defensive). */
typedef enum {
    CHAT_TAB_KIND_PUBLIC = 1,
    CHAT_TAB_KIND_PCHAT,
    CHAT_TAB_KIND_MSG,
} ChatTabKind;

static void
ensure_tables (void)
{
    if (!pchat_tabs)
        pchat_tabs = g_hash_table_new (g_direct_hash, g_direct_equal);
    if (!msg_tabs)
        msg_tabs   = g_hash_table_new (g_direct_hash, g_direct_equal);
}

/* AdwTabView::close-page handler. Fires synchronously when the user
 * clicks the tab's X (or any other path that ends up at
 * adw_tab_view_close_page). The contract is:
 *
 *   - Return GDK_EVENT_STOP and call adw_tab_view_close_page_finish
 *     to confirm the close.
 *   - Tear down the backing state inside the page kind's close
 *     handler (which removes from sess->gchats / sess->msg_windows
 *     etc.).
 *   - Remove our index entry.
 *
 * AdwTabView then detaches the page and destroys the content
 * widget tree. */
static gboolean
on_close_page (AdwTabView *view, AdwTabPage *page, gpointer user_data)
{
    ChatTabKind kind;
    guint id;

    (void)user_data;

    /* The dispatch keys off the page's stored kind/id, not its
     * child widget — earlier we bailed when the child was NULL,
     * but a teardown path that already unparented the child can
     * leave the page alive with a still-meaningful id; bailing
     * here would leak the pchat_tabs / msg_tabs entry and skip
     * the registered teardown handler. Always read kind/id and
     * dispatch. */
    kind = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (page),
                                               "chat-tab-kind"));
    id   = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (page),
                                                "chat-tab-id"));

    switch (kind) {
    case CHAT_TAB_KIND_PUBLIC:
        /* Public chat is pinned so the user can't actually close it
         * through the tab strip. Defensive: if some future code path
         * triggers it, decline. */
        adw_tab_view_close_page_finish (view, page, FALSE);
        return GDK_EVENT_STOP;

    case CHAT_TAB_KIND_PCHAT:
        if (pchat_tabs)
            g_hash_table_remove (pchat_tabs, GUINT_TO_POINTER (id));
        if (on_close_pchat)
            on_close_pchat ((guint32)id);
        break;

    case CHAT_TAB_KIND_MSG:
        if (msg_tabs)
            g_hash_table_remove (msg_tabs, GUINT_TO_POINTER (id));
        if (on_close_msg)
            on_close_msg ((guint16)id);
        break;

    default:
        /* Unknown kind — let the page close without further action. */
        break;
    }

    adw_tab_view_close_page_finish (view, page, TRUE);
    return GDK_EVENT_STOP;
}

/* Clear the newly-selected page's needs-attention flag, and ALSO
 * the Chat dock panel's flag (assumption: any attention on the
 * panel was due to a tab inside it; selecting any tab is the
 * user's acknowledgement). This is a slight over-clear in the
 * multi-flagged case but the case is rare and the alternative —
 * a full scan of all tabs to see if any still want attention —
 * would re-fire on every selection change. */
static void
on_selected_page_changed (AdwTabView *view, GParamSpec *pspec,
                          gpointer user_data)
{
    AdwTabPage *page;
    HxPanel *chat_panel;

    (void)pspec;
    (void)user_data;

    page = adw_tab_view_get_selected_page (view);
    if (page != NULL)
        adw_tab_page_set_needs_attention (page, FALSE);

    chat_panel = hx_panel_registry_lookup (HX_PANEL_ID_CHAT);
    if (chat_panel != NULL)
        panel_widget_set_needs_attention (PANEL_WIDGET (chat_panel), FALSE);
}

GtkWidget *
gtkhx_chat_tabs_init (void)
{
    if (the_tab_view != NULL)
        return GTK_WIDGET (the_tab_view);

    ensure_tables ();

    the_tab_view = ADW_TAB_VIEW (adw_tab_view_new ());

    /* needs-attention is set per-tab; we use the default
     * background/foreground for the tabs themselves. The tab bar
     * lives in the embedding code (Chat panel construction) above
     * the tab view. */
    g_signal_connect (the_tab_view, "close-page",
                      G_CALLBACK (on_close_page), NULL);
    g_signal_connect (the_tab_view, "notify::selected-page",
                      G_CALLBACK (on_selected_page_changed), NULL);

    return GTK_WIDGET (the_tab_view);
}

void
gtkhx_chat_tabs_set_close_pchat_handler (ChatTabsClosePchatFunc func)
{
    on_close_pchat = func;
}

void
gtkhx_chat_tabs_set_close_msg_handler (ChatTabsCloseMsgFunc func)
{
    on_close_msg = func;
}

void
gtkhx_chat_tabs_add_public (GtkWidget *content, const char *title)
{
    AdwTabPage *page;

    g_return_if_fail (the_tab_view != NULL);
    g_return_if_fail (GTK_IS_WIDGET (content));

    page = adw_tab_view_append_pinned (the_tab_view, content);
    adw_tab_page_set_title (page, title ? title : "Chat");
    g_object_set_data (G_OBJECT (page), "chat-tab-kind",
                       GINT_TO_POINTER (CHAT_TAB_KIND_PUBLIC));
    g_object_set_data (G_OBJECT (page), "chat-tab-id",
                       GUINT_TO_POINTER (0u));
    adw_tab_view_set_selected_page (the_tab_view, page);
}

AdwTabPage *
gtkhx_chat_tabs_add_pchat (GtkWidget *content, guint32 cid,
                           const char *title)
{
    AdwTabPage *page;

    g_return_val_if_fail (the_tab_view != NULL, NULL);
    g_return_val_if_fail (GTK_IS_WIDGET (content), NULL);

    ensure_tables ();

    page = adw_tab_view_append (the_tab_view, content);
    adw_tab_page_set_title (page, title ? title : "Private Chat");
    g_object_set_data (G_OBJECT (page), "chat-tab-kind",
                       GINT_TO_POINTER (CHAT_TAB_KIND_PCHAT));
    g_object_set_data (G_OBJECT (page), "chat-tab-id",
                       GUINT_TO_POINTER ((guint)cid));

    g_hash_table_replace (pchat_tabs, GUINT_TO_POINTER ((guint)cid), page);
    return page;
}

AdwTabPage *
gtkhx_chat_tabs_add_msg (GtkWidget *content, guint16 uid,
                         const char *title)
{
    AdwTabPage *page;

    g_return_val_if_fail (the_tab_view != NULL, NULL);
    g_return_val_if_fail (GTK_IS_WIDGET (content), NULL);

    ensure_tables ();

    page = adw_tab_view_append (the_tab_view, content);
    adw_tab_page_set_title (page, title ? title : "PM");
    g_object_set_data (G_OBJECT (page), "chat-tab-kind",
                       GINT_TO_POINTER (CHAT_TAB_KIND_MSG));
    g_object_set_data (G_OBJECT (page), "chat-tab-id",
                       GUINT_TO_POINTER ((guint)uid));

    g_hash_table_replace (msg_tabs, GUINT_TO_POINTER ((guint)uid), page);
    return page;
}

AdwTabPage *
gtkhx_chat_tabs_find_pchat (guint32 cid)
{
    if (!pchat_tabs)
        return NULL;
    return g_hash_table_lookup (pchat_tabs, GUINT_TO_POINTER ((guint)cid));
}

AdwTabPage *
gtkhx_chat_tabs_find_msg (guint16 uid)
{
    if (!msg_tabs)
        return NULL;
    return g_hash_table_lookup (msg_tabs, GUINT_TO_POINTER ((guint)uid));
}

/* Shared: raise the Chat dock panel so its tab strip is what the
 * user is looking at, then select the named tab. The
 * hx_panel_ensure_attached call covers the "Close all pages"
 * case (re-splices the panel back into its home area). */
static void
raise_chat_panel_and_select (AdwTabPage *page)
{
    HxPanel *chat_panel;

    if (page == NULL)
        return;

    chat_panel = hx_panel_registry_lookup (HX_PANEL_ID_CHAT);
    if (chat_panel != NULL) {
        hx_panel_ensure_attached (chat_panel);
        panel_widget_raise (PANEL_WIDGET (chat_panel));
    }
    if (the_tab_view != NULL)
        adw_tab_view_set_selected_page (the_tab_view, page);
}

void
gtkhx_chat_tabs_raise_pchat (guint32 cid)
{
    raise_chat_panel_and_select (gtkhx_chat_tabs_find_pchat (cid));
}

void
gtkhx_chat_tabs_raise_msg (guint16 uid)
{
    raise_chat_panel_and_select (gtkhx_chat_tabs_find_msg (uid));
}

void
gtkhx_chat_tabs_raise_public (void)
{
    AdwTabPage *page;

    if (the_tab_view == NULL)
        return;
    page = adw_tab_view_get_nth_page (the_tab_view, 0);
    raise_chat_panel_and_select (page);
}

/* Shared: set / clear needs-attention on a page + flag the Chat
 * panel so the dock tab strip shows the same hint when the panel
 * isn't currently visible. Clearing the page's flag also clears
 * the panel's flag — the assumption is the only reason the panel
 * was flagged was a tab inside it; this is conservative but the
 * other tabs' flags are stored on their pages, so a "still has
 * other tabs with attention" check can be added later if it
 * matters. */
static void
set_page_attention (AdwTabPage *page, gboolean state)
{
    HxPanel *chat_panel;

    if (page == NULL)
        return;

    adw_tab_page_set_needs_attention (page, state);

    chat_panel = hx_panel_registry_lookup (HX_PANEL_ID_CHAT);
    if (chat_panel != NULL)
        panel_widget_set_needs_attention (PANEL_WIDGET (chat_panel), state);
}

void
gtkhx_chat_tabs_set_attention_pchat (guint32 cid, gboolean state)
{
    set_page_attention (gtkhx_chat_tabs_find_pchat (cid), state);
}

void
gtkhx_chat_tabs_set_attention_msg (guint16 uid, gboolean state)
{
    set_page_attention (gtkhx_chat_tabs_find_msg (uid), state);
}

void
gtkhx_chat_tabs_set_title_pchat (guint32 cid, const char *title)
{
    AdwTabPage *page = gtkhx_chat_tabs_find_pchat (cid);
    if (page != NULL)
        adw_tab_page_set_title (page, title ? title : "Private Chat");
}

void
gtkhx_chat_tabs_set_title_msg (guint16 uid, const char *title)
{
    AdwTabPage *page = gtkhx_chat_tabs_find_msg (uid);
    if (page != NULL)
        adw_tab_page_set_title (page, title ? title : "PM");
}

void
gtkhx_chat_tabs_close_pchat (guint32 cid)
{
    AdwTabPage *page = gtkhx_chat_tabs_find_pchat (cid);
    if (page == NULL || the_tab_view == NULL)
        return;
    /* This will fire close-page → on_close_page; the dispatcher
     * removes the index entry and calls the close handler. */
    adw_tab_view_close_page (the_tab_view, page);
}

void
gtkhx_chat_tabs_close_msg (guint16 uid)
{
    AdwTabPage *page = gtkhx_chat_tabs_find_msg (uid);
    if (page == NULL || the_tab_view == NULL)
        return;
    adw_tab_view_close_page (the_tab_view, page);
}
