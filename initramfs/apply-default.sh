#!/bin/sh
# Reorders a discover-kernels.sh menu.tsv so a previously-set default
# entry (if it still exists) is moved to the front - ui/picker.c's
# auto-boot timeout and initramfs/init's picker-failure fallback both
# just take "the first entry", so this is the only place that needs to
# know about a persisted preference.
#
# Usage: apply-default.sh <menu.tsv> <default-marker-file>
#
# <default-marker-file> holds a single line: the linux path (e.g.
# /boot/vmlinuz-x) of the preferred default, written by initramfs/init
# when the picker UI's "Set Default" is used. A missing marker file, or
# one naming a path no longer present in menu.tsv (kernel since
# removed), is not an error - menu.tsv just passes through in its
# original (grub.cfg) order, today's existing behavior.

set -eu
menu="${1:?usage: apply-default.sh <menu.tsv> <default-marker-file>}"
marker="${2:?}"

if [ ! -f "$marker" ]; then
    cat "$menu"
    exit 0
fi

default_linux=$(head -n1 "$marker")

awk -F'\t' -v want="$default_linux" '
$2 == want { matched = matched $0 "\n"; next }
{ rest = rest $0 "\n" }
END { printf "%s%s", matched, rest }
' "$menu"
