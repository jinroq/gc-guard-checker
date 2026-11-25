#include "ruby/ruby.h"

/* RB_GC_GUARD(str) comes after the last use of p, so str is live at
 * rb_ary_new(). Not reported. */
VALUE ok_guard_after_use(VALUE str)
{
    const char *p = RSTRING_PTR(str);
    rb_ary_new();
    VALUE r = INT2FIX(p[0]);
    RB_GC_GUARD(str);
    return r;
}
