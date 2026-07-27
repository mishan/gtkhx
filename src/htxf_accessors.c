/* Rust-facing accessor seam for struct htxf_conn — see htxf_accessors.h.
 *
 * Thin getters/setters over the still-C-owned transfer struct so the Rust
 * receive handlers (hxxfer-recv) can own the transfer logic while the struct's
 * storage, refcount lifecycle, and cross-thread ownership stay in xfers.c. The
 * same getter/setter-seam the hxconn (htlc_conn) migration began with. */

#include <sys/time.h>
#include <sys/stat.h>

#include "hx.h"
#include "htxf_accessors.h"

/* hx_htxf_in_list moved to the Rust transfer registry (hxhandlers::xfer) with
 * the xfers[] list in Y1. */

int
hx_htxf_opt_retry (const struct htxf_conn *htxf)
{
    return htxf->opt.retry ? 1 : 0;
}

int
hx_htxf_opt_preview (const struct htxf_conn *htxf)
{
    return htxf->opt.preview ? 1 : 0;
}

void *
hx_htxf_preview (const struct htxf_conn *htxf)
{
    return htxf->preview;
}

const char *
hx_htxf_path (const struct htxf_conn *htxf)
{
    return htxf->path;
}

guint64
hx_htxf_data_size (const struct htxf_conn *htxf)
{
    return htxf->data_size;
}

void
hx_htxf_set_ref (struct htxf_conn *htxf, guint32 ref)
{
    htxf->ref = ref;
}

void
hx_htxf_set_total_size (struct htxf_conn *htxf, guint64 total_size)
{
    htxf->total_size = total_size;
}

void
hx_htxf_set_queue (struct htxf_conn *htxf, guint32 queue)
{
    htxf->queue = queue;
}

void
hx_htxf_set_data_pos (struct htxf_conn *htxf, guint64 data_pos)
{
    htxf->data_pos = data_pos;
}

void
hx_htxf_set_rsrc_pos (struct htxf_conn *htxf, guint64 rsrc_pos)
{
    htxf->rsrc_pos = rsrc_pos;
}

void
hx_htxf_set_data_size (struct htxf_conn *htxf, guint64 data_size)
{
    htxf->data_size = data_size;
}

void
hx_htxf_set_rsrc_size (struct htxf_conn *htxf, guint64 rsrc_size)
{
    htxf->rsrc_size = rsrc_size;
}

void
hx_htxf_set_gone (struct htxf_conn *htxf, guint8 gone)
{
    htxf->gone = gone;
}

void
hx_htxf_set_preview (struct htxf_conn *htxf, void *preview)
{
    htxf->preview = preview;
}

void
hx_htxf_set_serverhost (struct htxf_conn *htxf, const char *host)
{
    g_strlcpy (htxf->serverhost, host, sizeof (htxf->serverhost));
}

void
hx_htxf_set_serverport (struct htxf_conn *htxf, guint16 port)
{
    htxf->serverport = port;
}

void
hx_htxf_stamp_start (struct htxf_conn *htxf)
{
    gettimeofday (&htxf->start, 0);
}

gint64
hx_file_size (const char *path)
{
    struct stat sb;

    if (stat (path, &sb)) {
        return -1;
    }
    return (gint64) sb.st_size;
}
