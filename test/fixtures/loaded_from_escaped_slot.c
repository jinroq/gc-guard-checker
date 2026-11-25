#include "ruby/ruby.h"

/* The address of the slot of s escapes with RB_GC_GUARD(s) = ..., as
 * FilePathValue does. A value assigned later with s = ... (a plain store)
 * also stays on the stack while the slot lives and is not overwritten. v is
 * loaded from the slot and is still there at the GC call, so it is not
 * reported (the shape of basedir in rb_check_realpath_emulate in file.c). */
VALUE ok_loaded_from_escaped_slot(VALUE s, VALUE flag)
{
    RB_GC_GUARD(s) = rb_obj_as_string(s);
    if (RTEST(flag)) s = rb_str_new_frozen(s);
    const char *p = RSTRING_PTR(s);
    rb_ary_new();
    return INT2FIX(p[0]);
}

/* The slot is overwritten after v is loaded from it and before the GC call,
 * so it is reported (boundary). */
VALUE bad_loaded_then_slot_overwritten(VALUE s, VALUE flag)
{
    RB_GC_GUARD(s) = rb_obj_as_string(s);
    if (RTEST(flag)) s = rb_str_new_frozen(s);
    const char *p = RSTRING_PTR(s);
    s = Qnil;
    rb_ary_new();
    char c = p[0];
    RB_GC_GUARD(s);
    return INT2FIX(c);
}
