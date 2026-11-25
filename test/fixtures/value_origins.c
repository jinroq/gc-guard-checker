#include "ruby/ruby.h"

/* Reports carry the origin of v, used to prioritize triage. */

/* Return value of a call: likely referenced only from this frame. */
VALUE bad_origin_call(VALUE obj)
{
    VALUE str = rb_obj_as_string(obj);
    const char *p = RSTRING_PTR(str);
    rb_ary_new();
    return INT2FIX(p[0]);
}

/* Argument: often reachable from the caller, e.g. the VM stack. */
VALUE bad_origin_argument(VALUE str)
{
    const char *p = RSTRING_PTR(str);
    rb_ary_new();
    return INT2FIX(p[0]);
}

/* Loaded from memory: often reachable from where it was loaded. */
VALUE bad_origin_loaded(VALUE *slot)
{
    VALUE str = *slot;
    const char *p = RSTRING_PTR(str);
    rb_ary_new();
    return INT2FIX(p[0]);
}

/* A phi with a call result among its incoming values counts as call. */
VALUE bad_origin_phi_with_call(VALUE obj, VALUE flag)
{
    VALUE str = RTEST(flag) ? rb_obj_as_string(obj) : obj;
    const char *p = RSTRING_PTR(str);
    rb_ary_new();
    return INT2FIX(p[0]);
}
