#!/bin/bash
# Exercises remove-kernel.sh against a fake root tree.
#
# This is the one script in the project that can make the machine
# unbootable, so its guards get tested harder than anything else here:
# the interesting cases are all the ones where it must REFUSE.
#
#   bash initramfs/test-remove-kernel.sh
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
SCRIPT=$REPO/initramfs/remove-kernel.sh
pass=0; fail=0
ok()  { printf '  \033[32m[ok]\033[0m %s\n' "$*"; pass=$((pass+1)); }
bad() { printf '  \033[31m[FAIL]\033[0m %s\n' "$*"; fail=$((fail+1)); }

# $1 = space-separated kernel releases to pre-create
setup() {
  SB=$(mktemp -d)
  mkdir -p "$SB/root/boot/grub" "$SB/root/lib/modules" "$SB/root/proc" "$SB/root/sys" "$SB/bin"
  for r in $1; do
    : > "$SB/root/boot/vmlinuz-$r"
    : > "$SB/root/boot/initrd.img-$r"
    : > "$SB/root/boot/System.map-$r"
    : > "$SB/root/boot/config-$r"
    mkdir -p "$SB/root/lib/modules/$r"; : > "$SB/root/lib/modules/$r/modules.dep"
  done
  # mount/umount/chroot are all no-ops here; chroot records that it ran.
  printf '#!/bin/sh\nexit 0\n' > "$SB/bin/mount"
  printf '#!/bin/sh\nexit 0\n' > "$SB/bin/umount"
  cat > "$SB/bin/chroot" <<EOF
#!/bin/sh
case "\$*" in
  *grub-probe*) exit ${GRUB_PROBE_RC:-0} ;;
  *update-grub*) echo "MARKER_UPDATE_GRUB \$*" >&2; exit ${UPDATE_GRUB_RC:-0} ;;
esac
exit 0
EOF
  chmod +x "$SB"/bin/*
}
run() { PATH="$SB/bin:$PATH" sh "$SCRIPT" "$SB/root" "$1" >"$SB/out" 2>&1; echo $?; }
out() { cat "$SB/out"; }

echo "=== guards: it must refuse ==="

setup "7.1.12-only"
rc=$(run 7.1.12-only)
[ "$rc" != 0 ] && ok "refuses to remove the ONLY kernel" || bad "removed the only kernel (rc=$rc)"
out | grep -q "only remaining kernel" && ok "says why" || bad "no explanation: $(out|tail -1)"
[ -f "$SB/root/boot/vmlinuz-7.1.12-only" ] && ok "the kernel is still there" || bad "DELETED the only kernel"

setup "a b"
rc=$(run "does-not-exist")
[ "$rc" != 0 ] && ok "refuses a release that isn't installed" || bad "accepted a bogus release"
[ -f "$SB/root/boot/vmlinuz-a" ] && [ -f "$SB/root/boot/vmlinuz-b" ] && ok "touched nothing" || bad "removed something"

setup "a b"
rc=$(run "$(uname -r)")
[ "$rc" != 0 ] && ok "refuses the RUNNING kernel" || bad "would remove the running kernel"

setup "a b"
rc=$(run "../../etc/passwd")
[ "$rc" != 0 ] && ok "refuses a path-shaped release (no traversal)" || bad "accepted a path as a release"

echo "=== the good case ==="

setup "keep-me remove-me"
rc=$(run remove-me)
[ "$rc" = 0 ] && ok "removes a kernel when another remains" || bad "refused a legitimate removal: $(out|tail -2)"
for f in vmlinuz initrd.img System.map config; do
  [ -e "$SB/root/boot/$f-remove-me" ] && bad "left /boot/$f-remove-me behind" || ok "deleted /boot/$f-remove-me"
done
[ -d "$SB/root/lib/modules/remove-me" ] && bad "left the modules behind" || ok "deleted /lib/modules/remove-me"
[ -f "$SB/root/boot/vmlinuz-keep-me" ] && ok "left the OTHER kernel alone" || bad "collateral damage"
[ -d "$SB/root/lib/modules/keep-me" ] && ok "left the other kernel's modules alone" || bad "removed the wrong modules"
out | grep -q "MARKER_UPDATE_GRUB.*update-grub" && ok "regenerates grub.cfg so the entries go too" || bad "did not run update-grub"

echo "=== preflight: a chroot that cannot see the disk must not delete ==="
# The real failure: update-grub died with "grub-probe: failed to get
# canonical path of /dev/mmcblk0p2" AFTER the kernel was already gone,
# because /dev was never bind-mounted. The fix proves the chroot works
# BEFORE touching anything, so this case now changes nothing at all.
# NB: on setup, not run - the mock bakes the exit code in when it is written.
GRUB_PROBE_RC=1 setup "keep-me remove-me"
rc=$(run remove-me)
[ "$rc" != 0 ] && ok "refuses when grub-probe cannot resolve the root device" || bad "proceeded with a broken chroot"
[ -f "$SB/root/boot/vmlinuz-remove-me" ] && ok "deleted NOTHING - the files are still there" || bad "deleted the kernel anyway, then failed"
[ -d "$SB/root/lib/modules/remove-me" ] && ok "modules still there too" || bad "removed modules despite the failure"
out | grep -q "refusing to delete anything" && ok "says it refused rather than half-finishing" || bad "unclear message"

echo "=== the saved default pointing at the removed kernel ==="

setup "keep-me remove-me"
printf '%s\n' /boot/vmlinuz-remove-me > "$SB/root/boot/picker-default"
run remove-me >/dev/null
[ -e "$SB/root/boot/picker-default" ] && bad "left a default pointing at a deleted kernel" || ok "cleared the stale saved default"

setup "keep-me remove-me"
printf '%s\n' /boot/vmlinuz-keep-me > "$SB/root/boot/picker-default"
run remove-me >/dev/null
grep -qx /boot/vmlinuz-keep-me "$SB/root/boot/picker-default" 2>/dev/null \
  && ok "leaves an unrelated saved default alone" || bad "clobbered a default for a different kernel"

echo
echo "passed: $pass   failed: $fail"
[ "$fail" -eq 0 ]
