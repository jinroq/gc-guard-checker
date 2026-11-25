#include "ruby/ruby.h"

/* RB_GC_GUARD(str) は GC 呼び出しより前にあるが、guard で str のスロットの
 * 番地が asm に渡る (escape する) ので、コンパイラはその後の呼び出しの前に
 * スロットを最新に保たなければならない。引数のスロットは関数の終わりまで
 * 再利用されないので、rb_ary_new() の時点でも str はスタック上にある。
 * 報告しない (境界: guard の位置) */
VALUE ok_guard_before_gc(VALUE str)
{
    const char *p = RSTRING_PTR(str);
    RB_GC_GUARD(str);
    rb_ary_new();
    return INT2FIX(p[0]);
}

/* ブロックの中の変数を guard しても、ブロックを抜けると lifetime が終わり、
 * スロットは再利用されうる。その後の GC 呼び出しでは守られないので報告する */
VALUE bad_guarded_var_out_of_scope(VALUE obj)
{
    const char *p;
    {
        VALUE tmp = rb_obj_as_string(obj);
        p = RSTRING_PTR(tmp);
        RB_GC_GUARD(tmp);
    }
    rb_ary_new();
    return INT2FIX(p[0]);
}
