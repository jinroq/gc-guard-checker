# frozen_string_literal: true

require "test/unit"
require "open3"
require "tmpdir"

# Compiles the C files in fixtures to LLVM IR with clang -O2, runs the
# gc-guard-check pass on them and checks the reported functions.
class TestGCGuardCheck < Test::Unit::TestCase
  ROOT = File.expand_path("..", __dir__)
  FIXTURES = File.join(__dir__, "fixtures")
  LLVM_BIN = ENV.fetch("LLVM_BIN", "/usr/lib/llvm-21/bin")
  RUBY_SRC = ENV.fetch("RUBY_SRC", File.expand_path("~/dev/ruby/ruby"))
  RUBY_BUILD = ENV.fetch("RUBY_BUILD", File.join(RUBY_SRC, "build"))
  PLUGIN = ENV.fetch("GC_GUARD_CHECK_PLUGIN", File.join(ROOT, "build", "GCGuardCheck.so"))

  # Format of a report: "gc-guard-check: <function>: ..."
  REPORT_RE = /\Agc-guard-check: (\S+):/

  def test_no_report_without_derived_pointer
    assert_equal([], reported_functions("no_derived_pointer.c"))
  end

  def test_report_pointer_used_after_gc_call
    assert_equal(["bad_ptr_used_after_gc"], reported_functions("ptr_used_after_gc.c"))
  end

  def test_no_report_when_guarded_after_last_pointer_use
    assert_equal([], reported_functions("guard_after_use.c"))
  end

  def test_no_report_without_gc_call_between
    assert_equal([], reported_functions("no_gc_call_between.c"))
  end

  def test_guard_before_gc_call_keeps_value_while_slot_is_alive
    assert_equal(["bad_guarded_var_out_of_scope"], reported_functions("guard_before_gc.c"))
  end

  def test_no_report_when_value_is_used_after_pointer
    assert_equal([], reported_functions("value_used_after.c"))
  end

  def test_no_report_when_last_pointer_use_is_before_gc_call
    assert_equal([], reported_functions("ptr_last_use_before_gc.c"))
  end

  def test_report_when_gc_call_is_only_in_one_branch
    assert_equal(["bad_gc_in_one_branch"], reported_functions("gc_in_one_branch.c"))
  end

  def test_loop_reports_only_pointer_taken_outside_loop
    assert_equal(["bad_ptr_outside_loop"], reported_functions("gc_in_loop.c"))
  end

  def test_no_report_when_call_between_cannot_trigger_gc
    assert_equal([], reported_functions("non_gc_call_between.c"))
  end

  def test_report_when_unknown_function_is_called_between
    assert_equal(["bad_unknown_call_between"], reported_functions("unknown_call_between.c"))
  end

  def test_report_only_unguarded_value
    reports = analyze("multiple_values.c").lines.grep(REPORT_RE)
    assert_equal(1, reports.size, reports.join)
    assert_match(/derived from %1 /, reports.first)
  end

  def test_report_only_ruby_objects_not_c_pointers_in_value
    assert_equal(["bad_typed_data_used_after_gc"], reported_functions("c_pointer_in_value.c"))
  end

  def test_report_includes_source_locations
    report = analyze("ptr_used_after_gc.c").lines.grep(REPORT_RE).first
    assert_match(/ used at \S*ptr_used_after_gc\.c:9 after call to rb_ary_new at \S*ptr_used_after_gc\.c:8 /, report)
  end

  def test_no_report_for_known_non_gc_functions
    assert_equal(["bad_similar_name_to_non_gc_function"],
                 reported_functions("known_non_gc_functions.c"))
  end

  def test_no_report_when_value_is_live_through_phi
    assert_equal([], reported_functions("value_merged_by_phi.c"))
  end

  def test_guard_on_address_taken_variable
    assert_equal(["bad_slot_overwritten_before_gc"],
                 reported_functions("guard_on_address_taken_var.c"))
  end

  def test_value_known_equal_to_guarded_value
    assert_equal(["bad_guard_on_unequal_value"], reported_functions("guarded_equal_value.c"))
  end

  def test_report_includes_origin_of_value
    origins = analyze("value_origins.c").lines.grep(REPORT_RE).to_h do |l|
      [l[REPORT_RE, 1], l[/ \[origin=(\w+)\]$/, 1]]
    end
    assert_equal({
      "bad_origin_call" => "call",
      "bad_origin_argument" => "argument",
      "bad_origin_loaded" => "loaded",
      "bad_origin_phi_with_call" => "call",
    }, origins)
  end

  def test_io_helpers_that_gc_only_when_raising
    reports = analyze("io_pointer.c").lines.grep(REPORT_RE)
    assert_equal(["bad_origin_through_returned_argument"], reports.map { |l| l[REPORT_RE, 1] })
    assert_match(/\[origin=argument\]$/, reports.first)
  end

  def test_libc_calls_do_not_trigger_gc_unless_given_callback
    assert_equal(["bad_libc_call_with_callback"], reported_functions("libc_calls.c"))
  end

  def test_guard_assignment_keeps_value_on_stack
    assert_equal(["bad_guard_assignment_overwritten"], reported_functions("guard_assignment.c"))
  end

  def test_reachability_in_large_function
    assert_equal([], reported_functions("large_function.c"))
  end

  def test_pointer_passed_to_call
    assert_equal(["bad_ptr_passed_to_allocating_call", "bad_ptr_passed_to_memcpy_after_gc"],
                 reported_functions("ptr_passed_to_call.c"))
  end

  def test_value_loaded_from_escaped_slot
    assert_equal(["bad_loaded_then_slot_overwritten"],
                 reported_functions("loaded_from_escaped_slot.c"))
  end

  private

  def reported_functions(fixture)
    analyze(fixture).lines.filter_map { |l| l[REPORT_RE, 1] }.uniq.sort
  end

  def analyze(fixture)
    Dir.mktmpdir do |dir|
      ll = File.join(dir, "out.ll")
      run!(File.join(LLVM_BIN, "clang"), "-O2", "-g", "-S", "-emit-llvm", "-Wno-macro-redefined",
           "-I#{RUBY_SRC}/include", "-I#{arch_include}",
           File.join(FIXTURES, fixture), "-o", ll)
      run!(File.join(LLVM_BIN, "opt"), "-load-pass-plugin=#{PLUGIN}",
           "-passes=gc-guard-check", "-disable-output", ll)
    end
  end

  def arch_include
    Dir.glob(File.join(RUBY_BUILD, ".ext", "include", "*-*")).first or
      flunk "arch include dir not found under #{RUBY_BUILD}/.ext/include"
  end

  def run!(*cmd)
    out, err, status = Open3.capture3(*cmd)
    assert_true(status.success?, "command failed: #{cmd.join(' ')}\n#{err}")
    out
  end
end
