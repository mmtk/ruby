// clang -I.. -L mmtk/target/debug -lmmtk_ruby -undefined dynamic_lookup -g -O3 -dynamiclib -o ../build/libgc.mmtk.dylib mmtk.c

#include <stdbool.h>

#include "gc/gc.h"
#include "gc/gc_impl.h"
#include "gc/mmtk.h"

bool
rb_mmtk_is_mutator(void)
{
    return ruby_native_thread_p();
}

static size_t
rb_mmtk_vm_live_bytes(void)
{
    return 0;
}

// Bootup
MMTk_RubyUpcalls ruby_upcalls = {
    NULL,
    NULL,
    rb_mmtk_is_mutator,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    rb_mmtk_vm_live_bytes,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
};

void *
rb_gc_impl_objspace_alloc(void)
{
    MMTk_Builder *builder = mmtk_builder_default();
    mmtk_init_binding(builder, NULL, &ruby_upcalls);

    return NULL;
}

static st_table *obj_id_to_obj_table_alloc(void);

void rb_gc_impl_objspace_init(void *objspace_ptr) { }

void rb_gc_impl_objspace_free(void *objspace_ptr) { }

void *
rb_gc_impl_ractor_cache_alloc(void *objspace_ptr)
{
    // TODO: pass not NULL to tls
    return mmtk_bind_mutator(NULL);
}

void
rb_gc_impl_ractor_cache_free(void *objspace_ptr, void *cache)
{
    // TODO: implement mmtk_destroy_mutator
}

void rb_gc_impl_set_params(void *objspace_ptr) { }

void rb_gc_impl_init(void) { }

void rb_gc_impl_initial_stress_set(VALUE flag) { }

static size_t size_pool_sizes[6] = {
    40, 80, 160, 320, 640, 0
};

size_t *
rb_gc_impl_size_pool_sizes(void *objspace_ptr)
{
    return size_pool_sizes;
}

// Shutdown
void rb_gc_impl_shutdown_free_objects(void *objspace_ptr) { }

// GC
void
rb_gc_impl_start(void *objspace_ptr, bool full_mark, bool immediate_mark, bool immediate_sweep, bool compact)
{
    // TODO
}

bool
rb_gc_impl_during_gc_p(void *objspace_ptr)
{
    // TODO
    return false;
}

void
rb_gc_impl_prepare_heap(void *objspace_ptr)
{
    // TODO
}

void
rb_gc_impl_gc_enable(void *objspace_ptr)
{
    // TODO
}

void
rb_gc_impl_gc_disable(void *objspace_ptr, bool finish_current_gc)
{
    // TODO
}

bool
rb_gc_impl_gc_enabled_p(void *objspace_ptr)
{
    // TODO
    return true;
}

void
rb_gc_impl_stress_set(void *objspace_ptr, VALUE flag)
{
    // TODO
}

VALUE
rb_gc_impl_stress_get(void *objspace_ptr)
{
    // TODO
    return Qfalse;
}

VALUE
rb_gc_impl_config_get(void *objspace_ptr)
{
    // TODO
    return rb_hash_new();
}
VALUE
rb_gc_impl_config_set(void *objspace_ptr, VALUE hash)
{
    // TODO
    return hash;
}

// Object allocation

VALUE
rb_gc_impl_new_obj(void *objspace_ptr, void *cache_ptr, VALUE klass, VALUE flags, VALUE v1, VALUE v2, VALUE v3, bool wb_protected, size_t alloc_size)
{
#define MMTK_MIN_OBJ_ALIGN 8
#define MMTK_ALLOCATION_SEMANTICS_DEFAULT 0
    if (alloc_size > 640) rb_bug("too big");
    for (int i = 0; i < 5; i++) {
        if (alloc_size == size_pool_sizes[i]) break;
        if (alloc_size < size_pool_sizes[i]) {
            alloc_size = size_pool_sizes[i];
            break;
        }
    }

    VALUE *alloc_obj = mmtk_alloc(cache_ptr, alloc_size + 8, MMTK_MIN_OBJ_ALIGN, 0, MMTK_ALLOCATION_SEMANTICS_DEFAULT);
    alloc_obj++;
    alloc_obj[-1] = alloc_size;
    alloc_obj[0] = flags;
    alloc_obj[1] = klass;
    if (alloc_size > 16) alloc_obj[2] = v1;
    if (alloc_size > 24) alloc_obj[3] = v2;
    if (alloc_size > 32) alloc_obj[4] = v3;


    if (rb_gc_shutdown_call_finalizer_p((VALUE)alloc_obj)) {
        mmtk_add_obj_free_candidate(alloc_obj);
    }

    return (VALUE)alloc_obj;
}

size_t
rb_gc_impl_obj_slot_size(VALUE obj)
{
    return ((VALUE *)obj)[-1];
}

size_t
rb_gc_impl_size_pool_id_for_size(void *objspace_ptr, size_t size)
{
    for (int i = 0; i < 5; i++) {
        if (size == size_pool_sizes[i]) return i;
        if (size < size_pool_sizes[i])  return i;
    }

    rb_bug("size too big");
}

bool
rb_gc_impl_size_allocatable_p(size_t size)
{
    return size <= 640;
}

// Malloc
void *
rb_gc_impl_malloc(void *objspace_ptr, size_t size)
{
    // TODO: don't use system malloc
    return malloc(size);
}

void *
rb_gc_impl_calloc(void *objspace_ptr, size_t size)
{
    // TODO: don't use system calloc
    return calloc(1, size);
}

void *
rb_gc_impl_realloc(void *objspace_ptr, void *ptr, size_t new_size, size_t old_size)
{
    // TODO: don't use system realloc
    return realloc(ptr, new_size);
}

void
rb_gc_impl_free(void *objspace_ptr, void *ptr, size_t old_size)
{
    // TODO: don't use system free
    free(ptr);
}

void rb_gc_impl_adjust_memory_usage(void *objspace_ptr, ssize_t diff) { }
// Marking
void
rb_gc_impl_mark(void *objspace_ptr, VALUE obj)
{
    rb_bug("unimplemented");
}

void
rb_gc_impl_mark_and_move(void *objspace_ptr, VALUE *ptr)
{
    rb_bug("unimplemented");
}

void
rb_gc_impl_mark_and_pin(void *objspace_ptr, VALUE obj)
{
    rb_bug("unimplemented");
}

void
rb_gc_impl_mark_maybe(void *objspace_ptr, VALUE obj) {
    rb_bug("unimplemented");
}

void
rb_gc_impl_mark_weak(void *objspace_ptr, VALUE *ptr) {
    rb_bug("unimplemented");
}

void
rb_gc_impl_remove_weak(void *objspace_ptr, VALUE parent_obj, VALUE *ptr)
{
    rb_bug("unimplemented");
}

void
rb_gc_impl_objspace_mark(void *objspace_ptr)
{
    rb_bug("unimplemented");
}

// Compaction
bool
rb_gc_impl_object_moved_p(void *objspace_ptr, VALUE obj)
{
    rb_bug("unimplemented");
}

VALUE
rb_gc_impl_location(void *objspace_ptr, VALUE value)
{
    rb_bug("unimplemented");
}
// Write barriers
void rb_gc_impl_writebarrier(void *objspace_ptr, VALUE a, VALUE b) { }
void rb_gc_impl_writebarrier_unprotect(void *objspace_ptr, VALUE obj) { }
void rb_gc_impl_writebarrier_remember(void *objspace_ptr, VALUE obj) { }
// Heap walking
void rb_gc_impl_each_objects(void *objspace_ptr, int (*callback)(void *, void *, size_t, void *), void *data) { }
void rb_gc_impl_each_object(void *objspace_ptr, void (*func)(VALUE obj, void *data), void *data) { }
// Finalizers
void
rb_gc_impl_make_zombie(void *objspace_ptr, VALUE obj, void (*dfree)(void *), void *data)
{
    // TODO: real implementation of making zombie
    dfree(data);
}

VALUE rb_gc_impl_define_finalizer(void *objspace_ptr, VALUE obj, VALUE block) { }
VALUE rb_gc_impl_undefine_finalizer(void *objspace_ptr, VALUE obj) { }
void rb_gc_impl_copy_finalizer(void *objspace_ptr, VALUE dest, VALUE obj) { }

void
rb_gc_impl_shutdown_call_finalizer(void *objspace_ptr)
{
    struct MMTk_RawVecOfObjRef registered_candidates = mmtk_get_all_obj_free_candidates();
    for (size_t i = 0; i < registered_candidates.len; i++) {
        VALUE obj = (VALUE)registered_candidates.ptr[i];

        if (rb_gc_shutdown_call_finalizer_p(obj)) {
            rb_gc_obj_free(objspace_ptr, obj);
        }
    }
    mmtk_free_raw_vec_of_obj_ref(registered_candidates);
}

// Object ID
VALUE rb_gc_impl_object_id(void *objspace_ptr, VALUE obj) { }
VALUE rb_gc_impl_object_id_to_ref(void *objspace_ptr, VALUE object_id) { }
// Statistics
VALUE rb_gc_impl_set_measure_total_time(void *objspace_ptr, VALUE flag) { }
VALUE rb_gc_impl_get_measure_total_time(void *objspace_ptr) { }
VALUE rb_gc_impl_get_profile_total_time(void *objspace_ptr) { }
size_t rb_gc_impl_gc_count(void *objspace_ptr) { }
VALUE rb_gc_impl_latest_gc_info(void *objspace_ptr, VALUE key) { }
size_t rb_gc_impl_stat(void *objspace_ptr, VALUE hash_or_sym) { }
size_t rb_gc_impl_stat_heap(void *objspace_ptr, VALUE heap_name, VALUE hash_or_sym) { }
// Miscellaneous
size_t rb_gc_impl_obj_flags(void *objspace_ptr, VALUE obj, ID* flags, size_t max) { }
bool rb_gc_impl_pointer_to_heap_p(void *objspace_ptr, const void *ptr) { }
bool rb_gc_impl_garbage_object_p(void *objspace_ptr, VALUE obj) { }
void rb_gc_impl_set_event_hook(void *objspace_ptr, const rb_event_flag_t event) { }
void rb_gc_impl_copy_attributes(void *objspace_ptr, VALUE dest, VALUE obj) { }
