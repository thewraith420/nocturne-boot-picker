#!/bin/sh
# Parses a GRUB config for top-level menuentry stanzas and emits one
# "title<TAB>linux-path<TAB>initrd-path<TAB>cmdline" line per entry.
#
# Assumes GRUB_DISABLE_SUBMENU=true upstream, so every real kernel entry
# is a flat top-level menuentry rather than buried in an "Advanced
# options" submenu - see boot-integration/grub-picker-entry.cfg.
#
# Usage: discover-kernels.sh /path/to/grub.cfg > menu.tsv

set -eu
cfg="${1:?usage: discover-kernels.sh <grub.cfg>}"

awk '
BEGIN { depth = 0; title = ""; linux = ""; initrd = ""; cmdline = ""; id = "" }

$1 == "menuentry" {
    if (depth == 0) {
        line = $0
        sub(/^[^"'"'"']*['"'"'"]/, "", line)
        sub(/["'"'"'].*/, "", line)
        title = line
        id = ""
        for (i = 1; i <= NF; i++) {
            if ($i == "--id" && i < NF) { id = $(i + 1); break }
        }
        linux = ""; initrd = ""; cmdline = ""
    }
    depth++
    next
}

$1 == "submenu" { depth++; next }

depth == 1 && ($1 == "linux" || $1 == "linuxefi" || $1 == "linux16") {
    linux = $2
    cmdline = ""
    for (i = 3; i <= NF; i++) cmdline = cmdline (i > 3 ? " " : "") $i
    next
}

depth == 1 && ($1 == "initrd" || $1 == "initrdefi" || $1 == "initrd16") {
    initrd = $2
    next
}

/}/ {
    depth--
    if (depth == 0) {
        if (title != "" && linux != "" && id != "picker") {
            printf "%s\t%s\t%s\t%s\n", title, linux, initrd, cmdline
        }
        title = ""; linux = ""; initrd = ""; cmdline = ""; id = ""
    }
}
' "$cfg"
