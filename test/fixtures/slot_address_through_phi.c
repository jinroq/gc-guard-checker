#include "ruby/ruby.h"

VALUE use_values(long n, const VALUE *p);

/* As in rb_struct_new of CRuby: the address of tmpargs reaches a call
 * through a phi (mem), so it escapes, and the store of the hidden array to
 * tmpargs[0] stays. The array is in the slot at the call, which receives a
 * pointer into it. Not reported. */
VALUE
ok_slot_address_through_phi(long n)
{
    VALUE tmpargs[4], *mem = tmpargs;
    if (n > 4) {
        tmpargs[0] = rb_ary_hidden_new(n);
        mem = RARRAY_PTR(tmpargs[0]);
    }
    for (long i = 0; i < n; i++)
        mem[i] = INT2FIX(i);
    return use_values(n, mem);
}

/* The address of tmpargs does not escape, so the slot may be promoted to a
 * register and the array is not kept. Reported (boundary). */
VALUE
bad_slot_address_not_escaping(long n)
{
    VALUE tmpargs[4];
    tmpargs[0] = rb_ary_hidden_new(n);
    VALUE *mem = RARRAY_PTR(tmpargs[0]);
    for (long i = 0; i < n; i++)
        mem[i] = INT2FIX(i);
    return use_values(n, mem);
}
