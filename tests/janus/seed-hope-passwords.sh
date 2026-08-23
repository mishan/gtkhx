#!/usr/bin/env bash
#
# seed-hope-passwords.sh — start Janus once at image-build time,
# PATCH the admin password via the REST admin API so the bundled
# admin.yaml lands in the image with a HOPEPassword: field, then
# shut Janus down.
#
# Background: Janus stores TWO password representations per account
# — a bcrypt hash (legacy login) and a separately-encrypted
# HOPEPassword blob.
#
# Empirically, two paths land at a server that accepts HOPE login:
#
#   1. Account with empty password — bcrypt-of-empty in the YAML,
#      no HOPEPassword field. Janus's HOPE login handler computes
#      HMAC(key="", msg=session_key) server-side and compares.
#      Works without any seed step. This is how hotline.vespernet
#      .net's guest account is configured (verified by running
#      production GtkHx against it with cipher=CHACHA20-POLY1305).
#
#   2. Account with non-empty password seeded via PATCH /accounts/
#      {login}. The API claims to set HOPEPassword (response says
#      `has_hope_password: true` and a HOPEPassword: value lands
#      in the YAML), but the encrypted blob produced via the API
#      doesn't validate at HOPE-login time on container restart
#      — Janus logs "HOPE authentication failed" and the client
#      gets task-error "Incorrect login". I couldn't establish
#      whether this is a Janus bug or a documented intentional
#      gap; the empty-password path works reliably either way.
#
# So: we LEAVE guest alone (upstream ships bcrypt-of-empty + no
# HOPEPassword, which is exactly path #1) — test_hope_chacha20
# uses guest with password="" matching production's
# hotline.vespernet.net flow. We only seed admin, with a non-
# empty password, in case future tests need a non-empty HOPE
# password — and we deliberately tolerate Janus's API-PATCH
# path being flaky here, since no test depends on admin's
# HOPEPassword validating today.
#
# Sequence:
#   1. Launch Janus in the background. config.yaml has
#      EnableHOPE: true + APIAddr: ":8973" + a known build-seed-key
#      in APIKeys, so this just works.
#   2. Poll the GET endpoint until the API listener is up (Janus
#      takes ~1-2s in practice; we cap at 30s).
#   3. PATCH admin with password "adminpass". This writes a
#      HOPEPassword field into the YAML and nothing else — it does
#      NOT update the `Password:` bcrypt hash a non-HOPE login
#      checks, which is why admin/adminpass never logged in on any
#      path. The base image (hotline-docker/images/janus) now writes
#      that field directly; this PATCH is only here for the HOPE
#      blob.
#   4. Send SIGTERM to Janus and wait for it to flush — some YAML
#      rewrites happen on shutdown rather than synchronously
#      inside the PATCH handler.
#   5. Verify admin.yaml now has HOPEPassword: (best-effort —
#      fast-fail if the API path silently no-ops, but don't
#      assert anything about whether it'll actually validate).
#
# Lives in its own file (rather than inline in the Dockerfile)
# because Dockerfile RUN steps mixing `&` (background) with
# `&&` (conditional chain) silently misparse — the original
# inline version stopped Janus before the curl PATCHes could
# land, and the build still passed. A standalone script with
# `set -euo pipefail` makes the control flow auditable.

set -euo pipefail

JANUS_BIN=${JANUS_BIN:-/opt/janus/janus}
JANUS_DIR=${JANUS_DIR:-/opt/janus/Server}
API_URL=${API_URL:-http://127.0.0.1:8973}
API_KEY=${API_KEY:-build-seed-key}
LOG=${LOG:-/tmp/janus-seed.log}

"$JANUS_BIN" >"$LOG" 2>&1 &
JPID=$!

cleanup() {
    if kill -0 "$JPID" 2>/dev/null; then
        kill -TERM "$JPID" 2>/dev/null || true
        wait "$JPID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# Wait for the API listener (Janus binds it after the main listener
# is up, so a successful GET means the whole server is running).
api_up=0
for _ in $(seq 1 30); do
    if curl -fsS -H "X-API-Key: $API_KEY" \
            "$API_URL/api/v2/accounts/guest" >/dev/null 2>&1; then
        api_up=1
        break
    fi
    sleep 1
done

if [ "$api_up" = 0 ]; then
    echo "Janus API did not come up on :8973 after 30s" >&2
    echo "--- janus log tail ---" >&2
    tail -40 "$LOG" >&2
    exit 1
fi

# No account passwords are set here, and that is the fix rather than an
# omission. Passwords come from the account YAMLs the base image ships
# (hotline-docker/images/janus writes admin's bcrypt directly); this
# script only boots Janus far enough to generate Server/Data and then
# edits access bits.
#
# The admin REST API used to set admin's password here. It does not work:
# an account the API has touched cannot log in at all. Measured against a
# running container — a throwaway account created through the API with a
# known password was rejected with "Incorrect login." for that password,
# and PATCHing it to an empty password left has_hope_password true and
# still rejected. `guest`, the one account the API has never touched, logs
# in fine. So the PATCH was not merely failing to set the password, it was
# poisoning the account, which is why admin/adminpass never worked on any
# path including plain login.
#
# Whether the trigger is the HOPEPassword blob itself or something else
# the API writes, we could not separate — so the blob gets stripped below
# as well, restoring the shape guest is known to work with.

# Explicit shutdown before touching the YAMLs, so Janus is not going to
# write them back underneath us.
kill -TERM "$JPID"
wait "$JPID" 2>/dev/null || true
trap - EXIT

# Strip any HOPEPassword blob and require a bcrypt Password to survive.
# Nothing in this script writes a blob now, but a future base image or a
# stray API call would reintroduce the exact state that makes an account
# unloggable, and that failure is invisible until someone tries to log in.
for u in guest admin; do
    yaml="$JANUS_DIR/Users/$u.yaml"
    sed -i '/^HOPEPassword:/d' "$yaml"
    if grep -q '^HOPEPassword:' "$yaml"; then
        echo "failed to strip HOPEPassword from $yaml" >&2
        exit 1
    fi
    if ! grep -q '^Password:' "$yaml"; then
        echo "$yaml has no Password: line — the account cannot log in" >&2
        cat "$yaml" >&2
        exit 1
    fi
done

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
# Strategy: in-place YAML edit on each bundled account
# YAML after the seed-time Janus instance has shut down (so
# Janus isn't going to write the YAML back). Match the
# documented YAML shape — Janus writes one access bit per
# line as `<BitName>: <bool>` — and flip the booleans. We
# don't trust sed's pattern recall here; we apply it
# defensively and verify with grep, failing the build if the
# edit didn't take.
for u in guest admin; do
    yaml="$JANUS_DIR/Users/$u.yaml"
    if [ ! -f "$yaml" ]; then
        echo "missing $yaml after Janus first-run; cannot seed VoiceChat" >&2
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
        echo "--- janus log tail ---" >&2
        tail -40 "$LOG" >&2
        exit 1
    fi

    # Phase 9.F follow-up: flip SendMedia: true onto the same
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
        echo "--- janus log tail ---" >&2
        tail -40 "$LOG" >&2
        exit 1
    fi
done

echo "account seed OK (passwords come from the base image YAMLs)"
grep -E '^Login|^Password' \
    "$JANUS_DIR/Users/guest.yaml" \
    "$JANUS_DIR/Users/admin.yaml"
echo "VoiceChat seed OK (bit 55 set on guest + admin)"
