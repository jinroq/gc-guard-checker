#include "ruby/ruby.h"
#include <string.h>

/* rb_str_new allocates the object and then copies from p. If tmp is
 * collected by GC during the allocation, it copies from freed memory, so it
 * is reported. */
VALUE bad_ptr_passed_to_allocating_call(VALUE obj)
{
    VALUE tmp = rb_obj_as_string(obj);
    return rb_str_new(RSTRING_PTR(tmp), RSTRING_LEN(tmp));
}

/* tmp is guarded after the call, so it is not reported. */
VALUE ok_ptr_passed_with_guard(VALUE obj)
{
    VALUE tmp = rb_obj_as_string(obj);
    VALUE r = rb_str_new(RSTRING_PTR(tmp), RSTRING_LEN(tmp));
    RB_GC_GUARD(tmp);
    return r;
}

/* v itself is passed to the same call, so the callee holds v. Not reported. */
VALUE ok_value_passed_to_same_call(VALUE obj)
{
    VALUE tmp = rb_obj_as_string(obj);
    return rb_str_cat(tmp, RSTRING_PTR(tmp), 1);
}

/* p is passed to memcpy, which reads it, after a GC call, so it is reported. */
VALUE bad_ptr_passed_to_memcpy_after_gc(VALUE obj, char *buf, long n)
{
    VALUE tmp = rb_obj_as_string(obj);
    const char *p = RSTRING_PTR(tmp);
    rb_ary_new();
    memcpy(buf, p, n);
    return Qnil;
}

/* p is passed to strlen, which does not GC, before a GC call, and p is not
 * used after that, so it is not reported (boundary). */
VALUE ok_ptr_passed_before_gc(VALUE obj)
{
    VALUE tmp = rb_obj_as_string(obj);
    size_t n = strlen(RSTRING_PTR(tmp));
    rb_ary_new();
    return LONG2FIX(n);
}
