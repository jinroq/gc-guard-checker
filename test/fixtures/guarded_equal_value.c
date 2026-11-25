#include "ruby/ruby.h"

/* cur == obj と分かった範囲では、コンパイラが obj の代わりに cur を
 * 使うことがある。obj が guard されていれば cur も生きているので報告しない
 * (concurrent_set_try_resize_without_locking の形。acquire load があると
 * obj はスロットから読み直され、GVN が cur の方を残す) */
VALUE ok_guarded_equal_value(VALUE *slot, VALUE obj)
{
    VALUE cur = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (cur != obj) return Qnil;
    const char *p = RSTRING_PTR(obj);
    rb_ary_new();
    char c = p[0];
    RB_GC_GUARD(obj);
    return INT2FIX(c);
}

/* 比較はしているが、等しくない側でポインタを取っている。cur の guard は
 * obj を守らないので報告する (境界) */
VALUE bad_guard_on_unequal_value(VALUE *slot, VALUE obj)
{
    VALUE cur = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (cur == obj) return Qnil;
    const char *p = RSTRING_PTR(cur);
    rb_ary_new();
    char c = p[0];
    RB_GC_GUARD(obj);
    return INT2FIX(c);
}
