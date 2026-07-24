/* C-side collaborators for the Rust file-transfer receive handlers.
 *
 * The rcv_task_file_* / _folder_* / _banner_get / _file_getinfo bodies moved out
 * of rcv.c into the hxxfer-recv crate, which parses each reply natively
 * (hotline_proto::parse::*) and delegates the transfer-specific state change
 * here. struct htxf_conn is refcounted and cross-thread, so it stays C-owned;
 * these shims are the field stamping + preview construction + local-filesystem
 * probes + error policy that legitimately live on the C side. The logic is
 * lifted verbatim from the old rcv.c handlers. */

#include <sys/stat.h>
#include <sys/time.h>
#include <string.h>

#include "hx.h"
#include "xfers_recv_bridge.h"
#include "xfers.h"
#include "tasks.h"
#include "preview.h"
#include "files.h"       /* dirchar_basename */
#include "hfs.h"         /* resource_len, comment_len */
#include "hxconn.h"      /* hx_conn_serverhost / _serverport */
#include "session.h"     /* sess_from_htlc, timer_add_secs */
#include "hotline_proto.h" /* gtkhx_proto_hl_date_decode */

/* The shared file-transfer reply tail (hxxfer-recv crate): emit the queue
 * position, and — when unqueued — start moving bytes. */
extern void hx_xfer_announce (struct htlc_conn *htlc, struct htxf_conn *htxf,
                              guint32 queue);
/* file-info emit (hxxfer-recv crate) — fires the GtkhxSession file-info signal. */
extern void hx_file_info_recv (const char *path, const char *name,
                               const char *creator, const char *type,
                               const char *comments, const char *modified,
                               const char *created, guint64 size);

/* Display-time date format for the file-info dialog. Wire layout / per-format
 * decoding lives in hotline-proto (gtkhx_proto_hl_date_decode auto-detects the
 * Mac-1904 vs modern epoch via the year field); resolving to an absolute instant
 * + locale formatting is view-side GDateTime math. Servers commonly send ts=0 to
 * mean "no timestamp": hl_date_decode rejects that and we leave `out` empty (the
 * dialog renders empty values as an em-dash). */
static char *hx_timeformat = "%c";

static void
hx_format_hotline_date (const guint8 *bytes, char *out, size_t cap)
{
    struct gtkhx_proto_hl_date d;
    GDateTime *dt = NULL;

    if (cap == 0) {
        return;
    }
    out[0] = '\0';

    if (!gtkhx_proto_hl_date_decode (bytes, 8, &d)) {
        return;
    }
    if (d.kind == 0) {
        /* Mac 1904 epoch -> an absolute UTC instant. */
        dt = g_date_time_new_from_unix_local (
            (gint64)d.secs - (gint64)GTKHX_PROTO_MAC_TO_UNIX_EPOCH_OFFSET);
    } else {
        /* Modern -> seconds since Jan 1 `year` in local time. */
        GDateTime *base = g_date_time_new_local (d.year, 1, 1, 0, 0, 0);
        if (base) {
            dt = g_date_time_add_seconds (base, (double)d.secs);
            g_date_time_unref (base);
        }
    }
    if (!dt) {
        return;
    }
    char *s = g_date_time_format (dt, hx_timeformat);
    g_date_time_unref (dt);
    if (s) {
        g_strlcpy (out, s, cap);
        g_free (s);
    }
}

/* Adapter matching hx_preview_cancel_fn (void (*)(void *)). Registered with
 * hx_preview_set_cancel_cb on the preview's HTXF pointer so closing the preview
 * window mid-transfer fires xfer_delete on the right struct. A free-standing
 * adapter is required because xfer_delete takes a struct htxf_conn *: calling it
 * through a void* function-pointer type would be UB even though the args are
 * pointer-sized in practice. */
static void
preview_cancel_xfer_cb (void *user_data)
{
    xfer_delete ((struct htxf_conn *) user_data);
}

int
hx_xfer_in_list (struct htxf_conn *htxf)
{
    int i;

    for (i = 0; i < nxfers; i++) {
        if (xfers[i] == htxf) {
            return 1;
        }
    }
    return 0;
}

void
hx_xfer_get_error (struct htlc_conn *htlc, struct htxf_conn *htxf)
{
    if (htxf->opt.retry) {
        htxf->gone = 0;
        timer_add_secs (1, xfer_go_timer, htxf);
    } else {
        gtask_delete_htxf (sess_from_htlc (htlc), htxf);
        xfer_delete (htxf);
    }
}

void
hx_xfer_put_error (struct htlc_conn *htlc, struct htxf_conn *htxf)
{
    gtask_delete_htxf (sess_from_htlc (htlc), htxf);
    xfer_delete (htxf);
}

/* Stamp the HTXF subchannel target onto htxf so the worker can hand it straight
 * to GSocketClient without re-resolving — the subchannel is always (main server
 * hostname, main port + 1). */
static void
stamp_subchannel (struct htlc_conn *htlc, struct htxf_conn *htxf)
{
    gettimeofday (&htxf->start, 0);
    g_strlcpy (htxf->serverhost, hx_conn_serverhost (htlc),
               sizeof (htxf->serverhost));
    htxf->serverport = hx_conn_serverport (htlc) + 1;
}

void
hx_xfer_file_get_apply (struct htlc_conn *htlc, struct htxf_conn *htxf,
                        guint32 ref, guint64 total_size, guint32 queue)
{
    htxf->ref = ref;
    htxf->total_size = total_size;
    htxf->queue = queue;
    stamp_subchannel (htlc, htxf);

    /* For previews, build the GtkWindow + GtkTextView on the main thread (we are
     * on the main thread here — the receive dispatch path). The download worker
     * subsequently feeds bytes via htxf->preview without ever touching GTK,
     * sidestepping a class of lockups we hit when constructing widgets from a
     * worker. Built before hx_xfer_announce because that call starts the download
     * when unqueued, and the worker streams into htxf->preview. */
    if (htxf->opt.preview && !htxf->preview) {
        char *name = dirchar_basename (htxf->path);
        htxf->preview = hx_preview_new (name ? name : htxf->path);
        /* Wire the close-window-cancels-transfer hook. preview_cancel_xfer_cb is
         * the typed adapter that matches hx_preview_cancel_fn; casting
         * xfer_delete directly would call a function through a mismatched pointer
         * type (UB in C, even though both args are pointer-sized). */
        hx_preview_set_cancel_cb (htxf->preview, preview_cancel_xfer_cb, htxf);
    }

    hx_xfer_announce (htlc, htxf, htxf->queue);
}

void
hx_xfer_folder_get_apply (struct htlc_conn *htlc, struct htxf_conn *htxf,
                          guint32 ref, guint64 total_size, guint32 queue)
{
    htxf->ref = ref;
    /* total_size is the aggregate byte count for the whole tree; folder_get's
     * per-file calls bump total_pos so progress reads sensibly across the
     * folder. The caller has already clamped a server-reported 0 to 1. */
    htxf->total_size = total_size;
    htxf->queue = queue;
    stamp_subchannel (htlc, htxf);
    hx_xfer_announce (htlc, htxf, htxf->queue);
}

void
hx_xfer_file_put_apply (struct htlc_conn *htlc, struct htxf_conn *htxf,
                        guint32 ref, guint32 queue, guint32 data_pos,
                        guint32 rsrc_pos)
{
    struct stat sb;

    htxf->data_pos = data_pos;
    htxf->rsrc_pos = rsrc_pos;
    htxf->queue = queue;

    if (!stat (htxf->path, &sb)) {
        htxf->data_size = sb.st_size;
    }
    htxf->rsrc_size = resource_len (htxf->path);
    htxf->total_size = 133 + ((htxf->rsrc_size - htxf->rsrc_pos) ? 16 : 0)
                       + comment_len (htxf->path)
                       + (htxf->data_size - htxf->data_pos)
                       + (htxf->rsrc_size - htxf->rsrc_pos);
    htxf->ref = ref;
    stamp_subchannel (htlc, htxf);
    hx_xfer_announce (htlc, htxf, htxf->queue);
}

void
hx_xfer_folder_put_apply (struct htlc_conn *htlc, struct htxf_conn *htxf,
                          guint32 ref, guint32 queue)
{
    htxf->ref = ref;
    htxf->queue = queue;
    stamp_subchannel (htlc, htxf);
    hx_xfer_announce (htlc, htxf, htxf->queue);
}

/* Copy a (ptr, len) wire slice into a NUL-terminated fixed buffer, truncating to
 * cap - 1. The native parser already caps these (name 255 / type 31 / creator 31
 * / comment 255) so the min is defensive. */
static void
copy_nul (char *dst, size_t cap, const guint8 *src, gsize len)
{
    size_t n;

    if (cap == 0) {
        return;
    }
    n = len < cap - 1 ? len : cap - 1;
    if (n && src) {
        memcpy (dst, src, n);
    }
    dst[n] = '\0';
}

void
hx_xfer_file_info_apply (const char *path, const guint8 *name, gsize name_len,
                         const guint8 *type, gsize type_len,
                         const guint8 *creator, gsize creator_len,
                         const guint8 *comment, gsize comment_len,
                         const guint8 *date_create, const guint8 *date_modify,
                         guint64 size)
{
    char namez[256], typez[32], creaz[32], commentz[256];
    char created[32], modified[32];

    copy_nul (namez, sizeof namez, name, name_len);
    copy_nul (typez, sizeof typez, type, type_len);
    copy_nul (creaz, sizeof creaz, creator, creator_len);
    copy_nul (commentz, sizeof commentz, comment, comment_len);

    hx_format_hotline_date (date_create, created, sizeof created);
    hx_format_hotline_date (date_modify, modified, sizeof modified);

    hx_file_info_recv (path, namez, creaz, typez, commentz, modified, created,
                       size);
}
