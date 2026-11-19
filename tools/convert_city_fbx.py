# Run with Blender: blender -b --python tools/convert_city_fbx.py -- "C:/path/to/Exports/FBX (Unity)" "assets/city"
# Converts the eight models currently used by scripts/city_generator.luau.
import bpy
import sys
from pathlib import Path

names = [
    "Street_2Lane", "Street_4WayIntersection", "Street_TIntersection",
    "Building_Small_1", "Building_Medium_2_001", "Building_Large_2",
    "Sidewalk_Straight_3m", "Sidewalk_Corner_Round_3m",
]
args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if len(args) != 2:
    raise SystemExit("Usage: blender -b --python tools/convert_city_fbx.py -- SOURCE_FBX_DIR OUTPUT_GLB_DIR")
src, dst = map(Path, args)
dst.mkdir(parents=True, exist_ok=True)
for name in names:
    file = src / (name + ".fbx")
    if not file.is_file():
        print("SKIP missing:", file)
        continue
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=str(file))
    bpy.ops.export_scene.gltf(filepath=str(dst / (name + ".glb")), export_format="GLB")
    print("EXPORTED:", name)
print("Check mesh dimensions, pivots and material appearance in the game.")
