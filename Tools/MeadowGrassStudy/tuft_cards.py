"""Bake standing-height cutouts from the actual textured tuft, in eight azimuths."""
import copy
import io
import numpy as np
from PIL import Image

import oak_baseline as base


CELL = 512
SS = 3


def material_image(model, ref):
    image = model.doc["images"][model.doc["textures"][ref["index"]]["source"]]
    view = model.doc["bufferViews"][image["bufferView"]]
    start = view.get("byteOffset", 0)
    return np.array(Image.open(io.BytesIO(model.payload[start:start+view["byteLength"]])).convert("RGB"))/255


def srgb(rgb):
    return np.where(rgb <= .0031308, 12.92*rgb, 1.055*np.maximum(rgb, 0)**(1/2.4)-.055)


def primitives(model, mesh):
    parts = []
    for prim in model.doc["meshes"][mesh]["primitives"]:
        attrs = prim["attributes"]
        mat = model.doc["materials"][prim["material"]]
        pbr = mat["pbrMetallicRoughness"]
        pos = model.array(attrs["POSITION"])
        tex = material_image(model, pbr["baseColorTexture"]) if "baseColorTexture" in pbr else None
        uv = model.array(attrs["TEXCOORD_0"]) if tex is not None else None
        detail = mat.get("normalTexture")
        parts.append((pos, model.array(attrs["NORMAL"]), model.array(prim["indices"]).reshape(-1,3),
                      uv, tex, np.array(pbr.get("baseColorFactor",[1]*4)[:3]), mat["name"],
                      material_image(model,detail) if detail else None,detail.get("scale",1) if detail else 1))
    return parts


def projection(parts, angle, radius, floor, top):
    size = CELL*SS
    front = np.array([np.sin(angle),0,np.cos(angle)])
    right = np.array([np.cos(angle),0,-np.sin(angle)])
    depth = np.full(size*size, -np.inf)
    color = np.zeros((size*size,3),np.float32)
    normal = np.zeros_like(color)
    tissue = np.zeros(size*size,np.float32)
    for pos, normals, faces, uv, tex, factor, name, detail, strength in parts:
        x = (.5 + pos@right/(2*radius))*size-.5
        y = (top-pos[:,1])/(top-floor)*size-.5
        z = pos@front
        for begin in range(0,len(faces),96):
            tri = faces[begin:begin+96]
            tx,ty = x[tri],y[tri]
            x0,y0 = np.ceil(tx.min(1)).astype(int),np.ceil(ty.min(1)).astype(int)
            width = np.maximum(0, np.floor(tx.max(1)).astype(int)-x0+1)
            height = np.maximum(0, np.floor(ty.max(1)).astype(int)-y0+1)
            areas = width*height
            ids = np.repeat(np.arange(len(tri)),areas)
            offset = np.arange(len(ids))-np.repeat(areas.cumsum()-areas,areas)
            px,py = x0[ids]+offset%width[ids], y0[ids]+offset//width[ids]
            dx1,dx2 = tx[:,1]-tx[:,0], tx[:,2]-tx[:,0]
            dy1,dy2 = ty[:,1]-ty[:,0], ty[:,2]-ty[:,0]
            det = dx1*dy2-dx2*dy1
            dx,dy = px-tx[ids,0],py-ty[ids,0]
            with np.errstate(divide="ignore",invalid="ignore"):
                b = (dx*dy2[ids]-dy*dx2[ids])/det[ids]
                c = (dx1[ids]*dy-dy1[ids]*dx)/det[ids]
            inside = (b>=0)&(c>=0)&(b+c<=1)
            corners = tri[ids[inside]]
            weights = np.stack([1-b[inside]-c[inside],b[inside],c[inside]],axis=1)
            pixels = py[inside]*size+px[inside]
            dist = (weights*z[corners]).sum(1)
            np.maximum.at(depth,pixels,dist)
            win = dist==depth[pixels]
            pixels,corners,weights = pixels[win],corners[win],weights[win,:,None]
            if not len(pixels): continue
            rgb = factor
            if tex is not None:
                coords = (uv[corners]*weights).sum(1)%1
                sample = tex[(coords[:,1]*tex.shape[0]).astype(int),(coords[:,0]*tex.shape[1]).astype(int)]
                rgb = factor*np.where(sample<=.04045,sample/12.92,((sample+.055)/1.055)**2.4)
            color[pixels] = rgb
            n = (normals[corners]*weights).sum(1)
            n /= np.maximum(1e-8,np.linalg.norm(n,axis=1,keepdims=True))
            if name.startswith("Lamina |"):
                sign = np.where(n@front<0,-1,1)[:,None]
                if detail is not None:
                    dp = pos[corners[:,1:]]-pos[corners[:,:1]]
                    duv = uv[corners[:,1:]]-uv[corners[:,:1]]
                    handed = np.sign(duv[:,0,0]*duv[:,1,1]-duv[:,1,0]*duv[:,0,1])[:,None]
                    t = (dp[:,0]*duv[:,1,1:2]-dp[:,1]*duv[:,0,1:2])*handed
                    t -= (t*n).sum(1,keepdims=True)*n
                    t /= np.maximum(1e-8,np.linalg.norm(t,axis=1,keepdims=True))
                    bitangent = np.cross(n,t)
                    dv = (dp[:,1]*duv[:,0,0:1]-dp[:,0]*duv[:,1,0:1])*handed
                    bitangent *= np.sign((bitangent*dv).sum(1,keepdims=True))
                    fine = 2*detail[(coords[:,1]*detail.shape[0]).astype(int),(coords[:,0]*detail.shape[1]).astype(int)]-1
                    n = n*fine[:,2:3]+strength*(t*fine[:,:1]+bitangent*fine[:,1:2])
                    n /= np.maximum(1e-8,np.linalg.norm(n,axis=1,keepdims=True))
                n *= sign
            normal[pixels] = n
            tissue[pixels] = float(name.startswith("Lamina |"))
    def reduce(a):
        return a.reshape(CELL,SS,CELL,SS,-1).mean(axis=(1,3))
    coverage = reduce(np.isfinite(depth))
    color = srgb(reduce(color)/np.maximum(coverage,1e-7))
    # Bilinear sampling also touches uncovered pixels. Extend the nearby pigment
    # without widening the alpha silhouette or painting a dark fringe around each blade.
    occupied = coverage[:,:,0]>0
    for _ in range(5):
        total = np.zeros_like(color)
        weight = np.zeros_like(coverage)
        for axis in (0,1):
            for step in (-1,1):
                total += np.roll(color*occupied[:,:,None],step,axis)
                weight += np.roll(occupied,step,axis)[:,:,None]
        fill = ~occupied & (weight[:,:,0]>0)
        color[fill] = total[fill]/weight[fill]
        occupied |= fill
    # Premultiplying signed directions makes averaging uncovered texels safe in linear mips.
    normal = .5+.5*reduce(normal)
    def rgba(rgb,alpha):
        return Image.fromarray(np.uint8(np.clip(np.concatenate((rgb,alpha),axis=2),0,1)*255+.5))
    return rgba(color,coverage),rgba(normal,reduce(tissue))


def embed(model, img, name):
    encoded = io.BytesIO()
    img.save(encoded,format="PNG")
    doc = model.doc
    doc["images"].append(dict(name=name,mimeType="image/png",bufferView=base.buffer_view(model,encoded.getvalue())))
    doc["textures"].append(dict(source=len(doc["images"])-1))
    return dict(index=len(doc["textures"])-1)


def append_card(model, source):
    parts = primitives(model, source["mesh"])
    positions = np.concatenate([p[0] for p in parts])
    radius = float(np.linalg.norm(positions[:,[0,2]],axis=1).max())+.015
    floor,top = positions[:,1].min()-.01,positions[:,1].max()+.01
    images = [Image.new("RGBA",(CELL*4,CELL*2)) for _ in range(2)]
    for view in range(8):
        maps = projection(parts,view*np.pi/4,radius,floor,top)
        for atlas,cell in zip(images,maps):
            atlas.paste(cell,((view%4)*CELL,(view//4)*CELL))
    name = "Meadow tuft card | "+source["name"]
    color,normal = [embed(model,img,name+suffix) for img,suffix in zip(images,(" albedo"," normals"))]
    doc = model.doc
    doc["materials"].append(dict(name=name,alphaMode="MASK",alphaCutoff=.24,doubleSided=True,
        normalTexture=normal,pbrMetallicRoughness=dict(baseColorTexture=color,metallicFactor=0,roughnessFactor=.60)))
    # Bound each height band to the source in all azimuths to reduce empty cutout pixels.
    levels = np.linspace(floor,top,9)
    widths = np.zeros(len(levels))
    for pos,_,faces,*_ in parts:
        y = pos[faces,1]
        radial = np.linalg.norm(pos[:,[0,2]],axis=1)[faces].max(1)
        for i,(lo,hi) in enumerate(zip(levels,levels[1:])):
            cross = (y.min(1)<=hi)&(y.max(1)>=lo)
            if cross.any(): widths[i:i+2] = np.maximum(widths[i:i+2],radial[cross].max()+.008)
    vertices = [[x,y,0] for w,y in zip(widths,levels) for x in (-w,w)]
    uv = [[.5+x/(2*radius),(top-y)/(top-floor)] for x,y,_ in vertices]
    indices = [j for i in range(0,len(vertices)-2,2) for j in (i,i+1,i+2,i+2,i+1,i+3)]
    attrs = {key:base.accessor(model,np.asarray(data,"<f4")) for key,data in
             (("POSITION",vertices),("NORMAL",[[0,0,1]]*len(uv)),("TEXCOORD_0",uv))}
    prim = dict(attributes=attrs,indices=base.accessor(model,np.asarray(indices,"<u4").reshape(-1,1)),
                material=len(doc["materials"])-1)
    doc["meshes"].append(dict(name=name,primitives=[prim]))
    node = dict(copy.deepcopy(source),name=name,mesh=len(doc["meshes"])-1)
    # The loader traverses roots in reverse order. Opaque near geometry writes depth first.
    doc["scenes"][doc["scene"]]["nodes"].insert(0,len(doc["nodes"]))
    doc["nodes"].append(node)
