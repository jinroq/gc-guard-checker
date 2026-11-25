#include "ruby/ruby.h"

/* rb_rescue などのコールバックでは、VALUE 型の引数に C の構造体への
 * ポインタを詰めて渡すことがある。Ruby オブジェクトではないので報告しない */
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

/* RTYPEDDATA_GET_DATA で得た C の構造体は Ruby オブジェクトが所有するので、
 * そのオブジェクトが死んでいれば報告する */
static const rb_data_type_t data_type = { "callback_arg", { 0, 0, 0, }, 0, 0, 0 };

VALUE bad_typed_data_used_after_gc(VALUE obj)
{
    struct callback_arg *a = RTYPEDDATA_GET_DATA(obj);
    rb_ary_new();
    return INT2FIX(a->ptr[0]);
}
