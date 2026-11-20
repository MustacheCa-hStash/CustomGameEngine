"""Compare foreground depth using the renderer's camera and the UE wood-sidedness rule."""
import numpy as np
from CompareOakJoint import mixed_raster
from PackageOakAssembly import CHOICES
from ReplayOakAssembly import OUT, write

def run():
    rows = []
    for d in (41,100):
        for yaw in (132,90,180):
            suffix = "" if yaw == 132 else f"-yaw{yaw}"
            source = mixed_raster(OUT/"wood.mesh",d,yaw,True)
            fine = mixed_raster(OUT/f"nanite{d}{suffix}-fine.mesh",d,yaw,True)
            seed = "seed-custom01-epic/level01.mesh" if d==41 else "seed-epic03-custom/level01.mesh"
            flat = "epic-0.5/level02.mesh" if d==41 else CHOICES[d]
            for label, path in (("expanded",flat),("seeded",seed),("Nanite",f"nanite{d}{suffix}.mesh")):
                z = mixed_raster(OUT/path,d,yaw,True)
                for reference,depth in (("shared",source),("native-finest",fine)):
                    both = np.isfinite(z)&np.isfinite(depth)
                    error = abs(z[both]-depth[both])*1000
                    rows.append(dict(d=d,yaw=yaw,cut=label,reference=reference,shared=int(both.sum()),
                                     p50_mm=np.median(error),p95_mm=np.percentile(error,95),
                                     over_100mm=np.mean(error>100)))
    write("foreground-depth.csv",rows)

if __name__=="__main__":run()
