#ifndef GTKHX_HTXF_ACCESSORS_H
#define GTKHX_HTXF_ACCESSORS_H

#include <glib.h>
#include <stddef.h> /* size_t, for the layout-introspection helpers below */

/* Rust-facing accessor seam for struct htxf_conn.
 *
 * struct htxf_conn is still declared + field-accessed in C, but since S0 its
 * storage is allocated + freed Rust-side (hx_htxf_new / hx_htxf_free, in hxnet's
 * xfer_handle module). It's refcounted, held in the C xfers[] array, and mutated
 * by both the main thread and the tokio transfer worker. These getters/setters
 * are the generic field seam the Rust receive handlers (hxxfer-recv) reach it
 * through — so the transfer *logic* (parse, gates, stamping sequence, error
 * policy, upload-size math) lives in Rust while the struct stays in C. This is
 * the same getter/setter-seam step the hxconn (htlc_conn) migration started
 * with. */

struct htxf_conn;

/* ---- Storage (S0): allocation owned by hxnet's xfer_handle module ----------
 * struct htxf_conn is still declared + field-accessed in C, but the storage is
 * allocated + freed Rust-side (a #[repr(C)] mirror in rust/crates/hxnet), the
 * first step of moving its cross-thread lifecycle behind this ABI. The mirror
 * layout is pinned at runtime by tests/unit/test_htxf_layout.c against the
 * introspection helpers below. */
extern struct htxf_conn *hx_htxf_new (void);   /* zeroed; replaces g_malloc0 */
extern void hx_htxf_free (struct htxf_conn *htxf); /* NULL-safe; replaces g_free */

/* Layout introspection — a C test asserts these against sizeof / offsetof on the
 * real struct htxf_conn, so a field drift on either side fails the build/test. */
extern size_t hx_htxf_sizeof (void);
extern size_t hx_htxf_alignof (void);
extern size_t hx_htxf_offsetof_refcount (void);
extern size_t hx_htxf_offsetof_canceled (void);
extern size_t hx_htxf_offsetof_total_pos (void);

/* ---- Cross-thread lifecycle (S0.2) -----------------------------------------
 * The refcount, cancel flag, and total_pos byte counter are touched by both the
 * main thread and the tokio transfer worker; these are the atomic ABI (a 1:1
 * port of the old g_atomic_int_* calls) the C side now goes through instead of
 * accessing htxf->{refcount,canceled,total_pos} directly. */
extern gint hx_htxf_ref (struct htxf_conn *htxf);   /* inc; returns new count */
/* dec; on the last ref (count → 0) runs the registered destructor (the C
 * GTK/preview + channel teardown) then frees the handle. NULL-safe. */
extern void hx_htxf_unref (struct htxf_conn *htxf);
extern void hx_htxf_cancel (struct htxf_conn *htxf);
extern gint hx_htxf_is_canceled (const struct htxf_conn *htxf);
extern void hx_htxf_add_total_pos (struct htxf_conn *htxf, guint64 delta);
extern void hx_htxf_set_total_pos (struct htxf_conn *htxf, guint64 val);
extern guint64 hx_htxf_total_pos (const struct htxf_conn *htxf);

/* Register the destructor run on a handle's last unref, before free (S0.3) —
 * the GTK/preview + channel teardown that must stay in C. Called once at
 * startup. */
extern void hx_htxf_set_destructor (void (*cb) (struct htxf_conn *htxf));

/* Registry: is htxf still a live entry in the transfer list? (Downloads gate
 * their reply on this — a since-cancelled transfer's reply is dropped.) The body
 * moved to the Rust registry (hxhandlers::xfer) with the xfers[] list; the
 * prototype stays here to document the C ABI surface + guard against an
 * implicit-declaration call from any future C site. Only Rust calls it today. */
extern int hx_htxf_in_list (struct htxf_conn *htxf);

/* Getters. */
extern int          hx_htxf_opt_retry (const struct htxf_conn *htxf);
extern int          hx_htxf_opt_preview (const struct htxf_conn *htxf);
extern int          hx_htxf_opt_folder (const struct htxf_conn *htxf);
extern int          hx_htxf_opt_large (const struct htxf_conn *htxf);
/* opt-bitfield setters (C owns the bit layout) — used by the Rust xfers shell's
 * xfer_new / xfer_new_folder. */
extern void         hx_htxf_set_opt_preview (struct htxf_conn *htxf, int v);
extern void         hx_htxf_set_opt_folder (struct htxf_conn *htxf, int v);
extern void        *hx_htxf_preview (const struct htxf_conn *htxf);
extern const char  *hx_htxf_path (const struct htxf_conn *htxf);
extern guint64      hx_htxf_data_size (const struct htxf_conn *htxf);

/* Setters. */
extern void hx_htxf_set_ref (struct htxf_conn *htxf, guint32 ref);
extern void hx_htxf_set_total_size (struct htxf_conn *htxf, guint64 total_size);
extern void hx_htxf_set_queue (struct htxf_conn *htxf, guint32 queue);
extern void hx_htxf_set_data_pos (struct htxf_conn *htxf, guint64 data_pos);
extern void hx_htxf_set_rsrc_pos (struct htxf_conn *htxf, guint64 rsrc_pos);
extern void hx_htxf_set_data_size (struct htxf_conn *htxf, guint64 data_size);
extern void hx_htxf_set_rsrc_size (struct htxf_conn *htxf, guint64 rsrc_size);
extern void hx_htxf_set_gone (struct htxf_conn *htxf, guint8 gone);
extern void hx_htxf_set_preview (struct htxf_conn *htxf, void *preview);
extern void hx_htxf_set_serverhost (struct htxf_conn *htxf, const char *host);
extern void hx_htxf_set_serverport (struct htxf_conn *htxf, guint16 port);

/* Stamp htxf->start with the current time (gettimeofday) — keeps the
 * struct timeval layout on the C side. */
extern void hx_htxf_stamp_start (struct htxf_conn *htxf);

/* stat(2) the given path; returns the data-fork byte size, or -1 on error. Lets
 * the Rust file_put handler compute the upload byte total without marshaling the
 * path into Rust — the C string pointer (hx_htxf_path) is handed straight to the
 * fs primitives (this, resource_len, comment_len). */
extern gint64 hx_file_size (const char *path);

#endif /* GTKHX_HTXF_ACCESSORS_H */
