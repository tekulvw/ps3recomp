#!/usr/bin/env python3
"""Explicit Vulkan owner/provider fixture, headless and validation-required."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shlex
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-only',action='store_true')
    parser.add_argument('--require-hardware',action='store_true')
    parser.add_argument('--gpu-index',default='0');parser.add_argument('--vendor-id',default='0');parser.add_argument('--device-id',default='0')
    parser.add_argument('--report',type=Path);args=parser.parse_args()
    if args.report:args.report.unlink(missing_ok=True)
    flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','vulkan','sdl2'],text=True))
    libraries=shlex.split(subprocess.check_output(['pkg-config','--libs','sdl2'],text=True))
    sources=['libs/video/tests/test_native_owner_vulkan.c']+['libs/video/'+name+'.c' for name in
        ('rsx_draw_engine','rsx_commands','rsx_dispatch','rsx_texture_layout','rsx_vertex_compact','rsx_vertex_fetch','rsx_vp_decompiler','rsx_fp_decompiler')]+[
        'runtime/diagnostics/critical_trace.c','runtime/diagnostics/render_ablation.c','libs/video/rsx_shader_spirv.cpp']
    with tempfile.TemporaryDirectory(prefix='native-owner-vulkan-') as tmp:
        objects=[]
        for index,source in enumerate(sources):
            output=Path(tmp)/f'{index}.o';cxx=source.endswith('.cpp')
            command=[os.environ.get('CXX','c++')if cxx else os.environ.get('CC','cc'),'-std=c++17'if cxx else'-std=gnu17',
                '-O1','-g','-UNDEBUG','-ffunction-sections','-fdata-sections','-Iinclude','-Ilibs/video',*flags,'-c',source,'-o',str(output)]
            subprocess.run(command,cwd=ROOT,check=True,timeout=60);objects.append(str(output))
        binary=Path(tmp)/'fixture'
        subprocess.run([os.environ.get('CXX','c++'),*objects,'-Wl,--gc-sections','-pthread','-ldl','-lm',*libraries,'-o',str(binary)],cwd=ROOT,check=True,timeout=60)
        if args.build_only:print('Vulkan owner provider fixture compiled; GPU execution pending');return
        environment=dict(os.environ,PS3RECOMP_VK_DEVICE=args.gpu_index,VK_INSTANCE_LAYERS='VK_LAYER_KHRONOS_validation')
        loader=subprocess.check_output(['pkg-config','--variable=libdir','vulkan'],text=True).strip()
        environment['LD_LIBRARY_PATH']=os.pathsep.join(filter(None,[loader,environment.get('LD_LIBRARY_PATH','')]))
        # Per-operation submissions, then batched recording (PS3RECOMP_VK_BATCHED_ENGINE=1).
        for batched in ('0','1'):
            result=subprocess.run([str(binary),str(int(args.require_hardware)),args.vendor_id,args.device_id],env=dict(environment,PS3RECOMP_VK_BATCHED_ENGINE=batched),capture_output=True,text=True,timeout=20)
            print(result.stdout,end='');print(result.stderr,end='');result.check_returncode()
            if any(m in result.stdout+result.stderr for m in('VUID-','THREADING ERROR')):raise RuntimeError('Vulkan validation rejected the fixture')
        if args.report:
            tracked=sources+['libs/video/rsx_vulkan_backend.c','libs/video/rsx_vulkan_backend.h','libs/video/rsx_native_owner.h','libs/video/rsx_native_owner_gate.h','libs/video/rsx_original_owner.h','tools/check_native_owner_vulkan.py']
            record={'status':'passed','evidence':'actual provider lifecycle and queue boundaries; no frontend shader/game acceptance','hardware_required':args.require_hardware,
                'validation_required':True,'source_sha256':{name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest()for name in tracked},'stdout':result.stdout,'stderr':result.stderr}
            args.report.parent.mkdir(parents=True,exist_ok=True);args.report.write_text(json.dumps(record,indent=2)+'\n')
if __name__=='__main__':main()
