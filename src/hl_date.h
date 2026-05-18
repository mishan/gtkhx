/*
 * hl_date_decode — parse Hotline's 8-byte timestamp into a Unix
 * time_t, auto-detecting the wire format from the year field.
 *
 * See hl_date.c for the wire layout and the per-format decoding
 * rationale (per Capabilities.md's "Date Format Selection"
 * section in fogWraith/Hotline).
 */

#ifndef HX_HL_DATE_H
#define HX_HL_DATE_H 1

#include <time.h>
#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decode the 8-byte big-endian wire timestamp into a Unix time_t.
 * Returns TRUE on success (out_t set), FALSE if the input is
 * clearly invalid (NULL pointer, all-zero seconds, year outside
 * the plausible Hotline-era range). */
extern gboolean hl_date_decode (const guint8 *buf, time_t *out_t);

#ifdef __cplusplus
}
#endif

#endif /* HX_HL_DATE_H */
