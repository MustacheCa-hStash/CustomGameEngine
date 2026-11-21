"""Run headless using Blender 5.x. The tissue image lives beside the output blend."""
import math
import random
import json
import struct
from pathlib import Path

import bpy
from mathutils import Vector


ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / "assets/source/meadow-grass-study"
REVIEW = ROOT / "build/meadow-grass-study"
REVIEW.mkdir(exist_ok=True, parents=True)
TAU = math.tau
UP = Vector((0, 0, 1))
TRANSMISSION = 0.28


def new_mesh(name, collection, vertices, faces, indices, mats, uv=None):
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(vertices, [], faces)
    mesh.update()
    for material in mats:
        mesh.materials.append(material)
    for poly, index in zip(mesh.polygons, indices):
        poly.material_index = index
        poly.use_smooth = True
    if uv is not None:
        layer = mesh.uv_layers.new(name="Blade coordinates")
        layer.data.foreach_set("uv", [c for loop in mesh.loops for c in uv[loop.vertex_index]])
    obj = bpy.data.objects.new(name, mesh)
    collection.objects.link(obj)
    return obj


class Parts:
    def __init__(self):
        self.v, self.f, self.m, self.uv = [], [], [], []

    def vertex(self, co, uv=(0, 0)):
        self.v.append(tuple(co))
        self.uv.append(uv)
        return len(self.v) - 1

    def face(self, points, material):
        self.f.append(points)
        self.m.append(material)

    def tube(self, points, radii, material, sides=6):
        rows = []
        for i, (p, r) in enumerate(zip(points, radii)):
            tangent = (points[min(i+1, len(points)-1)] - points[max(i-1, 0)]).normalized()
            axis = tangent.cross(Vector((0, 1, 0))).normalized()
            side = tangent.cross(axis)
            rows.append([self.vertex(p + r*(axis*math.cos(TAU*j/sides) + side*math.sin(TAU*j/sides)))
                         for j in range(sides)])
        for low, high in zip(rows, rows[1:]):
            for j in range(sides):
                k = (j+1) % sides
                self.face((low[j], low[k], high[k], high[j]), material)

    def object(self, name, coll, materials):
        return new_mesh(name, coll, self.v, self.f, self.m, materials, self.uv)


def material(name, rgb, texture=None, translucency=0.0):
    mat = bpy.data.materials.new(name)
    mat.diffuse_color = (*rgb, 1)
    mat.use_nodes = True
    tree = mat.node_tree
    bsdf = tree.nodes.get("Principled BSDF")
    bsdf.inputs["Base Color"].default_value = (*rgb, 1)
    bsdf.inputs["Roughness"].default_value = 0.425 if texture else 0.56
    bsdf.inputs["IOR"].default_value = 1.42
    # Keep glTF's directly connected base color for the portable geometry asset.
    if texture:
        tex = tree.nodes.new("ShaderNodeTexImage")
        tex.image = texture
        tex.extension = "EXTEND"
        tree.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
        tex.location = (-480, 160)
        relief = tree.nodes.new("ShaderNodeTexImage")
        relief.image = RIBS
        relief.location = (-660,-210)
        normal = tree.nodes.new("ShaderNodeNormalMap")
        normal.inputs["Strength"].default_value = 0.78
        normal.location = (-400,-180)
        tree.links.new(relief.outputs["Color"],normal.inputs["Color"])
        tree.links.new(normal.outputs["Normal"],bsdf.inputs["Normal"])
        # In Blender the thin lamina also diffuses a fraction of incident light.
        output = tree.nodes.get("Material Output")
        through = tree.nodes.new("ShaderNodeBsdfTranslucent")
        tree.links.new(tex.outputs["Color"], through.inputs["Color"])
        mix = tree.nodes.new("ShaderNodeMixShader")
        mix.inputs[0].default_value = translucency
        tree.links.new(bsdf.outputs[0], mix.inputs[1])
        tree.links.new(through.outputs[0], mix.inputs[2])
        tree.links.new(mix.outputs[0], output.inputs["Surface"])
        through.location = (0, -480)
        mix.location = (360, 80)
        output.location = (560, 80)
    return mat


def blade(rng, geo, origin, angle, length, width, start, bend, mat):
    """Build a folded, twisting lamina by integrating its tangent along arc length."""
    drift = rng.uniform(-0.48, 0.48)
    roll0, roll_tip = rng.uniform(-0.65, 0.65), rng.uniform(-1.15, 1.15)
    fold = rng.uniform(0.15, 0.34)
    late = rng.uniform(0.15, 0.39)
    kink = rng.uniform(0.50, 0.78)
    u0, u1 = rng.uniform(0.01, 0.07), rng.uniform(0.93, 0.99)
    v0, v1 = rng.uniform(0.0, 0.09), rng.uniform(0.92, 1.0)
    if rng.random() < 0.5:
        u0, u1 = u1, u0
    p = origin.copy()
    old = None
    centers = []
    steps = 30
    for i in range(steps+1):
        t = i/steps
        def frame(s):
            hinge = 0.5 + 0.5*math.tanh((s-kink)*8)
            inc = math.radians(start + bend*((1-late)*s**1.18 + late*hinge))
            az = angle + drift*s**1.3
            radial = Vector((math.cos(az), math.sin(az), 0))
            tangent = radial*math.sin(inc) + UP*math.cos(inc)
            across = Vector((-math.sin(az), math.cos(az), 0))
            return tangent, across, tangent.cross(across)
        if i:
            p += frame(t-0.5/steps)[0]*(length/steps)
        centers.append(p.copy())
        if i == steps:
            tip = geo.vertex(p, ((u0+u1)*0.5, v1))
            for j in range(4):
                geo.face((old[j], tip, old[j+1]), mat)
            break
        tangent, across, normal = frame(t)
        roll = roll0 + roll_tip*t**1.45
        span = across*math.cos(roll) + normal*math.sin(roll)
        up = tangent.cross(span)
        w = width*(0.31+0.69*min(t/0.085, 1))*(1-t**2.55)**0.86
        row = []
        for x in (-1, -0.45, 0, 0.45, 1):
            z = (abs(x)*fold*(1-0.55*t) + 0.035*x*x + abs(x)**3*0.18*max(0,1-t/0.15))*w
            edge = 1+0.025*math.sin(t*18+roll0*4+x)*t
            row.append(geo.vertex(p + span*(x*w*0.5*edge) + up*z,
                                  (u0+(u1-u0)*(x+1)*0.5, v0+(v1-v0)*t)))
        if old:
            for j in range(4):
                geo.face((old[j], row[j], row[j+1], old[j+1]), mat)
        old = row
    return centers


def spikelet(rng, geo, base, axis, size):
    """Alternating overlapping florets with narrow glumes around a common rachilla."""
    axis = axis.normalized()
    side = axis.cross(UP)
    if side.length < 0.001:
        side = Vector((1, 0, 0))
    side.normalize()
    normal = axis.cross(side).normalized()
    count = rng.randint(4, 7)
    for i in range(count):
        sign = (-1)**i
        heel = base + axis*(size*0.54*i/count)
        length = size*(0.51 - 0.11*i/count)
        tip = heel + length*(axis+side*0.23*sign)
        mid = heel.lerp(tip, 0.44)
        w = length*0.20
        pts = [heel, mid+side*w, mid+normal*w*0.85, mid-side*w, mid-normal*w*0.68, tip]
        a,b,c,d,e,f = [geo.vertex(p) for p in pts]
        color = rng.choice((8,8,9,10))
        for face in ((a,c,b),(a,d,c),(a,e,d),(a,b,e),(f,b,c),(f,c,d),(f,d,e),(f,e,b)):
            geo.face(face, color)


def panicle(rng, geo, top, heading, length):
    radial = Vector((math.cos(heading), math.sin(heading), 0))
    def axis(t):
        return top + UP*(length*t) + radial*(length*(0.12*t+0.30*t*t))
    points = [axis(i/7) for i in range(8)]
    geo.tube(points, [0.00065*(1-0.84*i/7) for i in range(8)], 6, 5)
    for tier in range(6):
        t = 0.08 + 0.145*tier
        for k in range(2 if tier < 4 else 1):
            az = heading + tier*2.38 + k*1.4 + rng.uniform(-0.45, 0.45)
            out = Vector((math.cos(az), math.sin(az), 0))
            direction = (out*rng.uniform(0.66, 1.32) + UP*rng.uniform(0.55, 1)).normalized()
            extent = length*(0.52-0.34*t)*rng.uniform(0.72, 1.12)
            root = axis(t)
            end = root + direction*extent
            elbow = root.lerp(end, 0.50) - UP*extent*0.09
            geo.tube([root, elbow, end], [0.00040,0.00029,0.00015], 6, 4)
            spikelet(rng, geo, end, direction, rng.uniform(0.012, 0.019))
            for j in range(2 if tier < 4 else 1):
                branch = elbow.lerp(end, 0.37*j)
                d = (direction + Vector((-out.y,out.x,0))*(0.67 if j else -0.68)).normalized()
                twig = branch + d*extent*rng.uniform(0.19,0.34)
                geo.tube([branch, twig], [0.00024,0.00012], 6, 4)
                spikelet(rng, geo, twig, d, rng.uniform(0.011,0.017))
    spikelet(rng, geo, points[-1], (points[-1]-points[-2]).normalized(), 0.011)


def make_tuft(coll, seed, tillers, flowering, scale):
    rng = random.Random(seed)
    specimens = []
    for i in range(tillers):
        geo = Parts()
        fertile = i < flowering
        theta = i*2.39996+rng.uniform(-0.38, 0.38)
        r = rng.uniform(0.018,0.085) * (0.70 if fertile else 1)
        base = Vector((r*math.cos(theta), r*math.sin(theta)*0.73, 0))
        bearing = theta+rng.uniform(-1.1, 1.1)
        radial = Vector((math.cos(bearing), math.sin(bearing), 0))
        h = scale*(rng.uniform(0.59,0.83) if fertile else rng.uniform(0.060,0.21))
        lean = rng.uniform(0.10,0.35) if fertile else rng.uniform(0.1,0.85)
        def stem(t):
            return base + UP*(t*h) + radial*(h*lean*(0.32*t+0.68*t*t))
        thickness = rng.uniform(0.0017,0.0026) if fertile else rng.uniform(0.0012,0.0020)
        geo.tube([stem(t) for t in (0,0.28,0.58,0.78,1)],
                 [thickness*x for x in (1,0.88,0.72,0.62,0.46)], 5, 7)
        age = rng.random()
        count = rng.randint(4,6)
        for j in range(count):
            a = (j+0.35)/count
            t = 0.025+0.79*a if fertile else 0.04+0.94*a
            collar = stem(t)
            radius = thickness*(1-0.47*t)
            geo.tube([stem(max(0,t-0.115)), stem(t), stem(t+0.006)],
                     [radius*1.14,radius*1.25,radius*1.06], 7 if j==0 else 6, 7)
            az = bearing + (j%2)*math.pi + rng.uniform(-0.54,0.54)
            length = scale*rng.uniform(0.29,0.57)*(1-0.46*a if fertile else 0.75+0.35*a)
            width = rng.uniform(0.0060,0.012)*(0.92 if fertile else 1)
            mat = rng.choice([0,0,0,1,1,2])
            if j == 0 and age < 0.60:
                mat = rng.choice([2,3,4,4])
            elif j == 1 and age < 0.27:
                mat = 3
            start = rng.uniform(8,28) + (1-a)*15
            bend = rng.uniform(42,100) + (1-a)*29
            if mat == 4:
                start += 17
                bend += 25
                length *= 0.70
            if fertile and j == count-1:
                length *= 0.67
            first_v, first_f = len(geo.v), len(geo.f)
            centers = blade(rng, geo, collar, az, length, width, start, bend, mat)
            if not fertile and mat in (0,1) and width > 0.007:
                specimens.append((centers[19], centers, geo.v[first_v:], geo.uv[first_v:],
                                  [tuple(k-first_v for k in f) for f in geo.f[first_f:]],mat))
        if fertile:
            panicle(rng,geo,stem(1),bearing,scale*rng.uniform(0.13,0.20))
        geo.object(f"{'Flowering culm' if fertile else 'Leafy tiller'} {i+1:02}",coll,MATERIALS)
    return min(specimens, key=lambda item: item[0].y)


def camera(name, target, offset, width):
    data = bpy.data.cameras.new(name)
    obj = bpy.data.objects.new(name, data)
    STAGE.objects.link(obj)
    target, offset = Vector(target), Vector(offset)
    obj.location = target + offset
    obj.rotation_euler = (-offset).to_track_quat("-Z","Y").to_euler()
    data.type = "ORTHO"
    data.ortho_scale = width
    data.clip_start = 0.006
    return obj


def floor_material():
    mat = material("Stage | warm grey", (0.20,0.19,0.155))
    return mat


def export_transmission(path):
    # Same thin-surface extension the oak export and engine loader use.
    glb = path.read_bytes()
    size = struct.unpack_from("<I",glb,12)[0]
    doc = json.loads(glb[20:20+size])
    key = "KHR_materials_diffuse_transmission"
    doc.setdefault("extensionsUsed",[]).append(key)
    for mat in doc["materials"]:
        if mat["name"].startswith("Lamina |"):
            mat.setdefault("extensions",{})[key] = {"diffuseTransmissionFactor": TRANSMISSION}
    encoded = json.dumps(doc,separators=(",",":")).encode()
    encoded += b" "*(-len(encoded)%4)
    tail = glb[20+size:]
    path.write_bytes(struct.pack("<4sIII4s", b"glTF",2,20+len(encoded)+len(tail),
                                 len(encoded),b"JSON")+encoded+tail)


def render(render_scene, camera_obj, filename, size):
    render_scene.camera = camera_obj
    render_scene.render.resolution_x,render_scene.render.resolution_y = size
    render_scene.render.filepath = str(REVIEW / filename)
    bpy.ops.render.render(write_still=True,scene=render_scene.name)


bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.name = "Tall meadow grass"
scene.unit_settings.system = "METRIC"
scene.render.engine = "CYCLES"
scene.cycles.samples = 72
scene.cycles.use_denoising = True
try:
    cp = bpy.context.preferences.addons["cycles"].preferences
    cp.compute_device_type = "OPTIX"
    cp.refresh_devices()
    if any(d.type=="OPTIX" for d in cp.devices):
        for device in cp.devices:
            device.use = device.type == "OPTIX"
        scene.cycles.device = "GPU"
except (RuntimeError, TypeError):
    pass
scene.view_settings.view_transform = "AgX"
scene.view_settings.exposure = 0.72
scene.render.resolution_percentage = 100
scene.world = bpy.data.worlds.new("Open sky")
scene.world.use_nodes = True
world = scene.world.node_tree.nodes.get("Background")
world.inputs["Color"].default_value = (0.77,0.85,1,1)
world.inputs["Strength"].default_value = 0.64

textures = []
for label in ("green","cool","olive","fading","straw"):
    image = bpy.data.images.load(str(ASSETS / f"blade-{label}.png"))
    image.pack()
    textures.append(image)
RIBS = bpy.data.images.load(str(ASSETS / "blade-ribs.png"))
RIBS.colorspace_settings.name = "Non-Color"
RIBS.pack()
MATERIALS = [material("Lamina | "+im.name.split(".")[0],(0.18,0.29,0.06),im,TRANSMISSION) for im in textures]
for label, color in (("Culm",(0.215,0.313,0.072)),("Green sheath",(0.253,0.338,0.099)),
                     ("Old sheath",(0.305,0.215,0.082)),("Glume | olive",(0.264,0.287,0.081)),
                     ("Glume | chaff",(0.376,0.32,0.132)),("Glume | flushed",(0.213,0.128,0.067))):
    MATERIALS.append(material(label,color))

hero = bpy.data.collections.new("01 | Meadow tuft")
scene.collection.children.link(hero)
blade_info = make_tuft(hero,1343,30,4,1.0)
types = [hero]
for label, seed, n, fertile, size in (("Leafy",731,24,1,0.88),("Open",991,21,3,1.05)):
    coll = bpy.data.collections.new("Patch variant | "+label)
    make_tuft(coll,seed,n,fertile,size)
    types.append(coll)
STAGE = bpy.data.collections.new("02 | Preview stage")
scene.collection.children.link(STAGE)
patch = bpy.data.collections.new("03 | Meadow patch instances")
rng = random.Random(2333)
for j in range(-8,24):
    for i in range(-11,12):
        x = 0.295*(i+rng.uniform(-0.45,0.45))
        y = 0.28*(j+rng.uniform(-0.45,0.45))
        if x*x+y*y < 0.07 or rng.random() < 0.048:
            continue
        obj = bpy.data.objects.new("Tuft",None)
        patch.objects.link(obj)
        obj.instance_type = "COLLECTION"
        obj.instance_collection = rng.choices(types,[2,6,3])[0]
        obj.location = (x,y,-0.008)
        obj.rotation_euler.z = rng.uniform(0,TAU)
        s = rng.uniform(0.72,1.15)*(0.94+0.10*math.sin(x*2+y*0.8))
        obj.scale = (s,s,s*rng.uniform(0.88,1.1))
        if rng.random() < 0.73:
            young = obj.copy()
            young.name = "Intervening young shoot group"
            patch.objects.link(young)
            young.instance_collection = types[1]
            young.location.x += rng.uniform(0.105,0.17)
            young.location.y += rng.uniform(0.085,0.15)
            young.rotation_euler.z += rng.uniform(1,5)
            young.scale *= rng.uniform(0.38,0.68)

ground = new_mesh("Studio ground",STAGE,[(-100,-100,0.001),(100,-100,0.001),
                  (100,100,0.001),(-100,100,0.001)],[(0,1,2,3)],[0],[floor_material()])
sun_data = bpy.data.lights.new("Afternoon sun","SUN")
sun_data.energy = 3.5
sun_data.color = (1,0.928,0.782)
sun_data.angle = math.radians(1.5)
sun = bpy.data.objects.new("Afternoon sun",sun_data)
STAGE.objects.link(sun)
sun.rotation_euler = Vector((0.52,-0.82,-0.85)).to_track_quat("-Z","Y").to_euler()

zmax = max(v.co.z for obj in hero.objects for v in obj.data.vertices)
portrait = camera("A | Whole tuft", (0,0,0.46*zmax),(0.46,-2.6,0.65),1.24*zmax)
center, centers, verts, uvs, faces, mi = blade_info
detail = camera("B | Living blades", center,(0.046,-0.27,0.12),0.126)
detail.data.type = "PERSP"
detail.data.lens = 81
detail.data.dof.use_dof = True
detail.data.dof.focus_distance = (detail.location-center).length
detail.data.dof.aperture_fstop = 96
field = camera("C | Meadow patch",(0,0.35,0.45),(0.38,-2.15,0.75),2.15)
field.data.type = "PERSP"
field.data.lens = 48

field_scene = scene.copy()
field_scene.name = "Meadow patch"
field_scene.use_fake_user = True
stage_copy = field_scene.collection.children[STAGE.name]
field_scene.collection.children.unlink(stage_copy)
field_scene.collection.children.link(patch)
for obj in (sun,field):
    field_scene.collection.objects.link(obj)
earth = ground.copy()
earth.name = "Soil under meadow"
earth.data = ground.data.copy()
earth.data.materials[0] = material("Earth beneath meadow",(0.065,0.043,0.022))
field_scene.collection.objects.link(earth)
field_scene.camera = field
field_scene.render.resolution_x,field_scene.render.resolution_y = (1600,1030)

# Keep an editable, isolated leaf at native scale in its own Blender scene.
spec = scene.copy()
spec.name = "Blade specimen"
spec.use_fake_user = True
for coll in list(spec.collection.children):
    spec.collection.children.unlink(coll)
leaf = new_mesh("Blade | folded lamina",spec.collection,[Vector(p)-centers[0] for p in verts],
                faces,[0]*len(faces),[MATERIALS[mi]],uvs)
leaf.asset_mark()
hero.asset_mark()
spec.collection.objects.link(sun)
spec.collection.objects.link(ground)
row = 12
segment = centers[row]-centers[0]
axis = (centers[row+1]-centers[row-1]).normalized()
across = (Vector(verts[5*row+4])-Vector(verts[5*row])).normalized()
face = axis.cross(across).normalized()
if face.dot(Vector((-0.52,0.82,0.85)))<0:
    face.negate()
macro = camera("D | Blade surface",segment,face*0.083+axis*0.045,0.037)
spec.collection.objects.link(macro)
STAGE.objects.unlink(macro)
# Set roll from the local leaf axis so its ribs cross the image diagonally.
local_axis = macro.rotation_euler.to_quaternion().inverted() @ axis
macro.rotation_euler.rotate_axis("Z",math.atan2(local_axis.y,local_axis.x)-0.37)
spec.camera = macro
spec.render.resolution_x,spec.render.resolution_y = (1350,780)

# Export just the rooted tuft. Connect the shared opaque base-color shader for glTF.
for m in MATERIALS[:5]:
    nodes = m.node_tree.nodes
    m.node_tree.links.new(nodes.get("Principled BSDF").outputs[0],nodes.get("Material Output").inputs["Surface"])
bpy.ops.object.select_all(action="DESELECT")
for obj in hero.objects:
    obj.select_set(True)
bpy.context.view_layer.objects.active = hero.objects[0]
bpy.ops.export_scene.gltf(filepath=str(ASSETS / "meadow-tuft.glb"),use_selection=True,
                          export_yup=True,export_animations=False,export_cameras=False,export_lights=False)
export_transmission(ASSETS / "meadow-tuft.glb")
for m in MATERIALS[:5]:
    nodes = m.node_tree.nodes
    m.node_tree.links.new(nodes.get("Mix Shader").outputs[0],nodes.get("Material Output").inputs["Surface"])
bpy.ops.object.select_all(action="DESELECT")
scene.camera = portrait
for area in bpy.context.screen.areas:
    if area.type == "VIEW_3D":
        area.spaces.active.region_3d.view_perspective = "CAMERA"
        area.spaces.active.overlay.show_overlays = False
scene.render.resolution_x = scene.render.resolution_y = 1350
bpy.context.preferences.filepaths.save_version = 0
bpy.ops.wm.save_as_mainfile(filepath=str(ASSETS / "meadow-grass.blend"),compress=True)
render(scene,portrait,"tuft.png",(1350,1350))
render(scene,detail,"blade-detail.png",(1500,1050))
render(field_scene,field,"meadow-patch.png",(1600,1030))
render(spec,macro,"blade-surface.png",(1350,780))
print(f"Hero shoots {len(hero.objects)}, height {zmax:.3f} m, patch neighbours {len(patch.objects)}")
