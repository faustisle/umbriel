#!/usr/bin/env bash
# [general] force_tearing requests asynchronous presentation for ordinary desktop frames. It must bypass output opt-in,
# the fullscreen requirement, client hints, and window-rule vetoes. The headless backend accepts the tearing state
# without performing a physical page flip, so this observes the exact output state Umbriel submitted.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly BASELINE="$(< "$UMBRIEL_CONFIG")"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/global-tearing.log"
CLIENT_PID=

if [[ ! -x $CLIENT ]]; then
  echo "unmap-client is not built"
  exit 1
fi

cleanup() {
  if [[ -n $CLIENT_PID ]]; then
    kill -TERM "$CLIENT_PID" 2>/dev/null || true
    wait "$CLIENT_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

write_config() {
  local force=$1
  local extra=${2:-}
  {
    awk -v force="$force" '
      /^\[general\]$/ { print; print "force_tearing = " force; next }
      /^force_tearing *=/ { next }
      { print }
    ' <<< "$BASELINE"
    if [[ -n $extra ]]; then
      printf '\n%s\n' "$extra"
    fi
  } > "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
}

wait_for_state() {
  local filter=$1
  local state=
  for _ in $(seq 100); do
    state=$("$UMBRIEL" tearing --json)
    if jq -e "$filter" <<< "$state" > /dev/null; then
      printf '%s\n' "$state"
      return 0
    fi
    sleep 0.05
  done
  printf 'tearing state did not settle for filter:\n%s\nlast state: %s\n' "$filter" "$state" >&2
  return 1
}

start_client() {
  env TEARING_HINT=vsync REDRAW_ON_CLOSE=1 APP_ID=global-tearing \
    "$CLIENT" global-tearing > "$CLIENT_LOG" 2>&1 &
  CLIENT_PID=$!
  for _ in $(seq 80); do
    grep -q '^mapped$' "$CLIENT_LOG" && return 0
    if ! kill -0 "$CLIENT_PID" 2>/dev/null; then
      wait "$CLIENT_PID" 2>/dev/null || true
      echo "client exited before mapping: $(< "$CLIENT_LOG")"
      return 1
    fi
    sleep 0.05
  done
  echo "client did not map: $(< "$CLIENT_LOG")"
  return 1
}

redraw_client() {
  local before count
  before=$(grep -c '^redrawn$' "$CLIENT_LOG" || true)
  "$UMBRIEL" msg window-close > /dev/null
  for _ in $(seq 80); do
    count=$(grep -c '^redrawn$' "$CLIENT_LOG" || true)
    if ((count > before)); then
      return 0
    fi
    if ! kill -0 "$CLIENT_PID" 2>/dev/null; then
      wait "$CLIENT_PID" 2>/dev/null || true
      echo "client exited before redraw: $(< "$CLIENT_LOG")"
      return 1
    fi
    sleep 0.05
  done
  echo "client did not redraw after close request: $(< "$CLIENT_LOG")"
  return 1
}

start_client
wait_for_state '
  (.surfaces | length) == 1
  and .surfaces[0].title == "global-tearing"
  and .surfaces[0].fullscreen == false
' > /dev/null
redraw_client

wait_for_state '
  .outputs[0].forced == false
  and .outputs[0].allowed == false
  and .outputs[0].requested == false
  and .outputs[0].last_commit_tearing == false
  and .surfaces[0].eligible == false
' > /dev/null

write_config true
redraw_client

wait_for_state '
  .outputs[0].forced == true
  and .outputs[0].allowed == false
  and .outputs[0].requested == true
  and .outputs[0].last_commit_tearing == true
  and .outputs[0].fallback_reason == ""
  and .surfaces[0].fullscreen == false
  and .surfaces[0].hint == "vsync"
  and .surfaces[0].eligible == false
' > /dev/null

tearing_human=$("$UMBRIEL" tearing)
if ! grep -F "output HEADLESS-1: allowed no, requested yes, last commit async" <<< "$tearing_human" > /dev/null \
    || ! grep -F "(global force)" <<< "$tearing_human" > /dev/null; then
  echo "unexpected human-readable global tearing diagnostics: $tearing_human"
  exit 1
fi

# Global force is an override: an output opt-out and a window-rule veto must not disable it.
write_config true '[output.HEADLESS-1]
tearing = false

[[window_rule]]
match.app_id = "^global-tearing$"
tearing = false'
redraw_client

wait_for_state '
  .outputs[0].forced == true
  and .outputs[0].allowed == false
  and .outputs[0].requested == true
  and .outputs[0].last_commit_tearing == true
  and .outputs[0].fallback_reason == ""
  and .surfaces[0].rule_override == false
  and .surfaces[0].eligible == false
' > /dev/null

write_config false
redraw_client

wait_for_state '
  .outputs[0].forced == false
  and .outputs[0].requested == false
  and .outputs[0].last_commit_tearing == false
' > /dev/null

echo "global force requests async presentation for a windowed desktop and overrides output and rule vetoes"
