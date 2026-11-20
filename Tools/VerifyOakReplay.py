"""Check two additional camera angles and replay the decoded two-material cut."""
import numpy as np
from BenchmarkOakAssembly import joint_mask
from CompareOakJoint import finalist
from PackageOakAssembly import CHOICES
from ReplayOakAssembly import OUT,UE,capture,measure,quality,read_mask,stage,write

def run():
    out=[]
    exact=[]
    for d in (41,100):
        raw=OUT/f"joint-nanite{d}.mesh"
        mask=joint_mask("live-materials",finalist(raw),d,132)
        ue=read_mask(UE/f"joint-cut-{d}.png",True)
        exact.append(dict(d=d,ue_pixels=ue.sum(),extra=(mask&~ue).sum(),missing=(ue&~mask).sum(),
                          iou=next(measure(ue,[(0,0,mask)]))[2]))
        for yaw in (45,225):
            target,edge=quality(UE/f"wood-test-{d}-{yaw}.png",read_mask(UE/f"wood-test-{d}-{yaw}-fine.png",True),True)
            ref=stage("untuned-reference",OUT/"wood.mesh")
            source=read_mask(capture(ref,"untuned-source",d,yaw,one_sided=True)[0])
            test=stage("untuned-pilot",OUT/CHOICES[d])
            image,r=capture(test,"untuned-assembly",d,yaw,one_sided=True)
            ti,te=quality(image,source)
            out.append(dict(d=d,yaw=yaw,triangles=r["wood"],iou=ti,edge=te,ue_iou=target,ue_edge=edge,passed=ti>=target and te<=edge))
    write("additional-views.csv",out)
    write("joint-readback-control.csv",exact)

if __name__=="__main__":run()
