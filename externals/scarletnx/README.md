# ScarletNX CPU backend

Eglantine optionally builds the [ScarletNX](https://github.com/scarletnx/ScarletNX) CPU
recompiler library. It does not use ScarletNX's kernel, services or graphics runtime.
The dependency is GPL-3.0-or-later and pinned in `CMakeModules/ScarletNX.cmake`.

Enable it at configure time with `-DENABLE_SCARLETNX=ON` on native x86-64 Linux.
This requires Clang, `patch`, and LLVM development files. If `llvm-config` is not
on the path, pass `-DSCARLETNX_LLVM_CONFIG=/path/to/llvm-config`.

Then select **ScarletNX (Experimental)** in the CPU settings and disable
**Enable Reactive Flushing** in the graphics settings for that profile, or add
this to the CLI configuration:

```ini
[Cpu]
cpu_backend=2
cpu_backend\default=false

[Renderer]
use_reactive_flushing=false
use_reactive_flushing\default=false
```

ScarletNX is not the default, even in a build that includes it. Only AArch64
applications with multicore CPU emulation and fastmem enabled can use it.
A32, single-core, debugger, no-fastmem, and reactive-flushing sessions fall back
to Dynarmic without changing any settings. With reactive flushing disabled,
Eglantine uses targeted read traps for GPU writes. Leave `EDEN_NX2_NO_REACTIVE`
and `EDEN_NX2_READ_TRAPS` unset.

`fastmem.patch` confines generated memory accesses to Eglantine's reserved guest
address space and lets Eglantine provide the guest timer frequency. It also
matches Dynarmic's identity semantics for PAuth hints, register PAC/AUT/XPAC,
and authenticated returns, as well as its deterministic masked XOR for `PACGA`.
The register and return forms still require PAuth support in the guest profile.

Incremental JIT modules are loaded from uniquely named files in private
temporary directories. The loader unlinks each file after loading it, so
retaining compiled code doesn't retain a file descriptor. This adds temporary
filesystem work on JIT misses. `TMPDIR` (default `/tmp`) must allow executable
mappings.

## Boot profiling

Set `EDEN_SCARLETNX_PROFILE=1` for a ScarletNX run. `Core.ARM` emits cumulative
`ScarletNX profile` summaries every 4,096 completed resolves and at normal CPU
teardown. No addresses, code, or other guest data are included in these summaries.
Use consecutive summaries with log timestamps or visible boot milestones to
measure throughput over an interval. A forced kill can lose the final interval.

For a cold-cache A/B comparison on the same binary, leave
`EDEN_SCARLETNX_JIT_BUDGET` unset for the 2,048-instruction baseline and set it to
`8192` for the treatment. The accepted decimal range is 2048–8192; invalid
values warn and use the baseline. The threshold controls whether discovery
starts the next queued function within the existing code snapshot. The last
function can take the module beyond the threshold. The profile logs the
effective budget.

For the separate loader A/B, leave the budget unset in both runs. Set
`EDEN_SCARLETNX_INPROCESS_LOADER=1` only for the treatment. On Linux x86-64,
newly linked incremental modules can bypass `dlopen`. The loader accepts only
internal linker metadata, and it uses anonymous mappings finalized as R, RX
and RW without an executable/writable alias. It does not load cached files.
Opted-in incremental loads require a verified host personality without
`READ_IMPLIES_EXEC`, even on a cache hit. With that policy verified, cache
hits still use `dlopen`. A fresh rejection fails compilation without
retrying the image or writing it to the cache. Both loader paths accept the
descriptor before saving a linked module.
Anonymous executable pages are not registered with the dynamic loader and
may change RSS and swap behavior; measure both alongside boot time.

For the separate LLVM target A/B, keep `EDEN_SCARLETNX_INPROCESS_LOADER=1` and
the default 2,048 budget in both runs. Set `EDEN_SCARLETNX_REUSE_LLVM_TARGET=1`
only for treatment. On Linux x86-64, single-unit `-O0` incremental cache misses
then reuse one native target machine per recompiler, serially. Each unit and
descriptor still has its own LLVM context and module. Parallel units, other
optimization levels, full compilation, runtime library generation and disk
cache lookups are unchanged.

- Stage fields are `calls/successes/wall_ms/bytes`. A dispatch miss here is a
  generated-code lookup that reached the host resolver callback after its
  interrupt check. It may hit a published module, find a failed entry, or
  request a resolve. A resolve may finish without compiling, for example when
  another core published the entry while it waited.
- Resolve timings are nonoverlapping stages within a call. `snapshot` includes
  allocation plus the guest-memory copy; its bytes are requested. `publish`
  includes acquiring the module-table lock. `library` covers the full library
  call. `resolve_other` accounts for the rest, including cleanup. The totals
  sum host wall time across calls and cores, **not** elapsed boot time. In
  particular, `compile_lock_wait` on one core can overlap `library` on another.
- A `cache_lookup` success means a linked module was accepted, not just that
  its file existed. Lookup includes hashing, `cache_read`, and any
  `cache_image_load`. Both `cache_image_load` and `image_load` (new modules)
  include `image_write` and `dlopen`. `llvm_units` times the whole work queue,
  not individual worker CPU times. `llvm_metadata` includes the descriptor and
  `rtlib` (runtime support object); the latter includes its own disk-cache I/O.
  Reads report bytes returned; writes report requested bytes, with their
  successes counted separately. Writes are not a measure of physical disk I/O.
  `inprocess_load` counts attempts and successes; the difference counts
  rejected fresh images. A policy rejection before cache lookup does not
  start a load attempt. `inprocess_load` is included in `image_load`. Its `inprocess_map` child
  covers validation, copy, relocations and protections; its byte field is
  mapped size. `inprocess_descriptor` covers calling and checking the private
  descriptor and building dispatch.
- `llvm_new_target_ir` and `llvm_reused_target_ir` are disjoint setup counts
  for single-unit incremental code and descriptor IR. Both include context
  and module setup; only the former also constructs a target machine and its
  data layout. These times are already included in `llvm_units` or
  `llvm_metadata`. They do not measure the target-dependent work done during
  emission.
- The exclusive line subtracts immediate children from inclusive library
  stages, `library_other` is the time inside the library outside the listed
  top-level stages, and `image_load_both` combines the two loader paths before
  subtracting their writes, `dlopen` and `inprocess_load`. Do not sum parents
  with their children.
  Dispatch counters can include work that has not finished resolving yet.
