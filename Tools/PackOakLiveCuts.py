"""Convert the resident UE triangle BVH and reusable spray tiers to camera-independent D3D draw data."""
import argparse
import json
import struct
import tempfile
from pathlib import Path

import numpy as np
from DecodeNaniteOakCut import decode_cluster, records
from PackageReusableOak import layout
from ReusableOakSprays import ROOT, TRANSFORMS, expand_parts
from ReplayOakAssembly import BUILD
from ScoreReusableOak import slots

OUT = BUILD / "oak-live-selection"
IDENTITY = len(TRANSFORMS)
U32_MAX = 0xffffffff
NODE = struct.Struct("<12f4I")
SWAP = [0, 2, 1]

def native_buffer(folder, name):
    desc = next(x for x in records(folder/"Base/ResourceDescs.json") if x["Name"] == "Nanite."+name)
    return np.memmap(next((folder/"Resources").glob(desc["UniqueResourceName"]+".*.bin")),mode="r",dtype="u1")

class Package:
    def __init__(self):
        self.nodes = []
        self.vertices, self.indices, self.ranges, self.tiers = [], [], [], []
        self.vertex_count = self.index_count = 0
        self.regions = []
        self.replace = 0
        self.unloaded = 0
        self.sources = np.array([p for p,q,s in TRANSFORMS])

    def geometry(self, vertices, faces, labels):
        index = len(self.ranges)
        self.vertices.append(vertices.astype("<f4"))
        ranges = []
        if labels.max() > 2: raise ValueError(f"Unexpected oak material {labels.max()}")
        for slot in range(3):
            subset = (faces[labels == slot] + self.vertex_count).astype("<u4")
            ranges.extend((self.index_count, subset.size))
            self.indices.append(subset.ravel())
            self.index_count += subset.size
        self.vertex_count += len(vertices)
        self.ranges.append(ranges)
        return index

    def native(self, subject, materials=(0,1,2)):
        pointer=OUT/"native-input.txt"
        root=Path(pointer.read_text(encoding="utf-8-sig").strip()) if pointer.exists() else OUT
        folder = root/f"dump-{subject}"
        hier = native_buffer(folder,"StreamingManager.Hierarchy")
        pages = native_buffer(folder,"StreamingManager.ClusterPageData")
        base,count,transform_base,nt,root = map(int,(root/f"layout-{subject}.txt").read_text(encoding="utf-8-sig").split())
        transforms = np.frombuffer(hier,"<f4",nt*12,transform_base*4).reshape(-1,3,4).astype(float)
        source_ids = []
        for m in transforms:
            p = m[:,3][SWAP]*.01
            distance = np.sum((self.sources-p)**2,axis=1)
            index = distance.argmin()
            if distance[index] > 1e-8: raise ValueError(f"Unmatched native placement {p}")
            source_ids.append(index)
        cache = {}
        def cluster(ref):
            if ref in cache:return cache[ref]
            p,c=divmod(ref,256)
            page=p<<17 if p<4096 else (512<<20)+((p-4096)<<15)
            nc=struct.unpack_from("<H",pages,page)[0]
            if c>=nc:raise ValueError(f"Invalid resident cluster {p}:{c}/{nc}")
            def header(row,fmt):return struct.unpack_from(fmt,pages,page+16*(1+c+row*nc))
            sphere=header(2,"<4f")
            cx,cy,cz,error,edge=header(3,"<3f2e")
            ex,ey,ez,flags=header(4,"<3fI")
            pos,n,u,v,tri,s=decode_cluster(pages,page,c)
            # The full UE tree has trunk in slot 0 and the assembly part slots follow it.
            if subject=="crown":s=np.array([2,0,1])[s]
            keep=np.isin(s,materials)
            geometry=self.geometry(np.column_stack((pos[:,SWAP]*.01,n[:,SWAP],u,v)),tri[keep],s[keep]) if keep.any() else None
            cache[ref]=(sphere,(cx,cy,cz),(ex,ey,ez),0 if flags&2 else error,geometry)
            return cache[ref]
        def record(sphere,center,extent,minimum,maximum,kind,geom,xf):
            c=np.asarray(center);e=np.asarray(extent);s=np.asarray(sphere[:3]);scale=1.
            if xf!=65535:
                m=transforms[xf];a=m[:,:3];scale=np.linalg.norm(a,axis=0).max()
                c=a@c+m[:,3];s=a@s+m[:,3];e=abs(a)@e
            return [*(s[SWAP]*.01),sphere[3]*scale*.01,*(c[SWAP]*.01),*(e[SWAP]*.01),
                    minimum*scale*.01,maximum*scale*.01,0,kind,geom,IDENTITY if xf==65535 else source_ids[xf]]
        def visit(node,xf):
            if node>=count:raise ValueError(f"Invalid native hierarchy node {node}/{count}")
            address=(base+60*node)*4
            for k in range(4):
                sphere=struct.unpack_from("<4f",hier,address+16*k)
                cx,cy,cz,maximum,minimum=struct.unpack_from("<3f2e",hier,address+64+16*k)
                ex,ey,ez,ref=struct.unpack_from("<3fI",hier,address+128+16*k)
                key,packed,_=struct.unpack_from("<3I",hier,address+192+12*k)
                leaf=key!=U32_MAX;n=(packed>>16)&511
                if leaf and not (key or n):continue
                if ref==U32_MAX:
                    self.unloaded+=1
                    continue
                transform=packed&65535
                if transform==65535:transform=xf
                row=record(sphere,(cx,cy,cz),(ex,ey,ez),minimum,maximum,int(leaf),0,transform)
                self.nodes.append(row)
                if leaf:
                    for ci in range(n):
                        sphere2,c,e,error,g=cluster(ref+ci)
                        if g is None:continue
                        draw=record(sphere2,c,e,error,0,2,g,transform)
                        self.nodes.append(draw);draw[12]=len(self.nodes)
                else:visit(ref,transform)
                row[12]=len(self.nodes)
                if self.nodes[-1] is row:self.nodes.pop()
        visit(root,65535)
        if subject=="crown":self.replace=7
        else:
            batches=layout()[int(subject)]
            self.regions.append((batches[0][0],sum(len(g[1]) for g in batches)))

    def sprays(self,subject,materials=(0,1),midrange_margin=1):
        if not self.tiers:
            recipe=json.loads((ROOT/"policy.json").read_text())
            for limit,path in [*recipe,(float("inf"),"spray.mesh")]:
                vertices,faces,labels=slots(ROOT/path)
                keep=np.isin(labels,materials)
                if limit>42:limit*=midrange_margin
                self.tiers.append((limit,self.geometry(vertices,faces[keep],labels[keep])))
        if subject=="crown":
            ids=np.arange(IDENTITY);self.replace=3
        else:
            batches=layout()[int(subject)];ids=np.concatenate([g[1] for g in batches])
            self.regions.append((batches[0][0],len(ids)))
        lo,hi=np.array(json.loads((ROOT/"spray-bounds.json").read_text()))
        v=np.zeros((8,10));v[:,:3]=np.array(np.meshgrid(*zip(lo,hi))).reshape(3,-1).T;v[:,3]=1
        xyz=expand_parts((v,np.zeros((1,3),np.uint32)),ids)[0][:,:3].reshape(-1,8,3)
        for i,points in zip(ids,xyz):
            lo,hi=points.min(0),points.max(0)
            self.nodes.append([0,0,0,0,*((lo+hi)/2),*((hi-lo)/2+1e-5),0,0,len(self.nodes)+1,3,0,i])

    def save(self,path):
        def array(f,data):
            f.write(struct.pack("<I",len(data)));f.write(data.tobytes())
        placements=np.array([[*p,*q,*s] for p,q,s in TRANSFORMS]+[[0,0,0,0,0,0,1,1,1,1]],"<f4")
        with tempfile.NamedTemporaryFile(dir=path.parent,delete=False) as f:
            pending=Path(f.name)
            f.write(struct.pack("<8sI",b"OAKCUT01",self.replace))
            array(f,np.asarray(self.regions,"<u4").reshape(-1,2))
            array(f,placements)
            f.write(struct.pack("<I",len(self.nodes)))
            for node in self.nodes:f.write(NODE.pack(*node))
            array(f,np.asarray(self.ranges,"<u4"))
            f.write(struct.pack("<I",len(self.tiers)))
            for t,g in self.tiers:f.write(struct.pack("<fI",t,g))
            array(f,np.concatenate(self.vertices));array(f,np.concatenate(self.indices))
        pending.replace(path)
        print(path.name,"nodes",len(self.nodes),"unique geometry",len(self.ranges),"unloaded leaves",self.unloaded,flush=True)

def pack_finalist(arm,subject,spray_margin):
    package=Package()
    if arm in ("canopy","canopy1"):
        if subject!="crown":raise ValueError("The native trunk belongs to the full crown")
        package.native(subject,materials=(2,))
    package.sprays(subject,midrange_margin=1 if arm=="canopy1" else spray_margin)
    if arm in ("canopy","canopy1"):package.replace=7
    if package.unloaded:raise ValueError("Incomplete native trunk")
    package.save(OUT/f"{arm}-{subject}.oakcut")

if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--subjects",nargs="+",default=["7","8","both","crown"])
    parser.add_argument("--hybrid-only",action="store_true")
    parser.add_argument("--finalist",choices=["tiered","canopy","canopy1"])
    parser.add_argument("--spray-margin",type=float,default=.9)
    args=parser.parse_args()
    for subject in args.subjects:
        if args.finalist:
            pack_finalist(args.finalist,subject,args.spray_margin)
            continue
        for name in (("hybrid",) if args.hybrid_only else ("sprays","native","hybrid")):
            p=Package()
            for b in (("7","8") if subject=="both" else (subject,)):
                if name=="hybrid":
                    p.native(b,materials=(0,))
                    p.sprays(b,materials=(1,))
                    p.regions=list(dict.fromkeys(p.regions))
                else:getattr(p,name)(b)
            if p.unloaded:raise ValueError(f"{subject} is missing {p.unloaded} hierarchy references")
            p.save(OUT/f"{name}-{subject}.oakcut")
