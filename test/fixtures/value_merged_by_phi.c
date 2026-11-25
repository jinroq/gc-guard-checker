#include "ruby/ruby.h"

/* s is created as a different value on each branch and used after they are
 * merged by a phi. If the merged value is live, so is the original one, so
 * it is not reported (the shape when rb_ary_new is inlined into
 * rb_ary_reject). */
VALUE ok_value_merged_by_phi(VALUE flag)
{
    VALUE s;
    if (RTEST(flag)) {
        s = rb_str_new_cstr("a");
        if (RSTRING_LEN(s) == 0) return Qnil;
    }
    else {
        s = rb_str_new_cstr("b");
    }
    const char *p = RSTRING_PTR(s);
    rb_ary_new();
    char c = p[0];
    return rb_str_cat(s, &c, 1);
}
