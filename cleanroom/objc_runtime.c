/*
 * Clean-room Objective-C runtime core.
 *
 * Handles the non-fragile x86_64 metadata shipped by modern clang:
 *   - __objc_classlist with plain (24-byte) and relative (12-byte) method lists
 *   - __objc_catlist categories (instance and class methods)
 *   - selector strings, class/instance dispatch, super and struct-return sends
 *
 * Selectors are the selector cstrings themselves, which is what dyld leaves in
 * __objc_methname. Not implemented: protocols, properties metadata, ivar
 * layout, weak references, exceptions, and the full ARC optimizer surface.
 */
#define _GNU_SOURCE
#include "objc_abi.h"

#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Linker-visible metadata singletons. Class and category structures built by
 * clang reference these globals; they must exist even though this runtime
 * does not maintain a method cache. */
void *plsr_objc_empty_cache __asm__("__objc_empty_cache") = NULL;
void *plsr_objc_empty_vtable __asm__("__objc_empty_vtable") = NULL;

#define MAX_CLASSES 8192
#define MAX_CATEGORIES 4096

static plsr_class *g_classes[MAX_CLASSES];
static int g_nclasses;

struct category_entry {
    plsr_class *cls;
    plsr_method_list *instance_methods;
    plsr_method_list *class_methods;
};
static struct category_entry g_categories[MAX_CATEGORIES];
static int g_ncategories;

/* Every dispatch currently walks the category table and the superclass chain.
 * The app registers thousands of categories, which makes the run loop spend
 * seconds in NSDefaultRunLoopMode equality checks before it can build a
 * window.  Cache the resolved IMP (including a negative result) per
 * (class, selector).  The registry is static after image scanning, so the
 * only invalidation needed is when a class or category is registered. */
#define METHOD_CACHE_SIZE 16384u
#define METHOD_CACHE_MASK (METHOD_CACHE_SIZE - 1u)

struct method_cache_entry {
    plsr_class *cls;
    const char *sel;
    void *imp;
    uint32_t hash;
};
static struct method_cache_entry g_method_cache[METHOD_CACHE_SIZE];

static uint32_t method_cache_hash(plsr_class *c, const char *sel)
{
    uint32_t h = 2166136261u;
    uintptr_t p = (uintptr_t)c;
    for (int i = 0; i < (int)sizeof(p); i++) {
        h = (h ^ (uint32_t)((p >> (i * 8)) & 0xffu)) * 16777619u;
    }
    while (sel && *sel)
        h = (h ^ (unsigned char)*sel++) * 16777619u;
    return h;
}

static void method_cache_clear(void)
{
    for (size_t i = 0; i < METHOD_CACHE_SIZE; i++) {
        g_method_cache[i].sel = NULL;
        g_method_cache[i].cls = NULL;
        g_method_cache[i].imp = NULL;
        g_method_cache[i].hash = 0;
    }
}

static int method_cache_get(plsr_class *c, const char *sel, void **out)
{
    uint32_t h;
    struct method_cache_entry *e;
    if (!c || !sel)
        return 0;
    h = method_cache_hash(c, sel);
    e = &g_method_cache[h & METHOD_CACHE_MASK];
    if (e->cls == c && e->hash == h && e->sel && !strcmp(e->sel, sel)) {
        *out = e->imp;
        return 1;
    }
    return 0;
}

static void method_cache_put(plsr_class *c, const char *sel, void *imp)
{
    uint32_t h;
    struct method_cache_entry *e;
    if (!c || !sel)
        return;
    h = method_cache_hash(c, sel);
    e = &g_method_cache[h & METHOD_CACHE_MASK];
    /* Selector names are interned by the image loader; keep the stable
     * pointer rather than allocating/freeing cache keys.  This also avoids
     * a category-registration race with concurrent lookups. */
    e->sel = sel;
    e->cls = c;
    e->imp = imp;
    e->hash = h;
}

static const char *class_name(plsr_class *c)
{
    const plsr_class_ro *ro;
    if (!c || !c->data)
        return "";
    ro = PLSR_RO(c);
    return ro->name ? ro->name : "";
}

void plsr_objc_register_class(plsr_class *c)
{
    const char *name;
    if (!c || !c->data)
        return;
    name = class_name(c);
    if (!*name)
        return;
    for (int i = 0; i < g_nclasses; i++)
        if (!strcmp(class_name(g_classes[i]), name))
            return;
    if (g_nclasses < MAX_CLASSES)
        g_classes[g_nclasses++] = c;
    if (getenv("PLSR_OBJC_DEBUG"))
        dprintf(2, "objc: register class %s at %p\n", name, (void *)c);
}

void plsr_objc_register_category(plsr_class *cls, plsr_method_list *instance_methods,
                                 plsr_method_list *class_methods)
{
    if (!cls || (!instance_methods && !class_methods))
        return;
    if (g_ncategories >= MAX_CATEGORIES)
        return;
    method_cache_clear();
    g_categories[g_ncategories].cls = cls;
    g_categories[g_ncategories].instance_methods = instance_methods;
    g_categories[g_ncategories].class_methods = class_methods;
    g_ncategories++;
    if (getenv("PLSR_OBJC_DEBUG"))
        dprintf(2, "objc: category on %s (inst %p cls %p)\n",
                class_name(cls), (void *)instance_methods, (void *)class_methods);
}

/* ---- method lists: plain and relative ---- */

static int list_is_relative(const plsr_method_list *ml)
{
    return (ml->entsize_flags & 0x80000000u) != 0;
}

static uint32_t list_stride(const plsr_method_list *ml)
{
    uint32_t eaf;
    if (list_is_relative(ml))
        return 12;
    eaf = ml->entsize_flags & 0x0000fffc;
    return eaf ? eaf : PLSR_METHOD_SIZE;
}

static const uint8_t *list_entry(const plsr_method_list *ml, uint32_t i)
{
    return (const uint8_t *)ml + 8 + (size_t)i * list_stride(ml);
}

static const char *entry_name(const plsr_method_list *ml, const uint8_t *e)
{
    if (list_is_relative(ml)) {
        int32_t off;
        memcpy(&off, e, 4);
        return (const char *)(e + off);
    }
    return ((const plsr_method *)e)->name;
}

static void *entry_imp(const plsr_method_list *ml, const uint8_t *e)
{
    if (list_is_relative(ml)) {
        int32_t off;
        memcpy(&off, e + 8, 4);
        return (void *)(e + 8 + off);
    }
    return ((const plsr_method *)e)->imp;
}

static void *list_lookup(const plsr_method_list *ml, const char *sel)
{
    uint32_t i;
    if (!ml)
        return NULL;
    for (i = 0; i < ml->count; i++) {
        const uint8_t *e = list_entry(ml, i);
        const char *name = entry_name(ml, e);
        if (name && !strcmp(name, sel))
            return entry_imp(ml, e);
    }
    return NULL;
}

static void *category_lookup(plsr_class *c, const char *sel, int class_methods)
{
    for (int i = 0; i < g_ncategories; i++) {
        plsr_class *target = g_categories[i].cls;
        if (class_methods) {
            /* Lookup started from a metaclass: its class's categories apply. */
            if (target && target->isa == c) {
                void *imp = list_lookup(g_categories[i].class_methods, sel);
                if (imp)
                    return imp;
            }
        } else if (target == c) {
            void *imp = list_lookup(g_categories[i].instance_methods, sel);
            if (imp)
                return imp;
        }
    }
    return NULL;
}

static void *lookup_chain(plsr_class *c, const char *sel)
{
    plsr_class *start = c;
    void *cached;
    void *found = NULL;
    if (!c || !sel)
        return NULL;
    if (method_cache_get(c, sel, &cached))
        return cached;
    for (int depth = 0; c && depth < 64; depth++, c = c->superclass) {
        const plsr_class_ro *ro;
        void *imp;
        if (!c->data)
            continue;
        ro = PLSR_RO(c);
        imp = list_lookup(ro->baseMethods, sel);
        if (!imp)
            imp = category_lookup(c, sel, (ro->flags & PLSR_RO_META) != 0);
        if (imp) {
            found = imp;
            break;
        }
    }
    method_cache_put(start, sel, found);
    return found;
}

/* CoreFoundation's constant strings are not part of our Foundation shim.
 * Their layout is { isa, const char *cstr, uint32_t len }, so provide the
 * small NSString surface they need before falling back to a stub. */
void *plsr_objc_lookup_imp(void *obj, const char *sel);

static const char *cfstr_utf8(void *self, const char *sel)
{
    (void)sel;
    return *(const char **)((char *)self + 16);
}
static unsigned long cfstr_length(void *self, const char *sel)
{
    (void)sel;
    return *(const uint32_t *)((char *)self + 24);
}
static void *cfstr_self(void *self, const char *sel)
{
    (void)sel;
    return self;
}
static int cfstr_equal(void *self, const char *sel, void *other)
{
    const char *a = cfstr_utf8(self, sel);
    const char *b;
    if (self == other)
        return 1;
    if (!other)
        return 0;
    b = plsr_objc_lookup_imp(other, "UTF8String");
    return a && b && !strcmp(a, b);
}
static unsigned long cfstr_hash(void *self, const char *sel)
{
    const char *s = cfstr_utf8(self, sel);
    unsigned long h = 5381;
    if (s)
        while (*s)
            h = h * 33 + (unsigned char)*s++;
    return h;
}

typedef struct {
    unsigned long location;
    unsigned long length;
} cfstr_range;

static const char *cfstr_object_utf8(void *obj)
{
    const char *(*utf8)(void *, const char *);
    if (!obj)
        return NULL;
    utf8 = (const char *(*)(void *, const char *))
        plsr_objc_lookup_imp(obj, "UTF8String");
    return utf8 ? utf8(obj, "UTF8String") : NULL;
}

static void *cfstr_new(void *self, const char *buf, size_t len)
{
    void *out = calloc(1, 32);
    char *copy;
    if (!out)
        return NULL;
    copy = malloc(len + 1);
    if (!copy) {
        free(out);
        return NULL;
    }
    memcpy(copy, buf ? buf : "", len);
    copy[len] = 0;
    *(void **)out = *(void **)self;
    *(const char **)((char *)out + 16) = copy;
    *(uint32_t *)((char *)out + 24) = (uint32_t)len;
    return out;
}

static void *cfstr_string_by_appending(void *self, const char *sel, void *other)
{
    const char *a = cfstr_utf8(self, sel);
    const char *b = cfstr_object_utf8(other);
    size_t alen = a ? strlen(a) : 0;
    size_t blen = b ? strlen(b) : 0;
    char *buf = malloc(alen + blen + 1);
    void *out;
    if (!buf)
        return NULL;
    if (alen)
        memcpy(buf, a, alen);
    if (blen)
        memcpy(buf + alen, b, blen);
    buf[alen + blen] = 0;
    out = cfstr_new(self, buf, alen + blen);
    free(buf);
    return out;
}

static cfstr_range cfstr_range_of_string(void *self, const char *sel, void *other)
{
    const char *a = cfstr_utf8(self, sel);
    const char *b = cfstr_object_utf8(other);
    cfstr_range r = { (unsigned long)-1, 0 };
    const char *hit;
    if (!a || !b)
        return r;
    hit = strstr(a, b);
    if (hit) {
        r.location = (unsigned long)(hit - a);
        r.length = strlen(b);
    }
    return r;
}

static cfstr_range cfstr_range_of_string_options(void *self, const char *sel,
                                                 void *other, unsigned long options)
{
    (void)options;
    return cfstr_range_of_string(self, sel, other);
}

static int cfstr_has_prefix(void *self, const char *sel, void *other)
{
    const char *a = cfstr_utf8(self, sel);
    const char *b = cfstr_object_utf8(other);
    size_t n;
    if (!a || !b)
        return 0;
    n = strlen(b);
    return !strncmp(a, b, n);
}

static int cfstr_has_suffix(void *self, const char *sel, void *other)
{
    const char *a = cfstr_utf8(self, sel);
    const char *b = cfstr_object_utf8(other);
    size_t an, bn;
    if (!a || !b)
        return 0;
    an = strlen(a);
    bn = strlen(b);
    return an >= bn && !memcmp(a + an - bn, b, bn);
}

static unsigned short cfstr_character_at(void *self, const char *sel,
                                         unsigned long index)
{
    const char *a = cfstr_utf8(self, sel);
    size_t n = a ? strlen(a) : 0;
    return index < n ? (unsigned char)a[index] : 0;
}

static void *cfstr_substring_from(void *self, const char *sel, unsigned long index)
{
    const char *a = cfstr_utf8(self, sel);
    size_t n = a ? strlen(a) : 0;
    if (!a || index > n)
        index = n;
    return cfstr_new(self, a + index, n - index);
}

static void *cfstr_substring_to(void *self, const char *sel, unsigned long index)
{
    const char *a = cfstr_utf8(self, sel);
    size_t n = a ? strlen(a) : 0;
    if (index > n)
        index = n;
    return cfstr_new(self, a ? a : "", index);
}

static void *cfstr_substring_range(void *self, const char *sel, cfstr_range range)
{
    const char *a = cfstr_utf8(self, sel);
    size_t n = a ? strlen(a) : 0;
    if (!a || range.location > n)
        return cfstr_new(self, "", 0);
    if (range.length > n - range.location)
        range.length = n - range.location;
    return cfstr_new(self, a + range.location, range.length);
}

static void *cfstr_lookup(const char *sel)
{
    if (!strcmp(sel, "UTF8String"))
        return (void *)cfstr_utf8;
    if (!strcmp(sel, "length"))
        return (void *)cfstr_length;
    if (!strcmp(sel, "description") || !strcmp(sel, "copy") ||
        !strcmp(sel, "retain") || !strcmp(sel, "autorelease"))
        return (void *)cfstr_self;
    if (!strcmp(sel, "isEqual:") || !strcmp(sel, "isEqualToString:"))
        return (void *)cfstr_equal;
    if (!strcmp(sel, "hash"))
        return (void *)cfstr_hash;
    if (!strcmp(sel, "stringByAppendingString:"))
        return (void *)cfstr_string_by_appending;
    if (!strcmp(sel, "rangeOfString:"))
        return (void *)cfstr_range_of_string;
    if (!strcmp(sel, "rangeOfString:options:"))
        return (void *)cfstr_range_of_string_options;
    if (!strcmp(sel, "hasPrefix:"))
        return (void *)cfstr_has_prefix;
    if (!strcmp(sel, "hasSuffix:"))
        return (void *)cfstr_has_suffix;
    if (!strcmp(sel, "characterAtIndex:"))
        return (void *)cfstr_character_at;
    if (!strcmp(sel, "substringFromIndex:"))
        return (void *)cfstr_substring_from;
    if (!strcmp(sel, "substringToIndex:"))
        return (void *)cfstr_substring_to;
    if (!strcmp(sel, "substringWithRange:"))
        return (void *)cfstr_substring_range;
    return NULL;
}

static void *plsr_objc_nil_imp(void *self, const char *sel, ...)
{
    (void)self;
    (void)sel;
    return NULL;
}

static void *plsr_objc_class_imp(void *self, const char *sel)
{
    plsr_class *c = *(plsr_class **)self;
    (void)sel;
    if (c && c->data && (PLSR_RO(c)->flags & PLSR_RO_META))
        return self;
    return c;
}

static void *plsr_objc_self_imp(void *self, const char *sel)
{
    (void)sel;
    return self;
}

static int plsr_objc_is_kind_imp(void *self, const char *sel, void *cls)
{
    plsr_class *c = *(plsr_class **)self;
    (void)sel;
    while (c) {
        if ((void *)c == cls)
            return 1;
        c = c->superclass;
    }
    return 0;
}

static int plsr_objc_responds_imp(void *self, const char *sel, const char *which)
{
    (void)sel;
    return which && plsr_objc_lookup_imp(self, which) != NULL;
}

static int plsr_objc_is_subclass_imp(void *self, const char *sel, void *cls)
{
    plsr_class *c = self;
    (void)sel;
    for (int depth = 0; c && depth < 64; depth++, c = c->superclass)
        if ((void *)c == cls)
            return 1;
    return 0;
}

static void *plsr_objc_stub_imp(void *self, const char *sel, ...)
{
    (void)self;
    (void)sel;
    return NULL;
}

plsr_class *objc_getClass(const char *name) __asm__("_objc_getClass");

typedef void *(*plsr_override_fn)(const char *name, const char *sel, int class_method);
static plsr_override_fn plsr_override;
static int plsr_override_tried;

static void *foundation_override(const char *name, const char *sel, int class_method)
{
    if (!plsr_override_tried) {
        plsr_override = (plsr_override_fn)dlsym(RTLD_DEFAULT, "plsr_foundation_override");
        plsr_override_tried = 1;
    }
    return plsr_override ? plsr_override(name, sel, class_method) : NULL;
}

void *plsr_objc_lookup_imp(void *obj, const char *sel)
{
    void *imp;
    if (!sel)
        return NULL;
    /* Swift's AppKit overlay can reach +sharedApplication through a classref
     * that the manual loader has not rebound yet. Recover the class by name
     * instead of treating the send as a nil message. */
    if (!obj && !strcmp(sel, "sharedApplication"))
        obj = objc_getClass("NSApplication");
    if (!obj)
        return (void *)plsr_objc_nil_imp;
    {
        plsr_class *rc = *(plsr_class **)obj;
        int meta = rc && rc->data && (PLSR_RO(rc)->flags & PLSR_RO_META);
        imp = foundation_override(class_name(rc), sel, meta);
        if (!imp)
            imp = lookup_chain(rc, sel);
    }
    if (!imp) {
        plsr_class *c = *(plsr_class **)obj;
        if (!strcmp(sel, "class") || !strcmp(sel, "classForCoder"))
            imp = (void *)plsr_objc_class_imp;
        else if (!strcmp(sel, "self"))
            imp = (void *)plsr_objc_self_imp;
        else if (!strcmp(sel, "awakeAfterUsingCoder:"))
            imp = (void *)plsr_objc_self_imp;
        else if (!strcmp(sel, "isKindOfClass:"))
            imp = (void *)plsr_objc_is_kind_imp;
        else if (!strcmp(sel, "isSubclassOfClass:"))
            imp = (void *)plsr_objc_is_subclass_imp;
        else if (!strcmp(sel, "respondsToSelector:"))
            imp = (void *)plsr_objc_responds_imp;
        else if (!strcmp(sel, "willChangeValueForKey:") ||
                 !strcmp(sel, "didChangeValueForKey:") ||
                 !strncmp(sel, "_reef", 5))
            imp = (void *)plsr_objc_nil_imp;
        else if (c && !strcmp(class_name(c), "__NSCFConstantString"))
            imp = cfstr_lookup(sel);
        if (!imp && c && c->data && (PLSR_RO(c)->flags & PLSR_RO_STUB))
            imp = (void *)plsr_objc_stub_imp;
    }
    if (getenv("PLSR_OBJC_TRACE"))
        dprintf(2, "objc: send %-30s -> %p (class %s, obj %p)\n",
                sel, imp, class_name(*(plsr_class **)obj), obj);
    return imp;
}

/* objc_super { id receiver; Class currentClass; }: start above currentClass. */
void *plsr_objc_lookup_super(void *super_ptr, const char *sel)
{
    struct objc_super_local { void *receiver; plsr_class *cls; };
    struct objc_super_local *s = (struct objc_super_local *)super_ptr;
    if (!s || !s->cls || !sel)
        return NULL;
    return lookup_chain(s->cls->superclass, sel);
}

void plsr_objc_unrecognized(void *obj, const char *sel)
{
    const char *cls = "";
    if (obj && *(void **)obj)
        cls = class_name(*(plsr_class **)obj);
    dprintf(2, "objc: unrecognized selector %s to %s (%p)\n",
            sel ? sel : "(null)", cls, obj);
    if (getenv("PLSR_OBJC_TRACE")) {
        void *bt[32];
        int n = backtrace(bt, 32);
        for (int i = 0; i < n; i++) {
            Dl_info di;
            if (dladdr(bt[i], &di) && di.dli_fname) {
                dprintf(2, "  bt[%d] %s+0x%lx\n", i, di.dli_fname,
                        (unsigned long)((char *)bt[i] - (char *)di.dli_fbase));
            } else {
                FILE *maps = fopen("/proc/self/maps", "r");
                char line[512];
                unsigned long lo, hi;
                int found = 0;
                while (maps && fgets(line, sizeof line, maps)) {
                    if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 &&
                        (unsigned long)bt[i] >= lo && (unsigned long)bt[i] < hi) {
                        char *path = strchr(line, '/');
                        dprintf(2, "  bt[%d] %p %s+0x%lx\n", i, bt[i],
                                path ? path : "(anonymous)",
                                (unsigned long)bt[i] - lo);
                        found = 1;
                        break;
                    }
                }
                if (maps)
                    fclose(maps);
                if (!found)
                    dprintf(2, "  bt[%d] %p (unmapped)\n", i, bt[i]);
            }
        }
    }
    _exit(127);
}

/* ---- public class/object API ---- */

plsr_class *objc_getClass(const char *name) __asm__("_objc_getClass");
plsr_class *objc_getClass(const char *name)
{
    if (!name)
        return NULL;
    for (int i = 0; i < g_nclasses; i++)
        if (!strcmp(class_name(g_classes[i]), name))
            return g_classes[i];
    return NULL;
}

plsr_class *objc_lookUpClass(const char *name) __asm__("_objc_lookUpClass");
plsr_class *objc_lookUpClass(const char *name) { return objc_getClass(name); }

plsr_class *objc_getMetaClass(const char *name) __asm__("_objc_getMetaClass");
plsr_class *objc_getMetaClass(const char *name)
{
    plsr_class *c = objc_getClass(name);
    return c ? c->isa : NULL;
}

void *object_getClass(void *obj) __asm__("_object_getClass");
void *object_getClass(void *obj) { return obj ? *(void **)obj : NULL; }

void *object_setClass(void *obj, void *cls) __asm__("_object_setClass");
void *object_setClass(void *obj, void *cls)
{
    if (obj)
        *(void **)obj = cls;
    return cls;
}

const char *class_getName(void *cls) __asm__("_class_getName");
const char *class_getName(void *cls) { return class_name((plsr_class *)cls); }

const char *object_getClassName(void *obj) __asm__("_object_getClassName");
const char *object_getClassName(void *obj)
{
    return obj ? class_name(*(plsr_class **)obj) : "nil";
}

void *class_getSuperclass(void *cls) __asm__("_class_getSuperclass");
void *class_getSuperclass(void *cls)
{
    return cls ? ((plsr_class *)cls)->superclass : NULL;
}

static unsigned long instance_size_for(plsr_class *c, int depth)
{
    const plsr_class_ro *ro;
    unsigned long size, super_size, extra;
    if (!c || !c->data || depth > 64)
        return 0;
    ro = PLSR_RO(c);
    size = ro->instanceSize;
    super_size = instance_size_for(c->superclass, depth + 1);
    if (super_size && size <= super_size) {
        /* Swift class metadata records the class's own field span rather
         * than the full inherited object size.  The fields start at
         * instanceStart, so add that span after the superclass object. */
        extra = size > ro->instanceStart ? size - ro->instanceStart : 0;
        if (!extra)
            extra = sizeof(void *);
        size = super_size + extra;
    }
    if (size < 2 * sizeof(void *))
        size = 2 * sizeof(void *);
    return (size + sizeof(void *) - 1) & ~(sizeof(void *) - 1);
}

unsigned long class_getInstanceSize(void *cls) __asm__("_class_getInstanceSize");
unsigned long class_getInstanceSize(void *cls)
{
    return instance_size_for((plsr_class *)cls, 0);
}

void *class_getMethodImplementation(void *cls, const char *sel)
    __asm__("_class_getMethodImplementation");
void *class_getMethodImplementation(void *cls, const char *sel)
{
    return lookup_chain((plsr_class *)cls, sel);
}

int object_isClass(void *obj) __asm__("_object_isClass");
int object_isClass(void *obj)
{
    plsr_class *c;
    if (!obj || !(c = *(plsr_class **)obj) || !c->data)
        return 0;
    return (PLSR_RO(c)->flags & PLSR_RO_META) != 0;
}

void *object_getIndexedIvars(void *obj) __asm__("_object_getIndexedIvars");
void *object_getIndexedIvars(void *obj)
{
    plsr_class *c = obj ? *(plsr_class **)obj : NULL;
    unsigned long size = class_getInstanceSize(c);
    return (char *)obj + size;
}

void *object_getIvar(void *obj, void *ivar) __asm__("_object_getIvar");
void *object_getIvar(void *obj, void *ivar) { (void)obj; (void)ivar; return NULL; }

void object_setIvar(void *obj, void *ivar, void *value) __asm__("_object_setIvar");
void object_setIvar(void *obj, void *ivar, void *value)
{
    (void)obj; (void)ivar; (void)value;
}

const char *sel_getName(const char *sel) __asm__("_sel_getName");
const char *sel_getName(const char *sel) { return sel ? sel : ""; }

const char *sel_registerName(const char *sel) __asm__("_sel_registerName");
const char *sel_registerName(const char *sel) { return sel; }

int sel_isEqual(const char *a, const char *b) __asm__("_sel_isEqual");
int sel_isEqual(const char *a, const char *b)
{
    return a == b || (a && b && !strcmp(a, b));
}

/* ---- properties ---- */

void *objc_getProperty(void *self, const char *sel, long offset, int atomic)
    __asm__("_objc_getProperty");
void *objc_getProperty(void *self, const char *sel, long offset, int atomic)
{
    (void)sel; (void)atomic;
    return *(void **)((char *)self + offset);
}

void objc_setProperty(void *self, const char *sel, long offset, void *value,
                      int atomic, signed char should_copy)
    __asm__("_objc_setProperty");
void objc_setProperty(void *self, const char *sel, long offset, void *value,
                      int atomic, signed char should_copy)
{
    (void)sel; (void)atomic; (void)should_copy;
    *(void **)((char *)self + offset) = value;
}

void objc_setProperty_nonatomic(void *self, const char *sel, void *value, long offset)
    __asm__("_objc_setProperty_nonatomic");
void objc_setProperty_nonatomic(void *self, const char *sel, void *value, long offset)
{
    (void)sel;
    *(void **)((char *)self + offset) = value;
}

void objc_setProperty_atomic(void *self, const char *sel, void *value, long offset)
    __asm__("_objc_setProperty_atomic");
void objc_setProperty_atomic(void *self, const char *sel, void *value, long offset)
{
    (void)sel;
    *(void **)((char *)self + offset) = value;
}

void objc_setProperty_nonatomic_copy(void *self, const char *sel, void *value, long offset)
    __asm__("_objc_setProperty_nonatomic_copy");
void objc_setProperty_nonatomic_copy(void *self, const char *sel, void *value, long offset)
{
    (void)sel;
    *(void **)((char *)self + offset) = value;
}

void objc_setProperty_atomic_copy(void *self, const char *sel, void *value, long offset)
    __asm__("_objc_setProperty_atomic_copy");
void objc_setProperty_atomic_copy(void *self, const char *sel, void *value, long offset)
{
    (void)sel;
    *(void **)((char *)self + offset) = value;
}

/* ---- memory management (no ARC optimizer support yet) ---- */

void *objc_retain(void *obj) __asm__("_objc_retain");
void *objc_retain(void *obj) { return obj; }

void objc_release(void *obj) __asm__("_objc_release");
void objc_release(void *obj) { (void)obj; }

void objc_autorelease(void *obj) __asm__("_objc_autorelease");
void objc_autorelease(void *obj) { (void)obj; }

void *objc_retainAutorelease(void *obj) __asm__("_objc_retainAutorelease");
void *objc_retainAutorelease(void *obj) { return obj; }

void *objc_retainAutoreleasedReturnValue(void *obj)
    __asm__("_objc_retainAutoreleasedReturnValue");
void *objc_retainAutoreleasedReturnValue(void *obj) { return obj; }

void *objc_autoreleaseReturnValue(void *obj)
    __asm__("_objc_autoreleaseReturnValue");
void *objc_autoreleaseReturnValue(void *obj) { return obj; }

void *objc_retainAutoreleaseReturnValue(void *obj)
    __asm__("_objc_retainAutoreleaseReturnValue");
void *objc_retainAutoreleaseReturnValue(void *obj) { return obj; }

void *objc_claimAutoreleasedReturnValue(void *obj)
    __asm__("_objc_claimAutoreleasedReturnValue");
void *objc_claimAutoreleasedReturnValue(void *obj) { return obj; }

void *objc_unsafeClaimAutoreleasedReturnValue(void *obj)
    __asm__("_objc_unsafeClaimAutoreleasedReturnValue");
void *objc_unsafeClaimAutoreleasedReturnValue(void *obj) { return obj; }

void *objc_retainBlock(void *block) __asm__("_objc_retainBlock");
void *objc_retainBlock(void *block) { return block; }

void *objc_autoreleasePoolPush(void) __asm__("_objc_autoreleasePoolPush");
void *objc_autoreleasePoolPush(void) { return NULL; }

void objc_autoreleasePoolPop(void *pool) __asm__("_objc_autoreleasePoolPop");
void objc_autoreleasePoolPop(void *pool) { (void)pool; }

void objc_storeStrong(void **slot, void *obj) __asm__("_objc_storeStrong");
void objc_storeStrong(void **slot, void *obj)
{
    if (slot)
        *slot = obj;
}

int objc_sync_enter(void *obj) __asm__("_objc_sync_enter");
int objc_sync_enter(void *obj) { (void)obj; return 0; }

int objc_sync_exit(void *obj) __asm__("_objc_sync_exit");
int objc_sync_exit(void *obj) { (void)obj; return 0; }

void objc_enumerationMutation(void *obj) __asm__("_objc_enumerationMutation");
void objc_enumerationMutation(void *obj) { (void)obj; }

/* ---- allocation entry points ---- */

void *objc_alloc(void *cls, const char *sel) __asm__("_objc_alloc");
void *objc_alloc(void *cls, const char *sel)
{
    void *(*alloc)(void *, const char *) =
        (void *(*)(void *, const char *))plsr_objc_lookup_imp(cls, "alloc");
    return alloc ? alloc(cls, sel) : NULL;
}

void *objc_allocWithZone(void *cls, const char *sel) __asm__("_objc_allocWithZone");
void *objc_allocWithZone(void *cls, const char *sel) { return objc_alloc(cls, sel); }

void *objc_alloc_init(void *cls, const char *sel) __asm__("_objc_alloc_init");
void *objc_alloc_init(void *cls, const char *sel)
{
    void *obj = objc_alloc(cls, sel);
    void *(*init)(void *, const char *) =
        (void *(*)(void *, const char *))plsr_objc_lookup_imp(obj, "init");
    return init ? init(obj, sel) : obj;
}

void *objc_opt_new(void *cls) __asm__("_objc_opt_new");
void *objc_opt_new(void *cls)
{
    void *(*alloc)(void *, const char *);
    void *(*init)(void *, const char *);
    void *obj;
    alloc = (void *(*)(void *, const char *))plsr_objc_lookup_imp(cls, "alloc");
    if (!alloc)
        return NULL;
    obj = alloc(cls, "alloc");
    init = (void *(*)(void *, const char *))plsr_objc_lookup_imp(obj, "init");
    return init ? init(obj, "init") : obj;
}

void *objc_opt_class(void *obj) __asm__("_objc_opt_class");
void *objc_opt_class(void *obj) { return object_getClass(obj); }

void *objc_opt_self(void *obj) __asm__("_objc_opt_self");
void *objc_opt_self(void *obj) { return obj; }

int objc_opt_isKindOfClass(void *obj, void *cls)
    __asm__("_objc_opt_isKindOfClass");
int objc_opt_isKindOfClass(void *obj, void *cls)
{
    plsr_class *c = obj ? *(plsr_class **)obj : NULL;
    for (int depth = 0; c && depth < 64; depth++, c = c->superclass)
        if (c == (plsr_class *)cls)
            return 1;
    return 0;
}

int objc_opt_respondsToSelector(void *obj, const char *sel)
    __asm__("_objc_opt_respondsToSelector");
int objc_opt_respondsToSelector(void *obj, const char *sel)
{
    return plsr_objc_lookup_imp(obj, sel) != NULL;
}

/* ---- Mach-O metadata scan ---- */

static void scan_classes(const uint8_t *mh, uint64_t addr, uint64_t size, uint64_t base_va)
{
    plsr_class **list = (plsr_class **)(mh + (addr - base_va));
    if (getenv("PLSR_OBJC_DEBUG"))
        dprintf(2, "objc: classlist at %p size %llu\n",
                (void *)list, (unsigned long long)size);
    for (uint64_t i = 0; i < size / 8; i++)
        if (list[i])
            plsr_objc_register_class(list[i]);
}

static void scan_categories(const uint8_t *mh, uint64_t addr, uint64_t size, uint64_t base_va)
{
    uint64_t *list = (uint64_t *)(mh + (addr - base_va));
    if (getenv("PLSR_OBJC_DEBUG"))
        dprintf(2, "objc: catlist at %p size %llu\n",
                (void *)list, (unsigned long long)size);
    for (uint64_t i = 0; i < size / 8; i++) {
        uint64_t *cat = (uint64_t *)(uintptr_t)list[i];
        plsr_class *cls;
        plsr_method_list *inst, *cm;
        if (!cat)
            continue;
        cls = (plsr_class *)(uintptr_t)cat[1];
        inst = (plsr_method_list *)(uintptr_t)cat[2];
        cm = (plsr_method_list *)(uintptr_t)cat[3];
        plsr_objc_register_category(cls, inst, cm);
    }
}

void plsr_objc_scan_image(void *image)
{
    const uint8_t *mh = (const uint8_t *)image;
    uint32_t ncmds, sizeofcmds;
    const uint8_t *p, *end;
    uint64_t base_va = 0;
    int have_base = 0;

    if (!mh || mh[0] != 0xcf || mh[1] != 0xfa || mh[2] != 0xed || mh[3] != 0xfe)
        return;
    if (getenv("PLSR_OBJC_DEBUG"))
        dprintf(2, "objc: scan image %p\n", image);
    memcpy(&ncmds, mh + 16, 4);
    memcpy(&sizeofcmds, mh + 20, 4);
    p = mh + 32;
    end = p + sizeofcmds;

    for (uint32_t i = 0; i < ncmds && p + 8 <= end; i++) {
        uint32_t cmd, cmdsize;
        memcpy(&cmd, p, 4);
        memcpy(&cmdsize, p + 4, 4);
        if (cmdsize < 8 || p + cmdsize > end)
            break;
        if (cmd == 0x19 && cmdsize >= 72) {
            uint64_t vmaddr;
            uint32_t fileoff, filesize;
            memcpy(&vmaddr, p + 24, 8);
            memcpy(&fileoff, p + 40, 4);
            memcpy(&filesize, p + 48, 4);
            /* __PAGEZERO also has fileoff 0 but no file data; using it as
             * the image base makes every section address absolute. */
            if (fileoff == 0 && filesize > 0 && !have_base) {
                base_va = vmaddr;
                have_base = 1;
            }
        }
        p += cmdsize;
    }
    if (!have_base)
        return;

    p = mh + 32;
    for (uint32_t i = 0; i < ncmds && p + 8 <= end; i++) {
        uint32_t cmd, cmdsize;
        memcpy(&cmd, p, 4);
        memcpy(&cmdsize, p + 4, 4);
        if (cmdsize < 8 || p + cmdsize > end)
            return;
        if (cmd == 0x19 && cmdsize >= 72) {
            uint32_t nsects;
            const uint8_t *sect = p + 72;
            memcpy(&nsects, p + 64, 4);
            if (72u + (uint64_t)nsects * 80u > cmdsize)
                return;
            for (uint32_t k = 0; k < nsects; k++, sect += 80) {
                uint64_t addr, size;
                if (!memcmp(sect, "__objc_classlist", 16)) {
                    memcpy(&addr, sect + 32, 8);
                    memcpy(&size, sect + 40, 8);
                    size &= ~7ull;
                    if (addr >= base_va && size <= (1ull << 26))
                        scan_classes(mh, addr, size, base_va);
                } else if (!memcmp(sect, "__objc_catlist", 14)) {
                    memcpy(&addr, sect + 32, 8);
                    memcpy(&size, sect + 40, 8);
                    if (addr >= base_va)
                        scan_categories(mh, addr, size, base_va);
                }
            }
        }
        p += cmdsize;
    }
}

/* ---- extended introspection, associations, weak refs, exceptions ---- */

struct plsr_method_out {
    const char *name;
    const char *types;
    void *imp;
};

static struct plsr_method_out g_method_result;

void *class_getInstanceMethod(void *cls, const char *sel)
    __asm__("_class_getInstanceMethod");
void *class_getInstanceMethod(void *cls, const char *sel)
{
    void *imp = lookup_chain((plsr_class *)cls, sel);
    if (!imp)
        return NULL;
    g_method_result.name = sel;
    g_method_result.types = NULL;
    g_method_result.imp = imp;
    return &g_method_result;
}

void *class_getClassMethod(void *cls, const char *sel)
    __asm__("_class_getClassMethod");
void *class_getClassMethod(void *cls, const char *sel)
{
    plsr_class *c = cls;
    return c ? class_getInstanceMethod(c->isa, sel) : NULL;
}

int class_respondsToSelector(void *cls, const char *sel)
    __asm__("_class_respondsToSelector");
int class_respondsToSelector(void *cls, const char *sel)
{
    return lookup_chain((plsr_class *)cls, sel) != NULL;
}

int class_isMetaClass(void *cls) __asm__("_class_isMetaClass");
int class_isMetaClass(void *cls)
{
    if (!cls || !((plsr_class *)cls)->data)
        return 0;
    return (PLSR_RO((plsr_class *)cls)->flags & PLSR_RO_META) != 0;
}

int class_conformsToProtocol(void *cls, void *proto)
    __asm__("_class_conformsToProtocol");
int class_conformsToProtocol(void *cls, void *proto)
{
    (void)cls;
    (void)proto;
    return 0;
}

int class_addMethod(void *cls, const char *sel, void *imp, const char *types)
    __asm__("_class_addMethod");
int class_addMethod(void *cls, const char *sel, void *imp, const char *types)
{
    (void)cls;
    (void)sel;
    (void)imp;
    (void)types;
    return 0;
}

int class_addMethodsBulk(void *cls, const void *methods, unsigned int count,
                         const void *buffer)
    __asm__("_class_addMethodsBulk");
int class_addMethodsBulk(void *cls, const void *methods, unsigned int count,
                         const void *buffer)
{
    (void)cls;
    (void)methods;
    (void)count;
    (void)buffer;
    return 0;
}

int class_addProtocol(void *cls, void *proto) __asm__("_class_addProtocol");
int class_addProtocol(void *cls, void *proto)
{
    (void)cls;
    (void)proto;
    return 0;
}

void *class_copyMethodList(void *cls, unsigned int *outCount)
    __asm__("_class_copyMethodList");
void *class_copyMethodList(void *cls, unsigned int *outCount)
{
    plsr_class *c = cls;
    const plsr_class_ro *ro;
    plsr_method_list *ml;
    char *block;
    struct plsr_method_out **out;
    struct plsr_method_out *methods;
    if (outCount)
        *outCount = 0;
    if (!c || !c->data)
        return NULL;
    ro = PLSR_RO(c);
    ml = ro->baseMethods;
    if (!ml || !ml->count)
        return NULL;
    block = calloc(ml->count, sizeof(void *) + sizeof(*methods));
    if (!block)
        return NULL;
    out = (struct plsr_method_out **)block;
    methods = (struct plsr_method_out *)(block + ml->count * sizeof(void *));
    for (uint32_t i = 0; i < ml->count; i++) {
        const uint8_t *e = list_entry(ml, i);
        methods[i].name = entry_name(ml, e);
        methods[i].types = NULL;
        methods[i].imp = entry_imp(ml, e);
        out[i] = &methods[i];
    }
    if (outCount)
        *outCount = ml->count;
    return out;
}

void *class_copyIvarList(void *cls, unsigned int *outCount)
    __asm__("_class_copyIvarList");
void *class_copyIvarList(void *cls, unsigned int *outCount)
{
    (void)cls;
    if (outCount)
        *outCount = 0;
    return NULL;
}

void *class_copyPropertyList(void *cls, unsigned int *outCount)
    __asm__("_class_copyPropertyList");
void *class_copyPropertyList(void *cls, unsigned int *outCount)
{
    (void)cls;
    if (outCount)
        *outCount = 0;
    return NULL;
}

void *class_getInstanceVariable(void *cls, const char *name)
    __asm__("_class_getInstanceVariable");
void *class_getInstanceVariable(void *cls, const char *name)
{
    (void)cls;
    (void)name;
    return NULL;
}

long ivar_getOffset(void *ivar) __asm__("_ivar_getOffset");
long ivar_getOffset(void *ivar) { (void)ivar; return 0; }
const char *ivar_getTypeEncoding(void *ivar) __asm__("_ivar_getTypeEncoding");
const char *ivar_getTypeEncoding(void *ivar) { (void)ivar; return NULL; }
const char *property_getName(void *prop) __asm__("_property_getName");
const char *property_getName(void *prop) { (void)prop; return NULL; }
const char *property_getAttributes(void *prop) __asm__("_property_getAttributes");
const char *property_getAttributes(void *prop) { (void)prop; return NULL; }
const char *protocol_getName(void *proto) __asm__("_protocol_getName");
const char *protocol_getName(void *proto) { (void)proto; return NULL; }
int protocol_conformsToProtocol(void *a, void *b)
    __asm__("_protocol_conformsToProtocol");
int protocol_conformsToProtocol(void *a, void *b)
{
    (void)a;
    (void)b;
    return 0;
}
void *objc_getProtocol(const char *name) __asm__("_objc_getProtocol");
void *objc_getProtocol(const char *name) { (void)name; return NULL; }

void *class_createInstance(void *cls, size_t extraBytes)
    __asm__("_class_createInstance");
void *class_createInstance(void *cls, size_t extraBytes)
{
    unsigned long size = class_getInstanceSize(cls);
    void *obj = calloc(1, size + extraBytes);
    if (obj)
        *(void **)obj = cls;
    return obj;
}

void *class_setSuperclass(void *cls, void *newSuper)
    __asm__("_class_setSuperclass");
void *class_setSuperclass(void *cls, void *newSuper)
{
    (void)cls;
    (void)newSuper;
    return NULL;
}

const char *class_getImageName(void *cls) __asm__("_class_getImageName");
const char *class_getImageName(void *cls) { (void)cls; return NULL; }

void *objc_allocateClassPair(void *superclass, const char *name, size_t extra)
    __asm__("_objc_allocateClassPair");
void *objc_allocateClassPair(void *superclass, const char *name, size_t extra)
{
    (void)superclass;
    (void)name;
    (void)extra;
    return NULL;
}

void objc_registerClassPair(void *cls) __asm__("_objc_registerClassPair");
void objc_registerClassPair(void *cls) { (void)cls; }

void *objc_readClassPair(void *cls, const void *info)
    __asm__("_objc_readClassPair");
void *objc_readClassPair(void *cls, const void *info)
{
    (void)cls;
    (void)info;
    return NULL;
}

void *objc_constructInstance(void *cls, void *bytes)
    __asm__("_objc_constructInstance");
void *objc_constructInstance(void *cls, void *bytes)
{
    if (bytes)
        *(void **)bytes = cls;
    return bytes;
}

void *objc_destructInstance(void *obj) __asm__("_objc_destructInstance");
void *objc_destructInstance(void *obj) { (void)obj; return obj; }

void object_dispose(void *obj) __asm__("_object_dispose");
void object_dispose(void *obj) { free(obj); }

const char *method_getName(void *m) __asm__("_method_getName");
const char *method_getName(void *m)
{
    return m ? ((struct plsr_method_out *)m)->name : NULL;
}
const char *method_getTypeEncoding(void *m) __asm__("_method_getTypeEncoding");
const char *method_getTypeEncoding(void *m)
{
    return m ? ((struct plsr_method_out *)m)->types : NULL;
}
void *method_getImplementation(void *m) __asm__("_method_getImplementation");
void *method_getImplementation(void *m)
{
    return m ? ((struct plsr_method_out *)m)->imp : NULL;
}
void *method_setImplementation(void *m, void *imp)
    __asm__("_method_setImplementation");
void *method_setImplementation(void *m, void *imp)
{
    struct plsr_method_out *out = m;
    void *old = out ? out->imp : NULL;
    if (out)
        out->imp = imp;
    return old;
}
void method_exchangeImplementations(void *a, void *b)
    __asm__("_method_exchangeImplementations");
void method_exchangeImplementations(void *a, void *b)
{
    struct plsr_method_out *x = a, *y = b;
    void *tmp;
    if (!x || !y)
        return;
    tmp = x->imp;
    x->imp = y->imp;
    y->imp = tmp;
}

void objc_copyStruct(void *dest, const void *src, ptrdiff_t size, int atomic,
                     int hasStrong) __asm__("_objc_copyStruct");
void objc_copyStruct(void *dest, const void *src, ptrdiff_t size, int atomic,
                     int hasStrong)
{
    (void)atomic;
    (void)hasStrong;
    memcpy(dest, src, (size_t)size);
}

/* Associations are a simple linked list; policies do not retain. */
struct plsr_assoc {
    void *obj;
    const void *key;
    void *value;
    struct plsr_assoc *next;
};
static struct plsr_assoc *g_assocs;
static pthread_mutex_t g_assoc_lock = PTHREAD_MUTEX_INITIALIZER;

void objc_setAssociatedObject(void *obj, const void *key, void *value,
                              unsigned long policy)
    __asm__("_objc_setAssociatedObject");
void objc_setAssociatedObject(void *obj, const void *key, void *value,
                              unsigned long policy)
{
    struct plsr_assoc *a;
    (void)policy;
    pthread_mutex_lock(&g_assoc_lock);
    for (a = g_assocs; a; a = a->next)
        if (a->obj == obj && a->key == key)
            break;
    if (!a) {
        a = calloc(1, sizeof(*a));
        if (a) {
            a->obj = obj;
            a->key = key;
            a->next = g_assocs;
            g_assocs = a;
        }
    }
    if (a)
        a->value = value;
    pthread_mutex_unlock(&g_assoc_lock);
}

void *objc_getAssociatedObject(void *obj, const void *key)
    __asm__("_objc_getAssociatedObject");
void *objc_getAssociatedObject(void *obj, const void *key)
{
    struct plsr_assoc *a;
    void *value = NULL;
    pthread_mutex_lock(&g_assoc_lock);
    for (a = g_assocs; a; a = a->next)
        if (a->obj == obj && a->key == key) {
            value = a->value;
            break;
        }
    pthread_mutex_unlock(&g_assoc_lock);
    return value;
}

void *objc_initWeak(void **location, void *obj) __asm__("_objc_initWeak");
void *objc_initWeak(void **location, void *obj)
{
    if (location)
        *location = obj;
    return obj;
}
void *objc_loadWeak(void **location) __asm__("_objc_loadWeak");
void *objc_loadWeak(void **location) { return location ? *location : NULL; }
void *objc_loadWeakRetained(void **location)
    __asm__("_objc_loadWeakRetained");
void *objc_loadWeakRetained(void **location)
{
    return location ? *location : NULL;
}
void *objc_storeWeak(void **location, void *obj) __asm__("_objc_storeWeak");
void *objc_storeWeak(void **location, void *obj)
{
    if (location)
        *location = obj;
    return obj;
}
void objc_copyWeak(void **dest, void **src) __asm__("_objc_copyWeak");
void objc_copyWeak(void **dest, void **src)
{
    if (dest)
        *dest = src ? *src : NULL;
}
void objc_moveWeak(void **dest, void **src) __asm__("_objc_moveWeak");
void objc_moveWeak(void **dest, void **src)
{
    if (dest)
        *dest = src ? *src : NULL;
    if (src)
        *src = NULL;
}
void objc_destroyWeak(void **location) __asm__("_objc_destroyWeak");
void objc_destroyWeak(void **location)
{
    if (location)
        *location = NULL;
}

void objc_exception_throw(void *exception) __asm__("_objc_exception_throw");
void objc_exception_throw(void *exception)
{
    dprintf(2, "objc: uncaught exception %p\n", exception);
    abort();
}
void *objc_begin_catch(void *exception) __asm__("_objc_begin_catch");
void *objc_begin_catch(void *exception) { return exception; }
void objc_end_catch(void) __asm__("_objc_end_catch");
void objc_end_catch(void) {}
void objc_exception_rethrow(void) __asm__("_objc_exception_rethrow");
void objc_exception_rethrow(void) { abort(); }
void objc_terminate(void) __asm__("_objc_terminate");
void objc_terminate(void) { abort(); }
int plsr_objc_personality(int version, int actions, uint64_t exclass,
                          void *ex, void *ctx)
    __asm__("___objc_personality_v0");
int plsr_objc_personality(int version, int actions, uint64_t exclass,
                          void *ex, void *ctx)
{
    (void)version;
    (void)actions;
    (void)exclass;
    (void)ex;
    (void)ctx;
    return 0;
}
char plsr_ehtype_vtable[64] __asm__("_objc_ehtype_vtable");

void *objc_rootAutorelease(void *obj) __asm__("__objc_rootAutorelease");
void *objc_rootAutorelease(void *obj) { return obj; }

void objc_setEnumerationMutationHandler(void (*h)(void *))
    __asm__("_objc_setEnumerationMutationHandler");
void objc_setEnumerationMutationHandler(void (*h)(void *)) { (void)h; }
void objc_setExceptionPreprocessor(void *fn)
    __asm__("_objc_setExceptionPreprocessor");
void objc_setExceptionPreprocessor(void *fn) { (void)fn; }
void objc_setForwardHandler(void *fn, void *arg) __asm__("_objc_setForwardHandler");
void objc_setForwardHandler(void *fn, void *arg)
{
    (void)fn;
    (void)arg;
}
void objc_setUncaughtExceptionHandler(void *fn)
    __asm__("_objc_setUncaughtExceptionHandler");
void objc_setUncaughtExceptionHandler(void *fn) { (void)fn; }
void objc_setHook_getClass(void *fn) __asm__("_objc_setHook_getClass");
void objc_setHook_getClass(void *fn) { (void)fn; }
void objc_setHook_getImageName(void *fn) __asm__("_objc_setHook_getImageName");
void objc_setHook_getImageName(void *fn) { (void)fn; }
void objc_setHook_lazyClassNamer(void *fn)
    __asm__("_objc_setHook_lazyClassNamer");
void objc_setHook_lazyClassNamer(void *fn) { (void)fn; }

__attribute__((visibility("default"), used))
const char *const plsr_host_imports[] = {
    "/usr/lib/libobjc.A.dylib",
    NULL
};
