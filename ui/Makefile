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

# Headless regression test for the Edit dialog's z-order - the keyboard
# rendered fine and was dead to touch because it sat under the msgbox
# backdrop. Structural, so it needs no panel.
test-edit-layout: check-lvgl test-edit-layout.o $(LVGL_OBJ)
	$(CC) $(CFLAGS) -o $@ test-edit-layout.o $(LVGL_OBJ) $(LDLIBS)

test: test-edit-layout
	./test-edit-layout

clean:
	rm -f picker picker.o test-edit-layout test-edit-layout.o $(LVGL_OBJ)

.PHONY: clean check-lvgl test
