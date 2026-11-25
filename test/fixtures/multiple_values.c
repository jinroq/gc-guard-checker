#include "ruby/ruby.h"

/* a is guarded but b is not. Only b is reported. */
VALUE bad_one_of_two_values(VALUE a, VALUE b)
{
    const char *pa = RSTRING_PTR(a);
    const char *pb = RSTRING_PTR(b);
    rb_ary_new();
    VALUE r = INT2FIX(pa[0] + pb[0]);
    RB_GC_GUARD(a);
    return r;
}
