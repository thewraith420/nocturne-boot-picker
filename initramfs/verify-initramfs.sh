#!/bin/sh
# Unpacks a built initramfs and checks the things that otherwise only
# show up as a kernel panic on a device with no console to explain it.
#
#   ./verify-initramfs.sh picker-initramfs.img
#
# Checks, in rough order of how painful the failure would be:
#   - /init exists, is executable, has a shebang pointing at a shell
#     that exists *inside the image*
#   - every absolute /bin//sbin path init references is present (the
#     list is read out of init itself, so this can't drift)
#   - every ELF binary's DT_NEEDED libraries and its interpreter are
#     present *inside the image*, not merely on the build host - the
#     classic cause of "kernel can't run init" with no further detail
#   - busybox applet symlinks resolve to something real
#   - /dev/console exists, so init's own error messages have somewhere
#     to go (including the rescue-shell path)

set -eu

img=${1:?usage: verify-initramfs.sh <initramfs.img>}
[ -r "$img" ] || { echo "verify: cannot read $img" >&2; exit 1; }

root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT

fails=0
ok()   { printf '  [ ok ] %s\n' "$*"; }
bad()  { printf '  [FAIL] %s\n' "$*"; fails=$((fails + 1)); }

decompress() {
    case $(file -b "$img") in
        *gzip*) gzip -dc "$img" ;;
        *)      cat "$img" ;;
    esac
}

# Extracting as a normal user can't create the device nodes; that's
# expected and not a problem with the image, so tolerate it here and
# check those from the archive listing instead (below).
decompress | ( cd "$root" && cpio -idm --quiet 2>/dev/null || true )

listing=$(decompress | cpio -itv --quiet 2>/dev/null || true)

echo "verifying $img"

# ---- init itself -----------------------------------------------------
if [ -f "$root/init" ] && [ -x "$root/init" ]; then
    ok "/init present and executable"
else
    bad "/init missing or not executable - kernel will panic immediately"
fi

if [ -f "$root/init" ]; then
    shebang=$(head -c 128 "$root/init" | head -n1)
    interp=$(echo "$shebang" | sed -n 's|^#!\([^ ]*\).*|\1|p')
    if [ -n "$interp" ]; then
        if [ -e "$root$interp" ]; then
            ok "init shebang $interp exists in image"
        else
            bad "init shebang points at $interp which is NOT in the image"
        fi
    else
        bad "init has no shebang"
    fi
fi

# ---- paths init actually references ----------------------------------
# Read them out of init rather than hardcoding, so this stays honest if
# init changes.
for p in $(grep -oE '/(bin|sbin|etc|lib)/[a-zA-Z0-9._/-]+' "$root/init" 2>/dev/null | sort -u); do
    if [ -e "$root$p" ]; then
        ok "init references $p - present"
    else
        bad "init references $p - MISSING from image"
    fi
done

# ---- ELF dependencies, resolved inside the image ----------------------
elf_files=$(find "$root" -type f -exec sh -c 'file -b "$1" | grep -q "^ELF" && echo "$1"' _ {} \; 2>/dev/null || true)
for bin in $elf_files; do
    rel=${bin#"$root"}

    interp=$(readelf -l "$bin" 2>/dev/null | sed -n 's/.*program interpreter: \(.*\)\]/\1/p' | head -n1)
    needed=$(readelf -d "$bin" 2>/dev/null | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p')

    # Say something for every binary examined. A static binary has no
    # interpreter and no NEEDED entries, so without this it produces no
    # output at all - indistinguishable from never having been checked,
    # which is exactly the kind of silence that hides real problems.
    if [ -z "$interp" ] && [ -z "$needed" ]; then
        ok "$rel statically linked - no runtime deps to satisfy"
        continue
    fi

    if [ -n "$interp" ]; then
        if [ -e "$root$interp" ]; then
            ok "$rel interpreter $interp present"
        else
            bad "$rel needs interpreter $interp - MISSING from image"
        fi
    fi

    for lib in $needed; do
        if find "$root" -name "$lib" | grep -q .; then
            ok "$rel needs $lib - present"
        else
            bad "$rel needs $lib - MISSING from image"
        fi
    done
done

# ---- busybox applet symlinks -----------------------------------------
brokenlinks=0
for l in $(find "$root/bin" "$root/sbin" -type l 2>/dev/null); do
    if [ ! -e "$l" ]; then
        bad "dangling symlink: ${l#"$root"}"
        brokenlinks=$((brokenlinks + 1))
    fi
done
[ "$brokenlinks" -eq 0 ] && ok "no dangling symlinks in /bin and /sbin"

# ---- device nodes ----------------------------------------------------
# Read from the archive listing, not the extracted tree: an unprivileged
# extract silently drops device nodes, which would make this check lie.
for dev in dev/console dev/null; do
    if echo "$listing" | grep -qE "^c.* $dev\$"; then
        ok "/$dev present as a character device"
    else
        bad "/$dev missing or not a char device - init's output (and the rescue shell) goes nowhere"
    fi
done

echo
if [ "$fails" -eq 0 ]; then
    echo "verify: PASS - image looks bootable"
else
    echo "verify: FAIL - $fails problem(s); this image would likely panic at boot"
fi
exit $([ "$fails" -eq 0 ] && echo 0 || echo 1)
