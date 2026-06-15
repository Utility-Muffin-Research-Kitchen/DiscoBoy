#!/bin/sh
set -eu
PAK_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

# Source the Leaf platform env (paths, runtime dirs) when present.
PLATFORM="${PLATFORM:-mlp1}"
for root in "${SDCARD_PATH:-/mnt/sdcard}" /mnt/sdcard /media/sdcard1; do
  env_sh="$root/.system/leaf/platforms/$PLATFORM/launcher/env.sh"
  if [ -f "$env_sh" ]; then . "$env_sh"; break; fi
done

BIN="$PAK_DIR/bin/discoboy"
export DISCOBOY_PAK_DIR="$PAK_DIR"
LOG_DIR="${LOGS_PATH:-$PAK_DIR}"
mkdir -p "$LOG_DIR" 2>/dev/null || true

cd "$PAK_DIR"
exec "$BIN" 2>"$LOG_DIR/discoboy.log"
