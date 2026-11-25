#include "ruby/ruby.h"

/* The last use of p is before rb_ary_new(), so it is not reported
 * (boundary). */
VALUE ok_ptr_last_use_before_gc(VALUE str)
{
    const char *p = RSTRING_PTR(str);
    char c = p[0];
    rb_ary_new();
    return INT2FIX(c);
}
