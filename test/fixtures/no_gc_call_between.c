#include "ruby/ruby.h"

/* No call that may trigger GC between taking p and using it, so it is not
 * reported. */
VALUE ok_no_gc_call_between(VALUE str)
{
    const char *p = RSTRING_PTR(str);
    return INT2FIX(p[0]);
}
