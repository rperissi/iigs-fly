#!/usr/bin/env python3
"""
pack_fly.py - fetch MaleCNS v1.0 neuron skeletons and pack them for the IIgs.

Setup:
  pip install neuprint-python navis numpy
  export NEUPRINT_TOKEN=...   (neuprint.janelia.org -> account icon -> token)

Usage:
  python3 pack_fly.py                # default type list
  python3 pack_fly.py GF DNa01 MBON01

Output: FLYDATA.BIN (little-endian)

File format:
  magic      4 bytes  'FLY1'
  count      u16      number of neurons
  per neuron:
    bodyId   u32
    type     16 bytes ASCII, null padded
    nNodes   u16
    pre      u32      presynaptic site count
    post     u32      postsynaptic site count
    nodes    nNodes * (int16 x, int16 y, int16 z)   centered, scaled to +/-30000
    parents  nNodes * u16                            index of parent, 0xFFFF for root

GS side: rotate about Y with an 8.8 sin/cos table, orthographic project,
x>>8 and y>>8 to fit 320x200, color = rotated z mapped to 16 palette entries,
draw a line from each node to its parent.
"""
import heapq
import os
import struct
import sys

import numpy as np
import navis
import navis.interfaces.neuprint as neu
from neuprint import Client, NeuronCriteria as NC, fetch_neurons

MAX_NODES = 600          # keep per-neuron line count sane for 2.8 MHz
PER_TYPE = 2             # neurons to grab per cell type
OUT = "FLYDATA.BIN"

DEFAULT_TYPES = [
    "GF",        # giant fiber, the escape neuron, big and iconic
    "DNa01", "DNa02",   # descending neurons, brain to nerve cord
    "MBON01", "MBON03", # mushroom body output neurons
    "LC10", "LC4",      # visual projection neurons
    "PPL101",           # dopaminergic
    "aMe12",            # clock related
]

# MaleCNS v1.0 renamed a couple of the spec's cell types. Query the live
# name, write the spec name into the BIN so the GS tag table still matches.
QUERY_TYPE = {
    "GF": "DNp01",     # giant fiber
    "LC10": "LC10a",   # courtship visual projection
}


def cap_tree(nodes, max_nodes=MAX_NODES):
    """Keep at most max_nodes, dropping the shortest twigs first.

    navis.downsample_neuron preserves every branch point, so a MaleCNS
    descending neuron bottoms out around 2000-2600 nodes. The GS loader
    rejects anything over 600.
    """
    ids = [int(x) for x in nodes.node_id]
    pars_raw = [int(x) for x in nodes.parent_id]
    xyz = nodes[["x", "y", "z"]].to_numpy(np.float64)
    n = len(ids)
    id2i = {i: k for k, i in enumerate(ids)}
    parent = np.full(n, -1, dtype=np.int32)
    children = [[] for _ in range(n)]
    for k, p in enumerate(pars_raw):
        if p in id2i and id2i[p] != k:
            parent[k] = id2i[p]
            children[id2i[p]].append(k)
    nchild = np.array([len(children[k]) for k in range(n)], dtype=np.int32)
    alive = np.ones(n, dtype=bool)

    def seglen2(k):
        p = parent[k]
        if p < 0:
            return 1e18
        d = xyz[k] - xyz[p]
        return float(d[0] * d[0] + d[1] * d[1] + d[2] * d[2])

    heap = []
    for k in range(n):
        if nchild[k] == 0 and parent[k] >= 0:
            heapq.heappush(heap, (seglen2(k), k))

    nalive = n
    while nalive > max_nodes and heap:
        _, k = heapq.heappop(heap)
        if not alive[k] or nchild[k] != 0:
            continue
        alive[k] = False
        nalive -= 1
        p = int(parent[k])
        if p >= 0 and alive[p]:
            nchild[p] -= 1
            if nchild[p] == 0:
                heapq.heappush(heap, (seglen2(p), p))

    keep = np.nonzero(alive)[0]
    remap = {int(old): i for i, old in enumerate(keep)}
    xyz_out = xyz[keep].copy()
    xyz_out -= xyz_out.mean(axis=0)
    scale = 30000.0 / max(float(np.abs(xyz_out).max()), 1.0)
    xyz_out = np.round(xyz_out * scale).astype(np.int16)
    par_out = np.empty(len(keep), dtype=np.uint16)
    for i, old in enumerate(keep):
        p = int(parent[old])
        while p >= 0 and p not in remap:
            p = int(parent[p])
        par_out[i] = remap[p] if p >= 0 else 0xFFFF
    return xyz_out, par_out

def main():
    token = os.environ.get("NEUPRINT_TOKEN")
    if not token:
        sys.exit("set NEUPRINT_TOKEN first")
    spec_types = sys.argv[1:] or DEFAULT_TYPES
    query_types = [QUERY_TYPE.get(t, t) for t in spec_types]
    spec_of_query = {}
    for spec, q in zip(spec_types, query_types):
        spec_of_query.setdefault(q, spec)

    c = Client("https://neuprint.janelia.org", dataset="male-cns:v1.0", token=token)

    meta, _ = fetch_neurons(NC(type=query_types), client=c)
    meta = meta.groupby("type").head(PER_TYPE)
    print(f"fetching {len(meta)} neurons")

    skels = neu.fetch_skeletons(meta.bodyId.tolist(), client=c, with_synapses=False)

    order = {t: i for i, t in enumerate(spec_types)}
    records = []
    for n in skels:
        if n.n_nodes > MAX_NODES:
            n = navis.downsample_neuron(n, float("inf"), inplace=False)
        nodes = n.nodes.reset_index(drop=True)
        xyz, parents = cap_tree(nodes, MAX_NODES)

        row = meta[meta.bodyId == n.id].iloc[0]
        spec_type = spec_of_query.get(str(row.type), str(row.type))
        records.append((int(n.id), spec_type, xyz, parents, int(row.pre), int(row.post)))
        print(f"  {n.id} {spec_type:12s} nodes={len(xyz):4d} pre={row.pre} post={row.post}")

    records.sort(key=lambda r: (order.get(r[1], 99), r[0]))

    with open(OUT, "wb") as f:
        f.write(b"FLY1")
        f.write(struct.pack("<H", len(records)))
        for body, typ, xyz, parents, pre, post in records:
            f.write(struct.pack("<I", body))
            f.write(typ.encode("ascii", "replace")[:15].ljust(16, b"\0"))
            f.write(struct.pack("<HII", len(xyz), pre, post))
            f.write(xyz.astype("<i2").tobytes())
            f.write(parents.astype("<u2").tobytes())

    print(f"wrote {OUT} ({os.path.getsize(OUT)} bytes)")

if __name__ == "__main__":
    main()
