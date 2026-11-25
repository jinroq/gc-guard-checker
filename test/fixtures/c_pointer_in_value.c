#include "ruby/ruby.h"

/* Callbacks for rb_rescue and friends may receive a pointer to a C struct
 * disguised as a VALUE. It is not a Ruby object, so it is not reported. */
struct callback_arg {
    const char *ptr;
    long len;
};

VALUE ok_c_pointer_in_value(VALUE arg)
{
    struct callback_arg *a = (struct callback_arg *)arg;
    const char *p = a->ptr;
    rb_ary_new();
    return INT2FIX(p[0]);
}

/* A C struct obtained with RTYPEDDATA_GET_DATA is owned by the Ruby object,
 * so it is reported when the object is dead. */
static const rb_data_type_t data_type = { "callback_arg", { 0, 0, 0, }, 0, 0, 0 };

VALUE bad_typed_data_used_after_gc(VALUE obj)
{
    struct callback_arg *a = RTYPEDDATA_GET_DATA(obj);
    rb_ary_new();
    return INT2FIX(a->ptr[0]);
}
