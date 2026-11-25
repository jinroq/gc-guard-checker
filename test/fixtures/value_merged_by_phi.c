#include "ruby/ruby.h"

/* s は分岐ごとに別の値として作られ、phi で合流してから使われる。
 * 合流後の値が生きていれば元の値も生きているので報告しない
 * (rb_ary_reject で rb_ary_new がインライン展開されたときに起きる形) */
VALUE ok_value_merged_by_phi(VALUE flag)
{
    VALUE s;
    if (RTEST(flag)) {
        s = rb_str_new_cstr("a");
        if (RSTRING_LEN(s) == 0) return Qnil;
    }
    else {
        s = rb_str_new_cstr("b");
    }
    const char *p = RSTRING_PTR(s);
    rb_ary_new();
    char c = p[0];
    return rb_str_cat(s, &c, 1);
}
