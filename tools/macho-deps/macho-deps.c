/*
 * macho-deps: walk a Mach-O executable's LC_LOAD_DYLIB graph against an
 * extracted pulsar-appexec runtime root.
 *
 * Resolves @executable_path, @loader_path and @rpath, maps absolute macOS
 * paths under the runtime root, detects cycles, and reports missing images.
 * No execution and no Wayland involvement.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MH_MAGIC_64 0xfeedfacfU
#define MH_HEADER64_SIZE 32U
#define FAT_MAGIC 0xcafebabeU
#define CPU_TYPE_X86_64 0x01000007U
#define CPU_TYPE_ARM64 0x0100000cU

#define LC_LOAD_DYLIB 0x0cU
#define LC_ID_DYLIB 0x0dU
#define LC_LOAD_WEAK_DYLIB 0x80000018U
#define LC_REEXPORT_DYLIB 0x8000001fU
#define LC_LAZY_LOAD_DYLIB 0x20U
#define LC_LOAD_UPWARD_DYLIB 0x80000023U
#define LC_RPATH 0x8000001cU

struct strvec {
    char **v;
    size_t n, cap;
};

static void oom(void)
{
    fprintf(stderr, "macho-deps: out of memory\n");
    exit(2);
}

static char *xstrdup(const char *s)
{
    char *p = strdup(s ? s : "");
    if (!p)
        oom();
    return p;
}

static char *fmt(const char *f, ...)
{
    va_list ap;
    char *p = NULL;
    va_start(ap, f);
    if (vasprintf(&p, f, ap) < 0)
        oom();
    va_end(ap);
    return p;
}

static void sv_push(struct strvec *sv, const char *s)
{
    if (sv->n == sv->cap) {
        sv->cap = sv->cap ? sv->cap * 2 : 8;
        sv->v = realloc(sv->v, sv->cap * sizeof(*sv->v));
        if (!sv->v)
            oom();
    }
    sv->v[sv->n++] = xstrdup(s);
}

static bool sv_has(const struct strvec *sv, const char *s)
{
    for (size_t i = 0; i < sv->n; i++)
        if (!strcmp(sv->v[i], s))
            return true;
    return false;
}

static char *path_join(const char *a, const char *b)
{
    if (!a || !*a)
        return xstrdup(b);
    if (!b || !*b)
        return xstrdup(a);
    size_t na = strlen(a);
    while (na > 1 && a[na - 1] == '/')
        na--;
    return fmt("%.*s/%s", (int)na, a, b);
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

static bool file_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

struct dep {
    char *name;
    bool weak;
};

struct image {
    char *path;
    char *install;
    struct strvec rpaths;
    struct dep *deps;
    size_t ndeps;
};

static void image_free(struct image *im)
{
    free(im->path);
    free(im->install);
    for (size_t i = 0; i < im->rpaths.n; i++)
        free(im->rpaths.v[i]);
    free(im->rpaths.v);
    for (size_t i = 0; i < im->ndeps; i++)
        free(im->deps[i].name);
    free(im->deps);
    memset(im, 0, sizeof(*im));
}

static void add_dep(struct image *im, const char *name, bool weak)
{
    im->deps = realloc(im->deps, (im->ndeps + 1) * sizeof(*im->deps));
    if (!im->deps)
        oom();
    im->deps[im->ndeps].name = xstrdup(name);
    im->deps[im->ndeps].weak = weak;
    im->ndeps++;
}

static const uint8_t *select_slice(const uint8_t *data, size_t size,
                                   size_t *slice_size, char **err)
{
    if (size < MH_HEADER64_SIZE) {
        *err = xstrdup("file too short");
        return NULL;
    }
    uint32_t magic_le = le32(data);
    if (magic_le == MH_MAGIC_64) {
        *slice_size = size;
        return data;
    }
    if (be32(data) != FAT_MAGIC) {
        *err = xstrdup("not a 64-bit Mach-O or fat file");
        return NULL;
    }
    uint32_t nfat = be32(data + 4);
    if (nfat == 0 || nfat > 64 || 8 + (size_t)nfat * 20 > size) {
        *err = xstrdup("malformed fat header");
        return NULL;
    }
    const uint8_t *best = NULL;
    size_t best_size = 0;
    for (uint32_t i = 0; i < nfat; i++) {
        const uint8_t *e = data + 8 + (size_t)i * 20;
        uint32_t cpu = be32(e);
        uint32_t off = be32(e + 8);
        uint32_t len = be32(e + 12);
        if ((uint64_t)off + len > size || len < MH_HEADER64_SIZE)
            continue;
        if (cpu == CPU_TYPE_X86_64) {
            best = data + off;
            best_size = len;
            break;
        }
        if (!best && cpu == CPU_TYPE_ARM64) {
            best = data + off;
            best_size = len;
        }
    }
    if (!best) {
        *err = xstrdup("fat file has no x86_64 or arm64 slice");
        return NULL;
    }
    *slice_size = best_size;
    return best;
}

static bool cmd_is_dylib(uint32_t cmd, bool *weak)
{
    switch (cmd) {
    case LC_LOAD_DYLIB:
        *weak = false;
        return true;
    case LC_LAZY_LOAD_DYLIB:
        *weak = false;
        return true;
    case LC_REEXPORT_DYLIB:
        *weak = false;
        return true;
    case LC_LOAD_UPWARD_DYLIB:
        *weak = false;
        return true;
    case LC_LOAD_WEAK_DYLIB:
        *weak = true;
        return true;
    default:
        return false;
    }
}

static int parse_image(const char *path, struct image *im, char **err)
{
    memset(im, 0, sizeof(*im));
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        *err = fmt("cannot open: %s", strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < (off_t)MH_HEADER64_SIZE) {
        close(fd);
        *err = xstrdup("cannot stat or file too short");
        return -1;
    }
    size_t size = (size_t)st.st_size;
    uint8_t *data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) {
        *err = fmt("mmap: %s", strerror(errno));
        return -1;
    }

    size_t slice_size = 0;
    const uint8_t *m = select_slice(data, size, &slice_size, err);
    if (!m) {
        munmap(data, size);
        return -1;
    }

    uint32_t ncmds = le32(m + 16);
    uint32_t sizeofcmds = le32(m + 20);
    if (MH_HEADER64_SIZE + (uint64_t)sizeofcmds > slice_size) {
        *err = xstrdup("load commands exceed slice");
        munmap(data, size);
        return -1;
    }

    im->path = xstrdup(path);
    size_t off = MH_HEADER64_SIZE;
    for (uint32_t i = 0; i < ncmds; i++) {
        if (off + 8 > slice_size) {
            *err = xstrdup("truncated load command");
            break;
        }
        uint32_t cmd = le32(m + off);
        uint32_t cmdsize = le32(m + off + 4);
        if (cmdsize < 8 || off + cmdsize > slice_size) {
            *err = xstrdup("invalid load command size");
            break;
        }
        bool weak = false;
        if (cmd_is_dylib(cmd, &weak)) {
            if (cmdsize < 24) {
                *err = xstrdup("short dylib command");
                break;
            }
            uint32_t no = le32(m + off + 8);
            if (no < 24 || no >= cmdsize) {
                *err = xstrdup("bad dylib name offset");
                break;
            }
            const char *name = (const char *)(m + off + no);
            size_t max = cmdsize - no;
            if (!memchr(name, 0, max)) {
                *err = xstrdup("unterminated dylib name");
                break;
            }
            add_dep(im, name, weak);
        } else if (cmd == LC_ID_DYLIB) {
            uint32_t no = le32(m + off + 8);
            if (no >= 8 && no < cmdsize) {
                const char *name = (const char *)(m + off + no);
                if (memchr(name, 0, cmdsize - no))
                    im->install = xstrdup(name);
            }
        } else if (cmd == LC_RPATH) {
            uint32_t no = le32(m + off + 8);
            if (no >= 8 && no < cmdsize) {
                const char *p = (const char *)(m + off + no);
                if (memchr(p, 0, cmdsize - no))
                    sv_push(&im->rpaths, p);
            }
        }
        off += cmdsize;
    }

    munmap(data, size);
    if (*err) {
        image_free(im);
        return -1;
    }
    return 0;
}

static char *expand_token(const char *s, const char *loader_dir,
                          const char *exe_dir)
{
    if (!strncmp(s, "@loader_path", 12)) {
        if (s[12] == 0)
            return xstrdup(loader_dir);
        if (s[12] == '/')
            return path_join(loader_dir, s + 13);
    }
    if (!strncmp(s, "@executable_path", 16)) {
        if (s[16] == 0)
            return xstrdup(exe_dir);
        if (s[16] == '/')
            return path_join(exe_dir, s + 17);
    }
    return xstrdup(s);
}

static char *canonical_or_null(const char *p)
{
    char *r = realpath(p, NULL);
    return r;
}

static char *resolve_dep(const char *root, const struct image *im,
                         const char *exe_dir, const char *name)
{
    char *loader_dir = dir_name(im->path);
    char *out = NULL;

    if (!strncmp(name, "@rpath/", 7)) {
        const char *suffix = name + 7;
        for (size_t i = 0; i < im->rpaths.n && !out; i++) {
            char *base = expand_token(im->rpaths.v[i], loader_dir, exe_dir);
            char *cand = path_join(base, suffix);
            if (file_exists(cand))
                out = cand;
            else
                free(cand);
            free(base);
        }
    } else if (!strncmp(name, "@loader_path", 12) ||
               !strncmp(name, "@executable_path", 16)) {
        char *cand = expand_token(name, loader_dir, exe_dir);
        if (file_exists(cand))
            out = cand;
        else
            free(cand);
    } else if (name[0] == '/') {
        char *cand = path_join(root, name + 1);
        if (file_exists(cand))
            out = cand;
        else
            free(cand);
    } else {
        char *cand = path_join(loader_dir, name);
        if (file_exists(cand))
            out = cand;
        else
            free(cand);
    }

    free(loader_dir);
    return out;
}

struct walk {
    struct strvec seen;
    struct strvec images_list;
    struct strvec missing_list;
    size_t images;
    size_t missing;
    bool verbose;
    bool json;
    const char *root;
    const char *exe_dir;
};

static void json_string(const char *s)
{
    putchar('"');
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            printf("\\%c", c);
        else if (c < 0x20)
            printf("\\u%04x", c);
        else
            putchar(c);
    }
    putchar('"');
}

static void walk_image(struct walk *w, const char *path, int depth, const char *parent)
{
    char *canon = canonical_or_null(path);
    const char *key = canon ? canon : path;
    if (sv_has(&w->seen, key)) {
        if (w->verbose && !w->json)
            printf("%*s(seen) %s\n", depth * 2, "", path);
        free(canon);
        return;
    }
    sv_push(&w->seen, key);
    free(canon);
    w->images++;
    sv_push(&w->images_list, path);

    struct image im;
    char *err = NULL;
    if (parse_image(path, &im, &err) < 0) {
        w->missing++;
        char *line = fmt("%s <- %s: %s", parent ? parent : "", path,
                         err ? err : "parse failed");
        sv_push(&w->missing_list, line);
        free(line);
        if (!w->json)
            printf("%*s! %s (%s)\n", depth * 2, "", path,
                   err ? err : "parse failed");
        free(err);
        return;
    }
    if (w->verbose && !w->json) {
        printf("%*s%s\n", depth * 2, "", path);
        if (im.install)
            printf("%*sinstall: %s\n", (depth + 1) * 2, "", im.install);
    }

    for (size_t i = 0; i < im.ndeps; i++) {
        char *res = resolve_dep(w->root, &im, w->exe_dir, im.deps[i].name);
        if (!res) {
            if (!im.deps[i].weak) {
                w->missing++;
                char *line = fmt("%s <- %s: not found", path, im.deps[i].name);
                sv_push(&w->missing_list, line);
                free(line);
                if (!w->json)
                    printf("%*s! missing %s\n", (depth + 1) * 2, "",
                           im.deps[i].name);
            } else if (w->verbose && !w->json) {
                printf("%*s- weak missing %s\n", (depth + 1) * 2, "",
                       im.deps[i].name);
            }
            continue;
        }
        if (depth < 64)
            walk_image(w, res, depth + 1, path);
        free(res);
    }
    image_free(&im);
}

static const char *find_default_root(void)
{
    const char *env = getenv("PULSAR_ROOT");
    if (env && *env)
        return env;
    static char buf[PATH_MAX];
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n < 0)
        return NULL;
    self[n] = 0;
    char *d = dir_name(self);
    /* <workspace>/src/deps/macho-deps -> <workspace> */
    char *ws = dir_name(dir_name(d));
    free(d);
    snprintf(buf, sizeof(buf), "%s/runtime/lib/pulsarappexec", ws);
    free(ws);
    return buf;
}

static void usage(FILE *out, const char *prog)
{
    fprintf(out,
            "usage: %s [options] <macho> [--] [args...]\n"
            "  --root PATH       runtime root (default WORKSPACE/runtime/lib/pulsarappexec)\n"
            "  --verbose, -v     print the full dependency tree\n"
            "  --json            machine-readable summary and missing list\n"
            "  --allow-missing   exit 0 even when dependencies are missing\n"
            "  -h, --help        show this help\n",
            prog);
}

int main(int argc, char **argv)
{
    const char *root = NULL;
    const char *target = NULL;
    bool allow_missing = false;
    bool verbose = false;
    bool json = false;
    const char *prog = argv[0];

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--root") && i + 1 < argc)
            root = argv[++i];
        else if (!strcmp(a, "--verbose") || !strcmp(a, "-v"))
            verbose = true;
        else if (!strcmp(a, "--json"))
            json = true;
        else if (!strcmp(a, "--allow-missing"))
            allow_missing = true;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(stdout, prog);
            return 0;
        } else if (a[0] == '-') {
            fprintf(stderr, "%s: unknown option %s\n", prog, a);
            usage(stderr, prog);
            return 2;
        } else if (!target) {
            target = a;
        } else {
            fprintf(stderr, "%s: unexpected argument %s\n", prog, a);
            return 2;
        }
    }
    if (!target) {
        usage(stderr, prog);
        return 2;
    }
    if (!root)
        root = find_default_root();
    if (!root) {
        fprintf(stderr, "%s: cannot determine runtime root; use --root\n", prog);
        return 2;
    }
    char *root_real = realpath(root, NULL);
    if (!root_real) {
        fprintf(stderr, "%s: runtime root not found: %s\n", prog, root);
        return 2;
    }
    char *target_real = realpath(target, NULL);
    if (!target_real || !file_exists(target_real)) {
        fprintf(stderr, "%s: target not found: %s\n", prog, target);
        return 2;
    }

    char *exe_dir = dir_name(target_real);
    struct walk w;
    memset(&w, 0, sizeof(w));
    w.verbose = verbose;
    w.json = json;
    w.root = root_real;
    w.exe_dir = exe_dir;

    walk_image(&w, target_real, 0, NULL);

    if (json) {
        printf("{\"root\":");
        json_string(root_real);
        printf(",\"app\":");
        json_string(target_real);
        printf(",\"image_count\":%zu,\"missing_count\":%zu,\"images\":[",
               w.images, w.missing);
        for (size_t i = 0; i < w.images_list.n; i++) {
            if (i)
                putchar(',');
            json_string(w.images_list.v[i]);
        }
        printf("],\"missing\":[");
        for (size_t i = 0; i < w.missing_list.n; i++) {
            if (i)
                putchar(',');
            json_string(w.missing_list.v[i]);
        }
        printf("]}\n");
    } else {
        fprintf(stderr, "\nimages: %zu, missing: %zu, root: %s\n",
                w.images, w.missing, root_real);
    }

    int rc = (w.missing && !allow_missing) ? 1 : 0;
    free(exe_dir);
    free(target_real);
    free(root_real);
    for (size_t i = 0; i < w.seen.n; i++)
        free(w.seen.v[i]);
    free(w.seen.v);
    for (size_t i = 0; i < w.images_list.n; i++)
        free(w.images_list.v[i]);
    free(w.images_list.v);
    for (size_t i = 0; i < w.missing_list.n; i++)
        free(w.missing_list.v[i]);
    free(w.missing_list.v);
    return rc;
}
