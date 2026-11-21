"""Compare a flowering tuft with conservative binary voxels from two standing-height azimuths."""

import csv

import sys

from pathlib import Path

import numpy as np

from PIL import Image, ImageDraw, ImageFont


import distant as d



def voxels(tri, n):

    grid=np.zeros((n,n,n),bool)

    for batch in np.array_split(tri*n, max(1,len(tri)//160)):

        lo=np.maximum(0,np.floor(batch.min(1)).astype(int))

        hi=np.minimum(n-1,np.floor(batch.max(1)).astype(int))

        size=hi-lo+1

        count=size.prod(1)

        ids=np.repeat(np.arange(len(batch)),count)

        offsets=np.arange(len(ids))-np.repeat(np.cumsum(count)-count,count)

        s=size[ids]

        boxes=lo[ids]+np.stack([offsets%s[:,0],offsets//s[:,0]%s[:,1],offsets//(s[:,0]*s[:,1])],axis=1)

        centers=boxes+.5

        edges=np.roll(batch,-1,axis=1)-batch

        axes=[np.cross(edges[:,0],edges[:,1])]

        for i in range(3):

            for e in np.eye(3): axes.append(np.cross(edges[:,i],e))

        keep=np.ones(len(ids),bool)

        for axis in axes:

            dots=np.einsum('nkj,nj->nk',batch,axis)

            center=(centers*axis[ids]).sum(1)

            r=np.abs(axis[ids]).sum(1)*.5+1e-10

            keep &= (center+r>=dots.min(1)[ids]) & (center-r<=dots.max(1)[ids])

        grid[tuple(boxes[keep].T)]=True

    return grid



def silhouette(tris,axis,res=1536,volume=0):

    out=np.zeros((res,res),bool)

    if volume:

        sub=res//volume

        hits=np.zeros((volume,volume,volume,sub,sub),bool)

    for world in np.array_split(tris,max(1,len(tris)//100)):

        batch=world[:,:,[axis,1]]*res-.5

        lo=np.maximum(0,np.ceil(batch.min(1)).astype(int))

        hi=np.minimum(res-1,np.floor(batch.max(1)).astype(int))

        size=np.maximum(0,hi-lo+1)

        count=size.prod(1)

        ids=np.repeat(np.arange(len(batch)),count)

        offset=np.arange(len(ids))-np.repeat(np.cumsum(count)-count,count)

        if not len(ids): continue

        xy=lo[ids]+np.stack([offset%size[ids,0],offset//size[ids,0]],axis=1)

        a,b,c=batch[ids].transpose(1,0,2)

        def cross(u,v): return u[:,0]*v[:,1]-u[:,1]*v[:,0]

        det=cross(b-a,c-a)

        u=cross(xy-a,c-a);v=cross(b-a,xy-a)

        sign=np.sign(det)

        ok=(sign!=0)&(u*sign>=0)&(v*sign>=0)&((det-u-v)*sign>=0)

        x,y=xy[ok].T

        out[y,x]=True

        if volume:

            weights=np.stack(((det[ok]-u[ok]-v[ok])/det[ok],u[ok]/det[ok],v[ok]/det[ok]),axis=1)

            depth=(world[ids[ok],:,2-axis]*weights).sum(1)

            z=np.clip((depth*volume).astype(int),0,volume-1)

            hits[y//sub,x//sub,z,y%sub,x%sub]=True

    if volume:

        # Integrate independently filtered directional opacity along the ray.

        opacity=hits.mean(axis=(3,4))

        return (1-(1-opacity).prod(2))[::-1]

    return out[::-1]




def main():
    m=d.tuft.BASE.oak.OakGlb(d.tuft.ASSETS/d.tuft.BASE.MODEL.name)

    node=next(n for n in m.doc['nodes'] if n['name']=='Full grass strip')

    tri=np.concatenate([m.array(p['attributes']['POSITION'])[m.array(p['indices']).ravel()].reshape(-1,3,3)

                        for p in m.doc['meshes'][node['mesh']]['primitives']]).astype(float)

    low=tri.min((0,1));high=tri.max((0,1))

    side=max(high-low)+.02

    tri=(tri-(low+high)/2)/side+.5

    ref=[silhouette(tri,i) for i in (0,2)]

    out=d.OUT

    font=ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf',20)

    img=Image.new('RGB',(320*4,358),(246,246,241))

    def paint(slot,name,mask):

        atlas=Image.fromarray(np.uint8(np.rint(mask.astype(float)*255))).resize((320,320),Image.Resampling.BOX if mask.shape[0]>320 else Image.Resampling.NEAREST)

        ink=Image.new('RGB',(320,320),(44,72,43))

        img.paste(ink,(slot*320,38),atlas)

        ImageDraw.Draw(img).text((slot*320+6,5),name,fill=(32,39,33),font=font)

    paint(0,'Source silhouette',ref[0])

    rows=[]

    for slot,n in enumerate((16,32,64),1):

        occ=voxels(tri,n)

        masks=[occ.any(2).T[::-1],occ.any(0)[::-1,:]]

        if n in (32,64): paint(slot-1,f'{n}³ solid occupancy',masks[0])

        for axis,(source,mask) in enumerate(zip(ref,masks)):

            fractional=silhouette(tri,axis*2,n*16,volume=n)

            if n==32 and axis==0: paint(3,'32³ fractional coverage',fractional)

            rows.append(dict(fractional_coverage=float(fractional.mean()),cells=n,cell_mm=1000*side/n,occupied=int(occ.sum()),azimuth=axis*90,

                source_coverage=float(source.mean()),solid_coverage=float(mask.mean()),ratio=float(mask.mean()/source.mean())))

    with (out/'voxel-silhouette.csv').open('w',newline='') as f:

        w=csv.DictWriter(f,fieldnames=rows[0]);w.writeheader();w.writerows(rows)

    img.save(out/'voxel-silhouette.png')

    for row in rows: print(row)



if __name__=="__main__":
    main()
