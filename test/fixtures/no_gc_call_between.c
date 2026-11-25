#include "ruby/ruby.h"

/* p の取得から使用までの間に GC を起こしうる呼び出しが無いので報告しない */
VALUE ok_no_gc_call_between(VALUE str)
{
    const char *p = RSTRING_PTR(str);
    return INT2FIX(p[0]);
}
