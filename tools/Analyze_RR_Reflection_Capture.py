"""Inspect raw closest-ray exports; a complete capture is not an RR input certificate."""
import argparse
import json
import math
from pathlib import Path
import struct


def analyze(data):
    if len(data) < 16:
        raise ValueError("Missing reflection capture header")
    width, height, frame, version = struct.unpack_from("<4I", data)
    if version not in (1, 2, 3, 4, 5) or not 0 < width <= 8192 or not 0 < height <= 8192:
        raise ValueError("Unsupported reflection capture header")
    pixels = width * height
    payload_end = 16 + pixels * (16 if version >= 5 else 12)
    if len(data) != payload_end + (64 if version >= 4 else 48 if version == 3 else 0):
        raise ValueError("Truncated or oversized reflection capture")
    written = hits = misses = invalid = covered_without_ray = 0
    ray_exports = repeated_pixels = 0
    minimum, maximum = math.inf, -math.inf
    for record in struct.iter_unpack("<fIII" if version >= 5 else "<fII", memoryview(data)[16:payload_end]):
        distance, hit, present = record[:3]
        if version >= 5:
            count = record[3]
            if (present == 1) != (count > 0):
                raise ValueError("Inconsistent ray count and write marker")
            ray_exports += count
            repeated_pixels += count > 1
        if present == 2 and version >= 2:
            covered_without_ray += 1
            continue
        if present not in (0, 1):
            raise ValueError("Invalid per-pixel write marker")
        if not present:
            continue
        written += 1
        if hit == 0xffffffff:
            misses += 1
            continue
        hits += 1
        if not math.isfinite(distance) or distance < 0:
            invalid += 1
            continue
        minimum, maximum = min(minimum, distance), max(maximum, distance)
    result = dict(frame=frame, width=width, height=height, pixels=pixels, written=written,
                hits=hits, misses=misses, invalid_hit_parameters=invalid,
                missing=pixels-written, dense=written == pixels,
                covered=written+covered_without_ray, covered_without_ray=covered_without_ray,
                minimum_hit_parameter=minimum if math.isfinite(minimum) else None,
                maximum_hit_parameter=maximum if math.isfinite(maximum) else None,
                certified_rr_inputs=False)
    if version >= 5:
        result.update(ray_exports=ray_exports, repeated_pixels=repeated_pixels,
                      unique_ray_per_pixel=written == pixels and repeated_pixels == 0)
    if version >= 3:
        result["indirect_dispatch_groups"] = list(struct.iter_unpack("<3I", data[payload_end:payload_end+48]))
    if version >= 4:
        result["first_tile_words"] = list(struct.unpack_from("<4I", data, payload_end+48))
        result["empty_single_group_dispatches"] = sum(
            groups == (1, 1, 1) and tile == 0xffffffff
            for groups, tile in zip(result["indirect_dispatch_groups"], result["first_tile_words"]))
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = json.dumps(analyze(args.capture.read_bytes()), indent=2)
    if args.output:
        args.output.write_text(result + "\n", encoding="utf-8")
    print(result)
