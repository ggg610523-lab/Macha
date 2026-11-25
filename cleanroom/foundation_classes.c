/*
 * Clean-room Foundation class surface.
 *
 * Defines the class/metaclass symbols imported by a multi-image app closure
 * and implements the core classes on top of the small Foundation
 * slice in foundation.c. Long-tail classes exist with NSObject behaviour and
 * the runtime's tolerant stub fallback, so imports resolve and unknown
 * selectors return nil instead of aborting the process.
 */
#define _GNU_SOURCE
#include "objc_abi.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define EXTERN_CLASS(n)                                                       \
    extern plsr_class plsr_##n##_class __asm__("_OBJC_CLASS_$_" #n);          \
    extern plsr_class plsr_##n##_meta __asm__("_OBJC_METACLASS_$_" #n);
extern plsr_class plsr_NSObject_class __asm__("_OBJC_CLASS_$_NSObject");
extern plsr_class plsr_NSObject_meta __asm__("_OBJC_METACLASS_$_NSObject");
EXTERN_CLASS(NSString)
EXTERN_CLASS(NSMutableString)
EXTERN_CLASS(NSNumber)
EXTERN_CLASS(NSArray)
EXTERN_CLASS(NSMutableArray)
EXTERN_CLASS(NSDictionary)
EXTERN_CLASS(NSNull)
EXTERN_CLASS(NSData)

void plsr_objc_register_class(plsr_class *c);
void *plsr_objc_lookup_imp(void *obj, const char *sel);
void *objc_getClass(const char *name) __asm__("_objc_getClass");
const char *class_getName(void *cls) __asm__("_class_getName");
const char *sel_getName(const char *sel) __asm__("_sel_getName");

#define BASE_SIZE 128u

#define DECLARE_CLASS(n)                                                      \
    plsr_class plsr_##n##_class __asm__("_OBJC_CLASS_$_" #n);                 \
    plsr_class plsr_##n##_meta __asm__("_OBJC_METACLASS_$_" #n);

#define FOUNDATION_CLASSES(X)                                                 \
    X(NSAffineTransform) X(NSAppleEventDescriptor) X(NSAppleEventManager)     \
    X(NSAppleScript) X(NSAssertionHandler) X(NSAttributedString)              \
    X(NSBlockOperation) X(NSBundle) X(NSByteCountFormatter) X(NSCoder)        \
    X(NSCountedSet) X(NSDataDetector) X(NSDateComponents)                     \
    X(NSDateFormatter) X(NSDateInterval) X(NSDateIntervalFormatter)           \
    X(NSDecimalNumber) X(NSDimension) X(NSDistributedNotificationCenter)      \
    X(NSFileCoordinator) X(NSFileHandle) X(NSFileManager) X(NSFileSecurity)   \
    X(NSFileWrapper) X(NSFormatter) X(NSHashTable) X(NSHTTPCookie)            \
    X(NSHTTPURLResponse) X(NSIndexPath) X(NSIndexSet)                         \
    X(NSISO8601DateFormatter) X(NSItemProvider) X(NSJSONSerialization)        \
    X(NSKeyedArchiver) X(NSKeyedUnarchiver) X(NSLayoutConstraint) X(NSLock)   \
    X(NSMapTable) X(NSMeasurement)    X(NSMutableAttributedString) X(NSMutableDictionary)                       \
    X(NSMutableIndexSet) X(NSMutableOrderedSet) X(NSMutableURLRequest)        \
    X(NSNotification) X(NSNotificationCenter) X(NSNumberFormatter)            \
    X(NSOperation) X(NSOperationQueue) X(NSOrderedSet)                        \
    X(NSPersonNameComponents) X(NSPipe) X(NSPointerArray) X(NSProcessInfo)    \
    X(NSProgress) X(NSPropertyListSerialization) X(NSPropertySpecifier)       \
    X(NSProxy) X(NSRegularExpression) X(NSScanner) X(NSScriptCommand)         \
    X(NSTask) X(NSTextCheckingResult) X(NSThread) X(NSUndoManager)             \
    X(NSUnitConverterLinear) X(NSURLAuthenticationChallenge)                  \
    X(NSURLComponents) X(NSURLCredential) X(NSURLProtectionSpace)             \
    X(NSURLQueryItem) X(NSURLRequest) X(NSURLResponse) X(NSURLSession)        \
    X(NSURLSessionConfiguration) X(NSURLSessionWebSocketMessage)              \
    X(NSUserActivity) X(NSUserAppleScriptTask) X(NSUserUnixTask) X(NSUUID)    \
    X(NSXMLDocument) X(NSXPCConnection) X(NSXPCInterface)                     \
    X(OS_dispatch_data) X(OS_dispatch_queue) X(OS_dispatch_source)            \
    X(OS_os_log)

FOUNDATION_CLASSES(DECLARE_CLASS)

typedef struct { void *isa; } plsr_object;

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
    fn = (void *(*)(void *, const char *, void *, void *, void *))plsr_objc_lookup_imp(obj, sel);
    return fn ? fn(obj, sel, a, b, c) : NULL;
}

static void *send4(void *obj, const char *sel, void *a, void *b, void *c, void *d)
{
    void *(*fn)(void *, const char *, void *, void *, void *, void *);
    if (!obj)
        return NULL;
    fn = (void *(*)(void *, const char *, void *, void *, void *, void *))
        plsr_objc_lookup_imp(obj, sel);
    return fn ? fn(obj, sel, a, b, c, d) : NULL;
}

static double send_d(void *obj, const char *sel)
{
    double (*fn)(void *, const char *);
    if (!obj)
        return 0.0;
    fn = (double (*)(void *, const char *))plsr_objc_lookup_imp(obj, sel);
    return fn ? fn(obj, sel) : 0.0;
}

static double send_d1(void *obj, const char *sel, double a)
{
    double (*fn)(void *, const char *, double);
    if (!obj)
        return 0.0;
    fn = (double (*)(void *, const char *, double))plsr_objc_lookup_imp(obj, sel);
    return fn ? fn(obj, sel, a) : 0.0;
}

static void *send_dd(void *obj, const char *sel, double a, double b)
{
    void *(*fn)(void *, const char *, double, double);
    if (!obj)
        return NULL;
    fn = (void *(*)(void *, const char *, double, double))plsr_objc_lookup_imp(obj, sel);
    return fn ? fn(obj, sel, a, b) : NULL;
}

static void *alloc_obj(void *cls, size_t size)
{
    void *obj = calloc(1, size ? size : BASE_SIZE);
    if (obj)
        *(void **)obj = cls;
    return obj;
}

static const char *obj_utf8(void *obj)
{
    const char *s = send0(obj, "UTF8String");
    return s ? s : "";
}

static int class_name_is(void *obj, const char *name)
{
    const char *cn;
    if (!obj)
        return 0;
    cn = class_getName(*(void **)obj);
    return cn && !strcmp(cn, name);
}

static int is_string_obj(void *obj)
{
    const char *cn;
    if (!obj)
        return 0;
    cn = class_getName(*(void **)obj);
    return cn && strstr(cn, "String") != NULL;
}

static int is_number_obj(void *obj)
{
    const char *cn;
    if (!obj)
        return 0;
    cn = class_getName(*(void **)obj);
    return cn && strstr(cn, "Number") != NULL;
}

static int is_array_obj(void *obj)
{
    const char *cn;
    if (!obj)
        return 0;
    cn = class_getName(*(void **)obj);
    return cn && strstr(cn, "Array") != NULL;
}

static int is_dict_obj(void *obj)
{
    const char *cn;
    if (!obj)
        return 0;
    cn = class_getName(*(void **)obj);
    return cn && strstr(cn, "Dictionary") != NULL;
}

static void *nsstr(const char *s)
{
    return send1((void *)&plsr_NSString_class, "stringWithUTF8String:",
                 (void *)(s ? s : ""));
}

static void *nsnum(long long v)
{
    return send1((void *)&plsr_NSNumber_class, "numberWithLongLong:",
                 (void *)(intptr_t)v);
}

static void *nsbool(int v)
{
    return send1((void *)&plsr_NSNumber_class, "numberWithBool:",
                 (void *)(intptr_t)(v ? 1 : 0));
}

static void *send_obj_d1(void *obj, const char *sel, double a)
{
    void *(*fn)(void *, const char *, double);
    if (!obj)
        return NULL;
    fn = (void *(*)(void *, const char *, double))plsr_objc_lookup_imp(obj, sel);
    return fn ? fn(obj, sel, a) : NULL;
}

static void *nsdbl(double v)
{
    return send_obj_d1((void *)&plsr_NSNumber_class, "numberWithDouble:", v);
}

static void *nsdata(const void *bytes, unsigned long len)
{
    return send2((void *)&plsr_NSData_class, "dataWithBytes:length:",
                 (void *)bytes, (void *)(uintptr_t)len);
}

static void *nsarray(void **items, unsigned long n)
{
    void *(*fn)(void *, const char *, void **, unsigned long);
    if (!n)
        return send0((void *)&plsr_NSArray_class, "array");
    fn = (void *(*)(void *, const char *, void **, unsigned long))
        plsr_objc_lookup_imp((void *)&plsr_NSArray_class, "arrayWithObjects:count:");
    return fn ? fn((void *)&plsr_NSArray_class, "arrayWithObjects:count:", items, n)
              : NULL;
}

static void *nsdict(void **objects, void **keys, unsigned long n)
{
    void *(*fn)(void *, const char *, void **, void **, unsigned long);
    if (!n)
        return send0((void *)&plsr_NSDictionary_class, "dictionary");
    fn = (void *(*)(void *, const char *, void **, void **, unsigned long))
        plsr_objc_lookup_imp((void *)&plsr_NSDictionary_class,
                             "dictionaryWithObjects:forKeys:count:");
    return fn ? fn((void *)&plsr_NSDictionary_class,
                   "dictionaryWithObjects:forKeys:count:", objects, keys, n)
              : NULL;
}

void *plsr_cleanroom_dict_alloc(void *cls, unsigned long n);
int plsr_cleanroom_dict_set(void *self, void *key, void *obj);
void *plsr_cleanroom_dict_get(void *self, void *key);
int plsr_cleanroom_dict_remove(void *self, void *key);
unsigned long plsr_cleanroom_dict_count(void *self);
void *plsr_cleanroom_dict_key_at(void *self, unsigned long index);

static void *mutable_dict_class(void)
{
    void *cls = objc_getClass("NSMutableDictionary");
    return cls ? cls : (void *)&plsr_NSDictionary_class;
}

static void *dict_class_with_object_pair(void *cls, const char *sel,
                                         void *obj, void *key)
{
    void *d = plsr_cleanroom_dict_alloc(cls, 0);
    (void)sel;
    if (d)
        plsr_cleanroom_dict_set(d, key, obj);
    return d;
}

static void *mutable_dict_class_with_capacity(void *cls, const char *sel,
                                              unsigned long capacity)
{
    (void)cls;
    (void)sel;
    (void)capacity;
    return plsr_cleanroom_dict_alloc(mutable_dict_class(), 0);
}

static void *mutable_dict_class_dictionary(void *cls, const char *sel)
{
    (void)cls;
    (void)sel;
    return plsr_cleanroom_dict_alloc(mutable_dict_class(), 0);
}

static const plsr_method mutable_dict_class_methods[] = {
    { "dictionary", "", (void *)mutable_dict_class_dictionary },
    { "dictionaryWithObject:forKey:", "", (void *)dict_class_with_object_pair },
    { "dictionaryWithCapacity:", "", (void *)mutable_dict_class_with_capacity },
};

static void *mutable_dict_set_object(void *self, const char *sel, void *obj,
                                     void *key)
{
    (void)sel;
    plsr_cleanroom_dict_set(self, key, obj);
    return self;
}

static void *mutable_dict_remove_object(void *self, const char *sel, void *key)
{
    (void)sel;
    plsr_cleanroom_dict_remove(self, key);
    return self;
}

static void *mutable_dict_object(void *self, const char *sel, void *key)
{
    (void)sel;
    return plsr_cleanroom_dict_get(self, key);
}

static unsigned long mutable_dict_count(void *self, const char *sel)
{
    (void)sel;
    return ((unsigned long *)self)[1];
}

/* NSMutableDictionary exposes keys through an enumerator.  The shipped
 * Foundation NSEnumerator is a Swift class, so allocating it with calloc()
 * leaves the Swift runtime without valid object metadata and a later
 * getGenericArgs() dereferences null.  Return an array object owned by this
 * runtime instead; it carries a private cursor after the normal NSArray
 * fields and nextObject walks that cursor. */
typedef struct {
    void *isa;
    unsigned long count;
    void **items;
    unsigned long index;
} plsr_array_enum;

static void *array_enum_next_object(void *self, const char *sel)
{
    plsr_array_enum *e = self;
    (void)sel;
    if (!e || e->index >= e->count)
        return NULL;
    return e->items[e->index++];
}

static void *array_enum_all_objects(void *self, const char *sel)
{
    (void)sel;
    return self;
}

static const plsr_method array_enum_methods[] = {
    { "nextObject", "", (void *)array_enum_next_object },
    { "allObjects", "", (void *)array_enum_all_objects },
};

static void *dict_key_enumerator(void *self, const char *sel)
{
    void *cls = objc_getClass("NSArray");
    plsr_array_enum *e;
    unsigned long n, i;
    (void)sel;
    if (!cls)
        return NULL;
    n = plsr_cleanroom_dict_count(self);
    e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->isa = cls;
    e->count = n;
    e->items = n ? calloc(n, sizeof(void *)) : NULL;
    if (n && !e->items) {
        free(e);
        return NULL;
    }
    for (i = 0; i < n; i++)
        e->items[i] = plsr_cleanroom_dict_key_at(self, i);
    return e;
}

static const plsr_method mutable_dict_methods[] = {
    { "setObject:forKey:", "", (void *)mutable_dict_set_object },
    { "setObject:forKeyedSubscript:", "", (void *)mutable_dict_set_object },
    { "setValue:forKey:", "", (void *)mutable_dict_set_object },
    { "removeObjectForKey:", "", (void *)mutable_dict_remove_object },
    { "objectForKey:", "", (void *)mutable_dict_object },
    { "objectForKeyedSubscript:", "", (void *)mutable_dict_object },
    { "count", "", (void *)mutable_dict_count },
    { "keyEnumerator", "", (void *)dict_key_enumerator },
};

static void *mutable_array_class_with_capacity(void *cls, const char *sel,
                                               unsigned long capacity)
{
    (void)cls;
    (void)sel;
    (void)capacity;
    return send0((void *)&plsr_NSMutableArray_class, "array");
}

static const plsr_method mutable_array_class_methods[] = {
    { "arrayWithCapacity:", "", (void *)mutable_array_class_with_capacity },
};

/* Minimal keyed archiving.  Some apps archive a small property list while
 * collecting font features; the full NSKeyedArchiver format is unnecessary
 * for that path.  Store the object pointer in NSData and hand it back from
 * the unarchiver.  The runtime does not free objects, so the pointer remains
 * valid for the lifetime of the process. */
static void *keyed_archiver_archived_data(void *cls, const char *sel, void *root,
                                          int secure, void *error)
{
    (void)cls;
    (void)sel;
    (void)secure;
    (void)error;
    return nsdata(&root, sizeof(root));
}

static void *keyed_archiver_archived_data_plain(void *cls, const char *sel,
                                                void *root)
{
    return keyed_archiver_archived_data(cls, sel, root, 0, NULL);
}

static void *keyed_archiver_init_for_writing(void *self, const char *sel,
                                             void *data)
{
    (void)sel;
    (void)data;
    return self;
}

static void *keyed_archiver_encode(void *self, const char *sel, void *obj,
                                   void *key)
{
    (void)self;
    (void)sel;
    (void)obj;
    (void)key;
    return NULL;
}

static void keyed_archiver_finish(void *self, const char *sel)
{
    (void)self;
    (void)sel;
}

static void *keyed_unarchiver_root(void *cls, const char *sel, void *data,
                                   void *object_class, void *error)
{
    const void *bytes;
    unsigned long len;
    void *root = NULL;
    (void)cls;
    (void)sel;
    (void)object_class;
    (void)error;
    if (!data)
        return NULL;
    bytes = send0(data, "bytes");
    len = (unsigned long)send0(data, "length");
    if (bytes && len >= sizeof(root))
        memcpy(&root, bytes, sizeof(root));
    return root;
}

static void *keyed_unarchiver_init_for_reading(void *self, const char *sel,
                                               void *data)
{
    *(void **)((char *)self + sizeof(void *)) = data;
    (void)sel;
    return self;
}

static void *keyed_unarchiver_decode(void *self, const char *sel, void *key)
{
    void *data = *(void **)((char *)self + sizeof(void *));
    (void)key;
    return keyed_unarchiver_root(NULL, sel, data, NULL, NULL);
}

static const plsr_method keyed_archiver_class_methods[] = {
    { "archivedDataWithRootObject:", "", (void *)keyed_archiver_archived_data_plain },
    { "archivedDataWithRootObject:requiringSecureCoding:error:", "",
      (void *)keyed_archiver_archived_data },
};

static const plsr_method keyed_archiver_methods[] = {
    { "initForWritingWithMutableData:", "", (void *)keyed_archiver_init_for_writing },
    { "encodeObject:forKey:", "", (void *)keyed_archiver_encode },
    { "finishEncoding", "", (void *)keyed_archiver_finish },
};

static const plsr_method keyed_unarchiver_class_methods[] = {
    { "unarchiveObjectWithData:", "", (void *)keyed_unarchiver_root },
    { "unarchiveTopLevelObjectWithData:error:", "",
      (void *)keyed_unarchiver_root },
    { "unarchivedObjectOfClass:fromData:error:", "",
      (void *)keyed_unarchiver_root },
    { "unarchivedObjectOfClasses:fromData:error:", "",
      (void *)keyed_unarchiver_root },
};

static const plsr_method keyed_unarchiver_methods[] = {
    { "initForReadingWithData:", "", (void *)keyed_unarchiver_init_for_reading },
    { "decodeObjectForKey:", "", (void *)keyed_unarchiver_decode },
};

static void *mutable_set_class_set(void *cls, const char *sel)
{
    (void)cls;
    (void)sel;
    return send0((void *)&plsr_NSMutableArray_class, "array");
}

static const plsr_method mutable_set_class_methods[] = {
    { "set", "", (void *)mutable_set_class_set },
};

static void *character_set_with_string(void *cls, const char *sel, void *s)
{
    (void)cls;
    (void)sel;
    return s ? send1((void *)&plsr_NSString_class, "stringWithString:", s) : NULL;
}

static const plsr_method character_set_class_methods[] = {
    { "characterSetWithCharactersInString:", "",
      (void *)character_set_with_string },
};

static int file_exists(const char *path)
{
    return path && access(path, F_OK) == 0;
}

static char *read_whole_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    char *buf = NULL;
    long n;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)n + 1);
    if (buf && n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) {
        buf[n] = 0;
        if (out_len)
            *out_len = (size_t)n;
    }
    return buf;
}

typedef struct class_spec {
    const char *name;
    uint32_t size;
    uint32_t flags;
    const plsr_method *im;
    uint32_t nim;
    const plsr_method *cm;
    uint32_t ncm;
    plsr_class *cls;
    plsr_class *meta;
    plsr_class_ro ro;
    plsr_class_ro mro;
    plsr_method_list *ml;
    plsr_method_list *cml;
} class_spec;

#define SPEC(n) { #n, BASE_SIZE, PLSR_RO_STUB, NULL, 0, NULL, 0, NULL, NULL, \
                  { 0 }, { 0 }, NULL, NULL },
static class_spec g_specs[] = { FOUNDATION_CLASSES(SPEC) };
#define NSPECS (sizeof(g_specs) / sizeof(g_specs[0]))

static plsr_method_list *make_list(const plsr_method *methods, uint32_t n)
{
    plsr_method_list *l;
    if (!n)
        return NULL;
    l = malloc(sizeof(*l) + (size_t)n * sizeof(plsr_method));
    if (!l)
        return NULL;
    l->entsize_flags = 24;
    l->count = n;
    memcpy(l->methods, methods, (size_t)n * sizeof(plsr_method));
    return l;
}

static const plsr_method *methods_for(const char *name, uint32_t *n);
static const plsr_method *class_methods_for(const char *name, uint32_t *n);

static plsr_class *class_for_name(const char *name)
{
    for (size_t i = 0; i < NSPECS; i++)
        if (!strcmp(g_specs[i].name, name))
            return g_specs[i].cls;
    if (!strcmp(name, "NSObject"))
        return &plsr_NSObject_class;
    if (!strcmp(name, "NSString"))
        return &plsr_NSString_class;
    if (!strcmp(name, "NSNumber"))
        return &plsr_NSNumber_class;
    if (!strcmp(name, "NSArray"))
        return &plsr_NSArray_class;
    if (!strcmp(name, "NSDictionary"))
        return &plsr_NSDictionary_class;
    if (!strcmp(name, "NSData"))
        return &plsr_NSData_class;
    return objc_getClass(name);
}

static plsr_class *super_for(const char *name)
{
    if (!strcmp(name, "NSMutableAttributedString"))
        return &plsr_NSAttributedString_class;
    if (!strcmp(name, "NSMutableDictionary"))
        return &plsr_NSDictionary_class;
    if (!strcmp(name, "NSMutableIndexSet"))
        return &plsr_NSIndexSet_class;
    if (!strcmp(name, "NSMutableOrderedSet"))
        return &plsr_NSOrderedSet_class;
    if (!strcmp(name, "NSMutableURLRequest"))
        return &plsr_NSURLRequest_class;
    if (!strcmp(name, "NSBlockOperation"))
        return &plsr_NSOperation_class;
    if (!strcmp(name, "NSHTTPURLResponse"))
        return &plsr_NSURLResponse_class;
    if (!strcmp(name, "NSDecimalNumber"))
        return &plsr_NSNumber_class;
    if (!strcmp(name, "NSDateFormatter") || !strcmp(name, "NSNumberFormatter") ||
        !strcmp(name, "NSByteCountFormatter") ||
        !strcmp(name, "NSDateIntervalFormatter") ||
        !strcmp(name, "NSISO8601DateFormatter"))
        return &plsr_NSFormatter_class;
    if (!strcmp(name, "NSKeyedArchiver") || !strcmp(name, "NSKeyedUnarchiver"))
        return &plsr_NSCoder_class;
    return &plsr_NSObject_class;
}

static plsr_class *decl_for(const char *name)
{
#define DECL_LOOKUP(n) if (!strcmp(name, #n)) return &plsr_##n##_class;
    FOUNDATION_CLASSES(DECL_LOOKUP)
#undef DECL_LOOKUP
    return NULL;
}

static plsr_class *meta_decl_for(const char *name)
{
#define META_LOOKUP(n) if (!strcmp(name, #n)) return &plsr_##n##_meta;
    FOUNDATION_CLASSES(META_LOOKUP)
#undef META_LOOKUP
    return NULL;
}

static void foundation_classes_init(void) __attribute__((constructor));
static void foundation_classes_init(void)
{
    for (size_t i = 0; i < NSPECS; i++) {
        class_spec *s = &g_specs[i];
        s->cls = decl_for(s->name);
        s->meta = meta_decl_for(s->name);
        if (!s->cls || !s->meta)
            continue;
        s->im = methods_for(s->name, &s->nim);
        s->cm = class_methods_for(s->name, &s->ncm);
        s->ml = make_list(s->im, s->nim);
        s->cml = make_list(s->cm, s->ncm);
        memset(&s->ro, 0, sizeof(s->ro));
        memset(&s->mro, 0, sizeof(s->mro));
        s->ro.flags = s->flags;
        s->ro.instanceSize = s->size;
        s->ro.name = s->name;
        s->ro.baseMethods = s->ml;
        s->mro.flags = s->flags | PLSR_RO_META;
        s->mro.instanceSize = s->size;
        s->mro.name = s->name;
        s->mro.baseMethods = s->cml;
        s->cls->isa = s->meta;
        s->cls->superclass = NULL;
        s->cls->data = (uintptr_t)&s->ro;
        s->meta->isa = s->meta;
        s->meta->superclass = &plsr_NSObject_meta;
        s->meta->data = (uintptr_t)&s->mro;
        plsr_objc_register_class(s->cls);
    }
    for (size_t i = 0; i < NSPECS; i++) {
        class_spec *s = &g_specs[i];
        plsr_class *super = super_for(s->name);
        s->cls->superclass = super;
        s->meta->superclass = super->isa;
    }
}

static const plsr_method *core_instance_methods(const char *name, uint32_t *n);
static const plsr_method *core_class_methods(const char *name, uint32_t *n);

static const plsr_method *methods_for(const char *name, uint32_t *n)
{
    return core_instance_methods(name, n);
}

static const plsr_method *class_methods_for(const char *name, uint32_t *n)
{
    return core_class_methods(name, n);
}

/* ==== CORE METHODS ==== */

/* ---- NSBundle ---- */

typedef struct bundle_obj {
    void *isa;
    char *path;
} bundle_obj;

static void *bundle_main_object;

static char *app_bundle_path(void)
{
    const char *env = getenv("PLSR_APP_BUNDLE");
    char buf[PATH_MAX];
    size_t len = 0;
    if (env && *env)
        return strdup(env);
    char *cmdline = read_whole_file("/proc/self/cmdline", &len);
    ssize_t n;
    char *p;
    if (cmdline) {
        for (size_t i = 0; i < len;) {
            char *arg = cmdline + i;
            char *dot = strstr(arg, ".app");
            size_t alen = strlen(arg);
            if (dot) {
                char *out = strndup(arg, (size_t)(dot - arg) + 4);
                free(cmdline);
                if (out)
                    return out;
            }
            if (strstr(arg, "/Contents/MacOS/")) {
                char *out = strndup(arg, (size_t)(strstr(arg, "/Contents/MacOS/") - arg));
                free(cmdline);
                if (out)
                    return out;
            }
            i += alen + 1;
        }
        free(cmdline);
    }
    n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n < 0)
        return strdup(".");
    buf[n] = 0;
    p = strstr(buf, "/Contents/MacOS/");
    if (p)
        *p = 0;
    else if ((p = strrchr(buf, '/')) != NULL)
        *p = 0;
    return strdup(buf[0] ? buf : ".");
}

static char *bundle_info_path(const char *bundle)
{
    char buf[PATH_MAX];
    snprintf(buf, sizeof buf, "%s/Contents/Info.plist", bundle ? bundle : ".");
    return strdup(buf);
}

static char *plist_get(const char *file, const char *key)
{
    size_t len = 0;
    char *text = read_whole_file(file, &len);
    char pattern[256];
    char *p, *start, *end, *out;
    size_t n;
    (void)len;
    if (!text)
        return NULL;
    snprintf(pattern, sizeof pattern, "<key>%s</key>", key);
    p = strstr(text, pattern);
    if (!p) {
        free(text);
        return NULL;
    }
    p += strlen(pattern);
    start = strstr(p, "<string>");
    if (start) {
        start += 8;
        end = strstr(start, "</string>");
    } else if ((start = strstr(p, "<integer>")) != NULL) {
        start += 9;
        end = strstr(start, "</integer>");
    } else {
        free(text);
        return NULL;
    }
    if (!end) {
        free(text);
        return NULL;
    }
    while (start < end && (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r'))
        start++;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r'))
        end--;
    n = (size_t)(end - start);
    out = malloc(n + 1);
    if (out) {
        memcpy(out, start, n);
        out[n] = 0;
    }
    free(text);
    return out;
}

static void *bundle_main(void *cls, const char *sel)
{
    (void)sel;
    if (!bundle_main_object) {
        bundle_obj *b = alloc_obj(cls, BASE_SIZE);
        if (b)
            b->path = app_bundle_path();
        bundle_main_object = b;
    }
    return bundle_main_object;
}

static void *bundle_with_path(void *cls, const char *sel, void *path)
{
    bundle_obj *b = alloc_obj(cls, BASE_SIZE);
    (void)sel;
    if (b)
        b->path = strdup(path ? obj_utf8(path) : ".");
    return b;
}

static void *bundle_with_url(void *cls, const char *sel, void *url)
{
    return bundle_with_path(cls, sel, send0(url, "path"));
}

static void *bundle_for_class(void *cls, const char *sel, void *object)
{
    (void)cls;
    (void)sel;
    (void)object;
    return bundle_main((void *)&plsr_NSBundle_class, "mainBundle");
}

static void *bundle_path(void *self, const char *sel)
{
    bundle_obj *b = self;
    (void)sel;
    return nsstr(b && b->path ? b->path : "");
}

static void *bundle_resource_path(void *self, const char *sel)
{
    bundle_obj *b = self;
    char buf[PATH_MAX];
    (void)sel;
    snprintf(buf, sizeof buf, "%s/Contents/Resources", b && b->path ? b->path : ".");
    return nsstr(buf);
}

static void *bundle_executable_path(void *self, const char *sel)
{
    bundle_obj *b = self;
    char *info = bundle_info_path(b ? b->path : ".");
    char *name = plist_get(info, "CFBundleExecutable");
    char buf[PATH_MAX];
    (void)sel;
    if (!name) {
        char exe[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n < 0)
            n = 0;
        exe[n] = 0;
        name = strdup(strrchr(exe, '/') ? strrchr(exe, '/') + 1 : exe);
    }
    snprintf(buf, sizeof buf, "%s/Contents/MacOS/%s", b && b->path ? b->path : ".",
             name ? name : "app");
    free(info);
    free(name);
    return nsstr(buf);
}

static void *bundle_info_dictionary(void *self, const char *sel)
{
    static const char *keys[] = {
        "CFBundleIdentifier", "CFBundleName", "CFBundleDisplayName",
        "CFBundleExecutable", "CFBundleVersion", "CFBundleShortVersionString",
        "CFBundlePackageType", "CFBundleSignature", "CFBundleIconFile",
        "LSMinimumSystemVersion", "NSPrincipalClass", "NSMainNibFile",
        "NSMainStoryboardFile", "NSHumanReadableCopyright",
        "CFBundleDevelopmentRegion"
    };
    bundle_obj *b = self;
    char *info = bundle_info_path(b ? b->path : ".");
    void *values[16], *names[16];
    unsigned long n = 0;
    (void)sel;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        char *v = plist_get(info, keys[i]);
        if (!v)
            continue;
        values[n] = nsstr(v);
        names[n] = nsstr(keys[i]);
        n++;
        free(v);
    }
    free(info);
    return n ? nsdict(values, names, n) : send0((void *)&plsr_NSDictionary_class, "dictionary");
}

static void *bundle_object_for_info_key(void *self, const char *sel, void *key)
{
    void *dict = send0(self, "infoDictionary");
    void *value = NULL;
    const char *k = key ? obj_utf8(key) : "";
    (void)sel;
    if (dict)
        value = send1(dict, "objectForKey:", key);
    if (!value && !strcmp(k, "CFBundleVersion"))
        value = send1(dict, "objectForKey:", nsstr("CFBundleShortVersionString"));
    if (!value && !strcmp(k, "CFBundleShortVersionString"))
        value = send1(dict, "objectForKey:", nsstr("CFBundleVersion"));
    if (getenv("PLSR_OBJC_TRACE"))
        dprintf(2, "objc: infokey %s -> %p\n", k, value);
    return value;
}

static void *bundle_identifier(void *self, const char *sel)
{
    bundle_obj *b = self;
    char *info = bundle_info_path(b ? b->path : ".");
    char *v = plist_get(info, "CFBundleIdentifier");
    void *out = v ? nsstr(v) : NULL;
    (void)sel;
    free(info);
    free(v);
    return out;
}

static void *file_url(const char *path)
{
    void *cls = objc_getClass("NSURL");
    return cls ? send1(cls, "fileURLWithPath:", nsstr(path)) : NULL;
}

static int bundle_resource_exists(const char *bundle, const char *dir,
                                  const char *name, const char *type,
                                  char *out, size_t out_size)
{
    if (*dir)
        snprintf(out, out_size, "%s/Contents/Resources/%s/%s%s%s",
                 bundle, dir, name, *type ? "." : "", type);
    else
        snprintf(out, out_size, "%s/Contents/Resources/%s%s%s",
                 bundle, name, *type ? "." : "", type);
    return file_exists(out);
}

static void *bundle_path_for_resource(void *self, const char *sel, void *name, void *type)
{
    bundle_obj *b = self;
    const char *base = b && b->path ? b->path : ".";
    const char *n = name ? obj_utf8(name) : "";
    const char *t = type ? obj_utf8(type) : "";
    char buf[PATH_MAX];
    char resources[PATH_MAX];
    DIR *d;
    struct dirent *de;
    (void)sel;
    if (!*n)
        return NULL;

    /* Unlocalized resources first, then Base.lproj, then every .lproj in the
     * bundle.  The main storyboard usually lives in Base.lproj. */
    if (bundle_resource_exists(base, "", n, t, buf, sizeof buf))
        return nsstr(buf);
    if (bundle_resource_exists(base, "Base.lproj", n, t, buf, sizeof buf))
        return nsstr(buf);
    snprintf(resources, sizeof resources, "%s/Contents/Resources", base);
    d = opendir(resources);
    while (d && (de = readdir(d)) != NULL) {
        size_t len = strlen(de->d_name);
        if (len < 6 || strcmp(de->d_name + len - 6, ".lproj") != 0)
            continue;
        if (bundle_resource_exists(base, de->d_name, n, t, buf, sizeof buf)) {
            closedir(d);
            return nsstr(buf);
        }
    }
    if (d)
        closedir(d);
    return NULL;
}

static void *bundle_path_for_resource_dir(void *self, const char *sel, void *name,
                                          void *type, void *dir)
{
    const char *n = name ? obj_utf8(name) : "";
    const char *t = type ? obj_utf8(type) : "";
    const char *d = dir ? obj_utf8(dir) : "";
    char buf[PATH_MAX];
    char localized[PATH_MAX];
    bundle_obj *b = self;
    const char *base = b && b->path ? b->path : ".";
    (void)sel;
    if (!*d)
        return bundle_path_for_resource(self, sel, name, type);
    if (bundle_resource_exists(base, d, n, t, buf, sizeof buf))
        return nsstr(buf);
    snprintf(localized, sizeof localized, "%s.lproj", d);
    if (bundle_resource_exists(base, localized, n, t, buf, sizeof buf))
        return nsstr(buf);
    return NULL;
}

static void *bundle_url_for_resource(void *self, const char *sel, void *name, void *type)
{
    void *p = bundle_path_for_resource(self, sel, name, type);
    return p ? file_url(obj_utf8(p)) : NULL;
}

static void *bundle_load(void *self, const char *sel)
{
    (void)self;
    (void)sel;
    return (void *)1;
}

static int bundle_is_loaded(void *self, const char *sel)
{
    (void)self;
    (void)sel;
    return 1;
}

static void *bundle_principal_class(void *self, const char *sel)
{
    bundle_obj *b = self;
    char *info = bundle_info_path(b ? b->path : ".");
    char *name = plist_get(info, "NSPrincipalClass");
    void *cls = name ? objc_getClass(name) : NULL;
    (void)sel;
    free(info);
    free(name);
    return cls ? cls : objc_getClass("NSApplication");
}

static void *bundle_localized_string(void *self, const char *sel, void *key,
                                     void *value, void *table)
{
    (void)self;
    (void)sel;
    (void)table;
    if (value && obj_utf8(value)[0])
        return value;
    return key;
}

static void *bundle_bundle_url(void *self, const char *sel)
{
    bundle_obj *b = self;
    (void)sel;
    return file_url(b && b->path ? b->path : ".");
}

static void *bundle_resource_url(void *self, const char *sel)
{
    return file_url(obj_utf8(bundle_resource_path(self, sel)));
}

static void *bundle_executable_url(void *self, const char *sel)
{
    return file_url(obj_utf8(bundle_executable_path(self, sel)));
}

static void *bundle_all(void *cls, const char *sel)
{
    void *items[1];
    (void)sel;
    items[0] = bundle_main(cls, "mainBundle");
    return nsarray(items, 1);
}

static void *bundle_with_identifier(void *cls, const char *sel, void *ident)
{
    void *m = bundle_main(cls, "mainBundle");
    void *mid = send0(m, "bundleIdentifier");
    (void)sel;
    if (mid && ident && !strcmp(obj_utf8(mid), obj_utf8(ident)))
        return m;
    return NULL;
}

static void *bundle_localizations(void *self, const char *sel)
{
    bundle_obj *b = self;
    char resources[PATH_MAX];
    void **items = NULL;
    unsigned long n = 0, cap = 0;
    DIR *d;
    struct dirent *de;
    (void)sel;
    snprintf(resources, sizeof resources, "%s/Contents/Resources",
             b && b->path ? b->path : ".");
    d = opendir(resources);
    while (d && (de = readdir(d)) != NULL) {
        size_t len = strlen(de->d_name);
        char *name;
        if (len < 6 || strcmp(de->d_name + len - 6, ".lproj") != 0)
            continue;
        name = strndup(de->d_name, len - 6);
        if (!name)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 4;
            items = realloc(items, cap * sizeof(void *));
        }
        items[n++] = nsstr(name);
        free(name);
    }
    if (d)
        closedir(d);
    if (!n) {
        items = realloc(items, 2 * sizeof(void *));
        items[n++] = nsstr("Base");
        items[n++] = nsstr("en");
    }
    {
        void *out = nsarray(items, n);
        free(items);
        return out;
    }
}

static void *bundle_preferred_localizations(void *self, const char *sel)
{
    (void)self;
    (void)sel;
    {
        void *items[2] = { nsstr("Base"), nsstr("en") };
        return nsarray(items, 2);
    }
}

/* The nib loader lives in libplsr-appkit.so (nib.c).  Foundation stays
 * linkable on its own when AppKit is absent. */
extern signed char plsr_nib_load_named(void *bundle, void *name, void *owner,
                                       void **top) __attribute__((weak));

static signed char bundle_load_nib_named(void *self, const char *sel, void *name,
                                         void *owner, void **top)
{
    (void)sel;
    return plsr_nib_load_named ? plsr_nib_load_named(self, name, owner, top) : 0;
}

static signed char bundle_load_nib_owner(void *self, const char *sel, void *name,
                                         void *owner)
{
    return bundle_load_nib_named(self, sel, name, owner, NULL);
}

static const plsr_method bundle_methods[] = {
    { "loadNibNamed:owner:topLevelObjects:", "", (void *)bundle_load_nib_named },
    { "loadNibNamed:owner:", "", (void *)bundle_load_nib_owner },
    { "path", "", (void *)bundle_path },
    { "bundlePath", "", (void *)bundle_path },
    { "resourcePath", "", (void *)bundle_resource_path },
    { "executablePath", "", (void *)bundle_executable_path },
    { "infoDictionary", "", (void *)bundle_info_dictionary },
    { "localizedInfoDictionary", "", (void *)bundle_info_dictionary },
    { "objectForInfoDictionaryKey:", "", (void *)bundle_object_for_info_key },
    { "bundleIdentifier", "", (void *)bundle_identifier },
    { "pathForResource:ofType:", "", (void *)bundle_path_for_resource },
    { "pathForResource:ofType:inDirectory:", "", (void *)bundle_path_for_resource_dir },
    { "URLForResource:withExtension:", "", (void *)bundle_url_for_resource },
    { "load", "", (void *)bundle_load },
    { "loadAndReturnError:", "", (void *)bundle_load },
    { "isLoaded", "", (void *)bundle_is_loaded },
    { "principalClass", "", (void *)bundle_principal_class },
    { "localizedStringForKey:value:table:", "", (void *)bundle_localized_string },
    { "bundleURL", "", (void *)bundle_bundle_url },
    { "resourceURL", "", (void *)bundle_resource_url },
    { "executableURL", "", (void *)bundle_executable_url },
    { "localizations", "", (void *)bundle_localizations },
    { "preferredLocalizations", "", (void *)bundle_preferred_localizations },
    { "developmentRegion", "", (void *)bundle_localizations },
};

static const plsr_method bundle_class_methods[] = {
    { "mainBundle", "", (void *)bundle_main },
    { "bundleWithPath:", "", (void *)bundle_with_path },
    { "bundleWithURL:", "", (void *)bundle_with_url },
    { "bundleForClass:", "", (void *)bundle_for_class },
    { "bundleWithIdentifier:", "", (void *)bundle_with_identifier },
    { "allBundles", "", (void *)bundle_all },
    { "allFrameworks", "", (void *)bundle_all },
};

/* ---- NSProcessInfo ---- */

typedef struct { long major, minor, patch; } plsr_osversion;
static void *process_info_singleton;

static void *process_info_main(void *cls, const char *sel)
{
    (void)sel;
    if (!process_info_singleton)
        process_info_singleton = alloc_obj(cls, BASE_SIZE);
    return process_info_singleton;
}

static void *process_arguments(void *self, const char *sel)
{
    size_t len = 0;
    char *buf = read_whole_file("/proc/self/cmdline", &len);
    void **items = NULL;
    unsigned long n = 0, cap = 0;
    (void)self;
    (void)sel;
    if (!buf)
        return nsarray(NULL, 0);
    for (size_t i = 0; i < len;) {
        size_t j = i;
        while (j < len && buf[j])
            j++;
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            items = realloc(items, cap * sizeof(void *));
        }
        items[n++] = nsstr(buf + i);
        i = j + 1;
    }
    free(buf);
    {
        void *out = nsarray(items, n);
        free(items);
        return out;
    }
}

static void *process_environment(void *self, const char *sel)
{
    void **keys = NULL, **values = NULL;
    unsigned long n = 0, cap = 0;
    (void)self;
    (void)sel;
    for (char **e = environ; e && *e; e++) {
        char *eq = strchr(*e, '=');
        char *k;
        if (!eq)
            continue;
        k = strndup(*e, (size_t)(eq - *e));
        if (!k)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            keys = realloc(keys, cap * sizeof(void *));
            values = realloc(values, cap * sizeof(void *));
        }
        keys[n] = nsstr(k);
        values[n] = nsstr(eq + 1);
        n++;
        free(k);
    }
    {
        void *out = nsdict(values, keys, n);
        free(keys);
        free(values);
        return out;
    }
}

static void *process_identifier(void *self, const char *sel)
{
    (void)self;
    (void)sel;
    return nsnum(getpid());
}

static void *process_name(void *self, const char *sel)
{
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    char *base;
    (void)self;
    (void)sel;
    if (n < 0)
        return nsstr("app");
    buf[n] = 0;
    base = strrchr(buf, '/');
    return nsstr(base ? base + 1 : buf);
}

static void *process_host_name(void *self, const char *sel)
{
    char buf[256] = "localhost";
    (void)self;
    (void)sel;
    gethostname(buf, sizeof buf - 1);
    return nsstr(buf);
}

static void *process_user_name(void *self, const char *sel)
{
    const char *u = getenv("USER");
    (void)self;
    (void)sel;
    return nsstr(u && *u ? u : "user");
}

static plsr_osversion process_os_version(void *self, const char *sel)
{
    plsr_osversion v = { 14, 0, 0 };
    (void)self;
    (void)sel;
    return v;
}

static int process_at_least(void *self, const char *sel, plsr_osversion v)
{
    (void)self;
    (void)sel;
    (void)v;
    return 1;
}

static double process_uptime(void *self, const char *sel)
{
    struct timespec ts;
    (void)self;
    (void)sel;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void *process_physical_memory(void *self, const char *sel)
{
    long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGESIZE);
    (void)self;
    (void)sel;
    return nsnum(pages > 0 && size > 0 ? (long long)pages * size : 0);
}

static void *process_cpu_count(void *self, const char *sel)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    (void)self;
    (void)sel;
    return nsnum(n > 0 ? n : 1);
}

static void *process_unique_string(void *self, const char *sel)
{
    char buf[128];
    struct timespec ts;
    (void)self;
    (void)sel;
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(buf, sizeof buf, "%d-%ld-%ld", getpid(), (long)ts.tv_sec, (long)ts.tv_nsec);
    return nsstr(buf);
}

static const plsr_method process_info_methods[] = {
    { "arguments", "", (void *)process_arguments },
    { "environment", "", (void *)process_environment },
    { "processIdentifier", "", (void *)process_identifier },
    { "processName", "", (void *)process_name },
    { "hostName", "", (void *)process_host_name },
    { "userName", "", (void *)process_user_name },
    { "operatingSystemVersion", "", (void *)process_os_version },
    { "isOperatingSystemAtLeastVersion:", "", (void *)process_at_least },
    { "systemUptime", "", (void *)process_uptime },
    { "physicalMemory", "", (void *)process_physical_memory },
    { "activeProcessorCount", "", (void *)process_cpu_count },
    { "processorCount", "", (void *)process_cpu_count },
    { "globallyUniqueString", "", (void *)process_unique_string },
    { "operatingSystemVersionString", "", (void *)process_unique_string },
};

static const plsr_method process_info_class_methods[] = {
    { "processInfo", "", (void *)process_info_main },
};

/* ---- NSUserDefaults ---- */

typedef struct defaults_pair {
    char *key;
    void *value;
    struct defaults_pair *next;
} defaults_pair;

typedef struct {
    void *isa;
    defaults_pair *head;
} defaults_obj;

static void *defaults_singleton;

static void *defaults_standard(void *cls, const char *sel)
{
    (void)sel;
    if (!defaults_singleton)
        defaults_singleton = alloc_obj(cls, BASE_SIZE);
    return defaults_singleton;
}

static defaults_pair *defaults_find(defaults_obj *d, const char *key)
{
    for (defaults_pair *p = d ? d->head : NULL; p; p = p->next)
        if (!strcmp(p->key, key))
            return p;
    return NULL;
}

static void *defaults_object(void *self, const char *sel, void *key)
{
    defaults_obj *d = self;
    defaults_pair *p = defaults_find(d, key ? obj_utf8(key) : "");
    (void)sel;
    return p ? p->value : NULL;
}

static void defaults_set(void *self, const char *sel, void *value, void *key);

static void *defaults_value_key(void *self, const char *sel, void *key)
{
    return defaults_object(self, sel, key);
}

static void defaults_set_value(void *self, const char *sel, void *value, void *key)
{
    defaults_set(self, sel, value, key);
}

static void defaults_set(void *self, const char *sel, void *value, void *key)
{
    defaults_obj *d = self;
    const char *k = key ? obj_utf8(key) : "";
    defaults_pair *p;
    (void)sel;
    if (!d || !*k)
        return;
    p = defaults_find(d, k);
    if (!value) {
        defaults_pair **pp = &d->head;
        while (*pp && *pp != p)
            pp = &(*pp)->next;
        if (p) {
            *pp = p->next;
            free(p->key);
            free(p);
        }
        return;
    }
    if (!p) {
        p = calloc(1, sizeof(*p));
        if (!p)
            return;
        p->key = strdup(k);
        p->next = d->head;
        d->head = p;
    }
    p->value = value;
}

static void defaults_remove(void *self, const char *sel, void *key)
{
    defaults_set(self, sel, NULL, key);
}

static void *defaults_string(void *self, const char *sel, void *key)
{
    void *v = defaults_object(self, sel, key);
    return v && is_string_obj(v) ? v : NULL;
}

static void *defaults_array(void *self, const char *sel, void *key)
{
    void *v = defaults_object(self, sel, key);
    return v && is_array_obj(v) ? v : NULL;
}

static void *defaults_dict(void *self, const char *sel, void *key)
{
    void *v = defaults_object(self, sel, key);
    return v && is_dict_obj(v) ? v : NULL;
}

static int defaults_bool(void *self, const char *sel, void *key)
{
    void *v = defaults_object(self, sel, key);
    return v ? (int)(intptr_t)send0(v, "boolValue") : 0;
}

static long long defaults_integer(void *self, const char *sel, void *key)
{
    void *v = defaults_object(self, sel, key);
    return v ? (long long)(intptr_t)send0(v, "longLongValue") : 0;
}

static double defaults_double(void *self, const char *sel, void *key)
{
    void *v = defaults_object(self, sel, key);
    return v ? send_d(v, "doubleValue") : 0.0;
}

static void defaults_set_bool(void *self, const char *sel, int v, void *key)
{
    defaults_set(self, sel, nsbool(v), key);
}

static void defaults_set_integer(void *self, const char *sel, long long v, void *key)
{
    defaults_set(self, sel, nsnum(v), key);
}

static void defaults_set_double(void *self, const char *sel, double v, void *key)
{
    defaults_set(self, sel, nsdbl(v), key);
}

static int defaults_sync(void *self, const char *sel)
{
    (void)self;
    (void)sel;
    return 1;
}

static void defaults_register(void *self, const char *sel, void *domain)
{
    void *keys, *values;
    unsigned long n;
    (void)sel;
    if (!domain || !is_dict_obj(domain))
        return;
    keys = send0(domain, "allKeys");
    values = send0(domain, "allValues");
    n = keys ? (unsigned long)(uintptr_t)send0(keys, "count") : 0;
    for (unsigned long i = 0; i < n; i++) {
        void *k = send1(keys, "objectAtIndex:", (void *)(uintptr_t)i);
        if (!defaults_find(self, obj_utf8(k)))
            defaults_set(self, sel, send1(values, "objectAtIndex:", (void *)(uintptr_t)i), k);
    }
}

static void *defaults_dictionary(void *self, const char *sel)
{
    defaults_obj *d = self;
    void **keys = NULL, **values = NULL;
    unsigned long n = 0;
    (void)sel;
    for (defaults_pair *p = d ? d->head : NULL; p; p = p->next) {
        keys = realloc(keys, (n + 1) * sizeof(void *));
        values = realloc(values, (n + 1) * sizeof(void *));
        keys[n] = nsstr(p->key);
        values[n] = p->value;
        n++;
    }
    {
        void *out = nsdict(values, keys, n);
        free(keys);
        free(values);
        return out;
    }
}

static void defaults_noop(void *self, const char *sel, ...)
{
    (void)self;
    (void)sel;
}

static void *defaults_empty_dict(void *self, const char *sel, ...)
{
    (void)self;
    (void)sel;
    return send0((void *)&plsr_NSDictionary_class, "dictionary");
}

static void *defaults_empty_array(void *self, const char *sel, ...)
{
    (void)self;
    (void)sel;
    return nsarray(NULL, 0);
}

static const plsr_method defaults_methods[] = {
    { "objectForKey:", "", (void *)defaults_object },
    { "valueForKey:", "", (void *)defaults_value_key },
    { "setValue:forKey:", "", (void *)defaults_set_value },
    { "setObject:forKey:", "", (void *)defaults_set },
    { "removeObjectForKey:", "", (void *)defaults_remove },
    { "stringForKey:", "", (void *)defaults_string },
    { "arrayForKey:", "", (void *)defaults_array },
    { "dictionaryForKey:", "", (void *)defaults_dict },
    { "boolForKey:", "", (void *)defaults_bool },
    { "integerForKey:", "", (void *)defaults_integer },
    { "floatForKey:", "", (void *)defaults_double },
    { "doubleForKey:", "", (void *)defaults_double },
    { "setBool:forKey:", "", (void *)defaults_set_bool },
    { "setInteger:forKey:", "", (void *)defaults_set_integer },
    { "setDouble:forKey:", "", (void *)defaults_set_double },
    { "synchronize", "", (void *)defaults_sync },
    { "registerDefaults:", "", (void *)defaults_register },
    { "removeVolatileDomainForName:", "", (void *)defaults_noop },
    { "setVolatileDomain:forName:", "", (void *)defaults_noop },
    { "volatileDomainForName:", "", (void *)defaults_empty_dict },
    { "volatileDomainNames", "", (void *)defaults_empty_array },
    { "removePersistentDomainForName:", "", (void *)defaults_noop },
    { "setPersistentDomain:forName:", "", (void *)defaults_noop },
    { "persistentDomainForName:", "", (void *)defaults_empty_dict },
    { "addSuiteNamed:", "", (void *)defaults_noop },
    { "removeSuiteNamed:", "", (void *)defaults_noop },
    { "searchList", "", (void *)defaults_empty_array },
    { "objectForKey:inDomain:", "", (void *)defaults_object },
    { "setObject:forKey:inDomain:", "", (void *)defaults_set },
    { "dictionaryRepresentation", "", (void *)defaults_dictionary },
};

static const plsr_method defaults_class_methods[] = {
    { "standardUserDefaults", "", (void *)defaults_standard },
};

typedef struct {
    void *isa;
    void *path;
} plsr_url_obj;

static void *url_new(void *cls, const char *path)
{
    plsr_url_obj *u = calloc(1, sizeof(*u));
    if (!u)
        return NULL;
    u->isa = cls;
    u->path = nsstr(path);
    return u;
}

static void *url_class_file_url_with_path(void *cls, const char *sel, void *path)
{
    plsr_url_obj *u = calloc(1, sizeof(*u));
    (void)sel;
    if (!u)
        return NULL;
    u->isa = cls;
    u->path = path;
    return u;
}

static void *url_class_with_string(void *cls, const char *sel, void *string)
{
    (void)sel;
    return url_class_file_url_with_path(cls, sel, string);
}

static void *url_path(void *self, const char *sel)
{
    (void)sel;
    return ((plsr_url_obj *)self)->path;
}

static const char *url_file_system_representation(void *self, const char *sel)
{
    (void)sel;
    return obj_utf8(((plsr_url_obj *)self)->path);
}

static void *url_absolute_string(void *self, const char *sel)
{
    const char *path = obj_utf8(((plsr_url_obj *)self)->path);
    char *buf;
    void *out;
    (void)sel;
    if (strstr(path, "://"))
        return nsstr(path);
    buf = malloc(strlen(path) + 8);
    if (!buf)
        return NULL;
    snprintf(buf, strlen(path) + 8, "file://%s", path);
    out = nsstr(buf);
    free(buf);
    return out;
}

static void *url_with_path_value(void *self, const char *path)
{
    plsr_url_obj *u = calloc(1, sizeof(*u));
    if (!u)
        return NULL;
    u->isa = *(void **)self;
    u->path = nsstr(path);
    return u;
}

static void *url_by_deleting_last_path_component(void *self, const char *sel)
{
    const char *path = obj_utf8(((plsr_url_obj *)self)->path);
    char *buf = strdup(path);
    char *slash;
    void *out;
    (void)sel;
    if (!buf)
        return NULL;
    slash = strrchr(buf, '/');
    if (!slash) {
        free(buf);
        return NULL;
    }
    if (slash == buf)
        slash[1] = '\0';
    else
        *slash = '\0';
    out = url_with_path_value(self, buf);
    free(buf);
    return out;
}

static void *url_by_appending_path_component(void *self, const char *sel,
                                              void *component)
{
    const char *base = obj_utf8(((plsr_url_obj *)self)->path);
    const char *part = obj_utf8(component);
    size_t blen = strlen(base), plen = strlen(part);
    int slash = blen > 0 && base[blen - 1] != '/';
    char *buf = malloc(blen + plen + (slash ? 2 : 1));
    void *out;
    (void)sel;
    if (!buf)
        return NULL;
    memcpy(buf, base, blen);
    if (slash)
        buf[blen++] = '/';
    memcpy(buf + blen, part, plen + 1);
    out = url_with_path_value(self, buf);
    free(buf);
    return out;
}

static void *url_last_path_component(void *self, const char *sel)
{
    const char *path = obj_utf8(((plsr_url_obj *)self)->path);
    const char *slash = strrchr(path, '/');
    (void)sel;
    return nsstr(slash ? slash + 1 : path);
}

static char *url_normalize_path(const char *path)
{
    size_t len = strlen(path);
    char *out = malloc(len + 3);
    size_t j = 0;
    int absolute = path[0] == '/';
    const char *p = path;
    if (!out)
        return NULL;
    if (absolute)
        out[j++] = '/';
    while (*p) {
        const char *start;
        size_t n;
        while (*p == '/')
            p++;
        if (!*p)
            break;
        start = p;
        while (*p && *p != '/')
            p++;
        n = (size_t)(p - start);
        if (n == 1 && start[0] == '.')
            continue;
        if (n == 2 && start[0] == '.' && start[1] == '.') {
            if (j > (absolute ? 1u : 0u)) {
                while (j > (absolute ? 1u : 0u) && out[j - 1] != '/')
                    j--;
                if (j > (absolute ? 1u : 0u))
                    j--;
            }
            continue;
        }
        if (j > (absolute ? 1u : 0u) && out[j - 1] != '/')
            out[j++] = '/';
        memcpy(out + j, start, n);
        j += n;
    }
    if (!j)
        out[j++] = absolute ? '/' : '.';
    out[j] = 0;
    return out;
}

static void *url_with_normalized_path(void *self, const char *path)
{
    char *normalized = url_normalize_path(path);
    void *out;
    if (!normalized)
        return NULL;
    out = url_with_path_value(self, normalized);
    free(normalized);
    return out;
}

static void *url_absolute_path(void *self, const char *sel)
{
    (void)sel;
    return ((plsr_url_obj *)self)->path;
}

static void *url_relative_path(void *self, const char *sel)
{
    (void)sel;
    return ((plsr_url_obj *)self)->path;
}

static void *url_file_url(void *self, const char *sel)
{
    (void)sel;
    return self;
}

static void *url_path_extension(void *self, const char *sel)
{
    const char *path = obj_utf8(((plsr_url_obj *)self)->path);
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(slash ? slash + 1 : path, '.');
    (void)sel;
    return nsstr(dot && dot[1] ? dot + 1 : "");
}

static void *url_by_appending_path_extension(void *self, const char *sel,
                                              void *extension)
{
    const char *path = obj_utf8(((plsr_url_obj *)self)->path);
    const char *ext = obj_utf8(extension);
    size_t n = strlen(path), m = strlen(ext);
    char *buf = malloc(n + m + 2);
    void *out;
    (void)sel;
    if (!buf)
        return NULL;
    memcpy(buf, path, n);
    buf[n] = '.';
    memcpy(buf + n + 1, ext, m + 1);
    out = url_with_path_value(self, buf);
    free(buf);
    return out;
}

static void *url_by_deleting_path_extension(void *self, const char *sel)
{
    const char *path = obj_utf8(((plsr_url_obj *)self)->path);
    char *buf = strdup(path);
    char *slash;
    char *dot;
    void *out;
    (void)sel;
    if (!buf)
        return NULL;
    slash = strrchr(buf, '/');
    dot = strrchr(slash ? slash + 1 : buf, '.');
    if (dot)
        *dot = '\0';
    out = url_with_path_value(self, buf);
    free(buf);
    return out;
}

static void *url_by_standardizing_path(void *self, const char *sel)
{
    (void)sel;
    return url_with_normalized_path(self, obj_utf8(((plsr_url_obj *)self)->path));
}

static void *url_by_resolving_symlinks(void *self, const char *sel)
{
    char *resolved = realpath(obj_utf8(((plsr_url_obj *)self)->path), NULL);
    void *out;
    (void)sel;
    if (!resolved)
        return url_by_standardizing_path(self, sel);
    out = url_with_path_value(self, resolved);
    free(resolved);
    return out;
}

static void *url_path_components(void *self, const char *sel)
{
    const char *path = obj_utf8(((plsr_url_obj *)self)->path);
    void **items = NULL;
    unsigned long n = 0;
    const char *p = path;
    void *out;
    (void)sel;
    if (*p == '/') {
        items = realloc(items, sizeof(*items));
        items[n++] = nsstr("/");
    }
    while (*p) {
        const char *start;
        while (*p == '/')
            p++;
        if (!*p)
            break;
        start = p;
        while (*p && *p != '/')
            p++;
        items = realloc(items, (n + 1) * sizeof(*items));
        items[n] = nsstr(strndup(start, (size_t)(p - start)));
        n++;
    }
    out = nsarray(items, n);
    free(items);
    return out;
}

static int url_is_equal(void *self, const char *sel, void *other)
{
    (void)sel;
    return self == other ||
           (other && !strcmp(obj_utf8(((plsr_url_obj *)self)->path),
                             obj_utf8(((plsr_url_obj *)other)->path)));
}

static unsigned long url_hash(void *self, const char *sel)
{
    const char *p = obj_utf8(((plsr_url_obj *)self)->path);
    unsigned long h = 5381;
    (void)sel;
    while (*p)
        h = h * 33 + (unsigned char)*p++;
    return h;
}

static void *url_description(void *self, const char *sel)
{
    return url_absolute_string(self, sel);
}

static int url_check_reachable(void *self, const char *sel, void *error)
{
    (void)sel;
    (void)error;
    return access(obj_utf8(((plsr_url_obj *)self)->path), F_OK) == 0;
}

static int url_has_directory_path(void *self, const char *sel)
{
    struct stat st;
    (void)sel;
    return stat(obj_utf8(((plsr_url_obj *)self)->path), &st) == 0 && S_ISDIR(st.st_mode);
}

static void *url_by_appending_path_component_is_directory(void *self,
                                                           const char *sel,
                                                           void *component,
                                                           int is_directory)
{
    (void)is_directory;
    return url_by_appending_path_component(self, sel, component);
}

static void *url_class_file_url_with_path_is_directory(void *cls, const char *sel,
                                                        void *path, int is_directory)
{
    (void)is_directory;
    return url_class_file_url_with_path(cls, sel, path);
}

static void *url_class_file_url_with_path_relative(void *cls, const char *sel,
                                                    void *path, void *base)
{
    (void)base;
    return url_class_file_url_with_path(cls, sel, path);
}

static void *url_class_with_string_relative(void *cls, const char *sel,
                                             void *string, void *base)
{
    const char *s = obj_utf8(string);
    const char *b = base ? obj_utf8(((plsr_url_obj *)base)->path) : "";
    size_t slen, blen;
    char *buf;
    void *out;
    (void)sel;
    if (!s || *s == '/' || strstr(s, "://"))
        return url_class_file_url_with_path(cls, sel, string);
    slen = strlen(s);
    blen = strlen(b);
    buf = malloc(blen + slen + 2);
    if (!buf)
        return NULL;
    memcpy(buf, b, blen);
    if (blen && b[blen - 1] != '/')
        buf[blen++] = '/';
    memcpy(buf + blen, s, slen + 1);
    out = url_new(cls, buf);
    free(buf);
    return out;
}

static int url_is_file_url(void *self, const char *sel)
{
    const char *path = obj_utf8(((plsr_url_obj *)self)->path);
    (void)self;
    (void)sel;
    return !strncmp(path, "file://", 7) || path[0] == '/';
}

typedef struct {
    void *isa;
    double t;
} plsr_date_obj;

static void *date_new(void *cls, double t)
{
    plsr_date_obj *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->isa = cls;
    d->t = t;
    return d;
}

static void *date_distant_future(void *cls, const char *sel)
{
    (void)sel;
    return date_new(cls, 63113904000.0); /* 4001-01-01 */
}

static void *date_distant_past(void *cls, const char *sel)
{
    (void)sel;
    return date_new(cls, -63113904000.0);
}

static void *date_now(void *cls, const char *sel)
{
    (void)sel;
    return date_new(cls, (double)time(NULL));
}

static void *date_with_interval_since_now(void *cls, const char *sel, double dt)
{
    (void)sel;
    return date_new(cls, (double)time(NULL) + dt);
}

static void *date_with_interval_since_1970(void *cls, const char *sel, double t)
{
    (void)sel;
    return date_new(cls, t);
}

static void *date_with_interval_since_reference(void *cls, const char *sel, double t)
{
    (void)sel;
    return date_new(cls, t + 978307200.0); /* 2001-01-01 */
}

static void *date_with_interval_since_date(void *cls, const char *sel, double dt,
                                           void *since)
{
    (void)sel;
    return date_new(cls, (since ? ((plsr_date_obj *)since)->t : (double)time(NULL)) + dt);
}

static double date_time_interval_since_now(void *self, const char *sel)
{
    (void)sel;
    return (double)time(NULL) - ((plsr_date_obj *)self)->t;
}

static double date_time_interval_since_1970(void *self, const char *sel)
{
    (void)sel;
    return ((plsr_date_obj *)self)->t;
}

static double date_time_interval_since_reference(void *self, const char *sel)
{
    (void)sel;
    return ((plsr_date_obj *)self)->t - 978307200.0;
}

static double date_time_interval_since_date(void *self, const char *sel, void *other)
{
    (void)sel;
    return ((plsr_date_obj *)self)->t - ((plsr_date_obj *)other)->t;
}

static void *date_by_adding_time_interval(void *self, const char *sel, double dt)
{
    (void)sel;
    return date_new(*(void **)self, ((plsr_date_obj *)self)->t + dt);
}

static void *date_add_time_interval(void *self, const char *sel, double dt)
{
    return date_by_adding_time_interval(self, sel, dt);
}

static int date_is_equal(void *self, const char *sel, void *other);

static int date_is_equal_generic(void *self, const char *sel, void *other)
{
    (void)sel;
    return date_is_equal(self, "isEqualToDate:", other);
}

static unsigned long date_hash(void *self, const char *sel)
{
    union { double d; unsigned long u; } value;
    (void)sel;
    value.d = ((plsr_date_obj *)self)->t;
    return value.u;
}

static int date_compare(void *self, const char *sel, void *other)
{
    double a = ((plsr_date_obj *)self)->t;
    double b = ((plsr_date_obj *)other)->t;
    (void)sel;
    return a < b ? -1 : a > b ? 1 : 0;
}

static void *date_later_date(void *self, const char *sel, void *other)
{
    double a = ((plsr_date_obj *)self)->t;
    double b = ((plsr_date_obj *)other)->t;
    (void)sel;
    return a >= b ? self : other;
}

static void *date_earlier_date(void *self, const char *sel, void *other)
{
    double a = ((plsr_date_obj *)self)->t;
    double b = ((plsr_date_obj *)other)->t;
    (void)sel;
    return a <= b ? self : other;
}

static int date_is_equal(void *self, const char *sel, void *other)
{
    (void)sel;
    return self == other ||
           (other && ((plsr_date_obj *)self)->t == ((plsr_date_obj *)other)->t);
}

static void *date_description(void *self, const char *sel)
{
    char buf[64];
    (void)sel;
    snprintf(buf, sizeof buf, "%g", ((plsr_date_obj *)self)->t);
    return nsstr(buf);
}

static void *date_description_locale(void *self, const char *sel, void *locale)
{
    (void)locale;
    return date_description(self, sel);
}

static const plsr_method date_class_methods[] = {
    { "distantFuture", "", (void *)date_distant_future },
    { "distantPast", "", (void *)date_distant_past },
    { "date", "", (void *)date_now },
    { "dateWithTimeIntervalSinceNow:", "", (void *)date_with_interval_since_now },
    { "dateWithTimeIntervalSince1970:", "", (void *)date_with_interval_since_1970 },
    { "dateWithTimeIntervalSinceReferenceDate:", "",
      (void *)date_with_interval_since_reference },
    { "dateWithTimeInterval:sinceDate:", "", (void *)date_with_interval_since_date },
};

static const plsr_method date_methods[] = {
    { "timeIntervalSinceNow", "", (void *)date_time_interval_since_now },
    { "timeIntervalSince1970", "", (void *)date_time_interval_since_1970 },
    { "timeIntervalSinceReferenceDate", "",
      (void *)date_time_interval_since_reference },
    { "timeIntervalSinceDate:", "", (void *)date_time_interval_since_date },
    { "dateByAddingTimeInterval:", "", (void *)date_by_adding_time_interval },
    { "addTimeInterval:", "", (void *)date_add_time_interval },
    { "compare:", "", (void *)date_compare },
    { "laterDate:", "", (void *)date_later_date },
    { "earlierDate:", "", (void *)date_earlier_date },
    { "isEqualToDate:", "", (void *)date_is_equal },
    { "isEqual:", "", (void *)date_is_equal_generic },
    { "hash", "", (void *)date_hash },
    { "description", "", (void *)date_description },
    { "descriptionWithLocale:", "", (void *)date_description_locale },
};

static const plsr_method url_class_methods[] = {
    { "fileURLWithPath:", "", (void *)url_class_file_url_with_path },
    { "fileURLWithPath:isDirectory:", "",
      (void *)url_class_file_url_with_path_is_directory },
    { "fileURLWithPath:relativeToURL:", "",
      (void *)url_class_file_url_with_path_relative },
    { "URLWithString:", "", (void *)url_class_with_string },
    { "URLWithString:relativeToURL:", "",
      (void *)url_class_with_string_relative },
};

static const plsr_method url_methods[] = {
    { "path", "", (void *)url_path },
    { "absolutePath", "", (void *)url_absolute_path },
    { "relativePath", "", (void *)url_relative_path },
    { "fileURL", "", (void *)url_file_url },
    { "absoluteString", "", (void *)url_absolute_string },
    { "fileSystemRepresentation", "", (void *)url_file_system_representation },
    { "isFileURL", "", (void *)url_is_file_url },
    { "URLByDeletingLastPathComponent", "",
      (void *)url_by_deleting_last_path_component },
    { "URLByAppendingPathComponent:", "",
      (void *)url_by_appending_path_component },
    { "URLByAppendingPathComponent:isDirectory:", "",
      (void *)url_by_appending_path_component_is_directory },
    { "URLByAppendingPathExtension:", "",
      (void *)url_by_appending_path_extension },
    { "URLByDeletingPathExtension", "",
      (void *)url_by_deleting_path_extension },
    { "URLByStandardizingPath", "", (void *)url_by_standardizing_path },
    { "standardizedURL", "", (void *)url_by_standardizing_path },
    { "URLByResolvingSymlinksInPath", "",
      (void *)url_by_resolving_symlinks },
    { "lastPathComponent", "", (void *)url_last_path_component },
    { "pathExtension", "", (void *)url_path_extension },
    { "pathComponents", "", (void *)url_path_components },
    { "isEqual:", "", (void *)url_is_equal },
    { "hash", "", (void *)url_hash },
    { "description", "", (void *)url_description },
    { "checkResourceIsReachableAndReturnError:", "",
      (void *)url_check_reachable },
    { "hasDirectoryPath", "", (void *)url_has_directory_path },
};

static const plsr_method *core_instance_methods(const char *name, uint32_t *n)
{
    if (!strcmp(name, "NSMutableDictionary")) {
        *n = sizeof(mutable_dict_methods) / sizeof(mutable_dict_methods[0]);
        return mutable_dict_methods;
    }
    if (!strcmp(name, "NSArray")) {
        *n = sizeof(array_enum_methods) / sizeof(array_enum_methods[0]);
        return array_enum_methods;
    }
    if (!strcmp(name, "NSKeyedArchiver")) {
        *n = sizeof(keyed_archiver_methods) / sizeof(keyed_archiver_methods[0]);
        return keyed_archiver_methods;
    }
    if (!strcmp(name, "NSKeyedUnarchiver")) {
        *n = sizeof(keyed_unarchiver_methods) / sizeof(keyed_unarchiver_methods[0]);
        return keyed_unarchiver_methods;
    }
    if (!strcmp(name, "NSDate")) {
        *n = sizeof(date_methods) / sizeof(date_methods[0]);
        return date_methods;
    }
    if (!strcmp(name, "NSURL")) {
        *n = sizeof(url_methods) / sizeof(url_methods[0]);
        return url_methods;
    }
    if (!strcmp(name, "NSBundle")) {
        *n = sizeof(bundle_methods) / sizeof(bundle_methods[0]);
        return bundle_methods;
    }
    if (!strcmp(name, "NSProcessInfo")) {
        *n = sizeof(process_info_methods) / sizeof(process_info_methods[0]);
        return process_info_methods;
    }
    if (!strcmp(name, "NSUserDefaults")) {
        *n = sizeof(defaults_methods) / sizeof(defaults_methods[0]);
        return defaults_methods;
    }
    *n = 0;
    return NULL;
}

static const plsr_method *core_class_methods(const char *name, uint32_t *n)
{
    if (!strcmp(name, "NSCharacterSet")) {
        *n = sizeof(character_set_class_methods) /
             sizeof(character_set_class_methods[0]);
        return character_set_class_methods;
    }
    if (!strcmp(name, "NSMutableSet")) {
        *n = sizeof(mutable_set_class_methods) /
             sizeof(mutable_set_class_methods[0]);
        return mutable_set_class_methods;
    }
    if (!strcmp(name, "NSMutableArray")) {
        *n = sizeof(mutable_array_class_methods) /
             sizeof(mutable_array_class_methods[0]);
        return mutable_array_class_methods;
    }
    if (!strcmp(name, "NSMutableDictionary")) {
        *n = sizeof(mutable_dict_class_methods) /
             sizeof(mutable_dict_class_methods[0]);
        return mutable_dict_class_methods;
    }
    if (!strcmp(name, "NSKeyedArchiver")) {
        *n = sizeof(keyed_archiver_class_methods) /
             sizeof(keyed_archiver_class_methods[0]);
        return keyed_archiver_class_methods;
    }
    if (!strcmp(name, "NSKeyedUnarchiver")) {
        *n = sizeof(keyed_unarchiver_class_methods) /
             sizeof(keyed_unarchiver_class_methods[0]);
        return keyed_unarchiver_class_methods;
    }
    if (!strcmp(name, "NSDate")) {
        *n = sizeof(date_class_methods) / sizeof(date_class_methods[0]);
        return date_class_methods;
    }
    if (!strcmp(name, "NSURL")) {
        *n = sizeof(url_class_methods) / sizeof(url_class_methods[0]);
        return url_class_methods;
    }
    if (!strcmp(name, "NSBundle")) {
        *n = sizeof(bundle_class_methods) / sizeof(bundle_class_methods[0]);
        return bundle_class_methods;
    }
    if (!strcmp(name, "NSProcessInfo")) {
        *n = sizeof(process_info_class_methods) / sizeof(process_info_class_methods[0]);
        return process_info_class_methods;
    }
    if (!strcmp(name, "NSUserDefaults")) {
        *n = sizeof(defaults_class_methods) / sizeof(defaults_class_methods[0]);
        return defaults_class_methods;
    }
    *n = 0;
    return NULL;
}

void *plsr_foundation_override(const char *name, const char *sel, int class_method)
{
    uint32_t n = 0;
    const plsr_method *m = class_method ? core_class_methods(name, &n)
                                        : core_instance_methods(name, &n);
    for (uint32_t i = 0; i < n; i++)
        if (m[i].name && !strcmp(m[i].name, sel))
            return m[i].imp;
    return NULL;
}

/* ==== END CORE METHODS ==== */
