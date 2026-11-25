/*
 * pulsar-run: launch a macOS .app through pulsar-appexec, providing a
 * Wayland compositor when the host session only has X11.
 *
 * The macOS runtime itself is still the shipped appexec binary. This program
 * owns the environment: runtime root/SONAME paths, a private Wayland socket,
 * compositor lifetime, and child exit propagation.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "dmgmount.h"

extern char **environ;

static const char *prog = "pulsar-run";
static char *g_appexec;
static char *g_app;
static char *g_libpath;
static char *g_wayland_display;
static char *g_runtime_dir;
static char *g_kwin_log;
static int g_width = 1600;
static int g_height = 1000;
static int g_comp_timeout_ms = 10000;
static pid_t g_compositor = -1;
static pid_t g_app_pid = -1;
static bool g_keep_compositor;
static bool g_mount_only;

static void msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s: ", prog);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s: ", prog);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(127);
}

static char *xstrdup(const char *s)
{
    char *p = strdup(s ? s : "");
    if (!p)
        die("out of memory");
    return p;
}

static char *fmt(const char *f, ...)
{
    va_list ap;
    char *out = NULL;
    va_start(ap, f);
    if (vasprintf(&out, f, ap) < 0)
        die("out of memory");
    va_end(ap);
    return out;
}

static bool exists(const char *p)
{
    struct stat st;
    return p && stat(p, &st) == 0;
}

static bool is_socket(const char *p)
{
    struct stat st;
    return p && stat(p, &st) == 0 && S_ISSOCK(st.st_mode);
}

static char *path_join(const char *a, const char *b)
{
    if (!a || !*a)
        return xstrdup(b);
    if (!b || !*b)
        return xstrdup(a);
    size_t n = strlen(a) + strlen(b) + 2;
    char *p = malloc(n);
    if (!p)
        die("out of memory");
    snprintf(p, n, "%s/%s", a, b);
    return p;
}

static char *self_path(void)
{
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n < 0)
        die("cannot resolve /proc/self/exe: %s", strerror(errno));
    buf[n] = 0;
    return xstrdup(buf);
}

static char *dir_name(const char *path)
{
    char *p = xstrdup(path);
    char *s = strrchr(p, '/');
    if (!s)
        return xstrdup(".");
    if (s == p)
        s[1] = 0;
    else
        *s = 0;
    return p;
}

static char *find_workspace(const char *self)
{
    char *d = dir_name(self);
    for (int i = 0; i < 6; i++) {
        char *exe = path_join(d, "runtime/bin/appexec");
        char *app = path_join(d, "app/CotEditor.app");
        bool ok = exists(exe) && exists(app);
        free(exe);
        free(app);
        if (ok)
            return d;
        char *up = dir_name(d);
        if (!strcmp(up, d)) {
            free(up);
            break;
        }
        free(d);
        d = up;
    }
    return d;
}

static char *default_runtime_dir(void)
{
    const char *x = getenv("XDG_RUNTIME_DIR");
    if (x && *x)
        return xstrdup(x);
    uid_t uid = getuid();
    char *p = fmt("/tmp/pulsar-run-%u", (unsigned)uid);
    if (mkdir(p, 0700) < 0 && errno != EEXIST)
        die("cannot create %s: %s", p, strerror(errno));
    return p;
}

static char *socket_path(const char *runtime, const char *display)
{
    if (!display || !*display)
        return NULL;
    if (display[0] == '/')
        return xstrdup(display);
    return path_join(runtime, display);
}

static void kill_child(pid_t pid, int sig)
{
    if (pid > 0)
        kill(pid, sig);
}

static void cleanup(void)
{
    if (g_compositor > 0) {
        kill_child(g_compositor, SIGTERM);
        for (int i = 0; i < 40; i++) {
            int st;
            if (waitpid(g_compositor, &st, WNOHANG) == g_compositor)
                break;
            struct timespec ts = {0, 25 * 1000 * 1000};
            nanosleep(&ts, NULL);
        }
        kill_child(g_compositor, SIGKILL);
        waitpid(g_compositor, NULL, WNOHANG);
        g_compositor = -1;
    }
}

static void on_signal(int sig)
{
    if (g_app_pid > 0)
        kill_child(g_app_pid, sig);
    if (g_compositor > 0)
        kill_child(g_compositor, SIGTERM);
}

static void install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    atexit(cleanup);
}

static bool run_compositor(void)
{
    char *sock = fmt("plsr-wl-%ld", (long)getpid());
    g_kwin_log = path_join(g_runtime_dir, "plsr-kwin.log");

    pid_t pid = fork();
    if (pid < 0)
        die("fork: %s", strerror(errno));
    if (pid == 0) {
        int fd = open(g_kwin_log, O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO)
                close(fd);
        }
        char w[32], h[32];
        snprintf(w, sizeof(w), "%d", g_width);
        snprintf(h, sizeof(h), "%d", g_height);
        char *argv[] = {
            "kwin_wayland", "--virtual", "--socket", sock,
            "--width", w, "--height", h, NULL
        };
        execvp(argv[0], argv);
        _exit(127);
    }

    g_compositor = pid;
    char *sp = socket_path(g_runtime_dir, sock);
    int waited = 0;
    while (waited < g_comp_timeout_ms) {
        if (is_socket(sp)) {
            g_wayland_display = sock;
            msg("compositor ready: WAYLAND_DISPLAY=%s", sock);
            free(sp);
            return true;
        }
        int st;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) {
            g_compositor = -1;
            die("kwin_wayland exited before creating a socket (log: %s)", g_kwin_log);
        }
        struct timespec ts = {0, 50 * 1000 * 1000};
        nanosleep(&ts, NULL);
        waited += 50;
    }
    free(sp);
    die("timed out waiting for kwin_wayland socket %s", sock);
    return false;
}

static void ensure_compositor(void)
{
    const char *display = getenv("WAYLAND_DISPLAY");
    if (display && *display) {
        char *sp = socket_path(g_runtime_dir, display);
        if (is_socket(sp)) {
            g_wayland_display = xstrdup(display);
            msg("using existing WAYLAND_DISPLAY=%s", display);
            free(sp);
            return;
        }
        free(sp);
        msg("WAYLAND_DISPLAY=%s is not reachable; starting a private compositor", display);
    } else {
        msg("no WAYLAND_DISPLAY; starting a private compositor");
    }
    run_compositor();
}

static void usage(FILE *out)
{
    fprintf(out,
        "usage: %s [options] [--] [app] [app-args...]\n"
        "\n"
        "  --app PATH             .app bundle, Mach-O executable, or .dmg image\n"
        "  --mount-only           mount a .dmg, print the .app path, and exit\n"
        "  --workspace PATH       directory containing runtime/ and app/\n"
        "  --appexec PATH         appexec binary (default: WORKSPACE/runtime/bin/appexec)\n"
        "  --geometry WxH         private compositor size (default 1600x1000)\n"
        "  --private-compositor   always start a private kwin_wayland\n"
        "  --no-compositor        fail instead of starting kwin_wayland\n"
        "  --keep-compositor      leave the private compositor running on exit\n"
        "  --timeout MS           compositor startup timeout (default 10000)\n"
        "  -h, --help             show this help\n"
        "\n"
        "environment: PULSAR_WORKSPACE, PULSAR_APP, PULSAR_APPEXEC,\n"
        "             PULSAR_DMG_CACHE, PULSAR_7Z,\n"
        "             XDG_RUNTIME_DIR, WAYLAND_DISPLAY\n",
        prog);
}

int main(int argc, char **argv)
{
    const char *self = NULL;
    char *selfbuf = self_path();
    char *workspace = NULL;
    const char *app_env = getenv("PULSAR_APP");
    const char *exe_env = getenv("PULSAR_APPEXEC");
    const char *ws_env = getenv("PULSAR_WORKSPACE");
    const char *app_arg = NULL;
    const char *exe_arg = NULL;
    bool no_compositor = false;
    bool force_compositor = false;
    int i = 1;

    self = selfbuf;
    if (ws_env && *ws_env)
        workspace = xstrdup(ws_env);
    else
        workspace = find_workspace(self);

    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) {
            i++;
            break;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(a, "--app") && i + 1 < argc) {
            app_arg = argv[++i];
        } else if (!strcmp(a, "--appexec") && i + 1 < argc) {
            exe_arg = argv[++i];
        } else if (!strcmp(a, "--workspace") && i + 1 < argc) {
            free(workspace);
            workspace = xstrdup(argv[++i]);
        } else if (!strcmp(a, "--geometry") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &g_width, &g_height) != 2)
                die("bad geometry: %s", argv[i]);
        } else if (!strcmp(a, "--timeout") && i + 1 < argc) {
            g_comp_timeout_ms = atoi(argv[++i]);
        } else if (!strcmp(a, "--no-compositor")) {
            no_compositor = true;
        } else if (!strcmp(a, "--private-compositor")) {
            force_compositor = true;
        } else if (!strcmp(a, "--keep-compositor")) {
            g_keep_compositor = true;
        } else if (!strcmp(a, "--mount-only")) {
            g_mount_only = true;
        } else if (a[0] == '-') {
            die("unknown option: %s", a);
        } else {
            app_arg = a;
            i++;
            break;
        }
    }

    if (no_compositor && force_compositor)
        die("--private-compositor conflicts with --no-compositor");
    if (!workspace || !exists(workspace))
        die("workspace not found; use --workspace");
    if (exe_arg)
        g_appexec = xstrdup(exe_arg);
    else if (exe_env && *exe_env)
        g_appexec = xstrdup(exe_env);
    else
        g_appexec = path_join(workspace, "runtime/bin/appexec");
    if (access(g_appexec, X_OK) != 0)
        die("appexec is not executable: %s", g_appexec);

    if (app_arg)
        g_app = xstrdup(app_arg);
    else if (app_env && *app_env)
        g_app = xstrdup(app_env);
    else
        g_app = path_join(workspace, "app/CotEditor.app");
    if (!exists(g_app))
        die("app not found: %s", g_app);

    /* Launch straight from a disk image: mount it, then use the .app inside. */
    if (plsr_is_dmg(g_app)) {
        char *dmg = xstrdup(g_app);
        char *mount_dir = NULL;
        char *err = strdup("cannot mount disk image");
        char *app = plsr_mount_dmg(dmg, &mount_dir, &err);
        if (!app)
            die("cannot mount %s: %s", dmg, err ? err : "unknown error");
        msg("mounted %s at %s", dmg, mount_dir ? mount_dir : "?");
        free(g_app);
        g_app = app;
        free(mount_dir);
        free(err);
        free(dmg);
    }
    msg("app: %s", g_app);

    if (g_mount_only) {
        printf("%s\n", g_app);
        return 0;
    }

    g_runtime_dir = default_runtime_dir();
    if (chmod(g_runtime_dir, 0700) < 0) {
        /* not fatal for an existing shared runtime dir */
    }

    char *compat = path_join(workspace, "libs/compat");
    char *host = path_join(workspace, "libs/host");
    char *lib = path_join(workspace, "libs/lib");
    const char *old_ld = getenv("LD_LIBRARY_PATH");
    if (exists(compat) && exists(host) && exists(lib)) {
        if (old_ld && *old_ld)
            g_libpath = fmt("%s:%s:%s:%s", compat, host, lib, old_ld);
        else
            g_libpath = fmt("%s:%s:%s", compat, host, lib);
    } else {
        char *root = path_join(workspace, "runtime/lib/pulsarappexec");
        g_libpath = fmt("%s/compat:%s/host:%s/lib", root, root, root);
        free(root);
    }
    free(compat);
    free(host);
    free(lib);

    if (force_compositor) {
        msg("forcing a private Wayland compositor");
        run_compositor();
    } else if (no_compositor) {
        const char *d = getenv("WAYLAND_DISPLAY");
        if (!d || !*d)
            die("--no-compositor: WAYLAND_DISPLAY is not set");
        char *sp = socket_path(g_runtime_dir, d);
        if (!is_socket(sp))
            die("--no-compositor: Wayland socket is not reachable: %s", sp);
        free(sp);
        g_wayland_display = xstrdup(d);
        msg("using existing WAYLAND_DISPLAY=%s", d);
    } else {
        ensure_compositor();
    }

    install_signals();

    /* Build the app argv: appexec <app> [app-args...] */
    int app_argc = argc - i;
    char **app_argv = calloc((size_t)app_argc + 3, sizeof(char *));
    if (!app_argv)
        die("out of memory");
    app_argv[0] = g_appexec;
    app_argv[1] = g_app;
    for (int k = 0; k < app_argc; k++)
        app_argv[k + 2] = argv[i + k];
    app_argv[app_argc + 2] = NULL;

    pid_t pid = fork();
    if (pid < 0)
        die("fork: %s", strerror(errno));
    if (pid == 0) {
        setenv("XDG_RUNTIME_DIR", g_runtime_dir, 1);
        if (g_wayland_display)
            setenv("WAYLAND_DISPLAY", g_wayland_display, 1);
        setenv("LD_LIBRARY_PATH", g_libpath, 1);
        execv(g_appexec, app_argv);
        _exit(127);
    }
    g_app_pid = pid;

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            msg("waitpid: %s", strerror(errno));
            status = 127 << 8;
            break;
        }
    }
    g_app_pid = -1;

    if (!g_keep_compositor)
        cleanup();

    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 127;
}
