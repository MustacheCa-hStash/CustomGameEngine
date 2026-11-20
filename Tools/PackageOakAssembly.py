"""Package the one-bough, shared-input experiment beside its existing grid control."""
import math
import struct
import numpy as np
from ReplayOakAssembly import OUT, PREVIOUS, TREE, Hierarchy, mesh, viewer_assets

CHOICES = {41: "seed-custom01-epic/level01.mesh", 100: "epic-0.5/level04-paired.mesh"}
BUDGET = {41: 1.02211, 100: .49018}


def first_use(part):
    vertices = np.frombuffer(part[2], "V40")
    indices = np.frombuffer(part[3], "<u4")
    used, first = np.unique(indices, return_index=True)
    order = used[np.argsort(first)]
    remap = np.empty(len(vertices), dtype="<u4")
    remap[order] = np.arange(len(order))
    part[2], part[3] = vertices[order].tobytes(), remap[indices].tobytes()


def pixel_scale(node, distance, yaw):
    from ScoreOakAssembly import FOCUS
    center, extent = np.array(struct.unpack_from("<6f", node)).reshape(2, 3)
    y, p = np.radians([yaw, -5.46 if distance == 41 else -2.235])
    x = np.array([np.cos(y), 0., -np.sin(y)])
    z = np.array([np.sin(y)*np.cos(p), np.sin(p), np.cos(y)*np.cos(p)])
    axes = np.array([x, np.cross(z, x), z])
    center = axes @ (center-FOCUS)
    extent = abs(axes) @ extent
    near = distance-center[2]-extent[2]
    radial = np.linalg.norm(abs(center[:2])+extent[:2])
    return (1080/math.tan(math.pi/8))/near*math.hypot(1, radial/near)


def package():
    dest = viewer_assets("assembly-pilot", PREVIOUS/"grid")
    h = Hierarchy((PREVIOUS/"grid/assets"/TREE).read_bytes())
    part = h.parts[1]
    nodes = [part[1][k:k+56] for k in range(0,len(part[1]),56)]
    for node in nodes:
        first, count = struct.unpack_from("<2I", node, 40)
        if count and first >= 3:
            struct.pack_into("<I", node, 40, first+2)
    alternatives = []
    for index, d in enumerate((100,41)):
        node = nodes[2].copy()
        v, i = mesh(OUT/CHOICES[d])
        center, extent = np.array(struct.unpack_from("<6f", node)).reshape(2,3)
        lo = np.minimum(center-extent, v[:,:3].min(0))
        hi = np.maximum(center+extent, v[:,:3].max(0))
        struct.pack_into("<6f", node, 0, *((lo+hi)/2), *((hi-lo)/2))
        # These two thresholds are empirical silhouette admission values for the
        # calibrated cameras. They are stored only in the shared-input pilot.
        error = .997 * min(BUDGET[d]/pixel_scale(node,d,yaw) for yaw in (132,90,180))
        struct.pack_into("<IIffII", node, 24, len(part[3])//4, i.size, error, error, 3+index, 1)
        part[3] += (i+len(part[2])//40).astype("<u4").tobytes()
        part[2] += v.tobytes()
        alternatives.append(node)
    # Include outlying simplified vertices in the region and ancestor frustum tests.
    for parent, child in ((alternatives[0],alternatives[1]),(nodes[0],alternatives[0])):
        c, e = np.array(struct.unpack_from("<6f",parent)).reshape(2,3)
        cc, ce = np.array(struct.unpack_from("<6f",child)).reshape(2,3)
        if np.any(cc-ce<c-e) or np.any(cc+ce>c+e):
            lo,hi=np.minimum(c-e,cc-ce),np.maximum(c+e,cc+ce)
            struct.pack_into("<6f",parent,0,*((hi+lo)/2),*((hi-lo)/2))
    part[1] = b"".join(nodes[:2]+alternatives+nodes[2:])
    first_use(part)
    h.write(dest/"assets"/TREE)
    return dest


if __name__ == "__main__":
    print(package()/"CustomGameEngine.exe")
