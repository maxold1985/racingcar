#!/usr/bin/env python3
"""Download OpenStreetMap streets around central Londrina and export local X/Z segments.

Usage: python tools/import_londrina_osm.py
       python tools/import_londrina_osm.py --radius-km 3
Output: assets/city/londrina_roads.csv and londrina_buildings.csv
OSM data © OpenStreetMap contributors, ODbL 1.0.
"""
import argparse
import csv
import json
import math
import pathlib
import urllib.parse
import urllib.request
import urllib.error
import time

LAT0 = -23.311287
LON0 = -51.159504
ROAD_WIDTHS = {
    "motorway": 12, "trunk": 11, "primary": 10, "secondary": 9,
    "tertiary": 8, "residential": 6, "unclassified": 6, "living_street": 5,
    "service": 4,
}

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--radius-km", type=float, default=2.5)
    parser.add_argument("--output", default="assets/city/londrina_roads.csv")
    parser.add_argument("--request-timeout", type=int, default=35,
                        help="Seconds to wait for each Overpass request")
    args = parser.parse_args()
    if not 5 <= args.request_timeout <= 240:
        parser.error("request-timeout must be between 5 and 240 seconds")
    if not 0 < args.radius_km <= 10:
        parser.error("radius-km must be in (0, 10]")
    radius = args.radius_km * 1000
    lat_delta = radius / 111320
    lon_delta = radius / (111320 * math.cos(math.radians(LAT0)))
    bbox = (LAT0-lat_delta, LON0-lon_delta, LAT0+lat_delta, LON0+lon_delta)
    roads = "|".join(ROAD_WIDTHS)
    servers = [
        "https://overpass.kumi.systems/api/interpreter",
        "https://overpass-api.de/api/interpreter",
        "https://overpass.nchc.org.tw/api/interpreter",
    ]
    # Smaller tiles avoid a single very large, long-running Overpass response.
    # Each successful tile is cached, allowing interrupted imports to resume.
    tile_count = max(1, math.ceil((2 * radius) / 1000))
    cache_dir = pathlib.Path(args.output).parent / ".osm_cache"
    cache_dir.mkdir(parents=True, exist_ok=True)
    elements_by_id = {}
    for iy in range(tile_count):
        for ix in range(tile_count):
            south = bbox[0] + (bbox[2]-bbox[0])*iy/tile_count
            north = bbox[0] + (bbox[2]-bbox[0])*(iy+1)/tile_count
            west = bbox[1] + (bbox[3]-bbox[1])*ix/tile_count
            east = bbox[1] + (bbox[3]-bbox[1])*(ix+1)/tile_count
            box = f"({south:.7f},{west:.7f},{north:.7f},{east:.7f})"
            query = (
                f"[out:json][timeout:{args.request_timeout}];("
                f'way["highway"~"^({roads})$"]{box};'
                f'way["building"]{box};'
                ");out geom;"
            )
            cache_file = cache_dir / (
                f"tile_{LAT0}_{LON0}_{args.radius_km:g}_{tile_count}_{ix}_{iy}.json"
            )
            if cache_file.exists():
                try:
                    tile_data = json.loads(cache_file.read_text(encoding="utf-8"))
                except (ValueError, OSError):
                    cache_file.unlink(missing_ok=True)
                    tile_data = None
            else:
                tile_data = None
            if tile_data is None:
                last_error = None
                for attempt in range(3):
                    server = servers[attempt % len(servers)]
                    req = urllib.request.Request(
                        server,
                        data=urllib.parse.urlencode({"data": query}).encode(),
                        headers={"User-Agent": "RacingCar-OSM-Importer/1.1"},
                    )
                    try:
                        print(f"Tile {iy*tile_count+ix+1}/{tile_count**2}, "
                              f"attempt {attempt+1}/3: {server}", flush=True)
                        with urllib.request.urlopen(req, timeout=args.request_timeout+5) as response:
                            tile_data = json.load(response)
                        if "elements" not in tile_data:
                            raise ValueError("Overpass returned no elements field")
                        cache_file.write_text(json.dumps(tile_data), encoding="utf-8")
                        break
                    except (OSError, ValueError, TimeoutError,
                            urllib.error.HTTPError) as error:
                        last_error = error
                        print(f"  Request failed: {error}", flush=True)
                        if attempt < 2:
                            time.sleep(2)
                if tile_data is None:
                    raise RuntimeError(
                        f"Could not download tile {ix},{iy} after 3 attempts. "
                        f"Cached tiles are preserved; rerun the command. "
                        f"Last error: {last_error}"
                    )
            for element in tile_data.get("elements", []):
                if element.get("type") == "way" and "id" in element:
                    elements_by_id[element["id"]] = element
    data = {"elements": list(elements_by_id.values())}
    print(f"Imported {len(elements_by_id)} unique OSM ways.")
    output = pathlib.Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    count = 0
    with output.open("w", newline="", encoding="utf-8") as file:
        writer = csv.writer(file)
        writer.writerow(["x1", "z1", "x2", "z2", "width", "kind"])
        for way in data.get("elements", []):
            kind = way.get("tags", {}).get("highway")
            width = ROAD_WIDTHS.get(kind)
            points = way.get("geometry", [])
            if not width:
                continue
            for a, b in zip(points, points[1:]):
                x1 = math.radians(a["lon"]-LON0)*6378137*math.cos(math.radians(LAT0))
                z1 = -math.radians(a["lat"]-LAT0)*6378137
                x2 = math.radians(b["lon"]-LON0)*6378137*math.cos(math.radians(LAT0))
                z2 = -math.radians(b["lat"]-LAT0)*6378137
                if math.hypot(x2-x1, z2-z1) < 0.1:
                    continue
                writer.writerow([f"{x1:.3f}",f"{z1:.3f}",f"{x2:.3f}",f"{z2:.3f}",width,kind])
                count += 1
    print(f"Wrote {count} road segments to {output}")
    building_output = output.with_name("londrina_buildings.csv")
    building_count = 0
    with building_output.open("w", newline="", encoding="utf-8") as file:
        writer = csv.writer(file)
        writer.writerow(["x", "z", "width", "depth", "height"])
        for way in data.get("elements", []):
            tags = way.get("tags", {})
            if "building" not in tags:
                continue
            points = way.get("geometry", [])
            if len(points) < 4:
                continue
            xs = [math.radians(p["lon"]-LON0)*6378137*math.cos(math.radians(LAT0)) for p in points]
            zs = [-math.radians(p["lat"]-LAT0)*6378137 for p in points]
            width, depth = max(xs)-min(xs), max(zs)-min(zs)
            if not (2 <= width <= 150 and 2 <= depth <= 150):
                continue
            try:
                height = float(tags.get("height", "").replace(" m", ""))
            except ValueError:
                height = 0
            if height <= 0:
                try:
                    levels = float(tags.get("building:levels", ""))
                except ValueError:
                    levels = 2
                height = max(3.0, min(180.0, levels*3.2))
            writer.writerow([f"{(min(xs)+max(xs))/2:.3f}",
                             f"{(min(zs)+max(zs))/2:.3f}",
                             f"{width:.3f}", f"{depth:.3f}", f"{min(height,180):.3f}"])
            building_count += 1
    print(f"Wrote {building_count} building footprints to {building_output}")
    print("Source: © OpenStreetMap contributors (ODbL 1.0)")

if __name__ == "__main__":
    main()
