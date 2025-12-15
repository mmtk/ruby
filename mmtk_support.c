#include "ruby/internal/config.h"

#include "gc/gc.h"
#include "internal.h"
#include "internal/cmdlineopt.h"
#include "internal/concurrent_set.h"
#include "internal/gc.h"
#include "internal/imemo.h"
#include "internal/string.h"
#include "internal/symbol.h"
#include "internal/thread.h"
#include "internal/variable.h"
#include "ruby/ruby.h"
#include "ractor_core.h"
#include "symbol.h"
#include "vm_core.h"
#include "ruby/st.h"
#ifndef _WIN32
#include "stdatomic.h"
#endif

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#ifdef __GNUC__
#define PREFETCH(addr, write_p) __builtin_prefetch(addr, write_p)
#define EXPECT(expr, val) __builtin_expect(expr, val)
#define ATTRIBUTE_UNUSED  __attribute__((unused))
#else
#define PREFETCH(addr, write_p)
#define EXPECT(expr, val) (expr)
#define ATTRIBUTE_UNUSED
#endif

////////////////////////////////////////////////////////////////////////////////
// Workaround: Declare some data types defined elsewhere.
////////////////////////////////////////////////////////////////////////////////

// rb_objspace_t from gc.c
typedef struct rb_objspace rb_objspace_t;
#define rb_objspace (*rb_objspace_of(GET_VM()))
#define rb_objspace_of(vm) ((vm)->objspace)
// From ractor.c.  gc.c also declared this function locally.
bool rb_obj_is_main_ractor(VALUE gv);

#if USE_MMTK
#include "internal/mmtk_support.h"
#include "internal/mmtk.h"
////////////////////////////////////////////////////////////////////////////////
// Mirror some data structures from mmtk-core.
// TODO: We are having problem generating the BumpPointer struct from mmtk-core.
// It should be generated automatically using cbindgen.
////////////////////////////////////////////////////////////////////////////////

struct BumpPointer {
    uintptr_t cursor;
    uintptr_t limit;
};

////////////////////////////////////////////////////////////////////////////////
// Command line arguments
////////////////////////////////////////////////////////////////////////////////

const char *rb_mmtk_pre_arg_plan = NULL;
const char *rb_mmtk_post_arg_plan = NULL;
const char *rb_mmtk_chosen_plan = NULL;
bool rb_mmtk_plan_is_immix = false;
bool rb_mmtk_plan_uses_bump_pointer = false;
bool rb_mmtk_plan_implicitly_pinning = false;
bool rb_mmtk_use_barrier = false;

size_t rb_mmtk_pre_max_heap_size = 0;
size_t rb_mmtk_post_max_heap_size = 0;

bool rb_mmtk_max_heap_parse_error = false;
size_t rb_mmtk_max_heap_size = 0;

// Use up to 80% of memory for the heap
static const int rb_mmtk_heap_limit_percentage = 80;

////////////////////////////////////////////////////////////////////////////////
// Global and thread-local states.
////////////////////////////////////////////////////////////////////////////////

static bool mmtk_enable = false;

RubyBindingOptions ruby_binding_options;
MMTk_RubyUpcalls ruby_upcalls;

// TODO: Generate them as constants.
static uintptr_t mmtk_vo_bit_log_region_size;
static uintptr_t mmtk_vo_bit_base_addr;

bool rb_mmtk_obj_free_on_exit_started = false;


// DEBUG: Vanilla GC timing
static struct gc_timing {
    bool enabled;
    bool in_alloc_slow_path;
    uint64_t gc_time_ns;
    struct timespec last_enabled;
    struct timespec last_gc_start;
    uint64_t last_num_of_gc;
    uint64_t last_vanilla_mark;
    uint64_t last_vanilla_sweep;
} rb_mmtk_vanilla_timing;

// xmalloc accounting
struct rb_mmtk_xmalloc_accounting{
    size_t malloc_total;
} rb_mmtk_xmalloc_accounting_t;

struct RubyMMTKGlobal {
    pthread_mutex_t mutex;
    pthread_cond_t cond_world_stopped;
    pthread_cond_t cond_world_started;
    rb_atomic_t mutator_blocking_count;
    unsigned int fork_hook_vm_lock_lev;
    bool world_stopped;
    size_t start_the_world_count;
} rb_mmtk_global = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond_world_stopped = PTHREAD_COND_INITIALIZER,
    .cond_world_started = PTHREAD_COND_INITIALIZER,
    .mutator_blocking_count = 0,
    .fork_hook_vm_lock_lev = 0,
    .world_stopped = false,
    .start_the_world_count = 0,
};

struct rb_mmtk_address_buffer {
    void **slots;
    size_t len;
    size_t capa;
};

#ifdef RB_THREAD_LOCAL_SPECIFIER
RB_THREAD_LOCAL_SPECIFIER struct MMTk_GCThreadTLS *rb_mmtk_gc_thread_tls;
#else // RB_THREAD_LOCAL_SPECIFIER
#error We currently need language-supported TLS
#endif // RB_THREAD_LOCAL_SPECIFIER


////////////////////////////////////////////////////////////////////////////////
// Helper functions
////////////////////////////////////////////////////////////////////////////////

static void
rb_mmtk_use_mmtk_global(void (*func)(void *), void* arg)
{
    int err;
    if ((err = pthread_mutex_lock(&rb_mmtk_global.mutex)) != 0) {
        fprintf(stderr, "ERROR: cannot lock rb_mmtk_global.mutex: %s", strerror(err));
        abort();
    }

    func(arg);

    if ((err = pthread_mutex_unlock(&rb_mmtk_global.mutex)) != 0) {
        fprintf(stderr, "ERROR: cannot release rb_mmtk_global.mutex: %s", strerror(err));
        abort();
    }
}

// Helper functions for rb_mmtk_values_buffer
static bool
rb_mmtk_values_buffer_append(struct rb_mmtk_values_buffer *buffer, VALUE obj)
{
    RUBY_ASSERT(buffer != NULL);
    buffer->objects[buffer->len] = obj;
    buffer->len++;

    return buffer->len == RB_MMTK_VALUES_BUFFER_SIZE;
}

static void
rb_mmtk_values_buffer_clear(struct rb_mmtk_values_buffer *buffer)
{
    buffer->len = 0;

    // Just to be safe.
    memset(buffer->objects, 0, sizeof(buffer->objects));
}

struct rb_mmtk_mutator_local*
rb_mmtk_get_mutator_local()
{
    return rb_mmtk_ractor_get_mutator_local(GET_RACTOR());
}

struct rb_mmtk_mutator_local*
rb_mmtk_ractor_get_mutator_local(rb_ractor_t *ractor)
{
    return rb_mmtk_ractor_cache_get_mutator_local(ractor->newobj_cache);
}

////////////////////////////////////////////////////////////////////////////////
// Query for enabled/disabled.
////////////////////////////////////////////////////////////////////////////////

bool
rb_mmtk_enabled_p(void)
{
    return mmtk_enable;
}

////////////////////////////////////////////////////////////////////////////////
// MMTk binding initialization
////////////////////////////////////////////////////////////////////////////////

void
rb_mmtk_bind_mutator(rb_ractor_t *ractor, rb_ractor_newobj_cache_t *ractor_cache)
{
    // Note: The `ractor` instance is still being initialized.
    struct rb_mmtk_mutator_local *mutator_local = rb_mmtk_ractor_cache_get_mutator_local(ractor_cache);
    memset(mutator_local, 0, sizeof(struct rb_mmtk_mutator_local));

    MMTk_Mutator *mutator = mmtk_bind_mutator((MMTk_VMMutatorThread)ractor);
    mutator_local->mutator = mutator;
    mutator_local->immix_bump_pointer = (struct BumpPointer*)((char*)mutator + mmtk_get_immix_bump_ptr_offset());
}

static size_t
rb_mmtk_system_physical_memory(void)
{
#ifdef __linux__
    const long physical_pages = sysconf(_SC_PHYS_PAGES);
    const long page_size = sysconf(_SC_PAGE_SIZE);
    if (physical_pages == -1 || page_size == -1)
    {
        rb_bug("failed to get system physical memory size");
    }
    return (size_t) physical_pages * (size_t) page_size;
#elif defined(__APPLE__)
    int mib[2];
    mib[0] = CTL_HW;
    mib[1] = HW_MEMSIZE; // total physical memory
    int64_t physical_memory;
    size_t length = sizeof(int64_t);
    if (sysctl(mib, 2, &physical_memory, &length, NULL, 0) == -1)
    {
        rb_bug("failed to get system physical memory size");
    }
    return (size_t) physical_memory;
#else
#error no implementation of rb_mmtk_system_physical_memory on this platform
#endif
}

static size_t
rb_mmtk_available_system_memory(void)
{
    /*
     * If we're in a container, we should use the maximum container memory,
     * otherwise each container will try to use all system memory. There's
     * example logic for this in the JVM and SVM (see CgroupV1Subsystem
     * and CgroupV2Subsystem).
     */

    return rb_mmtk_system_physical_memory();
}

static void
set_default_options(MMTk_Builder *mmtk_builder)
{
    mmtk_builder_set_plan(mmtk_builder, MMTK_DEFAULT_PLAN);

    const size_t default_min = 1024 * 1024;
    size_t default_max = rb_mmtk_available_system_memory() / 100 * rb_mmtk_heap_limit_percentage;
    if (default_max < default_min) {
        default_max = default_min;
    }
    mmtk_builder_set_dynamic_heap_size(mmtk_builder, default_min, default_max);
}

static void
apply_cmdline_options(MMTk_Builder *mmtk_builder)
{
    if (rb_mmtk_chosen_plan != NULL) {
        mmtk_builder_set_plan(mmtk_builder, rb_mmtk_chosen_plan);
    }

    if (rb_mmtk_max_heap_size > 0) {
        mmtk_builder_set_fixed_heap_size(mmtk_builder, rb_mmtk_max_heap_size);
    }
}

static void
set_variables_from_options(MMTk_Builder *mmtk_builder)
{
    rb_mmtk_plan_is_immix = mmtk_builder_is_immix(mmtk_builder) || mmtk_builder_is_sticky_immix(mmtk_builder);
    RUBY_DEBUG_LOG("mmtk_plan_is_immix = %d\n", rb_mmtk_plan_is_immix);

    rb_mmtk_plan_uses_bump_pointer = rb_mmtk_plan_is_immix;
    RUBY_DEBUG_LOG("mmtk_plan_uses_bump_pointer = %d\n", rb_mmtk_plan_uses_bump_pointer);

    rb_mmtk_plan_implicitly_pinning = mmtk_builder_is_mark_sweep(mmtk_builder);
    RUBY_DEBUG_LOG("mmtk_plan_implicitly_pinning = %d\n", rb_mmtk_plan_implicitly_pinning);

    // We sometimes for disabling or enabling barriers to measure the impact of barriers.
    const char* barrier_env_var = getenv("RB_MMTK_FORCE_BARRIER");
    if (barrier_env_var != NULL) {
        if (strcmp(barrier_env_var, "1") == 0) {
            rb_mmtk_use_barrier = true;
            fprintf(stderr, "WARNING: Force enabling barrier!\n");
        } else {
            rb_mmtk_use_barrier = false;
            fprintf(stderr, "WARNING: Force disabling barrier!\n");
        }
    } else {
        rb_mmtk_use_barrier = mmtk_builder_is_sticky_immix(mmtk_builder);
    }
    RUBY_DEBUG_LOG("rb_mmtk_use_barrier = %d\n", rb_mmtk_use_barrier);
}

void
rb_mmtk_main_thread_init(void)
{
    // (1) Create the builder, using MMTk's built-in defaults.
    MMTk_Builder *mmtk_builder = mmtk_builder_default();

    // (2) Override MMTK defaults with Ruby defaults.
    set_default_options(mmtk_builder);

    // (3) Read MMTk environment options (e.g. MMTK_THREADS=100)
    mmtk_builder_read_env_var_settings(mmtk_builder);

    // (4) Apply cmdline or RUBYOPT options if set.
    apply_cmdline_options(mmtk_builder);

    // Set Ruby-level variables from the actually set options.
    set_variables_from_options(mmtk_builder);

#if RACTOR_CHECK_MODE
    ruby_binding_options.ractor_check_mode = true;
    // Ruby only needs a uint32_t for the ractor ID.
    // But we make the object size a multiple of alignment.
    ruby_binding_options.suffix_size = MMTK_MIN_OBJ_ALIGN > sizeof(uint32_t) ?
        MMTK_MIN_OBJ_ALIGN : sizeof(uint32_t);
#else
    ruby_binding_options.ractor_check_mode = false;
    ruby_binding_options.suffix_size = 0;
#endif

    mmtk_init_binding(mmtk_builder, &ruby_binding_options, &ruby_upcalls);

    mmtk_vo_bit_base_addr = mmtk_get_vo_bit_base();
    mmtk_vo_bit_log_region_size = mmtk_get_vo_bit_log_region_size();
}

////////////////////////////////////////////////////////////////////////////////
// Flushing and de-initialization
////////////////////////////////////////////////////////////////////////////////

static void rb_mmtk_flush_obj_free_candidates(struct rb_mmtk_values_buffer *buffer);
static void rb_mmtk_flush_ppp_buffer(struct rb_mmtk_values_buffer *buffer);

void
rb_mmtk_flush_mutator_local_buffers(rb_ractor_newobj_cache_t *ractor_cache)
{
    struct rb_mmtk_mutator_local *local = rb_mmtk_ractor_cache_get_mutator_local(ractor_cache);
    rb_mmtk_flush_obj_free_candidates(&local->obj_free_candidates);
    rb_mmtk_flush_ppp_buffer(&local->ppp_buffer);
}

void
rb_mmtk_destroy_mutator(rb_ractor_newobj_cache_t *ractor_cache, bool at_fork)
{
    struct rb_mmtk_mutator_local *local = rb_mmtk_ractor_cache_get_mutator_local(ractor_cache);

    if (!at_fork) {
        // A thread only destroys its own mutator when it exits normally (not at fork).
        // But after forking, only the forking thread continue to live in the child process.
        // The living thread will call this function to close the mutators of all dead threads.
        // So we skip the assertions at fork.
        // RUBY_ASSERT(cur_thread == GET_THREAD());
        // RUBY_ASSERT(cur_thread->mutator_local == &rb_mmtk_mutator_local);
    }

    rb_mmtk_flush_mutator_local_buffers(ractor_cache);

    MMTk_Mutator *mutator = local->mutator;
    mmtk_destroy_mutator(mutator);

    local->mutator = NULL;
}

////////////////////////////////////////////////////////////////////////////////
// Object layout
////////////////////////////////////////////////////////////////////////////////

size_t
rb_mmtk_prefix_size(void)
{
    return MMTK_OBJREF_OFFSET;
}

size_t
rb_mmtk_suffix_size(void)
{
    // In RACTOR_CHECK_MODE, an additional hidden field is added to hold the Ractor ID.
    return ruby_binding_options.suffix_size;
}

void
rb_mmtk_init_hidden_header(VALUE object, size_t payload_size)
{
    RUBY_ASSERT(payload_size <= MMTK_HIDDEN_SIZE_MASK,
                "payload size greater than MMTK_HIDDEN_SIZE_MASK. payload_size: %zu", payload_size);

    struct MMTk_HiddenHeader *hidden_header = (struct MMTk_HiddenHeader*)(object - MMTK_OBJREF_OFFSET);
    hidden_header->prefix = payload_size;

    RUBY_ASSERT(mmtk_hidden_header_is_sane(hidden_header),
                "Hidden header is not sane on construction.  Object: %p, prefix: %zx",
                (void*) object, hidden_header->prefix);
}

size_t
rb_mmtk_get_payload_size(VALUE object)
{
    struct MMTk_HiddenHeader *hidden_header = (struct MMTk_HiddenHeader*)(object - MMTK_OBJREF_OFFSET);
    size_t prefix = hidden_header->prefix;

    RUBY_ASSERT(mmtk_hidden_header_is_sane(hidden_header),
                "Hidden header is corrupted.  Object: %p, prefix: %zx",
                (void*) object, prefix);

    return prefix & MMTK_HIDDEN_SIZE_MASK;
}

////////////////////////////////////////////////////////////////////////////////
// Allocation
////////////////////////////////////////////////////////////////////////////////

static void*
rb_mmtk_immix_alloc_fast_bump_pointer(struct rb_mmtk_mutator_local *local, size_t size)
{
    // TODO: verify the usefulness of this prefetching.
    PREFETCH((void*)local->last_new_cursor, 1);
    PREFETCH((void*)local->last_meta_addr, 1);

    struct BumpPointer *immix_bump_pointer = local->immix_bump_pointer;
    uintptr_t cursor = immix_bump_pointer->cursor;
    uintptr_t limit = immix_bump_pointer->limit;

    void *result = (void*)cursor;
    uintptr_t new_cursor = cursor + size;

    // Note: If the selected plan is not Immix, then both the cursor and the limit will always be
    // 0.  In that case this function will return NULL and the caller will try the slow path.
    if (new_cursor > limit) {
        return NULL;
    } else {
        immix_bump_pointer->cursor = new_cursor;
        local->last_new_cursor = new_cursor; // save for prefetching
        return result;
    }
}

/// Wrap mmtk_alloc, but use fast path if possible.
static void*
rb_mmtk_alloc(struct rb_mmtk_mutator_local *local, size_t size, MMTk_AllocationSemantics semantics)
{
    if (semantics == MMTK_ALLOCATION_SEMANTICS_DEFAULT && rb_mmtk_plan_uses_bump_pointer) {
        // Try the fast path.
        void *fast_result = rb_mmtk_immix_alloc_fast_bump_pointer(local, size);
        if (fast_result != NULL) {
            return fast_result;
        }
    }

    // Fall back to the slow path.
    void *result = mmtk_alloc(local->mutator, size, MMTK_MIN_OBJ_ALIGN, 0, semantics);

    return result;
}

#define RB_MMTK_USE_POST_ALLOC_FAST_PATH true
#define RB_MMTK_VO_BIT_SET_NON_ATOMIC true

static void
rb_mmtk_post_alloc_fast_immix(struct rb_mmtk_mutator_local *local, VALUE obj)
{
    uintptr_t obj_addr = obj;
    uintptr_t region_offset = obj_addr >> mmtk_vo_bit_log_region_size;
    uintptr_t byte_offset = region_offset / 8;
    uintptr_t bit_offset = region_offset % 8;
    uintptr_t meta_byte_address = mmtk_vo_bit_base_addr + byte_offset;
    uint8_t byte = 1 << bit_offset;
    if (RB_MMTK_VO_BIT_SET_NON_ATOMIC) {
        uint8_t *meta_byte_ptr = (uint8_t*)meta_byte_address;
        *meta_byte_ptr |= byte;
    } else {
        volatile _Atomic uint8_t *meta_byte_ptr = (volatile _Atomic uint8_t*)meta_byte_address;
        // relaxed: We don't use VO bits for synchronisation during mutator phase.
        // When GC is triggered, the handshake between GC and mutator provides synchronization.
        atomic_fetch_or_explicit(meta_byte_ptr, byte, memory_order_relaxed);
    }
    local->last_meta_addr = meta_byte_address;
}

/// Wrap mmtk_post_alloc, but use fast path if possible.
static void
rb_mmtk_post_alloc(struct rb_mmtk_mutator_local *local, VALUE obj, size_t mmtk_alloc_size, MMTk_AllocationSemantics semantics)
{
    if (RB_MMTK_USE_POST_ALLOC_FAST_PATH && semantics == MMTK_ALLOCATION_SEMANTICS_DEFAULT && rb_mmtk_plan_is_immix) {
        rb_mmtk_post_alloc_fast_immix(local, obj);
    } else {
        // Call post_alloc.  This will initialize GC-specific metadata.
        mmtk_post_alloc(local->mutator, (void*)obj, mmtk_alloc_size, semantics);
    }
}

static VALUE
rb_mmtk_alloc_obj(struct rb_mmtk_mutator_local *local, size_t alloc_size)
{
    size_t prefix_size = rb_mmtk_prefix_size();
    size_t suffix_size = rb_mmtk_suffix_size();
    size_t mmtk_alloc_size = alloc_size + prefix_size + suffix_size;

    MMTk_AllocationSemantics semantics = mmtk_alloc_size <= MMTK_MAX_IMMIX_OBJECT_SIZE ? MMTK_ALLOCATION_SEMANTICS_DEFAULT
                                       : MMTK_ALLOCATION_SEMANTICS_LOS;

    // Allocate the object.
    void *addr = rb_mmtk_alloc(local, mmtk_alloc_size, semantics);

    // The Ruby-level object reference (i.e. VALUE) is at an offset from the MMTk-level
    // allocation unit.
    VALUE obj = (VALUE)addr + prefix_size;

    // Store the Ruby-level object size before the object.
    rb_mmtk_init_hidden_header(obj, alloc_size);

    rb_mmtk_post_alloc(local, obj, mmtk_alloc_size, semantics);

#if RACTOR_CHECK_MODE
    void rb_ractor_setup_belonging(VALUE obj);
    rb_ractor_setup_belonging(obj);
#endif

    return obj;
}


static void rb_mmtk_maybe_register_initial_obj_free_candidate(struct rb_mmtk_mutator_local *local, VALUE obj);
static void rb_mmtk_maybe_register_initial_ppp(struct rb_mmtk_mutator_local *local, VALUE obj);

// Use this to allocate imemo:mmtk_strbuf or imemo:mmtk_objbuf.
VALUE
rb_mmtk_new_obj_raw(struct rb_mmtk_mutator_local *local, VALUE klass, VALUE flags, int wb_protected, size_t alloc_size)
{
    VALUE obj = rb_mmtk_alloc_obj(local, alloc_size);

    VALUE *alloc_obj = (VALUE*)obj;
    alloc_obj[0] = flags;
    alloc_obj[1] = klass;

    return obj;
}

// Use this to allocate any other Ruby objects.
// Copied from mmtk.c, with lots of modification.
VALUE
rb_mmtk_new_obj(void *objspace_ptr, void *cache_ptr, VALUE klass, VALUE flags, bool wb_protected, size_t alloc_size, size_t size_pool_size)
{
    #define MMTK_ALLOCATION_SEMANTICS_DEFAULT 0
    // struct objspace *objspace = objspace_ptr;
    struct rb_mmtk_mutator_local *local = rb_mmtk_ractor_cache_get_mutator_local((rb_ractor_newobj_cache_t*)cache_ptr);

    // if (objspace->gc_stress) {
    //     mmtk_handle_user_collection_request(ractor_cache, false, false);
    // }

    // The size pool size of default.c is a bit subtle when ractor checking mode is enabled.
    // We just let default.c work out the size pool size and we shall allocate that size.
    VALUE obj = rb_mmtk_new_obj_raw(local, klass, flags, wb_protected, size_pool_size);

    rb_mmtk_maybe_register_initial_obj_free_candidate(local, obj);
    rb_mmtk_maybe_register_initial_ppp(local, obj);

    if (RB_UNLIKELY(wb_protected == FALSE)) {
        mmtk_register_wb_unprotected_object((MMTk_ObjectReference)obj);
    }

    // objspace->total_allocated_objects++;

    return obj;
}

////////////////////////////////////////////////////////////////////////////////
// Write barrier
////////////////////////////////////////////////////////////////////////////////
void
rb_mmtk_object_reference_write_post(struct rb_mmtk_mutator_local *local, MMTk_ObjectReference object)
{
    mmtk_object_reference_write_post(local->mutator, object);
}

////////////////////////////////////////////////////////////////////////////////
// Tracing
////////////////////////////////////////////////////////////////////////////////

static inline MMTk_ObjectReference
rb_mmtk_call_object_closure(MMTk_ObjectReference object, bool pin) {
    return rb_mmtk_gc_thread_tls->object_closure.c_function(rb_mmtk_gc_thread_tls->object_closure.rust_closure,
                                                            rb_mmtk_gc_thread_tls->gc_context,
                                                            object,
                                                            pin);
}

static inline void
rb_mmtk_mark(VALUE obj, bool pin)
{
    rb_mmtk_assert_mmtk_worker();
    RUBY_DEBUG_LOG("Marking: %s %s %p",
        pin ? "(pin)" : "     ",
        RB_SPECIAL_CONST_P(obj) ? "(spc)" : "     ",
        (void*)obj);

    RUBY_ASSERT(!RB_SPECIAL_CONST_P(obj));

    rb_mmtk_call_object_closure((MMTk_ObjectReference)obj, pin);
}

// This function is used to visit and update all fields during tracing.
// It shall call both gc_mark_children and gc_update_object_references during copying GC.
static inline void
rb_mmtk_scan_object_ruby_style(MMTk_ObjectReference object)
{
    rb_mmtk_assert_mmtk_worker();

    VALUE obj = (VALUE)object;

    // TODO: When mmtk-core can clear the VO bit (a.k.a. alloc-bit), we can remove this.
    if (RB_BUILTIN_TYPE(obj) == T_NONE) {
        return;
    }

    rb_mmtk_mark_children(obj);
    rb_mmtk_update_object_references(obj);
}

// This is used to determine the pinning fields of potential pinning parents (PPPs).
// It should only call gc_mark_children.
static inline void
rb_mmtk_call_gc_mark_children(MMTk_ObjectReference object)
{
    rb_mmtk_assert_mmtk_worker();

    VALUE obj = (VALUE)object;

    // TODO: When mmtk-core can clear the VO bit (a.k.a. alloc-bit), we can remove this.
    if (RB_BUILTIN_TYPE(obj) == T_NONE) {
        return;
    }

    rb_mmtk_mark_children(obj);
}

void
rb_mmtk_mark_movable(VALUE obj)
{
    rb_mmtk_mark(obj, false);
}

void
rb_mmtk_mark_pin(VALUE obj)
{
    rb_mmtk_mark(obj, true);
}

void
rb_mmtk_mark_and_move(VALUE *field)
{
    VALUE obj = *field;
    RUBY_ASSERT(!RB_SPECIAL_CONST_P(obj));

    MMTk_ObjectReference old_ref = (MMTk_ObjectReference)obj;
    MMTk_ObjectReference new_ref = rb_mmtk_call_object_closure(old_ref, false);
    if (new_ref != old_ref) {
        *field = (VALUE)new_ref;
    }
}

bool
rb_mmtk_object_moved_p(VALUE value)
{
    if (!SPECIAL_CONST_P(value)) {
        MMTk_ObjectReference object = (MMTk_ObjectReference)value;
        return rb_mmtk_call_object_closure(object, false) != object;
    } else {
        return false;
    }
}

VALUE
rb_mmtk_maybe_forward(VALUE value)
{
    if (!SPECIAL_CONST_P(value)) {
        return (VALUE)rb_mmtk_call_object_closure((MMTk_ObjectReference)value, false);
    } else {
        return value;
    }
}

////////////////////////////////////////////////////////////////////////////////
// PPP support
////////////////////////////////////////////////////////////////////////////////

// Return true if an object is a PPP when allocated.
// This does not include PPP types that may become PPPs during its lifetime, such as
// -   Hash (when starting to compare keys by identity)
// -   iseq (since ISEQ_COMPILE_DATA_ALLOC)
static bool
rb_mmtk_is_initially_ppp(VALUE obj)
{
    RUBY_ASSERT(!rb_special_const_p(obj));

    switch (RB_BUILTIN_TYPE(obj)) {
      case T_DATA:
        return true;
      case T_IMEMO:
        switch (imemo_type(obj)) {
          case imemo_tmpbuf:
          case imemo_ifunc:
          case imemo_memo:
            return true;
          default:
            return false;
        }
      default:
        return false;
    }
}

// Return true if a registered PPP is no longer a PPP.  Return false otherwise.
// The return value doesn't matter for objects that are not registered as PPP.
static bool
rb_mmtk_is_no_longer_ppp(MMTk_ObjectReference objref)
{
    // We no longer have any objects that become non-PPP during execution.
    // But we keep this function just in case any use cases appear again in the future.

    VALUE obj = (VALUE)objref;
    RUBY_ASSERT(!rb_special_const_p(obj));

    switch (RB_BUILTIN_TYPE(obj)) {
      case T_IMEMO:
        switch (imemo_type(obj)) {
          default:
            break;
        }
      default:
        break;
    }

    return false;
}

static void
rb_mmtk_flush_ppp_buffer(struct rb_mmtk_values_buffer *buffer)
{
    RUBY_ASSERT(buffer != NULL);
    mmtk_register_ppps((MMTk_ObjectReference*)buffer->objects, buffer->len);
    rb_mmtk_values_buffer_clear(buffer);
}

void
rb_mmtk_register_ppp(struct rb_mmtk_mutator_local *local, VALUE obj)
{
    RUBY_ASSERT(!rb_special_const_p(obj));

    struct rb_mmtk_values_buffer *buffer = &local->ppp_buffer;
    if (rb_mmtk_values_buffer_append(buffer, obj)) {
        rb_mmtk_flush_ppp_buffer(buffer);
    }
}

static void
rb_mmtk_maybe_register_initial_ppp(struct rb_mmtk_mutator_local *local, VALUE obj)
{
    RUBY_ASSERT(!rb_special_const_p(obj));

    if (rb_mmtk_is_initially_ppp(obj)) {
        rb_mmtk_register_ppp(local, obj);
    }
}

////////////////////////////////////////////////////////////////////////////////
// Finalization and exiting
////////////////////////////////////////////////////////////////////////////////

static void
rb_mmtk_flush_obj_free_candidates(struct rb_mmtk_values_buffer *buffer)
{
    RUBY_ASSERT(buffer != NULL);
    mmtk_add_obj_free_candidates((MMTk_ObjectReference*)buffer->objects, buffer->len);
    rb_mmtk_values_buffer_clear(buffer);
}

void
rb_mmtk_register_obj_free_candidate(struct rb_mmtk_mutator_local *local, VALUE obj)
{
    RUBY_DEBUG_LOG("Object registered for obj_free: %p: %s %s",
        (void*)obj,
        rb_type_str(RB_BUILTIN_TYPE(obj)),
        RB_BUILTIN_TYPE(obj) == T_IMEMO ? rb_imemo_name(imemo_type(obj)) :
        rb_obj_class(obj) == 0 ? "(null klass)" :
        rb_class2name(rb_obj_class(obj))
        );

    struct rb_mmtk_values_buffer *buffer = &local->obj_free_candidates;
    if (rb_mmtk_values_buffer_append(buffer, obj)) {
        rb_mmtk_flush_obj_free_candidates(buffer);
    }
}

static bool
rb_mmtk_is_initial_obj_free_candidate(VALUE obj)
{
    // Any object that has non-trivial cleaning-up code in `obj_free`
    // should be registered as "finalizable" to MMTk.
    switch (RB_BUILTIN_TYPE(obj)) {
      case T_OBJECT:
        // FIXME: Ordinary objects can be non-embedded, too,
        // but there are just too many such objects,
        // and few of them have large buffers.
        // Just let them leak for now.
        // We'll prioritize eliminating the underlying buffer of ordinary objects.
        return false;
      case T_DATA:
        // RTypedData with both RUBY_TYPED_EMBEDDABLE and RUBY_TYPED_DEFAULT_FREE do not need
        // obj_free.  However, this function is called in the early stage of allocation, so we can't
        // make this decision, yet.  So we return `false` for now and let `rb_data_object_wrap` and
        // `typed_data_alloc` to decide whether to register the object as a candidate.
        return false;
      case T_MODULE:
      case T_CLASS:
      case T_HASH:
      case T_REGEXP:
      case T_FILE:
      case T_ICLASS:
      case T_BIGNUM:
      case T_STRUCT:
        // These types need obj_free.
        return true;
      case T_IMEMO:
        switch (imemo_type(obj)) {
          case imemo_callinfo:
          case imemo_env:
          case imemo_iseq:
          case imemo_ment:
          case imemo_tmpbuf:
            // These imemos need obj_free.
            return true;
          default:
            // Other imemos don't need obj_free.
            return false;
        }
      case T_SYMBOL:
        // Will be unregistered from global symbol table during weak reference processing phase.
        return false;
      case T_STRING:
        // We use imemo:mmtk_strbuf (rb_mmtk_strbuf_t) as the underlying buffer.
        return false;
      case T_ARRAY:
        // We use imemo:mmtk_objbuf (rb_mmtk_objbuf_t) as the underlying buffer.
        return false;
      case T_MATCH:
        // We use imemo:mmtk_strbuf (rb_mmtk_strbuf_t) for its several underlying buffers.
        return false;
      case T_RATIONAL:
      case T_COMPLEX:
      case T_FLOAT:
        // There are only counters increments for these types in `obj_free`
        return false;
      case T_NIL:
      case T_FIXNUM:
      case T_TRUE:
      case T_FALSE:
        // These are non-heap value types.
      case T_MOVED:
        // Should not see this when object is just created.
      case T_NODE:
        // GC doesn't handle T_NODE.
        rb_bug("rb_mmtk_maybe_register_obj_free_candidate: unexpected data type 0x%x(%p) 0x%"PRIxVALUE,
               BUILTIN_TYPE(obj), (void*)obj, RBASIC(obj)->flags);
      default:
        rb_bug("rb_mmtk_maybe_register_obj_free_candidate: unknown data type 0x%x(%p) 0x%"PRIxVALUE,
               BUILTIN_TYPE(obj), (void*)obj, RBASIC(obj)->flags);
    }
    UNREACHABLE;
}

static void
rb_mmtk_maybe_register_initial_obj_free_candidate(struct rb_mmtk_mutator_local *local, VALUE obj)
{
    if (rb_mmtk_is_initial_obj_free_candidate(obj)) {
        rb_mmtk_register_obj_free_candidate(local, obj);
    }
}

static void
rb_mmtk_call_obj_free_inner(VALUE obj, bool on_exit) {
    if (on_exit && !rb_gc_shutdown_call_finalizer_p(obj)) {
        return;
    }

    RUBY_DEBUG_LOG("Freeing object: %p: %s", (void*)obj, rb_type_str(RB_BUILTIN_TYPE(obj)));
    rb_mmtk_obj_free(obj);

    // The object may contain dangling pointers after `obj_free`.
    // Clear its flags field to ensure the GC does not attempt to scan it.
    // TODO: We can instead clear the VO bit (a.k.a. alloc-bit) when mmtk-core supports that.
    RBASIC(obj)->flags = 0;
    *(VALUE*)(&RBASIC(obj)->klass) = 0;
}

static inline void
rb_mmtk_call_obj_free(MMTk_ObjectReference object)
{
    rb_mmtk_assert_mmtk_worker();

    VALUE obj = (VALUE)object;

    rb_mmtk_call_obj_free_inner(obj, false);
}

static void
rb_mmtk_call_obj_free_for_each_on_exit(VALUE *objects, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        VALUE obj = objects[i];
        rb_mmtk_call_obj_free_inner(obj, true);
    }
}

void
rb_mmtk_call_obj_free_on_exit(void)
{
    unsigned int lev = RB_GC_VM_LOCK();
    {
        struct MMTk_RawVecOfObjRef registered_candidates = mmtk_get_all_obj_free_candidates();
        rb_mmtk_call_obj_free_for_each_on_exit((VALUE*)registered_candidates.ptr, registered_candidates.len);
        mmtk_free_raw_vec_of_obj_ref(registered_candidates);

        rb_vm_t *vm = GET_VM();
        rb_ractor_t *ractor;

        ccan_list_for_each(&vm->ractor.set, ractor, vmlr_node) {
            // ractor.set only contains blocking or running ractors
            GC_ASSERT(rb_ractor_status_p(ractor, ractor_blocking) ||
                    rb_ractor_status_p(ractor, ractor_running));
            struct rb_mmtk_mutator_local *local = rb_mmtk_ractor_get_mutator_local(ractor);
            struct rb_mmtk_values_buffer *buffer = &local->obj_free_candidates;
            rb_mmtk_call_obj_free_for_each_on_exit(buffer->objects, buffer->len);
        }
    }
    RB_GC_VM_UNLOCK(lev);
}

bool
rb_gc_obj_free_on_exit_started(void) {
    return rb_mmtk_obj_free_on_exit_started;
}

void
rb_gc_set_obj_free_on_exit_started(void) {
    rb_mmtk_obj_free_on_exit_started = true;
}

////////////////////////////////////////////////////////////////////////////////
// Weak table processing
////////////////////////////////////////////////////////////////////////////////

struct rb_mmtk_weak_table_rebuilding_context {
    st_table *old_table;
    st_table *new_table;
    enum RbMmtkWeakTableValueKind values_kind;
    rb_mmtk_hash_on_delete_func on_delete;
    void *on_delete_arg;
};

static int
rb_mmtk_update_weak_table_migrate_each(st_data_t key, st_data_t value, st_data_t arg)
{
    struct rb_mmtk_weak_table_rebuilding_context *ctx =
        (struct rb_mmtk_weak_table_rebuilding_context*)arg;

    // Preconditions:
    // The key must be an object reference,
    RUBY_ASSERT(!SPECIAL_CONST_P((VALUE)key));
    // and the key must point to a valid object (may be dead, but must be allocated).
    RUBY_ASSERT(mmtk_is_mmtk_object((MMTk_ObjectReference)key));

    bool key_live = mmtk_is_reachable((MMTk_ObjectReference)key);
    bool keep = key_live;
    bool value_live = true;

    if (ctx->values_kind == RB_MMTK_VALUES_WEAK_REF) {
        RUBY_ASSERT(
            // The value is either a primitive value (e.g. Fixnum that represents an ID)
            SPECIAL_CONST_P((VALUE)value) ||
            // or a valid object reference (e.g. to a Bignum that represents an ID).
            // It may be dead, but must be allocated.
            mmtk_is_mmtk_object((MMTk_ObjectReference)value));
        if (!SPECIAL_CONST_P((VALUE)value)) {
            value_live = mmtk_is_reachable((MMTk_ObjectReference)value);
            keep = keep && value_live;
        }
    }

    if (keep) {
        st_data_t new_key = (st_data_t)rb_mmtk_call_object_closure((MMTk_ObjectReference)key, false);
        st_data_t new_value = ctx->values_kind == RB_MMTK_VALUES_NON_REF ?
            value :
            (st_data_t)rb_mmtk_maybe_forward((VALUE)value); // Note that value may be primitive value or objref.
        st_insert(ctx->new_table, new_key, new_value);
        RUBY_DEBUG_LOG("Forwarding key-value pair: (%p, %p) -> (%p, %p)",
            (void*)key, (void*)value, (void*)new_key, (void*)new_value);
    } else {
        // The key or the value is dead. Discard the entry.
        RUBY_DEBUG_LOG("Discarding key-value pair: (%p, %p). Key is %s, value is %s",
            (void*)key, (void*)value, key_live ? "live" : "dead", value_live ? "live" : "dead");
        if (ctx->on_delete != NULL) {
            ctx->on_delete(key, value, ctx->on_delete_arg);
        }
    }

    return ST_CONTINUE;
}

struct rb_mmtk_weak_table_updating_context {
    enum RbMmtkWeakTableValueKind values_kind;
    rb_mmtk_hash_on_delete_func on_delete;
    void *on_delete_arg;
};

static int
rb_mmtk_update_weak_table_should_replace(st_data_t key, st_data_t value, st_data_t argp, int error)
{
    struct rb_mmtk_weak_table_updating_context *ctx =
        (struct rb_mmtk_weak_table_updating_context*)argp;

    if (!mmtk_is_live_object((MMTk_ObjectReference)key)) {
        return ST_DELETE;
    }

    if (ctx->values_kind == RB_MMTK_VALUES_WEAK_REF && !mmtk_is_live_object((MMTk_ObjectReference)value)) {
        return ST_DELETE;
    }

    MMTk_ObjectReference new_key = mmtk_get_forwarded_object((MMTk_ObjectReference)key);
    if (new_key != NULL && new_key != (MMTk_ObjectReference)key) {
        return ST_REPLACE;
    }

    if (ctx->values_kind != RB_MMTK_VALUES_NON_REF) {
        MMTk_ObjectReference new_value = mmtk_get_forwarded_object((MMTk_ObjectReference)value);
        if (new_value != NULL && new_value != (MMTk_ObjectReference)value) {
            return ST_REPLACE;
        }
    }

    return ST_CONTINUE;
}

static int
rb_mmtk_update_weak_table_replace(st_data_t *key, st_data_t *value, st_data_t argp, int existing)
{
    struct rb_mmtk_weak_table_updating_context *ctx =
        (struct rb_mmtk_weak_table_updating_context*)argp;

    MMTk_ObjectReference new_key = mmtk_get_forwarded_object((MMTk_ObjectReference)*key);
    if (new_key != NULL && new_key != (MMTk_ObjectReference)*key) {
        *key = (st_data_t)new_key;
    }

    if (ctx->values_kind != RB_MMTK_VALUES_NON_REF) {
        MMTk_ObjectReference new_value = mmtk_get_forwarded_object((MMTk_ObjectReference)*value);
        if (new_value != NULL && new_value != (MMTk_ObjectReference)*value) {
            *value = (st_data_t)new_value;
        }
    }

    return ST_CONTINUE;
}

/*
 * Update a weak hash table after a copying GC finished.
 * If a key points to a live object, keep the key-value pair,
 * and update the key (and optionally the value) to point to their new addresses.
 * If a key points to a dead object, discard the key-value pair.
 * If update_values is true, also discard the key-value pair if the value is dead.
 */
void
rb_mmtk_update_weak_table(st_table *table,
                          bool addr_hashed,
                          enum RbMmtkWeakTableValueKind values_kind,
                          rb_mmtk_hash_on_delete_func on_delete,
                          void *on_delete_arg)
{
    if (!table || table->num_entries == 0) return;

    // HACK: The way we update non-address-hashed tables will be unsound if we run `obj_free` in
    // parallel or before we update the weak table.  When deleting entries from st_table,
    // st_general_foreach will try to compare elements by value.  But if `obj_free` has been called
    // on dead objects, it may destroy the object (for example, freeing the underlying off-heap
    // buffer of strings), making them unable to be compared by value.  Creating another hash table
    // and replacing the existing one has a performance overhead, but is correct.  We should find a
    // more efficient way to delete dead objects from a hash table.
    addr_hashed = true;

    if (addr_hashed) {
        // The has table uses the address of the key object as key.
        // If a key object is moved, its hash is changed as well.
        // Therefore we must rebuild the whole hash table.
        // TODO: Implement address-based hashing to avoid this need.

        st_table *old_table = table;
        st_table *new_table = st_init_table(old_table->type);

        struct rb_mmtk_weak_table_rebuilding_context ctx = {
            .old_table = old_table,
            .new_table = new_table,
            .values_kind = values_kind,
            .on_delete = on_delete,
            .on_delete_arg = on_delete_arg,
        };
        if (st_foreach(old_table, rb_mmtk_update_weak_table_migrate_each, (st_data_t)&ctx)) {
            fprintf(stderr, "Did anything go wrong?");
            abort();
        }

        // Swap the contents of the old and the new table.
        // Note: The mutator may be rebuilding the same table when GC is updating it.
        // (see `rebuild_table` in st.c)
        // If the old table was not big enough, it will allocate a new table, but that may trigger GC.
        // After GC finishes and the new table is allocated,
        // the mutator will copy entries from the old table.
        // If we replace the whole old table,
        // the mutator shouldn't notice that the entire old table has been replaced during GC.
        st_table old_table_copy = *old_table;
        *old_table = *new_table;
        *new_table = old_table_copy;

        st_free_table(new_table);
    } else {
        // The table uses the content of the key object to compute the hash.
        // The hash will not change if the object is moved.
        // We can update the table in place.
        struct rb_mmtk_weak_table_updating_context ctx = {
            .values_kind = values_kind,
            .on_delete = on_delete,
            .on_delete_arg = on_delete_arg,
        };
        if (st_foreach_with_replace(table,
                                    rb_mmtk_update_weak_table_should_replace,
                                    rb_mmtk_update_weak_table_replace,
                                    (st_data_t)&ctx)) {
            fprintf(stderr, "Did anything go wrong?");
            abort();
        }
    }
}

// Copied from mmtk.c
int
rb_mmtk_update_table_i(VALUE val, void *data)
{
    if (!mmtk_is_reachable((MMTk_ObjectReference)val)) {
        return ST_DELETE;
    }

    return ST_REPLACE;
}

int
rb_mmtk_update_table_replace_i(VALUE *value, void *data)
{
    VALUE new_value = (VALUE)mmtk_get_forwarded_object((MMTk_ObjectReference)(*value));
    if (new_value != 0) {
        RUBY_DEBUG_LOG("Forwarding weak table key or value: %p -> %p\n", (void*)*value, (void*)new_value);
        *value = new_value;
    }
    return ST_CONTINUE;
}

/////////////// BEGIN: Concrete global weak tables ////////////////
// Note: Follow the order of `rb_gc_vm_weak_table_foreach` in `gc.c`

//////// CI table

size_t
rb_mmtk_get_ci_table_size(void)
{
    return GET_VM()->ci_table->num_entries;
}

void
rb_mmtk_update_ci_table(void)
{
    // The CI table is a deduplicating table for callinfo.
    // Used as a HashSet (key always equals value).
    // Compared and hashed by callinfo fields.  Two CIs are equal if all fields are equal.
    rb_mmtk_st_update_dedup_table(GET_VM()->ci_table);
}

//////// Overloaded CME table

size_t
rb_mmtk_get_overloaded_cme_table_size(void)
{
    return GET_VM()->overloaded_cme_table->num_entries;
}

static void
rb_mmtk_on_overloaded_cme_delete(st_data_t key, st_data_t value, void *arg)
{
#if USE_RUBY_DEBUG_LOG
    RUBY_DEBUG_LOG("Deleting from overloaded_cme_table: %p -> %p", (void*)key, (void*)value);
#endif
}

void
rb_mmtk_update_overloaded_cme_table(void)
{
    // The overloaded CME table.  It has both weak keys and weak values.
    rb_mmtk_update_weak_table(GET_VM()->overloaded_cme_table,
                              true,
                              true, // Currently values are pinned.
                              rb_mmtk_on_overloaded_cme_delete,
                              NULL);
}

//////// Global symbols table

size_t
rb_mmtk_get_global_symbols_table_size(void)
{
    return rb_mmtk_get_sym_set_num_entries();
}

void
rb_mmtk_update_global_symbols_table(void)
{
    // The global symbols table is a weak set of symbols.
    // It is backed by a concurrent_set.
    rb_gc_vm_weak_table_foreach(rb_mmtk_update_table_i, rb_mmtk_update_table_replace_i, NULL, false, RB_GC_VM_GLOBAL_SYMBOLS_TABLE);
}

//////// Finalizer and id2ref tables

// Defined in default.c.  The finalizer table is private to the default GC.
size_t rb_mmtk_get_finalizer_table_size(void);

// Defined in gc.c.  The id2ref table is a static variable in gc.c.
size_t rb_mmtk_get_id2ref_table_size(void);

// Defined in default.c.  The finalizer table is private to the default GC.
void rb_mmtk_update_finalizer_and_obj_id_tables(void);

//////// Generic fields table

// Defined in variable.c
struct st_table *rb_generic_fields_tbl_get(void);

size_t
rb_mmtk_get_generic_fields_tbl_size(void) {
    return rb_generic_fields_tbl_get()->num_entries;
}

void
rb_mmtk_update_generic_fields_table(void)
{
    // The generic_fields_tbl_ maps each object to its imemo:fields object.
    // Each key-value pair represents a strong edge from each key to its value.
    // rb_gc_mark_children traces the edge from key to value as if it were a field of the key.
    // We need to update both keys and values, and removed entries of dead keys.
    rb_gc_vm_weak_table_foreach(rb_mmtk_update_table_i, rb_mmtk_update_table_replace_i, NULL, false, RB_GC_VM_GENERIC_FIELDS_TABLE);
}

//////// Frozen strings table

size_t
rb_mmtk_get_frozen_strings_table_size(void)
{
    return rb_mmtk_debug_get_num_fstrings();
}

void
rb_mmtk_update_frozen_strings_table(void)
{
    // The frozen strings table is a deduplicating table for frozen strings.
    // It is now implemented as a special data structure `fstring_table_struct`.
    // We just use the default implementation to clean it up.
    // TODO: See if we need to parallelize it.
    // Since the default implementation simply does a linear scan, it is trivial to parallelize.

    rb_gc_vm_weak_table_foreach(rb_mmtk_update_table_i, rb_mmtk_update_table_replace_i, NULL, true, RB_GC_VM_FROZEN_STRINGS_TABLE);
}

//////// CC refinement table

size_t
rb_mmtk_get_cc_refinement_table_size(void)
{
    return GET_VM()->cc_refinement_table->num_entries;
}

void
rb_mmtk_update_cc_refinement_table(void)
{
    // We just use the default implementation to clean it up.

    rb_gc_vm_weak_table_foreach(rb_mmtk_update_table_i, rb_mmtk_update_table_replace_i, NULL, true, RB_GC_VM_CC_REFINEMENT_TABLE);
}


/////////////// END: Concrete global weak tables ////////////////

////////////////////////////////////////////////////////////////////////////////
// String buffer implementation
////////////////////////////////////////////////////////////////////////////////

rb_mmtk_strbuf_t*
rb_mmtk_new_strbuf(size_t capa)
{
    VALUE flags = T_IMEMO | (imemo_mmtk_strbuf << FL_USHIFT);
    size_t payload_size = offsetof(rb_mmtk_strbuf_t, ary) + capa;
    if (payload_size % MMTK_MIN_OBJ_ALIGN != 0) {
        payload_size = (payload_size + MMTK_MIN_OBJ_ALIGN - 1) & ~(MMTK_MIN_OBJ_ALIGN - 1);
    }
    VALUE obj = rb_mmtk_new_obj_raw(rb_mmtk_get_mutator_local(), capa, flags, true, payload_size);
    return (rb_mmtk_strbuf_t*)obj;
}

char*
rb_mmtk_strbuf_to_chars(rb_mmtk_strbuf_t* strbuf)
{
    return strbuf->ary;
}

rb_mmtk_strbuf_t*
rb_mmtk_chars_to_strbuf(char* chars)
{
    return (rb_mmtk_strbuf_t*)(chars - offsetof(rb_mmtk_strbuf_t, ary));
}

rb_mmtk_strbuf_t*
rb_mmtk_strbuf_realloc(rb_mmtk_strbuf_t* old_strbuf, size_t new_capa)
{
    // Allocate a new strbuf.
    rb_mmtk_strbuf_t *new_strbuf = rb_mmtk_new_strbuf(new_capa);

    // Copy content if old_strbuf is not NULL.
    if (old_strbuf != NULL) {
        size_t old_capa = old_strbuf->capa;
        size_t copy_size = old_capa > new_capa ? new_capa : old_capa;
        memcpy(new_strbuf->ary, old_strbuf->ary, copy_size);
    }

    return new_strbuf;
}

void
rb_mmtk_scan_offsetted_strbuf_field(char** field, bool update)
{
    // If the field contains NULL, return immediately.
    char *old_field_value = *field;
    if (old_field_value == NULL) {
        return;
    }

    // Trace the actual object.
    VALUE old_ref = (VALUE)rb_mmtk_chars_to_strbuf(old_field_value);
    VALUE new_ref = rb_mmtk_maybe_forward(old_ref);

    // Update the field if needed.
    if (update && new_ref != old_ref) {
        char *new_field_value = rb_mmtk_strbuf_to_chars((rb_mmtk_strbuf_t*)new_ref);
        *field = new_field_value;
    }
}

////////////////////////////////////////////////////////////////////////////////
// Object buffer implementation
////////////////////////////////////////////////////////////////////////////////

rb_mmtk_objbuf_t*
rb_mmtk_new_objbuf(size_t capa)
{
    VALUE flags = T_IMEMO | (imemo_mmtk_objbuf << FL_USHIFT);
    size_t payload_size = offsetof(rb_mmtk_objbuf_t, ary) + capa * sizeof(VALUE);
    if (payload_size % MMTK_MIN_OBJ_ALIGN != 0) {
        payload_size = (payload_size + MMTK_MIN_OBJ_ALIGN - 1) & ~(MMTK_MIN_OBJ_ALIGN - 1);
    }
    VALUE obj = rb_mmtk_new_obj_raw(rb_mmtk_get_mutator_local(), capa, flags, true, payload_size);
    return (rb_mmtk_objbuf_t*)obj;
}

VALUE*
rb_mmtk_objbuf_to_elems(rb_mmtk_objbuf_t* objbuf)
{
    return objbuf->ary;
}

////////////////////////////////////////////////////////////////////////////////
// Object pinning
////////////////////////////////////////////////////////////////////////////////

// Pin an object.  Do nothing if the plan implicitly pins all objects (i.e. non-moving).
void
rb_mmtk_pin_object(VALUE obj)
{
    if (!rb_mmtk_plan_implicitly_pinning) {
        mmtk_pin_object((MMTk_ObjectReference)obj);
    }
}

// Assert if an object is pinned.  Do nothing if the plan implicitly pins all objects (i.e. non-moving).
void
rb_mmtk_assert_is_pinned(VALUE obj)
{
    if (!rb_mmtk_plan_implicitly_pinning) {
        RUBY_ASSERT(mmtk_is_pinned((MMTk_ObjectReference)obj));
    }
}

////////////////////////////////////////////////////////////////////////////////
// Forking support
////////////////////////////////////////////////////////////////////////////////
void
rb_mmtk_shutdown_gc_threads(void)
{
  retry:
    rb_mmtk_global.fork_hook_vm_lock_lev = RB_GC_VM_LOCK();
    rb_gc_vm_barrier();

    /* At this point, we know that all the Ractors are paused because of the
     * rb_gc_vm_barrier above. Since rb_mmtk_block_for_gc is a barrier point,
     * one or more Ractors could be paused there. However, mmtk_before_fork is
     * not compatible with that because it assumes that the MMTk workers are idle,
     * but the workers are not idle because they are busy working on a GC.
     *
     * This essentially implements a trylock. It will optimistically lock but will
     * release the lock if it detects that any other Ractors are waiting in
     * rb_mmtk_block_for_gc.
     */
    rb_atomic_t mutator_blocking_count = RUBY_ATOMIC_LOAD(rb_mmtk_global.mutator_blocking_count);
    if (mutator_blocking_count != 0) {
        RB_GC_VM_UNLOCK(rb_mmtk_global.fork_hook_vm_lock_lev);
        goto retry;
    }

    mmtk_prepare_to_fork();
}

void rb_mmtk_respawn_gc_threads(void)
{
    mmtk_after_fork(GET_THREAD());

    RB_GC_VM_UNLOCK(rb_mmtk_global.fork_hook_vm_lock_lev);
}

////////////////////////////////////////////////////////////////////////////////
// GC module and information (GC)
////////////////////////////////////////////////////////////////////////////////

// Return the number of GCs happened since the program started.
size_t
rb_mmtk_gc_count(void)
{
    return rb_mmtk_global.start_the_world_count;
}

////////////////////////////////////////////////////////////////////////////////
// MMTk-specific Ruby module (GC::MMTk)
////////////////////////////////////////////////////////////////////////////////

void
rb_mmtk_define_gc_mmtk_module(void)
{
    VALUE rb_mMMTk = rb_define_module_under(rb_mGC, "MMTk");
    rb_define_singleton_method(rb_mMMTk, "plan_name", rb_mmtk_plan_name, 0);
    rb_define_singleton_method(rb_mMMTk, "enabled?", rb_mmtk_enabled, 0);
    rb_define_singleton_method(rb_mMMTk, "harness_begin", rb_mmtk_harness_begin, 0);
    rb_define_singleton_method(rb_mMMTk, "harness_end", rb_mmtk_harness_end, 0);
}

/*
 *  call-seq:
 *      GC::MMTk.plan_name -> String
 *
 *  Returns the name of the current MMTk plan.
 */
VALUE
rb_mmtk_plan_name(VALUE _)
{
    if (!rb_mmtk_enabled_p()) {
        rb_raise(rb_eRuntimeError, "Debug harness can only be used when MMTk is enabled, re-run with --mmtk.");
    }
    const char* plan_name = mmtk_plan_name();
    return rb_str_new(plan_name, strlen(plan_name));
}

/*
 *  call-seq:
 *      GC::MMTk.enabled? -> true or false
 *
 *  Returns true if using MMTk as garbage collector, false otherwise.
 *
 *  Note: If the Ruby interpreter is not compiled with MMTk support, the
 *  <code>GC::MMTk</code> module will not exist in the first place.
 *  You can check if the module exists by
 *
 *    defined? GC::MMTk
 */
VALUE
rb_mmtk_enabled(VALUE _)
{
    return RBOOL(rb_mmtk_enabled_p());
}

/*
 *  call-seq:
 *      GC::MMTk.harness_begin
 *
 *  A hook to be called before a benchmark begins.
 *
 *  MMTk will do necessary preparations (such as triggering a full-heap GC)
 *  and start collecting statistic data, such as the number of GC triggered,
 *  time spent in GC, time spent in mutator, etc.
 */
VALUE
rb_mmtk_harness_begin(VALUE _)
{
    if (rb_mmtk_enabled_p()) {
        mmtk_harness_begin((MMTk_VMMutatorThread)GET_THREAD());
    } else {
        rb_mmtk_vanilla_timing.last_num_of_gc = rb_gc_count();
        rb_mmtk_get_vanilla_times(&rb_mmtk_vanilla_timing.last_vanilla_mark, &rb_mmtk_vanilla_timing.last_vanilla_sweep);
        rb_mmtk_vanilla_timing.enabled = true;
        clock_gettime(CLOCK_MONOTONIC, &rb_mmtk_vanilla_timing.last_enabled);
    }

    return Qnil;
}

static uint64_t elapsed_ns(struct timespec *now, struct timespec *then) {
    uint64_t diff_s = now->tv_sec - then->tv_sec;
    uint64_t elapsed = diff_s * 1000000000 + now->tv_nsec - then->tv_nsec;
    return elapsed;
}

/*
 *  call-seq:
 *      GC::MMTk.harness_end
 *
 *  A hook to be called after a benchmark ends.
 *
 *  When this method is called, MMTk will stop collecting statistic data and
 *  print out the data already collected.
 */
VALUE
rb_mmtk_harness_end(VALUE _)
{
    if (rb_mmtk_enabled_p()) {
        mmtk_harness_end((MMTk_VMMutatorThread)GET_THREAD());
    } else {
        rb_mmtk_vanilla_timing.enabled = false;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t total_time_ns = elapsed_ns(&now, &rb_mmtk_vanilla_timing.last_enabled);
        uint64_t gc_time_ns = rb_mmtk_vanilla_timing.gc_time_ns;
        uint64_t stw_time_ns = total_time_ns - gc_time_ns;

        double total_time_ms = total_time_ns / 1000000.0;
        double gc_time_ms = gc_time_ns / 1000000.0;
        double stw_time_ms = stw_time_ns / 1000000.0;

        size_t num_of_gc = rb_gc_count() - rb_mmtk_vanilla_timing.last_num_of_gc;

        uint64_t cur_vanilla_mark, cur_vanilla_sweep;
        rb_mmtk_get_vanilla_times(&cur_vanilla_mark, &cur_vanilla_sweep);
        uint64_t vanilla_mark = cur_vanilla_mark - rb_mmtk_vanilla_timing.last_vanilla_mark;
        uint64_t vanilla_sweep = cur_vanilla_sweep - rb_mmtk_vanilla_timing.last_vanilla_sweep;
        uint64_t vanilla_time = vanilla_mark + vanilla_sweep;

        double vanilla_time_ms = vanilla_time / 1000000.0;
        double vanilla_mark_ms = vanilla_mark / 1000000.0;
        double vanilla_sweep_ms = vanilla_sweep / 1000000.0;

        fprintf(stderr, "======== Begin vanilla GC timing report (mmtk-ruby) ========\n");
        fprintf(stderr, "%10s %18s %18s %18s %18s %18s\n", "GC", "time.other", "time.stw", "v.time.gc", "v.time.mark", "v.time.sweep");
        fprintf(stderr, "%10zu %18lf %18lf %18lf %18lf %18lf\n", num_of_gc, stw_time_ms, gc_time_ms, vanilla_time_ms, vanilla_mark_ms, vanilla_sweep_ms);
        fprintf(stderr, "Total time: %lf ms\n", total_time_ms);
        fprintf(stderr, "======== End vanilla GC timing report (mmtk-ruby) ========\n");
    }

    return Qnil;
}

////////////////////////////////////////////////////////////////////////////////
// Debugging
////////////////////////////////////////////////////////////////////////////////

bool
rb_mmtk_is_mmtk_worker(void)
{
    return rb_mmtk_gc_thread_tls != NULL;
}

bool
rb_mmtk_is_mutator(void)
{
    return ruby_native_thread_p();
}

void
rb_mmtk_assert_mmtk_worker(void)
{
    RUBY_ASSERT_MESG(rb_mmtk_is_mmtk_worker(), "The current thread is not an MMTk worker");
}

void
rb_mmtk_assert_mutator(void)
{
    RUBY_ASSERT_MESG(rb_mmtk_is_mutator(), "The current thread is not a mutator (i.e. Ruby thread)");
}

bool
rb_mmtk_is_valid_objref(VALUE obj)
{
    return obj != 0 && obj % sizeof(VALUE) == 0 && mmtk_is_mmtk_object((MMTk_Address)obj);
}

////////////////////////////////////////////////////////////////////////////////
// Vanilla GC timing
////////////////////////////////////////////////////////////////////////////////

void
rb_mmtk_gc_probe(bool enter)
{
    if (!rb_mmtk_vanilla_timing.enabled) {
        return;
    }

    if (rb_mmtk_vanilla_timing.in_alloc_slow_path) {
        return;
    }

    if (enter) {
        // Note: Vanilla GC also has timing facilities exposed with `GC.stat[:time]`.
        // But that uses `current_process_time` which uses `CLOCK_PROCESS_CPUTIME_ID`
        // while MMTk uses Rust's `std::time::Instant` which uses `CLOCK_MONOTONIC`.
        // To be fair, we reimplmenet the probing and use `CLOCK_MONOTONIC` instead.
        clock_gettime(CLOCK_MONOTONIC, &rb_mmtk_vanilla_timing.last_gc_start);
    } else {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t elapsed = elapsed_ns(&now, &rb_mmtk_vanilla_timing.last_gc_start);
        rb_mmtk_vanilla_timing.gc_time_ns += elapsed;
    }
}

// Use this to exclude the allocation slow path from the STW time.
void
rb_mmtk_gc_probe_slowpath(bool enter)
{
    rb_mmtk_vanilla_timing.in_alloc_slow_path = enter;
}

////////////////////////////////////////////////////////////////////////////////
// xmalloc accounting
////////////////////////////////////////////////////////////////////////////////

// copied from gc.c
static inline void
atomic_sub_nounderflow(size_t *var, size_t sub)
{
    if (sub == 0) return;

    while (1) {
        size_t val = *var;
        if (val < sub) sub = val;
        if (ATOMIC_SIZE_CAS(*var, val, val-sub) == val) break;
    }
}

// Record the increment of xmalloc-ed memory and potentially trigger a GC.
void
rb_mmtk_xmalloc_increase_body(size_t new_size, size_t old_size)
{
    if (new_size > old_size) {
        ATOMIC_SIZE_ADD(rb_mmtk_xmalloc_accounting_t.malloc_total, new_size - old_size);
    }
    else {
        atomic_sub_nounderflow(&rb_mmtk_xmalloc_accounting_t.malloc_total, old_size - new_size);
    }
}

static size_t
rb_mmtk_vm_live_bytes(void)
{
    return rb_mmtk_xmalloc_accounting_t.malloc_total;
}

////////////////////////////////////////////////////////////////////////////////
// MMTk-Ruby Upcalls
////////////////////////////////////////////////////////////////////////////////

static void
rb_mmtk_init_gc_worker_thread(MMTk_VMWorkerThread gc_thread_tls)
{
    rb_mmtk_gc_thread_tls = gc_thread_tls;
}

static MMTk_VMWorkerThread
rb_mmtk_get_gc_thread_tls(void)
{
    return rb_mmtk_gc_thread_tls;
}

static void
rb_mmtk_wait_until_ractors_stopped(void *unused)
{
    while (!rb_mmtk_global.world_stopped) {
        RUBY_DEBUG_LOG("Waiting for the world to stop...");
        pthread_cond_wait(&rb_mmtk_global.cond_world_stopped, &rb_mmtk_global.mutex);
    }
}

static void
rb_mmtk_stop_the_world(MMTk_VMWorkerThread _tls)
{
    rb_mmtk_assert_mmtk_worker();

    // CRuby uses VM barrier to do STW.  But only one CRuby ractor can stop other CRuby ractors.
    // We let `rb_mmtk_block_for_gc` initiate the STW.
    // Here we just wait for the world to come to a stop.
    rb_mmtk_use_mmtk_global(rb_mmtk_wait_until_ractors_stopped, NULL);

    rb_mmtk_set_during_gc(true);
}

static void
rb_mmtk_increment_start_the_world_count(void *unused)
{
    (void)unused;
    rb_mmtk_global.start_the_world_count++;
    pthread_cond_broadcast(&rb_mmtk_global.cond_world_started);
}

static void
rb_mmtk_resume_mutators(MMTk_VMWorkerThread tls)
{
    rb_mmtk_assert_mmtk_worker();

    rb_mmtk_set_during_gc(false);

    rb_mmtk_use_mmtk_global(rb_mmtk_increment_start_the_world_count, NULL);
}

static void
rb_mmtk_block_for_gc_internal(void *unused)
{
    // Mark that the world has stopped, and notify the GC worker thread.
    rb_mmtk_global.world_stopped = true;
    pthread_cond_broadcast(&rb_mmtk_global.cond_world_stopped);

    // Wait for GC end
    size_t my_count = rb_mmtk_global.start_the_world_count;
    size_t next_count = my_count + 1;

    while (rb_mmtk_global.start_the_world_count < next_count) {
        RUBY_DEBUG_LOG("Will wait until world start again. cur: %zu, expected: %zu",
                rb_mmtk_global.start_the_world_count, next_count);
        pthread_cond_wait(&rb_mmtk_global.cond_world_started, &rb_mmtk_global.mutex);
    }

    // Mark that the world has started.
    rb_mmtk_global.world_stopped = false;

    RUBY_DEBUG_LOG("GC finished.");
}

static void
rb_mmtk_block_for_gc(MMTk_VMMutatorThread tls)
{
    rb_mmtk_assert_mutator();

    // Save the execution context of the current thread.
    // Other threads in the current mutator should have saved their contexts before they go to sleep.
    rb_thread_t *cur_th = GET_THREAD();
    RB_VM_SAVE_MACHINE_CONTEXT(cur_th);

    // Get the start-the-world count.
    // It is thread-safe because the count is only mutated by a GC worker when all mutators stopped.
    size_t my_count = rb_mmtk_global.start_the_world_count;

    // Acquire the VM lock.
    // Note that the current ractor may not be the only mutator that requests GC.
    // The first mutator reached here will acquire the lock and initiate the VM barrier.
    // Subsequent mutators reached here will block until the GC finishes.
    // We use a mutator blocking count to communicate with rb_gc_impl_before_fork.
    RUBY_ATOMIC_INC(rb_mmtk_global.mutator_blocking_count);
    int lock_lev = RB_GC_VM_LOCK();
    RUBY_ATOMIC_DEC(rb_mmtk_global.mutator_blocking_count);


    if (rb_mmtk_global.start_the_world_count == my_count) {
        // If the GC count is the same, we are the first mutator reached here.

        // Execute GC event hooks.
        // TODO: Should we do this when other Ractors have reached the VM barrier?
        rb_gc_event_hook(0, RUBY_INTERNAL_EVENT_GC_START);

        // Stop other Ractors.
        rb_gc_vm_barrier();
        // By the time we reach here, other Ractors have stopped, attempting to acquire the VM lock.

        // Flush ractor-local (mutator-local) buffers.
        {
            rb_vm_t *vm = GET_VM();
            rb_ractor_t *ractor;
            ccan_list_for_each(&vm->ractor.set, ractor, vmlr_node) {
                // ractor.set only contains blocking or running ractors
                GC_ASSERT(rb_ractor_status_p(ractor, ractor_blocking) ||
                        rb_ractor_status_p(ractor, ractor_running));
                rb_mmtk_flush_mutator_local_buffers(ractor->newobj_cache);
            }
        }

        // Notify GC worker and wait for GC to finish.
        rb_mmtk_use_mmtk_global(rb_mmtk_block_for_gc_internal, NULL);
        // By the time we reach here, the GC has finished.
    }

    // Release the VM lock.
    // Other Ractors will continue from rb_gc_vm_lock and find that the GC has finished.
    RB_GC_VM_UNLOCK(lock_lev);

    // Trigger postponed job so that a mutator will start running pending final jobs soon.
    rb_mmtk_gc_finalize_deferred_register();
}

static void
rb_mmtk_get_mutators(void (*visit_mutator)(MMTk_Mutator *mutator, void *data), void *data)
{
    rb_mmtk_assert_mmtk_worker();

    rb_vm_t *vm = GET_VM();
    rb_ractor_t *ractor;

    ccan_list_for_each(&vm->ractor.set, ractor, vmlr_node) {
        // ractor.set only contains blocking or running ractors
        GC_ASSERT(rb_ractor_status_p(ractor, ractor_blocking) ||
                  rb_ractor_status_p(ractor, ractor_running));
        struct rb_mmtk_mutator_local *local = rb_mmtk_ractor_get_mutator_local(ractor);
        visit_mutator(local->mutator, data);
    }
}

static void
increment_mutator_counter(MMTk_Mutator *mutator, void *data)
{
    size_t *counter = (size_t*)data;
    (*counter)++;
}

static size_t
rb_mmtk_number_of_mutators(void)
{
    rb_mmtk_assert_mmtk_worker();

    size_t counter = 0;
    rb_mmtk_get_mutators(increment_mutator_counter, (void*)&counter);
    return counter;
}

static void
rb_mmtk_scan_roots_in_mutator_thread(MMTk_VMMutatorThread vm_mutator, MMTk_VMWorkerThread worker)
{
    rb_mmtk_assert_mmtk_worker();

    // We don't really need to do anything because all ractors and stacks are reachable from rb_vm_t.
}


bool
rb_mmtk_has_exivar(MMTk_ObjectReference object)
{
    return rb_obj_gen_fields_p((VALUE)object);
}

static MMTk_ObjectReference
rb_mmtk_get_fstring_table_obj_wrapper(void)
{
    return (MMTk_ObjectReference)rb_mmtk_get_fstring_table_obj();
}

static MMTk_ObjectReference
rb_mmtk_get_global_symbols_table_obj(void)
{
    return (MMTk_ObjectReference)rb_mmtk_get_sym_set();
}

static size_t
rb_mmtk_concurrent_set_get_num_entries_wrapper(MMTk_ObjectReference set_obj)
{
    return rb_mmtk_concurrent_set_get_num_entries((VALUE)set_obj);
}

static size_t
rb_mmtk_concurrent_set_get_capacity_wrapper(MMTk_ObjectReference set_obj)
{
    return rb_mmtk_concurrent_set_get_capacity((VALUE)set_obj);
}

static int
rb_mmtk_concurrent_set_update_entries_range_i(VALUE *key, void *data)
{
    MMTk_ConcurrentSetStats *stats = (MMTk_ConcurrentSetStats*)data;
    VALUE old_key = *key;

    if (rb_special_const_p(old_key)) {
        // The global symbols table may contain integers which encode static symbols.
        // We skip them.
        return ST_CONTINUE;
    }

    if (mmtk_is_reachable((MMTk_ObjectReference)old_key)) {
        stats->live++;
        VALUE new_key = (VALUE)mmtk_get_forwarded_object((MMTk_ObjectReference)old_key);
        if (new_key != 0 && new_key != old_key) {
            stats->moved++;
            *key = new_key;
        }
        return ST_CONTINUE;
    } else {
        stats->deleted++;
        return ST_DELETE;
    }
}

static void
rb_mmtk_concurrent_set_update_entries_range(MMTk_ObjectReference set_obj, size_t begin, size_t end, uint8_t kind, MMTk_ConcurrentSetStats *stats)
{
    int (*callback)(VALUE*, void*) = rb_mmtk_concurrent_set_update_entries_range_i;
    void *data = stats;

    switch (kind) {
        case MMTK_WEAK_CONCURRENT_SET_KIND_GLOBAL_SYMBOLS: {
            // Global symbols table need special handling.
            // rb_mmtk_sym_global_symbol_table_foreach_weak_reference_range will handle static symbols.
            assert((VALUE)set_obj == rb_mmtk_get_sym_set());
            rb_mmtk_sym_global_symbol_table_foreach_weak_reference_range(begin, end, callback, data);
            break;
        }
        default: {
            rb_mmtk_concurrent_set_foreach_with_replace_range((VALUE)set_obj, begin, end, callback, data);
            break;
        }
    }
}

MMTk_RubyUpcalls ruby_upcalls = {
    rb_mmtk_init_gc_worker_thread,
    rb_mmtk_get_gc_thread_tls,
    rb_mmtk_is_mutator,
    rb_mmtk_stop_the_world,
    rb_mmtk_resume_mutators,
    rb_mmtk_block_for_gc,
    rb_mmtk_number_of_mutators,
    rb_mmtk_get_mutators,
    rb_mmtk_scan_vm_roots,
    rb_mmtk_scan_end_proc_roots,
    rb_mmtk_scan_global_tbl_roots,
    rb_mmtk_scan_yjit_roots,
    rb_mmtk_scan_global_symbols_roots,
    rb_mmtk_scan_finalizer_tbl_roots,
    rb_mmtk_scan_misc_roots,
    rb_mmtk_scan_final_jobs_roots,
    rb_mmtk_scan_roots_in_mutator_thread,
    rb_mmtk_is_no_longer_ppp,
    rb_mmtk_scan_object_ruby_style,
    rb_mmtk_call_gc_mark_children,
    rb_mmtk_call_obj_free,
    rb_mmtk_vm_live_bytes,
    rb_mmtk_has_exivar,
    // Simple table size query and update functions
    rb_mmtk_get_ci_table_size,
    rb_mmtk_update_ci_table,
    rb_mmtk_get_overloaded_cme_table_size,
    rb_mmtk_update_overloaded_cme_table,
    rb_mmtk_get_global_symbols_table_size,
    rb_mmtk_update_global_symbols_table,
    rb_mmtk_get_finalizer_table_size,
    rb_mmtk_get_id2ref_table_size,
    rb_mmtk_update_finalizer_and_obj_id_tables,
    rb_mmtk_get_generic_fields_tbl_size,
    rb_mmtk_update_generic_fields_table,
    rb_mmtk_get_frozen_strings_table_size,
    rb_mmtk_update_frozen_strings_table,
    rb_mmtk_get_cc_refinement_table_size,
    rb_mmtk_update_cc_refinement_table,
    // Get tables for specialized processing
    rb_mmtk_get_fstring_table_obj_wrapper,
    rb_mmtk_get_global_symbols_table_obj,
    // Detailed st_table info queries and operations
    rb_mmtk_st_get_num_entries,
    rb_mmtk_st_get_size_info,
    rb_mmtk_st_update_entries_range,
    rb_mmtk_st_update_bins_range,
    // Detailed concurrent_set info queries and operations
    rb_mmtk_concurrent_set_get_num_entries_wrapper,
    rb_mmtk_concurrent_set_get_capacity_wrapper,
    rb_mmtk_concurrent_set_update_entries_range,
    // Memory protection for code memory
    rb_gc_before_updating_jit_code,
    rb_gc_after_updating_jit_code,
};

////////////////////////////////////////////////////////////////////////////////
// Commandline options parsing
////////////////////////////////////////////////////////////////////////////////

static size_t
rb_mmtk_parse_heap_limit(const char *argv, bool* had_error)
{
    char *endval = NULL;
    int pow = 0;

    size_t base = strtol(argv, &endval, 10);
    if (base == 0) {
        *had_error = true;
    }

    // if there were non-numbers in the string
    // try and parse them as IEC units
    if (*endval) {
        if (strcmp(endval, "TiB") == 0)  {
            pow = 40; // tebibytes. 2^40
        } else if (strcmp(endval, "GiB") == 0)  {
            pow = 30; // gibibytes. 2^30
        } else if (strcmp(endval, "MiB") == 0)  {
            pow = 20; // mebibytes. 2^20
        } else if (strcmp(endval, "KiB") == 0)  {
            pow = 10; // kibibytes. 2^10
        }
    }

    return (base << pow);
}

void rb_mmtk_pre_process_opts(int argc, char **argv) {
    /*
     * Processing these arguments is a mess - we have to process them before
     * Ruby is set up, when arguments are normally processed, because we need
     * the GC up and running to set up Ruby. We have to kind of rough parsing
     * and then re-parse them properly later and compare against our rough
     * parsing. We also can't report errors using exceptions. Needs tidying
     * up in general, but may always be a bit awkward.
     */

    bool enable_rubyopt = true;

    for (int n = 1; n < argc; n++) {
        if (strcmp(argv[n], "--") == 0) {
            break;
        }
        else if (strcmp(argv[n], "--mmtk") == 0) {
            mmtk_enable = true;
        }
        else if (strcmp(argv[n], "--enable") == 0
                && argc > (n + 1) && strcmp(argv[n+1], "all") == 0) {
            mmtk_enable = true;
            enable_rubyopt = true;
        }
        else if (strcmp(argv[n], "--enable-all") == 0
                || strcmp(argv[n], "--enable=all") == 0) {
            mmtk_enable = true;
            enable_rubyopt = true;
        }
        else if (strcmp(argv[n], "--enable-rubyopt") == 0
                || strcmp(argv[n], "--enable=rubyopt") == 0) {
            enable_rubyopt = true;
        }
        else if (strcmp(argv[n], "--disable-rubyopt") == 0
                || strcmp(argv[n], "--disable=rubyopt") == 0) {
            enable_rubyopt = false;
        }
        else if (strcmp(argv[n], "--enable-mmtk") == 0
                || strcmp(argv[n], "--enable=mmtk") == 0) {
            mmtk_enable = true;
        }
        else if (strcmp(argv[n], "--disable-mmtk") == 0
                || strcmp(argv[n], "--disable=mmtk") == 0) {
            mmtk_enable = false;
        }
        else if (strncmp(argv[n], "--mmtk-plan", strlen("--mmtk-plan")) == 0) {
            mmtk_enable = true;
            rb_mmtk_pre_arg_plan = argv[n] + strlen("--mmtk-plan=");
            if (argv[n][strlen("--mmtk-plan")] != '=' || strlen(rb_mmtk_pre_arg_plan) == 0) {
                fputs("[FATAL] --mmtk-plan needs an argument\n", stderr);
                exit(EXIT_FAILURE);
            }
        }
        else if (strncmp(argv[n], "--mmtk-max-heap", strlen("--mmtk-max-heap")) == 0) {
            mmtk_enable = true;
            char *mmtk_max_heap_size_arg = argv[n] + strlen("--mmtk-max-heap=");
            if (argv[n][strlen("--mmtk-max-heap")] != '=' || strlen(mmtk_max_heap_size_arg) == 0) {
                fputs("[FATAL] --mmtk-max-heap needs an argument\n", stderr);
                exit(EXIT_FAILURE);
            }
            rb_mmtk_pre_max_heap_size = rb_mmtk_parse_heap_limit(mmtk_max_heap_size_arg, &rb_mmtk_max_heap_parse_error);
            rb_mmtk_max_heap_size = rb_mmtk_pre_max_heap_size;
        }
    }

    if (enable_rubyopt) {
        char *env_args = getenv("RUBYOPT");
        if (env_args != NULL) {
            while (*env_args != '\0') {
                if (ISSPACE(*env_args)) {
                    env_args++;
                }
                else {
                    size_t length = 0;
                    while (env_args[length] != '\0' && !ISSPACE(env_args[length])) {
                        length++;
                    }

                    if (strncmp(env_args, "--mmtk", strlen("--mmtk")) == 0) {
                        mmtk_enable = true;
                    } else if (strncmp(env_args, "--enable-mmtk", strlen("--enable-mmtk")) == 0) {
                        mmtk_enable = true;
                    } else if (strncmp(env_args, "--enable=mmtk", strlen("--enable=mmtk")) == 0) {
                        mmtk_enable = true;
                    }

                    if (strncmp(env_args, "--mmtk-plan", strlen("--mmtk-plan")) == 0) {
                        if (env_args[strlen("--mmtk-plan")] != '=') {
                            fputs("[FATAL] --mmtk-plan needs an argument\n", stderr);
                            exit(EXIT_FAILURE);
                        }
                        rb_mmtk_pre_arg_plan = strndup(env_args + strlen("--mmtk-plan="), length - strlen("--mmtk-plan="));
                        if (rb_mmtk_pre_arg_plan == NULL) {
                            rb_bug("could not allocate space for argument");
                        }
                        if (strlen(rb_mmtk_pre_arg_plan) == 0) {
                            fputs("[FATAL] --mmtk-plan needs an argument\n", stderr);
                            exit(EXIT_FAILURE);
                        }
                    } else if (strncmp(env_args, "--mmtk-max-heap", strlen("--mmtk-max-heap")) == 0) {
                        if (env_args[strlen("--mmtk-max-heap")] != '=') {
                            fputs("[FATAL] --mmtk-max-heap needs an argument\n", stderr);
                            exit(EXIT_FAILURE);
                        }
                        char *mmtk_max_heap_size_arg = strndup(env_args + strlen("--mmtk-max-heap="), length - strlen("--mmtk-max-heap="));
                        if (mmtk_max_heap_size_arg == NULL) {
                            rb_bug("could not allocate space for argument");
                        }
                        if (strlen(mmtk_max_heap_size_arg) == 0) {
                            fputs("[FATAL] --mmtk-max-heap needs an argument\n", stderr);
                            exit(EXIT_FAILURE);
                        }
                        rb_mmtk_pre_max_heap_size = rb_mmtk_parse_heap_limit(mmtk_max_heap_size_arg, &rb_mmtk_max_heap_parse_error);
                        rb_mmtk_max_heap_size = rb_mmtk_pre_max_heap_size;
                    }

                    env_args += length;
                }
            }
        }
    }

    if (rb_mmtk_pre_arg_plan) {
        rb_mmtk_chosen_plan = rb_mmtk_pre_arg_plan;
    }
}

#define opt_match_arg(s, l, name) \
    opt_match(s, l, name) && (*(s) ? 1 : (rb_raise(rb_eRuntimeError, "--mmtk-" name " needs an argument"), 0))

void rb_mmtk_post_process_opts(const char *s) {
    const size_t l = strlen(s);
    if (l == 0) {
        return;
    }
    if (opt_match_arg(s, l, "plan")) {
        rb_mmtk_post_arg_plan = s + 1;
    }
    else if (opt_match_arg(s, l, "max-heap")) {
        rb_mmtk_post_max_heap_size = rb_mmtk_parse_heap_limit((char *) (s + 1), &rb_mmtk_max_heap_parse_error);
    }
    else {
        rb_raise(rb_eRuntimeError,
                 "invalid MMTk option `%s' (--help will show valid MMTk options)", s);
    }
}

void rb_mmtk_post_process_opts_finish(bool feature_enable) {
    if (feature_enable && !mmtk_enable) {
        rb_raise(rb_eRuntimeError, "--mmtk values disagree");
    }

    if (strcmp(rb_mmtk_pre_arg_plan ? rb_mmtk_pre_arg_plan : "", rb_mmtk_post_arg_plan ? rb_mmtk_post_arg_plan : "") != 0) {
        rb_raise(rb_eRuntimeError, "--mmtk-plan values disagree");
    }

    if (rb_mmtk_pre_max_heap_size != 0 && rb_mmtk_post_max_heap_size != 0 && rb_mmtk_pre_max_heap_size != rb_mmtk_post_max_heap_size) {
        rb_raise(rb_eRuntimeError, "--mmtk-max-heap values disagree");
    }

    if (rb_mmtk_max_heap_parse_error) {
        rb_raise(rb_eRuntimeError,
                "--mmtk-max-heap Invalid. Valid values positive integers, with optional KiB, MiB, GiB, TiB suffixes.");
    }
}

#endif // USE_MMTK
