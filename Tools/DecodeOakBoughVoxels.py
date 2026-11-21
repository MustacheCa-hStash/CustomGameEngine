"""Decode UE 5.8 visible triangle and voxel clusters into the exposed oak camera fixture."""
import csv
import json
import struct
from pathlib import Path

import numpy as np
from PIL import Image
from DecodeNaniteOakCut import decode_cluster, packed_fields, records
from PackageReusableOak import layout
from PrepareOakAssembly import save
from ScoreOakAssembly import FOCUS
from ReplayOakAssembly import BUILD, UE

ROOT = BUILD/"oak-bough-voxel"
BITS = np.array([[i%4,i//4%4,i//16] for i in range(64)])

def write_csv(path, rows):
    with path.open("w", newline="") as f:
        w=csv.DictWriter(f,rows[0].keys()); w.writeheader(); w.writerows(rows)

def pack_normals(axis):
    n=axis/np.maximum(np.abs(axis).sum(1,keepdims=True),1e-20)
    xy=np.where(n[:,2:]<0,(1-np.abs(n[:,[1,0]]))*np.where(n[:,:2]>=0,1,-1),n[:,:2])
    x,y=np.rint((xy+1)*2047.5).clip(0,4095).astype("<u4").T
    return x | y<<12 | 255<<24

def write_bake(path, slots, replacement, group=None, mixed=False):
    group=layout()[7] if group is None else group
    with path.open("wb") as f:
        f.write(struct.pack("<8sIII",b"OAKMX001" if mixed else b"OAKVX001",group[0][0],sum(len(g[1]) for g in group),replacement))
        for bricks in slots:
            f.write(struct.pack("<II",len(bricks),sum(b[1].bit_count() for b in bricks)))
            if not bricks:continue
            axes=[]; records=[]; normals=[]; offsets=[]; count=0
            for frame,mask,n in bricks:
                assert n.shape==(mask.bit_count(),3)
                cells=BITS[[i for i in range(64) if mask>>i&1]]
                bounds=sum(int(k)<<(3*i) for i,k in enumerate([*cells.min(0),*(cells.max(0)+1)]))
                records.append((0,0,0,mask&0xffffffff,mask>>32,bounds))
                orientation=np.zeros((4,4),"<f4");orientation[:,:3]=frame
                orientation[0,3]=np.linalg.det(frame[1:])<0
                axes.append(orientation);offsets.append(count);count+=len(n)
                packed=pack_normals(n)
                normals.append(np.column_stack((packed,packed,np.zeros((len(n),2),"<u4"))))
            for a in (np.asarray(records,"<u4"),np.asarray(axes,"<f4"),np.asarray(offsets,"<u4"),np.concatenate(normals)):
                f.write(a.tobytes())

def material_slots(data,page,enc,count):
    if enc<0xfe000000:
        a=((enc>>18)&127)+1;b=(enc>>25)&127
        return np.repeat([enc&63,(enc>>6)&63,(enc>>12)&63],[a,b,count-a-b])
    slots=np.full(count,255)
    for word in struct.unpack_from("<"+"I"*(1+((enc>>19)&63)),data,page+4*(enc&0x7ffff)):
        begin=word&255; slots[begin:begin+((word>>8)&255)]=(word>>16)&63
    return slots

def extract(d):
    pointer=ROOT/f"dump-{d}-path.json"
    dump=ROOT/(json.loads(pointer.read_text()) if pointer.exists() else f"dump{d}")
    files={}
    for a in records(dump/"Base/ResourceDescs.json"):
        files.setdefault(a["Name"].removeprefix("Nanite."),next((dump/"Resources").glob(a["UniqueResourceName"]+".*.bin")))
    args=np.fromfile(files["MainRasterizeArgsSWHW"],"<u4",8)
    assert args[0]>0 and args[4]==0, args
    visible=np.fromfile(files["VisibleClustersSWHW"],"u1",int(args[0])*12).reshape(-1,12)
    pages=np.memmap(files["StreamingManager.ClusterPageData"],mode="r",dtype="u1")
    transforms=np.memmap(files["AssemblyTransforms"],mode="r",dtype="<f4").reshape(-1,3,4)
    slots=[[],[]]; vv=[];ii=[];mm=[];nv=0; trace=[]; sizes=[]; refs=set(); inst=set()
    yaw,pitch=np.radians([132,-2.235 if d==100 else -5.46])
    back=np.array([np.sin(yaw)*np.cos(pitch),np.sin(pitch),np.cos(yaw)*np.cos(pitch)])
    for raw in visible:
        word=int.from_bytes(raw.tobytes(),"little");ref=(word>>41)&0x7fffff;instance=(word>>64)&0xffffff
        if ref<4096*256:p,c=divmod(ref,256);page=p<<17
        else:p,c=divmod(ref-4096*256,64);page=(4096<<17)+(p<<15);p+=4096
        nc=struct.unpack_from("<H",pages,page)[0]
        assert c<nc
        def h(row):return struct.unpack_from("<4I",pages,page+16*(1+c+row*nc))
        m=transforms[instance].astype(float) if instance!=0xffffff else np.eye(3,4)
        lin=m[:,:3]; convert=lambda pos:(pos@lin.T+m[:,3])[:,[0,2,1]]*.01
        def normal(n):
            n=(n@np.linalg.inv(lin))[:,[0,2,1]]
            return n/np.maximum(np.linalg.norm(n,axis=1)[:,None],1e-12)
        nt=h(0)[1]&255
        row=dict(page=p,cluster=c,instance=instance,triangles=nt,bricks=0,cells=0,edge_cm=0)
        if nt:
            if d==100:
                pos,n,u,v,ix,s=decode_cluster(pages,page,c)
                vv.append(np.column_stack([convert(pos),normal(n),u,v]));mm.append(s)
                ii.append((ix[:,[0,2,1]] if np.linalg.det(lin)<0 else ix)+nv);nv+=len(pos)
        else:
            refkey=(p,c);refs.add(refkey)
            if instance!=0xffffff:inst.add(instance)
            desc=h(6)[1];nb=desc>>22;off=page+(desc&0x3fffff)
            edge=struct.unpack("<e",struct.pack("<H",h(3)[3]&65535))[0]
            axes=(np.eye(3)*edge@lin.T)[:,[0,2,1]]*.01
            # World edge equivalent by volume. Project the transverse longest axis at each brick.
            edge_m=abs(np.linalg.det(axes))**(1/3)
            projected=max(np.linalg.norm(a-a.dot(back)*back) for a in axes)
            mats=material_slots(pages,page,h(5)[3],nb)
            nn=h(0)[0]&16383; row.update(bricks=nb,cells=nn,edge_cm=edge)
            if d==100:
                attr=h(5)[0];bits=(h(1)[3]>>24)&15
                octa=packed_fields(pages,page+(attr&0x3fffff),attr>>22,nn,[bits,bits])*(2/((1<<bits)-1))-1
                n=np.column_stack([octa,1-np.abs(octa).sum(1)])
                n[:,:2]-=np.where(n[:,:2]>=0,1,-1)*np.maximum(-n[:,2:],0)
                n=normal(n)
            cells=0
            for bi in range(nb):
                lo,hi,z,w,attr=struct.unpack_from("<5I",pages,off+20*bi)
                mask=int(f"{(hi<<32)|lo:064b}"[::-1],2);count=mask.bit_count();cells+=count
                pos=(w<<32)|z
                xyz=np.array([[((((pos>>s)&0x7ffff)^0x40000)-0x40000) for s in (6,25,44)]])
                lower=convert(xyz*edge)[0];depth=d-(lower+axes.sum(0)*2-FOCUS).dot(back)
                sizes.append((edge,edge_m*100,2607.35065*projected/depth,count))
                if d==100:
                    at=attr&65535
                    slots[mats[bi]].append((np.vstack([lower,axes]),mask,n[at:at+count]))
            assert cells==nn,(cells,nn)
        trace.append(row)
    write_csv(ROOT/f"clusters-{d}.csv",trace)
    if d==100:
        save(ROOT/"native.mesh",np.concatenate(vv),np.concatenate(ii))
        (ROOT/"native.mat").write_bytes(np.concatenate(mm).astype("u1").tobytes())
        write_bake(ROOT/"native.oakvx",slots,0)
    pix=np.asarray(Image.open(UE/f"vx7-{d}-Voxels.png").convert("RGB"))
    red,green=pix[:,:,0],pix[:,:,1];colored=red!=green
    out=dict(distance=d,clusters=len(trace),tris=sum(t["triangles"] for t in trace),bricks=sum(t["bricks"] for t in trace),
        cells=sum(t["cells"] for t in trace),unique_voxel_clusters=len(refs),part_instances=len(inst),
        instanced_bricks=sum(t["bricks"] for t in trace if t["instance"]!=0xffffff),
        voxel_pixels=int((green>red).sum()),visible_pixels=int(colored.sum()))
    s=np.array(sizes).reshape(-1,4)
    for col,label in enumerate(("local_cm","world_cm","projected_px")):
        values=np.percentile(np.repeat(s[:,col],s[:,3].astype(int)),[10,50,90]) if len(s) else [None]*3
        for q,v in zip((10,50,90),values):out[f"{label}_p{q}"]=v
    return out

if __name__=="__main__":
    rows=[]
    for d in (41,50,52,56,58,100):
        rows.append(extract(d));print(rows[-1],flush=True)
    write_csv(ROOT/"native-cuts.csv",rows)
