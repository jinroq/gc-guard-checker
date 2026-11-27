#include "ruby/ruby.h"

#define NOINLINE __attribute__((noinline))

/* A method registered with rb_define_method. The VM keeps the receiver and
 * the arguments on its stack during the call, so it is not reported. */
static VALUE
ok_method_argument(VALUE self, VALUE str)
{
    const char *p = RSTRING_PTR(str);
    rb_ary_new();
    return INT2FIX(p[0]);
}

/* With argc == -1, the arguments are in argv, which points into the VM stack.
 * Not reported. */
static VALUE
ok_method_argv(int argc, VALUE *argv, VALUE self)
{
    const char *p = RSTRING_PTR(argv[0]);
    rb_ary_new();
    return INT2FIX(p[0]);
}

/* A static function whose only caller uses the argument after the call. Not
 * reported. */
static NOINLINE VALUE
ok_callee_of_keeping_caller(VALUE str)
{
    const char *p = RSTRING_PTR(str);
    rb_ary_new();
    return INT2FIX(p[0]);
}

VALUE
keeping_caller(VALUE str)
{
    VALUE r = ok_callee_of_keeping_caller(str);
    return rb_assoc_new(r, str);
}

/* The only caller passes its own argument, which the VM keeps since the caller
 * is a method. Not reported. */
static NOINLINE VALUE
ok_callee_of_method(VALUE str)
{
    const char *p = RSTRING_PTR(str);
    rb_ary_new();
    return INT2FIX(p[0]);
}

static VALUE
method_calling_callee(VALUE self, VALUE str)
{
    return ok_callee_of_method(str);
}

/* One caller passes a constant, which is not an object in the heap, and the
 * other keeps the argument. Not reported (with two callers, the constant is
 * not propagated into the function). */
static NOINLINE VALUE
ok_callee_of_constant_and_keeping_callers(VALUE str)
{
    const char *p = RSTRING_PTR(str);
    rb_ary_new();
    return INT2FIX(p[0]);
}

VALUE
constant_caller(void)
{
    return ok_callee_of_constant_and_keeping_callers(Qnil);
}

VALUE
keeping_caller_with_constant_sibling(VALUE str)
{
    VALUE r = ok_callee_of_constant_and_keeping_callers(str);
    return rb_assoc_new(r, str);
}

/* The only caller passes a new string that it does not use after the call, so
 * it is reported. */
static NOINLINE VALUE
bad_callee_of_temporary(VALUE str)
{
    const char *p = RSTRING_PTR(str);
    rb_ary_new();
    return INT2FIX(p[0]);
}

VALUE
temporary_caller(void)
{
    return bad_callee_of_temporary(rb_str_new_cstr("x"));
}

/* One of the callers keeps the argument and the other does not. All the
 * callers have to keep it, so it is reported. */
static NOINLINE VALUE
bad_callee_of_mixed_callers(VALUE str)
{
    const char *p = RSTRING_PTR(str);
    rb_ary_new();
    return INT2FIX(p[0]);
}

VALUE
mixed_caller_keeping(VALUE str)
{
    VALUE r = bad_callee_of_mixed_callers(str);
    return rb_assoc_new(r, str);
}

VALUE
mixed_caller_temporary(void)
{
    return bad_callee_of_mixed_callers(rb_str_new_cstr("x"));
}

/* The caller keeps the first argument but not the second, from which the
 * pointer is derived. Reported. */
static NOINLINE VALUE
bad_second_argument_not_kept(VALUE a, VALUE str)
{
    const char *p = RSTRING_PTR(str);
    rb_ary_new();
    return INT2FIX(p[0]);
}

VALUE
first_argument_keeping_caller(VALUE a)
{
    VALUE r = bad_second_argument_not_kept(a, rb_str_new_cstr("x"));
    return rb_assoc_new(r, a);
}

/* A function with external linkage may be called from other files, whose
 * callers are unknown. Reported even though the caller here keeps it. */
NOINLINE VALUE
bad_external_callee(VALUE str)
{
    const char *p = RSTRING_PTR(str);
    rb_ary_new();
    return INT2FIX(p[0]);
}

VALUE
external_callee_caller(VALUE str)
{
    VALUE r = bad_external_callee(str);
    return rb_assoc_new(r, str);
}

/* A static function whose address is used other than for defining a method,
 * here as a callback of rb_protect. Its callers are unknown, so it is
 * reported. */
static VALUE
bad_callback(VALUE str)
{
    const char *p = RSTRING_PTR(str);
    rb_ary_new();
    return INT2FIX(p[0]);
}

VALUE
callback_user(VALUE str)
{
    int state;
    VALUE r = rb_protect(bad_callback, str, &state);
    return rb_assoc_new(r, str);
}

/* A method that is also called directly with a new string. Reported. */
static NOINLINE VALUE
bad_method_called_with_temporary(VALUE self, VALUE str)
{
    const char *p = RSTRING_PTR(str);
    rb_ary_new();
    return INT2FIX(p[0]);
}

VALUE
method_direct_caller(VALUE self)
{
    return bad_method_called_with_temporary(self, rb_str_new_cstr("x"));
}

/* A method with argc == -1 that writes to argv. argv is then not trusted to
 * hold the values passed by the VM, so it is reported. */
static VALUE
bad_method_writing_argv(int argc, VALUE *argv, VALUE self)
{
    argv[1] = Qnil;
    const char *p = RSTRING_PTR(argv[0]);
    rb_ary_new();
    return INT2FIX(p[0]);
}

/* A method with argc == -1 that is also called directly with an array of new
 * strings. Reported. */
static NOINLINE VALUE
bad_variadic_method_called_directly(int argc, VALUE *argv, VALUE self)
{
    const char *p = RSTRING_PTR(argv[0]);
    rb_ary_new();
    return INT2FIX(p[0]);
}

VALUE
variadic_direct_caller(VALUE self)
{
    VALUE args[1] = {rb_str_new_cstr("x")};
    return bad_variadic_method_called_directly(1, args, self);
}

/* argv is passed to a function that may write to it. Reported. */
void fill(VALUE *argv);

static VALUE
bad_method_argv_passed_to_writer(int argc, VALUE *argv, VALUE self)
{
    fill(argv);
    const char *p = RSTRING_PTR(argv[0]);
    rb_ary_new();
    return INT2FIX(p[0]);
}

/* argv itself is stored to memory, through which anything may write to it.
 * Reported. */
VALUE *saved_argv;

static VALUE
bad_method_argv_stored(int argc, VALUE *argv, VALUE self)
{
    saved_argv = argv;
    const char *p = RSTRING_PTR(argv[0]);
    rb_ary_new();
    return INT2FIX(p[0]);
}

/* rb_scan_args only reads argv. (Parenthesized to call the function rather
 * than the macro.) Not reported. */
static VALUE
ok_method_argv_passed_to_rb_scan_args(int argc, VALUE *argv, VALUE self)
{
    VALUE str;
    (rb_scan_args)(argc, argv, "1", &str);
    const char *p = RSTRING_PTR(argv[0]);
    rb_ary_new();
    return INT2FIX(p[0] + (str != Qnil));
}

/* rb_scan_args only reads the argv argument, but writes to the pointers
 * after the format, here into argv. Reported. */
static VALUE
bad_method_argv_as_output_of_rb_scan_args(int argc, VALUE *argv, VALUE self)
{
    (rb_scan_args)(argc, argv, "1", &argv[0]);
    const char *p = RSTRING_PTR(argv[0]);
    rb_ary_new();
    return INT2FIX(p[0]);
}

/* With argc == -2, the arguments are passed as an Array that the VM makes for
 * the call, which is not on the VM stack. Reported. */
static VALUE
bad_method_args_array(VALUE self, VALUE args)
{
    const VALUE *p = RARRAY_CONST_PTR(args);
    rb_ary_new();
    return p[0];
}

/* The receiver of a method with argc == -2 is on the VM stack. Not
 * reported. */
static VALUE
ok_method_receiver_with_args_array(VALUE self, VALUE args)
{
    const char *p = RSTRING_PTR(self);
    rb_ary_new();
    return INT2FIX(p[0]);
}

/* When argc is not a constant, which arguments are on the VM stack is not
 * known. Reported. */
static VALUE
bad_method_with_unknown_argc(VALUE self, VALUE str)
{
    const char *p = RSTRING_PTR(str);
    rb_ary_new();
    return INT2FIX(p[0]);
}

/* As string_options in the prism extension: a static function that returns
 * a value read through argv, which it only reads. */
static NOINLINE VALUE
first_argument(int argc, const VALUE *argv)
{
    if (argc < 1)
        rb_raise(rb_eArgError, "no argument");
    return argv[0];
}

/* The value returned by first_argument is argv[0] of this method, which is
 * on the VM stack. Not reported. */
static VALUE
ok_method_value_returned_from_argv(int argc, VALUE *argv, VALUE self)
{
    VALUE str = first_argument(argc, argv);
    const char *p = RSTRING_PTR(str);
    rb_ary_new();
    return INT2FIX(p[0]);
}

/* The same function given an array of new strings, which is not on the VM
 * stack. Reported (boundary). */
VALUE
bad_value_returned_from_new_array(void)
{
    VALUE args[1] = {rb_str_new_cstr("x")};
    VALUE str = first_argument(1, args);
    const char *p = RSTRING_PTR(str);
    rb_ary_new();
    return INT2FIX(p[0]);
}

void
define_with_argc(VALUE c, int argc)
{
    rb_define_method(c, "j", bad_method_with_unknown_argc, argc);
}

void
Init_argument_kept_by_callers(void)
{
    VALUE c = rb_define_class("ArgumentKeptByCallers", rb_cObject);
    rb_define_method(c, "a", ok_method_argument, 1);
    rb_define_method(c, "b", ok_method_argv, -1);
    rb_define_singleton_method(c, "c", method_calling_callee, 1);
    rb_define_method(c, "d", bad_method_called_with_temporary, 1);
    rb_define_method(c, "e", bad_method_writing_argv, -1);
    rb_define_method(c, "f", bad_variadic_method_called_directly, -1);
    rb_define_method(c, "g", bad_method_args_array, -2);
    rb_define_method(c, "h", ok_method_receiver_with_args_array, -2);
    rb_define_method(c, "k", bad_method_argv_passed_to_writer, -1);
    rb_define_method(c, "l", bad_method_argv_stored, -1);
    rb_define_method(c, "m", ok_method_argv_passed_to_rb_scan_args, -1);
    rb_define_method(c, "n", bad_method_argv_as_output_of_rb_scan_args, -1);
    rb_define_method(c, "o", ok_method_value_returned_from_argv, -1);
}
