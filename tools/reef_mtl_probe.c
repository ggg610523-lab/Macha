/*
 * reef_mtl_probe: verify the libpulsar-metal-transl export surface and ABI.
 * Uses dlopen/dlsym only, so it does not need the library's private types.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static const char *symbols[] = {
    "reef_mtl_translator_version",
    "reef_mtl_library_create_from_source",
    "reef_mtl_library_create_from_data",
    "reef_mtl_library_release",
    "reef_mtl_library_function_count",
    "reef_mtl_library_function_name",
    "reef_mtl_library_function_stage",
    "reef_mtl_function_create",
    "reef_mtl_function_release",
    "reef_mtl_function_name",
    "reef_mtl_function_stage",
    "reef_mtl_function_attribute_count",
    "reef_mtl_function_attribute",
    "reef_mtl_function_constant_count",
    "reef_mtl_function_constant_at",
    "reef_mtl_function_translate",
    "reef_mtl_function_translate_linked",
    "reef_mtl_shader_stage",
    "reef_mtl_shader_glsl",
    "reef_mtl_shader_flags",
    "reef_mtl_shader_binding_count",
    "reef_mtl_shader_binding",
    "reef_mtl_shader_constant_sampler_count",
    "reef_mtl_shader_constant_sampler",
    "reef_mtl_shader_attribute_count",
    "reef_mtl_shader_attribute",
    "reef_mtl_shader_varying_count",
    "reef_mtl_shader_varying",
    "reef_mtl_shader_render_target_count",
    "reef_mtl_shader_render_target",
    "reef_mtl_shader_clip_distance_count",
    "reef_mtl_shader_uniform_location",
    "reef_mtl_shader_threadgroup_size",
    "reef_mtl_shader_release",
};

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] :
        "/home/mango/Desktop/reef-mac/pulsar-appexec/usr/lib/pulsar-appexec/"
        "lib/libpulsar-metal-transl.so";

    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 2;
    }

    size_t n = sizeof(symbols) / sizeof(symbols[0]);
    size_t missing = 0;
    for (size_t i = 0; i < n; i++) {
        void *p = dlsym(h, symbols[i]);
        if (!p) {
            printf("MISSING %s\n", symbols[i]);
            missing++;
        }
    }

    uint64_t (*version)(void) = (uint64_t (*)(void))
        dlsym(h, "reef_mtl_translator_version");
    if (version)
        printf("reef_mtl_translator_version() = %llu\n",
               (unsigned long long)version());

    dlclose(h);
    printf("symbols: %zu present, %zu missing\n", n - missing, missing);
    return missing ? 1 : 0;
}
