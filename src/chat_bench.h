/* chat_bench.h — A/B benchmark for the two chat-view backends.
 *
 * See chat_bench.c for methodology and for how to read the numbers.
 * No-op unless GTKHX_CHATVIEW_BENCH is set, so this costs one getenv in
 * a normal run. */

#ifndef GTKHX_CHAT_BENCH_H
#define GTKHX_CHAT_BENCH_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Arm the benchmark on `view` if GTKHX_CHATVIEW_BENCH is set. Safe to
 * call with a NULL view, and safe to call unconditionally. */
void hx_chat_bench_maybe_start (GtkWidget *view);

G_END_DECLS

#endif /* GTKHX_CHAT_BENCH_H */
