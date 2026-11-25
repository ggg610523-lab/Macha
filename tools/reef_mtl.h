/*
 * reef_mtl.h: recovered accessor ABI for libpulsar-metal-transl.so.
 *
 * Handles are opaque. The accessor prototypes below are taken from the
 * decompiled exports; creator/translator entry points still have partially
 * recovered aggregate/pointer ABIs and are intentionally not declared here.
 */
#ifndef REEF_MTL_H
#define REEF_MTL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REEF_MTL_TRANSLATOR_VERSION 1

typedef struct reef_mtl_library reef_mtl_library;
typedef struct reef_mtl_function reef_mtl_function;
typedef struct reef_mtl_shader reef_mtl_shader;
typedef struct reef_mtl_attribute reef_mtl_attribute;
typedef struct reef_mtl_constant reef_mtl_constant;
typedef struct reef_mtl_binding reef_mtl_binding;
typedef struct reef_mtl_sampler reef_mtl_sampler;
typedef struct reef_mtl_varying reef_mtl_varying;

uint64_t reef_mtl_translator_version(void);

int reef_mtl_library_function_count(const reef_mtl_library *lib);
const char *reef_mtl_library_function_name(const reef_mtl_library *lib, int index);
uint32_t reef_mtl_library_function_stage(const reef_mtl_library *lib, int index);
void reef_mtl_library_release(reef_mtl_library *lib);

const char *reef_mtl_function_name(const reef_mtl_function *fn);
uint32_t reef_mtl_function_stage(const reef_mtl_function *fn);
int64_t reef_mtl_function_attribute_count(const reef_mtl_function *fn);
reef_mtl_attribute *reef_mtl_function_attribute(const reef_mtl_function *fn, int index);
int reef_mtl_function_constant_count(const reef_mtl_function *fn);
reef_mtl_constant *reef_mtl_function_constant_at(const reef_mtl_function *fn, int index);
void reef_mtl_function_release(reef_mtl_function *fn);

uint32_t reef_mtl_shader_stage(const reef_mtl_shader *sh);
const char *reef_mtl_shader_glsl(const reef_mtl_shader *sh);
uint64_t reef_mtl_shader_flags(const reef_mtl_shader *sh);
int64_t reef_mtl_shader_binding_count(const reef_mtl_shader *sh);
reef_mtl_binding *reef_mtl_shader_binding(const reef_mtl_shader *sh, int index);
int reef_mtl_shader_constant_sampler_count(const reef_mtl_shader *sh);
reef_mtl_sampler *reef_mtl_shader_constant_sampler(const reef_mtl_shader *sh, int index);
int64_t reef_mtl_shader_attribute_count(const reef_mtl_shader *sh);
reef_mtl_attribute *reef_mtl_shader_attribute(const reef_mtl_shader *sh, int index);
int64_t reef_mtl_shader_varying_count(const reef_mtl_shader *sh);
reef_mtl_varying *reef_mtl_shader_varying(const reef_mtl_shader *sh, int index);
int64_t reef_mtl_shader_render_target_count(const reef_mtl_shader *sh);
void *reef_mtl_shader_render_target(const reef_mtl_shader *sh, int index);
uint32_t reef_mtl_shader_clip_distance_count(const reef_mtl_shader *sh);
uint32_t reef_mtl_shader_uniform_location(const reef_mtl_shader *sh, uint32_t index);
void reef_mtl_shader_threadgroup_size(const reef_mtl_shader *sh,
                                      uint32_t *x, uint32_t *y, uint32_t *z);
void reef_mtl_shader_release(reef_mtl_shader *sh);

#ifdef __cplusplus
}
#endif

#endif
