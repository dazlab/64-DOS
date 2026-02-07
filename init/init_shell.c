// init_shell.c
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define DOS_C_ROOT "/dos/c"

static volatile sig_atomic_t g_got_sigchld = 0;
static int g_echo_on = 1;

static int g_fg = 7; // DOS-ish default: light gray
static int g_bg = 0; // DOS-ish default: black

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

/* Case-insensitive command match: must end at space/tab/EOL */
static int is_cmd(const char *line, const char *cmd) {
    size_t i = 0;
    for (;;) {
        char cc = cmd[i];
        char cl = line[i];

        if (!cc) return cl == 0 || cl == ' ' || cl == '\t';
        if (!cl) return 0;

        if (tolower((unsigned char)cl) != tolower((unsigned char)cc))
            return 0;

        i++;
    }
}

/* Detect /? anywhere in the argument string */
static int is_help_switch(const char *s) {
    if (!s) return 0;
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;

        const char *t = s;
        while (*s && *s != ' ' && *s != '\t') s++;

        size_t n = (size_t)(s - t);
        if (n == 2 && t[0] == '/' && t[1] == '?') return 1;
    }
    return 0;
}

static int has_wildcards(const char *s) {
    if (!s) return 0;
    for (; *s; s++) if (*s == '*' || *s == '?') return 1;
    return 0;
}

/* DOS wildcard match, case-insensitive: '*' any sequence, '?' any single char */
static int wildmatch_ci(const char *pat, const char *str) {
    const char *p = pat, *s = str;
    const char *star = NULL, *ss = NULL;

    while (*s) {
        if (*p == '*') { star = p++; ss = s; continue; }

        if (*p == '?' ||
            tolower((unsigned char)*p) == tolower((unsigned char)*s)) {
            p++; s++; continue;
        }

        if (star) { p = star + 1; s = ++ss; continue; }
        return 0;
    }

    while (*p == '*') p++;
    return *p == 0;
}

/* Split Linux spec into directory + pattern */
static void split_dir_pat(const char *linuxspec,
                          char *out_dir, size_t dirsz,
                          char *out_pat, size_t patsz) {
    const char *slash = strrchr(linuxspec, '/');
    if (!slash) {
        snprintf(out_dir, dirsz, ".");
        snprintf(out_pat, patsz, "%s", linuxspec);
        return;
    }

    size_t dlen = (size_t)(slash - linuxspec);
    if (dlen == 0) snprintf(out_dir, dirsz, "/");
    else {
        if (dlen >= dirsz) dlen = dirsz - 1;
        memcpy(out_dir, linuxspec, dlen);
        out_dir[dlen] = 0;
    }

    const char *p = slash + 1;
    if (*p == 0) snprintf(out_pat, patsz, "*");
    else snprintf(out_pat, patsz, "%s", p);
}

static int is_dir_path(const char *linuxp) {
    struct stat st;
    if (stat(linuxp, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
}

static const char *dos_basename(const char *p) {
    const char *last = p;
    for (; *p; p++) {
        if (*p == '\\' || *p == '/') last = p + 1;
    }
    return last;
}

/* DIR switch parsing + filespec extraction */
static void parse_dir_switches(const char *arg, int *wide, int *all) {
    *wide = 0;
    *all  = 0;
    if (!arg) return;

    const char *p = arg;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        const char *t = p;
        while (*p && *p != ' ' && *p != '\t') p++;

        size_t n = (size_t)(p - t);
        if (n >= 2 && t[0] == '/') {
            char sw = t[1];
            if (sw == 'w' || sw == 'W') *wide = 1;
            if (sw == 'a' || sw == 'A') *all  = 1;
        }
    }
}

static const char *dir_find_filespec(const char *arg, char *out, size_t outsz) {
    if (!arg) return NULL;

    const char *p = arg;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        const char *t = p;
        while (*p && *p != ' ' && *p != '\t') p++;

        size_t n = (size_t)(p - t);
        if (n == 0) continue;

        if (t[0] == '/') continue; // skip switches

        if (n >= outsz) n = outsz - 1;
        memcpy(out, t, n);
        out[n] = 0;
        return out;
    }
    return NULL;
}

/* --- DOS path mapping (single drive C:) --- */

static void linux_to_dos_cwd(char *out, size_t outlen) {
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) {
        snprintf(out, outlen, "C:\\");
        return;
    }

    size_t rootlen = strlen(DOS_C_ROOT);
    if (strncmp(cwd, DOS_C_ROOT, rootlen) != 0) {
        snprintf(out, outlen, "C:\\");
        return;
    }

    const char *rel = cwd + rootlen; // "" or "/FOO"
    if (rel[0] == 0) {
        snprintf(out, outlen, "C:\\");
        return;
    }

    size_t j = 0;
    if (outlen < 4) { if (outlen) out[0] = 0; return; }

    out[j++] = 'C';
    out[j++] = ':';
    out[j++] = '\\';

    for (size_t i = 1; rel[i] && j + 1 < outlen; i++) {
        char c = rel[i];
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
        int n = snprintf(out, outlen, "%s/", DOS_C_ROOT);
        if (n < 0 || (size_t)n >= outlen) return -1;
        size_t j = (size_t)n;

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

/* --- COLOR persistence --- */

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    return -1;
}

static void apply_color(void) {
    int fg_code = (g_fg < 8) ? (30 + g_fg) : (90 + (g_fg - 8));
    int bg_code = (g_bg < 8) ? (40 + g_bg) : (100 + (g_bg - 8));

    char seq[32];
    int n = snprintf(seq, sizeof seq, "\033[%d;%dm", bg_code, fg_code);
    if (n > 0) (void)write(1, seq, (size_t)n);
}

static void save_config(void) {
    // We're already chdir'd into DOS_C_ROOT, so write relative
    int fd = open("DOS.CFG", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;

    char buf[64];
    int n = snprintf(buf, sizeof buf, "COLOR=%X%X\n", g_bg, g_fg);
    if (n > 0) (void)write(fd, buf, (size_t)n);
    close(fd);
}

static void load_config(void) {
    // Try both cases (in case the file was created/edited manually)
    int fd = open("DOS.CFG", O_RDONLY);
    if (fd < 0) fd = open("dos.cfg", O_RDONLY);
    if (fd < 0) return;

    char buf[256];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;

    // Find a line that starts with COLOR= (case-insensitive)
    for (char *p = buf; *p; ) {
        // skip leading whitespace
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;

        if (!strncasecmp(p, "COLOR=", 6)) {
            p += 6;
            int a = hexval((unsigned char)p[0]);
            int b = hexval((unsigned char)p[1]);
            if (a >= 0 && b >= 0 && a != b) {
                g_bg = a;
                g_fg = b;
            }
            return;
        }

        // next line
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
}

/* --- prompt --- */

static void print_prompt(void) {
    char dos[PATH_MAX + 8];
    linux_to_dos_cwd(dos, sizeof dos);

    if (!strcmp(dos, "C:\\")) {
        (void)write(1, "C:\\> ", 5);
        return;
    }

    size_t n = strnlen(dos, sizeof dos);
    (void)write(1, dos, n);
    (void)write(1, "> ", 2);
}

/* --- builtins --- */

static void builtin_cls(void) {
    const char *seq = "\033[2J\033[H";
    (void)write(1, seq, 7);
}

static void builtin_pause(void) {
    const char *msg = "Press any key to continue . . .";
    (void)write(1, msg, strlen(msg));

    struct termios oldt, raw;
    int has_tty = (tcgetattr(0, &oldt) == 0);
    if (has_tty) {
        raw = oldt;
        raw.c_lflag &= ~(ICANON | ECHO | ISIG);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        (void)tcsetattr(0, TCSANOW, &raw);
    }

    unsigned char c;
    (void)read(0, &c, 1);

    if (has_tty) (void)tcsetattr(0, TCSANOW, &oldt);

    (void)write(1, "\r\n", 2);
}

static void builtin_exit(void) {
    do_poweroff();
}

static void builtin_echo(const char *arg) {
    if (is_help_switch(arg)) {
        const char *msg =
            "ECHO [ON|OFF|message]\n"
            "  Displays messages, or turns command echoing on or off.\n";
        (void)write(1, msg, strlen(msg));
        return;
    }

    if (!arg || !*arg) {
        if (g_echo_on) (void)write(1, "ECHO is on.\n", 12);
        else           (void)write(1, "ECHO is off.\n", 13);
        return;
    }

    while (*arg == ' ' || *arg == '\t') arg++;
    if (!*arg) {
        if (g_echo_on) (void)write(1, "ECHO is on.\n", 12);
        else           (void)write(1, "ECHO is off.\n", 13);
        return;
    }

    if (!strncasecmp(arg, "on", 2) && (arg[2] == 0 || arg[2] == ' ' || arg[2] == '\t')) {
        g_echo_on = 1;
        return;
    }
    if (!strncasecmp(arg, "off", 3) && (arg[3] == 0 || arg[3] == ' ' || arg[3] == '\t')) {
        g_echo_on = 0;
        return;
    }

    (void)write(1, arg, strlen(arg));
    (void)write(1, "\n", 1);
}

static void builtin_color(const char *arg) {
    if (is_help_switch(arg)) {
        const char *msg =
            "COLOR [attr]\n"
            "  attr: two hex digits. First = background, second = foreground.\n"
            "  Example: COLOR 1F\n";
        (void)write(1, msg, strlen(msg));
        return;
    }

    if (!arg || !*arg) {
        char msg[64];
        snprintf(msg, sizeof msg, "Current color: %X%X\n", g_bg, g_fg);
        (void)write(1, msg, strlen(msg));
        return;
    }

    while (*arg == ' ' || *arg == '\t') arg++;

    int a = hexval((unsigned char)arg[0]);
    int b = hexval((unsigned char)arg[1]);
    if (a < 0 || b < 0) { (void)write(1, "Invalid parameter\n", 18); return; }

    const char *p = arg + 2;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != 0) { (void)write(1, "Invalid parameter\n", 18); return; }

    if (a == b) { (void)write(1, "Invalid parameter\n", 18); return; }

    g_bg = a;
    g_fg = b;
    apply_color();
    save_config();
}

static void builtin_type(const char *arg) {
    if (is_help_switch(arg)) {
        const char *msg = "TYPE file\n  Displays the contents of a text file.\n";
        (void)write(1, msg, strlen(msg));
        return;
    }

    if (!arg || !*arg) { (void)write(1, "File not found\n", 15); return; }

    char linuxp[PATH_MAX];
    if (dos_to_linux_path(arg, linuxp, sizeof linuxp) != 0) {
        (void)write(1, "File not found\n", 15);
        return;
    }

    int fd = open(linuxp, O_RDONLY);
    if (fd < 0) { (void)write(1, "File not found\n", 15); return; }

    char buf[512];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        (void)write(1, buf, (size_t)n);
    }
    close(fd);
}

static void builtin_copy_con(const char *dst_dos) {
    if (!dst_dos || !*dst_dos) {
        (void)write(1, "Invalid number of parameters\n", 29);
        return;
    }

    char dst_linux[PATH_MAX];
    if (dos_to_linux_path(dst_dos, dst_linux, sizeof dst_linux) != 0) {
        (void)write(1, "Invalid drive\n", 14);
        return;
    }

    int fd = open(dst_linux, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { (void)write(1, "Access denied\n", 14); return; }

    (void)write(1, "Enter text. End with Ctrl+Z.\r\n", 31);

    struct termios oldt, raw;
    int has_tty = (tcgetattr(0, &oldt) == 0);
    if (has_tty) {
        raw = oldt;
        raw.c_lflag &= ~(ICANON | ECHO | ISIG);
        raw.c_iflag &= ~(ICRNL);
        raw.c_oflag &= ~(ONLCR);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        (void)tcsetattr(0, TCSANOW, &raw);
    }

    char linebuf[512];
    size_t len = 0;

    for (;;) {
        unsigned char c;
        ssize_t n = read(0, &c, 1);
        if (n <= 0) break;

        if (c == 0x1A) { // Ctrl+Z
            (void)write(1, "^Z\r\n", 4);
            if (len) (void)write(fd, linebuf, len);
            break;
        }

        if (c == '\r' || c == '\n') {
            (void)write(1, "\r\n", 2);
            if (len) (void)write(fd, linebuf, len);
            (void)write(fd, "\r\n", 2);
            len = 0;
            continue;
        }

        if (c == 0x08 || c == 0x7F) {
            if (len > 0) {
                len--;
                (void)write(1, "\b \b", 3);
            }
            continue;
        }

        if (c < 0x20 && c != '\t') continue;

        if (len + 1 < sizeof linebuf) {
            linebuf[len++] = (char)c;
            (void)write(1, &c, 1);
        }
    }

    if (has_tty) (void)tcsetattr(0, TCSANOW, &oldt);

    close(fd);
    (void)write(1, "        1 file(s) copied.\r\n", 28);
}

static void builtin_del(const char *arg) {
    if (is_help_switch(arg)) {
        const char *msg =
            "DEL [filespec]\nERASE [filespec]\n"
            "  Deletes file(s). Wildcards: * and ?\n";
        (void)write(1, msg, strlen(msg));
        return;
    }

    if (!arg || !*arg) { (void)write(1, "File not found\n", 15); return; }

    char linuxspec[PATH_MAX];
    if (dos_to_linux_path(arg, linuxspec, sizeof linuxspec) != 0) {
        (void)write(1, "File not found\n", 15);
        return;
    }

    if (!has_wildcards(linuxspec)) {
        struct stat st;
        if (stat(linuxspec, &st) != 0) { (void)write(1, "File not found\n", 15); return; }
        if (S_ISDIR(st.st_mode)) { (void)write(1, "Access denied\n", 14); return; }
        if (unlink(linuxspec) != 0) { (void)write(1, "Access denied\n", 14); return; }
        return;
    }

    char dirpath[PATH_MAX];
    char pattern[PATH_MAX];
    split_dir_pat(linuxspec, dirpath, sizeof dirpath, pattern, sizeof pattern);

    DIR *d = opendir(dirpath);
    if (!d) { (void)write(1, "File not found\n", 15); return; }

    long long deleted = 0;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        if (!wildmatch_ci(pattern, name)) continue;

        char full[PATH_MAX * 2];
        snprintf(full, sizeof full, "%s/%s", dirpath, name);

        struct stat st;
        if (lstat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) continue;

        if (unlink(full) == 0) deleted++;
    }

    closedir(d);

    if (deleted == 0) (void)write(1, "File not found\n", 15);
}

static void builtin_cd(const char *arg) {
    if (is_help_switch(arg)) {
        const char *msg =
            "CD [path]\n  Changes the current directory.\n";
        (void)write(1, msg, strlen(msg));
        return;
    }

    if (!arg || !*arg) {
        char dos[PATH_MAX + 8];
        linux_to_dos_cwd(dos, sizeof dos);
        (void)write(1, dos, strnlen(dos, sizeof dos));
        (void)write(1, "\n", 1);
        return;
    }

    char linuxp[PATH_MAX];
    if (dos_to_linux_path(arg, linuxp, sizeof linuxp) != 0) {
        (void)write(1, "Invalid drive\n", 14);
        return;
    }

    if (chdir(linuxp) == 0) return;

    (void)write(1, "The system cannot find the path specified.\n", 43);
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

    (void)write(1, buf, strnlen(buf, sizeof buf));
}

static void builtin_dir(const char *arg) {
    if (is_help_switch(arg)) {
        const char *msg =
            "DIR [filespec] [/W] [/A]\n"
            "  /W  Wide listing\n"
            "  /A  Show all (includes . and ..)\n"
            "  Wildcards: * and ?\n";
        (void)write(1, msg, strlen(msg));
        return;
    }

    int wide = 0, all = 0;
    parse_dir_switches(arg, &wide, &all);

    char filespec_tok[PATH_MAX];
    const char *filespec = dir_find_filespec(arg, filespec_tok, sizeof filespec_tok);

    char linuxspec[PATH_MAX];
    const char *spec_linux = NULL;

    if (filespec && *filespec) {
        if (dos_to_linux_path(filespec, linuxspec, sizeof linuxspec) != 0) {
            (void)write(1, "Invalid drive\n", 14);
            return;
        }
        spec_linux = linuxspec;
    }

    char dirpath[PATH_MAX];
    char pattern[PATH_MAX];

    if (!spec_linux) {
        snprintf(dirpath, sizeof dirpath, ".");
        snprintf(pattern, sizeof pattern, "*");
    } else {
        if (has_wildcards(spec_linux)) {
            split_dir_pat(spec_linux, dirpath, sizeof dirpath, pattern, sizeof pattern);
        } else if (is_dir_path(spec_linux)) {
            snprintf(dirpath, sizeof dirpath, "%s", spec_linux);
            snprintf(pattern, sizeof pattern, "*");
        } else {
            // e.g. DIR FOO.TXT or DIR \BIN\KERNEL.SYS
            split_dir_pat(spec_linux, dirpath, sizeof dirpath, pattern, sizeof pattern);
        }
    }

    DIR *d = opendir(dirpath);
    if (!d) {
        (void)write(1, "File not found\n", 15);
        return;
    }

    // Header path
    char doshdr[PATH_MAX + 8];
    if (!filespec) {
        linux_to_dos_cwd(doshdr, sizeof doshdr);
    } else {
        // If they passed a directory, show that dir. If they passed a filespec, show the spec token.
        if (filespec[0] == '\\' || filespec[0] == '/') snprintf(doshdr, sizeof doshdr, "C:%s", filespec);
        else if (isalpha((unsigned char)filespec[0]) && filespec[1] == ':') snprintf(doshdr, sizeof doshdr, "%s", filespec);
        else {
            // relative token: show current dir (DOS does)
            linux_to_dos_cwd(doshdr, sizeof doshdr);
        }
        for (size_t i = 0; doshdr[i]; i++) if (doshdr[i] == '/') doshdr[i] = '\\';
    }

    {
        char hdr[PATH_MAX + 96];
        snprintf(hdr, sizeof hdr, "\n Directory of %s\n\n", doshdr);
        (void)write(1, hdr, strnlen(hdr, sizeof hdr));
    }

    long long total_bytes = 0;
    long long file_count = 0;
    long long dir_count = 0;
    long long shown = 0;

    int col = 0;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;

        if (!all) {
            if (!strcmp(name, ".") || !strcmp(name, ".."))
                continue;
        }

        if (!wildmatch_ci(pattern, name))
            continue;

        char full[PATH_MAX * 2];
        snprintf(full, sizeof full, "%s/%s", dirpath, name);

        struct stat st;
        if (lstat(full, &st) != 0) continue;

        if (wide) {
            char out[32];
            snprintf(out, sizeof out, "%-15s", name);
            (void)write(1, out, strlen(out));
            col++;
            if (col == 5) {
                (void)write(1, "\n", 1);
                col = 0;
            }
        } else {
            dos_print_dir_line(name, &st);
        }

        shown++;

        if (S_ISDIR(st.st_mode)) dir_count++;
        else { file_count++; total_bytes += (long long)st.st_size; }
    }

    closedir(d);

    if (wide && col != 0) (void)write(1, "\n", 1);

    if (shown == 0) {
        (void)write(1, "File not found\n", 15);
        return;
    }

    {
        char tail[256];
        snprintf(tail, sizeof tail,
                 "\n%8lld File(s) %14lld bytes\n%8lld Dir(s)\n\n",
                 file_count, total_bytes, dir_count);
        (void)write(1, tail, strnlen(tail, sizeof tail));
    }
}

static void builtin_ren(const char *arg) {
    if (is_help_switch(arg)) {
        const char *msg = "REN src dest\nRENAME src dest\n  Renames a file.\n";
        (void)write(1, msg, strlen(msg));
        return;
    }

    if (!arg || !*arg) { (void)write(1, "File not found\n", 15); return; }

    char tmp[1024];
    strncpy(tmp, arg, sizeof tmp - 1);
    tmp[sizeof tmp - 1] = 0;

    char *p = tmp;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) { (void)write(1, "File not found\n", 15); return; }

    char *src = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) *p++ = 0;

    while (*p == ' ' || *p == '\t') p++;
    char *dst = *p ? p : NULL;
    if (!dst || !*dst) { (void)write(1, "File not found\n", 15); return; }

    char src_linux[PATH_MAX];
    char dst_linux[PATH_MAX];

    if (dos_to_linux_path(src, src_linux, sizeof src_linux) != 0) {
        (void)write(1, "File not found\n", 15);
        return;
    }

    if (strchr(dst, '\\') || strchr(dst, '/') || (isalpha((unsigned char)dst[0]) && dst[1] == ':')) {
        if (dos_to_linux_path(dst, dst_linux, sizeof dst_linux) != 0) {
            (void)write(1, "File not found\n", 15);
            return;
        }
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof cwd)) { (void)write(1, "Access denied\n", 14); return; }
        snprintf(dst_linux, sizeof dst_linux, "%s/%s", cwd, dst);
    }

    struct stat st;
    if (stat(src_linux, &st) != 0) { (void)write(1, "File not found\n", 15); return; }
    if (S_ISDIR(st.st_mode)) { (void)write(1, "Access denied\n", 14); return; }

    if (rename(src_linux, dst_linux) != 0) {
        (void)write(1, "Access denied\n", 14);
        return;
    }
}

static void builtin_md(const char *arg) {
    if (is_help_switch(arg)) {
        const char *msg = "MD dir\nMKDIR dir\n  Creates a directory.\n";
        (void)write(1, msg, strlen(msg));
        return;
    }

    if (!arg || !*arg) { (void)write(1, "Invalid directory\n", 18); return; }

    char linuxp[PATH_MAX];
    if (dos_to_linux_path(arg, linuxp, sizeof linuxp) != 0) {
        (void)write(1, "Invalid drive\n", 14);
        return;
    }

    if (mkdir(linuxp, 0755) == 0) return;

    if (errno == EEXIST) (void)write(1, "A subdirectory or file already exists.\n", 39);
    else                 (void)write(1, "Access denied\n", 14);
}

static void builtin_rd(const char *arg) {
    if (is_help_switch(arg)) {
        const char *msg = "RD dir\nRMDIR dir\n  Removes an empty directory.\n";
        (void)write(1, msg, strlen(msg));
        return;
    }

    if (!arg || !*arg) { (void)write(1, "Invalid directory\n", 18); return; }

    char linuxp[PATH_MAX];
    if (dos_to_linux_path(arg, linuxp, sizeof linuxp) != 0) {
        (void)write(1, "Invalid drive\n", 14);
        return;
    }

    if (!strcmp(linuxp, DOS_C_ROOT) || !strcmp(linuxp, DOS_C_ROOT "/")) {
        (void)write(1, "Access denied\n", 14);
        return;
    }

    if (rmdir(linuxp) == 0) return;

    if (errno == ENOTEMPTY || errno == EEXIST)      (void)write(1, "The directory is not empty.\n", 28);
    else if (errno == ENOENT)                       (void)write(1, "The system cannot find the file specified.\n", 44);
    else                                            (void)write(1, "Access denied\n", 14);
}

static void builtin_copy(const char *arg) {
    if (is_help_switch(arg)) {
        const char *msg =
            "COPY src [dest]\n"
            "COPY src1+src2 dest\n"
            "  Copies file(s).\n"
            "  Wildcards supported in src: * and ?\n"
            "  Use: COPY CON file   (create file from keyboard)\n";
        (void)write(1, msg, strlen(msg));
        return;
    }

    if (!arg || !*arg) { (void)write(1, "File not found\n", 15); return; }

    char tmp[1024];
    strncpy(tmp, arg, sizeof tmp - 1);
    tmp[sizeof tmp - 1] = 0;

    char *p = tmp;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) { (void)write(1, "File not found\n", 15); return; }

    // src token
    char *src = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) *p++ = 0;

    while (*p == ' ' || *p == '\t') p++;
    char *dst = *p ? p : NULL;

    // Concat mode: SRC1+SRC2 DEST (no wildcards here)
    if (strchr(src, '+')) {
        if (!dst || !*dst) { (void)write(1, "Invalid number of parameters\n", 29); return; }
        if (has_wildcards(src) || (dst && has_wildcards(dst))) {
            (void)write(1, "Invalid number of parameters\n", 29);
            return;
        }

        char dst_linux[PATH_MAX];
        if (dos_to_linux_path(dst, dst_linux, sizeof dst_linux) != 0) {
            (void)write(1, "Invalid drive\n", 14);
            return;
        }

        int files_copied = 0;

        for (;;) {
            char *plus = strchr(src, '+');
            if (plus) *plus = 0;

            char src_linux[PATH_MAX];
            if (dos_to_linux_path(src, src_linux, sizeof src_linux) != 0) {
                (void)write(1, "File not found\n", 15);
                return;
            }

            int in = open(src_linux, O_RDONLY);
            if (in < 0) { (void)write(1, "File not found\n", 15); return; }

            int out_flags = O_WRONLY | O_CREAT;
            out_flags |= (files_copied == 0) ? O_TRUNC : O_APPEND;

            int out = open(dst_linux, out_flags, 0644);
            if (out < 0) { close(in); (void)write(1, "Access denied\n", 14); return; }

            char buf[4096];
            ssize_t n;
            while ((n = read(in, buf, sizeof buf)) > 0) {
                ssize_t off = 0;
                while (off < n) {
                    ssize_t w = write(out, buf + off, (size_t)(n - off));
                    if (w < 0) { close(in); close(out); (void)write(1, "Access denied\n", 14); return; }
                    off += w;
                }
            }

            close(in);
            close(out);
            if (n < 0) { (void)write(1, "Access denied\n", 14); return; }

            files_copied++;

            if (!plus) break;
            src = plus + 1;
            while (*src == ' ' || *src == '\t') src++;
            if (!*src) break;
        }

        char msg[64];
        snprintf(msg, sizeof msg, "        %d file(s) copied.\n", files_copied);
        (void)write(1, msg, strlen(msg));
        return;
    }

    // Normal mode (supports wildcards in src)
    char src_linuxspec[PATH_MAX];
    if (dos_to_linux_path(src, src_linuxspec, sizeof src_linuxspec) != 0) {
        (void)write(1, "File not found\n", 15);
        return;
    }

    char dst_linux[PATH_MAX];
    int have_dst = (dst && *dst);

    if (have_dst) {
        if (dos_to_linux_path(dst, dst_linux, sizeof dst_linux) != 0) {
            (void)write(1, "Invalid drive\n", 14);
            return;
        }
    }

    // Wildcard source
    if (has_wildcards(src_linuxspec)) {
        if (!have_dst) {
            (void)write(1, "Invalid number of parameters\n", 29);
            return;
        }

        char dirpath[PATH_MAX];
        char pattern[PATH_MAX];
        split_dir_pat(src_linuxspec, dirpath, sizeof dirpath, pattern, sizeof pattern);

        DIR *d = opendir(dirpath);
        if (!d) { (void)write(1, "File not found\n", 15); return; }

        int dst_is_dir = is_dir_path(dst_linux);
        int files_copied = 0;

        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            const char *name = de->d_name;
            if (!wildmatch_ci(pattern, name)) continue;

            char fullsrc[PATH_MAX * 2];
            snprintf(fullsrc, sizeof fullsrc, "%s/%s", dirpath, name);

            struct stat st;
            if (stat(fullsrc, &st) != 0) continue;
            if (!S_ISREG(st.st_mode)) continue;

            char fulldst[PATH_MAX * 2];

            if (dst_is_dir) {
                snprintf(fulldst, sizeof fulldst, "%s/%s", dst_linux, name);
            } else {
                if (files_copied >= 1) {
                    closedir(d);
                    (void)write(1, "Invalid number of parameters\n", 29);
                    return;
                }
                snprintf(fulldst, sizeof fulldst, "%s", dst_linux);
            }

            int in = open(fullsrc, O_RDONLY);
            if (in < 0) continue;

            int out = open(fulldst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (out < 0) { close(in); closedir(d); (void)write(1, "Access denied\n", 14); return; }

            char buf[4096];
            ssize_t n;
            while ((n = read(in, buf, sizeof buf)) > 0) {
                ssize_t off = 0;
                while (off < n) {
                    ssize_t w = write(out, buf + off, (size_t)(n - off));
                    if (w < 0) { close(in); close(out); closedir(d); (void)write(1, "Access denied\n", 14); return; }
                    off += w;
                }
            }

            close(in);
            close(out);
            if (n < 0) { closedir(d); (void)write(1, "Access denied\n", 14); return; }

            files_copied++;
        }

        closedir(d);

        if (files_copied == 0) { (void)write(1, "File not found\n", 15); return; }

        char msg[64];
        snprintf(msg, sizeof msg, "        %d file(s) copied.\n", files_copied);
        (void)write(1, msg, strlen(msg));
        return;
    }

    // Single-file copy (no wildcard)
    char final_dst[PATH_MAX * 2];

    if (!have_dst) {
        const char *base = dos_basename(src);
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof cwd)) { (void)write(1, "Access denied\n", 14); return; }
        snprintf(final_dst, sizeof final_dst, "%s/%s", cwd, base);
    } else if (is_dir_path(dst_linux)) {
        const char *base = dos_basename(src);
        snprintf(final_dst, sizeof final_dst, "%s/%s", dst_linux, base);
    } else {
        snprintf(final_dst, sizeof final_dst, "%s", dst_linux);
    }

    int in = open(src_linuxspec, O_RDONLY);
    if (in < 0) { (void)write(1, "File not found\n", 15); return; }

    int out = open(final_dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { close(in); (void)write(1, "Access denied\n", 14); return; }

    char buf[4096];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w < 0) { close(in); close(out); (void)write(1, "Access denied\n", 14); return; }
            off += w;
        }
    }

    close(in);
    close(out);
    if (n < 0) { (void)write(1, "Access denied\n", 14); return; }

    (void)write(1, "        1 file(s) copied.\n", 27);
}

/* --- main --- */

int main(void) {
    setsid();
    ensure_stdio();
    mount_basic_fs();

    mkdir("/dos", 0755);
    mkdir(DOS_C_ROOT, 0755);
    (void)chdir(DOS_C_ROOT);

    load_config();
    apply_color();

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigchld;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, 0);

    char line[1024];

    (void)write(1, "\nDazLab 64-DOS 0.1\nDistributed under a MIT license\n", 52);
    (void)write(1, "Type 'help' or 'poweroff'\n\n", 27);

    for (;;) {
        if (g_got_sigchld) reap_children_nonblock();

        print_prompt();

        if (!fgets(line, sizeof line, stdin)) {
            sleep(1);
            continue;
        }

        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == 0) continue;

        if (is_cmd(line, "help")) {
            const char *msg =
                "Built-ins (use /? after a command for help):\n"
                "  HELP  VER  CLS  COLOR  ECHO  PAUSE  EXIT\n"
                "  CD    DIR  TYPE\n"
                "  DEL/ERASE   REN/RENAME\n"
                "  MD/MKDIR    RD/RMDIR\n"
                "  COPY (also: COPY CON file)\n"
                "  POWEROFF\n";
            (void)write(1, msg, strlen(msg));
            continue;
        }

        if (is_cmd(line, "ver")) {
            (void)write(1, "DOS-modern 0.0.1\n", 17);
            continue;
        }

        if (is_cmd(line, "cls")) {
            builtin_cls();
            continue;
        }

        if (is_cmd(line, "color")) {
            char *arg = line + 5;
            while (*arg == ' ' || *arg == '\t') arg++;
            builtin_color(*arg ? arg : 0);
            continue;
        }

        if (is_cmd(line, "echo")) {
            char *arg = line + 4;
            while (*arg == ' ' || *arg == '\t') arg++;
            builtin_echo(*arg ? arg : 0);
            continue;
        }

        if (is_cmd(line, "pause")) {
            builtin_pause();
            continue;
        }

        if (is_cmd(line, "exit")) {
            builtin_exit();
            continue;
        }

        if (is_cmd(line, "poweroff")) {
            do_poweroff();
            continue;
        }

        if (is_cmd(line, "cd")) {
            char *arg = line + 2;
            while (*arg == ' ' || *arg == '\t') arg++;
            builtin_cd(*arg ? arg : 0);
            continue;
        }

        if (is_cmd(line, "dir")) {
            char *arg = line + 3;
            while (*arg == ' ' || *arg == '\t') arg++;
            builtin_dir(*arg ? arg : 0);
            continue;
        }

        if (is_cmd(line, "type")) {
            char *arg = line + 4;
            while (*arg == ' ' || *arg == '\t') arg++;
            builtin_type(*arg ? arg : 0);
            continue;
        }

        if (is_cmd(line, "del") || is_cmd(line, "erase")) {
            char *arg = line + (tolower((unsigned char)line[0]) == 'd' ? 3 : 5);
            while (*arg == ' ' || *arg == '\t') arg++;
            builtin_del(*arg ? arg : 0);
            continue;
        }

        if (is_cmd(line, "ren") || is_cmd(line, "rename")) {
            char *arg = line + (tolower((unsigned char)line[0]) == 'r' && tolower((unsigned char)line[1]) == 'e' ? 3 : 6);
            while (*arg == ' ' || *arg == '\t') arg++;
            builtin_ren(*arg ? arg : 0);
            continue;
        }

        if (is_cmd(line, "md") || is_cmd(line, "mkdir")) {
            char *arg = line + (tolower((unsigned char)line[1]) == 'd' ? 2 : 5);
            while (*arg == ' ' || *arg == '\t') arg++;
            builtin_md(*arg ? arg : 0);
            continue;
        }

        if (is_cmd(line, "rd") || is_cmd(line, "rmdir")) {
            char *arg = line + (tolower((unsigned char)line[1]) == 'd' ? 2 : 5);
            while (*arg == ' ' || *arg == '\t') arg++;
            builtin_rd(*arg ? arg : 0);
            continue;
        }

        if (is_cmd(line, "copy")) {
            char *arg = line + 4;
            while (*arg == ' ' || *arg == '\t') arg++;

            // COPY CON filename
            if (arg[0] &&
                tolower((unsigned char)arg[0]) == 'c' &&
                tolower((unsigned char)arg[1]) == 'o' &&
                tolower((unsigned char)arg[2]) == 'n' &&
                (arg[3] == 0 || arg[3] == ' ' || arg[3] == '\t')) {

                char *dst = arg + 3;
                while (*dst == ' ' || *dst == '\t') dst++;
                builtin_copy_con(*dst ? dst : 0);
                continue;
            }

            builtin_copy(*arg ? arg : 0);
            continue;
        }

        (void)write(1, "Bad command or file name\n", 25);
    }
}

