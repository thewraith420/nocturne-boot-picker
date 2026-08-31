#!/bin/sh
# Reorders a discover-kernels.sh menu.tsv so a previously-set default
# entry (if it still exists) is moved to the front - ui/picker.c's
# auto-boot timeout and initramfs/init's picker-failure fallback both
# just take "the first entry", so this is the only place that needs to
# know about a persisted preference. Also appends a 5th
# "is_default" field ("1" or empty) to every line, so ui/picker.c can
# show which entry is the default directly on the menu, not just act
# on it at boot time.
#
# Usage: apply-default.sh <menu.tsv> <default-marker-file>
#
# <default-marker-file> holds a single line: the linux path (e.g.
# /boot/vmlinuz-x) of the preferred default, written by initramfs/init
# when the picker UI's "Set Default" is used. A missing marker file, or
# one naming a path no longer present in menu.tsv (kernel since
# removed), is not an error - menu.tsv just passes through in its
# original (grub.cfg) order, today's existing behavior, with every
# is_default field empty.

set -eu
menu="${1:?usage: apply-default.sh <menu.tsv> <default-marker-file>}"
marker="${2:?}"

default_linux=""
if [ -f "$marker" ]; then
    default_linux=$(head -n1 "$marker")
fi

awk -F'\t' -v OFS='\t' -v want="$default_linux" '
{
    is_default = (want != "" && $2 == want) ? "1" : ""
    line = $0 OFS is_default "\n"
    if (is_default == "1") matched = matched line
    else rest = rest line
}
END { printf "%s%s", matched, rest }
' "$menu"
