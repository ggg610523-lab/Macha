#define _GNU_SOURCE
#include "engine.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Values                                                              */
/* ------------------------------------------------------------------ */

sw_value sw_value_none(void)
{
    sw_value value;
    memset(&value, 0, sizeof(value));
    value.kind = SW_VALUE_NONE;
    return value;
}

sw_value sw_value_double(double number)
{
    sw_value value = sw_value_none();
    value.kind = SW_VALUE_DOUBLE;
    value.as.number = number;
    return value;
}

sw_value sw_value_int(int integer)
{
    sw_value value = sw_value_none();
    value.kind = SW_VALUE_INT;
    value.as.integer = integer;
    return value;
}

sw_value sw_value_bool(int boolean)
{
    sw_value value = sw_value_none();
    value.kind = SW_VALUE_BOOL;
    value.as.boolean = boolean;
    return value;
}

sw_value sw_value_string(const char *string)
{
    sw_value value = sw_value_none();
    value.kind = SW_VALUE_STRING;
    value.as.string = string;
    return value;
}

sw_value sw_value_color(sw_color color)
{
    sw_value value = sw_value_none();
    value.kind = SW_VALUE_COLOR;
    value.as.color = color;
    return value;
}

sw_value sw_value_font(sw_font font)
{
    sw_value value = sw_value_none();
    value.kind = SW_VALUE_FONT;
    value.as.font = font;
    return value;
}

double sw_value_as_double(sw_value value, double fallback)
{
    switch (value.kind) {
    case SW_VALUE_DOUBLE:
        return value.as.number;
    case SW_VALUE_INT:
        return (double)value.as.integer;
    case SW_VALUE_BOOL:
        return (double)value.as.boolean;
    default:
        return fallback;
    }
}

int sw_value_as_int(sw_value value, int fallback)
{
    switch (value.kind) {
    case SW_VALUE_INT:
        return value.as.integer;
    case SW_VALUE_BOOL:
        return value.as.boolean;
    case SW_VALUE_DOUBLE:
        return (int)value.as.number;
    default:
        return fallback;
    }
}

int sw_value_as_bool(sw_value value, int fallback)
{
    switch (value.kind) {
    case SW_VALUE_BOOL:
        return value.as.boolean != 0;
    case SW_VALUE_INT:
        return value.as.integer != 0;
    case SW_VALUE_DOUBLE:
        return value.as.number != 0.0;
    default:
        return fallback;
    }
}

const char *sw_value_as_string(sw_value value, const char *fallback)
{
    if (value.kind == SW_VALUE_STRING && value.as.string)
        return value.as.string;
    return fallback;
}

sw_color sw_value_as_color(sw_value value, sw_color fallback)
{
    return value.kind == SW_VALUE_COLOR ? value.as.color : fallback;
}

sw_font sw_value_as_font(sw_value value, sw_font fallback)
{
    return value.kind == SW_VALUE_FONT ? value.as.font : fallback;
}

/* ------------------------------------------------------------------ */
/* Environment                                                         */
/* ------------------------------------------------------------------ */

struct sw_environment {
    struct {
        char *key;
        sw_value value;
    } *entries;
    size_t count;
    size_t capacity;
};

sw_environment *sw_environment_new(void)
{
    return calloc(1, sizeof(sw_environment));
}

void sw_environment_free(sw_environment *environment)
{
    if (!environment)
        return;
    for (size_t i = 0; i < environment->count; i++)
        free(environment->entries[i].key);
    free(environment->entries);
    free(environment);
}

void sw_environment_set(sw_environment *environment, const char *key,
                        sw_value value)
{
    if (!environment || !key)
        return;

    for (size_t i = 0; i < environment->count; i++) {
        if (strcmp(environment->entries[i].key, key) == 0) {
            environment->entries[i].value = value;
            return;
        }
    }

    if (environment->count == environment->capacity) {
        size_t capacity = environment->capacity ? environment->capacity * 2 : 8;
        void *entries = realloc(environment->entries,
                                capacity * sizeof(*environment->entries));
        if (!entries)
            return;
        environment->entries = entries;
        environment->capacity = capacity;
    }

    environment->entries[environment->count].key = strdup(key);
    if (!environment->entries[environment->count].key)
        return;
    environment->entries[environment->count].value = value;
    environment->count++;
}

int sw_environment_has(const sw_environment *environment, const char *key)
{
    if (!environment || !key)
        return 0;
    for (size_t i = 0; i < environment->count; i++) {
        if (strcmp(environment->entries[i].key, key) == 0)
            return 1;
    }
    return 0;
}

sw_value sw_environment_value(const sw_environment *environment,
                              const char *key)
{
    if (!environment || !key)
        return sw_value_none();
    for (size_t i = 0; i < environment->count; i++) {
        if (strcmp(environment->entries[i].key, key) == 0)
            return environment->entries[i].value;
    }
    return sw_value_none();
}

sw_value sw_environment_resolve(const sw_environment *root,
                                const struct sw_node *node, const char *key)
{
    if (!key)
        return sw_value_none();

    for (const struct sw_node *current = node; current;
         current = current->parent) {
        if (current->environment &&
            sw_environment_has(current->environment, key))
            return sw_environment_value(current->environment, key);
    }
    if (root && sw_environment_has(root, key))
        return sw_environment_value(root, key);
    return sw_value_none();
}

/* ------------------------------------------------------------------ */
/* Binding                                                             */
/* ------------------------------------------------------------------ */

struct sw_binding {
    sw_binding_getter get;
    sw_binding_setter set;
    void *context;
    void (*destroy)(void *context);
    sw_value constant;
    int is_constant;
};

sw_binding *sw_binding_new(sw_binding_getter get, sw_binding_setter set,
                           void *context)
{
    if (!get)
        return NULL;
    sw_binding *binding = calloc(1, sizeof(*binding));
    if (!binding)
        return NULL;
    binding->get = get;
    binding->set = set;
    binding->context = context;
    return binding;
}

sw_binding *sw_binding_constant(sw_value value)
{
    sw_binding *binding = calloc(1, sizeof(*binding));
    if (!binding)
        return NULL;
    binding->is_constant = 1;
    binding->constant = value;
    return binding;
}

sw_value sw_binding_get(const sw_binding *binding)
{
    if (!binding)
        return sw_value_none();
    if (binding->is_constant)
        return binding->constant;
    return binding->get ? binding->get(binding->context) : sw_value_none();
}

void sw_binding_set(sw_binding *binding, sw_value value)
{
    if (!binding || binding->is_constant)
        return;
    if (binding->set)
        binding->set(binding->context, value);
}

void sw_binding_free(sw_binding *binding)
{
    if (!binding)
        return;
    if (binding->destroy)
        binding->destroy(binding->context);
    free(binding);
}

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static unsigned long g_generation;

struct sw_state {
    sw_value value;
    unsigned revision;
};

sw_state *sw_state_new(sw_value initial)
{
    sw_state *state = calloc(1, sizeof(*state));
    if (!state)
        return NULL;
    state->value = initial;
    return state;
}

void sw_state_free(sw_state *state)
{
    free(state);
}

sw_value sw_state_value(const sw_state *state)
{
    return state ? state->value : sw_value_none();
}

void sw_state_set(sw_state *state, sw_value value)
{
    if (!state)
        return;
    state->value = value;
    state->revision++;
    g_generation++;
}

unsigned sw_state_revision(const sw_state *state)
{
    return state ? state->revision : 0;
}

static sw_value state_get(void *context)
{
    return ((const sw_state *)context)->value;
}

static void state_set(void *context, sw_value value)
{
    sw_state_set((sw_state *)context, value);
}

sw_binding *sw_state_binding(sw_state *state)
{
    if (!state)
        return NULL;
    return sw_binding_new(state_get, state_set, state);
}

unsigned long sw_state_generation(void)
{
    return g_generation;
}

/* ------------------------------------------------------------------ */
/* Observable                                                          */
/* ------------------------------------------------------------------ */

struct sw_observable {
    struct {
        char *key;
        sw_value value;
    } *entries;
    size_t count;
    size_t capacity;
    struct {
        sw_observer_fn observer;
        void *context;
    } *watchers;
    size_t watcher_count;
    size_t watcher_capacity;
    unsigned long generation;
};

sw_observable *sw_observable_new(void)
{
    return calloc(1, sizeof(sw_observable));
}

void sw_observable_free(sw_observable *observable)
{
    if (!observable)
        return;
    for (size_t i = 0; i < observable->count; i++)
        free(observable->entries[i].key);
    free(observable->entries);
    free(observable->watchers);
    free(observable);
}

sw_value sw_observable_get(const sw_observable *observable, const char *key)
{
    if (!observable || !key)
        return sw_value_none();
    for (size_t i = 0; i < observable->count; i++) {
        if (strcmp(observable->entries[i].key, key) == 0)
            return observable->entries[i].value;
    }
    return sw_value_none();
}

void sw_observable_set(sw_observable *observable, const char *key,
                       sw_value value)
{
    if (!observable || !key)
        return;

    size_t slot = observable->count;
    for (size_t i = 0; i < observable->count; i++) {
        if (strcmp(observable->entries[i].key, key) == 0) {
            slot = i;
            break;
        }
    }

    if (slot == observable->count) {
        if (observable->count == observable->capacity) {
            size_t capacity =
                observable->capacity ? observable->capacity * 2 : 8;
            void *entries = realloc(observable->entries,
                                    capacity * sizeof(*observable->entries));
            if (!entries)
                return;
            observable->entries = entries;
            observable->capacity = capacity;
        }
        observable->entries[slot].key = strdup(key);
        if (!observable->entries[slot].key)
            return;
        observable->count++;
    }

    observable->entries[slot].value = value;
    observable->generation++;
    g_generation++;

    for (size_t i = 0; i < observable->watcher_count; i++) {
        if (observable->watchers[i].observer)
            observable->watchers[i].observer(observable->watchers[i].context,
                                             key, value);
    }
}

typedef struct {
    sw_observable *observable;
    char *key;
} observable_context;

static sw_value observable_get(void *context)
{
    const observable_context *ctx = context;
    return sw_observable_get(ctx->observable, ctx->key);
}

static void observable_set(void *context, sw_value value)
{
    const observable_context *ctx = context;
    sw_observable_set(ctx->observable, ctx->key, value);
}

static void observable_context_free(void *context)
{
    observable_context *ctx = context;
    if (!ctx)
        return;
    free(ctx->key);
    free(ctx);
}

sw_binding *sw_observable_binding(sw_observable *observable, const char *key)
{
    if (!observable || !key)
        return NULL;
    observable_context *context = calloc(1, sizeof(*context));
    if (!context)
        return NULL;
    context->observable = observable;
    context->key = strdup(key);
    if (!context->key) {
        free(context);
        return NULL;
    }
    sw_binding *binding = sw_binding_new(observable_get, observable_set,
                                         context);
    if (!binding) {
        observable_context_free(context);
        return NULL;
    }
    binding->destroy = observable_context_free;
    return binding;
}

void sw_observable_watch(sw_observable *observable, sw_observer_fn observer,
                         void *context)
{
    if (!observable || !observer)
        return;
    if (observable->watcher_count == observable->watcher_capacity) {
        size_t capacity = observable->watcher_capacity
                              ? observable->watcher_capacity * 2
                              : 4;
        void *watchers = realloc(observable->watchers,
                                 capacity * sizeof(*observable->watchers));
        if (!watchers)
            return;
        observable->watchers = watchers;
        observable->watcher_capacity = capacity;
    }
    observable->watchers[observable->watcher_count].observer = observer;
    observable->watchers[observable->watcher_count].context = context;
    observable->watcher_count++;
}

unsigned long sw_observable_generation(const sw_observable *observable)
{
    return observable ? observable->generation : 0;
}

/* ------------------------------------------------------------------ */
/* Node-local state (@State) and environment modifiers                 */
/* ------------------------------------------------------------------ */

struct sw_node_state {
    char *key;
    sw_state *state;
};

sw_state *sw_node_state(sw_node *node, const char *key, sw_value initial)
{
    if (!node || !key)
        return NULL;

    for (size_t i = 0; i < node->state_count; i++) {
        if (strcmp(node->states[i].key, key) == 0)
            return node->states[i].state;
    }

    void *states = realloc(node->states,
                           (node->state_count + 1) * sizeof(*node->states));
    if (!states)
        return NULL;
    node->states = states;

    sw_state *state = sw_state_new(initial);
    char *name = strdup(key);
    if (!state || !name) {
        sw_state_free(state);
        free(name);
        return NULL;
    }

    node->states[node->state_count].key = name;
    node->states[node->state_count].state = state;
    node->state_count++;
    return state;
}

void sw_node_states_free(sw_node *node)
{
    if (!node)
        return;
    for (size_t i = 0; i < node->state_count; i++) {
        free(node->states[i].key);
        sw_state_free(node->states[i].state);
    }
    free(node->states);
    node->states = NULL;
    node->state_count = 0;
}

sw_node *sw_environment_apply(sw_node *node, const char *key, sw_value value)
{
    if (!node || !key)
        return node;
    if (!node->environment)
        node->environment = sw_environment_new();
    if (node->environment)
        sw_environment_set(node->environment, key, value);
    return node;
}

sw_node *sw_environment_color(sw_node *node, const char *key, sw_color color)
{
    return sw_environment_apply(node, key, sw_value_color(color));
}

sw_node *sw_environment_font(sw_node *node, sw_font font)
{
    return sw_environment_apply(node, SW_ENV_FONT, sw_value_font(font));
}

sw_node *sw_environment_int(sw_node *node, const char *key, int integer)
{
    return sw_environment_apply(node, key, sw_value_int(integer));
}

sw_node *sw_environment_bool(sw_node *node, const char *key, int boolean)
{
    return sw_environment_apply(node, key, sw_value_bool(boolean));
}

sw_node *sw_environment_string(sw_node *node, const char *key,
                               const char *string)
{
    return sw_environment_apply(node, key, sw_value_string(string));
}

sw_node *sw_environment_dark(sw_node *node)
{
    return sw_environment_int(node, SW_ENV_COLOR_SCHEME, 1);
}
