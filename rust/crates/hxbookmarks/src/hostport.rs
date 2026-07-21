//! IPv6-aware "host:port" split/join, matching the C `gtkhx_parse_host_port`
//! / `gtkhx_join_host_port` semantics used by the legacy bookmark format.
//!
//! The bookmark model stores host and port separately; the legacy HTsc file
//! stores a single "host[:port]" string, so import splits and export joins.

/// Split a `host`, `host:port`, `[ipv6]`, `[ipv6]:port`, or bare-IPv6
/// (`fe80::1`) string into `(host, port)`. `port` is `""` when absent.
///
/// Mirrors `gtkhx_parse_host_port` plus the C bookmark-load fallback:
/// anything that doesn't parse as a clean host:port (non-numeric or
/// out-of-range port, unterminated bracket) is kept whole as the host with
/// no port, rather than rejected.
pub fn split(s: &str) -> (String, String) {
    let s = s.trim();
    if s.is_empty() {
        return (String::new(), String::new());
    }

    // Bracketed IPv6: "[addr]" or "[addr]:port".
    if let Some(rest) = s.strip_prefix('[') {
        if let Some(close) = rest.find(']') {
            let host = &rest[..close];
            let after = &rest[close + 1..];
            if after.is_empty() {
                return (host.to_string(), String::new());
            }
            if let Some(p) = after.strip_prefix(':') {
                if valid_port(p) {
                    return (host.to_string(), p.to_string());
                }
            }
        }
        // Unterminated / malformed bracket → keep whole as host.
        return (s.to_string(), String::new());
    }

    match s.matches(':').count() {
        0 => (s.to_string(), String::new()),
        // Unbracketed IPv6 literal (2+ colons) can't be told apart from
        // host:port — treat as host-only, matching the C.
        1 => {
            let (h, p) = s.split_once(':').unwrap();
            if valid_port(p) {
                (h.to_string(), p.to_string())
            } else {
                (s.to_string(), String::new())
            }
        }
        _ => (s.to_string(), String::new()),
    }
}

/// Join `host` + `port` into "host:port" (or just "host" when `port` is
/// empty). Naive, matching the C bookmark writer — no IPv6 bracketing, since
/// that's the exact byte shape legacy readers (and other Hotline clients)
/// expect.
pub fn join(host: &str, port: &str) -> String {
    if port.is_empty() {
        host.to_string()
    } else {
        format!("{host}:{port}")
    }
}

/// A complete decimal port in 1..=65535.
fn valid_port(p: &str) -> bool {
    !p.is_empty()
        && p.bytes().all(|b| b.is_ascii_digit())
        && matches!(p.parse::<u32>(), Ok(n) if (1..=65535).contains(&n))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn plain_host() {
        assert_eq!(split("hlserver.com"), ("hlserver.com".into(), "".into()));
    }

    #[test]
    fn host_port() {
        assert_eq!(split("host:5500"), ("host".into(), "5500".into()));
    }

    #[test]
    fn non_numeric_port_kept_whole() {
        assert_eq!(split("host:abc"), ("host:abc".into(), "".into()));
    }

    #[test]
    fn out_of_range_port_kept_whole() {
        assert_eq!(split("host:99999"), ("host:99999".into(), "".into()));
    }

    #[test]
    fn bare_ipv6_is_host_only() {
        assert_eq!(split("fe80::1"), ("fe80::1".into(), "".into()));
    }

    #[test]
    fn bracketed_ipv6_with_port() {
        assert_eq!(split("[::1]:5500"), ("::1".into(), "5500".into()));
    }

    #[test]
    fn bracketed_ipv6_no_port() {
        assert_eq!(split("[::1]"), ("::1".into(), "".into()));
    }

    #[test]
    fn join_roundtrips() {
        assert_eq!(join("host", "5500"), "host:5500");
        assert_eq!(join("host", ""), "host");
    }
}
