#include "ruby/ruby.h"

/* VALUE から派生ポインタを取らないので報告しない */
VALUE ok_no_derived_pointer(VALUE ary)
{
    rb_ary_push(ary, rb_ary_new());
    return ary;
}
