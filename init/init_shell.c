// init_shell.c
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_got_sigchld = 0;

static void on_sigchld(int sig) {
    (void)sig;
    g_got_sigchld = 1;
}

static void reap_children_nonblock(void) {
    int status;
    pid_t p;
    for (;;) {
        p = waitpid(-1, &status, WNOHANG);
        if (p <= 0) break;
    }
    g_got_sigchld = 0;
}

static void ensure_stdio(void) {
    int fd = open("/dev/console", O_RDWR);
    if (fd < 0) return;
    dup2(fd, 0);
    dup2(fd, 1);
    dup2(fd, 2);
    if (fd > 2) close(fd);
}

static void mount_basic_fs(void) {
    mkdir("/proc", 0555);
    mkdir("/sys", 0555);
    mkdir("/dev", 0555);
    mkdir("/run", 0555);

    mount("proc", "/proc", "proc", 0, "");
    mount("sysfs", "/sys", "sysfs", 0, "");
    mount("devtmpfs", "/dev", "devtmpfs", 0, "");
    mount("tmpfs", "/run", "tmpfs", 0, "mode=0755");
}

static void do_poweroff(void) {
    sync();
    reboot(RB_POWER_OFF);
}

/* --- DOS path + prompt --- */

static void linux_to_dos_cwd(char *out, size_t outlen) {
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) {
        snprintf(out, outlen, "C:\\");
        return;
    }

    size_t j = 0;
    if (outlen < 4) { if (outlen) out[0] = 0; return; }

    out[j++] = 'C';
    out[j++] = ':';
    out[j++] = '\\';

    if (!strcmp(cwd, "/")) {
        out[j] = 0;
        return;
    }

    for (size_t i = 1; cwd[i] && j + 1 < outlen; i++) {
        char c = cwd[i];
        if (c == '/') c = '\\';
        out[j++] = c;
    }
    out[j] = 0;
}

// Accept: "\FOO\BAR", "FOO\BAR" (relative), "C:\FOO", "C:FOO" (relative), and "/" as "\"
static int dos_to_linux_path(const char *dos, char *out, size_t outlen) {
    if (!dos) return -1;

    const char *p = dos;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return -1;

    if (isalpha((unsigned char)p[0]) && p[1] == ':') {
        char d = (char)toupper((unsigned char)p[0]);
        if (d != 'C') return -1;
        p += 2;
    }

    int absolute = (*p == '\\' || *p == '/');

    if (absolute) {
        size_t j = 0;
        if (outlen < 2) return -1;
        out[j++] = '/';
        while (*p == '\\' || *p == '/') p++;

        for (; *p && j + 1 < outlen; p++) {
            char c = *p;
            if (c == '\\' || c == '/') c = '/';
            out[j++] = c;
        }
        out[j] = 0;
        return 0;
    } else {
        size_t j = 0;
        for (; *p && j + 1 < outlen; p++) {
            char c = *p;
            if (c == '\\' || c == '/') c = '/';
            out[j++] = c;
        }
        out[j] = 0;
        return 0;
    }
}

static void print_prompt(void) {
    char dos[PATH_MAX + 8];
    linux_to_dos_cwd(dos, sizeof dos);

    if (!strcmp(dos, "C:\\")) {
        write(1, "C:\\> ", 5);
        return;
    }

    size_t n = strnlen(dos, sizeof dos);
    write(1, dos, n);
    write(1, "> ", 2);
}

/* --- builtins --- */

static void builtin_cd(const char *arg) {
    if (!arg || !*arg) {
        char dos[PATH_MAX + 8];
        linux_to_dos_cwd(dos, sizeof dos);
        write(1, dos, strnlen(dos, sizeof dos));
        write(1, "\n", 1);
        return;
    }

    char linuxp[PATH_MAX];
    if (dos_to_linux_path(arg, linuxp, sizeof linuxp) != 0) {
        write(1, "Invalid drive\n", 14);
        return;
    }

    if (chdir(linuxp) == 0) return;

    write(1, "The system cannot find the path specified.\n", 43);
}

static void dos_print_dir_line(const char *name, const struct stat *st) {
    struct tm tm;
    localtime_r(&st->st_mtime, &tm);

    int hour = tm.tm_hour;
    const char *ampm = (hour >= 12) ? "PM" : "AM";
    hour %= 12;
    if (hour == 0) hour = 12;

    char buf[512];

    if (S_ISDIR(st->st_mode)) {
        snprintf(buf, sizeof buf,
                 "%02d-%02d-%02d  %02d:%02d%s    <DIR>          %s\n",
                 tm.tm_mon + 1, tm.tm_mday, (tm.tm_year % 100),
                 hour, tm.tm_min, ampm,
                 name);
    } else {
        snprintf(buf, sizeof buf,
                 "%02d-%02d-%02d  %02d:%02d%s %14lld %s\n",
                 tm.tm_mon + 1, tm.tm_mday, (tm.tm_year % 100),
                 hour, tm.tm_min, ampm,
                 (long long)st->st_size,
                 name);
    }

    write(1, buf, strnlen(buf, sizeof buf));
}

static void builtin_dir(const char *arg) {
    char linuxp[PATH_MAX];
    const char *target_linux = ".";

    if (arg && *arg) {
        if (dos_to_linux_path(arg, linuxp, sizeof linuxp) != 0) {
            write(1, "Invalid drive\n", 14);
            return;
        }
        target_linux = linuxp;
    }

    DIR *d = opendir(target_linux);
    if (!d) {
        write(1, "File not found\n", 15);
        return;
    }

    char doshdr[PATH_MAX + 8];
    if (!arg || !*arg) {
        linux_to_dos_cwd(doshdr, sizeof doshdr);
    } else {
        // best-effort cosmetic header
        const char *p = arg;
        while (*p == ' ' || *p == '\t') p++;
        if ((p[0] == '\\') || (p[0] == '/')) {
            snprintf(doshdr, sizeof doshdr, "C:%s", p);
        } else if (isalpha((unsigned char)p[0]) && p[1] == ':') {
            snprintf(doshdr, sizeof doshdr, "%s", p);
        } else {
            linux_to_dos_cwd(doshdr, sizeof doshdr);
        }
        for (size_t i = 0; doshdr[i]; i++) if (doshdr[i] == '/') doshdr[i] = '\\';
    }

    {
        char hdr[PATH_MAX + 64];
        snprintf(hdr, sizeof hdr, "\n Directory of %s\n\n", doshdr);
        write(1, hdr, strnlen(hdr, sizeof hdr));
    }

    long long total_bytes = 0;
    long long file_count = 0;
    long long dir_count = 0;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;

        char full[PATH_MAX * 2];
        if (!strcmp(target_linux, ".")) {
            snprintf(full, sizeof full, "%s", name);
        } else {
            snprintf(full, sizeof full, "%s/%s", target_linux, name);
        }

        struct stat st;
        if (lstat(full, &st) != 0) continue;

        dos_print_dir_line(name, &st);

        if (S_ISDIR(st.st_mode)) dir_count++;
        else { file_count++; total_bytes += (long long)st.st_size; }
    }

    closedir(d);

    {
        char tail[256];
        snprintf(tail, sizeof tail,
                 "\n%8lld File(s) %14lld bytes\n%8lld Dir(s)\n\n",
                 file_count, total_bytes, dir_count);
        write(1, tail, strnlen(tail, sizeof tail));
    }
}

/* --- main --- */

int main(void) {
    setsid();
    ensure_stdio();
    mount_basic_fs();

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigchld;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, 0);

    char line[1024];

    write(1, "\nDOS-modern init shell (stage 1)\n", 33);
    write(1, "Type 'help' or 'poweroff'\n\n", 27);

    for (;;) {
        if (g_got_sigchld) reap_children_nonblock();

        print_prompt();

        if (!fgets(line, sizeof line, stdin)) {
            sleep(1);
            continue;
        }

        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == 0) continue;

        if (!strcmp(line, "help")) {
            const char *msg =
                "Built-ins:\n"
                "  help        Show this help\n"
                "  ver         Show version\n"
                "  cd [path]   Change directory (DOS paths)\n"
                "  dir [path]  List directory\n"
                "  poweroff    Power off\n";
            write(1, msg, strlen(msg));
            continue;
        }

        if (!strcmp(line, "ver")) {
            write(1, "DOS-modern 0.0.1\n", 17);
            continue;
        }

        if (!strcmp(line, "poweroff")) {
            do_poweroff();
            continue;
        }

        if (!strncmp(line, "cd", 2) && (line[2] == 0 || line[2] == ' ' || line[2] == '\t')) {
            char *arg = line + 2;
            while (*arg == ' ' || *arg == '\t') arg++;
            builtin_cd(*arg ? arg : 0);
            continue;
        }

        if (!strncmp(line, "dir", 3) && (line[3] == 0 || line[3] == ' ' || line[3] == '\t')) {
            char *arg = line + 3;
            while (*arg == ' ' || *arg == '\t') arg++;
            builtin_dir(*arg ? arg : 0);
            continue;
        }

        write(1, "Bad command or file name\n", 25);
    }
}

