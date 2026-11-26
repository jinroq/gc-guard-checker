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

`v` is considered live at C when

- `v`, or a value with the same bits (casts, phis and selects merging `v`, a
  value compared equal to `v` in the dominated region), is used after C,
- `v` is in a stack slot whose address has escaped (as `RB_GC_GUARD(v)` does)
  and the slot is neither overwritten nor ends its lifetime before C. Once the
  address escapes, the compiler cannot remove or delay stores to the slot, so
  `RB_GC_GUARD` anywhere in the function protects `v` as long as `v` is not
  reassigned and the slot lives.

A call is considered not to trigger GC when it

- only reads memory (from LLVM's memory attributes),
- is a library function known to LLVM (`fstat`, `memcpy`, `malloc`, ...) and
  receives no function pointer (unlike `qsort`), or
- is in a short list of CRuby functions whose implementations were checked not
  to GC when they return (`rb_gc_writebarrier`, `rb_io_check_closed`, ...).
  See `isKnownNonGCFunction` in `src/GCGuardCheck.cpp`.

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
| `argument` | A function argument | Lower: the receiver and arguments of a method are on the VM stack |

Set `WITH_EXT=0` to skip the extensions. A few files that need special compiler
flags (the SIMD implementations of BLAKE3) cannot be compiled by the script;
they do not use the Ruby API. If the checker itself fails on a file, the
reports are incomplete, so the script lists the file and exits with an error.

## Limitations

Reports are candidates to be checked by reading the code, not confirmed bugs.

- The analysis is intraprocedural. It cannot tell that `v` is reachable from
  elsewhere: a global table, an object field, or the caller's frame. Most
  reports with `origin=argument` are of this kind.
- Library functions that LLVM does not know, such as OpenSSL's, are treated as
  possibly triggering GC.
- Indirect calls are treated as possibly triggering GC.
- It analyzes the IR from `clang -O2`, while CRuby is usually built with
  `-O3`, often with GCC. Register allocation and stack slot reuse are not
  modeled; a value that is dead in the IR may still happen to be in a register
  or a stale stack slot, which also makes these bugs hard to reproduce.
- The knowledge of CRuby functions assumes the default GC.

## License

MIT. See [LICENSE](LICENSE).
