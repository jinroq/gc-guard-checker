# gc-guard-checker

An LLVM pass that looks for missing `RB_GC_GUARD` in C code using the CRuby
API, such as CRuby itself and its C extensions.

CRuby's GC scans the machine stack and registers conservatively. When a C
function takes a pointer into an object, such as `RSTRING_PTR(str)`, and then
stops using `str` itself, the compiler may drop `str` from registers and the
stack. If GC runs before the pointer is used again, `str` can be collected (or
moved by compaction) and the pointer dangles. `RB_GC_GUARD(str)` after the last
use of the pointer prevents this.

```c
VALUE tmp = rb_obj_as_string(obj);
const char *p = RSTRING_PTR(tmp);
VALUE ary = rb_ary_new();   /* may trigger GC: tmp is no longer used ... */
use(p[0]);                  /* ... so p may point to freed memory */
```

## How it works

The pass runs on optimized LLVM IR (`clang -O2`). For each `VALUE` `v` that is
converted to a pointer (`inttoptr`), it collects the pointers derived from it
(through GEPs, phis, selects and loads of pointers, which covers
`RSTRING_PTR`, `RARRAY_CONST_PTR`, `RTYPEDDATA_GET_DATA`, ...). It reports `v`
when there is a path

    derived pointer is taken -> call that may trigger GC (C) -> access through the pointer

and `v` is not live at C. An access is a load or store through the pointer, or
a call that receives the pointer: `rb_str_new(RSTRING_PTR(tmp), len)` is
reported when `tmp` is dead, since `rb_str_new` allocates before copying.

A call C is not checked where `v` is known to be a constant (Qnil, Qfalse,
...), as dominated by the edge on which `v` compares equal to it: a constant
is not an object in the heap.

`v` is considered live at C when

- `v`, or a value with the same bits (casts, phis and selects merging `v`, a
  value compared equal to `v` in the dominated region), is used after C. In a
  loop, a use in the next iteration after `v` is defined again is of a new
  value, so it does not count,
- on every path from the definition of `v` to C, `v` is in a stack slot that
  stays in memory, as its address has escaped (as `RB_GC_GUARD(v)` does) or it
  is accessed with volatile loads or stores (as by `StringValue(v)` with
  `rb_string_value` inlined): it is stored or copied there, and the slot is neither overwritten nor ends its lifetime before C.
  Once the address escapes, the compiler cannot remove or delay stores to the
  slot, so `RB_GC_GUARD` anywhere in the function protects `v` as long as `v`
  is not reassigned and the slot lives. `v` may be in different slots on
  different paths (`link` and `link_orig` in `realpath_rec` of CRuby).
- `v` is kept alive by the callers during the whole call of the function:
  - the receiver and the arguments of a method defined with
    `rb_define_method` and friends, which are on the VM stack during the call
    (with `argc == -1`, the values in `argv` too, unless the method writes to
    `argv` or is also called directly; with `argc == -2`, only the receiver,
    as the arguments are in an `Array` made for the call), or
  - an argument of a `static` function, when every caller keeps the passed
    value alive across the call in the same way.
  - the value returned by a function defined in the same file that always
    returns one of its arguments (when that is kept alive), or a value read
    through an argument that it only reads (when that points into `argv` of
    such a method), as `string_options` in the prism extension returns
    `argv[0]`. Functions with external
    linkage, or whose address is used otherwise (as a callback, in a table,
    ...), may have unknown callers, so their arguments are not considered
    kept.

Finally, a report is kept only when all of this holds on one path: following
the paths from the definition of `v`, tracking at once the slots holding `v`
and the values that are `v` or derived from it, C is reached with `v` in no
slot, and an access through a pointer derived from `v` follows. Otherwise
the slots and the pointer could be considered on different paths: in
`pack_pack` of CRuby, `v` is out of its slot at C only in a later iteration
where the call receives `""` rather than a pointer into `v`.

A call is considered not to trigger GC when it

- only reads memory (from LLVM's memory attributes),
- is a library function known to LLVM (`fstat`, `memcpy`, `malloc`, ...) and
  receives no function pointer (unlike `qsort`), or
- is in a short list of functions whose implementations were checked not to GC
  when they return: CRuby functions (`rb_gc_writebarrier`,
  `rb_io_check_closed`, ...), system calls that LLVM does not know (`send`,
  `recv`, ...) and zlib's checksums (`crc32`, `adler32`). See
  `isKnownNonGCFunction` in `src/GCGuardCheck.cpp`.

A `VALUE` that is only accessed as a non-Ruby struct (by its TBAA type) is
skipped, since callbacks for `rb_rescue` and friends often receive a C pointer
disguised as a `VALUE`.

## Requirements

- LLVM 21 with development files (`llvm-21-dev` on Debian/Ubuntu) and clang 21
- CMake 3.20 or later
- Ruby with test-unit, to run the tests
- A configured CRuby source tree, for the headers used by the tests

## Build and test

```sh
cmake -S . -B build
cmake --build build
ruby test/test_gc_guard_check.rb
```

The tests compile the C files in `test/fixtures` with the CRuby headers and
check the reports. Functions named `bad_*` must be reported and `ok_*` must
not. They use these environment variables:

| Variable | Default |
|---|---|
| `RUBY_SRC` | `~/dev/ruby/ruby` |
| `RUBY_BUILD` | `$RUBY_SRC/build` (a configured build directory) |
| `LLVM_BIN` | `/usr/lib/llvm-21/bin` |
| `GC_GUARD_CHECK_PLUGIN` | `build/GCGuardCheck.so` |

## Running on CRuby

Build CRuby with clang, so that the generated headers and the Makefiles of the
extensions exist:

```sh
mkdir ruby-clang && cd ruby-clang
path/to/ruby/configure CC=clang --disable-install-doc
make -j
```

Then run the checker over the core sources and the extensions:

```sh
tool/run-cruby.sh path/to/ruby-clang out
```

The reports are written to `out/reports.txt`:

```
gc-guard-check: rb_home_dir_of: pointer derived from %3 is used at .../include/ruby/internal/memory.h:714 after call to rb_str_resize at .../file.c:4940 without keeping it alive [origin=call]
```

Locations come from debug info, so a use inlined from a header (here `memcpy`
via `MEMCPY`) points into the header. `%3` is the `VALUE` in the IR, which is
kept in `out/<file>.ll`.

`origin` tells where `v` comes from and is useful for triage:

| origin | Meaning | Priority |
|---|---|---|
| `call` | Returned by a call in the function, e.g. a new object | High: likely referenced only from this frame |
| `loaded` | Loaded from memory | Lower: often reachable from where it was loaded |
| `argument` | A function argument | Lower: often kept by a caller in another file |

Set `WITH_EXT=0` to skip the extensions. A few files that need special compiler
flags (the SIMD implementations of BLAKE3) cannot be compiled by the script;
they do not use the Ruby API. If the checker itself fails on a file, the
reports are incomplete, so the script lists the file and exits with an error.

## Triaging reports

Most reports are not bugs, so it pays to start from the shapes that turned
out to be bugs in CRuby:

- A pointer into a new string, or a string converted with `#to_str`, is
  passed to a call that allocates before copying from it: `rb_str_buf_cat`
  resizing the destination, `rb_str_new`, `rb_str_tmp_new`, ...
  (`pack("w")`, `String#scrub` with a replacement responding to `#to_str`,
  `IO#set_encoding`, `rb_home_dir_of`, `append_fspath`).
- A pointer into the hidden array of `NEW_MEMO_FOR` is used after the block
  or `#<=>` is called, once `rb_block_call` has returned
  (`Enumerable#minmax`).

Grepping `reports.txt` for these calls (`after call to rb_str_cat`, `after
call to rb_yield`, ...) finds them quickly. The common false reports are:

- `v` is reachable from an object or a struct: the string of a
  `StringScanner`, the buffer of a zlib stream, the source of a `Regexp`, the
  arguments kept in an `Enumerator`, ...
- The call does not trigger GC in practice, but is not known to the checker
  (`rb_st_lookup` on a table with ID keys, a callback of `rb_id_table_foreach`
  that does not allocate, debug dumps with `ruby_snprintf`, ...).
- `v` is out of its slots on a path where the pointer used after the call is
  not from `v`, when the confirmation on one path gives up (see Limitations).

To check a report, build CRuby with AddressSanitizer
(`cflags="-fsanitize=address -fno-omit-frame-pointer" LDFLAGS=-fsanitize=address`)
and run the code under `GC.stress = true`. When it does not crash, the value
is often kept alive only by stale copies below the stack pointer, left by
earlier calls. With gdb, break at the call that may trigger GC and check that
no register and no slot between the stack pointer and the stack top holds
`v`, then break at `gc_mark_maybe_internal` with `$rsi == v` while the call
runs, to see where it is marked from. Calling functions from gdb overwrites
the stack below the stack pointer, which may make the bug show up.

## Limitations

Reports are candidates to be checked by reading the code, not confirmed bugs.
When the analysis cannot tell, it errs on the side of reporting: a false
report costs some reading, while a missed one leaves a bug. The known cases
of each kind are listed below.

It analyzes the IR from `clang -O2`, while CRuby is usually built with `-O3`,
often with GCC. Register allocation and stack slot reuse are not modeled; a
value that is dead in the IR may still happen to be in a register or a stale
stack slot, which also makes these bugs hard to reproduce.

Missed reports:

- A call that receives `v` is assumed to keep it during the call, as
  `rb_str_resize(str, ...)` does. If the callee is done with `v` before it
  triggers GC, the report is missed.
- The knowledge of CRuby functions (the functions that do not trigger GC, or
  that only read `argv`) comes from reading their implementations, and
  assumes the default GC.

False reports:

- Apart from the callers of methods and `static` functions above, it cannot
  tell that `v` is reachable from elsewhere: a global table, an object field,
  or the frame of a caller in another file. Most reports with
  `origin=argument` and `origin=loaded` are of this kind.
- Library functions that LLVM does not know, such as OpenSSL's, are treated as
  possibly triggering GC unless listed. Being in a system library is not
  enough: OpenSSL calls back into Ruby through callbacks registered earlier
  (`SSL_CTX_set_verify`, ...), and zlib allocates through `xmalloc`.
- Indirect calls are treated as possibly triggering GC. In CRuby they are
  mostly callbacks (`rb_class_foreach_subclass`, the interface of random
  number generators, ...), which may run any code.
- Calls to functions defined in the same file are treated as possibly
  triggering GC, without looking into their bodies.
- A use of `v` by a phi at the edge from an `invoke` (or `callbr`) that may
  trigger GC is not counted, as the use is taken to be at the end of the
  block, which is the call itself. This does not happen in C.
- Beyond 32 stack slots or 32 copies between them for a value, the rest are
  not tracked, and `v` there does not count.
- The confirmation on one path gives up, keeping the report, beyond 64 values
  that are `v` or derived from it, 32 slots, 8 values `v` is made from as is
  (`v = rb_io_taint_check(x)`, counting `v`), 200000 blocks scanned for a call,
  or 1000000 for all the calls of a `v`.

Cost:

- Following arguments around cycles of calls, and the paths through stack
  slots, may take long for some shapes of code. On CRuby the whole run takes
  about a minute.

## License

MIT. See [LICENSE](LICENSE).
