#include "ruby/ruby.h"

/* RB_GC_GUARD(str) の位置が早すぎる。rb_ary_new() の時点では str の
 * 使用がもう無いので報告する (境界: guard の位置) */
VALUE bad_guard_before_gc(VALUE str)
{
    const char *p = RSTRING_PTR(str);
    RB_GC_GUARD(str);
    rb_ary_new();
    return INT2FIX(p[0]);
}
