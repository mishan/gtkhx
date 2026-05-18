/*
 * algo_list_nth — HOPE algorithm-list walker. Wire layout:
 *
 *   u16 count
 *   count records of { u8 namelen, name[namelen] }
 *
 * See algo_list.c for the rationale on extraction (testability,
 * the historic NULL-deref crash on empty lists called out by the
 * HOPE-Secure-Login spec).
 */

#ifndef HX_ALGO_LIST_H
#define HX_ALGO_LIST_H 1

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns a pointer to the {namelen, name} record for the nth
 * entry, or NULL if the input is malformed or n is out of range.
 * Safe to call with NULL list or listlen < 3. */
extern guint8 *list_n (guint8 *list, guint16 listlen, unsigned int n);

#ifdef __cplusplus
}
#endif

#endif /* HX_ALGO_LIST_H */
