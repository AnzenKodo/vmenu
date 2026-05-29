/* build.c - Build system for vmenu
 *
 * Bootstrap (first time only):
 *   cc -o build build.c && ./build
 *
 * After that, ./build will recompile itself automatically when build.c changes.
 *
 * Commands:
 *   ./build              Build vmenu (default)
 *   ./build clean        Remove build artifacts
 *   ./build install      Install to PREFIX (default: /usr/local)
 *   ./build uninstall    Remove from PREFIX
 *   ./build dist         Create source tarball vmenu-VERSION.tar.gz
 *
 * Environment overrides:
 *   DESTDIR, PREFIX, MANPREFIX
 */

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

/* ── User-facing configuration ──────────────────────────────────────────── */

#define VERSION     "5.4"
#define CC          "cc"

#ifndef PREFIX
#  define PREFIX     "/usr/local"
#endif
#ifndef MANPREFIX
#  define MANPREFIX  PREFIX "/share/man"
#endif

/* X11 */
#define X11INC      "/usr/X11R6/include"
#define X11LIB      "/usr/X11R6/lib"

/* Freetype */
#define FREETYPEINC "/usr/include/freetype2"

/* Compiler flags (passed to every translation unit) */
#define CFLAGS_STR \
    "-std=c99 -pedantic -Wall -Os" \
    " -I" X11INC " -I" FREETYPEINC \
    " -D_DEFAULT_SOURCE -D_BSD_SOURCE" \
    " -D_XOPEN_SOURCE=700 -D_POSIX_C_SOURCE=200809L" \
    " -DVERSION=\\\"" VERSION "\\\"" \
    " -DXINERAMA"

/* Linker flags */
#define LDFLAGS_STR \
    "-L" X11LIB " -lX11 -lXinerama -lfontconfig -lXft"

/* Sources that compile into the vmenu binary */
static const char *vmenu_srcs[] = { "vmenu.c", "drw.c", "util.c", NULL };

/* Headers that, when changed, trigger a full rebuild */
static const char *vmenu_headers[] = { "drw.h", "util.h", "arg.h", NULL };

/* ── Utility helpers ────────────────────────────────────────────────────── */

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

/* Run a shell command, print it, return its exit code. */
static int run(const char *fmt, ...)
{
    char cmd[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    printf("  %s\n", cmd);
    int r = system(cmd);
    return WIFEXITED(r) ? WEXITSTATUS(r) : 1;
}

/* Like run() but terminate the build on failure. */
static void must(const char *fmt, ...)
{
    char cmd[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    printf("  %s\n", cmd);
    int r = system(cmd);
    int code = WIFEXITED(r) ? WEXITSTATUS(r) : 1;
    if (code != 0) {
        fprintf(stderr, "build error: command exited %d\n  %s\n", code, cmd);
        exit(1);
    }
}

static void mkdirp(const char *path)
{
    must("mkdir -p '%s'", path);
}

/* Derive the .o name from a .c name (in-place, 256-byte buffer). */
static void c_to_o(const char *src, char *out, size_t sz)
{
    size_t len = strlen(src);
    if (len >= sz) len = sz - 1;
    memcpy(out, src, len);
    out[len] = '\0';
    if (len > 2 && out[len - 2] == '.' && out[len - 1] == 'c')
        out[len - 1] = 'o';
}

/* ── Self-rebuild ───────────────────────────────────────────────────────── */

/* If build.c is newer than the running binary, recompile and re-exec. */
static void self_rebuild(int argc, char *argv[])
{
    const char *self = argv[0];
    if (!file_exists("build.c")) return;
    if (file_mtime("build.c") <= file_mtime(self)) return;

    printf("build.c changed — rebuilding...\n");
    if (run(CC " -o %s build.c", self) != 0) {
        fprintf(stderr, "build error: failed to rebuild build binary\n");
        exit(1);
    }
    execv(self, argv);
    perror("execv");
    exit(1);
}

/* ── Compile one .c → .o, skip if up to date ────────────────────────────── */

/* Returns 1 if already up to date (no recompile needed). */
static int compile_obj(const char *src)
{
    char obj[256];
    c_to_o(src, obj, sizeof obj);

    int stale = !file_exists(obj) || file_mtime(src) > file_mtime(obj);
    if (!stale) {
        for (int h = 0; vmenu_headers[h] && !stale; h++)
            if (file_exists(vmenu_headers[h]) &&
                file_mtime(vmenu_headers[h]) > file_mtime(obj))
                stale = 1;
    }

    if (!stale) {
        printf("  [up to date] %s\n", obj);
        return 1;
    }
    must(CC " -c " CFLAGS_STR " -o %s %s", obj, src);
    return 0;
}

/* ── Build targets ──────────────────────────────────────────────────────── */

static void build_vmenu(void)
{
    printf("Building vmenu " VERSION "...\n");

    int any_rebuilt = 0;
    for (int i = 0; vmenu_srcs[i]; i++)
        if (!compile_obj(vmenu_srcs[i]))
            any_rebuilt = 1;

    if (!any_rebuilt && file_exists("vmenu")) {
        printf("  [up to date] vmenu\n");
        return;
    }

    /* assemble the object list */
    char objlist[1024] = "";
    for (int i = 0; vmenu_srcs[i]; i++) {
        char obj[256];
        c_to_o(vmenu_srcs[i], obj, sizeof obj);
        if (i) strncat(objlist, " ", sizeof objlist - strlen(objlist) - 1);
        strncat(objlist, obj, sizeof objlist - strlen(objlist) - 1);
    }

    must(CC " -o vmenu %s " LDFLAGS_STR, objlist);
    printf("Done: ./vmenu\n");
}

static void do_clean(void)
{
    printf("Cleaning...\n");
    const char *rm[] = { "vmenu", "vmenu.o", "drw.o", "util.o", NULL };
    for (int i = 0; rm[i]; i++)
        if (file_exists(rm[i]))
            must("rm -f '%s'", rm[i]);
    run("rm -f vmenu-" VERSION ".tar.gz");
    printf("Clean.\n");
}

static void do_install(void)
{
    if (!file_exists("vmenu")) {
        fprintf(stderr, "error: vmenu is not built yet — run ./build first\n");
        exit(1);
    }

    const char *destdir = getenv("DESTDIR")   ? getenv("DESTDIR")   : "";
    const char *pfx     = getenv("PREFIX")    ? getenv("PREFIX")    : PREFIX;
    const char *manpfx  = getenv("MANPREFIX") ? getenv("MANPREFIX") : MANPREFIX;

    char bindir[512], man1dir[512];
    snprintf(bindir,   sizeof bindir,   "%s%s/bin",       destdir, pfx);
    snprintf(man1dir,  sizeof man1dir,  "%s%s/man1",      destdir, manpfx);

    printf("Installing binaries → %s\n", bindir);
    mkdirp(bindir);
    must("cp -f vmenu       '%s/vmenu'",       bindir);
    must("cp -f vmenu_path  '%s/vmenu_path'",  bindir);
    must("cp -f vmenu_run   '%s/vmenu_run'",   bindir);
    must("chmod 755 '%s/vmenu' '%s/vmenu_path' '%s/vmenu_run'",
         bindir, bindir, bindir);

    printf("Installing man page → %s\n", man1dir);
    mkdirp(man1dir);
    must("sed 's/VERSION/" VERSION "/g' < vmenu.1 > '%s/vmenu.1'", man1dir);
    must("chmod 644 '%s/vmenu.1'", man1dir);

    printf("Installed.\n");
}

static void do_uninstall(void)
{
    const char *destdir = getenv("DESTDIR")   ? getenv("DESTDIR")   : "";
    const char *pfx     = getenv("PREFIX")    ? getenv("PREFIX")    : PREFIX;
    const char *manpfx  = getenv("MANPREFIX") ? getenv("MANPREFIX") : MANPREFIX;

    char bindir[512], man1dir[512];
    snprintf(bindir,   sizeof bindir,   "%s%s/bin",   destdir, pfx);
    snprintf(man1dir,  sizeof man1dir,  "%s%s/man1",  destdir, manpfx);

    printf("Uninstalling...\n");
    run("rm -f '%s/vmenu' '%s/vmenu_path' '%s/vmenu_run'",
        bindir, bindir, bindir);
    run("rm -f '%s/vmenu.1'", man1dir);
    printf("Uninstalled.\n");
}

static void do_dist(void)
{
    const char *dir = "vmenu-" VERSION;
    const char *tgz = "vmenu-" VERSION ".tar.gz";

    printf("Creating %s...\n", tgz);
    run("rm -rf '%s'", dir);
    mkdirp(dir);

    const char *files[] = {
        "LICENSE", "README", "build.c",
        "arg.h", "drw.h", "drw.c",
        "util.h", "util.c",
        "vmenu.c", "vmenu.1",
        "vmenu_path", "vmenu_run",
        NULL
    };
    for (int i = 0; files[i]; i++)
        must("cp '%s' '%s/'", files[i], dir);

    must("tar -czf '%s' '%s'", tgz, dir);
    run("rm -rf '%s'", dir);
    printf("Created: %s\n", tgz);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [command]\n"
        "\n"
        "Commands:\n"
        "  (none)       Build vmenu (default)\n"
        "  clean        Remove build artifacts\n"
        "  install      Install to PREFIX\n"
        "  uninstall    Remove from PREFIX\n"
        "  dist         Create source tarball\n"
        "\n"
        "Environment:\n"
        "  DESTDIR      Staging directory prefix  (default: empty)\n"
        "  PREFIX       Install prefix            (default: " PREFIX ")\n"
        "  MANPREFIX    Man page install prefix   (default: " MANPREFIX ")\n",
        argv0);
}

/* ── Entry point ────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    self_rebuild(argc, argv);

    if (argc < 2) {
        build_vmenu();
        return 0;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "clean") == 0)           do_clean();
    else if (strcmp(cmd, "install") == 0)  { build_vmenu(); do_install(); }
    else if (strcmp(cmd, "uninstall") == 0)  do_uninstall();
    else if (strcmp(cmd, "dist") == 0)       do_dist();
    else {
        fprintf(stderr, "error: unknown command '%s'\n\n", cmd);
        usage(argv[0]);
        return 1;
    }

    return 0;
}
