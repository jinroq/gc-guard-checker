#include "ruby/ruby.h"
#include <fcntl.h>

/* rb_gc_writebarrier, rb_errno_ptr and fcntl do not trigger GC, so it is not
 * reported. */
VALUE ok_known_non_gc_functions(VALUE str, VALUE a, VALUE b, int fd)
{
    const char *p = RSTRING_PTR(str);
    rb_gc_writebarrier(a, b);
    *rb_errno_ptr() = 0;
    fcntl(fd, F_GETFL);
    return INT2FIX(p[0]);
}

/* The list is matched exactly. A function with a merely similar name is
 * assumed to possibly trigger GC, so it is reported (boundary). */
void rb_gc_writebarrier_x(VALUE a, VALUE b);

VALUE bad_similar_name_to_non_gc_function(VALUE str, VALUE a, VALUE b)
{
    const char *p = RSTRING_PTR(str);
    rb_gc_writebarrier_x(a, b);
    return INT2FIX(p[0]);
}
