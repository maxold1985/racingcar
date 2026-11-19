# Blender 4.x/5.x FBX -> GLB conversion with texture relinking.
# Usage: blender -b --python tools/convert_city_fbx.py -- SOURCE_FBX_DIR OUTPUT_GLB_DIR
import bpy
import sys
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

print(f"[CITY] Indexed {len(texture_index)} texture names under {kit_root}")
for name in NAMES:
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
    output = dst / (name + ".glb")
    bpy.ops.export_scene.gltf(filepath=str(output), export_format="GLB",
                              export_materials="EXPORT", export_image_format="AUTO")
    print(f"[CITY] EXPORTED {name}: relinked={relinked}, missing_images={unresolved}")
print("[CITY] Finished. If models still lack textures, inspect FBX materials and original texture files.")
