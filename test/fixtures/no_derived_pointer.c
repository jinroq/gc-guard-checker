#include "ruby/ruby.h"

/* No pointer is derived from a VALUE, so nothing is reported. */
VALUE ok_no_derived_pointer(VALUE ary)
{
    rb_ary_push(ary, rb_ary_new());
    return ary;
}
