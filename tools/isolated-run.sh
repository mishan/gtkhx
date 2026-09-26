#!/bin/sh
# Run a command headless, sealed off from the desktop session:
#
#   - its own X server (xvfb-run), with DISPLAY and WAYLAND_DISPLAY cleared
#     so nothing can reach the real one;
#   - its own D-Bus session bus with no service directories, so nothing is
#     auto-started on it: portal and notification calls find no desktop
#     services, rather than launching real ones;
#   - its own PipeWire + WirePlumber (+ pipewire-pulse) in a private
#     XDG_RUNTIME_DIR, with the ALSA, V4L2, libcamera and Bluetooth
#     monitors off and a null sink and source standing in for hardware;
#   - GStreamer's plugins that open hardware directly (V4L2, libcamera, UVC
#     H.264, OSS, DeckLink) left off its plugin path, so a device scan and
#     every pipeline see only the private PipeWire. Their device providers
#     ignore a GST_PLUGIN_FEATURE_RANK override, so hiding the plugin is
#     the only way to keep a scan off the real cameras.
#
# Usage:
#   tools/isolated-run.sh meson test -C build-voice --suite integration
#   tools/isolated-run.sh cargo test -p gtkhx-ui --features voice
#
# Set ISOLATED_RUN_KEEP=1 to keep the scratch directory for inspection.

set -eu

if [ "$#" -eq 0 ]; then
    echo "usage: $0 <command> [args...]" >&2
    exit 2
fi

for tool in xvfb-run dbus-run-session pipewire wireplumber pipewire-pulse wpctl; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "$0: $tool not found" >&2
        exit 2
    }
done

# Re-entered inside dbus-run-session: bring up the audio stack, then run the
# command under xvfb.
if [ "${ISOLATED_RUN_INNER:-}" = 1 ]; then
    unset ISOLATED_RUN_INNER
    scratch=$ISOLATED_RUN_SCRATCH

    pipewire >"$scratch/pipewire.log" 2>&1 &
    pw_pid=$!
    i=0
    until [ -S "$XDG_RUNTIME_DIR/pipewire-0" ]; do
        i=$((i + 1))
        if [ "$i" -gt 100 ] || ! kill -0 "$pw_pid" 2>/dev/null; then
            echo "$0: pipewire did not start; see $scratch/pipewire.log" >&2
            exit 1
        fi
        sleep 0.05
    done
    wireplumber >"$scratch/wireplumber.log" 2>&1 &
    wp_pid=$!
    pipewire-pulse >"$scratch/pipewire-pulse.log" 2>&1 &
    pp_pid=$!

    # Wait until WirePlumber has picked a default sink, so the first
    # autoaudiosink doesn't race session setup.
    i=0
    until wpctl inspect @DEFAULT_AUDIO_SINK@ >/dev/null 2>&1; do
        i=$((i + 1))
        [ "$i" -gt 100 ] && break
        sleep 0.05
    done

    status=0
    GDK_BACKEND=x11 xvfb-run -a "$@" || status=$?
    kill "$pp_pid" "$wp_pid" "$pw_pid" 2>/dev/null || true
    wait 2>/dev/null || true
    exit "$status"
fi

scratch=$(mktemp -d "${TMPDIR:-/tmp}/gtkhx-isolated.XXXXXX")
chmod 700 "$scratch"
if [ "${ISOLATED_RUN_KEEP:-}" = 1 ]; then
    echo "$0: scratch directory $scratch" >&2
else
    trap 'rm -rf "$scratch"' EXIT INT TERM
fi

mkdir -p "$scratch/runtime" "$scratch/config/pipewire/pipewire.conf.d" \
    "$scratch/config/wireplumber/wireplumber.conf.d" "$scratch/state"
chmod 700 "$scratch/runtime"

cat >"$scratch/config/pipewire/pipewire.conf.d/10-null-devices.conf" <<'EOF'
context.objects = [
    { factory = adapter
      args = {
          factory.name     = support.null-audio-sink
          node.name        = isolated-sink
          node.description = "Isolated null sink"
          media.class      = Audio/Sink
          audio.position   = [ FL FR ]
      }
    }
    { factory = adapter
      args = {
          factory.name     = support.null-audio-sink
          node.name        = isolated-source
          node.description = "Isolated null source"
          media.class      = Audio/Source/Virtual
          audio.position   = [ MONO ]
      }
    }
]
EOF

cat >"$scratch/config/wireplumber/wireplumber.conf.d/10-no-hardware.conf" <<'EOF'
wireplumber.profiles = {
    main = {
        monitor.alsa = disabled
        monitor.alsa-midi = disabled
        monitor.bluez = disabled
        monitor.bluez-midi = disabled
        monitor.v4l2 = disabled
        monitor.libcamera = disabled
    }
}
EOF

mkdir -p "$scratch/dbus-services"
cat >"$scratch/dbus-session.conf" <<EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <keep_umask/>
  <listen>unix:tmpdir=$scratch</listen>
  <auth>EXTERNAL</auth>
  <servicedir>$scratch/dbus-services</servicedir>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
EOF

unset DISPLAY WAYLAND_DISPLAY PULSE_SERVER PULSE_RUNTIME_PATH PIPEWIRE_REMOTE \
    PIPEWIRE_RUNTIME_DIR DBUS_SESSION_BUS_ADDRESS XAUTHORITY
export XDG_RUNTIME_DIR="$scratch/runtime"
export XDG_CONFIG_HOME="$scratch/config"
export XDG_STATE_HOME="$scratch/state"
# Without this, the first GIO user activates gvfsd on the private bus, which
# then complains on stderr when the bus goes away.
export GIO_USE_VFS=local
export ISOLATED_RUN_SCRATCH="$scratch"
export ISOLATED_RUN_INNER=1

gst_system=$(pkg-config --variable=pluginsdir gstreamer-1.0 2>/dev/null ||
    echo /usr/lib/x86_64-linux-gnu/gstreamer-1.0)
mkdir -p "$scratch/gst-plugins"
for plugin in "$gst_system"/libgst*.so; do
    case ${plugin##*/} in
    libgstvideo4linux2.so | libgstv4l2codecs.so | libgstlibcamera.so | \
        libgstuvch264.so | libgstoss4.so | libgstossaudio.so | libgstdecklink.so) ;;
    *) ln -s "$plugin" "$scratch/gst-plugins/" ;;
    esac
done
export GST_PLUGIN_SYSTEM_PATH_1_0="$scratch/gst-plugins"
export GST_REGISTRY_1_0="$scratch/gst-registry.bin"
unset GST_PLUGIN_PATH GST_PLUGIN_PATH_1_0 GST_PLUGIN_SYSTEM_PATH GST_REGISTRY

dbus-run-session --config-file="$scratch/dbus-session.conf" -- "$0" "$@"
