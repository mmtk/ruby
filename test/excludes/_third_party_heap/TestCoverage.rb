exclude :test_clear_with_methods, "Coverage.peek_result depends on Objspace, which is GC-implementation dependent"
exclude :test_method_coverage, "Coverage.peek_result depends on Objspace, which is GC-implementation dependent"
exclude :test_method_coverage_for_alias, "Coverage.peek_result depends on Objspace, which is GC-implementation dependent"
exclude :test_method_coverage_for_singleton_class, "Coverage.peek_result depends on Objspace, which is GC-implementation dependent"
exclude :test_method_coverage_for_define_method, "Coverage.peek_result depends on Objspace, which is GC-implementation dependent"