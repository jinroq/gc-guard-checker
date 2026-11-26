#include "ruby/ruby.h"

void use_value(VALUE v);

/* v merges a new string and a constant by a phi. Constants have no use lists,
 * so the analysis of the stack slots must skip them. v is not kept alive, so
 * it is reported. */
VALUE
bad_phi_with_constant_incoming(int c, int d)
{
    VALUE v = c ? rb_str_new_cstr("x") : Qfalse;
    if (d)
        use_value(v);
    if (!RTEST(v))
        return Qnil;
    const char *p = RSTRING_PTR(v);
    rb_ary_new();
    return INT2FIX(p[0]);
}
