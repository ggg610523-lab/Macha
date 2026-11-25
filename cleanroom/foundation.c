/*
 * Clean-room Foundation slice.
 *
 * Exports class/metaclass symbols for NSObject, NSConstantString, NSString,
 * NSMutableString, NSNumber, NSArray, NSMutableArray, NSDictionary, NSNull and
 * NSData, plus the NSLog entry point. Implementations are deliberately small:
 * enough for metadata and simple collection code, not a full Foundation.
 *
 * Strings use { isa; char *cstr; unsigned long len; unsigned long cap; }.
 * Constants keep the shipped layout { isa; const char *cstr; uint32_t len; }
 * and their own length/equality methods.
 */
#define _GNU_SOURCE
#include "objc_abi.h"

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void plsr_objc_register_class(plsr_class *c);
void *plsr_objc_lookup_imp(void *obj, const char *sel);
void *objc_getClass(const char *name) __asm__("_objc_getClass");
const char *class_getName(void *cls) __asm__("_class_getName");
const char *sel_getName(const char *sel) __asm__("_sel_getName");

typedef struct plsr_string {
    void *isa;
    char *cstr;
    unsigned long len;
    unsigned long cap;
} plsr_string;

/* NSString * constants exported by Foundation. Layout matches the shipped
 * NSConstantString metadata: isa, cstr, 32-bit length. */
typedef struct {
    void *isa;
    const char *cstr;
    uint32_t len;
} plsr_const_str;

/* The definition with the same asm label appears below; the constants need the
 * symbol before that point. */
extern plsr_class plsr_NSConstantString_class __asm__("_OBJC_CLASS_$_NSConstantString");

#define NS_CONST(name, text)                                                  \
    static plsr_const_str plsr_const_##name = {                               \
        &plsr_NSConstantString_class, (text), (uint32_t)(sizeof(text) - 1) };  \
    void *plsr_nsconst_##name __asm__("_" #name) = &plsr_const_##name

NS_CONST(NSFileCreationDate, "NSFileCreationDate");
NS_CONST(NSFileModificationDate, "NSFileModificationDate");
NS_CONST(NSFileSize, "NSFileSize");
NS_CONST(NSFileType, "NSFileType");
NS_CONST(NSFilePosixPermissions, "NSFilePosixPermissions");
NS_CONST(NSFileOwnerAccountName, "NSFileOwnerAccountName");
NS_CONST(NSFileGroupOwnerAccountName, "NSFileGroupOwnerAccountName");
NS_CONST(NSFileSystemFileNumber, "NSFileSystemFileNumber");
NS_CONST(NSFileExtensionHidden, "NSFileExtensionHidden");
NS_CONST(NSFileHFSCreatorCode, "NSFileHFSCreatorCode");
NS_CONST(NSFileHFSTypeCode, "NSFileHFSTypeCode");
NS_CONST(NSFileReferenceCount, "NSFileReferenceCount");
NS_CONST(NSFileIdentifier, "NSFileIdentifier");
NS_CONST(NSFileDeviceIdentifier, "NSFileDeviceIdentifier");
NS_CONST(NSFileSystemNumber, "NSFileSystemNumber");
NS_CONST(NSFileSystemSize, "NSFileSystemSize");
NS_CONST(NSFileSystemFreeSize, "NSFileSystemFreeSize");
NS_CONST(NSFileSystemNodes, "NSFileSystemNodes");
NS_CONST(NSFileSystemFreeNodes, "NSFileSystemFreeNodes");
NS_CONST(NSLocalizedDescriptionKey, "NSLocalizedDescriptionKey");
NS_CONST(NSUnderlyingErrorKey, "NSUnderlyingErrorKey");
NS_CONST(NSFilePathErrorKey, "NSFilePathErrorKey");
NS_CONST(NSURLErrorKey, "NSURLErrorKey");
NS_CONST(NSHelpAnchorErrorKey, "NSHelpAnchorErrorKey");
NS_CONST(NSDebugDescriptionErrorKey, "NSDebugDescriptionErrorKey");
NS_CONST(NSDefaultRunLoopMode, "NSDefaultRunLoopMode");
NS_CONST(NSRunLoopCommonModes, "NSRunLoopCommonModes");
NS_CONST(NSFontAttributeName, "NSFontAttributeName");
NS_CONST(NSForegroundColorAttributeName, "NSForegroundColorAttributeName");
NS_CONST(NSBackgroundColorAttributeName, "NSBackgroundColorAttributeName");
NS_CONST(NSParagraphStyleAttributeName, "NSParagraphStyleAttributeName");
NS_CONST(NSKernAttributeName, "NSKernAttributeName");
NS_CONST(NSLigatureAttributeName, "NSLigatureAttributeName");
NS_CONST(NSStrokeWidthAttributeName, "NSStrokeWidthAttributeName");
NS_CONST(NSStrokeColorAttributeName, "NSStrokeColorAttributeName");
NS_CONST(NSUnderlineStyleAttributeName, "NSUnderlineStyleAttributeName");
NS_CONST(NSBaselineOffsetAttributeName, "NSBaselineOffsetAttributeName");
NS_CONST(NSLinkAttributeName, "NSLinkAttributeName");
NS_CONST(NSShadowAttributeName, "NSShadowAttributeName");
NS_CONST(NSPasteboardTypeString, "public.utf8-plain-text");
NS_CONST(NSFilenamesPboardType, "NSFilenamesPboardType");
NS_CONST(NSDragPboard, "Apple drag pboard");
NS_CONST(NSKeyValueChangeKindKey, "NSKeyValueChangeKindKey");
NS_CONST(NSKeyValueChangeNewKey, "NSKeyValueChangeNewKey");
NS_CONST(NSKeyValueChangeOldKey, "NSKeyValueChangeOldKey");
NS_CONST(NSKeyValueChangeIndexesKey, "NSKeyValueChangeIndexesKey");
NS_CONST(NSKeyValueChangeNotificationIsPriorKey, "NSKeyValueChangeNotificationIsPriorKey");
NS_CONST(NSCocoaErrorDomain, "NSCocoaErrorDomain");
NS_CONST(NSPOSIXErrorDomain, "NSPOSIXErrorDomain");
NS_CONST(NSOSStatusErrorDomain, "NSOSStatusErrorDomain");
NS_CONST(NSMachErrorDomain, "NSMachErrorDomain");
NS_CONST(NSApplicationDidFinishLaunchingNotification, "NSApplicationDidFinishLaunchingNotification");
NS_CONST(NSApplicationWillTerminateNotification, "NSApplicationWillTerminateNotification");
NS_CONST(NSApplicationDidBecomeActiveNotification, "NSApplicationDidBecomeActiveNotification");
NS_CONST(NSApplicationWillResignActiveNotification, "NSApplicationWillResignActiveNotification");
NS_CONST(NSWindowDidBecomeKeyNotification, "NSWindowDidBecomeKeyNotification");
NS_CONST(NSWindowDidResignKeyNotification, "NSWindowDidResignKeyNotification");
NS_CONST(NSWindowWillCloseNotification, "NSWindowWillCloseNotification");
NS_CONST(NSWindowDidResizeNotification, "NSWindowDidResizeNotification");
NS_CONST(NSTextDidChangeNotification, "NSTextDidChangeNotification");
NS_CONST(NSTextViewDidChangeSelectionNotification, "NSTextViewDidChangeSelectionNotification");
NS_CONST(NSUserDefaultsDidChangeNotification, "NSUserDefaultsDidChangeNotification");

/* ---- Foundation geometry and error/cookie constants ---- */

typedef struct { double x, y; } plsr_nspoint;
typedef struct { double width, height; } plsr_nssize;
typedef struct { plsr_nspoint origin; plsr_nssize size; } plsr_nsrect;
typedef struct { double top, left, bottom, right; } plsr_nsedgeinsets;
typedef struct { unsigned long location, length; } plsr_nsrange;

plsr_nspoint plsr_NSZeroPoint __asm__("_NSZeroPoint") = { 0.0, 0.0 };
plsr_nssize plsr_NSZeroSize __asm__("_NSZeroSize") = { 0.0, 0.0 };
plsr_nsrect plsr_NSZeroRect __asm__("_NSZeroRect") = { { 0.0, 0.0 }, { 0.0, 0.0 } };
plsr_nsedgeinsets plsr_NSEdgeInsetsZero __asm__("_NSEdgeInsetsZero") =
    { 0.0, 0.0, 0.0, 0.0 };
const double plsr_NSFoundationVersionNumber __asm__("_NSFoundationVersionNumber") =
    2000.0;
const int64_t plsr_NSURLSessionTransferSizeUnknown
    __asm__("_NSURLSessionTransferSizeUnknown") = -1;

NS_CONST(NSAppleScriptErrorAppName, "NSAppleScriptErrorAppName");
NS_CONST(NSAppleScriptErrorBriefMessage, "NSAppleScriptErrorBriefMessage");
NS_CONST(NSAppleScriptErrorMessage, "NSAppleScriptErrorMessage");
NS_CONST(NSAppleScriptErrorNumber, "NSAppleScriptErrorNumber");
NS_CONST(NSAppleScriptErrorRange, "NSAppleScriptErrorRange");
NS_CONST(NSFileOwnerAccountID, "NSFileOwnerAccountID");
NS_CONST(NSFileGroupOwnerAccountID, "NSFileGroupOwnerAccountID");
NS_CONST(NSFileTypeDirectory, "NSFileTypeDirectory");
NS_CONST(NSHTTPCookieDiscard, "Discard");
NS_CONST(NSHTTPCookieDomain, "Domain");
NS_CONST(NSHTTPCookieExpires, "Expires");
NS_CONST(NSHTTPCookieName, "Name");
NS_CONST(NSHTTPCookiePath, "Path");
NS_CONST(NSHTTPCookieSameSiteLax, "SameSiteLax");
NS_CONST(NSHTTPCookieSameSitePolicy, "SameSitePolicy");
NS_CONST(NSHTTPCookieSameSiteStrict, "SameSiteStrict");
NS_CONST(NSHTTPCookieSecure, "Secure");
NS_CONST(NSHTTPCookieValue, "Value");
NS_CONST(NSIsNilTransformerName, "NSIsNilTransformerName");
NS_CONST(NSLocalizedFailureErrorKey, "NSLocalizedFailureErrorKey");
NS_CONST(NSLocalizedFailureReasonErrorKey, "NSLocalizedFailureReasonErrorKey");
NS_CONST(NSLocalizedRecoveryOptionsErrorKey, "NSLocalizedRecoveryOptionsErrorKey");
NS_CONST(NSLocalizedRecoverySuggestionErrorKey, "NSLocalizedRecoverySuggestionErrorKey");
NS_CONST(NSMultipleUnderlyingErrorsKey, "NSMultipleUnderlyingErrorsKey");
NS_CONST(NSPresentationIntentAttributeName, "NSPresentationIntentAttributeName");
NS_CONST(NSProgressKindFile, "NSProgressKindFile");
NS_CONST(NSRecoveryAttempterErrorKey, "NSRecoveryAttempterErrorKey");
NS_CONST(NSRegistrationDomain, "NSRegistrationDomain");
NS_CONST(NSStringEncodingErrorKey, "NSStringEncodingErrorKey");
NS_CONST(NSUndoManagerDidCloseUndoGroupNotification, "NSUndoManagerDidCloseUndoGroupNotification");
NS_CONST(NSUndoManagerDidRedoChangeNotification, "NSUndoManagerDidRedoChangeNotification");
NS_CONST(NSUndoManagerDidUndoChangeNotification, "NSUndoManagerDidUndoChangeNotification");
NS_CONST(NSURLAuthenticationMethodClientCertificate, "NSURLAuthenticationMethodClientCertificate");
NS_CONST(NSURLAuthenticationMethodDefault, "NSURLAuthenticationMethodDefault");
NS_CONST(NSURLAuthenticationMethodHTMLForm, "NSURLAuthenticationMethodHTMLForm");
NS_CONST(NSURLAuthenticationMethodHTTPBasic, "NSURLAuthenticationMethodHTTPBasic");
NS_CONST(NSURLAuthenticationMethodHTTPDigest, "NSURLAuthenticationMethodHTTPDigest");
NS_CONST(NSURLAuthenticationMethodNegotiate, "NSURLAuthenticationMethodNegotiate");
NS_CONST(NSURLAuthenticationMethodNTLM, "NSURLAuthenticationMethodNTLM");
NS_CONST(NSURLAuthenticationMethodServerTrust, "NSURLAuthenticationMethodServerTrust");
NS_CONST(NSURLErrorDomain, "NSURLErrorDomain");
NS_CONST(NSURLErrorBackgroundTaskCancelledReasonKey, "NSURLErrorBackgroundTaskCancelledReasonKey");
NS_CONST(NSURLErrorFailingURLErrorKey, "NSURLErrorFailingURLErrorKey");
NS_CONST(NSURLErrorFailingURLPeerTrustErrorKey, "NSURLErrorFailingURLPeerTrustErrorKey");
NS_CONST(NSURLErrorFailingURLStringErrorKey, "NSURLErrorFailingURLStringErrorKey");
NS_CONST(NSURLErrorNetworkUnavailableReasonKey, "NSURLErrorNetworkUnavailableReasonKey");
NS_CONST(NSURLProtectionSpaceFTPProxy, "NSURLProtectionSpaceFTPProxy");
NS_CONST(NSURLProtectionSpaceHTTPProxy, "NSURLProtectionSpaceHTTPProxy");
NS_CONST(NSURLProtectionSpaceHTTPSProxy, "NSURLProtectionSpaceHTTPSProxy");
NS_CONST(NSURLProtectionSpaceSOCKSProxy, "NSURLProtectionSpaceSOCKSProxy");
NS_CONST(NSURLSessionDownloadTaskResumeData, "NSURLSessionDownloadTaskResumeData");
NS_CONST(NSURLThumbnailDictionaryKey, "NSURLThumbnailDictionaryKey");
NS_CONST(NSURLThumbnailKey, "NSURLThumbnailKey");
NS_CONST(NSURLUbiquitousItemContainerDisplayNameKey, "NSURLUbiquitousItemContainerDisplayNameKey");
NS_CONST(NSURLUbiquitousItemDownloadRequestedKey, "NSURLUbiquitousItemDownloadRequestedKey");
NS_CONST(NSURLUbiquitousItemIsSharedKey, "NSURLUbiquitousItemIsSharedKey");
NS_CONST(NSURLUbiquitousSharedItemCurrentUserPermissionsKey, "NSURLUbiquitousSharedItemCurrentUserPermissionsKey");
NS_CONST(NSURLUbiquitousSharedItemCurrentUserRoleKey, "NSURLUbiquitousSharedItemCurrentUserRoleKey");
NS_CONST(NSURLUbiquitousSharedItemMostRecentEditorNameComponentsKey, "NSURLUbiquitousSharedItemMostRecentEditorNameComponentsKey");
NS_CONST(NSURLUbiquitousSharedItemOwnerNameComponentsKey, "NSURLUbiquitousSharedItemOwnerNameComponentsKey");

typedef struct method_array {
    uint32_t entsize_flags;
    uint32_t count;
    plsr_method methods[32];
} method_array;

/* Class and metaclass symbols bound by shipped Mach-O images. */
plsr_class plsr_NSObject_class __asm__("_OBJC_CLASS_$_NSObject");
plsr_class plsr_NSObject_meta __asm__("_OBJC_METACLASS_$_NSObject");
plsr_class plsr_NSConstantString_class __asm__("_OBJC_CLASS_$_NSConstantString");
plsr_class plsr_NSConstantString_meta __asm__("_OBJC_METACLASS_$_NSConstantString");
plsr_class plsr_NSString_class __asm__("_OBJC_CLASS_$_NSString");
plsr_class plsr_NSString_meta __asm__("_OBJC_METACLASS_$_NSString");
plsr_class plsr_NSMutableString_class __asm__("_OBJC_CLASS_$_NSMutableString");
plsr_class plsr_NSMutableString_meta __asm__("_OBJC_METACLASS_$_NSMutableString");
plsr_class plsr_NSNumber_class __asm__("_OBJC_CLASS_$_NSNumber");
plsr_class plsr_NSNumber_meta __asm__("_OBJC_METACLASS_$_NSNumber");
plsr_class plsr_NSArray_class __asm__("_OBJC_CLASS_$_NSArray");
plsr_class plsr_NSArray_meta __asm__("_OBJC_METACLASS_$_NSArray");
plsr_class plsr_NSMutableArray_class __asm__("_OBJC_CLASS_$_NSMutableArray");
plsr_class plsr_NSMutableArray_meta __asm__("_OBJC_METACLASS_$_NSMutableArray");
plsr_class plsr_NSDictionary_class __asm__("_OBJC_CLASS_$_NSDictionary");
plsr_class plsr_NSDictionary_meta __asm__("_OBJC_METACLASS_$_NSDictionary");
plsr_class plsr_NSNull_class __asm__("_OBJC_CLASS_$_NSNull");
plsr_class plsr_NSNull_meta __asm__("_OBJC_METACLASS_$_NSNull");
plsr_class plsr_NSData_class __asm__("_OBJC_CLASS_$_NSData");
plsr_class plsr_NSData_meta __asm__("_OBJC_METACLASS_$_NSData");

/* ---- generic helpers ---- */

static void *msg(void *obj, const char *sel)
{
    if (!obj)
        return NULL;
    void *(*fn)(void *, const char *) =
        (void *(*)(void *, const char *))plsr_objc_lookup_imp(obj, sel);
    return fn ? fn(obj, sel) : NULL;
}

static const char *obj_utf8(void *obj)
{
    void *s = msg(obj, "UTF8String");
    return s ? (const char *)s : "";
}

static const char *obj_class_name(void *obj)
{
    if (!obj || !*(void **)obj)
        return "nil";
    return PLSR_RO(*(plsr_class **)obj)->name;
}

static plsr_string *str_alloc(void *cls, size_t len)
{
    plsr_string *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->isa = cls;
    s->cap = len + 1;
    s->cstr = calloc(1, s->cap);
    s->len = len;
    return s;
}

static plsr_string *str_from_cstr(void *cls, const char *c)
{
    size_t len = c ? strlen(c) : 0;
    plsr_string *s = str_alloc(cls, len);
    if (!s)
        return NULL;
    if (c)
        memcpy(s->cstr, c, len);
    return s;
}

static plsr_string *str_from_buf(void *cls, const char *buf, size_t len)
{
    plsr_string *s = str_alloc(cls, len);
    if (!s)
        return NULL;
    if (buf && len)
        memcpy(s->cstr, buf, len);
    return s;
}

/* ---- Foundation path helpers ---- */

static void *home_string(void)
{
    const char *home = getenv("HOME");
    return str_from_cstr(&plsr_NSString_class, home && *home ? home : "/");
}

void *plsr_NSHomeDirectory(void) __asm__("_NSHomeDirectory");
void *plsr_NSHomeDirectory(void) { return home_string(); }

void *plsr_NSTemporaryDirectory(void) __asm__("_NSTemporaryDirectory");
void *plsr_NSTemporaryDirectory(void)
{
    const char *tmp = getenv("TMPDIR");
    return str_from_cstr(&plsr_NSString_class, tmp && *tmp ? tmp : "/tmp");
}

void *plsr_NSUserName(void) __asm__("_NSUserName");
void *plsr_NSUserName(void)
{
    const char *user = getenv("USER");
    return str_from_cstr(&plsr_NSString_class, user && *user ? user : "user");
}

void *plsr_NSFullUserName(void) __asm__("_NSFullUserName");
void *plsr_NSFullUserName(void)
{
    const char *user = getenv("USER");
    return str_from_cstr(&plsr_NSString_class, user && *user ? user : "user");
}

void *plsr_NSHomeDirectoryForUser(void *user) __asm__("_NSHomeDirectoryForUser");
void *plsr_NSHomeDirectoryForUser(void *user) { (void)user; return home_string(); }

void *plsr_NSOpenStepRootDirectory(void) __asm__("_NSOpenStepRootDirectory");
void *plsr_NSOpenStepRootDirectory(void)
{
    return str_from_cstr(&plsr_NSString_class, "/");
}

/* ---- class and selector reflection ---- */

void *plsr_NSClassFromString(void *name) __asm__("_NSClassFromString");
void *plsr_NSClassFromString(void *name)
{
    const char *n = name ? obj_utf8(name) : "";
    return objc_getClass(n);
}

void *plsr_NSStringFromClass(void *cls) __asm__("_NSStringFromClass");
void *plsr_NSStringFromClass(void *cls)
{
    return str_from_cstr(&plsr_NSString_class, class_getName(cls));
}

void *plsr_NSStringFromSelector(void *sel) __asm__("_NSStringFromSelector");
void *plsr_NSStringFromSelector(void *sel)
{
    return str_from_cstr(&plsr_NSString_class, sel_getName(sel));
}

void *plsr_NSSelectorFromString(void *name) __asm__("_NSSelectorFromString");
void *plsr_NSSelectorFromString(void *name)
{
    return (void *)(name ? obj_utf8(name) : "");
}

struct buf {
    char *p;
    size_t len, cap;
};

static void bput(struct buf *b, const char *s)
{
    size_t n = strlen(s);
    if (b->len + n + 1 > b->cap) {
        size_t cap = (b->len + n + 1) * 2;
        char *p = realloc(b->p, cap);
        if (!p)
            return;
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n + 1);
    b->len += n;
}

static void bputc(struct buf *b, char c)
{
    char tmp[2] = { c, 0 };
    bput(b, tmp);
}

/* ---- NSObject ---- */

static void *ns_init(void *self, const char *sel) { (void)sel; return self; }
static void ns_dealloc(void *self, const char *sel) { (void)sel; free(self); }
static void *ns_self(void *self, const char *sel) { (void)sel; return self; }
static void *ns_class(void *self, const char *sel)
{
    (void)sel;
    return *(void **)self;
}
static void *ns_retain(void *self, const char *sel) { (void)sel; return self; }
static void ns_release(void *self, const char *sel) { (void)self; (void)sel; }
static void ns_autorelease(void *self, const char *sel) { (void)self; (void)sel; }
static void *ns_copy(void *self, const char *sel) { (void)sel; return self; }
static int ns_is_equal(void *self, const char *sel, void *other)
{
    (void)sel;
    return self == other;
}
static unsigned long ns_hash(void *self, const char *sel)
{
    (void)sel;
    return (unsigned long)(uintptr_t)self;
}
static int ns_is_kind_of_class(void *self, const char *sel, void *cls)
{
    plsr_class *c = *(plsr_class **)self;
    (void)sel;
    for (int depth = 0; c && depth < 64; depth++, c = c->superclass)
        if (c == (plsr_class *)cls)
            return 1;
    return 0;
}
static int ns_responds_to_selector(void *self, const char *sel, const char *name)
{
    (void)sel;
    return plsr_objc_lookup_imp(self, name) != NULL;
}

unsigned long class_getInstanceSize(void *cls)
    __asm__("_class_getInstanceSize");

static void *ns_alloc(void *cls, const char *sel)
{
    size_t size = (size_t)class_getInstanceSize(cls);
    void *obj;
    if (!size)
        size = 8;
    obj = calloc(1, size);
    (void)sel;
    if (obj)
        *(void **)obj = cls;
    return obj;
}
static void *ns_new(void *cls, const char *sel)
{
    void *obj = ns_alloc(cls, sel);
    void *(*init)(void *, const char *) =
        (void *(*)(void *, const char *))plsr_objc_lookup_imp(obj, "init");
    return init ? init(obj, "init") : obj;
}

/* ---- NSConstantString ---- */

static const char *constant_cstring(void *self)
{
    return *(const char **)((char *)self + 8);
}
static const char *cs_utf8(void *self, const char *sel)
{
    (void)sel;
    return constant_cstring(self);
}
static unsigned long cs_length(void *self, const char *sel)
{
    (void)sel;
    return (unsigned long)*(const uint32_t *)((char *)self + 16);
}
static void *cs_description(void *self, const char *sel) { (void)sel; return self; }
static int cs_is_equal(void *self, const char *sel, void *other)
{
    (void)sel;
    if (self == other)
        return 1;
    if (!other)
        return 0;
    return !strcmp(constant_cstring(self), obj_utf8(other));
}
static int cs_is_equal_to_string(void *self, const char *sel, void *other)
{
    (void)sel;
    if (self == other)
        return 1;
    return other && !strcmp(constant_cstring(self), obj_utf8(other));
}
static unsigned long cs_hash(void *self, const char *sel)
{
    const char *s = constant_cstring(self);
    unsigned long h = 5381;
    (void)sel;
    while (*s)
        h = (h * 33) ^ (unsigned char)*s++;
    return h;
}

/* ---- NSString / NSMutableString ---- */

static void *str_init(void *self, const char *sel)
{
    plsr_string *s = self;
    (void)sel;
    if (!s->cstr) {
        s->cstr = calloc(1, 1);
        s->cap = 1;
    }
    return self;
}
static void *str_init_with_utf8(void *self, const char *sel, const char *c)
{
    plsr_string *s = self;
    size_t len = c ? strlen(c) : 0;
    (void)sel;
    free(s->cstr);
    s->cstr = calloc(1, len + 1);
    if (s->cstr && c)
        memcpy(s->cstr, c, len);
    s->len = len;
    s->cap = len + 1;
    return self;
}
static void *str_init_with_bytes_encoding(void *self, const char *sel,
                                          const void *bytes, unsigned long len,
                                          unsigned long encoding)
{
    plsr_string *s = self;
    (void)sel;
    (void)encoding;
    free(s->cstr);
    s->cstr = calloc(1, (size_t)len + 1);
    if (s->cstr && bytes)
        memcpy(s->cstr, bytes, len);
    s->len = len;
    s->cap = (size_t)len + 1;
    return self;
}

static void *str_init_with_data_encoding(void *self, const char *sel,
                                         void *data, unsigned long encoding)
{
    void *bytes = data ? msg(data, "bytes") : NULL;
    unsigned long len = data ? (unsigned long)(uintptr_t)msg(data, "length") : 0;
    return str_init_with_bytes_encoding(self, sel, bytes, len, encoding);
}

static const char *str_utf8(void *self, const char *sel)
{
    plsr_string *s = self;
    (void)sel;
    return s->cstr ? s->cstr : "";
}
static unsigned long str_length(void *self, const char *sel)
{
    plsr_string *s = self;
    (void)sel;
    return s->cstr ? strlen(s->cstr) : 0;
}

static void str_get_characters(void *self, const char *sel,
                               unsigned short *buffer, plsr_nsrange range)
{
    const char *s = str_utf8(self, sel);
    size_t len = strlen(s);
    if (!buffer)
        return;
    /* Exception messages and storyboard metadata are ASCII; copy the byte
     * range so AppKit's exception formatter can inspect it safely. */
    for (unsigned long i = 0; i < range.length && range.location + i < len; i++)
        buffer[i] = (unsigned char)s[range.location + i];
}

static int str_character_is_member(void *self, const char *sel, unsigned short c)
{
    const char *s = str_utf8(self, sel);
    return c < 128 && strchr(s, (char)c) != NULL;
}

static void *str_inverted_set(void *self, const char *sel)
{
    (void)sel;
    return self;
}

static plsr_nsrange str_range_of_character_from_set(void *self, const char *sel,
                                                    void *set)
{
    const char *s = str_utf8(self, sel);
    const char *chars = set ? obj_utf8(set) : "";
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i++)
        if (strchr(chars, (unsigned char)s[i])) {
            plsr_nsrange r = { i, 1 };
            return r;
        }
    {
        plsr_nsrange r = { (unsigned long)-1, 0 };
        return r;
    }
}

static plsr_nsrange str_range_of_character_from_set_options(
    void *self, const char *sel, void *set, unsigned long options)
{
    (void)options;
    return str_range_of_character_from_set(self, sel, set);
}

static plsr_nsrange str_range_of_character_from_set_options_range(
    void *self, const char *sel, void *set, unsigned long options,
    plsr_nsrange range)
{
    const char *s = str_utf8(self, sel);
    const char *chars = set ? obj_utf8(set) : "";
    size_t n = strlen(s);
    (void)options;
    for (unsigned long i = 0; i < range.length && range.location + i < n; i++)
        if (strchr(chars, (unsigned char)s[range.location + i])) {
            plsr_nsrange r = { range.location + i, 1 };
            return r;
        }
    {
        plsr_nsrange r = { (unsigned long)-1, 0 };
        return r;
    }
}
static void *str_description(void *self, const char *sel) { (void)sel; return self; }
static int str_is_equal(void *self, const char *sel, void *other)
{
    (void)sel;
    if (self == other)
        return 1;
    if (!other)
        return 0;
    return !strcmp(str_utf8(self, sel), obj_utf8(other));
}
static int str_is_equal_to_string(void *self, const char *sel, void *other)
{
    (void)sel;
    if (self == other)
        return 1;
    return other && !strcmp(str_utf8(self, sel), obj_utf8(other));
}
static unsigned long str_hash(void *self, const char *sel)
{
    const char *p = str_utf8(self, sel);
    unsigned long h = 5381;
    while (*p)
        h = (h * 33) ^ (unsigned char)*p++;
    return h;
}
static void *str_append(void *self, const char *sel, void *other)
{
    plsr_string *s = self;
    const char *add = obj_utf8(other);
    size_t n = strlen(add);
    size_t need = (s->cstr ? strlen(s->cstr) : 0) + n + 1;
    (void)sel;
    if (need > s->cap) {
        char *p = realloc(s->cstr, need);
        if (!p)
            return self;
        s->cstr = p;
        s->cap = need;
    }
    if (s->cstr)
        memcpy(s->cstr + strlen(s->cstr), add, n + 1);
    return self;
}
static void *str_set_string(void *self, const char *sel, void *other)
{
    return str_init_with_utf8(self, sel, obj_utf8(other));
}
static void *str_string_by_appending(void *self, const char *sel, void *other)
{
    plsr_string *out = str_from_cstr(*(void **)self, str_utf8(self, sel));
    (void)sel;
    if (out)
        str_append(out, "appendString:", other);
    return out;
}

static void *str_by_appending_path_component(void *self, const char *sel,
                                            void *component)
{
    const char *base = str_utf8(self, sel);
    const char *part = obj_utf8(component);
    size_t blen = strlen(base), plen = strlen(part);
    int slash = blen > 0 && base[blen - 1] != '/';
    char *buf = malloc(blen + plen + (slash ? 2 : 1));
    void *out;
    if (!buf)
        return NULL;
    memcpy(buf, base, blen);
    if (slash)
        buf[blen++] = '/';
    memcpy(buf + blen, part, plen + 1);
    out = str_from_cstr(*(void **)self, buf);
    free(buf);
    return out;
}

static const char *str_path_basename(const char *path)
{
    const char *slash = path ? strrchr(path, '/') : NULL;
    return slash ? slash + 1 : (path ? path : "");
}

static void *str_by_appending_path_extension(void *self, const char *sel,
                                             void *extension)
{
    const char *base = obj_utf8(self);
    const char *ext = extension ? obj_utf8(extension) : "";
    size_t n = strlen(base), m = strlen(ext);
    char *buf;
    void *out;
    (void)sel;
    if (!m)
        return str_from_cstr(&plsr_NSString_class, base);
    buf = malloc(n + m + 2);
    if (!buf)
        return NULL;
    memcpy(buf, base, n);
    buf[n] = '.';
    memcpy(buf + n + 1, ext, m + 1);
    out = str_from_cstr(&plsr_NSString_class, buf);
    free(buf);
    return out;
}

static void *str_path_extension(void *self, const char *sel)
{
    const char *base = str_path_basename(obj_utf8(self));
    const char *dot = strrchr(base, '.');
    (void)sel;
    return str_from_cstr(&plsr_NSString_class,
                         dot && dot != base ? dot + 1 : "");
}

static void *str_by_deleting_path_extension(void *self, const char *sel)
{
    const char *path = obj_utf8(self);
    const char *base = str_path_basename(path);
    const char *dot = strrchr(base, '.');
    size_t n;
    char *buf;
    void *out;
    (void)sel;
    if (!dot || dot == base)
        return str_from_cstr(&plsr_NSString_class, path);
    n = (size_t)(dot - path);
    buf = malloc(n + 1);
    if (!buf)
        return NULL;
    memcpy(buf, path, n);
    buf[n] = 0;
    out = str_from_cstr(&plsr_NSString_class, buf);
    free(buf);
    return out;
}

static void *str_last_path_component(void *self, const char *sel)
{
    const char *path = obj_utf8(self);
    const char *base = str_path_basename(path);
    char *buf;
    void *out;
    size_t n;
    (void)sel;
    if (!*base && path[0] == '/')
        return str_from_cstr(&plsr_NSString_class, "/");
    n = strlen(base);
    while (n > 0 && base[n - 1] == '/')
        n--;
    buf = malloc(n + 1);
    if (!buf)
        return NULL;
    memcpy(buf, base, n);
    buf[n] = 0;
    out = str_from_cstr(&plsr_NSString_class, buf);
    free(buf);
    return out;
}

static void *str_by_deleting_last_path_component(void *self, const char *sel)
{
    const char *path = obj_utf8(self);
    char *buf = strdup(path);
    char *slash;
    size_t n;
    void *out;
    (void)sel;
    if (!buf)
        return NULL;
    n = strlen(buf);
    while (n > 1 && buf[n - 1] == '/')
        buf[--n] = 0;
    slash = strrchr(buf, '/');
    if (!slash)
        out = str_from_cstr(&plsr_NSString_class, "");
    else if (slash == buf)
        out = str_from_cstr(&plsr_NSString_class, "/");
    else {
        *slash = 0;
        out = str_from_cstr(&plsr_NSString_class, buf);
    }
    free(buf);
    return out;
}

static int str_encoding_cant_be_stored_eight_bit(void *self, const char *sel)
{
    (void)self;
    (void)sel;
    return 0; /* our strings are UTF-8/C strings, representable as 8-bit */
}

static void *str_copy(void *self, const char *sel)
{
    (void)sel;
    return str_from_cstr(*(void **)self, str_utf8(self, sel));
}
static void *str_mutable_copy(void *self, const char *sel)
{
    plsr_string *out = str_from_cstr(*(void **)self, str_utf8(self, sel));
    (void)sel;
    if (out)
        out->isa = &plsr_NSString_class; /* mutable class wired after decl */
    return out;
}
static void str_dealloc(void *self, const char *sel)
{
    plsr_string *s = self;
    (void)sel;
    free(s->cstr);
    free(s);
}
static void *str_class_string(void *cls, const char *sel)
{
    (void)sel;
    return str_from_cstr(cls, "");
}
static void *str_class_with_utf8(void *cls, const char *sel, const char *c)
{
    (void)sel;
    return str_from_cstr(cls, c);
}
static void *str_class_with_string(void *cls, const char *sel, void *other)
{
    (void)sel;
    return str_from_cstr(cls, obj_utf8(other));
}

static void append_format(struct buf *b, const char *format, va_list ap);

static void *str_class_with_format(void *cls, const char *sel, void *format, ...)
{
    struct buf b = { 0 };
    va_list ap;
    void *out;
    (void)sel;
    va_start(ap, format);
    append_format(&b, format ? obj_utf8(format) : "", ap);
    va_end(ap);
    out = str_from_buf(cls, b.p ? b.p : "", b.len);
    free(b.p);
    return out;
}

static void *str_class_with_contents_file(void *cls, const char *sel,
                                          void *path, unsigned long encoding,
                                          void *error)
{
    const char *p = path ? obj_utf8(path) : NULL;
    FILE *f = p ? fopen(p, "rb") : NULL;
    char *buf;
    long n;
    void *out;
    (void)sel;
    (void)encoding;
    (void)error;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (n && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[n] = 0;
    fclose(f);
    out = str_from_buf(cls, buf, (size_t)n);
    free(buf);
    return out;
}

/* ---- NSNumber ---- */

typedef struct {
    void *isa;
    long long i;
    double d;
    int is_double;
} plsr_number;

static void *num_with_int(void *cls, const char *sel, int v)
{
    plsr_number *n = calloc(1, sizeof(*n));
    (void)sel;
    if (n) {
        n->isa = cls;
        n->i = v;
    }
    return n;
}
static void *num_with_long(void *cls, const char *sel, long long v)
{
    plsr_number *n = calloc(1, sizeof(*n));
    (void)sel;
    if (n) {
        n->isa = cls;
        n->i = v;
    }
    return n;
}
static void *num_with_double(void *cls, const char *sel, double v)
{
    plsr_number *n = calloc(1, sizeof(*n));
    (void)sel;
    if (n) {
        n->isa = cls;
        n->i = (long long)v;
        n->d = v;
        n->is_double = 1;
    }
    return n;
}
static void *num_with_bool(void *cls, const char *sel, signed char v)
{
    return num_with_int(cls, sel, v != 0 ? 1 : 0);
}
static void *num_with_uint(void *cls, const char *sel, unsigned int v)
{
    return num_with_long(cls, sel, (long long)v);
}
static void *num_with_ull(void *cls, const char *sel, unsigned long long v)
{
    return num_with_long(cls, sel, (long long)v);
}
static void *num_with_float(void *cls, const char *sel, float v)
{
    return num_with_double(cls, sel, (double)v);
}
static void *num_with_char(void *cls, const char *sel, signed char v)
{
    return num_with_int(cls, sel, v);
}
static void *num_with_short(void *cls, const char *sel, short v)
{
    return num_with_int(cls, sel, v);
}
static void *num_with_uchar(void *cls, const char *sel, unsigned char v)
{
    return num_with_int(cls, sel, v);
}
static void *num_with_ushort(void *cls, const char *sel, unsigned short v)
{
    return num_with_int(cls, sel, v);
}
static void *num_with_ulong(void *cls, const char *sel, unsigned long v)
{
    return num_with_ull(cls, sel, (unsigned long long)v);
}
static int num_int_value(void *self, const char *sel)
{
    plsr_number *n = self;
    (void)sel;
    return n->is_double ? (int)n->d : (int)n->i;
}
static long long num_integer_value(void *self, const char *sel)
{
    plsr_number *n = self;
    (void)sel;
    return n->is_double ? (long long)n->d : n->i;
}
static long long num_long_long_value(void *self, const char *sel)
{
    return num_integer_value(self, sel);
}
static double num_double_value(void *self, const char *sel);

static unsigned int num_unsigned_int_value(void *self, const char *sel)
{
    return (unsigned int)num_integer_value(self, sel);
}
static unsigned long long num_unsigned_long_long_value(void *self, const char *sel)
{
    return (unsigned long long)num_integer_value(self, sel);
}
static float num_float_value(void *self, const char *sel)
{
    return (float)num_double_value(self, sel);
}
static signed char num_char_value(void *self, const char *sel)
{
    return (signed char)num_integer_value(self, sel);
}
static short num_short_value(void *self, const char *sel)
{
    return (short)num_integer_value(self, sel);
}
static unsigned char num_unsigned_char_value(void *self, const char *sel)
{
    return (unsigned char)num_integer_value(self, sel);
}
static unsigned short num_unsigned_short_value(void *self, const char *sel)
{
    return (unsigned short)num_integer_value(self, sel);
}
static double num_double_value(void *self, const char *sel)
{
    plsr_number *n = self;
    (void)sel;
    return n->is_double ? n->d : (double)n->i;
}
static signed char num_bool_value(void *self, const char *sel)
{
    return num_int_value(self, sel) != 0;
}
static void *num_string_value(void *self, const char *sel)
{
    plsr_number *n = self;
    char tmp[64];
    (void)sel;
    if (n->is_double)
        snprintf(tmp, sizeof tmp, "%g", n->d);
    else
        snprintf(tmp, sizeof tmp, "%lld", n->i);
    return str_from_cstr(&plsr_NSString_class, tmp);
}
static void *num_description(void *self, const char *sel)
{
    return num_string_value(self, sel);
}
static int num_is_equal(void *self, const char *sel, void *other)
{
    (void)sel;
    return self == other;
}
static unsigned long num_hash(void *self, const char *sel)
{
    plsr_number *n = self;
    (void)sel;
    return (unsigned long)n->i;
}
static void num_dealloc(void *self, const char *sel) { (void)sel; free(self); }

/* ---- NSArray / NSMutableArray ---- */

typedef struct {
    void *isa;
    unsigned long count;
    void **items;
} plsr_array;

static plsr_array *array_alloc(void *cls, unsigned long n)
{
    plsr_array *a = calloc(1, sizeof(*a));
    if (!a)
        return NULL;
    a->isa = cls;
    a->count = n;
    if (n)
        a->items = calloc(n, sizeof(void *));
    return a;
}

static void *array_class_array(void *cls, const char *sel)
{
    (void)sel;
    return array_alloc(cls, 0);
}
static void *array_class_with_object(void *cls, const char *sel, void *obj)
{
    plsr_array *a = array_alloc(cls, 1);
    (void)sel;
    if (a && obj)
        a->items[0] = obj;
    return a;
}
static void *array_class_with_objects_count(void *cls, const char *sel,
                                            void **items, unsigned long n)
{
    plsr_array *a = array_alloc(cls, n);
    (void)sel;
    if (a && n)
        memcpy(a->items, items, n * sizeof(void *));
    return a;
}
static void *array_class_with_objects(void *cls, const char *sel, void *first, ...)
{
    va_list ap;
    unsigned long n = 0;
    void **items;
    plsr_array *a;
    void *v = first;
    (void)sel;
    va_start(ap, first);
    while (v) {
        n++;
        v = va_arg(ap, void *);
    }
    va_end(ap);
    items = n ? calloc(n, sizeof(void *)) : NULL;
    if (items) {
        unsigned long i = 0;
        va_start(ap, first);
        v = first;
        while (v && i < n) {
            items[i++] = v;
            v = va_arg(ap, void *);
        }
        va_end(ap);
    }
    a = array_alloc(cls, n);
    if (a && n) {
        memcpy(a->items, items, n * sizeof(void *));
        free(items);
    }
    return a;
}
static unsigned long array_count(void *self, const char *sel)
{
    (void)sel;
    return ((plsr_array *)self)->count;
}
static void *array_object_at(void *self, const char *sel, unsigned long idx)
{
    plsr_array *a = self;
    (void)sel;
    return idx < a->count ? a->items[idx] : NULL;
}
static void *array_first(void *self, const char *sel)
{
    plsr_array *a = self;
    (void)sel;
    return a->count ? a->items[0] : NULL;
}
static void *array_last(void *self, const char *sel)
{
    plsr_array *a = self;
    (void)sel;
    return a->count ? a->items[a->count - 1] : NULL;
}
static int array_contains(void *self, const char *sel, void *obj)
{
    plsr_array *a = self;
    (void)sel;
    for (unsigned long i = 0; i < a->count; i++)
        if (a->items[i] == obj)
            return 1;
    return 0;
}

typedef struct {
    unsigned long state;
    void **itemsPtr;
    unsigned long *mutationsPtr;
    unsigned long extra[5];
} plsr_fast_enumeration_state;

static unsigned long array_count_by_enumerating(
    void *self, const char *sel, plsr_fast_enumeration_state *state,
    void **buffer, unsigned long count)
{
    plsr_array *a = self;
    unsigned long offset, n;
    (void)sel;
    (void)buffer;
    if (!state)
        return 0;
    if (!state->state) {
        state->state = 1;
        state->extra[0] = 0;
    }
    offset = state->extra[0];
    if (offset >= a->count || !count)
        return 0;
    state->itemsPtr = a->items + offset;
    state->mutationsPtr = &a->count;
    n = a->count - offset;
    if (n > count)
        n = count;
    state->extra[0] = offset + n;
    return n;
}
static void *array_description(void *self, const char *sel)
{
    plsr_array *a = self;
    struct buf b = { 0 };
    (void)sel;
    bput(&b, "(");
    for (unsigned long i = 0; i < a->count; i++) {
        if (i)
            bput(&b, ", ");
        bput(&b, obj_utf8(msg(a->items[i], "description")));
    }
    bput(&b, ")");
    {
        plsr_string *s = str_from_buf(&plsr_NSString_class, b.p ? b.p : "()",
                                      b.len);
        free(b.p);
        return s;
    }
}
static void array_dealloc(void *self, const char *sel)
{
    /* The runtime currently treats retain/release as no-ops, while AppKit
     * may still send dealloc to a collection it no longer owns.  Keep the
     * backing store alive so a later addObject: cannot realloc freed memory. */
    (void)self;
    (void)sel;
}
static void array_add_objects_from_array(void *self, const char *sel, void *other)
{
    plsr_array *a = self;
    plsr_array *b = other;
    void **items;
    unsigned long old_count;
    (void)sel;
    if (!b || !b->count)
        return;
    old_count = a->count;
    items = malloc((old_count + b->count) * sizeof(void *));
    if (!items)
        return;
    if (old_count)
        memcpy(items, a->items, old_count * sizeof(void *));
    memcpy(items + old_count, b->items, b->count * sizeof(void *));
    free(a->items);
    a->items = items;
    a->count = old_count + b->count;
}

static void array_add_object(void *self, const char *sel, void *obj)
{
    plsr_array *a = self;
    void **p;
    (void)sel;
    if (!obj)
        return;
    p = realloc(a->items, (a->count + 1) * sizeof(void *));
    if (!p)
        return;
    a->items = p;
    a->items[a->count++] = obj;
}
static void array_insert_object_at(void *self, const char *sel, void *obj,
                                   unsigned long idx)
{
    plsr_array *a = self;
    (void)sel;
    if (!obj || idx > a->count)
        return;
    void **p = realloc(a->items, (a->count + 1) * sizeof(void *));
    if (!p)
        return;
    a->items = p;
    memmove(&a->items[idx + 1], &a->items[idx],
            (a->count - idx) * sizeof(void *));
    a->items[idx] = obj;
    a->count++;
}
static void array_remove_at(void *self, const char *sel, unsigned long idx)
{
    plsr_array *a = self;
    (void)sel;
    if (idx >= a->count)
        return;
    memmove(&a->items[idx], &a->items[idx + 1],
            (a->count - idx - 1) * sizeof(void *));
    a->count--;
}
static void array_remove_all(void *self, const char *sel)
{
    plsr_array *a = self;
    (void)sel;
    a->count = 0;
}
static void array_set_object_at_index(void *self, const char *sel, void *obj,
                                      unsigned long idx)
{
    plsr_array *a = self;
    (void)sel;
    if (obj && idx < a->count)
        a->items[idx] = obj;
}

static void *array_copy_to_class(void *self, void *cls)
{
    plsr_array *a = self;
    plsr_array *out = array_alloc(cls, a->count);
    if (out && a->count)
        memcpy(out->items, a->items, a->count * sizeof(void *));
    return out;
}

static void *array_copy(void *self, const char *sel)
{
    (void)sel;
    return array_copy_to_class(self, (void *)&plsr_NSArray_class);
}

static void *array_mutable_copy(void *self, const char *sel)
{
    (void)sel;
    return array_copy_to_class(self, (void *)&plsr_NSMutableArray_class);
}

static void *array_copy_with_zone(void *self, const char *sel, void *zone)
{
    (void)sel;
    (void)zone;
    return array_copy_to_class(self, (void *)&plsr_NSArray_class);
}

/* ---- NSDictionary ---- */

typedef struct {
    void *isa;
    unsigned long count;
    void **keys;
    void **values;
} plsr_dict;

static plsr_dict *dict_alloc(void *cls, unsigned long n)
{
    plsr_dict *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->isa = cls;
    d->count = n;
    if (n) {
        d->keys = calloc(n, sizeof(void *));
        d->values = calloc(n, sizeof(void *));
    }
    return d;
}

static void *dict_class_dictionary(void *cls, const char *sel)
{
    (void)sel;
    return dict_alloc(cls, 0);
}
static void *dict_class_with_objects(void *cls, const char *sel, void **objects,
                                     void **keys, unsigned long n)
{
    plsr_dict *d = dict_alloc(cls, n);
    (void)sel;
    if (d && n) {
        memcpy(d->keys, keys, n * sizeof(void *));
        memcpy(d->values, objects, n * sizeof(void *));
    }
    return d;
}
static void *dict_class_with_object(void *cls, const char *sel, void *obj,
                                    void *key)
{
    plsr_dict *d = dict_alloc(cls, 1);
    (void)sel;
    if (d && obj) {
        d->keys[0] = key;
        d->values[0] = obj;
    }
    return d;
}

static void *dict_copy_to_class(void *self, void *cls)
{
    plsr_dict *d = self;
    plsr_dict *out = dict_alloc(cls, d->count);
    if (out && d->count) {
        memcpy(out->keys, d->keys, d->count * sizeof(void *));
        memcpy(out->values, d->values, d->count * sizeof(void *));
    }
    return out;
}

static void *dict_copy(void *self, const char *sel)
{
    (void)sel;
    return dict_copy_to_class(self, (void *)&plsr_NSDictionary_class);
}

static void *dict_mutable_copy(void *self, const char *sel)
{
    void *cls = objc_getClass("NSMutableDictionary");
    (void)sel;
    return dict_copy_to_class(self, cls ? cls : (void *)&plsr_NSDictionary_class);
}

static void *dict_copy_with_zone(void *self, const char *sel, void *zone)
{
    (void)sel;
    (void)zone;
    return dict_copy_to_class(self, (void *)&plsr_NSDictionary_class);
}

/* ---- binary property lists (needed by NSStoryboard's Info.plist) ---- */

typedef struct {
    const unsigned char *buf;
    size_t len;
    unsigned off_size, ref_size;
    uint64_t nobjects, top, table;
    void **memo;
} bplist_ctx;

static uint64_t bplist_be(const unsigned char *p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; i++)
        v = (v << 8) | p[i];
    return v;
}

static int bplist_count(const unsigned char *p, const unsigned char *end,
                        uint64_t *out, const unsigned char **next)
{
    unsigned low;
    if (p >= end)
        return 0;
    low = *p & 0x0f;
    if (low != 0x0f) {
        *out = low;
        *next = p + 1;
        return 1;
    }
    p++;
    if (p >= end || (*p >> 4) != 1)
        return 0;
    {
        unsigned nbytes = 1u << (*p & 0x0f);
        p++;
        if (p + nbytes > end)
            return 0;
        *out = bplist_be(p, nbytes);
        *next = p + nbytes;
    }
    return 1;
}

static double bplist_real(const unsigned char *p, unsigned n)
{
    unsigned char tmp[8];
    double d;
    float f;
    if (n == 8) {
        for (unsigned i = 0; i < 8; i++)
            tmp[i] = p[7 - i];
        memcpy(&d, tmp, sizeof d);
        return d;
    }
    if (n == 4) {
        for (unsigned i = 0; i < 4; i++)
            tmp[i] = p[3 - i];
        memcpy(&f, tmp, sizeof f);
        return (double)f;
    }
    return 0.0;
}

static void *bplist_utf16(const unsigned char *p, uint64_t n)
{
    char *out = malloc((size_t)n * 3 + 1);
    size_t o = 0;
    if (!out)
        return NULL;
    for (uint64_t i = 0; i < n; i++) {
        uint32_t c = ((uint32_t)p[i * 2] << 8) | p[i * 2 + 1];
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < n) {
            uint32_t lo = ((uint32_t)p[(i + 1) * 2] << 8) | p[(i + 1) * 2 + 1];
            if (lo >= 0xdc00 && lo <= 0xdfff) {
                c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
                i++;
            }
        }
        if (c < 0x80) {
            out[o++] = (char)c;
        } else if (c < 0x800) {
            out[o++] = (char)(0xc0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3f));
        } else if (c < 0x10000) {
            out[o++] = (char)(0xe0 | (c >> 12));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3f));
            out[o++] = (char)(0x80 | (c & 0x3f));
        } else {
            out[o++] = (char)(0xf0 | (c >> 18));
            out[o++] = (char)(0x80 | ((c >> 12) & 0x3f));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3f));
            out[o++] = (char)(0x80 | (c & 0x3f));
        }
    }
    {
        void *s = str_from_buf(&plsr_NSString_class, out, o);
        free(out);
        return s;
    }
}

static void *bplist_object(bplist_ctx *c, uint64_t idx, int depth);

static void *bplist_object(bplist_ctx *c, uint64_t idx, int depth)
{
    const unsigned char *end = c->buf + c->len;
    const unsigned char *p;
    uint64_t off, count;
    unsigned type, low;
    void *out = NULL;
    if (idx >= c->nobjects || depth > 64) {
        if (getenv("PLSR_OBJC_TRACE"))
            dprintf(2, "bplist: bad index %llu depth %d\n",
                    (unsigned long long)idx, depth);
        return NULL;
    }
    if (c->memo[idx])
        return c->memo[idx];
    off = bplist_be(c->buf + c->table + idx * c->off_size, c->off_size);
    if (off >= c->len) {
        if (getenv("PLSR_OBJC_TRACE"))
            dprintf(2, "bplist: obj %llu off %llu >= len %zu\n",
                    (unsigned long long)idx, (unsigned long long)off, c->len);
        return NULL;
    }
    p = c->buf + off;
    type = *p >> 4;
    low = *p & 0x0f;

    if (type == 0x0) {
        if (low == 0x9)
            out = num_with_bool(&plsr_NSNumber_class, "numberWithBool:", 1);
        else if (low == 0x8)
            out = num_with_bool(&plsr_NSNumber_class, "numberWithBool:", 0);
    } else if (type == 0x1) {
        unsigned nbytes = 1u << low;
        if (p + 1 + nbytes <= end)
            out = num_with_int(&plsr_NSNumber_class, "numberWithInt:",
                               (int)bplist_be(p + 1, nbytes));
    } else if (type == 0x2) {
        unsigned nbytes = 1u << low;
        if (p + 1 + nbytes <= end)
            out = num_with_double(&plsr_NSNumber_class, "numberWithDouble:",
                                  bplist_real(p + 1, nbytes));
    } else if (type == 0x5 || type == 0x6 || type == 0x7) {
        const unsigned char *q;
        if (bplist_count(p, end, &count, &q)) {
            if (type == 0x6) {
                if (q + count * 2 <= end)
                    out = bplist_utf16(q, count);
            } else if (q + count <= end) {
                out = str_from_buf(&plsr_NSString_class, (const char *)q, count);
            }
        }
    } else if (type == 0xa) {
        const unsigned char *q;
        if (bplist_count(p, end, &count, &q) && q + count * c->ref_size <= end) {
            void **items = count ? calloc(count, sizeof(void *)) : NULL;
            if (items || !count) {
                for (uint64_t i = 0; i < count; i++)
                    items[i] = bplist_object(c, bplist_be(q + i * c->ref_size,
                                                           c->ref_size),
                                             depth + 1);
                out = array_class_with_objects_count(&plsr_NSArray_class,
                                                     "arrayWithObjects:count:",
                                                     items, count);
                free(items);
            }
        }
    } else if (type == 0xd) {
        const unsigned char *q;
        if (bplist_count(p, end, &count, &q) &&
            q + count * 2 * c->ref_size <= end) {
            const unsigned char *vals = q + count * c->ref_size;
            plsr_dict *d = dict_alloc(&plsr_NSDictionary_class, count);
            if (d) {
                for (uint64_t i = 0; i < count; i++) {
                    d->keys[i] = bplist_object(c, bplist_be(q + i * c->ref_size,
                                                             c->ref_size),
                                               depth + 1);
                    d->values[i] = bplist_object(c, bplist_be(vals + i * c->ref_size,
                                                               c->ref_size),
                                                 depth + 1);
                }
                out = d;
            }
        }
    }
    if (getenv("PLSR_OBJC_TRACE"))
        dprintf(2, "bplist: obj %llu type %u low %u -> %p\n",
                (unsigned long long)idx, type, low, out);
    c->memo[idx] = out;
    return out;
}

static unsigned char *bplist_read_file(const char *path, size_t *len)
{
    FILE *f = path ? fopen(path, "rb") : NULL;
    unsigned char *buf;
    long n;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
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

static void *bplist_parse_file(const char *path)
{
    size_t len = 0;
    unsigned char *buf = bplist_read_file(path, &len);
    const unsigned char *trailer;
    bplist_ctx c;
    void *out;
    if (!buf || len < 40 || memcmp(buf, "bplist00", 8) != 0) {
        free(buf);
        return NULL;
    }
    trailer = buf + len - 32;
    memset(&c, 0, sizeof c);
    c.buf = buf;
    c.len = len;
    c.off_size = trailer[6];
    c.ref_size = trailer[7];
    c.nobjects = bplist_be(trailer + 8, 8);
    c.top = bplist_be(trailer + 16, 8);
    c.table = bplist_be(trailer + 24, 8);
    if (!c.off_size || !c.ref_size || c.nobjects > (1u << 20) ||
        c.top >= c.nobjects || c.table >= len ||
        c.table + c.nobjects * c.off_size > len) {
        free(buf);
        return NULL;
    }
    if (getenv("PLSR_OBJC_TRACE"))
        dprintf(2, "bplist: len=%zu off=%u ref=%u nobj=%llu top=%llu table=%llu\n",
                len, c.off_size, c.ref_size,
                (unsigned long long)c.nobjects,
                (unsigned long long)c.top,
                (unsigned long long)c.table);
    c.memo = calloc((size_t)c.nobjects, sizeof(void *));
    if (!c.memo) {
        free(buf);
        return NULL;
    }
    out = bplist_object(&c, c.top, 0);
    if (getenv("PLSR_OBJC_TRACE"))
        dprintf(2, "bplist: top %llu -> %p\n",
                (unsigned long long)c.top, out);
    free(c.memo);
    free(buf);
    return out;
}

static void *dict_class_with_contents_of_file(void *cls, const char *sel,
                                              void *path)
{
    void *out = path ? bplist_parse_file(obj_utf8(path)) : NULL;
    (void)cls;
    (void)sel;
    if (getenv("PLSR_OBJC_TRACE")) {
        dprintf(2, "plist: %s -> %p", path ? obj_utf8(path) : "(nil)", out);
        if (out && ((plsr_dict *)out)->isa == &plsr_NSDictionary_class) {
            plsr_dict *d = out;
            dprintf(2, " count=%lu", d->count);
            for (unsigned long i = 0; i < d->count; i++)
                dprintf(2, " [%s]", obj_utf8(d->keys[i]));
        }
        dprintf(2, "\n");
    }
    return out;
}

static void *dict_class_with_contents_of_url(void *cls, const char *sel,
                                             void *url)
{
    (void)cls;
    (void)sel;
    return url ? dict_class_with_contents_of_file(cls, sel,
                                                  msg(url, "path")) : NULL;
}

static unsigned long dict_count(void *self, const char *sel)
{
    (void)sel;
    return ((plsr_dict *)self)->count;
}
static void *dict_object_for_key(void *self, const char *sel, void *key)
{
    plsr_dict *d = self;
    const char *k;
    (void)sel;
    if (!key)
        return NULL;
    if (getenv("PLSR_OBJC_TRACE")) {
        uintptr_t *q = key;
        dprintf(2, "objc: dict key words %p %p %p %p\n",
                (void *)q[0], (void *)q[1], (void *)q[2], (void *)q[3]);
    }
    k = obj_utf8(key);
    if (getenv("PLSR_OBJC_TRACE"))
        dprintf(2, "objc: dict lookup key=%p '%s' count=%lu self=%p\n",
                key, k, d->count, self);
    for (unsigned long i = 0; i < d->count; i++) {
        if (getenv("PLSR_OBJC_TRACE"))
            dprintf(2, "objc:   key[%lu]=%p value=%p\n", i, d->keys[i], d->values[i]);
        if (!strcmp(obj_utf8(d->keys[i]), k))
            return d->values[i];
    }
    return NULL;
}
static void *dict_all_keys(void *self, const char *sel)
{
    plsr_dict *d = self;
    (void)sel;
    return array_class_with_objects_count(&plsr_NSArray_class, sel, d->keys,
                                          d->count);
}
static void *dict_all_values(void *self, const char *sel)
{
    plsr_dict *d = self;
    (void)sel;
    return array_class_with_objects_count(&plsr_NSArray_class, sel, d->values,
                                          d->count);
}
static void *dict_description(void *self, const char *sel)
{
    plsr_dict *d = self;
    struct buf b = { 0 };
    (void)sel;
    bput(&b, "{");
    for (unsigned long i = 0; i < d->count; i++) {
        if (i)
            bput(&b, ", ");
        bput(&b, obj_utf8(msg(d->keys[i], "description")));
        bput(&b, " = ");
        bput(&b, obj_utf8(msg(d->values[i], "description")));
        bput(&b, ";");
    }
    bput(&b, "}");
    {
        plsr_string *s = str_from_buf(&plsr_NSString_class, b.p ? b.p : "{}",
                                      b.len);
        free(b.p);
        return s;
    }
}
static void dict_dealloc(void *self, const char *sel)
{
    plsr_dict *d = self;
    (void)sel;
    free(d->keys);
    free(d->values);
    free(d);
}

/* Used by the shipped NSMutableDictionary class through
 * plsr_foundation_override; the class itself is still Foundation's. */
void *plsr_cleanroom_dict_alloc(void *cls, unsigned long n)
{
    return dict_alloc(cls, n);
}

int plsr_cleanroom_dict_set(void *self, void *key, void *obj)
{
    plsr_dict *d = self;
    const char *k;
    if (!d || !key)
        return 0;
    k = obj_utf8(key);
    for (unsigned long i = 0; i < d->count; i++) {
        if (!strcmp(obj_utf8(d->keys[i]), k)) {
            d->values[i] = obj;
            return 1;
        }
    }
    {
        void **keys = realloc(d->keys, (d->count + 1) * sizeof(void *));
        void **values;
        if (!keys)
            return 0;
        d->keys = keys;
        values = realloc(d->values, (d->count + 1) * sizeof(void *));
        if (!values)
            return 0;
        d->values = values;
        d->keys[d->count] = key;
        d->values[d->count] = obj;
        d->count++;
    }
    return 1;
}

unsigned long plsr_cleanroom_dict_count(void *self)
{
    plsr_dict *d = self;
    return d ? d->count : 0;
}

void *plsr_cleanroom_dict_key_at(void *self, unsigned long index)
{
    plsr_dict *d = self;
    return d && index < d->count ? d->keys[index] : NULL;
}

void *plsr_cleanroom_dict_get(void *self, void *key)
{
    plsr_dict *d = self;
    const char *k;
    if (!d || !key)
        return NULL;
    k = obj_utf8(key);
    for (unsigned long i = 0; i < d->count; i++)
        if (!strcmp(obj_utf8(d->keys[i]), k))
            return d->values[i];
    return NULL;
}

int plsr_cleanroom_dict_remove(void *self, void *key)
{
    plsr_dict *d = self;
    const char *k;
    if (!d || !key)
        return 0;
    k = obj_utf8(key);
    for (unsigned long i = 0; i < d->count; i++) {
        if (!strcmp(obj_utf8(d->keys[i]), k)) {
            memmove(d->keys + i, d->keys + i + 1,
                    (d->count - i - 1) * sizeof(void *));
            memmove(d->values + i, d->values + i + 1,
                    (d->count - i - 1) * sizeof(void *));
            d->count--;
            return 1;
        }
    }
    return 0;
}

/* ---- NSNull ---- */

static void *null_singleton;

static void *null_class_null(void *cls, const char *sel)
{
    (void)sel;
    if (!null_singleton) {
        null_singleton = calloc(1, 8);
        if (null_singleton)
            *(void **)null_singleton = cls;
    }
    return null_singleton;
}
static void *null_description(void *self, const char *sel)
{
    (void)self; (void)sel;
    return str_from_cstr(&plsr_NSString_class, "<null>");
}
static int null_is_equal(void *self, const char *sel, void *other)
{
    (void)sel;
    return self == other;
}

/* ---- NSData ---- */

typedef struct {
    void *isa;
    void *bytes;
    unsigned long length;
} plsr_data;

static void *data_class_data(void *cls, const char *sel)
{
    plsr_data *d = calloc(1, sizeof(*d));
    (void)sel;
    if (d)
        d->isa = cls;
    return d;
}
static void *data_class_with_bytes(void *cls, const char *sel, const void *bytes,
                                   unsigned long n)
{
    plsr_data *d = data_class_data(cls, sel);
    if (!d)
        return NULL;
    d->bytes = malloc(n ? n : 1);
    if (d->bytes && bytes)
        memcpy(d->bytes, bytes, n);
    d->length = n;
    return d;
}
static void *data_class_with_contents_of_file(void *cls, const char *sel,
                                               void *path)
{
    const char *p = path ? obj_utf8(path) : NULL;
    FILE *f = p ? fopen(p, "rb") : NULL;
    long n;
    plsr_data *d;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    d = data_class_data(cls, sel);
    if (!d) {
        fclose(f);
        return NULL;
    }
    d->bytes = malloc((size_t)n ? (size_t)n : 1);
    if (!d->bytes) {
        fclose(f);
        free(d);
        return NULL;
    }
    d->length = (unsigned long)n;
    if (n && fread(d->bytes, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(d->bytes);
        free(d);
        return NULL;
    }
    fclose(f);
    return d;
}

static void *data_class_with_contents_of_url(void *cls, const char *sel,
                                              void *url)
{
    void *path = url ? msg(url, "path") : NULL;
    return data_class_with_contents_of_file(cls, sel, path);
}

static void *data_class_with_contents_file_options(void *cls, const char *sel,
                                                    void *path, unsigned long options,
                                                    void *error)
{
    (void)options;
    (void)error;
    return data_class_with_contents_of_file(cls, sel, path);
}

static void *data_class_with_contents_url_options(void *cls, const char *sel,
                                                   void *url, unsigned long options,
                                                   void *error)
{
    (void)options;
    (void)error;
    return data_class_with_contents_of_url(cls, sel, url);
}

static void *data_class_with_bytes_no_copy(void *cls, const char *sel,
                                            const void *bytes, unsigned long n,
                                            int free_when_done)
{
    (void)free_when_done;
    return data_class_with_bytes(cls, sel, bytes, n);
}

static void *data_class_with_capacity(void *cls, const char *sel, unsigned long capacity)
{
    (void)capacity;
    return data_class_data(cls, sel);
}

static void *data_class_with_length(void *cls, const char *sel, unsigned long n)
{
    plsr_data *d = data_class_data(cls, sel);
    if (!d)
        return NULL;
    d->bytes = calloc(n ? n : 1, 1);
    if (!d->bytes)
        return NULL;
    d->length = n;
    return d;
}

static void *data_class_with_data(void *cls, const char *sel, void *other)
{
    plsr_data *o = other;
    if (!o)
        return data_class_data(cls, sel);
    return data_class_with_bytes(cls, sel, o->bytes, o->length);
}

static void *data_subdata_with_range(void *self, const char *sel, plsr_nsrange r)
{
    plsr_data *d = self;
    if (!d || r.location > d->length)
        return NULL;
    if (r.length > d->length - r.location)
        r.length = d->length - r.location;
    return data_class_with_bytes(*(void **)self, sel,
                                 (const unsigned char *)d->bytes + r.location,
                                 r.length);
}

static void data_get_bytes(void *self, const char *sel, void *buffer, unsigned long n)
{
    plsr_data *d = self;
    (void)sel;
    if (buffer && d && d->bytes)
        memcpy(buffer, d->bytes, n < d->length ? n : d->length);
}

static void data_get_bytes_range(void *self, const char *sel, void *buffer, plsr_nsrange r)
{
    plsr_data *d = self;
    (void)sel;
    if (!buffer || !d || !d->bytes || r.location > d->length)
        return;
    if (r.length > d->length - r.location)
        r.length = d->length - r.location;
    memcpy(buffer, (const unsigned char *)d->bytes + r.location, r.length);
}

static int data_is_equal_to_data(void *self, const char *sel, void *other)
{
    plsr_data *a = self;
    plsr_data *b = other;
    (void)sel;
    return a == b ||
           (a && b && a->length == b->length &&
            (!a->length || !memcmp(a->bytes, b->bytes, a->length)));
}

static int data_write_file(plsr_data *d, const char *path)
{
    FILE *f;
    if (!d || !path)
        return 0;
    f = fopen(path, "wb");
    if (!f)
        return 0;
    if (d->length && fwrite(d->bytes, 1, d->length, f) != d->length) {
        fclose(f);
        return 0;
    }
    return fclose(f) == 0;
}

static int data_write_to_file(void *self, const char *sel, void *path,
                              unsigned long options, void *error)
{
    (void)sel;
    (void)options;
    (void)error;
    return data_write_file(self, path ? obj_utf8(path) : NULL);
}

static int data_write_to_file_atomically(void *self, const char *sel, void *path,
                                          int atomically)
{
    (void)atomically;
    return data_write_to_file(self, sel, path, 0, NULL);
}

static int data_write_to_url(void *self, const char *sel, void *url,
                             unsigned long options, void *error)
{
    void *path = url ? msg(url, "path") : NULL;
    return data_write_to_file(self, sel, path, options, error);
}

static int data_write_to_url_atomically(void *self, const char *sel, void *url,
                                         int atomically)
{
    (void)atomically;
    return data_write_to_url(self, sel, url, 0, NULL);
}

static void *data_copy(void *self, const char *sel)
{
    (void)sel;
    return data_class_with_data(*(void **)self, sel, self);
}

static void *data_description(void *self, const char *sel)
{
    char buf[64];
    plsr_data *d = self;
    (void)sel;
    snprintf(buf, sizeof buf, "%lu bytes", d ? d->length : 0);
    return str_from_cstr(&plsr_NSString_class, buf);
}

static void *data_init_with_data(void *self, const char *sel, void *other)
{
    plsr_data *d = self;
    plsr_data *o = other;
    (void)sel;
    if (!d || !o || d == o)
        return self;
    free(d->bytes);
    d->bytes = malloc(o->length ? o->length : 1);
    if (!d->bytes)
        return self;
    if (o->length)
        memcpy(d->bytes, o->bytes, o->length);
    d->length = o->length;
    return self;
}

static void *data_bytes(void *self, const char *sel)
{
    (void)sel;
    return ((plsr_data *)self)->bytes;
}
static unsigned long data_length(void *self, const char *sel)
{
    (void)sel;
    return ((plsr_data *)self)->length;
}
static void data_dealloc(void *self, const char *sel)
{
    plsr_data *d = self;
    (void)sel;
    free(d->bytes);
    free(d);
}

/* ---- method tables ---- */

static method_array NSObject_instance_methods = {
    24, 12, {
        { "init", "", (void *)ns_init },
        { "dealloc", "", (void *)ns_dealloc },
        { "self", "", (void *)ns_self },
        { "class", "", (void *)ns_class },
        { "retain", "", (void *)ns_retain },
        { "release", "", (void *)ns_release },
        { "autorelease", "", (void *)ns_autorelease },
        { "copy", "", (void *)ns_copy },
        { "isEqual:", "", (void *)ns_is_equal },
        { "hash", "", (void *)ns_hash },
        { "isKindOfClass:", "", (void *)ns_is_kind_of_class },
        { "respondsToSelector:", "", (void *)ns_responds_to_selector },
    }
};

static method_array NSObject_class_methods = {
    24, 2, {
        { "alloc", "", (void *)ns_alloc },
        { "new", "", (void *)ns_new },
    }
};

static method_array NSConstantString_methods = {
    24, 13, {
        { "UTF8String", "", (void *)cs_utf8 },
        { "stringByAppendingString:", "", (void *)str_string_by_appending },
        { "length", "", (void *)cs_length },
        { "description", "", (void *)cs_description },
        { "isEqual:", "", (void *)cs_is_equal },
        { "isEqualToString:", "", (void *)cs_is_equal_to_string },
        { "hash", "", (void *)cs_hash },
        { "getCharacters:range:", "", (void *)str_get_characters },
        { "stringByAppendingPathExtension:", "",
          (void *)str_by_appending_path_extension },
        { "stringByDeletingPathExtension", "",
          (void *)str_by_deleting_path_extension },
        { "lastPathComponent", "", (void *)str_last_path_component },
        { "_encodingCantBeStoredInEightBitCFString", "",
          (void *)str_encoding_cant_be_stored_eight_bit },
        { "copy", "", (void *)ns_copy },
    }
};

static method_array NSString_methods = {
    24, 29, {
        { "init", "", (void *)str_init },
        { "initWithUTF8String:", "", (void *)str_init_with_utf8 },
        { "initWithBytes:length:encoding:", "", (void *)str_init_with_bytes_encoding },
        { "initWithData:encoding:", "", (void *)str_init_with_data_encoding },
        { "UTF8String", "", (void *)str_utf8 },
        { "length", "", (void *)str_length },
        { "description", "", (void *)str_description },
        { "isEqual:", "", (void *)str_is_equal },
        { "isEqualToString:", "", (void *)str_is_equal_to_string },
        { "hash", "", (void *)str_hash },
        { "getCharacters:range:", "", (void *)str_get_characters },
        { "characterIsMember:", "", (void *)str_character_is_member },
        { "longCharacterIsMember:", "", (void *)str_character_is_member },
        { "invertedSet", "", (void *)str_inverted_set },
        { "rangeOfCharacterFromSet:", "",
          (void *)str_range_of_character_from_set },
        { "rangeOfCharacterFromSet:options:", "",
          (void *)str_range_of_character_from_set_options },
        { "rangeOfCharacterFromSet:options:range:", "",
          (void *)str_range_of_character_from_set_options_range },
        { "stringByAppendingString:", "", (void *)str_string_by_appending },
        { "stringByAppendingPathComponent:", "",
          (void *)str_by_appending_path_component },
        { "stringByAppendingPathExtension:", "",
          (void *)str_by_appending_path_extension },
        { "stringByDeletingPathExtension", "",
          (void *)str_by_deleting_path_extension },
        { "lastPathComponent", "", (void *)str_last_path_component },
        { "stringByDeletingLastPathComponent", "",
          (void *)str_by_deleting_last_path_component },
        { "pathExtension", "", (void *)str_path_extension },
        { "_encodingCantBeStoredInEightBitCFString", "",
          (void *)str_encoding_cant_be_stored_eight_bit },
        { "copy", "", (void *)str_copy },
        { "mutableCopy", "", (void *)str_mutable_copy },
        { "dealloc", "", (void *)str_dealloc },
        { "appendString:", "", (void *)str_append },
    }
};

static method_array NSString_class_methods = {
    24, 6, {
        { "string", "", (void *)str_class_string },
        { "stringWithUTF8String:", "", (void *)str_class_with_utf8 },
        { "stringWithString:", "", (void *)str_class_with_string },
        { "stringWithFormat:", "", (void *)str_class_with_format },
        { "stringWithContentsOfFile:", "", (void *)str_class_with_contents_file },
        { "stringWithContentsOfFile:encoding:error:", "",
          (void *)str_class_with_contents_file },
    }
};

static method_array NSMutableString_methods = {
    24, 2, {
        { "appendString:", "", (void *)str_append },
        { "setString:", "", (void *)str_set_string },
    }
};

static method_array NSMutableString_class_methods = {
    24, 1, {
        { "string", "", (void *)str_class_string },
    }
};

static method_array NSNumber_class_methods = {
    24, 15, {
        { "numberWithInt:", "", (void *)num_with_int },
        { "numberWithInteger:", "", (void *)num_with_long },
        { "numberWithLongLong:", "", (void *)num_with_long },
        { "numberWithUnsignedInt:", "", (void *)num_with_uint },
        { "numberWithUnsignedInteger:", "", (void *)num_with_ull },
        { "numberWithUnsignedLongLong:", "", (void *)num_with_ull },
        { "numberWithDouble:", "", (void *)num_with_double },
        { "numberWithFloat:", "", (void *)num_with_float },
        { "numberWithChar:", "", (void *)num_with_char },
        { "numberWithShort:", "", (void *)num_with_short },
        { "numberWithUnsignedChar:", "", (void *)num_with_uchar },
        { "numberWithUnsignedShort:", "", (void *)num_with_ushort },
        { "numberWithUnsignedLong:", "", (void *)num_with_ulong },
        { "numberWithBool:", "", (void *)num_with_bool },
    }
};

static method_array NSNumber_methods = {
    24, 18, {
        { "intValue", "", (void *)num_int_value },
        { "integerValue", "", (void *)num_integer_value },
        { "longLongValue", "", (void *)num_long_long_value },
        { "unsignedIntValue", "", (void *)num_unsigned_int_value },
        { "unsignedIntegerValue", "", (void *)num_unsigned_long_long_value },
        { "unsignedLongLongValue", "", (void *)num_unsigned_long_long_value },
        { "floatValue", "", (void *)num_float_value },
        { "charValue", "", (void *)num_char_value },
        { "shortValue", "", (void *)num_short_value },
        { "unsignedCharValue", "", (void *)num_unsigned_char_value },
        { "unsignedShortValue", "", (void *)num_unsigned_short_value },
        { "doubleValue", "", (void *)num_double_value },
        { "boolValue", "", (void *)num_bool_value },
        { "stringValue", "", (void *)num_string_value },
        { "description", "", (void *)num_description },
        { "isEqual:", "", (void *)num_is_equal },
        { "hash", "", (void *)num_hash },
        { "dealloc", "", (void *)num_dealloc },
    }
};

static method_array NSArray_class_methods = {
    24, 4, {
        { "array", "", (void *)array_class_array },
        { "arrayWithObject:", "", (void *)array_class_with_object },
        { "arrayWithObjects:count:", "", (void *)array_class_with_objects_count },
        { "arrayWithObjects:", "", (void *)array_class_with_objects },
    }
};

static method_array NSArray_methods = {
    24, 13, {
        { "count", "", (void *)array_count },
        { "countByEnumeratingWithState:objects:count:", "",
          (void *)array_count_by_enumerating },
        { "objectAtIndex:", "", (void *)array_object_at },
        { "objectAtIndexedSubscript:", "", (void *)array_object_at },
        { "firstObject", "", (void *)array_first },
        { "lastObject", "", (void *)array_last },
        { "containsObject:", "", (void *)array_contains },
        { "description", "", (void *)array_description },
        { "copy", "", (void *)array_copy },
        { "mutableCopy", "", (void *)array_mutable_copy },
        { "copyWithZone:", "", (void *)array_copy_with_zone },
        { "dealloc", "", (void *)array_dealloc },
        { "hash", "", (void *)ns_hash },
    }
};

static method_array NSMutableArray_methods = {
    24, 12, {
        { "countByEnumeratingWithState:objects:count:", "",
          (void *)array_count_by_enumerating },
        { "addObject:", "", (void *)array_add_object },
        { "addObjectsFromArray:", "", (void *)array_add_objects_from_array },
        { "insertObject:atIndex:", "", (void *)array_insert_object_at },
        { "removeObjectAtIndex:", "", (void *)array_remove_at },
        { "removeAllObjects", "", (void *)array_remove_all },
        { "removeLastObject", "", (void *)array_remove_at },
        { "setObject:atIndexedSubscript:", "", (void *)array_set_object_at_index },
        { "replaceObjectAtIndex:withObject:", "", (void *)array_set_object_at_index },
        { "copy", "", (void *)array_copy },
        { "mutableCopy", "", (void *)array_mutable_copy },
        { "copyWithZone:", "", (void *)array_copy_with_zone },
    }
};

static method_array NSMutableArray_class_methods = {
    24, 1, {
        { "array", "", (void *)array_class_array },
    }
};

static method_array NSDictionary_class_methods = {
    24, 5, {
        { "dictionary", "", (void *)dict_class_dictionary },
        { "dictionaryWithObjects:forKeys:count:", "", (void *)dict_class_with_objects },
        { "dictionaryWithObject:forKey:", "", (void *)dict_class_with_object },
        { "dictionaryWithContentsOfFile:", "", (void *)dict_class_with_contents_of_file },
        { "dictionaryWithContentsOfURL:", "", (void *)dict_class_with_contents_of_url },
    }
};

static method_array NSDictionary_methods = {
    24, 10, {
        { "count", "", (void *)dict_count },
        { "objectForKey:", "", (void *)dict_object_for_key },
        { "objectForKeyedSubscript:", "", (void *)dict_object_for_key },
        { "allKeys", "", (void *)dict_all_keys },
        { "allValues", "", (void *)dict_all_values },
        { "description", "", (void *)dict_description },
        { "copy", "", (void *)dict_copy },
        { "mutableCopy", "", (void *)dict_mutable_copy },
        { "copyWithZone:", "", (void *)dict_copy_with_zone },
        { "dealloc", "", (void *)dict_dealloc },
    }
};

static method_array NSNull_class_methods = {
    24, 1, { { "null", "", (void *)null_class_null } }
};
static method_array NSNull_methods = {
    24, 2, {
        { "description", "", (void *)null_description },
        { "isEqual:", "", (void *)null_is_equal },
    }
};

static method_array NSData_class_methods = {
    24, 11, {
        { "data", "", (void *)data_class_data },
        { "dataWithBytes:length:", "", (void *)data_class_with_bytes },
        { "dataWithBytesNoCopy:length:", "", (void *)data_class_with_bytes_no_copy },
        { "dataWithBytesNoCopy:length:freeWhenDone:", "",
          (void *)data_class_with_bytes_no_copy },
        { "dataWithCapacity:", "", (void *)data_class_with_capacity },
        { "dataWithContentsOfFile:", "", (void *)data_class_with_contents_of_file },
        { "dataWithContentsOfFile:options:error:", "",
          (void *)data_class_with_contents_file_options },
        { "dataWithContentsOfURL:", "", (void *)data_class_with_contents_of_url },
        { "dataWithContentsOfURL:options:error:", "",
          (void *)data_class_with_contents_url_options },
        { "dataWithData:", "", (void *)data_class_with_data },
        { "dataWithLength:", "", (void *)data_class_with_length },
    }
};
static method_array NSData_methods = {
    24, 15, {
        { "bytes", "", (void *)data_bytes },
        { "length", "", (void *)data_length },
        { "subdataWithRange:", "", (void *)data_subdata_with_range },
        { "getBytes:length:", "", (void *)data_get_bytes },
        { "getBytes:range:", "", (void *)data_get_bytes_range },
        { "isEqualToData:", "", (void *)data_is_equal_to_data },
        { "writeToFile:atomically:", "", (void *)data_write_to_file_atomically },
        { "writeToFile:options:error:", "", (void *)data_write_to_file },
        { "writeToURL:atomically:", "", (void *)data_write_to_url_atomically },
        { "writeToURL:options:error:", "", (void *)data_write_to_url },
        { "copy", "", (void *)data_copy },
        { "mutableCopy", "", (void *)data_copy },
        { "description", "", (void *)data_description },
        { "initWithData:", "", (void *)data_init_with_data },
        { "dealloc", "", (void *)data_dealloc },
    }
};

/* ---- class metadata ---- */

#define RO(flags_, size_, name_, methods_) \
    { (flags_), 0, (size_), 0, NULL, (name_), (plsr_method_list *)(methods_), \
      NULL, NULL, NULL, NULL }

static const plsr_class_ro NSObject_ro = RO(0, 8, "NSObject", &NSObject_instance_methods);
static const plsr_class_ro NSObject_meta_ro = RO(PLSR_RO_META, 8, "NSObject", &NSObject_class_methods);
static const plsr_class_ro NSConstantString_ro = RO(0, 24, "NSConstantString", &NSConstantString_methods);
static const plsr_class_ro NSConstantString_meta_ro = RO(PLSR_RO_META, 24, "NSConstantString", NULL);
static const plsr_class_ro NSString_ro = RO(0, 32, "NSString", &NSString_methods);
static const plsr_class_ro NSString_meta_ro = RO(PLSR_RO_META, 32, "NSString", &NSString_class_methods);
static const plsr_class_ro NSMutableString_ro = RO(0, 32, "NSMutableString", &NSMutableString_methods);
static const plsr_class_ro NSMutableString_meta_ro = RO(PLSR_RO_META, 32, "NSMutableString", &NSMutableString_class_methods);
static const plsr_class_ro NSNumber_ro = RO(0, 32, "NSNumber", &NSNumber_methods);
static const plsr_class_ro NSNumber_meta_ro = RO(PLSR_RO_META, 32, "NSNumber", &NSNumber_class_methods);
static const plsr_class_ro NSArray_ro = RO(0, 24, "NSArray", &NSArray_methods);
static const plsr_class_ro NSArray_meta_ro = RO(PLSR_RO_META, 24, "NSArray", &NSArray_class_methods);
static const plsr_class_ro NSMutableArray_ro = RO(0, 24, "NSMutableArray", &NSMutableArray_methods);
static const plsr_class_ro NSMutableArray_meta_ro = RO(PLSR_RO_META, 24, "NSMutableArray", &NSMutableArray_class_methods);
static const plsr_class_ro NSDictionary_ro = RO(0, 32, "NSDictionary", &NSDictionary_methods);
static const plsr_class_ro NSDictionary_meta_ro = RO(PLSR_RO_META, 32, "NSDictionary", &NSDictionary_class_methods);
static const plsr_class_ro NSNull_ro = RO(0, 8, "NSNull", &NSNull_methods);
static const plsr_class_ro NSNull_meta_ro = RO(PLSR_RO_META, 8, "NSNull", &NSNull_class_methods);
static const plsr_class_ro NSData_ro = RO(0, 32, "NSData", &NSData_methods);
static const plsr_class_ro NSData_meta_ro = RO(PLSR_RO_META, 32, "NSData", &NSData_class_methods);

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

static void foundation_init(void) __attribute__((constructor));
static void foundation_init(void)
{
    wire(&plsr_NSObject_class, &plsr_NSObject_meta, NULL, NULL,
         &NSObject_ro, &NSObject_meta_ro);
    wire(&plsr_NSString_class, &plsr_NSString_meta,
         &plsr_NSObject_class, &plsr_NSObject_meta, &NSString_ro, &NSString_meta_ro);
    wire(&plsr_NSMutableString_class, &plsr_NSMutableString_meta,
         &plsr_NSString_class, &plsr_NSString_meta,
         &NSMutableString_ro, &NSMutableString_meta_ro);
    wire(&plsr_NSConstantString_class, &plsr_NSConstantString_meta,
         &plsr_NSString_class, &plsr_NSString_meta,
         &NSConstantString_ro, &NSConstantString_meta_ro);
    wire(&plsr_NSNumber_class, &plsr_NSNumber_meta,
         &plsr_NSObject_class, &plsr_NSObject_meta, &NSNumber_ro, &NSNumber_meta_ro);
    wire(&plsr_NSArray_class, &plsr_NSArray_meta,
         &plsr_NSObject_class, &plsr_NSObject_meta, &NSArray_ro, &NSArray_meta_ro);
    wire(&plsr_NSMutableArray_class, &plsr_NSMutableArray_meta,
         &plsr_NSArray_class, &plsr_NSArray_meta,
         &NSMutableArray_ro, &NSMutableArray_meta_ro);
    wire(&plsr_NSDictionary_class, &plsr_NSDictionary_meta,
         &plsr_NSObject_class, &plsr_NSObject_meta,
         &NSDictionary_ro, &NSDictionary_meta_ro);
    wire(&plsr_NSNull_class, &plsr_NSNull_meta,
         &plsr_NSObject_class, &plsr_NSObject_meta, &NSNull_ro, &NSNull_meta_ro);
    wire(&plsr_NSData_class, &plsr_NSData_meta,
         &plsr_NSObject_class, &plsr_NSObject_meta, &NSData_ro, &NSData_meta_ro);
}

/* ---- Foundation geometry ---- */

int plsr_NSContainsRect(plsr_nsrect outer, plsr_nsrect inner)
    __asm__("_NSContainsRect");
int plsr_NSContainsRect(plsr_nsrect outer, plsr_nsrect inner)
{
    return outer.origin.x <= inner.origin.x &&
           outer.origin.y <= inner.origin.y &&
           outer.origin.x + outer.size.width >= inner.origin.x + inner.size.width &&
           outer.origin.y + outer.size.height >= inner.origin.y + inner.size.height;
}

int plsr_NSEqualPoints(plsr_nspoint a, plsr_nspoint b) __asm__("_NSEqualPoints");
int plsr_NSEqualPoints(plsr_nspoint a, plsr_nspoint b)
{
    return a.x == b.x && a.y == b.y;
}
int plsr_NSEqualSizes(plsr_nssize a, plsr_nssize b) __asm__("_NSEqualSizes");
int plsr_NSEqualSizes(plsr_nssize a, plsr_nssize b)
{
    return a.width == b.width && a.height == b.height;
}
int plsr_NSEqualRects(plsr_nsrect a, plsr_nsrect b) __asm__("_NSEqualRects");
int plsr_NSEqualRects(plsr_nsrect a, plsr_nsrect b)
{
    return plsr_NSEqualPoints(a.origin, b.origin) &&
           plsr_NSEqualSizes(a.size, b.size);
}
int plsr_NSEdgeInsetsEqual(plsr_nsedgeinsets a, plsr_nsedgeinsets b)
    __asm__("_NSEdgeInsetsEqual");
int plsr_NSEdgeInsetsEqual(plsr_nsedgeinsets a, plsr_nsedgeinsets b)
{
    return a.top == b.top && a.left == b.left && a.bottom == b.bottom &&
           a.right == b.right;
}

int plsr_NSIntersectsRect(plsr_nsrect a, plsr_nsrect b) __asm__("_NSIntersectsRect");
int plsr_NSIntersectsRect(plsr_nsrect a, plsr_nsrect b)
{
    return a.origin.x < b.origin.x + b.size.width &&
           b.origin.x < a.origin.x + a.size.width &&
           a.origin.y < b.origin.y + b.size.height &&
           b.origin.y < a.origin.y + a.size.height;
}

int plsr_NSIsEmptyRect(plsr_nsrect r) __asm__("_NSIsEmptyRect");
int plsr_NSIsEmptyRect(plsr_nsrect r)
{
    return r.size.width <= 0.0 || r.size.height <= 0.0;
}

int plsr_NSPointInRect(plsr_nspoint p, plsr_nsrect r) __asm__("_NSPointInRect");
int plsr_NSPointInRect(plsr_nspoint p, plsr_nsrect r)
{
    return p.x >= r.origin.x && p.x <= r.origin.x + r.size.width &&
           p.y >= r.origin.y && p.y <= r.origin.y + r.size.height;
}

int plsr_NSMouseInRect(plsr_nspoint p, plsr_nsrect r, int flipped)
    __asm__("_NSMouseInRect");
int plsr_NSMouseInRect(plsr_nspoint p, plsr_nsrect r, int flipped)
{
    (void)flipped;
    return plsr_NSPointInRect(p, r);
}

plsr_nsrect plsr_NSInsetRect(plsr_nsrect r, double dx, double dy) __asm__("_NSInsetRect");
plsr_nsrect plsr_NSInsetRect(plsr_nsrect r, double dx, double dy)
{
    r.origin.x += dx;
    r.origin.y += dy;
    r.size.width -= 2.0 * dx;
    r.size.height -= 2.0 * dy;
    return r;
}

plsr_nsrect plsr_NSOffsetRect(plsr_nsrect r, double dx, double dy)
    __asm__("_NSOffsetRect");
plsr_nsrect plsr_NSOffsetRect(plsr_nsrect r, double dx, double dy)
{
    r.origin.x += dx;
    r.origin.y += dy;
    return r;
}

plsr_nsrect plsr_NSIntegralRect(plsr_nsrect r) __asm__("_NSIntegralRect");
plsr_nsrect plsr_NSIntegralRect(plsr_nsrect r)
{
    double maxx = r.origin.x + r.size.width;
    double maxy = r.origin.y + r.size.height;
    r.origin.x = floor(r.origin.x);
    r.origin.y = floor(r.origin.y);
    r.size.width = ceil(maxx) - r.origin.x;
    r.size.height = ceil(maxy) - r.origin.y;
    return r;
}

plsr_nsrect plsr_NSIntegralRectWithOptions(plsr_nsrect r, unsigned long options)
    __asm__("_NSIntegralRectWithOptions");
plsr_nsrect plsr_NSIntegralRectWithOptions(plsr_nsrect r, unsigned long options)
{
    (void)options;
    return plsr_NSIntegralRect(r);
}

plsr_nsrect plsr_NSIntersectionRect(plsr_nsrect a, plsr_nsrect b)
    __asm__("_NSIntersectionRect");
plsr_nsrect plsr_NSIntersectionRect(plsr_nsrect a, plsr_nsrect b)
{
    double x0 = a.origin.x > b.origin.x ? a.origin.x : b.origin.x;
    double y0 = a.origin.y > b.origin.y ? a.origin.y : b.origin.y;
    double x1 = a.origin.x + a.size.width < b.origin.x + b.size.width
                    ? a.origin.x + a.size.width
                    : b.origin.x + b.size.width;
    double y1 = a.origin.y + a.size.height < b.origin.y + b.size.height
                    ? a.origin.y + a.size.height
                    : b.origin.y + b.size.height;
    plsr_nsrect r = { { x0, y0 }, { x1 - x0, y1 - y0 } };
    if (r.size.width < 0.0)
        r.size.width = 0.0;
    if (r.size.height < 0.0)
        r.size.height = 0.0;
    return r;
}

plsr_nsrect plsr_NSUnionRect(plsr_nsrect a, plsr_nsrect b) __asm__("_NSUnionRect");
plsr_nsrect plsr_NSUnionRect(plsr_nsrect a, plsr_nsrect b)
{
    double x0, y0, x1, y1;
    if (plsr_NSIsEmptyRect(a))
        return b;
    if (plsr_NSIsEmptyRect(b))
        return a;
    x0 = a.origin.x < b.origin.x ? a.origin.x : b.origin.x;
    y0 = a.origin.y < b.origin.y ? a.origin.y : b.origin.y;
    x1 = a.origin.x + a.size.width > b.origin.x + b.size.width
             ? a.origin.x + a.size.width
             : b.origin.x + b.size.width;
    y1 = a.origin.y + a.size.height > b.origin.y + b.size.height
             ? a.origin.y + a.size.height
             : b.origin.y + b.size.height;
    {
        plsr_nsrect r = { { x0, y0 }, { x1 - x0, y1 - y0 } };
        return r;
    }
}

void plsr_NSDivideRect(plsr_nsrect in, plsr_nsrect *slice, plsr_nsrect *remainder,
                       double amount, unsigned long edge)
    __asm__("_NSDivideRect");
void plsr_NSDivideRect(plsr_nsrect in, plsr_nsrect *slice, plsr_nsrect *remainder,
                       double amount, unsigned long edge)
{
    plsr_nsrect s = in, rem = in;
    switch (edge) {
    case 0: /* minX */
        s.size.width = amount;
        rem.origin.x += amount;
        rem.size.width -= amount;
        break;
    case 1: /* minY */
        s.size.height = amount;
        rem.origin.y += amount;
        rem.size.height -= amount;
        break;
    case 2: /* maxX */
        s.origin.x = in.origin.x + in.size.width - amount;
        s.size.width = amount;
        rem.size.width -= amount;
        break;
    default: /* maxY */
        s.origin.y = in.origin.y + in.size.height - amount;
        s.size.height = amount;
        rem.size.height -= amount;
        break;
    }
    if (slice)
        *slice = s;
    if (remainder)
        *remainder = rem;
}

plsr_nsrange plsr_NSIntersectionRange(plsr_nsrange a, plsr_nsrange b)
    __asm__("_NSIntersectionRange");
plsr_nsrange plsr_NSIntersectionRange(plsr_nsrange a, plsr_nsrange b)
{
    unsigned long start = a.location > b.location ? a.location : b.location;
    unsigned long end_a = a.location + a.length;
    unsigned long end_b = b.location + b.length;
    unsigned long end = end_a < end_b ? end_a : end_b;
    plsr_nsrange r = { start, end > start ? end - start : 0 };
    return r;
}

plsr_nsrange plsr_NSUnionRange(plsr_nsrange a, plsr_nsrange b) __asm__("_NSUnionRange");
plsr_nsrange plsr_NSUnionRange(plsr_nsrange a, plsr_nsrange b)
{
    unsigned long start = a.location < b.location ? a.location : b.location;
    unsigned long end_a = a.location + a.length;
    unsigned long end_b = b.location + b.length;
    unsigned long end = end_a > end_b ? end_a : end_b;
    plsr_nsrange r = { start, end - start };
    return r;
}

void *plsr_NSStringFromPoint(plsr_nspoint p) __asm__("_NSStringFromPoint");
void *plsr_NSStringFromPoint(plsr_nspoint p)
{
    char buf[128];
    snprintf(buf, sizeof buf, "{%g, %g}", p.x, p.y);
    return str_from_cstr(&plsr_NSString_class, buf);
}
void *plsr_NSStringFromSize(plsr_nssize s) __asm__("_NSStringFromSize");
void *plsr_NSStringFromSize(plsr_nssize s)
{
    char buf[128];
    snprintf(buf, sizeof buf, "{%g, %g}", s.width, s.height);
    return str_from_cstr(&plsr_NSString_class, buf);
}
void *plsr_NSStringFromRect(plsr_nsrect r) __asm__("_NSStringFromRect");
void *plsr_NSStringFromRect(plsr_nsrect r)
{
    char buf[256];
    snprintf(buf, sizeof buf, "{{%g, %g}, {%g, %g}}", r.origin.x, r.origin.y,
             r.size.width, r.size.height);
    return str_from_cstr(&plsr_NSString_class, buf);
}
void *plsr_NSStringFromRange(plsr_nsrange r) __asm__("_NSStringFromRange");
void *plsr_NSStringFromRange(plsr_nsrange r)
{
    char buf[128];
    snprintf(buf, sizeof buf, "{%lu, %lu}", r.location, r.length);
    return str_from_cstr(&plsr_NSString_class, buf);
}

plsr_nspoint plsr_NSPointFromString(void *s) __asm__("_NSPointFromString");
plsr_nspoint plsr_NSPointFromString(void *s)
{
    plsr_nspoint p = { 0.0, 0.0 };
    sscanf(obj_utf8(s), " { %lf , %lf } ", &p.x, &p.y);
    return p;
}
plsr_nssize plsr_NSSizeFromString(void *s) __asm__("_NSSizeFromString");
plsr_nssize plsr_NSSizeFromString(void *s)
{
    plsr_nssize z = { 0.0, 0.0 };
    sscanf(obj_utf8(s), " { %lf , %lf } ", &z.width, &z.height);
    return z;
}
plsr_nsrect plsr_NSRectFromString(void *s) __asm__("_NSRectFromString");
plsr_nsrect plsr_NSRectFromString(void *s)
{
    plsr_nsrect r = { { 0.0, 0.0 }, { 0.0, 0.0 } };
    sscanf(obj_utf8(s), " { { %lf , %lf } , { %lf , %lf } } ", &r.origin.x,
           &r.origin.y, &r.size.width, &r.size.height);
    return r;
}
plsr_nsrange plsr_NSRangeFromString(void *s) __asm__("_NSRangeFromString");
plsr_nsrange plsr_NSRangeFromString(void *s)
{
    plsr_nsrange r = { 0, 0 };
    sscanf(obj_utf8(s), " { %lu , %lu } ", &r.location, &r.length);
    return r;
}

unsigned long plsr_NSPageSize(void) __asm__("_NSPageSize");
unsigned long plsr_NSPageSize(void)
{
    long n = sysconf(_SC_PAGESIZE);
    return n > 0 ? (unsigned long)n : 4096;
}
unsigned long plsr_NSRoundUpToMultipleOfPageSize(unsigned long n)
    __asm__("_NSRoundUpToMultipleOfPageSize");
unsigned long plsr_NSRoundUpToMultipleOfPageSize(unsigned long n)
{
    unsigned long p = plsr_NSPageSize();
    return p ? (n + p - 1) / p * p : n;
}
unsigned long plsr_NSRoundDownToMultipleOfPageSize(unsigned long n)
    __asm__("_NSRoundDownToMultipleOfPageSize");
unsigned long plsr_NSRoundDownToMultipleOfPageSize(unsigned long n)
{
    unsigned long p = plsr_NSPageSize();
    return p ? n / p * p : n;
}
int plsr_NSCopyMemoryPages(const void *src, void *dst, unsigned long bytes)
    __asm__("_NSCopyMemoryPages");
int plsr_NSCopyMemoryPages(const void *src, void *dst, unsigned long bytes)
{
    memcpy(dst, src, bytes);
    return 0;
}
void *plsr_NSCopyObject(void *obj, unsigned long extra, void *zone)
    __asm__("_NSCopyObject");
void *plsr_NSCopyObject(void *obj, unsigned long extra, void *zone)
{
    (void)obj;
    (void)extra;
    (void)zone;
    return NULL;
}
void *plsr_NSFileTypeForHFSTypeCode(uint32_t code) __asm__("_NSFileTypeForHFSTypeCode");
void *plsr_NSFileTypeForHFSTypeCode(uint32_t code)
{
    (void)code;
    return NULL;
}
const char *plsr_NSGetSizeAndAlignment(const char *type, unsigned long *size,
                                      unsigned long *align)
    __asm__("_NSGetSizeAndAlignment");
const char *plsr_NSGetSizeAndAlignment(const char *type, unsigned long *size,
                                      unsigned long *align)
{
    if (size)
        *size = 0;
    if (align)
        *align = 0;
    return type;
}
void *plsr_NSSearchPathForDirectoriesInDomains(unsigned long dir,
                                               unsigned long mask, int expand)
    __asm__("_NSSearchPathForDirectoriesInDomains");
void *plsr_NSSearchPathForDirectoriesInDomains(unsigned long dir,
                                               unsigned long mask, int expand)
{
    const char *home = getenv("HOME");
    char path[PATH_MAX];
    void *items[1];
    const char *suffix = "";
    (void)mask;
    (void)expand;
    if (!home || !*home)
        home = "/";
    switch (dir) {
    case 14: suffix = "/Library/Application Support"; break;
    case 5: suffix = "/Library"; break;
    case 13: suffix = "/Library/Caches"; break;
    case 12: suffix = "/Library/Preferences"; break;
    default: suffix = ""; break;
    }
    snprintf(path, sizeof path, "%s%s", home, suffix);
    items[0] = str_from_cstr(&plsr_NSString_class, path);
    return array_class_with_objects_count(&plsr_NSArray_class,
                                          "arrayWithObjects:count:", items, 1);
}

void *plsr_NSFormatList(void) __asm__("__NSFormatList");
void *plsr_NSFormatList(void) { return NULL; }
void *plsr_NSFormatNumberWithSkeleton(void) __asm__("__NSFormatNumberWithSkeleton");
void *plsr_NSFormatNumberWithSkeleton(void) { return NULL; }

int plsr_NSDecimalAdd(void *result, const void *lhs, const void *rhs, unsigned long mode)
    __asm__("_NSDecimalAdd");
int plsr_NSDecimalAdd(void *result, const void *lhs, const void *rhs, unsigned long mode)
{
    (void)result; (void)lhs; (void)rhs; (void)mode; return 1;
}
int plsr_NSDecimalSubtract(void *result, const void *lhs, const void *rhs, unsigned long mode)
    __asm__("_NSDecimalSubtract");
int plsr_NSDecimalSubtract(void *result, const void *lhs, const void *rhs, unsigned long mode)
{
    (void)result; (void)lhs; (void)rhs; (void)mode; return 1;
}
int plsr_NSDecimalMultiply(void *result, const void *lhs, const void *rhs, unsigned long mode)
    __asm__("_NSDecimalMultiply");
int plsr_NSDecimalMultiply(void *result, const void *lhs, const void *rhs, unsigned long mode)
{
    (void)result; (void)lhs; (void)rhs; (void)mode; return 1;
}
int plsr_NSDecimalDivide(void *result, const void *lhs, const void *rhs, unsigned long mode)
    __asm__("_NSDecimalDivide");
int plsr_NSDecimalDivide(void *result, const void *lhs, const void *rhs, unsigned long mode)
{
    (void)result; (void)lhs; (void)rhs; (void)mode; return 1;
}
int plsr_NSDecimalPower(void *result, const void *base, unsigned long power,
                        unsigned long mode) __asm__("_NSDecimalPower");
int plsr_NSDecimalPower(void *result, const void *base, unsigned long power,
                        unsigned long mode)
{
    (void)result; (void)base; (void)power; (void)mode; return 1;
}
void plsr_NSDecimalCompact(void *number) __asm__("_NSDecimalCompact");
void plsr_NSDecimalCompact(void *number) { (void)number; }
int plsr_NSDecimalCompare(const void *lhs, const void *rhs) __asm__("_NSDecimalCompare");
int plsr_NSDecimalCompare(const void *lhs, const void *rhs)
{
    (void)lhs; (void)rhs; return 0;
}
void *plsr_NSDecimalString(const void *number, void *locale)
    __asm__("_NSDecimalString");
void *plsr_NSDecimalString(const void *number, void *locale)
{
    (void)number; (void)locale; return NULL;
}

/* ---- NSLog ---- */

static void append_object(struct buf *b, void *obj)
{
    void *desc;
    if (!obj) {
        bput(b, "(null)");
        return;
    }
    desc = msg(obj, "description");
    if (desc && msg(desc, "UTF8String"))
        bput(b, obj_utf8(desc));
    else if (msg(obj, "UTF8String"))
        bput(b, obj_utf8(obj));
    else
        bput(b, obj_class_name(obj));
}

static int format_flag(char c)
{
    return c && strchr("-+ #0'", c) != NULL;
}

static void format_append_number(struct buf *b, const char *spec, va_list ap,
                                 int lenmod, int signed_value)
{
    char tmp[160];
    if (signed_value) {
        switch (lenmod) {
        case 1: case 2: snprintf(tmp, sizeof tmp, spec, va_arg(ap, int)); break;
        case 3: snprintf(tmp, sizeof tmp, spec, va_arg(ap, long long)); break;
        case 4: snprintf(tmp, sizeof tmp, spec, va_arg(ap, long)); break;
        case 5: snprintf(tmp, sizeof tmp, spec, va_arg(ap, long)); break;
        case 6: snprintf(tmp, sizeof tmp, spec, va_arg(ap, ptrdiff_t)); break;
        case 7: snprintf(tmp, sizeof tmp, spec, va_arg(ap, intmax_t)); break;
        default: snprintf(tmp, sizeof tmp, spec, va_arg(ap, int)); break;
        }
    } else {
        switch (lenmod) {
        case 1: case 2: snprintf(tmp, sizeof tmp, spec, va_arg(ap, unsigned)); break;
        case 3: snprintf(tmp, sizeof tmp, spec, va_arg(ap, unsigned long long)); break;
        case 4: snprintf(tmp, sizeof tmp, spec, va_arg(ap, unsigned long)); break;
        case 5: snprintf(tmp, sizeof tmp, spec, va_arg(ap, size_t)); break;
        case 6: snprintf(tmp, sizeof tmp, spec, va_arg(ap, size_t)); break;
        case 7: snprintf(tmp, sizeof tmp, spec, va_arg(ap, uintmax_t)); break;
        default: snprintf(tmp, sizeof tmp, spec, va_arg(ap, unsigned)); break;
        }
    }
    bput(b, tmp);
}

static void append_format(struct buf *b, const char *format, va_list ap)
{
    const char *p = format ? format : "";
    while (*p) {
        char spec[96];
        size_t n = 0;
        int lenmod = 0;
        char conv;
        if (*p != '%') {
            bputc(b, *p++);
            continue;
        }
        const char *start = p++;
        if (*p == '%') {
            bputc(b, '%');
            p++;
            continue;
        }
        spec[n++] = '%';
        while (*p && format_flag(*p) && n < sizeof(spec) - 8)
            spec[n++] = *p++;
        if (*p == '*') {
            int width = va_arg(ap, int);
            char num[24];
            int k = snprintf(num, sizeof num, "%d", width);
            for (int i = 0; i < k && n < sizeof(spec) - 8; i++)
                spec[n++] = num[i];
            p++;
        } else {
            while (*p >= '0' && *p <= '9' && n < sizeof(spec) - 8)
                spec[n++] = *p++;
        }
        if (*p == '.') {
            spec[n++] = '.';
            p++;
            if (*p == '*') {
                int prec = va_arg(ap, int);
                char num[24];
                int k = snprintf(num, sizeof num, "%d", prec);
                for (int i = 0; i < k && n < sizeof(spec) - 8; i++)
                    spec[n++] = num[i];
                p++;
            } else {
                while (*p >= '0' && *p <= '9' && n < sizeof(spec) - 8)
                    spec[n++] = *p++;
            }
        }
        if (p[0] == 'h' && p[1] == 'h') {
            lenmod = 1; spec[n++] = 'h'; spec[n++] = 'h'; p += 2;
        } else if (p[0] == 'h') {
            lenmod = 2; spec[n++] = 'h'; p++;
        } else if (p[0] == 'l' && p[1] == 'l') {
            lenmod = 3; spec[n++] = 'l'; spec[n++] = 'l'; p += 2;
        } else if (p[0] == 'l') {
            lenmod = 4; spec[n++] = 'l'; p++;
        } else if (p[0] == 'q') {
            lenmod = 3; spec[n++] = 'q'; p++;
        } else if (p[0] == 'z') {
            lenmod = 5; spec[n++] = 'z'; p++;
        } else if (p[0] == 't') {
            lenmod = 6; spec[n++] = 't'; p++;
        } else if (p[0] == 'j') {
            lenmod = 7; spec[n++] = 'j'; p++;
        }
        if (!*p) {
            spec[n] = 0;
            bput(b, spec);
            break;
        }
        conv = *p++;
        spec[n++] = conv;
        spec[n] = 0;
        switch (conv) {
        case '@': append_object(b, va_arg(ap, void *)); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            char tmp[1024];
            if (!s) s = "(null)";
            snprintf(tmp, sizeof tmp, spec, s);
            bput(b, tmp);
            break;
        }
        case 'c': {
            int c = va_arg(ap, int);
            char tmp[64];
            snprintf(tmp, sizeof tmp, spec, c);
            bput(b, tmp);
            break;
        }
        case 'd': case 'i':
            format_append_number(b, spec, ap, lenmod, 1);
            break;
        case 'u': case 'x': case 'X': case 'o':
            format_append_number(b, spec, ap, lenmod, 0);
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
        case 'a': case 'A': {
            char tmp[160];
            snprintf(tmp, sizeof tmp, spec, va_arg(ap, double));
            bput(b, tmp);
            break;
        }
        case 'p': {
            char tmp[64];
            snprintf(tmp, sizeof tmp, spec, va_arg(ap, void *));
            bput(b, tmp);
            break;
        }
        case 'n':
            (void)va_arg(ap, void *);
            break;
        default:
            bput(b, start);
            break;
        }
    }
}

void plsr_NSLogv(void *format, va_list ap) __asm__("_NSLogv");
void plsr_NSLogv(void *format, va_list ap)
{
    const char *f = format ? obj_utf8(format) : "";
    struct buf b = { 0 };
    append_format(&b, f, ap);
    if (b.p)
        fputs(b.p, stderr);
    fputc('\n', stderr);
    free(b.p);
}

void NSLog(void *format, ...) __asm__("_NSLog");
void NSLog(void *format, ...)
{
    va_list ap;
    va_start(ap, format);
    plsr_NSLogv(format, ap);
    va_end(ap);
}

__attribute__((visibility("default"), used))
const char *const plsr_host_imports[] = {
    "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation",
    NULL
};
