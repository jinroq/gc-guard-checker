#include "ruby/ruby.h"
#include <fcntl.h>

/* 分岐の多い関数でも、到達可能性を正しく判定する。LLVM の
 * isPotentiallyReachable は探索するブロック数に上限があり、超えると
 * 「到達しうる」と答えてしまうので、上限の無い判定が必要になる */
#define IF1(i) if (flags & (1UL << ((i) % 64))) fcntl(fd, F_GETFL);
#define IF10(i) IF1(i##0) IF1(i##1) IF1(i##2) IF1(i##3) IF1(i##4) \
                IF1(i##5) IF1(i##6) IF1(i##7) IF1(i##8) IF1(i##9)

static inline VALUE
to_string(VALUE obj, const char **cstr)
{
    VALUE s = rb_obj_as_string(obj);
    *cstr = RSTRING_PTR(s);
    return s;
}

/* str のスロットへの最初の store は volatile な store より前にしか無いので、
 * GC 呼び出しまでにスロットが上書きされることは無い。報告しない */
VALUE ok_large_function(VALUE str, unsigned long flags, int fd)
{
    const char *p;
    RB_GC_GUARD(str) = to_string(str, &p);
    IF10(1) IF10(2) IF10(3) IF10(4) IF10(5)
    rb_ary_new();
    return INT2FIX(p[0]);
}
