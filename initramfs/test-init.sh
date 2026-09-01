#!/bin/bash
# Exercises initramfs/init against mocked dependencies.
#
# Runs the REAL init file - absolute paths rewritten into a sandbox - so
# it cannot drift from a copy of its own logic. Covers the fallback
# chain, the diagnostics, and the device-wait race.
#
#   bash initramfs/test-init.sh
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
pass=0; fail=0
ok()  { printf '  \033[32m[ok]\033[0m %s\n' "$*"; pass=$((pass+1)); }
bad() { printf '  \033[31m[FAIL]\033[0m %s\n' "$*"; fail=$((fail+1)); }

# $1=name  $2=picker behaviour  $3=root-mount rc  $4=discover rc
# $5=devices: "present" (default) | "late" | "never"
setup() {
  SB=$(mktemp -d); export SB
  devmode=${5:-present}
  mkdir -p "$SB"/bin "$SB"/sbin "$SB"/run/picker "$SB"/mnt/root
  # mnt/root/boot exists only if the root mount succeeds - that IS what
  # mounting the real root does. Pre-creating it unconditionally made
  # save_log's "is the root actually mounted?" guard untestable.
  [ "$3" = 0 ] && mkdir -p "$SB"/mnt/root/boot

  # Fake device tree. "late" has a helper create the nodes after a
  # delay, modelling a driver that binds just after init gets there.
  mkdir -p "$SB"/dev "$SB"/sys/class/drm/card0-eDP-1
  : > "$SB/dev/rootdev"
  conn() { echo "$1" > "$SB/sys/class/drm/card0-eDP-1/status"; }
  case "$devmode" in
    present) mkdir -p "$SB"/dev/dri "$SB"/dev/input
             : > "$SB/dev/dri/card0"; : > "$SB/dev/input/event0"; conn connected ;;
    # Staggered on purpose: i915 and I2C-HID are separate drivers that
    # bind at different moments, so both waits get exercised. Creating
    # them simultaneously makes the second wait a no-op and silently
    # stops testing it.
    # Three separate arrivals, in the order real hardware does it: the
    # card node, then the panel reporting connected, then the touch
    # controller. Collapsing any two makes the later wait a no-op and
    # silently stops testing it.
    late)    conn disconnected
             ( sleep 2; mkdir -p "$SB"/dev/dri; : > "$SB/dev/dri/card0"
               sleep 1; conn connected
               sleep 1; mkdir -p "$SB"/dev/input; : > "$SB/dev/input/event0" ) & ;;
    # The case waiting on the card node alone would sail straight past:
    # i915 has published cardN, but the panel is not reporting connected
    # yet, so drm_open_first_connected() would still reject it.
    notconn) mkdir -p "$SB"/dev/dri "$SB"/dev/input
             : > "$SB/dev/dri/card0"; : > "$SB/dev/input/event0"; conn disconnected ;;
    never)   conn disconnected ;;
  esac

  # --- mocks -------------------------------------------------------
  cat > "$SB/bin/mount" <<EOF
#!/bin/sh
case "\$*" in
  *remount*) exit 0 ;;
  *rootdev*) exit $3 ;;
esac
exit 0
EOF
  cat > "$SB/bin/sh" <<'EOF'
#!/bin/sh
echo "MARKER_RESCUE_SHELL_REACHED"
exit 0
EOF
  printf '#!/bin/sh\nexit 0\n'                              > "$SB/bin/mdev"
  printf '#!/bin/sh\necho "[0.000000] MARKER_DMESG_LINE"\n' > "$SB/bin/dmesg"
  printf '#!/bin/sh\necho "MARKER_KEXEC root=$1 linux=$2"\nexit 0\n' > "$SB/sbin/kexec-boot.sh"
  cat > "$SB/bin/discover-kernels.sh" <<EOF
#!/bin/sh
[ "$4" = 0 ] || exit 1
printf 'Ubuntu\t/boot/vmlinuz-real\t/boot/initrd.img-real\tro quiet\t\n'
printf 'Ubuntu old\t/boot/vmlinuz-old\t/boot/initrd.img-old\tro quiet\t\n'
EOF
  printf '#!/bin/sh\ncat "$1"\n' > "$SB/bin/apply-default.sh"
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
    crash) printf '#!/bin/sh\necho "picker mock: MARKER_DRM_OPEN_FAILED /dev/dri/card0" >&2\nexit 3\n' > "$SB/bin/picker" ;;
    empty) printf '#!/bin/sh\necho "picker mock: chose nothing" >&2\nexit 0\n' > "$SB/bin/picker" ;;
  esac
  chmod +x "$SB"/bin/* "$SB"/sbin/*

  # --- the real init, redirected into the sandbox -------------------
  sed -e "s|/bin/|$SB/bin/|g" -e "s|/sbin/|$SB/sbin/|g" \
      -e "s|/mnt/root|$SB/mnt/root|g" -e "s|/run/picker|$SB/run/picker|g" \
      -e "s|/dev/dri|$SB/dev/dri|g" -e "s|/dev/input|$SB/dev/input|g" \
      -e "s|/sys/class/drm|$SB/sys/class/drm|g" \
      "$REPO/initramfs/init" > "$SB/init"
}

# NB: run the interpreter by ABSOLUTE path. $SB/bin/sh is the mocked
# rescue shell and is first in PATH, so a bare `sh` here silently runs
# the mock instead of init - which looks exactly like "init dropped to
# rescue immediately" and cost a debugging round.
run() {
  PATH="$SB/bin:$SB/sbin:$PATH" \
  REAL_ROOT_DEV="$SB/dev/rootdev" PICKER_FALLBACK_PAUSE=0 \
  PICKER_WAIT_ROOT=${W:-3} PICKER_WAIT_DRM=${W:-3} PICKER_WAIT_INPUT=${W:-3} \
    /bin/sh "$SB/init" >"$SB/out" 2>"$SB/err"
}

log()  { cat "$SB/mnt/root/boot/picker-last-boot.log" 2>/dev/null; }
both() { cat "$SB/out" "$SB/err" 2>/dev/null; }

echo "=== 1. happy path: picker returns a selection ==="
setup happy ok 0 0; run
both | grep -q "MARKER_KEXEC.*vmlinuz-chosen" && ok "kexecs the user's choice" || bad "did not kexec the choice"
log  | grep -qx "outcome:  booted user selection" && ok "log outcome line exact" || bad "outcome wrong: [$(log | grep outcome)]"
log  | grep -q "MARKER_DMESG_LINE"    && ok "log captures dmesg" || bad "no dmesg in log"
log  | grep -q "drew the menu"        && ok "log captures picker stderr" || bad "no picker stderr in log"
both | grep -q "MARKER_RESCUE"        && bad "unexpectedly hit rescue" || ok "no rescue on happy path"
log  | grep -q "waiting for"          && bad "waited despite devices being present" || ok "no wait when devices already there"

echo "=== 2. picker crashes (the first-boot suspect) ==="
setup crash crash 0 0; run
both | grep -q "MARKER_KEXEC.*vmlinuz-real"  && ok "falls back to first discovered kernel" || bad "no fallback kexec"
both | grep -q "the menu could not be shown" && ok "says so ON SCREEN (was silent before)" || bad "still silent on screen"
both | grep -q "MARKER_DRM_OPEN_FAILED"      && ok "replays picker stderr to screen" || bad "stderr not shown on screen"
log  | grep -q "picker exit code: 3"         && ok "log records exit code 3" || bad "exit code missing"
log  | grep -qx "outcome:  fell back: picker exited 3" && ok "log outcome exact (no duplication)" || bad "outcome wrong: [$(log | grep outcome)]"

echo "=== 3. picker exits 0 but selects nothing ==="
setup empty empty 0 0; run
both | grep -q "MARKER_KEXEC.*vmlinuz-real" && ok "falls back" || bad "no fallback"
log  | grep -qx "outcome:  fell back: picker produced no selection" && ok "distinguished from a crash" || bad "outcome wrong: [$(log | grep outcome)]"

echo "=== 4. root mount fails -> rescue ==="
setup mountfail ok 1 0; run
both | grep -q "MARKER_RESCUE_SHELL_REACHED" && ok "drops to rescue shell" || bad "no rescue shell"
both | grep -q "cannot save a boot log"      && ok "explains the missing log" || bad "silent about no log"
both | grep -q "MARKER_KEXEC"                && bad "kexec'd despite no root!" || ok "does not kexec"

echo "=== 5. discovery fails -> rescue, root mounted so log survives ==="
setup discfail ok 0 1; run
both | grep -q "MARKER_RESCUE_SHELL_REACHED" && ok "drops to rescue shell" || bad "no rescue shell"
log  | grep -q "outcome:  rescue: no kernel entries found" && ok "log records rescue reason" || bad "outcome wrong"
log  | grep -q "mounting real root"          && ok "log shows stage trail" || bad "no stage trail"

echo "=== 6. THE RACE: devices appear 2s late (was an instant silent failure) ==="
W=10 setup late ok 0 0 late; W=10 run
both | grep -q "waiting for DRM device"      && ok "waits instead of sampling once" || bad "did not wait"
log  | grep -qE "DRM device .* appeared after [0-9]+s" && ok "records how long DRM took" || bad "no appearance timing: [$(log | grep -i drm | head -2)]"
log  | grep -qE "touch input device .* appeared after [0-9]+s" && ok "waits for touch too" || bad "no touch wait recorded"
log  | grep -qE "connected DRM output appeared after [0-9]+s" && ok "waits for the connector to report connected" || bad "no connector wait: [$(log | grep -i connect | head -2)]"
both | grep -q "MARKER_KEXEC.*vmlinuz-chosen" && ok "still boots the user's choice" || bad "lost the selection"
log  | grep -q "TIMEOUT"                     && bad "reported a timeout despite devices arriving" || ok "no spurious timeout"

echo "=== 7. devices never appear: must NOT hang or rescue ==="
W=2 setup never crash 0 0 never; W=2 run
log  | grep -q "TIMEOUT: DRM device"         && ok "logs the timeout explicitly" || bad "timeout not logged"
both | grep -q "MARKER_KEXEC.*vmlinuz-real"  && ok "still falls back to a bootable kernel" || bad "did not boot anything"
both | grep -q "MARKER_RESCUE"               && bad "stranded the user in rescue" || ok "does not strand the user"

echo "=== 8. card node present but connector NOT connected (the sufficiency gap) ==="
W=2 setup notconn crash 0 0 notconn; W=2 run
log  | grep -q "TIMEOUT: connected DRM output" && ok "waits on connector status, not just the card node" || bad "sailed past a disconnected panel: [$(log | grep -iE 'timeout|connect' | head -3)]"
log  | grep -q "TIMEOUT: DRM device"           && bad "wrongly reported the card node missing" || ok "card-node wait passed (it IS present)"
both | grep -q "MARKER_KEXEC.*vmlinuz-real"    && ok "still falls back to a bootable kernel" || bad "did not boot anything"

echo
echo "passed: $pass   failed: $fail"
[ "$fail" -eq 0 ]
