//! The typed settings schema.
//!
//! Every field's `Default` is the compiled-in default the C client ships
//! today, so a fresh profile behaves the same before and after the flip.
//! Where the C code has two defaults for one field — a conservative static
//! initializer in `options.c` plus a real value assigned in `init_variables`
//! — the value here is the one `init_variables` lands on, because that is
//! what a running client has always seen.
//!
//! One field is deliberately *not* carried across: the default nickname is
//! `$USER` in the C startup path, and an environment read has no business
//! being a schema default. It stays at the call site, where
//! [`Identity::nick`]'s "empty means not set" already accommodates it.
//!
//! The schema is deliberately *narrower* than `cfgvars[]`. Dropped on the way
//! across, each for a reason recorded in `docs/preferences.md`: the per-panel
//! window sizes (written on every save, never read back), the four
//! "panel has been opened" latches (set once on first construction and never
//! cleared, with no UI), the never-touched geometry and rate-limit fields, and
//! the `#if 0`'d logging key — logging returns with the feature, under its own
//! table. The cumulative-uptime counter went with `/stats`.

/// The colour scheme applied through `AdwStyleManager`.
///
/// `System` follows the desktop-wide `org.freedesktop.appearance` setting;
/// the other two force it. Stored on disk as the lowercase name, which is the
/// same vocabulary the `THEME` key used, so the migration is a rename.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum ColorScheme {
    #[default]
    System,
    Light,
    Dark,
}

impl ColorScheme {
    pub const ALL: &'static [&'static str] = &["system", "light", "dark"];

    pub fn as_str(self) -> &'static str {
        match self {
            ColorScheme::System => "system",
            ColorScheme::Light => "light",
            ColorScheme::Dark => "dark",
        }
    }

    pub fn parse(s: &str) -> Option<ColorScheme> {
        match s {
            "system" => Some(ColorScheme::System),
            "light" => Some(ColorScheme::Light),
            "dark" => Some(ColorScheme::Dark),
            _ => None,
        }
    }
}

/// The global default identity.
///
/// Today these two preferences have no storage of their own — they alias the
/// live connection's wire name buffer and icon field through a runtime binder,
/// which is why `/nick` on any server rewrites the saved global nickname.
/// Here they are ordinary owned values. Per-connection overrides live in the
/// connection file and resolve at connect time; nothing aliases anything.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Identity {
    /// Empty means "not set". The startup path supplies `$USER` as its
    /// fallback, which is where an environment read belongs.
    pub nick: String,
    /// The Hotline icon ID. Zero is a real (blank) icon rather than an "unset"
    /// sentinel, so the default is the one the C startup path stamps onto a
    /// fresh connection.
    pub icon: u16,
    /// `0x00RRGGBB`, or `NICK_COLOR_NONE` for "no colour set", which makes the
    /// client omit the colour chunk entirely rather than send a value.
    pub nick_color: i32,
}

/// The "no nickname colour" sentinel. Matches `HX_NICK_COLOR_NONE` reinterpreted
/// as a signed int, which is what the `NICKCOLOR` key has always stored.
pub const NICK_COLOR_NONE: i32 = -1;

impl Default for Identity {
    fn default() -> Self {
        Identity {
            nick: String::new(),
            icon: 500,
            nick_color: NICK_COLOR_NONE,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Appearance {
    pub color_scheme: ColorScheme,
    /// Names a theme under `$CONFIG/themes/`. The built-in ships as a
    /// GResource and loads when no on-disk file by that name is found.
    pub theme: String,
    /// Show a `StatusNotifierItem` tray icon. On by default; the runtime
    /// no-ops when no SNI host is around, so it costs nothing where it can't
    /// be rendered.
    pub tray: bool,
}

impl Default for Appearance {
    fn default() -> Self {
        Appearance {
            color_scheme: ColorScheme::System,
            theme: "default".into(),
            tray: true,
        }
    }
}

/// What gets copied when a chat selection is copied.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AutoCopy {
    pub text: bool,
    pub timestamp: bool,
    pub color: bool,
}

impl Default for AutoCopy {
    fn default() -> Self {
        // Text on matches every modern chat client; stamp and colour stay off
        // because most people want a clean copy of the message body.
        AutoCopy {
            text: true,
            timestamp: false,
            color: false,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Emoji {
    /// Convert emoji to and from `:shortcode:` text. Drives the legacy-server
    /// send encode and the always-on receive decode.
    pub shortcodes: bool,
    /// The inline `:prefix` suggestion popup.
    pub typeahead: bool,
}

impl Default for Emoji {
    fn default() -> Self {
        Emoji {
            shortcodes: true,
            typeahead: true,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Chat {
    pub font: String,
    pub word_wrap: bool,
    pub scrollback_lines: u32,
    pub timestamp: bool,
    /// `strftime(3)` format for the per-line timestamp column.
    pub timestamp_format: String,
    /// Speaker avatars in the chat gutter — see [`Users::animate_avatars`] for
    /// the user list, which is a separate toggle.
    pub avatars: bool,
    /// Render markdown in incoming messages. Sending is unaffected: markdown
    /// goes out literally, because the wire format has no styling.
    pub markdown: bool,
    pub show_joins: bool,
    /// How many chat-history entries to pull on the initial post-login fetch
    /// and on each subsequent "Load older messages". Zero disables the initial
    /// pull; the affordance still works once engaged manually.
    pub history_initial: u32,
    /// Words that highlight a chat line, in addition to your own nickname,
    /// which always matches. An array rather than the comma-separated string
    /// the `HIGHLIGHTWORDS` key held, which deletes the splitting change hook.
    pub highlight_words: Vec<String>,
    pub legacy_nick_completion: bool,
    pub autocopy: AutoCopy,
    pub emoji: Emoji,
}

impl Default for Chat {
    fn default() -> Self {
        Chat {
            font: "Monospace 10".into(),
            word_wrap: false,
            scrollback_lines: 500,
            timestamp: false,
            timestamp_format: "[%H:%M:%S] ".into(),
            avatars: true,
            markdown: true,
            show_joins: true,
            history_initial: 50,
            highlight_words: Vec::new(),
            legacy_nick_completion: false,
            autocopy: AutoCopy::default(),
            emoji: Emoji::default(),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Users {
    /// Animate GIF avatars in the user list. When off they render as the still
    /// first frame. The per-user pause is a separate, transient override.
    pub animate_avatars: bool,
}

impl Default for Users {
    fn default() -> Self {
        Users {
            animate_avatars: true,
        }
    }
}

/// Per-event desktop notification toggles.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Notify {
    pub chat: bool,
    pub chat_highlight: bool,
    pub private_message: bool,
    pub private_chat: bool,
    pub private_chat_highlight: bool,
    pub private_chat_invite: bool,
    pub news: bool,
    pub transfer: bool,
    pub broadcast: bool,
    /// Only notify when the relevant window doesn't already have focus.
    pub omit_focused: bool,
}

impl Default for Notify {
    fn default() -> Self {
        // The high-signal events (mentions, private messages, invites) are on;
        // the noisy ones (every chat line, every news post) are off, so a fresh
        // install doesn't immediately spam.
        Notify {
            chat: false,
            chat_highlight: true,
            private_message: true,
            private_chat: true,
            private_chat_highlight: true,
            private_chat_invite: true,
            news: false,
            transfer: true,
            broadcast: true,
            omit_focused: true,
        }
    }
}

/// Per-event sound toggles, gated as a group by [`Sound::enabled`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Sound {
    pub enabled: bool,
    pub chat: bool,
    pub error: bool,
    pub transfer: bool,
    pub invite: bool,
    pub join: bool,
    pub leave: bool,
    pub login: bool,
    pub private_message: bool,
    pub news: bool,
    /// Kept unconditionally, including in a build without voice support, so
    /// that build doesn't discard a user's saved toggles.
    pub voice_join: bool,
    pub voice_leave: bool,
}

impl Default for Sound {
    fn default() -> Self {
        // Every individual event on, the master switch off: turning sound on
        // should do something audible without a second trip through the page.
        Sound {
            enabled: false,
            chat: true,
            error: true,
            transfer: true,
            invite: true,
            join: true,
            leave: true,
            login: true,
            private_message: true,
            news: true,
            voice_join: true,
            voice_leave: true,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Transfers {
    pub download_dir: String,
    /// Queue downloads rather than running them all at once.
    pub queue: bool,
}

impl Default for Transfers {
    fn default() -> Self {
        Transfers {
            download_dir: ".".into(),
            queue: true,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Trackers {
    /// An array rather than the comma-separated string the `TRACKER` key held,
    /// which deletes both the splitting change hook and the derived pointer
    /// array that lived beside it.
    pub addresses: Vec<String>,
    pub case_sensitive: bool,
}

impl Default for Trackers {
    fn default() -> Self {
        Trackers {
            addresses: vec!["hltracker.com".into()],
            case_sensitive: true,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Voice {
    /// A stable device name. Empty means "system default", which the runtime
    /// resolves through `autoaudiosrc` / `autoaudiosink`.
    pub input_device: String,
    pub output_device: String,
    pub ptt_enabled: bool,
    /// Canonical key name, e.g. `Pause`, `F8`, `<Control>F12`. Empty means the
    /// user turned push-to-talk on but hasn't picked a key yet.
    pub ptt_key: String,
}

/// The toolbar window's outer size. Everything *inside* the window — the split
/// tree, divider positions, undocked panel sizes — belongs to the dock layout
/// file, and that split is deliberate.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Window {
    pub toolbar_width: u32,
    pub toolbar_height: u32,
}

impl Default for Window {
    fn default() -> Self {
        // What the toolbar has always fallen back to when no size was saved.
        // The C read-back treats zero as "unset"; here the default *is* the
        // fallback, so the sentinel goes away.
        Window {
            toolbar_width: 1100,
            toolbar_height: 700,
        }
    }
}

/// Everything in `gtkhx.toml`.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Settings {
    pub identity: Identity,
    pub appearance: Appearance,
    pub chat: Chat,
    pub users: Users,
    pub notify: Notify,
    pub sound: Sound,
    pub transfers: Transfers,
    pub trackers: Trackers,
    pub voice: Voice,
    pub window: Window,
}
