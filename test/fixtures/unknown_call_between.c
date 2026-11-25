#include "ruby/ruby.h"

void some_unknown_function(void);

/* An external function whose body is unknown is assumed to possibly trigger
 * GC, so it is reported. */
VALUE bad_unknown_call_between(VALUE str)
{
    const char *p = RSTRING_PTR(str);
    some_unknown_function();
    return INT2FIX(p[0]);
}
