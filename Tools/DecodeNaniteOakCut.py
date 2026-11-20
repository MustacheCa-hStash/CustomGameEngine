"""Decode the selected static oak triangles of a UE 5.8 Nanite GPU dump.

The input layout follows NaniteDefinitions.h, NaniteDataDecode.ush and
NaniteAttributeDecode.ush. Preserve the selected cluster/transform records
alongside the D3D viewer mesh so individual ranges can be traced back to UE.
"""
import argparse
import json
import struct
from pathlib import Path

import numpy as np


def records(path):
    data = path.read_text().strip()
    decoder = json.JSONDecoder()
    while data:
        record, end = decoder.raw_decode(data)
        yield record
        data = data[end:].lstrip()


def packed_fields(data, start, stride, count, widths, offset=0):
    indices = np.arange(count, dtype=np.int64) * stride + offset
    result = []
    for width in widths:
        address = start + indices // 8
        value = np.zeros(count, dtype=np.uint64)
        for byte in range((int(width) + 14) // 8):
            value |= data[address + byte].astype(np.uint64) << (8 * byte)
        result.append((value >> (indices % 8).astype(np.uint64)) & ((1 << int(width)) - 1))
        indices += int(width)
    return np.column_stack(result).astype(np.int64)


def decode_cluster(data, page, cid):
    clusters = int.from_bytes(data[page:page+2].tobytes(), "little")
    if cid >= clusters:
        raise ValueError("Invalid page or cluster index")
    def header(row):
        return struct.unpack_from("<4I", data, page + 16 * (1 + cid + row * clusters))
    vword, iword, _, colors = header(0)
    nv, nt = vword & 16383, iword & 255
    if not 0 < nt <= 128:
        raise ValueError("Expected a triangle cluster")
    x,y,z,fmt = header(1)
    widths = [(fmt >> shift) & 31 for shift in (9,14,19)]
    pos = packed_fields(data, page+(vword >> 14), sum(widths), nv, widths)
    pos = (pos + np.array([x,y,z], dtype="<u4").view("<i4")) * 2.**(20-((fmt >> 3) & 63))
    ibits = (fmt & 7)+1
    tri = packed_fields(data,page+(iword >> 8),ibits+10,nt,[ibits,5,5])
    tri[:,1:] += tri[:,:1]
    assert tri.max() < nv
    attr, info, uv_offsets, encoding = header(5)
    if encoding < 0xfe000000:
        a = ((encoding >> 18) & 127) + 1
        b = (encoding >> 25) & 127
        slots = np.repeat([encoding & 63, (encoding >> 6) & 63, (encoding >> 12) & 63], [a,b,nt-a-b])
    else:
        slots = np.full(nt,255,np.uint8)
        offset = page + 4*(encoding & 0x7ffff)
        for word in struct.unpack_from("<"+"I" * (1+((encoding>>19)&63)),data,offset):
            begin, count = word & 255, (word>>8)&255
            slots[begin:begin+count] = (word>>16)&63
        assert (slots < 64).all()
    attr_addr, stride = page+(attr & 0x3fffff), attr >> 22
    bits = (fmt >> 24) & 15
    normal = packed_fields(data,attr_addr,stride,nv,[bits,bits]) * (2/((1<<bits)-1)) - 1
    normal = np.column_stack([normal, 1-np.abs(normal).sum(axis=1)])
    normal[:,:2] -= np.where(normal[:,:2]>=0,1,-1)*np.maximum(-normal[:,2:],0)
    normal /= np.linalg.norm(normal,axis=1)[:,None]
    uv = []
    assert not info & (1<<22), "The oak fixture uses implicit tangents"
    base = bits*2 + sum((colors >> s)&15 for s in (0,4,8,12))
    for i in range((info>>24)&7):
        limits = struct.unpack_from("<2I",data,page+(info & 0x3fffff)+8*i)
        val = packed_fields(data,attr_addr,stride,nv,[lim & 31 for lim in limits],
                            base+((uv_offsets>>(8*i))&255)) + [lim>>5 for lim in limits]
        neg = val < (1<<19)
        encoded = np.where(neg,~val,val) & ((1<<19)-1)
        f = (0x3f000000+(encoded<<9)).astype("<u4").view("<f4")
        uv.append(np.where(neg,-1.,1.)*np.minimum(f*2-1,f))
    assert uv
    return pos, normal, uv[0], uv[min(1,len(uv)-1)], tri, slots


def extract(folder, output, streaming_mb=512):
    paths = {r["Name"]: next((folder/"Resources").glob(r["UniqueResourceName"]+".*.bin"))
             for r in records(folder/"Base/ResourceDescs.json")}
    def array(name,dtype):
        return np.memmap(paths["Nanite."+name],mode="r",dtype=dtype)
    sw,_,_,_,hw,*_ = array("MainRasterizeArgsSWHW","<u4")
    assert sw and not hw, "Expected a software-raster triangle capture"
    selected = array("VisibleClustersSWHW","u1")[:int(sw)*12].reshape(-1,12)
    pages = array("StreamingManager.ClusterPageData","u1")
    transforms = array("AssemblyTransforms","<f4").reshape(-1,3,4)
    threshold = streaming_mb << (20-17)
    vertices, indices, trace, materials = [], [], [], []
    first_vertex = first_face = 0
    cache = {}
    for raw in selected:
        word = int.from_bytes(raw.tobytes(),"little")
        ref, instance = (word>>41) & 0x7fffff, (word>>64) & 0xffffff
        if ref < threshold*256:
            p,c = divmod(ref,256)
            page = p<<17
        else:
            p,c = divmod(ref-threshold*256,64)
            page = (threshold<<17)+(p<<15)
            p += threshold
        if ref not in cache:
            cache[ref] = decode_cluster(pages,page,c)
        pos,norm,uv0,uv1,tri,slots = cache[ref]
        if instance != 0xffffff:
            m = np.array(transforms[instance],dtype=float)
            pos = pos @ m[:,:3].T + m[:,3]
            norm = norm @ np.linalg.inv(m[:,:3])
            norm /= np.linalg.norm(norm,axis=1)[:,None]
            if np.linalg.det(m[:,:3]) < 0:
                tri = tri[:,[0,2,1]]
        trace.append(dict(page=p, cluster=c, instance=instance, first_triangle=first_face,
                          triangles=len(tri), transform=transforms[instance].tolist() if instance!=0xffffff else None))
        vertices.append(np.column_stack([pos[:,[0,2,1]]*.01,norm[:,[0,2,1]],uv0,uv1]))
        indices.append(tri+first_vertex)
        materials.append(slots)
        first_face += len(tri)
        first_vertex += len(pos)
    v = np.concatenate(vertices).astype("<f4")
    ix = np.concatenate(indices).astype("<u4")
    assert np.isfinite(v).all()
    output.write_bytes(struct.pack("<II",len(v),ix.size)+v.tobytes()+ix.tobytes())
    output.with_suffix(".mat").write_bytes(np.concatenate(materials).astype("u1").tobytes())
    output.with_suffix(".clusters.json").write_text(json.dumps(trace,separators=(",",":")))
    return len(trace),len(ix)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump",type=Path)
    parser.add_argument("mesh",type=Path)
    parser.add_argument("--streaming-mb",type=int,default=512)
    args = parser.parse_args()
    print("Clusters and triangles:",extract(args.dump,args.mesh,args.streaming_mb))
