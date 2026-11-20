# Blender 4.x/5.x FBX -> GLB conversion with texture relinking.
# Usage: blender -b --python tools/convert_city_fbx.py -- SOURCE_FBX_DIR OUTPUT_GLB_DIR
import bpy
import sys
import json
import struct
from mathutils import Vector
from pathlib import Path

NAMES = [
    "Street_2Lane", "Street_4WayIntersection", "Street_TIntersection",
    "Building_Small_1", "Building_Medium_2_001", "Building_Large_2",
    "Sidewalk_Straight_3m", "Sidewalk_Corner_Round_3m",
]
args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(args) != 2:
    raise SystemExit("Usage: blender -b --python tools/convert_city_fbx.py -- SOURCE_FBX_DIR OUTPUT_GLB_DIR")
src, dst = map(Path, args)
src = src.resolve()
dst = dst.resolve()
dst.mkdir(parents=True, exist_ok=True)

# Asset packs often store texture images outside the FBX export directory.
# Index the kit root and its export subdirectories by case-insensitive basename.
kit_root = src
for _ in range(2):
    kit_root = kit_root.parent
texture_index = {}
for file in kit_root.rglob("*"):
    if file.is_file() and file.suffix.lower() in {".png", ".jpg", ".jpeg", ".tga", ".webp", ".bmp"}:
        texture_index.setdefault(file.name.lower(), file)

def strip_extra_vertex_colors(glb_path):
    """Remove COLOR_1+ from glTF primitives without touching UVs or image data.

    Some FBX exports contain a secondary red vertex-color layer. The raylib
    importer can apply it as a tint, even though COLOR_0 is neutral white.
    """
    raw = glb_path.read_bytes()
    if raw[:4] != b"glTF" or len(raw) < 20:
        raise RuntimeError(f"Not a valid GLB: {glb_path}")
    version, total_size = struct.unpack_from("<II", raw, 4)
    if version != 2 or total_size != len(raw):
        raise RuntimeError(f"Invalid GLB header: {glb_path}")
    offset = 12
    chunks = []
    removed = 0
    while offset < len(raw):
        length, chunk_type = struct.unpack_from("<I4s", raw, offset)
        offset += 8
        payload = raw[offset:offset + length]
        if len(payload) != length:
            raise RuntimeError("Truncated GLB chunk")
        offset += length
        if chunk_type == b"JSON":
            doc = json.loads(payload.decode("utf-8"))
            for mesh in doc.get("meshes", []):
                for primitive in mesh.get("primitives", []):
                    attrs = primitive.get("attributes", {})
                    for key in list(attrs):
                        if key.startswith("COLOR_") and key != "COLOR_0":
                            del attrs[key]
                            removed += 1
            payload = json.dumps(doc, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
            payload += b" " * (-len(payload) % 4)
        chunks.append((chunk_type, payload))
    rebuilt = bytearray(struct.pack("<4sII", b"glTF", 2,
        12 + sum(8 + len(payload) for _, payload in chunks)))
    for chunk_type, payload in chunks:
        rebuilt += struct.pack("<I4s", len(payload), chunk_type)
        rebuilt += payload
    glb_path.write_bytes(rebuilt)
    return removed


print(f"[CITY] Indexed {len(texture_index)} texture names under {kit_root}")
metrics = {}
for model_id, name in enumerate(NAMES, 1):
    file = src / (name + ".fbx")
    if not file.is_file():
        print("[CITY] SKIP missing:", file)
        continue
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=str(file))

    relinked = 0
    unresolved = []
    for img in bpy.data.images:
        if img.source != "FILE":
            continue
        original = Path(bpy.path.abspath(img.filepath))
        if original.is_file():
            continue
        candidate = texture_index.get(original.name.lower())
        if candidate is None:
            # FBX can contain Windows-style backslashes on non-Windows machines.
            candidate = texture_index.get(img.filepath.replace("\\", "/").split("/")[-1].lower())
        if candidate is not None:
            img.filepath = str(candidate)
            img.reload()
            relinked += 1
        else:
            unresolved.append(img.name)
    for img in bpy.data.images:
        if img.source == "FILE" and Path(bpy.path.abspath(img.filepath)).is_file():
            try:
                img.pack()
            except RuntimeError as error:
                print(f"[CITY] Cannot pack {img.name}: {error}")
    # Evaluated world-space bounds match the exported geometry after FBX import.
    bpy.context.view_layer.update()
    corners = []
    for obj in bpy.context.scene.objects:
        if obj.type == "MESH":
            corners.extend(obj.matrix_world @ Vector(corner) for corner in obj.bound_box)
    if corners:
        width = max(v.x for v in corners) - min(v.x for v in corners)
        depth = max(v.y for v in corners) - min(v.y for v in corners)
        # Blender glTF exports Z-up meshes as Y-up for raylib.
        # Blender Y footprint becomes raylib Z footprint.
        metrics[model_id] = (width, depth)
    else:
        metrics[model_id] = (0.0, 0.0)
    output = dst / (name + ".glb")
    bpy.ops.export_scene.gltf(filepath=str(output), export_format="GLB",
                              export_materials="EXPORT", export_image_format="AUTO")
    stripped = strip_extra_vertex_colors(output)
    print(f"[CITY] EXPORTED {name}: relinked={relinked}, removed_extra_color_layers={stripped}, missing_images={unresolved}")
# The runtime loads these metrics before the procedural generator.
metrics_path = dst.parent.parent / "scripts" / "city_asset_metrics.luau"
metrics_path.parent.mkdir(parents=True, exist_ok=True)
with metrics_path.open("w", encoding="utf-8") as out:
    out.write("-- Generated by tools/convert_city_fbx.py. Do not edit manually.\\nCityAssets = {\\n")
    for index, (width, depth) in metrics.items():
        out.write(f"    [{index}] = {{width = {width:.8g}, depth = {depth:.8g}}},\\n")
    out.write("}\\n")
print(f"[CITY] Wrote dimensions: {metrics_path}")
print("[CITY] Finished. If models still lack textures, inspect FBX materials and original texture files.")
