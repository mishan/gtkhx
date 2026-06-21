/*
 * gtkhx_theme.c — implementation of the themable-state singleton.
 * See gtkhx_theme.h and docs/theming-scoping.md.
 */
#include "config.h"

#include "gtkhx_theme.h"
#include "hx.h" /* gtkhx_prefs */

struct _GtkhxTheme {
    GObject parent_instance;
};

G_DEFINE_FINAL_TYPE (GtkhxTheme, gtkhx_theme, G_TYPE_OBJECT)

enum {
    SIGNAL_CHANGED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

/* The built-in "default theme": the scale each area renders at when
 * the user hasn't set an override. These are the *real* factors GtkHx
 * has always applied, expressed honestly against the unscaled source
 * art (the 16x16 button pixmaps; the user-list icon's natural size and
 * the base font). The toolbar / window buttons have always drawn at 2x
 * their source art, and the standalone Users window at 1.25x — so the
 * default theme says 200 / 200 / 125 / 125, *not* 100. A fresh prefs
 * file therefore reproduces today's appearance, and Settings shows the
 * true 200% / 125% rather than a misleading 100%. A future named theme
 * would supply its own table here. */
static const int default_theme_pct[GTKHX_SCALE_N_AREAS] = {
    200, /* GTKHX_SCALE_TOOLBAR */
    200, /* GTKHX_SCALE_WINDOW_BUTTONS */
    125, /* GTKHX_SCALE_USERLIST_ICON */
    125, /* GTKHX_SCALE_USERLIST_TEXT */
};

static void
gtkhx_theme_class_init (GtkhxThemeClass *klass)
{
    /* Zero-argument notification. Subscribers re-read whatever scale
	 * areas they care about via gtkhx_theme_scale() and refresh. A
	 * single coalesced signal keeps the fan-out cheap — there's no
	 * per-area signal because a Settings apply can touch several at
	 * once and every consumer already re-reads only its own areas. */
    signals[SIGNAL_CHANGED]
        = g_signal_new ("changed", G_TYPE_FROM_CLASS (klass),
                        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
gtkhx_theme_init (GtkhxTheme *self)
{
    (void)self;
}

GtkhxTheme *
gtkhx_theme_get_default (void)
{
    static GtkhxTheme *singleton = NULL;

    if (singleton == NULL) {
        singleton = g_object_new (GTKHX_TYPE_THEME, NULL);
    }
    return singleton;
}

int
gtkhx_theme_clamp_percent (int pct)
{
    if (pct < GTKHX_SCALE_MIN) {
        return GTKHX_SCALE_MIN;
    }
    if (pct > GTKHX_SCALE_MAX) {
        return GTKHX_SCALE_MAX;
    }
    return pct;
}

/* Map an area to its backing gtkhx_prefs field. The prefs struct is
 * the single source of truth so theming state round-trips through the
 * existing GKeyFile persistence with no parallel store. */
static int *
prefs_slot (GtkhxScaleArea area)
{
    switch (area) {
    case GTKHX_SCALE_TOOLBAR:
        return &gtkhx_prefs.scale_toolbar;
    case GTKHX_SCALE_WINDOW_BUTTONS:
        return &gtkhx_prefs.scale_window_buttons;
    case GTKHX_SCALE_USERLIST_ICON:
        return &gtkhx_prefs.scale_userlist_icon;
    case GTKHX_SCALE_USERLIST_TEXT:
        return &gtkhx_prefs.scale_userlist_text;
    case GTKHX_SCALE_N_AREAS:
    default:
        return NULL;
    }
}

int
gtkhx_theme_get_default_percent (GtkhxScaleArea area)
{
    if (area < 0 || area >= GTKHX_SCALE_N_AREAS) {
        return 100;
    }
    return default_theme_pct[area];
}

int
gtkhx_theme_get_percent (GtkhxScaleArea area)
{
    int *slot = prefs_slot (area);

    /* 0 (or a bogus non-positive value, e.g. from a prefs file that
	 * predates these keys) means "no user override" → fall back to the
	 * default theme's value for this area. So an upgrade lands on the
	 * shipped appearance honestly (toolbar 200%, user list 125%, …),
	 * and only an explicit Settings change becomes a stored override. */
    if (!slot || *slot <= 0) {
        return gtkhx_theme_get_default_percent (area);
    }
    return gtkhx_theme_clamp_percent (*slot);
}

double
gtkhx_theme_scale (GtkhxScaleArea area)
{
    return gtkhx_theme_get_percent (area) / 100.0;
}

void
gtkhx_theme_set_percent (GtkhxScaleArea area, int pct)
{
    int *slot = prefs_slot (area);
    int clamped = gtkhx_theme_clamp_percent (pct);

    if (!slot || *slot == clamped) {
        return;
    }
    *slot = clamped;
    g_signal_emit (gtkhx_theme_get_default (), signals[SIGNAL_CHANGED], 0);
}

void
gtkhx_theme_notify_changed (void)
{
    int a;

    for (a = 0; a < GTKHX_SCALE_N_AREAS; a++) {
        int *slot = prefs_slot ((GtkhxScaleArea)a);
        if (slot && *slot > 0) {
            *slot = gtkhx_theme_clamp_percent (*slot);
        }
    }
    g_signal_emit (gtkhx_theme_get_default (), signals[SIGNAL_CHANGED], 0);
}
