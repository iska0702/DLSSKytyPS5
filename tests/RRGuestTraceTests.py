"""Regression cases for temporal/resource-version mistakes in lighting trace analysis."""
import importlib.util
from pathlib import Path
import sys
import unittest

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location(
    "rr_trace", Path(__file__).resolve().parents[1] / "tools/Analyze_RR_Guest_Trace.py")
trace = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trace)


def image(address, read=False, written=False):
    return {"address": address, "read": read, "written": written, "stage": "CS", "index": "0"}


def operation(ident, frame, images=(), targets=()):
    return {"id": ident, "frame": frame, "images": list(images), "targets": list(targets)}


def native_image(address, generation=1, mip="0:1", layer="0:1", aspect="1", **access):
    return dict(image(address, **access), native=f"17:{generation}", mip=mip, layer=layer, aspect=aspect)


class TraceTests(unittest.TestCase):
    def test_native_identity_links_views_with_different_guest_bases(self):
        edges = trace.analyze([operation(1, 10, [native_image(0x1000, written=True)]),
                               operation(2, 10, [native_image(0x2000, read=True)])])
        self.assertEqual([(e["from"], e["to"]) for e in edges], [(1, 2)])
        self.assertEqual(edges[0]["certainty"], "native-subresource candidate")

    def test_recycled_native_slot_is_not_the_old_allocation(self):
        self.assertEqual(trace.analyze([
            operation(1, 10, [native_image(0x1000, generation=1, written=True)]),
            operation(2, 10, [native_image(0x1000, generation=2, read=True)])]), [])

    def test_disjoint_native_subresources_are_not_linked(self):
        for changed in ({"mip": "1:1"}, {"layer": "1:1"}, {"aspect": "2"}):
            with self.subTest(changed=changed):
                self.assertEqual(trace.analyze([
                    operation(1, 10, [native_image(0x1000, written=True)]),
                    operation(2, 10, [native_image(0x1000, read=True, **changed)])]), [])

    def test_partial_native_write_preserves_other_mip_producer(self):
        edges = trace.analyze([
            operation(1, 10, [native_image(0x1000, mip="0:2", written=True)]),
            operation(2, 10, [native_image(0x1000, mip="1:1", written=True)]),
            operation(3, 10, [native_image(0x1000, mip="0:2", read=True)])])
        self.assertEqual([(e["from"], e["to"]) for e in edges], [(1, 3), (2, 3)])

    def test_full_native_write_replaces_old_producer(self):
        edges = trace.analyze([
            operation(1, 10, [native_image(0x1000, written=True)]),
            operation(2, 10, [native_image(0x1000, written=True)]),
            operation(3, 10, [native_image(0x1000, read=True)])])
        self.assertEqual([(e["from"], e["to"]) for e in edges], [(2, 3)])

    def test_invalid_native_view_rejected(self):
        with self.assertRaisesRegex(ValueError, "Invalid native"):
            trace.analyze([operation(1, 10, [native_image(0x1000, mip="0:0", read=True)])])

    def test_read_modify_write_consumes_preceding_version(self):
        edges = trace.analyze([
            operation(1, 10, [image(0x1000, written=True)]),
            operation(2, 10, [image(0x1000, read=True, written=True)]),
            operation(3, 10, [image(0x1000, read=True)])])
        self.assertEqual([(e["from"], e["to"]) for e in edges], [(1, 2), (2, 3)])

    def test_reused_address_in_next_frame_is_not_linked(self):
        self.assertEqual(trace.analyze([
            operation(1, 10, [image(0x1000, written=True)]),
            operation(2, 11, [image(0x1000, read=True)])]), [])

    def test_attachment_can_preserve_ray_output(self):
        edges = trace.analyze([
            operation(1, 10, [image(0x1000, written=True)]),
            operation(2, 10, targets=[{"address": 0x1000, "slot": "0", "written": True}])])
        self.assertEqual(edges[0]["certainty"], "attachment preservation candidate")

    def test_interleaved_log_lines_are_associated_by_pass_id(self):
        passes = trace.parse([
            "[RRTrace][Pass] id=2 frame=10 kind=draw shader=BB",
            "[RRTrace][Pass] id=1 frame=10 kind=compute shader=AA",
            "[RRTrace][Stage] pass=1 stage=CS shader=AA bvh=true bda_writes=false",
            "[RRTrace][End] pass=2", "[RRTrace][End] pass=1"])
        self.assertEqual([p["id"] for p in passes], [1, 2])
        self.assertTrue(passes[0]["stages"][0]["bvh"])

    def test_incomplete_capture_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "Incomplete pass"):
            trace.parse(["[RRTrace][Pass] id=1 frame=10 kind=compute shader=AA"])


if __name__ == "__main__":
    unittest.main()
