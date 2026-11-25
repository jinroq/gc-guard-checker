#include "ruby/ruby.h"

/* Only one branch has a call that may trigger GC. str is dead on that path,
 * so it is reported. */
VALUE bad_gc_in_one_branch(VALUE str, VALUE flag)
{
    const char *p = RSTRING_PTR(str);
    if (RTEST(flag)) {
        rb_ary_new();
    }
    return INT2FIX(p[0]);
}
