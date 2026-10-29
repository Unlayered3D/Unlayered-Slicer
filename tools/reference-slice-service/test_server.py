# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Unlayered 3D. See LICENSE in this directory.
"""End-to-end tests of the reference slicing service over real HTTP (standard library only).

    python -m unittest -v          (from this directory)

Each test talks to a server on an ephemeral loopback port exactly as the slicer does: the same
multipart fields, the same polling, the grant, the download and the refusal shapes of
SLICE_SERVICE_PROTOCOL.md.
"""

from __future__ import annotations

import hashlib
import io
import json
import re
import struct
import threading
import time
import unittest
import uuid
import zipfile
from urllib.error import HTTPError
from urllib.request import Request, urlopen

import server

# A 20 x 30 x 15 mm box whose local origin is at its corner, placed by the build item at (100, 90).
MODEL_XML = """<?xml version="1.0" encoding="UTF-8"?>
<model unit="millimeter" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">
 <resources>
  <object id="1" type="model"><mesh><vertices>
   <vertex x="0" y="0" z="0"/><vertex x="20" y="0" z="0"/><vertex x="20" y="30" z="0"/><vertex x="0" y="30" z="0"/>
   <vertex x="0" y="0" z="15"/><vertex x="20" y="0" z="15"/><vertex x="20" y="30" z="15"/><vertex x="0" y="30" z="15"/>
  </vertices><triangles><triangle v1="0" v2="1" v3="2"/></triangles></mesh></object>
  <object id="2" type="model"><components><component objectid="1" transform="1 0 0 0 1 0 0 0 1 0 0 0"/></components></object>
 </resources>
 <build><item objectid="2" transform="1 0 0 0 1 0 0 0 1 100 90 0"/></build>
</model>"""


def plate_3mf(layer_height="0.3") -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr("3D/3dmodel.model", MODEL_XML)
        z.writestr("Metadata/project_settings.config",
                   json.dumps({"layer_height": layer_height, "initial_layer_print_height": "0.3",
                               "nozzle_temperature": ["215"], "hot_plate_temp": ["55"]}))
    return buf.getvalue()


def binary_stl() -> bytes:
    tri = struct.pack("<12fH", 0, 0, 1, 0, 0, 0, 10, 0, 0, 0, 10, 5, 0)
    return b"\0" * 80 + struct.pack("<I", 1) + tri


def multipart(fields: dict, files: dict):
    boundary = "----ref" + uuid.uuid4().hex
    out = io.BytesIO()
    for k, v in fields.items():
        out.write(f'--{boundary}\r\nContent-Disposition: form-data; name="{k}"\r\n\r\n{v}\r\n'.encode())
    for k, (fname, data) in files.items():
        out.write(f'--{boundary}\r\nContent-Disposition: form-data; name="{k}"; filename="{fname}"\r\n'
                  f'Content-Type: application/octet-stream\r\n\r\n'.encode() + data + b"\r\n")
    out.write(f"--{boundary}--\r\n".encode())
    return out.getvalue(), f"multipart/form-data; boundary={boundary}"


def slicer_form(mesh=None, name="plate_1.3mf", **extra):
    """The fields the slicer sends with /slice and /prepare (SLICE_SERVICE_PROTOCOL.md section 4)."""
    fields = {"settings_from_mesh": "1", "plate": "1", "machine_axes": "3",
              "printer": "My Printer 0.4 nozzle", "process": "0.20mm Standard", "filament": "Generic PLA"}
    fields.update(extra)
    return multipart(fields, {"mesh": (name, plate_3mf() if mesh is None else mesh)})


class ServerCase(unittest.TestCase):
    server_args: dict = {}

    @classmethod
    def setUpClass(cls):
        cls.srv = server.make_server("127.0.0.1", 0, stage_seconds=0.05, **cls.server_args)
        cls.thread = threading.Thread(target=cls.srv.serve_forever, daemon=True)
        cls.thread.start()
        cls.root = f"http://127.0.0.1:{cls.srv.server_address[1]}"

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()
        cls.srv.server_close()

    def call(self, method, path, body=None, ctype=None, headers=None):
        req = Request(self.root + "/" + path.lstrip("/"), data=body, method=method)
        if ctype:
            req.add_header("Content-Type", ctype)
        for k, v in (headers or {}).items():
            req.add_header(k, v)
        try:
            with urlopen(req, timeout=10) as r:
                return r.status, dict(r.headers), r.read()
        except HTTPError as e:
            return e.code, dict(e.headers), e.read()

    def json_call(self, method, path, body=None, ctype=None, headers=None):
        status, _, data = self.call(method, path, body, ctype, headers)
        return status, json.loads(data) if data else None

    def submit(self, **form):
        body, ctype = slicer_form(**form)
        return self.json_call("POST", "slice", body, ctype)

    def wait(self, job_id, timeout=10):
        seen = []
        deadline = time.time() + timeout
        while time.time() < deadline:
            status, job = self.json_call("GET", f"jobs/{job_id}")
            self.assertEqual(status, 200)
            seen.append(job)
            if job["status"] in ("succeeded", "failed"):
                return job, seen
            time.sleep(0.02)
        self.fail("job did not finish")


class ProtocolTest(ServerCase):
    def test_discovery(self):
        status, d = self.json_call("GET", "")
        self.assertEqual(status, 200)
        self.assertEqual(d["protocol"], 2)
        self.assertTrue(d["display_name"])
        self.assertTrue(all({"id", "label"} <= set(m) for m in d["modes"]))
        modes = {m["id"]: m for m in d["modes"]}
        self.assertEqual(modes["outline_double"]["options"], {"loops": 2})            # fixed values
        self.assertEqual(modes["outline_alternate"]["show_if"], {"machine_axes": 5})  # machine facts only

    def test_ui_schema_has_show_if_and_default_by(self):
        status, s = self.json_call("GET", "ui-schema")
        self.assertEqual(status, 200)
        fields = {f["dest"]: f for f in s["fields"]}
        self.assertTrue(all(f["type"] in ("enum", "bool", "number") for f in s["fields"]))
        self.assertEqual(fields["outline_gap_mm"]["show_if"], {"loops": [2, 3]})
        self.assertEqual(fields["direction"]["choices"][2]["show_if"], {"machine_axes": 5})
        self.assertEqual(fields["direction"]["default_by"], {"machine_axes": {"3": "ccw", "5": "alternate"}})

    def test_slice_poll_download(self):
        status, sub = self.submit(options=json.dumps({"offset_mm": 5}))
        self.assertEqual(status, 202)
        self.assertEqual(sub["status"], "queued")
        job, seen = self.wait(sub["job_id"])
        self.assertEqual(job["status"], "succeeded", job)
        # progress and stages while running: exactly one stage active, progress within 0..1
        running = [j for j in seen if j["status"] in ("queued", "running")]
        self.assertTrue(running)
        for j in running:
            self.assertTrue(0 <= j["progress"] <= 1)
            self.assertLessEqual(sum(1 for st in j["stages"] if st["state"] == "active"), 1)
        progress = [j["progress"] for j in running]
        self.assertEqual(progress, sorted(progress))
        self.assertTrue(job["warnings"])

        status, headers, data = self.call("GET", f"jobs/{sub['job_id']}/gcode")
        self.assertEqual(status, 200)
        self.assertEqual(int(headers["Content-Length"]), len(data))
        self.assertEqual(hashlib.sha256(data).hexdigest(), job["result"]["sha256"])
        self.assertEqual(job["result"]["size"], len(data))
        self.assertTrue(job["result"]["filename"].endswith(".gcode"))
        g = data.decode()
        # 0.3 mm layers up to the default 2 mm height, outline 5 mm outside X 100..120, Y 90..120
        self.assertIn("; layer_height = 0.3", g)
        self.assertEqual(g.count(";LAYER_CHANGE"), 6)
        xs = [float(x) for x in re.findall(r"^G[01] X([\d.]+)", g, re.M)]
        ys = [float(y) for y in re.findall(r"^G[01] X[\d.]+ Y([\d.]+)", g, re.M)]
        self.assertAlmostEqual(min(xs), 95.0, places=3)
        self.assertAlmostEqual(max(xs), 125.0, places=3)
        self.assertAlmostEqual(min(ys), 85.0, places=3)
        self.assertAlmostEqual(max(ys), 125.0, places=3)
        self.assertIn("M104 S215", g)

        status, headers, head = self.call("HEAD", f"jobs/{sub['job_id']}/gcode")
        self.assertEqual(status, 200)
        self.assertEqual(head, b"")
        self.assertEqual(int(headers["Content-Length"]), len(data))

    def test_grant_is_one_use_and_relative(self):
        _, sub = self.submit()
        job_id = sub["job_id"]
        self.wait(job_id)
        status, grant = self.json_call("POST", f"jobs/{job_id}/grant", b"{}", "application/json")
        self.assertEqual(status, 200)
        self.assertTrue(grant["url"].startswith(f"jobs/{job_id}/"))  # the slicer's acceptance rule
        status, _, data = self.call("GET", grant["url"])
        self.assertEqual(status, 200)
        self.assertTrue(data.startswith(b"; generated by the reference slicing service"))
        status, _, again = self.call("GET", grant["url"])
        self.assertEqual(status, 403)
        self.assertEqual(json.loads(again)["error"]["code"], "grant_used")

    def test_grant_before_result_is_refused(self):
        _, sub = self.submit()
        status, _ = self.json_call("POST", f"jobs/{sub['job_id']}/grant", b"{}", "application/json")
        self.assertIn(status, (200, 409))  # 200 only if it already finished

    def test_cancel(self):
        self.srv.service.stage_seconds = 2.0
        try:
            _, sub = self.submit()
            status, c = self.json_call("POST", f"jobs/{sub['job_id']}/cancel", b"{}", "application/json")
            self.assertEqual(status, 200)
            job, _ = self.wait(sub["job_id"])
            self.assertEqual(job["status"], "failed")
            self.assertTrue(job["error"])
        finally:
            self.srv.service.stage_seconds = 0.05
        status, _ = self.json_call("POST", "jobs/nope/cancel", b"{}", "application/json")
        self.assertEqual(status, 404)  # the slicer ignores this

    def test_prepare(self):
        body, ctype = slicer_form(mesh=plate_3mf("0.25"))
        status, p = self.json_call("POST", "prepare", body, ctype)
        self.assertEqual((status, p["status"]), (200, "queued"))
        status, p = self.json_call("POST", "prepare", body, ctype)
        self.assertEqual((status, p["status"]), (200, "skipped"))

    def test_mode_machine_axes_and_default_by(self):
        _, sub = self.submit(mode="outline_double", machine_axes="5",
                             options=json.dumps({"outline_gap_mm": 4}))
        job, _ = self.wait(sub["job_id"])
        _, _, data = self.call("GET", f"jobs/{sub['job_id']}/gcode")
        g = data.decode()
        self.assertIn("; machine_axes = 5", g)
        self.assertIn("; mode = outline_double", g)
        xs = [float(x) for x in re.findall(r"^G[01] X([\d.]+)", g, re.M)]
        self.assertAlmostEqual(min(xs), 100 - 3 - 4, places=3)  # second loop 4 mm further out

    def test_mode_show_if_and_fixed_options(self):
        status, e = self.submit(mode="outline_alternate", machine_axes="3")
        self.assertEqual((status, e["error"]["code"]), (400, "mode_not_offered"))
        status, sub = self.submit(mode="outline_alternate", machine_axes="5",
                                  options=json.dumps({"direction": "cw"}))  # the mode's value wins
        self.assertEqual(status, 202)
        self.wait(sub["job_id"])
        _, _, data = self.call("GET", f"jobs/{sub['job_id']}/gcode")
        moves = [l for l in data.decode().split(chr(10)) if l.startswith("G0 X")]
        self.assertNotEqual(moves[0], moves[1])  # alternate: each layer starts at the other end
        _, sub = self.submit()  # no mode field: the first mode
        self.wait(sub["job_id"])
        _, _, data = self.call("GET", f"jobs/{sub['job_id']}/gcode")
        self.assertIn("; mode = outline", data.decode())

    def test_stl_upload(self):
        _, sub = self.submit(mesh=binary_stl(), name="part.stl")
        job, _ = self.wait(sub["job_id"])
        self.assertEqual(job["status"], "succeeded")

    def test_bad_requests_use_the_error_shape(self):
        body, ctype = multipart({"plate": "1"}, {})
        status, e = self.json_call("POST", "slice", body, ctype)
        self.assertEqual(status, 400)
        self.assertEqual(e["error"]["code"], "no_mesh")
        status, e = self.submit(options="[1, 2]")
        self.assertEqual((status, e["error"]["code"]), (400, "bad_options"))
        status, e = self.submit(mode="nonsense")
        self.assertEqual((status, e["error"]["code"]), (400, "unknown_mode"))
        status, e = self.json_call("GET", "jobs/does-not-exist")
        self.assertEqual(status, 404)
        r = server.read_refusal(status, json.dumps(e))
        self.assertFalse(r["refusal"])
        self.assertEqual(r["message"], "No such job.")


class GcodeThreeMfTest(ServerCase):
    server_args = {"as_3mf": True, "require_grant": True}

    def test_gcode_3mf_result_and_required_grant(self):
        _, sub = self.submit()
        job, _ = self.wait(sub["job_id"])
        self.assertTrue(job["result"]["filename"].endswith(".gcode.3mf"))
        status, _, _ = self.call("GET", f"jobs/{sub['job_id']}/gcode")
        self.assertEqual(status, 403)  # the slicer then reports the refusal
        _, grant = self.json_call("POST", f"jobs/{sub['job_id']}/grant", b"{}", "application/json")
        status, _, data = self.call("GET", grant["url"])
        self.assertEqual(status, 200)
        self.assertEqual(hashlib.sha256(data).hexdigest(), job["result"]["sha256"])
        with zipfile.ZipFile(io.BytesIO(data)) as z:  # what the slicer extracts
            self.assertIn(b";LAYER_CHANGE", z.read("Metadata/plate_1.gcode"))


class RefusalTest(ServerCase):
    server_args = {"refuse": True}

    def test_refuse_is_canonical_402(self):
        for endpoint in ("slice", "prepare"):
            body, ctype = slicer_form()
            status, _, data = self.call("POST", endpoint, body, ctype)
            self.assertEqual(status, 402)
            err = json.loads(data)["error"]
            self.assertEqual(set(err), {"code", "message", "action_url", "action_label"})
            r = server.read_refusal(status, data, self.root)
            self.assertTrue(r["refusal"])
            self.assertEqual(r["message"], err["message"])
            self.assertEqual(r["action_url"], "https://example.com/plans")
            self.assertEqual(r["action_label"], "See plans")
        # discovery and the form stay readable, so the dropdown still shows the service
        self.assertEqual(self.json_call("GET", "")[0], 200)
        self.assertEqual(self.json_call("GET", "ui-schema")[0], 200)


class DetailRefusalTest(ServerCase):
    server_args = {"refuse": True, "refuse_style": "detail", "refuse_status": 403}

    def test_detail_body_is_a_valid_refusal(self):
        body, ctype = slicer_form()
        status, _, data = self.call("POST", "slice", body, ctype)
        self.assertEqual(status, 403)
        self.assertEqual(set(json.loads(data)), {"detail"})  # FastAPI's default shape
        r = server.read_refusal(status, data)
        self.assertTrue(r["refusal"])
        self.assertEqual(r["message"], "This slicing service is not included in your plan.")
        self.assertEqual(r["action_url"], "")  # no button: the message is still shown


class AuthTest(ServerCase):
    server_args = {"token": "s3cret"}

    def test_bearer_required(self):
        status, _, data = self.call("GET", "ui-schema")
        self.assertEqual(status, 401)
        r = server.read_refusal(status, data)
        self.assertTrue(r["refusal"])
        self.assertEqual(r["code"], "sign_in_required")
        status, _ = self.json_call("GET", "ui-schema", headers={"Authorization": "Bearer s3cret"})
        self.assertEqual(status, 200)
        self.assertEqual(self.json_call("GET", "")[0], 200)  # discovery is public


class RefusalParsingTest(unittest.TestCase):
    """Every refusal shape SLICE_SERVICE_PROTOCOL.md section 5 says a client accepts."""

    def test_canonical(self):
        r = server.read_refusal(402, json.dumps(server.refusal_body(
            "canonical", "plan_required", "Not in your plan.", "/plans", "See plans")), "https://svc.example")
        self.assertEqual((r["code"], r["message"], r["action_url"], r["action_label"]),
                         ("plan_required", "Not in your plan.", "https://svc.example/plans", "See plans"))

    def test_string_error_and_upgrade_url(self):
        r = server.read_refusal(403, '{"error": "Upgrade to slice.", "upgradeUrl": "/pro"}', "https://site.example")
        self.assertEqual((r["message"], r["action_url"]), ("Upgrade to slice.", "https://site.example/pro"))

    def test_fastapi_detail(self):
        r = server.read_refusal(401, '{"detail": "Not authenticated"}')
        self.assertEqual((r["refusal"], r["message"]), (True, "Not authenticated"))
        r = server.read_refusal(422, '{"detail": [{"loc": ["body", "mesh"], "msg": "field required"}]}')
        self.assertFalse(r["refusal"])
        self.assertIn("field required", r["message"])

    def test_raw_body_and_unsafe_url(self):
        r = server.read_refusal(402, "Payment required")
        self.assertEqual(r["message"], "Payment required")
        r = server.read_refusal(402, '{"error": {"message": "x", "action_url": "javascript:alert(1)"}}')
        self.assertEqual(r["action_url"], "")  # only http(s) is ever opened


if __name__ == "__main__":
    unittest.main()
