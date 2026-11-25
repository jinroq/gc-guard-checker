#include "ruby/ruby.h"

static inline void
maybe_replace(VALUE *xp)
{
    if (RSTRING_LEN(*xp) == 0) *xp = rb_str_new_cstr("z");
}

/* &x is taken, so x lives on the stack and v is loaded from there. The guard
 * refers to the slot later, so it is not reported. */
VALUE ok_guard_on_reloaded_var(VALUE x)
{
    maybe_replace(&x);
    const char *p = RSTRING_PTR(x);
    rb_ary_new();
    char c = p[0];
    RB_GC_GUARD(x);
    return INT2FIX(c);
}

/* After the swap, x is a phi of values from different slots depending on the
 * path, but the slot of x holds it on every path. Not reported (the shape in
 * rb_big_and). */
VALUE ok_guard_after_swap(VALUE x, VALUE y, VALUE flag)
{
    maybe_replace(&x);
    maybe_replace(&y);
    if (RTEST(flag)) {
        VALUE t = x; x = y; y = t;
    }
    const char *p = RSTRING_PTR(x);
    rb_ary_new();
    char c = p[0];
    RB_GC_GUARD(x);
    RB_GC_GUARD(y);
    return INT2FIX(c);
}

/* The slot is overwritten with another value after v is loaded from it. The
 * slot does not hold v at the GC call, so it is reported. */
VALUE bad_slot_overwritten_before_gc(VALUE x)
{
    maybe_replace(&x);
    const char *p = RSTRING_PTR(x);
    x = Qnil;
    rb_ary_new();
    char c = p[0];
    RB_GC_GUARD(x);
    return INT2FIX(c);
}
