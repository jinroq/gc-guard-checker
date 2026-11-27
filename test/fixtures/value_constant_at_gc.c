#include "ruby/ruby.h"

/* As in rb_file_expand_path_internal of CRuby: the GC call is only on the
 * path where str is 0 (Qfalse), where it is not an object, so it does not
 * matter whether str is kept there. Not reported. */
long
ok_value_constant_at_gc(VALUE str, int c)
{
    const char *p = "";
    if (c)
        p = RSTRING_PTR(str);
    if (!str)
        rb_ary_new();
    return p[0];
}

/* The GC call is on the path where str is not 0. Reported (boundary). */
long
bad_value_not_constant_at_gc(VALUE str, int c)
{
    const char *p = "";
    if (c)
        p = RSTRING_PTR(str);
    if (str)
        rb_ary_new();
    return p[0];
}
