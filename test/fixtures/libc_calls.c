#include "ruby/ruby.h"
#include <stdlib.h>
#include <sys/stat.h>

/* libc functions such as fstat never call back into Ruby, so they do not
 * trigger GC. */
VALUE ok_libc_call_between(VALUE str, int fd)
{
    struct stat st;
    const char *p = RSTRING_PTR(str);
    if (fstat(fd, &st) == -1) return Qnil;
    return INT2FIX(p[0] + st.st_size);
}

/* A call that receives a callback, like qsort, may trigger GC through the
 * callback, so it is reported (boundary). */
int compare_with_gc(const void *a, const void *b);

VALUE bad_libc_call_with_callback(VALUE str, void *base, size_t n)
{
    const char *p = RSTRING_PTR(str);
    qsort(base, n, 1, compare_with_gc);
    return INT2FIX(p[0]);
}
