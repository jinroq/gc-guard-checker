#include "ruby/ruby.h"

/* s は FilePathValue と同じ RB_GC_GUARD(s) = ... でスロットの番地が逃げて
 * いる。その後の s = ... (普通の store) の値も、スロットが上書きされず
 * 生きている間はスタック上に残る。v はスロットから load した値になるが、
 * GC 呼び出しの時点でもスロットにあるので報告しない
 * (file.c の rb_check_realpath_emulate の basedir の形) */
VALUE ok_loaded_from_escaped_slot(VALUE s, VALUE flag)
{
    RB_GC_GUARD(s) = rb_obj_as_string(s);
    if (RTEST(flag)) s = rb_str_new_frozen(s);
    const char *p = RSTRING_PTR(s);
    rb_ary_new();
    return INT2FIX(p[0]);
}

/* スロットから load した後、GC 呼び出しの前にスロットを上書きしているので
 * 報告する (境界) */
VALUE bad_loaded_then_slot_overwritten(VALUE s, VALUE flag)
{
    RB_GC_GUARD(s) = rb_obj_as_string(s);
    if (RTEST(flag)) s = rb_str_new_frozen(s);
    const char *p = RSTRING_PTR(s);
    s = Qnil;
    rb_ary_new();
    char c = p[0];
    RB_GC_GUARD(s);
    return INT2FIX(c);
}
