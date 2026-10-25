#!/usr/bin/env python3
"""
Parametric enclosure generator for the nursery air monitor.
Outputs base.stl and lid.stl (binary STL, millimetres) plus preview.png.
Needs only numpy (and matplotlib for the preview).  Edit the parameters, re-run.

Design: one box, two compartments side by side along X.
  [ESP32 compartment | divider w/ cable slot | vented sensor compartment]
Boards lie flat on printed ledges, no screws.  The lid slides into grooves in the
side walls from the sensor end and clicks into a small detent.  Two mounting ears
at the ends take a screw each if you want to hang it on a wall.
"""
import struct, sys, os
import numpy as np

# ------------------------------ parameters (mm) ------------------------------
WALL, FLOOR = 2.4, 2.0
BOARD_T     = 1.6
STANDOFF    = 3.0            # ledge height under the boards

ESP_L, ESP_W = 58.4, 22.9    # SparkFun Thing Plus C (2.3" x 0.9")
SEN_L, SEN_W = 25.4, 25.4    # BME680 Qwiic breakout -- VERIFY with calipers
ESP_SLACK_L  = 1.6           # extra length in the ESP32 compartment
ESP_FIT      = 0.5           # extra width around the ESP32 board
SEN_SLACK    = 2.6           # extra length in the sensor compartment

IW      = max(SEN_W + 0.6, 26.0)   # interior width (Y), shared by both compartments
ZL      = 14.0               # underside of lid above bed (cavity height)
LID_T   = 2.0
CL      = 0.25               # sliding clearance of lid in its grooves
GD      = 1.2                # groove depth into the side walls
ROOF    = 1.5                # wall material above the groove
DIV     = 2.0                # divider thickness
EAR_L, EAR_T, EAR_W, EAR_HOLE = 10.0, 2.5, 16.0, 4.5

VENT_N, VENT_W, VENT_P = 6, 1.8, 3.6   # vent slots: count, width, pitch
BUMP_H = 0.35                           # detent bump height on the lid
POCKET = 0.6                            # detent pocket depth in the groove floor

# ------------------------------ derived geometry ------------------------------
ESP_IL = ESP_L + ESP_SLACK_L
SEN_IL = SEN_L + SEN_SLACK
Y      = IW + 2 * WALL
YC     = Y / 2
x_e0   = WALL
x_e1   = x_e0 + ESP_IL
x_s0   = x_e1 + DIV
x_s1   = x_s0 + SEN_IL
X      = x_s1 + WALL
TOP    = ZL + LID_T + CL + ROOF
G      = (IW - (ESP_W + ESP_FIT)) / 2           # wall thickening beside the ESP32 board
GUIDE_TOP = FLOOR + STANDOFF + BOARD_T + 3.0
x_f    = WALL + 0.15                            # lid front edge when closed
xb     = x_f + 9.0                              # detent centre
vx0    = x_s0 + (SEN_IL - ((VENT_N - 1) * VENT_P + VENT_W)) / 2


class Solid:
    """Union/difference of axis-aligned boxes, meshed on a coordinate-compressed grid.
    The result is watertight by construction."""
    def __init__(self):
        self.ops = []

    def add(self, x0, x1, y0, y1, z0, z1):
        self.ops.append((True, (x0, x1, y0, y1, z0, z1)))

    def cut(self, x0, x1, y0, y1, z0, z1):
        self.ops.append((False, (x0, x1, y0, y1, z0, z1)))

    def mesh(self):
        r = lambda v: round(float(v), 4)
        axes = []
        for a in range(3):
            axes.append(sorted({r(b[2 * a + k]) for _, b in self.ops for k in (0, 1)}))
        idx = [{v: i for i, v in enumerate(ax)} for ax in axes]
        occ = np.zeros(tuple(len(ax) - 1 for ax in axes), bool)
        for fill, b in self.ops:
            s = tuple(slice(idx[a][r(b[2 * a])], idx[a][r(b[2 * a + 1])]) for a in range(3))
            occ[s] = fill
        P = np.pad(occ, 1)
        quads = []  # each: 4 lattice vertices (i,j,k), CCW seen from outside
        for axis in range(3):
            u, v = (axis + 1) % 3, (axis + 2) % 3
            for sign in (+1, -1):
                sl = [slice(1, -1)] * 3
                nb = list(sl)
                nb[axis] = slice(2, None) if sign > 0 else slice(None, -2)
                mask = P[tuple(sl)] & ~P[tuple(nb)]
                cells = np.argwhere(mask)
                if not len(cells):
                    continue
                n = cells.shape[0]
                base = cells.copy()
                if sign > 0:
                    base[:, axis] += 1
                corners = []
                for du, dv in ((0, 0), (1, 0), (1, 1), (0, 1)):
                    c = base.copy()
                    c[:, u] += du
                    c[:, v] += dv
                    corners.append(c)
                if sign < 0:
                    corners = corners[::-1]
                quads.append(np.stack(corners, axis=1))  # (n,4,3)
        q = np.concatenate(quads, axis=0)
        coords = [np.array(ax) for ax in axes]
        verts = np.stack([coords[a][q[:, :, a]] for a in range(3)], axis=2)  # (n,4,3) mm
        return q, verts, [len(ax) for ax in axes]


def triangles(verts):
    a, b, c, d = verts[:, 0], verts[:, 1], verts[:, 2], verts[:, 3]
    return np.concatenate([np.stack([a, b, c], 1), np.stack([a, c, d], 1)], axis=0)


def check(q, dims):
    """Every lattice edge must border exactly two faces; volume must be positive."""
    nx, ny, nz = dims
    vid = (q[:, :, 0] * ny + q[:, :, 1]) * nz + q[:, :, 2]
    e = np.concatenate([np.stack([vid[:, i], vid[:, (i + 1) % 4]], 1) for i in range(4)])
    e = np.sort(e, axis=1)
    key = e[:, 0].astype(np.int64) * (nx * ny * nz) + e[:, 1]
    _, cnt = np.unique(key, return_counts=True)
    return bool(np.all(cnt == 2))


def write_stl(path, tris, shift):
    tris = tris - shift
    n = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
    ln = np.linalg.norm(n, axis=1, keepdims=True)
    n = np.divide(n, ln, out=np.zeros_like(n), where=ln > 0)
    vol = np.einsum('ij,ij->i', tris[:, 0], np.cross(tris[:, 1], tris[:, 2])).sum() / 6.0
    rec = np.zeros(len(tris), dtype=[('n', '<f4', 3), ('v', '<f4', (3, 3)), ('a', '<u2')])
    rec['n'], rec['v'] = n, tris
    with open(path, 'wb') as f:
        f.write(b'nursery air monitor'.ljust(80, b' '))
        f.write(struct.pack('<I', len(tris)))
        f.write(rec.tobytes())
    return vol


# ------------------------------ base ------------------------------
base = Solid()
base.add(0, X, 0, Y, 0, TOP)                                   # outer block
base.cut(WALL, X - WALL, WALL, Y - WALL, FLOOR, TOP + 1)       # cavity

# divider with cable slot
base.add(x_e1, x_s0, WALL, Y - WALL, FLOOR, ZL - 0.3)
base.cut(x_e1, x_s0, YC - 5, YC + 5, FLOOR, 9.0)

# ESP32 side: thicken walls to locate the board, plus ledges under its long edges
for y0, y1, l0, l1 in ((WALL, WALL + G, WALL + G, WALL + G + 1.5),
                       (Y - WALL - G, Y - WALL, Y - WALL - G - 1.5, Y - WALL - G)):
    base.add(x_e0, x_e1, y0, y1, FLOOR, GUIDE_TOP)
    base.add(x_e0, x_e1, l0, l1, FLOOR, FLOOR + STANDOFF)

# sensor side: ledges only
base.add(x_s0, x_s1, WALL, WALL + 1.5, FLOOR, FLOOR + STANDOFF)
base.add(x_s0, x_s1, Y - WALL - 1.5, Y - WALL, FLOOR, FLOOR + STANDOFF)

# lid grooves (open at the sensor end), end-wall cut-out, detent pockets
zg0, zg1 = ZL - CL, ZL + LID_T + CL
base.cut(WALL, X + 1, WALL - GD, WALL, zg0, zg1)
base.cut(WALL, X + 1, Y - WALL, Y - WALL + GD, zg0, zg1)
base.cut(X - WALL, X + 1, WALL, Y - WALL, zg0, TOP + 1)
for ya, yb in ((WALL - 1.0, WALL), (Y - WALL, Y - WALL + 1.0)):
    base.cut(xb - 2.8, xb + 2.8, ya, yb, zg0 - POCKET, zg0)

# side vents beside the sensor
for i in range(VENT_N):
    x0 = vx0 + i * VENT_P
    base.cut(x0, x0 + VENT_W, -1, WALL + 0.01, FLOOR + STANDOFF + 0.2, zg0 - 0.6)
    base.cut(x0, x0 + VENT_W, Y - WALL - 0.01, Y + 1, FLOOR + STANDOFF + 0.2, zg0 - 0.6)

# USB-C opening at the ESP32 end
base.cut(-1, WALL, YC - 7, YC + 7, FLOOR + STANDOFF - 0.2, 12.5)

# mounting ears with screw holes
for xa, xb_, hc in ((-EAR_L, 0, -EAR_L / 2), (X, X + EAR_L, X + EAR_L / 2)):
    base.add(xa, xb_, YC - EAR_W / 2, YC + EAR_W / 2, 0, EAR_T)
    base.cut(hc - EAR_HOLE / 2, hc + EAR_HOLE / 2, YC - EAR_HOLE / 2, YC + EAR_HOLE / 2, -1, EAR_T + 1)

# ------------------------------ lid (printed face-down) ------------------------------
lid = Solid()
yl0, yl1 = WALL - (GD - CL), Y - WALL + (GD - CL)
lid.add(x_f, X, yl0, yl1, 0, LID_T)
for i in range(VENT_N):
    x0 = vx0 + i * VENT_P
    lid.cut(x0, x0 + VENT_W, YC - 7, YC + 7, -1, LID_T + 1)
for ya, yb in ((1.5, 2.3), (Y - 2.3, Y - 1.5)):
    lid.add(xb - 2.0, xb + 2.0, ya, yb, LID_T, LID_T + BUMP_H)

# ------------------------------ output ------------------------------
out = os.path.dirname(os.path.abspath(__file__))
report = {}
meshes = {}
for name, solid in (('base', base), ('lid', lid)):
    q, verts, dims = solid.mesh()
    tris = triangles(verts)
    shift = tris.reshape(-1, 3).min(axis=0)
    vol = write_stl(os.path.join(out, name + '.stl'), tris, shift)
    size = (tris.reshape(-1, 3).max(axis=0) - shift)
    report[name] = (len(tris), check(q, dims), vol / 1000.0, size)
    meshes[name] = (verts - shift)
for k, (nt, ok, vol, size) in report.items():
    print(f"{k}.stl: {nt} triangles, manifold={ok}, volume={vol:.1f} cm3, "
          f"size={size[0]:.1f} x {size[1]:.1f} x {size[2]:.1f} mm")

try:
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from mpl_toolkits.mplot3d.art3d import Poly3DCollection
    fig = plt.figure(figsize=(12, 6.5))
    light = np.array([0.4, -0.6, 0.7]); light /= np.linalg.norm(light)
    for i, name in enumerate(('base', 'lid')):
        v = meshes[name]
        n = np.cross(v[:, 1] - v[:, 0], v[:, 2] - v[:, 0])
        n /= np.linalg.norm(n, axis=1, keepdims=True)
        shade = 0.35 + 0.65 * np.clip(n @ light, 0, 1)
        cols = np.stack([shade * 0.55, shade * 0.7, shade * 0.95, np.ones_like(shade)], 1)
        ax = fig.add_subplot(1, 2, i + 1, projection='3d')
        ax.add_collection3d(Poly3DCollection(v, facecolors=cols, edgecolors='none'))
        mx = v.reshape(-1, 3).max(axis=0)
        h = mx.max() / 2.0 * 0.62
        ax.set_xlim(mx[0] / 2 - h, mx[0] / 2 + h); ax.set_ylim(mx[1] / 2 - h, mx[1] / 2 + h)
        ax.set_zlim(mx[2] / 2 - h, mx[2] / 2 + h); ax.set_box_aspect((1, 1, 1))
        ax.view_init(elev=38, azim=-62); ax.set_axis_off(); ax.set_title(name)
    fig.tight_layout()
    fig.savefig(os.path.join(out, 'preview.png'), dpi=110)
except Exception as exc:
    print("preview skipped:", exc)
