#include "ruby/ruby.h"
#include <fcntl.h>

/* Reachability must be correct even in a function with many branches. LLVM's
 * isPotentiallyReachable gives up after exploring a limited number of blocks
 * and answers "reachable", so an unbounded check is needed. */
#define IF1(i) if (flags & (1UL << ((i) % 64))) fcntl(fd, F_GETFL);
#define IF10(i) IF1(i##0) IF1(i##1) IF1(i##2) IF1(i##3) IF1(i##4) \
                IF1(i##5) IF1(i##6) IF1(i##7) IF1(i##8) IF1(i##9)

static inline VALUE
to_string(VALUE obj, const char **cstr)
{
    VALUE s = rb_obj_as_string(obj);
    *cstr = RSTRING_PTR(s);
    return s;
}

/* The only other store to the slot of str is before the volatile store, so
 * the slot is not overwritten before the GC call. Not reported. */
VALUE ok_large_function(VALUE str, unsigned long flags, int fd)
{
    const char *p;
    RB_GC_GUARD(str) = to_string(str, &p);
    IF10(1) IF10(2) IF10(3) IF10(4) IF10(5)
    rb_ary_new();
    return INT2FIX(p[0]);
}
