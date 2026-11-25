#include "ruby/ruby.h"

/* p の最後の使用が rb_ary_new() より前なので報告しない (境界) */
VALUE ok_ptr_last_use_before_gc(VALUE str)
{
    const char *p = RSTRING_PTR(str);
    char c = p[0];
    rb_ary_new();
    return INT2FIX(c);
}
