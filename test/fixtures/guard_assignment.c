#include "ruby/ruby.h"

/* Same shape as CheckPath in file.c: RB_GC_GUARD(str) = ... is a volatile
 * store to the slot of str. str is an argument, so its slot lives for the
 * whole function, and str stays on the stack at the GC call even if it is
 * not read later. */
static inline VALUE
to_string(VALUE obj, const char **cstr)
{
    VALUE s = rb_obj_as_string(obj);
    *cstr = RSTRING_PTR(s);
    return s;
}
#define CHECK_STRING(str, cstr) RB_GC_GUARD(str) = to_string(str, &cstr);

VALUE ok_guard_assignment(VALUE str)
{
    const char *p;
    CHECK_STRING(str, p);
    rb_ary_new();
    return INT2FIX(p[0]);
}

/* The slot is overwritten after the volatile store and before the GC call,
 * so it is reported (boundary). */
VALUE bad_guard_assignment_overwritten(VALUE str)
{
    const char *p;
    CHECK_STRING(str, p);
    str = Qnil;
    rb_ary_new();
    char c = p[0];
    RB_GC_GUARD(str);
    return INT2FIX(c);
}
