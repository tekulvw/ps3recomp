# Audio ring observations

Set `AUDIO_DIAGNOSTICS=1` to observe guest PCM and the final stereo submission.
Other values leave these diagnostics disabled. No audio device starts in the
synthetic regression fixture.

The per-port `peaks`, `nonzero`, and `nonfinite` fields cover samples consumed
during the reporting window. The following fields describe one complete ring
snapshot, taken before the first block consumed in that window:

| Field | Meaning |
| --- | --- |
| `ring_ridx` | Read slot when the ring snapshot was taken. |
| `ring_mask` | Bit N is set if slot N holds a finite, nonzero sample in any channel. |
| `ring_peak` | Greatest finite absolute sample value across all channels and slots. |
| `ring_nonfinite` | Number of NaN or infinite samples in that snapshot. |

The ring scan visits all samples once per reporting window, bounded by the
supported 32 slots and eight channels. It does not move a cursor, rewrite PCM,
change gain, submit extra samples, or wake a producer. Samples in future slots
remain untouched until the existing mixer consumes them.

`nonzero=0` together with a nonempty `ring_mask` distinguishes an empty consumed
slot from an entirely empty ring at the snapshot boundary. A zero mask does not
prove that no producer wrote between snapshots. Producer-side DMA observations
or a stopped-process capture can resolve that remaining distinction. These
fields diagnose sample delivery; they do not establish audible host output or
correct scheduling.

Validation: `python3 tools/test_audio_diagnostics.py` exercises all eight
channels and all 32 slots, including bit 31, preserves every unconsumed byte,
accounts for a nonfinite sample, checks unchanged stereo downmix and block
clearing, and checks disabled/enabled output, SDL errors, and window reset.
