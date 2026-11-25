#include "ruby/ruby.h"

/* str の最後の使用は RSTRING_PTR。rb_ary_new() で GC が走ると str は
 * 回収され得るのに、その後で p を読んでいるので報告する */
VALUE bad_ptr_used_after_gc(VALUE str)
{
    const char *p = RSTRING_PTR(str);
    rb_ary_new();
    return INT2FIX(p[0]);
}
