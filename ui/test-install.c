/* Exercises picker's install plumbing: the child process, its output
 * reaching the progress screen, the exit status, and the fallback when
 * the install script isn't runnable.
 *
 * Includes picker.c so these are the real functions - start_install()
 * really forks, really execs, and the output really comes back down a
 * pipe. Only the script is a stand-in.
 */
#include <sys/stat.h>
#define main picker_real_main
#include "picker.c"
#undef main

static int fails, passes;
static void ck(int c, const char *m) { printf(c ? "  [ok] %s\n" : "  [FAIL] %s\n", m); c ? passes++ : fails++; }

static void dummy_flush(lv_display_t *d, const lv_area_t *a, uint8_t *p) {
    (void)a; (void)p; lv_display_flush_ready(d);
}

/* Drains the child exactly the way the main loop does. */
static int drain(void) {
    char buf[512]; size_t len = 0;
    for (;;) {
        ssize_t got = read(g_install_fd, buf + len, sizeof(buf) - len - 1);
        if (got > 0) {
            len += (size_t)got; buf[len] = '\0';
            char *start = buf, *nl;
            while ((nl = strchr(start, '\n'))) { *nl = '\0'; prog_append(start); start = nl + 1; }
            len = strlen(start); memmove(buf, start, len + 1);
        } else {
            close(g_install_fd); g_install_fd = -1;
            if (len) { buf[len] = '\0'; prog_append(buf); }
            int st = 0;
            waitpid(g_install_pid, &st, 0);
            g_install_pid = -1;
            return WIFEXITED(st) && WEXITSTATUS(st) == 0;
        }
    }
}

int main(void) {
    lv_init();
    lv_display_t *disp = lv_display_create(600, 900);
    static uint8_t buf[600 * 10 * 4];
    lv_display_set_buffers(disp, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, dummy_flush);

    static struct entry e[1] = {{ "Ubuntu", "/boot/vmlinuz-x", "/boot/initrd-x", "ro", 0 }};
    static struct tarball tb[1] = {{ "/tmp/fake-installer.tar.gz", "9.9.9-test", "1M" }};
    g_tarballs = tb; g_tarball_n = 1;
    lv_obj_t *cd = NULL;
    build_ui(e, 1, 0, &cd);

    /* --- fallback when the script cannot run --- */
    setenv("PICKER_INSTALL_SH", "/nonexistent/install-kernel.sh", 1);
    ck(start_install(0) == -1, "falls back when the install script is missing (init does it instead)");

    /* --- a script that succeeds --- */
    FILE *f = fopen("/tmp/mock-install-ok.sh", "w");
    fprintf(f, "#!/bin/sh\necho \"install-kernel: reading $2\"\n"
               "echo 'install-kernel: extracting kernel and modules'\n"
               "echo 'install-kernel: update-initramfs -c -k 9.9.9-test'\n"
               "exit 0\n");
    fclose(f); chmod("/tmp/mock-install-ok.sh", 0755);
    setenv("PICKER_INSTALL_SH", "/tmp/mock-install-ok.sh", 1);
    setenv("PICKER_ROOT", "/mnt/root", 1);

    ck(start_install(0) == 0, "starts the child when the script is runnable");
    ck(g_install_fd >= 0 && g_install_pid > 0, "has a live pipe and pid");
    int ok = drain();
    ck(ok, "reports success for an exit-0 install");
    ck(g_prog_n >= 3, "captured the child's output lines");
    ck(strstr(g_prog_lines[0], "/tmp/fake-installer.tar.gz") != NULL,
       "passed the chosen tarball to the script");
    ck(strstr(g_prog_lines[g_prog_n - 1], "update-initramfs") != NULL,
       "last line is the most recent step (this is what the user sees)");
    ck(g_installing == 1, "install sets the no-auto-boot interlock");
    install_finished(ok);
    ck(g_prog_spinner == NULL, "spinner removed once finished");
    ck(g_reload == 0, "does not ask for a reload until the user taps Back");

    /* --- a script that fails --- */
    f = fopen("/tmp/mock-install-bad.sh", "w");
    fprintf(f, "#!/bin/sh\necho 'install-kernel: ERROR: update-initramfs failed'\nexit 1\n");
    fclose(f); chmod("/tmp/mock-install-bad.sh", 0755);
    setenv("PICKER_INSTALL_SH", "/tmp/mock-install-bad.sh", 1);
    g_prog_n = 0;
    ck(start_install(0) == 0, "starts a failing install too");
    ck(drain() == 0, "reports FAILURE for an exit-1 install (not silently ok)");

    remove("/tmp/mock-install-ok.sh"); remove("/tmp/mock-install-bad.sh");
    printf("\npassed: %d  failed: %d\n", passes, fails);
    return fails ? 1 : 0;
}
