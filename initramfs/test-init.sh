#!/bin/bash
# Exercises initramfs/init's new diagnostics against mocked dependencies.
# Runs the REAL init file (paths rewritten into a sandbox), not a copy of
# its logic, so it can't drift.
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
pass=0; fail=0
ok()  { printf '  \033[32m[ok]\033[0m %s\n' "$*"; pass=$((pass+1)); }
bad() { printf '  \033[31m[FAIL]\033[0m %s\n' "$*"; fail=$((fail+1)); }

# $1=scenario name, $2=picker behaviour, $3=mount behaviour, $4=discover behaviour
setup() {
  SB=$(mktemp -d); export SB
  mkdir -p "$SB"/bin "$SB"/sbin "$SB"/run/picker "$SB"/mnt/root "$SB"/dev
  # mnt/root/boot exists only if the root mount succeeds - that IS what
  # mounting the real root does. Pre-creating it unconditionally made
  # save_log's "is the root actually mounted?" guard untestable.
  [ "$3" = 0 ] && mkdir -p "$SB"/mnt/root/boot

  # --- mocks -------------------------------------------------------
  cat > "$SB/bin/mount" <<EOF
#!/bin/sh
# remounts always succeed; the initial root mount obeys the scenario
case "\$*" in
  *remount*) exit 0 ;;
  *mmcblk0p2*) exit $3 ;;
esac
exit 0
EOF
  cat > "$SB/bin/sh" <<'EOF'
#!/bin/sh
echo "MARKER_RESCUE_SHELL_REACHED"
exit 0
EOF
  cat > "$SB/bin/mdev" <<'EOF'
#!/bin/sh
exit 0
EOF
  cat > "$SB/bin/dmesg" <<'EOF'
#!/bin/sh
echo "[    0.000000] MARKER_DMESG_LINE"
EOF
  cat > "$SB/sbin/kexec-boot.sh" <<'EOF'
#!/bin/sh
echo "MARKER_KEXEC root=$1 linux=$2"
exit 0
EOF
  cat > "$SB/bin/discover-kernels.sh" <<EOF
#!/bin/sh
[ "$4" = 0 ] || exit 1
printf 'Ubuntu\t/boot/vmlinuz-real\t/boot/initrd.img-real\tro quiet\t\n'
printf 'Ubuntu old\t/boot/vmlinuz-old\t/boot/initrd.img-old\tro quiet\t\n'
EOF
  cat > "$SB/bin/apply-default.sh" <<'EOF'
#!/bin/sh
cat "$1"
EOF
  # picker: $2 selects behaviour
  case "$2" in
    ok)    cat > "$SB/bin/picker" <<'EOF'
#!/bin/sh
echo "picker mock: drew the menu" >&2
echo 'SELECTED_LINUX=/boot/vmlinuz-chosen'
echo 'SELECTED_INITRD=/boot/initrd.img-chosen'
echo 'SELECTED_CMDLINE=ro quiet'
exit 0
EOF
    ;;
    crash) cat > "$SB/bin/picker" <<'EOF'
#!/bin/sh
echo "picker mock: MARKER_DRM_OPEN_FAILED /dev/dri/card0" >&2
exit 3
EOF
    ;;
    empty) cat > "$SB/bin/picker" <<'EOF'
#!/bin/sh
echo "picker mock: exited clean but chose nothing" >&2
exit 0
EOF
    ;;
  esac
  chmod +x "$SB"/bin/* "$SB"/sbin/*

  # --- the real init, redirected into the sandbox -------------------
  sed -e "s|/bin/|$SB/bin/|g" -e "s|/sbin/|$SB/sbin/|g" \
      -e "s|/mnt/root|$SB/mnt/root|g" -e "s|/run/picker|$SB/run/picker|g" \
      "$REPO/initramfs/init" > "$SB/init"
}

# NB: run the interpreter by ABSOLUTE path. $SB/bin/sh is the mocked
# rescue shell and is first in PATH, so a bare `sh` here silently runs
# the mock instead of init - which looks exactly like "init dropped to
# rescue immediately" and cost a debugging round.
run() { PATH="$SB/bin:$SB/sbin:$PATH" PICKER_FALLBACK_PAUSE=0 \
          /bin/sh "$SB/init" >"$SB/out" 2>"$SB/err"; echo $?; }

log() { cat "$SB/mnt/root/boot/picker-last-boot.log" 2>/dev/null; }
both() { cat "$SB/out" "$SB/err" 2>/dev/null; }

echo "=== 1. happy path: picker returns a selection ==="
setup happy ok 0 0; run >/dev/null
both | grep -q "MARKER_KEXEC.*vmlinuz-chosen" && ok "kexecs the user's choice" || bad "did not kexec the choice"
log  | grep -qx "outcome:  booted user selection" && ok "log outcome line is exactly right" || bad "outcome line wrong: [$(log | grep outcome)]"
log  | grep -q "MARKER_DMESG_LINE"                && ok "log captures dmesg" || bad "no dmesg in log"
log  | grep -q "picker mock: drew the menu"       && ok "log captures picker stderr" || bad "no picker stderr in log"
both | grep -q "MARKER_RESCUE"                    && bad "unexpectedly hit rescue" || ok "no rescue on happy path"

echo "=== 2. picker crashes (the real first-boot suspect) ==="
setup crash crash 0 0; run >/dev/null
both | grep -q "MARKER_KEXEC.*vmlinuz-real" && ok "falls back to first discovered kernel" || bad "no fallback kexec"
both | grep -q "the menu could not be shown" && ok "says so ON SCREEN (was silent before)" || bad "still silent on screen"
both | grep -q "MARKER_DRM_OPEN_FAILED"      && ok "replays picker stderr to screen" || bad "stderr not shown"
log  | grep -q "picker exit code: 3"         && ok "log records exit code 3" || bad "exit code missing: $(log | grep -i 'exit code')"
log  | grep -qx "outcome:  fell back: picker exited 3" && ok "log outcome line is exactly right (no duplication)" || bad "outcome line wrong: [$(log | grep outcome)]"
log  | grep -q "MARKER_DRM_OPEN_FAILED"      && ok "log captures the DRM error" || bad "DRM error not logged"
log  | grep -q -- "--- /dev/dri"             && ok "log includes device listing" || bad "no device listing"

echo "=== 3. picker exits 0 but selects nothing ==="
setup empty empty 0 0; run >/dev/null
both | grep -q "MARKER_KEXEC.*vmlinuz-real"   && ok "falls back" || bad "no fallback"
log  | grep -qx "outcome:  fell back: picker produced no selection" && ok "log distinguishes this from a crash" || bad "outcome line wrong: [$(log | grep outcome)]"

echo "=== 4. root mount fails -> rescue, and says why it can't log ==="
setup mountfail ok 1 0; run >/dev/null
both | grep -q "MARKER_RESCUE_SHELL_REACHED"  && ok "drops to rescue shell" || bad "no rescue shell"
both | grep -q "cannot save a boot log"       && ok "explains the missing log instead of silently skipping" || bad "silent about no log"
both | grep -q "MARKER_KEXEC"                 && bad "kexec'd despite no root!" || ok "does not kexec"

echo "=== 5. discovery fails -> rescue, but root IS mounted so log survives ==="
setup discfail ok 0 1; run >/dev/null
both | grep -q "MARKER_RESCUE_SHELL_REACHED"  && ok "drops to rescue shell" || bad "no rescue shell"
log  | grep -q "outcome:  rescue: no kernel entries found" && ok "log records the rescue reason" || bad "outcome: $(log | grep outcome)"
log  | grep -q "mounting real root"           && ok "log shows stages reached" || bad "no stage trail"

echo
echo "passed: $pass   failed: $fail"
[ "$fail" -eq 0 ]
