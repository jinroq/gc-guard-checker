#include "ruby/ruby.h"
#include "ruby/encoding.h"
#include "ruby/re.h"
#include <fcntl.h>

/* rb_gc_writebarrier, rb_errno_ptr and fcntl do not trigger GC, so it is not
 * reported. */
VALUE ok_known_non_gc_functions(VALUE str, VALUE a, VALUE b, int fd)
{
    const char *p = RSTRING_PTR(str);
    rb_gc_writebarrier(a, b);
    *rb_errno_ptr() = 0;
    fcntl(fd, F_GETFL);
    return INT2FIX(p[0]);
}

/* Lookups, sizes, the remembered set, and encoding and byte functions, which
 * do not trigger GC either. Not reported. (Internal ones are declared here.) */
struct rb_id_table;
int rb_id_table_lookup(struct rb_id_table *tbl, ID id, VALUE *valp);
void *rb_current_ec(void);
void rb_gc_writebarrier_remember(VALUE obj);

VALUE ok_lookups_and_encoding_functions(VALUE str, VALUE a, struct rb_id_table *tbl,
                                        st_table *st, OnigRegex reg, rb_encoding *enc)
{
    const char *p = RSTRING_PTR(str);
    VALUE v;
    int *nums;
    long n = rb_id_table_lookup(tbl, 1, &v) + (long)rb_st_table_size(st);
    rb_gc_writebarrier_remember(a);
    n += rb_current_ec() != 0;
    n += rb_enc_precise_mbclen(p, p + 1, enc) + rb_enc_ascget(p, p + 1, NULL, enc);
    n += rb_memsearch("a", 1, p, 1, enc) + rb_memcicmp(p, "a", 1);
    n += onig_name_to_group_numbers(reg, (const OnigUChar *)"a",
                                    (const OnigUChar *)"a" + 1, &nums);
    return INT2FIX(p[0] + n);
}

/* The list is matched exactly. A function with a merely similar name is
 * assumed to possibly trigger GC, so it is reported (boundary). */
void rb_gc_writebarrier_x(VALUE a, VALUE b);

VALUE bad_similar_name_to_non_gc_function(VALUE str, VALUE a, VALUE b)
{
    const char *p = RSTRING_PTR(str);
    rb_gc_writebarrier_x(a, b);
    return INT2FIX(p[0]);
}
