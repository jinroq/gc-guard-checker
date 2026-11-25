#include "ruby/ruby.h"

void some_unknown_function(void);

/* 中身の分からない外部関数は GC を起こしうるとみなして報告する */
VALUE bad_unknown_call_between(VALUE str)
{
    const char *p = RSTRING_PTR(str);
    some_unknown_function();
    return INT2FIX(p[0]);
}
