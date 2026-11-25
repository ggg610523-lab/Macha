/*
 * Clean-room CoreFoundation slice.
 *
 * CoreFoundation sits below Foundation in the macOS stack but the two share
 * objects through toll-free bridging.  This slice mirrors that: CFStringRef is
 * an NSString/NSMutableString, CFArrayRef an NSArray/NSMutableArray,
 * CFDictionaryRef an NSDictionary/NSMutableDictionary, CFNumberRef an
 * NSNumber, CFNullRef the NSNull singleton, and CFBooleanRef a small NSNumber
 * subclass.  CFSet, CFData, CFUUID, CFURL, CFRunLoop, CFNotificationCenter and
 * the run-loop objects have their own storage because Foundation has no exact
 * counterpart here.
 *
 * CFSTR() constants get a real __NSCFConstantString class exported under the
 * symbol name ___CFConstantStringClassReference so compiled CF code can create
 * constant strings with the Darwin layout { isa, flags, length, cstr }.
 *
 * Everything is intentionally small: UTF-8 strings, linear collections, a
 * single-threaded run loop with timers/observers/sources, an in-memory
 * preference store and bundle lookups through NSBundle.
 */
#define _GNU_SOURCE
#include "objc_abi.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

void plsr_objc_register_class(plsr_class *c);
void *plsr_objc_lookup_imp(void *obj, const char *sel);
void *objc_getClass(const char *name) __asm__("_objc_getClass");

/* ---- shared Foundation object layouts ---- */

typedef struct {
    void *isa;
    char *cstr;
    unsigned long len;
    unsigned long cap;
} cf_string;

typedef struct {
    void *isa;
    unsigned long count;
    void **items;
} cf_array;

typedef struct {
    void *isa;
    unsigned long count;
    void **keys;
    void **values;
} cf_dict;

typedef struct {
    void *isa;
    long long i;
    double d;
    int is_double;
} cf_number;

typedef struct {
    void *isa;
    unsigned char *bytes;
    unsigned long len;
    unsigned long cap;
} cf_data;

typedef struct {
    void *isa;
    unsigned long count;
    unsigned long cap;
    void **items;
} cf_set;

typedef struct {
    void *isa;
    unsigned char bytes[16];
} cf_uuid;

typedef struct {
    void *isa;
    char *string;
    char *path;
    int is_file;
} cf_url;

typedef struct {
    unsigned long location;
    unsigned long length;
} cf_range;

typedef struct {
    void *isa;
    long long i;
    double d;
    int is_double;
} cf_boolean;

/* ---- class symbols ---- */

plsr_class plsr_cfconst_class __asm__("___CFConstantStringClassReference");
static plsr_class cfconst_meta;

static plsr_class plsr_cfbool_class;
static plsr_class plsr_cfbool_meta;
static plsr_class plsr_cfdata_class;
static plsr_class plsr_cfdata_meta;
static plsr_class plsr_cfset_class;
static plsr_class plsr_cfset_meta;
static plsr_class plsr_cfuuid_class;
static plsr_class plsr_cfuuid_meta;
static plsr_class plsr_cfurl_class;
static plsr_class plsr_cfurl_meta;
static plsr_class plsr_cfrunloop_class;
static plsr_class plsr_cfrunloop_meta;
static plsr_class plsr_cfnc_class;
static plsr_class plsr_cfnc_meta;
static plsr_class plsr_cftimer_class;
static plsr_class plsr_cftimer_meta;
static plsr_class plsr_cfobserver_class;
static plsr_class plsr_cfobserver_meta;
static plsr_class plsr_cfsource_class;
static plsr_class plsr_cfsource_meta;
static plsr_class plsr_cfalloc_class;
static plsr_class plsr_cfalloc_meta;

extern plsr_class plsr_NSString_class __asm__("_OBJC_CLASS_$_NSString");
extern plsr_class plsr_NSString_meta __asm__("_OBJC_METACLASS_$_NSString");
extern plsr_class plsr_NSArray_class __asm__("_OBJC_CLASS_$_NSArray");
extern plsr_class plsr_NSArray_meta __asm__("_OBJC_METACLASS_$_NSArray");
extern plsr_class plsr_NSDictionary_class __asm__("_OBJC_CLASS_$_NSDictionary");
extern plsr_class plsr_NSDictionary_meta __asm__("_OBJC_METACLASS_$_NSDictionary");
extern plsr_class plsr_NSNumber_class __asm__("_OBJC_CLASS_$_NSNumber");
extern plsr_class plsr_NSNumber_meta __asm__("_OBJC_METACLASS_$_NSNumber");
extern plsr_class plsr_NSData_class __asm__("_OBJC_CLASS_$_NSData");
extern plsr_class plsr_NSData_meta __asm__("_OBJC_METACLASS_$_NSData");
extern plsr_class plsr_NSObject_class __asm__("_OBJC_CLASS_$_NSObject");
extern plsr_class plsr_NSObject_meta __asm__("_OBJC_METACLASS_$_NSObject");
extern plsr_class plsr_NSNull_class __asm__("_OBJC_CLASS_$_NSNull");

/* ---- function declarations (leading-underscore Darwin names) ---- */

void *CFRetain(const void *obj) __asm__("_CFRetain");
void CFRelease(const void *obj) __asm__("_CFRelease");
const void *CFAutorelease(const void *obj) __asm__("_CFAutorelease");
unsigned long CFGetRetainCount(const void *obj) __asm__("_CFGetRetainCount");
unsigned char CFEqual(const void *a, const void *b) __asm__("_CFEqual");
unsigned long CFHash(const void *obj) __asm__("_CFHash");
void *CFCopyDescription(const void *obj) __asm__("_CFCopyDescription");
void CFShow(const void *obj) __asm__("_CFShow");
unsigned long CFGetTypeID(const void *obj) __asm__("_CFGetTypeID");
void *CFGetAllocator(const void *obj) __asm__("_CFGetAllocator");
void *CFCopyTypeIDDescription(unsigned long type) __asm__("_CFCopyTypeIDDescription");

void *CFAllocatorGetDefault(void) __asm__("_CFAllocatorGetDefault");
void CFAllocatorSetDefault(void *alloc) __asm__("_CFAllocatorSetDefault");
unsigned long CFAllocatorGetPreferredSizeForSize(void *alloc, unsigned long size,
                                                 unsigned long hint)
    __asm__("_CFAllocatorGetPreferredSizeForSize");
void *CFAllocatorAllocate(void *alloc, unsigned long size, unsigned long hint)
    __asm__("_CFAllocatorAllocate");
void CFAllocatorDeallocate(void *alloc, void *p) __asm__("_CFAllocatorDeallocate");
void *CFAllocatorReallocate(void *alloc, void *p, unsigned long size,
                            unsigned long hint)
    __asm__("_CFAllocatorReallocate");
unsigned long CFAllocatorGetTypeID(void) __asm__("_CFAllocatorGetTypeID");

unsigned long CFStringGetTypeID(void) __asm__("_CFStringGetTypeID");
void *CFStringCreateWithCString(void *alloc, const char *cstr,
                                unsigned encoding) __asm__("_CFStringCreateWithCString");
void *CFStringCreateWithCStringNoCopy(void *alloc, const char *cstr,
                                      unsigned encoding, void *contentsDeallocator)
    __asm__("_CFStringCreateWithCStringNoCopy");
void *CFStringCreateWithBytes(void *alloc, const unsigned char *bytes,
                              long numBytes, unsigned encoding,
                              unsigned char external)
    __asm__("_CFStringCreateWithBytes");
void *CFStringCreateWithBytesNoCopy(void *alloc, const unsigned char *bytes,
                                    long numBytes, unsigned encoding,
                                    unsigned char external,
                                    void *contentsDeallocator)
    __asm__("_CFStringCreateWithBytesNoCopy");
void *CFStringCreateWithCharacters(void *alloc, const unsigned short *chars,
                                   long numChars)
    __asm__("_CFStringCreateWithCharacters");
void *CFStringCreateWithSubstring(void *alloc, void *str, cf_range range)
    __asm__("_CFStringCreateWithSubstring");
void *CFStringCreateCopy(void *alloc, void *str) __asm__("_CFStringCreateCopy");
void *CFStringCreateMutable(void *alloc, long maxLength)
    __asm__("_CFStringCreateMutable");
void *CFStringCreateMutableCopy(void *alloc, long maxLength, void *str)
    __asm__("_CFStringCreateMutableCopy");
unsigned char CFStringGetCString(void *str, char *buffer, long bufferSize,
                                 unsigned encoding)
    __asm__("_CFStringGetCString");
const char *CFStringGetCStringPtr(void *str, unsigned encoding)
    __asm__("_CFStringGetCStringPtr");
long CFStringGetLength(void *str) __asm__("_CFStringGetLength");
long CFStringGetMaximumSizeForEncoding(long length, unsigned encoding)
    __asm__("_CFStringGetMaximumSizeForEncoding");
void CFStringGetCharacters(void *str, cf_range range, unsigned short *buffer)
    __asm__("_CFStringGetCharacters");
unsigned short CFStringGetCharacterAtIndex(void *str, unsigned long idx)
    __asm__("_CFStringGetCharacterAtIndex");
void CFStringAppend(void *str, void *other) __asm__("_CFStringAppend");
void CFStringAppendCString(void *str, const char *cstr, unsigned encoding)
    __asm__("_CFStringAppendCString");
void CFStringAppendFormat(void *str, void *format, ...) __asm__("_CFStringAppendFormat");
void CFStringInsert(void *str, long idx, void *other) __asm__("_CFStringInsert");
void CFStringDelete(void *str, cf_range range) __asm__("_CFStringDelete");
void CFStringReplace(void *str, cf_range range, void *replacement)
    __asm__("_CFStringReplace");
long CFStringCompare(void *a, void *b, unsigned long options)
    __asm__("_CFStringCompare");
long CFStringCompareWithOptions(void *a, void *b, cf_range range,
                                unsigned long options)
    __asm__("_CFStringCompareWithOptions");
unsigned char CFStringHasPrefix(void *str, void *prefix)
    __asm__("_CFStringHasPrefix");
unsigned char CFStringHasSuffix(void *str, void *suffix)
    __asm__("_CFStringHasSuffix");
unsigned char CFStringFind(void *str, void *needle, unsigned long options,
                           cf_range *found) __asm__("_CFStringFind");
void *CFStringLowercase(void *str, void *locale) __asm__("_CFStringLowercase");
void *CFStringUppercase(void *str, void *locale) __asm__("_CFStringUppercase");
long CFStringGetIntValue(void *str) __asm__("_CFStringGetIntValue");
double CFStringGetDoubleValue(void *str) __asm__("_CFStringGetDoubleValue");
void *CFStringCreateWithFormat(void *alloc, void *format, ...)
    __asm__("_CFStringCreateWithFormat");

unsigned long CFArrayGetTypeID(void) __asm__("_CFArrayGetTypeID");
void *CFArrayCreate(void *alloc, const void **values, long numValues,
                    const void *callbacks) __asm__("_CFArrayCreate");
void *CFArrayCreateMutable(void *alloc, long capacity, const void *callbacks)
    __asm__("_CFArrayCreateMutable");
void *CFArrayCreateCopy(void *alloc, void *array) __asm__("_CFArrayCreateCopy");
void *CFArrayCreateMutableCopy(void *alloc, long capacity, void *array)
    __asm__("_CFArrayCreateMutableCopy");
long CFArrayGetCount(void *array) __asm__("_CFArrayGetCount");
void *CFArrayGetValueAtIndex(void *array, long idx)
    __asm__("_CFArrayGetValueAtIndex");
void CFArrayGetValues(void *array, cf_range range, const void **values)
    __asm__("_CFArrayGetValues");
void CFArrayAppendValue(void *array, const void *value)
    __asm__("_CFArrayAppendValue");
void CFArrayAppendArray(void *array, void *other, cf_range range)
    __asm__("_CFArrayAppendArray");
void CFArraySetValueAtIndex(void *array, long idx, const void *value)
    __asm__("_CFArraySetValueAtIndex");
void CFArrayInsertValueAtIndex(void *array, long idx, const void *value)
    __asm__("_CFArrayInsertValueAtIndex");
void CFArrayRemoveValueAtIndex(void *array, long idx)
    __asm__("_CFArrayRemoveValueAtIndex");
void CFArrayRemoveAllValues(void *array) __asm__("_CFArrayRemoveAllValues");
unsigned char CFArrayContainsValue(void *array, cf_range range, const void *value)
    __asm__("_CFArrayContainsValue");
long CFArrayGetFirstIndexOfValue(void *array, cf_range range, const void *value)
    __asm__("_CFArrayGetFirstIndexOfValue");

unsigned long CFDictionaryGetTypeID(void) __asm__("_CFDictionaryGetTypeID");
void *CFDictionaryCreate(void *alloc, const void **keys, const void **values,
                         long numValues, const void *keyCallbacks,
                         const void *valueCallbacks)
    __asm__("_CFDictionaryCreate");
void *CFDictionaryCreateMutable(void *alloc, long capacity,
                                const void *keyCallbacks,
                                const void *valueCallbacks)
    __asm__("_CFDictionaryCreateMutable");
void *CFDictionaryCreateCopy(void *alloc, void *dict)
    __asm__("_CFDictionaryCreateCopy");
void *CFDictionaryCreateMutableCopy(void *alloc, long capacity, void *dict)
    __asm__("_CFDictionaryCreateMutableCopy");
long CFDictionaryGetCount(void *dict) __asm__("_CFDictionaryGetCount");
void *CFDictionaryGetValue(void *dict, const void *key)
    __asm__("_CFDictionaryGetValue");
unsigned char CFDictionaryGetValueIfPresent(void *dict, const void *key,
                                            const void **value)
    __asm__("_CFDictionaryGetValueIfPresent");
unsigned char CFDictionaryContainsKey(void *dict, const void *key)
    __asm__("_CFDictionaryContainsKey");
void CFDictionarySetValue(void *dict, const void *key, const void *value)
    __asm__("_CFDictionarySetValue");
void CFDictionaryAddValue(void *dict, const void *key, const void *value)
    __asm__("_CFDictionaryAddValue");
void CFDictionaryReplaceValue(void *dict, const void *key, const void *value)
    __asm__("_CFDictionaryReplaceValue");
void CFDictionaryRemoveValue(void *dict, const void *key)
    __asm__("_CFDictionaryRemoveValue");
void CFDictionaryRemoveAllValues(void *dict)
    __asm__("_CFDictionaryRemoveAllValues");
void CFDictionaryGetKeysAndValues(void *dict, const void **keys,
                                  const void **values)
    __asm__("_CFDictionaryGetKeysAndValues");

unsigned long CFNumberGetTypeID(void) __asm__("_CFNumberGetTypeID");
void *CFNumberCreate(void *alloc, long type, const void *valuePtr)
    __asm__("_CFNumberCreate");
unsigned char CFNumberGetValue(void *number, long type, void *valuePtr)
    __asm__("_CFNumberGetValue");
long CFNumberGetType(void *number) __asm__("_CFNumberGetType");
long CFNumberGetByteSize(void *number) __asm__("_CFNumberGetByteSize");
unsigned char CFNumberIsFloatType(void *number) __asm__("_CFNumberIsFloatType");

unsigned long CFBooleanGetTypeID(void) __asm__("_CFBooleanGetTypeID");
unsigned char CFBooleanGetValue(void *boolean) __asm__("_CFBooleanGetValue");

unsigned long CFDataGetTypeID(void) __asm__("_CFDataGetTypeID");
void *CFDataCreate(void *alloc, const unsigned char *bytes, long length)
    __asm__("_CFDataCreate");
void *CFDataCreateMutable(void *alloc, long capacity)
    __asm__("_CFDataCreateMutable");
void *CFDataCreateCopy(void *alloc, void *data) __asm__("_CFDataCreateCopy");
void *CFDataCreateMutableCopy(void *alloc, long capacity, void *data)
    __asm__("_CFDataCreateMutableCopy");
long CFDataGetLength(void *data) __asm__("_CFDataGetLength");
const unsigned char *CFDataGetBytePtr(void *data) __asm__("_CFDataGetBytePtr");
unsigned char *CFDataGetMutableBytePtr(void *data)
    __asm__("_CFDataGetMutableBytePtr");
void CFDataAppendBytes(void *data, const unsigned char *bytes, long length)
    __asm__("_CFDataAppendBytes");
void CFDataSetLength(void *data, long length) __asm__("_CFDataSetLength");
void CFDataReplaceBytes(void *data, cf_range range, const unsigned char *bytes,
                        long length) __asm__("_CFDataReplaceBytes");
void CFDataDeleteBytes(void *data, cf_range range)
    __asm__("_CFDataDeleteBytes");

unsigned long CFSetGetTypeID(void) __asm__("_CFSetGetTypeID");
void *CFSetCreate(void *alloc, const void **values, long numValues,
                  const void *callbacks) __asm__("_CFSetCreate");
void *CFSetCreateMutable(void *alloc, long capacity, const void *callbacks)
    __asm__("_CFSetCreateMutable");
void *CFSetCreateCopy(void *alloc, void *set) __asm__("_CFSetCreateCopy");
long CFSetGetCount(void *set) __asm__("_CFSetGetCount");
unsigned char CFSetContainsValue(void *set, const void *value)
    __asm__("_CFSetContainsValue");
void *CFSetGetValue(void *set, const void *value) __asm__("_CFSetGetValue");
void CFSetGetValues(void *set, const void **values) __asm__("_CFSetGetValues");
void CFSetAddValue(void *set, const void *value) __asm__("_CFSetAddValue");
void CFSetSetValue(void *set, const void *value) __asm__("_CFSetSetValue");
void CFSetRemoveValue(void *set, const void *value) __asm__("_CFSetRemoveValue");
void CFSetRemoveAllValues(void *set) __asm__("_CFSetRemoveAllValues");

unsigned long CFUUIDGetTypeID(void) __asm__("_CFUUIDGetTypeID");
void *CFUUIDCreate(void *alloc) __asm__("_CFUUIDCreate");
void *CFUUIDCreateWithBytes(void *alloc, unsigned char b0, unsigned char b1,
                            unsigned char b2, unsigned char b3, unsigned char b4,
                            unsigned char b5, unsigned char b6, unsigned char b7,
                            unsigned char b8, unsigned char b9, unsigned char b10,
                            unsigned char b11, unsigned char b12, unsigned char b13,
                            unsigned char b14, unsigned char b15)
    __asm__("_CFUUIDCreateWithBytes");
void *CFUUIDCreateString(void *alloc, void *uuid) __asm__("_CFUUIDCreateString");
void CFUUIDGetUUIDBytes(void *uuid, unsigned char out[16])
    __asm__("_CFUUIDGetUUIDBytes");

unsigned long CFURLGetTypeID(void) __asm__("_CFURLGetTypeID");
void *CFURLCreateWithString(void *alloc, void *string, void *baseURL)
    __asm__("_CFURLCreateWithString");
void *CFURLCreateWithFileSystemPath(void *alloc, void *path, long style,
                                    unsigned char isDirectory)
    __asm__("_CFURLCreateWithFileSystemPath");
void *CFURLCreateFromFileSystemRepresentation(void *alloc,
                                              const unsigned char *buffer,
                                              long length,
                                              unsigned char isDirectory)
    __asm__("_CFURLCreateFromFileSystemRepresentation");
void *CFURLCopyAbsoluteURL(void *url) __asm__("_CFURLCopyAbsoluteURL");
void *CFURLGetString(void *url) __asm__("_CFURLGetString");
void *CFURLCopyFileSystemPath(void *url, long style)
    __asm__("_CFURLCopyFileSystemPath");
unsigned char CFURLGetFileSystemRepresentation(void *url,
                                               unsigned char resolve,
                                               unsigned char *buffer,
                                               long maxLen)
    __asm__("_CFURLGetFileSystemRepresentation");
unsigned char CFURLIsFileURL(void *url) __asm__("_CFURLIsFileURL");
void *CFURLCreateCopyAppendingPathComponent(void *alloc, void *url,
                                            void *component,
                                            unsigned char isDirectory)
    __asm__("_CFURLCreateCopyAppendingPathComponent");
void *CFURLCreateCopyDeletingLastPathComponent(void *alloc, void *url)
    __asm__("_CFURLCreateCopyDeletingLastPathComponent");

unsigned long CFBundleGetTypeID(void) __asm__("_CFBundleGetTypeID");
void *CFBundleGetMainBundle(void) __asm__("_CFBundleGetMainBundle");
void *CFBundleGetIdentifier(void *bundle) __asm__("_CFBundleGetIdentifier");
void *CFBundleGetValueForInfoDictionaryKey(void *bundle, void *key)
    __asm__("_CFBundleGetValueForInfoDictionaryKey");
void *CFBundleCopyBundleURL(void *bundle) __asm__("_CFBundleCopyBundleURL");
void *CFBundleCopyExecutableURL(void *bundle) __asm__("_CFBundleCopyExecutableURL");
void *CFBundleCopyResourceURL(void *bundle, void *resourceName,
                              void *resourceType, void *subDir)
    __asm__("_CFBundleCopyResourceURL");
void *CFBundleCopyLocalizedString(void *bundle, void *key, void *value,
                                  void *tableName)
    __asm__("_CFBundleCopyLocalizedString");

unsigned long CFRunLoopGetTypeID(void) __asm__("_CFRunLoopGetTypeID");
void *CFRunLoopGetCurrent(void) __asm__("_CFRunLoopGetCurrent");
void *CFRunLoopGetMain(void) __asm__("_CFRunLoopGetMain");
void CFRunLoopRun(void) __asm__("_CFRunLoopRun");
long CFRunLoopRunInMode(void *mode, double seconds,
                        unsigned char returnAfterSourceHandled)
    __asm__("_CFRunLoopRunInMode");
void CFRunLoopStop(void *loop) __asm__("_CFRunLoopStop");
void CFRunLoopWakeUp(void *loop) __asm__("_CFRunLoopWakeUp");
unsigned char CFRunLoopIsWaiting(void *loop) __asm__("_CFRunLoopIsWaiting");
void CFRunLoopAddTimer(void *loop, void *timer, void *mode)
    __asm__("_CFRunLoopAddTimer");
void CFRunLoopRemoveTimer(void *loop, void *timer, void *mode)
    __asm__("_CFRunLoopRemoveTimer");
void CFRunLoopAddObserver(void *loop, void *observer, void *mode)
    __asm__("_CFRunLoopAddObserver");
void CFRunLoopRemoveObserver(void *loop, void *observer, void *mode)
    __asm__("_CFRunLoopRemoveObserver");
void CFRunLoopAddSource(void *loop, void *source, void *mode)
    __asm__("_CFRunLoopAddSource");
void CFRunLoopRemoveSource(void *loop, void *source, void *mode)
    __asm__("_CFRunLoopRemoveSource");
void *CFRunLoopCopyCurrentMode(void *loop) __asm__("_CFRunLoopCopyCurrentMode");
double CFAbsoluteTimeGetCurrent(void) __asm__("_CFAbsoluteTimeGetCurrent");

unsigned long CFRunLoopTimerGetTypeID(void) __asm__("_CFRunLoopTimerGetTypeID");
void *CFRunLoopTimerCreate(void *alloc, double fireDate, double interval,
                           unsigned long flags, long order,
                           void (*callout)(void *, void *), void *info)
    __asm__("_CFRunLoopTimerCreate");
void CFRunLoopTimerInvalidate(void *timer) __asm__("_CFRunLoopTimerInvalidate");
unsigned char CFRunLoopTimerIsValid(void *timer) __asm__("_CFRunLoopTimerIsValid");
void CFRunLoopTimerSetNextFireDate(void *timer, double fireDate)
    __asm__("_CFRunLoopTimerSetNextFireDate");
double CFRunLoopTimerGetNextFireDate(void *timer)
    __asm__("_CFRunLoopTimerGetNextFireDate");
unsigned char CFRunLoopTimerDoesRepeat(void *timer)
    __asm__("_CFRunLoopTimerDoesRepeat");

unsigned long CFRunLoopObserverGetTypeID(void)
    __asm__("_CFRunLoopObserverGetTypeID");
void *CFRunLoopObserverCreate(void *alloc, unsigned long activities,
                              unsigned char repeats, long order,
                              void (*callout)(void *, unsigned long, void *),
                              void *info) __asm__("_CFRunLoopObserverCreate");
void CFRunLoopObserverInvalidate(void *observer)
    __asm__("_CFRunLoopObserverInvalidate");
unsigned char CFRunLoopObserverIsValid(void *observer)
    __asm__("_CFRunLoopObserverIsValid");

unsigned long CFRunLoopSourceGetTypeID(void) __asm__("_CFRunLoopSourceGetTypeID");
void *CFRunLoopSourceCreate(void *alloc, long order, void *context)
    __asm__("_CFRunLoopSourceCreate");
void CFRunLoopSourceSignal(void *source) __asm__("_CFRunLoopSourceSignal");
void CFRunLoopSourceInvalidate(void *source) __asm__("_CFRunLoopSourceInvalidate");
unsigned char CFRunLoopSourceIsValid(void *source)
    __asm__("_CFRunLoopSourceIsValid");

unsigned long CFNotificationCenterGetTypeID(void)
    __asm__("_CFNotificationCenterGetTypeID");
void *CFNotificationCenterGetLocalCenter(void)
    __asm__("_CFNotificationCenterGetLocalCenter");
void *CFNotificationCenterGetDistributedCenter(void)
    __asm__("_CFNotificationCenterGetDistributedCenter");
void CFNotificationCenterAddObserver(void *center, void *observer,
                                     void (*callBack)(void *, void *, void *,
                                                      void *, void *),
                                     void *name, const void *object,
                                     long suspensionBehavior)
    __asm__("_CFNotificationCenterAddObserver");
void CFNotificationCenterRemoveObserver(void *center, void *observer, void *name,
                                        const void *object)
    __asm__("_CFNotificationCenterRemoveObserver");
void CFNotificationCenterPostNotificationName(void *center, void *name,
                                              const void *object, void *userInfo,
                                              unsigned char deliverImmediately)
    __asm__("_CFNotificationCenterPostNotificationName");

void *CFPreferencesCopyAppValue(void *key, void *appID)
    __asm__("_CFPreferencesCopyAppValue");
void CFPreferencesSetAppValue(void *key, void *value, void *appID)
    __asm__("_CFPreferencesSetAppValue");
unsigned char CFPreferencesAppSynchronize(void *appID)
    __asm__("_CFPreferencesAppSynchronize");
unsigned char CFPreferencesGetAppBooleanValue(void *key, void *appID,
                                              unsigned char *valid)
    __asm__("_CFPreferencesGetAppBooleanValue");
long CFPreferencesGetAppIntegerValue(void *key, void *appID,
                                     unsigned char *valid)
    __asm__("_CFPreferencesGetAppIntegerValue");
void *CFPreferencesCopyKeyList(void *appID, void *user, void *host)
    __asm__("_CFPreferencesCopyKeyList");

/* ---- generic messaging helpers ---- */

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

static long long send_ll(void *obj, const char *sel)
{
    long long (*fn)(void *, const char *);
    if (!obj)
        return 0;
    fn = (long long (*)(void *, const char *))plsr_objc_lookup_imp(obj, sel);
    return fn ? fn(obj, sel) : 0;
}

static double send_d(void *obj, const char *sel)
{
    double (*fn)(void *, const char *);
    if (!obj)
        return 0.0;
    fn = (double (*)(void *, const char *))plsr_objc_lookup_imp(obj, sel);
    return fn ? fn(obj, sel) : 0.0;
}

static int send_i(void *obj, const char *sel)
{
    int (*fn)(void *, const char *);
    if (!obj)
        return 0;
    fn = (int (*)(void *, const char *))plsr_objc_lookup_imp(obj, sel);
    return fn ? fn(obj, sel) : 0;
}

static unsigned long send_ul(void *obj, const char *sel)
{
    unsigned long (*fn)(void *, const char *);
    if (!obj)
        return 0;
    fn = (unsigned long (*)(void *, const char *))plsr_objc_lookup_imp(obj, sel);
    return fn ? fn(obj, sel) : 0;
}

static const char *cstr(void *obj)
{
    const char *s = obj ? (const char *)send0(obj, "UTF8String") : NULL;
    return s ? s : "";
}

static void *ns_string_bytes(const void *bytes, size_t len)
{
    void *cls = objc_getClass("NSString");
    void *obj;
    void *(*init)(void *, const char *, const void *, unsigned long, unsigned long);
    if (!cls)
        return NULL;
    obj = send0(cls, "alloc");
    if (!obj)
        return NULL;
    init = (void *(*)(void *, const char *, const void *, unsigned long,
                      unsigned long))plsr_objc_lookup_imp(obj,
                                                          "initWithBytes:length:encoding:");
    if (!init)
        return obj;
    return init(obj, "initWithBytes:length:encoding:", bytes, (unsigned long)len, 4);
}

static void *ns_string(const char *c)
{
    return ns_string_bytes(c ? c : "", c ? strlen(c) : 0);
}

static void *ns_mutable_string(void)
{
    void *cls = objc_getClass("NSMutableString");
    return cls ? send0(cls, "string") : NULL;
}

static void *ns_array_class(void)
{
    return objc_getClass("NSArray");
}

static void *ns_mutable_array_class(void)
{
    return objc_getClass("NSMutableArray");
}

static void *ns_dict_class(void)
{
    return objc_getClass("NSDictionary");
}

static void *ns_mutable_dict_class(void)
{
    return objc_getClass("NSMutableDictionary");
}

static void *ns_number_class(void)
{
    return objc_getClass("NSNumber");
}

static void *ns_alloc_init(void *cls)
{
    void *obj;
    if (!cls)
        return NULL;
    obj = send0(cls, "alloc");
    return obj ? send0(obj, "init") : NULL;
}

/* ---- string buffer for formatting ---- */

struct sbuf {
    char *p;
    size_t len, cap;
};

static void sb_put(struct sbuf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = (b->len + n + 1) * 2;
        char *p = realloc(b->p, cap);
        if (!p)
            return;
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void sb_cstr(struct sbuf *b, const char *s)
{
    if (s)
        sb_put(b, s, strlen(s));
}

static void sb_char(struct sbuf *b, char c)
{
    sb_put(b, &c, 1);
}

static void sb_format(struct sbuf *b, const char *fmt, va_list ap)
{
    const char *p = fmt;
    while (*p) {
        if (*p != '%') {
            sb_char(b, *p++);
            continue;
        }
        p++;
        if (*p == '%') {
            sb_char(b, '%');
            p++;
            continue;
        }
        {
            char spec[32];
            size_t si = 0;
            char tmp[160];
            int is_long = 0;
            spec[si++] = '%';
            while (*p && strchr("-+ #0", *p) && si < sizeof spec - 2)
                spec[si++] = *p++;
            while (*p && isdigit((unsigned char)*p) && si < sizeof spec - 2)
                spec[si++] = *p++;
            if (*p == 'l') {
                is_long = 1;
                spec[si++] = *p++;
                if (*p == 'l') {
                    is_long = 2;
                    spec[si++] = *p++;
                }
            } else if (*p == 'z' || *p == 'h') {
                spec[si++] = *p++;
            }
            if (!*p)
                break;
            switch (*p) {
            case '@': {
                void *obj = va_arg(ap, void *);
                const char *s = obj ? cstr(obj) : "(null)";
                sb_cstr(b, s);
                p++;
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char *);
                sb_cstr(b, s ? s : "(null)");
                p++;
                break;
            }
            case 'c': {
                spec[si++] = 'c';
                spec[si] = 0;
                snprintf(tmp, sizeof tmp, spec, va_arg(ap, int));
                sb_cstr(b, tmp);
                p++;
                break;
            }
            case 'd':
            case 'i': {
                spec[si++] = 'd';
                spec[si] = 0;
                if (is_long == 2)
                    snprintf(tmp, sizeof tmp, spec, va_arg(ap, long long));
                else if (is_long)
                    snprintf(tmp, sizeof tmp, spec, va_arg(ap, long));
                else
                    snprintf(tmp, sizeof tmp, spec, va_arg(ap, int));
                sb_cstr(b, tmp);
                p++;
                break;
            }
            case 'u':
            case 'x':
            case 'X':
            case 'o': {
                spec[si++] = *p;
                spec[si] = 0;
                if (is_long == 2)
                    snprintf(tmp, sizeof tmp, spec,
                             va_arg(ap, unsigned long long));
                else if (is_long)
                    snprintf(tmp, sizeof tmp, spec, va_arg(ap, unsigned long));
                else
                    snprintf(tmp, sizeof tmp, spec, va_arg(ap, unsigned int));
                sb_cstr(b, tmp);
                p++;
                break;
            }
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G': {
                spec[si++] = *p;
                spec[si] = 0;
                snprintf(tmp, sizeof tmp, spec, va_arg(ap, double));
                sb_cstr(b, tmp);
                p++;
                break;
            }
            case 'p': {
                spec[si++] = 'p';
                spec[si] = 0;
                snprintf(tmp, sizeof tmp, spec, va_arg(ap, void *));
                sb_cstr(b, tmp);
                p++;
                break;
            }
            default:
                sb_char(b, '%');
                sb_char(b, *p++);
                break;
            }
        }
    }
}

/* ---- CFSTR constants ---- */

typedef struct {
    void *isa;
    uint32_t flags;
    uint32_t len;
    const char *cstr;
} cf_const_string;

#define CFSTR_CONST(name, text)                                               \
    static cf_const_string cfstr_##name = {                                   \
        &plsr_cfconst_class, 0x07c8u, (uint32_t)(sizeof(text) - 1), (text) }

CFSTR_CONST(default_mode, "kCFRunLoopDefaultMode");
CFSTR_CONST(common_modes, "kCFRunLoopCommonModes");
CFSTR_CONST(bundle_identifier, "CFBundleIdentifier");
CFSTR_CONST(bundle_name, "CFBundleName");
CFSTR_CONST(bundle_executable, "CFBundleExecutable");
CFSTR_CONST(bundle_version, "CFBundleVersion");
CFSTR_CONST(bundle_short_version, "CFBundleShortVersionString");
CFSTR_CONST(bundle_package_type, "CFBundlePackageType");
CFSTR_CONST(bundle_signature, "CFBundleSignature");
CFSTR_CONST(bundle_development_region, "CFBundleDevelopmentRegion");
CFSTR_CONST(bundle_info_version, "CFBundleInfoDictionaryVersion");
CFSTR_CONST(bundle_localizations, "CFBundleLocalizations");
CFSTR_CONST(prefs_any_app, "kCFPreferencesAnyApplication");
CFSTR_CONST(prefs_current_app, "kCFPreferencesCurrentApplication");
CFSTR_CONST(prefs_any_user, "kCFPreferencesAnyUser");
CFSTR_CONST(prefs_current_user, "kCFPreferencesCurrentUser");
CFSTR_CONST(prefs_any_host, "kCFPreferencesAnyHost");
CFSTR_CONST(prefs_current_host, "kCFPreferencesCurrentHost");

/* Exported CF string constants are variables holding the CFStringRef. */
void *plsr_kCFRunLoopDefaultMode __asm__("_kCFRunLoopDefaultMode") =
    &cfstr_default_mode;
void *plsr_kCFRunLoopCommonModes __asm__("_kCFRunLoopCommonModes") =
    &cfstr_common_modes;
void *plsr_kCFBundleIdentifierKey __asm__("_kCFBundleIdentifierKey") =
    &cfstr_bundle_identifier;
void *plsr_kCFBundleNameKey __asm__("_kCFBundleNameKey") = &cfstr_bundle_name;
void *plsr_kCFBundleExecutableKey __asm__("_kCFBundleExecutableKey") =
    &cfstr_bundle_executable;
void *plsr_kCFBundleVersionKey __asm__("_kCFBundleVersionKey") =
    &cfstr_bundle_version;
void *plsr_kCFBundleShortVersionStringKey
    __asm__("_kCFBundleShortVersionStringKey") = &cfstr_bundle_short_version;
void *plsr_kCFBundlePackageTypeKey __asm__("_kCFBundlePackageTypeKey") =
    &cfstr_bundle_package_type;
void *plsr_kCFBundleSignatureKey __asm__("_kCFBundleSignatureKey") =
    &cfstr_bundle_signature;
void *plsr_kCFBundleDevelopmentRegionKey
    __asm__("_kCFBundleDevelopmentRegionKey") = &cfstr_bundle_development_region;
void *plsr_kCFBundleInfoDictionaryVersionKey
    __asm__("_kCFBundleInfoDictionaryVersionKey") = &cfstr_bundle_info_version;
void *plsr_kCFBundleLocalizationsKey __asm__("_kCFBundleLocalizationsKey") =
    &cfstr_bundle_localizations;
void *plsr_kCFPreferencesAnyApplication
    __asm__("_kCFPreferencesAnyApplication") = &cfstr_prefs_any_app;
void *plsr_kCFPreferencesCurrentApplication
    __asm__("_kCFPreferencesCurrentApplication") = &cfstr_prefs_current_app;
void *plsr_kCFPreferencesAnyUser __asm__("_kCFPreferencesAnyUser") =
    &cfstr_prefs_any_user;
void *plsr_kCFPreferencesCurrentUser __asm__("_kCFPreferencesCurrentUser") =
    &cfstr_prefs_current_user;
void *plsr_kCFPreferencesAnyHost __asm__("_kCFPreferencesAnyHost") =
    &cfstr_prefs_any_host;
void *plsr_kCFPreferencesCurrentHost __asm__("_kCFPreferencesCurrentHost") =
    &cfstr_prefs_current_host;

/* ---- type IDs ---- */

enum {
    CF_TYPE_STRING = 1,
    CF_TYPE_ARRAY,
    CF_TYPE_DICTIONARY,
    CF_TYPE_NUMBER,
    CF_TYPE_BOOLEAN,
    CF_TYPE_DATA,
    CF_TYPE_NULL,
    CF_TYPE_SET,
    CF_TYPE_UUID,
    CF_TYPE_URL,
    CF_TYPE_BUNDLE,
    CF_TYPE_RUNLOOP,
    CF_TYPE_NOTIFICATION_CENTER,
    CF_TYPE_ALLOCATOR,
    CF_TYPE_TIMER,
    CF_TYPE_OBSERVER,
    CF_TYPE_SOURCE
};

static const char *class_name_of(void *cls)
{
    const plsr_class_ro *ro;
    if (!cls)
        return "";
    ro = PLSR_RO((plsr_class *)cls);
    return ro && ro->name ? ro->name : "";
}

static int isa_named(void *obj, const char *name)
{
    plsr_class *c = obj ? *(plsr_class **)obj : NULL;
    for (int depth = 0; c && depth < 16; depth++, c = c->superclass)
        if (!strcmp(class_name_of(c), name))
            return 1;
    return 0;
}

unsigned long CFGetTypeID(const void *obj)
{
    void *o = (void *)obj;
    if (!o)
        return 0;
    if (isa_named(o, "__NSCFConstantString") || isa_named(o, "NSConstantString") ||
        isa_named(o, "NSString") || isa_named(o, "NSMutableString"))
        return CF_TYPE_STRING;
    if (isa_named(o, "__NSCFBoolean"))
        return CF_TYPE_BOOLEAN;
    if (isa_named(o, "NSNumber"))
        return CF_TYPE_NUMBER;
    if (isa_named(o, "NSArray") || isa_named(o, "NSMutableArray"))
        return CF_TYPE_ARRAY;
    if (isa_named(o, "NSDictionary") || isa_named(o, "NSMutableDictionary"))
        return CF_TYPE_DICTIONARY;
    if (isa_named(o, "__NSCFData") || isa_named(o, "NSData"))
        return CF_TYPE_DATA;
    if (isa_named(o, "NSNull"))
        return CF_TYPE_NULL;
    if (isa_named(o, "__NSCFSet"))
        return CF_TYPE_SET;
    if (isa_named(o, "__NSCFUUID"))
        return CF_TYPE_UUID;
    if (isa_named(o, "__NSCFURL"))
        return CF_TYPE_URL;
    if (isa_named(o, "NSBundle"))
        return CF_TYPE_BUNDLE;
    if (isa_named(o, "__NSCFRunLoop"))
        return CF_TYPE_RUNLOOP;
    if (isa_named(o, "__NSCFNotificationCenter"))
        return CF_TYPE_NOTIFICATION_CENTER;
    if (isa_named(o, "__NSCFAllocator"))
        return CF_TYPE_ALLOCATOR;
    if (isa_named(o, "__NSCFTimer"))
        return CF_TYPE_TIMER;
    if (isa_named(o, "__NSCFObserver"))
        return CF_TYPE_OBSERVER;
    if (isa_named(o, "__NSCFSource"))
        return CF_TYPE_SOURCE;
    return 0;
}

unsigned long CFStringGetTypeID(void) { return CF_TYPE_STRING; }
unsigned long CFArrayGetTypeID(void) { return CF_TYPE_ARRAY; }
unsigned long CFDictionaryGetTypeID(void) { return CF_TYPE_DICTIONARY; }
unsigned long CFNumberGetTypeID(void) { return CF_TYPE_NUMBER; }
unsigned long CFBooleanGetTypeID(void) { return CF_TYPE_BOOLEAN; }
unsigned long CFDataGetTypeID(void) { return CF_TYPE_DATA; }
unsigned long CFSetGetTypeID(void) { return CF_TYPE_SET; }
unsigned long CFUUIDGetTypeID(void) { return CF_TYPE_UUID; }
unsigned long CFURLGetTypeID(void) { return CF_TYPE_URL; }
unsigned long CFBundleGetTypeID(void) { return CF_TYPE_BUNDLE; }
unsigned long CFRunLoopGetTypeID(void) { return CF_TYPE_RUNLOOP; }
unsigned long CFNotificationCenterGetTypeID(void) { return CF_TYPE_NOTIFICATION_CENTER; }
unsigned long CFAllocatorGetTypeID(void) { return CF_TYPE_ALLOCATOR; }
unsigned long CFRunLoopTimerGetTypeID(void) { return CF_TYPE_TIMER; }
unsigned long CFRunLoopObserverGetTypeID(void) { return CF_TYPE_OBSERVER; }
unsigned long CFRunLoopSourceGetTypeID(void) { return CF_TYPE_SOURCE; }

void *CFCopyTypeIDDescription(unsigned long type)
{
    switch (type) {
    case CF_TYPE_STRING: return ns_string("CFString");
    case CF_TYPE_ARRAY: return ns_string("CFArray");
    case CF_TYPE_DICTIONARY: return ns_string("CFDictionary");
    case CF_TYPE_NUMBER: return ns_string("CFNumber");
    case CF_TYPE_BOOLEAN: return ns_string("CFBoolean");
    case CF_TYPE_DATA: return ns_string("CFData");
    case CF_TYPE_NULL: return ns_string("CFNull");
    case CF_TYPE_SET: return ns_string("CFSet");
    case CF_TYPE_UUID: return ns_string("CFUUID");
    case CF_TYPE_URL: return ns_string("CFURL");
    case CF_TYPE_BUNDLE: return ns_string("CFBundle");
    case CF_TYPE_RUNLOOP: return ns_string("CFRunLoop");
    case CF_TYPE_NOTIFICATION_CENTER: return ns_string("CFNotificationCenter");
    case CF_TYPE_ALLOCATOR: return ns_string("CFAllocator");
    case CF_TYPE_TIMER: return ns_string("CFRunLoopTimer");
    case CF_TYPE_OBSERVER: return ns_string("CFRunLoopObserver");
    case CF_TYPE_SOURCE: return ns_string("CFRunLoopSource");
    default: return ns_string("CFType");
    }
}

/* ---- allocator ---- */

typedef struct {
    void *isa;
    int kind;
} cf_allocator;

static cf_allocator cf_alloc_default = { &plsr_cfalloc_class, 0 };
static cf_allocator cf_alloc_system = { &plsr_cfalloc_class, 1 };
static cf_allocator cf_alloc_malloc = { &plsr_cfalloc_class, 2 };
static cf_allocator cf_alloc_malloc_zone = { &plsr_cfalloc_class, 3 };
static cf_allocator cf_alloc_null = { &plsr_cfalloc_class, 4 };
static cf_allocator cf_alloc_context = { &plsr_cfalloc_class, 5 };
static cf_allocator *cf_alloc_current = &cf_alloc_default;

void *plsr_kCFAllocatorDefault __asm__("_kCFAllocatorDefault") = &cf_alloc_default;
void *plsr_kCFAllocatorSystemDefault __asm__("_kCFAllocatorSystemDefault") =
    &cf_alloc_system;
void *plsr_kCFAllocatorMalloc __asm__("_kCFAllocatorMalloc") = &cf_alloc_malloc;
void *plsr_kCFAllocatorMallocZone __asm__("_kCFAllocatorMallocZone") =
    &cf_alloc_malloc_zone;
void *plsr_kCFAllocatorNull __asm__("_kCFAllocatorNull") = &cf_alloc_null;
void *plsr_kCFAllocatorUseContext __asm__("_kCFAllocatorUseContext") =
    &cf_alloc_context;

void *CFAllocatorGetDefault(void) { return cf_alloc_current; }

void CFAllocatorSetDefault(void *alloc)
{
    if (alloc)
        cf_alloc_current = alloc;
}

unsigned long CFAllocatorGetPreferredSizeForSize(void *alloc, unsigned long size,
                                                 unsigned long hint)
{
    (void)alloc;
    (void)hint;
    return size;
}

void *CFAllocatorAllocate(void *alloc, unsigned long size, unsigned long hint)
{
    (void)alloc;
    (void)hint;
    return calloc(1, size ? size : 1);
}

void CFAllocatorDeallocate(void *alloc, void *p)
{
    (void)alloc;
    free(p);
}

void *CFAllocatorReallocate(void *alloc, void *p, unsigned long size,
                            unsigned long hint)
{
    (void)alloc;
    (void)hint;
    return realloc(p, size ? size : 1);
}

/* ---- object model ---- */

void *CFRetain(const void *obj) { return (void *)obj; }
void CFRelease(const void *obj) { (void)obj; }
const void *CFAutorelease(const void *obj) { return obj; }
unsigned long CFGetRetainCount(const void *obj) { (void)obj; return 1; }

unsigned char CFEqual(const void *a, const void *b)
{
    int (*eq)(void *, const char *, void *);
    if (a == b)
        return 1;
    if (!a || !b)
        return 0;
    eq = (int (*)(void *, const char *, void *))
        plsr_objc_lookup_imp((void *)a, "isEqual:");
    return eq ? (eq((void *)a, "isEqual:", (void *)b) ? 1 : 0) : 0;
}

unsigned long CFHash(const void *obj)
{
    if (!obj)
        return 0;
    return send_ul((void *)obj, "hash");
}

void *CFCopyDescription(const void *obj)
{
    void *d;
    if (!obj)
        return ns_string("(null)");
    d = send0((void *)obj, "description");
    if (d)
        return d;
    {
        char tmp[64];
        snprintf(tmp, sizeof tmp, "<CFType %p>", obj);
        return ns_string(tmp);
    }
}

void CFShow(const void *obj)
{
    void *d = CFCopyDescription(obj);
    fprintf(stderr, "%s\n", cstr(d));
}

void *CFGetAllocator(const void *obj)
{
    (void)obj;
    return CFAllocatorGetDefault();
}

/* ---- strings ---- */

static void *cf_string_utf16(const unsigned short *chars, size_t n)
{
    char *buf = malloc(n * 3 + 1);
    size_t o = 0;
    void *out;
    if (!buf)
        return NULL;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = chars[i];
        if (c < 0x80) {
            buf[o++] = (char)c;
        } else if (c < 0x800) {
            buf[o++] = (char)(0xc0 | (c >> 6));
            buf[o++] = (char)(0x80 | (c & 0x3f));
        } else {
            buf[o++] = (char)(0xe0 | (c >> 12));
            buf[o++] = (char)(0x80 | ((c >> 6) & 0x3f));
            buf[o++] = (char)(0x80 | (c & 0x3f));
        }
    }
    out = ns_string_bytes(buf, o);
    free(buf);
    return out;
}

void *CFStringCreateWithCString(void *alloc, const char *cstr_in,
                                unsigned encoding)
{
    (void)alloc;
    (void)encoding;
    if (!cstr_in)
        return NULL;
    return ns_string(cstr_in);
}

void *CFStringCreateWithCStringNoCopy(void *alloc, const char *cstr_in,
                                      unsigned encoding, void *deallocator)
{
    (void)deallocator;
    return CFStringCreateWithCString(alloc, cstr_in, encoding);
}

void *CFStringCreateWithBytes(void *alloc, const unsigned char *bytes,
                              long numBytes, unsigned encoding,
                              unsigned char external)
{
    (void)alloc;
    (void)external;
    if (!bytes || numBytes < 0)
        return NULL;
    if (encoding == 0x0100) /* kCFStringEncodingUTF16 */
        return cf_string_utf16((const unsigned short *)bytes,
                               (size_t)numBytes / 2);
    return ns_string_bytes(bytes, (size_t)numBytes);
}

void *CFStringCreateWithBytesNoCopy(void *alloc, const unsigned char *bytes,
                                    long numBytes, unsigned encoding,
                                    unsigned char external, void *deallocator)
{
    (void)deallocator;
    return CFStringCreateWithBytes(alloc, bytes, numBytes, encoding, external);
}

void *CFStringCreateWithCharacters(void *alloc, const unsigned short *chars,
                                   long numChars)
{
    (void)alloc;
    if (!chars || numChars < 0)
        return NULL;
    return cf_string_utf16(chars, (size_t)numChars);
}

void *CFStringCreateWithSubstring(void *alloc, void *str, cf_range range)
{
    const char *s;
    size_t n;
    (void)alloc;
    if (!str)
        return NULL;
    s = cstr(str);
    n = strlen(s);
    if (range.location > n)
        return NULL;
    if (range.location + range.length > n)
        range.length = n - range.location;
    return ns_string_bytes(s + range.location, (size_t)range.length);
}

void *CFStringCreateCopy(void *alloc, void *str)
{
    (void)alloc;
    return str ? ns_string(cstr(str)) : NULL;
}

void *CFStringCreateMutable(void *alloc, long maxLength)
{
    (void)alloc;
    (void)maxLength;
    return ns_mutable_string();
}

void *CFStringCreateMutableCopy(void *alloc, long maxLength, void *str)
{
    (void)alloc;
    (void)maxLength;
    if (!str)
        return ns_mutable_string();
    return send0(str, "mutableCopy");
}

unsigned char CFStringGetCString(void *str, char *buffer, long bufferSize,
                                 unsigned encoding)
{
    const char *s;
    size_t n;
    (void)encoding;
    if (!str || !buffer || bufferSize <= 0)
        return 0;
    s = cstr(str);
    n = strlen(s);
    if ((long)n + 1 > bufferSize)
        return 0;
    memcpy(buffer, s, n + 1);
    return 1;
}

const char *CFStringGetCStringPtr(void *str, unsigned encoding)
{
    (void)encoding;
    return str ? cstr(str) : NULL;
}

long CFStringGetLength(void *str)
{
    return str ? (long)send_ul(str, "length") : 0;
}

long CFStringGetMaximumSizeForEncoding(long length, unsigned encoding)
{
    (void)encoding;
    return length * 4 + 1;
}

void CFStringGetCharacters(void *str, cf_range range, unsigned short *buffer)
{
    void (*fn)(void *, const char *, unsigned short *, cf_range);
    const char *s;
    if (!str || !buffer)
        return;
    fn = (void (*)(void *, const char *, unsigned short *, cf_range))
        plsr_objc_lookup_imp(str, "getCharacters:range:");
    if (fn) {
        fn(str, "getCharacters:range:", buffer, range);
        return;
    }
    s = cstr(str);
    for (unsigned long i = 0; i < range.length; i++)
        buffer[i] = (unsigned char)s[range.location + i];
}

unsigned short CFStringGetCharacterAtIndex(void *str, unsigned long idx)
{
    unsigned short (*fn)(void *, const char *, unsigned long);
    if (!str)
        return 0;
    fn = (unsigned short (*)(void *, const char *, unsigned long))
        plsr_objc_lookup_imp(str, "characterAtIndex:");
    if (fn)
        return fn(str, "characterAtIndex:", idx);
    return (unsigned char)cstr(str)[idx];
}

void CFStringAppend(void *str, void *other)
{
    if (str && other)
        send1(str, "appendString:", other);
}

void CFStringAppendCString(void *str, const char *cstr_in, unsigned encoding)
{
    (void)encoding;
    if (str && cstr_in) {
        void *tmp = ns_string(cstr_in);
        send1(str, "appendString:", tmp);
    }
}

void CFStringAppendFormat(void *str, void *format, ...)
{
    struct sbuf b = { 0 };
    va_list ap;
    void *tmp;
    if (!str || !format)
        return;
    va_start(ap, format);
    sb_format(&b, cstr(format), ap);
    va_end(ap);
    tmp = ns_string_bytes(b.p ? b.p : "", b.len);
    send1(str, "appendString:", tmp);
    free(b.p);
}

static void cf_string_set(void *str, const char *buf, size_t len)
{
    void *(*init)(void *, const char *, const void *, unsigned long, unsigned long);
    init = (void *(*)(void *, const char *, const void *, unsigned long,
                      unsigned long))plsr_objc_lookup_imp(str,
                                                          "initWithBytes:length:encoding:");
    if (init)
        init(str, "initWithBytes:length:encoding:", buf, (unsigned long)len, 4);
}

void CFStringInsert(void *str, long idx, void *other)
{
    const char *s, *ins;
    size_t n, m, at;
    char *buf;
    if (!str || !other)
        return;
    s = cstr(str);
    ins = cstr(other);
    n = strlen(s);
    m = strlen(ins);
    at = idx < 0 ? 0 : (size_t)idx;
    if (at > n)
        at = n;
    buf = malloc(n + m + 1);
    if (!buf)
        return;
    memcpy(buf, s, at);
    memcpy(buf + at, ins, m);
    memcpy(buf + at + m, s + at, n - at + 1);
    cf_string_set(str, buf, n + m);
    free(buf);
}

void CFStringDelete(void *str, cf_range range)
{
    const char *s;
    size_t n;
    char *buf;
    if (!str)
        return;
    s = cstr(str);
    n = strlen(s);
    if (range.location > n)
        return;
    if (range.location + range.length > n)
        range.length = n - range.location;
    buf = malloc(n - range.length + 1);
    if (!buf)
        return;
    memcpy(buf, s, range.location);
    memcpy(buf + range.location, s + range.location + range.length,
           n - range.location - range.length + 1);
    cf_string_set(str, buf, n - range.length);
    free(buf);
}

void CFStringReplace(void *str, cf_range range, void *replacement)
{
    const char *s, *rep;
    size_t n, m;
    char *buf;
    if (!str || !replacement)
        return;
    s = cstr(str);
    rep = cstr(replacement);
    n = strlen(s);
    m = strlen(rep);
    if (range.location > n)
        return;
    if (range.location + range.length > n)
        range.length = n - range.location;
    buf = malloc(n - range.length + m + 1);
    if (!buf)
        return;
    memcpy(buf, s, range.location);
    memcpy(buf + range.location, rep, m);
    memcpy(buf + range.location + m, s + range.location + range.length,
           n - range.location - range.length + 1);
    cf_string_set(str, buf, n - range.length + m);
    free(buf);
}

long CFStringCompare(void *a, void *b, unsigned long options)
{
    const char *x, *y;
    int r;
    if (a == b)
        return 0;
    if (!a)
        return -1;
    if (!b)
        return 1;
    x = cstr(a);
    y = cstr(b);
    r = (options & 1) ? strcasecmp(x, y) : strcmp(x, y);
    return r < 0 ? -1 : (r > 0 ? 1 : 0);
}

long CFStringCompareWithOptions(void *a, void *b, cf_range range,
                                unsigned long options)
{
    void *sub = CFStringCreateWithSubstring(NULL, a, range);
    long r = CFStringCompare(sub, b, options);
    return r;
}

unsigned char CFStringHasPrefix(void *str, void *prefix)
{
    const char *s, *p;
    size_t n, m;
    if (!str || !prefix)
        return 0;
    s = cstr(str);
    p = cstr(prefix);
    n = strlen(s);
    m = strlen(p);
    return n >= m && !memcmp(s, p, m);
}

unsigned char CFStringHasSuffix(void *str, void *suffix)
{
    const char *s, *p;
    size_t n, m;
    if (!str || !suffix)
        return 0;
    s = cstr(str);
    p = cstr(suffix);
    n = strlen(s);
    m = strlen(p);
    return n >= m && !memcmp(s + n - m, p, m);
}

unsigned char CFStringFind(void *str, void *needle, unsigned long options,
                           cf_range *found)
{
    const char *s, *p, *at;
    (void)options;
    if (found) {
        found->location = (unsigned long)-1;
        found->length = 0;
    }
    if (!str || !needle)
        return 0;
    s = cstr(str);
    p = cstr(needle);
    at = strstr(s, p);
    if (!at)
        return 0;
    if (found) {
        found->location = (unsigned long)(at - s);
        found->length = strlen(p);
    }
    return 1;
}

static void *cf_string_map(void *str, int upper)
{
    const char *s = cstr(str);
    size_t n = strlen(s);
    char *buf = malloc(n + 1);
    void *out;
    if (!buf)
        return NULL;
    for (size_t i = 0; i < n; i++)
        buf[i] = (char)(upper ? toupper((unsigned char)s[i])
                              : tolower((unsigned char)s[i]));
    buf[n] = 0;
    out = ns_string_bytes(buf, n);
    free(buf);
    return out;
}

void *CFStringLowercase(void *str, void *locale)
{
    (void)locale;
    return str ? cf_string_map(str, 0) : NULL;
}

void *CFStringUppercase(void *str, void *locale)
{
    (void)locale;
    return str ? cf_string_map(str, 1) : NULL;
}

long CFStringGetIntValue(void *str)
{
    return str ? strtoll(cstr(str), NULL, 10) : 0;
}

double CFStringGetDoubleValue(void *str)
{
    return str ? strtod(cstr(str), NULL) : 0.0;
}

void *CFStringCreateWithFormat(void *alloc, void *format, ...)
{
    struct sbuf b = { 0 };
    va_list ap;
    void *out;
    (void)alloc;
    if (!format)
        return NULL;
    va_start(ap, format);
    sb_format(&b, cstr(format), ap);
    va_end(ap);
    out = ns_string_bytes(b.p ? b.p : "", b.len);
    free(b.p);
    return out;
}

/* ---- arrays ---- */

void *CFArrayCreate(void *alloc, const void **values, long numValues,
                    const void *callbacks)
{
    void *cls = ns_array_class();
    (void)alloc;
    (void)callbacks;
    if (!cls)
        return NULL;
    if (numValues <= 0)
        return send0(cls, "array");
    {
        void *(*fn)(void *, const char *, void **, unsigned long);
        fn = (void *(*)(void *, const char *, void **, unsigned long))
            plsr_objc_lookup_imp(cls, "arrayWithObjects:count:");
        return fn ? fn(cls, "arrayWithObjects:count:", (void **)values,
                       (unsigned long)numValues)
                  : NULL;
    }
}

void *CFArrayCreateMutable(void *alloc, long capacity, const void *callbacks)
{
    void *cls = ns_mutable_array_class();
    (void)alloc;
    (void)capacity;
    (void)callbacks;
    return cls ? send0(cls, "array") : NULL;
}

void *CFArrayCreateCopy(void *alloc, void *array)
{
    (void)alloc;
    return array ? send0(array, "copy") : NULL;
}

void *CFArrayCreateMutableCopy(void *alloc, long capacity, void *array)
{
    (void)alloc;
    (void)capacity;
    return array ? send0(array, "mutableCopy") : NULL;
}

long CFArrayGetCount(void *array)
{
    return array ? (long)send_ul(array, "count") : 0;
}

void *CFArrayGetValueAtIndex(void *array, long idx)
{
    if (!array || idx < 0)
        return NULL;
    return send1(array, "objectAtIndex:", (void *)(uintptr_t)idx);
}

void CFArrayGetValues(void *array, cf_range range, const void **values)
{
    long n;
    if (!array || !values)
        return;
    n = CFArrayGetCount(array);
    for (unsigned long i = 0; i < range.length; i++) {
        unsigned long idx = range.location + i;
        values[i] = idx < (unsigned long)n ? CFArrayGetValueAtIndex(array, (long)idx)
                                           : NULL;
    }
}

void CFArrayAppendValue(void *array, const void *value)
{
    if (array && value)
        send1(array, "addObject:", (void *)value);
}

void CFArrayAppendArray(void *array, void *other, cf_range range)
{
    (void)range;
    if (array && other)
        send1(array, "addObjectsFromArray:", other);
}

void CFArraySetValueAtIndex(void *array, long idx, const void *value)
{
    if (array && idx >= 0 && value) {
        void (*fn)(void *, const char *, void *, unsigned long);
        fn = (void (*)(void *, const char *, void *, unsigned long))
            plsr_objc_lookup_imp(array, "setObject:atIndexedSubscript:");
        if (fn)
            fn(array, "setObject:atIndexedSubscript:", (void *)value,
               (unsigned long)idx);
    }
}

void CFArrayInsertValueAtIndex(void *array, long idx, const void *value)
{
    if (array && idx >= 0 && value) {
        void (*fn)(void *, const char *, void *, unsigned long);
        fn = (void (*)(void *, const char *, void *, unsigned long))
            plsr_objc_lookup_imp(array, "insertObject:atIndex:");
        if (fn)
            fn(array, "insertObject:atIndex:", (void *)value,
               (unsigned long)idx);
    }
}

void CFArrayRemoveValueAtIndex(void *array, long idx)
{
    if (array && idx >= 0) {
        void (*fn)(void *, const char *, unsigned long);
        fn = (void (*)(void *, const char *, unsigned long))
            plsr_objc_lookup_imp(array, "removeObjectAtIndex:");
        if (fn)
            fn(array, "removeObjectAtIndex:", (unsigned long)idx);
    }
}

void CFArrayRemoveAllValues(void *array)
{
    if (array)
        send0(array, "removeAllObjects");
}

unsigned char CFArrayContainsValue(void *array, cf_range range,
                                   const void *value)
{
    return CFArrayGetFirstIndexOfValue(array, range, value) >= 0;
}

long CFArrayGetFirstIndexOfValue(void *array, cf_range range,
                                 const void *value)
{
    long n;
    if (!array)
        return -1;
    n = CFArrayGetCount(array);
    for (unsigned long i = 0; i < range.length; i++) {
        unsigned long idx = range.location + i;
        if (idx >= (unsigned long)n)
            break;
        if (CFEqual(CFArrayGetValueAtIndex(array, (long)idx), value))
            return (long)idx;
    }
    return -1;
}

/* ---- dictionaries ---- */

void *CFDictionaryCreate(void *alloc, const void **keys, const void **values,
                         long numValues, const void *keyCallbacks,
                         const void *valueCallbacks)
{
    void *cls = ns_dict_class();
    (void)alloc;
    (void)keyCallbacks;
    (void)valueCallbacks;
    if (!cls)
        return NULL;
    if (numValues <= 0)
        return send0(cls, "dictionary");
    {
        void *(*fn)(void *, const char *, void **, void **, unsigned long);
        fn = (void *(*)(void *, const char *, void **, void **, unsigned long))
            plsr_objc_lookup_imp(cls, "dictionaryWithObjects:forKeys:count:");
        return fn ? fn(cls, "dictionaryWithObjects:forKeys:count:",
                       (void **)values, (void **)keys,
                       (unsigned long)numValues)
                  : NULL;
    }
}

void *CFDictionaryCreateMutable(void *alloc, long capacity,
                                const void *keyCallbacks,
                                const void *valueCallbacks)
{
    void *cls = ns_mutable_dict_class();
    void *dict;
    (void)alloc;
    (void)capacity;
    (void)keyCallbacks;
    (void)valueCallbacks;
    if (!cls)
        return NULL;
    dict = send0(cls, "dictionary");
    return dict ? dict : ns_alloc_init(cls);
}

void *CFDictionaryCreateCopy(void *alloc, void *dict)
{
    (void)alloc;
    return dict ? send0(dict, "copy") : NULL;
}

void *CFDictionaryCreateMutableCopy(void *alloc, long capacity, void *dict)
{
    (void)alloc;
    (void)capacity;
    return dict ? send0(dict, "mutableCopy") : NULL;
}

long CFDictionaryGetCount(void *dict)
{
    return dict ? (long)send_ul(dict, "count") : 0;
}

void *CFDictionaryGetValue(void *dict, const void *key)
{
    return dict && key ? send1(dict, "objectForKey:", (void *)key) : NULL;
}

unsigned char CFDictionaryGetValueIfPresent(void *dict, const void *key,
                                            const void **value)
{
    void *v = CFDictionaryGetValue(dict, key);
    if (value)
        *value = v;
    return v != NULL;
}

unsigned char CFDictionaryContainsKey(void *dict, const void *key)
{
    return CFDictionaryGetValue(dict, key) != NULL;
}

void CFDictionarySetValue(void *dict, const void *key, const void *value)
{
    void (*fn)(void *, const char *, void *, void *);
    if (!dict || !key || !value)
        return;
    fn = (void (*)(void *, const char *, void *, void *))
        plsr_objc_lookup_imp(dict, "setObject:forKey:");
    if (fn)
        fn(dict, "setObject:forKey:", (void *)value, (void *)key);
}

void CFDictionaryAddValue(void *dict, const void *key, const void *value)
{
    CFDictionarySetValue(dict, key, value);
}

void CFDictionaryReplaceValue(void *dict, const void *key, const void *value)
{
    CFDictionarySetValue(dict, key, value);
}

void CFDictionaryRemoveValue(void *dict, const void *key)
{
    void (*fn)(void *, const char *, void *);
    if (!dict || !key)
        return;
    fn = (void (*)(void *, const char *, void *))
        plsr_objc_lookup_imp(dict, "removeObjectForKey:");
    if (fn)
        fn(dict, "removeObjectForKey:", (void *)key);
}

void CFDictionaryRemoveAllValues(void *dict)
{
    void *keys;
    long n;
    if (!dict)
        return;
    keys = send0(dict, "allKeys");
    n = keys ? CFArrayGetCount(keys) : 0;
    for (long i = 0; i < n; i++)
        CFDictionaryRemoveValue(dict, CFArrayGetValueAtIndex(keys, i));
}

void CFDictionaryGetKeysAndValues(void *dict, const void **keys,
                                  const void **values)
{
    void *k, *v;
    long n;
    if (!dict)
        return;
    k = send0(dict, "allKeys");
    v = send0(dict, "allValues");
    n = k ? CFArrayGetCount(k) : 0;
    for (long i = 0; i < n; i++) {
        if (keys)
            keys[i] = CFArrayGetValueAtIndex(k, i);
        if (values)
            values[i] = v ? CFArrayGetValueAtIndex(v, i) : NULL;
    }
}

/* ---- numbers and booleans ---- */

static cf_boolean cf_bool_true = { &plsr_cfbool_class, 1, 1.0, 0 };
static cf_boolean cf_bool_false = { &plsr_cfbool_class, 0, 0.0, 0 };

void *plsr_kCFBooleanTrue __asm__("_kCFBooleanTrue") = &cf_bool_true;
void *plsr_kCFBooleanFalse __asm__("_kCFBooleanFalse") = &cf_bool_false;
void *plsr_kCFNull __asm__("_kCFNull");

void *CFNumberCreate(void *alloc, long type, const void *valuePtr)
{
    void *cls = ns_number_class();
    (void)alloc;
    if (!cls || !valuePtr)
        return NULL;
    switch (type) {
    case 1: case 2: case 3: case 4: case 7: case 9: case 15: case 16: case 17:
        return send1(cls, "numberWithLongLong:",
                     (void *)(intptr_t)*(const long long *)valuePtr);
    case 5: case 6: case 10: case 11: case 12: case 13: case 14: case 18: {
        double d = (type == 5 || type == 11)
                       ? (double)*(const float *)valuePtr
                       : *(const double *)valuePtr;
        void *(*fn)(void *, const char *, double);
        fn = (void *(*)(void *, const char *, double))
            plsr_objc_lookup_imp(cls, "numberWithDouble:");
        if (!fn)
            return NULL;
        return fn(cls, "numberWithDouble:", d);
    }
    default:
        return send1(cls, "numberWithLongLong:",
                     (void *)(intptr_t)*(const long long *)valuePtr);
    }
}

unsigned char CFNumberGetValue(void *number, long type, void *valuePtr)
{
    if (!number || !valuePtr)
        return 0;
    switch (type) {
    case 1: case 2: case 3: case 4: case 7: case 9: case 15: case 16: case 17:
        *(long long *)valuePtr = send_ll(number, "longLongValue");
        return 1;
    case 5: case 6: case 10: case 11: case 12: case 13: case 14: case 18:
        if (type == 5 || type == 11)
            *(float *)valuePtr = (float)send_d(number, "doubleValue");
        else
            *(double *)valuePtr = send_d(number, "doubleValue");
        return 1;
    default:
        return 0;
    }
}

long CFNumberGetType(void *number)
{
    double d;
    long long i;
    if (!number)
        return 0;
    d = send_d(number, "doubleValue");
    i = send_ll(number, "longLongValue");
    return d == (double)i ? 4 : 6; /* kCFNumberSInt64Type / kCFNumberDoubleType */
}

long CFNumberGetByteSize(void *number)
{
    return CFNumberIsFloatType(number) ? 8 : 8;
}

unsigned char CFNumberIsFloatType(void *number)
{
    return CFNumberGetType(number) == 6;
}

unsigned char CFBooleanGetValue(void *boolean)
{
    return boolean ? (((cf_boolean *)boolean)->i != 0) : 0;
}

/* ---- data ---- */

static cf_data *cf_data_alloc(size_t len)
{
    cf_data *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->isa = &plsr_cfdata_class;
    d->len = len;
    d->cap = len ? len : 16;
    d->bytes = calloc(1, d->cap);
    return d;
}

static void *data_bytes_m(void *self, const char *sel)
{
    (void)sel;
    return ((cf_data *)self)->bytes;
}

static unsigned long data_length_m(void *self, const char *sel)
{
    (void)sel;
    return ((cf_data *)self)->len;
}

static void data_append_m(void *self, const char *sel, const void *bytes,
                          unsigned long n)
{
    cf_data *d = self;
    (void)sel;
    if (!n)
        return;
    if (d->len + n > d->cap) {
        size_t cap = (d->len + n) * 2;
        unsigned char *p = realloc(d->bytes, cap);
        if (!p)
            return;
        d->bytes = p;
        d->cap = cap;
    }
    memcpy(d->bytes + d->len, bytes, n);
    d->len += n;
}

static void data_set_length_m(void *self, const char *sel, unsigned long n)
{
    cf_data *d = self;
    (void)sel;
    if (n > d->cap) {
        unsigned char *p = realloc(d->bytes, n);
        if (!p)
            return;
        d->bytes = p;
        d->cap = n;
    }
    if (n > d->len)
        memset(d->bytes + d->len, 0, n - d->len);
    d->len = n;
}

static void *data_mutable_bytes_m(void *self, const char *sel)
{
    (void)sel;
    return ((cf_data *)self)->bytes;
}

void *CFDataCreate(void *alloc, const unsigned char *bytes, long length)
{
    cf_data *d;
    (void)alloc;
    if (length < 0)
        return NULL;
    d = cf_data_alloc((size_t)length);
    if (d && bytes && length)
        memcpy(d->bytes, bytes, (size_t)length);
    return d;
}

void *CFDataCreateMutable(void *alloc, long capacity)
{
    (void)alloc;
    return cf_data_alloc(capacity > 0 ? (size_t)capacity : 0);
}

void *CFDataCreateCopy(void *alloc, void *data)
{
    (void)alloc;
    if (!data)
        return NULL;
    return CFDataCreate(NULL, CFDataGetBytePtr(data), CFDataGetLength(data));
}

void *CFDataCreateMutableCopy(void *alloc, long capacity, void *data)
{
    (void)alloc;
    (void)capacity;
    return CFDataCreateCopy(NULL, data);
}

long CFDataGetLength(void *data)
{
    return data ? (long)((cf_data *)data)->len : 0;
}

const unsigned char *CFDataGetBytePtr(void *data)
{
    return data ? ((cf_data *)data)->bytes : NULL;
}

unsigned char *CFDataGetMutableBytePtr(void *data)
{
    return data ? ((cf_data *)data)->bytes : NULL;
}

void CFDataAppendBytes(void *data, const unsigned char *bytes, long length)
{
    if (!data || !bytes || length <= 0)
        return;
    data_append_m(data, "appendBytes:length:", bytes, (unsigned long)length);
}

void CFDataSetLength(void *data, long length)
{
    if (data && length >= 0)
        data_set_length_m(data, "setLength:", (unsigned long)length);
}

void CFDataReplaceBytes(void *data, cf_range range, const unsigned char *bytes,
                        long length)
{
    cf_data *d = data;
    size_t tail;
    if (!d || range.location > d->len)
        return;
    if (range.location + range.length > d->len)
        range.length = d->len - range.location;
    if (length < 0)
        length = 0;
    tail = d->len - range.location - range.length;
    {
        size_t need = range.location + (size_t)length + tail;
        unsigned char *buf = malloc(need ? need : 1);
        if (!buf)
            return;
        memcpy(buf, d->bytes, range.location);
        if (bytes && length)
            memcpy(buf + range.location, bytes, (size_t)length);
        memcpy(buf + range.location + length,
               d->bytes + range.location + range.length, tail);
        free(d->bytes);
        d->bytes = buf;
        d->len = need;
        d->cap = need;
    }
}

void CFDataDeleteBytes(void *data, cf_range range)
{
    CFDataReplaceBytes(data, range, NULL, 0);
}

/* ---- sets ---- */

static cf_set *cf_set_alloc(size_t cap)
{
    cf_set *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->isa = &plsr_cfset_class;
    s->cap = cap ? cap : 8;
    s->items = calloc(s->cap, sizeof(void *));
    return s;
}

void *CFSetCreate(void *alloc, const void **values, long numValues,
                  const void *callbacks)
{
    cf_set *s;
    (void)alloc;
    (void)callbacks;
    if (numValues < 0)
        return NULL;
    s = cf_set_alloc(numValues > 0 ? (size_t)numValues : 0);
    if (!s)
        return NULL;
    for (long i = 0; i < numValues; i++)
        CFSetAddValue(s, values[i]);
    return s;
}

void *CFSetCreateMutable(void *alloc, long capacity, const void *callbacks)
{
    (void)alloc;
    (void)callbacks;
    return cf_set_alloc(capacity > 0 ? (size_t)capacity : 0);
}

void *CFSetCreateCopy(void *alloc, void *set)
{
    cf_set *s = set;
    (void)alloc;
    if (!s)
        return NULL;
    return CFSetCreate(NULL, (const void **)s->items, (long)s->count, NULL);
}

long CFSetGetCount(void *set)
{
    return set ? (long)((cf_set *)set)->count : 0;
}

unsigned char CFSetContainsValue(void *set, const void *value)
{
    cf_set *s = set;
    if (!s)
        return 0;
    for (unsigned long i = 0; i < s->count; i++)
        if (CFEqual(s->items[i], value))
            return 1;
    return 0;
}

void *CFSetGetValue(void *set, const void *value)
{
    cf_set *s = set;
    if (!s)
        return NULL;
    for (unsigned long i = 0; i < s->count; i++)
        if (CFEqual(s->items[i], value))
            return s->items[i];
    return NULL;
}

void CFSetGetValues(void *set, const void **values)
{
    cf_set *s = set;
    if (!s || !values)
        return;
    for (unsigned long i = 0; i < s->count; i++)
        values[i] = s->items[i];
}

void CFSetAddValue(void *set, const void *value)
{
    cf_set *s = set;
    if (!s || !value || CFSetContainsValue(s, value))
        return;
    if (s->count == s->cap) {
        size_t cap = s->cap * 2;
        void **p = realloc(s->items, cap * sizeof(void *));
        if (!p)
            return;
        s->items = p;
        s->cap = cap;
    }
    s->items[s->count++] = (void *)value;
}

void CFSetSetValue(void *set, const void *value)
{
    cf_set *s = set;
    if (!s || !value)
        return;
    for (unsigned long i = 0; i < s->count; i++)
        if (CFEqual(s->items[i], value)) {
            s->items[i] = (void *)value;
            return;
        }
    CFSetAddValue(s, value);
}

void CFSetRemoveValue(void *set, const void *value)
{
    cf_set *s = set;
    if (!s)
        return;
    for (unsigned long i = 0; i < s->count; i++)
        if (CFEqual(s->items[i], value)) {
            memmove(&s->items[i], &s->items[i + 1],
                    (s->count - i - 1) * sizeof(void *));
            s->count--;
            return;
        }
}

void CFSetRemoveAllValues(void *set)
{
    cf_set *s = set;
    if (s)
        s->count = 0;
}

/* ---- UUID ---- */

void *CFUUIDCreate(void *alloc)
{
    cf_uuid *u = calloc(1, sizeof(*u));
    int fd;
    (void)alloc;
    if (!u)
        return NULL;
    u->isa = &plsr_cfuuid_class;
    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, u->bytes, 16) != 16) {
        for (int i = 0; i < 16; i++)
            u->bytes[i] = (unsigned char)(rand() & 0xff);
    }
    if (fd >= 0)
        close(fd);
    u->bytes[6] = (unsigned char)((u->bytes[6] & 0x0f) | 0x40);
    u->bytes[8] = (unsigned char)((u->bytes[8] & 0x3f) | 0x80);
    return u;
}

void *CFUUIDCreateWithBytes(void *alloc, unsigned char b0, unsigned char b1,
                            unsigned char b2, unsigned char b3, unsigned char b4,
                            unsigned char b5, unsigned char b6, unsigned char b7,
                            unsigned char b8, unsigned char b9, unsigned char b10,
                            unsigned char b11, unsigned char b12, unsigned char b13,
                            unsigned char b14, unsigned char b15)
{
    cf_uuid *u = calloc(1, sizeof(*u));
    (void)alloc;
    if (!u)
        return NULL;
    u->isa = &plsr_cfuuid_class;
    u->bytes[0] = b0; u->bytes[1] = b1; u->bytes[2] = b2; u->bytes[3] = b3;
    u->bytes[4] = b4; u->bytes[5] = b5; u->bytes[6] = b6; u->bytes[7] = b7;
    u->bytes[8] = b8; u->bytes[9] = b9; u->bytes[10] = b10; u->bytes[11] = b11;
    u->bytes[12] = b12; u->bytes[13] = b13; u->bytes[14] = b14; u->bytes[15] = b15;
    return u;
}

void *CFUUIDCreateString(void *alloc, void *uuid)
{
    const unsigned char *b = uuid ? ((cf_uuid *)uuid)->bytes : NULL;
    char buf[64];
    (void)alloc;
    if (!b)
        return NULL;
    snprintf(buf, sizeof buf,
             "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9],
             b[10], b[11], b[12], b[13], b[14], b[15]);
    return ns_string(buf);
}

void CFUUIDGetUUIDBytes(void *uuid, unsigned char out[16])
{
    if (uuid && out)
        memcpy(out, ((cf_uuid *)uuid)->bytes, 16);
}

/* ---- URLs ---- */

static cf_url *cf_url_alloc(const char *string, const char *path, int is_file)
{
    cf_url *u = calloc(1, sizeof(*u));
    if (!u)
        return NULL;
    u->isa = &plsr_cfurl_class;
    u->string = strdup(string ? string : "");
    u->path = strdup(path ? path : "");
    u->is_file = is_file;
    return u;
}

static char *url_percent_decode(const char *s)
{
    size_t n = strlen(s);
    char *out = malloc(n + 1);
    size_t o = 0;
    if (!out)
        return NULL;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n && isxdigit((unsigned char)s[i + 1]) &&
            isxdigit((unsigned char)s[i + 2])) {
            char hex[3] = { s[i + 1], s[i + 2], 0 };
            out[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            out[o++] = s[i];
        }
    }
    out[o] = 0;
    return out;
}

void *CFURLCreateWithString(void *alloc, void *string, void *baseURL)
{
    const char *s;
    char *path;
    (void)alloc;
    (void)baseURL;
    if (!string)
        return NULL;
    s = cstr(string);
    path = NULL;
    if (!strncmp(s, "file://", 7)) {
        path = url_percent_decode(s + 7);
        return cf_url_alloc(s, path ? path : "", 1);
    }
    return cf_url_alloc(s, "", 0);
}

void *CFURLCreateWithFileSystemPath(void *alloc, void *path, long style,
                                    unsigned char isDirectory)
{
    const char *p;
    char *buf;
    size_t n;
    cf_url *u;
    (void)alloc;
    (void)style;
    if (!path)
        return NULL;
    p = cstr(path);
    n = strlen(p);
    buf = malloc(n + 16);
    if (!buf)
        return NULL;
    snprintf(buf, n + 16, "file://%s%s", p[0] == '/' ? "" : "/", p);
    if (isDirectory && n && p[n - 1] != '/')
        strcat(buf, "/");
    u = cf_url_alloc(buf, p, 1);
    free(buf);
    return u;
}

void *CFURLCreateFromFileSystemRepresentation(void *alloc,
                                              const unsigned char *buffer,
                                              long length,
                                              unsigned char isDirectory)
{
    char *p;
    void *str;
    void *url;
    if (!buffer || length < 0)
        return NULL;
    p = malloc((size_t)length + 1);
    if (!p)
        return NULL;
    memcpy(p, buffer, (size_t)length);
    p[length] = 0;
    str = ns_string(p);
    url = CFURLCreateWithFileSystemPath(alloc, str, 0, isDirectory);
    free(p);
    return url;
}

void *CFURLCopyAbsoluteURL(void *url)
{
    cf_url *u = url;
    if (!u)
        return NULL;
    return cf_url_alloc(u->string, u->path, u->is_file);
}

void *CFURLGetString(void *url)
{
    cf_url *u = url;
    return u ? ns_string(u->string) : NULL;
}

void *CFURLCopyFileSystemPath(void *url, long style)
{
    cf_url *u = url;
    (void)style;
    if (!u)
        return NULL;
    if (u->path)
        return ns_string(u->path);
    if (u->is_file)
        return ns_string(u->string + 7);
    return ns_string("");
}

unsigned char CFURLGetFileSystemRepresentation(void *url, unsigned char resolve,
                                               unsigned char *buffer,
                                               long maxLen)
{
    cf_url *u = url;
    const char *p;
    size_t n;
    (void)resolve;
    if (!u || !buffer || maxLen <= 0)
        return 0;
    p = u->path ? u->path : "";
    n = strlen(p);
    if ((long)n + 1 > maxLen)
        return 0;
    memcpy(buffer, p, n + 1);
    return 1;
}

unsigned char CFURLIsFileURL(void *url)
{
    cf_url *u = url;
    return u && u->is_file;
}

void *CFURLCreateCopyAppendingPathComponent(void *alloc, void *url,
                                            void *component,
                                            unsigned char isDirectory)
{
    cf_url *u = url;
    const char *c;
    char *path;
    size_t n, m;
    void *out;
    (void)alloc;
    if (!u || !component)
        return NULL;
    c = cstr(component);
    n = strlen(u->path ? u->path : "");
    m = strlen(c);
    path = malloc(n + m + 3);
    if (!path)
        return NULL;
    snprintf(path, n + m + 3, "%s%s%s%s", u->path ? u->path : "",
             n && u->path[n - 1] == '/' ? "" : "/", c, isDirectory ? "/" : "");
    {
        void *s = ns_string(path);
        out = CFURLCreateWithFileSystemPath(NULL, s, 0, 0);
    }
    free(path);
    return out;
}

void *CFURLCreateCopyDeletingLastPathComponent(void *alloc, void *url)
{
    cf_url *u = url;
    char *path;
    char *slash;
    void *out;
    (void)alloc;
    if (!u)
        return NULL;
    path = strdup(u->path ? u->path : "");
    if (!path)
        return NULL;
    slash = strrchr(path, '/');
    if (slash && slash != path)
        *slash = 0;
    else if (slash == path)
        path[1] = 0;
    out = CFURLCreateWithFileSystemPath(NULL, ns_string(path), 0, 0);
    free(path);
    return out;
}

/* ---- bundles ---- */

void *CFBundleGetMainBundle(void)
{
    void *cls = objc_getClass("NSBundle");
    return cls ? send0(cls, "mainBundle") : NULL;
}

void *CFBundleGetIdentifier(void *bundle)
{
    return bundle ? send0(bundle, "bundleIdentifier") : NULL;
}

void *CFBundleGetValueForInfoDictionaryKey(void *bundle, void *key)
{
    return bundle && key ? send1(bundle, "objectForInfoDictionaryKey:", key)
                         : NULL;
}

void *CFBundleCopyBundleURL(void *bundle)
{
    void *path = bundle ? send0(bundle, "bundlePath") : NULL;
    return path ? CFURLCreateWithFileSystemPath(NULL, path, 0, 0) : NULL;
}

void *CFBundleCopyExecutableURL(void *bundle)
{
    void *name;
    const char *n;
    char buf[4096];
    if (!bundle)
        return NULL;
    name = CFBundleGetValueForInfoDictionaryKey(bundle,
                                                ns_string("CFBundleExecutable"));
    n = name ? cstr(name) : NULL;
    if (!n || !*n)
        return NULL;
    snprintf(buf, sizeof buf, "%s/Contents/MacOS/%s", cstr(send0(bundle, "bundlePath")),
             n);
    return CFURLCreateWithFileSystemPath(NULL, ns_string(buf), 0, 0);
}

void *CFBundleCopyResourceURL(void *bundle, void *resourceName,
                              void *resourceType, void *subDir)
{
    char buf[4096];
    const char *name = cstr(resourceName);
    const char *type = resourceType ? cstr(resourceType) : "";
    const char *sub = subDir ? cstr(subDir) : "";
    if (!bundle || !*name)
        return NULL;
    snprintf(buf, sizeof buf, "%s/Contents/Resources/%s%s%s%s%s",
             cstr(send0(bundle, "bundlePath")), *sub ? sub : "",
             *sub ? "/" : "", name, *type ? "." : "", type);
    return CFURLCreateWithFileSystemPath(NULL, ns_string(buf), 0, 0);
}

void *CFBundleCopyLocalizedString(void *bundle, void *key, void *value,
                                  void *tableName)
{
    (void)bundle;
    (void)tableName;
    return key ? key : value;
}

/* ---- run loop ---- */

typedef struct cf_timer {
    void *isa;
    struct cf_timer *next;
    double when;
    double interval;
    int repeats;
    int valid;
    void (*callout)(void *, void *);
    void *info;
} cf_timer;

typedef struct cf_observer {
    void *isa;
    struct cf_observer *next;
    unsigned long activities;
    int repeats;
    int valid;
    void (*callout)(void *, unsigned long, void *);
    void *info;
} cf_observer;

typedef struct cf_source {
    void *isa;
    struct cf_source *next;
    int valid;
    int pending;
    void *context;
} cf_source;

typedef struct {
    void *isa;
    cf_timer *timers;
    cf_observer *observers;
    cf_source *sources;
    int stopped;
    int running;
    int waiting;
    char *mode;
} cf_runloop;

typedef struct {
    long version;
    void *info;
    const void *retain;
    const void *release;
    const void *copyDescription;
    const void *equal;
    const void *hash;
    void (*schedule)(void *, void *, void *);
    void (*cancel)(void *, void *, void *);
    void (*perform)(void *);
} cf_source_context;

static cf_runloop cf_main_loop = { &plsr_cfrunloop_class, NULL, NULL, NULL, 0, 0, 0, NULL };
static __thread cf_runloop *cf_tls_loop;
/* Sources and stop requests may be signalled from a dispatch or worker
 * thread while the application thread is asleep in the run loop. */
static pthread_mutex_t cf_runloop_wait_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cf_runloop_wait_cond = PTHREAD_COND_INITIALIZER;

static void cf_runloop_wake_waiters(void)
{
    pthread_mutex_lock(&cf_runloop_wait_lock);
    pthread_cond_broadcast(&cf_runloop_wait_cond);
    pthread_mutex_unlock(&cf_runloop_wait_lock);
}

static void cf_runloop_wait(double seconds)
{
    struct timespec until;
    if (seconds < 0)
        seconds = 0;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += (time_t)seconds;
    until.tv_nsec += (long)((seconds - (double)(time_t)seconds) * 1e9);
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&cf_runloop_wait_lock);
    pthread_cond_timedwait(&cf_runloop_wait_cond, &cf_runloop_wait_lock, &until);
    pthread_mutex_unlock(&cf_runloop_wait_lock);
}

static double cf_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec - 978307200.0 + (double)tv.tv_usec / 1e6;
}

double CFAbsoluteTimeGetCurrent(void) { return cf_now(); }

void *CFRunLoopGetMain(void) { return &cf_main_loop; }

void *CFRunLoopGetCurrent(void)
{
    if (!cf_tls_loop)
        cf_tls_loop = &cf_main_loop;
    return cf_tls_loop;
}

void CFRunLoopStop(void *loop)
{
    cf_runloop *l = loop;
    if (l) {
        pthread_mutex_lock(&cf_runloop_wait_lock);
        l->stopped = 1;
        pthread_cond_broadcast(&cf_runloop_wait_cond);
        pthread_mutex_unlock(&cf_runloop_wait_lock);
    }
}

void CFRunLoopWakeUp(void *loop) { (void)loop; cf_runloop_wake_waiters(); }

unsigned char CFRunLoopIsWaiting(void *loop)
{
    return loop ? (unsigned char)((cf_runloop *)loop)->waiting : 0;
}

static void cf_fire_observers(cf_runloop *l, unsigned long activity)
{
    for (cf_observer *o = l->observers; o; o = o->next)
        if (o->valid && (o->activities & activity) && o->callout)
            o->callout(o, activity, o->info);
}

long CFRunLoopRunInMode(void *mode, double seconds,
                        unsigned char returnAfterSourceHandled)
{
    cf_runloop *l = CFRunLoopGetCurrent();
    double deadline = cf_now() + (seconds < 0 ? 1e9 : seconds);
    long result = 1; /* kCFRunLoopRunFinished */
    (void)mode;
    l->stopped = 0;
    l->running++;
    cf_fire_observers(l, 1u); /* kCFRunLoopEntry */
    for (;;) {
        double now;
        int have_next = 0, handled = 0;
        double next = 0.0;
        if (l->stopped) {
            result = 2; /* kCFRunLoopRunStopped */
            break;
        }
        /* Real CF: a run loop with no valid sources or timers in the mode
         * finishes immediately instead of waiting for the deadline. */
        {
            int has_work = 0;
            for (cf_timer *t = l->timers; t; t = t->next)
                if (t->valid) { has_work = 1; break; }
            if (!has_work)
                for (cf_source *s = l->sources; s; s = s->next)
                    if (s->valid) { has_work = 1; break; }
            if (!has_work) {
                result = 1; /* kCFRunLoopRunFinished */
                break;
            }
        }
        now = cf_now();
        for (cf_timer *t = l->timers; t; t = t->next) {
            if (!t->valid || t->when > now)
                continue;
            if (t->repeats)
                t->when = now + t->interval;
            else
                t->valid = 0;
            if (t->callout)
                t->callout(t, t->info);
            if (l->stopped)
                break;
        }
        if (l->stopped) {
            result = 2;
            break;
        }
        for (cf_source *s = l->sources; s; s = s->next) {
            if (!s->valid || !s->pending)
                continue;
            s->pending = 0;
            if (s->context) {
                cf_source_context *c = s->context;
                if (c->perform)
                    c->perform(c->info);
            }
            handled = 1;
            if (returnAfterSourceHandled)
                break;
        }
        if (l->stopped) {
            result = 2;
            break;
        }
        if (handled && returnAfterSourceHandled) {
            result = 4; /* kCFRunLoopRunHandledSource */
            break;
        }
        for (cf_timer *t = l->timers; t; t = t->next)
            if (t->valid && (!have_next || t->when < next)) {
                next = t->when;
                have_next = 1;
            }
        {
            double wait = have_next ? next - cf_now() : deadline - cf_now();
            double left = deadline - cf_now();
            if (wait > left)
                wait = left;
            /* Bound the sleep to close the signal-before-wait race while
             * callers mutate source state without holding the wait lock. */
            if (wait > 0.05)
                wait = 0.05;
            if (wait > 0) {
                l->waiting = 1;
                cf_runloop_wait(wait);
                l->waiting = 0;
            }
            if (cf_now() >= deadline) {
                result = 3; /* kCFRunLoopRunTimedOut */
                break;
            }
        }
    }
    cf_fire_observers(l, 128u); /* kCFRunLoopExit */
    l->running--;
    return result;
}

void CFRunLoopRun(void)
{
    CFRunLoopRunInMode(CFRunLoopGetCurrent(), 1e9, 0);
}

void CFRunLoopAddTimer(void *loop, void *timer, void *mode)
{
    cf_runloop *l = loop;
    cf_timer *t = timer;
    (void)mode;
    if (!l || !t)
        return;
    for (cf_timer *i = l->timers; i; i = i->next)
        if (i == t)
            return;
    t->next = l->timers;
    l->timers = t;
    cf_runloop_wake_waiters();
}

void CFRunLoopRemoveTimer(void *loop, void *timer, void *mode)
{
    cf_runloop *l = loop;
    (void)mode;
    if (!l || !timer)
        return;
    for (cf_timer **pp = &l->timers; *pp; pp = &(*pp)->next)
        if (*pp == timer) {
            *pp = (*pp)->next;
            cf_runloop_wake_waiters();
            return;
        }
}

void CFRunLoopAddObserver(void *loop, void *observer, void *mode)
{
    cf_runloop *l = loop;
    cf_observer *o = observer;
    (void)mode;
    if (!l || !o)
        return;
    for (cf_observer *i = l->observers; i; i = i->next)
        if (i == o)
            return;
    o->next = l->observers;
    l->observers = o;
}

void CFRunLoopRemoveObserver(void *loop, void *observer, void *mode)
{
    cf_runloop *l = loop;
    (void)mode;
    if (!l || !observer)
        return;
    for (cf_observer **pp = &l->observers; *pp; pp = &(*pp)->next)
        if (*pp == observer) {
            *pp = (*pp)->next;
            return;
        }
}

void CFRunLoopAddSource(void *loop, void *source, void *mode)
{
    cf_runloop *l = loop;
    cf_source *s = source;
    (void)mode;
    if (!l || !s)
        return;
    for (cf_source *i = l->sources; i; i = i->next)
        if (i == s)
            return;
    s->next = l->sources;
    l->sources = s;
}

void CFRunLoopRemoveSource(void *loop, void *source, void *mode)
{
    cf_runloop *l = loop;
    (void)mode;
    if (!l || !source)
        return;
    for (cf_source **pp = &l->sources; *pp; pp = &(*pp)->next)
        if (*pp == source) {
            *pp = (*pp)->next;
            return;
        }
}

void *CFRunLoopCopyCurrentMode(void *loop)
{
    (void)loop;
    return ns_string("kCFRunLoopDefaultMode");
}

void *CFRunLoopTimerCreate(void *alloc, double fireDate, double interval,
                           unsigned long flags, long order,
                           void (*callout)(void *, void *), void *info)
{
    cf_timer *t = calloc(1, sizeof(*t));
    (void)alloc;
    (void)flags;
    (void)order;
    if (!t)
        return NULL;
    t->isa = &plsr_cftimer_class;
    t->when = fireDate;
    t->interval = interval;
    t->repeats = interval > 0.0;
    t->valid = 1;
    t->callout = callout;
    t->info = info;
    return t;
}

void CFRunLoopTimerInvalidate(void *timer)
{
    cf_timer *t = timer;
    if (t)
        t->valid = 0;
}

unsigned char CFRunLoopTimerIsValid(void *timer)
{
    return timer ? (unsigned char)((cf_timer *)timer)->valid : 0;
}

void CFRunLoopTimerSetNextFireDate(void *timer, double fireDate)
{
    cf_timer *t = timer;
    if (t) {
        t->when = fireDate;
        t->valid = 1;
    }
}

double CFRunLoopTimerGetNextFireDate(void *timer)
{
    return timer ? ((cf_timer *)timer)->when : 0.0;
}

unsigned char CFRunLoopTimerDoesRepeat(void *timer)
{
    return timer ? (unsigned char)((cf_timer *)timer)->repeats : 0;
}

void *CFRunLoopObserverCreate(void *alloc, unsigned long activities,
                              unsigned char repeats, long order,
                              void (*callout)(void *, unsigned long, void *),
                              void *info)
{
    cf_observer *o = calloc(1, sizeof(*o));
    (void)alloc;
    (void)order;
    if (!o)
        return NULL;
    o->isa = &plsr_cfobserver_class;
    o->activities = activities;
    o->repeats = repeats;
    o->valid = 1;
    o->callout = callout;
    o->info = info;
    return o;
}

void CFRunLoopObserverInvalidate(void *observer)
{
    cf_observer *o = observer;
    if (o)
        o->valid = 0;
}

unsigned char CFRunLoopObserverIsValid(void *observer)
{
    return observer ? (unsigned char)((cf_observer *)observer)->valid : 0;
}

void *CFRunLoopSourceCreate(void *alloc, long order, void *context)
{
    cf_source *s = calloc(1, sizeof(*s));
    (void)alloc;
    (void)order;
    if (!s)
        return NULL;
    s->isa = &plsr_cfsource_class;
    s->valid = 1;
    s->context = context;
    return s;
}

void CFRunLoopSourceSignal(void *source)
{
    cf_source *s = source;
    if (s && s->valid) {
        s->pending = 1;
        cf_runloop_wake_waiters();
    }
}

void CFRunLoopSourceInvalidate(void *source)
{
    cf_source *s = source;
    if (s) {
        s->valid = 0;
        s->pending = 0;
    }
}

unsigned char CFRunLoopSourceIsValid(void *source)
{
    return source ? (unsigned char)((cf_source *)source)->valid : 0;
}

/* ---- notification center ---- */

typedef struct cf_observer_entry {
    void *isa;
    struct cf_observer_entry *next;
    void *observer;
    void (*callBack)(void *, void *, void *, void *, void *);
    void *name;
    const void *object;
} cf_observer_entry;

typedef struct {
    void *isa;
    cf_observer_entry *entries;
} cf_notification_center;

static cf_notification_center cf_local_center = { &plsr_cfnc_class, NULL };

void *CFNotificationCenterGetLocalCenter(void) { return &cf_local_center; }
void *CFNotificationCenterGetDistributedCenter(void) { return &cf_local_center; }

void CFNotificationCenterAddObserver(void *center, void *observer,
                                     void (*callBack)(void *, void *, void *,
                                                      void *, void *),
                                     void *name, const void *object,
                                     long suspensionBehavior)
{
    cf_notification_center *c = center;
    cf_observer_entry *e;
    (void)suspensionBehavior;
    if (!c || !callBack)
        return;
    e = calloc(1, sizeof(*e));
    if (!e)
        return;
    e->observer = observer;
    e->callBack = callBack;
    e->name = name;
    e->object = object;
    e->next = c->entries;
    c->entries = e;
}

void CFNotificationCenterRemoveObserver(void *center, void *observer, void *name,
                                        const void *object)
{
    cf_notification_center *c = center;
    if (!c)
        return;
    for (cf_observer_entry **pp = &c->entries; *pp; pp = &(*pp)->next) {
        cf_observer_entry *e = *pp;
        if (e->observer == observer &&
            (!name || CFEqual(e->name, name)) &&
            (!object || e->object == object)) {
            *pp = e->next;
            free(e);
            return;
        }
    }
}

void CFNotificationCenterPostNotificationName(void *center, void *name,
                                              const void *object, void *userInfo,
                                              unsigned char deliverImmediately)
{
    cf_notification_center *c = center;
    (void)deliverImmediately;
    if (!c || !name)
        return;
    for (cf_observer_entry *e = c->entries; e; e = e->next) {
        if (e->name && !CFEqual(e->name, name))
            continue;
        if (e->object && e->object != object)
            continue;
        e->callBack(c, e->observer, name, (void *)object, userInfo);
    }
}

/* ---- preferences ---- */

typedef struct cf_pref {
    struct cf_pref *next;
    char *app;
    char *key;
    void *value;
} cf_pref;

static cf_pref *cf_prefs;

static const char *cf_app_id(void *appID)
{
    if (!appID)
        return "current";
    if (CFEqual(appID, plsr_kCFPreferencesCurrentApplication)) {
        void *bundle = CFBundleGetMainBundle();
        void *ident = bundle ? CFBundleGetIdentifier(bundle) : NULL;
        return ident ? cstr(ident) : "current";
    }
    if (CFEqual(appID, plsr_kCFPreferencesAnyApplication))
        return "any";
    return cstr(appID);
}

void *CFPreferencesCopyAppValue(void *key, void *appID)
{
    const char *app = cf_app_id(appID);
    const char *k = cstr(key);
    for (cf_pref *p = cf_prefs; p; p = p->next)
        if (!strcmp(p->app, app) && !strcmp(p->key, k))
            return p->value;
    return NULL;
}

void CFPreferencesSetAppValue(void *key, void *value, void *appID)
{
    const char *app = cf_app_id(appID);
    const char *k = cstr(key);
    cf_pref *p;
    if (!key)
        return;
    for (p = cf_prefs; p; p = p->next)
        if (!strcmp(p->app, app) && !strcmp(p->key, k)) {
            if (!value) {
                p->value = NULL;
            } else {
                p->value = value;
            }
            return;
        }
    if (!value)
        return;
    p = calloc(1, sizeof(*p));
    if (!p)
        return;
    p->app = strdup(app);
    p->key = strdup(k);
    p->value = value;
    p->next = cf_prefs;
    cf_prefs = p;
}

unsigned char CFPreferencesAppSynchronize(void *appID)
{
    (void)appID;
    return 1;
}

unsigned char CFPreferencesGetAppBooleanValue(void *key, void *appID,
                                              unsigned char *valid)
{
    void *v = CFPreferencesCopyAppValue(key, appID);
    if (valid)
        *valid = v ? 1 : 0;
    return v ? (unsigned char)(send_i(v, "boolValue") != 0) : 0;
}

long CFPreferencesGetAppIntegerValue(void *key, void *appID,
                                     unsigned char *valid)
{
    void *v = CFPreferencesCopyAppValue(key, appID);
    if (valid)
        *valid = v ? 1 : 0;
    return v ? (long)send_ll(v, "longLongValue") : 0;
}

void *CFPreferencesCopyKeyList(void *appID, void *user, void *host)
{
    const char *app = cf_app_id(appID);
    void *cls = ns_array_class();
    void *array = cls ? send0(cls, "array") : NULL;
    (void)user;
    (void)host;
    for (cf_pref *p = cf_prefs; p; p = p->next)
        if (!strcmp(p->app, app) && p->value)
            send1(array, "addObject:", ns_string(p->key));
    return array;
}

/* ---- CFSTR class methods ---- */

static const char *cfconst_cstr(void *self)
{
    return ((cf_const_string *)self)->cstr;
}

static const char *cfconst_utf8(void *self, const char *sel)
{
    (void)sel;
    return cfconst_cstr(self);
}

static unsigned long cfconst_length(void *self, const char *sel)
{
    (void)sel;
    return ((cf_const_string *)self)->len;
}

static unsigned short cfconst_char_at(void *self, const char *sel, unsigned long idx)
{
    cf_const_string *s = self;
    (void)sel;
    return idx < s->len ? (unsigned short)(unsigned char)s->cstr[idx] : 0;
}

static void cfconst_get_chars(void *self, const char *sel, unsigned short *buffer,
                              cf_range range)
{
    cf_const_string *s = self;
    (void)sel;
    if (!buffer)
        return;
    for (unsigned long i = 0; i < range.length; i++) {
        unsigned long idx = range.location + i;
        buffer[i] = idx < s->len ? (unsigned short)(unsigned char)s->cstr[idx] : 0;
    }
}

static void *cfconst_description(void *self, const char *sel)
{
    (void)sel;
    return self;
}

static int cfconst_is_equal(void *self, const char *sel, void *other)
{
    (void)sel;
    if (self == other)
        return 1;
    return other && !strcmp(cfconst_cstr(self), cstr(other));
}

static unsigned long cfconst_hash(void *self, const char *sel)
{
    const char *p = cfconst_cstr(self);
    unsigned long h = 5381;
    (void)sel;
    while (*p)
        h = (h * 33) ^ (unsigned char)*p++;
    return h;
}

/* ---- data/set/uuid/url object methods ---- */

static unsigned long cfset_count_m(void *self, const char *sel)
{
    (void)sel;
    return ((cf_set *)self)->count;
}

static void *cfset_description(void *self, const char *sel)
{
    cf_set *s = self;
    struct sbuf b = { 0 };
    void *out;
    (void)sel;
    sb_cstr(&b, "{(");
    for (unsigned long i = 0; i < s->count; i++) {
        if (i)
            sb_cstr(&b, ", ");
        sb_cstr(&b, cstr(s->items[i]));
    }
    sb_cstr(&b, ")}");
    out = ns_string_bytes(b.p ? b.p : "{}", b.len);
    free(b.p);
    return out;
}

static void *cfuuid_description(void *self, const char *sel)
{
    (void)sel;
    return CFUUIDCreateString(NULL, self);
}

static void *cfurl_absolute_string(void *self, const char *sel)
{
    (void)sel;
    return ns_string(((cf_url *)self)->string);
}

static void *cfurl_path(void *self, const char *sel)
{
    (void)sel;
    return ns_string(((cf_url *)self)->path);
}

static signed char cfurl_is_file(void *self, const char *sel)
{
    (void)sel;
    return (signed char)((cf_url *)self)->is_file;
}

static void *cfurl_description(void *self, const char *sel)
{
    (void)sel;
    return ns_string(((cf_url *)self)->string);
}

/* ---- class metadata ---- */

typedef struct {
    uint32_t entsize_flags;
    uint32_t count;
    plsr_method methods[16];
} cf_method_array;

#define RO(flags_, size_, name_, methods_) \
    { (flags_), 0, (size_), 0, NULL, (name_), (plsr_method_list *)(methods_), \
      NULL, NULL, NULL, NULL }

static cf_method_array cfconst_methods = { 24, 9, {
    { "UTF8String", "", (void *)cfconst_utf8 },
    { "length", "", (void *)cfconst_length },
    { "characterAtIndex:", "", (void *)cfconst_char_at },
    { "getCharacters:range:", "", (void *)cfconst_get_chars },
    { "description", "", (void *)cfconst_description },
    { "isEqual:", "", (void *)cfconst_is_equal },
    { "isEqualToString:", "", (void *)cfconst_is_equal },
    { "hash", "", (void *)cfconst_hash },
    { "copy", "", (void *)CFRetain },
} };

static cf_method_array cfset_methods = { 24, 2, {
    { "count", "", (void *)cfset_count_m },
    { "description", "", (void *)cfset_description },
} };

static cf_method_array cfuuid_methods = { 24, 1, {
    { "description", "", (void *)cfuuid_description },
} };

static cf_method_array cfurl_methods = { 24, 4, {
    { "absoluteString", "", (void *)cfurl_absolute_string },
    { "path", "", (void *)cfurl_path },
    { "isFileURL", "", (void *)cfurl_is_file },
    { "description", "", (void *)cfurl_description },
} };

static cf_method_array cfdata_methods = { 24, 5, {
    { "bytes", "", (void *)data_bytes_m },
    { "length", "", (void *)data_length_m },
    { "appendBytes:length:", "", (void *)data_append_m },
    { "setLength:", "", (void *)data_set_length_m },
    { "mutableBytes", "", (void *)data_mutable_bytes_m },
} };

static const plsr_class_ro cfconst_ro =
    RO(0, sizeof(cf_const_string), "__NSCFConstantString", &cfconst_methods);
static const plsr_class_ro cfconst_meta_ro =
    RO(PLSR_RO_META, sizeof(cf_const_string), "__NSCFConstantString", NULL);
static const plsr_class_ro cfbool_ro =
    RO(0, sizeof(cf_boolean), "__NSCFBoolean", NULL);
static const plsr_class_ro cfbool_meta_ro =
    RO(PLSR_RO_META, sizeof(cf_boolean), "__NSCFBoolean", NULL);
static const plsr_class_ro cfdata_ro =
    RO(0, sizeof(cf_data), "__NSCFData", &cfdata_methods);
static const plsr_class_ro cfdata_meta_ro =
    RO(PLSR_RO_META, sizeof(cf_data), "__NSCFData", NULL);
static const plsr_class_ro cfset_ro =
    RO(0, sizeof(cf_set), "__NSCFSet", &cfset_methods);
static const plsr_class_ro cfset_meta_ro =
    RO(PLSR_RO_META, sizeof(cf_set), "__NSCFSet", NULL);
static const plsr_class_ro cfuuid_ro =
    RO(0, sizeof(cf_uuid), "__NSCFUUID", &cfuuid_methods);
static const plsr_class_ro cfuuid_meta_ro =
    RO(PLSR_RO_META, sizeof(cf_uuid), "__NSCFUUID", NULL);
static const plsr_class_ro cfurl_ro =
    RO(0, sizeof(cf_url), "__NSCFURL", &cfurl_methods);
static const plsr_class_ro cfurl_meta_ro =
    RO(PLSR_RO_META, sizeof(cf_url), "__NSCFURL", NULL);
static const plsr_class_ro cfrunloop_ro =
    RO(0, sizeof(cf_runloop), "__NSCFRunLoop", NULL);
static const plsr_class_ro cfrunloop_meta_ro =
    RO(PLSR_RO_META, sizeof(cf_runloop), "__NSCFRunLoop", NULL);
static const plsr_class_ro cfnc_ro =
    RO(0, sizeof(cf_notification_center), "__NSCFNotificationCenter", NULL);
static const plsr_class_ro cfnc_meta_ro =
    RO(PLSR_RO_META, sizeof(cf_notification_center), "__NSCFNotificationCenter", NULL);
static const plsr_class_ro cftimer_ro =
    RO(0, sizeof(cf_timer), "__NSCFTimer", NULL);
static const plsr_class_ro cftimer_meta_ro =
    RO(PLSR_RO_META, sizeof(cf_timer), "__NSCFTimer", NULL);
static const plsr_class_ro cfobserver_ro =
    RO(0, sizeof(cf_observer), "__NSCFObserver", NULL);
static const plsr_class_ro cfobserver_meta_ro =
    RO(PLSR_RO_META, sizeof(cf_observer), "__NSCFObserver", NULL);
static const plsr_class_ro cfsource_ro =
    RO(0, sizeof(cf_source), "__NSCFSource", NULL);
static const plsr_class_ro cfsource_meta_ro =
    RO(PLSR_RO_META, sizeof(cf_source), "__NSCFSource", NULL);
static const plsr_class_ro cfalloc_ro =
    RO(0, sizeof(cf_allocator), "__NSCFAllocator", NULL);
static const plsr_class_ro cfalloc_meta_ro =
    RO(PLSR_RO_META, sizeof(cf_allocator), "__NSCFAllocator", NULL);

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

static void cf_init(void) __attribute__((constructor));
static void cf_init(void)
{
    wire(&plsr_cfconst_class, &cfconst_meta, &plsr_NSString_class,
         &plsr_NSString_meta, &cfconst_ro, &cfconst_meta_ro);
    wire(&plsr_cfbool_class, &plsr_cfbool_meta, &plsr_NSNumber_class,
         &plsr_NSNumber_meta, &cfbool_ro, &cfbool_meta_ro);
    wire(&plsr_cfdata_class, &plsr_cfdata_meta, &plsr_NSData_class,
         &plsr_NSData_meta, &cfdata_ro, &cfdata_meta_ro);
    wire(&plsr_cfset_class, &plsr_cfset_meta, &plsr_NSObject_class,
         &plsr_NSObject_meta, &cfset_ro, &cfset_meta_ro);
    wire(&plsr_cfuuid_class, &plsr_cfuuid_meta, &plsr_NSObject_class,
         &plsr_NSObject_meta, &cfuuid_ro, &cfuuid_meta_ro);
    wire(&plsr_cfurl_class, &plsr_cfurl_meta, &plsr_NSObject_class,
         &plsr_NSObject_meta, &cfurl_ro, &cfurl_meta_ro);
    wire(&plsr_cfrunloop_class, &plsr_cfrunloop_meta, &plsr_NSObject_class,
         &plsr_NSObject_meta, &cfrunloop_ro, &cfrunloop_meta_ro);
    wire(&plsr_cfnc_class, &plsr_cfnc_meta, &plsr_NSObject_class,
         &plsr_NSObject_meta, &cfnc_ro, &cfnc_meta_ro);
    wire(&plsr_cftimer_class, &plsr_cftimer_meta, &plsr_NSObject_class,
         &plsr_NSObject_meta, &cftimer_ro, &cftimer_meta_ro);
    wire(&plsr_cfobserver_class, &plsr_cfobserver_meta, &plsr_NSObject_class,
         &plsr_NSObject_meta, &cfobserver_ro, &cfobserver_meta_ro);
    wire(&plsr_cfsource_class, &plsr_cfsource_meta, &plsr_NSObject_class,
         &plsr_NSObject_meta, &cfsource_ro, &cfsource_meta_ro);
    wire(&plsr_cfalloc_class, &plsr_cfalloc_meta, &plsr_NSObject_class,
         &plsr_NSObject_meta, &cfalloc_ro, &cfalloc_meta_ro);
    plsr_kCFNull = send0(objc_getClass("NSNull"), "null");
}

/* ---- callback tables and install names ---- */

typedef struct {
    long version;
    const void *retain;
    const void *release;
    const void *copyDescription;
    const void *equal;
    const void *hash;
} cf_array_callbacks;

typedef struct {
    long version;
    const void *keyRetain;
    const void *keyRelease;
    const void *keyCopyDescription;
    const void *keyEqual;
    const void *keyHash;
    const void *valueRetain;
    const void *valueRelease;
    const void *valueCopyDescription;
    const void *valueEqual;
    const void *valueHash;
} cf_dict_callbacks;

cf_array_callbacks plsr_kCFTypeArrayCallBacks __asm__("_kCFTypeArrayCallBacks") = {
    0, (const void *)CFRetain, (const void *)CFRelease,
    (const void *)CFCopyDescription, (const void *)CFEqual, (const void *)CFHash
};

cf_array_callbacks plsr_kCFTypeSetCallBacks __asm__("_kCFTypeSetCallBacks") = {
    0, (const void *)CFRetain, (const void *)CFRelease,
    (const void *)CFCopyDescription, (const void *)CFEqual, (const void *)CFHash
};

cf_dict_callbacks plsr_kCFTypeDictionaryKeyCallBacks
    __asm__("_kCFTypeDictionaryKeyCallBacks") = {
    0, (const void *)CFRetain, (const void *)CFRelease,
    (const void *)CFCopyDescription, (const void *)CFEqual, (const void *)CFHash,
    NULL, NULL, NULL, NULL, NULL
};

cf_dict_callbacks plsr_kCFTypeDictionaryValueCallBacks
    __asm__("_kCFTypeDictionaryValueCallBacks") = {
    0, NULL, NULL, NULL, NULL, NULL,
    (const void *)CFRetain, (const void *)CFRelease,
    (const void *)CFCopyDescription, (const void *)CFEqual, (const void *)CFHash
};

__attribute__((visibility("default"), used))
const char *const plsr_host_imports[] = {
    "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation",
    NULL
};
