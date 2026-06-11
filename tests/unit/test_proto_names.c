/*
 * tests/unit/test_proto_names.c — verify proto_hdr_name() and
 * proto_data_name() map opcode integers to the symbolic names we
 * print in [proto] traces.
 *
 * Why bother testing string-table functions:
 *   - The trace logger is the diagnostic tool we reach for whenever a
 *     server says "Uh, no." or sends an unexpected reply — see the
 *     hlserver / Badmoon / mhxd debugging history. If a switch case
 *     returns the wrong constant, every "what just went over the
 *     wire?" investigation that uses the trace gets misled.
 *   - The two functions also have a 0x????-fallback path for unknown
 *     opcodes that uses a static buffer; we exercise that too so we
 *     don't accidentally change the format string and break log
 *     greps people have learned.
 *
 * Coverage:
 *   - One known opcode from each major group (HTLC_HDR_*, HTLS_HDR_*,
 *     HTLC_HDR_PING, HTLC/S_DATA_*).
 *   - Unknown opcode → "0x000123"-style hex fallback.
 */

#include "config.h"
#include <string.h>
#include <glib.h>
/* protocol.h transitively pulls in compat.h (where PACKED is
 * defined) and hotline.h (where HTLC_HDR_* / HTLS_DATA_* opcodes
 * live) in the right order. Including hotline.h directly without
 * compat.h first chokes on the PACKED attribute. */
#include "protocol.h"
#include "proto_trace.h"

/* ---------- proto_hdr_name ---------- */

static void
test_hdr_login (void)
{
    g_assert_cmpstr (proto_hdr_name (HTLC_HDR_LOGIN), ==, "HTLC_HDR_LOGIN");
}

static void
test_hdr_chat (void)
{
    g_assert_cmpstr (proto_hdr_name (HTLC_HDR_CHAT), ==, "HTLC_HDR_CHAT");
}

static void
test_hdr_news_getfile (void)
{
    g_assert_cmpstr (proto_hdr_name (HTLC_HDR_NEWS_GETFILE), ==,
                     "HTLC_HDR_NEWS_GETFILE");
}

static void
test_hdr_user_getlist (void)
{
    g_assert_cmpstr (proto_hdr_name (HTLC_HDR_USER_GETLIST), ==,
                     "HTLC_HDR_USER_GETLIST");
}

static void
test_hdr_ping (void)
{
    /* HTLC_HDR_PING = 0x1f4 = 500 (mhxd extension; we send it once
	 * a minute on >= 1.5 servers). The proto trace uses the
	 * "HTLC/S_HDR_*" combined-direction form for opcodes that share
	 * the same numeric value in both directions (PING, MSG_BROADCAST,
	 * CHAT_INVITE — see proto_trace.c). Catches a renumber AND a
	 * label-style change. */
    g_assert_cmpstr (proto_hdr_name (HTLC_HDR_PING), ==, "HTLC/S_HDR_PING");
}

static void
test_hdr_task_reply (void)
{
    /* Server-side task reply opcode — what task-error replies arrive
	 * tagged as in the trace ("← trans=N type=HTLS_HDR_TASK ..."). */
    g_assert_cmpstr (proto_hdr_name (HTLS_HDR_TASK), ==, "HTLS_HDR_TASK");
}

static void
test_hdr_unknown_falls_back_to_hex (void)
{
    /* No real opcode collides with 0xdead. The static buffer
	 * fallback formats it as "0x00dead" via %06x. */
    const char *out = proto_hdr_name (0xdeadu);
    g_assert_nonnull (out);
    g_assert_cmpstr (out, ==, "0x00dead");
}

static void
test_hdr_unknown_zero_falls_back (void)
{
    /* Opcode 0 isn't a real Hotline opcode either. */
    const char *out = proto_hdr_name (0u);
    g_assert_nonnull (out);
    g_assert_cmpstr (out, ==, "0x000000");
}

/* ---------- proto_data_name ---------- */

static void
test_data_access_chunk (void)
{
    /* HTLC_DATA_ACCESS / HTLS_DATA_ACCESS are the same opcode 0x6e
	 * by design (the 8-byte access bitmap chunk). The aliased name
	 * the trace prints is whatever proto_data_name's switch picks. */
    g_assert_cmpstr (proto_data_name (HTLS_DATA_ACCESS), ==,
                     "HTLC/S_DATA_ACCESS");
}

static void
test_data_taskerror (void)
{
    g_assert_cmpstr (proto_data_name (HTLS_DATA_TASKERROR), ==,
                     "HTLS_DATA_TASKERROR");
}

static void
test_data_version (void)
{
    g_assert_cmpstr (proto_data_name (HTLS_DATA_VERSION), ==,
                     "HTLS_DATA_VERSION");
}

static void
test_data_chat (void)
{
    /* HTLC_DATA_CHAT == HTLS_DATA_CHAT == 0x65, and the same opcode
	 * is reused for MSG / NEWS_POST / AGREEMENT / USER_INFO bodies.
	 * proto_trace.c returns the combined label so a [proto] dump
	 * line tells you all the meanings at a glance. */
    g_assert_cmpstr (
        proto_data_name (HTLS_DATA_CHAT), ==,
        "HTLC/S_DATA_CHAT/MSG/NEWS_POST/AGREEMENT/USER_INFO (0x65)");
}

static void
test_data_unknown_falls_back_to_hex (void)
{
    /* DATA_* are 16-bit opcodes; %04x for the hex fallback. */
    const char *out = proto_data_name ((guint16)0xbeefu);
    g_assert_nonnull (out);
    g_assert_cmpstr (out, ==, "0xbeef");
}

/* Phase B HOPE-Secure-Login chunks. Both directions share opcode so
 * proto_trace.c emits the combined "HTLC/S_" label. */
static void
test_data_hope_app_id (void)
{
    g_assert_cmpstr (proto_data_name (HTLC_DATA_HOPE_APP_ID), ==,
                     "HTLC/S_DATA_HOPE_APP_ID");
}

static void
test_data_hope_app_string (void)
{
    g_assert_cmpstr (proto_data_name (HTLS_DATA_HOPE_APP_STRING), ==,
                     "HTLC/S_DATA_HOPE_APP_STRING");
}

/* HOPE ChaCha20 extension cipher-negotiation chunks (Phase B/C). The
 * server-direction (0xec1/3/5/7) and client-direction (0xec2/4/6/8)
 * opcodes alternate, so we get distinct names per direction. */
static void
test_data_cipher_mode (void)
{
    g_assert_cmpstr (proto_data_name (HTLS_DATA_CIPHER_MODE), ==,
                     "HTLS_DATA_CIPHER_MODE");
    g_assert_cmpstr (proto_data_name (HTLC_DATA_CIPHER_MODE), ==,
                     "HTLC_DATA_CIPHER_MODE");
}

static void
test_data_cipher_ivec (void)
{
    g_assert_cmpstr (proto_data_name (HTLS_DATA_CIPHER_IVEC), ==,
                     "HTLS_DATA_CIPHER_IVEC");
    g_assert_cmpstr (proto_data_name (HTLC_DATA_CIPHER_IVEC), ==,
                     "HTLC_DATA_CIPHER_IVEC");
}

static void
test_data_checksum_alg (void)
{
    g_assert_cmpstr (proto_data_name (HTLS_DATA_CHECKSUM_ALG), ==,
                     "HTLS_DATA_CHECKSUM_ALG");
    g_assert_cmpstr (proto_data_name (HTLC_DATA_CHECKSUM_ALG), ==,
                     "HTLC_DATA_CHECKSUM_ALG");
}

static void
test_data_capabilities (void)
{
    /* Post-login feature-flag bitmask. Both directions share 0x01f0. */
    g_assert_cmpstr (proto_data_name (HTLS_DATA_CAPABILITIES), ==,
                     "HTLC/S_DATA_CAPABILITIES");
}

static void
test_data_history_entry (void)
{
    /* Server-only opcode — array element in a GET_CHAT_HISTORY reply. */
    g_assert_cmpstr (proto_data_name (HTLS_DATA_HISTORY_ENTRY), ==,
                     "HTLS_DATA_HISTORY_ENTRY");
}

static void
test_data_history_has_more (void)
{
    g_assert_cmpstr (proto_data_name (HTLS_DATA_HISTORY_HAS_MORE), ==,
                     "HTLS_DATA_HISTORY_HAS_MORE");
}

static void
test_hdr_get_chat_history (void)
{
    /* Request and reply share opcode 0x2bc (700) — combined label. */
    g_assert_cmpstr (proto_hdr_name (HTLC_HDR_GET_CHAT_HISTORY), ==,
                     "HTLC/S_HDR_GET_CHAT_HISTORY");
}

static void
test_hdr_file_getfolder (void)
{
    g_assert_cmpstr (proto_hdr_name (HTLC_HDR_FILE_GETFOLDER), ==,
                     "HTLC_HDR_FILE_GETFOLDER");
}

static void
test_hdr_file_putfolder (void)
{
    g_assert_cmpstr (proto_hdr_name (HTLC_HDR_FILE_PUTFOLDER), ==,
                     "HTLC_HDR_FILE_PUTFOLDER");
}

/* Voice-chat extension opcodes 600-606. 604 ICE is bidirectional and
 * carries the combined HTLC/S label, mirroring the existing
 * GET_CHAT_HISTORY convention. */
static void
test_hdr_voice_join (void)
{
    g_assert_cmpstr (proto_hdr_name (HTLC_HDR_VOICE_JOIN), ==,
                     "HTLC_HDR_VOICE_JOIN");
}

static void
test_hdr_voice_leave (void)
{
    g_assert_cmpstr (proto_hdr_name (HTLC_HDR_VOICE_LEAVE), ==,
                     "HTLC_HDR_VOICE_LEAVE");
}

static void
test_hdr_voice_sdp_offer (void)
{
    g_assert_cmpstr (proto_hdr_name (HTLS_HDR_VOICE_SDP_OFFER), ==,
                     "HTLS_HDR_VOICE_SDP_OFFER");
}

static void
test_hdr_voice_sdp_answer (void)
{
    g_assert_cmpstr (proto_hdr_name (HTLC_HDR_VOICE_SDP_ANSWER), ==,
                     "HTLC_HDR_VOICE_SDP_ANSWER");
}

static void
test_hdr_voice_ice_bidirectional (void)
{
    /* Same opcode for both directions — combined label. */
    g_assert_cmpstr (proto_hdr_name (HTLC_HDR_VOICE_ICE), ==,
                     "HTLC/S_HDR_VOICE_ICE");
    g_assert_cmpstr (proto_hdr_name (HTLS_HDR_VOICE_ICE), ==,
                     "HTLC/S_HDR_VOICE_ICE");
}

static void
test_hdr_voice_room_status (void)
{
    g_assert_cmpstr (proto_hdr_name (HTLS_HDR_VOICE_ROOM_STATUS), ==,
                     "HTLS_HDR_VOICE_ROOM_STATUS");
}

static void
test_hdr_voice_mute (void)
{
    g_assert_cmpstr (proto_hdr_name (HTLC_HDR_VOICE_MUTE), ==,
                     "HTLC_HDR_VOICE_MUTE");
}

static void
test_data_voice_sdp (void)
{
    g_assert_cmpstr (proto_data_name (HTLC_DATA_VOICE_SDP), ==,
                     "HTLC/S_DATA_VOICE_SDP");
}

static void
test_data_voice_ice (void)
{
    g_assert_cmpstr (proto_data_name (HTLC_DATA_VOICE_ICE), ==,
                     "HTLC/S_DATA_VOICE_ICE");
}

static void
test_data_voice_codec (void)
{
    g_assert_cmpstr (proto_data_name (HTLC_DATA_VOICE_CODEC), ==,
                     "HTLC/S_DATA_VOICE_CODEC");
}

static void
test_data_voice_muted (void)
{
    g_assert_cmpstr (proto_data_name (HTLC_DATA_VOICE_MUTED), ==,
                     "HTLC/S_DATA_VOICE_MUTED");
}

static void
test_data_voice_participants (void)
{
    /* Server-only — sent in VOICE_ROOM_STATUS (605) and the JOIN
     * reply. The C header doesn't define an HTLC_ alias because
     * clients never emit this field. */
    g_assert_cmpstr (proto_data_name (HTLS_DATA_VOICE_PARTICIPANTS), ==,
                     "HTLS_DATA_VOICE_PARTICIPANTS");
}

static void
test_data_unknown_zero_falls_back (void)
{
    const char *out = proto_data_name ((guint16)0u);
    g_assert_nonnull (out);
    g_assert_cmpstr (out, ==, "0x0000");
}

/* ---------- Static buffer reuse note ----------
 *
 * proto_hdr_name and proto_data_name each hand out a per-call static
 * buffer for the hex fallback path (single-threaded by virtue of
 * being called from the trace logger only). Document the contract
 * with a test: two consecutive calls to the same function with
 * different unknown opcodes must NOT both return identical strings —
 * the second call clobbers the first. (Callers must therefore use
 * the result before the next call.)
 *
 * If we ever switch to a thread-local or non-shared formatter, this
 * test changes — leave it here as the explicit canary.
 */
static void
test_hdr_unknown_buffer_is_overwritten_on_next_call (void)
{
    const char *first = proto_hdr_name (0x111111u);
    const char *second = proto_hdr_name (0x222222u);
    /* Same buffer pointer, same string content (the second call
	 * stomped the first). This is the documented behaviour. */
    g_assert_true (first == second);
    g_assert_cmpstr (first, ==, "0x222222");
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/proto_names/hdr_login", test_hdr_login);
    g_test_add_func ("/proto_names/hdr_chat", test_hdr_chat);
    g_test_add_func ("/proto_names/hdr_news_getfile", test_hdr_news_getfile);
    g_test_add_func ("/proto_names/hdr_user_getlist", test_hdr_user_getlist);
    g_test_add_func ("/proto_names/hdr_ping", test_hdr_ping);
    g_test_add_func ("/proto_names/hdr_task_reply", test_hdr_task_reply);
    g_test_add_func ("/proto_names/hdr_unknown_falls_back_to_hex",
                     test_hdr_unknown_falls_back_to_hex);
    g_test_add_func ("/proto_names/hdr_unknown_zero_falls_back",
                     test_hdr_unknown_zero_falls_back);

    g_test_add_func ("/proto_names/data_access_chunk", test_data_access_chunk);
    g_test_add_func ("/proto_names/data_taskerror", test_data_taskerror);
    g_test_add_func ("/proto_names/data_version", test_data_version);
    g_test_add_func ("/proto_names/data_chat", test_data_chat);
    g_test_add_func ("/proto_names/data_unknown_falls_back_to_hex",
                     test_data_unknown_falls_back_to_hex);
    g_test_add_func ("/proto_names/data_unknown_zero_falls_back",
                     test_data_unknown_zero_falls_back);

    g_test_add_func (
        "/proto_names/hdr_unknown_buffer_is_overwritten_on_next_call",
        test_hdr_unknown_buffer_is_overwritten_on_next_call);

    /* Phase B / HOPE ChaCha20 / chat-history / folder-xfer additions. */
    g_test_add_func ("/proto_names/data_hope_app_id", test_data_hope_app_id);
    g_test_add_func ("/proto_names/data_hope_app_string",
                     test_data_hope_app_string);
    g_test_add_func ("/proto_names/data_cipher_mode", test_data_cipher_mode);
    g_test_add_func ("/proto_names/data_cipher_ivec", test_data_cipher_ivec);
    g_test_add_func ("/proto_names/data_checksum_alg", test_data_checksum_alg);
    g_test_add_func ("/proto_names/data_capabilities", test_data_capabilities);
    g_test_add_func ("/proto_names/data_history_entry", test_data_history_entry);
    g_test_add_func ("/proto_names/data_history_has_more",
                     test_data_history_has_more);
    g_test_add_func ("/proto_names/hdr_get_chat_history",
                     test_hdr_get_chat_history);
    g_test_add_func ("/proto_names/hdr_file_getfolder",
                     test_hdr_file_getfolder);
    g_test_add_func ("/proto_names/hdr_file_putfolder",
                     test_hdr_file_putfolder);

    g_test_add_func ("/proto_names/hdr_voice_join", test_hdr_voice_join);
    g_test_add_func ("/proto_names/hdr_voice_leave", test_hdr_voice_leave);
    g_test_add_func ("/proto_names/hdr_voice_sdp_offer",
                     test_hdr_voice_sdp_offer);
    g_test_add_func ("/proto_names/hdr_voice_sdp_answer",
                     test_hdr_voice_sdp_answer);
    g_test_add_func ("/proto_names/hdr_voice_ice_bidirectional",
                     test_hdr_voice_ice_bidirectional);
    g_test_add_func ("/proto_names/hdr_voice_room_status",
                     test_hdr_voice_room_status);
    g_test_add_func ("/proto_names/hdr_voice_mute", test_hdr_voice_mute);
    g_test_add_func ("/proto_names/data_voice_sdp", test_data_voice_sdp);
    g_test_add_func ("/proto_names/data_voice_ice", test_data_voice_ice);
    g_test_add_func ("/proto_names/data_voice_codec", test_data_voice_codec);
    g_test_add_func ("/proto_names/data_voice_muted", test_data_voice_muted);
    g_test_add_func ("/proto_names/data_voice_participants",
                     test_data_voice_participants);

    return g_test_run ();
}
