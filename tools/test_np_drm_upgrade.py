#!/usr/bin/env python3
"""Verify missing upgrade licenses, unsupported present files, and generated NIDs."""
"""Synthetic upgrade-license absence, denial, lookup and generated ABI checks.

Runs on POSIX and Windows (clang-cl); no original content, keys or game runs.
"""
from pathlib import Path
import os, subprocess, sys, tempfile
r=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="np-drm-upgrade-") as d:
 p=Path(d);gen=p/"nids.cpp"
 subprocess.run([sys.executable,str(r/"tools/gen_hle_nids.py"),"sceNpDrm","--out",str(gen)],check=True,timeout=60)
 win=os.name=="nt"
 cc=os.environ.get("CC","clang-cl" if win else "cc")
 cxx=os.environ.get("CXX","clang-cl" if win else "c++")
 objects=[]
 for n,src in enumerate([r/"libs/network/sceNpDrm.c",r/"tests/test_np_drm_upgrade.c"]):
  obj=p/f"{n}.obj"; objects.append(obj)
  flags=(["/nologo","/std:c17","/W3","/WX","/D_CRT_SECURE_NO_WARNINGS","/UNDEBUG","/I"+str(r/"include"),"/c",str(src),"/Fo"+str(obj)] if win else
         ["-std=gnu17","-Wall","-Wextra","-Werror","-UNDEBUG","-I"+str(r/"include"),"-c",str(src),"-o",str(obj)])
  subprocess.run([cc,*flags],check=True,timeout=60)
 exe=p/("fixture.exe" if win else "fixture")
 link=(["/nologo",str(gen),*map(str,objects),"/Fe:"+str(exe)] if win else [str(gen),*map(str,objects),"-o",str(exe)])
 subprocess.run([cxx,*link],check=True,timeout=60)
 cid="UP0000-TEST00000_00-SYNTHETIC0000000"
 # No home directory is a genuine missing license.
 subprocess.run([str(exe),str(p/"absent"),"0"],check=True,timeout=10)
 root=p/"root"
 for user in ["00000000","not-user","00000001","00000002"]:
  path=root/"home"/user/"exdata";path.mkdir(parents=True)
 # Unrelated IDs and invalid user directories cannot confer entitlement.
 (root/"home/00000000/exdata"/(cid+".edat")).write_bytes(b"not a license")
 (root/"home/not-user/exdata"/(cid+".edat")).write_bytes(b"not a license")
 (root/"home/00000001/exdata/UNRELATED.edat").write_bytes(b"not a license")
 subprocess.run([str(exe),str(root),"0"],check=True,timeout=10)
 # Scan all valid users. A present (even empty) file is explicitly unsupported.
 candidate=root/"home/00000002/exdata"/(cid+".edat")
 candidate.write_bytes(b"")
 subprocess.run([str(exe),str(root),"1"],check=True,timeout=10)
 candidate.unlink();candidate.mkdir()
 subprocess.run([str(exe),str(root),"0"],check=True,timeout=10)
 # A configured home that is a file is I/O failure, not absence.
 broken=p/"broken";broken.mkdir();(broken/"home").write_bytes(b"not a directory")
 subprocess.run([str(exe),str(broken),"2"],check=True,timeout=10)
