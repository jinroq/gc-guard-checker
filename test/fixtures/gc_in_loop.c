#include "ruby/ruby.h"

/* p is taken outside the loop and used after a GC call in the loop. str is
 * not used in the loop, so it is reported. */
VALUE bad_ptr_outside_loop(VALUE str, VALUE ary)
{
    const char *p = RSTRING_PTR(str);
    long n = RARRAY_LEN(ary);
    long sum = 0;
    for (long i = 0; i < n; i++) {
        rb_ary_new();
        sum += p[i];
    }
    return LONG2FIX(sum);
}

/* p is taken from str in every iteration, so str is live at the GC call for
 * the next iteration. After the last iteration str is no longer used, but
 * neither is p, so it is not reported. */
VALUE ok_ptr_reloaded_in_loop(VALUE str, VALUE ary)
{
    long n = RARRAY_LEN(ary);
    long sum = 0;
    for (long i = 0; i < n; i++) {
        const char *p = RSTRING_PTR(str);
        sum += p[i];
        rb_ary_new();
    }
    return LONG2FIX(sum);
}
