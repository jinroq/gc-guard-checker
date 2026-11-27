#include "ruby/ruby.h"

/* As in pack_pack of CRuby: from is converted with StringValue, so its slot
 * stays in memory. In the next iteration, from is overwritten, and when it is
 * nil the call receives "" rather than a pointer into the string, so the
 * string being out of the slot there does not matter. On the path where the
 * call receives a pointer into the string, the slot holds it. Not reported. */
VALUE
ok_slot_overwritten_on_path_without_pointer(VALUE ary, VALUE res)
{
    VALUE from;
    long n = RARRAY_LEN(ary);
    for (long i = 0; i < n; i++) {
        const char *ptr;
        long len;
        from = RARRAY_AREF(ary, i);
        if (NIL_P(from)) {
            ptr = "";
            len = 0;
        }
        else {
            StringValue(from);
            ptr = RSTRING_PTR(from);
            len = RSTRING_LEN(from);
        }
        rb_str_cat(res, ptr, len);
    }
    return res;
}

/* When from is nil, the pointer into the string of a previous iteration is
 * used again, while the slot has been overwritten with nil. Reported
 * (boundary). */
VALUE
bad_pointer_kept_after_slot_overwritten(VALUE ary, VALUE res)
{
    VALUE from;
    const char *ptr = "";
    long len = 0;
    long n = RARRAY_LEN(ary);
    for (long i = 0; i < n; i++) {
        from = RARRAY_AREF(ary, i);
        if (!NIL_P(from)) {
            StringValue(from);
            ptr = RSTRING_PTR(from);
            len = RSTRING_LEN(from);
        }
        rb_str_cat(res, ptr, len);
    }
    return res;
}

/* A pointer into the string is saved to a slot whose address escapes. A
 * pointer into an object in the stack does not keep it alive, so the string
 * is not kept. Reported. */
void save_pointer(const char **slot);

long
bad_interior_pointer_in_escaped_slot(void)
{
    VALUE s = rb_str_new_cstr("x");
    const char *p = RSTRING_PTR(s);
    const char *saved = p;
    save_pointer(&saved);
    rb_ary_new();
    return p[0];
}

/* The address of the slot of s is passed to a call, which may store another
 * value there (as rb_string_value(&v) stores the converted value). s may not
 * be in the slot at the GC call after that. Reported. */
void may_overwrite(VALUE *slot);

long
bad_slot_overwritten_by_call(void)
{
    VALUE s = rb_str_new_cstr("x");
    const char *p = RSTRING_PTR(s);
    may_overwrite(&s);
    rb_ary_new();
    return p[0];
}

/* RB_GC_GUARD passes the address of the slot to an asm, which does not store
 * there. Not reported (boundary). */
long
ok_slot_address_to_guard(void)
{
    VALUE s = rb_str_new_cstr("x");
    const char *p = RSTRING_PTR(s);
    RB_GC_GUARD(s);
    rb_ary_new();
    return p[0];
}

/* v = rb_io_taint_check(from) returns from as is, so from, loaded from its
 * slot before v is defined, is v too, and the slot holds it. As in
 * ok_slot_overwritten_on_path_without_pointer, the slot is overwritten only
 * on paths where the call receives "". Not reported. */
VALUE rb_io_taint_check(VALUE io);

VALUE
ok_alias_defined_before_value(VALUE ary, VALUE res)
{
    VALUE from;
    long n = RARRAY_LEN(ary);
    for (long i = 0; i < n; i++) {
        const char *ptr;
        long len;
        from = RARRAY_AREF(ary, i);
        if (NIL_P(from)) {
            ptr = "";
            len = 0;
        }
        else {
            StringValue(from);
            VALUE v = rb_io_taint_check(from);
            ptr = RSTRING_PTR(v);
            len = RSTRING_LEN(v);
        }
        rb_str_cat(res, ptr, len);
    }
    return res;
}

/* x is loaded from the slot of from, which is then overwritten before v is
 * made from x as is. v is in no slot at the GC call. Reported. */
long
bad_origin_slot_overwritten_before_value(VALUE ary)
{
    VALUE from = RARRAY_AREF(ary, 0);
    StringValue(from);
    VALUE x = from;
    from = Qnil;
    VALUE v = rb_io_taint_check(x);
    const char *p = RSTRING_PTR(v);
    rb_ary_new();
    return p[0] + RTEST(from);
}

/* x is copied to the escaped slot keep before v is made from it, and keep
 * holds v at the GC call, while from is overwritten. The slots are
 * overwritten (with nil) only on paths where the call receives "". Not
 * reported. */
void escape(VALUE *slot);

VALUE
ok_origin_copied_before_value(VALUE ary, VALUE res)
{
    VALUE from, keep = Qnil;
    escape(&keep);
    long n = RARRAY_LEN(ary);
    for (long i = 0; i < n; i++) {
        const char *ptr;
        long len;
        from = RARRAY_AREF(ary, i);
        if (NIL_P(from)) {
            keep = Qnil;
            ptr = "";
            len = 0;
        }
        else {
            StringValue(from);
            keep = from;
            VALUE v = rb_io_taint_check(from);
            from = Qnil;
            ptr = RSTRING_PTR(v);
            len = RSTRING_LEN(v);
        }
        rb_str_cat(res, ptr, len + RTEST(from));
    }
    return res;
}

/* v is made from from as is through 7 calls, so 8 values with v, the limit
 * followed. As ok_alias_defined_before_value. Not reported. */
VALUE
ok_origin_chain_at_limit(VALUE ary, VALUE res)
{
    VALUE from;
    long n = RARRAY_LEN(ary);
    for (long i = 0; i < n; i++) {
        const char *ptr;
        long len;
        from = RARRAY_AREF(ary, i);
        if (NIL_P(from)) {
            ptr = "";
            len = 0;
        }
        else {
            StringValue(from);
            VALUE v = rb_io_taint_check(rb_io_taint_check(rb_io_taint_check(rb_io_taint_check(rb_io_taint_check(rb_io_taint_check(rb_io_taint_check(from)))))));
            ptr = RSTRING_PTR(v);
            len = RSTRING_LEN(v);
        }
        rb_str_cat(res, ptr, len);
    }
    return res;
}

/* The same through 8 calls, 9 values, over the limit, so the confirmation
 * gives up and keeps the report, a false one here. Reported (boundary). */
VALUE
bad_origin_chain_over_limit(VALUE ary, VALUE res)
{
    VALUE from;
    long n = RARRAY_LEN(ary);
    for (long i = 0; i < n; i++) {
        const char *ptr;
        long len;
        from = RARRAY_AREF(ary, i);
        if (NIL_P(from)) {
            ptr = "";
            len = 0;
        }
        else {
            StringValue(from);
            VALUE v = rb_io_taint_check(rb_io_taint_check(rb_io_taint_check(rb_io_taint_check(rb_io_taint_check(rb_io_taint_check(rb_io_taint_check(rb_io_taint_check(from))))))));
            ptr = RSTRING_PTR(v);
            len = RSTRING_LEN(v);
        }
        rb_str_cat(res, ptr, len);
    }
    return res;
}

/* v is made from the argument arg as is, which no caller is known to keep.
 * Reported. */
long
bad_origin_argument(VALUE arg)
{
    VALUE v = rb_io_taint_check(arg);
    const char *p = RSTRING_PTR(v);
    rb_ary_new();
    return p[0];
}

/* arg is copied to the slot keep, escaped by RB_GC_GUARD, before v is made
 * from it, and keep holds v at the GC call. Not reported. */
long
ok_origin_argument_kept_in_slot(VALUE arg)
{
    VALUE keep = arg;
    VALUE v = rb_io_taint_check(arg);
    const char *p = RSTRING_PTR(v);
    rb_ary_new();
    long r = p[0];
    RB_GC_GUARD(keep);
    return r;
}

/* The address of the slot of s may reach the call through a select, so the
 * call may store another value there. Reported. */
long
bad_slot_overwritten_through_select(int c)
{
    VALUE s = rb_str_new_cstr("x");
    VALUE other = Qnil;
    VALUE *slot = c ? &s : &other;
    const char *p = RSTRING_PTR(s);
    may_overwrite(slot);
    rb_ary_new();
    return p[0] + RTEST(other);
}

/* s is at the start of the struct h, whose address escapes (to an asm), and a
 * call receives the address of another field. It cannot store over s, which
 * stays in h. Not reported. */
struct holder {
    VALUE v;
    char rest[16];
};
void fill(char *p);

long
ok_call_receiving_other_field(void)
{
    struct holder h;
    h.v = rb_str_new_cstr("x");
    __asm__ volatile("" : : "r"(&h) : "memory");
    const char *p = RSTRING_PTR(h.v);
    fill(h.rest);
    rb_ary_new();
    return p[0];
}

/* The same, but the call receives the address of the last byte of s, so it
 * may store over s. Reported (boundary: the field after s is at 8). */
long
bad_call_receiving_last_byte_of_value(void)
{
    struct holder h;
    h.v = rb_str_new_cstr("x");
    __asm__ volatile("" : : "r"(&h) : "memory");
    const char *p = RSTRING_PTR(h.v);
    fill((char *)&h + sizeof(VALUE) - 1);
    rb_ary_new();
    return p[0];
}
