#!/usr/bin/env python3
"""Real optional decoder regression; disabled builds have no FFmpeg dependency."""
import hashlib, os, subprocess, tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
def main():
    if os.name=='nt':
        print('SKIP: standalone C fixture needs POSIX toolchain');return
    with tempfile.TemporaryDirectory(prefix='sail-video-') as tmp:
        tmp=Path(tmp);cc=os.environ.get('CC','cc')
        stub=tmp/'stub.c';stub.write_text('#include "sail_video_decoder.h"\n#include <assert.h>\nint main(){SailVideoFrame f={0};assert(!sail_video_available());assert(sail_video_decode_picture(0,0,0,0,0,&f)==SAIL_VIDEO_UNAVAILABLE);sail_video_frame_release(&f);}')
        subprocess.run([cc,'-std=c17','-I'+str(root/'libs/codec'),str(stub),str(root/'libs/codec/sail_video_decoder.c'),'-o',str(tmp/'stub')],check=True,timeout=30)
        subprocess.run([str(tmp/'stub')],check=True,timeout=5)
        inc=os.environ.get('PS3RECOMP_TEST_FFMPEG_INCLUDE');libs=os.environ.get('PS3RECOMP_TEST_FFMPEG_LIB_DIR')
        if not inc or not libs:
            print('PASS disabled decoder; SKIP optional decode (set PS3RECOMP_TEST_FFMPEG_INCLUDE and PS3RECOMP_TEST_FFMPEG_LIB_DIR)');return
        libfiles=[]
        for lib in ('avcodec','avformat','avutil','swscale'):
            matches=sorted(Path(libs).glob('lib'+lib+'.*.dylib')) or sorted(Path(libs).glob('lib'+lib+'.so*'))
            if len(matches)!=1:raise RuntimeError(f'expect one explicit {lib} library, got {matches}')
            libfiles.append(str(matches[0]))
        exe=tmp/'decode';rgba=tmp/'picture.rgba'
        subprocess.run([cc,'-std=c17','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror','-DPS3RECOMP_SAIL_FFMPEG=1','-I'+inc,str(root/'tests/test_sail_video_decoder.c'),str(root/'libs/codec/sail_video_decoder.c'),*libfiles,'-Wl,-rpath,'+libs,'-o',str(exe)],check=True,timeout=40)
        subprocess.run([str(exe),str(root/'tests/fixtures/sail-synthetic-mpeg2.mpg'),str(rgba)],check=True,timeout=15)
        assert hashlib.sha256(rgba.read_bytes()).hexdigest()=='74e57ebd446e5dcc163920dfe306b730948d77b7683d0767b81cb8cac27d5b3e'
        print('PASS actual RGBA checksum matches independent reference')
if __name__=='__main__':main()
