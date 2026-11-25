#include "ruby/ruby.h"

/* RB_GC_GUARD(str) comes before the GC call, but the guard passes the
 * address of the slot of str to an asm (it escapes), so the compiler must
 * keep the slot up to date before later calls. The slot of an argument is
 * not reused until the function returns, so str is still on the stack at
 * rb_ary_new(). Not reported (boundary: position of the guard). */
VALUE ok_guard_before_gc(VALUE str)
{
    const char *p = RSTRING_PTR(str);
    RB_GC_GUARD(str);
    rb_ary_new();
    return INT2FIX(p[0]);
}

/* Even if a variable in a block is guarded, its lifetime ends when leaving
 * the block and the slot may be reused. It is not protected at a later GC
 * call, so it is reported. */
VALUE bad_guarded_var_out_of_scope(VALUE obj)
{
    const char *p;
    {
        VALUE tmp = rb_obj_as_string(obj);
        p = RSTRING_PTR(tmp);
        RB_GC_GUARD(tmp);
    }
    rb_ary_new();
    return INT2FIX(p[0]);
}
