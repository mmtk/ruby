# frozen_string_literal: true

# GC.latest_gc_info doesn't return anything
MSpec.register(:exclude, "CApiGCSpecs rb_gc_latest_gc_info returns a value when symbol is given")
MSpec.register(:exclude, "CApiGCSpecs rb_gc_latest_gc_info returns the populated hash when a hash is given")
# Testing behaviour specific to default GC
MSpec.register(:exclude, "GC.stat increases major_gc_count after GC is run")
MSpec.register(:exclude, "GC.stat provides some number for heap_free_slots")
# GC::Profiler is not implemented
MSpec.register(:exclude, "GC::Profiler.disable disables the profiler")
MSpec.register(:exclude, "GC::Profiler.enable enables the profiler")
MSpec.register(:exclude, "GC::Profiler.enabled? reports as disabled when disabled")
MSpec.register(:exclude, "GC::Profiler.enabled? reports as enabled when enabled")
MSpec.register(:exclude, "GC::Profiler.result returns a string")
MSpec.register(:exclude, "GC::Profiler.total_time returns an float")
