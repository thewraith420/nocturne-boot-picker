CFLAGS ?= -Wall -Wextra -O2
LDLIBS := -ldrm -lm
CPPFLAGS := -I/usr/include/libdrm -I. -Ilvgl

LVGL_SRC := $(shell find lvgl/src -name '*.c' 2>/dev/null)
LVGL_OBJ := $(LVGL_SRC:.c=.o)

picker: check-lvgl picker.o $(LVGL_OBJ)
	$(CC) $(CFLAGS) -o $@ picker.o $(LVGL_OBJ) $(LDLIBS)

check-lvgl:
	@test -d lvgl || (echo "lvgl/ not found - run ./fetch-lvgl.sh first" && exit 1)

# Every object depends on lv_conf.h: changing it (enabling a font,
# switching the allocator) changes what the LVGL sources compile to,
# and without this make silently links stale objects - which shows up
# as confusing undefined-reference errors, or worse, a binary built
# half against the old config.
%.o: %.c lv_conf.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# The harnesses and the renderer #include picker.c so they exercise the
# real functions rather than copies. That makes picker.c a dependency
# make cannot see on its own - without this they silently keep testing
# and rendering whatever picker.c looked like when they were last built,
# which is the "green suite proving nothing" failure in its purest form.
test-edit-layout.o test-screenshot.o test-flush.o render-screens.o: picker.c

# Headless regression test for the Edit dialog's z-order - the keyboard
# rendered fine and was dead to touch because it sat under the msgbox
# backdrop. Structural, so it needs no panel.
test-edit-layout: check-lvgl test-edit-layout.o $(LVGL_OBJ)
	$(CC) $(CFLAGS) -o $@ test-edit-layout.o $(LVGL_OBJ) $(LDLIBS)

# Verifies the screenshot dump un-rotates correctly against a synthetic
# framebuffer. Includes picker.c, so it exercises the real function.
test-screenshot: check-lvgl test-screenshot.o $(LVGL_OBJ)
	$(CC) $(CFLAGS) -o $@ test-screenshot.o $(LVGL_OBJ) $(LDLIBS)

# Renders the real screens offscreen for documentation - includes
# picker.c, so it is the actual UI code, not a mockup.
render-screens: check-lvgl render-screens.o $(LVGL_OBJ)
	$(CC) $(CFLAGS) -o $@ render-screens.o $(LVGL_OBJ) $(LDLIBS)

# Proves the optimised flush_cb is pixel-identical to the per-pixel
# version it replaced, for every rotation.
test-flush: check-lvgl test-flush.o $(LVGL_OBJ)
	$(CC) $(CFLAGS) -o $@ test-flush.o $(LVGL_OBJ) $(LDLIBS)

test: test-edit-layout test-screenshot test-flush
	./test-edit-layout
	./test-screenshot
	./test-flush

clean:
	rm -f picker picker.o test-edit-layout test-edit-layout.o \
	      test-screenshot test-screenshot.o \
	      render-screens render-screens.o \
	      test-flush test-flush.o $(LVGL_OBJ)

.PHONY: clean check-lvgl test
