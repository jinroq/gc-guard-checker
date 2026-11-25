#include "ruby/ruby.h"
#include <string.h>

/* 間にある strlen はメモリを読むだけで GC を起こさないので報告しない */
VALUE ok_non_gc_call_between(VALUE str, const char *s)
{
    const char *p = RSTRING_PTR(str);
    size_t n = strlen(s);
    return INT2FIX(p[n]);
}
