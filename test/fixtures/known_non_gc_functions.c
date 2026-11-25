#include "ruby/ruby.h"
#include <fcntl.h>

/* rb_gc_writebarrier / rb_errno_ptr / fcntl は GC を起こさないので報告しない */
VALUE ok_known_non_gc_functions(VALUE str, VALUE a, VALUE b, int fd)
{
    const char *p = RSTRING_PTR(str);
    rb_gc_writebarrier(a, b);
    *rb_errno_ptr() = 0;
    fcntl(fd, F_GETFL);
    return INT2FIX(p[0]);
}

/* 一覧は完全一致で照合する。名前が似ているだけの関数は GC を起こしうる
 * とみなして報告する (境界) */
void rb_gc_writebarrier_x(VALUE a, VALUE b);

VALUE bad_similar_name_to_non_gc_function(VALUE str, VALUE a, VALUE b)
{
    const char *p = RSTRING_PTR(str);
    rb_gc_writebarrier_x(a, b);
    return INT2FIX(p[0]);
}
