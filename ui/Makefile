CFLAGS ?= -Wall -Wextra -O2
LDLIBS := -ldrm -lm
CPPFLAGS := -I/usr/include/libdrm -I. -Ilvgl

LVGL_SRC := $(shell find lvgl/src -name '*.c' 2>/dev/null)
LVGL_OBJ := $(LVGL_SRC:.c=.o)

picker: check-lvgl picker.o $(LVGL_OBJ)
	$(CC) $(CFLAGS) -o $@ picker.o $(LVGL_OBJ) $(LDLIBS)

check-lvgl:
	@test -d lvgl || (echo "lvgl/ not found - run ./fetch-lvgl.sh first" && exit 1)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -f picker picker.o $(LVGL_OBJ)

.PHONY: clean check-lvgl
