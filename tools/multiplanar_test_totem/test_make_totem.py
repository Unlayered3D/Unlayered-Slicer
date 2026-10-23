import argparse
import math
import unittest

import numpy as np

from make_totem import DEFAULT_OVERHANGS, DEFAULT_TILTS, build, manifest, shelf


class TotemGeometryTests(unittest.TestCase):
    def test_shelf_underside_has_its_declared_angle(self):
        # Measure the actual downward-facing mesh facets, including the root overlap.
        # Previously the overlap added horizontal run without extending the slope,
        # making a declared 15-degree shelf about 13.2 degrees instead.
        for scale in (0.5, 1.0, 2.0):
            for angle in DEFAULT_OVERHANGS:
                with self.subTest(scale=scale, angle=angle):
                    mesh = shelf(7 * scale, 8 * scale, 6 * scale, angle, 1.6 * scale,
                                 root_inset=scale)
                    downward = mesh.face_normals[mesh.face_normals[:, 2] < -0.01]
                    self.assertEqual(len(downward), 2)
                    measured = np.degrees(np.arccos(-downward[:, 2]))
                    np.testing.assert_allclose(measured, angle, atol=1e-8)
                    self.assertTrue(mesh.is_watertight)

    def test_whole_totem_scales_uniformly_and_is_one_connected_solid(self):
        outputs = []
        for scale in (1.0, 0.5):
            p = argparse.Namespace(scale=scale, plate=76., tilts=DEFAULT_TILTS,
                                   overhangs=DEFAULT_OVERHANGS, only="")
            mesh, _ = build(p)
            self.assertTrue(mesh.is_watertight)
            self.assertEqual(mesh.body_count, 1)
            outputs.append(mesh)
        np.testing.assert_allclose(outputs[1].bounds * 2, outputs[0].bounds, atol=1e-5)
        self.assertTrue(math.isclose(outputs[1].volume * 8, outputs[0].volume, rel_tol=1e-5))

    def test_subsets_keep_foundations_and_report_only_present_features(self):
        for selection in ("shelves", "tilt_ladders"):
            with self.subTest(selection=selection):
                p = argparse.Namespace(scale=1., plate=76., tilts=DEFAULT_TILTS,
                                       overhangs=DEFAULT_OVERHANGS, only=selection)
                mesh, modules = build(p)
                self.assertTrue(mesh.is_watertight)
                self.assertEqual(mesh.body_count, 1)
                info = manifest(p, mesh, modules)
                self.assertEqual(bool(info["tilt_rungs"]), selection == "tilt_ladders")
                self.assertEqual(bool(info["overhang_shelves"]), selection == "shelves")
                self.assertEqual(info["corners"], {})
                if selection == "shelves":
                    self.assertIn("tower_dome", modules)


if __name__ == "__main__":
    unittest.main()
