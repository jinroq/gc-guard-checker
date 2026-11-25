#include "ruby/ruby.h"

/* file.c の CheckPath と同じ形: RB_GC_GUARD(str) = ... は str のスロットへの
 * volatile な store になる。str は引数なのでスロットは関数全体で確保された
 * ままであり、後で読まれなくても GC 呼び出しの時点でスタック上に残っている */
static inline VALUE
to_string(VALUE obj, const char **cstr)
{
    VALUE s = rb_obj_as_string(obj);
    *cstr = RSTRING_PTR(s);
    return s;
}
#define CHECK_STRING(str, cstr) RB_GC_GUARD(str) = to_string(str, &cstr);

VALUE ok_guard_assignment(VALUE str)
{
    const char *p;
    CHECK_STRING(str, p);
    rb_ary_new();
    return INT2FIX(p[0]);
}

/* volatile に store した後、GC 呼び出しより前にスロットを上書きしている
 * ので報告する (境界) */
VALUE bad_guard_assignment_overwritten(VALUE str)
{
    const char *p;
    CHECK_STRING(str, p);
    str = Qnil;
    rb_ary_new();
    char c = p[0];
    RB_GC_GUARD(str);
    return INT2FIX(c);
}
