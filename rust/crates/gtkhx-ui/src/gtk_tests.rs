//! The crate's display-backed tests, all of them, in one `#[test]`.
//!
//! **Why one.** GTK 4 asserts main-thread affinity inside every widget
//! constructor, and cargo runs each `#[test]` on its own thread. Whichever
//! test reaches `gtk::init()` first claims GTK for its thread; a second test
//! then fails twice over — `gtk::init()` returns `Err` from the wrong thread,
//! and any widget call it makes anyway aborts with "GTK may only be used from
//! the main thread". A mutex does not help, because the problem is thread
//! identity rather than concurrency.
//!
//! So the crate gets exactly one test that owns the GTK thread, and everything
//! needing a display hangs off it as a plain function. New display-backed
//! checks go here as another call, not as another `#[test]`.
//!
//! **A display is required, never optional.** GTK 4 has no headless backend,
//! so CI runs the suite under `xvfb-run` with `GDK_BACKEND=x11`. A missing
//! display fails; it must never skip, because a skip is indistinguishable
//! from a pass in a CI log.

use gtk4 as gtk;
use libadwaita as adw;

#[test]
fn display_backed() {
    assert!(
        gtk::init().is_ok(),
        "no display: run under xvfb-run with GDK_BACKEND=x11"
    );
    adw::init().expect("libadwaita init");

    crate::options_window::tests::check_every_page_builds();
    crate::conn_tabs::tests::check_strip_indexes_connections();
    crate::conn_tabs::tests::check_live_connection_asks_before_closing();
    crate::conn_tabs::tests::check_wheel_is_wired_to_the_bar();
    crate::dock::tests::check_windows_close_with_their_connection();
    crate::files::complete::tests::check_completes_and_survives_entry_destroy();
    crate::emoji::tests::check_chooser_is_built_on_first_open();
    crate::users_view::tests::check_rows_land_in_one_batch();
    crate::pchat::tests::check_a_rename_retitles_the_tabs();
    crate::users_view::tests::check_avatars_stay_with_their_connection();
    crate::updates::tests::check_banner_starts_hidden();
    crate::chat_output::tests::check_older_pages_go_above_the_first();
    crate::chat_output::tests::check_server_prose_takes_a_row_a_line();
    crate::chat_output::tests::check_a_line_has_one_row_found_by_its_id();
    crate::chat_output::tests::check_trimmed_live_rows_are_let_go();
    crate::chat_output::tests::check_a_cleared_view_starts_over();
    crate::chat_output::tests::check_a_page_read_before_a_change_is_asked_again();
    crate::chat_output::tests::check_chips_go_on_the_lines_row();
    crate::chat_output::tests::check_a_chip_clicked_asks_for_it_and_shows_it();
    crate::chat_output::tests::check_a_line_told_of_is_fresh_again();
    crate::chat_output::tests::check_who_reacted_lists_emoji_alone();
    crate::chat_output::tests::check_a_moderator_is_offered_to_clear_a_line_with_chips();
    crate::chat_output::tests::check_a_private_chats_kept_line_is_kind_4();
    crate::chat_output::tests::check_a_reply_is_headed_with_what_it_answers();
    crate::chat_output::tests::check_a_quote_asked_for_heads_its_replies();
    crate::chat_output::tests::check_a_line_taken_away_is_quoted_as_removed();
    crate::chat_output::tests::check_a_trimmed_reply_asks_for_no_quote();
    crate::reactions::tests::check_who_reacted_says_who_once_answered();
    crate::msg::tests::check_an_instant_messages_chips_are_its_parties();
    crate::icon_picker::tests::check_banners_fit_the_dialog();
    #[cfg(feature = "voice")]
    crate::users_voice_col::voice_impl::tests::check_header_is_an_icon();
    #[cfg(feature = "voice")]
    crate::video_panel::tests::check_tile_shows_a_frame();
    #[cfg(feature = "voice")]
    crate::video_panel::tests::check_relabel_follows_a_nick_change();
    #[cfg(feature = "voice")]
    crate::video_panel::tests::check_tiles_sort_by_user();
    #[cfg(feature = "voice")]
    crate::video_panel::tests::check_panel_shows_tiles_and_frames();
    #[cfg(feature = "voice")]
    crate::video_panel::tests::check_reach_follows_the_scroll();
    #[cfg(feature = "voice")]
    crate::video_panel::tests::check_focus_follows_the_user();
    #[cfg(feature = "voice")]
    crate::video_panel::tests::check_stop_watching_and_watch_again();
}
