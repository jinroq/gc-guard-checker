#include "ruby/ruby.h"

/* p の最後の使用より後に RB_GC_GUARD(str) があるので、rb_ary_new() の
 * 時点で str は生きている。報告しない */
VALUE ok_guard_after_use(VALUE str)
{
    const char *p = RSTRING_PTR(str);
    rb_ary_new();
    VALUE r = INT2FIX(p[0]);
    RB_GC_GUARD(str);
    return r;
}
