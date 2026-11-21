"""Compare static meadow tuft slices and the First Settlers pilot in the same four-oak scene."""
import argparse
import copy
import json
import re
import shutil
from collections import defaultdict
from pathlib import Path

import numpy as np
from PIL import Image
import cards
import tuft_cards as baker

BASE = cards.far.base
OUT = cards.OUT.parent / "meadow-tuft-models"
ASSETS = OUT / "assets"
EXE = OUT / "MeadowTufts.exe"
FIRST = BASE.ROOT.parent / "FirstSettlers/Assets/ArtReview/MeadowGrass"
ARMS = ("reference", "pilot", "slices", "ribbons", "all-cards", "bare")
POSES = (*cards.GROVE.CAMERAS, "foreground")


def primitive(model, positions, normals, uvs, indices, material):
    attrs = {name: BASE.accessor(model, np.asarray(values, "<f4"))
             for name, values in (("POSITION", positions), ("NORMAL", normals), ("TEXCOORD_0", uvs))}
    return dict(attributes=attrs, material=material,
                indices=BASE.accessor(model, np.asarray(indices, "<u4").reshape(-1, 1)))


def add_proxy(model, original, arm, prim):
    doc = model.doc
    name = f"Near tuft | {arm} | {original['name']}"
    doc["meshes"].append(dict(name=name, primitives=prim if isinstance(prim,list) else [prim]))
    roots = doc["scenes"][doc["scene"]]["nodes"]
    # Keep the same draw order relative to the other plants and the distant cards.
    roots.insert(roots.index(doc["nodes"].index(original)), len(doc["nodes"]))
    doc["nodes"].append(dict(copy.deepcopy(original), name=name, mesh=len(doc["meshes"])-1))


def pilot_material(model):
    with Image.open(FIRST / "BladeAtlasNear4K.png") as atlas:
        tex = baker.embed(model, atlas.convert("RGBA"), "First Settlers near atlas")
    materials = model.doc["materials"]
    materials.append(dict(name="Tuft pilot", alphaMode="MASK", alphaCutoff=.24, doubleSided=True,
        pbrMetallicRoughness=dict(baseColorTexture=tex, baseColorFactor=[.75,.79,.59,1],
                                  metallicFactor=0, roughnessFactor=.6),
        extensions={"KHR_materials_diffuse_transmission":dict(diffuseTransmissionFactor=.28,
                  diffuseTransmissionColorTexture=tex)}))
    return len(materials)-1


def pilot(model, variant, target, material):
    raw = (FIRST / "PilotMeadowHybridNear.asset").read_text()
    def stream(field):
        return bytes.fromhex(re.search(r"\b"+field+r": ([a-fA-F0-9]+)", raw)[1])
    v = np.frombuffer(stream("_typelessdata"), "<f4").reshape(52, 26)
    indices = np.frombuffer(stream("m_IndexBuffer"), "<u2").reshape(-1,3)
    if variant == 0:
        p, n, uv = v[:,:3], v[:,3:6], v[:,6:8].copy()
    else:
        i = 8*variant
        p, n = v[:,i:i+3], v[:,i+4:i+7]
        uv = v[:,[i+3,i+7]].copy()
    # Unity and glTF have opposite handedness and texture V origins.
    uv[:,1] = 1-uv[:,1]
    scale = np.ptp(target,axis=0)/np.ptp(p,axis=0)
    p = (p-p.min(0))*scale+target.min(0)
    p[:,2] = target.min(0)[2]+target.max(0)[2]-p[:,2]
    n = n/scale * [1,1,-1]
    n /= np.linalg.norm(n,axis=1,keepdims=True).clip(1e-7)
    # Unity's clockwise front becomes glTF CCW after reflection.
    return primitive(model,p,n,uv,indices,material)


def components(faces):
    """Keep stems and chaff intact even when several elements share a material."""
    parent = list(range(int(faces.max())+1))
    def root(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i
    for a,b,c in faces:
        a = root(a)
        parent[root(b)] = a
        parent[root(c)] = a
    sets = defaultdict(list)
    for face in faces:
        sets[root(face[0])].append(face)
    return [np.array(f) for f in sets.values()]


def slices(model, source, parts, flowering=False):
    # Blades bend predominantly in a radial plane. Assign each whole blade once
    # and keep two depth layers per orientation so the foreground has parallax.
    directions = np.arange(4)*np.pi/4
    fronts = np.stack((np.sin(directions),np.zeros(4),np.cos(directions)),axis=1)
    groups = defaultdict(lambda: defaultdict(list))
    if flowering:
        culm = next(p for p in parts if p[6]=="Culm")
        stems = [culm[0][np.unique(g)] for g in components(culm[2])]
        stems = sorted(stems,key=lambda s:s[:,1].max(),reverse=True)[:4]
        tips = np.stack([s[s[:,1].argmax()] for s in stems])
        assert tips[:,1].min()>.45

    for part_index, part in enumerate(parts):
        pos, _, faces, *_ = part
        for group in components(faces):
            points = pos[np.unique(group)]
            center = points.mean(0)
            if flowering:
                if part[6].startswith("Lamina |") or points[:,1].max()<tips[:,1].min()-.025:
                    continue
                tip = int(np.argmin(np.linalg.norm(tips[:,[0,2]]-center[[0,2]],axis=1)))
                key = (tip,tip)
            elif part[6].startswith("Lamina |"):
                depths = points@fronts.T
                orientation = int(np.argmin(np.ptp(depths,axis=0)))
                layer = int(center@fronts[orientation]>0)
                key = (orientation,layer)
            else:
                # Adjacent spikelets and their tiny branches share a depth plane.
                angle = np.arctan2(center[2],center[0])
                sector = int(np.floor((angle+np.pi)*4/(2*np.pi)))%4
                key = ((2*sector+1)%4,2+sector)
            groups[key][part_index].append(group)
    cell = baker.CELL
    columns, rows = 4, 1 << int(np.ceil(np.log2(np.ceil(len(groups)/4))))
    textures = [Image.new("RGBA",(columns*cell,rows*cell),background)
                for background in ((0,0,0,0),(128,128,128,0))]
    verts, norms, uv, ix = [], [], [], []
    for slot, ((orientation,_), groups_by_part) in enumerate(sorted(groups.items())):
        front = fronts[orientation]
        angle = directions[orientation]
        right = np.array([np.cos(angle),0,-np.sin(angle)])
        part_list = []
        for index, faces in groups_by_part.items():
            p,n,_,t,*rest = parts[index]
            used, mapping = np.unique(np.concatenate(faces),return_inverse=True)
            part_list.append((p[used],n[used],mapping.reshape(-1,3),None if t is None else t[used],*rest))
        cloud = np.concatenate([p[0] for p in part_list])
        x, y, z = cloud@right, cloud[:,1], cloud@front
        mid = (x.max()+x.min())*.5
        radius = (x.max()-x.min())*.5+.004
        floor, top = y.min()-.004, y.max()+.004
        # Fit a sloping support to this portion of the plant. Baking along the
        # same axis preserves the original silhouette from its primary azimuth.
        design = np.column_stack((np.ones_like(y),x-mid,y-floor))
        plane = np.linalg.lstsq(design,z,rcond=None)[0]
        layers = [(p-right*mid,*rest) for p,*rest in part_list]
        color, surface = baker.projection(layers,angle,radius,floor,top)
        col,row = slot%columns,slot//columns
        for atlas,tile in zip(textures,(color,surface)):
            atlas.paste(tile,(cell*col,cell*row))
        alpha = np.asarray(color)[:,:,3]
        levels = np.linspace(0,cell,7,dtype=int)
        low = np.ones(len(levels))*cell
        high = np.zeros(len(levels))
        for j,(a,b) in enumerate(zip(levels,levels[1:])):
            px = np.where(alpha[a:b]>0)[1]
            if len(px):
                low[j:j+2] = np.minimum(low[j:j+2],max(0,px.min()-2))
                high[j:j+2] = np.maximum(high[j:j+2],min(cell,px.max()+3))
        # Bridge occasional empty strips with a degenerate line at the nearest stem.
        empty = low>high
        if empty.any():
            occupied=np.flatnonzero(~empty)
            centers=np.interp(np.flatnonzero(empty),occupied,((low+high)*.5)[occupied])
            low[empty]=high[empty]=centers
        start = len(verts)
        for j,v in enumerate(levels/cell):
            y0 = top-v*(top-floor)
            for u in (low[j]/cell,high[j]/cell):
                lateral = (u-.5)*2*radius
                depth = plane@[1,lateral,y0-floor]
                verts.append(right*(mid+lateral)+[0,y0,0]+front*depth)
                norms.append(front)
                uv.append([(col+u)/columns,(row+v)/rows])
        for i in range(start,len(verts)-2,2):
            ix.extend((i,i+1,i+2,i+2,i+1,i+3))
    name = ("Tuft slices | flowers | " if flowering else "Tuft slices | ")+source["name"]
    color, normal = [baker.embed(model,img,name+suffix) for img,suffix in zip(textures,(" pigment"," surface"))]
    model.doc["materials"].append(dict(name=name,alphaMode="MASK",alphaCutoff=.24,doubleSided=True,
        normalTexture=normal,pbrMetallicRoughness=dict(baseColorTexture=color,roughnessFactor=.6,metallicFactor=0)))
    return primitive(model,verts,norms,uv,ix,len(model.doc["materials"])-1)


def ribbons(model, source, parts):
    # Atlas the five authored pigments into one draw, retaining tissue normals
    # and roughness. Use two edges and the tip of each source blade.
    blades=[p for p in parts if p[6].startswith("Lamina |")]
    colors, details=[Image.new("RGB",(2048,1024)) for _ in range(2)]
    vertices, normals, uvs, indices=[],[],[],[]
    rows = 6 if source["name"]=="Short grass" else 8
    stride = rows*3+1
    for slot,(pos,n,faces,uv,tex,factor,name,detail,strength) in enumerate(blades):
        for atlas,data in ((colors,tex),(details,detail)):
            atlas.paste(Image.fromarray(np.uint8(np.rint(data*255))),(256*slot,0))
        for start in range(0,len(pos),stride):
            # Keep the largest deviations from the straight root-tip chord.
            centers=pos[start+np.minimum(np.arange(rows+1)*3+1,stride-1)]
            cuts=[0,rows]
            while len(cuts)<4:
                error=np.full(rows+1,-1.)
                for a,b in zip(cuts,cuts[1:]):
                    if b-a<2: continue
                    t=np.arange(1,b-a)[:,None]/(b-a)
                    error[a+1:b]=np.linalg.norm(centers[a+1:b]-(centers[a]+t*(centers[b]-centers[a])),axis=1)
                cuts=sorted([*cuts,int(np.argmax(error))])
            ids=[start+r*3+lane for r in cuts[:-1] for lane in (0,2)]+[start+stride-1]
            base=len(vertices)
            vertices.extend(pos[ids]); normals.extend(n[ids])
            cell_uv=uv[ids].copy()
            cell_uv[:,0]=(slot+cell_uv[:,0])/8
            uvs.extend(cell_uv)
            for j in (0,2):
                indices.extend(base+k for k in (j,j+2,j+3,j,j+3,j+1))
            indices.extend(base+k for k in (4,6,5))
    a=baker.embed(model,colors,"Ribbon pigment")
    n=baker.embed(model,details,"Ribbon tissue normals")
    # Use the living-blade growth tint in the meadow shader.
    model.doc["materials"].append(dict(name="Lamina | blade-green",doubleSided=True,
        normalTexture=dict(n,scale=.78),pbrMetallicRoughness=dict(baseColorTexture=a,metallicFactor=0,roughnessFactor=.425),
        extensions={"KHR_materials_diffuse_transmission":dict(diffuseTransmissionFactor=.28,diffuseTransmissionColorTexture=a)}))
    result=[primitive(model,vertices,normals,uvs,indices,len(model.doc["materials"])-1)]
    if source["name"]!="Short grass":
        result.append(slices(model,source,parts,flowering=True))
    return result


def export_tuft(scene, source, parts):
    """Write the uninstanced flowering tuft for individual inspection in the study engine."""
    model = copy.copy(scene)
    model.payload = bytearray()
    model.doc = {key: [] for key in ("accessors", "bufferViews", "images", "materials", "textures")}
    model.doc.update(asset={"version":"2.0"}, extensionsUsed=["KHR_materials_diffuse_transmission"],
                     scene=0, scenes=[{"nodes":[0]}], nodes=[{"name":"Meadow flowering tuft","mesh":0}])
    model.doc["meshes"] = [dict(name="Meadow flowering tuft", primitives=ribbons(model,source,parts))]
    path = OUT/"models"/"MeadowFloweringTuft.glb"
    path.parent.mkdir(parents=True,exist_ok=True)
    cards.far.grass.write(model,path)


def stage():
    ASSETS.mkdir(exist_ok=True,parents=True)
    for path in (cards.STAGE/"assets").iterdir():
        if path.name != BASE.MODEL.name:
            shutil.copy2(path,ASSETS/path.name)
    model = BASE.oak.OakGlb(cards.STAGE/"assets"/BASE.MODEL.name)
    mat = pilot_material(model)
    for variant, source in enumerate(n for n in model.doc["nodes"][:] if n["name"].startswith(("Full grass strip","Short grass"))):
        print("Baking",source["name"],flush=True)
        parts = baker.primitives(model,source["mesh"])
        p = pilot(model,variant,np.concatenate([p[0] for p in parts]),mat)
        s = slices(model,source,parts)
        add_proxy(model,source,"pilot",p)
        add_proxy(model,source,"slices",s)
        r = ribbons(model,source,parts)
        add_proxy(model,source,"ribbons",r)
        if source["name"]=="Full grass strip":
            export_tuft(model,source,parts)
        print("Ribbon triangles",sum(model.array(pr["indices"]).size//3 for pr in r),flush=True)
        print("Triangles",source["name"],*[model.array(a["indices"]).size//3 for a in (p,s)],flush=True)
    cards.far.grass.write(model,ASSETS/BASE.MODEL.name)


def timing_options(mode, pipeline_stats=False):
    if mode != "capture":
        return []
    if pipeline_stats:
        return ["--timing-samples", "1", "--camera-pipeline-stats"]
    return ["--timing-samples", "36"]


def capture(arm, pose, mode="capture", run="single", pipeline_stats=False):
    env = cards.env("cards")
    for key in ("MEADOW_TUFT_SLICES","MEADOW_TUFT_PILOT","MEADOW_TUFT_RIBBONS","MEADOW_CARDS_TO_ROOT","MEADOW_HIDE_ALL_GRASS_CAMERA"):
        env.pop(key,None)
    if arm == "bare":
        env["MEADOW_HIDE_ALL_GRASS_CAMERA"]="1"
    elif arm == "all-cards":
        env["MEADOW_CARDS_TO_ROOT"]="1"
    elif arm != "reference":
        env["MEADOW_TUFT_"+arm.upper()]="1"
    cards.GROVE.CAMERAS["foreground"]=((-26.,-13.),(-30.,-10.),32.)
    options=timing_options(mode,pipeline_stats)
    cards.GROVE.capture(pose,mode,"combined",False,viewer=EXE,environment=env,
        focus=json.loads((cards.STAGE/"scene.json").read_text())["focus"],extra_args=options,
        output_dir=OUT/"review"/run/arm)


if __name__ == "__main__":
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("mode",choices=("prepare","stage","view","capture","measure","counters"))
    ap.add_argument("--arm",choices=ARMS,default="ribbons")
    ap.add_argument("--pose",choices=POSES,default="clearing")
    args=ap.parse_args()
    if args.mode in ("prepare","stage"):
        if not cards.VIEWER.exists(): cards.prepare()
        stage()
        if args.mode=="prepare":
            cards.far.grass.lighting.prepare_viewer(upstream=cards.OUT/"source",out=OUT,
                patch=Path(__file__).with_suffix(".patch"),destination=EXE)
    elif args.mode=="measure":
        cases=[(arm,pose) for pose in POSES for arm in ARMS]
        for run,order in (("forward",cases),("reverse",reversed(cases))):
            for arm,pose in order:
                print(run,arm,pose,flush=True)
                capture(arm,pose,run=run)
    elif args.mode=="counters":
        for arm in ARMS:
            capture(arm,args.pose,run="counters",pipeline_stats=True)
    else:
        capture(args.arm,args.pose,args.mode)
