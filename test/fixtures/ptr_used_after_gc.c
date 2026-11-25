#include "ruby/ruby.h"

/* The last use of str is RSTRING_PTR. str may be collected if GC runs in
 * rb_ary_new(), yet p is read after that, so it is reported. */
VALUE bad_ptr_used_after_gc(VALUE str)
{
    const char *p = RSTRING_PTR(str);
    rb_ary_new();
    return INT2FIX(p[0]);
}
