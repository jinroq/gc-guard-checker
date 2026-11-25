#include "ruby/ruby.h"

/* p を使った後で str 自体も使っているので、rb_ary_new() の時点で str は
 * 生きている。報告しない */
VALUE ok_value_used_after(VALUE str)
{
    const char *p = RSTRING_PTR(str);
    rb_ary_new();
    char c = p[0];
    return rb_str_cat(str, &c, 1);
}
