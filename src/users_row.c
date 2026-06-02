/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "config.h"

#include <glib.h>
#include <string.h>

#include "users.h"     /* user_nick_color_gdk */
#include "users_row.h"

struct _HxUserRow {
    GObject parent_instance;
    struct hx_user *user; /* borrowed */
    char *name;
    guint16 icon;
    guint16 color;
    GdkRGBA fg;
    gboolean has_fg;
};

G_DEFINE_FINAL_TYPE (HxUserRow, hx_user_row, G_TYPE_OBJECT)

enum {
    SIGNAL_CHANGED,
    SIGNAL_LAST
};
static guint signals[SIGNAL_LAST];

static void
hx_user_row_finalize (GObject *obj)
{
    HxUserRow *r = HX_USER_ROW (obj);
    g_free (r->name);
    G_OBJECT_CLASS (hx_user_row_parent_class)->finalize (obj);
}

static void
hx_user_row_class_init (HxUserRowClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = hx_user_row_finalize;

    /* Emitted on every hx_user_row_set_state. HxUserListView
     * connects to bump its sort model and re-snapshot the cell. */
    signals[SIGNAL_CHANGED]
        = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                        0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
hx_user_row_init (HxUserRow *self)
{
    (void)self;
}

/* Recompute the cached foreground from the row's user + status.
 * user_nick_color_gdk prefers the per-user RGB nick color and
 * falls back to the status palette; NULL means "regular user
 * slot, theme default." We stash a copy + a has_fg flag because
 * the caller passes a stack-local GdkRGBA buffer that we can't
 * keep a pointer to. */
static void
hx_user_row_refresh_fg (HxUserRow *r)
{
    GdkRGBA tmp;
    GdkRGBA *fg = user_nick_color_gdk (r->user, r->color, &tmp);
    if (fg) {
        r->fg = *fg;
        r->has_fg = TRUE;
    } else {
        r->has_fg = FALSE;
    }
}

HxUserRow *
hx_user_row_new (struct hx_user *user, const char *nam, guint16 icon,
                 guint16 color)
{
    HxUserRow *r = g_object_new (HX_TYPE_USER_ROW, NULL);
    r->user = user;
    r->name = g_strdup (nam ? nam : "");
    r->icon = icon;
    r->color = color;
    hx_user_row_refresh_fg (r);
    return r;
}

void
hx_user_row_set_state (HxUserRow *r, const char *nam, guint16 icon,
                       guint16 color)
{
    if (!r) {
        return;
    }
    g_free (r->name);
    r->name = g_strdup (nam ? nam : "");
    r->icon = icon;
    r->color = color;
    hx_user_row_refresh_fg (r);
    g_signal_emit (r, signals[SIGNAL_CHANGED], 0);
}

struct hx_user *
hx_user_row_get_user (HxUserRow *r)
{
    return r ? r->user : NULL;
}

const char *
hx_user_row_get_name (HxUserRow *r)
{
    return r ? r->name : "";
}

guint16
hx_user_row_get_icon (HxUserRow *r)
{
    return r ? r->icon : 0;
}

guint16
hx_user_row_get_color (HxUserRow *r)
{
    return r ? r->color : 0;
}

const GdkRGBA *
hx_user_row_get_foreground (HxUserRow *r)
{
    if (!r || !r->has_fg) {
        return NULL;
    }
    return &r->fg;
}

guint16
hx_user_row_get_uid (HxUserRow *r)
{
    return (r && r->user) ? r->user->uid : 0;
}
