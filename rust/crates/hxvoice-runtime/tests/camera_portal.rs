//! The Camera portal path before the portal has opened anything: no
//! camera may come from anywhere else. Its own test binary, because it
//! sets a process-wide environment variable.

#![cfg(target_os = "linux")]

use hxvoice_runtime::hxvoice::VideoKind;
use hxvoice_runtime::video;

#[test]
fn the_portal_path_finds_no_camera_without_a_remote() {
    // The only test in this binary, so nothing reads the environment
    // concurrently.
    std::env::set_var(video::CAMERA_PORTAL_ENV, "1");
    assert!(video::camera_via_portal());
    assert!(!video::camera_remote_open());
    // No remote: the sandbox's empty /dev would be all the device monitor
    // saw, so nothing is listed, whatever the host has plugged in.
    assert!(video::list_cameras().is_empty());

    // Until the portal has said, the button offers to try, as long as the
    // capture chain is installed at all.
    let chain = gstreamer::init().is_ok()
        && [
            "videoconvert",
            "videoscale",
            "videorate",
            "vp8enc",
            "rtpvp8pay",
            "tee",
            "queue",
            "appsink",
            "pipewiresrc",
        ]
        .iter()
        .all(|f| gstreamer::ElementFactory::find(f).is_some());
    assert_eq!(video::publish_available(VideoKind::Camera), chain);
    // Once it says there's no camera, it doesn't.
    video::set_camera_portal_present(false);
    assert!(!video::publish_available(VideoKind::Camera));
}
