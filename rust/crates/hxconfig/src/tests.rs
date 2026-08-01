//! Everything here runs headless against a temp directory. No display, no
//! glib, no C.
//!
//! The tests are chosen to pin the defects that motivated the rewrite, not
//! just to exercise the happy path: the old system had no round-trip test at
//! all, which is exactly what let its escape asymmetry survive.

use super::*;
use std::path::{Path, PathBuf};

/// A temp directory that removes itself. Small enough not to be worth a
/// dev-dependency, and it keeps the crate's dependency list at one entry.
struct TempDir(PathBuf);

impl TempDir {
    fn new(tag: &str) -> TempDir {
        use std::sync::atomic::{AtomicU32, Ordering};
        static SEQ: AtomicU32 = AtomicU32::new(0);
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or(0);
        let dir = std::env::temp_dir().join(format!(
            "hxconfig-{tag}-{}-{}-{}",
            std::process::id(),
            nanos,
            SEQ.fetch_add(1, Ordering::Relaxed)
        ));
        std::fs::create_dir_all(&dir).expect("create temp dir");
        TempDir(dir)
    }

    fn path(&self) -> &Path {
        &self.0
    }

    fn write(&self, contents: &str) {
        std::fs::write(path(&self.0), contents).expect("seed settings file");
    }

    fn read(&self) -> String {
        std::fs::read_to_string(path(&self.0)).expect("read settings file")
    }

    /// Every entry in the directory, sorted — used to assert the atomic write
    /// leaves no temp files behind.
    fn entries(&self) -> Vec<String> {
        let mut names: Vec<String> = std::fs::read_dir(&self.0)
            .expect("read temp dir")
            .map(|e| {
                e.expect("dir entry")
                    .file_name()
                    .to_string_lossy()
                    .into_owned()
            })
            .collect();
        names.sort();
        names
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

// ------------------------------------------------------------- the basics --

#[test]
fn missing_file_yields_defaults() {
    let dir = TempDir::new("missing");
    let config = Config::load(dir.path());

    assert_eq!(*config.provenance(), Provenance::Fresh);
    assert!(config.warnings().is_empty());
    assert_eq!(*config.settings(), Settings::default());
    assert!(config.can_save());
}

#[test]
fn defaults_match_the_shipped_c_defaults() {
    // Spot-checks against `options.c` / `init_variables` / `sound.c`. If one of
    // these changes on the C side without changing here, a fresh profile would
    // behave differently before and after the flip.
    let s = Settings::default();
    assert_eq!(s.chat.font, "Monospace 10");
    assert_eq!(s.chat.scrollback_lines, 500);
    assert_eq!(s.chat.timestamp_format, "[%H:%M:%S] ");
    assert_eq!(s.chat.history_initial, 50);
    assert!(s.chat.markdown);
    assert!(s.chat.avatars);
    assert!(!s.chat.word_wrap);
    assert!(s.chat.autocopy.text);
    assert!(!s.chat.autocopy.timestamp);
    assert_eq!(s.identity.nick_color, NICK_COLOR_NONE);
    // The C startup path stamps 500 onto a fresh connection before prefs are
    // read; 0 is a real blank icon, not an "unset" sentinel.
    assert_eq!(s.identity.icon, 500);
    // The nickname is the one field whose C default is environmental ($USER),
    // so the schema leaves it empty and the startup path fills it in.
    assert_eq!(s.identity.nick, "");
    assert_eq!(s.window.toolbar_width, 1100);
    assert_eq!(s.window.toolbar_height, 700);
    assert_eq!(s.appearance.color_scheme, ColorScheme::System);
    assert!(s.appearance.tray);
    assert_eq!(s.transfers.download_dir, ".");
    assert!(s.transfers.queue);
    assert_eq!(s.trackers.addresses, vec!["hltracker.com".to_string()]);
    assert!(s.trackers.case_sensitive);
    // The high-signal notifications on, the noisy ones off.
    assert!(s.notify.chat_highlight);
    assert!(s.notify.private_message);
    assert!(!s.notify.chat);
    assert!(!s.notify.news);
    // Master sound switch off, every individual event on.
    assert!(!s.sound.enabled);
    assert!(s.sound.chat && s.sound.voice_leave);
}

#[test]
fn round_trip_preserves_every_field() {
    let dir = TempDir::new("roundtrip");

    let mut written = Config::defaults();
    let s = written.settings_mut();
    s.identity.nick = "Misha".into();
    s.identity.icon = 500;
    s.identity.nick_color = 0x00_66_cc_ff;
    s.appearance.color_scheme = ColorScheme::Dark;
    s.appearance.theme = "midnight".into();
    s.appearance.tray = false;
    s.chat.font = "Cantarell 12".into();
    s.chat.word_wrap = true;
    s.chat.scrollback_lines = 4096;
    s.chat.timestamp = true;
    s.chat.timestamp_format = "<%H%M> ".into();
    s.chat.avatars = false;
    s.chat.markdown = false;
    s.chat.show_joins = false;
    s.chat.history_initial = 0;
    s.chat.highlight_words = vec!["gtkhx".into(), "hotline".into()];
    s.chat.legacy_nick_completion = true;
    s.chat.autocopy.timestamp = true;
    s.chat.emoji.typeahead = false;
    s.users.animate_avatars = false;
    s.notify.chat = true;
    s.notify.omit_focused = false;
    s.sound.enabled = true;
    s.sound.leave = false;
    s.transfers.download_dir = "/home/misha/Downloads".into();
    s.transfers.queue = false;
    s.trackers.addresses = vec!["hltracker.com".into(), "tracker.preterhuman.net".into()];
    s.trackers.case_sensitive = false;
    s.voice.input_device = "alsa_input.pci-0000_00_1f.3".into();
    s.voice.ptt_enabled = true;
    s.voice.ptt_key = "<Control>F12".into();
    s.window.toolbar_width = 1440;
    s.window.toolbar_height = 900;
    let expected = written.settings().clone();

    written.save(dir.path()).expect("save");

    let reloaded = Config::load(dir.path());
    assert!(reloaded.warnings().is_empty(), "{:?}", reloaded.warnings());
    assert_eq!(*reloaded.provenance(), Provenance::Current);
    assert_eq!(*reloaded.settings(), expected);
}

#[test]
fn a_fresh_file_declares_the_current_version_first() {
    let dir = TempDir::new("version-first");
    Config::defaults().save(dir.path()).expect("save");

    let text = dir.read();
    let first = text.lines().find(|l| !l.trim().is_empty()).unwrap_or("");
    assert_eq!(first.trim(), "version = 1");

    // And it parses back, which is the real assertion: TOML requires a
    // top-level scalar before any table header.
    let reparsed = Config::load(dir.path());
    assert_eq!(*reparsed.provenance(), Provenance::Current);
}

// --------------------------------------------- the defects being fixed --

#[test]
fn unknown_keys_and_comments_survive_a_save() {
    // The old writer built a fresh GKeyFile from its table, so an unknown key
    // was dropped on the next save and so were the user's comments — despite
    // the loader passing G_KEY_FILE_KEEP_COMMENTS.
    let dir = TempDir::new("unknown-keys");
    dir.write(
        "\
version = 1

# I set this myself and I would like to keep it.
[chat]
font = \"Cantarell 12\"
some_future_key = 42

[an_entirely_unknown_table]
hello = \"world\"
",
    );

    let mut config = Config::load(dir.path());
    config.settings_mut().chat.timestamp = true;
    config.save(dir.path()).expect("save");

    let text = dir.read();
    assert!(text.contains("some_future_key = 42"), "{text}");
    assert!(text.contains("[an_entirely_unknown_table]"), "{text}");
    assert!(text.contains("hello = \"world\""), "{text}");
    assert!(
        text.contains("# I set this myself and I would like to keep it."),
        "{text}"
    );
    assert!(text.contains("timestamp = true"), "{text}");
}

#[test]
fn a_comment_beside_a_changed_value_survives() {
    let dir = TempDir::new("inline-comment");
    dir.write(
        "\
version = 1

[chat]
scrollback_lines = 500 # as many as my laptop will take
",
    );

    let mut config = Config::load(dir.path());
    config.settings_mut().chat.scrollback_lines = 4000;
    config.save(dir.path()).expect("save");

    let text = dir.read();
    assert!(text.contains("scrollback_lines = 4000"), "{text}");
    assert!(text.contains("# as many as my laptop will take"), "{text}");
}

#[test]
fn backslashes_do_not_grow_across_save_cycles() {
    // The old writer escaped through g_key_file_set_string and the reader did
    // not unescape through g_key_file_get_value, so a value containing a
    // backslash gained an escape level per cycle. The realistic victim was a
    // Windows download path.
    let dir = TempDir::new("backslash");
    let windows_path = r"C:\Users\Misha\Downloads";

    let mut config = Config::defaults();
    config.settings_mut().transfers.download_dir = windows_path.into();
    config.save(dir.path()).expect("first save");

    for _ in 0..5 {
        let mut config = Config::load(dir.path());
        assert_eq!(
            config.settings().transfers.download_dir,
            windows_path,
            "value drifted; file was:\n{}",
            dir.read()
        );
        config.save(dir.path()).expect("resave");
    }
}

#[test]
fn a_malformed_number_is_diagnosed_not_silently_zeroed() {
    // atoi turned every unparsable INT / UINT16 into 0 with no diagnostic.
    let dir = TempDir::new("malformed-number");
    dir.write(
        "\
version = 1

[chat]
scrollback_lines = \"lots\"
",
    );

    let config = Config::load(dir.path());
    assert_eq!(
        config.settings().chat.scrollback_lines,
        Settings::default().chat.scrollback_lines,
        "should keep the default, not become zero"
    );
    let w = config
        .warnings()
        .iter()
        .find(|w| w.path == "chat.scrollback_lines")
        .expect("a warning naming the key");
    assert!(matches!(
        w.kind,
        WarningKind::WrongType {
            expected: "integer",
            found: "string"
        }
    ));
}

#[test]
fn an_out_of_range_number_is_diagnosed() {
    let dir = TempDir::new("out-of-range");
    dir.write(
        "\
version = 1

[identity]
icon = 70000

[chat]
scrollback_lines = -1
",
    );

    let config = Config::load(dir.path());
    let s = Settings::default();
    assert_eq!(config.settings().identity.icon, s.identity.icon);
    assert_eq!(
        config.settings().chat.scrollback_lines,
        s.chat.scrollback_lines
    );

    let paths: Vec<&str> = config.warnings().iter().map(|w| w.path.as_str()).collect();
    assert!(paths.contains(&"identity.icon"), "{paths:?}");
    assert!(paths.contains(&"chat.scrollback_lines"), "{paths:?}");
    assert!(config
        .warnings()
        .iter()
        .all(|w| matches!(w.kind, WarningKind::OutOfRange { .. })));
}

#[test]
fn a_scalar_where_a_table_belongs_is_diagnosed_once_and_survives_a_save() {
    // A hand-edited `chat = 5` is valid TOML, so it loads. Every key under it
    // then resolves to nothing, which is indistinguishable from the keys being
    // absent — a whole page of settings reverting with nothing said. And
    // toml_edit's IndexMut panics on a scalar, so the save used to abort the
    // client outright, after the user had already done work.
    for bad in ["chat = 5\n", "chat = \"off\"\n", "[[chat]]\nfont = \"x\"\n"] {
        let dir = TempDir::new("scalar-table");
        dir.write(&format!("version = 1\n{bad}"));

        let mut config = Config::load(dir.path());
        let complaints: Vec<&Warning> = config
            .warnings()
            .iter()
            .filter(|w| w.path == "chat")
            .collect();
        assert_eq!(
            complaints.len(),
            1,
            "want exactly one for {bad:?}, not one per key beneath it"
        );
        assert!(matches!(
            complaints[0].kind,
            WarningKind::WrongType {
                expected: "table",
                ..
            }
        ));
        assert_eq!(config.settings().chat, Chat::default());

        config.save(dir.path()).expect("save must not panic");
        let reloaded = Config::load(dir.path());
        assert!(reloaded.warnings().is_empty(), "{:?}", reloaded.warnings());
        assert_eq!(reloaded.settings().chat, Chat::default());
    }
}

#[test]
fn a_zero_window_dimension_is_rejected() {
    // Zero was the toolbar's "no size saved" sentinel and the C struct's
    // static default, so a migrated zero is likely. Honouring it literally
    // would open a 0x0 window.
    let dir = TempDir::new("zero-extent");
    dir.write("version = 1\n[window]\ntoolbar_width = 0\ntoolbar_height = 600\n");

    let config = Config::load(dir.path());
    assert_eq!(
        config.settings().window.toolbar_width,
        Settings::default().window.toolbar_width
    );
    assert_eq!(config.settings().window.toolbar_height, 600);
    assert_eq!(config.warnings()[0].path, "window.toolbar_width");
}

#[test]
fn an_unknown_enum_value_is_diagnosed() {
    let dir = TempDir::new("bad-enum");
    dir.write("version = 1\n[appearance]\ncolor_scheme = \"sepia\"\n");

    let config = Config::load(dir.path());
    assert_eq!(
        config.settings().appearance.color_scheme,
        ColorScheme::System
    );
    let w = &config.warnings()[0];
    assert_eq!(w.path, "appearance.color_scheme");
    assert!(matches!(w.kind, WarningKind::UnknownValue { .. }));
    assert!(w.to_string().contains("system, light, dark"));
}

#[test]
fn one_bad_list_element_does_not_cost_the_others() {
    let dir = TempDir::new("bad-list-element");
    dir.write("version = 1\n[trackers]\naddresses = [\"a.example\", 7, \"b.example\"]\n");

    let config = Config::load(dir.path());
    assert_eq!(
        config.settings().trackers.addresses,
        vec!["a.example".to_string(), "b.example".to_string()]
    );
    assert_eq!(config.warnings()[0].path, "trackers.addresses[1]");
}

#[test]
fn an_omitted_key_keeps_its_default_rather_than_becoming_zero() {
    let dir = TempDir::new("omitted");
    dir.write("version = 1\n[chat]\ntimestamp = true\n");

    let config = Config::load(dir.path());
    assert!(config.warnings().is_empty());
    assert!(config.settings().chat.timestamp);
    assert_eq!(config.settings().chat.font, Settings::default().chat.font);
    assert_eq!(
        config.settings().trackers.addresses,
        Settings::default().trackers.addresses
    );
}

// ---------------------------------------------------------- corrupt files --

#[test]
fn an_unparsable_file_falls_back_to_defaults_and_is_kept() {
    // Settings are reconstructible, so a corrupt file degrades to defaults and
    // the client carries on — the opposite of the connection file's policy,
    // which refuses to save so one typo can't empty the server list. But the
    // bad file is still worth keeping.
    let dir = TempDir::new("unparsable");
    dir.write("version = 1\n[chat\nfont = broken\n");

    let mut config = Config::load(dir.path());
    assert_eq!(*config.provenance(), Provenance::Unusable);
    assert_eq!(*config.settings(), Settings::default());
    assert!(matches!(
        config.warnings()[0].kind,
        WarningKind::Unparsable(_)
    ));

    assert!(config.can_save());
    config.save(dir.path()).expect("save over a corrupt file");

    let salvaged = std::fs::read_to_string(dir.path().join(format!("{FILE_NAME}{SALVAGE_SUFFIX}")))
        .expect("the corrupt file is kept beside the new one");
    assert!(salvaged.contains("[chat\n"));

    let reloaded = Config::load(dir.path());
    assert_eq!(*reloaded.provenance(), Provenance::Current);
}

#[test]
fn saving_twice_does_not_re_salvage() {
    let dir = TempDir::new("resalvage");
    dir.write("nonsense = = =\n");

    let mut config = Config::load(dir.path());
    config.save(dir.path()).expect("first save");
    config.settings_mut().chat.timestamp = true;
    config.save(dir.path()).expect("second save");

    // The salvage file still holds the original, not the first save's output.
    let salvaged = std::fs::read_to_string(dir.path().join(format!("{FILE_NAME}{SALVAGE_SUFFIX}")))
        .expect("salvage file");
    assert!(salvaged.contains("nonsense"));
    assert!(dir.read().contains("timestamp = true"));
}

// ------------------------------------------------------------ versioning --

#[test]
fn a_current_version_file_loads_and_saves() {
    let dir = TempDir::new("v-current");
    dir.write("version = 1\n[chat]\nmarkdown = false\n");

    let mut config = Config::load(dir.path());
    assert_eq!(*config.provenance(), Provenance::Current);
    assert!(!config.settings().chat.markdown);
    config.save(dir.path()).expect("save");
    assert!(dir.read().contains("version = 1"));
}

#[test]
fn a_newer_version_file_loads_read_only_until_acknowledged() {
    // Silently rewriting a newer file at an older schema is how someone who
    // tried a newer build loses their settings when they go back. The bookmark
    // store's version field is not checked at all; this one is.
    let dir = TempDir::new("v-newer");
    let original = "version = 99\n[chat]\nmarkdown = false\nfuture_key = \"kept\"\n";
    dir.write(original);

    let mut config = Config::load(dir.path());
    assert_eq!(*config.provenance(), Provenance::Newer { found: 99 });
    // What parses is still loaded, so the client is usable.
    assert!(!config.settings().chat.markdown);
    assert!(config
        .warnings()
        .iter()
        .any(|w| matches!(w.kind, WarningKind::NewerVersion { found: 99, .. })));

    assert!(!config.can_save());
    assert!(matches!(
        config.save(dir.path()),
        Err(SaveError::NewerSchema)
    ));
    assert_eq!(dir.read(), original, "the file must be untouched");

    config.acknowledge_newer();
    assert!(config.can_save());
    config.save(dir.path()).expect("save after acknowledging");

    let text = dir.read();
    assert!(text.contains("version = 1"), "{text}");
    assert!(text.contains("future_key = \"kept\""), "{text}");
    assert_eq!(*config.provenance(), Provenance::Current);
}

#[test]
fn the_two_bad_version_cases_resolve_differently() {
    // Asserted against `read_version` directly rather than through
    // `Provenance`, because CURRENT_VERSION is 1 today and both answers land
    // on `Current`. The distinction only becomes observable at the first
    // schema bump — which is exactly the moment getting it wrong would cost
    // someone their settings, and far too late to discover the policy was
    // never pinned.
    let resolve = |s: &str| -> (u32, Vec<Warning>) {
        let doc: toml_edit::DocumentMut = s.parse().expect("valid TOML");
        let mut warnings = Vec::new();
        let version = read_version(&doc, &mut warnings);
        (version, warnings)
    };

    // Missing: the file predates the key, so it is the first schema and has to
    // go through the migration chain. Not a defect, so not a warning.
    let (version, warnings) = resolve("[chat]\nmarkdown = false\n");
    assert_eq!(version, 1);
    assert!(
        warnings.is_empty(),
        "a missing version is not a defect: {warnings:?}"
    );

    // Malformed: someone mistyped the version of a file that is otherwise this
    // build's, and migrating it would do more damage than leaving it alone.
    for bad in ["version = \"one\"\n", "version = 0\n", "version = -3\n"] {
        let (version, warnings) = resolve(bad);
        assert_eq!(version, CURRENT_VERSION, "for {bad:?}");
        assert_eq!(warnings.len(), 1, "for {bad:?}");
        assert_eq!(warnings[0].path, "version");
        assert!(
            matches!(warnings[0].kind, WarningKind::BadVersion(_)),
            "for {bad:?}"
        );
    }
}

#[test]
fn a_missing_or_bogus_version_still_loads() {
    // The observable half of the above, at today's CURRENT_VERSION.
    let dir = TempDir::new("v-missing");

    dir.write("[chat]\nmarkdown = false\n");
    let config = Config::load(dir.path());
    assert_eq!(*config.provenance(), Provenance::Current);
    assert!(config.warnings().is_empty());
    assert!(!config.settings().chat.markdown);

    dir.write("version = \"one\"\n[chat]\nmarkdown = false\n");
    let config = Config::load(dir.path());
    assert_eq!(*config.provenance(), Provenance::Current);
    assert_eq!(config.warnings()[0].path, "version");
    assert!(matches!(
        config.warnings()[0].kind,
        WarningKind::BadVersion(_)
    ));
    assert!(!config.settings().chat.markdown);
}

#[test]
fn saving_stamps_the_version_onto_a_file_that_lacked_one() {
    let dir = TempDir::new("v-stamp");
    dir.write("[chat]\nmarkdown = false\n");

    let mut config = Config::load(dir.path());
    config.save(dir.path()).expect("save");

    let text = dir.read();
    assert!(text.starts_with("version = 1"), "{text}");
    assert!(text.contains("markdown = false"), "{text}");
}

// ---------------------------------------------------------------- writing --

#[test]
fn the_write_leaves_no_temp_files_behind() {
    let dir = TempDir::new("no-temps");
    let mut config = Config::defaults();
    config.save(dir.path()).expect("save");
    config.save(dir.path()).expect("save again");

    assert_eq!(dir.entries(), vec![FILE_NAME.to_string()]);
}

#[cfg(unix)]
#[test]
fn the_file_is_written_owner_only() {
    use std::os::unix::fs::PermissionsExt;

    let dir = TempDir::new("perms");
    Config::defaults().save(dir.path()).expect("save");

    let mode = std::fs::metadata(path(dir.path()))
        .expect("stat")
        .permissions()
        .mode();
    assert_eq!(mode & 0o777, 0o600, "got {:o}", mode & 0o777);
}

#[test]
fn saving_into_a_missing_directory_reports_rather_than_panics() {
    let dir = TempDir::new("no-dir");
    let missing = dir.path().join("does-not-exist");
    assert!(matches!(
        Config::defaults().save(&missing),
        Err(SaveError::Io(_))
    ));
}

#[test]
fn a_fresh_file_uses_table_headers_not_inline_tables() {
    // toml_edit's indexing auto-creates *inline* tables, which round-trip
    // perfectly and read terribly — the whole of [chat] lands on one line. A
    // file people are meant to hand-edit gets real headers.
    let text = Config::defaults().to_toml();
    for header in [
        "[identity]",
        "[appearance]",
        "[chat]",
        "[chat.autocopy]",
        "[chat.emoji]",
        "[users]",
        "[notify]",
        "[sound]",
        "[transfers]",
        "[trackers]",
        "[voice]",
        "[window]",
    ] {
        assert!(text.contains(header), "missing {header} in:\n{text}");
    }
    assert!(!text.contains(" = { "), "an inline table crept in:\n{text}");
}

#[test]
fn an_inline_table_the_user_wrote_is_left_alone() {
    let dir = TempDir::new("inline-kept");
    dir.write("version = 1\nchat = { font = \"Cantarell 12\", markdown = false }\n");

    let mut config = Config::load(dir.path());
    assert_eq!(config.settings().chat.font, "Cantarell 12");
    assert!(!config.settings().chat.markdown);

    config.settings_mut().chat.timestamp = true;
    config.save(dir.path()).expect("save");

    let reloaded = Config::load(dir.path());
    assert!(reloaded.warnings().is_empty(), "{:?}", reloaded.warnings());
    assert_eq!(reloaded.settings().chat.font, "Cantarell 12");
    assert!(reloaded.settings().chat.timestamp);
    assert!(!reloaded.settings().chat.markdown);
}

#[test]
fn hand_written_literal_forms_are_read_correctly_then_normalised() {
    // TOML has more ways to spell a value than the writer emits, and a
    // rewritten line comes back in the writer's spelling. That is a real
    // change to the file, so it is worth pinning what does and doesn't move:
    // the *value* must survive, the *spelling* need not.
    let dir = TempDir::new("literal-forms");
    dir.write(
        "\
version = 1

[chat]
font = 'Cantarell 12'
scrollback_lines = 1_000

[identity]
icon = 0x1f4
",
    );

    let mut config = Config::load(dir.path());
    assert!(config.warnings().is_empty(), "{:?}", config.warnings());
    assert_eq!(config.settings().chat.font, "Cantarell 12");
    assert_eq!(config.settings().chat.scrollback_lines, 1000);
    assert_eq!(config.settings().identity.icon, 500);

    // Untouched, each keeps the form the user wrote.
    config.save(dir.path()).expect("save");
    let text = dir.read();
    assert!(text.contains("'Cantarell 12'"), "{text}");
    assert!(text.contains("1_000"), "{text}");
    assert!(text.contains("0x1f4"), "{text}");

    // Changed, they come back in the writer's spelling — and still round-trip.
    let mut config = Config::load(dir.path());
    config.settings_mut().chat.scrollback_lines = 2000;
    config.save(dir.path()).expect("save");
    assert_eq!(
        Config::load(dir.path()).settings().chat.scrollback_lines,
        2000
    );
}

#[test]
fn an_untouched_array_keeps_its_layout_and_element_comments() {
    // Rebuilding the array unconditionally would reflow a hand-written
    // multi-line one onto a single line and drop its per-element comments — on
    // a save that never touched the list. The two array fields are the ones a
    // user is most likely to have formatted by hand.
    let dir = TempDir::new("array-layout");
    let original = "\
version = 1

[trackers]
addresses = [
    \"a.example\", # the one that actually answers
    \"b.example\",
]
";
    dir.write(original);

    let mut config = Config::load(dir.path());
    config.settings_mut().chat.timestamp = true;
    config.save(dir.path()).expect("save");

    let text = dir.read();
    assert!(text.contains("# the one that actually answers"), "{text}");
    assert!(text.contains("    \"a.example\","), "reflowed:\n{text}");

    // Changing the list, of course, does rewrite it.
    let mut config = Config::load(dir.path());
    config.settings_mut().trackers.addresses = vec!["c.example".into()];
    config.save(dir.path()).expect("save");
    assert!(
        dir.read().contains("addresses = [\"c.example\"]"),
        "{}",
        dir.read()
    );
}

#[test]
fn to_toml_does_not_mutate_the_config() {
    let mut config = Config::defaults();
    config.settings_mut().chat.timestamp = true;
    let once = config.to_toml();
    let twice = config.to_toml();
    assert_eq!(once, twice);
    assert!(once.contains("timestamp = true"));
}

#[test]
fn an_untouched_file_is_rewritten_byte_for_byte() {
    // Load, change nothing, save: nothing about the file should move. This is
    // what makes "the client rewrote my config" a non-event.
    let dir = TempDir::new("byte-identical");
    let mut config = Config::defaults();
    config.settings_mut().chat.highlight_words = vec!["gtkhx".into()];
    config.save(dir.path()).expect("seed");
    let before = dir.read();

    let mut reloaded = Config::load(dir.path());
    reloaded.save(dir.path()).expect("resave");
    assert_eq!(dir.read(), before);
}

// ------------------------------------------------------------ the schema --

#[test]
fn every_path_is_unique_and_well_formed() {
    let mut seen = std::collections::BTreeSet::new();
    for p in PATHS {
        assert!(seen.insert(*p), "duplicate path {p}");
        assert!(!p.is_empty());
        assert!(!p.starts_with('.') && !p.ends_with('.'), "{p}");
        assert!(
            p.split('.').all(|s| !s.is_empty()
                && s.chars()
                    .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '_')),
            "{p} is not snake_case dotted"
        );
        assert_ne!(*p, "version", "version is handled outside the field table");
    }
}

#[test]
fn every_path_round_trips_a_non_default_value() {
    // A field that the writer emits but the reader ignores (or vice versa)
    // would silently lose a user's setting. Rather than trust the macro,
    // perturb every field, write, read back, and require the whole struct to
    // survive — which fails if any single path is one-directional.
    let dir = TempDir::new("all-paths");

    let mut config = Config::defaults();
    let s = config.settings_mut();
    // Flip every boolean away from its default and give every other field a
    // value nothing defaults to.
    for flag in [
        &mut s.appearance.tray,
        &mut s.chat.word_wrap,
        &mut s.chat.timestamp,
        &mut s.chat.avatars,
        &mut s.chat.markdown,
        &mut s.chat.show_joins,
        &mut s.chat.legacy_nick_completion,
        &mut s.chat.autocopy.text,
        &mut s.chat.autocopy.timestamp,
        &mut s.chat.autocopy.color,
        &mut s.chat.emoji.shortcodes,
        &mut s.chat.emoji.typeahead,
        &mut s.users.animate_avatars,
        &mut s.notify.chat,
        &mut s.notify.chat_highlight,
        &mut s.notify.private_message,
        &mut s.notify.private_chat,
        &mut s.notify.private_chat_highlight,
        &mut s.notify.private_chat_invite,
        &mut s.notify.news,
        &mut s.notify.transfer,
        &mut s.notify.broadcast,
        &mut s.notify.omit_focused,
        &mut s.sound.enabled,
        &mut s.sound.chat,
        &mut s.sound.error,
        &mut s.sound.transfer,
        &mut s.sound.invite,
        &mut s.sound.join,
        &mut s.sound.leave,
        &mut s.sound.login,
        &mut s.sound.private_message,
        &mut s.sound.news,
        &mut s.sound.voice_join,
        &mut s.sound.voice_leave,
        &mut s.transfers.queue,
        &mut s.trackers.case_sensitive,
        &mut s.voice.ptt_enabled,
    ] {
        *flag = !*flag;
    }
    s.identity.nick = "nick!".into();
    s.identity.icon = 4242;
    s.identity.nick_color = 0x0012_3456;
    s.appearance.color_scheme = ColorScheme::Light;
    s.appearance.theme = "theme!".into();
    s.chat.font = "font!".into();
    s.chat.scrollback_lines = 1234;
    s.chat.timestamp_format = "fmt!".into();
    s.chat.history_initial = 7;
    s.chat.highlight_words = vec!["one".into(), "two".into()];
    s.transfers.download_dir = "dir!".into();
    s.trackers.addresses = vec!["t1".into(), "t2".into(), "t3".into()];
    s.voice.input_device = "in!".into();
    s.voice.output_device = "out!".into();
    s.voice.ptt_key = "key!".into();
    s.window.toolbar_width = 111;
    s.window.toolbar_height = 222;

    let expected = config.settings().clone();
    assert_ne!(expected, Settings::default(), "the test perturbed nothing");
    config.save(dir.path()).expect("save");

    let reloaded = Config::load(dir.path());
    assert!(reloaded.warnings().is_empty(), "{:?}", reloaded.warnings());
    assert_eq!(*reloaded.settings(), expected);

    // And every path resolves to an actual value in the written file, so a
    // field the writer skipped can't pass by coincidentally matching its own
    // default. Resolved through the parser rather than by substring: leaf
    // names like `chat`, `text`, `news` and `transfer` all occur inside
    // unrelated keys, which would make a `contains` check nearly vacuous.
    let doc: toml_edit::DocumentMut = dir.read().parse().expect("reparse");
    for p in PATHS {
        let mut item = doc.as_item();
        for segment in p.split('.') {
            item = item
                .as_table_like()
                .and_then(|t| t.get(segment))
                .unwrap_or_else(|| panic!("{p} never made it into the file"));
        }
        assert!(item.is_value(), "{p} is not a value");
    }
}

// =========================================================== migration ====
//
// The old `gtkhxrc` is read once, on the first run of a build that has this
// crate, and never again. There is no second chance at getting it right, which
// is why these lean on a real captured profile rather than only on cases I
// thought to imagine.

use crate::legacy::{self, Form};
use crate::migrate::{self, Target};

/// A real `gtkhxrc`, captured from a live profile.
const REAL_PROFILE: &str = include_str!("../fixtures/real-profile-gtkhxrc");

/// The C key header, compiled in so the coverage test reads the authoritative
/// list rather than a copy of it. It used to scrape the `cfgvars[]` table out
/// of `options.c` as well; that table is gone, and the header is now the whole
/// of the C side's vocabulary.
const C_CFGKEYS: &str = include_str!("../../../../src/cfgkeys.h");

/// Macros in `cfgkeys.h` that hold a *value* rather than a key: the three
/// spellings `appearance.color_scheme` accepts. Named here because a scrape
/// cannot tell them apart from a key by shape.
const C_VALUE_MACROS: &[&str] = &["CFG_THEME_SYSTEM", "CFG_THEME_LIGHT", "CFG_THEME_DARK"];

/// Every config key the C side spells, read out of `cfgkeys.h`.
///
/// Derived rather than transcribed on purpose. A hand-copied list cannot do the
/// one thing a coverage test is for: a key added on the C side and forgotten in
/// both the map and the copy would pass. This fails.
fn c_config_keys() -> Vec<String> {
    let mut keys = Vec::new();
    for line in C_CFGKEYS.lines() {
        let Some(rest) = line.trim().strip_prefix("#define CFG_") else {
            continue;
        };
        let Some((name, value)) = rest.split_once(char::is_whitespace) else {
            continue;
        };
        if C_VALUE_MACROS.contains(&format!("CFG_{name}").as_str()) {
            continue;
        }
        let Some(literal) = value
            .trim()
            .strip_prefix('"')
            .and_then(|v| v.split('"').next())
        else {
            continue;
        };
        keys.push(literal.to_string());
    }
    keys
}

#[test]
fn the_map_covers_every_key_the_c_side_spells() {
    // A key C can name with no migration target is a setting that would
    // silently vanish, in a one-shot the user never gets to redo — and one the
    // by-name ABI would report as unknown, leaving its Settings row dead.
    let keys = c_config_keys();
    assert!(
        keys.len() > 60,
        "only found {} keys in cfgkeys.h — the parse is wrong, which would \
         make this test pass vacuously: {keys:?}",
        keys.len()
    );

    for key in &keys {
        assert!(
            migrate::target_of(key).is_some(),
            "{key} is in cfgkeys.h but has no migration target"
        );
    }
}

#[test]
fn every_target_is_a_path_the_schema_really_has() {
    // Guards the other direction of the same drift: a typo'd or renamed target
    // would migrate a value into a path nothing reads.
    for (key, target) in migrate::MAP {
        if let Target::Path(path) = target {
            assert!(
                crate::kind_of(path).is_some(),
                "{key} maps to {path}, which the schema does not have"
            );
        }
    }
}

#[test]
fn every_schema_path_has_a_migration_source() {
    // And the third direction: a setting added to the schema with no old key
    // feeding it defaults for everyone who upgrades, which is sometimes right
    // and should always be deliberate. Saying so in NEW_PATHS is the way to
    // be deliberate about it.
    for path in crate::PATHS {
        let mapped = migrate::MAP
            .iter()
            .any(|(_, t)| matches!(t, Target::Path(p) if p == path));
        assert!(
            mapped || migrate::NEW_PATHS.contains(path),
            "{path} has no old key feeding it; add one to MAP, or list it in \
             NEW_PATHS to say the default is intended"
        );
    }
}

#[test]
fn no_duplicate_keys_or_targets_in_the_map() {
    let mut keys = std::collections::BTreeSet::new();
    let mut targets = std::collections::BTreeSet::new();
    for (key, target) in migrate::MAP {
        assert!(keys.insert(*key), "{key} appears twice");
        if let Target::Path(path) = target {
            assert!(targets.insert(*path), "{path} is the target of two keys");
        }
    }
}

#[test]
fn the_real_profile_migrates_whole() {
    let (keys, form) = legacy::parse(REAL_PROFILE);
    assert_eq!(form, Form::KeyFile);

    let mut warnings = Vec::new();
    let doc = migrate::to_document(&keys, form, &mut warnings);
    assert!(warnings.is_empty(), "{warnings:?}");

    let dir = TempDir::new("real-profile");
    std::fs::write(path(dir.path()), doc.to_string()).expect("seed");
    let config = Config::load(dir.path());
    assert!(config.warnings().is_empty(), "{:?}", config.warnings());
    let s = config.settings();

    // Identity, including the two keys that had no storage of their own and
    // aliased the live connection's wire fields.
    assert_eq!(s.identity.nick, "misha");
    assert_eq!(s.identity.icon, 32766);
    assert_eq!(s.identity.nick_color, 12607947);

    assert_eq!(s.appearance.color_scheme, ColorScheme::System);
    assert_eq!(s.appearance.theme, "default");
    assert!(s.appearance.tray);

    assert_eq!(s.chat.font, "Monospace 11");
    assert!(s.chat.word_wrap);
    assert_eq!(s.chat.scrollback_lines, 500);
    assert!(s.chat.timestamp);
    // The trailing space matters and is easy to lose: GKeyFile does not escape
    // a trailing space, but a reader that trimmed would silently jam the
    // timestamp against the message text.
    assert_eq!(s.chat.timestamp_format, "[%H:%M:%S] ");
    assert!(s.chat.avatars);
    assert!(s.chat.markdown);
    assert!(s.chat.show_joins);
    assert_eq!(s.chat.history_initial, 50);
    // HIGHLIGHTWORDS= is empty, which must become an empty list rather than a
    // list holding one empty string.
    assert!(s.chat.highlight_words.is_empty());
    assert!(!s.chat.legacy_nick_completion);
    assert!(s.chat.autocopy.text);
    assert!(s.chat.autocopy.timestamp);
    assert!(!s.chat.autocopy.color);
    assert!(s.chat.emoji.shortcodes && s.chat.emoji.typeahead);

    assert!(s.users.animate_avatars);

    // NOTIFYMSG → notify.private_message and NOTIFYXFER → notify.transfer are
    // the renames most likely to be got wrong.
    assert!(s.notify.private_message);
    assert!(s.notify.transfer);
    assert!(s.notify.broadcast && s.notify.chat && s.notify.news);
    assert!(s.notify.private_chat && s.notify.private_chat_invite);
    assert!(s.notify.omit_focused);

    // SOUNDSON → sound.enabled, SOUNDFILE → sound.transfer,
    // SOUNDPART → sound.leave, SOUNDMSG → sound.private_message.
    assert!(s.sound.enabled);
    assert!(s.sound.transfer && s.sound.leave && s.sound.private_message);
    assert!(s.sound.voice_join && s.sound.voice_leave);

    assert_eq!(s.transfers.download_dir, "/home/misha/downloads/");
    assert!(s.transfers.queue);

    assert_eq!(
        s.trackers.addresses,
        vec![
            "tracker.vespernet.net".to_string(),
            "hlserver.com".to_string(),
            "tracker.preterhuman.net".to_string(),
        ]
    );
    assert!(!s.trackers.case_sensitive);

    assert_eq!(s.voice.input_device, "pipewiredevice66");
    assert_eq!(s.voice.output_device, "");
    assert!(s.voice.ptt_enabled);
    assert_eq!(s.voice.ptt_key, "Pause");

    // TOOLXSIZE / TOOLYSIZE are live and carry over; the other four panels'
    // sizes are dropped.
    assert_eq!(s.window.toolbar_width, 2807);
    assert_eq!(s.window.toolbar_height, 1356);
}

#[test]
fn the_real_profile_drops_exactly_what_it_should() {
    let (keys, form) = legacy::parse(REAL_PROFILE);
    let mut warnings = Vec::new();
    let doc = migrate::to_document(&keys, form, &mut warnings);

    // Assert on the *paths*, not on the numbers: a bare-substring check on
    // "782" passes today only because nothing else happens to contain it, and
    // would fail confusingly the first time something did.
    let resolves = |path: &str| -> bool {
        let mut item = doc.as_item();
        for segment in path.split('.') {
            match item.as_table_like().and_then(|t| t.get(segment)) {
                Some(next) => item = next,
                None => return false,
            }
        }
        item.is_value()
    };

    // Every path the schema has must be present — the profile sets every key.
    for path in crate::PATHS {
        assert!(
            resolves(path),
            "{path} is missing from the migrated document"
        );
    }

    // And the twelve dropped keys must leave nothing behind under any name.
    let text = doc.to_string();
    for gone in [
        "CHATXSIZE",
        "CHATYSIZE",
        "NEWSXSIZE",
        "NEWSYSIZE",
        "TASKXSIZE",
        "TASKYSIZE",
        "USERXSIZE",
        "USERYSIZE",
        "OPENCHAT",
        "OPENNEWS",
        "OPENTASKS",
        "OPENUSERS",
    ] {
        assert!(!text.contains(gone), "{gone} survived:\n{text}");
    }
    // The four panels' sizes differ from the toolbar's, so if one had been
    // mapped to window.* by mistake the toolbar assertions would catch it.
    assert!(text.contains("2807") && text.contains("1356"), "{text}");
}

#[test]
fn escapes_are_undone_the_way_the_old_writer_made_them() {
    // g_key_file_set_string escapes exactly four things — backslash, LF, CR,
    // and *leading* whitespace — and notably not the list separator, commas,
    // '#', '=', brackets, tabs after the first non-space character, or
    // trailing spaces. Anything more aggressive here would corrupt values that
    // were never escaped in the first place.
    let (keys, form) = legacy::parse(
        "[gtkhx]\n\
         DOWNLOAD=C:\\\\Users\\\\Misha\n\
         FONT=\\sLeading Space 10\n\
         THEMENAME=a;b,c#d=e[f]\n\
         VOICEPTTKEY=tab\there\n",
    );
    assert_eq!(form, Form::KeyFile);
    assert_eq!(keys["DOWNLOAD"], r"C:\Users\Misha");
    assert_eq!(keys["FONT"], " Leading Space 10");
    assert_eq!(keys["THEMENAME"], "a;b,c#d=e[f]");
    assert_eq!(keys["VOICEPTTKEY"], "tab\there");
}

#[test]
fn a_value_escaped_more_than_once_is_flagged_rather_than_guessed_at() {
    // Each save/load cycle doubled every backslash, and we can undo exactly
    // one doubling. Handing over a silently-wrong download path is the failure
    // this whole rewrite exists to stop, so say so instead.
    let (keys, form) = legacy::parse("[gtkhx]\nDOWNLOAD=C:\\\\\\\\Users\\\\\\\\Misha\n");
    assert_eq!(keys["DOWNLOAD"], r"C:\\Users\\Misha", "one doubling undone");

    let mut warnings = Vec::new();
    let doc = migrate::to_document(&keys, form, &mut warnings);
    let w = warnings
        .iter()
        .find(|w| w.path == "transfers.download_dir")
        .expect("a warning about the path");
    assert!(matches!(w.kind, WarningKind::OverEscaped { .. }));
    assert!(w.to_string().contains("DOWNLOAD"));

    // The value is still carried across — flagged, not withheld.
    assert!(doc.to_string().contains(r"C:\\Users\\Misha"));

    // And a single escape level produces no warning at all.
    let (keys, form) = legacy::parse("[gtkhx]\nDOWNLOAD=C:\\\\Users\n");
    let mut warnings = Vec::new();
    migrate::to_document(&keys, form, &mut warnings);
    assert!(warnings.is_empty(), "{warnings:?}");
}

#[test]
fn the_boolean_parser_matches_the_c_one_exactly() {
    // Only the first byte is looked at, which is silly and is also what every
    // existing file was written against.
    for (input, want) in [
        ("true", Some(true)),
        ("false", Some(false)),
        ("1", Some(true)),
        ("0", Some(false)),
        ("yes", Some(true)),
        ("no", Some(false)),
        ("T", Some(true)),
        ("N", Some(false)),
        ("tarantino", Some(true)),
        ("nautical", Some(false)),
        ("2", None),
        ("", None),
        (" true", None),
        ("xyz", None),
    ] {
        assert_eq!(legacy::parse_boolean(input), want, "for {input:?}");
    }
}

#[test]
fn an_unusable_boolean_keeps_the_default_and_says_so() {
    let (keys, form) = legacy::parse("[gtkhx]\nMARKDOWN=2\n");
    let mut warnings = Vec::new();
    let doc = migrate::to_document(&keys, form, &mut warnings);

    assert!(!doc.to_string().contains("markdown"), "{doc}");
    let w = &warnings[0];
    assert_eq!(w.path, "chat.markdown");
    assert!(matches!(w.kind, WarningKind::UnmigratableValue { .. }));
    assert!(w.to_string().contains("MARKDOWN"));
}

#[test]
fn a_malformed_number_is_reported_rather_than_becoming_zero() {
    // atoi turned this into 0 with no diagnostic, which for XBUF_MAX meant a
    // scrollback of nothing.
    let (keys, form) = legacy::parse("[gtkhx]\nXBUF_MAX=lots\n");
    let mut warnings = Vec::new();
    let doc = migrate::to_document(&keys, form, &mut warnings);

    assert!(!doc.to_string().contains("scrollback"), "{doc}");
    assert_eq!(warnings[0].path, "chat.scrollback_lines");
    assert!(matches!(
        warnings[0].kind,
        WarningKind::UnmigratableValue { .. }
    ));
}

#[test]
fn an_unknown_old_key_is_reported_but_a_dropped_one_is_not() {
    // A key nothing ever had is worth telling the user about — it is the one
    // case they can act on. The deliberately-dropped ones are not: the user
    // never asked for them and can do nothing about them.
    let (keys, form) =
        legacy::parse("[gtkhx]\nWHAT_IS_THIS=1\nOPENCHAT=true\nCHATXSIZE=782\nFILE_SAMEWINDOW=1\n");
    let mut warnings = Vec::new();
    migrate::to_document(&keys, form, &mut warnings);

    assert_eq!(warnings.len(), 1, "{warnings:?}");
    assert_eq!(warnings[0].path, "WHAT_IS_THIS");
    assert!(matches!(warnings[0].kind, WarningKind::UnknownLegacyKey));
}

#[test]
fn the_pre_keyfile_line_format_still_reads() {
    // No group header, so GKeyFile refused it and the C reader fell through to
    // a line parser. Only reachable via ~/.gtkhxrc now, but a profile nobody
    // has opened in twenty years is exactly the one that needs migrating.
    let (keys, form) = legacy::parse("NICK=bob\nFONT=Monospace 10\nTRAY=0\n");
    assert_eq!(form, Form::Lines);
    assert_eq!(keys["NICK"], "bob");
    assert_eq!(keys["FONT"], "Monospace 10");
    assert_eq!(keys["TRAY"], "0");
}

#[test]
fn the_line_form_keeps_its_comment_convention_but_not_its_fgets_bug() {
    // Two behaviours of the old line parser that look alike and are not.
    //
    // Dropping a final line with no trailing newline is a `fgets` loop
    // mistake with nothing behind it, so reading the line can only recover a
    // setting that was being thrown away.
    let (keys, form) = legacy::parse("NICK=bob\nFONT=Monospace 10");
    assert_eq!(form, Form::Lines);
    assert_eq!(
        keys["FONT"], "Monospace 10",
        "the old parser dropped this line for want of a newline"
    );

    // Truncating at '#' looks like the same class of thing, but '#' was this
    // format's only comment convention, so truncating *is* parsing it
    // correctly. `XBUF_MAX=1000 # lines` means a thousand lines; refusing to
    // truncate would make it an unparseable number and silently fall back to
    // the default, which is the outcome we are trying to avoid.
    let (keys, form) = legacy::parse("XBUF_MAX=1000 # how many\nDOWNLOAD=/home/a # notes\n");
    // Verbatim, trailing space and all — the C parser NUL'd at the '#' and
    // handed the rest to g_strdup. Tolerating that space is the integer
    // conversion's job, not the parser's; trimming here would destroy a
    // trailing space that some values need. See the next test.
    assert_eq!(keys["XBUF_MAX"], "1000 ");
    assert_eq!(keys["DOWNLOAD"], "/home/a ");

    // And it does still parse as a thousand.
    let doc = migrate::to_document(&keys, form, &mut Vec::new());
    assert!(doc.to_string().contains("scrollback_lines = 1000"), "{doc}");

    // And a '#' before any '=' makes the whole line a comment, which is the
    // only reason comment lines work at all.
    let (keys, _) = legacy::parse("# NICK=notme\nNICK=bob\n");
    assert_eq!(keys["NICK"], "bob");
    assert_eq!(keys.len(), 1);

    // None of which applies to the GKeyFile form, which has no inline
    // comments: there, a '#' after the separator really is part of the value.
    let (keys, form) = legacy::parse("[gtkhx]\nDOWNLOAD=/home/a#b/dl\n");
    assert_eq!(form, Form::KeyFile);
    assert_eq!(keys["DOWNLOAD"], "/home/a#b/dl");
}

#[test]
fn keyfile_details_that_the_c_reader_relied_on() {
    let (keys, _) = legacy::parse(
        "# a comment\n\
         [other]\n\
         NICK=wrong\n\
         [gtkhx]\n\
         NICK = spaced\n\
         FONT[de]=localised\n\
         FONT=Monospace 10\n\
         TRAY=1\n\
         TRAY=0\n\
         \n\
         [gtkhx]\n\
         THEMENAME=merged\n",
    );
    // Keys outside [gtkhx] are invisible; whitespace around '=' is trimmed;
    // locale variants are discarded (the load flags don't keep translations);
    // a repeated key resolves to the last one; a repeated group merges.
    assert_eq!(keys["NICK"], "spaced");
    assert_eq!(keys["FONT"], "Monospace 10");
    assert_eq!(keys["TRAY"], "0");
    assert_eq!(keys["THEMENAME"], "merged");
    assert_eq!(keys.len(), 4);
}

#[test]
fn a_file_with_no_gtkhx_group_falls_through_to_the_line_parser() {
    // Which is what the C reader does when g_key_file_get_keys returns NULL.
    let (keys, form) = legacy::parse("[somethingelse]\nNICK=bob\n");
    assert_eq!(form, Form::Lines);
    assert_eq!(keys["NICK"], "bob");
}

// ------------------------------------------------------- the import path --

#[test]
fn an_old_profile_is_imported_when_there_is_no_new_file() {
    let dir = TempDir::new("import");
    let old = dir.path().join("gtkhxrc");
    std::fs::write(&old, REAL_PROFILE).expect("seed");

    let mut config = Config::load_or_migrate(dir.path(), std::slice::from_ref(&old));
    assert_eq!(
        *config.provenance(),
        Provenance::Imported {
            form: Form::KeyFile
        }
    );
    assert_eq!(config.settings().identity.nick, "misha");

    // Importing does not write, so a first run that falls over doesn't leave a
    // half-migrated file behind.
    assert!(!path(dir.path()).exists());

    config.save(dir.path()).expect("save");
    assert_eq!(Config::load(dir.path()).settings().identity.nick, "misha");

    // And the old file is left exactly where it was — if this build turns out
    // to be a mistake, the previous one still starts up with the settings.
    assert_eq!(
        std::fs::read_to_string(&old).expect("still there"),
        REAL_PROFILE
    );
}

#[test]
fn a_new_file_wins_over_an_old_one() {
    let dir = TempDir::new("import-skip");
    let old = dir.path().join("gtkhxrc");
    std::fs::write(&old, REAL_PROFILE).expect("seed");
    dir.write("version = 1\n[identity]\nnick = \"newer\"\n");

    let config = Config::load_or_migrate(dir.path(), &[old]);
    assert_eq!(*config.provenance(), Provenance::Current);
    assert_eq!(config.settings().identity.nick, "newer");
}

#[test]
fn a_corrupt_new_file_does_not_fall_back_to_the_old_one() {
    // Silently reverting to a years-old gtkhxrc because today's file has a
    // typo in it would be a far stranger thing to do than starting at
    // defaults with the bad file preserved.
    let dir = TempDir::new("import-corrupt");
    let old = dir.path().join("gtkhxrc");
    std::fs::write(&old, REAL_PROFILE).expect("seed");
    dir.write("version = 1\n[identity\nnick = broken\n");

    let config = Config::load_or_migrate(dir.path(), &[old]);
    assert_eq!(*config.provenance(), Provenance::Unusable);
    assert_eq!(config.settings().identity.nick, "");
}

#[test]
fn legacy_paths_are_tried_in_order() {
    let dir = TempDir::new("import-order");
    let first = dir.path().join("gtkhxrc");
    let second = dir.path().join("dot-gtkhxrc");
    std::fs::write(&second, "NICK=fallback\n").expect("seed");

    // Only the second exists.
    let config = Config::load_or_migrate(dir.path(), &[first.clone(), second.clone()]);
    assert_eq!(config.settings().identity.nick, "fallback");
    assert_eq!(
        *config.provenance(),
        Provenance::Imported { form: Form::Lines }
    );

    // Now both do, and the first wins.
    std::fs::write(&first, "[gtkhx]\nNICK=primary\n").expect("seed");
    let config = Config::load_or_migrate(dir.path(), &[first, second]);
    assert_eq!(config.settings().identity.nick, "primary");
}

#[test]
fn no_old_file_at_all_is_just_defaults() {
    let dir = TempDir::new("import-none");
    let config = Config::load_or_migrate(dir.path(), &[dir.path().join("nope")]);
    assert_eq!(*config.provenance(), Provenance::Fresh);
    assert_eq!(*config.settings(), Settings::default());
}

#[test]
fn an_empty_or_unrecognisable_old_file_is_not_an_import() {
    let dir = TempDir::new("import-empty");
    let empty = dir.path().join("empty");
    let junk = dir.path().join("junk");
    std::fs::write(&empty, "").expect("seed");
    std::fs::write(&junk, "this file is not a gtkhxrc at all\n").expect("seed");

    let config = Config::load_or_migrate(dir.path(), &[empty, junk]);
    assert_eq!(*config.provenance(), Provenance::Fresh);
}

#[test]
fn a_legacy_file_that_is_not_utf8_is_still_migrated() {
    // This is the one that would have hurt. `read_to_string` rejects invalid
    // UTF-8, and treating that as "no file" resets every setting the user had,
    // once, with no diagnostic and no way back. The C reader handles these
    // bytes deliberately — its STRING32 arm runs a failed UTF-8 check through
    // Mac Roman — and the writer never validated, so they round-tripped
    // through every save since.
    let dir = TempDir::new("not-utf8");
    let old = dir.path().join("gtkhxrc");
    // 0xCA is a non-breaking space in Mac Roman, and not valid UTF-8 alone.
    let mut bytes = b"[gtkhx]\nNICK=mi".to_vec();
    bytes.push(0xCA);
    bytes.extend_from_slice(b"sha\nFONT=Monospace 11\n");
    std::fs::write(&old, &bytes).expect("seed");

    let config = Config::load_or_migrate(dir.path(), std::slice::from_ref(&old));
    assert!(
        matches!(config.provenance(), Provenance::Imported { .. }),
        "{:?} — the whole profile was discarded",
        config.provenance()
    );
    assert_eq!(config.settings().chat.font, "Monospace 11");
    assert_eq!(config.settings().identity.nick, "mi\u{00A0}sha");
    assert!(config
        .warnings()
        .iter()
        .any(|w| matches!(w.kind, WarningKind::NotUtf8)));
}

#[test]
fn an_unreadable_legacy_file_is_reported_not_skipped_past() {
    // "Exists but I can't read it" is not "absent", and quietly falling
    // through to an older file would be the wrong answer.
    let dir = TempDir::new("unreadable");
    let bad = dir.path().join("is-a-directory");
    std::fs::create_dir(&bad).expect("seed");
    let older = dir.path().join("older");
    std::fs::write(&older, "NICK=fallback\n").expect("seed");

    let config = Config::load_or_migrate(dir.path(), &[bad, older]);
    assert_eq!(*config.provenance(), Provenance::Fresh);
    assert_eq!(config.settings().identity.nick, "");
    assert!(matches!(
        config.warnings()[0].kind,
        WarningKind::Unreadable(_)
    ));
}

#[test]
fn the_first_legacy_file_that_exists_wins_even_if_it_is_empty() {
    // The C reader tests for existence and returns unconditionally. "Your
    // current file was empty so I used your twenty-year-old one" would be a
    // strange thing to do unasked.
    let dir = TempDir::new("empty-first");
    let first = dir.path().join("gtkhxrc");
    let second = dir.path().join("dot-gtkhxrc");
    std::fs::write(&first, "").expect("seed");
    std::fs::write(&second, "NICK=ancient\n").expect("seed");

    let config = Config::load_or_migrate(dir.path(), &[first, second]);
    assert_eq!(*config.provenance(), Provenance::Fresh);
    assert_eq!(config.settings().identity.nick, "");
}

#[test]
fn an_empty_theme_name_falls_back_to_the_default() {
    // theme_name is NULL in C until the user picks one, and the writer emits
    // NULL as "". C read empty as "use the built-in"; carrying "" across would
    // leave a theme name that resolves to nothing.
    let (keys, form) = legacy::parse("[gtkhx]\nTHEMENAME=\nVOICEOUTPUTDEVICE=\nVOICEPTTKEY=\n");
    let mut warnings = Vec::new();
    let doc = migrate::to_document(&keys, form, &mut warnings);
    assert!(warnings.is_empty(), "{warnings:?}");

    let dir = TempDir::new("empty-theme");
    std::fs::write(path(dir.path()), doc.to_string()).expect("seed");
    let s = Config::load(dir.path());
    assert_eq!(s.settings().appearance.theme, "default");

    // But empty stays empty where it means something: the voice devices read
    // it as "system default" and the PTT key as "not bound yet".
    assert_eq!(s.settings().voice.output_device, "");
    assert_eq!(s.settings().voice.ptt_key, "");
}

#[test]
fn a_zero_toolbar_size_is_the_never_saved_sentinel_not_an_error() {
    // The geo.tool static default is {0, 0, ...}, so a profile from before the
    // toolbar was ever resized carries TOOLXSIZE=0. That is not the user
    // getting something wrong, so it should not be reported as out of range.
    let (keys, form) = legacy::parse("[gtkhx]\nTOOLXSIZE=0\nTOOLYSIZE=0\n");
    let mut warnings = Vec::new();
    let doc = migrate::to_document(&keys, form, &mut warnings);
    assert!(warnings.is_empty(), "{warnings:?}");

    let dir = TempDir::new("zero-tool");
    std::fs::write(path(dir.path()), doc.to_string()).expect("seed");
    let config = Config::load(dir.path());
    assert!(config.warnings().is_empty(), "{:?}", config.warnings());
    assert_eq!(config.settings().window.toolbar_width, 1100);
    assert_eq!(config.settings().window.toolbar_height, 700);
}

#[test]
fn an_over_long_nick_is_clamped_to_the_wire_field() {
    // identity.nick becomes the connection's 32-byte name buffer. The C reader
    // clamped on the way in, so a file the C writer produced can't exceed it —
    // but a hand-edited or bare-lines one can, and it would otherwise surface
    // as a wire problem at connect time rather than here.
    let long = "a".repeat(80);
    let (keys, form) = legacy::parse(&format!("[gtkhx]\nNICK={long}\n"));
    let mut warnings = Vec::new();
    let doc = migrate::to_document(&keys, form, &mut warnings);
    assert!(doc.to_string().contains(&"a".repeat(31)));
    assert!(!doc.to_string().contains(&"a".repeat(32)));

    // Cut on a character boundary, or the result isn't valid UTF-8 and the
    // document can't be written at all.
    let wide = "é".repeat(40);
    let (keys, form) = legacy::parse(&format!("[gtkhx]\nNICK={wide}\n"));
    let doc = migrate::to_document(&keys, form, &mut Vec::new());
    let dir = TempDir::new("wide-nick");
    std::fs::write(path(dir.path()), doc.to_string()).expect("seed");
    let nick = Config::load(dir.path()).settings().identity.nick.clone();
    assert!(nick.len() <= 31, "{} bytes", nick.len());
    assert_eq!(nick, "é".repeat(15));
}

#[test]
fn a_migrated_file_is_laid_out_like_a_fresh_one() {
    // An upgrading user's first gtkhx.toml should not look different from a
    // new user's. Both come out in schema order.
    let (keys, form) = legacy::parse(REAL_PROFILE);
    let migrated = migrate::to_document(&keys, form, &mut Vec::new()).to_string();
    let fresh = Config::defaults().to_toml();

    let headers = |s: &str| -> Vec<String> {
        s.lines()
            .filter(|l| l.starts_with('['))
            .map(str::to_string)
            .collect()
    };
    assert_eq!(headers(&migrated), headers(&fresh));
    assert!(migrated.starts_with("version = 1"), "{migrated}");
}

#[test]
fn a_meaningful_trailing_space_survives_the_line_form() {
    // The line parser must not trim. The default timestamp format ends in a
    // space, and a reader that trimmed would jam the timestamp against the
    // message text — the exact quiet corruption this rewrite exists to end.
    // The C parser passed the value straight to g_strdup, so verbatim is both
    // correct and faithful.
    let (keys, form) = legacy::parse("TIMESTAMPFORMAT=[%H:%M:%S] \nNICK=bob\n");
    assert_eq!(form, Form::Lines);
    assert_eq!(keys["TIMESTAMPFORMAT"], "[%H:%M:%S] ");

    let doc = migrate::to_document(&keys, form, &mut Vec::new());
    let dir = TempDir::new("line-trailing-space");
    std::fs::write(path(dir.path()), doc.to_string()).expect("seed");
    assert_eq!(
        Config::load(dir.path()).settings().chat.timestamp_format,
        "[%H:%M:%S] "
    );
}

#[test]
fn the_over_escape_warning_does_not_fire_on_the_line_form() {
    // Nothing ever wrote the line form through g_key_file_set_string, so
    // nothing escaped it and nothing unescapes it. A doubled backslash there
    // is just a doubled backslash — a UNC path, most likely — and warning
    // about it would explain a bug that cannot have happened to that file.
    let (keys, form) = legacy::parse(r"DOWNLOAD=\\server\share");
    assert_eq!(form, Form::Lines);
    assert_eq!(keys["DOWNLOAD"], r"\\server\share");

    let mut warnings = Vec::new();
    let doc = migrate::to_document(&keys, form, &mut warnings);
    assert!(warnings.is_empty(), "{warnings:?}");
    assert!(doc.to_string().contains(r"\\server\share"));

    // The same bytes in the GKeyFile form *do* warn, because there the
    // asymmetry is real and one round of unescaping has already run.
    let (keys, form) = legacy::parse("[gtkhx]\nDOWNLOAD=\\\\\\\\server\\\\share\n");
    assert_eq!(form, Form::KeyFile);
    let mut warnings = Vec::new();
    migrate::to_document(&keys, form, &mut warnings);
    assert!(warnings
        .iter()
        .any(|w| matches!(w.kind, WarningKind::OverEscaped { .. })));
}

// ============================================================== the C ABI ==
//
// The by-name bridge is what every Settings page goes through, so a break here
// is a settings dialog that silently does nothing. The names are the old
// SHOUTING_CASE keys, resolved through the migration map — which means these
// also pin that the map stays usable as a live lookup, not just a one-shot.

use crate::ffi;
use std::ffi::{c_int, CString};

/// Call an FFI entry point with a C string, without leaking one per call.
fn c(s: &str) -> CString {
    CString::new(s).expect("no interior NUL")
}

/// Take a Rust-allocated C string back as a `String`, freeing it the way C
/// would.
fn take(p: *mut std::ffi::c_char) -> String {
    if p.is_null() {
        return String::new();
    }
    let out = unsafe { std::ffi::CStr::from_ptr(p) }
        .to_string_lossy()
        .into_owned();
    unsafe { ffi::hxconfig_free_string(p) };
    out
}

#[test]
fn the_by_name_bridge_round_trips_each_type() {
    ffi::hxconfig_load_defaults_for_test();

    // Type tags match the old cfgvars[] constants, because the Settings pages
    // switch on them: 1 INT, 2 BOOLEAN, 3 STRING, 5 UINT16.
    assert_eq!(ffi::hxconfig_type(c("XBUF_MAX").as_ptr()), 1);
    assert_eq!(ffi::hxconfig_type(c("MARKDOWN").as_ptr()), 2);
    assert_eq!(ffi::hxconfig_type(c("FONT").as_ptr()), 3);
    assert_eq!(ffi::hxconfig_type(c("ICON").as_ptr()), 5);
    // A name nothing recognises, and one that was deliberately dropped, both
    // read as "not a setting" rather than as a real key with a zero value.
    assert_eq!(ffi::hxconfig_type(c("NOPE").as_ptr()), 0);
    assert_eq!(ffi::hxconfig_type(c("OPENCHAT").as_ptr()), 0);

    assert_eq!(ffi::hxconfig_get_bool(c("MARKDOWN").as_ptr()), 1);
    assert_eq!(ffi::hxconfig_set_bool(c("MARKDOWN").as_ptr(), 0), 1);
    assert_eq!(ffi::hxconfig_get_bool(c("MARKDOWN").as_ptr()), 0);
    // Setting the same value again reports "nothing changed", which is what
    // lets the caller skip the change hook — the nickname's puts a packet on
    // the wire, so a redundant one is visible to everyone in the room.
    assert_eq!(ffi::hxconfig_set_bool(c("MARKDOWN").as_ptr(), 0), 0);

    assert_eq!(ffi::hxconfig_get_int(c("XBUF_MAX").as_ptr()), 500);
    assert_eq!(ffi::hxconfig_set_int(c("XBUF_MAX").as_ptr(), 4096), 1);
    assert_eq!(ffi::hxconfig_get_int(c("XBUF_MAX").as_ptr()), 4096);

    assert_eq!(
        take(ffi::hxconfig_get_string(c("FONT").as_ptr())),
        "Monospace 10"
    );
    unsafe {
        assert_eq!(
            ffi::hxconfig_set_string(c("FONT").as_ptr(), c("Cantarell 12").as_ptr()),
            1
        );
    }
    assert_eq!(
        take(ffi::hxconfig_get_string(c("FONT").as_ptr())),
        "Cantarell 12"
    );

    let s = ffi::snapshot().expect("loaded");
    assert!(!s.chat.markdown);
    assert_eq!(s.chat.scrollback_lines, 4096);
    assert_eq!(s.chat.font, "Cantarell 12");
}

#[test]
fn a_list_reads_and_writes_as_one_comma_separated_string() {
    // The old keys were comma-separated strings and the Settings entry rows
    // still edit one. The schema stores a real array; the comma is a boundary
    // format here, not a storage decision.
    ffi::hxconfig_load_defaults_for_test();

    assert_eq!(
        take(ffi::hxconfig_get_string(c("TRACKER").as_ptr())),
        "hltracker.com"
    );
    unsafe {
        ffi::hxconfig_set_string(
            c("TRACKER").as_ptr(),
            c("a.example, b.example ,, c.example").as_ptr(),
        );
    }
    assert_eq!(
        ffi::snapshot().unwrap().trackers.addresses,
        vec![
            "a.example".to_string(),
            "b.example".to_string(),
            "c.example".to_string()
        ],
        "each element trimmed, empties dropped"
    );
    assert_eq!(
        take(ffi::hxconfig_get_string(c("TRACKER").as_ptr())),
        "a.example,b.example,c.example"
    );

    // And the array is reachable element-wise, which is how the mirror builds
    // the char** the tracker fetch wants.
    assert_eq!(ffi::hxconfig_tracker_count(), 3);
    assert_eq!(take(ffi::hxconfig_tracker_at(1)), "b.example");
    // Out of range in either direction is "no such address". A negative index
    // clamped to zero would answer a nonsense question with a real-looking
    // tracker, which is how the C caller's loop bound would go unnoticed.
    assert!(ffi::hxconfig_tracker_at(3).is_null());
    assert!(ffi::hxconfig_tracker_at(99).is_null());
    assert!(ffi::hxconfig_tracker_at(-1).is_null());
    assert!(ffi::hxconfig_tracker_at(c_int::MIN).is_null());
}

#[test]
fn an_unknown_name_is_inert_rather_than_dangerous() {
    // The old lookup returned NULL and the callers turned that into 0 or "",
    // indistinguishable from a real value. That is unchanged and deliberate —
    // but a *write* to an unknown name must not land anywhere, which is the
    // part worth pinning.
    ffi::hxconfig_load_defaults_for_test();
    let before = ffi::snapshot().unwrap();

    assert_eq!(ffi::hxconfig_get_bool(c("NOT_A_KEY").as_ptr()), 0);
    assert_eq!(ffi::hxconfig_get_int(c("NOT_A_KEY").as_ptr()), 0);
    assert_eq!(take(ffi::hxconfig_get_string(c("NOT_A_KEY").as_ptr())), "");
    assert_eq!(ffi::hxconfig_set_bool(c("NOT_A_KEY").as_ptr(), 1), 0);
    assert_eq!(ffi::hxconfig_set_int(c("NOT_A_KEY").as_ptr(), 7), 0);
    unsafe {
        assert_eq!(
            ffi::hxconfig_set_string(c("NOT_A_KEY").as_ptr(), c("x").as_ptr()),
            0
        );
    }

    assert_eq!(ffi::snapshot().unwrap(), before);
}

#[test]
fn a_null_name_does_not_crash() {
    ffi::hxconfig_load_defaults_for_test();
    assert_eq!(ffi::hxconfig_type(std::ptr::null()), 0);
    assert_eq!(ffi::hxconfig_get_bool(std::ptr::null()), 0);
    assert_eq!(take(ffi::hxconfig_get_string(std::ptr::null())), "");
    assert_eq!(ffi::hxconfig_set_bool(std::ptr::null(), 1), 0);
    // A NULL *value* reads as empty, which is what C passes for a cleared
    // entry row.
    unsafe {
        ffi::hxconfig_set_string(c("VOICEPTTKEY").as_ptr(), std::ptr::null());
    }
    assert_eq!(ffi::snapshot().unwrap().voice.ptt_key, "");
}

#[test]
fn changes_coalesce_into_one_write() {
    // Every switch toggle used to rebuild and rewrite the whole file
    // synchronously. Now a run of changes marks the state dirty and one flush
    // writes it.
    let dir = TempDir::new("ffi-flush");
    let d = c(dir.path().to_str().unwrap());
    unsafe {
        ffi::hxconfig_load(d.as_ptr(), std::ptr::null(), std::ptr::null());
    }
    assert_eq!(ffi::hxconfig_is_first_run(), 1);
    assert!(!path(dir.path()).exists(), "loading must not write");

    ffi::hxconfig_set_bool(c("MARKDOWN").as_ptr(), 0);
    ffi::hxconfig_set_int(c("XBUF_MAX").as_ptr(), 900);
    assert!(!path(dir.path()).exists(), "still nothing on disk");

    assert_eq!(ffi::hxconfig_flush(), 1);
    let reloaded = Config::load(dir.path());
    assert!(!reloaded.settings().chat.markdown);
    assert_eq!(reloaded.settings().chat.scrollback_lines, 900);

    // A flush with nothing pending is a no-op that still reports success.
    assert_eq!(ffi::hxconfig_flush(), 1);
}

#[test]
fn an_imported_profile_is_visible_through_the_bridge() {
    // The whole point of the load entry point: a user upgrading gets their old
    // settings, and C sees them through the same by-name calls.
    let dir = TempDir::new("ffi-import");
    let old = dir.path().join("gtkhxrc");
    std::fs::write(&old, REAL_PROFILE).expect("seed");

    let d = c(dir.path().to_str().unwrap());
    let o = c(old.to_str().unwrap());
    let warnings = unsafe { ffi::hxconfig_load(d.as_ptr(), o.as_ptr(), std::ptr::null()) };
    assert_eq!(warnings, 0);
    assert_eq!(ffi::hxconfig_is_first_run(), 0);

    assert_eq!(take(ffi::hxconfig_get_string(c("NICK").as_ptr())), "misha");
    assert_eq!(ffi::hxconfig_get_int(c("ICON").as_ptr()), 32766);
    assert_eq!(
        take(ffi::hxconfig_get_string(c("TIMESTAMPFORMAT").as_ptr())),
        "[%H:%M:%S] "
    );
    assert_eq!(ffi::hxconfig_get_bool(c("SOUNDSON").as_ptr()), 1);
    assert_eq!(ffi::hxconfig_get_int(c("TOOLXSIZE").as_ptr()), 2807);
}

#[test]
fn diagnostics_are_reachable_from_c() {
    let dir = TempDir::new("ffi-warnings");
    dir.write("version = 1\n[chat]\nscrollback_lines = \"lots\"\n");

    let d = c(dir.path().to_str().unwrap());
    let n = unsafe { ffi::hxconfig_load(d.as_ptr(), std::ptr::null(), std::ptr::null()) };
    assert_eq!(n, 1);

    let text = take(ffi::hxconfig_warning(0));
    assert!(text.contains("chat.scrollback_lines"), "{text}");
    assert!(ffi::hxconfig_warning(1).is_null());
    assert!(ffi::hxconfig_warning(-1).is_null());
    assert!(ffi::hxconfig_warning(c_int::MIN).is_null());
}

// ------------------------------------------------------- the nick colour --

#[test]
fn a_colour_is_written_the_way_a_person_writes_one() {
    // 12607947 tells a reader nothing and cannot be hand-edited with any
    // confidence. The value is still an integer in memory and on the wire —
    // only the file spells it #rrggbb.
    let dir = TempDir::new("colour-write");
    let mut config = Config::defaults();
    config.settings_mut().identity.nick_color = 0x00c0_65cb;
    config.save(dir.path()).expect("save");

    assert!(
        dir.read().contains(r##"nick_color = "#c065cb""##),
        "{}",
        dir.read()
    );
    assert_eq!(
        Config::load(dir.path()).settings().identity.nick_color,
        0x00c0_65cb
    );

    // No colour is the empty string, matching the rest of the schema, where an
    // empty nickname means unset and an empty voice device means the default.
    let mut config = Config::defaults();
    assert_eq!(config.settings().identity.nick_color, NICK_COLOR_NONE);
    config.save(dir.path()).expect("save");
    assert!(
        dir.read().contains(r##"nick_color = """##),
        "{}",
        dir.read()
    );
    assert_eq!(
        Config::load(dir.path()).settings().identity.nick_color,
        NICK_COLOR_NONE
    );
}

#[test]
fn a_colour_is_read_the_way_a_person_might_have_written_one() {
    for (text, want) in [
        ("#c065cb", 0x00c0_65cb),
        ("#C065CB", 0x00c0_65cb),
        ("c065cb", 0x00c0_65cb),
        // The three-digit CSS shorthand is what most people reach for.
        ("#abc", 0x00aa_bbcc),
        ("abc", 0x00aa_bbcc),
        ("  #c065cb  ", 0x00c0_65cb),
        ("", NICK_COLOR_NONE),
        ("none", NICK_COLOR_NONE),
        ("None", NICK_COLOR_NONE),
    ] {
        assert_eq!(parse_nick_color(text), Some(want), "for {text:?}");
    }

    for bad in ["#12345", "#gg0000", "red", "#1234567", "0x00c065cb"] {
        assert_eq!(parse_nick_color(bad), None, "for {bad:?}");
    }

    // Every accepted spelling reaches the setting itself, not just the parser.
    let dir = TempDir::new("colour-read");
    dir.write("version = 1\n[identity]\nnick_color = \"#ABC\"\n");
    let config = Config::load(dir.path());
    assert!(config.warnings().is_empty(), "{:?}", config.warnings());
    assert_eq!(config.settings().identity.nick_color, 0x00aa_bbcc);
}

#[test]
fn a_bare_integer_colour_still_reads_and_heals_on_save() {
    // What the key held before, and what someone reaching for a decimal would
    // write. Accepting both on read and writing one means a file fixes itself
    // on the next save rather than needing a schema version to carry the
    // change.
    let dir = TempDir::new("colour-legacy");
    dir.write("version = 1\n[identity]\nnick_color = 12607947\n");

    let mut config = Config::load(dir.path());
    assert!(config.warnings().is_empty(), "{:?}", config.warnings());
    assert_eq!(config.settings().identity.nick_color, 12607947);

    config.settings_mut().chat.timestamp = true;
    config.save(dir.path()).expect("save");
    assert!(
        dir.read().contains(r##"nick_color = "#c061cb""##),
        "{}",
        dir.read()
    );
}

#[test]
fn a_value_that_is_not_a_colour_is_diagnosed() {
    let dir = TempDir::new("colour-bad");
    dir.write("version = 1\n[identity]\nnick_color = \"puce\"\n");

    let config = Config::load(dir.path());
    assert_eq!(config.settings().identity.nick_color, NICK_COLOR_NONE);
    let w = &config.warnings()[0];
    assert_eq!(w.path, "identity.nick_color");
    assert!(matches!(w.kind, WarningKind::UnknownValue { .. }));
    assert!(w.to_string().contains("#rrggbb"), "{w}");
}

#[test]
fn the_migration_converts_the_old_decimal_colour() {
    // The real profile carries NICKCOLOR=12607947.
    let (keys, form) = legacy::parse(REAL_PROFILE);
    let doc = migrate::to_document(&keys, form, &mut Vec::new());
    assert!(
        doc.to_string().contains(r##"nick_color = "#c061cb""##),
        "{doc}"
    );
    assert!(!doc.to_string().contains("12607947"), "{doc}");
}

#[test]
fn the_c_abi_still_sees_a_colour_as_an_integer() {
    // The Settings colour picker and the C mirror both deal in 0x00RRGGBB
    // ints; only the file spells it in hex. So the type tag stays INT and the
    // int accessors keep working.
    ffi::hxconfig_load_defaults_for_test();
    assert_eq!(ffi::hxconfig_type(c("NICKCOLOR").as_ptr()), 1);
    assert_eq!(
        ffi::hxconfig_get_int(c("NICKCOLOR").as_ptr()),
        NICK_COLOR_NONE
    );
    assert_eq!(
        ffi::hxconfig_set_int(c("NICKCOLOR").as_ptr(), 0x00c0_65cb),
        1
    );
    assert_eq!(ffi::hxconfig_get_int(c("NICKCOLOR").as_ptr()), 0x00c0_65cb);
}
