/* chat_bench.c — A/B benchmark for the two chat-view backends.
 *
 * The point of this file is to answer one question with numbers rather
 * than impressions: is the hxchat backend good enough to delete xtext?
 *
 * It measures *in situ* — the real widget, in the real window, with the
 * real append path — rather than in a standalone harness, because a
 * standalone harness would have to reproduce the wiring and would then
 * be measuring the reproduction. The same binary runs both backends;
 * `GTKHX_CHATVIEW` picks which, exactly as it does normally, so the only
 * difference between the two runs is the widget under test.
 *
 *   GTKHX_CHATVIEW=xtext GTKHX_CHATVIEW_BENCH=20000 ./src/gtkhx
 *   GTKHX_CHATVIEW=new   GTKHX_CHATVIEW_BENCH=20000 ./src/gtkhx
 *
 * Check the `backend` line in the report before trusting any of it.
 * `want_hxchat` used to accept only "new"/"hxchat", so a run asked for
 * with `=1` reported xtext and looked entirely normal.
 *
 * Add `GTKHX_CHATVIEW_BENCH_QUIT=1` to exit as soon as the report is
 * printed, which is what makes it scriptable.
 *
 * ---- What the numbers mean, and what they don't -------------------
 *
 * The two backends divide the work differently, so a single "append"
 * timing would flatter the new one and mislead. xtext line-wraps at
 * append time (gtk_xtext_append_entry → calc_lines); hxchat stores the
 * message and defers layout to the frame that needs it. Measuring
 * ingest alone would therefore compare "did the work" against "wrote it
 * down". So ingest and the first paint after it are reported separately
 * and should be read together: their sum is the honest cost of getting N
 * messages on screen.
 *
 * Frame timings come from the frame clock, so they include GTK's own
 * compositing, not just our snapshot. That is the right denominator —
 * it is what the user feels — but it means the numbers are only
 * comparable between two runs on the same machine, same window size,
 * same theme. Don't compare across machines.
 *
 * The relayout phase changes the *font*, not the width. An earlier
 * version shrank the view's size-request and timed one tick, which
 * measured nothing at all: the chat output is `hexpand`, so lowering its
 * minimum width does not change its allocation and nothing re-wraps.
 * Both backends reported one vsync interval. A font change is
 * client-side, needs no compositor cooperation, and really does
 * invalidate every wrap point — and the phase samples ten frames rather
 * than one, so a backend that re-wraps 20k messages cannot hide the cost
 * in the frame after the one being timed.
 *
 * RSS is read from /proc/self/statm, so it is Linux-only and includes
 * everything the process has touched, not just the chat buffer. Only the
 * *delta* across the ingest phase is reported, which cancels most of the
 * shared baseline.
 */

#include "chat_bench.h"

#include "chat_view.h"
#include "debug.h"

#include <glib.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Frames to let settle before timing anything. The first frames after a
 * window maps are dominated by one-off GTK work (CSS, font map, GL
 * context) that belongs to neither backend. */
#define BENCH_WARMUP_FRAMES 20

/* Frames sampled in the scroll phase. */
#define BENCH_SCROLL_FRAMES 120

/* Frames sampled after the relayout trigger.
 *
 * Sampling a window rather than "the next tick" is the whole point: a
 * backend that re-wraps the entire scrollback spends that time
 * *somewhere*, and if it lands after the tick being timed then timing one
 * tick reports a vsync interval and calls it a result. Which is exactly
 * what the first version of this file did. */
#define BENCH_RELAYOUT_FRAMES 10

/* Frames to let the prep font settle before timing the real change. */
#define BENCH_RELAYOUT_PREP_FRAMES 3

/* The two fonts the relayout phase toggles between. Different sizes, so
 * every cached width and every wrap point is genuinely invalid. */
#define BENCH_FONT_A "Monospace 10"
#define BENCH_FONT_B "Monospace 12" 

typedef enum {
    PHASE_WARMUP = 0,
    PHASE_INGEST,      /* appending; waiting for the append loop to finish */
    PHASE_FIRST_PAINT, /* appended, waiting for the frame that shows it */
    PHASE_RELAYOUT_PREP, /* font set once, letting it settle before timing */
    PHASE_RELAYOUT,      /* font changed again; sampling the frames it costs */
    PHASE_SCROLL,      /* stepping the adjustment, sampling frame times */
    PHASE_DONE
} BenchPhase;

typedef struct {
    GtkWidget *view;
    guint n_messages;
    gboolean quit_when_done;

    BenchPhase phase;
    guint frames_seen;
    guint tick_id;

    gint64 t_mark; /* start of the phase being timed, monotonic us */

    /* Results. */
    gint64 ingest_us;
    gint64 first_paint_us;
    gint64 relayout_total_us;
    gint64 relayout_worst_us;
    gint64 relayout_frames[BENCH_RELAYOUT_FRAMES];
    guint relayout_count;
    guint prep_frames;
    gsize rss_before_kb;
    gsize rss_after_kb;

    /* Scroll sampling. */
    gint64 frame_us[BENCH_SCROLL_FRAMES];
    guint frame_count;
    gint64 last_frame_time;
    int saved_width;
} Bench;

/* Resident set size in KB, or 0 where /proc isn't available. */
static gsize
bench_rss_kb (void)
{
    gsize pages = 0, resident = 0;
    FILE *f = fopen ("/proc/self/statm", "r");

    if (!f) {
        return 0;
    }
    if (fscanf (f, "%zu %zu", &pages, &resident) != 2) {
        resident = 0;
    }
    fclose (f);
    return resident * (gsize)(sysconf (_SC_PAGESIZE) / 1024);
}

/* One synthetic chat line. Deliberately varied in length so wrapping is
 * exercised rather than one cached width being reused for every row, and
 * with a nick column so the indent path runs. */
static void
bench_append_one (GtkWidget *view, guint i)
{
    char nick[64];
    char body[512];
    /* Nick widths cycle so the gutter settles early and then stays put,
     * matching a real room rather than a pathological one. */
    static const char *names[]
        = { "misha", "alice", "bob", "carol", "dave-with-a-long-name" };
    const char *nam = names[i % G_N_ELEMENTS (names)];
    guint words = 3 + (i % 17); /* 3..19 words: short lines and wrapped ones */
    gsize off = 0;
    guint w;

    g_snprintf (nick, sizeof nick, "<%s>", nam);
    for (w = 0; w < words && off < sizeof body - 24; w++) {
        off += (gsize)g_snprintf (body + off, sizeof body - off, "%sword%u",
                                  w ? " " : "", (i * 7 + w) % 1000);
    }
    hx_chat_view_append_indent (view, nick, (int)strlen (nick), body,
                                (int)off, 0);
}

static void
bench_report (Bench *b)
{
    gint64 total_us = b->ingest_us + b->first_paint_us;
    double ingest_rate
        = b->ingest_us > 0 ? (double)b->n_messages / ((double)b->ingest_us / 1e6)
                           : 0.0;
    gint64 sorted[BENCH_SCROLL_FRAMES];
    gint64 sum = 0;
    guint i;
    double mean_ms = 0.0, p95_ms = 0.0;

    memcpy (sorted, b->frame_us, sizeof (gint64) * b->frame_count);
    for (i = 0; i < b->frame_count; i++) {
        sum += sorted[i];
    }
    /* Insertion sort — at most BENCH_SCROLL_FRAMES entries. */
    for (i = 1; i < b->frame_count; i++) {
        gint64 v = sorted[i];
        guint j = i;
        while (j > 0 && sorted[j - 1] > v) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = v;
    }
    if (b->frame_count > 0) {
        mean_ms = ((double)sum / (double)b->frame_count) / 1000.0;
        p95_ms = (double)sorted[(b->frame_count * 95) / 100] / 1000.0;
    }

    printf ("\n");
    printf ("=== chat-view benchmark =============================\n");
    /* can_search is the only exported predicate that distinguishes the
     * backends — search is hxchat-only by construction (chat_view.h). It
     * reads oddly as a backend test, so: that is what it is being used
     * for, and if search ever lands for xtext this line needs a real
     * predicate instead. */
    printf ("backend            %s\n",
            hx_chat_view_can_search (b->view) ? "hxchat (new)" : "xtext (old)");
    printf ("messages           %u\n", b->n_messages);
    printf ("-----------------------------------------------------\n");
    printf ("ingest             %8.1f ms   (%.0f msgs/s)\n",
            (double)b->ingest_us / 1000.0, ingest_rate);
    printf ("first paint        %8.1f ms\n",
            (double)b->first_paint_us / 1000.0);
    printf ("  ingest + paint   %8.1f ms   <- compare THIS across backends\n",
            (double)total_us / 1000.0);
    printf ("relayout total     %8.1f ms   (%u frames after a font change)\n",
            (double)b->relayout_total_us / 1000.0, b->relayout_count);
    printf ("relayout worst frm %8.1f ms   <- whole-scrollback re-wrap shows HERE\n",
            (double)b->relayout_worst_us / 1000.0);
    printf ("scroll frame mean  %8.2f ms\n", mean_ms);
    printf ("scroll frame p95   %8.2f ms   (%u frames)\n", p95_ms,
            b->frame_count);
    if (b->rss_before_kb && b->rss_after_kb) {
        double per10k
            = b->n_messages
                  ? ((double)(b->rss_after_kb - b->rss_before_kb) * 10000.0
                     / (double)b->n_messages)
                  : 0.0;
        printf ("RSS delta          %8.1f MB  (%.1f MB / 10k msgs)\n",
                (double)(b->rss_after_kb - b->rss_before_kb) / 1024.0,
                per10k / 1024.0);
    }
    printf ("=====================================================\n");
    fflush (stdout);
}

static gboolean
bench_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
    Bench *b = data;
    gint64 now = g_get_monotonic_time ();

    (void)widget;

    switch (b->phase) {
    case PHASE_WARMUP:
        if (++b->frames_seen < BENCH_WARMUP_FRAMES) {
            return G_SOURCE_CONTINUE;
        }
        /* ---- ingest ------------------------------------------------ */
        b->rss_before_kb = bench_rss_kb ();
        b->t_mark = g_get_monotonic_time ();
        for (guint i = 0; i < b->n_messages; i++) {
            bench_append_one (b->view, i);
        }
        b->ingest_us = g_get_monotonic_time () - b->t_mark;
        b->rss_after_kb = bench_rss_kb ();

        /* Whatever layout the backend deferred is still owed; time the
         * frame that pays it. */
        gtk_widget_queue_resize (b->view);
        b->t_mark = g_get_monotonic_time ();
        b->phase = PHASE_FIRST_PAINT;
        return G_SOURCE_CONTINUE;

    case PHASE_INGEST:
        /* unused; ingest is synchronous inside the warmup tick */
        b->phase = PHASE_FIRST_PAINT;
        return G_SOURCE_CONTINUE;

    case PHASE_FIRST_PAINT:
        b->first_paint_us = now - b->t_mark;

        /* ---- relayout ---------------------------------------------- */
        /* Trigger a full invalidation with a *font* change, not a width
         * change.
         *
         * The first version of this benchmark shrank the view's
         * size-request and timed the next tick. That measured nothing:
         * the chat output is `hexpand`, so lowering its *minimum* width
         * leaves the allocation untouched and no re-wrap happens at all.
         * Both backends duly reported ~16.4 ms — one vsync interval —
         * and the scoping doc nearly recorded that as "no difference in
         * reflow".
         *
         * A font change is fully client-side, needs no cooperation from
         * the compositor, and genuinely invalidates every cached width
         * and every wrap point in both backends. */
        hx_chat_view_set_font (b->view, BENCH_FONT_A);
        b->prep_frames = 0;
        b->phase = PHASE_RELAYOUT_PREP;
        return G_SOURCE_CONTINUE;

    case PHASE_RELAYOUT_PREP:
        /* Let font A land, so the change we time is a real transition
         * rather than a possible no-op against whatever the prefs said. */
        if (++b->prep_frames < BENCH_RELAYOUT_PREP_FRAMES) {
            return G_SOURCE_CONTINUE;
        }
        b->relayout_count = 0;
        b->relayout_total_us = 0;
        b->relayout_worst_us = 0;
        b->last_frame_time = g_get_monotonic_time ();
        hx_chat_view_set_font (b->view, BENCH_FONT_B);
        gtk_widget_queue_resize (b->view);
        b->phase = PHASE_RELAYOUT;
        return G_SOURCE_CONTINUE;

    case PHASE_RELAYOUT: {
        gint64 d = now - b->last_frame_time;

        b->last_frame_time = now;
        if (b->relayout_count < BENCH_RELAYOUT_FRAMES) {
            b->relayout_frames[b->relayout_count++] = d;
            b->relayout_total_us += d;
            if (d > b->relayout_worst_us) {
                b->relayout_worst_us = d;
            }
        }
        gtk_widget_queue_draw (b->view);
        if (b->relayout_count < BENCH_RELAYOUT_FRAMES) {
            return G_SOURCE_CONTINUE;
        }

        /* ---- scroll ------------------------------------------------ */
        b->frame_count = 0;
        b->last_frame_time = now;
        b->phase = PHASE_SCROLL;
        return G_SOURCE_CONTINUE;
    }

    case PHASE_SCROLL: {
        GtkAdjustment *adj = hx_chat_view_get_vadjustment (b->view);

        if (b->frame_count < BENCH_SCROLL_FRAMES) {
            b->frame_us[b->frame_count++] = now - b->last_frame_time;
        }
        b->last_frame_time = now;

        if (adj) {
            /* Walk from the top down, one third of a page per frame, so
             * every frame lands on freshly-uncached rows rather than
             * re-showing the same ones. */
            double page = gtk_adjustment_get_page_size (adj);
            double upper = gtk_adjustment_get_upper (adj);
            double step = page / 3.0;
            double v = gtk_adjustment_get_value (adj) + step;
            if (v > upper - page) {
                v = 0.0;
            }
            gtk_adjustment_set_value (adj, v);
        }
        gtk_widget_queue_draw (b->view);

        if (b->frame_count >= BENCH_SCROLL_FRAMES) {
            b->phase = PHASE_DONE;
            bench_report (b);
            if (b->quit_when_done) {
                GApplication *app = g_application_get_default ();
                if (app) {
                    g_application_quit (app);
                } else {
                    exit (0);
                }
            }
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }

    case PHASE_DONE:
    default:
        return G_SOURCE_REMOVE;
    }
}

void
hx_chat_bench_maybe_start (GtkWidget *view)
{
    const char *env = g_getenv ("GTKHX_CHATVIEW_BENCH");
    Bench *b;
    guint64 n;

    if (!env || !*env || !view) {
        return;
    }
    if (!g_ascii_string_to_unsigned (env, 10, 1, 1000000, &n, NULL)) {
        g_warning ("GTKHX_CHATVIEW_BENCH: expected a message count, got '%s'",
                   env);
        return;
    }

    b = g_new0 (Bench, 1);
    b->view = view;
    b->n_messages = (guint)n;
    b->quit_when_done = g_getenv ("GTKHX_CHATVIEW_BENCH_QUIT") != NULL;
    b->phase = PHASE_WARMUP;

    debug_log ("bench", "starting: %u messages, backend=%s", b->n_messages,
               hx_chat_view_can_search (view) ? "hxchat" : "xtext");

    /* g_free as the destroy notify: the callback removes itself at
     * PHASE_DONE and the tick machinery then drops the closure. */
    b->tick_id = gtk_widget_add_tick_callback (view, bench_tick, b, g_free);
}
