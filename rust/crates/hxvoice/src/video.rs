//! The video extension's vocabulary, as the state machine needs it.
//!
//! Video rides the voice session: the same peer connection, the same
//! offer/answer cycle, the same room. What it adds to the machine is the
//! set of media sections a mid can name, the room's publication list
//! (611), the subscription set the client declares (610), and the local
//! publications it starts, pauses and stops (607–609).
//!
//! The mid scanner here is a copy of `hxproto::voice::parse_voice_mid_label`
//! rather than a call to it, for the same reason the opcodes are
//! restated in `state.rs`: this crate stays `no_std` with no protocol
//! dependency. `hxvoice-runtime`'s tests hold the two to the same answers
//! over a shared corpus.

/// A stream kind. The wire values are the spec's: camera 1, screen 2.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum VideoKind {
    Camera,
    Screen,
}

impl VideoKind {
    /// Both kinds, in wire order.
    pub const ALL: [VideoKind; 2] = [VideoKind::Camera, VideoKind::Screen];

    /// The `DATA_VIDEO_KIND` value.
    pub fn wire(self) -> u16 {
        match self {
            VideoKind::Camera => 1,
            VideoKind::Screen => 2,
        }
    }

    /// Decode a wire value; 0 and every reserved value are `None`.
    pub fn from_wire(v: u16) -> Option<VideoKind> {
        match v {
            1 => Some(VideoKind::Camera),
            2 => Some(VideoKind::Screen),
            _ => None,
        }
    }

    /// Slot index for per-kind arrays.
    pub fn index(self) -> usize {
        match self {
            VideoKind::Camera => 0,
            VideoKind::Screen => 1,
        }
    }

    /// The mid of this client's own send section for the kind.
    pub fn send_mid(self) -> &'static str {
        match self {
            VideoKind::Camera => "cam-send",
            VideoKind::Screen => "scr-send",
        }
    }
}

/// One publication in the room, as a 611 reports it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Publication {
    pub user_id: u16,
    pub kind: VideoKind,
    pub paused: bool,
}

/// One stream this client wants to receive.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Stream {
    pub user_id: u16,
    pub kind: VideoKind,
}

/// What a media section carries, read off its mid.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Track {
    /// `send` — this client's microphone.
    Mic,
    /// `user-N` — N's audio.
    Audio(u16),
    /// `cam-send` / `scr-send` — this client's own video of the kind.
    VideoSend(VideoKind),
    /// `cam-user-N` / `scr-user-N` — N's video of the kind.
    Video(u16, VideoKind),
}

impl Track {
    /// The remote user a receive section belongs to.
    pub fn user_id(self) -> Option<u16> {
        match self {
            Track::Audio(u) | Track::Video(u, _) => Some(u),
            _ => None,
        }
    }

    /// True for a section carrying somebody else's media to us.
    pub fn is_receive(self) -> bool {
        matches!(self, Track::Audio(_) | Track::Video(..))
    }
}

/// The spec's mid ceiling: 16 bytes, the RFC 8285 one-byte extension
/// limit. Longer mids are rejected, not truncated.
pub const MID_MAX_LEN: usize = 16;

fn parse_uid(rest: &str) -> Option<u16> {
    if rest.is_empty() || rest.starts_with('0') || !rest.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    let v: u32 = rest.parse().ok()?;
    if (1..=u16::MAX as u32).contains(&v) {
        Some(v as u16)
    } else {
        None
    }
}

/// Map a mid to the track it names; `None` for a mid this client must
/// mirror in its answer but never map to a user or play.
pub fn parse_mid(mid: &str) -> Option<Track> {
    if mid.len() > MID_MAX_LEN {
        return None;
    }
    match mid {
        "send" => return Some(Track::Mic),
        "cam-send" => return Some(Track::VideoSend(VideoKind::Camera)),
        "scr-send" => return Some(Track::VideoSend(VideoKind::Screen)),
        _ => {}
    }
    if let Some(rest) = mid.strip_prefix("user-") {
        return parse_uid(rest).map(Track::Audio);
    }
    if let Some(rest) = mid.strip_prefix("cam-user-") {
        return parse_uid(rest).map(|u| Track::Video(u, VideoKind::Camera));
    }
    if let Some(rest) = mid.strip_prefix("scr-user-") {
        return parse_uid(rest).map(|u| Track::Video(u, VideoKind::Screen));
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_mid_covers_the_grammar() {
        assert_eq!(parse_mid("send"), Some(Track::Mic));
        assert_eq!(parse_mid("user-12"), Some(Track::Audio(12)));
        assert_eq!(
            parse_mid("cam-send"),
            Some(Track::VideoSend(VideoKind::Camera))
        );
        assert_eq!(
            parse_mid("scr-send"),
            Some(Track::VideoSend(VideoKind::Screen))
        );
        assert_eq!(
            parse_mid("cam-user-7"),
            Some(Track::Video(7, VideoKind::Camera))
        );
        assert_eq!(
            parse_mid("scr-user-65535"),
            Some(Track::Video(65535, VideoKind::Screen))
        );
        for bad in [
            "",
            "user-",
            "user-0",
            "user-05",
            "user-65536",
            "cam-user-0",
            "sca-send",
            "sca-user-3",
            "screen-user-5",
            "send ",
            "0",
            "cam-user-12345678",
        ] {
            assert_eq!(parse_mid(bad), None, "{bad:?}");
        }
    }

    #[test]
    fn kinds_round_trip_the_wire() {
        for k in VideoKind::ALL {
            assert_eq!(VideoKind::from_wire(k.wire()), Some(k));
        }
        assert_eq!(VideoKind::from_wire(0), None);
        assert_eq!(VideoKind::from_wire(3), None);
    }
}
