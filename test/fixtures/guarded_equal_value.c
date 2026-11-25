#include "ruby/ruby.h"

/* Where cur == obj is known, the compiler may use cur instead of obj. obj is
 * guarded, so cur is live too and it is not reported (the shape in
 * concurrent_set_try_resize_without_locking: with an acquire load, obj is
 * reloaded from its slot and GVN keeps cur). */
VALUE ok_guarded_equal_value(VALUE *slot, VALUE obj)
{
    VALUE cur = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (cur != obj) return Qnil;
    const char *p = RSTRING_PTR(obj);
    rb_ary_new();
    char c = p[0];
    RB_GC_GUARD(obj);
    return INT2FIX(c);
}

/* The values are compared, but the pointer is taken on the unequal side. The
 * guard of obj does not protect cur, so it is reported (boundary). */
VALUE bad_guard_on_unequal_value(VALUE *slot, VALUE obj)
{
    VALUE cur = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (cur == obj) return Qnil;
    const char *p = RSTRING_PTR(cur);
    rb_ary_new();
    char c = p[0];
    RB_GC_GUARD(obj);
    return INT2FIX(c);
}
