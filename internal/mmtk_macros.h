#ifndef INTERNAL_MMTK_MACROS_H                                 /*-*-C-*-vi:se ft=c:*/
#define INTERNAL_MMTK_MACROS_H

#include "ruby/internal/config.h"

#if USE_MMTK
#define IF_USE_MMTK(a) a
#define IF_NOT_USE_MMTK(a)
#define IF_USE_MMTK2(a, b) a
#define WHEN_USING_MMTK(a) if (rb_mmtk_enabled_p()) { a }
#define WHEN_NOT_USING_MMTK(a) if (!rb_mmtk_enabled_p()) { a }
#define WHEN_USING_MMTK2(a, b) if (rb_mmtk_enabled_p()) { a } else { b }
#define WHEN_USING_MMTK_CONDITIONAL(cond, a) if (!rb_mmtk_enabled_p() || (cond)) { a }
#else
#define IF_USE_MMTK(a)
#define IF_NOT_USE_MMTK(a) a
#define IF_USE_MMTK2(a, b) b
#define WHEN_USING_MMTK(a)
#define WHEN_NOT_USING_MMTK(a) a
#define WHEN_USING_MMTK2(a, b) b
#define WHEN_USING_MMTK_CONDITIONAL(cond, a) a
#endif

#endif /* INTERNAL_MMTK_MACROS_H */
