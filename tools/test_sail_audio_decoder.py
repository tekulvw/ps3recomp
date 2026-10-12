#!/usr/bin/env python3
"""Bounded real ATRAC decoder checks; original media stays outside the repo."""
import os,subprocess,tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
def main():
    if os.name=='nt':print('SKIP standalone POSIX C fixture');return
    with tempfile.TemporaryDirectory(prefix='sail-audio-') as directory:
        tmp=Path(directory);cc=os.environ.get('CC','cc')
        stub=tmp/'disabled.c';stub.write_text('#include "sail_video_decoder.h"\n#include <assert.h>\nint main(){SailAudioPCM a={0};assert(sail_audio_decode_pcm(0,0,0,0,&a)==SAIL_VIDEO_UNAVAILABLE);sail_audio_pcm_release(&a);assert(!a.samples);}')
        subprocess.run([cc,'-std=c17','-I'+str(root/'libs/codec'),str(stub),str(root/'libs/codec/sail_video_decoder.c'),'-o',str(tmp/'disabled')],check=True,timeout=30)
        subprocess.run([str(tmp/'disabled')],check=True,timeout=5)
        inc=os.environ.get('PS3RECOMP_TEST_FFMPEG_INCLUDE');libs=os.environ.get('PS3RECOMP_TEST_FFMPEG_LIB_DIR');thx=os.environ.get('PS3RECOMP_TEST_THX_PAMF');intro=os.environ.get('PS3RECOMP_TEST_INTRO_PAMF')
        if not inc or not libs or not thx or not intro:print('PASS disabled audio; SKIP optional original-media decode (explicit FFmpeg include/libs and THX/INTRO PAMF paths required)');return
        paths=[]
        for name in ('avcodec','avformat','avutil','swscale'):
            files=sorted(Path(libs).glob('lib'+name+'.*.dylib')) or sorted(Path(libs).glob('lib'+name+'.so*'))
            if len(files)!=1:raise RuntimeError(f'expect one explicit {name} library: {files}')
            paths.append(str(files[0]))
        exe=tmp/'audio';subprocess.run([cc,'-std=c17','-D_POSIX_C_SOURCE=200809L','-DPS3RECOMP_SAIL_FFMPEG=1','-Wall','-Wextra','-Werror','-I'+inc,str(root/'tests/test_sail_audio_decoder.c'),str(root/'libs/codec/sail_video_decoder.c'),*paths,'-Wl,-rpath,'+libs,'-o',str(exe)],check=True,timeout=40)
        for media,channels,count in ((thx,2,290816),(intro,6,2897920)):
            subprocess.run([str(exe),media,str(channels),str(count)],check=True,timeout=30)
if __name__=='__main__':main()
