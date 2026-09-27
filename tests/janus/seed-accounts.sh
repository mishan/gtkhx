#!/usr/bin/env bash
#
# seed-accounts.sh — edit the bundled guest / admin account YAMLs at
# image-build time so the test suites can use them.
#
# admin gets the empty password. Janus's plain (non-HOPE) login
# checks the password field as it arrives on the wire, without
# undoing the XOR-0xFF obfuscation every Hotline client applies to
# it, so a non-empty password never matches when a real client
# sends it. The empty password obfuscates to itself and sidesteps
# that; it is also the password HOPE login verifies without a
# stored HOPEPassword blob (Janus computes HMAC(key="", session_key)
# server-side), which is how guest has always logged in over HOPE.
#
# The bcrypt-of-empty hash is copied from guest.yaml, which the
# upstream tarball ships with the empty password, rather than pinned
# here. Any HOPEPassword: line is dropped, since it would hold some
# other password.
#
# Then both accounts get the VoiceChat and SendMedia access bits,
# below.
#
# Janus isn't running during any of this, so nothing writes the
# YAMLs back over the edits.

set -euo pipefail

JANUS_DIR=${JANUS_DIR:-/opt/janus/Server}

empty_hash=$(sed -n 's/^Password:[[:space:]]*//p' "$JANUS_DIR/Users/guest.yaml")
case "$empty_hash" in
    '$2'*) ;;
    *)
        echo "guest.yaml has no bcrypt Password: line to copy" >&2
        cat "$JANUS_DIR/Users/guest.yaml" >&2
        exit 1
        ;;
esac
sed -i -e "s|^Password:.*|Password: ${empty_hash}|" -e '/^HOPEPassword:/d' \
    "$JANUS_DIR/Users/admin.yaml"
if ! grep -qxF "Password: ${empty_hash}" "$JANUS_DIR/Users/admin.yaml"; then
    echo "admin password seed failed" >&2
    cat "$JANUS_DIR/Users/admin.yaml" >&2
    exit 1
fi

# Seed VoiceChat=true onto the bundled guest + admin accounts.
#
# Background: the spec mandates access bit 55
# (HL_ACCESS_VOICE_CHAT) for HTLC_HDR_VOICE_JOIN; the server
# rejects 600 with DATA_ERROR_TEXT ("You are not allowed to
# join voice chat.") when the bit is unset. Unlike the
# chat-history extension, voice has no fallback to a lower-
# privilege bit — bit 55 is the only gate.
#
# The NewUserDefaults block in config.yaml already sets
# VoiceChat: true, which covers any account the integration
# suite creates at runtime via the admin API. But the bundled
# `guest` and `admin` YAMLs ship from the upstream Janus
# tarball with VoiceChat: false, so any voice test that uses
# either of those credentials would bounce off bit 55 unless
# we flip it now.
#
# Strategy: in-place edit of each bundled account YAML. Match
# the documented YAML shape — Janus writes one access bit per
# line as `<BitName>: <bool>` — and flip the booleans. We don't
# trust sed's pattern recall here; we apply it defensively and
# verify with grep, failing the build if the edit didn't take.
for u in guest admin; do
    yaml="$JANUS_DIR/Users/$u.yaml"
    if [ ! -f "$yaml" ]; then
        echo "missing $yaml; cannot seed VoiceChat" >&2
        exit 1
    fi
    # Indent-agnostic match. Upstream Janus has historically shipped
    # the per-bit lines under `Access:` two-space-indented, but newer
    # builds (≥ 2.0.8) write them four-space-indented (verified
    # 2026-06 against the version that pulled CI's container build).
    # Pin to extended-regex `^[[:space:]]*` so future indent changes
    # don't regress this script silently.
    #
    # Hazard the earlier two-space-only version masked: VoiceChat
    # happens to default `true` in upstream Janus's `NewUserDefaults`
    # now, so the verify grep below was passing-by-accident — the sed
    # never matched. Discovered while debugging the matching
    # SendMedia seed below, which is genuinely `false` upstream and
    # therefore doesn't have the same false-positive shield.
    sed -i -E \
        "s/^([[:space:]]*VoiceChat:)[[:space:]]+false[[:space:]]*$/\\1 true/" \
        "$yaml"
    if ! grep -E '^[[:space:]]*VoiceChat:[[:space:]]+true' "$yaml" \
            >/dev/null; then
        echo "VoiceChat seed failed for $u" >&2
        echo "--- $yaml ---" >&2
        cat "$yaml" >&2
        exit 1
    fi

    # Flip SendMedia: true onto the same
    # bundled accounts so the inline-media full-round-trip tests
    # (upload + chat-with-handle + relay + download) can run end-
    # to-end. The NewUserDefaults block in config.yaml already
    # carries SendMedia: true for any account created at runtime;
    # this is the matching patch for the pre-shipped guest /
    # admin YAMLs. Same defensive grep-and-verify shape as the
    # VoiceChat seed above.
    sed -i -E \
        "s/^([[:space:]]*SendMedia:)[[:space:]]+false[[:space:]]*$/\\1 true/" \
        "$yaml"
    if ! grep -E '^[[:space:]]*SendMedia:[[:space:]]+true' "$yaml" \
            >/dev/null; then
        echo "SendMedia seed failed for $u" >&2
        echo "--- $yaml ---" >&2
        cat "$yaml" >&2
        exit 1
    fi
done

echo "account seed OK (admin: empty password; VoiceChat + SendMedia on guest + admin)"
