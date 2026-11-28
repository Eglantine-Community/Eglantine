# Terrain resource history (emulated GPU)

This opt-in recorder follows OoT's native terrain fragment program
`44c5c4f3fdaaf250` through the Vulkan emulated GPU renderer. It does not instrument
NVN/VTX-HLE and does not change shaders, cache policy, uploads or guest inputs.

Enable **debug knob 13** through the existing HTTP MCP while the floor is clean:

```sh
python3 -I tools/mcp/scripts/mcp.py debug_knobs '{"set":[13]}'
```

The default directory is the emulator's log directory plus `terrain-trace`
(normally `~/.local/share/eglantine/log/terrain-trace`). Alternatively launch the
emulator with `EDEN_NX2_TERRAIN_TRACE=/absolute/output/directory` to record from
boot. The environment enables recording independently of knob13; clearing the
knob only disables recording when that environment variable is absent.

At the location to compare, save a clean checkpoint:

```sh
python3 -I tools/nx2/terrain_trace.py checkpoint clean
```

When the crosshatch first appears, stay in that session and save its recent
history immediately:

```sh
python3 -I tools/nx2/terrain_trace.py checkpoint bad
```

Use `--directory` for a custom output directory. The script waits up to15seconds
for the rasterizer to acknowledge the trigger and reports failure if it cannot.
Triggers are consumed between presented frames. Pausing the game can delay them.

## Data and bounds

- Sampled terrain frames every60presents, at most128matching draws per sampled
  frame. Image lifecycle events are recorded between snapshots as well.
- Actual TICs, raw TIC/TSC descriptors, view swizzles, formats, mip/layer ranges,
  image identities, addresses, flags and modification ticks. Native shader stage
  debug `reads` are still compile-time metadata; `texture-handles` and
  `texture-binding` are the current draw's inputs.
- Image registration/deletion, CPU invalidation, GPU writes, upload requests,
  device/GPU unmapping, and alias-copy source/destination identities. An upload
  event says the upload path was entered, not that a queued decode completed.
- Bound buffer mappings, map serials, cache identities, reuse/deletion and actual
  upload ranges. Buffer lifecycle tracking begins after a terrain snapshot has
  watched the range; it does not claim to reconstruct earlier history.
- Guest source hashes: first4KiB of images and storage buffers, first64KiB of
  uniform buffers. GPU-modified guest contents may be stale. Mapped stream and
  imported uniform hashes come from the actual descriptor's CPU-visible memory,
  capped at64KiB. Hash size is always included; zero size means unavailable.
- Repeated image GPU-write and alias-copy/source events are coalesced per image
  and event type within each60-frame interval. Creation, deletion, upload and
  unmapping events are not coalesced. GPU writes also coalesce when the compute
  path leaves `first_written_frame` at zero. The header counts suppressed repeats.
- R8G8 normal-map lifecycle changes and producer draws (including skipped draws)
  also enter a separate2MiB/4096event journal, saved as `critical-events.jsonl`
  at bad checkpoints, disable and stop. Ordinary render-target traffic cannot
  displace those events. Both journals report their own dropped count. Compute
  producers are coalesced per image identity/program within each60-frame interval.
- `normal-invalidation` records the caller (`OnCPUWrite`, `InnerInvalidation`,
  `InvalidateRegion`, and other rasterizer paths), written device range, image
  device range, and whether the write covers the whole generated normal image.
  This is recorded before a previously GPU-modified normal is invalidated.
- `read-trap-arm` records requests to protect CPU reads of GPU-written images.
  In a build with texture read traps, check for these and subsequent `download`
  events before CPU relocation or invalidation. An arm request alone does not
  prove the guest read the image or that a download completed.
- The in-memory main journal retains at most8MiB or16384events, whichever fills first.
  Its header counts discarded events. Record immediately when corruption starts.
- `baseline.jsonl` saves the first sampled terrain frame automatically.
  `clean.jsonl` saves an explicit clean checkpoint. `history.jsonl` and
  `previous-history.jsonl` retain the two latest bad/manual/disable/stop dumps.
  Checkpoints write bounded metadata on the rasterizer thread and may cause a
  brief hitch. No continuous image or buffer readback runs.

Inspect a saved journal with:

```sh
python3 -I tools/nx2/terrain_trace.py inspect ~/.local/share/eglantine/log/terrain-trace/history.jsonl
```

## Explicit GPU readbacks

While tracing is enabled, the existing `capture_frame` with `buffers:true` also
captures the actual Vulkan buffer descriptors used by terrain draws as
`kind:host_descriptor`. This includes stream/imported uniforms, independently of
the buffer cache's guest binding descriptions. Each is read once per
(host buffer,offset,range) per capture, capped at16MiB, sufficient for the current
9.5MiB clustered-light list. The descriptor ordinal joins to
`host-buffer-binding` in periodic trace snapshots; frame-capture draw numbers
join within that capture. The original buffer entries remain separately labeled.
These are host GPU copies taken in command order. They require a frame capture,
can interrupt render passes and may stutter. The existing2GiB capture cap still
applies; always check `summary.truncated` and each entry's `size`/`bound_size`.
The journal alone does not hash opaque device-local storage or image contents.

Preserve clean and bad checkpoints plus screenshots at the **same location and
camera**, ideally before moving or reloading. Differences in another scene's
lights, handles or tile data are not evidence of corruption.
