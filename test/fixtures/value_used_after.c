#include "ruby/ruby.h"

/* str itself is used after p, so str is live at rb_ary_new(). Not reported. */
VALUE ok_value_used_after(VALUE str)
{
    const char *p = RSTRING_PTR(str);
    rb_ary_new();
    char c = p[0];
    return rb_str_cat(str, &c, 1);
}
