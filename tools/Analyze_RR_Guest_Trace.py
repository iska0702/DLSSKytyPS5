"""Build per-frame descriptor provenance from KYTY_RR_TRACE_FRAME captures.

This produces investigation evidence, never an RR input certification. Exact base-address
matches are candidate dependencies: allocation generations, aliases, blend reads, BDA and
indirect image accesses still need native resource/GPU verification before replacing a pass.
"""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import re

RECORD = re.compile(r"\[RRTrace\]\[(\w+)\]\s+(.*)")


def parse(lines):
    passes = {}
    pending = defaultdict(list)
    completed = set()
    for line in lines:
        match = RECORD.search(line)
        if not match:
            continue
        kind, text = match.groups()
        values = dict(field.split("=", 1) for field in text.split() if "=" in field)
        if kind == "Pass":
            ident = int(values["id"])
            if ident in passes:
                raise ValueError(f"Duplicate pass {ident}; analyze each process separately")
            passes[ident] = {"id": ident, "frame": int(values["frame"]),
                             "kind": values["kind"], "shader": values["shader"],
                             "stages": [], "images": [], "buffers": [], "targets": []}
        else:
            pending[int(values["pass"])].append((kind, values))
    for ident, records in pending.items():
        if ident not in passes:
            raise ValueError(f"Resource without pass {ident}: incomplete capture")
        for kind, values in records:
            if kind == "End":
                completed.add(ident)
                continue
            for key in ("read", "written", "atomic", "bvh", "bda_writes", "depth"):
                if key in values:
                    values[key] = values[key] == "true"
            if "address" in values:
                values["address"] = int(values["address"], 16)
            field = {"Stage": "stages", "Image": "images", "Buffer": "buffers",
                     "Target": "targets"}.get(kind)
            if field:
                passes[ident][field].append(values)
    if set(passes) != completed:
        raise ValueError(f"Incomplete pass records: {sorted(set(passes) - completed)[:16]}")
    return sorted(passes.values(), key=lambda p: p["id"])


def native_view(image):
    identity = image.get("native", "unknown")
    if identity == "unknown":
        return None
    index, generation = map(int, identity.split(":"))
    mip, levels = map(int, image["mip"].split(":"))
    layer, layers = map(int, image["layer"].split(":"))
    aspect = int(image["aspect"])
    if index < 0 or generation <= 0 or mip < 0 or layer < 0 or levels <= 0 or layers <= 0 or aspect <= 0:
        raise ValueError("Invalid native image generation/subresource range")
    return ((index, generation), aspect, mip, mip + levels, layer, layer + layers)


def overlaps(a, b):
    return bool(a[1] & b[1]) and a[2] < b[3] and b[2] < a[3] and a[4] < b[5] and b[4] < a[5]


def contains(a, b):
    return (a[1] & b[1]) == b[1] and a[2] <= b[2] and a[3] >= b[3] and a[4] <= b[4] and a[5] >= b[5]


def analyze(passes):
    writers = {}
    native_writers = defaultdict(list)
    edges = []
    frame = None
    for item in passes:
        if item["frame"] != frame:
            # Never connect a previous frame/allocation just because an address was reused.
            writers.clear()
            native_writers.clear()
            frame = item["frame"]
        seen = set()
        for image in item["images"] + item["targets"]:
            address = image["address"]
            view = native_view(image)
            sources = ([source for old_view, source in native_writers[view[0]] if overlaps(old_view, view)]
                       if view else [writers.get(address)])
            attachment = "slot" in image
            # A draw can preserve prior pixels (coverage, discard, blending). Keep that
            # conservative dependency instead of treating every attachment as a full clear.
            for source in sources:
                if (image.get("read") or attachment) and address and source and (source, address) not in seen:
                    edges.append({"from": source, "to": item["id"], "address": address,
                                  "stage": image.get("stage", "attachment"),
                                  "index": int(image.get("index", image.get("slot", 0))),
                                  "native": image.get("native", "unknown"),
                                  "certainty": "attachment preservation candidate" if attachment else
                                               "native-subresource candidate" if view else "base-address candidate"})
                    seen.add((source, address))
        # Read/modify/write resources consume the preceding version, not their own output.
        for image in item["images"] + item["targets"]:
            if image.get("written") and image["address"]:
                view = native_view(image)
                if view:
                    history = native_writers[view[0]]
                    # Preserve writers of untouched levels/layers/aspects. Partial writes
                    # remain conservative; attachment reads above preserve prior contents.
                    history[:] = [(old, source) for old, source in history if not contains(view, old)]
                    history.append((view, item["id"]))
                else:
                    writers[image["address"]] = item["id"]
    return edges


def report(passes, edges, game="Guest"):
    by_id = {p["id"]: p for p in passes}
    successors = defaultdict(list)
    for edge in edges:
        successors[edge["from"]].append(edge)
    bvh = [p for p in passes if any(s["bvh"] for s in p["stages"])]
    lines = [f"# {game} RR lighting investigation", "",
             f"Captured {len(passes)} passes, {len(edges)} candidate image dependencies, "
             f"{len(bvh)} BVH-capable shader submissions.", "",
             "Descriptor traces do not certify noisy HDR, albedo, normal space, roughness "
             "or reflection distance. Native identity/generation and overlapping subresources are used "
             "when present; older records use base-address candidates. BDA, distinct native image aliases, "
             "pixel coverage and attachment blend reads remain unresolved. Pass order is host recording order.", "",
             "A BVH-capable submission may have zero work or an empty tile list; it does not prove that any ray instruction executed.", "",
             "## Per-frame coverage", ""]
    for frame in sorted({p["frame"] for p in passes}):
        counts = Counter(p["kind"] for p in passes if p["frame"] == frame)
        lines.append(f"- Frame {frame}: " + ", ".join(f"{n} {kind}" for kind, n in counts.items()))
    lines += ["", "## BVH outputs and following readers", ""]
    if not bvh:
        lines.append("No submitted BVH dispatch was captured in this frame window.")
    for item in bvh:
        lines.append(f"- Frame {item['frame']} pass {item['id']}: `{item['shader']}` ({item['kind']})")
        outputs = [i for i in item["images"] if i["written"]]
        if not outputs:
            lines.append("  - No explicit image output; inspect buffer/BDA writes and indirect resources.")
        for image in outputs:
            lines.append(f"  - Image {image['index']} at `0x{image['address']:X}`, "
                         f"{image['extent']}, guest format {image['format']}, source {image['source']}.")
        visited = {item["id"]}
        frontier = [item["id"]]
        for depth in range(3):
            following = []
            for ident in frontier:
                for edge in successors[ident]:
                    target = by_id[edge["to"]]
                    if target["id"] in visited:
                        continue
                    visited.add(target["id"])
                    following.append(target["id"])
                    lines.append(f"  - Reader depth {depth + 1}: pass {target['id']} "
                                 f"`{target['shader']}`, input {edge['stage']}:{edge['index']} "
                                 f"at `0x{edge['address']:X}`.")
            frontier = following
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--output", type=Path, required=True, help="Output basename (.json and .md)")
    parser.add_argument("--game", default="Guest", help="Game name for the report title (not an input certificate)")
    args = parser.parse_args()
    with args.log.open(encoding="utf-8", errors="replace") as stream:
        passes = parse(stream)
    if not passes:
        parser.error("No RRTrace records; set KYTY_RR_TRACE_FRAME and --dlss-diagnostics")
    edges = analyze(passes)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.with_suffix(".json").write_text(
        json.dumps({"schema": 1, "game": args.game, "certified_rr_inputs": False, "passes": passes,
                    "candidate_image_edges": edges}, indent=2), encoding="utf-8")
    args.output.with_suffix(".md").write_text(report(passes, edges, args.game), encoding="utf-8")
    print(f"{len(passes)} passes, {len(edges)} candidate dependencies -> {args.output}.json/.md")


if __name__ == "__main__":
    main()
