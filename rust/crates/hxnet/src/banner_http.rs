/*
 * Copyright (C) 2026 Misha Nasledov <misha@nasledov.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

//! Banner URL-mode fetch (R3 item 5).
//!
//! The server-banner URL leg used to be a C/libsoup async GET in
//! `banner.c`. Here it's a small blocking `ureq` GET (rustls TLS for
//! `https://` banners) run on the tokio blocking pool — the same
//! `spawn_blocking` shape the file-mode HTXF banner worker already uses,
//! so a sync HTTP client fits naturally and keeps the dependency surface
//! light. The bytes come back to the C banner code through a one-shot
//! poll FFI that mirrors `hxnet_tracker_fetch_*`; `banner.c` feeds them
//! to the shared glycin decode helper.
//!
//! Cancellation is cooperative: closing the handle aborts the awaiting
//! task and drops the receiver, and `banner.c` drops stale results via
//! its generation check. The blocking `ureq` call itself can't be
//! interrupted mid-read — same constraint the HTXF banner worker lives
//! with — but banner images are small (KB to tens of KB) and finish
//! quickly.

use std::io::Read;
use std::os::raw::c_int;
use std::time::Duration;

use hxbridge::runtime::Runtime;
use tokio::sync::{mpsc, Semaphore};
use tokio::task::JoinHandle;

/// Cap on a banner body. Real banners are a few KB to ~50 KB; refuse to
/// slurp a hostile multi-gigabyte response into memory.
const MAX_BANNER_BYTES: usize = 8 * 1024 * 1024;

/// Bound on concurrent blocking banner GETs across the whole process.
/// Closing a handle aborts the async wrapper but can't interrupt a
/// blocking `ureq` GET already in flight (it runs to its read timeout),
/// so a server that floods banner updates could otherwise pile up
/// blocking-pool threads. The permit is held by the *blocking* task (not
/// the abortable wrapper), so it's only released when the GET actually
/// finishes — capping in-flight GETs at this count regardless of how
/// fast handles are opened and closed. A fetch that can't get a permit
/// fails fast instead of queuing.
static BANNER_FETCH_SLOTS: Semaphore = Semaphore::const_new(4);

/// Outcome of one banner fetch.
enum BannerResult {
    Ok(Vec<u8>),
    Err(String),
}

/// Blocking HTTP(S) GET of `url`, capped at [`MAX_BANNER_BYTES`]. Runs on
/// a blocking-pool thread (never an async executor). `https://` URLs go
/// through ureq's rustls backend with the webpki root set.
fn http_get(url: &str) -> Result<Vec<u8>, String> {
    let agent = ureq::builder()
        .timeout_connect(Duration::from_secs(10))
        .timeout_read(Duration::from_secs(20))
        .user_agent("GtkHx")
        .build();

    let resp = match agent.get(url).call() {
        Ok(r) => r,
        Err(ureq::Error::Status(code, _)) => return Err(format!("HTTP {code}")),
        Err(e) => return Err(e.to_string()),
    };

    // Read one byte past the cap so we can tell "exactly at cap" from
    // "over the cap" and reject the latter.
    let mut buf = Vec::new();
    let mut reader = resp.into_reader().take((MAX_BANNER_BYTES as u64) + 1);
    if let Err(e) = reader.read_to_end(&mut buf) {
        return Err(format!("read: {e}"));
    }
    if buf.len() > MAX_BANNER_BYTES {
        return Err(format!("banner exceeds {MAX_BANNER_BYTES} bytes"));
    }
    Ok(buf)
}

// ---- FFI -----------------------------------------------------------------

/// Opaque handle for an in-flight banner fetch.
pub struct HxnetBannerFetch {
    rx: mpsc::Receiver<BannerResult>,
    join: JoinHandle<()>,
    /// Backing store for the borrowed pointers handed out by the last
    /// poll; replaced never (one-shot) and dropped on close.
    current: Option<BannerResult>,
}

/// [`hxnet_banner_fetch_poll`] return codes.
pub const HXNET_BANNER_PENDING: c_int = 0;
pub const HXNET_BANNER_DONE: c_int = 1;
pub const HXNET_BANNER_ERROR: c_int = -1;

/// Result view filled by [`hxnet_banner_fetch_poll`]. On DONE the
/// `bytes_*` pair points at the response body (borrowed from the handle,
/// valid until close); on ERROR the `err_*` pair points at a message.
/// Empty buffers come back as a NULL pointer with len 0.
#[repr(C)]
pub struct HxnetBannerOut {
    pub bytes_ptr: *const u8,
    pub bytes_len: usize,
    pub err_ptr: *const u8,
    pub err_len: usize,
}

// Mirror the C header's _Static_asserts: pin every field offset (all
// pointer-sized: 2 ptrs + 2 usize) plus the total size, so a layout drift
// on either side is a compile error even in Rust-only builds.
const _: () = {
    let p = std::mem::size_of::<*const u8>();
    assert!(std::mem::offset_of!(HxnetBannerOut, bytes_ptr) == 0);
    assert!(std::mem::offset_of!(HxnetBannerOut, bytes_len) == p);
    assert!(std::mem::offset_of!(HxnetBannerOut, err_ptr) == 2 * p);
    assert!(std::mem::offset_of!(HxnetBannerOut, err_len) == 3 * p);
    assert!(std::mem::size_of::<HxnetBannerOut>() == 4 * p);
    assert!(std::mem::align_of::<HxnetBannerOut>() == std::mem::align_of::<*const u8>());
};

fn slice_ptr_len(b: &[u8]) -> (*const u8, usize) {
    if b.is_empty() {
        (std::ptr::null(), 0)
    } else {
        (b.as_ptr(), b.len())
    }
}

/// Open a banner fetch for `url` (`url_len` bytes, UTF-8). Spawns the
/// blocking GET on the tokio blocking pool; the result is drained with
/// [`hxnet_banner_fetch_poll`]. Returns an owned handle (free with
/// [`hxnet_banner_fetch_close`]) or NULL on bad arguments.
///
/// # Safety
///
/// `url` must point at `url_len` readable bytes for the duration of the
/// call.
#[no_mangle]
pub unsafe extern "C" fn hxnet_banner_fetch_open(
    url: *const u8,
    url_len: usize,
) -> *mut HxnetBannerFetch {
    if url.is_null() || url_len == 0 {
        glib::g_critical!("hxnet", "hxnet_banner_fetch_open: NULL or empty url");
        return std::ptr::null_mut();
    }
    if url_len > (isize::MAX as usize) {
        glib::g_critical!("hxnet", "hxnet_banner_fetch_open: url_len exceeds isize::MAX");
        return std::ptr::null_mut();
    }
    let url_str = match std::str::from_utf8(std::slice::from_raw_parts(url, url_len)) {
        Ok(s) => s.to_owned(),
        Err(_) => {
            glib::g_critical!("hxnet", "hxnet_banner_fetch_open: url is not valid UTF-8");
            return std::ptr::null_mut();
        }
    };

    let rt = match std::panic::catch_unwind(std::panic::AssertUnwindSafe(Runtime::global)) {
        Ok(rt) => rt,
        Err(_) => {
            glib::g_critical!(
                "hxnet",
                "hxnet_banner_fetch_open: Runtime::global panicked; aborting"
            );
            std::process::abort();
        }
    };

    let (tx, rx) = mpsc::channel::<BannerResult>(1);
    let join = rt.handle().spawn(async move {
        // ureq is blocking — keep it off the async executor. Acquire the
        // process-wide slot INSIDE the blocking task so the permit tracks
        // the GET's real lifetime (aborting this wrapper can't release it
        // early), bounding how many blocking GETs a flooding server can
        // pile onto the pool.
        let res = tokio::task::spawn_blocking(move || {
            let _permit = match BANNER_FETCH_SLOTS.try_acquire() {
                Ok(p) => p,
                Err(_) => return Err("too many banner fetches in flight".to_owned()),
            };
            http_get(&url_str)
        })
        .await;
        let result = match res {
            Ok(Ok(bytes)) => BannerResult::Ok(bytes),
            Ok(Err(msg)) => BannerResult::Err(msg),
            Err(_) => BannerResult::Err("banner fetch worker panicked".to_owned()),
        };
        let _ = tx.send(result).await;
    });

    Box::into_raw(Box::new(HxnetBannerFetch {
        rx,
        join,
        current: None,
    }))
}

/// Poll for the result. Returns [`HXNET_BANNER_PENDING`] (not ready —
/// try later), [`HXNET_BANNER_DONE`] (`out.bytes_*` set, valid until
/// close), or [`HXNET_BANNER_ERROR`] (`out.err_*` set, or the worker
/// vanished).
///
/// # Safety
///
/// `handle` must be live from [`hxnet_banner_fetch_open`]; `out` must
/// point at a writable [`HxnetBannerOut`].
#[no_mangle]
pub unsafe extern "C" fn hxnet_banner_fetch_poll(
    handle: *mut HxnetBannerFetch,
    out: *mut HxnetBannerOut,
) -> c_int {
    // Zero `out` first whenever it's non-NULL, so a caller that passes a
    // NULL handle but a real out pointer never reads uninitialized
    // ptr/len fields off it.
    if !out.is_null() {
        std::ptr::write_bytes(out, 0, 1);
    }
    if handle.is_null() || out.is_null() {
        glib::g_critical!("hxnet", "hxnet_banner_fetch_poll: NULL handle or out");
        return HXNET_BANNER_ERROR;
    }
    let h = &mut *handle;

    // If a previous poll already resolved this one-shot fetch, keep
    // returning the same result (its borrowed pointers stay valid until
    // close) rather than reading the now-disconnected channel — polling
    // after completion must be idempotent, not flip to ERROR.
    if h.current.is_none() {
        match h.rx.try_recv() {
            Ok(res) => h.current = Some(res),
            Err(mpsc::error::TryRecvError::Empty) => return HXNET_BANNER_PENDING,
            // The worker always sends exactly one result before the
            // sender drops, so a closed-and-empty channel means it
            // vanished — record that so subsequent polls stay consistent.
            Err(mpsc::error::TryRecvError::Disconnected) => {
                h.current = Some(BannerResult::Err("banner fetch worker vanished".to_owned()));
            }
        }
    }

    let o = &mut *out;
    match h.current.as_ref().unwrap() {
        BannerResult::Ok(b) => {
            (o.bytes_ptr, o.bytes_len) = slice_ptr_len(b);
            HXNET_BANNER_DONE
        }
        BannerResult::Err(m) => {
            (o.err_ptr, o.err_len) = slice_ptr_len(m.as_bytes());
            HXNET_BANNER_ERROR
        }
    }
}

/// Cancel (if running) and free a banner fetch handle. NULL-safe.
///
/// # Safety
///
/// `handle` must be NULL or live from [`hxnet_banner_fetch_open`], not
/// used afterwards.
#[no_mangle]
pub unsafe extern "C" fn hxnet_banner_fetch_close(handle: *mut HxnetBannerFetch) {
    if handle.is_null() {
        return;
    }
    let h = Box::from_raw(handle);
    h.join.abort();
    drop(h);
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;
    use std::net::TcpListener;

    /// Spawn a one-shot HTTP/1.1 server that replies `status` with `body`,
    /// returning its base URL.
    fn serve_once(status_line: &'static str, body: &'static [u8]) -> (String, std::thread::JoinHandle<()>) {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        let handle = std::thread::spawn(move || {
            let (mut sock, _) = listener.accept().unwrap();
            let mut buf = [0u8; 2048];
            let _ = sock.read(&mut buf); // drain the request line/headers
            let hdr = format!(
                "HTTP/1.1 {status_line}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                body.len()
            );
            let _ = sock.write_all(hdr.as_bytes());
            let _ = sock.write_all(body);
            let _ = sock.flush();
        });
        (format!("http://127.0.0.1:{port}/banner"), handle)
    }

    #[test]
    fn http_get_returns_body_on_200() {
        let body = b"\x89PNG\r\n\x1a\n-pretend-banner";
        let (url, server) = serve_once("200 OK", body);
        let got = http_get(&url).expect("200 should yield the body");
        server.join().unwrap();
        assert_eq!(got, body);
    }

    #[test]
    fn http_get_errors_on_404() {
        let (url, server) = serve_once("404 Not Found", b"nope");
        let err = http_get(&url).unwrap_err();
        server.join().unwrap();
        assert!(err.contains("404"), "error should mention the status: {err}");
    }

    #[test]
    fn poll_is_idempotent_after_done() {
        // Drive the FFI: open → poll to DONE → poll again must stay DONE
        // with the same borrowed bytes (not flip to ERROR once the
        // one-shot channel disconnects).
        let body = b"\x89PNG-idempotent-banner";
        let (url, server) = serve_once("200 OK", body);
        let h = unsafe { hxnet_banner_fetch_open(url.as_ptr(), url.len()) };
        assert!(!h.is_null());

        let mut out: HxnetBannerOut = unsafe { std::mem::zeroed() };
        let mut rc = HXNET_BANNER_PENDING;
        for _ in 0..2000 {
            rc = unsafe { hxnet_banner_fetch_poll(h, &mut out) };
            if rc != HXNET_BANNER_PENDING {
                break;
            }
            std::thread::sleep(Duration::from_millis(5));
        }
        assert_eq!(rc, HXNET_BANNER_DONE);
        let got1 =
            unsafe { std::slice::from_raw_parts(out.bytes_ptr, out.bytes_len) }.to_vec();
        assert_eq!(got1, body);

        // Poll again — still DONE, same bytes.
        let rc2 = unsafe { hxnet_banner_fetch_poll(h, &mut out) };
        assert_eq!(rc2, HXNET_BANNER_DONE);
        let got2 = unsafe { std::slice::from_raw_parts(out.bytes_ptr, out.bytes_len) };
        assert_eq!(got2, body);

        unsafe { hxnet_banner_fetch_close(h) };
        server.join().unwrap();
    }
}
