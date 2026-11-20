#!/usr/bin/env python3
"""Download OpenStreetMap streets around central Londrina and export local X/Z segments.

Usage: python tools/import_londrina_osm.py
       python tools/import_londrina_osm.py --radius-km 3
Output: assets/city/londrina_roads.csv (x1,z1,x2,z2,width,kind)
OSM data © OpenStreetMap contributors, ODbL 1.0.
"""
import argparse
import csv
import json
import math
import pathlib
import urllib.parse
import urllib.request

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
    args = parser.parse_args()
    if not 0 < args.radius_km <= 10:
        parser.error("radius-km must be in (0, 10]")
    radius = args.radius_km * 1000
    lat_delta = radius / 111320
    lon_delta = radius / (111320 * math.cos(math.radians(LAT0)))
    bbox = (LAT0-lat_delta, LON0-lon_delta, LAT0+lat_delta, LON0+lon_delta)
    roads = "|".join(ROAD_WIDTHS)
    query = (
        "[out:json][timeout:180];"
        f'way["highway"~"^({roads})$"]'
        f"({bbox[0]:.7f},{bbox[1]:.7f},{bbox[2]:.7f},{bbox[3]:.7f});"
        "out geom;"
    )
    req = urllib.request.Request(
        "https://overpass.kumi.systems/api/interpreter",
        data=urllib.parse.urlencode({"data": query}).encode(),
        headers={"User-Agent": "RacingCar-OSM-Importer/1.0 (educational game)"},
    )
    print("Downloading road geometry from OpenStreetMap Overpass...")
    with urllib.request.urlopen(req, timeout=240) as response:
        data = json.load(response)
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
    print("Source: © OpenStreetMap contributors (ODbL 1.0)")

if __name__ == "__main__":
    main()
