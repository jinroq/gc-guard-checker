#include "ruby/ruby.h"
#include <stdlib.h>
#include <sys/stat.h>

/* fstat などの libc 関数は Ruby のコードを呼び返さないので GC を起こさない */
VALUE ok_libc_call_between(VALUE str, int fd)
{
    struct stat st;
    const char *p = RSTRING_PTR(str);
    if (fstat(fd, &st) == -1) return Qnil;
    return INT2FIX(p[0] + st.st_size);
}

/* qsort のようにコールバックを受け取る呼び出しは、コールバックが GC を
 * 起こしうるので報告する (境界) */
int compare_with_gc(const void *a, const void *b);

VALUE bad_libc_call_with_callback(VALUE str, void *base, size_t n)
{
    const char *p = RSTRING_PTR(str);
    qsort(base, n, 1, compare_with_gc);
    return INT2FIX(p[0]);
}
