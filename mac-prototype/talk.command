#!/bin/bash
# talk — one double-click: bring up the BLE link, then converse.
#
# WHY A .command AND NOT A SHELL FUNCTION
#   macOS attributes Bluetooth to the RESPONSIBLE process. Launched from
#   Finder, launchd spawns this and Terminal is responsible, so CoreBluetooth
#   permits it. The same commands run from an agent-spawned shell are killed
#   (claude.app has no usage description). This is why the agent cannot start
#   the link for you — see CLAUDE.md.
#
# Ctrl-C stops it. This is a real terminal, so that actually works.
set -uo pipefail
cd "$(dirname "$0")"

CEILING="${CEILING:-stance}"          # read | leds | audio | dome | stance
TURNS="${TURNS:-8}"
DEVICE="${DEVICE:-BRIO}"
EXTRA="${EXTRA:---animate}"

# --- credentials -----------------------------------------------------------
if [[ ! -f ../.env ]]; then
  echo "no ../.env — copy .env.example and fill in LLM_API_KEY." >&2; exit 1
fi
set -a; source ../.env; set +a
if [[ -z "${LLM_API_KEY:-}" ]]; then
  echo "LLM_API_KEY is empty in ../.env — reasoning and speech need it." >&2; exit 1
fi

# --- the link --------------------------------------------------------------
# A stale daemon holds the radio and the new one fails with a misleading
# "R2-D2 not found", so check before launching rather than after.
if pgrep -f "r2_probe.py daemon" >/dev/null; then
  HELD="$(python3 -c 'import json;print(json.load(open(".bridge/daemon.lock"))["ceiling"])' 2>/dev/null || echo unknown)"
  echo "daemon already running at ceiling '$HELD' — reusing it."
  [[ "$HELD" != "$CEILING" ]] && echo "  (wanted '$CEILING'; kill it and re-run to change)"
else
  echo "starting daemon at ceiling '$CEILING'…"
  ./r2 daemon --allow "$CEILING" &
  DAEMON_PID=$!
  trap 'kill $DAEMON_PID 2>/dev/null' EXIT
  for _ in $(seq 1 20); do
    [[ -f .bridge/daemon.lock ]] && break
    sleep 1
  done
  [[ -f .bridge/daemon.lock ]] || { echo "daemon never came up." >&2; exit 1; }
  sleep 2
fi

# --- talk ------------------------------------------------------------------
echo
exec .venv/bin/python voice/converse.py \
  --turns "$TURNS" --device "$DEVICE" --send "$CEILING" \
  --bridge-dir "$PWD/.bridge" $EXTRA
