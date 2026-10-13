# Outgoing DMA publication observer

This diagnostic snapshots the source of validated outgoing direct/list PUTs immediately before the normal copy. It never modifies the local store, guest memory, DMA command, tag status or transfer ordering. A record proves the bytes submitted to that copy, not subsequent GPU consumption or a successful atomic commit. PUTLLC/PUTLLUC are outside this hook.

Set `SPU_DMA_PUBLICATION=<hexEAlo>-<hexEAhi>` to snapshot complete, naturally aligned BE32 words in the intersection of an outgoing transfer and that half-open guest range. Partial boundary words are excluded.

To find a pointer before a descriptor buffer is reused, set both `SPU_DMA_PUBLICATION_MATCH_RANGE=<hexEAlo>-<hexEAhi>` and `SPU_DMA_PUBLICATION_MATCH_WORD=<hexBE32value>`. Only complete aligned words in that transfer's intersection are searched. Each transfer scans at most 16 KiB. A match logs its guest address and up to 16 surrounding source words, clipped to the configured range and transfer. The match is always included in that window. Zero is a valid search value.

`SPU_DMA_PUBLICATION_WORDS` chooses 1–16 words per record (default 16). `SPU_DMA_PUBLICATION_LIMIT` chooses 1–32768 records (default 128). Both modes share one process-global atomic cap, including calls from different generated translation units and concurrent SPU workers. Configuration initializes once using the platform's synchronized once primitive. Malformed or incomplete configuration disables both modes. With no configured mode, there are no additional local-store reads. Once the cap is exhausted, the diagnostic stops scanning. No guest addresses are selected by default.

Records include image ID, physical SPU ID, current PC, command, transfer LSA/EA/size, sample EA, word count, truncation and match EA. Payload is comma-separated raw big-endian words. `kind=span` samples the watched intersection; `kind=match` samples a matching descriptor word. Data is a pre-copy source snapshot; enabling logging changes host timing. The record cap bounds output, while the per-transfer bound limits work even when a selected descriptor range contains no matches.

Run `python3 tools/test_spu_dma_publication.py` for manufactured direct/list PUT data, unchanged DMA bytes, range clipping, exact descriptor matches, trailing 16 KiB matches, strict option validation, unreadable-LS disabled mode, concurrent cap and C++ to C linkage. This fixture does not require game data.
