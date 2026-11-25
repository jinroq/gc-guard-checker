#include "ruby/ruby.h"

/* 片方の分岐だけで GC を起こしうる呼び出しがある。その経路では str が
 * 死んでいるので報告する */
VALUE bad_gc_in_one_branch(VALUE str, VALUE flag)
{
    const char *p = RSTRING_PTR(str);
    if (RTEST(flag)) {
        rb_ary_new();
    }
    return INT2FIX(p[0]);
}
