CFLAGS ?= -Wall -Wextra -O2
LDLIBS := -ldrm
CPPFLAGS := -I/usr/include/libdrm

picker: picker.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f picker

.PHONY: clean
