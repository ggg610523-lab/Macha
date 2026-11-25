#define _GNU_SOURCE
#include "swiftui.h"

#include <string.h>

static const plsr_swiftui_framework g_frameworks[] = {
#include "generated/frameworks_manifest.inc"
};

const plsr_swiftui_framework *plsr_swiftui_frameworks(size_t *count)
{
    if (count)
        *count = sizeof(g_frameworks) / sizeof(g_frameworks[0]);
    return g_frameworks;
}

const plsr_swiftui_framework *plsr_swiftui_framework_at(size_t index)
{
    size_t n = sizeof(g_frameworks) / sizeof(g_frameworks[0]);
    return index < n ? &g_frameworks[index] : NULL;
}

const plsr_swiftui_framework *plsr_swiftui_framework_named(const char *name)
{
    if (!name)
        return NULL;
    for (size_t i = 0; i < sizeof(g_frameworks) / sizeof(g_frameworks[0]); i++)
        if (!strcmp(g_frameworks[i].name, name))
            return &g_frameworks[i];
    return NULL;
}

const plsr_swiftui_framework *plsr_swiftui_framework_for_install_name(
    const char *install_name)
{
    if (!install_name)
        return NULL;
    for (size_t i = 0; i < sizeof(g_frameworks) / sizeof(g_frameworks[0]); i++)
        if (!strcmp(g_frameworks[i].install_name, install_name))
            return &g_frameworks[i];
    return NULL;
}

/* An external loader reads this manifest to bind install names. */
const char *const plsr_host_imports[] = {
#include "generated/install_names.inc"
    NULL
};
