//! A camera plugged in or out while a [`video::CameraWatch`] is held
//! reaches the watcher, the camera list and the camera button. The camera
//! is a device provider registered here, so no hardware is involved. Its
//! own test binary: it owns the default main context, and registers a
//! provider every device monitor in the process would see.

use std::cell::Cell;
use std::rc::Rc;
use std::sync::atomic::{AtomicUsize, Ordering};

use gstreamer as gst;
use gstreamer::glib;
use gstreamer::prelude::*;
use gstreamer::subclass::prelude::*;
use hxvoice_runtime::hxvoice::VideoKind;
use hxvoice_runtime::video;

/// How often the test provider was started and stopped.
static STARTS: AtomicUsize = AtomicUsize::new(0);
static STOPS: AtomicUsize = AtomicUsize::new(0);

mod imp {
    use super::*;

    #[derive(Default)]
    pub struct TestCamera;

    #[glib::object_subclass]
    impl ObjectSubclass for TestCamera {
        const NAME: &'static str = "HxTestCamera";
        type Type = super::TestCamera;
        type ParentType = gst::Device;
    }

    impl ObjectImpl for TestCamera {}
    impl GstObjectImpl for TestCamera {}
    impl DeviceImpl for TestCamera {}

    #[derive(Default)]
    pub struct TestCameraProvider;

    #[glib::object_subclass]
    impl ObjectSubclass for TestCameraProvider {
        const NAME: &'static str = "HxTestCameraProvider";
        type Type = super::TestCameraProvider;
        type ParentType = gst::DeviceProvider;
    }

    impl ObjectImpl for TestCameraProvider {}
    impl GstObjectImpl for TestCameraProvider {}
    impl DeviceProviderImpl for TestCameraProvider {
        fn metadata() -> Option<&'static gst::subclass::DeviceProviderMetadata> {
            static META: std::sync::OnceLock<gst::subclass::DeviceProviderMetadata> =
                std::sync::OnceLock::new();
            Some(META.get_or_init(|| {
                gst::subclass::DeviceProviderMetadata::new(
                    "Test cameras",
                    "Video/Source",
                    "Cameras the test plugs in",
                    "GtkHx",
                )
            }))
        }

        fn start(&self) -> Result<(), gst::LoggableError> {
            STARTS.fetch_add(1, Ordering::SeqCst);
            Ok(())
        }

        fn stop(&self) {
            STOPS.fetch_add(1, Ordering::SeqCst);
        }
    }
}

glib::wrapper! {
    pub struct TestCamera(ObjectSubclass<imp::TestCamera>)
        @extends gst::Device, gst::Object;
}

glib::wrapper! {
    pub struct TestCameraProvider(ObjectSubclass<imp::TestCameraProvider>)
        @extends gst::DeviceProvider, gst::Object;
}

const NAME: &str = "GtkHx Test Camera";

fn listed() -> bool {
    video::list_cameras().iter().any(|c| c.display_name == NAME)
}

#[test]
fn a_watched_camera_comes_and_goes() {
    gst::init().unwrap();
    let ctx = glib::MainContext::default();
    let _owner = ctx.acquire().expect("sole test in this binary");
    gst::DeviceProvider::register(
        None,
        "hxtestcameraprovider",
        gst::Rank::PRIMARY,
        TestCameraProvider::static_type(),
    )
    .unwrap();

    let changes = Rc::new(Cell::new(0));
    let watch = video::watch_cameras({
        let changes = changes.clone();
        move || changes.set(changes.get() + 1)
    });
    assert!(!listed());

    // The monitor started the provider, and the factory hands out that
    // same instance.
    let provider = gst::DeviceProviderFactory::find("hxtestcameraprovider")
        .and_then(|f| f.get())
        .unwrap();
    let camera: TestCamera = glib::Object::builder()
        .property("display-name", NAME)
        .property("device-class", "Video/Source")
        .property("caps", gst::Caps::new_empty_simple("video/x-raw"))
        .build();
    let pump = |want: u32| {
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
        while changes.get() < want && std::time::Instant::now() < deadline {
            ctx.iteration(false);
        }
        assert_eq!(changes.get(), want);
    };

    // The button follows the live set, given the encoder chain.
    let encodable = [
        "videoconvert",
        "videoscale",
        "videorate",
        "vp8enc",
        "rtpvp8pay",
        "tee",
        "queue",
        "appsink",
    ]
    .iter()
    .all(|f| gst::ElementFactory::find(f).is_some());
    let button = || {
        assert_eq!(
            video::publish_available(VideoKind::Camera),
            encodable && !video::list_cameras().is_empty()
        )
    };
    // Until the monitor has listed a camera, an empty list may only be a
    // provider hiding one, and autovideosrc still gets to try.
    assert_eq!(
        video::publish_available(VideoKind::Camera),
        encodable
            && (!video::list_cameras().is_empty()
                || gst::ElementFactory::find("autovideosrc").is_some())
    );

    provider.device_add(&camera);
    pump(1);
    assert!(listed());
    button();

    provider.device_remove(&camera);
    pump(2);
    assert!(!listed());
    button();

    // A second watch shares the monitor and is told too; dropping the
    // first leaves the second working.
    assert_eq!(STARTS.load(Ordering::SeqCst), 1);
    let second = Rc::new(Cell::new(0));
    let watch2 = video::watch_cameras({
        let second = second.clone();
        move || second.set(second.get() + 1)
    });
    drop(watch);
    provider.device_add(&camera);
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
    while second.get() < 1 && std::time::Instant::now() < deadline {
        ctx.iteration(false);
    }
    assert_eq!(second.get(), 1);
    assert_eq!(changes.get(), 2);
    assert!(listed());
    assert_eq!(STARTS.load(Ordering::SeqCst), 1, "one monitor for both");
    assert_eq!(STOPS.load(Ordering::SeqCst), 0);

    // The last watch stopped the monitor: a change reaches no one.
    drop(watch2);
    assert_eq!(STOPS.load(Ordering::SeqCst), 1);
    provider.device_remove(&camera);
    while ctx.iteration(false) {}
    assert_eq!(second.get(), 1);
}
