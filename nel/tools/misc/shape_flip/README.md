# shape_flip

Small NeL tool to inspect a `.shape` file and flip the triangle winding of the
render passes that use a given texture, **in place in the file bytes**.

It was written to prepare a data-only override for
`ge_mission_enclos.shape` (the mission "enclos" prop, `g_tower2` sub-mesh), whose
wall panels show up in the Vulkan client at some spots. No client code is
involved: the patched shape is dropped in the client's `user/` directory, which
is searched before the `.bnp` archives.

## Build

```sh
cmake -DWITH_NEL_TOOLS=ON ... && ninja shape_flip
```

## Usage

```sh
# Inspect: materials, textures, per-LOD render passes and triangle counts
shape_flip ge_mission_enclos.shape

# Flip the winding of every pass whose material uses G_tower2.tga
shape_flip ge_mission_enclos.shape ge_mission_enclos_fixed.shape g_tower2

# Dry run: same, but report the blocks that would be patched
shape_flip ge_mission_enclos.shape - g_tower2

# Select by material index instead of texture (list or range)
shape_flip ge_newbieland_starting_village.shape out.shape --mat=12,38,65,83
shape_flip ge_newbieland_starting_village.shape out.shape --mat=52-65 --degenerate

# --degenerate: collapse every triangle (i0,i0,i0) instead of swapping the
# 2nd/3rd index. The faces disappear whatever the driver's winding convention
# is, which makes it a reliable probe for "which material is this surface?".

# Per-material geometry statistics (triangle count, bbox, longest edge, area)
shape_flip ge_newbieland_starting_village.shape stats

# Debug: dump the serialized form of each selected index block
shape_flip ge_mission_enclos.shape dump
```

`<out.shape>` has exactly the same size as the input. Only the 4-byte index
values of the selected pass blocks change, and only their 2nd and 3rd index are
swapped, so if anything goes wrong the file is obviously broken instead of
subtly different.

The tool prints each material with its flags (two-sided, blend, lighting, alpha
test), shader, texgen and textures, including the lightmaps, which is what is
needed to tell a lightmapped prop material from a plain one.

## How the patch is located

`CIndexBuffer::serial()` stores the indices as `uint32` whatever the runtime
format is, preceded by a version byte, `_NbIndexes`, `_Capacity` and the
`std::vector` count. The tool asks the NeL loader for the index data of each
selected render pass, searches for that exact byte pattern from the end of the
previous block, and requires the prefix above to match, so it does not need to
parse the whole shape format.

## Extracting the shape from the archives

`.bnp` files are a flat name -> (offset, size) table plus data; the table offset
is the last `uint32` of the file. Any extractor works, for example
`bnp_make`/`snp_make` or a 20-line script.

`ge_mission_enclos.shape` lives in `objects.bnp` (not in
`newbieland_shapes.bnp`, which has `canope_newbieland.shape`).

## Overriding a shape in a client install

Create `<install>/user/` (it already exists in a normal Ryzom install) and copy
the patched file there under its original name:

```sh
cp ge_mission_enclos_fixed.shape "<install>/user/ge_mission_enclos.shape"
```

The client adds `user/` as a search path before `data/` (`addPreDataPaths()` in
`ryzom/client/src/init.cpp`) and files are indexed by base name, so a flat
`user/<name>.shape` wins over `objects.bnp@<name>.shape`. The client log proves
it:

```
PATH: CPath::insertFileInMap(ge_mission_enclos.shape, objects.bnp@ge_mission_enclos.shape, 0, shape):
      already inserted from '<install>/user/', skip it
```

`unpack/` is *not* a search path: it is the patch manager's staging directory
(`ClientPatchPath`) and its contents are moved into `data/` (and can be deleted
by the patcher), so it must not be used for overrides.
