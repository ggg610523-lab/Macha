/* Exercise libplsr-corefoundation.so from the host: toll-free bridging to
 * Foundation, strings, collections, numbers, data, sets, UUID/URL/bundle,
 * the run loop, notifications and preferences.  Handles are dlopen'd so the
 * test does not need the Darwin spelling at link time. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long CFTypeID;

#define CSTR(o, b) (str_get_cstr((o), (b), (long)sizeof(b), 0x08000100) ? (b) : "")

static int failures;
#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL: %s\n", msg);                           \
            failures++;                                                   \
        }                                                                 \
    } while (0)

struct cf_range {
    unsigned long location, length;
};

/* CFSTR object layout produced by clang: isa, flags, length, cstr. */
struct cfstr {
    void *isa;
    uint32_t flags;
    uint32_t len;
    const char *cstr;
};

static int ticks;
static void *loop_ref;
static void (*g_timer_invalidate)(void *);
static void (*g_stop)(void *);
static int entry_hits, exit_hits;
static int notif_hits;

static void timer_fired(void *timer, void *info)
{
    (void)info;
    ticks++;
    if (ticks == 2) {
        if (g_timer_invalidate)
            g_timer_invalidate(timer);
        if (g_stop)
            g_stop(loop_ref);
    }
}

static void observer_fired(void *observer, unsigned long activity, void *info)
{
    (void)observer;
    (void)info;
    if (activity == 1u)
        entry_hits++;
    else if (activity == 128u)
        exit_hits++;
}

static void notification_fired(void *center, void *observer, void *name,
                               void *object, void *userInfo)
{
    (void)center;
    (void)observer;
    (void)object;
    (void)userInfo;
    if (name)
        notif_hits++;
}

int main(int argc, char **argv)
{
    const char *objc_path = argc > 1 ? argv[1] : "build/libplsr-objc.so";
    const char *found_path = argc > 2 ? argv[2] : "build/libplsr-foundation.so";
    const char *cf_path = argc > 3 ? argv[3] : "build/libplsr-corefoundation.so";
    const char *app = argc > 4 ? argv[4] : NULL;
    void *hobjc, *hfound, *hcf;
    void *(*str_create)(void *, const char *, unsigned);
    unsigned char (*str_get_cstr)(void *, char *, long, unsigned);
    long (*str_length)(void *);
    long (*str_compare)(void *, void *, unsigned long);
    unsigned char (*str_has_prefix)(void *, void *);
    unsigned char (*str_has_suffix)(void *, void *);
    void *(*str_create_mutable)(void *, long);
    void (*str_append)(void *, void *);
    void *(*str_substring)(void *, void *, struct cf_range);
    void *(*str_copy)(void *, void *);
    void *(*array_create)(void *, const void **, long, const void *);
    void *(*array_create_mutable)(void *, long, const void *);
    long (*array_count)(void *);
    void *(*array_get)(void *, long);
    void (*array_append)(void *, const void *);
    void (*array_remove)(void *, long);
    void *(*dict_create)(void *, const void **, const void **, long, const void *,
                         const void *);
    void *(*dict_create_mutable)(void *, long, const void *, const void *);
    long (*dict_count)(void *);
    void *(*dict_get)(void *, const void *);
    void (*dict_set)(void *, const void *, const void *);
    void (*dict_remove)(void *, const void *);
    void *(*number_create)(void *, long, const void *);
    unsigned char (*number_get)(void *, long, void *);
    void *(*data_create)(void *, const unsigned char *, long);
    void (*data_append)(void *, const unsigned char *, long);
    long (*data_length)(void *);
    const unsigned char *(*data_bytes)(void *);
    void *(*set_create)(void *, const void **, long, const void *);
    long (*set_count)(void *);
    unsigned char (*set_contains)(void *, const void *);
    void (*set_add)(void *, const void *);
    void *(*uuid_create)(void *);
    void *(*uuid_string)(void *, void *);
    void *(*url_file)(void *, void *, long, unsigned char);
    void *(*url_get_string)(void *);
    void *(*url_copy_path)(void *, long);
    void *(*bundle_main)(void);
    void *(*bundle_id)(void *);
    void *(*bundle_exe)(void *);
    void *(*runloop_current)(void);
    long (*runloop_run)(void *, double, unsigned char);
    void (*runloop_stop)(void *);
    void (*runloop_add_timer)(void *, void *, void *);
    void (*runloop_add_observer)(void *, void *, void *);
    void *(*timer_create)(void *, double, double, unsigned long, long,
                          void (*)(void *, void *), void *);
    void (*timer_invalidate)(void *);
    void *(*observer_create)(void *, unsigned long, unsigned char, long,
                             void (*)(void *, unsigned long, void *), void *);
    void *(*nc_local)(void);
    void (*nc_add)(void *, void *, void (*)(void *, void *, void *, void *, void *),
                   void *, const void *, long);
    void (*nc_post)(void *, void *, const void *, void *, unsigned char);
    void (*pref_set)(void *, void *, void *);
    void *(*pref_copy)(void *, void *);
    unsigned char (*pref_bool)(void *, void *, unsigned char *);
    unsigned long (*type_id)(const void *);
    unsigned long (*type_string)(void);
    unsigned long (*type_array)(void);
    unsigned long (*type_dict)(void);
    unsigned long (*type_number)(void);
    unsigned long (*type_bool)(void);
    unsigned long (*type_data)(void);
    unsigned long (*type_set)(void);
    unsigned char (*cf_equal)(const void *, const void *);
    unsigned long (*cf_hash)(const void *);
    void *(*cf_retain)(const void *);
    void (*cf_release)(const void *);
    unsigned long (*cf_retain_count)(const void *);
    void *(*cf_description)(const void *);
    double (*abs_time)(void);
    char buf[256];
    void *s, *s2, *arr, *dict, *num, *data, *set, *uuid, *url, *bundle;
    void *loop, *timer, *observer, *center, *value;
    void *bool_true;
    unsigned long tid_string, tid_array, tid_dict, tid_number, tid_bool;
    unsigned long tid_data, tid_set;
    long long int_out = 0;
    double dbl_out = 0.0;

    hobjc = dlopen(objc_path, RTLD_NOW | RTLD_GLOBAL);
    hfound = dlopen(found_path, RTLD_NOW | RTLD_GLOBAL);
    hcf = dlopen(cf_path, RTLD_NOW | RTLD_GLOBAL);
    if (!hobjc || !hfound || !hcf) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 2;
    }
#define LOAD(var, name)                                                   \
    do {                                                                  \
        *(void **)(&var) = dlsym(hcf, name);                              \
        if (!var) {                                                       \
            fprintf(stderr, "missing %s\n", name);                        \
            return 2;                                                     \
        }                                                                 \
    } while (0)
    LOAD(str_create, "_CFStringCreateWithCString");
    LOAD(str_get_cstr, "_CFStringGetCString");
    LOAD(str_length, "_CFStringGetLength");
    LOAD(str_compare, "_CFStringCompare");
    LOAD(str_has_prefix, "_CFStringHasPrefix");
    LOAD(str_has_suffix, "_CFStringHasSuffix");
    LOAD(str_create_mutable, "_CFStringCreateMutable");
    LOAD(str_append, "_CFStringAppend");
    LOAD(str_substring, "_CFStringCreateWithSubstring");
    LOAD(str_copy, "_CFStringCreateCopy");
    LOAD(array_create, "_CFArrayCreate");
    LOAD(array_create_mutable, "_CFArrayCreateMutable");
    LOAD(array_count, "_CFArrayGetCount");
    LOAD(array_get, "_CFArrayGetValueAtIndex");
    LOAD(array_append, "_CFArrayAppendValue");
    LOAD(array_remove, "_CFArrayRemoveValueAtIndex");
    LOAD(dict_create, "_CFDictionaryCreate");
    LOAD(dict_create_mutable, "_CFDictionaryCreateMutable");
    LOAD(dict_count, "_CFDictionaryGetCount");
    LOAD(dict_get, "_CFDictionaryGetValue");
    LOAD(dict_set, "_CFDictionarySetValue");
    LOAD(dict_remove, "_CFDictionaryRemoveValue");
    LOAD(number_create, "_CFNumberCreate");
    LOAD(number_get, "_CFNumberGetValue");
    LOAD(data_create, "_CFDataCreate");
    LOAD(data_append, "_CFDataAppendBytes");
    LOAD(data_length, "_CFDataGetLength");
    LOAD(data_bytes, "_CFDataGetBytePtr");
    LOAD(set_create, "_CFSetCreate");
    LOAD(set_count, "_CFSetGetCount");
    LOAD(set_contains, "_CFSetContainsValue");
    LOAD(set_add, "_CFSetAddValue");
    LOAD(uuid_create, "_CFUUIDCreate");
    LOAD(uuid_string, "_CFUUIDCreateString");
    LOAD(url_file, "_CFURLCreateWithFileSystemPath");
    LOAD(url_get_string, "_CFURLGetString");
    LOAD(url_copy_path, "_CFURLCopyFileSystemPath");
    LOAD(bundle_main, "_CFBundleGetMainBundle");
    LOAD(bundle_id, "_CFBundleGetIdentifier");
    LOAD(bundle_exe, "_CFBundleCopyExecutableURL");
    LOAD(runloop_current, "_CFRunLoopGetCurrent");
    LOAD(runloop_run, "_CFRunLoopRunInMode");
    LOAD(runloop_stop, "_CFRunLoopStop");
    LOAD(runloop_add_timer, "_CFRunLoopAddTimer");
    LOAD(runloop_add_observer, "_CFRunLoopAddObserver");
    LOAD(timer_create, "_CFRunLoopTimerCreate");
    LOAD(timer_invalidate, "_CFRunLoopTimerInvalidate");
    LOAD(observer_create, "_CFRunLoopObserverCreate");
    LOAD(nc_local, "_CFNotificationCenterGetLocalCenter");
    LOAD(nc_add, "_CFNotificationCenterAddObserver");
    LOAD(nc_post, "_CFNotificationCenterPostNotificationName");
    LOAD(pref_set, "_CFPreferencesSetAppValue");
    LOAD(pref_copy, "_CFPreferencesCopyAppValue");
    LOAD(pref_bool, "_CFPreferencesGetAppBooleanValue");
    LOAD(type_id, "_CFGetTypeID");
    LOAD(type_string, "_CFStringGetTypeID");
    LOAD(type_array, "_CFArrayGetTypeID");
    LOAD(type_dict, "_CFDictionaryGetTypeID");
    LOAD(type_number, "_CFNumberGetTypeID");
    LOAD(type_bool, "_CFBooleanGetTypeID");
    LOAD(type_data, "_CFDataGetTypeID");
    LOAD(type_set, "_CFSetGetTypeID");
    LOAD(cf_equal, "_CFEqual");
    LOAD(cf_hash, "_CFHash");
    LOAD(cf_retain, "_CFRetain");
    LOAD(cf_release, "_CFRelease");
    LOAD(cf_retain_count, "_CFGetRetainCount");
    LOAD(cf_description, "_CFCopyDescription");
    LOAD(abs_time, "_CFAbsoluteTimeGetCurrent");
#undef LOAD
    {
        void **sym = (void **)dlsym(hcf, "_kCFBooleanTrue");
        bool_true = sym ? *sym : NULL;
    }

    tid_string = type_string();
    tid_array = type_array();
    tid_dict = type_dict();
    tid_number = type_number();
    tid_bool = type_bool();
    tid_data = type_data();
    tid_set = type_set();

    /* strings */
    s = str_create(NULL, "hello", 0x08000100);
    CHECK(s != NULL, "CFStringCreateWithCString returned NULL");
    CHECK(type_id(s) == tid_string, "string type ID");
    CHECK(str_length(s) == 5, "string length");
    CHECK(str_get_cstr(s, buf, (long)sizeof buf, 0x08000100) && !strcmp(buf, "hello"),
          "string roundtrip");
    s2 = str_create(NULL, "hello", 0x08000100);
    CHECK(str_compare(s, s2, 0) == 0, "equal strings compare equal");
    CHECK(str_compare(s, str_create(NULL, "help", 0x08000100), 0) != 0,
          "different strings compare non-zero");
    CHECK(str_has_prefix(s, str_create(NULL, "he", 0x08000100)), "prefix");
    CHECK(str_has_suffix(s, str_create(NULL, "lo", 0x08000100)), "suffix");
    {
        void *m = str_create_mutable(NULL, 0);
        str_append(m, s);
        str_append(m, str_create(NULL, ", world", 0x08000100));
        str_get_cstr(m, buf, (long)sizeof buf, 0x08000100);
        CHECK(!strcmp(buf, "hello, world"), "mutable append");
        CHECK(str_length(m) == 12, "mutable length");
    }
    {
        struct cf_range r = { 1, 3 };
        void *sub = str_substring(NULL, s, r);
        str_get_cstr(sub, buf, (long)sizeof buf, 0x08000100);
        CHECK(!strcmp(buf, "ell"), "substring");
    }
    {
        void *copy = str_copy(NULL, s);
        CHECK(copy != s && !strcmp(CSTR(copy, buf), "hello"), "string copy");
    }

    /* arrays */
    {
        const void *items[3];
        items[0] = str_create(NULL, "a", 0x08000100);
        items[1] = str_create(NULL, "b", 0x08000100);
        items[2] = str_create(NULL, "c", 0x08000100);
        arr = array_create(NULL, items, 3, NULL);
        CHECK(arr != NULL && type_id(arr) == tid_array, "array type");
        CHECK(array_count(arr) == 3, "array count");
        CHECK(!strcmp(CSTR(array_get(arr, 1), buf), "b"), "array element");
    }
    {
        void *m = array_create_mutable(NULL, 0, NULL);
        array_append(m, str_create(NULL, "x", 0x08000100));
        array_append(m, str_create(NULL, "y", 0x08000100));
        array_remove(m, 0);
        CHECK(array_count(m) == 1, "mutable array remove");
        CHECK(!strcmp(CSTR(array_get(m, 0), buf), "y"), "mutable array element");
    }

    /* dictionaries */
    {
        const void *keys[2], *vals[2];
        keys[0] = str_create(NULL, "one", 0x08000100);
        keys[1] = str_create(NULL, "two", 0x08000100);
        vals[0] = str_create(NULL, "1", 0x08000100);
        vals[1] = str_create(NULL, "2", 0x08000100);
        dict = dict_create(NULL, keys, vals, 2, NULL, NULL);
        CHECK(dict != NULL && type_id(dict) == tid_dict, "dictionary type");
        CHECK(dict_count(dict) == 2, "dictionary count");
        CHECK(!strcmp(CSTR(dict_get(dict, keys[1]), buf), "2"),
              "dictionary value");
    }
    {
        void *m = dict_create_mutable(NULL, 0, NULL, NULL);
        void *k = str_create(NULL, "k", 0x08000100);
        dict_set(m, k, str_create(NULL, "v", 0x08000100));
        CHECK(dict_get(m, k) != NULL, "mutable dictionary set");
        dict_remove(m, k);
        CHECK(dict_get(m, k) == NULL, "mutable dictionary remove");
    }

    /* numbers and booleans */
    {
        long long v = 42;
        num = number_create(NULL, 4, &v); /* kCFNumberSInt64Type */
        CHECK(num != NULL && type_id(num) == tid_number, "number type");
        CHECK(number_get(num, 4, &int_out) && int_out == 42, "number int value");
        dbl_out = 0.0;
        CHECK(number_get(num, 6, &dbl_out) && dbl_out == 42.0, "number double value");
    }
    {
        double d = 1.5;
        void *dn = number_create(NULL, 6, &d);
        CHECK(number_get(dn, 6, &dbl_out) && dbl_out == 1.5, "double number");
    }
    CHECK(bool_true != NULL, "kCFBooleanTrue exported");
    CHECK(type_id(bool_true) == tid_bool, "boolean type ID");
    {
        unsigned char (*bool_get)(void *) =
            (unsigned char (*)(void *))dlsym(hcf, "_CFBooleanGetValue");
        CHECK(bool_get && bool_get(bool_true), "boolean value");
    }

    /* data */
    data = data_create(NULL, (const unsigned char *)"abc", 3);
    data_append(data, (const unsigned char *)"de", 2);
    CHECK(data != NULL && type_id(data) == tid_data, "data type");
    CHECK(data_length(data) == 5, "data length");
    CHECK(data_bytes(data)[3] == 'd', "data bytes");

    /* sets */
    {
        const void *items[2];
        items[0] = str_create(NULL, "a", 0x08000100);
        items[1] = str_create(NULL, "b", 0x08000100);
        set = set_create(NULL, items, 2, NULL);
        CHECK(set != NULL && type_id(set) == tid_set, "set type");
        CHECK(set_count(set) == 2, "set count");
        set_add(set, str_create(NULL, "a", 0x08000100));
        CHECK(set_count(set) == 2, "set deduplicates");
        CHECK(set_contains(set, items[1]), "set contains");
    }

    /* UUID and URL */
    uuid = uuid_create(NULL);
    CHECK(uuid != NULL, "uuid create");
    CHECK(str_length(uuid_string(NULL, uuid)) == 36, "uuid string length");
    url = url_file(NULL, str_create(NULL, "/tmp/x", 0x08000100), 0, 0);
    CHECK(url != NULL, "url create");
    CHECK(!strcmp(CSTR(url_get_string(url), buf), "file:///tmp/x"), "url string");
    CHECK(!strcmp(CSTR(url_copy_path(url, 0), buf), "/tmp/x"), "url path");

    /* bundle */
    if (app)
        setenv("PLSR_APP_BUNDLE", app, 1);
    bundle = bundle_main();
    CHECK(bundle != NULL, "main bundle");
    if (bundle) {
        void *ident = bundle_id(bundle);
        void *exe = bundle_exe(bundle);
        CHECK(ident != NULL && *CSTR(ident, buf), "bundle identifier");
        CHECK(exe != NULL, "bundle executable URL");
        if (exe) {
            (void)str_get_cstr(url_copy_path(exe, 0), buf, (long)sizeof buf,
                               0x08000100);
            CHECK(strstr(buf, "Contents/MacOS/") != NULL, "executable path");
        }
    }

    /* run loop: repeating timer stops the loop on its second fire */
    loop = runloop_current();
    CHECK(loop != NULL, "current run loop");
    loop_ref = loop;
    g_timer_invalidate = timer_invalidate;
    g_stop = runloop_stop;
    timer = timer_create(NULL, abs_time() + 0.01, 0.01, 0, 0, timer_fired,
                         (void *)timer_invalidate);
    CHECK(timer != NULL, "timer create");
    runloop_add_timer(loop, timer, NULL);
    ticks = 0;
    CHECK(runloop_run(loop, 2.0, 0) == 2, "timer run returns stopped");
    CHECK(ticks == 2, "timer fired twice");

    /* no timers left: the loop finishes immediately */
    CHECK(runloop_run(loop, 0.05, 0) == 1, "empty run loop finishes");

    /* observers see entry and exit */
    observer = observer_create(NULL, 1u | 128u, 1, 0, observer_fired, NULL);
    CHECK(observer != NULL, "observer create");
    runloop_add_observer(loop, observer, NULL);
    entry_hits = exit_hits = 0;
    runloop_run(loop, 0.01, 0);
    CHECK(entry_hits == 1 && exit_hits == 1, "observer entry/exit");

    /* notification center */
    center = nc_local();
    CHECK(center != NULL, "local notification center");
    nc_add(center, (void *)&notif_hits, notification_fired, NULL, NULL, 0);
    notif_hits = 0;
    nc_post(center, str_create(NULL, "TestNote", 0x08000100), NULL, NULL, 0);
    CHECK(notif_hits == 1, "notification delivered");

    /* preferences */
    pref_set(str_create(NULL, "plsr.key", 0x08000100),
             str_create(NULL, "plsr.value", 0x08000100),
             str_create(NULL, "com.plsr.test", 0x08000100));
    value = pref_copy(str_create(NULL, "plsr.key", 0x08000100),
                      str_create(NULL, "com.plsr.test", 0x08000100));
    CHECK(value != NULL && !strcmp(CSTR(value, buf), "plsr.value"),
          "preference roundtrip");
    pref_set(str_create(NULL, "plsr.flag", 0x08000100), bool_true,
             str_create(NULL, "com.plsr.test", 0x08000100));
    {
        unsigned char valid = 0;
        CHECK(pref_bool(str_create(NULL, "plsr.flag", 0x08000100),
                        str_create(NULL, "com.plsr.test", 0x08000100),
                        &valid) == 1 &&
                  valid,
              "preference boolean");
    }

    /* object model */
    CHECK(tid_string != tid_array && tid_array != tid_dict &&
              tid_dict != tid_number && tid_number != tid_bool &&
              tid_bool != tid_data && tid_data != tid_set,
          "type IDs are distinct");
    CHECK(cf_equal(s, s2), "CFEqual equal strings");
    CHECK(!cf_equal(s, str_create(NULL, "world", 0x08000100)), "CFEqual different");
    CHECK(cf_hash(s) == cf_hash(s2), "CFHash stable for equal strings");
    CHECK(cf_retain(s) == s, "CFRetain returns the object");
    CHECK(cf_retain_count(s) == 1, "CFGetRetainCount");
    cf_release(s);
    CHECK(str_get_cstr(cf_description(s), buf, (long)sizeof buf, 0x08000100) &&
              !strcmp(buf, "hello"),
          "CFCopyDescription");

    /* CFSTR layout through ___CFConstantStringClassReference */
    {
        void *cls = dlsym(hcf, "___CFConstantStringClassReference");
        struct cfstr constant = { cls, 0x07c8u, 5u, "hello" };
        CHECK(cls != NULL, "CFSTR class reference exported");
        CHECK(type_id(&constant) == tid_string, "CFSTR type ID");
        CHECK(str_length(&constant) == 5, "CFSTR length");
        CHECK(str_get_cstr(&constant, buf, (long)sizeof buf, 0x08000100) &&
                  !strcmp(buf, "hello"),
              "CFSTR contents");
        CHECK(str_compare(&constant, s, 0) == 0, "CFSTR compares to NSString");
    }

    if (failures) {
        fprintf(stderr, "%d CoreFoundation check(s) failed\n", failures);
        return 1;
    }
    printf("PASS: CoreFoundation strings, collections, run loop, bundle and preferences\n");
    return 0;
}
