"""Stage the stock bough grid and a hierarchy of shared local-space spray ranges."""
import json
import hashlib
import struct
import tempfile
from pathlib import Path
import numpy as np
from functools import lru_cache
from ReusableOakSprays import ROOT, expand_parts
from ScoreReusableOak import slots
from ReplayOakAssembly import TREE,PREVIOUS,Hierarchy,viewer_assets

def array(values,fmt):
    a=np.asarray(values,dtype=fmt)
    return struct.pack("<I",a.size)+a.tobytes()

@lru_cache(maxsize=1)
def layout():
    h=Hierarchy((ROOT/"grid-native.treelod").read_bytes())
    order=np.frombuffer(h.order,"<u4")
    # Enclose the shared source and every candidate used in the experiment.
    bounds=ROOT/"spray-bounds.json"
    if bounds.exists():lo,hi=np.array(json.loads(bounds.read_text()))
    else:
        lo=np.full(3,np.inf); hi=-lo
        for p in [ROOT/"spray.mesh",*ROOT.glob("*/level*.mesh"),*ROOT.glob("native-part/*.mesh")]:
            v=slots(p)[0][:,:3]
            lo=np.minimum(lo,v.min(0));hi=np.maximum(hi,v.max(0))
        bounds.write_text(json.dumps([lo.tolist(),hi.tolist()]))
    corners=np.array(np.meshgrid(*zip(lo,hi))).reshape(3,-1).T
    v=np.zeros((8,10));v[:,:3]=corners;v[:,3]=1
    regions=json.loads((ROOT/"regions.json").read_text())
    result={}
    for node,b in enumerate(regions,2):
        start,n=struct.unpack_from("<II",h.parts[0][1],node*56+48)
        ordered=order[start:start+n]
        assert sorted(ordered)==regions[b]
        batches=[]
        for first in range(0,n,12):
            ids=ordered[first:first+12]
            xyz=expand_parts((v,np.zeros((1,3),np.uint32)),ids)[0][:,:3]
            lower,upper=xyz.min(0),xyz.max(0)
            nodebox=struct.pack("<6f",*((lower+upper)/2),*((upper-lower)/2+1e-5))
            batches.append((start+first,ids,nodebox))
        result[int(b)]=batches
    return result

def put_bounds(node,children):
    c,e=np.array(struct.unpack_from("<6f",node)).reshape(2,3)
    lower,upper=c-e,c+e
    for child,_ in children:
        cc,ce=np.array(struct.unpack_from("<6f",child)).reshape(2,3)
        lower=np.minimum(lower,cc-ce);upper=np.maximum(upper,cc+ce)
    struct.pack_into("<6f",node,0,*((lower+upper)/2),*((upper-lower)/2+1e-5))

def package(label, policy=None, only=None, source=False, live=None):
    """only selects one bough for a diagnostic, using degenerate draws elsewhere."""
    folder=viewer_assets("reusable/"+label,PREVIOUS/"current")
    h=Hierarchy((ROOT/"grid-native.treelod").read_bytes())
    h.header=h.header[:8]+Hierarchy((PREVIOUS/"current/assets"/TREE).read_bytes()).header[8:]
    flags=[]
    groups=layout()
    for slot,part in enumerate(h.parts):
        old=[part[1][i:i+56] for i in range(0,len(part[1]),56)]
        for node in old:node[32:36]=node[36:40]
        # Counter-only sentinel is subtracted in isolated reports.
        dummy=(len(part[3])//4,3)
        part[3]+=bytes(12)
        if slot==2:
            if only is not None:struct.pack_into("<IIff",old[0],24,*dummy,0.,0.)
            part[1]=b"".join(old);flags.append([]);continue
        ranges={}; material_ranges={}
        def geometry(name):
            if name not in ranges:
                v,i,s=slots(ROOT/name)
                i=i[s==slot]
                if not len(i):raise ValueError(f"Missing slot {slot} in {name}")
                # Stable first use in each material range.
                u,idx=np.unique(i.ravel(),return_index=True);use=u[np.argsort(idx)]
                remap=np.zeros(len(v),np.uint32);remap[use]=np.arange(len(use))
                vertices=v[use].astype("<f4").tobytes();local=remap[i]
                key=hashlib.sha256(vertices+local.astype("<u4").tobytes()).digest()
                if key not in material_ranges:
                    material_ranges[key]=(len(part[3])//4,i.size)
                    part[3]+=(local+len(part[2])//40).astype("<u4").tobytes()
                    part[2]+=vertices
                ranges[name]=material_ranges[key]
            return ranges[name]
        def chain(group,tiers):
            first,ids,box=group
            node=bytearray(box+struct.pack("<IIff4I",0,0,0.,0.,0,0,first,len(ids)))
            tree=(node,False,[])
            for threshold,path in reversed(tiers):
                parent=node.copy()
                error=0 if threshold==0 else 1/threshold
                struct.pack_into("<IIff",parent,24,*geometry(path),error,error)
                tree=(parent,True,[tree]);node=parent
            return tree
        def original(i):
            n=old[i].copy(); first,count=struct.unpack_from("<2I",n,40)
            return n,False,[original(x) for x in range(first,first+count)]
        root=original(0)
        for i,b in enumerate([None,*groups],1):
            node=root[2][i-1][0].copy()
            if only is not None and only!=b:
                struct.pack_into("<IIff",node,24,*dummy,0.,0.)
                root[2][i-1]=(node,False,[])
            elif b is not None and live:
                struct.pack_into("<IIff",node,24,*geometry(live),0.,0.)
                root[2][i-1]=(node,False,[])
            elif b is not None and (source or policy):
                if source:tiers=[(0,"spray.mesh")]
                else:tiers=policy
                children=[chain(g,tiers) for g in groups[b]]
                struct.pack_into("<IIff",node,24,0,0,0.,0.)
                put_bounds(node,[(c[0],None) for c in children])
                replacement=(node,False,children)
                if source:root[2][i-1]=replacement
                else:
                    route=node.copy()
                    struct.pack_into("<2f",route,32,1/95,1/95)
                    root[2][i-1]=(route,2,[replacement,root[2][i-1]])
        put_bounds(root[0],[(c[0],None) for c in root[2]])
        nodes=[];inst=[]
        def emit(tree,idx):
            n,repeat,kids=tree;n=n.copy()
            first=len(nodes) if kids else 0
            struct.pack_into("<II",n,40,first,len(kids));nodes[idx]=n;inst[idx]=int(repeat)
            nodes.extend([None]*len(kids));inst.extend([0]*len(kids))
            for ix,k in enumerate(kids):emit(k,first+ix)
        nodes.append(None);inst.append(0);emit(root,0)
        part[1]=b"".join(nodes);flags.append(inst if any(inst) else [])
    if any(flags):
        h.header=b"TREELOD4"+h.header[8:]
        h.tail=b"".join(array([],"<u4") for _ in h.parts)+b"".join(array(f,"u1") for f in flags)
    target = folder/"assets"/TREE
    with tempfile.NamedTemporaryFile(dir=target.parent, prefix=target.stem+"-", delete=False) as pending:
        temporary = Path(pending.name)
    try:
        h.write(temporary)
        temporary.replace(target)
    finally:
        temporary.unlink(missing_ok=True)
    return folder

if __name__=="__main__":
    policy=json.loads((ROOT/"policy.json").read_text())
    print(package("pilot",policy)/"CustomGameEngine.exe")
