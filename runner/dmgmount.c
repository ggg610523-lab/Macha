/*
 * dmgmount: extract an Apple disk image into a cache directory and locate the
 * .app bundle inside it. See dmgmount.h for the contract.
 */
#define _GNU_SOURCE
#include "dmgmount.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_APPS 16
#define MAX_NESTED 8

static char *xstrdup(const char *s)
{
    char *p = strdup(s ? s : "");
    if (!p) {
        fprintf(stderr, "pulsar-run: out of memory\n");
        exit(127);
    }
    return p;
}

static char *fmt(const char *f, ...)
{
    va_list ap;
    char *out = NULL;
    va_start(ap, f);
    if (vasprintf(&out, f, ap) < 0) {
        va_end(ap);
        fprintf(stderr, "pulsar-run: out of memory\n");
        exit(127);
    }
    va_end(ap);
    return out;
}

static void set_err(char **err, const char *f, ...)
{
    va_list ap;
    if (!err)
        return;
    free(*err);
    *err = NULL;
    va_start(ap, f);
    if (vasprintf(err, f, ap) < 0)
        *err = NULL;
    va_end(ap);
}

static bool has_suffix_ci(const char *s, const char *suf)
{
    size_t n = strlen(s), m = strlen(suf);
    return n > m && !strcasecmp(s + n - m, suf);
}

static bool is_dir(const char *p)
{
    struct stat st;
    return p && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_file(const char *p)
{
    struct stat st;
    return p && stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static char *path_join(const char *a, const char *b)
{
    if (!a || !*a)
        return xstrdup(b);
    if (!b || !*b)
        return xstrdup(a);
    return fmt("%s/%s", a, b);
}

static char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return xstrdup(s ? s + 1 : path);
}

static char *strip_suffix_ci(char *s, const char *suf)
{
    size_t n = strlen(s), m = strlen(suf);
    if (n > m && !strcasecmp(s + n - m, suf))
        s[n - m] = 0;
    return s;
}

static char *sanitize(const char *s)
{
    char *o = xstrdup(s);
    for (char *p = o; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            *p = '_';
    }
    return o;
}

bool plsr_is_dmg(const char *path)
{
    return has_suffix_ci(path ? path : "", ".dmg") && is_file(path);
}

/* mkdir -p; returns false and sets errno on failure. */
static bool mkdir_p(const char *path, char **err)
{
    char *p = xstrdup(path);
    for (char *s = p + 1; *s; s++) {
        if (*s != '/')
            continue;
        *s = 0;
        if (mkdir(p, 0700) < 0 && errno != EEXIST) {
            set_err(err, "cannot create %s: %s", p, strerror(errno));
            free(p);
            return false;
        }
        *s = '/';
    }
    if (mkdir(p, 0700) < 0 && errno != EEXIST) {
        set_err(err, "cannot create %s: %s", p, strerror(errno));
        free(p);
        return false;
    }
    free(p);
    return true;
}

static void wipe(const char *path)
{
    char *argv[] = {(char *)"rm", (char *)"-rf", (char *)path, NULL};
    pid_t pid = fork();
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    if (pid > 0)
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
            ;
}

static int run_tool(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            if (fd > STDERR_FILENO)
                close(fd);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* First 7-Zip binary we can execute: $PULSAR_7Z, then 7z/7zz/7za/7zr on PATH. */
static char *find_7z(void)
{
    static const char *names[] = {"7z", "7zz", "7za", "7zr", NULL};
    const char *env = getenv("PULSAR_7Z");
    if (env && *env) {
        if (access(env, X_OK) == 0)
            return xstrdup(env);
        return NULL;
    }
    const char *path = getenv("PATH");
    if (path) {
        char *list = xstrdup(path);
        char *save = NULL;
        for (char *dir = strtok_r(list, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
            for (int i = 0; names[i]; i++) {
                char *cand = path_join(dir, names[i]);
                if (access(cand, X_OK) == 0) {
                    free(list);
                    return cand;
                }
                free(cand);
            }
        }
        free(list);
    }
    return NULL;
}

/* 7-Zip: 0 = ok, 1 = warning (e.g. an entry it could not represent). */
static bool extract(const char *tool, const char *image, const char *dest)
{
    char *out = fmt("-o%s", dest);
    char *argv[] = {(char *)tool, (char *)"x", (char *)image, out, (char *)"-y", NULL};
    int rc = run_tool(argv);
    free(out);
    return rc >= 0 && rc <= 1;
}

/*
 * One-pass extraction covers current 7-Zip (it descends into the APFS
 * container itself). Older builds stop at the container, so unpack any
 * .apfs, .hfs, and .img payloads left in dest, then drop the container file.
 */
static bool extract_nested(const char *tool, const char *dest, char **err)
{
    static const char *exts[] = {".apfs", ".hfs", ".img", NULL};
    char *images[MAX_NESTED];
    int n = 0;

    DIR *d = opendir(dest);
    if (!d) {
        set_err(err, "cannot open %s: %s", dest, strerror(errno));
        return false;
    }
    struct dirent *de;
    while ((de = readdir(d)) && n < MAX_NESTED) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        bool nested = false;
        for (int i = 0; exts[i] && !nested; i++)
            nested = has_suffix_ci(de->d_name, exts[i]);
        if (!nested)
            continue;
        char *p = path_join(dest, de->d_name);
        if (is_file(p))
            images[n++] = p;
        else
            free(p);
    }
    closedir(d);

    bool ok = true;
    for (int i = 0; i < n; i++) {
        if (!extract(tool, images[i], dest)) {
            set_err(err, "7z failed to unpack nested image %s", images[i]);
            ok = false;
        } else {
            unlink(images[i]);
        }
        free(images[i]);
    }
    return ok;
}

static int collect_apps(const char *dir, char **out, int n, int depth)
{
    DIR *d = opendir(dir);
    if (!d)
        return n;
    struct dirent *de;
    while ((de = readdir(d)) && n < MAX_APPS) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        char *p = path_join(dir, de->d_name);
        if (!is_dir(p)) {
            free(p);
            continue;
        }
        if (has_suffix_ci(de->d_name, ".app")) {
            out[n++] = p;
        } else if (depth > 0 && de->d_name[0] != '.') {
            n = collect_apps(p, out, n, depth - 1);
            free(p);
        } else {
            free(p);
        }
    }
    closedir(d);
    return n;
}

static bool looks_like_bundle(const char *app)
{
    char *plist = fmt("%s/Contents/Info.plist", app);
    char *macos = fmt("%s/Contents/MacOS", app);
    bool ok = is_file(plist) || is_dir(macos);
    free(plist);
    free(macos);
    return ok;
}

static char *pick_app(char **apps, int n, const char *dmg, char **err)
{
    int i;
    for (i = 0; i < n; i++) {
        if (looks_like_bundle(apps[i]))
            break;
    }
    if (i == n) {
        set_err(err, "no .app bundle with Contents/Info.plist or Contents/MacOS in the image");
        return NULL;
    }
    if (i == n - 1)
        return xstrdup(apps[i]);

    /* Several bundles: prefer the one named after the image. */
    char *stem = strip_suffix_ci(base_name(dmg), ".dmg");
    char *appname = strip_suffix_ci(base_name(apps[i]), ".app");
    bool match = strcasestr(stem, appname) != NULL;
    free(appname);
    free(stem);
    if (match)
        return xstrdup(apps[i]);

    char *list = xstrdup("");
    for (int k = i; k < n; k++) {
        if (!looks_like_bundle(apps[k]))
            continue;
        char *next = fmt("%s%s%s", list, *list ? ", " : "", apps[k]);
        free(list);
        list = next;
    }
    set_err(err, "several .app bundles in the image and none matches its name: %s", list);
    free(list);
    return NULL;
}

static char *cache_root(void)
{
    const char *e = getenv("PULSAR_DMG_CACHE");
    if (e && *e)
        return xstrdup(e);
    e = getenv("XDG_CACHE_HOME");
    if (e && *e)
        return fmt("%s/pulsar-dmg", e);
    e = getenv("HOME");
    if (e && *e)
        return fmt("%s/.cache/pulsar-dmg", e);
    return fmt("/tmp/pulsar-dmg-%u", (unsigned)getuid());
}

char *plsr_mount_dmg(const char *dmg, char **mount_dir, char **err)
{
    if (mount_dir)
        *mount_dir = NULL;
    if (err)
        *err = NULL;

    struct stat st;
    if (stat(dmg, &st) < 0) {
        set_err(err, "cannot stat %s: %s", dmg, strerror(errno));
        return NULL;
    }
    if (!S_ISREG(st.st_mode)) {
        set_err(err, "%s is not a regular file", dmg);
        return NULL;
    }

    char *tool = find_7z();
    if (!tool) {
        set_err(err, "no 7-Zip extractor found (install p7zip or set PULSAR_7Z)");
        return NULL;
    }

    char *root = cache_root();
    if (!mkdir_p(root, err)) {
        free(root);
        free(tool);
        return NULL;
    }

    char *stem = sanitize(strip_suffix_ci(base_name(dmg), ".dmg"));
    char *dest = fmt("%s/%s-%lld-%lld", root, stem,
                     (long long)st.st_size, (long long)st.st_mtime);
    char *marker = fmt("%s/.plsr-mounted", dest);

    if (access(marker, F_OK) != 0) {
        char *tmp = fmt("%s.partial.%ld", dest, (long)getpid());
        wipe(tmp);
        if (!mkdir_p(tmp, err)) {
            free(tmp);
            goto fail;
        }
        if (!extract(tool, dmg, tmp)) {
            set_err(err, "7z failed to extract %s", dmg);
            wipe(tmp);
            free(tmp);
            goto fail;
        }
        if (!extract_nested(tool, tmp, err)) {
            wipe(tmp);
            free(tmp);
            goto fail;
        }
        char *tmpmarker = fmt("%s/.plsr-mounted", tmp);
        FILE *f = fopen(tmpmarker, "w");
        if (f) {
            fprintf(f, "%s\n", dmg);
            fclose(f);
        }
        free(tmpmarker);

        if (access(marker, F_OK) == 0) {
            wipe(tmp); /* another process finished the same mount first */
        } else {
            if (access(dest, F_OK) == 0)
                wipe(dest); /* stale partial mount from an interrupted run */
            if (rename(tmp, dest) < 0) {
                set_err(err, "cannot finalize mount at %s: %s", dest, strerror(errno));
                wipe(tmp);
                free(tmp);
                goto fail;
            }
        }
        free(tmp);
    }

    char *apps[MAX_APPS];
    int n = collect_apps(dest, apps, 0, 1);
    char *app = pick_app(apps, n, dmg, err);
    for (int i = 0; i < n; i++)
        free(apps[i]);

    if (app && mount_dir)
        *mount_dir = xstrdup(dest);
    free(dest);
    free(marker);
    free(stem);
    free(root);
    free(tool);
    return app;

fail:
    free(dest);
    free(marker);
    free(stem);
    free(root);
    free(tool);
    return NULL;
}
