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
typedef uint32_t MMTk_AllocationSemantics;


#define MMTK_OBJREF_OFFSET 8

#define MMTK_MIN_OBJ_ALIGN 8

#define MMTK_GC_THREAD_KIND_CONTROLLER 0

#define MMTK_GC_THREAD_KIND_WORKER 1

typedef MMTk_ObjectReference (*ObjectClosureFunction)(void*, void*, MMTk_ObjectReference);

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
    void (*stop_the_world)(MMTk_VMWorkerThread tls);
    void (*resume_mutators)(MMTk_VMWorkerThread tls);
    void (*block_for_gc)(MMTk_VMMutatorThread tls);
    size_t (*number_of_mutators)(void);
    void (*reset_mutator_iterator)(void);
    MMTk_Mutator *(*get_next_mutator)(void);
    void (*scan_vm_specific_roots)(void);
    void (*scan_thread_roots)(void);
    void (*scan_thread_root)(MMTk_VMMutatorThread mutator_tls, MMTk_VMWorkerThread worker_tls);
    void (*scan_object_ruby_style)(MMTk_ObjectReference object);
    void (*call_obj_free)(MMTk_ObjectReference object);
    void (*update_global_weak_tables)(void);
} MMTk_RubyUpcalls;

typedef struct MMTk_RawVecOfObjRef {
    MMTk_ObjectReference *ptr;
    size_t len;
    size_t capa;
} MMTk_RawVecOfObjRef;

/**
 * Create an MMTKBuilder instance with default options.
 * This instance shall be consumed by `mmtk_init_binding`.
 */
MMTk_Builder *mmtk_builder_default(void);

/**
 * Set the `heap_size` option.
 */
void mmtk_builder_set_heap_size(MMTk_Builder *builder, size_t heap_size);

/**
 * Set the plan.  `plan_name` is a case-sensitive C-style ('\0'-terminated) string matching
 * one of the cases of `enum PlanSelector`.
 */
void mmtk_builder_set_plan(MMTk_Builder *builder, const char *plan_name);

/**
 * Build an MMTk instance.
 *
 * -   `builder` is the pointer to the `MMTKBuilder` instance created by the
 *     `mmtk_builder_default()` function, and the `MMTKBuilder` will be consumed after building
 *     the MMTk instance.
 * -   `upcalls` points to the struct that contains upcalls.  It is allocated in C as static.
 */
void mmtk_init_binding(MMTk_Builder *builder, const struct MMTk_RubyUpcalls *upcalls);

MMTk_Mutator *mmtk_bind_mutator(MMTk_VMMutatorThread tls);

void mmtk_destroy_mutator(MMTk_Mutator *mutator);

MMTk_Address mmtk_alloc(MMTk_Mutator *mutator,
                        size_t size,
                        size_t align,
                        ptrdiff_t offset,
                        MMTk_AllocationSemantics semantics);

void mmtk_post_alloc(MMTk_Mutator *mutator,
                     MMTk_ObjectReference refer,
                     size_t bytes,
                     MMTk_AllocationSemantics semantics);

bool mmtk_will_never_move(MMTk_ObjectReference object);

void mmtk_initialize_collection(MMTk_VMThread tls);

void mmtk_enable_collection(void);

const char *mmtk_plan_name(void);

size_t mmtk_used_bytes(void);

size_t mmtk_free_bytes(void);

size_t mmtk_total_bytes(void);

bool mmtk_is_reachable(MMTk_ObjectReference object);

bool mmtk_is_live_object(MMTk_ObjectReference object);

bool mmtk_is_mmtk_object(MMTk_Address addr);

void mmtk_modify_check(MMTk_ObjectReference object);

void mmtk_handle_user_collection_request(MMTk_VMMutatorThread tls);

void mmtk_harness_begin(MMTk_VMMutatorThread tls);

void mmtk_harness_end(MMTk_VMMutatorThread _tls);

MMTk_Address mmtk_starting_heap_address(void);

MMTk_Address mmtk_last_heap_address(void);

void mmtk_add_obj_free_candidate(MMTk_ObjectReference object);

struct MMTk_RawVecOfObjRef mmtk_get_all_obj_free_candidates(void);

void mmtk_free_raw_vec_of_obj_ref(struct MMTk_RawVecOfObjRef raw_vec);

#endif /* MMTK_H */
