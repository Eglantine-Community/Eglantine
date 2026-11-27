# ScarletNX AArch64 backend

This optional Linux x86-64 backend links the [ScarletNX recompiler](https://github.com/scarletnx/ScarletNX), licensed GPL-3.0-or-later by the ScarletNX contributors. The adapter is based on Lizzie's [Eden PR 4580](https://git.eden-emu.dev/eden-emu/eden/pulls/4580), with its copyright and licensing retained in the source. The initial integration was built against ScarletNX revision `1b6f70a52dc4`, ABI 19.

Configure with `-DENABLE_SCARLETNX=ON` and set `ScarletNXRecompiler_DIR` to the installed package directory. Use `cpu_backend=2` in the emulator settings or set `EDEN_NX2_SCARLETNX=1`. Build and runtime selection are opt-in. Dynarmic remains the default and continues to handle 32-bit applications and system processes. ScarletNX requires multicore, fastmem, no debugger, and `use_reactive_flushing=false`. The recompiler emits direct host accesses, so it cannot implement the legacy mode that requires a callback on every access to a cached page. The explicit write/read traps used in the non-reactive mode still apply.

The recompiler uses a per-core exclusive reservation. Accuracy profiles that require the shared exclusive monitor continue to use Dynarmic.

Use this experimental backend with trusted guest code. The upstream recompiler assumes guest addresses are valid and does not bound its direct host memory accesses to the fastmem arena.

Guest code compiles in progressive batches using the ScarletNX C API. One sparse host virtual table publishes function entry points for all emulated cores. The table is 1 TiB of address space, with physical pages allocated only for published entries. The translated entry points and module cache are independent of the GPU. Cold compilations may occur throughout initialization and gameplay.

The adapter uses the emulator's SVC dispatch, including NVN HLE thunks. The NVN bridge identifies its generated instructions in the writable SDK data segment for its owning process. The compiler reads only the slot instructions from that segment, with the target table left to guest loads. A change to the guest PC or suspended context restarts native dispatch. For the guest counter frequency, only the compiler's snapshot rewrites `MRS Xt, CNTFRQ_EL0` to a private SVC. The host handler checks the original instruction at PC-4 before returning the game's configured frequency. The counter value itself comes from the emulator timing service. Mutable code is reread when invalidated.

If initialization or dispatch fails, the adapter logs why it selected Dynarmic. The first guest dispatch and per-core exit summary identify whether ScarletNX actually ran and whether the application later switched back. A runtime opcode rejected at an instruction boundary can be retried with Dynarmic. A host fault inside an arbitrary native store/load cannot provide an accurate guest instruction PC; an unresolved fault stops that guest thread.

For an A/B, use identical configurations apart from the backend. Set `use_reactive_flushing=false` in both profiles and verify the startup/exit logs before interpreting timings. Keep separate fresh/warm ScarletNX caches for any cold/warm experiment.
