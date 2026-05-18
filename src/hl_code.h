/*
 * hl_code — Hotline's symmetric XOR-with-0xff transform used to
 * obfuscate LOGIN and PASSWORD chunks on the wire. See hl_code.c
 * for the rationale behind keeping it in its own translation unit
 * (Tier 1 testability without dragging in network.c's deps).
 *
 * encode and decode are the same operation; the protocol.h macros
 * `hl_encode` and `hl_decode` just spell out the intent at call
 * sites.
 */

#ifndef HX_HL_CODE_H
#define HX_HL_CODE_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void hl_code (void *__dst, const void *__src, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* HX_HL_CODE_H */
