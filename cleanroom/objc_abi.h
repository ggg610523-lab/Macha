#ifndef PLSR_OBJC_ABI_H
#define PLSR_OBJC_ABI_H

#include <stdint.h>

/* x86_64 non-fragile ABI layout, matching the metadata emitted by clang:
 * class_t { isa, superclass, cache, vtable, data } where data is a
 * class_ro_t pointer with the low bits used as flags. */
typedef struct plsr_class plsr_class;

typedef struct plsr_method {
    const char *name;   /* SEL: in our runtime, the selector cstring */
    const char *types;
    void *imp;
} plsr_method;

typedef struct plsr_method_list {
    uint32_t entsize_flags;
    uint32_t count;
    plsr_method methods[];
} plsr_method_list;

typedef struct plsr_class_ro {
    uint32_t flags;
    uint32_t instanceStart;
    uint32_t instanceSize;
    uint32_t reserved;
    const uint8_t *ivarLayout;
    const char *name;
    plsr_method_list *baseMethods;
    void *baseProtocols;
    void *ivars;
    const uint8_t *weakIvarLayout;
    void *baseProperties;
} plsr_class_ro;

struct plsr_class {
    plsr_class *isa;
    plsr_class *superclass;
    void *cache;
    void *vtable;
    uintptr_t data;
};

#define PLSR_RO(c) ((const plsr_class_ro *)((c)->data & ~(uintptr_t)7))
#define PLSR_RO_META 1u
#define PLSR_RO_STUB 0x100u

#define PLSR_METHOD_SIZE 24u

#endif
