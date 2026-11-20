"""Validate and time the automatic bough pilot and its same-engine controls."""
import argparse
import numpy as np
from CompareOakJoint import finalist, stage_joint
from PackageOakAssembly import BUDGET, CHOICES, first_use, package
from ReplayOakAssembly import BUILD, OUT, PREVIOUS, TREE, UE, Hierarchy, capture, measure, quality, read_mask, reference, stage, viewer_assets, write

SEED = {41:"seed-custom01-epic/level01.mesh",100:"seed-epic03-custom/level01.mesh"}
JOINT = {41:"joint-epic-mixedtest/level02.mesh",100:"joint-epic-mixedtest/level03-paired.mesh"}
SEPARATE = {41:("leaf-epic-mixedtest/level02.mesh","epic-0.5/level02.mesh"),
            100:("leaf-epic-mixedtest/level04.mesh","custom-0.68/level06.mesh")}

def control(label, path=None):
    src = PREVIOUS/("grid" if label=="packed-grid" else "current")
    dest = viewer_assets(label,src)
    h = Hierarchy((path or src/"assets"/TREE).read_bytes())
    if path:
        h.header = h.header[:8]+Hierarchy((src/"assets"/TREE).read_bytes()).header[8:]
        for part in h.parts:
            for i in range(0,len(part[1]),56):
                part[1][i+32:i+36]=part[1][i+36:i+40]
    else:
        first_use(h.parts[1])
    h.write(dest/"assets"/TREE)
    return dest

def joint_view(label,parts,leaf_only=False):
    if leaf_only:
        parts = [parts[0],(np.zeros((1,10),"<f4"),np.zeros((1,3),"<u4"))]
    return stage_joint(label,parts)

def joint_mask(label,parts,d,yaw):
    # For opaque geometry the coverage of (two-sided leaves U one-sided wood)
    # is the union of two hardware masks, independent of which surface wins depth.
    tag = f"jm-{label}-{d}"
    combined = OUT/tag
    leaves = OUT/(tag+"-leaves")
    if yaw==132:
        joint_view(tag,parts)
        joint_view(tag+"-leaves",parts,True)
    mask = read_mask(capture(combined,tag,d,yaw,one_sided=True)[0])
    return mask | read_mask(capture(leaves,leaves.name,d,yaw)[0])

def check():
    pilot = package()
    current = control("production-new",BUILD/"Release/assets"/TREE)
    old = control("production-old",OUT/"production-before.treelod")
    grid = control("packed-grid")
    assert (old/"assets"/TREE).read_bytes()==(PREVIOUS/"current/assets"/TREE).read_bytes()
    assert (current/"assets"/TREE).read_bytes()==(PREVIOUS/"current-firstuse/assets"/TREE).read_bytes()
    output=[]
    joint_rows=[]
    transitions=[]
    near, near_counts=capture(pilot,"fallback-24",24,budget=1)
    base, base_counts=capture(grid,"grid-fallback-24",24,budget=1)
    assert near_counts["wood"]==base_counts["wood"] and np.array_equal(read_mask(near),read_mask(base))
    transitions.append(dict(d=24,yaw=132,selection="existing hierarchy",wood=near_counts["wood"],identical=True))
    for d in (41,100):
        cut=OUT/CHOICES[d]
        forced = stage(f"frozen-{d}",cut,True)
        isolated = stage(f"selected-{d}",cut)
        seeded = stage(f"seeded-{d}",OUT/SEED[d])
        variant = cut.parent.name+"-"+cut.stem
        for yaw in (132,90,180):
            p, r = capture(pilot,"auto-"+variant,d,yaw)
            f, v = capture(forced,"frozen-"+variant,d,yaw)
            equal = r["wood"]==v["wood"] and np.array_equal(read_mask(p),read_mask(f))
            assert equal,(d,yaw,r,v)
            transitions.append(dict(d=d,yaw=yaw,selection=CHOICES[d],wood=r["wood"],identical=equal))
        write("auto-selection.csv",transitions)
        for name, viewer in (("old",old),("production",current),("grid",grid),("assembly",pilot)):
            image,c=capture(viewer,"whole-"+name+("-"+variant if name=="assembly" else ""),d)
            qi,qe=quality(image,reference(d,whole=True))
            output.append(dict(d=d,yaw=132,scope="whole",cut=name,triangles=c["triangles"],iou=qi,edge=qe))
        source_parts=finalist(OUT/"leaf.mesh",OUT/"wood.mesh")
        separate_parts=finalist(*[OUT/k for k in SEPARATE[d]])
        joint_parts=finalist(OUT/JOINT[d])
        for yaw in (132,90,180):
            for name,viewer in (("assembly",isolated),("seeded",seeded)):
                p,r=capture(viewer,name+"-stock"+("-"+variant if name=="assembly" else ""),d,yaw)
                qi,qe=quality(p,reference(d,yaw))
                output.append(dict(d=d,yaw=yaw,scope="wood",cut=name,triangles=r["wood"],iou=qi,edge=qe))
            ref=joint_mask("source",source_parts,d,yaw)
            suffix="" if yaw==132 else f"-yaw{yaw}"
            qi0,qe0=quality(UE/f"joint-cut-{d}{suffix}.png",read_mask(UE/f"joint-cut-{d}{suffix}-fine.png",True),True)
            for name,parts in (("separate",separate_parts),("joint",joint_parts)):
                mask=joint_mask(name,parts,d,yaw)
                m=next(measure(ref,[(0,0,mask)]))
                joint_rows.append(dict(d=d,yaw=yaw,cut=name,triangles=sum(len(i) for v,i in parts),
                                       iou=m[2],edge=m[-1],ue_iou=qi0,ue_edge=qe0,passed=m[2]>=qi0 and m[-1]<=qe0))
        write("final-masks.csv",output)
        write("joint-hardware.csv",joint_rows)

def timing():
    rows=[]
    for d in (41,100):
        stage(f"live{d}-wood",OUT/f"nanite{d}.mesh")
        stage(f"selected-{d}",OUT/CHOICES[d])
        samples=[("old","production-old",False),("production","production-new",False),
                 ("grid","packed-grid",False),("assembly","assembly-pilot",False),
                 ("wood",f"selected-{d}",False),("wood-ue-raster",f"selected-{d}",True),
                 ("nanite-replay",f"live{d}-wood",True),
                 ("previous-grid",PREVIOUS/"iso-grid",False),("previous-connected",PREVIOUS/"iso-cut",False),
                 ("joint",f"jm-joint-{d}",False),("separate",f"jm-separate-{d}",False)]
        for j in range(4):
            for name,viewer,front in samples if j%2==0 else reversed(samples):
                budget={"previous-grid":.3875 if d==41 else .8,
                        "previous-connected":.4100 if d==41 else .3425}.get(name,BUDGET[d])
                tag = ("timed-mesh-raster-" if front else "timed-")+f"{name}-{j}"
                p,r=capture(OUT/viewer,tag,d,budget=budget,shaded=True,one_sided=front)
                rows.append(dict(d=d,run=j,cut=name,budget=budget,**r))
            write("final-timings.csv",rows)

if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("job",choices=("check","time"))
    {"check":check,"time":timing}[parser.parse_args().job]()
