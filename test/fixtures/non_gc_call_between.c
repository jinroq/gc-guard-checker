#include "ruby/ruby.h"
#include <string.h>

/* strlen in between only reads memory and does not trigger GC, so it is not
 * reported. */
VALUE ok_non_gc_call_between(VALUE str, const char *s)
{
    const char *p = RSTRING_PTR(str);
    size_t n = strlen(s);
    return INT2FIX(p[n]);
}
