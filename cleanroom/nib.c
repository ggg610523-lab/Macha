/*
 * Clean-room nib / storyboard loading.
 *
 * Compiled nibs are NIBArchive, ibtool's output: a class table, a key table, a
 * value table and an object table of (class, first value, value count).  The
 * reference are shipped storyboards: MainMenu.nib alone is 2,206
 * objects (35 menus, 258 items, 170 control connectors, 11 outlets), and the
 * scene nibs pair an NSViewController with a view nib.
 *
 * Objects are built through the ObjC classes registered by Foundation and the
 * AppKit slice, so an app's own classes (Swift or ObjC) are instantiated when
 * they exist; a class swapper falls back to its original class, which is how a
 * menu item still builds when its Swift subclass is not loaded.  Connectors are
 * applied after the graph exists: outlets become setters, control connectors
 * become target/action, and the nib's root object becomes the owner passed in.
 *
 * Archives are kept for the life of the process: nib object graphs are
 * long-lived, and outlets commonly point into them.
 */
#define _GNU_SOURCE
#include "objc_abi.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void plsr_objc_register_class(plsr_class *c);
void *plsr_objc_lookup_imp(void *obj, const char *sel);
void *objc_getClass(const char *name) __asm__("_objc_getClass");
const char *class_getName(void *cls) __asm__("_class_getName");
void *class_getSuperclass(void *cls) __asm__("_class_getSuperclass");

/* ---- message helpers --------------------------------------------------- */

typedef struct { double x, y, w, h; } rect;

typedef struct {
    uint32_t entsize_flags;
    uint32_t count;
    plsr_method methods[40];
} method_array;

/* A storyboard segue template as archived in a scene nib. */
typedef struct {
    void *isa;
    void *identifier;
    void *source;
    void *destination;
    long style;
    long options;
} segue_template_obj;

/* NSStoryboardSegue: the instantiated segue handed to prepareForSegue:. */
typedef struct {
    void *isa;
    void *identifier;
    void *source;
    void *destination;
    void *handler;
} segue_obj;

static void vc_add_segue(void *vc, void *tmpl);

static void *send0(void *obj, const char *sel)
{
    void *(*fn)(void *, const char *);
    if (!obj)
        return NULL;
    fn = (void *(*)(void *, const char *))plsr_objc_lookup_imp(obj, sel);
    return fn ? fn(obj, sel) : NULL;
}

static void *send1(void *obj, const char *sel, void *a)
{
    void *(*fn)(void *, const char *, void *);
    if (!obj)
        return NULL;
    fn = (void *(*)(void *, const char *, void *))plsr_objc_lookup_imp(obj, sel);
    return fn ? fn(obj, sel, a) : NULL;
}

static void *send2(void *obj, const char *sel, void *a, void *b)
{
    void *(*fn)(void *, const char *, void *, void *);
    if (!obj)
        return NULL;
    fn = (void *(*)(void *, const char *, void *, void *))plsr_objc_lookup_imp(obj, sel);
    return fn ? fn(obj, sel, a, b) : NULL;
}

static void *send3(void *obj, const char *sel, void *a, void *b, void *c)
{
    void *(*fn)(void *, const char *, void *, void *, void *);
    if (!obj)
        return NULL;
    fn = (void *(*)(void *, const char *, void *, void *, void *))
        plsr_objc_lookup_imp(obj, sel);
    return fn ? fn(obj, sel, a, b, c) : NULL;
}


static void *send_rect(void *obj, const char *sel, rect r)
{
    void *(*fn)(void *, const char *, rect);
    if (!obj)
        return NULL;
    fn = (void *(*)(void *, const char *, rect))plsr_objc_lookup_imp(obj, sel);
    return fn ? fn(obj, sel, r) : NULL;
}

static int responds(void *obj, const char *sel)
{
    return obj && plsr_objc_lookup_imp(obj, sel) != NULL;
}

static const char *cstr(void *nsstring)
{
    const char *(*fn)(void *, const char *);
    if (!nsstring)
        return NULL;
    fn = (const char *(*)(void *, const char *))plsr_objc_lookup_imp(
        nsstring, "UTF8String");
    return fn ? fn(nsstring, "UTF8String") : NULL;
}

static void *mkstr(const char *s)
{
    void *cls = objc_getClass("NSString");
    void *(*fn)(void *, const char *, const char *);
    if (!cls || !s)
        return NULL;
    fn = (void *(*)(void *, const char *, const char *))plsr_objc_lookup_imp(
        cls, "stringWithUTF8String:");
    return fn ? fn(cls, "stringWithUTF8String:", s) : NULL;
}

static void *mkstrn(const uint8_t *p, size_t n)
{
    char *buf = malloc(n + 1);
    void *s;
    if (!buf)
        return NULL;
    memcpy(buf, p, n);
    buf[n] = 0;
    s = mkstr(buf);
    free(buf);
    return s;
}

static void *mkint(long long v)
{
    void *cls = objc_getClass("NSNumber");
    void *(*fn)(void *, const char *, long long);
    if (!cls)
        return NULL;
    fn = (void *(*)(void *, const char *, long long))plsr_objc_lookup_imp(
        cls, "numberWithInt:");
    return fn ? fn(cls, "numberWithInt:", v) : NULL;
}

static void *mkdouble(double v)
{
    void *cls = objc_getClass("NSNumber");
    void *(*fn)(void *, const char *, double);
    if (!cls)
        return NULL;
    fn = (void *(*)(void *, const char *, double))plsr_objc_lookup_imp(
        cls, "numberWithDouble:");
    return fn ? fn(cls, "numberWithDouble:", v) : NULL;
}

static void *mkbool(int v)
{
    void *cls = objc_getClass("NSNumber");
    void *(*fn)(void *, const char *, signed char);
    if (!cls)
        return NULL;
    fn = (void *(*)(void *, const char *, signed char))plsr_objc_lookup_imp(
        cls, "numberWithBool:");
    return fn ? fn(cls, "numberWithBool:", (signed char)(v != 0)) : NULL;
}

static void *mkdata(const uint8_t *p, size_t n)
{
    void *cls = objc_getClass("NSData");
    void *(*fn)(void *, const char *, const void *, unsigned long);
    if (!cls)
        return NULL;
    fn = (void *(*)(void *, const char *, const void *, unsigned long))
        plsr_objc_lookup_imp(cls, "dataWithBytes:length:");
    return fn ? fn(cls, "dataWithBytes:length:", p, (unsigned long)n) : NULL;
}

static void *mkarray(void)
{
    void *cls = objc_getClass("NSMutableArray");
    void *(*fn)(void *, const char *, unsigned long);
    if (!cls)
        return NULL;
    fn = (void *(*)(void *, const char *, unsigned long))plsr_objc_lookup_imp(
        cls, "arrayWithCapacity:");
    return fn ? fn(cls, "arrayWithCapacity:", 0) : NULL;
}

static void array_add(void *array, void *obj)
{
    send1(array, "addObject:", obj);
}

static unsigned long array_count(void *array)
{
    return array ? (unsigned long)(intptr_t)send0(array, "count") : 0;
}

static void *array_at(void *array, unsigned long i)
{
    return array ? send1(array, "objectAtIndex:", (void *)(intptr_t)i) : NULL;
}

static void *mkdict(void)
{
    void *cls = objc_getClass("NSMutableDictionary");
    void *(*fn)(void *, const char *, unsigned long);
    if (!cls)
        return NULL;
    fn = (void *(*)(void *, const char *, unsigned long))plsr_objc_lookup_imp(
        cls, "dictionaryWithCapacity:");
    return fn ? fn(cls, "dictionaryWithCapacity:", 0) : NULL;
}

/* ---- class lookup ------------------------------------------------------ */

/* Swift registers "Module.Class"; a nib stores the mangled _TtC name. */
static void *lookup_class(const char *name)
{
    void *cls;
    if (!name || !*name)
        return NULL;
    cls = objc_getClass(name);
    if (cls)
        return cls;
    if (!strncmp(name, "_TtC", 4)) {
        char buf[512];
        const char *p = name + 4;
        size_t o = 0;
        int part = 0;
        while (*p && o + 2 < sizeof buf) {
            char *end;
            long n = strtol(p, &end, 10);
            if (end == p || n <= 0)
                break;
            p = end;
            if (part++ && o + 1 < sizeof buf)
                buf[o++] = '.';
            if ((size_t)n >= sizeof buf - o)
                break;
            memcpy(buf + o, p, (size_t)n);
            o += (size_t)n;
            p += n;
        }
        buf[o] = 0;
        if (o)
            cls = objc_getClass(buf);
    }
    return cls;
}

static void *alloc_class(void *cls)
{
    void *(*fn)(void *, const char *);
    if (!cls)
        return NULL;
    fn = (void *(*)(void *, const char *))plsr_objc_lookup_imp(cls, "alloc");
    return fn ? fn(cls, "alloc") : NULL;
}

static int is_kind_of(void *obj, const char *clsname)
{
    void *cls = objc_getClass(clsname);
    void *c = obj ? *(void **)obj : NULL;
    for (int depth = 0; c && depth < 64; depth++) {
        if (c == cls)
            return 1;
        c = class_getSuperclass(c);
    }
    return 0;
}

/* ---- archive ----------------------------------------------------------- */

enum { NV_NULL, NV_INT, NV_DBL, NV_BOOL, NV_DATA, NV_REF };

typedef struct {
    int type;
    long long i;
    double d;
    const uint8_t *p;
    size_t n;
    uint32_t ref;
} nibval;

typedef struct {
    uint8_t *buf;
    size_t len;
    uint32_t nobj, oobj, nkey, okey, nval, oval, ncls, ocls;
    char **keys;
    char **classes;
    uint32_t *val_key;
    nibval *vals;
    uint32_t *obj_class, *obj_val, *obj_count;
    void **objs;
    unsigned char *state;   /* 0 = pending, 1 = building, 2 = built */
    void *owner;
    uint32_t root;
} nib_archive;

static int nib_vint(const uint8_t *b, size_t len, size_t *pos, uint64_t *out)
{
    uint64_t r = 0;
    int shift = 0;
    for (;;) {
        uint8_t c;
        if (*pos >= len || shift > 63)
            return 0;
        c = b[(*pos)++];
        r |= (uint64_t)(c & 0x7f) << shift;
        shift += 7;
        if (c & 0x80) {
            *out = r;
            return 1;
        }
    }
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = path ? fopen(path, "rb") : NULL;
    uint8_t *buf;
    long n;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)n);
    if (!buf || (n && fread(buf, 1, (size_t)n, f) != (size_t)n)) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static void nib_archive_free(nib_archive *a)
{
    if (!a)
        return;
    if (a->keys)
        for (uint32_t i = 0; i < a->nkey; i++)
            free(a->keys[i]);
    if (a->classes)
        for (uint32_t i = 0; i < a->ncls; i++)
            free(a->classes[i]);
    free(a->keys);
    free(a->classes);
    free(a->val_key);
    free(a->vals);
    free(a->obj_class);
    free(a->obj_val);
    free(a->obj_count);
    free(a->objs);
    free(a->state);
    free(a->buf);
    free(a);
}

static nib_archive *nib_archive_parse(const uint8_t *data, size_t len)
{
    nib_archive *a;
    uint32_t h[10];
    size_t pos;
    if (!data || len < 50 || memcmp(data, "NIBArchive", 10) != 0)
        return NULL;
    a = calloc(1, sizeof *a);
    if (!a)
        return NULL;
    a->buf = malloc(len);
    if (!a->buf) {
        free(a);
        return NULL;
    }
    memcpy(a->buf, data, len);
    a->len = len;
    memcpy(h, data + 10, sizeof h);
    a->nobj = h[2];
    a->oobj = h[3];
    a->nkey = h[4];
    a->okey = h[5];
    a->nval = h[6];
    a->oval = h[7];
    a->ncls = h[8];
    a->ocls = h[9];
    if (a->nobj > (1u << 22) || a->nkey > (1u << 20) || a->nval > (1u << 23) ||
        a->ncls > (1u << 16) || a->oobj >= len || a->okey >= len ||
        a->oval >= len || a->ocls > len)
        goto bad;

    a->keys = calloc(a->nkey ? a->nkey : 1, sizeof *a->keys);
    a->classes = calloc(a->ncls ? a->ncls : 1, sizeof *a->classes);
    a->val_key = calloc(a->nval ? a->nval : 1, sizeof *a->val_key);
    a->vals = calloc(a->nval ? a->nval : 1, sizeof *a->vals);
    a->obj_class = calloc(a->nobj ? a->nobj : 1, sizeof *a->obj_class);
    a->obj_val = calloc(a->nobj ? a->nobj : 1, sizeof *a->obj_val);
    a->obj_count = calloc(a->nobj ? a->nobj : 1, sizeof *a->obj_count);
    a->objs = calloc(a->nobj ? a->nobj : 1, sizeof *a->objs);
    a->state = calloc(a->nobj ? a->nobj : 1, 1);
    if (!a->keys || !a->classes || !a->val_key || !a->vals || !a->obj_class ||
        !a->obj_val || !a->obj_count || !a->objs || !a->state)
        goto bad;

    pos = a->okey;
    for (uint32_t i = 0; i < a->nkey; i++) {
        uint64_t n;
        if (!nib_vint(a->buf, len, &pos, &n) || pos + n > len)
            goto bad;
        a->keys[i] = strndup((const char *)a->buf + pos, (size_t)n);
        pos += (size_t)n;
    }

    pos = a->ocls;
    for (uint32_t i = 0; i < a->ncls; i++) {
        uint64_t n, extra;
        if (!nib_vint(a->buf, len, &pos, &n) ||
            !nib_vint(a->buf, len, &pos, &extra))
            goto bad;
        if (pos + extra * 4 > len || pos + extra * 4 + n > len)
            goto bad;
        pos += (size_t)extra * 4;
        a->classes[i] = strndup((const char *)a->buf + pos, (size_t)n);
        pos += (size_t)n;
        if (a->classes[i] && n && a->classes[i][n - 1] == 0)
            a->classes[i][n - 1] = 0;
    }

    pos = a->oval;
    for (uint32_t i = 0; i < a->nval; i++) {
        uint64_t k;
        uint8_t t;
        nibval *v = &a->vals[i];
        if (!nib_vint(a->buf, len, &pos, &k) || k >= a->nkey || pos >= len)
            goto bad;
        a->val_key[i] = (uint32_t)k;
        t = a->buf[pos++];
        switch (t) {
        case 0:
            if (pos + 1 > len)
                goto bad;
            v->type = NV_INT;
            v->i = (int8_t)a->buf[pos];
            pos += 1;
            break;
        case 1: {
            int16_t x;
            if (pos + 2 > len)
                goto bad;
            memcpy(&x, a->buf + pos, 2);
            v->type = NV_INT;
            v->i = x;
            pos += 2;
            break;
        }
        case 2: {
            int32_t x;
            if (pos + 4 > len)
                goto bad;
            memcpy(&x, a->buf + pos, 4);
            v->type = NV_INT;
            v->i = x;
            pos += 4;
            break;
        }
        case 3: {
            int64_t x;
            if (pos + 8 > len)
                goto bad;
            memcpy(&x, a->buf + pos, 8);
            v->type = NV_INT;
            v->i = x;
            pos += 8;
            break;
        }
        case 4:
            v->type = NV_BOOL;
            v->i = 1;
            break;
        case 5:
            v->type = NV_BOOL;
            v->i = 0;
            break;
        case 6: {
            float x;
            if (pos + 4 > len)
                goto bad;
            memcpy(&x, a->buf + pos, 4);
            v->type = NV_DBL;
            v->d = x;
            pos += 4;
            break;
        }
        case 7: {
            double x;
            if (pos + 8 > len)
                goto bad;
            memcpy(&x, a->buf + pos, 8);
            v->type = NV_DBL;
            v->d = x;
            pos += 8;
            break;
        }
        case 8: {
            uint64_t n;
            if (!nib_vint(a->buf, len, &pos, &n) || pos + n > len)
                goto bad;
            v->type = NV_DATA;
            v->p = a->buf + pos;
            v->n = (size_t)n;
            pos += (size_t)n;
            break;
        }
        case 9:
            v->type = NV_NULL;
            break;
        case 10: {
            uint32_t r;
            if (pos + 4 > len)
                goto bad;
            memcpy(&r, a->buf + pos, 4);
            v->type = NV_REF;
            v->ref = r;
            pos += 4;
            break;
        }
        default:
            goto bad;
        }
    }

    pos = a->oobj;
    for (uint32_t i = 0; i < a->nobj; i++) {
        uint64_t c, vi, vc;
        if (!nib_vint(a->buf, len, &pos, &c) ||
            !nib_vint(a->buf, len, &pos, &vi) ||
            !nib_vint(a->buf, len, &pos, &vc))
            goto bad;
        if (c >= a->ncls || vi + vc > a->nval)
            goto bad;
        a->obj_class[i] = (uint32_t)c;
        a->obj_val[i] = (uint32_t)vi;
        a->obj_count[i] = (uint32_t)vc;
    }
    a->root = 0;
    return a;
bad:
    nib_archive_free(a);
    return NULL;
}

static const nibval *nib_raw(nib_archive *a, uint32_t obj, const char *key)
{
    if (!a || obj >= a->nobj)
        return NULL;
    for (uint32_t i = 0; i < a->obj_count[obj]; i++) {
        uint32_t vi = a->obj_val[obj] + i;
        if (!strcmp(a->keys[a->val_key[vi]], key))
            return &a->vals[vi];
    }
    return NULL;
}

static void *nib_ref(nib_archive *a, uint32_t i);

static void *nib_value(nib_archive *a, const nibval *v)
{
    if (!v)
        return NULL;
    switch (v->type) {
    case NV_REF:
        return nib_ref(a, v->ref);
    case NV_DATA:
        return mkdata(v->p, v->n);
    case NV_INT:
        return mkint(v->i);
    case NV_DBL:
        return mkdouble(v->d);
    case NV_BOOL:
        return mkbool((int)v->i);
    default:
        return NULL;
    }
}

static void *nib_get(nib_archive *a, uint32_t obj, const char *key)
{
    return nib_value(a, nib_raw(a, obj, key));
}

static const char *nib_str(nib_archive *a, uint32_t obj, const char *key)
{
    const nibval *v = nib_raw(a, obj, key);
    if (!v)
        return NULL;
    if (v->type == NV_REF) {
        void *s = nib_ref(a, v->ref);
        return s ? cstr(s) : NULL;
    }
    if (v->type == NV_DATA) {
        char *buf = malloc(v->n + 1);
        if (!buf)
            return NULL;
        memcpy(buf, v->p, v->n);
        buf[v->n] = 0;
        return buf;   /* archives live for the process; small and bounded */
    }
    return NULL;
}

static long long nib_int(nib_archive *a, uint32_t obj, const char *key, long long dflt)
{
    const nibval *v = nib_raw(a, obj, key);
    if (!v)
        return dflt;
    if (v->type == NV_INT)
        return v->i;
    if (v->type == NV_BOOL)
        return v->i ? 1 : 0;
    if (v->type == NV_DBL)
        return (long long)v->d;
    if (v->type == NV_REF) {
        void *o = nib_ref(a, v->ref);
        if (o && responds(o, "intValue"))
            return (long long)(intptr_t)send0(o, "intValue");
    }
    return dflt;
}

static rect nib_rect(nib_archive *a, uint32_t obj)
{
    rect r = { 0, 0, 0, 0 };
    const char *s = nib_str(a, obj, "NSFrame");
    if (!s)
        s = nib_str(a, obj, "NSWindowRect");
    if (s)
        sscanf(s, "{{%lf, %lf}, {%lf, %lf}}", &r.x, &r.y, &r.w, &r.h);
    if (r.w == 0 && r.h == 0) {
        const char *sz = nib_str(a, obj, "NSFrameSize");
        if (sz)
            sscanf(sz, "{%lf, %lf}", &r.w, &r.h);
    }
    return r;
}

/* Views keep the autoresizing mask ibtool stored next to their frame; without
 * it a decoded tree has mask 0 and never re-lays out when its window resizes. */
static unsigned long nib_autoresizing(nib_archive *a, uint32_t obj)
{
    long long m = nib_int(a, obj, "NSAutoresizingMask", 0);
    if (!m)
        m = nib_int(a, obj, "NSViewAutoresizingMask", 0);
    if (!m)
        m = nib_int(a, obj, "NSFrameAutoresizingMask", 0);
    if (!m) {
        /* ibtool packs the six AppKit mask bits into the low bits of NSvFlags
         * (0x100 is its own "mask present" flag); e.g. 274 -> 18. */
        long long flags = nib_int(a, obj, "NSvFlags", 0);
        if (flags)
            m = flags & 0x3f;
    }
    return (unsigned long)m;
}

/* The class a nib object should instantiate: the swapper's class when the app
 * provides it, otherwise its original class. */
static void *nib_object_class(nib_archive *a, uint32_t i, const char **name)
{
    const char *nibcn = a->classes[a->obj_class[i]];
    const char *cn = nib_str(a, i, "NSClassName");
    const char *orig = nib_str(a, i, "NSOriginalClassName");
    void *cls = lookup_class(cn);
    if (!cls)
        cls = lookup_class(orig);
    if (!cls)
        cls = lookup_class(nibcn);
    /* A custom view whose own class is unavailable is still a view. */
    if (!cls && nibcn && !strcmp(nibcn, "NSCustomView"))
        cls = objc_getClass("NSView");
    if (name)
        *name = cn ? cn : (orig ? orig : a->classes[a->obj_class[i]]);
    return cls;
}

static void apply_outlet(void *src, const char *label, void *dst);
static void apply_action(void *src, const char *label, void *dst);

static void *nib_build(nib_archive *a, uint32_t i)
{
    const char *cn = a->classes[a->obj_class[i]];
    void *o = NULL;

    if (getenv("PLSR_NIB_KEYS")) {
        fprintf(stderr, "nibobj %u %s\n", i, cn);
        for (uint32_t k = 0; k < a->obj_count[i]; k++) {
            uint32_t vi = a->obj_val[i] + k;
            const nibval *v = &a->vals[vi];
            fprintf(stderr, "  %s t=%d i=%lld d=%g\n",
                    a->keys[a->val_key[vi]], (int)v->type, (long long)v->i, v->d);
        }
    }

    if (!strcmp(cn, "NSString") || !strcmp(cn, "NSMutableString") ||
        !strcmp(cn, "NSLocalizableString")) {
        const nibval *v = nib_raw(a, i, "NS.bytes");
        if (v && v->type == NV_DATA)
            return mkstrn(v->p, v->n);
        return mkstr("");
    }
    if (!strcmp(cn, "NSNumber")) {
        const nibval *v = nib_raw(a, i, "NS.intval");
        if (v && v->type == NV_INT)
            return mkint(v->i);
        v = nib_raw(a, i, "NS.boolval");
        if (v)
            return mkbool(v->type == NV_BOOL ? (int)v->i : (int)nib_int(a, i, "NS.boolval", 0));
        v = nib_raw(a, i, "NS.dblval");
        if (v && v->type == NV_DBL)
            return mkdouble(v->d);
        return mkint(0);
    }
    if (!strcmp(cn, "NSArray") || !strcmp(cn, "NSMutableArray")) {
        void *arr = mkarray();
        a->objs[i] = arr;
        a->state[i] = 1;
        for (uint32_t k = 0; k < a->obj_count[i]; k++) {
            const nibval *v = &a->vals[a->obj_val[i] + k];
            const char *key = a->keys[a->val_key[a->obj_val[i] + k]];
            if (strcmp(key, "UINibEncoderEmptyKey") && strcmp(key, "NS.objects"))
                continue;
            if (v->type == NV_REF)
                array_add(arr, nib_ref(a, v->ref));
        }
        return arr;
    }
    if (!strcmp(cn, "NSSet") || !strcmp(cn, "NSMutableSet")) {
        void *arr = mkarray();
        for (uint32_t k = 0; k < a->obj_count[i]; k++) {
            const nibval *v = &a->vals[a->obj_val[i] + k];
            if (v->type == NV_REF)
                array_add(arr, nib_ref(a, v->ref));
        }
        return arr;
    }
    if (!strcmp(cn, "NSDictionary") || !strcmp(cn, "NSMutableDictionary")) {
        void *d = mkdict();
        void *keys = nib_get(a, i, "NS.keys");
        void *vals = nib_get(a, i, "NS.objects");
        a->objs[i] = d;
        a->state[i] = 1;
        for (unsigned long k = 0; k < array_count(keys); k++) {
            void *key = array_at(keys, k);
            void *val = array_at(vals, k);
            if (key && val)
                send2(d, "setObject:forKey:", val, key);
        }
        return d;
    }
    if (!strcmp(cn, "NSData")) {
        const nibval *v = nib_raw(a, i, "NS.bytes");
        return (v && v->type == NV_DATA) ? mkdata(v->p, v->n) : mkdata(NULL, 0);
    }
    if (!strcmp(cn, "NSValue")) {
        void *s = nib_get(a, i, "NS.sizeval");
        if (!s)
            s = nib_get(a, i, "NS.rectval");
        if (!s)
            s = nib_get(a, i, "NS.pointval");
        return s ? s : mkstr("");
    }
    if (!strcmp(cn, "NSAttributedString")) {
        void *s = nib_get(a, i, "NSString");
        return s;
    }
    if (!strcmp(cn, "NSNull"))
        return NULL;
    if (!strcmp(cn, "NSCustomResource") || !strcmp(cn, "NSImage") ||
        !strcmp(cn, "NSColor") || !strcmp(cn, "NSColorSpace") ||
        !strcmp(cn, "NSFont") || !strcmp(cn, "NSFontDescriptor"))
        return NULL;
    if (!strcmp(cn, "NSCustomObject")) {
        const char *name = nib_str(a, i, "NSClassName");
        if (name && !strcmp(name, "NSApplication"))
            return send0(objc_getClass("NSApplication"), "sharedApplication");
        void *cls = lookup_class(name);
        if (!cls)
            cls = objc_getClass("NSObject");
        o = alloc_class(cls);
        if (o && responds(o, "init"))
            o = send0(o, "init");
        if (o) {
            void *menu = nib_get(a, i, "IBMainMenu");
            if (menu && is_kind_of(menu, "NSMenu"))
                send1(send0(objc_getClass("NSApplication"), "sharedApplication"),
                      "setMainMenu:", menu);
        }
        return o;
    }
    if (!strcmp(cn, "NSClassSwapper")) {
        const char *name = NULL;
        void *cls = nib_object_class(a, i, &name);
        if (!cls)
            return NULL;
        o = alloc_class(cls);
        if (o && responds(o, "init"))
            o = send0(o, "init");
        a->objs[i] = o;
        a->state[i] = 1;
        {
            const char *nibname = nib_str(a, i, "NSNibName");
            if (nibname && responds(o, "setNibName:"))
                send1(o, "setNibName:", mkstr(nibname));
        }
        /* A swapped view/menu item carries the base class's properties. */
        if (is_kind_of(o, "NSMenuItem")) {
            const char *title = nib_str(a, i, "NSTitle");
            const char *key = nib_str(a, i, "NSKeyEquiv");
            const char *action = nib_str(a, i, "NSAction");
            send1(o, "setTitle:", mkstr(title ? title : ""));
            send1(o, "setKeyEquivalent:", mkstr(key ? key : ""));
            if (action)
                send1(o, "setAction:", (void *)action);
            {
                void *sub = nib_get(a, i, "NSSubmenu");
                if (sub)
                    send1(o, "setSubmenu:", sub);
            }
            if (nib_int(a, i, "NSIsDisabled", 0))
                send1(o, "setEnabled:", NULL);
            send1(o, "setKeyEquivalentModifierMask:",
                  (void *)(uintptr_t)nib_int(a, i, "NSKeyEquivModMask", 0));
            send1(o, "setState:", (void *)(intptr_t)nib_int(a, i, "NSState", 0));
            if (nib_int(a, i, "NSIsAlternate", 0))
                send1(o, "setAlternate:", (void *)1);
        } else if (is_kind_of(o, "NSMenu")) {
            const char *title = nib_str(a, i, "NSTitle");
            void *items;
            send1(o, "setTitle:", mkstr(title ? title : ""));
            items = nib_get(a, i, "NSMenuItems");
            for (unsigned long k = 0; k < array_count(items); k++)
                send1(o, "addItem:", array_at(items, k));
        }
        return o;
    }
    if (!strcmp(cn, "NSNibExternalObjectPlaceholder"))
        return a->owner;
    if (!strcmp(cn, "NSApplication"))
        return send0(objc_getClass("NSApplication"), "sharedApplication");
    if (!strcmp(cn, "NSMenu")) {
        const char *title = nib_str(a, i, "NSTitle");
        void *m = alloc_class(objc_getClass("NSMenu"));
        if (!m)
            return NULL;
        m = send1(m, "initWithTitle:", mkstr(title ? title : ""));
        a->objs[i] = m;
        a->state[i] = 1;
        void *items = nib_get(a, i, "NSMenuItems");
        for (unsigned long k = 0; k < array_count(items); k++)
            send1(m, "addItem:", array_at(items, k));
        const char *nm = nib_str(a, i, "NSName");
        if (nm) {
            void *app = send0(objc_getClass("NSApplication"), "sharedApplication");
            if (!strcmp(nm, "_NSMainMenu"))
                send1(app, "setMainMenu:", m);
            else if (!strcmp(nm, "_NSWindowsMenu"))
                send1(app, "setWindowsMenu:", m);
            else if (!strcmp(nm, "_NSServicesMenu"))
                send1(app, "setServicesMenu:", m);
            else if (!strcmp(nm, "_NSHelpMenu"))
                send1(app, "setHelpMenu:", m);
            else if (!strcmp(nm, "_NSAppleMenu"))
                send1(app, "setAppleMenu:", m);
        }
        return m;
    }
    if (!strcmp(cn, "NSMenuItem")) {
        const char *title = nib_str(a, i, "NSTitle");
        const char *key = nib_str(a, i, "NSKeyEquiv");
        const char *action = nib_str(a, i, "NSAction");
        int separator = (int)nib_int(a, i, "NSIsSeparator", 0);
        void *cls = objc_getClass("NSMenuItem");
        if (separator && responds(cls, "separatorItem")) {
            o = send0(cls, "separatorItem");
        } else {
            o = alloc_class(cls);
            o = send3(o, "initWithTitle:action:keyEquivalent:",
                      mkstr(title ? title : ""), (void *)action,
                      mkstr(key ? key : ""));
        }
        if (!o)
            return NULL;
        a->objs[i] = o;
        a->state[i] = 1;
        void *sub = nib_get(a, i, "NSSubmenu");
        if (sub)
            send1(o, "setSubmenu:", sub);
        if (nib_int(a, i, "NSIsDisabled", 0))
            send1(o, "setEnabled:", NULL);
        send1(o, "setKeyEquivalentModifierMask:",
              (void *)(uintptr_t)nib_int(a, i, "NSKeyEquivModMask", 0));
        send1(o, "setState:", (void *)(intptr_t)nib_int(a, i, "NSState", 0));
        if (nib_int(a, i, "NSIsAlternate", 0))
            send1(o, "setAlternate:", (void *)1);
        return o;
    }
    if (!strcmp(cn, "NSWindowTemplate")) {
        rect r = nib_rect(a, i);
        const char *wcls = nib_str(a, i, "NSWindowClass");
        void *cls = lookup_class(wcls ? wcls : "NSWindow");
        unsigned long mask = (unsigned long)nib_int(a, i, "NSWindowStyleMask", 0);
        if (!cls)
            return NULL;
        o = alloc_class(cls);
        {
            void *(*fn)(void *, const char *, rect, unsigned long, unsigned long,
                        signed char);
            fn = (void *(*)(void *, const char *, rect, unsigned long,
                            unsigned long, signed char))plsr_objc_lookup_imp(
                o, "initWithContentRect:styleMask:backing:defer:");
            o = fn ? fn(o, "initWithContentRect:styleMask:backing:defer:", r, mask,
                        2, 0)
                   : send0(o, "init");
        }
        a->objs[i] = o;
        a->state[i] = 1;
        const char *title = nib_str(a, i, "NSWindowTitle");
        if (title)
            send1(o, "setTitle:", mkstr(title));
        {
            void *cv = nib_get(a, i, "NSWindowView");
            if (getenv("PLSR_NIB_TRACE"))
                dprintf(2, "nib: window %u title=%s view=%p isView=%d\n", i,
                        title ? title : "(nil)", cv,
                        cv ? is_kind_of(cv, "NSView") : 0);
            if (cv && is_kind_of(cv, "NSView"))
                send1(o, "setContentView:", cv);
        }
        void *tb = nib_get(a, i, "NSWindowToolbar");
        if (tb)
            send1(o, "setToolbar:", tb);
        return o;
    }

    /* Any other nib object whose class is a view becomes a view node. */
    {
        const char *nibcn = a->classes[a->obj_class[i]];
        const char *name = NULL;
        void *cls = nib_object_class(a, i, &name);
        void *probe = NULL;
        if (!cls && !strcmp(nibcn, "NSCustomView"))
            cls = objc_getClass("NSView");
        /* A segue template is archived as its own object (sometimes behind a
         * class swapper), carrying the identifier and destination links. */
        {
            void *tmpl_cls = objc_getClass("NSStoryboardSegueTemplate");
            void *c = cls;
            for (int depth = 0; c && tmpl_cls && depth < 64;
                 depth++, c = class_getSuperclass(c))
                if (c == tmpl_cls) {
                    segue_template_obj *t = alloc_class(cls);
                    if (!t)
                        return NULL;
                    a->objs[i] = t;
                    a->state[i] = 1;
                    t->identifier = nib_get(a, i, "NSIdentifier");
                    t->source = nib_get(a, i, "NSSource");
                    t->destination = nib_get(a, i, "NSDestination");
                    t->style = (long)nib_int(a, i, "showSeguePresentationStyle", 0);
                    t->options = (long)nib_int(
                        a, i, "NSStoryboardSegueDestinationOptions", 0);
                    return t;
                }
        }
        if (cls) {
            void *nsview = objc_getClass("NSView");
            void *c = cls;
            for (int depth = 0; c && depth < 64; depth++, c = class_getSuperclass(c))
                if (c == nsview) {
                    probe = cls;
                    break;
                }
        }
        if (probe) {
            rect r = nib_rect(a, i);
            o = alloc_class(probe);
            if (!o)
                return NULL;
            if (responds(o, "initWithFrame:"))
                o = send_rect(o, "initWithFrame:", r);
            else
                o = send0(o, "init");
            a->objs[i] = o;
            a->state[i] = 1;
            if (responds(o, "setAutoresizingMask:"))
                send1(o, "setAutoresizingMask:",
                      (void *)(uintptr_t)nib_autoresizing(a, i));
            void *subviews = nib_get(a, i, "NSSubviews");
            for (unsigned long k = 0; k < array_count(subviews); k++) {
                void *sv = array_at(subviews, k);
                if (sv)
                    send1(o, "addSubview:", sv);
            }
            void *doc = nib_get(a, i, "NSDocView");
            if (doc) {
                if (responds(o, "setDocumentView:"))
                    send1(o, "setDocumentView:", doc);
                else
                    send1(o, "addSubview:", doc);
            }
            const char *contents = nib_str(a, i, "NSContents");
            if (!contents) {
                const nibval *cell = nib_raw(a, i, "NSCell");
                if (cell && cell->type == NV_REF)
                    contents = nib_str(a, cell->ref, "NSContents");
            }
            if (contents) {
                if (responds(o, "setTitle:"))
                    send1(o, "setTitle:", mkstr(contents));
                else if (responds(o, "setStringValue:"))
                    send1(o, "setStringValue:", mkstr(contents));
            }
            if (nib_int(a, i, "NSEnabled", 1) == 0)
                send1(o, "setEnabled:", NULL);
            if (nib_int(a, i, "NSEditable", 0) && responds(o, "setEditable:"))
                send1(o, "setEditable:", (void *)1);
            return o;
        }
        /* Non-view, non-collection object (a cell, a formatter, ...). */
        if (cls) {
            o = alloc_class(cls);
            if (o && responds(o, "init"))
                o = send0(o, "init");
        }
        return o;
    }
}

static void *nib_ref(nib_archive *a, uint32_t i)
{
    if (!a || i >= a->nobj)
        return NULL;
    if (a->state[i])
        return a->objs[i];
    a->state[i] = 1;
    a->objs[i] = NULL;
    {
        void *o = nib_build(a, i);
        a->objs[i] = o;
        a->state[i] = 2;
        return o;
    }
}

/* ---- connectors -------------------------------------------------------- */

static void apply_outlet(void *src, const char *label, void *dst)
{
    char setter[256];
    size_t len;
    if (!src || !label || !*label)
        return;
    len = strlen(label);
    if (len + 5 > sizeof setter)
        return;
    setter[0] = 's';
    setter[1] = 'e';
    setter[2] = 't';
    setter[3] = (label[0] >= 'a' && label[0] <= 'z') ? (char)(label[0] - 32)
                                                     : label[0];
    memcpy(setter + 4, label + 1, len - 1);
    setter[len + 3] = ':';
    setter[len + 4] = 0;
    if (getenv("PLSR_NIB_TRACE"))
        dprintf(2, "nib: outlet %p %s -> %p (%s found=%d)\n", src, label, dst,
                setter, responds(src, setter));
    if (responds(src, setter))
        send1(src, setter, dst);
}

static void apply_action(void *src, const char *label, void *dst)
{
    if (!src || !label)
        return;
    if (responds(src, "setTarget:"))
        send1(src, "setTarget:", dst);
    if (responds(src, "setAction:"))
        send1(src, "setAction:", (void *)label);
}

static void nib_apply_connectors(nib_archive *a)
{
    for (uint32_t i = 0; i < a->nobj; i++) {
        const char *cn = a->classes[a->obj_class[i]];
        int outlet = !strcmp(cn, "NSNibOutletConnector");
        int action = !strcmp(cn, "NSNibControlConnector");
        const char *label;
        void *src, *dst;
        if (!outlet && !action)
            continue;
        label = nib_str(a, i, "NSLabel");
        src = nib_get(a, i, "NSSource");
        dst = nib_get(a, i, "NSDestination");
        if (outlet)
            apply_outlet(src, label, dst);
        else
            apply_action(src, label, dst);
    }
}

/* ---- loading ----------------------------------------------------------- */

static void collect_top(nib_archive *a, void *array)
{
    for (uint32_t i = 0; i < a->nobj; i++) {
        void *o = a->objs[i];
        if (!o)
            continue;
        if (is_kind_of(o, "NSWindow") || is_kind_of(o, "NSMenu") ||
            is_kind_of(o, "NSViewController")) {
            array_add(array, o);
            continue;
        }
        if (is_kind_of(o, "NSView") && !send0(o, "superview"))
            array_add(array, o);
    }
}

/* Load one compiled nib.  `top` receives an NSArray of top-level objects and
 * `controller` the scene's view controller when the nib has one. */
signed char plsr_nib_load(const char *path, void *owner, void **top,
                          void **controller)
{
    size_t len = 0;
    uint8_t *data;
    nib_archive *a;
    if (top)
        *top = NULL;
    if (controller)
        *controller = NULL;
    if (!path)
        return 0;
    if (getenv("PLSR_NIB_TRACE"))
        dprintf(2, "nib: load %s\n", path);
    data = read_file(path, &len);
    if (!data) {
        if (getenv("PLSR_NIB_TRACE"))
            dprintf(2, "nib: cannot read %s\n", path);
        return 0;
    }
    a = nib_archive_parse(data, len);
    free(data);
    if (!a) {
        if (getenv("PLSR_NIB_TRACE"))
            dprintf(2, "nib: not a NIBArchive: %s\n", path);
        return 0;
    }

    a->owner = owner;
    {
        const nibval *r = nib_raw(a, 1, "NSRoot");
        if (r && r->type == NV_REF)
            a->root = r->ref;
        else {
            for (uint32_t i = 0; i < a->nobj; i++)
                if (!strcmp(a->classes[a->obj_class[i]], "NSIBObjectData")) {
                    a->root = i;
                    break;
                }
        }
    }
    if (a->root && owner) {
        a->objs[a->root] = owner;
        a->state[a->root] = 2;
    }
    for (uint32_t i = 0; i < a->nobj; i++)
        nib_ref(a, i);
    nib_apply_connectors(a);

    if (top) {
        void *arr = mkarray();
        collect_top(a, arr);
        *top = arr;
    }
    {
        void *scene_controller = NULL;
        for (uint32_t i = 0; i < a->nobj; i++) {
            void *o = a->objs[i];
            if (o && is_kind_of(o, "NSViewController") &&
                responds(o, "setView:")) {
                scene_controller = o;
                break;
            }
        }
        /* Templates belong to their source controller, or to the scene's only
         * controller when the archive records no explicit source. */
        for (uint32_t i = 0; i < a->nobj; i++) {
            void *o = a->objs[i];
            segue_template_obj *t;
            void *src;
            if (!o || !is_kind_of(o, "NSStoryboardSegueTemplate"))
                continue;
            t = (segue_template_obj *)o;
            src = t->source;
            if (!src || !is_kind_of(src, "NSViewController"))
                src = scene_controller;
            if (src)
                vc_add_segue(src, o);
        }
        if (controller)
            *controller = scene_controller;
    }
    {
        void *vis = nib_get(a, a->root, "NSVisibleWindows");
        for (unsigned long k = 0; k < array_count(vis); k++) {
            void *w = array_at(vis, k);
            if (w)
                send1(w, "makeKeyAndOrderFront:", NULL);
        }
    }
    return 1;
}

signed char plsr_nib_load_named(void *bundle, void *name, void *owner, void **top)
{
    const char *n = name ? cstr(name) : NULL;
    void *path;
    if (!n || !*n)
        return 0;
    path = send2(bundle, "pathForResource:ofType:", name, mkstr("nib"));
    return plsr_nib_load(path ? cstr(path) : n, owner, top, NULL);
}

/* ---- NSNib ------------------------------------------------------------- */

typedef struct {
    void *isa;
    char *path;
    void *data;
} nib_obj;

static void *nib_alloc(void *cls, const char *sel)
{
    nib_obj *n = calloc(1, sizeof *n);
    (void)sel;
    if (n)
        n->isa = cls;
    return n;
}

static void *nib_init_named(void *self, const char *sel, void *name, void *bundle)
{
    nib_obj *n = self;
    void *path;
    (void)sel;
    if (!n)
        return NULL;
    if (!bundle)
        bundle = send0(objc_getClass("NSBundle"), "mainBundle");
    path = send2(bundle, "pathForResource:ofType:", name, mkstr("nib"));
    n->path = path ? strdup(cstr(path)) : NULL;
    return n;
}

static void *nib_init_data(void *self, const char *sel, void *data, void *bundle)
{
    nib_obj *n = self;
    (void)sel;
    (void)bundle;
    if (!n)
        return NULL;
    n->data = data;
    return n;
}

static signed char nib_instantiate(void *self, const char *sel, void *owner,
                                   void **top)
{
    nib_obj *n = self;
    (void)sel;
    if (!n || !n->path)
        return 0;
    return plsr_nib_load(n->path, owner, top, NULL);
}

static method_array NSNib_methods = {
    24, 3, {
        { "initWithNibNamed:bundle:", "", (void *)nib_init_named },
        { "initWithNibData:bundle:", "", (void *)nib_init_data },
        { "instantiateWithOwner:topLevelObjects:", "", (void *)nib_instantiate },
    }
};

static method_array NSNib_class_methods = {
    24, 1, { { "alloc", "", (void *)nib_alloc } }
};

/* ---- NSViewController -------------------------------------------------- */

typedef struct {
    void *isa;
    void *nib_name;
    void *bundle;
    void *view;
    void *represented;
    void *identifier;
    void *title;
    void *storyboard;
    signed char loaded;
    void **presented;      /* controllers this controller presents */
    int npresented;
    void *presenting;      /* controller that presented this one */
    void *presented_window;
    void **segue_templates;
    int nsegues;
} vc_obj;

static void *vc_alloc(void *cls, const char *sel)
{
    vc_obj *v = calloc(1, sizeof *v);
    (void)sel;
    if (v)
        v->isa = cls;
    return v;
}

static void *vc_init_with_nib(void *self, const char *sel, void *name, void *bundle)
{
    vc_obj *v = self;
    (void)sel;
    if (v) {
        v->nib_name = name;
        v->bundle = bundle;
    }
    return self;
}

static void *vc_nib_name(void *self, const char *sel)
{
    (void)sel;
    return ((vc_obj *)self)->nib_name;
}

static void *vc_bundle(void *self, const char *sel)
{
    (void)sel;
    return ((vc_obj *)self)->bundle;
}

static const char *sb_directory(void *sb);
static char *vc_nib_path(vc_obj *v);

static void *vc_load_view(vc_obj *v, int *found)
{
    char *path;
    void *top = NULL;
    if (found)
        *found = 0;
    if (!v || !v->nib_name)
        return NULL;
    path = vc_nib_path(v);
    if (getenv("PLSR_NIB_TRACE"))
        dprintf(2, "nib: controller %p nib '%s' path %s\n", (void *)v,
                v->nib_name ? cstr(v->nib_name) : "(nil)", path ? path : "(nil)");
    if (!path)
        return NULL;
    if (plsr_nib_load(path, v, &top, NULL)) {
        if (!v->view && top) {
            for (unsigned long i = 0; i < array_count(top); i++) {
                void *o = array_at(top, i);
                if (o && is_kind_of(o, "NSView")) {
                    v->view = o;
                    break;
                }
            }
        }
        if (found)
            *found = 1;
    }
    free(path);
    return v->view;
}

static void *vc_view(void *self, const char *sel)
{
    vc_obj *v = self;
    int found = 0;
    (void)sel;
    if (!v)
        return NULL;
    if (!v->view && !v->loaded) {
        v->loaded = 1;
        vc_load_view(v, &found);
        if (responds(self, "viewDidLoad"))
            send0(self, "viewDidLoad");
    }
    return v->view;
}

static void vc_set_view(void *self, const char *sel, void *view)
{
    (void)sel;
    ((vc_obj *)self)->view = view;
}

static signed char vc_is_view_loaded(void *self, const char *sel)
{
    (void)sel;
    return (signed char)(((vc_obj *)self)->view != NULL);
}

static void *vc_load_view_sel(void *self, const char *sel)
{
    (void)sel;
    return vc_load_view(self, NULL);
}

static void vc_set_nib_name(void *self, const char *sel, void *name)
{
    (void)sel;
    ((vc_obj *)self)->nib_name = name;
}

static void vc_set_represented(void *self, const char *sel, void *obj)
{
    (void)sel;
    ((vc_obj *)self)->represented = obj;
}

static void *vc_represented(void *self, const char *sel)
{
    (void)sel;
    return ((vc_obj *)self)->represented;
}

static void vc_set_identifier(void *self, const char *sel, void *ident)
{
    (void)sel;
    ((vc_obj *)self)->identifier = ident;
}

static void *vc_identifier(void *self, const char *sel)
{
    (void)sel;
    return ((vc_obj *)self)->identifier;
}

static void vc_set_title(void *self, const char *sel, void *title)
{
    (void)sel;
    ((vc_obj *)self)->title = title;
}

static void *vc_title(void *self, const char *sel)
{
    (void)sel;
    return ((vc_obj *)self)->title;
}

static void vc_set_storyboard(void *self, const char *sel, void *sb)
{
    (void)sel;
    ((vc_obj *)self)->storyboard = sb;
}

static void *vc_storyboard(void *self, const char *sel)
{
    (void)sel;
    return ((vc_obj *)self)->storyboard;
}

static void vc_add_segue(void *vc, void *tmpl)
{
    vc_obj *v = vc;
    void **grown;
    if (!v || !tmpl)
        return;
    grown = realloc(v->segue_templates,
                    (size_t)(v->nsegues + 1) * sizeof *grown);
    if (!grown)
        return;
    v->segue_templates = grown;
    v->segue_templates[v->nsegues++] = tmpl;
}

/* ---- presentation ------------------------------------------------------ */

static rect vc_view_frame(void *view)
{
    rect (*fn)(void *, const char *);
    rect r = { 0, 0, 480, 320 };
    if (!view)
        return r;
    fn = (rect (*)(void *, const char *))plsr_objc_lookup_imp(view, "frame");
    if (fn) {
        r = fn(view, "frame");
        if (r.w < 1.0 || r.h < 1.0) {
            r.w = 480;
            r.h = 320;
        }
    }
    return r;
}

static void *vc_presentation_window(rect r)
{
    void *cls = objc_getClass("NSWindow");
    void *win;
    void *(*fn)(void *, const char *, rect, unsigned long, unsigned long,
                signed char);
    if (!cls)
        return NULL;
    win = alloc_class(cls);
    if (!win)
        return NULL;
    fn = (void *(*)(void *, const char *, rect, unsigned long, unsigned long,
                    signed char))plsr_objc_lookup_imp(
        win, "initWithContentRect:styleMask:backing:defer:");
    if (fn)
        return fn(win, "initWithContentRect:styleMask:backing:defer:", r,
                  1 | 2 | 8, 2, 0);
    return send0(win, "init");
}

static void vc_presented_add(vc_obj *v, void *vc)
{
    void **grown = realloc(v->presented,
                           (size_t)(v->npresented + 1) * sizeof *grown);
    if (!grown)
        return;
    v->presented = grown;
    v->presented[v->npresented++] = vc;
}

/* Modal windows, sheets and popovers all get a real window in the software
 * surface; the sheet/popover chrome itself is not drawn yet. */
static void vc_present_controller(void *self, void *vc)
{
    vc_obj *v = self, *d = vc;
    void *view, *win;
    rect r;
    if (!v || !d || v == d)
        return;
    view = d->view ? d->view : vc_view(vc, "view");
    r = vc_view_frame(view);
    win = vc_presentation_window(r);
    if (!win)
        return;
    if (view)
        send1(win, "setContentView:", view);
    send1(win, "setContentViewController:", vc);
    if (d->title)
        send1(win, "setTitle:", d->title);
    if (responds(vc, "viewWillAppear"))
        send0(vc, "viewWillAppear");
    send1(win, "makeKeyAndOrderFront:", NULL);
    if (responds(vc, "viewDidAppear"))
        send0(vc, "viewDidAppear");
    d->presented_window = win;
    d->presenting = self;
    vc_presented_add(v, vc);
}

static void vc_present_modal(void *self, const char *sel, void *vc)
{
    (void)sel;
    vc_present_controller(self, vc);
}

static void vc_present_sheet(void *self, const char *sel, void *vc)
{
    (void)sel;
    vc_present_controller(self, vc);
}

static void vc_present_popover(void *self, const char *sel, void *vc, rect r,
                               void *view, unsigned long edge, long behavior)
{
    (void)sel;
    (void)r;
    (void)view;
    (void)edge;
    (void)behavior;
    vc_present_controller(self, vc);
}

static void *vc_presented_list(void *self, const char *sel)
{
    vc_obj *v = self;
    void *arr = mkarray();
    (void)sel;
    if (!v || !arr)
        return arr;
    for (int i = 0; i < v->npresented; i++)
        array_add(arr, v->presented[i]);
    return arr;
}

static void *vc_presenting(void *self, const char *sel)
{
    (void)sel;
    return ((vc_obj *)self)->presenting;
}

static void vc_dismiss(void *self, const char *sel, void *vc)
{
    vc_obj *v = self, *d = vc;
    (void)sel;
    if (!v || !d)
        return;
    for (int i = 0; i < v->npresented; i++)
        if (v->presented[i] == vc) {
            memmove(&v->presented[i], &v->presented[i + 1],
                    (size_t)(v->npresented - i - 1) * sizeof *v->presented);
            v->npresented--;
            break;
        }
    if (d->presented_window) {
        send1(d->presented_window, "orderOut:", NULL);
        d->presented_window = NULL;
    }
    d->presenting = NULL;
}

static void vc_dismiss_self(void *self, const char *sel, void *sender)
{
    vc_obj *v = self;
    (void)sel;
    (void)sender;
    if (v && v->presenting)
        send1(v->presenting, "dismissViewController:", self);
}

/* ---- segues ------------------------------------------------------------ */

static void *segue_alloc(void *cls, const char *sel)
{
    segue_obj *s = calloc(1, sizeof *s);
    (void)sel;
    if (s)
        s->isa = cls;
    return s;
}

static void *segue_new(void *ident, void *src, void *dst, void *handler)
{
    void *cls = objc_getClass("NSStoryboardSegue");
    segue_obj *s = cls ? segue_alloc(cls, "alloc") : NULL;
    if (!s)
        return NULL;
    s->identifier = ident;
    s->source = src;
    s->destination = dst;
    s->handler = handler;
    return s;
}

static void *segue_init(void *self, const char *sel, void *ident, void *src,
                        void *dst, void *handler)
{
    segue_obj *s = self;
    (void)sel;
    if (s) {
        s->identifier = ident;
        s->source = src;
        s->destination = dst;
        s->handler = handler;
    }
    return self;
}

static void *segue_with(void *cls, const char *sel, void *ident, void *src,
                        void *dst, void *handler)
{
    (void)sel;
    (void)cls;
    return segue_new(ident, src, dst, handler);
}

static void *segue_identifier(void *self, const char *sel)
{
    (void)sel;
    return ((segue_obj *)self)->identifier;
}

static void *segue_source(void *self, const char *sel)
{
    (void)sel;
    return ((segue_obj *)self)->source;
}

static void *segue_destination(void *self, const char *sel)
{
    (void)sel;
    return ((segue_obj *)self)->destination;
}

static void *segue_handler(void *self, const char *sel)
{
    (void)sel;
    return ((segue_obj *)self)->handler;
}

static void segue_perform(void *self, const char *sel)
{
    segue_obj *s = self;
    (void)sel;
    if (!s || s->handler)
        return;   /* block perform handlers are not modelled yet */
    if (s->source && s->destination &&
        responds(s->source, "presentViewControllerAsModalWindow:"))
        send1(s->source, "presentViewControllerAsModalWindow:", s->destination);
}

static void *segue_template_alloc(void *cls, const char *sel)
{
    segue_template_obj *t = calloc(1, sizeof *t);
    (void)sel;
    if (t)
        t->isa = cls;
    return t;
}

static signed char vc_should_perform_segue(void *self, const char *sel,
                                           void *ident, void *sender)
{
    (void)self;
    (void)sel;
    (void)ident;
    (void)sender;
    return 1;
}

static void vc_prepare_segue(void *self, const char *sel, void *segue,
                             void *sender)
{
    (void)self;
    (void)sel;
    (void)segue;
    (void)sender;
}

static void vc_perform_segue(void *self, const char *sel, void *ident,
                             void *sender)
{
    vc_obj *v = self;
    const char *id = ident ? cstr(ident) : NULL;
    void *dest = NULL;
    void *segue;
    (void)sel;
    if (!v || !id)
        return;
    if (!(signed char)(intptr_t)send2(
            self, "shouldPerformSegueWithIdentifier:sender:", ident, sender))
        return;
    for (int i = 0; i < v->nsegues; i++) {
        segue_template_obj *t = (segue_template_obj *)v->segue_templates[i];
        const char *tid = t && t->identifier ? cstr(t->identifier) : NULL;
        if (tid && !strcmp(tid, id)) {
            dest = t->destination;
            break;
        }
    }
    if (!dest)
        return;
    segue = segue_new(ident, self, dest, NULL);
    if (!segue)
        return;
    if (responds(self, "prepareForSegue:sender:"))
        send2(self, "prepareForSegue:sender:", segue, sender);
    send0(segue, "perform");
}

static void vc_noop(void *self, const char *sel)
{
    (void)self;
    (void)sel;
}

static method_array NSViewController_methods = {
    24, 29, {
        { "initWithNibName:bundle:", "", (void *)vc_init_with_nib },
        { "nibName", "", (void *)vc_nib_name },
        { "bundle", "", (void *)vc_bundle },
        { "view", "", (void *)vc_view },
        { "setView:", "", (void *)vc_set_view },
        { "isViewLoaded", "", (void *)vc_is_view_loaded },
        { "loadView", "", (void *)vc_load_view_sel },
        { "viewDidLoad", "", (void *)vc_noop },
        { "setNibName:", "", (void *)vc_set_nib_name },
        { "setRepresentedObject:", "", (void *)vc_set_represented },
        { "representedObject", "", (void *)vc_represented },
        { "setIdentifier:", "", (void *)vc_set_identifier },
        { "identifier", "", (void *)vc_identifier },
        { "setTitle:", "", (void *)vc_set_title },
        { "title", "", (void *)vc_title },
        { "setStoryboard:", "", (void *)vc_set_storyboard },
        { "storyboard", "", (void *)vc_storyboard },
        { "viewWillAppear", "", (void *)vc_noop },
        { "presentViewControllerAsModalWindow:", "", (void *)vc_present_modal },
        { "presentViewControllerAsSheet:", "", (void *)vc_present_sheet },
        { "presentViewController:asPopoverRelativeToRect:ofView:preferredEdge:behavior:", "",
          (void *)vc_present_popover },
        { "presentedViewControllers", "", (void *)vc_presented_list },
        { "presentingViewController", "", (void *)vc_presenting },
        { "dismissViewController:", "", (void *)vc_dismiss },
        { "dismissController:", "", (void *)vc_dismiss_self },
        { "performSegueWithIdentifier:sender:", "", (void *)vc_perform_segue },
        { "shouldPerformSegueWithIdentifier:sender:", "",
          (void *)vc_should_perform_segue },
        { "prepareForSegue:sender:", "", (void *)vc_prepare_segue },
    }
};

static method_array NSStoryboardSegue_methods = {
    24, 6, {
        { "initWithIdentifier:source:destination:performHandler:", "",
          (void *)segue_init },
        { "identifier", "", (void *)segue_identifier },
        { "sourceController", "", (void *)segue_source },
        { "destinationController", "", (void *)segue_destination },
        { "performHandler", "", (void *)segue_handler },
        { "perform", "", (void *)segue_perform },
    }
};

static method_array NSStoryboardSegue_class_methods = {
    24, 2, {
        { "alloc", "", (void *)segue_alloc },
        { "segueWithIdentifier:source:destination:performHandler:", "",
          (void *)segue_with },
    }
};

static method_array NSStoryboardSegueTemplate_class_methods = {
    24, 1, { { "alloc", "", (void *)segue_template_alloc } }
};

static method_array NSViewController_class_methods = {
    24, 1, { { "alloc", "", (void *)vc_alloc } }
};

/* ---- NSStoryboard ------------------------------------------------------ */

typedef struct {
    void *isa;
    char *dir;
    char *name;
    void *bundle;
    void *nib_map;
    void *uuid_map;
    char *entry;
    char *main_menu;
} sb_obj;

static const char *sb_directory(void *sb)
{
    return sb ? ((sb_obj *)sb)->dir : NULL;
}

static char *vc_nib_path(vc_obj *v)
{
    const char *name = v && v->nib_name ? cstr(v->nib_name) : NULL;
    const char *dir = v && v->storyboard ? sb_directory(v->storyboard) : NULL;
    char buf[PATH_MAX];
    if (!name || !*name)
        return NULL;
    if (dir) {
        snprintf(buf, sizeof buf, "%s/%s.nib", dir, name);
        return strdup(buf);
    }
    {
        void *bundle = v->bundle ? v->bundle
                                 : send0(objc_getClass("NSBundle"), "mainBundle");
        void *path = send2(bundle, "pathForResource:ofType:", v->nib_name,
                           mkstr("nib"));
        return path ? strdup(cstr(path)) : NULL;
    }
}

static void *sb_alloc(void *cls, const char *sel)
{
    sb_obj *s = calloc(1, sizeof *s);
    (void)sel;
    if (s)
        s->isa = cls;
    return s;
}

static const char *dict_string(void *dict, const char *key)
{
    void *v = send1(dict, "objectForKey:", mkstr(key));
    return v ? cstr(v) : NULL;
}

static void *sb_storyboard_with_name(void *cls, const char *sel, void *name,
                                     void *bundle)
{
    const char *n = name ? cstr(name) : NULL;
    void *path, *info, *sb;
    char buf[PATH_MAX];
    sb_obj *s;
    (void)sel;
    if (!n || !*n)
        return NULL;
    if (!bundle)
        bundle = send0(objc_getClass("NSBundle"), "mainBundle");
    path = send2(bundle, "pathForResource:ofType:", name, mkstr("storyboardc"));
    if (!path)
        return NULL;
    sb = sb_alloc(cls, "alloc");
    s = sb;
    if (!s)
        return NULL;
    s->dir = strdup(cstr(path));
    s->name = strdup(n);
    s->bundle = bundle;
    snprintf(buf, sizeof buf, "%s/Info.plist", s->dir);
    info = send1(objc_getClass("NSDictionary"), "dictionaryWithContentsOfFile:",
                 mkstr(buf));
    if (info) {
        const char *v;
        s->nib_map = send1(info, "objectForKey:",
                           mkstr("NSViewControllerIdentifiersToNibNames"));
        s->uuid_map = send1(info, "objectForKey:",
                            mkstr("NSViewControllerIdentifiersToUUIDs"));
        v = dict_string(info, "NSStoryboardDesignatedEntryPointIdentifier");
        s->entry = v ? strdup(v) : NULL;
        v = dict_string(info, "NSStoryboardMainMenu");
        s->main_menu = v ? strdup(v) : NULL;
    }
    return sb;
}

static void *sb_main_storyboard(void *cls, const char *sel)
{
    void *bundle = send0(objc_getClass("NSBundle"), "mainBundle");
    void *name = send1(bundle, "objectForInfoDictionaryKey:",
                       mkstr("NSMainStoryboardFile"));
    (void)sel;
    if (!name)
        return NULL;
    return sb_storyboard_with_name(cls, "storyboardWithName:bundle:", name, bundle);
}

static char *sb_scene_path(sb_obj *s, const char *nibname)
{
    char buf[PATH_MAX];
    if (!s || !nibname)
        return NULL;
    snprintf(buf, sizeof buf, "%s/%s.nib", s->dir, nibname);
    return strdup(buf);
}

static void *sb_load_scene(sb_obj *s, const char *nibname)
{
    char *path;
    void *top = NULL, *controller = NULL;
    if (!s || !nibname || !*nibname)
        return NULL;
    path = sb_scene_path(s, nibname);
    if (!path)
        return NULL;
    if (!plsr_nib_load(path, NULL, &top, &controller)) {
        free(path);
        return NULL;
    }
    free(path);
    if (controller && responds(controller, "setStoryboard:"))
        send1(controller, "setStoryboard:", s);
    return controller;
}

static void *sb_instantiate_initial(void *self, const char *sel)
{
    sb_obj *s = self;
    const char *nibname = NULL;
    (void)sel;
    if (!s)
        return NULL;
    if (s->entry && s->nib_map)
        nibname = dict_string(s->nib_map, s->entry);
    if (!nibname && s->main_menu)
        return NULL;   /* menu-only storyboard: no initial controller */
    return nibname ? sb_load_scene(s, nibname) : NULL;
}

static void *sb_instantiate_identifier(void *self, const char *sel, void *ident)
{
    sb_obj *s = self;
    const char *id = ident ? cstr(ident) : NULL;
    const char *nibname;
    (void)sel;
    if (!s || !s->nib_map || !id)
        return NULL;
    nibname = dict_string(s->nib_map, id);
    return nibname ? sb_load_scene(s, nibname) : NULL;
}

static void *sb_load_main_menu(void *self, const char *sel, void *owner)
{
    sb_obj *s = self;
    char *path;
    void *top = NULL;
    (void)sel;
    if (!s || !s->main_menu)
        return NULL;
    path = sb_scene_path(s, s->main_menu);
    if (!path)
        return NULL;
    if (!plsr_nib_load(path, owner, &top, NULL)) {
        free(path);
        return NULL;
    }
    free(path);
    if (top)
        for (unsigned long i = 0; i < array_count(top); i++) {
            void *o = array_at(top, i);
            if (o && is_kind_of(o, "NSMenu"))
                return o;
        }
    return NULL;
}

static void *sb_name(void *self, const char *sel)
{
    (void)sel;
    return mkstr(((sb_obj *)self)->name ? ((sb_obj *)self)->name : "");
}

static void *sb_bundle(void *self, const char *sel)
{
    (void)sel;
    return ((sb_obj *)self)->bundle;
}

static void *sb_main_menu_name(void *self, const char *sel)
{
    (void)sel;
    return ((sb_obj *)self)->main_menu ? mkstr(((sb_obj *)self)->main_menu) : NULL;
}

static void *sb_load_main_menu0(void *self, const char *sel)
{
    void *app = send0(objc_getClass("NSApplication"), "sharedApplication");
    return sb_load_main_menu(self, sel, app);
}

static void *sb_nib_for_identifier(void *self, const char *sel, void *ident)
{
    sb_obj *s = self;
    const char *id = ident ? cstr(ident) : NULL;
    (void)sel;
    return (s && s->nib_map && id) ? mkstr(dict_string(s->nib_map, id)) : NULL;
}

static method_array NSStoryboard_methods = {
    24, 8, {
        { "instantiateInitialController", "", (void *)sb_instantiate_initial },
        { "instantiateControllerWithIdentifier:", "", (void *)sb_instantiate_identifier },
        { "mainMenu", "", (void *)sb_main_menu_name },
        { "name", "", (void *)sb_name },
        { "containingBundle", "", (void *)sb_bundle },
        { "loadMainMenuWithOwner:", "", (void *)sb_load_main_menu },
        { "loadMainMenu", "", (void *)sb_load_main_menu0 },
        { "nibForControllerWithIdentifier:", "", (void *)sb_nib_for_identifier },
    }
};

static method_array NSStoryboard_class_methods = {
    24, 3, {
        { "alloc", "", (void *)sb_alloc },
        { "storyboardWithName:bundle:", "", (void *)sb_storyboard_with_name },
        { "mainStoryboard", "", (void *)sb_main_storyboard },
    }
};


/* ---- class metadata ---------------------------------------------------- */

#define RO(flags_, size_, name_, methods_)                                    \
    { (flags_), 0, (size_), 0, NULL, (name_), (plsr_method_list *)(methods_),   \
      NULL, NULL, NULL, NULL }

#define DECLARE_CLASS(n)                                                      \
    plsr_class plsr_##n##_class __asm__("_OBJC_CLASS_$_" #n);                 \
    plsr_class plsr_##n##_meta __asm__("_OBJC_METACLASS_$_" #n);

extern plsr_class plsr_NSObject_class __asm__("_OBJC_CLASS_$_NSObject");
extern plsr_class plsr_NSObject_meta __asm__("_OBJC_METACLASS_$_NSObject");
extern plsr_class plsr_NSResponder_class __asm__("_OBJC_CLASS_$_NSResponder");
extern plsr_class plsr_NSResponder_meta __asm__("_OBJC_METACLASS_$_NSResponder");

DECLARE_CLASS(NSNib)
DECLARE_CLASS(NSStoryboard)
DECLARE_CLASS(NSViewController)
DECLARE_CLASS(NSStoryboardSegue)
DECLARE_CLASS(NSStoryboardSegueTemplate)

static const plsr_class_ro NSNib_ro = RO(0, sizeof(nib_obj), "NSNib", &NSNib_methods);
static const plsr_class_ro NSNib_meta_ro = RO(PLSR_RO_META, sizeof(nib_obj), "NSNib", &NSNib_class_methods);
static const plsr_class_ro NSStoryboard_ro = RO(0, sizeof(sb_obj), "NSStoryboard", &NSStoryboard_methods);
static const plsr_class_ro NSStoryboard_meta_ro = RO(PLSR_RO_META, sizeof(sb_obj), "NSStoryboard", &NSStoryboard_class_methods);
static const plsr_class_ro NSViewController_ro = RO(0, sizeof(vc_obj), "NSViewController", &NSViewController_methods);
static const plsr_class_ro NSViewController_meta_ro = RO(PLSR_RO_META, sizeof(vc_obj), "NSViewController", &NSViewController_class_methods);
static const plsr_class_ro NSStoryboardSegue_ro = RO(0, sizeof(segue_obj), "NSStoryboardSegue", &NSStoryboardSegue_methods);
static const plsr_class_ro NSStoryboardSegue_meta_ro = RO(PLSR_RO_META, sizeof(segue_obj), "NSStoryboardSegue", &NSStoryboardSegue_class_methods);
static const plsr_class_ro NSStoryboardSegueTemplate_ro = RO(0, sizeof(segue_template_obj), "NSStoryboardSegueTemplate", NULL);
static const plsr_class_ro NSStoryboardSegueTemplate_meta_ro = RO(PLSR_RO_META, sizeof(segue_template_obj), "NSStoryboardSegueTemplate", &NSStoryboardSegueTemplate_class_methods);

static void wire(plsr_class *cls, plsr_class *meta, plsr_class *super,
                 plsr_class *meta_super, const plsr_class_ro *ro,
                 const plsr_class_ro *meta_ro)
{
    cls->isa = meta;
    cls->superclass = super;
    cls->data = (uintptr_t)ro;
    meta->isa = meta;
    meta->superclass = meta_super;
    meta->data = (uintptr_t)meta_ro;
    plsr_objc_register_class(cls);
}

static void nib_init(void) __attribute__((constructor));
static void nib_init(void)
{
    /* Instance methods live in the class's own list; class methods in the
     * metaclass list.  NSNib's instantiate selector is an instance method. */
    wire(&plsr_NSNib_class, &plsr_NSNib_meta, &plsr_NSObject_class,
         &plsr_NSObject_meta, &NSNib_ro, &NSNib_meta_ro);
    wire(&plsr_NSStoryboard_class, &plsr_NSStoryboard_meta, &plsr_NSObject_class,
         &plsr_NSObject_meta, &NSStoryboard_ro, &NSStoryboard_meta_ro);
    wire(&plsr_NSViewController_class, &plsr_NSViewController_meta,
         &plsr_NSResponder_class, &plsr_NSResponder_meta, &NSViewController_ro,
         &NSViewController_meta_ro);
    wire(&plsr_NSStoryboardSegue_class, &plsr_NSStoryboardSegue_meta,
         &plsr_NSObject_class, &plsr_NSObject_meta, &NSStoryboardSegue_ro,
         &NSStoryboardSegue_meta_ro);
    wire(&plsr_NSStoryboardSegueTemplate_class,
         &plsr_NSStoryboardSegueTemplate_meta, &plsr_NSObject_class,
         &plsr_NSObject_meta, &NSStoryboardSegueTemplate_ro,
         &NSStoryboardSegueTemplate_meta_ro);
}
