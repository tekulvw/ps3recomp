# SPU writes land line by line for the FIFO walker

An MFC transfer that writes main memory (`PUT` and its fenced, barrier and
list forms, and `SDCRZ`) is carried out by the Cell's bus in naturally aligned
128-byte lines. A reader on the other side, such as the RSX front end
fetching commands, sees each line either as it was before the transfer or as
the transfer left it. It never sees a line half old and half new. Lines of
one transfer can land in any order.

A title can build a protocol on that. It is enough to start every 128-byte
line of a region it will fill later with a word that stops the reader. For
the RSX that is a JUMP to its own address, a park. The reader then stops at
the first line that has not landed yet. Once the line's first word has
changed, the whole line is the new one.

## The problem

The runtime carries a PUT out as one host copy into guest memory. A copy is
not line-atomic. It writes the destination in pieces of whatever size the
host copy routine uses, and the SPU's host thread can lose its CPU in the
middle of it. The FIFO walker reads guest memory word by word on another
thread. When it catches up with a stalled copy it can read a line whose
first word is already new and whose second half is still old.

Seen in MAG (Mac lab, load 3 to 5, any mode). A template job fills each
1 KiB command slot with stale command and constant data, with a park at
every 128-byte line start and at the start of each body. The header job and
the 752-byte body job then fill the slot. The body PUT's copy stalled for 2
to 22 µs. The walker followed the new words, including the new zero padding
that had replaced the park at a line start, past the copy's position. It
then read the old second half of that line: a float from the stale constant
block (`0x3DBC809A`, `0x3F7FFFFF`, `0x3F0E170D`). It decoded it as a JUMP
to unmapped IO or as an unknown word, and the stream derailed:

- `[cellGcmSys] FIFO JUMP to unmapped IO … (word 0x3F7FFFFF) -- not taken`;
- `[gcm checked REF] failed reason=unmapped-JUMP …; reclamation frozen`;
- `[gcm checked REF] unknown word …`.

No further labels were released. About 30 s later the SPU job polling one
of them (EA `0xB00004xx`) was halted by the identical-transfer guard. The
recorder showed the word the walker derailed on in the copy's before-image
at the same offset, and the copy's completion 2 to 22 µs after the walker
had read past it. No `SDCRZ` touched the region, and no semaphore acquire
was involved.

## Rule

Within each 128-byte guest line that an SPU write touches, the first
aligned word the write stores in that line becomes visible last:

1. the rest of the write's bytes in that line are stored first;
2. then that first word is stored with release ordering.

This applies to every DMA write path: direct and list `PUT` in each of the
copy branches (plain, under the line lock, partly under the line lock, and
the coherence-version write) and `SDCRZ`. It also applies to a whole-line
commit (`PUTLLC`/`PUTLLUC` under coherence versions), whose first 8 bytes
are stored last.

A segment shorter than 8 bytes, or one whose start is not 4-byte aligned,
is stored as before. One aligned store of at most 8 bytes is already
single-copy atomic on every supported host, and the MFC's own small
transfers are naturally aligned.

The FIFO walker loads the first command word it reads in each line with
acquire ordering. If it sees a line's first word changed, it therefore also
sees every other byte that the same write stored in that line. If it sees
the old first word (a park), it parks and re-reads it on the next pass.

## Scope and limits

- This gives the walker the guarantee the park idiom needs ("first word
  changed, so the line is complete"). It is not full line atomicity: a
  reader that starts in the middle of a line, or reads the tail before the
  first word, can still see a mix. The walker reads a line from its first
  word, or from a branch target, which a producer writes as the first word of
  its own segment.
- Lines can still land in any order, as on the hardware. A title must park
  every line it fills later, and MAG does.
- PPU stores are not affected. They are individual stores on the hardware
  too.
- There is no switch. The order of stores inside one transfer is not
  guest-visible except to a concurrent reader, and that reader now sees what
  the hardware allows. The extra cost is one 4-byte release store per line
  written.

## Test

`python3 tools/test_spu_put_line_order.py` drives real direct and list
`PUT`s and `SDCRZ` through the MFC path. The test routes every host copy
the path makes through a recorder that copies byte by byte, ascending. After
every byte it checks every line of the destination, as a walker preempting
the producer at that byte would see it: if a line's first word has changed,
the rest of that line must already be new. The destination starts with a
park at each line start and stale data elsewhere, the shape MAG uses. The
cases are a 752-byte `PUTF` body starting 0x20 into a line (plain copy, one
line reserved, so the partly locked branch, and a one-line write under the
lock), a two-element `PUTL` and a 1 KiB `SDCRZ`. List elements are separate
writes, so the rule holds per element. On `c45bf830` the check fails on the
body's first line: its first word is new while the rest of the line is
still old.
