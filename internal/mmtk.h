#ifndef MMTK_H
#define MMTK_H

/* Warning, this file is autogenerated by cbindgen from the mmtk-ruby repository. Don't modify this manually. */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct MMTk_Builder MMTk_Builder;
typedef struct MMTk_Mutator MMTk_Mutator;

struct rb_thread_struct;
typedef struct rb_thread_struct rb_thread_t;
typedef rb_thread_t* MMTk_VMThread;
typedef rb_thread_t* MMTk_VMMutatorThread;
typedef struct MMTk_GCThreadTLS* MMTk_VMWorkerThread;
typedef void* MMTk_Address;
typedef void* MMTk_ObjectReference;
typedef void* MMTk_NullableObjectReference;
typedef uint32_t MMTk_AllocationSemantics;


#define MMTK_OBJREF_OFFSET 8

#define MMTK_MIN_OBJ_ALIGN 8

#define MMTK_GC_THREAD_KIND_WORKER 1

#define MMTK_HAS_MOVED_GIVTBL 9223372036854775808ull

#define MMTK_HIDDEN_SIZE_MASK 281474976710655

typedef struct st_table st_table;

typedef struct RubyBindingOptions {
    bool ractor_check_mode;
    size_t suffix_size;
} RubyBindingOptions;

typedef MMTk_ObjectReference (*ObjectClosureFunction)(void*, void*, MMTk_ObjectReference, bool);

typedef struct MMTk_ObjectClosure {
    /**
     * The function to be called from C.
     */
    ObjectClosureFunction c_function;
    /**
     * The pointer to the Rust-level closure object.
     */
    void *rust_closure;
} MMTk_ObjectClosure;

typedef struct MMTk_GCThreadTLS {
    int kind;
    void *gc_context;
    struct MMTk_ObjectClosure object_closure;
} MMTk_GCThreadTLS;

typedef struct MMTk_RubyUpcalls {
    void (*init_gc_worker_thread)(struct MMTk_GCThreadTLS *gc_worker_tls);
    struct MMTk_GCThreadTLS *(*get_gc_thread_tls)(void);
    bool (*is_mutator)(void);
    void (*stop_the_world)(MMTk_VMWorkerThread tls);
    void (*resume_mutators)(MMTk_VMWorkerThread tls);
    void (*block_for_gc)(MMTk_VMMutatorThread tls);
    size_t (*number_of_mutators)(void);
    void (*get_mutators)(void (*visit_mutator)(MMTk_Mutator*, void*), void *data);
    void (*scan_vm_roots)(void);
    void (*scan_end_proc_roots)(void);
    void (*scan_global_tbl_roots)(void);
    void (*scan_yjit_roots)(void);
    void (*scan_finalizer_tbl_roots)(void);
    void (*scan_obj_to_id_tbl_roots)(void);
    void (*scan_misc_roots)(void);
    void (*scan_final_jobs_roots)(void);
    void (*scan_roots_in_mutator_thread)(MMTk_VMMutatorThread mutator_tls,
                                         MMTk_VMWorkerThread worker_tls);
    bool (*is_no_longer_ppp)(MMTk_ObjectReference);
    void (*scan_object_ruby_style)(MMTk_ObjectReference object);
    void (*call_gc_mark_children)(MMTk_ObjectReference object);
    void (*call_obj_free)(MMTk_ObjectReference object);
    void (*cleanup_generic_iv_tbl)(void);
    void *(*get_original_givtbl)(MMTk_ObjectReference object);
    void (*move_givtbl)(MMTk_ObjectReference old_objref, MMTk_ObjectReference new_objref);
    size_t (*vm_live_bytes)(void);
    void (*update_frozen_strings_table)(void);
    void (*update_finalizer_and_obj_id_tables)(void);
    void (*update_global_symbols_table)(void);
    void (*update_overloaded_cme_table)(void);
    void (*update_ci_table)(void);
    struct st_table *(*get_frozen_strings_table)(void);
    struct st_table *(*get_finalizer_table)(void);
    struct st_table *(*get_obj_to_id_table)(void);
    struct st_table *(*get_id_to_obj_table)(void);
    struct st_table *(*get_global_symbols_table)(void);
    struct st_table *(*get_overloaded_cme_table)(void);
    struct st_table *(*get_ci_table)(void);
    size_t (*st_get_num_entries)(const struct st_table *table);
    void (*st_get_size_info)(const struct st_table *table,
                             size_t *entries_start,
                             size_t *entries_bound,
                             size_t *bins_num);
    void (*st_update_entries_range)(struct st_table *table,
                                    size_t begin,
                                    size_t end,
                                    bool weak_keys,
                                    bool weak_records,
                                    bool forward);
    void (*st_update_bins_range)(struct st_table *table, size_t begin, size_t end);
} MMTk_RubyUpcalls;

typedef struct MMTk_RawVecOfObjRef {
    MMTk_ObjectReference *ptr;
    size_t len;
    size_t capa;
} MMTk_RawVecOfObjRef;

typedef struct MMTk_HiddenHeader {
    size_t prefix;
} MMTk_HiddenHeader;

/**
 * Create an MMTKBuilder instance with default options.
 * This instance shall be consumed by `mmtk_init_binding`.
 */
MMTk_Builder *mmtk_builder_default(void);

/**
 * Let the MMTKBuilder read options from environment variables,
 * such as `MMTK_THREADS`.
 */
void mmtk_builder_read_env_var_settings(MMTk_Builder *builder);

/**
 * Set the GC trigger to dynamically adjust heap size.
 */
void mmtk_builder_set_dynamic_heap_size(MMTk_Builder *builder, size_t low, size_t high);

/**
 * Set the GC trigger to use a fixed heap size.
 */
void mmtk_builder_set_fixed_heap_size(MMTk_Builder *builder, size_t heap_size);

/**
 * Set the plan.  `plan_name` is a case-sensitive C-style ('\0'-terminated) string matching
 * one of the cases of `enum PlanSelector`.
 */
void mmtk_builder_set_plan(MMTk_Builder *builder, const char *plan_name);

/**
 * Query if the selected plan is MarkSweep.
 */
bool mmtk_builder_is_mark_sweep(MMTk_Builder *builder);

/**
 * Query if the selected plan is Immix.
 */
bool mmtk_builder_is_immix(MMTk_Builder *builder);

/**
 * Query if the selected plan is StickyImmix.
 */
bool mmtk_builder_is_sticky_immix(MMTk_Builder *builder);

/**
 * Build an MMTk instance.
 *
 * -   `builder` is the pointer to the `MMTKBuilder` instance created by the
 *     `mmtk_builder_default()` function, and the `MMTKBuilder` will be consumed after building
 *     the MMTk instance.
 * -   `upcalls` points to the struct that contains upcalls.  It is allocated in C as static.
 */
void mmtk_init_binding(MMTk_Builder *builder,
                       const struct RubyBindingOptions *binding_options,
                       const struct MMTk_RubyUpcalls *upcalls);

MMTk_Mutator *mmtk_bind_mutator(MMTk_VMMutatorThread tls);

void mmtk_destroy_mutator(MMTk_Mutator *mutator);

MMTk_Address mmtk_alloc(MMTk_Mutator *mutator,
                        size_t size,
                        size_t align,
                        size_t offset,
                        MMTk_AllocationSemantics semantics);

void mmtk_post_alloc(MMTk_Mutator *mutator,
                     MMTk_ObjectReference refer,
                     size_t bytes,
                     MMTk_AllocationSemantics semantics);

bool mmtk_will_never_move(MMTk_ObjectReference object);

void mmtk_initialize_collection(MMTk_VMThread tls);

void mmtk_prepare_to_fork(void);

void mmtk_after_fork(MMTk_VMThread tls);

void mmtk_enable_collection(void);

void mmtk_disable_collection(void);

const char *mmtk_plan_name(void);

size_t mmtk_used_bytes(void);

size_t mmtk_free_bytes(void);

size_t mmtk_total_bytes(void);

bool mmtk_is_reachable(MMTk_ObjectReference object);

bool mmtk_is_live_object(MMTk_ObjectReference object);

MMTk_NullableObjectReference mmtk_get_forwarded_object(MMTk_ObjectReference object);

bool mmtk_is_mmtk_object(MMTk_Address addr);

void mmtk_handle_user_collection_request(MMTk_VMMutatorThread tls);

void mmtk_harness_begin(MMTk_VMMutatorThread tls);

void mmtk_harness_end(MMTk_VMMutatorThread _tls);

MMTk_Address mmtk_starting_heap_address(void);

MMTk_Address mmtk_last_heap_address(void);

void mmtk_add_obj_free_candidate(MMTk_ObjectReference object);

void mmtk_add_obj_free_candidates(const MMTk_ObjectReference *objects, size_t len);

struct MMTk_RawVecOfObjRef mmtk_get_all_obj_free_candidates(void);

void mmtk_free_raw_vec_of_obj_ref(struct MMTk_RawVecOfObjRef raw_vec);

void mmtk_register_ppp(MMTk_ObjectReference object);

void mmtk_register_ppps(const MMTk_ObjectReference *objects, size_t len);

void *mmtk_get_givtbl_during_gc(MMTk_ObjectReference object);

size_t mmtk_get_vo_bit_log_region_size(void);

size_t mmtk_get_vo_bit_base(void);

void mmtk_gc_poll(MMTk_VMMutatorThread tls);

size_t mmtk_get_immix_bump_ptr_offset(void);

bool mmtk_pin_object(MMTk_ObjectReference object);

bool mmtk_unpin_object(MMTk_ObjectReference object);

bool mmtk_is_pinned(MMTk_ObjectReference object);

void mmtk_register_wb_unprotected_object(MMTk_ObjectReference object);

bool mmtk_is_object_wb_unprotected(MMTk_ObjectReference object);

void mmtk_object_reference_write_post(MMTk_Mutator *mutator, MMTk_ObjectReference object);

/**
 * Enumerate objects.  This function will call `callback(object, data)` for each object. It has
 * undefined behavior if allocation or GC happens while this function is running.
 */
void mmtk_enumerate_objects(void (*callback)(MMTk_ObjectReference, void*), void *data);

#endif /* MMTK_H */
