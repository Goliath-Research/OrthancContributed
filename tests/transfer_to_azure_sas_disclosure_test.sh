#!/usr/bin/env bash
# Checks that Scripts/Migration/2020-09-08-TransferToAzure.sh does not disclose
# the SAS token it is given with -t.
#
# The token is an obvious placeholder. These checks fail while the script writes
# that placeholder to captured output or to an azcopy command line another local
# user can read. They pass once the copy still runs and those channels do not
# contain the placeholder.
#
# The script process's own arguments are not an assertion target: the kernel
# records whatever this test passes with -t, and the script cannot clear that.
# The assertions cover the channels the script creates: standard output,
# standard error, descendant azcopy command lines, and files another user can read.

set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT="${ROOT}/Scripts/Migration/2020-09-08-TransferToAzure.sh"
TOKEN="PLACEHOLDER_SAS_TOKEN"
ACCOUNT="PLACEHOLDER_ACCOUNT"
CONTAINER="PLACEHOLDER_CONTAINER"
OBSERVER="sasobserver"

failures=0
cleanup_paths=()

cleanup() {
  local p
  for p in "${cleanup_paths[@]}"; do
    rm -rf -- "$p" 2>/dev/null || true
  done
}
trap cleanup EXIT

fail() {
  printf 'FAIL: %s\n' "$*"
  failures=$((failures + 1))
}

pass() {
  printf 'PASS: %s\n' "$*"
}

require_observer() {
  if ! id "$OBSERVER" >/dev/null 2>&1; then
    sudo useradd --create-home --shell /usr/sbin/nologin "$OBSERVER"
  fi
  if ! sudo -u "$OBSERVER" true; then
    printf 'FAIL: cannot run commands as %s\n' "$OBSERVER"
    exit 1
  fi
}

install_stub() {
  local bindir="$1"
  local pidfile="$2"
  mkdir -p "$bindir"
  cat > "${bindir}/azcopy" << EOF
#!/usr/bin/env bash
if [ "\${1:-}" = "login" ] || [ "\${1:-}" = "logout" ]; then
  exit 0
fi
echo \$\$ > "${pidfile}"
sleep 2
exit 0
EOF
  chmod 755 "${bindir}/azcopy"
}

prepare_source() {
  local src="$1"
  mkdir -p "${src}/nested/leaf"
  printf 'note\n' > "${src}/note"
  printf 'object\n' > "${src}/nested/leaf/obj"
}

scan_readable_files() {
  local root="$1"
  local dest="$2"
  local f
  [ -d "$root" ] || return 0
  while IFS= read -r -d '' f; do
    if sudo -u "$OBSERVER" test -r "$f" && sudo -u "$OBSERVER" grep -q -F -- "$TOKEN" "$f"; then
      printf '%s\n' "$f" >> "$dest"
    fi
  done < <(find "$root" -type f -print0 2>/dev/null)
}

run_mode() {
  local mode="$1"
  local name="$2"
  local work src home tmp bindir pidfile observed leaked out err
  work="$(mktemp -d)"
  cleanup_paths+=("$work")
  src="${work}/src"
  home="${work}/home"
  tmp="${work}/tmp"
  bindir="${work}/bin"
  pidfile="${work}/azcopy.pid"
  observed="${work}/observed-cmdline.txt"
  leaked="${work}/leaked-files.txt"
  mkdir -p "$src" "$home" "$tmp"
  prepare_source "$src"
  install_stub "$bindir" "$pidfile"
  : > "$observed"
  : > "$leaked"

  out="/tmp/transfer-azure-${mode}-$$.out"
  err="/tmp/transfer-azure-${mode}-$$.err"
  cleanup_paths+=("$out" "$err")
  rm -f "$out" "$err"

  local -a args
  args=(-s "$src" -a "$ACCOUNT" -c "$CONTAINER" -t "$TOKEN")
  if [ "$mode" != "default" ]; then
    args+=(-l "$mode")
  fi

  umask 022
  PATH="${bindir}:${PATH}" HOME="$home" TMPDIR="$tmp" \
    bash "$SCRIPT" "${args[@]}" >"$out" 2>"$err" &
  local spid=$!

  local seen=" "
  local deadline=$((SECONDS + 30))
  while [ "$SECONDS" -lt "$deadline" ]; do
    if [ -s "$pidfile" ]; then
      local apid
      apid="$(cat "$pidfile")"
      if [[ "$seen" != *" ${apid} "* ]]; then
        seen+=" ${apid} "
        if [ -r "/proc/${apid}/cmdline" ] && sudo -u "$OBSERVER" cat "/proc/${apid}/cmdline" > "${work}/raw-${apid}.bin" 2>"${work}/raw-${apid}.err"; then
          tr '\0' ' ' < "${work}/raw-${apid}.bin" >> "$observed"
          printf '\n' >> "$observed"
          scan_readable_files "$tmp" "$leaked"
        elif kill -0 "$apid" 2>/dev/null; then
          printf 'UNREADABLE %s\n' "$apid" >> "$observed"
        fi
        rm -f "$pidfile"
      fi
    fi
    if ! kill -0 "$spid" 2>/dev/null; then
      break
    fi
    sleep 0.05
  done
  wait "$spid" || true
  scan_readable_files "$tmp" "$leaked"

  if sudo -u "$OBSERVER" test -r "$out"; then
    sudo -u "$OBSERVER" cat -- "$out" > "${work}/observer-out.txt"
    if grep -q -F -- "$TOKEN" "${work}/observer-out.txt"; then
      fail "${name}: another local user read the SAS token from captured standard output"
    else
      pass "${name}: captured standard output does not disclose the SAS token"
    fi
  else
    fail "${name}: another local user could not read captured standard output, so the disclosure check did not run"
  fi

  if sudo -u "$OBSERVER" test -r "$err"; then
    sudo -u "$OBSERVER" cat -- "$err" > "${work}/observer-err.txt"
    if grep -q -F -- "$TOKEN" "${work}/observer-err.txt"; then
      fail "${name}: another local user read the SAS token from captured standard error"
    else
      pass "${name}: captured standard error does not disclose the SAS token"
    fi
  else
    fail "${name}: another local user could not read captured standard error, so the disclosure check did not run"
  fi

  if [ -s "$leaked" ]; then
    fail "${name}: another local user read the SAS token from a file created during the migration"
  else
    pass "${name}: no file another local user can read contains the SAS token"
  fi

  if grep -q 'UNREADABLE' "$observed"; then
    fail "${name}: azcopy was running but another local user could not read its command line"
  fi

  if ! grep -q -F -- "$ACCOUNT" "$observed" || ! grep -q -F -- "$CONTAINER" "$observed"; then
    fail "${name}: azcopy was not invoked with the storage account and container"
  elif grep -q -F -- "$TOKEN" "$observed"; then
    fail "${name}: another local user read the SAS token from the azcopy process command line"
  else
    pass "${name}: azcopy process command line does not disclose the SAS token"
  fi
}

if [ ! -f "$SCRIPT" ]; then
  printf 'FAIL: migration script not found at %s\n' "$SCRIPT"
  exit 1
fi

require_observer
run_mode default "default individual copy"
run_mode off "individual copy (-l off)"
run_mode main "batch copy (-l main)"
run_mode sub "batch copy (-l sub)"

if [ "$failures" -ne 0 ]; then
  printf '%s check(s) failed\n' "$failures"
  exit 1
fi
printf 'all checks passed\n'
exit 0
