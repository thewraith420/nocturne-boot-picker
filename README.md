# nocturne-boot-picker

A touch-driven boot picker for the Google Pixel Slate (`nocturne`), TWRP-style —
replacing GRUB's mouse/keyboard-only menu with something you can actually use
on a tablet with no keyboard attached.

**Status: framework only.** This repo is scaffolding + a design doc, laid down
so a fresh work session has a concrete starting point. No code has been
written yet. This is a deliberately separate project from
[BobZKernel](https://github.com/thewraith420/BobZKernel) (which builds the
*real* installed kernels this picker will boot into) — different shape of
work: BobZKernel is patch-and-verify against an established pipeline; this is
designing a minimal system from scratch (kernel config, initramfs, touch UI,
kexec flow).

## The problem

The Slate is tablet-first — no permanently attached keyboard or mouse. GRUB's
menu needs both to navigate. Today, picking a non-default kernel entry means
finding a keyboard, or not doing it at all.

## The design (why this is tractable, not "build a bootloader")

**UEFI/GRUB has essentially no touchscreen driver support.** That's a dead
end - don't try to add touch input to GRUB itself.

**The unlock: TWRP was never a bootloader.** It's a minimal Linux kernel +
initramfs *recovery image* with real touch drivers, that the *real*
bootloader (on Android, the boot partition / fastboot) hands control to
instead of booting normally. The same trick applies here:

1. GRUB stays **completely unmodified**, timeout set short (or auto-selects
   immediately).
2. GRUB's default entry boots a small dedicated **picker kernel + initramfs**
   - not a real OS, just enough Linux to draw a touch menu.
3. The picker has **real mainline touch + DRM drivers** (see hardware facts
   below), draws a list of available real kernels, and waits for a tap.
4. On selection, the picker `kexec`s straight into whichever real installed
   kernel was tapped. GRUB's own menu UI is never shown to the user at all -
   they only ever see the touch picker.

This means the actual scope is: a minimal kernel config + a small initramfs +
a touch UI + kexec glue. Not a bootloader rewrite.

## Hardware facts (confirmed live on the machine, not guessed)

- **Touch stack is fully generic/mainline**: `i2c_hid_acpi` -> `i2c_hid` ->
  `hid_multitouch`. No vendor blob, no goodix/elants/silead-style out-of-tree
  driver. This is the entire touchscreen dependency list for the picker's
  initramfs/kernel config.
- **Graphics is `i915`** (Intel UHD 615, Nocturne Y-series). The main OS
  needed `i915.enable_dpcd_backlight=2 i915.enable_psr=0` for backlight to
  work at all (see BobZKernel's pixel-slate README) - the picker kernel will
  very likely need the same cmdline quirks or it may have a working display
  with no visible backlight, which would look identical to "not working."
- Display is a 3000x2000 @ 3:2 panel - the touch UI needs to render sanely at
  that resolution/aspect, likely wants deliberately large touch targets given
  the DPI.

## Division of labor (mirrors how the rest of this project has worked)

- **On-device / hardware side**: real hardware testing, one-shot GRUB edits,
  checking actual `/boot` layout and existing kernel entries, verifying touch
  input events live, confirming what the real kexec target list needs to look
  like.
- **Build environment side**: minimal kernel config iteration, initramfs
  construction, `kexec-tools` integration, the touch menu rendering code
  itself.

Cross-session messaging (`SendMessage`/`ListAgents`) is available for this
project's back-and-forth, instead of the handoff-doc-file pattern used
earlier in the BobZKernel work - this project's rhythm (test on hardware ->
report back -> adjust) suits a live channel better than documents.

## Decisions

1. **Kernel config**: reuse BobZKernel's `pixel-slate` branch as the base,
   on a dedicated `picker-kernel` branch forked from it (currently at
   `5d8832c`). Inherits the load-bearing platform fixes for free - the i915
   backlight quirks, the `hid_google_hammer` crash fix, the GOOG0007 button
   fix - without polluting the real installed-kernel branch. From here the
   config gets stripped down: drop camera/IPU3, v4l2loopback, Waydroid
   binder, storage/network drivers, anything not needed to draw a menu and
   `kexec`. Lives in BobZKernel's repo, not this one - see `picker-kernel/`
   here for how it plugs into this project.

## Open questions - not yet decided, resolve these next

2. **Touch UI rendering**: raw DRM+fbdev drawing (smallest, most control,
   most manual work) vs. pulling in something like a tiny SDL or LVGL setup
   (faster to build a real UI, bigger initramfs, another dependency to keep
   minimal/secure in a privileged pre-boot environment).
3. **Boot entry discovery**: parse the real GRUB config / BLS entries that
   already exist on the Slate's `/boot` (always in sync with reality, but
   depends on a stable/parseable format) vs. a simple config file the picker
   owns and someone updates when kernels change (simpler code, can drift out
   of sync with what's actually installed).

## Directory layout

```
nocturne-boot-picker/
├── README.md              # this file
├── picker-kernel/         # minimal kernel config for the picker (once decided)
├── initramfs/             # initramfs build scripts + root filesystem layout
├── ui/                    # touch menu rendering code
├── boot-integration/      # the GRUB entry that boots the picker + kexec glue
└── docs/                  # hardware findings, design decisions, session logs
```

## Where this came from

Proposed by a Claude session working hands-on on the Slate, mid-way through
unrelated BobZKernel kernel-patch work (a `hid_google_hammer` crash fix and,
separately, a firmware regression that broke the volume buttons - both
tracked in BobZKernel's `pixel-slate` branch, unrelated to this project).
Evaluated and agreed on by Bob and the BobZKernel-side Claude session as a
sound design worth pursuing, explicitly as its own thread/project rather than
folded into the kernel-patch work.
