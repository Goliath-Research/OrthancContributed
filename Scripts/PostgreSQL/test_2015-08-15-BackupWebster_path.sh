#!/usr/bin/env bash
# Spec tests for Scripts/PostgreSQL/2015-08-15-BackupWebster.sh.
#
# A PostgreSQL role that can create databases, and that cannot write server
# files itself, chooses database names. The backup script must not create,
# truncate, or replace a file outside /data/backups/postgres-backup/ because
# of those names. A name made only of letters, digits, and underscores must
# still be dumped inside that directory.
#
# These assertions fail while the script copies database names into the dump
# path. They pass once that write stays inside the backup directory.
set -u

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SCRIPT="$SCRIPT_DIR/2015-08-15-BackupWebster.sh"
BACKUP_DIR=/data/backups/postgres-backup
LOG_DIR=/Orthanc/scripts
LOG_FILE=$LOG_DIR/dbbackup.log
ESCAPE_DIR=/tmp/orthanc-trav-test
DIRECT_NAME="../../../tmp/orthanc-trav-test/direct"
SPACED_NAME=".. / .. / .. / tmp / orthanc-trav-test / spaced"
SPACED_STRIPPED="../../../tmp/orthanc-trav-test/spaced"
SAFE_NAME="orthanc_ok"
SAFE_MARKER="safe-marker-orthanc-backup"
DIRECT_MARKER="direct-marker-orthanc-backup"
WORKDIR=
PG_CTL=
PGDATA=
PGSOCK=
PGPORT=
failures=0

die() {
  echo "SETUP ERROR: $*" >&2
  exit 2
}

cleanup() {
  if [[ -n "${PG_CTL:-}" && -n "${PGDATA:-}" && -d "$PGDATA" ]]; then
    "$PG_CTL" -D "$PGDATA" -m immediate stop >/dev/null 2>&1 || true
  fi
  if [[ -n "${WORKDIR:-}" && -d "$WORKDIR" ]]; then
    rm -rf "$WORKDIR"
  fi
  if [[ -d "$ESCAPE_DIR" ]]; then
    rm -rf "$ESCAPE_DIR"
  fi
  if [[ -d "$BACKUP_DIR" ]]; then
    find "$BACKUP_DIR" -maxdepth 1 -type f -name '*-pg_dump.*.gz' -delete 2>/dev/null || true
  fi
  rm -f "$LOG_FILE" "$LOG_DIR/trimmed.log" 2>/dev/null || true
}
trap cleanup EXIT

[[ -f "$SCRIPT" ]] || die "backup script not found at $SCRIPT"
command -v psql >/dev/null || die "psql is not installed"
command -v pg_dump >/dev/null || die "pg_dump is not installed"
command -v pg_restore >/dev/null || die "pg_restore is not installed"
command -v pg_config >/dev/null || die "pg_config is not installed"
PG_BIN=$(pg_config --bindir)
INITDB=$PG_BIN/initdb
PG_CTL=$PG_BIN/pg_ctl
[[ -x "$INITDB" && -x "$PG_CTL" ]] || die "initdb or pg_ctl missing under $PG_BIN"

WORKDIR=$(mktemp -d /tmp/orthanc-backup-test.XXXXXX)
PGDATA=$WORKDIR/data
PGSOCK=$WORKDIR/sock
mkdir -p "$PGSOCK" "$WORKDIR/cwd"
PGPORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')

"$INITDB" -D "$PGDATA" --auth=trust --username="$(id -un)" --no-instructions >/dev/null
"$PG_CTL" -D "$PGDATA" -w -o "-k ${PGSOCK} -p ${PGPORT} -c listen_addresses=" start >/dev/null

export PGHOST="$PGSOCK"
export PGPORT
export PGUSER="$(id -un)"
unset PGDATABASE PGSERVICE PGOPTIONS PGAPPNAME || true

psql -d template1 -v ON_ERROR_STOP=1 -c "SELECT 1" >/dev/null \
  || die "cannot connect to the test cluster"

psql -d template1 -v ON_ERROR_STOP=1 <<SQL >/dev/null
CREATE ROLE app_creator LOGIN CREATEDB NOSUPERUSER NOCREATEROLE NOREPLICATION;
SQL

# The attacker role must not already be able to write a server-side file.
rm -f "$WORKDIR/copy-probe"
set +e
copy_out=$(psql -d template1 -U app_creator -c "COPY (SELECT 1) TO '${WORKDIR}/copy-probe';" 2>&1)
copy_status=$?
set -e
if [[ "$copy_status" -eq 0 || -e "$WORKDIR/copy-probe" ]]; then
  die "app_creator can COPY to a server file; the privilege boundary is not set up"
fi
if ! grep -q "permission denied" <<<"$copy_out"; then
  echo "$copy_out" >&2
  die "expected COPY TO file to be denied for app_creator"
fi

psql -d template1 -U app_creator -v ON_ERROR_STOP=1 <<SQL >/dev/null
CREATE DATABASE "${DIRECT_NAME}";
CREATE DATABASE "${SPACED_NAME}";
CREATE DATABASE ${SAFE_NAME};
SQL

psql -d "$DIRECT_NAME" -U app_creator -v ON_ERROR_STOP=1 \
  -c "CREATE TABLE marker (v text); INSERT INTO marker VALUES ('${DIRECT_MARKER}');" >/dev/null
psql -d "$SAFE_NAME" -U app_creator -v ON_ERROR_STOP=1 \
  -c "CREATE TABLE marker (v text); INSERT INTO marker VALUES ('${SAFE_MARKER}');" >/dev/null

echo "---- attacker role ----"
psql -d template1 -c "SELECT rolname, rolsuper, rolcreatedb FROM pg_roles WHERE rolname = 'app_creator';"
echo "---- database owners ----"
psql -d template1 -c "SELECT datname, pg_catalog.pg_get_userbyid(datdba) AS owner FROM pg_database WHERE datname IN ('${DIRECT_NAME}', '${SPACED_NAME}', '${SAFE_NAME}');"

# Same listing pipeline as the script, used only to prove the names reached psql.
listed=$(psql -lqt | grep -vE '^ +(template[0-9]+|postgres)? *\|' | cut -d'|' -f1 | sed -e 's/ //g' -e '/^$/d')
grep -Fxq "$DIRECT_NAME" <<<"$listed" || die "direct traversal name was not listed as $DIRECT_NAME"
grep -Fxq "$SPACED_STRIPPED" <<<"$listed" || die "spaced name was not listed as $SPACED_STRIPPED after space removal"
grep -Fxq "$SAFE_NAME" <<<"$listed" || die "safe database name was not listed"

python3 - "$DIRECT_NAME" "$SPACED_NAME" <<'PY' || die "database name exceeds 63 bytes"
import sys
for name in sys.argv[1:]:
    if len(name.encode()) > 63:
        sys.exit(1)
PY

if [[ ! -w "$BACKUP_DIR" || ! -w "$LOG_DIR" ]]; then
  sudo mkdir -p "$BACKUP_DIR" "$LOG_DIR"
  sudo chown "$(id -un)" "$BACKUP_DIR" "$LOG_DIR"
fi
[[ -w "$BACKUP_DIR" && -w "$LOG_DIR" ]] || die "backup directory and log directory are not writable"
: >"$LOG_FILE"

rm -rf "$ESCAPE_DIR"
mkdir -p "$ESCAPE_DIR"
# Sentinel files whose exact paths the vulnerable script replaces.
date_stamp=$(date +%y%m%d%H%M)
direct_target="$ESCAPE_DIR/direct-pg_dump.${date_stamp}.gz"
spaced_target="$ESCAPE_DIR/spaced-pg_dump.${date_stamp}.gz"
printf 'sentinel-direct\n' >"$direct_target"
printf 'sentinel-spaced\n' >"$spaced_target"
chmod 644 "$direct_target" "$spaced_target"

snapshot() {
  local dir="$1"
  find "$dir" -type f -printf '%P\n' | sort | while read -r rel; do
    printf '%s ' "$rel"
    sha256sum "$dir/$rel" | awk '{print $1}'
  done
}

before_escape=$(snapshot "$ESCAPE_DIR")
before_backup=$(snapshot "$BACKUP_DIR")

(
  cd "$WORKDIR/cwd"
  bash "$SCRIPT"
) >"$WORKDIR/script.out" 2>"$WORKDIR/script.err" || true

after_escape=$(snapshot "$ESCAPE_DIR")
after_backup=$(snapshot "$BACKUP_DIR")

echo "---- script stdout ----"
cat "$WORKDIR/script.out"
echo "---- script stderr ----"
cat "$WORKDIR/script.err"
echo "---- escape directory ----"
find "$ESCAPE_DIR" -type f -printf '%p mode=%m bytes=%s\n' | sort

describe_outside() {
  local target="$1"
  [[ -f "$target" ]] || return 0
  echo "stat $target: $(stat -c 'mode=%a owner=%U bytes=%s' "$target")"
  rm -f "$WORKDIR/outside.sql"
  if gzip -t "$target" 2>/dev/null; then
    gzip -dc "$target" >"$WORKDIR/outside.bin" || return 0
    if pg_restore -f "$WORKDIR/outside.sql" "$WORKDIR/outside.bin" 2>"$WORKDIR/outside.restore"; then
      if grep -q "$DIRECT_MARKER" "$WORKDIR/outside.sql"; then
        echo "outside file contains the direct database marker"
      fi
      if grep -q "$SAFE_MARKER" "$WORKDIR/outside.sql"; then
        echo "outside file contains the safe database marker"
      fi
    else
      echo "outside file is gzip but not a pg_restore archive ($(wc -c <"$WORKDIR/outside.bin") bytes uncompressed)"
    fi
  else
    echo "outside file is not gzip; first line: $(head -n 1 "$target")"
  fi
}

check_escape_unchanged() {
  local label="$1"
  if [[ "$before_escape" == "$after_escape" ]]; then
    echo "PASS: $label"
    return 0
  fi
  echo "FAIL: $label"
  echo "Escape directory changed. Before:"
  printf '%s\n' "$before_escape"
  echo "After:"
  printf '%s\n' "$after_escape"
  describe_outside "$direct_target"
  describe_outside "$spaced_target"
  failures=$((failures + 1))
}

check_escape_unchanged "database names that traverse out of the backup directory do not create or replace files"

safe_inside=0
while IFS= read -r rel; do
  [[ -n "$rel" ]] || continue
  if grep -Fxq "$rel" <<<"$before_backup"; then
    continue
  fi
  file="$BACKUP_DIR/$rel"
  resolved=$(realpath -m "$file")
  case "$resolved" in
    "$(realpath -m "$BACKUP_DIR")"/*) ;;
    *)
      echo "FAIL: new backup file resolves outside the backup directory: $resolved"
      failures=$((failures + 1))
      continue
      ;;
  esac
  gzip -dc "$file" >"$WORKDIR/inside.bin" 2>/dev/null || continue
  pg_restore -f "$WORKDIR/inside.sql" "$WORKDIR/inside.bin" 2>/dev/null || true
  if grep -q "$SAFE_MARKER" "$WORKDIR/inside.sql" 2>/dev/null; then
    safe_inside=1
    echo "PASS: safe database dump is inside the backup directory ($resolved)"
  fi
  if grep -q "$DIRECT_MARKER" "$WORKDIR/inside.sql" 2>/dev/null; then
    echo "NOTE: direct database was dumped inside the backup directory ($resolved)"
  fi
done < <(snapshot "$BACKUP_DIR" | awk '{print $1}')

if [[ "$safe_inside" -ne 1 ]]; then
  echo "FAIL: safe database name was not dumped inside the backup directory"
  failures=$((failures + 1))
fi

if [[ "$failures" -ne 0 ]]; then
  echo "$failures assertion(s) failed"
  exit 1
fi
echo "All backup path assertions passed"
exit 0
