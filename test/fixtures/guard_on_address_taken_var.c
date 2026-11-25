#include "ruby/ruby.h"

static inline void
maybe_replace(VALUE *xp)
{
    if (RSTRING_LEN(*xp) == 0) *xp = rb_str_new_cstr("z");
}

/* x は &x を取られているのでスタック上に置かれ、v はそこから load される。
 * guard がそのスロットを後で参照するので報告しない */
VALUE ok_guard_on_reloaded_var(VALUE x)
{
    maybe_replace(&x);
    const char *p = RSTRING_PTR(x);
    rb_ary_new();
    char c = p[0];
    RB_GC_GUARD(x);
    return INT2FIX(c);
}

/* swap 後の x は経路によって別のスロットから来た値の phi になるが、
 * どの経路でも x のスロットが保持している。報告しない (rb_big_and の形) */
VALUE ok_guard_after_swap(VALUE x, VALUE y, VALUE flag)
{
    maybe_replace(&x);
    maybe_replace(&y);
    if (RTEST(flag)) {
        VALUE t = x; x = y; y = t;
    }
    const char *p = RSTRING_PTR(x);
    rb_ary_new();
    char c = p[0];
    RB_GC_GUARD(x);
    RB_GC_GUARD(y);
    return INT2FIX(c);
}

/* スロットから load した後で、そのスロットを別の値で上書きしている。
 * GC 呼び出しの時点ではスロットは v を保持していないので報告する */
VALUE bad_slot_overwritten_before_gc(VALUE x)
{
    maybe_replace(&x);
    const char *p = RSTRING_PTR(x);
    x = Qnil;
    rb_ary_new();
    char c = p[0];
    RB_GC_GUARD(x);
    return INT2FIX(c);
}
