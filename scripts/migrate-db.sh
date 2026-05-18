#!/bin/sh
# bms_bridge database / config migration.
#
# Run once, out of process, from the Debian postinst. The binary itself
# never mutates an existing DB schema or the operator's config — all
# migration policy lives here.
#
# What it does (all steps idempotent and safe to re-run):
#   1. Locate the active config and the history DB it points at.
#   2. If the DB exists, add columns introduced by newer versions
#      (ADD COLUMN is ignored if the column is already present).
#   3. If the config is still in the pre-1.7 single-battery format,
#      drop a new-format template next to it as *.conf.new and warn.
#      The operator's file is never modified or replaced — choosing
#      battery names is their decision, not the package's.
#
# Legacy rows keep battery_id = '' on purpose: that empty id is the
# "legacy battery", surfaced as its own tab in the dashboard. No row
# backfill is done here.

set -eu

CONFIG_FILE="${1:-/etc/bms_bridge/bms_bridge.conf}"
TEMPLATE_FILE="${2:-/usr/share/bms_bridge/bms_bridge.conf.template}"
DEFAULT_DB="/var/lib/bms_bridge/history.sqlite3"

log() { echo "bms_bridge migrate: $*"; }

# ---- config: detect format, offer new template ----------------------------

config_is_new_format() {
    # New format has at least one  batery "<name>" {  section header.
    grep -Eq '^[[:space:]]*batery[[:space:]]+"' "$CONFIG_FILE"
}

if [ -f "$CONFIG_FILE" ]; then
    if config_is_new_format; then
        log "config $CONFIG_FILE is already in the new format"
    else
        NEW="$CONFIG_FILE.new"
        if [ -f "$TEMPLATE_FILE" ]; then
            cp "$TEMPLATE_FILE" "$NEW"
            log "WARNING: $CONFIG_FILE uses the old single-battery format."
            log "WARNING: a new-format template was written to $NEW."
            log "WARNING: the daemon will NOT start until you migrate your"
            log "WARNING: settings into the new format (battery names are"
            log "WARNING: your choice). Your existing config was left intact."
        else
            log "WARNING: $CONFIG_FILE is old-format but no template found at"
            log "WARNING: $TEMPLATE_FILE; cannot drop a .new sample."
        fi
    fi
else
    log "no config at $CONFIG_FILE yet (fresh install); nothing to migrate"
fi

# ---- find the history DB --------------------------------------------------

# Pull the first uncommented db_path / history_db_path value (both contain
# the substring "db_path"); fall back to the packaged default.
DB_PATH="$DEFAULT_DB"
if [ -f "$CONFIG_FILE" ]; then
    found=$(awk -F'"' '
        /^[[:space:]]*#/ { next }
        /db_path[[:space:]]*=/ { print $2; exit }
    ' "$CONFIG_FILE" 2>/dev/null || true)
    [ -n "${found:-}" ] && DB_PATH="$found"
fi

if [ ! -f "$DB_PATH" ]; then
    log "no DB at $DB_PATH; schema will be created by the daemon on first run"
    exit 0
fi

if ! command -v sqlite3 >/dev/null 2>&1; then
    log "WARNING: sqlite3 not found; skipping DB schema migration of $DB_PATH"
    exit 0
fi

# ---- DB: migrate to the normalized multi-battery schema -------------------
# All statements are idempotent. ALTER TABLE ADD COLUMN errors out if the
# column already exists — that is the expected outcome on re-run, so its
# failure is swallowed; the others use IF NOT EXISTS / OR IGNORE.

run()   { sqlite3 "$DB_PATH" ".timeout 5000" "$1" >/dev/null 2>&1; }
quiet() { run "$1" || true; }

log "migrating DB schema in $DB_PATH"

# 1. batteries table + the legacy battery (id=1, empty name). Pre-multi-
#    battery rows all belong to it; an absent ?battery= request resolves
#    here, so the old dashboard keeps showing the old data.
quiet "CREATE TABLE IF NOT EXISTS batteries (
           id   INTEGER PRIMARY KEY AUTOINCREMENT,
           name TEXT NOT NULL UNIQUE)"
quiet "INSERT OR IGNORE INTO batteries (id, name) VALUES (1, '')"

# 2. columns added in later versions. ADD COLUMN can't carry a FK clause;
#    integrity is enforced by the app. All existing rows -> legacy (1).
quiet "ALTER TABLE samples ADD COLUMN balance_current_ma INTEGER NOT NULL DEFAULT 0"
quiet "ALTER TABLE samples ADD COLUMN battery_id INTEGER NOT NULL DEFAULT 1"

# 3. lookup index for range queries (battery_id, ts_ms).
quiet "CREATE INDEX IF NOT EXISTS idx_samples_bat_ts ON samples(battery_id, ts_ms)"

log "DB schema migration done"
exit 0
