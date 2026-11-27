#include "ruby/ruby.h"

void opaque(void);

/* As rb_string_value in string.c, which is inlined there: the converted value
 * is stored through a pointer to volatile, and the address does not escape. */
static inline VALUE
string_value(volatile VALUE *ptr)
{
    VALUE s = *ptr;
    if (!RB_TYPE_P(s, T_STRING)) {
        s = rb_str_to_str(s);
        *ptr = s;
    }
    return s;
}

/* The slot of obj is accessed with volatile loads and stores, so it is in
 * memory, and obj loaded from it after the call in between (as in
 * rb_str_crypt) is in the slot at the GC call. Not reported. */
VALUE
ok_value_in_volatile_slot(VALUE obj)
{
    string_value(&obj);
    opaque();
    const char *p = RSTRING_PTR(obj);
    rb_ary_new();
    return INT2FIX(p[0]);
}

/* The slot is overwritten after the pointer is taken, so it does not hold the
 * value at the GC call. Reported (boundary). */
VALUE
bad_volatile_slot_overwritten(VALUE obj)
{
    string_value(&obj);
    opaque();
    const char *p = RSTRING_PTR(obj);
    obj = Qnil;
    rb_ary_new();
    return INT2FIX(p[0] + RTEST(obj));
}
