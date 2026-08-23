//! Outgoing-message builders for the SEND path.
//!
//! Each `hx_send_*` transaction's chunk layout lives here as a
//! `build_*_chunks` helper; the caller pushes the resulting chunks onto
//! the socket via `hlwrite_chunks` (a separate step). This module is
//! purely the "build the message" half.
//!
//! ## API shape
//!
//! Each builder takes a typed request struct plus two caller-owned
//! buffers:
//!
//! - `chunks: &mut [HxChunk]` — populated with (tag, len, data-ptr)
//!   triples on success. The caller hands the slice to `hlwrite_chunks`
//!   which copies bytes out before the buffers go out of scope.
//! - `scratch: &mut [u8]` — backing storage for the integer chunks
//!   (style/uid/cid, big-endian). Variable-length payloads (the chat
//!   body, the message body) live in the request struct's `body`
//!   pointer; the FFI documents the lifetime constraint.
//!
//! Returns the number of chunks populated, or `0` on validation
//! failure (buffer too small / NULL pointer). Matches the C builder
//! return convention so the dispatch site can `if (hc > 0) hlwrite_chunks(...)`.
//!
//! ## What stays in C
//!
//! Text conversion (`gtkhx_text_for_wire`, the iconv-backed UTF-8 ↔
//! Mac Roman path in `src/text_util.c`) is the caller's responsibility —
//! the builder receives already-encoded body bytes. This keeps the
//! Rust crate free of GLib iconv dependencies.

use crate::messages::tag;

/// C-ABI mirror of `struct hx_chunk` in `src/proto_helpers.h`. Layout
/// must match exactly:
///
/// ```c
/// struct hx_chunk {
///     guint16 type;
///     guint16 len;
///     const void *data;
/// };
/// ```
///
/// On 64-bit hosts that's `u16 + u16 + 4 pad + 8-byte ptr` = 16 bytes.
/// `#[repr(C)]` reproduces the C compiler's natural alignment of the
/// pointer, so we don't spell the padding out.
#[repr(C)]
pub struct HxChunk {
    pub tag: u16,
    pub len: u16,
    pub data: *const u8,
}

// Compile-time pin on the HxChunk layout. Same discipline as the
// Phase R1 AEAD state's size_of assert (cipher.h has a paired
// `_Static_assert(sizeof(chacha_aead_state) == 48, ...)`). If a future
// edit reorders the fields or accidentally changes the ABI, this fails
// the build before any C caller can read garbage at runtime.
//
// The numbers below are derived from pointer-width so they hold on
// both 64-bit (size 16, data offset 8) and 32-bit (size 8, data
// offset 4) targets — both layouts a `#[repr(C)]` mirror of
// `{ u16, u16, const void * }` produces under natural alignment.
const _: () = {
    assert!(std::mem::offset_of!(HxChunk, tag) == 0);
    assert!(std::mem::offset_of!(HxChunk, len) == 2);
    assert!(std::mem::offset_of!(HxChunk, data) == std::mem::size_of::<*const u8>());
    assert!(std::mem::size_of::<HxChunk>() == 2 * std::mem::size_of::<*const u8>());
    assert!(std::mem::align_of::<HxChunk>() == std::mem::align_of::<*const u8>());
};

impl HxChunk {
    /// Fresh empty chunk (tag 0, len 0, NULL data). Used to pre-fill
    /// the caller's chunks buffer in tests; production callers
    /// overwrite every slot the builder fills.
    pub const EMPTY: HxChunk = HxChunk {
        tag: 0,
        len: 0,
        data: std::ptr::null(),
    };
}

// Builders here borrow `body` (and similar pointer fields) — the
// returned chunks reference the original buffer via raw pointers. The
// builders themselves never read those pointers; they only stash them
// into the chunk array for the C side to copy out via hlwrite_chunks.

/// Request data for [`build_chat_chunks`].
pub struct ChatRequest<'a> {
    /// Private-chat channel id, or 0 for the public/lobby chat. The
    /// C handler treats 0 as "no CHAT_ID chunk" — historical Hotline
    /// servers default to lobby for that case.
    pub cid: u32,
    /// Style bitmap (mIRC-ish flags). Always emitted as a u16 chunk.
    pub style: u16,
    /// Already-encoded body bytes (UTF-8 if CAP_TEXT_ENCODING was
    /// negotiated, Mac Roman otherwise — see `gtkhx_text_for_wire`).
    /// Empty bodies are legal and emit a zero-length CHAT chunk.
    pub body: &'a [u8],
}

/// Build the chunk array for `HTLC_HDR_CHAT`. Wire shape:
///
/// - `HTLC_DATA_STYLE` (`tag::STYLE`, 0x006d) — u16 BE, 2 bytes.
/// - `HTLC_DATA_CHAT` (`tag::BODY`, 0x0065) — body bytes (already encoded).
/// - `HTLC_DATA_CHAT_ID` (`tag::CHAT_ID`, 0x0072) — u32 BE, 4 bytes,
///   **only when `cid != 0`**. Public chat (cid == 0) omits this chunk.
///
/// Scratch usage: 6 bytes (style at +0, cid at +2). The chunks
/// `data` pointers reference into `scratch` and into `req.body`, so
/// both buffers must outlive the eventual `hlwrite_chunks` call.
///
/// Returns 2 (no cid) or 3 (with cid) on success, or 0 on validation
/// failure: `chunks` has fewer than 3 slots, `scratch` is shorter than
/// 6 bytes, or `body.len() > u16::MAX` (Hotline chunk lengths are
/// 16-bit on the wire — without this check, a Rust caller passing a
/// 65 KiB body would silently wrap and emit an invalid chunk).
pub fn build_chat_chunks(
    req: &ChatRequest<'_>,
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    if chunks.len() < 3 || scratch.len() < 6 || req.body.len() > u16::MAX as usize {
        return 0;
    }
    let with_cid = req.cid != 0;

    let style_be = req.style.to_be_bytes();
    let cid_be = req.cid.to_be_bytes();
    scratch[0..2].copy_from_slice(&style_be);
    scratch[2..6].copy_from_slice(&cid_be);

    chunks[0] = HxChunk {
        tag: tag::STYLE,
        len: 2,
        data: scratch.as_ptr(),
    };
    chunks[1] = HxChunk {
        tag: tag::BODY,
        len: req.body.len() as u16,
        // Empty body is legal: hand the C side a non-NULL pointer so
        // it doesn't trip a defensive null-check; len 0 means no bytes
        // are read anyway.
        data: if req.body.is_empty() {
            b"".as_ptr()
        } else {
            req.body.as_ptr()
        },
    };
    if with_cid {
        chunks[2] = HxChunk {
            tag: tag::CHAT_ID,
            len: 4,
            // Cid lives at scratch[2..6]; take a subslice and read its
            // pointer — safe and bounds-checked, vs. the manual
            // pointer-add this used to do.
            data: scratch[2..6].as_ptr(),
        };
        3
    } else {
        2
    }
}

/// Request data for [`build_msg_chunks`].
pub struct MsgRequest<'a> {
    /// Recipient user id. Always emitted as a u16 chunk.
    pub uid: u16,
    /// Already-encoded body bytes.
    pub body: &'a [u8],
}

/// Build the chunk array for `HTLC_HDR_MSG`. Wire shape:
///
/// - `HTLC_DATA_UID` (`tag::UID`, 0x0067) — u16 BE, 2 bytes.
/// - `HTLC_DATA_MSG` (`tag::BODY`, 0x0065) — body bytes.
///
/// Scratch usage: 2 bytes (the uid). Returns 2 on success, or 0 on
/// validation failure: too-small chunks / scratch buffers, or
/// `body.len() > u16::MAX` (chunk lengths are 16-bit on the wire).
pub fn build_msg_chunks(req: &MsgRequest<'_>, chunks: &mut [HxChunk], scratch: &mut [u8]) -> usize {
    if chunks.len() < 2 || scratch.len() < 2 || req.body.len() > u16::MAX as usize {
        return 0;
    }

    let uid_be = req.uid.to_be_bytes();
    scratch[0..2].copy_from_slice(&uid_be);

    chunks[0] = HxChunk {
        tag: tag::UID,
        len: 2,
        data: scratch.as_ptr(),
    };
    chunks[1] = HxChunk {
        tag: tag::BODY,
        len: req.body.len() as u16,
        data: if req.body.is_empty() {
            b"".as_ptr()
        } else {
            req.body.as_ptr()
        },
    };
    2
}

/// Request data for [`build_broadcast_chunks`].
pub struct BroadcastRequest<'a> {
    /// Already-encoded body bytes.
    pub body: &'a [u8],
}

/// Build the chunk array for `HTLC_HDR_MSG_BROADCAST`. Wire shape:
///
/// - `HTLC_DATA_MSG` (`tag::BODY`, 0x0065) — body bytes.
///
/// No scratch needed (no integer chunks). Returns 1 on success, or 0
/// on validation failure: empty `chunks` slice, or `body.len() >
/// u16::MAX` (chunk lengths are 16-bit on the wire).
pub fn build_broadcast_chunks(req: &BroadcastRequest<'_>, chunks: &mut [HxChunk]) -> usize {
    if chunks.is_empty() || req.body.len() > u16::MAX as usize {
        return 0;
    }
    chunks[0] = HxChunk {
        tag: tag::BODY,
        len: req.body.len() as u16,
        data: if req.body.is_empty() {
            b"".as_ptr()
        } else {
            req.body.as_ptr()
        },
    };
    1
}

// ---- Chat-admin send opcodes ------------------------------------------
//
// HTLC_HDR_CHAT_CREATE / _INVITE / _JOIN / _PART / _DECLINE / _SUBJECT
// are the chat-room management opcodes. Most are pure integer-chunk
// requests; the only one with a variable-length payload is
// CHAT_SUBJECT (the subject body).

/// Build chunks for `HTLC_HDR_CHAT_CREATE` — `HTLC_DATA_UID` (u16 BE)
/// only, 1 chunk. Requires `chunks_cap >= 1`, `scratch_cap >= 2`.
pub fn build_chat_create_chunks(uid: u16, chunks: &mut [HxChunk], scratch: &mut [u8]) -> usize {
    if chunks.is_empty() || scratch.len() < 2 {
        return 0;
    }
    scratch[0..2].copy_from_slice(&uid.to_be_bytes());
    chunks[0] = HxChunk {
        tag: tag::UID,
        len: 2,
        data: scratch.as_ptr(),
    };
    1
}

/// Build chunks for `HTLC_HDR_CHAT_INVITE` — `HTLC_DATA_CHAT_ID` (u32)
/// then `HTLC_DATA_UID` (u16). 2 chunks; `chunks_cap >= 2`,
/// `scratch_cap >= 6` (cid at offset 0, uid at offset 4).
pub fn build_chat_invite_chunks(
    cid: u32,
    uid: u16,
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    if chunks.len() < 2 || scratch.len() < 6 {
        return 0;
    }
    scratch[0..4].copy_from_slice(&cid.to_be_bytes());
    scratch[4..6].copy_from_slice(&uid.to_be_bytes());
    chunks[0] = HxChunk {
        tag: tag::CHAT_ID,
        len: 4,
        data: scratch.as_ptr(),
    };
    chunks[1] = HxChunk {
        tag: tag::UID,
        len: 2,
        // Uid lives at scratch[4..6]; subslice + as_ptr is safe and
        // bounds-checked, scratch.len() >= 6 already verified above.
        data: scratch[4..6].as_ptr(),
    };
    2
}

/// Internal helper: single-`CHAT_ID` chunk for the JOIN / PART / DECLINE
/// opcodes (all three share the wire shape). The caller picks the
/// header type when handing the chunks to `hlwrite_chunks`.
fn build_chat_id_only_chunks(cid: u32, chunks: &mut [HxChunk], scratch: &mut [u8]) -> usize {
    if chunks.is_empty() || scratch.len() < 4 {
        return 0;
    }
    scratch[0..4].copy_from_slice(&cid.to_be_bytes());
    chunks[0] = HxChunk {
        tag: tag::CHAT_ID,
        len: 4,
        data: scratch.as_ptr(),
    };
    1
}

/// Build chunks for `HTLC_HDR_CHAT_JOIN` — `HTLC_DATA_CHAT_ID` only.
pub fn build_chat_join_chunks(cid: u32, chunks: &mut [HxChunk], scratch: &mut [u8]) -> usize {
    build_chat_id_only_chunks(cid, chunks, scratch)
}

/// Build chunks for `HTLC_HDR_CHAT_PART` — `HTLC_DATA_CHAT_ID` only.
pub fn build_chat_part_chunks(cid: u32, chunks: &mut [HxChunk], scratch: &mut [u8]) -> usize {
    build_chat_id_only_chunks(cid, chunks, scratch)
}

/// Build chunks for `HTLC_HDR_CHAT_DECLINE` — `HTLC_DATA_CHAT_ID` only.
pub fn build_chat_decline_chunks(cid: u32, chunks: &mut [HxChunk], scratch: &mut [u8]) -> usize {
    build_chat_id_only_chunks(cid, chunks, scratch)
}

/// Request data for [`build_chat_subject_chunks`].
pub struct ChatSubjectRequest<'a> {
    pub cid: u32,
    /// Already-encoded subject bytes (UTF-8 or Mac Roman per
    /// CAP_TEXT_ENCODING). Empty subject is legal.
    pub subject: &'a [u8],
}

/// Build chunks for `HTLC_HDR_CHAT_SUBJECT` — `HTLC_DATA_CHAT_ID` (u32) +
/// `HTLC_DATA_CHAT_SUBJECT` (bytes). 2 chunks; `chunks_cap >= 2`,
/// `scratch_cap >= 4`. Also rejects `subject.len() > u16::MAX` (the
/// wire chunk length is 16-bit; without this check a 65 KiB subject
/// would silently wrap and emit an invalid chunk).
pub fn build_chat_subject_chunks(
    req: &ChatSubjectRequest<'_>,
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    if chunks.len() < 2 || scratch.len() < 4 || req.subject.len() > u16::MAX as usize {
        return 0;
    }
    scratch[0..4].copy_from_slice(&req.cid.to_be_bytes());
    chunks[0] = HxChunk {
        tag: tag::CHAT_ID,
        len: 4,
        data: scratch.as_ptr(),
    };
    chunks[1] = HxChunk {
        tag: tag::CHAT_SUBJECT,
        len: req.subject.len() as u16,
        data: if req.subject.is_empty() {
            b"".as_ptr()
        } else {
            req.subject.as_ptr()
        },
    };
    2
}

// ---- HTLC_HDR_GET_CHAT_HISTORY (700) ----------------------------------
//
// fogWraith chat-history extension. Wire shape: a mandatory CHANNEL_ID (u32)
// plus up to three optional cursor / limit chunks, each emitted only when its
// value is non-zero (the server treats absence as "default"):
//   BEFORE (u64) → messages older than this id
//   AFTER  (u64) → messages newer than this id (reconnect catch-up)
//   LIMIT  (u16) → max results (0 = server default)

/// Request data for [`build_get_chat_history_chunks`].
pub struct GetChatHistoryRequest {
    pub channel_id: u32,
    /// "before" cursor; omitted when 0.
    pub before: u64,
    /// "after" cursor; omitted when 0.
    pub after: u64,
    /// max-results limit; omitted when 0.
    pub limit: u16,
}

/// Build the chunk array for `HTLC_HDR_GET_CHAT_HISTORY`. `channel_id` is
/// always emitted; `before` / `after` / `limit` are emitted only when non-zero,
/// so the count is 1..4. Scratch layout: channel @0..4, before @4..12,
/// after @12..20, limit @20..22 (22 bytes). Returns the chunk count, or 0 if
/// `chunks` has fewer than 4 slots or `scratch` fewer than 22 bytes.
pub fn build_get_chat_history_chunks(
    req: &GetChatHistoryRequest,
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    if chunks.len() < 4 || scratch.len() < 22 {
        return 0;
    }
    scratch[0..4].copy_from_slice(&req.channel_id.to_be_bytes());
    scratch[4..12].copy_from_slice(&req.before.to_be_bytes());
    scratch[12..20].copy_from_slice(&req.after.to_be_bytes());
    scratch[20..22].copy_from_slice(&req.limit.to_be_bytes());

    let mut hc = 0;
    chunks[hc] = HxChunk {
        tag: tag::CHANNEL_ID,
        len: 4,
        data: scratch[0..4].as_ptr(),
    };
    hc += 1;
    if req.before != 0 {
        chunks[hc] = HxChunk {
            tag: tag::HISTORY_BEFORE,
            len: 8,
            data: scratch[4..12].as_ptr(),
        };
        hc += 1;
    }
    if req.after != 0 {
        chunks[hc] = HxChunk {
            tag: tag::HISTORY_AFTER,
            len: 8,
            data: scratch[12..20].as_ptr(),
        };
        hc += 1;
    }
    if req.limit != 0 {
        chunks[hc] = HxChunk {
            tag: tag::HISTORY_LIMIT,
            len: 2,
            data: scratch[20..22].as_ptr(),
        };
        hc += 1;
    }
    hc
}

// ---- HTLC_HDR_AGREEMENTAGREE ------------------------------------------
//
// Wire shape: ICON + NAME + OPTIONS, all three mandatory (Mobius panics
// without OPTIONS). The C call sites (network.c::hx_send_agreement_agree
// and the integration harness) reach this through the
// `gtkhx_proto_build_agreement_agree_chunks` FFI shim.

/// Request data for [`build_agreement_agree_chunks`].
pub struct AgreementAgreeRequest<'a> {
    /// HTLC_DATA_ICON value. Always emitted.
    pub icon: u16,
    /// HTLC_DATA_NAME body, already encoded to the negotiated wire
    /// encoding. Empty is legal (zero-length NAME chunk).
    pub display_name: &'a [u8],
    /// HTLC_DATA_OPTIONS bitmap. Mandatory — Mobius panics without it,
    /// see the long comment in `src/network.c::hx_send_agreement_agree`.
    pub options: u16,
}

/// Build the chunk array for `HTLC_HDR_AGREEMENTAGREE`. Always 3
/// chunks (ICON + NAME + OPTIONS). Requires `chunks_cap >= 3`,
/// `scratch_cap >= 4` (icon at offset 0, options at offset 2). Also
/// rejects `display_name.len() > u16::MAX` (chunk lengths are 16-bit
/// on the wire — without this check a 65 KiB nick would silently
/// wrap and emit an invalid chunk).
pub fn build_agreement_agree_chunks(
    req: &AgreementAgreeRequest<'_>,
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    if chunks.len() < 3 || scratch.len() < 4 || req.display_name.len() > u16::MAX as usize {
        return 0;
    }
    scratch[0..2].copy_from_slice(&req.icon.to_be_bytes());
    scratch[2..4].copy_from_slice(&req.options.to_be_bytes());

    chunks[0] = HxChunk {
        tag: tag::ICON,
        len: 2,
        data: scratch.as_ptr(),
    };
    chunks[1] = HxChunk {
        tag: tag::NAME,
        len: req.display_name.len() as u16,
        data: if req.display_name.is_empty() {
            b"".as_ptr()
        } else {
            req.display_name.as_ptr()
        },
    };
    chunks[2] = HxChunk {
        tag: tag::OPTIONS,
        len: 2,
        // Options lives at scratch[2..4]; subslice + as_ptr is safe
        // and bounds-checked, scratch.len() >= 4 already verified.
        data: scratch[2..4].as_ptr(),
    };
    3
}

// ---- User-management send opcodes ------------------------------------
//
// HTLC_HDR_USER_CHANGE: client pushes its current ICON / NAME (and
// optionally the Colored-Nicknames COLOR) — the server broadcasts it
// back as HTLS_HDR_USER_CHANGE to everyone in the chat.
// HTLC_HDR_USER_KICK: kick-with-optional-ban (the BAN flag chunk shares
// 0x0071 with OPTIONS, see the `tag::BAN` doc).
// HTLC_HDR_USER_GETINFO: ask the server for an arbitrary user's info
// (the reply is a TASK whose body carries the user-info text).

/// Request data for [`build_user_change_chunks`]. Mirrors the C
/// `hx_change_name_icon` call site in `src/users.c`.
pub struct UserChangeRequest<'a> {
    /// HTLC_DATA_ICON value. Always emitted.
    pub icon: u16,
    /// HTLC_DATA_NAME body, already encoded to the negotiated wire
    /// encoding (UTF-8 or Mac Roman per CAP_TEXT_ENCODING). Empty is
    /// legal (zero-length NAME chunk).
    pub name: &'a [u8],
    /// Colored-Nicknames extension. When `Some(c)`, emit
    /// HTLC_DATA_COLOR (BE u32 0x00RRGGBB); when `None`, omit the
    /// chunk entirely. The C side passes `None` for
    /// `HX_NICK_COLOR_NONE` (the spec's auto-opt-in fires on first
    /// DATA_COLOR receipt regardless of value, and a "no color"
    /// client shouldn't opt in — see the long comment in
    /// hx_change_name_icon for the rationale).
    pub nick_color: Option<u32>,
}

/// Build the chunk array for `HTLC_HDR_USER_CHANGE`. Wire shape:
///
/// - `HTLC_DATA_ICON` (`tag::ICON`, 0x0068) — u16 BE.
/// - `HTLC_DATA_NAME` (`tag::NAME`, 0x0066) — body bytes.
/// - `HTLC_DATA_COLOR` (`tag::COLOR`, 0x0500) — u32 BE — **only when
///   `nick_color.is_some()`**. Servers that don't know the
///   Colored-Nicknames extension ignore this trailing chunk; supporting
///   servers mark the session color-aware on first DATA_COLOR receipt.
///
/// Returns 2 (no color) or 3 (with color) on success, or 0 on
/// validation failure: too-small chunks / scratch buffers, or
/// `name.len() > u16::MAX` (16-bit chunk-length boundary).
///
/// Scratch layout: icon at +0 (2 bytes), color at +2 (4 bytes). The
/// chunks' data pointers reference into `scratch` and into `req.name`,
/// so both must outlive the eventual `hlwrite_chunks` call.
pub fn build_user_change_chunks(
    req: &UserChangeRequest<'_>,
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    if chunks.len() < 3 || scratch.len() < 6 || req.name.len() > u16::MAX as usize {
        return 0;
    }

    scratch[0..2].copy_from_slice(&req.icon.to_be_bytes());

    chunks[0] = HxChunk {
        tag: tag::ICON,
        len: 2,
        data: scratch.as_ptr(),
    };
    chunks[1] = HxChunk {
        tag: tag::NAME,
        len: req.name.len() as u16,
        data: if req.name.is_empty() {
            b"".as_ptr()
        } else {
            req.name.as_ptr()
        },
    };
    if let Some(c) = req.nick_color {
        scratch[2..6].copy_from_slice(&c.to_be_bytes());
        chunks[2] = HxChunk {
            tag: tag::COLOR,
            len: 4,
            data: scratch[2..6].as_ptr(),
        };
        3
    } else {
        2
    }
}

/// Request data for [`build_user_kick_chunks`]. Mirrors the C
/// `hx_kick_user` call site: the BAN flag is emitted only when
/// `ban != 0`, and (matching the wire ordering used in production)
/// BAN comes BEFORE UID.
pub struct UserKickRequest {
    pub uid: u16,
    /// Non-zero means "also ban" (the BAN chunk is emitted with this
    /// value); zero suppresses the BAN chunk entirely.
    pub ban: u16,
}

/// Build the chunk array for `HTLC_HDR_USER_KICK`. Wire shape:
///
/// - `HTLC_DATA_BAN` (`tag::BAN`, 0x0071) — u16 BE — only when
///   `ban != 0`. Emitted FIRST when present, matching the C call site.
/// - `HTLC_DATA_UID` (`tag::UID`, 0x0067) — u16 BE.
///
/// Returns 1 (no ban) or 2 (with ban) on success, or 0 on a too-small
/// chunks / scratch buffer. Scratch layout: ban at +0 (2 bytes), uid
/// at +2 (2 bytes).
pub fn build_user_kick_chunks(
    req: &UserKickRequest,
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    if chunks.len() < 2 || scratch.len() < 4 {
        return 0;
    }

    let with_ban = req.ban != 0;
    let uid_be = req.uid.to_be_bytes();

    if with_ban {
        let ban_be = req.ban.to_be_bytes();
        scratch[0..2].copy_from_slice(&ban_be);
        scratch[2..4].copy_from_slice(&uid_be);
        chunks[0] = HxChunk {
            tag: tag::BAN,
            len: 2,
            data: scratch.as_ptr(),
        };
        chunks[1] = HxChunk {
            tag: tag::UID,
            len: 2,
            data: scratch[2..4].as_ptr(),
        };
        2
    } else {
        scratch[0..2].copy_from_slice(&uid_be);
        chunks[0] = HxChunk {
            tag: tag::UID,
            len: 2,
            data: scratch.as_ptr(),
        };
        1
    }
}

/// Build the chunk array for `HTLC_HDR_USER_GETINFO`. Single
/// `HTLC_DATA_UID` chunk (u16 BE). Returns 1 on success, 0 on a
/// too-small chunks / scratch buffer.
pub fn build_user_getinfo_chunks(uid: u16, chunks: &mut [HxChunk], scratch: &mut [u8]) -> usize {
    if chunks.is_empty() || scratch.len() < 2 {
        return 0;
    }
    scratch[0..2].copy_from_slice(&uid.to_be_bytes());
    chunks[0] = HxChunk {
        tag: tag::UID,
        len: 2,
        data: scratch.as_ptr(),
    };
    1
}

// ---- Account-management send opcodes ---------------------------------
//
// HTLC_HDR_ACCOUNT_READ:   ask the server for an account's fields
//                          (reply is a TASK carrying the user record).
// HTLC_HDR_ACCOUNT_MODIFY: create / overwrite an account
//                          (LOGIN + PASSWORD + NAME + ACCESS).
// HTLC_HDR_ACCOUNT_DELETE: remove an account by login.
//
// Login and password fields use the Hotline obfuscation (XOR with 0xFF),
// applied by the caller before invoking these builders. The builders
// themselves treat all the byte fields as opaque payload — same
// discipline as the chat / msg body fields.

/// Build the chunk array for `HTLC_HDR_ACCOUNT_READ`. Single
/// `HTLC_DATA_LOGIN` chunk. Returns 1 on success, 0 on a too-small
/// chunks buffer or `login.len() > u16::MAX`.
///
/// Note: the C `hx_useredit_open` call site passes the login bytes
/// *unencoded* (a deliberate mhxd convention — READ takes a raw login,
/// MODIFY / DELETE take an hl_encoded one). Either way, the encoding
/// decision belongs to the caller.
pub fn build_account_read_chunks(login: &[u8], chunks: &mut [HxChunk]) -> usize {
    if chunks.is_empty() || login.len() > u16::MAX as usize {
        return 0;
    }
    chunks[0] = HxChunk {
        tag: tag::LOGIN,
        len: login.len() as u16,
        data: if login.is_empty() {
            b"".as_ptr()
        } else {
            login.as_ptr()
        },
    };
    1
}

/// Build the chunk array for `HTLC_HDR_ACCOUNT_DELETE`. Same wire
/// shape as ACCOUNT_READ (a single `HTLC_DATA_LOGIN` chunk); the
/// caller picks the header opcode. Returns 1 on success, 0 on a
/// too-small chunks buffer or `login.len() > u16::MAX`.
pub fn build_account_delete_chunks(login: &[u8], chunks: &mut [HxChunk]) -> usize {
    // Same wire shape as READ — reuse the helper.
    build_account_read_chunks(login, chunks)
}

/// Request data for [`build_account_modify_chunks`]. Mirrors the C
/// `hx_useredit_create` call site in `src/usermod.c`. The login,
/// password, and name fields are already-encoded byte buffers (the
/// caller has applied `hl_encode` to LOGIN / PASSWORD where
/// appropriate).
pub struct AccountModifyRequest<'a> {
    /// `HTLC_DATA_LOGIN` body. hl_encoded by the caller.
    pub login: &'a [u8],
    /// `HTLC_DATA_PASSWORD` body. hl_encoded by the caller. The C
    /// convention for an empty password is a single 0x00 byte
    /// (i.e. `password = &[0]`), not a zero-length slice — see the
    /// `if (!*pass)` branch in `hx_useredit_create`.
    pub password: &'a [u8],
    /// `HTLC_DATA_NAME` body. Plain text; the wire format here does
    /// not apply `hl_encode` to NAME.
    pub name: &'a [u8],
    /// `HTLC_DATA_ACCESS` body — 8 raw wire bytes (the
    /// `hl_access_bits` bitmap, big-endian-in-memory). `hl_access.h`
    /// is the canonical reference.
    pub access: [u8; 8],
}

/// Build the chunk array for `HTLC_HDR_ACCOUNT_MODIFY`. Wire shape
/// (the order matches the C call site):
///
/// - `HTLC_DATA_LOGIN`    — bytes (hl_encoded).
/// - `HTLC_DATA_PASSWORD` — bytes (hl_encoded).
/// - `HTLC_DATA_NAME`     — bytes (plain).
/// - `HTLC_DATA_ACCESS`   — 8 raw bytes (the access bitmap).
///
/// Returns 4 on success. Requires `chunks_cap >= 4`, `scratch_cap >= 8`
/// (the access bytes live in `scratch`). Rejects any of login /
/// password / name longer than `u16::MAX` (the 16-bit chunk-length
/// boundary).
pub fn build_account_modify_chunks(
    req: &AccountModifyRequest<'_>,
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    if chunks.len() < 4
        || scratch.len() < 8
        || req.login.len() > u16::MAX as usize
        || req.password.len() > u16::MAX as usize
        || req.name.len() > u16::MAX as usize
    {
        return 0;
    }

    scratch[0..8].copy_from_slice(&req.access);

    chunks[0] = HxChunk {
        tag: tag::LOGIN,
        len: req.login.len() as u16,
        data: if req.login.is_empty() {
            b"".as_ptr()
        } else {
            req.login.as_ptr()
        },
    };
    chunks[1] = HxChunk {
        tag: tag::PASSWORD,
        len: req.password.len() as u16,
        data: if req.password.is_empty() {
            b"".as_ptr()
        } else {
            req.password.as_ptr()
        },
    };
    chunks[2] = HxChunk {
        tag: tag::NAME,
        len: req.name.len() as u16,
        data: if req.name.is_empty() {
            b"".as_ptr()
        } else {
            req.name.as_ptr()
        },
    };
    chunks[3] = HxChunk {
        tag: tag::ACCESS,
        len: 8,
        data: scratch.as_ptr(),
    };
    4
}

// ---- News send opcodes -----------------------------------------------
//
// HTLC_HDR_NEWS_POST: 1.0 flat-news post (HTLC_DATA_NEWS_POST body —
//                     same tag as BODY/MSG/etc., reused per opcode).
// HTLC_HDR_NEWSCATLIST / _NEWSDIRLIST / _DELNEWSDIRCAT / _MAKENEWSDIR:
//                     1.5 threaded-news opcodes whose request body is
//                     a single HTLC_DATA_NEWSPATH chunk. The path
//                     encoding is the caller's responsibility
//                     (`path_to_hldir` on the C side).

/// Build the chunk array for `HTLC_HDR_NEWS_POST` (1.0 flat news).
/// Single chunk: `HTLC_DATA_NEWS_POST` body. Returns 1 on success, or
/// 0 on a too-small `chunks` slice or `body.len() > u16::MAX`.
///
/// Note: `HTLC_DATA_NEWS_POST` shares the 0x0065 tag with BODY / MSG /
/// CHAT / AGREEMENT — same opcode-distinct reuse pattern the protocol
/// uses everywhere. The Rust crate spells this as [`tag::BODY`].
pub fn build_news_post_chunks(body: &[u8], chunks: &mut [HxChunk]) -> usize {
    if chunks.is_empty() || body.len() > u16::MAX as usize {
        return 0;
    }
    chunks[0] = HxChunk {
        tag: tag::BODY,
        len: body.len() as u16,
        data: if body.is_empty() {
            b"".as_ptr()
        } else {
            body.as_ptr()
        },
    };
    1
}

/// Internal helper for the four NEWSPATH-only 1.5 news opcodes. Each
/// public wrapper picks the matching header type when handing the
/// chunks to `hlwrite_chunks`. Returns 1 on success, 0 on too-small
/// `chunks` slice or `path.len() > u16::MAX`.
fn build_newspath_only_chunks(path: &[u8], chunks: &mut [HxChunk]) -> usize {
    if chunks.is_empty() || path.len() > u16::MAX as usize {
        return 0;
    }
    chunks[0] = HxChunk {
        tag: tag::NEWSPATH,
        len: path.len() as u16,
        data: if path.is_empty() {
            b"".as_ptr()
        } else {
            path.as_ptr()
        },
    };
    1
}

/// Build the chunk array for `HTLC_HDR_NEWSCATLIST` — single
/// `HTLC_DATA_NEWSPATH` chunk.
pub fn build_news_catlist_chunks(path: &[u8], chunks: &mut [HxChunk]) -> usize {
    build_newspath_only_chunks(path, chunks)
}

/// Build the chunk array for `HTLC_HDR_NEWSDIRLIST` — single
/// `HTLC_DATA_NEWSPATH` chunk.
pub fn build_news_dirlist_chunks(path: &[u8], chunks: &mut [HxChunk]) -> usize {
    build_newspath_only_chunks(path, chunks)
}

/// Build the chunk array for `HTLC_HDR_DELNEWSDIRCAT` — single
/// `HTLC_DATA_NEWSPATH` chunk. The same wire shape works for deleting
/// either a category or a folder; mhxd inspects the path to decide.
pub fn build_news_delete_chunks(path: &[u8], chunks: &mut [HxChunk]) -> usize {
    build_newspath_only_chunks(path, chunks)
}

/// Request data for [`build_news_mkdir_chunks`].
pub struct NewsMakeDirRequest<'a> {
    /// The *parent* folder's wire-encoded path. Empty for the root.
    pub path: &'a [u8],
    /// New folder name, already encoded by the caller.
    pub name: &'a [u8],
}

/// Build the chunk array for `HTLC_HDR_MAKENEWSDIR` — `HTLC_DATA_NEWSPATH`
/// (the *parent*) + `HTLC_DATA_FILE_NAME` (the new folder). Returns 2 on
/// success, 0 on validation failure. `chunks.len() >= 2`.
///
/// This used to emit a lone NEWSPATH whose last component was the new name, on
/// the assumption that MAKENEWSDIR had the same shape as NEWSDIRLIST. It does
/// not, and the reference server says so plainly: `rcv_news_mkdir` reads
/// NEWSPATH and FILE_NAME as separate fields, resolves NEWSPATH through
/// `hldir_to_path` — which requires every component to already exist — and
/// creates `<resolved>/<FILE_NAME>`. Folding the new name into the path asked
/// the server to resolve a directory that by definition wasn't there yet, so
/// the request came back ENOENT ("No such file or directory") and no folder was
/// ever created. Verified against mhxd both ways round.
pub fn build_news_mkdir_chunks(req: &NewsMakeDirRequest<'_>, chunks: &mut [HxChunk]) -> usize {
    if chunks.len() < 2 || req.path.len() > u16::MAX as usize || req.name.len() > u16::MAX as usize
    {
        return 0;
    }
    chunks[0] = HxChunk {
        tag: tag::NEWSPATH,
        len: req.path.len() as u16,
        data: if req.path.is_empty() {
            b"".as_ptr()
        } else {
            req.path.as_ptr()
        },
    };
    chunks[1] = HxChunk {
        tag: tag::FILE_NAME,
        len: req.name.len() as u16,
        data: if req.name.is_empty() {
            b"".as_ptr()
        } else {
            req.name.as_ptr()
        },
    };
    2
}

// ---- 1.5 news send opcodes with extra fields -------------------------
//
// Build on top of the NEWSPATH-only shape with one or more additional
// chunks: THREADID (u32), NEWSTYPE / NEWSSUBJECT / NEWSDATA /
// CATEGORY (byte payloads), PARENTTHREAD (u32, gtkhx always sends 0).
//
// All the variable-length payloads (mime type, subject, body, category
// name) are pre-encoded by the C caller via `gtkhx_text_for_wire`;
// the builders treat them as opaque byte buffers — same discipline as
// chat / msg / agreement-agree.

/// Request data for [`build_news_delete_thread_chunks`].
pub struct NewsDeleteThreadRequest<'a> {
    pub path: &'a [u8],
    pub threadid: u32,
}

/// Build the chunk array for `HTLC_HDR_DELETETHREAD` — NEWSPATH +
/// THREADID. Returns 2 on success, 0 on validation failure.
/// `chunks_cap >= 2`, `scratch_cap >= 4` (the u32 threadid).
pub fn build_news_delete_thread_chunks(
    req: &NewsDeleteThreadRequest<'_>,
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    if chunks.len() < 2 || scratch.len() < 4 || req.path.len() > u16::MAX as usize {
        return 0;
    }
    scratch[0..4].copy_from_slice(&req.threadid.to_be_bytes());
    chunks[0] = HxChunk {
        tag: tag::NEWSPATH,
        len: req.path.len() as u16,
        data: if req.path.is_empty() {
            b"".as_ptr()
        } else {
            req.path.as_ptr()
        },
    };
    chunks[1] = HxChunk {
        tag: tag::THREADID,
        len: 4,
        data: scratch.as_ptr(),
    };
    2
}

/// Request data for [`build_news_getthread_chunks`].
pub struct NewsGetThreadRequest<'a> {
    pub path: &'a [u8],
    pub threadid: u32,
    /// MIME type bytes (e.g. b"text/plain"). Already encoded by the
    /// caller; ASCII passes through both encoding paths verbatim.
    pub mime_type: &'a [u8],
}

/// Build the chunk array for `HTLC_HDR_GETTHREAD` — NEWSPATH +
/// THREADID + NEWSTYPE. 3 chunks; `chunks_cap >= 3`, `scratch_cap >= 4`.
/// Returns 3 on success, 0 on validation failure (including
/// `path.len() > u16::MAX` or `mime_type.len() > u16::MAX`).
pub fn build_news_getthread_chunks(
    req: &NewsGetThreadRequest<'_>,
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    if chunks.len() < 3
        || scratch.len() < 4
        || req.path.len() > u16::MAX as usize
        || req.mime_type.len() > u16::MAX as usize
    {
        return 0;
    }
    scratch[0..4].copy_from_slice(&req.threadid.to_be_bytes());
    chunks[0] = HxChunk {
        tag: tag::NEWSPATH,
        len: req.path.len() as u16,
        data: if req.path.is_empty() {
            b"".as_ptr()
        } else {
            req.path.as_ptr()
        },
    };
    chunks[1] = HxChunk {
        tag: tag::THREADID,
        len: 4,
        data: scratch.as_ptr(),
    };
    chunks[2] = HxChunk {
        tag: tag::NEWSTYPE,
        len: req.mime_type.len() as u16,
        data: if req.mime_type.is_empty() {
            b"".as_ptr()
        } else {
            req.mime_type.as_ptr()
        },
    };
    3
}

/// Request data for [`build_news_mkcat_chunks`].
pub struct NewsMakeCategoryRequest<'a> {
    pub path: &'a [u8],
    /// New category name, already encoded by the caller.
    pub name: &'a [u8],
}

/// Build the chunk array for `HTLC_HDR_MAKECATEGORY` — NEWSPATH +
/// CATEGORY. 2 chunks; the `chunks` slice must have at least 2 slots.
/// No scratch needed. Returns 2 on success, 0 on validation failure
/// (`chunks.len() < 2`, `path.len() > u16::MAX`, or `name.len() >
/// u16::MAX`). NULL-pointer rejects live in the FFI shim
/// `gtkhx_proto_build_news_mkcat_chunks` — at the Rust level the
/// arguments are slices and a reference, so they can't be null.
pub fn build_news_mkcat_chunks(req: &NewsMakeCategoryRequest<'_>, chunks: &mut [HxChunk]) -> usize {
    if chunks.len() < 2 || req.path.len() > u16::MAX as usize || req.name.len() > u16::MAX as usize
    {
        return 0;
    }
    chunks[0] = HxChunk {
        tag: tag::NEWSPATH,
        len: req.path.len() as u16,
        data: if req.path.is_empty() {
            b"".as_ptr()
        } else {
            req.path.as_ptr()
        },
    };
    chunks[1] = HxChunk {
        tag: tag::CATEGORY,
        len: req.name.len() as u16,
        data: if req.name.is_empty() {
            b"".as_ptr()
        } else {
            req.name.as_ptr()
        },
    };
    2
}

/// Request data for [`build_news_post_thread_chunks`]. Mirrors the C
/// `hx_news15_post_thread` call site: subject, text, and mime type are
/// pre-encoded bytes; PARENTTHREAD is opaque to mhxd (the C side
/// always sends 0) but the wire spec requires the chunk to be
/// present.
pub struct NewsPostThreadRequest<'a> {
    pub path: &'a [u8],
    /// PARENTTHREAD chunk value. gtkhx always sends 0; the wire shape
    /// requires the chunk regardless of value.
    pub parent_thread: u32,
    /// MIME type bytes (the C call site hard-codes "text/plain").
    pub mime_type: &'a [u8],
    /// Single-line subject bytes, already encoded by the caller's
    /// `gtkhx_text_for_wire` (called with `is_body = FALSE` so the
    /// LF→CR send-path normalisation is skipped — subjects don't
    /// carry line endings).
    pub subject: &'a [u8],
    /// Article body bytes, already encoded by the caller's
    /// `gtkhx_text_for_wire` (called with `is_body = TRUE` so the
    /// LF→CR send-path normalisation is applied for legacy Mac
    /// servers).
    pub text: &'a [u8],
    /// THREADID for the post being replied to. mhxd writes this into
    /// the new post's `References:` header.
    pub thread_id: u32,
}

/// Build the chunk array for `HTLC_HDR_POSTTHREAD` — 6 chunks in this
/// wire order:
///
/// 1. `HTLC_DATA_NEWSPATH`
/// 2. `HTLC_DATA_PARENTTHREAD`  (u32 BE; gtkhx always sends 0)
/// 3. `HTLC_DATA_NEWSTYPE`      (e.g. "text/plain")
/// 4. `HTLC_DATA_NEWSSUBJECT`
/// 5. `HTLC_DATA_NEWSDATA`
/// 6. `HTLC_DATA_THREADID`      (u32 BE; the article being replied to)
///
/// `chunks_cap >= 6`, `scratch_cap >= 8` (two u32s — parent at +0,
/// thread at +4). Returns 6 on success, 0 on validation failure.
pub fn build_news_post_thread_chunks(
    req: &NewsPostThreadRequest<'_>,
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    if chunks.len() < 6
        || scratch.len() < 8
        || req.path.len() > u16::MAX as usize
        || req.mime_type.len() > u16::MAX as usize
        || req.subject.len() > u16::MAX as usize
        || req.text.len() > u16::MAX as usize
    {
        return 0;
    }
    scratch[0..4].copy_from_slice(&req.parent_thread.to_be_bytes());
    scratch[4..8].copy_from_slice(&req.thread_id.to_be_bytes());

    chunks[0] = HxChunk {
        tag: tag::NEWSPATH,
        len: req.path.len() as u16,
        data: if req.path.is_empty() {
            b"".as_ptr()
        } else {
            req.path.as_ptr()
        },
    };
    chunks[1] = HxChunk {
        tag: tag::PARENTTHREAD,
        len: 4,
        data: scratch.as_ptr(),
    };
    chunks[2] = HxChunk {
        tag: tag::NEWSTYPE,
        len: req.mime_type.len() as u16,
        data: if req.mime_type.is_empty() {
            b"".as_ptr()
        } else {
            req.mime_type.as_ptr()
        },
    };
    chunks[3] = HxChunk {
        tag: tag::NEWSSUBJECT,
        len: req.subject.len() as u16,
        data: if req.subject.is_empty() {
            b"".as_ptr()
        } else {
            req.subject.as_ptr()
        },
    };
    chunks[4] = HxChunk {
        tag: tag::NEWSDATA,
        len: req.text.len() as u16,
        data: if req.text.is_empty() {
            b"".as_ptr()
        } else {
            req.text.as_ptr()
        },
    };
    chunks[5] = HxChunk {
        tag: tag::THREADID,
        len: 4,
        data: scratch[4..8].as_ptr(),
    };
    6
}

// ---- File send opcodes -----------------------------------------------
//
// HTLC_HDR_FILE_MKDIR:   create a directory (single HTLC_DATA_DIR
//                        chunk).
// HTLC_HDR_FILE_DELETE / _GETINFO / _GETFOLDER:
//                        FILE_NAME + optional DIR. The presence of
//                        DIR distinguishes "file in a subdir"
//                        (FILE_NAME = basename, DIR = parent path)
//                        from "file at the root" (FILE_NAME alone).
//
// All variable-length payloads (file name, directory path) are
// pre-encoded by the C caller: gtkhx_text_for_wire handles UTF-8 /
// Mac Roman conversion for the filename; path_to_hldir builds the DIR
// chunk bytes verbatim. The Rust crate treats both as opaque byte
// buffers.

/// Build the chunk array for `HTLC_HDR_FILE_MKDIR` — single
/// `HTLC_DATA_DIR` chunk. Returns 1 on success, or 0 on a too-small
/// `chunks` slice or `dir.len() > u16::MAX`.
pub fn build_file_mkdir_chunks(dir: &[u8], chunks: &mut [HxChunk]) -> usize {
    if chunks.is_empty() || dir.len() > u16::MAX as usize {
        return 0;
    }
    chunks[0] = HxChunk {
        tag: tag::DIR,
        len: dir.len() as u16,
        data: if dir.is_empty() {
            b"".as_ptr()
        } else {
            dir.as_ptr()
        },
    };
    1
}

/// Build the chunk array for `HTLC_HDR_FILE_LIST` — single
/// `HTLC_DATA_DIR` chunk. Wire-identical shape to `FILE_MKDIR` (server
/// dispatches on the header opcode, not the chunk layout). Returns 1
/// on success, or 0 on a too-small `chunks` slice or `dir.len() >
/// u16::MAX`.
pub fn build_file_list_chunks(dir: &[u8], chunks: &mut [HxChunk]) -> usize {
    build_file_mkdir_chunks(dir, chunks)
}

/// Internal helper for the three file-ops opcodes that share the
/// FILE_NAME + optional DIR wire shape (FILE_DELETE / FILE_GETINFO /
/// FILE_GETFOLDER). When `dir` is `Some`, emits both chunks (FILE_NAME
/// first, then DIR); when `dir` is `None`, emits just FILE_NAME. The
/// caller's public wrappers below pick the matching header opcode.
fn build_file_name_with_optional_dir_chunks(
    name: &[u8],
    dir: Option<&[u8]>,
    chunks: &mut [HxChunk],
) -> usize {
    if name.len() > u16::MAX as usize {
        return 0;
    }
    if let Some(d) = dir {
        if d.len() > u16::MAX as usize {
            return 0;
        }
        if chunks.len() < 2 {
            return 0;
        }
    } else if chunks.is_empty() {
        return 0;
    }

    chunks[0] = HxChunk {
        tag: tag::FILE_NAME,
        len: name.len() as u16,
        data: if name.is_empty() {
            b"".as_ptr()
        } else {
            name.as_ptr()
        },
    };
    match dir {
        Some(d) => {
            chunks[1] = HxChunk {
                tag: tag::DIR,
                len: d.len() as u16,
                data: if d.is_empty() {
                    b"".as_ptr()
                } else {
                    d.as_ptr()
                },
            };
            2
        }
        None => 1,
    }
}

/// Build the chunk array for `HTLC_HDR_FILE_DELETE` — FILE_NAME +
/// optional DIR. Returns 1 (no dir) or 2 (with dir) on success, or 0
/// on validation failure (`chunks` too short, `name.len()` or
/// `dir.len() > u16::MAX`).
pub fn build_file_delete_chunks(name: &[u8], dir: Option<&[u8]>, chunks: &mut [HxChunk]) -> usize {
    build_file_name_with_optional_dir_chunks(name, dir, chunks)
}

/// Build the chunk array for `HTLC_HDR_FILE_GETINFO`. Same wire shape
/// as FILE_DELETE.
pub fn build_file_getinfo_chunks(name: &[u8], dir: Option<&[u8]>, chunks: &mut [HxChunk]) -> usize {
    build_file_name_with_optional_dir_chunks(name, dir, chunks)
}

/// Build the chunk array for `HTLC_HDR_FILE_GETFOLDER`. Same wire
/// shape as FILE_DELETE / FILE_GETINFO; the caller picks the header
/// opcode based on what they want the server to do with the path.
pub fn build_file_getfolder_chunks(
    name: &[u8],
    dir: Option<&[u8]>,
    chunks: &mut [HxChunk],
) -> usize {
    build_file_name_with_optional_dir_chunks(name, dir, chunks)
}

// ---- Larger files.c send opcodes -------------------------------------
//
// HTLC_HDR_FILE_SETINFO:   NAME + RENAME + optional COMMENT + optional
//                          DIR. The two C call sites — the rename +
//                          comment dialog and the rename-within-dir
//                          path in hx_file_move — share this single
//                          builder, distinguished by whether COMMENT
//                          is `Some`.
// HTLC_HDR_FILE_MOVE:      NAME + DIR + DIR_RENAME. Move a file
//                          across directories; both src dir and dst
//                          dir are mandatory.
// HTLC_HDR_FILE_SYMLINK:   NAME + DIR + DIR_RENAME + RENAME. Same
//                          shape as MOVE plus the new basename.
// HTLC_HDR_FILE_PUTFOLDER: NAME + optional DIR + HTXF_SIZE + NFILES.
//                          Folder-upload kickoff; size + file-count
//                          are u32 BE values for the server's queue
//                          UI (mhxd doesn't validate against the
//                          actual stream).

/// Request data for [`build_file_setinfo_chunks`]. Covers both the
/// full setinfo (rename + comment + optional dir) and the rename-only
/// variant used by `hx_file_move` (rename + dir, no comment).
pub struct FileSetInfoRequest<'a> {
    /// `HTLC_DATA_FILE_NAME` — the current basename. Mandatory.
    pub name: &'a [u8],
    /// `HTLC_DATA_FILE_RENAME` — the new basename. Mandatory in both
    /// call sites (rename is what FILE_SETINFO is for in this code).
    pub rename: &'a [u8],
    /// `HTLC_DATA_FILE_COMMENT` — file-comment text. `None` skips
    /// the chunk entirely (the rename-only variant in `hx_file_move`).
    pub comment: Option<&'a [u8]>,
    /// `HTLC_DATA_DIR` — parent directory path. `None` skips the
    /// chunk (file lives at the root and the full-setinfo call site
    /// is operating on a root-level file).
    pub dir: Option<&'a [u8]>,
}

/// Build the chunk array for `HTLC_HDR_FILE_SETINFO`. Wire shape:
///
/// 1. `HTLC_DATA_FILE_NAME`   — always
/// 2. `HTLC_DATA_FILE_RENAME` — always
/// 3. `HTLC_DATA_FILE_COMMENT` — when `comment.is_some()`
/// 4. `HTLC_DATA_DIR`         — when `dir.is_some()`
///
/// Returns the chunk count (2..=4) on success, or 0 on validation
/// failure (`chunks` slice too small for the chunks that will be
/// emitted, or any of `name` / `rename` / `comment` / `dir` longer
/// than `u16::MAX`).
pub fn build_file_setinfo_chunks(req: &FileSetInfoRequest<'_>, chunks: &mut [HxChunk]) -> usize {
    if req.name.len() > u16::MAX as usize || req.rename.len() > u16::MAX as usize {
        return 0;
    }
    if let Some(c) = req.comment {
        if c.len() > u16::MAX as usize {
            return 0;
        }
    }
    if let Some(d) = req.dir {
        if d.len() > u16::MAX as usize {
            return 0;
        }
    }
    let needed = 2 + usize::from(req.comment.is_some()) + usize::from(req.dir.is_some());
    if chunks.len() < needed {
        return 0;
    }

    chunks[0] = HxChunk {
        tag: tag::FILE_NAME,
        len: req.name.len() as u16,
        data: if req.name.is_empty() {
            b"".as_ptr()
        } else {
            req.name.as_ptr()
        },
    };
    chunks[1] = HxChunk {
        tag: tag::FILE_RENAME,
        len: req.rename.len() as u16,
        data: if req.rename.is_empty() {
            b"".as_ptr()
        } else {
            req.rename.as_ptr()
        },
    };
    let mut hc = 2;
    if let Some(c) = req.comment {
        chunks[hc] = HxChunk {
            tag: tag::FILE_COMMENT,
            len: c.len() as u16,
            data: if c.is_empty() {
                b"".as_ptr()
            } else {
                c.as_ptr()
            },
        };
        hc += 1;
    }
    if let Some(d) = req.dir {
        chunks[hc] = HxChunk {
            tag: tag::DIR,
            len: d.len() as u16,
            data: if d.is_empty() {
                b"".as_ptr()
            } else {
                d.as_ptr()
            },
        };
        hc += 1;
    }
    hc
}

/// Request data for [`build_file_move_chunks`].
pub struct FileMoveRequest<'a> {
    /// Source basename.
    pub name: &'a [u8],
    /// Source directory.
    pub dir: &'a [u8],
    /// Destination directory.
    pub dir_rename: &'a [u8],
}

/// Build the chunk array for `HTLC_HDR_FILE_MOVE` — NAME + DIR +
/// DIR_RENAME. 3 chunks; `chunks.len() >= 3`. No scratch needed.
/// Returns 3 on success, 0 on validation failure (short slice or
/// any field longer than `u16::MAX`).
pub fn build_file_move_chunks(req: &FileMoveRequest<'_>, chunks: &mut [HxChunk]) -> usize {
    if chunks.len() < 3
        || req.name.len() > u16::MAX as usize
        || req.dir.len() > u16::MAX as usize
        || req.dir_rename.len() > u16::MAX as usize
    {
        return 0;
    }
    chunks[0] = HxChunk {
        tag: tag::FILE_NAME,
        len: req.name.len() as u16,
        data: if req.name.is_empty() {
            b"".as_ptr()
        } else {
            req.name.as_ptr()
        },
    };
    chunks[1] = HxChunk {
        tag: tag::DIR,
        len: req.dir.len() as u16,
        data: if req.dir.is_empty() {
            b"".as_ptr()
        } else {
            req.dir.as_ptr()
        },
    };
    chunks[2] = HxChunk {
        tag: tag::DIR_RENAME,
        len: req.dir_rename.len() as u16,
        data: if req.dir_rename.is_empty() {
            b"".as_ptr()
        } else {
            req.dir_rename.as_ptr()
        },
    };
    3
}

/// Request data for [`build_file_symlink_chunks`].
pub struct FileSymlinkRequest<'a> {
    /// Source basename.
    pub name: &'a [u8],
    /// Source directory.
    pub dir: &'a [u8],
    /// Destination directory.
    pub dir_rename: &'a [u8],
    /// New basename in the destination directory.
    pub rename: &'a [u8],
}

/// Build the chunk array for `HTLC_HDR_FILE_SYMLINK` — NAME + DIR +
/// DIR_RENAME + RENAME. 4 chunks. Returns 4 on success, 0 on
/// validation failure (short slice or any field longer than
/// `u16::MAX`).
pub fn build_file_symlink_chunks(req: &FileSymlinkRequest<'_>, chunks: &mut [HxChunk]) -> usize {
    if chunks.len() < 4
        || req.name.len() > u16::MAX as usize
        || req.dir.len() > u16::MAX as usize
        || req.dir_rename.len() > u16::MAX as usize
        || req.rename.len() > u16::MAX as usize
    {
        return 0;
    }
    chunks[0] = HxChunk {
        tag: tag::FILE_NAME,
        len: req.name.len() as u16,
        data: if req.name.is_empty() {
            b"".as_ptr()
        } else {
            req.name.as_ptr()
        },
    };
    chunks[1] = HxChunk {
        tag: tag::DIR,
        len: req.dir.len() as u16,
        data: if req.dir.is_empty() {
            b"".as_ptr()
        } else {
            req.dir.as_ptr()
        },
    };
    chunks[2] = HxChunk {
        tag: tag::DIR_RENAME,
        len: req.dir_rename.len() as u16,
        data: if req.dir_rename.is_empty() {
            b"".as_ptr()
        } else {
            req.dir_rename.as_ptr()
        },
    };
    chunks[3] = HxChunk {
        tag: tag::FILE_RENAME,
        len: req.rename.len() as u16,
        data: if req.rename.is_empty() {
            b"".as_ptr()
        } else {
            req.rename.as_ptr()
        },
    };
    4
}

/// Request data for [`build_file_putfolder_chunks`].
pub struct FilePutFolderRequest<'a> {
    /// `HTLC_DATA_FILE_NAME` — the folder name being uploaded.
    pub name: &'a [u8],
    /// `HTLC_DATA_DIR` — parent directory. `None` when uploading at
    /// the root.
    pub dir: Option<&'a [u8]>,
    /// `HTLC_DATA_HTXF_SIZE` — aggregate byte size of the folder
    /// (host order; the builder big-endian-encodes it). Clamps at
    /// `G_MAXUINT32` on the caller side; the wire field is u32.
    pub size: u32,
    /// `HTLC_DATA_FILE_NFILES` — number of regular files in the
    /// folder tree (caller-supplied; mhxd uses it for queue display
    /// rather than framing).
    pub nfiles: u32,
}

/// Build the chunk array for `HTLC_HDR_FILE_PUTFOLDER`. Wire shape:
///
/// 1. `HTLC_DATA_FILE_NAME`   — always
/// 2. `HTLC_DATA_DIR`         — when `dir.is_some()`
/// 3. `HTLC_DATA_HTXF_SIZE`   — u32 BE, always
/// 4. `HTLC_DATA_FILE_NFILES` — u32 BE, always
///
/// Returns 3 (no dir) or 4 (with dir) on success. Requires
/// `chunks.len()` ≥ the matching count and `scratch.len() >= 8`
/// (two BE u32 slots). Rejects `name.len() > u16::MAX` or
/// `dir.len() > u16::MAX`.
pub fn build_file_putfolder_chunks(
    req: &FilePutFolderRequest<'_>,
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    if scratch.len() < 8 || req.name.len() > u16::MAX as usize {
        return 0;
    }
    if let Some(d) = req.dir {
        if d.len() > u16::MAX as usize {
            return 0;
        }
    }
    let needed = 3 + usize::from(req.dir.is_some());
    if chunks.len() < needed {
        return 0;
    }

    scratch[0..4].copy_from_slice(&req.size.to_be_bytes());
    scratch[4..8].copy_from_slice(&req.nfiles.to_be_bytes());

    chunks[0] = HxChunk {
        tag: tag::FILE_NAME,
        len: req.name.len() as u16,
        data: if req.name.is_empty() {
            b"".as_ptr()
        } else {
            req.name.as_ptr()
        },
    };
    let mut hc = 1;
    if let Some(d) = req.dir {
        chunks[hc] = HxChunk {
            tag: tag::DIR,
            len: d.len() as u16,
            data: if d.is_empty() {
                b"".as_ptr()
            } else {
                d.as_ptr()
            },
        };
        hc += 1;
    }
    chunks[hc] = HxChunk {
        tag: tag::HTXF_SIZE,
        len: 4,
        data: scratch.as_ptr(),
    };
    hc += 1;
    chunks[hc] = HxChunk {
        tag: tag::FILE_NFILES,
        len: 4,
        data: scratch[4..8].as_ptr(),
    };
    hc + 1
}

// ---- xfers.c send opcodes (FILE_GET / FILE_PUT) ---------------------
//
// HTLC_HDR_FILE_GET:  NAME + optional DIR + optional RFLT(74). RFLT
//                     is present iff the local file already has a
//                     partial copy and we want to resume; the C
//                     caller builds the 74-byte blob with the fork
//                     offsets baked in.
//
// HTLC_HDR_FILE_PUT:  NAME + optional DIR + optional FILE_PREVIEW(2)
//                     + HTXF_SIZE(u32 BE) + optional XFERSIZE64(u64
//                     BE). FILE_PREVIEW is "\0\1" and indicates the
//                     file already exists at the destination (the
//                     server picks rename / overwrite based on
//                     this); XFERSIZE64 is the Large-Files cap.
//                     HTXF_SIZE is the clamped-to-u32 size; the
//                     caller is responsible for clamping a u64 down
//                     to u32 before the call.

/// Request data for [`build_file_get_chunks`].
pub struct FileGetRequest<'a> {
    /// `HTLC_DATA_FILE_NAME` — file basename, already encoded.
    pub name: &'a [u8],
    /// `HTLC_DATA_DIR` — parent directory bytes. `None` skips.
    pub dir: Option<&'a [u8]>,
    /// `HTLC_DATA_RFLT` — 74-byte resume payload (the C caller
    /// builds the binary blob with fork offsets at `[46..50]` /
    /// `[62..66]`). `None` skips (no-resume / fresh download).
    /// The builder rejects any length other than 0 or 74 — RFLT
    /// is a fixed-size record on the wire.
    pub rflt: Option<&'a [u8]>,
}

/// Build the chunk array for `HTLC_HDR_FILE_GET`. Wire shape:
///
/// 1. `HTLC_DATA_FILE_NAME` — always
/// 2. `HTLC_DATA_DIR`       — when `dir.is_some()`
/// 3. `HTLC_DATA_RFLT`      — when `rflt.is_some()` (resume)
///
/// Returns the chunk count (1..=3) on success, or 0 on validation
/// failure (`chunks` slice too small for the chunks that will be
/// emitted, `name.len() > u16::MAX`, `dir.len() > u16::MAX`, or
/// `rflt.is_some() && rflt.len() != 74`).
pub fn build_file_get_chunks(req: &FileGetRequest<'_>, chunks: &mut [HxChunk]) -> usize {
    if req.name.len() > u16::MAX as usize {
        return 0;
    }
    if let Some(d) = req.dir {
        if d.len() > u16::MAX as usize {
            return 0;
        }
    }
    if let Some(r) = req.rflt {
        if r.len() != 74 {
            return 0;
        }
    }
    let needed = 1 + usize::from(req.dir.is_some()) + usize::from(req.rflt.is_some());
    if chunks.len() < needed {
        return 0;
    }

    chunks[0] = HxChunk {
        tag: tag::FILE_NAME,
        len: req.name.len() as u16,
        data: if req.name.is_empty() {
            b"".as_ptr()
        } else {
            req.name.as_ptr()
        },
    };
    let mut hc = 1;
    if let Some(d) = req.dir {
        chunks[hc] = HxChunk {
            tag: tag::DIR,
            len: d.len() as u16,
            data: if d.is_empty() {
                b"".as_ptr()
            } else {
                d.as_ptr()
            },
        };
        hc += 1;
    }
    if let Some(r) = req.rflt {
        chunks[hc] = HxChunk {
            tag: tag::RFLT,
            len: 74,
            data: r.as_ptr(),
        };
        hc += 1;
    }
    hc
}

/// Request data for [`build_file_put_chunks`].
pub struct FilePutRequest<'a> {
    /// `HTLC_DATA_FILE_NAME` — file basename, already encoded.
    pub name: &'a [u8],
    /// `HTLC_DATA_DIR` — parent directory bytes. `None` skips.
    pub dir: Option<&'a [u8]>,
    /// `HTLC_DATA_FILE_PREVIEW` — when `true`, emit the 2-byte
    /// `"\0\1"` preview marker (signals overwrite/preview because
    /// the file already exists at the destination).
    pub has_preview: bool,
    /// `HTLC_DATA_HTXF_SIZE` — u32 host-order size, big-endian-
    /// encoded into scratch. The caller is responsible for
    /// clamping a u64 total size down to u32 (set to `u32::MAX`
    /// on overflow per the Large-Files spec).
    pub size: u32,
    /// `HTLC_DATA_XFERSIZE64` — u64 host-order size (the true
    /// total, no clamp); big-endian-encoded into scratch. `None`
    /// skips (legacy client / server didn't negotiate
    /// `CAP_LARGE_FILES`).
    pub size64: Option<u64>,
}

/// Build the chunk array for `HTLC_HDR_FILE_PUT`. Wire shape:
///
/// 1. `HTLC_DATA_FILE_NAME`     — always
/// 2. `HTLC_DATA_DIR`           — when `dir.is_some()`
/// 3. `HTLC_DATA_FILE_PREVIEW`  — when `has_preview`
/// 4. `HTLC_DATA_HTXF_SIZE`     — u32 BE, always
/// 5. `HTLC_DATA_XFERSIZE64`    — u64 BE, when `size64.is_some()`
///
/// Returns the chunk count (2..=5) on success, or 0 on validation
/// failure: `chunks` slice too small (count up to 5), `scratch` <
/// 12 bytes (u32 at +0, u64 at +4), `name.len() > u16::MAX`, or
/// `dir.len() > u16::MAX`.
pub fn build_file_put_chunks(
    req: &FilePutRequest<'_>,
    chunks: &mut [HxChunk],
    scratch: &mut [u8],
) -> usize {
    if scratch.len() < 12 || req.name.len() > u16::MAX as usize {
        return 0;
    }
    if let Some(d) = req.dir {
        if d.len() > u16::MAX as usize {
            return 0;
        }
    }
    let needed = 2
        + usize::from(req.dir.is_some())
        + usize::from(req.has_preview)
        + usize::from(req.size64.is_some());
    if chunks.len() < needed {
        return 0;
    }

    scratch[0..4].copy_from_slice(&req.size.to_be_bytes());
    if let Some(s64) = req.size64 {
        scratch[4..12].copy_from_slice(&s64.to_be_bytes());
    }

    chunks[0] = HxChunk {
        tag: tag::FILE_NAME,
        len: req.name.len() as u16,
        data: if req.name.is_empty() {
            b"".as_ptr()
        } else {
            req.name.as_ptr()
        },
    };
    let mut hc = 1;
    if let Some(d) = req.dir {
        chunks[hc] = HxChunk {
            tag: tag::DIR,
            len: d.len() as u16,
            data: if d.is_empty() {
                b"".as_ptr()
            } else {
                d.as_ptr()
            },
        };
        hc += 1;
    }
    if req.has_preview {
        chunks[hc] = HxChunk {
            tag: tag::FILE_PREVIEW,
            len: 2,
            data: b"\x00\x01".as_ptr(),
        };
        hc += 1;
    }
    chunks[hc] = HxChunk {
        tag: tag::HTXF_SIZE,
        len: 4,
        data: scratch.as_ptr(),
    };
    hc += 1;
    if req.size64.is_some() {
        chunks[hc] = HxChunk {
            tag: tag::XFERSIZE64,
            len: 8,
            data: scratch[4..12].as_ptr(),
        };
        hc += 1;
    }
    hc
}

// ---- HTXF subframe header packer -------------------------------------

/// Magic bytes prefixing every HTXF subframe. "HTXF" as a big-endian
/// `u32`, mirroring `HTXF_MAGIC_INT` in `src/hotline.h`.
pub const HTXF_MAGIC: u32 = 0x48545846;

/// Fixed size of the HTXF subframe header (`SIZEOF_HTXF_HDR` in C).
pub const HTXF_HDR_SIZE: usize = 16;

/// Pack the 16-byte HTXF subframe header into `out[..HTXF_HDR_SIZE]`.
///
/// Wire layout (big-endian throughout):
///
/// ```text
///   off  size  field
///    0    4    magic     = HTXF_MAGIC ("HTXF" big-endian)
///    4    4    ref       = transfer reference
///    8    4    len       = wire payload length
///   12    4    (type<<16) | flags
/// ```
///
/// The trailing u32 is shared between two readings: classic Mac-native
/// servers read the high u16 as a *type* code to dispatch the
/// subchannel; capability-aware peers read the low u16 as *flags* to
/// know whether to expect the 24-byte large-file variant. Both
/// interpretations share the same word — production gives type a
/// non-zero value and flags the appropriate bits, so the same packed
/// header serves both.
///
/// Returns `false` and writes nothing if `out` is shorter than
/// `HTXF_HDR_SIZE`; otherwise writes 16 bytes and returns `true`.
pub fn build_htxf_hdr(
    out: &mut [u8],
    ref_id: u32,
    payload_len: u32,
    type_code: u16,
    flags: u16,
) -> bool {
    if out.len() < HTXF_HDR_SIZE {
        return false;
    }
    out[0..4].copy_from_slice(&HTXF_MAGIC.to_be_bytes());
    out[4..8].copy_from_slice(&ref_id.to_be_bytes());
    out[8..12].copy_from_slice(&payload_len.to_be_bytes());
    let type_flags = ((type_code as u32) << 16) | (flags as u32);
    out[12..16].copy_from_slice(&type_flags.to_be_bytes());
    true
}

/// HTXF handshake flag bits (mirror `HTXF_FLAG_*` in `src/hotline.h`).
pub const HTXF_FLAG_LARGE_FILE: u16 = 0x0001;
pub const HTXF_FLAG_SIZE64: u16 = 0x0002;

/// Pack the full HTXF subchannel handshake preamble: the 16-byte header, plus —
/// when `size64` — an 8-byte big-endian `total_size` after it (the large-file
/// variant). Mirrors C `hx_htxf_subchannel_pack_preamble` byte-for-byte.
///
/// The `size64` form sets `HTXF_FLAG_LARGE_FILE | HTXF_FLAG_SIZE64`, zeroes the
/// legacy 32-bit length field (so a non-large-file peer can't mistake a partial
/// read for completion), and writes the true size in the trailing 8 bytes. The
/// legacy 16-byte form fails closed (returns 0) if `total_size` exceeds
/// `u32::MAX` rather than silently truncating.
///
/// Returns the number of bytes written (16 or 24), or 0 on a too-small `out`
/// (or the >4 GiB legacy case).
pub fn build_htxf_preamble(
    out: &mut [u8],
    ref_id: u32,
    total_size: u64,
    type_code: u16,
    flags: u16,
    size64: bool,
) -> usize {
    if size64 {
        if out.len() < HTXF_HDR_SIZE + 8 {
            return 0;
        }
        let wire_flags = flags | HTXF_FLAG_LARGE_FILE | HTXF_FLAG_SIZE64;
        build_htxf_hdr(out, ref_id, 0, type_code, wire_flags);
        out[HTXF_HDR_SIZE..HTXF_HDR_SIZE + 8].copy_from_slice(&total_size.to_be_bytes());
        HTXF_HDR_SIZE + 8
    } else {
        if out.len() < HTXF_HDR_SIZE || total_size > u32::MAX as u64 {
            return 0;
        }
        build_htxf_hdr(out, ref_id, total_size as u32, type_code, flags);
        HTXF_HDR_SIZE
    }
}

// ---- Full message packer (Hotline transaction wire format) ----------

/// A chunk with a safely-borrowed payload, used by the pure-Rust
/// [`pack_message`] / [`pack_message_size`] interface. Distinct from the
/// FFI-shaped [`HxChunk`] (which carries a raw pointer): the FFI shim in
/// `ffi.rs` converts `HxChunk` → `PackChunk` in one unsafe block, then
/// hands a slice of these to the safe pack function. This is what keeps
/// `build.rs` free of `unsafe` despite the underlying C ABI carrying
/// raw pointers.
#[derive(Clone, Copy)]
pub struct PackChunk<'a> {
    pub tag: u16,
    pub data: &'a [u8],
}

/// Maximum chunks per packed message. Sized well above what any current
/// builder produces (login is the largest at ~10 chunks) — exists so the
/// FFI shim can stack-allocate the `[PackChunk; MAX_PACK_CHUNKS]`
/// staging buffer without a heap allocation. Bump if a future builder
/// needs more; this is also the cap that lets `pack_message` reject
/// pathological `chunks_len` values at the API boundary.
pub const MAX_PACK_CHUNKS: usize = 64;

/// Total byte count a full Hotline message will occupy when packed:
/// 22-byte header + every chunk's 4-byte data header + that chunk's
/// payload length.
///
/// **Unchecked.** This is the pure size arithmetic; it computes a number
/// for any input slice. The constraints [`pack_message`] enforces —
/// `chunks.len() <= MAX_PACK_CHUNKS` and per-chunk `data.len() <=
/// u16::MAX` — are NOT re-checked here. Callers that use this value to
/// pre-size a buffer should either (a) also call `pack_message` and
/// handle its `None`, or (b) range-check the slice themselves before
/// calling this. The FFI shim at `gtkhx_proto_pack_message_size` does
/// the latter and returns 0 to signal a rejected request.
pub fn pack_message_size(chunks: &[PackChunk<'_>]) -> usize {
    let mut n = crate::HL_HDR_LEN;
    for c in chunks {
        n += crate::HL_DATA_HDR_LEN + c.data.len();
    }
    n
}

/// Pack a 22-byte Hotline transaction header into `out` (big-endian).
///
/// `body_len` is the application-level body byte count (everything after the
/// header, *excluding* hc); the wire `len` / `len2` fields encode `body_len +
/// sizeof(hc)` because the protocol counts hc — which physically sits at the
/// tail of the 22-byte header — as the first two bytes of the data section.
/// `len2` is written identical to `len`, matching what [`pack_message`] and the
/// C `hlpack` path produce, so a header packed here is byte-for-byte identical.
///
/// This is the single wire-header encoder: [`pack_message`] calls it for the
/// header portion, and the receive-side bridge calls it (via
/// `gtkhx_proto_pack_header`) to reconstruct the header of a frame the Rust
/// actor already parsed, so the C handlers can decode it back out of
/// `htlc->in`.
///
/// # Panics
/// Panics if `out` is shorter than [`HL_HDR_LEN`](crate::HL_HDR_LEN) (22), or if
/// `body_len + sizeof(hc)` overflows `u32`. Both are caller bugs (a real
/// Hotline body is bounded well under `u32::MAX`); asserting turns them into a
/// deterministic, diagnosable failure instead of a bare slice-range panic or a
/// silent release-mode wrap.
pub fn pack_header(out: &mut [u8], type_: u32, trans: u32, flag: u32, hc: u16, body_len: u32) {
    assert!(
        out.len() >= crate::HL_HDR_LEN,
        "pack_header: output buffer too short: {} < {} bytes",
        out.len(),
        crate::HL_HDR_LEN
    );
    let wire_len = body_len
        .checked_add(core::mem::size_of::<u16>() as u32)
        .expect("pack_header: body_len + sizeof(hc) overflows u32");
    out[0..4].copy_from_slice(&type_.to_be_bytes());
    out[4..8].copy_from_slice(&trans.to_be_bytes());
    out[8..12].copy_from_slice(&flag.to_be_bytes());
    out[12..16].copy_from_slice(&wire_len.to_be_bytes());
    out[16..20].copy_from_slice(&wire_len.to_be_bytes()); // len2 == len
    out[20..22].copy_from_slice(&hc.to_be_bytes());
}

/// Pack a full Hotline transaction (header + chunks) into `out`.
///
/// Wire layout (big-endian throughout):
///
/// ```text
///   off  size  field
///    0    4    type
///    4    4    trans
///    8    4    flag
///   12    4    len      = body bytes + sizeof(hc)
///   16    4    len2     = identical to `len`
///   20    2    hc       = chunk count
///  --- per chunk ---
///   0    2    type
///   2    2    len
///   4    L    payload
/// ```
///
/// Returns `Some(bytes_written)` (always equal to [`pack_message_size`])
/// or `None` when:
///   - `out` is shorter than `pack_message_size(chunks)`,
///   - any chunk's `data.len()` does not fit in u16,
///   - `chunks.len()` exceeds [`MAX_PACK_CHUNKS`].
///
/// All inputs are safely-borrowed slices, so there is no NULL-pointer
/// edge case to model — a zero-length payload on the wire is simply a
/// `PackChunk { tag, data: &[] }`.
pub fn pack_message(
    out: &mut [u8],
    type_: u32,
    trans: u32,
    flag: u32,
    chunks: &[PackChunk<'_>],
) -> Option<usize> {
    if chunks.len() > MAX_PACK_CHUNKS {
        return None;
    }
    // Each chunk's payload length lives in a u16 on the wire.
    for c in chunks {
        if c.data.len() > u16::MAX as usize {
            return None;
        }
    }
    let needed = pack_message_size(chunks);
    if out.len() < needed {
        return None;
    }
    // The header body_len is everything after the 22-byte header. pack_header
    // adds back the sizeof(hc) the wire `len` field folds in.
    let body_len = (needed - crate::HL_HDR_LEN) as u32;
    pack_header(
        &mut out[..crate::HL_HDR_LEN],
        type_,
        trans,
        flag,
        chunks.len() as u16,
        body_len,
    );
    let mut pos = crate::HL_HDR_LEN;
    for c in chunks {
        let plen = c.data.len() as u16;
        out[pos..pos + 2].copy_from_slice(&c.tag.to_be_bytes());
        out[pos + 2..pos + 4].copy_from_slice(&plen.to_be_bytes());
        pos += crate::HL_DATA_HDR_LEN;
        if !c.data.is_empty() {
            out[pos..pos + c.data.len()].copy_from_slice(c.data);
            pos += c.data.len();
        }
    }
    debug_assert_eq!(pos, needed);
    Some(needed)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Read a chunk's payload bytes through its raw pointer (safe in
    /// tests because we know the buffers are still alive).
    unsafe fn chunk_bytes(c: &HxChunk) -> &[u8] {
        std::slice::from_raw_parts(c.data, c.len as usize)
    }

    #[test]
    fn pack_header_byte_layout() {
        // Mirror the explicit vector in tests/unit/test_hxnet_bridge.c so the
        // Rust encoder and the C round-trip test can never silently diverge.
        let mut hdr = [0u8; crate::HL_HDR_LEN];
        pack_header(&mut hdr, 0x0102_0304, 0x0506_0708, 0x090a_0b0c, 0x0d0e, 10);
        let expected: [u8; crate::HL_HDR_LEN] = [
            0x01, 0x02, 0x03, 0x04, // type
            0x05, 0x06, 0x07, 0x08, // trans
            0x09, 0x0a, 0x0b, 0x0c, // flag
            0x00, 0x00, 0x00, 0x0c, // len = body(10) + hc(2) = 12
            0x00, 0x00, 0x00, 0x0c, // len2 == len
            0x0d, 0x0e, // hc
        ];
        assert_eq!(hdr, expected);
    }

    #[test]
    fn pack_header_zero_body_encodes_hc_only_len() {
        // Body-less frame: wire len == sizeof(hc) == 2.
        let mut hdr = [0u8; crate::HL_HDR_LEN];
        pack_header(&mut hdr, 0x0001_0000, 0, 0, 0, 0);
        assert_eq!(&hdr[12..16], &[0, 0, 0, 2]); // len
        assert_eq!(&hdr[16..20], &[0, 0, 0, 2]); // len2
        assert_eq!(&hdr[20..22], &[0, 0]); // hc
    }

    #[test]
    fn pack_message_header_matches_pack_header() {
        // pack_message must emit the same header pack_header does for the same
        // (type, trans, flag, hc=chunk-count, body_len).
        let chunks = [PackChunk {
            tag: 0x0069,
            data: b"hello",
        }];
        let mut msg = [0u8; 64];
        let n = pack_message(&mut msg, 0x0000_006a, 0x1234_5678, 0, &chunks).unwrap();
        let body_len = (n - crate::HL_HDR_LEN) as u32;
        let mut hdr = [0u8; crate::HL_HDR_LEN];
        pack_header(&mut hdr, 0x0000_006a, 0x1234_5678, 0, 1, body_len);
        assert_eq!(&msg[..crate::HL_HDR_LEN], &hdr);
    }

    #[test]
    fn chat_no_cid_emits_two_chunks() {
        let req = ChatRequest {
            cid: 0,
            style: 0x0102,
            body: b"hello",
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 8];
        let hc = build_chat_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 2);
        assert_eq!(chunks[0].tag, tag::STYLE);
        assert_eq!(chunks[0].len, 2);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, &[0x01, 0x02]);
        assert_eq!(chunks[1].tag, tag::BODY);
        assert_eq!(chunks[1].len, 5);
        assert_eq!(unsafe { chunk_bytes(&chunks[1]) }, b"hello");
    }

    #[test]
    fn chat_with_cid_emits_three_chunks_in_order() {
        let req = ChatRequest {
            cid: 0xdead_beef,
            style: 0,
            body: b"hi",
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 8];
        let hc = build_chat_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 3);
        assert_eq!(chunks[0].tag, tag::STYLE);
        assert_eq!(chunks[1].tag, tag::BODY);
        assert_eq!(chunks[2].tag, tag::CHAT_ID);
        assert_eq!(chunks[2].len, 4);
        assert_eq!(
            unsafe { chunk_bytes(&chunks[2]) },
            &[0xde, 0xad, 0xbe, 0xef]
        );
    }

    #[test]
    fn chat_empty_body_is_legal() {
        let req = ChatRequest {
            cid: 0,
            style: 0,
            body: b"",
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 8];
        let hc = build_chat_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 2);
        assert_eq!(chunks[1].len, 0);
        // data pointer is non-NULL so the C side won't trip a guard.
        assert!(!chunks[1].data.is_null());
    }

    #[test]
    fn chat_rejects_short_chunks_buffer() {
        let req = ChatRequest {
            cid: 1,
            style: 0,
            body: b"",
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY]; // 2 < 3
        let mut scratch = [0u8; 8];
        assert_eq!(build_chat_chunks(&req, &mut chunks, &mut scratch), 0);
    }

    #[test]
    fn chat_rejects_short_scratch_buffer() {
        let req = ChatRequest {
            cid: 1,
            style: 0,
            body: b"",
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 4]; // 4 < 6
        assert_eq!(build_chat_chunks(&req, &mut chunks, &mut scratch), 0);
    }

    #[test]
    fn msg_emits_uid_then_body() {
        let req = MsgRequest {
            uid: 0x1234,
            body: b"hello",
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        let hc = build_msg_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 2);
        assert_eq!(chunks[0].tag, tag::UID);
        assert_eq!(chunks[0].len, 2);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, &[0x12, 0x34]);
        assert_eq!(chunks[1].tag, tag::BODY);
        assert_eq!(unsafe { chunk_bytes(&chunks[1]) }, b"hello");
    }

    #[test]
    fn msg_rejects_short_buffers() {
        let req = MsgRequest { uid: 1, body: b"" };
        let mut chunks_short = [HxChunk::EMPTY]; // 1 < 2
        let mut scratch = [0u8; 4];
        assert_eq!(build_msg_chunks(&req, &mut chunks_short, &mut scratch), 0);

        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch_short = [0u8; 1]; // 1 < 2
        assert_eq!(build_msg_chunks(&req, &mut chunks, &mut scratch_short), 0);
    }

    #[test]
    fn broadcast_emits_just_body() {
        let req = BroadcastRequest {
            body: b"server going down",
        };
        let mut chunks = [HxChunk::EMPTY];
        let hc = build_broadcast_chunks(&req, &mut chunks);
        assert_eq!(hc, 1);
        assert_eq!(chunks[0].tag, tag::BODY);
        assert_eq!(chunks[0].len, 17);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, b"server going down");
    }

    #[test]
    fn broadcast_rejects_empty_chunks_slice() {
        let req = BroadcastRequest { body: b"" };
        let mut chunks: [HxChunk; 0] = [];
        assert_eq!(build_broadcast_chunks(&req, &mut chunks), 0);
    }

    // ---- oversize-body rejects ----
    //
    // Chunk lengths on the Hotline wire are 16-bit. Each builder
    // truncates body.len() to u16 when populating the chunk; without
    // the explicit length check, a 65 KiB body would silently wrap to
    // a small chunk-length and put an invalid frame on the wire. Cover
    // the boundary (u16::MAX + 1) for each builder. Allocating a
    // 65537-byte vec is cheap in test.

    #[test]
    fn chat_rejects_body_larger_than_u16_max() {
        let big = vec![0u8; u16::MAX as usize + 1];
        let req = ChatRequest {
            cid: 0,
            style: 0,
            body: &big,
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 8];
        assert_eq!(build_chat_chunks(&req, &mut chunks, &mut scratch), 0);
    }

    #[test]
    fn chat_accepts_body_exactly_u16_max() {
        let big = vec![0u8; u16::MAX as usize];
        let req = ChatRequest {
            cid: 0,
            style: 0,
            body: &big,
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 8];
        let hc = build_chat_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 2);
        assert_eq!(chunks[1].len, u16::MAX);
    }

    #[test]
    fn msg_rejects_body_larger_than_u16_max() {
        let big = vec![0u8; u16::MAX as usize + 1];
        let req = MsgRequest { uid: 0, body: &big };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 2];
        assert_eq!(build_msg_chunks(&req, &mut chunks, &mut scratch), 0);
    }

    #[test]
    fn broadcast_rejects_body_larger_than_u16_max() {
        let big = vec![0u8; u16::MAX as usize + 1];
        let req = BroadcastRequest { body: &big };
        let mut chunks = [HxChunk::EMPTY];
        assert_eq!(build_broadcast_chunks(&req, &mut chunks), 0);
    }

    // ---- chat admin ----

    #[test]
    fn chat_create_emits_uid_chunk() {
        let mut chunks = [HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        let hc = build_chat_create_chunks(0x1234, &mut chunks, &mut scratch);
        assert_eq!(hc, 1);
        assert_eq!(chunks[0].tag, tag::UID);
        assert_eq!(chunks[0].len, 2);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, &[0x12, 0x34]);
    }

    #[test]
    fn chat_create_rejects_short_buffers() {
        let mut chunks_short: [HxChunk; 0] = [];
        let mut scratch = [0u8; 4];
        assert_eq!(
            build_chat_create_chunks(1, &mut chunks_short, &mut scratch),
            0
        );

        let mut chunks = [HxChunk::EMPTY];
        let mut scratch_short = [0u8; 1];
        assert_eq!(
            build_chat_create_chunks(1, &mut chunks, &mut scratch_short),
            0
        );
    }

    #[test]
    fn chat_invite_emits_cid_then_uid() {
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 8];
        let hc = build_chat_invite_chunks(0xdead_beef, 0x42, &mut chunks, &mut scratch);
        assert_eq!(hc, 2);
        assert_eq!(chunks[0].tag, tag::CHAT_ID);
        assert_eq!(chunks[0].len, 4);
        assert_eq!(
            unsafe { chunk_bytes(&chunks[0]) },
            &[0xde, 0xad, 0xbe, 0xef]
        );
        assert_eq!(chunks[1].tag, tag::UID);
        assert_eq!(chunks[1].len, 2);
        assert_eq!(unsafe { chunk_bytes(&chunks[1]) }, &[0x00, 0x42]);
    }

    #[test]
    fn chat_invite_rejects_short_buffers() {
        let mut chunks_short = [HxChunk::EMPTY];
        let mut scratch = [0u8; 8];
        assert_eq!(
            build_chat_invite_chunks(1, 1, &mut chunks_short, &mut scratch),
            0
        );

        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch_short = [0u8; 5];
        assert_eq!(
            build_chat_invite_chunks(1, 1, &mut chunks, &mut scratch_short),
            0
        );
    }

    #[test]
    fn chat_id_only_builders_share_shape() {
        // join, part, decline all wrap the same helper — verify each
        // emits a single CHAT_ID chunk with the BE-encoded cid.
        for &builder in &[
            build_chat_join_chunks as fn(u32, &mut [HxChunk], &mut [u8]) -> usize,
            build_chat_part_chunks as fn(u32, &mut [HxChunk], &mut [u8]) -> usize,
            build_chat_decline_chunks as fn(u32, &mut [HxChunk], &mut [u8]) -> usize,
        ] {
            let mut chunks = [HxChunk::EMPTY];
            let mut scratch = [0u8; 4];
            let hc = builder(0xcafe_babe, &mut chunks, &mut scratch);
            assert_eq!(hc, 1);
            assert_eq!(chunks[0].tag, tag::CHAT_ID);
            assert_eq!(chunks[0].len, 4);
            assert_eq!(
                unsafe { chunk_bytes(&chunks[0]) },
                &[0xca, 0xfe, 0xba, 0xbe]
            );
        }
    }

    #[test]
    fn chat_id_only_rejects_short_buffers() {
        let mut chunks_short: [HxChunk; 0] = [];
        let mut scratch = [0u8; 4];
        assert_eq!(
            build_chat_join_chunks(1, &mut chunks_short, &mut scratch),
            0
        );

        let mut chunks = [HxChunk::EMPTY];
        let mut scratch_short = [0u8; 2];
        assert_eq!(
            build_chat_part_chunks(1, &mut chunks, &mut scratch_short),
            0
        );
    }

    #[test]
    fn chat_subject_emits_cid_then_subject() {
        let req = ChatSubjectRequest {
            cid: 0x0000_0007,
            subject: b"Welcome",
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        let hc = build_chat_subject_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 2);
        assert_eq!(chunks[0].tag, tag::CHAT_ID);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, &[0, 0, 0, 7]);
        assert_eq!(chunks[1].tag, tag::CHAT_SUBJECT);
        assert_eq!(chunks[1].len, 7);
        assert_eq!(unsafe { chunk_bytes(&chunks[1]) }, b"Welcome");
    }

    #[test]
    fn chat_subject_empty_subject_legal() {
        let req = ChatSubjectRequest {
            cid: 1,
            subject: b"",
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        let hc = build_chat_subject_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 2);
        assert_eq!(chunks[1].len, 0);
        assert!(!chunks[1].data.is_null());
    }

    #[test]
    fn chat_subject_rejects_short_buffers() {
        let req = ChatSubjectRequest {
            cid: 1,
            subject: b"",
        };
        let mut chunks_short = [HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        assert_eq!(
            build_chat_subject_chunks(&req, &mut chunks_short, &mut scratch),
            0
        );

        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch_short = [0u8; 3];
        assert_eq!(
            build_chat_subject_chunks(&req, &mut chunks, &mut scratch_short),
            0
        );
    }

    // ---- agreement agree ----

    #[test]
    fn agreement_agree_emits_three_chunks_in_order() {
        let req = AgreementAgreeRequest {
            icon: 0x01f4,
            display_name: b"misha",
            options: 0,
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        let hc = build_agreement_agree_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 3);
        assert_eq!(chunks[0].tag, tag::ICON);
        assert_eq!(chunks[0].len, 2);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, &[0x01, 0xf4]);
        assert_eq!(chunks[1].tag, tag::NAME);
        assert_eq!(chunks[1].len, 5);
        assert_eq!(unsafe { chunk_bytes(&chunks[1]) }, b"misha");
        assert_eq!(chunks[2].tag, tag::OPTIONS);
        assert_eq!(chunks[2].len, 2);
        assert_eq!(unsafe { chunk_bytes(&chunks[2]) }, &[0x00, 0x00]);
    }

    #[test]
    fn agreement_agree_empty_name_legal() {
        let req = AgreementAgreeRequest {
            icon: 0,
            display_name: b"",
            options: 0,
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        let hc = build_agreement_agree_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 3);
        assert_eq!(chunks[1].len, 0);
        // OPTIONS chunk is always emitted (Mobius-panic invariant).
        assert_eq!(chunks[2].tag, tag::OPTIONS);
    }

    #[test]
    fn agreement_agree_rejects_short_buffers() {
        let req = AgreementAgreeRequest {
            icon: 0,
            display_name: b"",
            options: 0,
        };
        let mut chunks_short = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        assert_eq!(
            build_agreement_agree_chunks(&req, &mut chunks_short, &mut scratch),
            0
        );

        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch_short = [0u8; 3];
        assert_eq!(
            build_agreement_agree_chunks(&req, &mut chunks, &mut scratch_short),
            0
        );
    }

    // ---- oversize-body rejects (chat_subject + agreement_agree) ----
    //
    // Same 16-bit-chunk-length boundary checked above for the
    // chat / msg / broadcast builders, applied to the
    // variable-length payloads on the chat-admin builders.

    #[test]
    fn chat_subject_rejects_subject_larger_than_u16_max() {
        let big = vec![0u8; u16::MAX as usize + 1];
        let req = ChatSubjectRequest {
            cid: 1,
            subject: &big,
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        assert_eq!(
            build_chat_subject_chunks(&req, &mut chunks, &mut scratch),
            0
        );
    }

    #[test]
    fn chat_subject_accepts_subject_exactly_u16_max() {
        let big = vec![b's'; u16::MAX as usize];
        let req = ChatSubjectRequest {
            cid: 1,
            subject: &big,
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        let hc = build_chat_subject_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 2);
        assert_eq!(chunks[1].len, u16::MAX);
    }

    #[test]
    fn agreement_agree_rejects_name_larger_than_u16_max() {
        let big = vec![b'q'; u16::MAX as usize + 1];
        let req = AgreementAgreeRequest {
            icon: 0,
            display_name: &big,
            options: 0,
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        assert_eq!(
            build_agreement_agree_chunks(&req, &mut chunks, &mut scratch),
            0
        );
    }

    // ---- user change ----

    #[test]
    fn user_change_no_color_emits_two_chunks() {
        let req = UserChangeRequest {
            icon: 0x01f4,
            name: b"misha",
            nick_color: None,
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 8];
        let hc = build_user_change_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 2);
        assert_eq!(chunks[0].tag, tag::ICON);
        assert_eq!(chunks[0].len, 2);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, &[0x01, 0xf4]);
        assert_eq!(chunks[1].tag, tag::NAME);
        assert_eq!(chunks[1].len, 5);
        assert_eq!(unsafe { chunk_bytes(&chunks[1]) }, b"misha");
    }

    #[test]
    fn user_change_with_color_emits_three_chunks_in_order() {
        let req = UserChangeRequest {
            icon: 0x0005,
            name: b"alice",
            nick_color: Some(0x00ff_8800),
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 8];
        let hc = build_user_change_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 3);
        assert_eq!(chunks[0].tag, tag::ICON);
        assert_eq!(chunks[1].tag, tag::NAME);
        assert_eq!(chunks[2].tag, tag::COLOR);
        assert_eq!(chunks[2].len, 4);
        assert_eq!(
            unsafe { chunk_bytes(&chunks[2]) },
            &[0x00, 0xff, 0x88, 0x00]
        );
    }

    #[test]
    fn user_change_empty_name_legal() {
        let req = UserChangeRequest {
            icon: 0,
            name: b"",
            nick_color: None,
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 8];
        let hc = build_user_change_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 2);
        assert_eq!(chunks[1].len, 0);
        assert!(!chunks[1].data.is_null());
    }

    #[test]
    fn user_change_rejects_short_buffers() {
        let req = UserChangeRequest {
            icon: 0,
            name: b"",
            nick_color: Some(0),
        };
        let mut chunks_short = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 8];
        assert_eq!(
            build_user_change_chunks(&req, &mut chunks_short, &mut scratch),
            0
        );

        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch_short = [0u8; 5];
        assert_eq!(
            build_user_change_chunks(&req, &mut chunks, &mut scratch_short),
            0
        );
    }

    #[test]
    fn user_change_rejects_name_larger_than_u16_max() {
        let big = vec![b'q'; u16::MAX as usize + 1];
        let req = UserChangeRequest {
            icon: 0,
            name: &big,
            nick_color: None,
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 8];
        assert_eq!(build_user_change_chunks(&req, &mut chunks, &mut scratch), 0);
    }

    // ---- user kick ----

    #[test]
    fn user_kick_no_ban_emits_just_uid() {
        let req = UserKickRequest {
            uid: 0x1234,
            ban: 0,
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        let hc = build_user_kick_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 1);
        assert_eq!(chunks[0].tag, tag::UID);
        assert_eq!(chunks[0].len, 2);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, &[0x12, 0x34]);
    }

    #[test]
    fn user_kick_with_ban_emits_ban_then_uid() {
        // BAN comes BEFORE UID — matches the C call-site ordering, which
        // mhxd cares about (the kick handler reads chunks in order).
        let req = UserKickRequest {
            uid: 0x1234,
            ban: 1,
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        let hc = build_user_kick_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 2);
        assert_eq!(chunks[0].tag, tag::BAN);
        assert_eq!(chunks[0].len, 2);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, &[0x00, 0x01]);
        assert_eq!(chunks[1].tag, tag::UID);
        assert_eq!(chunks[1].len, 2);
        assert_eq!(unsafe { chunk_bytes(&chunks[1]) }, &[0x12, 0x34]);
    }

    #[test]
    fn user_kick_rejects_short_buffers() {
        let req = UserKickRequest { uid: 1, ban: 1 };
        let mut chunks_short = [HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        assert_eq!(
            build_user_kick_chunks(&req, &mut chunks_short, &mut scratch),
            0
        );

        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch_short = [0u8; 3];
        assert_eq!(
            build_user_kick_chunks(&req, &mut chunks, &mut scratch_short),
            0
        );
    }

    // ---- user get info ----

    #[test]
    fn user_getinfo_emits_uid_chunk() {
        let mut chunks = [HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        let hc = build_user_getinfo_chunks(0xabcd, &mut chunks, &mut scratch);
        assert_eq!(hc, 1);
        assert_eq!(chunks[0].tag, tag::UID);
        assert_eq!(chunks[0].len, 2);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, &[0xab, 0xcd]);
    }

    #[test]
    fn user_getinfo_rejects_short_buffers() {
        let mut chunks_short: [HxChunk; 0] = [];
        let mut scratch = [0u8; 4];
        assert_eq!(
            build_user_getinfo_chunks(1, &mut chunks_short, &mut scratch),
            0
        );

        let mut chunks = [HxChunk::EMPTY];
        let mut scratch_short = [0u8; 1];
        assert_eq!(
            build_user_getinfo_chunks(1, &mut chunks, &mut scratch_short),
            0
        );
    }

    // ---- account read / delete (single LOGIN chunk) ----

    #[test]
    fn account_read_emits_login_chunk() {
        let mut chunks = [HxChunk::EMPTY];
        let hc = build_account_read_chunks(b"admin", &mut chunks);
        assert_eq!(hc, 1);
        assert_eq!(chunks[0].tag, tag::LOGIN);
        assert_eq!(chunks[0].len, 5);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, b"admin");
    }

    #[test]
    fn account_delete_same_shape_as_read() {
        let mut chunks_r = [HxChunk::EMPTY];
        let mut chunks_d = [HxChunk::EMPTY];
        let r = build_account_read_chunks(b"alice", &mut chunks_r);
        let d = build_account_delete_chunks(b"alice", &mut chunks_d);
        assert_eq!(r, d);
        assert_eq!(chunks_r[0].tag, chunks_d[0].tag);
        assert_eq!(chunks_r[0].len, chunks_d[0].len);
        assert_eq!(unsafe { chunk_bytes(&chunks_r[0]) }, b"alice");
        assert_eq!(unsafe { chunk_bytes(&chunks_d[0]) }, b"alice");
    }

    #[test]
    fn account_read_empty_login_legal() {
        let mut chunks = [HxChunk::EMPTY];
        let hc = build_account_read_chunks(b"", &mut chunks);
        assert_eq!(hc, 1);
        assert_eq!(chunks[0].len, 0);
        assert!(!chunks[0].data.is_null());
    }

    #[test]
    fn account_read_rejects_empty_chunks_slice() {
        let mut chunks: [HxChunk; 0] = [];
        assert_eq!(build_account_read_chunks(b"x", &mut chunks), 0);
    }

    #[test]
    fn account_read_rejects_login_larger_than_u16_max() {
        let big = vec![b'q'; u16::MAX as usize + 1];
        let mut chunks = [HxChunk::EMPTY];
        assert_eq!(build_account_read_chunks(&big, &mut chunks), 0);
    }

    // ---- account modify ----

    #[test]
    fn account_modify_emits_four_chunks_in_order() {
        let req = AccountModifyRequest {
            login: b"admin",
            // hl_encoded password — caller-supplied bytes, builder
            // doesn't care about the encoding scheme.
            password: &[0x9e, 0x90, 0x93, 0x9e, 0x91], // hl_encode("admin")
            name: b"Administrator",
            access: [0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff],
        };
        let mut chunks = [HxChunk::EMPTY; 4];
        let mut scratch = [0u8; 8];
        let hc = build_account_modify_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 4);
        assert_eq!(chunks[0].tag, tag::LOGIN);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, b"admin");
        assert_eq!(chunks[1].tag, tag::PASSWORD);
        assert_eq!(
            unsafe { chunk_bytes(&chunks[1]) },
            &[0x9e, 0x90, 0x93, 0x9e, 0x91]
        );
        assert_eq!(chunks[2].tag, tag::NAME);
        assert_eq!(unsafe { chunk_bytes(&chunks[2]) }, b"Administrator");
        assert_eq!(chunks[3].tag, tag::ACCESS);
        assert_eq!(chunks[3].len, 8);
        assert_eq!(
            unsafe { chunk_bytes(&chunks[3]) },
            &[0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff]
        );
    }

    #[test]
    fn account_modify_zero_byte_password_is_legal() {
        // The C `if (!*pass)` branch writes a single 0x00 byte for an
        // empty password — verify we can emit that exact 1-byte chunk.
        let req = AccountModifyRequest {
            login: b"u",
            password: &[0x00],
            name: b"n",
            access: [0; 8],
        };
        let mut chunks = [HxChunk::EMPTY; 4];
        let mut scratch = [0u8; 8];
        let hc = build_account_modify_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 4);
        assert_eq!(chunks[1].tag, tag::PASSWORD);
        assert_eq!(chunks[1].len, 1);
        assert_eq!(unsafe { chunk_bytes(&chunks[1]) }, &[0x00]);
    }

    #[test]
    fn account_modify_rejects_short_buffers() {
        let req = AccountModifyRequest {
            login: b"u",
            password: b"p",
            name: b"n",
            access: [0; 8],
        };
        let mut chunks_short = [HxChunk::EMPTY; 3];
        let mut scratch = [0u8; 8];
        assert_eq!(
            build_account_modify_chunks(&req, &mut chunks_short, &mut scratch),
            0
        );

        let mut chunks = [HxChunk::EMPTY; 4];
        let mut scratch_short = [0u8; 7];
        assert_eq!(
            build_account_modify_chunks(&req, &mut chunks, &mut scratch_short),
            0
        );
    }

    #[test]
    fn account_modify_rejects_oversize_fields() {
        let big = vec![b'q'; u16::MAX as usize + 1];
        for which in 0..3 {
            let req = AccountModifyRequest {
                login: if which == 0 { &big } else { b"u" },
                password: if which == 1 { &big } else { b"p" },
                name: if which == 2 { &big } else { b"n" },
                access: [0; 8],
            };
            let mut chunks = [HxChunk::EMPTY; 4];
            let mut scratch = [0u8; 8];
            assert_eq!(
                build_account_modify_chunks(&req, &mut chunks, &mut scratch),
                0,
                "oversize field {which} should be rejected"
            );
        }
    }

    // ---- news_post ----

    #[test]
    fn news_post_emits_body_chunk() {
        let mut chunks = [HxChunk::EMPTY];
        let hc = build_news_post_chunks(b"Hello, world", &mut chunks);
        assert_eq!(hc, 1);
        // Tag is BODY (0x0065), same code point HTLC_DATA_NEWS_POST
        // aliases — protocol re-uses 0x0065 per opcode context.
        assert_eq!(chunks[0].tag, tag::BODY);
        assert_eq!(chunks[0].len, 12);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, b"Hello, world");
    }

    #[test]
    fn news_post_empty_body_legal() {
        let mut chunks = [HxChunk::EMPTY];
        let hc = build_news_post_chunks(b"", &mut chunks);
        assert_eq!(hc, 1);
        assert_eq!(chunks[0].len, 0);
        assert!(!chunks[0].data.is_null());
    }

    #[test]
    fn news_post_rejects_empty_chunks_slice() {
        let mut chunks: [HxChunk; 0] = [];
        assert_eq!(build_news_post_chunks(b"x", &mut chunks), 0);
    }

    #[test]
    fn news_post_rejects_body_larger_than_u16_max() {
        let big = vec![0u8; u16::MAX as usize + 1];
        let mut chunks = [HxChunk::EMPTY];
        assert_eq!(build_news_post_chunks(&big, &mut chunks), 0);
    }

    // ---- NEWSPATH-only opcodes ----

    #[test]
    fn news_path_only_builders_share_shape() {
        // catlist / dirlist / delete / mkdir all wrap the same helper.
        // Each must emit a single NEWSPATH chunk with the verbatim
        // bytes.
        for &builder in &[
            build_news_catlist_chunks as fn(&[u8], &mut [HxChunk]) -> usize,
            build_news_dirlist_chunks,
            build_news_delete_chunks,
        ] {
            let mut chunks = [HxChunk::EMPTY];
            let hc = builder(b"/Articles/2026", &mut chunks);
            assert_eq!(hc, 1);
            assert_eq!(chunks[0].tag, tag::NEWSPATH);
            assert_eq!(chunks[0].len, 14);
            assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, b"/Articles/2026");
        }
    }

    #[test]
    fn mkdir_sends_parent_path_and_name_separately() {
        // MAKENEWSDIR is NOT a NEWSPATH-only opcode, which is what the
        // single-chunk shape above used to assume. The server resolves the
        // path as an existing folder and creates FILE_NAME inside it, so the
        // new folder's name must not be folded into the path.
        let mut chunks = [HxChunk::EMPTY; 2];
        let req = NewsMakeDirRequest {
            path: b"/Articles",
            name: b"2026",
        };
        assert_eq!(build_news_mkdir_chunks(&req, &mut chunks), 2);
        assert_eq!(chunks[0].tag, tag::NEWSPATH);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, b"/Articles");
        assert_eq!(chunks[1].tag, tag::FILE_NAME);
        assert_eq!(unsafe { chunk_bytes(&chunks[1]) }, b"2026");

        // Creating at the root is an empty path, not a missing chunk.
        let req = NewsMakeDirRequest {
            path: b"",
            name: b"Top",
        };
        assert_eq!(build_news_mkdir_chunks(&req, &mut chunks), 2);
        assert_eq!(chunks[0].tag, tag::NEWSPATH);
        assert_eq!(chunks[0].len, 0);
        assert_eq!(unsafe { chunk_bytes(&chunks[1]) }, b"Top");

        // Too few slots is a validation failure, not a partial write.
        let mut one = [HxChunk::EMPTY; 1];
        assert_eq!(build_news_mkdir_chunks(&req, &mut one), 0);
    }

    #[test]
    fn news_path_only_empty_path_legal() {
        // Root path: zero-length NEWSPATH chunk. Empty body is legal
        // (same convention as every other variable-length builder
        // here); data pointer non-NULL so the C side's defensive
        // null-checks don't fire.
        let mut chunks = [HxChunk::EMPTY];
        let hc = build_news_catlist_chunks(b"", &mut chunks);
        assert_eq!(hc, 1);
        assert_eq!(chunks[0].len, 0);
        assert!(!chunks[0].data.is_null());
    }

    #[test]
    fn news_path_only_rejects_empty_chunks_slice() {
        let mut chunks: [HxChunk; 0] = [];
        assert_eq!(build_news_dirlist_chunks(b"x", &mut chunks), 0);
    }

    #[test]
    fn news_path_only_rejects_path_larger_than_u16_max() {
        let big = vec![b'/'; u16::MAX as usize + 1];
        let mut chunks = [HxChunk::EMPTY];
        assert_eq!(build_news_delete_chunks(&big, &mut chunks), 0);
    }

    // ---- news delete_thread ----

    #[test]
    fn news_delete_thread_emits_path_then_threadid() {
        let req = NewsDeleteThreadRequest {
            path: b"/Articles",
            threadid: 0xdead_beef,
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        let hc = build_news_delete_thread_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 2);
        assert_eq!(chunks[0].tag, tag::NEWSPATH);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, b"/Articles");
        assert_eq!(chunks[1].tag, tag::THREADID);
        assert_eq!(chunks[1].len, 4);
        assert_eq!(
            unsafe { chunk_bytes(&chunks[1]) },
            &[0xde, 0xad, 0xbe, 0xef]
        );
    }

    #[test]
    fn news_delete_thread_rejects_short_buffers() {
        let req = NewsDeleteThreadRequest {
            path: b"p",
            threadid: 1,
        };
        let mut chunks_short = [HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        assert_eq!(
            build_news_delete_thread_chunks(&req, &mut chunks_short, &mut scratch),
            0
        );
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch_short = [0u8; 3];
        assert_eq!(
            build_news_delete_thread_chunks(&req, &mut chunks, &mut scratch_short),
            0
        );
    }

    // ---- news getthread ----

    #[test]
    fn news_getthread_emits_path_threadid_mime() {
        let req = NewsGetThreadRequest {
            path: b"/News",
            threadid: 0x42,
            mime_type: b"text/plain",
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        let hc = build_news_getthread_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 3);
        assert_eq!(chunks[0].tag, tag::NEWSPATH);
        assert_eq!(chunks[1].tag, tag::THREADID);
        assert_eq!(unsafe { chunk_bytes(&chunks[1]) }, &[0, 0, 0, 0x42]);
        assert_eq!(chunks[2].tag, tag::NEWSTYPE);
        assert_eq!(chunks[2].len, 10);
        assert_eq!(unsafe { chunk_bytes(&chunks[2]) }, b"text/plain");
    }

    #[test]
    fn news_getthread_rejects_oversize_fields() {
        let big = vec![b'x'; u16::MAX as usize + 1];
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY, HxChunk::EMPTY];
        let mut scratch = [0u8; 4];
        // Oversize path.
        let req_path = NewsGetThreadRequest {
            path: &big,
            threadid: 0,
            mime_type: b"text/plain",
        };
        assert_eq!(
            build_news_getthread_chunks(&req_path, &mut chunks, &mut scratch),
            0
        );
        // Oversize mime.
        let req_mime = NewsGetThreadRequest {
            path: b"p",
            threadid: 0,
            mime_type: &big,
        };
        assert_eq!(
            build_news_getthread_chunks(&req_mime, &mut chunks, &mut scratch),
            0
        );
    }

    // ---- news mkcat ----

    #[test]
    fn news_mkcat_emits_path_then_category() {
        let req = NewsMakeCategoryRequest {
            path: b"/Articles",
            name: b"Reviews",
        };
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        let hc = build_news_mkcat_chunks(&req, &mut chunks);
        assert_eq!(hc, 2);
        assert_eq!(chunks[0].tag, tag::NEWSPATH);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, b"/Articles");
        assert_eq!(chunks[1].tag, tag::CATEGORY);
        assert_eq!(chunks[1].len, 7);
        assert_eq!(unsafe { chunk_bytes(&chunks[1]) }, b"Reviews");
    }

    #[test]
    fn news_mkcat_rejects_short_buffer_or_oversize_fields() {
        let req = NewsMakeCategoryRequest {
            path: b"p",
            name: b"n",
        };
        let mut chunks_short = [HxChunk::EMPTY];
        assert_eq!(build_news_mkcat_chunks(&req, &mut chunks_short), 0);

        let big = vec![b'q'; u16::MAX as usize + 1];
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        // Oversize path.
        let req_path = NewsMakeCategoryRequest {
            path: &big,
            name: b"n",
        };
        assert_eq!(build_news_mkcat_chunks(&req_path, &mut chunks), 0);
        // Oversize name.
        let req_name = NewsMakeCategoryRequest {
            path: b"p",
            name: &big,
        };
        assert_eq!(build_news_mkcat_chunks(&req_name, &mut chunks), 0);
    }

    // ---- news post_thread (6 chunks) ----

    #[test]
    fn news_post_thread_emits_six_chunks_in_order() {
        let req = NewsPostThreadRequest {
            path: b"/Articles",
            parent_thread: 0,
            mime_type: b"text/plain",
            subject: b"Hello",
            text: b"World",
            thread_id: 0xcafe_babe,
        };
        let mut chunks = [HxChunk::EMPTY; 6];
        let mut scratch = [0u8; 8];
        let hc = build_news_post_thread_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 6);
        assert_eq!(chunks[0].tag, tag::NEWSPATH);
        assert_eq!(chunks[1].tag, tag::PARENTTHREAD);
        assert_eq!(unsafe { chunk_bytes(&chunks[1]) }, &[0, 0, 0, 0]);
        assert_eq!(chunks[2].tag, tag::NEWSTYPE);
        assert_eq!(unsafe { chunk_bytes(&chunks[2]) }, b"text/plain");
        assert_eq!(chunks[3].tag, tag::NEWSSUBJECT);
        assert_eq!(unsafe { chunk_bytes(&chunks[3]) }, b"Hello");
        assert_eq!(chunks[4].tag, tag::NEWSDATA);
        assert_eq!(unsafe { chunk_bytes(&chunks[4]) }, b"World");
        assert_eq!(chunks[5].tag, tag::THREADID);
        assert_eq!(
            unsafe { chunk_bytes(&chunks[5]) },
            &[0xca, 0xfe, 0xba, 0xbe]
        );
    }

    #[test]
    fn news_post_thread_rejects_short_buffers() {
        let req = NewsPostThreadRequest {
            path: b"p",
            parent_thread: 0,
            mime_type: b"m",
            subject: b"s",
            text: b"t",
            thread_id: 0,
        };
        let mut chunks_short = [HxChunk::EMPTY; 5];
        let mut scratch = [0u8; 8];
        assert_eq!(
            build_news_post_thread_chunks(&req, &mut chunks_short, &mut scratch),
            0
        );
        let mut chunks = [HxChunk::EMPTY; 6];
        let mut scratch_short = [0u8; 7];
        assert_eq!(
            build_news_post_thread_chunks(&req, &mut chunks, &mut scratch_short),
            0
        );
    }

    #[test]
    fn news_post_thread_rejects_oversize_fields() {
        let big = vec![b'q'; u16::MAX as usize + 1];
        for which in 0..4 {
            let req = NewsPostThreadRequest {
                path: if which == 0 { &big } else { b"p" },
                parent_thread: 0,
                mime_type: if which == 1 { &big } else { b"m" },
                subject: if which == 2 { &big } else { b"s" },
                text: if which == 3 { &big } else { b"t" },
                thread_id: 0,
            };
            let mut chunks = [HxChunk::EMPTY; 6];
            let mut scratch = [0u8; 8];
            assert_eq!(
                build_news_post_thread_chunks(&req, &mut chunks, &mut scratch),
                0,
                "oversize field {which} should be rejected"
            );
        }
    }

    // ---- file mkdir ----

    #[test]
    fn file_mkdir_emits_dir_chunk() {
        let mut chunks = [HxChunk::EMPTY];
        let hc = build_file_mkdir_chunks(b"Uploads/2026", &mut chunks);
        assert_eq!(hc, 1);
        assert_eq!(chunks[0].tag, tag::DIR);
        assert_eq!(chunks[0].len, 12);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, b"Uploads/2026");
    }

    #[test]
    fn file_mkdir_rejects_short_buffer_and_oversize() {
        let mut empty: [HxChunk; 0] = [];
        assert_eq!(build_file_mkdir_chunks(b"x", &mut empty), 0);
        let big = vec![b'/'; u16::MAX as usize + 1];
        let mut chunks = [HxChunk::EMPTY];
        assert_eq!(build_file_mkdir_chunks(&big, &mut chunks), 0);
    }

    #[test]
    fn file_list_shares_mkdir_shape() {
        // FILE_LIST is wire-identical to FILE_MKDIR (server dispatches
        // on the header opcode). Verify the same single-DIR contract
        // and validation behaviour holds through the alias.
        let mut chunks = [HxChunk::EMPTY];
        let hc = build_file_list_chunks(b"Public", &mut chunks);
        assert_eq!(hc, 1);
        assert_eq!(chunks[0].tag, tag::DIR);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, b"Public");

        let mut empty: [HxChunk; 0] = [];
        assert_eq!(build_file_list_chunks(b"x", &mut empty), 0);
        let big = vec![b'/'; u16::MAX as usize + 1];
        let mut chunks2 = [HxChunk::EMPTY];
        assert_eq!(build_file_list_chunks(&big, &mut chunks2), 0);
    }

    // ---- file delete / getinfo / getfolder (shared shape) ----

    #[test]
    fn file_name_with_dir_emits_two_chunks() {
        // FILE_NAME first, then DIR — verified across all three
        // wrappers that share the helper.
        for &builder in &[
            build_file_delete_chunks as fn(&[u8], Option<&[u8]>, &mut [HxChunk]) -> usize,
            build_file_getinfo_chunks,
            build_file_getfolder_chunks,
        ] {
            let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
            let hc = builder(b"file.txt", Some(b"Public"), &mut chunks);
            assert_eq!(hc, 2);
            assert_eq!(chunks[0].tag, tag::FILE_NAME);
            assert_eq!(chunks[0].len, 8);
            assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, b"file.txt");
            assert_eq!(chunks[1].tag, tag::DIR);
            assert_eq!(chunks[1].len, 6);
            assert_eq!(unsafe { chunk_bytes(&chunks[1]) }, b"Public");
        }
    }

    #[test]
    fn file_name_without_dir_emits_one_chunk() {
        // None for the dir argument: only the FILE_NAME chunk is
        // emitted. The single-chunk slice is sufficient.
        let mut chunks = [HxChunk::EMPTY];
        let hc = build_file_delete_chunks(b"root.bin", None, &mut chunks);
        assert_eq!(hc, 1);
        assert_eq!(chunks[0].tag, tag::FILE_NAME);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, b"root.bin");
    }

    #[test]
    fn file_name_with_dir_rejects_short_chunks_slice() {
        // With dir present, builder needs >= 2 slots.
        let mut chunks = [HxChunk::EMPTY];
        assert_eq!(build_file_getinfo_chunks(b"f", Some(b"d"), &mut chunks), 0);
    }

    #[test]
    fn file_name_only_rejects_empty_chunks_slice() {
        let mut chunks: [HxChunk; 0] = [];
        assert_eq!(build_file_getfolder_chunks(b"x", None, &mut chunks), 0);
    }

    #[test]
    fn file_name_with_dir_empty_strings_legal() {
        // Empty name + empty dir: zero-length chunks, non-NULL data
        // pointers so the C side's defensive null-checks don't fire.
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        let hc = build_file_getinfo_chunks(b"", Some(b""), &mut chunks);
        assert_eq!(hc, 2);
        assert_eq!(chunks[0].len, 0);
        assert_eq!(chunks[1].len, 0);
        assert!(!chunks[0].data.is_null());
        assert!(!chunks[1].data.is_null());
    }

    #[test]
    fn file_name_with_dir_rejects_oversize_fields() {
        let big = vec![b'q'; u16::MAX as usize + 1];
        let mut chunks = [HxChunk::EMPTY, HxChunk::EMPTY];
        // Oversize name.
        assert_eq!(build_file_delete_chunks(&big, Some(b"d"), &mut chunks), 0);
        // Oversize dir.
        assert_eq!(build_file_delete_chunks(b"n", Some(&big), &mut chunks), 0);
        // Oversize name with no dir.
        let mut chunks_single = [HxChunk::EMPTY];
        assert_eq!(build_file_delete_chunks(&big, None, &mut chunks_single), 0);
    }

    // ---- file setinfo ----

    #[test]
    fn file_setinfo_full_emits_four_chunks_in_order() {
        // The "rename + comment dialog" call site: all four fields
        // present. Verifies NAME → RENAME → COMMENT → DIR ordering.
        let req = FileSetInfoRequest {
            name: b"old.txt",
            rename: b"new.txt",
            comment: Some(b"the comment"),
            dir: Some(b"Uploads"),
        };
        let mut chunks = [HxChunk::EMPTY; 4];
        let hc = build_file_setinfo_chunks(&req, &mut chunks);
        assert_eq!(hc, 4);
        assert_eq!(chunks[0].tag, tag::FILE_NAME);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, b"old.txt");
        assert_eq!(chunks[1].tag, tag::FILE_RENAME);
        assert_eq!(unsafe { chunk_bytes(&chunks[1]) }, b"new.txt");
        assert_eq!(chunks[2].tag, tag::FILE_COMMENT);
        assert_eq!(unsafe { chunk_bytes(&chunks[2]) }, b"the comment");
        assert_eq!(chunks[3].tag, tag::DIR);
        assert_eq!(unsafe { chunk_bytes(&chunks[3]) }, b"Uploads");
    }

    #[test]
    fn file_setinfo_rename_only_emits_three_chunks() {
        // The hx_file_move within-dir rename branch: comment is None,
        // dir is Some. Output is NAME + RENAME + DIR (no COMMENT chunk).
        let req = FileSetInfoRequest {
            name: b"old.txt",
            rename: b"new.txt",
            comment: None,
            dir: Some(b"Uploads"),
        };
        let mut chunks = [HxChunk::EMPTY; 3];
        let hc = build_file_setinfo_chunks(&req, &mut chunks);
        assert_eq!(hc, 3);
        assert_eq!(chunks[0].tag, tag::FILE_NAME);
        assert_eq!(chunks[1].tag, tag::FILE_RENAME);
        assert_eq!(chunks[2].tag, tag::DIR);
    }

    #[test]
    fn file_setinfo_no_comment_no_dir_emits_two_chunks() {
        // Root-level rename with no comment dialog text: only NAME +
        // RENAME. Smallest legal chunks slice is 2.
        let req = FileSetInfoRequest {
            name: b"a",
            rename: b"b",
            comment: None,
            dir: None,
        };
        let mut chunks = [HxChunk::EMPTY; 2];
        let hc = build_file_setinfo_chunks(&req, &mut chunks);
        assert_eq!(hc, 2);
        assert_eq!(chunks[0].tag, tag::FILE_NAME);
        assert_eq!(chunks[1].tag, tag::FILE_RENAME);
    }

    #[test]
    fn file_setinfo_comment_only_emits_three_chunks() {
        // Rename + comment on a root-level file: NAME + RENAME +
        // COMMENT, no DIR.
        let req = FileSetInfoRequest {
            name: b"a",
            rename: b"b",
            comment: Some(b"hi"),
            dir: None,
        };
        let mut chunks = [HxChunk::EMPTY; 3];
        let hc = build_file_setinfo_chunks(&req, &mut chunks);
        assert_eq!(hc, 3);
        assert_eq!(chunks[2].tag, tag::FILE_COMMENT);
    }

    #[test]
    fn file_setinfo_rejects_short_chunks_slice() {
        // Full setinfo wants 4 slots; 3 is too few.
        let req = FileSetInfoRequest {
            name: b"a",
            rename: b"b",
            comment: Some(b"c"),
            dir: Some(b"d"),
        };
        let mut chunks = [HxChunk::EMPTY; 3];
        assert_eq!(build_file_setinfo_chunks(&req, &mut chunks), 0);
        // 2-slot slice is too few for rename-only-with-dir (needs 3).
        let req2 = FileSetInfoRequest {
            name: b"a",
            rename: b"b",
            comment: None,
            dir: Some(b"d"),
        };
        let mut chunks2 = [HxChunk::EMPTY; 2];
        assert_eq!(build_file_setinfo_chunks(&req2, &mut chunks2), 0);
        // 1-slot slice is too few even for the minimal NAME+RENAME.
        let req3 = FileSetInfoRequest {
            name: b"a",
            rename: b"b",
            comment: None,
            dir: None,
        };
        let mut chunks3 = [HxChunk::EMPTY; 1];
        assert_eq!(build_file_setinfo_chunks(&req3, &mut chunks3), 0);
    }

    #[test]
    fn file_setinfo_rejects_oversize_fields() {
        let big = vec![b'x'; u16::MAX as usize + 1];
        for which in 0..4 {
            let req = FileSetInfoRequest {
                name: if which == 0 { &big } else { b"n" },
                rename: if which == 1 { &big } else { b"r" },
                comment: Some(if which == 2 { big.as_slice() } else { b"c" }),
                dir: Some(if which == 3 { big.as_slice() } else { b"d" }),
            };
            let mut chunks = [HxChunk::EMPTY; 4];
            assert_eq!(
                build_file_setinfo_chunks(&req, &mut chunks),
                0,
                "oversize field {which} should be rejected"
            );
        }
    }

    #[test]
    fn file_setinfo_empty_strings_legal() {
        // All fields empty: non-NULL data pointers preserved so the C
        // side's defensive checks don't fire.
        let req = FileSetInfoRequest {
            name: b"",
            rename: b"",
            comment: Some(b""),
            dir: Some(b""),
        };
        let mut chunks = [HxChunk::EMPTY; 4];
        let hc = build_file_setinfo_chunks(&req, &mut chunks);
        assert_eq!(hc, 4);
        for c in &chunks {
            assert_eq!(c.len, 0);
            assert!(!c.data.is_null());
        }
    }

    // ---- file move ----

    #[test]
    fn file_move_emits_three_chunks_in_order() {
        let req = FileMoveRequest {
            name: b"file.bin",
            dir: b"src",
            dir_rename: b"dst",
        };
        let mut chunks = [HxChunk::EMPTY; 3];
        let hc = build_file_move_chunks(&req, &mut chunks);
        assert_eq!(hc, 3);
        assert_eq!(chunks[0].tag, tag::FILE_NAME);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, b"file.bin");
        assert_eq!(chunks[1].tag, tag::DIR);
        assert_eq!(unsafe { chunk_bytes(&chunks[1]) }, b"src");
        assert_eq!(chunks[2].tag, tag::DIR_RENAME);
        assert_eq!(unsafe { chunk_bytes(&chunks[2]) }, b"dst");
    }

    #[test]
    fn file_move_rejects_short_chunks_slice() {
        let req = FileMoveRequest {
            name: b"n",
            dir: b"d",
            dir_rename: b"r",
        };
        let mut chunks = [HxChunk::EMPTY; 2];
        assert_eq!(build_file_move_chunks(&req, &mut chunks), 0);
    }

    #[test]
    fn file_move_rejects_oversize_fields() {
        let big = vec![b'q'; u16::MAX as usize + 1];
        for which in 0..3 {
            let req = FileMoveRequest {
                name: if which == 0 { &big } else { b"n" },
                dir: if which == 1 { &big } else { b"d" },
                dir_rename: if which == 2 { &big } else { b"r" },
            };
            let mut chunks = [HxChunk::EMPTY; 3];
            assert_eq!(
                build_file_move_chunks(&req, &mut chunks),
                0,
                "oversize field {which} should be rejected"
            );
        }
    }

    #[test]
    fn file_move_empty_strings_legal() {
        let req = FileMoveRequest {
            name: b"",
            dir: b"",
            dir_rename: b"",
        };
        let mut chunks = [HxChunk::EMPTY; 3];
        let hc = build_file_move_chunks(&req, &mut chunks);
        assert_eq!(hc, 3);
        for c in &chunks {
            assert_eq!(c.len, 0);
            assert!(!c.data.is_null());
        }
    }

    // ---- file symlink ----

    #[test]
    fn file_symlink_emits_four_chunks_in_order() {
        let req = FileSymlinkRequest {
            name: b"file.bin",
            dir: b"src",
            dir_rename: b"dst",
            rename: b"link.bin",
        };
        let mut chunks = [HxChunk::EMPTY; 4];
        let hc = build_file_symlink_chunks(&req, &mut chunks);
        assert_eq!(hc, 4);
        assert_eq!(chunks[0].tag, tag::FILE_NAME);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, b"file.bin");
        assert_eq!(chunks[1].tag, tag::DIR);
        assert_eq!(unsafe { chunk_bytes(&chunks[1]) }, b"src");
        assert_eq!(chunks[2].tag, tag::DIR_RENAME);
        assert_eq!(unsafe { chunk_bytes(&chunks[2]) }, b"dst");
        assert_eq!(chunks[3].tag, tag::FILE_RENAME);
        assert_eq!(unsafe { chunk_bytes(&chunks[3]) }, b"link.bin");
    }

    #[test]
    fn file_symlink_rejects_short_chunks_slice() {
        let req = FileSymlinkRequest {
            name: b"n",
            dir: b"d",
            dir_rename: b"r",
            rename: b"l",
        };
        let mut chunks = [HxChunk::EMPTY; 3];
        assert_eq!(build_file_symlink_chunks(&req, &mut chunks), 0);
    }

    #[test]
    fn file_symlink_rejects_oversize_fields() {
        let big = vec![b'q'; u16::MAX as usize + 1];
        for which in 0..4 {
            let req = FileSymlinkRequest {
                name: if which == 0 { &big } else { b"n" },
                dir: if which == 1 { &big } else { b"d" },
                dir_rename: if which == 2 { &big } else { b"r" },
                rename: if which == 3 { &big } else { b"l" },
            };
            let mut chunks = [HxChunk::EMPTY; 4];
            assert_eq!(
                build_file_symlink_chunks(&req, &mut chunks),
                0,
                "oversize field {which} should be rejected"
            );
        }
    }

    // ---- file putfolder ----

    #[test]
    fn file_putfolder_with_dir_emits_four_chunks_in_order() {
        let req = FilePutFolderRequest {
            name: b"MyFolder",
            dir: Some(b"Uploads"),
            size: 0x0102_0304,
            nfiles: 42,
        };
        let mut chunks = [HxChunk::EMPTY; 4];
        let mut scratch = [0u8; 8];
        let hc = build_file_putfolder_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 4);
        assert_eq!(chunks[0].tag, tag::FILE_NAME);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, b"MyFolder");
        assert_eq!(chunks[1].tag, tag::DIR);
        assert_eq!(unsafe { chunk_bytes(&chunks[1]) }, b"Uploads");
        assert_eq!(chunks[2].tag, tag::HTXF_SIZE);
        assert_eq!(chunks[2].len, 4);
        assert_eq!(
            unsafe { chunk_bytes(&chunks[2]) },
            &[0x01, 0x02, 0x03, 0x04]
        );
        assert_eq!(chunks[3].tag, tag::FILE_NFILES);
        assert_eq!(chunks[3].len, 4);
        assert_eq!(unsafe { chunk_bytes(&chunks[3]) }, &[0, 0, 0, 42]);
    }

    #[test]
    fn file_putfolder_without_dir_emits_three_chunks() {
        let req = FilePutFolderRequest {
            name: b"MyFolder",
            dir: None,
            size: 1024,
            nfiles: 3,
        };
        let mut chunks = [HxChunk::EMPTY; 3];
        let mut scratch = [0u8; 8];
        let hc = build_file_putfolder_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 3);
        assert_eq!(chunks[0].tag, tag::FILE_NAME);
        assert_eq!(chunks[1].tag, tag::HTXF_SIZE);
        assert_eq!(chunks[2].tag, tag::FILE_NFILES);
    }

    #[test]
    fn file_putfolder_rejects_short_scratch() {
        let req = FilePutFolderRequest {
            name: b"x",
            dir: None,
            size: 0,
            nfiles: 0,
        };
        let mut chunks = [HxChunk::EMPTY; 3];
        let mut scratch = [0u8; 7];
        assert_eq!(
            build_file_putfolder_chunks(&req, &mut chunks, &mut scratch),
            0
        );
    }

    #[test]
    fn file_putfolder_rejects_short_chunks_slice() {
        // With dir: needs 4 slots.
        let req = FilePutFolderRequest {
            name: b"x",
            dir: Some(b"d"),
            size: 0,
            nfiles: 0,
        };
        let mut chunks = [HxChunk::EMPTY; 3];
        let mut scratch = [0u8; 8];
        assert_eq!(
            build_file_putfolder_chunks(&req, &mut chunks, &mut scratch),
            0
        );
        // Without dir: needs 3 slots; 2 is too few.
        let req2 = FilePutFolderRequest {
            name: b"x",
            dir: None,
            size: 0,
            nfiles: 0,
        };
        let mut chunks2 = [HxChunk::EMPTY; 2];
        assert_eq!(
            build_file_putfolder_chunks(&req2, &mut chunks2, &mut scratch),
            0
        );
    }

    #[test]
    fn file_putfolder_rejects_oversize_fields() {
        let big = vec![b'q'; u16::MAX as usize + 1];
        // Oversize name.
        let req = FilePutFolderRequest {
            name: &big,
            dir: None,
            size: 0,
            nfiles: 0,
        };
        let mut chunks = [HxChunk::EMPTY; 4];
        let mut scratch = [0u8; 8];
        assert_eq!(
            build_file_putfolder_chunks(&req, &mut chunks, &mut scratch),
            0
        );
        // Oversize dir.
        let req2 = FilePutFolderRequest {
            name: b"x",
            dir: Some(&big),
            size: 0,
            nfiles: 0,
        };
        assert_eq!(
            build_file_putfolder_chunks(&req2, &mut chunks, &mut scratch),
            0
        );
    }

    // ---- file get ----

    fn make_rflt() -> [u8; 74] {
        let mut rflt = [0u8; 74];
        rflt[0..4].copy_from_slice(b"RFLT");
        // Fork offsets at 46 / 62; rest is the static template the
        // C side memcpys before stamping the offsets in.
        rflt[46..50].copy_from_slice(&0x0000_1234u32.to_be_bytes());
        rflt[62..66].copy_from_slice(&0x0000_5678u32.to_be_bytes());
        rflt
    }

    #[test]
    fn file_get_minimal_emits_just_name() {
        // Fresh download from the root: NAME alone.
        let req = FileGetRequest {
            name: b"file.bin",
            dir: None,
            rflt: None,
        };
        let mut chunks = [HxChunk::EMPTY];
        let hc = build_file_get_chunks(&req, &mut chunks);
        assert_eq!(hc, 1);
        assert_eq!(chunks[0].tag, tag::FILE_NAME);
        assert_eq!(unsafe { chunk_bytes(&chunks[0]) }, b"file.bin");
    }

    #[test]
    fn file_get_with_dir_emits_two_chunks() {
        let req = FileGetRequest {
            name: b"file.bin",
            dir: Some(b"Public"),
            rflt: None,
        };
        let mut chunks = [HxChunk::EMPTY; 2];
        let hc = build_file_get_chunks(&req, &mut chunks);
        assert_eq!(hc, 2);
        assert_eq!(chunks[0].tag, tag::FILE_NAME);
        assert_eq!(chunks[1].tag, tag::DIR);
    }

    #[test]
    fn file_get_resume_with_dir_emits_three_chunks_in_order() {
        let rflt = make_rflt();
        let req = FileGetRequest {
            name: b"file.bin",
            dir: Some(b"Public"),
            rflt: Some(&rflt),
        };
        let mut chunks = [HxChunk::EMPTY; 3];
        let hc = build_file_get_chunks(&req, &mut chunks);
        assert_eq!(hc, 3);
        assert_eq!(chunks[0].tag, tag::FILE_NAME);
        assert_eq!(chunks[1].tag, tag::DIR);
        assert_eq!(chunks[2].tag, tag::RFLT);
        assert_eq!(chunks[2].len, 74);
        assert_eq!(unsafe { chunk_bytes(&chunks[2]) }, &rflt);
    }

    #[test]
    fn file_get_resume_root_emits_name_then_rflt() {
        let rflt = make_rflt();
        let req = FileGetRequest {
            name: b"file.bin",
            dir: None,
            rflt: Some(&rflt),
        };
        let mut chunks = [HxChunk::EMPTY; 2];
        let hc = build_file_get_chunks(&req, &mut chunks);
        assert_eq!(hc, 2);
        assert_eq!(chunks[0].tag, tag::FILE_NAME);
        assert_eq!(chunks[1].tag, tag::RFLT);
    }

    #[test]
    fn file_get_rejects_rflt_of_wrong_length() {
        // RFLT is a fixed 74-byte record. Anything else is a bug.
        let short = [0u8; 73];
        let long = [0u8; 75];
        let req_short = FileGetRequest {
            name: b"f",
            dir: None,
            rflt: Some(&short),
        };
        let req_long = FileGetRequest {
            name: b"f",
            dir: None,
            rflt: Some(&long),
        };
        let mut chunks = [HxChunk::EMPTY; 2];
        assert_eq!(build_file_get_chunks(&req_short, &mut chunks), 0);
        assert_eq!(build_file_get_chunks(&req_long, &mut chunks), 0);
    }

    #[test]
    fn file_get_rejects_short_chunks_slice() {
        let rflt = make_rflt();
        // Resume + dir = 3 chunks; 2-slot slice insufficient.
        let req = FileGetRequest {
            name: b"f",
            dir: Some(b"d"),
            rflt: Some(&rflt),
        };
        let mut chunks2 = [HxChunk::EMPTY; 2];
        assert_eq!(build_file_get_chunks(&req, &mut chunks2), 0);
        // Empty slice rejects even the minimal NAME-only variant.
        let req_min = FileGetRequest {
            name: b"f",
            dir: None,
            rflt: None,
        };
        let mut empty: [HxChunk; 0] = [];
        assert_eq!(build_file_get_chunks(&req_min, &mut empty), 0);
    }

    #[test]
    fn file_get_rejects_oversize_fields() {
        let big = vec![b'x'; u16::MAX as usize + 1];
        // Oversize name.
        let req = FileGetRequest {
            name: &big,
            dir: None,
            rflt: None,
        };
        let mut chunks = [HxChunk::EMPTY; 3];
        assert_eq!(build_file_get_chunks(&req, &mut chunks), 0);
        // Oversize dir.
        let req2 = FileGetRequest {
            name: b"n",
            dir: Some(&big),
            rflt: None,
        };
        assert_eq!(build_file_get_chunks(&req2, &mut chunks), 0);
    }

    // ---- file put ----

    #[test]
    fn file_put_minimal_emits_name_then_size() {
        // No dir, no preview, no large: NAME + HTXF_SIZE.
        let req = FilePutRequest {
            name: b"upload.bin",
            dir: None,
            has_preview: false,
            size: 0x0102_0304,
            size64: None,
        };
        let mut chunks = [HxChunk::EMPTY; 2];
        let mut scratch = [0u8; 12];
        let hc = build_file_put_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 2);
        assert_eq!(chunks[0].tag, tag::FILE_NAME);
        assert_eq!(chunks[1].tag, tag::HTXF_SIZE);
        assert_eq!(chunks[1].len, 4);
        assert_eq!(
            unsafe { chunk_bytes(&chunks[1]) },
            &[0x01, 0x02, 0x03, 0x04]
        );
    }

    #[test]
    fn file_put_full_emits_all_five_chunks_in_order() {
        // Everything on: NAME + DIR + PREVIEW + HTXF_SIZE + XFERSIZE64.
        let req = FilePutRequest {
            name: b"upload.bin",
            dir: Some(b"Uploads"),
            has_preview: true,
            size: u32::MAX,
            size64: Some(0x0000_0001_0000_0000),
        };
        let mut chunks = [HxChunk::EMPTY; 5];
        let mut scratch = [0u8; 12];
        let hc = build_file_put_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 5);
        assert_eq!(chunks[0].tag, tag::FILE_NAME);
        assert_eq!(chunks[1].tag, tag::DIR);
        assert_eq!(chunks[2].tag, tag::FILE_PREVIEW);
        assert_eq!(chunks[2].len, 2);
        assert_eq!(unsafe { chunk_bytes(&chunks[2]) }, &[0, 1]);
        assert_eq!(chunks[3].tag, tag::HTXF_SIZE);
        assert_eq!(chunks[3].len, 4);
        assert_eq!(
            unsafe { chunk_bytes(&chunks[3]) },
            &[0xff, 0xff, 0xff, 0xff]
        );
        assert_eq!(chunks[4].tag, tag::XFERSIZE64);
        assert_eq!(chunks[4].len, 8);
        assert_eq!(
            unsafe { chunk_bytes(&chunks[4]) },
            &[0, 0, 0, 1, 0, 0, 0, 0]
        );
    }

    #[test]
    fn file_put_dir_no_preview_no_large_emits_three_chunks() {
        let req = FilePutRequest {
            name: b"u",
            dir: Some(b"d"),
            has_preview: false,
            size: 0,
            size64: None,
        };
        let mut chunks = [HxChunk::EMPTY; 3];
        let mut scratch = [0u8; 12];
        let hc = build_file_put_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 3);
        assert_eq!(chunks[0].tag, tag::FILE_NAME);
        assert_eq!(chunks[1].tag, tag::DIR);
        assert_eq!(chunks[2].tag, tag::HTXF_SIZE);
    }

    #[test]
    fn file_put_preview_no_dir_emits_three_chunks() {
        let req = FilePutRequest {
            name: b"u",
            dir: None,
            has_preview: true,
            size: 0,
            size64: None,
        };
        let mut chunks = [HxChunk::EMPTY; 3];
        let mut scratch = [0u8; 12];
        let hc = build_file_put_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 3);
        assert_eq!(chunks[0].tag, tag::FILE_NAME);
        assert_eq!(chunks[1].tag, tag::FILE_PREVIEW);
        assert_eq!(chunks[2].tag, tag::HTXF_SIZE);
    }

    #[test]
    fn file_put_large_no_dir_no_preview_emits_three_chunks() {
        // Large-files-mode, root, fresh: NAME + HTXF_SIZE + XFERSIZE64.
        let req = FilePutRequest {
            name: b"u",
            dir: None,
            has_preview: false,
            size: 0,
            size64: Some(0),
        };
        let mut chunks = [HxChunk::EMPTY; 3];
        let mut scratch = [0u8; 12];
        let hc = build_file_put_chunks(&req, &mut chunks, &mut scratch);
        assert_eq!(hc, 3);
        assert_eq!(chunks[0].tag, tag::FILE_NAME);
        assert_eq!(chunks[1].tag, tag::HTXF_SIZE);
        assert_eq!(chunks[2].tag, tag::XFERSIZE64);
    }

    #[test]
    fn file_put_rejects_short_scratch() {
        let req = FilePutRequest {
            name: b"u",
            dir: None,
            has_preview: false,
            size: 0,
            size64: None,
        };
        let mut chunks = [HxChunk::EMPTY; 2];
        let mut scratch = [0u8; 11];
        assert_eq!(build_file_put_chunks(&req, &mut chunks, &mut scratch), 0);
    }

    #[test]
    fn file_put_rejects_short_chunks_slice() {
        // Full layout (5 chunks) into a 4-slot slice.
        let req = FilePutRequest {
            name: b"u",
            dir: Some(b"d"),
            has_preview: true,
            size: 0,
            size64: Some(0),
        };
        let mut chunks = [HxChunk::EMPTY; 4];
        let mut scratch = [0u8; 12];
        assert_eq!(build_file_put_chunks(&req, &mut chunks, &mut scratch), 0);
        // Minimal layout (2 chunks) into a 1-slot slice.
        let req_min = FilePutRequest {
            name: b"u",
            dir: None,
            has_preview: false,
            size: 0,
            size64: None,
        };
        let mut chunks_min = [HxChunk::EMPTY; 1];
        assert_eq!(
            build_file_put_chunks(&req_min, &mut chunks_min, &mut scratch),
            0
        );
    }

    #[test]
    fn file_put_rejects_oversize_fields() {
        let big = vec![b'x'; u16::MAX as usize + 1];
        let req = FilePutRequest {
            name: &big,
            dir: None,
            has_preview: false,
            size: 0,
            size64: None,
        };
        let mut chunks = [HxChunk::EMPTY; 5];
        let mut scratch = [0u8; 12];
        assert_eq!(build_file_put_chunks(&req, &mut chunks, &mut scratch), 0);
        let req2 = FilePutRequest {
            name: b"n",
            dir: Some(&big),
            has_preview: false,
            size: 0,
            size64: None,
        };
        assert_eq!(build_file_put_chunks(&req2, &mut chunks, &mut scratch), 0);
    }

    // ---- HTXF subframe header packer ----

    #[test]
    fn htxf_hdr_packs_full_16_bytes_be() {
        let mut out = [0u8; 16];
        let ok = build_htxf_hdr(&mut out, 0xdeadbeef, 0x12345678, 0xcafe, 0x0001);
        assert!(ok);
        // magic "HTXF" big-endian.
        assert_eq!(&out[0..4], b"HTXF");
        // ref BE
        assert_eq!(&out[4..8], &0xdeadbeef_u32.to_be_bytes());
        // payload len BE
        assert_eq!(&out[8..12], &0x12345678_u32.to_be_bytes());
        // (type << 16) | flags BE — high u16 is type, low u16 is flags
        assert_eq!(&out[12..16], &0xcafe0001_u32.to_be_bytes());
    }

    #[test]
    fn htxf_preamble_legacy_16_and_size64_24() {
        // Legacy 16-byte variant: header only, real length in the 32-bit field.
        let mut out = [0u8; 24];
        let n = build_htxf_preamble(&mut out, 0xdeadbeef, 0x1234, 0xcafe, 0x0000, false);
        assert_eq!(n, 16);
        assert_eq!(&out[0..4], b"HTXF");
        assert_eq!(&out[8..12], &0x1234_u32.to_be_bytes());
        assert_eq!(&out[12..16], &0xcafe0000_u32.to_be_bytes());

        // size64 variant: 24 bytes, legacy length zeroed, LARGE_FILE|SIZE64 set,
        // true 64-bit size in the trailing 8 bytes.
        let mut out = [0u8; 24];
        let big: u64 = 0x1_0000_0002;
        let n = build_htxf_preamble(&mut out, 7, big, 0x0001, 0x0000, true);
        assert_eq!(n, 24);
        assert_eq!(&out[8..12], &[0u8; 4], "legacy length zeroed under size64");
        let flags = u16::from_be_bytes([out[14], out[15]]);
        assert_eq!(flags, HTXF_FLAG_LARGE_FILE | HTXF_FLAG_SIZE64);
        assert_eq!(&out[16..24], &big.to_be_bytes());

        // Legacy variant fails closed for a >4 GiB size (no silent truncation).
        let mut out = [0u8; 24];
        assert_eq!(
            build_htxf_preamble(&mut out, 1, 0x1_0000_0000, 1, 0, false),
            0
        );
        // Too-small buffers return 0.
        assert_eq!(build_htxf_preamble(&mut [0u8; 15], 1, 1, 1, 0, false), 0);
        assert_eq!(build_htxf_preamble(&mut [0u8; 23], 1, 1, 1, 0, true), 0);
    }

    #[test]
    fn htxf_hdr_zero_type_and_flags_packs_zero_trailer() {
        let mut out = [0u8; 16];
        assert!(build_htxf_hdr(&mut out, 1, 2, 0, 0));
        assert_eq!(&out[12..16], &[0u8; 4]);
    }

    #[test]
    fn htxf_hdr_rejects_short_buffer() {
        let mut out = [0xffu8; 15];
        let copy = out;
        let ok = build_htxf_hdr(&mut out, 1, 2, 3, 4);
        assert!(!ok);
        // Failed pack must not touch the buffer.
        assert_eq!(out, copy);
    }

    #[test]
    fn htxf_hdr_size_const_matches_layout() {
        // The constant exists so C callers (and the FFI shim) can size
        // their buffers without knowing the byte breakdown. Verify it
        // matches the actual write.
        assert_eq!(HTXF_HDR_SIZE, 16);
        let mut out = [0u8; HTXF_HDR_SIZE];
        assert!(build_htxf_hdr(&mut out, 0, 0, 0, 0));
    }

    #[test]
    fn htxf_hdr_extra_capacity_does_not_overwrite_tail() {
        // The pack writes exactly HTXF_HDR_SIZE bytes — anything past
        // that in `out` stays at its original value.
        let mut out = [0xabu8; 32];
        assert!(build_htxf_hdr(&mut out, 1, 2, 3, 4));
        assert!(out[16..].iter().all(|&b| b == 0xab));
    }

    // ---- pack_message ----

    fn pc(tag: u16, data: &[u8]) -> PackChunk<'_> {
        PackChunk { tag, data }
    }

    #[test]
    fn pack_message_size_header_only() {
        assert_eq!(pack_message_size(&[]), 22);
    }

    #[test]
    fn pack_message_size_single_chunk() {
        let body = b"hello";
        let c = [pc(0x101, body)];
        // 22 header + 4 data hdr + 5 body = 31.
        assert_eq!(pack_message_size(&c), 31);
    }

    #[test]
    fn pack_message_size_three_chunks() {
        let a = b"a";
        let bb = b"bb";
        let ccc = b"ccc";
        let chunks = [pc(1, a), pc(2, bb), pc(3, ccc)];
        // 22 + 3 * 4 + 1 + 2 + 3 = 40.
        assert_eq!(pack_message_size(&chunks), 40);
    }

    #[test]
    fn pack_message_header_only_writes_22_bytes() {
        let mut out = [0u8; 32];
        let n = pack_message(&mut out, 0xdeadbeef, 7, 1, &[]).unwrap();
        assert_eq!(n, 22);
        assert_eq!(&out[0..4], &0xdeadbeef_u32.to_be_bytes());
        assert_eq!(&out[4..8], &7u32.to_be_bytes());
        assert_eq!(&out[8..12], &1u32.to_be_bytes());
        // body bytes after the header + sizeof(hc) = 0 + 2 = 2.
        assert_eq!(&out[12..16], &2u32.to_be_bytes());
        assert_eq!(&out[16..20], &2u32.to_be_bytes());
        // hc = 0.
        assert_eq!(&out[20..22], &0u16.to_be_bytes());
        // Bytes past 22 are unchanged.
        assert!(out[22..].iter().all(|&b| b == 0));
    }

    #[test]
    fn pack_message_with_one_chunk_lays_out_data_hdr_then_body() {
        let body = b"hi";
        let chunks = [pc(0x0067, body)]; // 0x67 = HTLC_DATA_CHAT
        let mut out = [0u8; 64];
        let n = pack_message(&mut out, 0x010d, 42, 0, &chunks).unwrap();
        // 22 header + 4 data hdr + 2 body = 28.
        assert_eq!(n, 28);
        // Header
        assert_eq!(&out[0..4], &0x010d_u32.to_be_bytes());
        assert_eq!(&out[4..8], &42u32.to_be_bytes());
        // wire_len = body + hc = (4 + 2) + 2 = 8.
        assert_eq!(&out[12..16], &8u32.to_be_bytes());
        assert_eq!(&out[20..22], &1u16.to_be_bytes());
        // Data hdr
        assert_eq!(&out[22..24], &0x0067_u16.to_be_bytes());
        assert_eq!(&out[24..26], &2u16.to_be_bytes());
        // Body
        assert_eq!(&out[26..28], b"hi");
    }

    #[test]
    fn pack_message_rejects_undersized_buffer() {
        let mut out = [0u8; 21];
        let copy = out;
        let r = pack_message(&mut out, 0, 0, 0, &[]);
        assert!(r.is_none());
        assert_eq!(out, copy);
    }

    #[test]
    fn pack_message_accepts_empty_payload_chunk() {
        // HOPE Step 1 sends an empty HTLC_DATA_SESSIONKEY — an empty
        // payload in the safe API is just a zero-len slice.
        let empty = pc(0xcafe, &[]);
        let mut out = [0u8; 64];
        let n = pack_message(&mut out, 1, 2, 3, &[empty]).unwrap();
        assert_eq!(n, 26); // 22 + 4 + 0
        assert_eq!(&out[22..24], &0xcafe_u16.to_be_bytes());
        assert_eq!(&out[24..26], &0u16.to_be_bytes());
    }

    #[test]
    fn pack_message_does_not_overrun_buffer_past_needed() {
        let body = b"abc";
        let chunks = [pc(9, body)];
        let mut out = [0xffu8; 32];
        let n = pack_message(&mut out, 0, 0, 0, &chunks).unwrap();
        assert_eq!(n, 29);
        assert!(out[29..].iter().all(|&b| b == 0xff));
    }

    #[test]
    fn pack_message_rejects_too_many_chunks() {
        let big: Vec<PackChunk<'_>> = (0..MAX_PACK_CHUNKS + 1)
            .map(|i| pc(i as u16, &[]))
            .collect();
        let mut out = [0u8; 4096];
        assert!(pack_message(&mut out, 0, 0, 0, &big).is_none());
    }

    #[test]
    fn pack_message_rejects_chunk_payload_above_u16_max() {
        // A payload longer than 65535 bytes can't be expressed in the
        // wire's u16 length field. Construct a slice that big via
        // Vec::with_capacity, then take a borrow.
        let huge = vec![0u8; u16::MAX as usize + 1];
        let chunks = [pc(1, &huge)];
        let mut out = vec![0u8; huge.len() + 64];
        assert!(pack_message(&mut out, 0, 0, 0, &chunks).is_none());
    }
}
