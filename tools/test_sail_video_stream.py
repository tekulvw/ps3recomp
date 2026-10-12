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
        exe=tmp/'stream'
        subprocess.run([cc,'-std=c17','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror','-DPS3RECOMP_SAIL_FFMPEG=1','-I'+inc,str(root/'tests/test_sail_video_stream.c'),str(root/'libs/codec/sail_video_decoder.c'),*libfiles,'-Wl,-rpath,'+libs,'-o',str(exe)],check=True,timeout=40)
        sample=root/'tests/fixtures/sail-synthetic-mpeg2.mpg'
        result=subprocess.run([str(exe),str(sample),'0',str(sample.stat().st_size),'32'],check=True,capture_output=True,text=True,timeout=20)
        for length,expected in ((sample.stat().st_size,-2),(sample.stat().st_size,-3),(sample.stat().st_size//2,-1),(833,-1),(2352,-1),(6327,-1),(8528,-1),(10445,-1),(12799,-1)):
            subprocess.run([str(exe),str(sample),'0',str(length),str(expected)],check=True,capture_output=True,text=True,timeout=20)
        assert len(result.stdout.splitlines())==32
        assert result.stdout.splitlines()[29].endswith('74e57ebd446e5dcc163920dfe306b730948d77b7683d0767b81cb8cac27d5b3e')
        print('PASS32 streamed synthetic pictures, metadata and exact drain EOF')
        retail=os.environ.get('PS3RECOMP_TEST_THX_PAMF');reference=os.environ.get('PS3RECOMP_TEST_THX_REFERENCE')
        if retail and reference:
            import json
            source=Path(retail);header=source.open('rb').read(16);offset=int.from_bytes(header[8:12],'big')*2048;length=int.from_bytes(header[12:16],'big')*2048
            result=subprocess.run([str(exe),retail,str(offset),str(length),'179'],check=True,capture_output=True,text=True,timeout=45)
            lines=result.stdout.splitlines();refs=json.loads(Path(reference).read_text())['pictures'];assert len(lines)==len(refs)==179
            for line,ref in zip(lines,refs):
                ordinal,pts,valid,duration,digest=line.split()
                assert digest==ref['rgba_sha256'],(line,ref['frame'])
                assert int(ordinal)==int(ref['frame']) and int(pts)==int(ref['pts'])
                assert int(valid)==(int(ordinal)<179) and int(duration)==3003
            print('PASS all179 retail picture hashes and terminal unknown PTS/drain EOF')
if __name__=='__main__':main()
