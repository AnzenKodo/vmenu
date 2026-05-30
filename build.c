/* build.c - Build system for vmenu */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

/* ── Configuration ──────────────────────────────────────────────────────── */

#define BUILD_DIR   "build"

static char version_buf[128] = "";
static const char *get_version(void)
{
    if (version_buf[0] != '\0') return version_buf;
    FILE *fp = fopen("VERSION", "r");
    if (!fp) {
        strcpy(version_buf, "unknown");
        return version_buf;
    }
    if (fgets(version_buf, sizeof(version_buf), fp)) {
        size_t len = strlen(version_buf);
        while (len > 0 && (version_buf[len-1] == '\r' || version_buf[len-1] == '\n' || version_buf[len-1] == ' ' || version_buf[len-1] == '\t')) {
            version_buf[len-1] = '\0';
            len--;
        }
    } else {
        strcpy(version_buf, "unknown");
    }
    fclose(fp);
    return version_buf;
}
#define VERSION get_version()

static const char *get_cc(void)
{
    const char *env_cc = getenv("CC");
    return env_cc ? env_cc : "cc";
}
#define CC get_cc()

#ifndef PREFIX
#  define PREFIX     "/usr/local"
#endif
#ifndef MANPREFIX
#  define MANPREFIX  PREFIX "/share/man"
#endif

#define X11INC      "/usr/X11R6/include"
#define X11LIB      "/usr/X11R6/lib"
#define FREETYPEINC "/usr/include/freetype2"

/* Common flags shared by all build types */
#define CFLAGS_COMMON \
    "-std=c99 -pedantic -Wall -Wextra" \
    " -I" X11INC " -I" FREETYPEINC \
    " -D_DEFAULT_SOURCE -D_BSD_SOURCE" \
    " -D_XOPEN_SOURCE=700 -D_POSIX_C_SOURCE=200809L" \
    " -DXINERAMA"

/* Release: optimized, no debug symbols */
#define CFLAGS_RELEASE  CFLAGS_COMMON " -O3"

/* Dev: debug friendly, full debug info, sanitizers */
#define CFLAGS_DEV \
    CFLAGS_COMMON \
    " -O0 -g3 -ggdb" \
    " -DBUILD_DEBUG=1" \
    " -fsanitize=address,undefined" \
    " -fno-omit-frame-pointer"

/* Debug: debug friendly, full debug info, NO sanitizers */
#define CFLAGS_DEBUG \
    CFLAGS_COMMON \
    " -O0 -g3 -ggdb" \
    " -DBUILD_DEBUG=1"

#define LDFLAGS_STR \
    "-L" X11LIB " -lX11 -lXinerama -lfontconfig -lXft"

/* Dev linker needs sanitizer runtime */
#define LDFLAGS_DEV_STR \
    LDFLAGS_STR " -fsanitize=address,undefined"

/* Source files */
static const char *vmenu_srcs[]    = { "vmenu.c", "drw.c", "util.c", NULL };
static const char *vmenu_headers[] = { "drw.h", "util.h", NULL };

/* ── Build type ─────────────────────────────────────────────────────────── */

typedef enum {
    BUILD_DEV,
    BUILD_DEBUG,
    BUILD_RELEASE
} Build_Type;

static const char *build_type_name(Build_Type t)
{
    switch (t) {
        case BUILD_DEV:     return "dev";
        case BUILD_DEBUG:   return "debug";
        case BUILD_RELEASE: return "release";
    }
    return "unknown";
}

/* Binary path for a given build type */
static void bin_path(Build_Type t, char *out, size_t sz)
{
    switch (t) {
        case BUILD_DEV:
            snprintf(out, sz, BUILD_DIR "/vmenu_dev");
            break;
        case BUILD_DEBUG:
            snprintf(out, sz, BUILD_DIR "/vmenu_debug");
            break;
        case BUILD_RELEASE:
            snprintf(out, sz, BUILD_DIR "/vmenu");
            break;
    }
}

/* ── Utility ────────────────────────────────────────────────────────────── */

static int file_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

static time_t file_mtime(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 ? st.st_mtime : 0;
}

static int run(const char *fmt, ...)
{
    char cmd[8192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    printf("  %s\n", cmd);
    int r = system(cmd);
    return WIFEXITED(r) ? WEXITSTATUS(r) : 1;
}

static void must(const char *fmt, ...)
{
    char cmd[8192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    printf("  %s\n", cmd);
    int r = system(cmd);
    int code = WIFEXITED(r) ? WEXITSTATUS(r) : 1;
    if (code != 0) {
        fprintf(stderr, "build error (exit %d): %s\n", code, cmd);
        exit(1);
    }
}

static void mkdirp(const char *path)
{
    must("mkdir -p '%s'", path);
}

/* Derive .o path inside BUILD_DIR from a .c filename and Build_Type. */
static void obj_path(const char *src, Build_Type t, char *out, size_t sz)
{
    /* e.g. "vmenu.c" and BUILD_DEV → "build/vmenu_dev.o" */
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    char tmp[256];
    size_t len = strlen(base);
    if (len >= sizeof tmp) len = sizeof tmp - 1;
    memcpy(tmp, base, len);
    tmp[len] = '\0';
    if (len > 2 && tmp[len-2] == '.' && tmp[len-1] == 'c')
        tmp[len-2] = '\0'; /* Strip ".c" */
    snprintf(out, sz, BUILD_DIR "/%s_%s.o", tmp, build_type_name(t));
}

/* ── Self-rebuild ───────────────────────────────────────────────────────── */

static void self_rebuild(int argc, char *argv[])
{
    const char *self = argv[0];
    if (!file_exists("build.c")) return;
    if (file_mtime("build.c") <= file_mtime(self)) return;

    printf("build.c changed — rebuilding...\n");
    if (run("cc -o %s build.c", self) != 0) {
        fprintf(stderr, "build error: could not rebuild build binary\n");
        exit(1);
    }
    execv(self, argv);
    perror("execv");
    exit(1);
}

/* ── Compile one .c → build/.o, skip if up to date ─────────────────────── */

/* Returns 1 if already up to date. */
static int compile_obj(const char *src, Build_Type t)
{
    char obj[512];
    obj_path(src, t, obj, sizeof obj);

    int stale = !file_exists(obj) || file_mtime(src) > file_mtime(obj);
    for (int h = 0; !stale && vmenu_headers[h]; h++)
        if (file_exists(vmenu_headers[h]) &&
            file_mtime(vmenu_headers[h]) > file_mtime(obj))
            stale = 1;

    if (!stale) {
        printf("  [up to date] %s\n", obj);
        return 1;
    }

    const char *cflags;
    switch (t) {
        case BUILD_DEV:     cflags = CFLAGS_DEV;     break;
        case BUILD_DEBUG:   cflags = CFLAGS_DEBUG;   break;
        case BUILD_RELEASE: cflags = CFLAGS_RELEASE; break;
        default:            cflags = CFLAGS_DEV;     break;
    }

    char cflags_buf[8192];
    snprintf(cflags_buf, sizeof(cflags_buf), "%s -DVERSION=\\\"%s\\\"", cflags, VERSION);
    must("%s -c %s -o %s %s", CC, cflags_buf, obj, src);
    return 0;
}

/* ── Build ──────────────────────────────────────────────────────────────── */

static void do_build(Build_Type t)
{
    char bin[512];
    bin_path(t, bin, sizeof bin);

    printf("Building vmenu %s [%s] → %s\n", VERSION, build_type_name(t), bin);
    mkdirp(BUILD_DIR);

    int any_rebuilt = 0;
    for (int i = 0; vmenu_srcs[i]; i++)
        if (!compile_obj(vmenu_srcs[i], t))
            any_rebuilt = 1;

    if (!any_rebuilt && file_exists(bin)) {
        printf("  [up to date] %s\n", bin);
        return;
    }

    /* build object list */
    char objlist[2048] = "";
    for (int i = 0; vmenu_srcs[i]; i++) {
        char obj[512];
        obj_path(vmenu_srcs[i], t, obj, sizeof obj);
        if (i) strncat(objlist, " ", sizeof objlist - strlen(objlist) - 1);
        strncat(objlist, obj, sizeof objlist - strlen(objlist) - 1);
    }

    const char *ldflags = (t == BUILD_DEV) ? LDFLAGS_DEV_STR : LDFLAGS_STR;
    must("%s -o %s %s %s", CC, bin, objlist, ldflags);
    printf("Done: %s\n", bin);
}

/* ── Run ────────────────────────────────────────────────────────────────── */

static void do_run(Build_Type t, int argc, char *argv[], int arg_start)
{
    char bin[512];
    bin_path(t, bin, sizeof bin);

    if (!file_exists(bin)) {
        fprintf(stderr, "error: %s not found — build it first\n", bin);
        exit(1);
    }

    printf("Running [%s]: %s\n", build_type_name(t), bin);

    /* Construct the command line with optional arguments */
    char cmd[8192];
    int len = snprintf(cmd, sizeof(cmd), "echo -e 'option 1\\noption 2\\noption 3' | %s", bin);
    for (int i = arg_start; i < argc; i++) {
        len += snprintf(cmd + len, sizeof(cmd) - len, " '%s'", argv[i]);
    }

    must("%s", cmd);
}

/* ── Clean ──────────────────────────────────────────────────────────────── */

static void do_clean(void)
{
    printf("Cleaning build artifacts...\n");
    run("rm -f " BUILD_DIR "/*.o");
    run("rm -f " BUILD_DIR "/vmenu " BUILD_DIR "/vmenu_dev " BUILD_DIR "/vmenu_debug");
    run("rm -f vmenu vmenu.o drw.o util.o dmenu.o stest.o a.out");
    run("rm -f vmenu-%s.tar.gz", VERSION);
    printf("Clean.\n");
}

/* ── Install / Uninstall ────────────────────────────────────────────────── */

static void do_install(void)
{
    char bin[512];
    bin_path(BUILD_RELEASE, bin, sizeof bin);

    if (!file_exists(bin)) {
        fprintf(stderr, "error: release build not found (%s)\n"
                        "       run './build build release' first\n", bin);
        exit(1);
    }

    const char *destdir = getenv("DESTDIR")   ? getenv("DESTDIR")   : "";
    const char *pfx     = getenv("PREFIX")    ? getenv("PREFIX")    : PREFIX;
    const char *manpfx  = getenv("MANPREFIX") ? getenv("MANPREFIX") : MANPREFIX;

    char bindir[512], man1dir[512];
    snprintf(bindir,  sizeof bindir,  "%s%s/bin",  destdir, pfx);
    snprintf(man1dir, sizeof man1dir, "%s%s/man1", destdir, manpfx);

    printf("Installing binaries → %s\n", bindir);
    mkdirp(bindir);
    must("cp -f '%s'       '%s/vmenu'",      bin,    bindir);
    must("cp -f vmenu_path '%s/vmenu_path'", bindir);
    must("cp -f vmenu_run  '%s/vmenu_run'",  bindir);
    must("chmod 755 '%s/vmenu' '%s/vmenu_path' '%s/vmenu_run'",
         bindir, bindir, bindir);

    printf("Installing man page → %s\n", man1dir);
    mkdirp(man1dir);
    must("sed 's/VERSION/%s/g' < man/vmenu.1 > '%s/vmenu.1'", VERSION, man1dir);
    must("chmod 644 '%s/vmenu.1'", man1dir);

    printf("Installed.\n");
}

static void do_uninstall(void)
{
    const char *destdir = getenv("DESTDIR")   ? getenv("DESTDIR")   : "";
    const char *pfx     = getenv("PREFIX")    ? getenv("PREFIX")    : PREFIX;
    const char *manpfx  = getenv("MANPREFIX") ? getenv("MANPREFIX") : MANPREFIX;

    char bindir[512], man1dir[512];
    snprintf(bindir,  sizeof bindir,  "%s%s/bin",  destdir, pfx);
    snprintf(man1dir, sizeof man1dir, "%s%s/man1", destdir, manpfx);

    printf("Uninstalling...\n");
    run("rm -f '%s/vmenu' '%s/vmenu_path' '%s/vmenu_run'",
         bindir, bindir, bindir);
    run("rm -f '%s/vmenu.1'", man1dir);
    printf("Uninstalled.\n");
}

/* ── Dist ───────────────────────────────────────────────────────────────── */

static void do_dist(void)
{
    char dir[512];
    char tgz[512];
    snprintf(dir, sizeof(dir), "vmenu-%s", VERSION);
    snprintf(tgz, sizeof(tgz), "vmenu-%s.tar.gz", VERSION);

    printf("Creating %s...\n", tgz);
    run("rm -rf '%s'", dir);
    mkdirp(dir);

    const char *files[] = {
        "LICENSE", "README.md", "build.c", "drw.h", "drw.c", "util.h", "util.c",
        "vmenu.c", "man/vmenu.1", NULL
    };
    for (int i = 0; files[i]; i++)
        must("cp '%s' '%s/'", files[i], dir);

    must("tar -czf '%s' '%s'", tgz, dir);
    run("rm -rf '%s'", dir);
    printf("Created: %s\n", tgz);
}

/* ── Usage ──────────────────────────────────────────────────────────────── */

static void usage(const char *argv0)
{
    printf(
        "usage: %s [command] [type/args]\n"
        "\n"
        "Commands:\n"
        "  build            Build target (default type: dev)\n"
        "  run              Run target (default type: dev)\n"
        "  build-run        Build and run target (default type: dev)\n"
        "  build-debugger   Build debugger target (type: debug)\n"
        "  generate-version Generate version number from current date\n"
        "  set-version      Set version number (writes to VERSION file)\n"
        "  clean            Remove build artifacts\n"
        "  install          Install release build to PREFIX\n"
        "  uninstall        Remove installed files\n"
        "  dist             Create source tarball\n"
        "\n"
        "Types:\n"
        "  dev              Debug-friendly + sanitizers → " BUILD_DIR "/vmenu_dev\n"
        "  debug            Debug-friendly + no sanitizers → " BUILD_DIR "/vmenu_debug\n"
        "  release          Highly optimized → " BUILD_DIR "/vmenu\n"
        "\n"
        "Environment:\n"
        "  DESTDIR          Staging prefix      (default: empty)\n"
        "  PREFIX           Install prefix      (default: " PREFIX ")\n"
        "  MANPREFIX        Man page prefix     (default: " MANPREFIX ")\n",
        argv0);
}

/* ── Entry point ────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    self_rebuild(argc, argv);

    /* default: build dev */
    if (argc < 2) {
        do_build(BUILD_DEV);
        return 0;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "build") == 0) {
        Build_Type t = BUILD_DEV;
        if (argc >= 3) {
            if (strcmp(argv[2], "release") == 0)
                t = BUILD_RELEASE;
            else if (strcmp(argv[2], "debug") == 0)
                t = BUILD_DEBUG;
            else if (strcmp(argv[2], "dev") == 0)
                t = BUILD_DEV;
            else {
                fprintf(stderr, "error: unknown build type '%s'\n", argv[2]);
                return 1;
            }
        }
        do_build(t);
        return 0;
    }

    if (strcmp(cmd, "run") == 0) {
        Build_Type t = BUILD_DEV;
        int arg_start = 2;
        if (argc >= 3) {
            if (strcmp(argv[2], "release") == 0) {
                t = BUILD_RELEASE;
                arg_start = 3;
            } else if (strcmp(argv[2], "debug") == 0) {
                t = BUILD_DEBUG;
                arg_start = 3;
            } else if (strcmp(argv[2], "dev") == 0) {
                t = BUILD_DEV;
                arg_start = 3;
            }
        }
        do_run(t, argc, argv, arg_start);
        return 0;
    }

    if (strcmp(cmd, "build-run") == 0) {
        Build_Type t = BUILD_DEV;
        int arg_start = 2;
        if (argc >= 3) {
            if (strcmp(argv[2], "release") == 0) {
                t = BUILD_RELEASE;
                arg_start = 3;
            } else if (strcmp(argv[2], "debug") == 0) {
                t = BUILD_DEBUG;
                arg_start = 3;
            } else if (strcmp(argv[2], "dev") == 0) {
                t = BUILD_DEV;
                arg_start = 3;
            }
        }
        do_build(t);
        do_run(t, argc, argv, arg_start);
        return 0;
    }

    if (strcmp(cmd, "build-debugger") == 0) {
        do_build(BUILD_DEBUG);
        return 0;
    }

    if (strcmp(cmd, "clean") == 0) {
        do_clean();
        return 0;
    }

    if (strcmp(cmd, "install") == 0) {
        do_build(BUILD_RELEASE);
        do_install();
        return 0;
    }

    if (strcmp(cmd, "uninstall") == 0) {
        do_uninstall();
        return 0;
    }

    if (strcmp(cmd, "dist") == 0) {
        do_dist();
        return 0;
    }

    if (strcmp(cmd, "generate-version") == 0) {
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        char date_buf[64];
        if (tm) {
            snprintf(date_buf, sizeof(date_buf), "%04d.%02d", tm->tm_year + 1900, tm->tm_mon + 1);
        } else {
            strcpy(date_buf, "unknown-date");
        }
        FILE *fp = fopen("VERSION", "w");
        if (!fp) {
            fprintf(stderr, "error: could not write to VERSION file\n");
            return 1;
        }
        fprintf(fp, "%s\n", date_buf);
        fclose(fp);
        printf("Version generated and saved: %s\n", date_buf);
        return 0;
    }

    if (strcmp(cmd, "set-version") == 0) {
        if (argc < 3) {
            fprintf(stderr, "error: missing version argument\n");
            return 1;
        }
        FILE *fp = fopen("VERSION", "w");
        if (!fp) {
            fprintf(stderr, "error: could not write to VERSION file\n");
            return 1;
        }
        fprintf(fp, "%s\n", argv[2]);
        fclose(fp);
        printf("Version updated to: %s\n", argv[2]);
        return 0;
    }

    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        usage(argv[0]);
        return 0;
    }

    fprintf(stderr, "error: unknown command '%s'\n\n", cmd);
    usage(argv[0]);
    return 1;
}
