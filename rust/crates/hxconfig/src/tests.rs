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
fn a_missing_or_bogus_version_is_treated_as_current() {
    // A hand-written file that forgot the key is far likelier than a file from
    // a schema that never existed, and treating it as ancient would run it
    // through every migration in the chain.
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

    dir.write("version = 0\n[chat]\nmarkdown = false\n");
    let config = Config::load(dir.path());
    assert_eq!(*config.provenance(), Provenance::Current);
    assert!(matches!(
        config.warnings()[0].kind,
        WarningKind::BadVersion(_)
    ));
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
nick_color = 0x0066ccff
",
    );

    let mut config = Config::load(dir.path());
    assert!(config.warnings().is_empty(), "{:?}", config.warnings());
    assert_eq!(config.settings().chat.font, "Cantarell 12");
    assert_eq!(config.settings().chat.scrollback_lines, 1000);
    assert_eq!(config.settings().identity.nick_color, 0x0066_ccff);

    // Untouched, each keeps the form the user wrote.
    config.save(dir.path()).expect("save");
    let text = dir.read();
    assert!(text.contains("'Cantarell 12'"), "{text}");
    assert!(text.contains("1_000"), "{text}");
    assert!(text.contains("0x0066ccff"), "{text}");

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
