# Optional Sail MPEG-2 picture decoder

This branch adds a host decoder component, not media playback. It borrows an
already-open local FILE and reads only the explicit payload offset/length through
custom FFmpeg AVIO. It selects the requested MPEG-2 PES stream (0x1E0..0x1EF),
decodes to the requested one-based ordinal, and returns actual dimensions, packed
RGBA bytes, source PTS validity/time base, frame rate and color metadata. It restores
the caller's file position and releases demux/codec/conversion resources on all paths.
No network URL, guest allocation, player state, timing or synthetic EOS is introduced.

Enable with `PS3RECOMP_SAIL_FFMPEG=ON`. CMake finds headers and avcodec/avformat/
avutil/swscale, or accept explicit PS3RECOMP_FFMPEG_INCLUDE_DIR and
PS3RECOMP_FFMPEG_{AVCODEC,AVFORMAT,AVUTIL,SWSCALE}_LIBRARY paths. Production code
has no dependency on a particular application's installation. FFmpeg9 is currently
supported; compile/runtime version integers must match exactly for public-struct access.
The default is OFF and compiles without FFmpeg. Consult the license of the FFmpeg
build you distribute; this branch does not bundle FFmpeg libraries.

Limits:512MiB payload,4096-square pictures,250000 packets,ordinal<=2000; synchronous
one-picture extraction, MPEG-2 yuv420p with explicit BT709/601 and known color range.
Unknown metadata is unsupported. Missing PTS remains explicitly unknown, never inferred.
Audio, clocks, seeking, buffers and native presentation are separate work.

`tools/test_sail_video_decoder.py` always tests the dependency-free unavailable path.
Set PS3RECOMP_TEST_FFMPEG_INCLUDE and PS3RECOMP_TEST_FFMPEG_LIB_DIR to run actual
synthetic decode and independent packed-RGBA SHA256 verification. The original formula-
generated32-picture fixture is CC0 (see tests/fixtures), not game media; its generator
uses proper public FFmpeg APIs. Corrupt/truncated inputs must not substitute earlier
pictures. An earlier intact picture may succeed before later corruption is encountered.
