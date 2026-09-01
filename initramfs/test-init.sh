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
  conn() { echo "$1" > "$SB/sys/class/drm/card0-eDP-1/status"; }
  # picker owns waiting for DRM and touch (PICKER_WAIT_SECS, ui/picker.c)
  # since only touch_open() knows what a usable device is. The ONLY wait
  # left in init is the root device, so that is what these exercise.
  case "$devmode" in
    rootlate)  ( sleep 2; : > "$SB/dev/rootdev" ) & ;;
    rootnever) ;;
    *)         : > "$SB/dev/rootdev" ;;
  esac
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
  # Models the real failure: one early probe line, then a flood of
  # later noise. A tailed capture loses the early line - which is
  # exactly what made a real boot log unable to answer whether the
  # touch driver ever probed.
  cat > "$SB/bin/dmesg" <<'EOF'
#!/bin/sh
echo "[    0.100000] i2c_designware i2c_designware.0: MARKER_EARLY_I2C_PROBE"
i=0
while [ $i -lt 300 ]; do echo "[   14.400000] pcieport 0000:00:1c.0: PCIe Bus Error MARKER_SPAM $i"; i=$((i+1)); done
echo "[   34.400000] MARKER_DMESG_LINE late"
EOF
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
echo 'SELECTED_BY=timeout'
exit 0
EOF
    ;;
    crash) printf '#!/bin/sh\necho "picker mock: MARKER_DRM_OPEN_FAILED /dev/dri/card0" >&2\nexit 3\n' > "$SB/bin/picker" ;;
    empty) printf '#!/bin/sh\necho "picker mock: chose nothing" >&2\nexit 0\n' > "$SB/bin/picker" ;;
  esac
  chmod +x "$SB"/bin/* "$SB"/sbin/*

  # Applet dir for BUSYBOX mode. Placed AFTER $SB/bin in PATH so the
  # mocks above still win; this only supplies the real utilities init
  # calls (grep/head/cat/ls/...). init runs under busybox in the actual
  # initramfs, so testing it under dash+coreutils tests the wrong
  # userspace - the same mistake that let the hand-run picker tests miss
  # everything the initramfs later hit.
  if [ -n "${USE_BUSYBOX:-}" ]; then
    mkdir -p "$SB/bbin"
    # APPLETS is a multi-line quoted assignment; pull the whole thing.
    applets=$(sed -n '/^APPLETS="/,/"$/p' "$REPO/initramfs/build-initramfs.sh" \
              | tr '\n' ' ' | sed 's/.*APPLETS="//; s/".*//')
    [ -n "$applets" ] || { echo "TEST BUG: could not parse APPLETS" >&2; exit 2; }
    for a in $applets; do
      busybox --list 2>/dev/null | grep -qx "$a" && ln -sf "$(command -v busybox)" "$SB/bbin/$a"
    done
  fi

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
  # PATH: mocks first (they must win), then busybox applets in busybox
  # mode, then the host - unless STRICT_BB, which drops the host
  # entirely so busybox has to supply everything. Built with plain
  # logic: a nested ${VAR:+...}${VAR:-...} pair here silently produced
  # "$SB/bbin1" and 10 bogus failures, which is the same expansion trap
  # that duplicated the log outcome line earlier.
  _path="$SB/bin:$SB/sbin"
  if [ -n "${USE_BUSYBOX:-}" ]; then _path="$_path:$SB/bbin"; fi
  if [ -z "${STRICT_BB:-}" ];  then _path="$_path:$PATH"; fi

  PATH="$_path" \
  REAL_ROOT_DEV="$SB/dev/rootdev" PICKER_FALLBACK_PAUSE=0 \
  PICKER_WAIT_ROOT=${W:-3} PICKER_WAIT_DRM=${W:-3} PICKER_WAIT_INPUT=${W:-3} \
    ${TEST_SH:-/bin/sh} "$SB/init" >"$SB/out" 2>"$SB/err"
}

log()  { cat "$SB/mnt/root/boot/picker-last-boot.log" 2>/dev/null; }
both() { cat "$SB/out" "$SB/err" 2>/dev/null; }

echo "=== 1. happy path: picker returns a selection ==="
setup happy ok 0 0; run
both | grep -q "MARKER_KEXEC.*vmlinuz-chosen" && ok "kexecs the user's choice" || bad "did not kexec the choice"
log  | grep -qx "outcome:  booted user selection" && ok "log outcome line exact" || bad "outcome wrong: [$(log | grep outcome)]"
log  | grep -q "MARKER_DMESG_LINE"    && ok "log captures dmesg" || bad "no dmesg in log"
log  | grep -q "MARKER_EARLY_I2C_PROBE" && ok "early probe line survives 300 later lines (was lost to tail -150)" || bad "early dmesg line lost - the log cannot answer 'did the driver bind'"
log  | sed -n "/hardware probe lines/,/full, up to/p" | grep -q "MARKER_EARLY_I2C_PROBE" && ok "probe lines pulled into their own section" || bad "no probe summary section"
log  | sed -n "/hardware probe lines/,/full, up to/p" | grep -q "MARKER_SPAM" && bad "probe section polluted with unrelated noise" || ok "probe section excludes unrelated spam"
log  | grep -q "drew the menu"        && ok "log captures picker stderr" || bad "no picker stderr in log"
log  | grep -qx "chosen_by=timeout"   && ok "log distinguishes timeout auto-boot from a real tap" || bad "chosen_by wrong: [$(log | grep chosen_by)]"
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

echo "=== 6. THE RACE: root device appears 2s late ==="
W=10 setup rootlate ok 0 0 rootlate; W=10 run
both | grep -q "waiting for root device"  && ok "waits instead of sampling once" || bad "did not wait"
log  | grep -qE "root device .* appeared after [0-9]+s" && ok "records how long it took" || bad "no timing: [$(log | grep -i root | head -2)]"
both | grep -q "MARKER_KEXEC.*vmlinuz-chosen" && ok "goes on to boot normally" || bad "did not boot"
log  | grep -q "TIMEOUT"                  && bad "spurious timeout" || ok "no spurious timeout"

echo "=== 7. root device never appears: must NOT hang ==="
W=2 setup rootnever ok 1 0 rootnever; W=2 run
log  | grep -q "TIMEOUT: root device"        && bad "log unreachable when root never mounts" || ok "no log (root never mounted - nothing to write to)"
both | grep -q "TIMEOUT: root device"        && ok "reports the timeout on screen" || bad "timeout not reported"
both | grep -q "MARKER_RESCUE_SHELL_REACHED" && ok "drops to rescue rather than hanging" || bad "did not reach rescue"
both | grep -q "MARKER_KEXEC"                && bad "kexec'd with no root!" || ok "does not kexec"

echo
echo "passed: $pass   failed: $fail  (${MODE_NAME:-dash + coreutils})"
[ "$fail" -eq 0 ] || exit 1

# init runs under BUSYBOX in the real initramfs, so a pass under
# dash+coreutils proves less than it looks. Re-run the whole suite
# against busybox with the host PATH removed, so busybox has to supply
# everything. Testing the wrong userspace is how the hand-run picker
# tests missed every problem the initramfs later hit.
if [ -z "${USE_BUSYBOX:-}" ]; then
  if command -v busybox >/dev/null 2>&1; then
    echo
    USE_BUSYBOX=1 STRICT_BB=1 MODE_NAME="busybox (host PATH removed)" \
      TEST_SH="$(command -v busybox) sh" "$0" "$@"
  else
    echo
    echo "NOTE: busybox not installed - skipped the busybox pass, which is"
    echo "      the userspace init actually runs under. Install busybox-static."
  fi
fi
