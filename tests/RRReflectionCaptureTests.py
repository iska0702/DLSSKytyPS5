import importlib.util
from pathlib import Path
import struct
import sys
import unittest

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location(
    "rr_reflections", Path(__file__).resolve().parents[1] / "tools/Analyze_RR_Reflection_Capture.py")
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


def fixture(*pixels):
    return struct.pack("<4I", len(pixels), 1, 600, 1) + b"".join(
        struct.pack("<fII", *pixel) for pixel in pixels)


class ReflectionTests(unittest.TestCase):
    def test_unwritten_pixel_is_not_fabricated_miss(self):
        r = capture.analyze(fixture((2.5, 17, 1), (float("inf"), 0xffffffff, 1), (0, 0, 0)))
        self.assertEqual((r["hits"], r["misses"], r["missing"]), (1, 1, 1))
        self.assertEqual(r["minimum_hit_parameter"], 2.5)
        self.assertFalse(r["dense"])
        self.assertFalse(r["certified_rr_inputs"])

    def test_bad_hit_parameter_detected_but_miss_parameter_ignored(self):
        r = capture.analyze(fixture((float("nan"), 1, 1), (-1, 2, 1), (float("nan"), 0xffffffff, 1)))
        self.assertEqual(r["invalid_hit_parameters"], 2)
        self.assertTrue(r["dense"])
        self.assertFalse(r["certified_rr_inputs"])

    def test_truncated_and_extra_data_rejected(self):
        good = fixture((1, 1, 1))
        for data in (good[:10], good[:-1], good + b"x"):
            with self.subTest(length=len(data)), self.assertRaises(ValueError):
                capture.analyze(data)

    def test_unknown_write_marker_rejected(self):
        with self.assertRaisesRegex(ValueError, "write marker"):
            capture.analyze(fixture((1, 1, 2)))

    def test_lighting_coverage_does_not_certify_ray_hit(self):
        data = bytearray(fixture((0, 0, 2), (2.5, 17, 1)))
        struct.pack_into("<I", data, 12, 2)
        result = capture.analyze(data)
        self.assertEqual((result["covered"], result["written"], result["missing"]), (2, 1, 1))
        self.assertFalse(result["dense"])

    def test_zero_indirect_work_is_distinct_from_ray_misses(self):
        data = bytearray(fixture((0, 0, 0)))
        struct.pack_into("<I", data, 12, 3)
        data += bytes(48)
        result = capture.analyze(data)
        self.assertEqual(result["indirect_dispatch_groups"], [(0, 0, 0)] * 4)
        self.assertEqual((result["misses"], result["written"]), (0, 0))

    def test_empty_tile_sentinel_explains_one_group_without_lighting(self):
        data = bytearray(fixture((0, 0, 0)))
        struct.pack_into("<I", data, 12, 4)
        data += struct.pack("<12I", *([1] * 12))
        data += struct.pack("<4I", *([0xffffffff] * 4))
        result = capture.analyze(data)
        self.assertEqual(result["empty_single_group_dispatches"], 4)
        self.assertEqual((result["hits"], result["covered"]), (0, 0))

    def test_multiple_probe_rays_are_not_a_unique_reflection_guide(self):
        data = struct.pack("<4I", 2, 1, 600, 5)
        data += struct.pack("<fIII", 2.5, 17, 1, 3)
        data += struct.pack("<fIII", 1.0, 18, 1, 1) + bytes(64)
        result = capture.analyze(data)
        self.assertTrue(result["dense"])
        self.assertEqual(result["ray_exports"], 4)
        self.assertEqual(result["repeated_pixels"], 1)
        self.assertFalse(result["unique_ray_per_pixel"])
        self.assertFalse(result["certified_rr_inputs"])

    def test_inconsistent_ray_count_rejected(self):
        for marker, count in ((1, 0), (0, 1), (2, 1)):
            data = struct.pack("<4IfIII", 1, 1, 600, 5, 0, 0, marker, count) + bytes(64)
            with self.subTest(marker=marker, count=count), self.assertRaisesRegex(ValueError, "ray count"):
                capture.analyze(data)


if __name__ == "__main__":
    unittest.main()
