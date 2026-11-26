#include "ruby/ruby.h"

void use(const char *p);
void escape(VALUE *slot);

/* s is stored to an escaped slot only when c is true. On the other path the
 * slot does not hold s at the GC call. Reported. */
VALUE
bad_conditionally_stored_to_slot(int c)
{
    VALUE slot = Qnil;
    escape(&slot);
    VALUE s = rb_str_new_cstr("x");
    if (c)
        slot = s;
    const char *p = RSTRING_PTR(s);
    rb_ary_new();
    use(p);
    return Qnil;
}

/* As in realpath_rec of CRuby: when cond() is true, link is reassigned and
 * the original is kept in link_orig. On each path one of the two slots holds
 * the original at the GC call. Not reported. */
VALUE other(VALUE v);
int cond(void);

VALUE
ok_kept_in_either_slot(long n)
{
    for (long i = 0; i < n; i++) {
        VALUE link_orig = Qnil;
        VALUE link = rb_str_new_cstr("x");
        const char *p = RSTRING_PTR(link);
        if (cond()) {
            link_orig = link;
            link = other(link);
        }
        rb_ary_new();
        use(p);
        RB_GC_GUARD(link_orig);
        RB_GC_GUARD(link);
    }
    return Qnil;
}

/* tmp is loaded from a before v is stored there, so b gets the old value of
 * a. Then a is overwritten, and v is in no slot at the GC call. Reported. */
void opaque(void);

VALUE
bad_copied_before_stored(VALUE old)
{
    VALUE a = old, b = Qnil;
    escape(&a);
    escape(&b);
    VALUE v = rb_str_new_cstr("x");
    const char *p = RSTRING_PTR(v);
    VALUE tmp = *(volatile VALUE *)&a;
    a = v;
    opaque();
    b = tmp;
    a = Qnil;
    rb_ary_new();
    use(p);
    return Qnil;
}

/* Stored on both paths, so the slot holds s at the GC call. Not reported
 * (boundary). */
VALUE
ok_stored_to_slot_on_both_paths(int c)
{
    VALUE slot = Qnil;
    escape(&slot);
    VALUE s = rb_str_new_cstr("x");
    if (c) {
        slot = s;
        use("a");
    }
    else {
        slot = s;
        use("b");
    }
    const char *p = RSTRING_PTR(s);
    rb_ary_new();
    use(p);
    return Qnil;
}
