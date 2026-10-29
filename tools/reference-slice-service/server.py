#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Unlayered 3D. See LICENSE in this directory.
"""Reference slicing service for SLICE_SERVICE_PROTOCOL.md (protocol version 2).

A complete, dependency-free implementation of every endpoint the slicer uses, written to be read
top to bottom and copied. It does not slice anything worth printing: the "result" is a short,
valid planar G-code outline traced around the plate's bounding box, at the plate's own layer
height, so the whole round trip (discovery, options form, prepare, submit, progress, cancel, grant,
download, .gcode.3mf) can be exercised against a real slicer.

    python server.py                       # http://127.0.0.1:8000
    python server.py --port 9000 --stage-seconds 2
    python server.py --refuse              # every /slice and /prepare answers 402 (refusal format)
    python server.py --token s3cret        # require "Authorization: Bearer s3cret" (401 otherwise)
    python server.py --gcode-3mf           # hand results back as .gcode.3mf

Then set Preferences > Advanced > Web apps & services > Slicing service URL to
http://127.0.0.1:8000 (plain http is accepted for loopback only).

Python 3.8+ standard library only: http.server, threading, zipfile, xml.etree, email (multipart).
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import math
import re
import secrets
import struct
import threading
import time
import uuid
import zipfile
import xml.etree.ElementTree as ET
from email.parser import BytesParser
from email.policy import HTTP
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit, parse_qs, urljoin

PROTOCOL = 2
MAX_UPLOAD = 256 * 1024 * 1024

# GET /. Each mode is one Slice-dropdown entry; the first one offered is the default. A mode may
# carry `show_if` (machine facts only: offered on five-axis machines only, here) and `options`
# (field values the mode fixes: hidden in the options dialog and always sent with it).
DISCOVERY = {
    "display_name": "Reference Slicing",
    "protocol": PROTOCOL,
    "modes": [
        {"id": "outline", "label": "Outline (reference service)"},
        {"id": "outline_double", "label": "Double outline (reference service)", "options": {"loops": 2}},
        {"id": "outline_alternate", "label": "Alternating outline (reference service)",
         "show_if": {"machine_axes": 5}, "options": {"direction": "alternate"}},
    ],
}

# The options form: one `show_if` on another field (the gap only matters with two or more
# loops), a choice offered only on five-axis machines, and one `default_by` (the loop direction
# depends on the machine's axis count) -- the whole vocabulary, in miniature.
UI_SCHEMA = {
    "title": "Reference Slicing",
    "version": 1,
    "fields": [
        {"dest": "offset_mm", "label": "Distance from the part", "type": "number", "unit": "mm",
         "min": 1, "max": 30, "step": 0.5, "default": 3,
         "help": "How far outside the plate's bounding box the outline runs."},
        {"dest": "loops", "label": "Outlines", "type": "number", "min": 1, "max": 3, "step": 1, "default": 1,
         "help": "How many outlines, one outside the other."},
        {"dest": "outline_gap_mm", "label": "Gap between the outlines", "type": "number", "unit": "mm",
         "min": 0.5, "max": 10, "step": 0.5, "default": 2,
         "show_if": {"loops": [2, 3]},
         "help": "Only with two or more outlines."},
        {"dest": "direction", "label": "Direction", "type": "enum",
         "choices": [{"value": "ccw", "label": "Counter-clockwise"},
                     {"value": "cw", "label": "Clockwise"},
                     {"value": "alternate", "label": "Alternate per layer", "show_if": {"machine_axes": 5}}],
         "default": "ccw",
         "default_by": {"machine_axes": {"3": "ccw", "5": "alternate"}},
         "help": "Alternating is offered on five-axis machines only."},
        {"dest": "max_height_mm", "label": "Height", "type": "number", "unit": "mm",
         "min": 0.2, "max": 20, "default": 2, "help": "How tall the outline is printed."},
        {"dest": "fan", "label": "Part cooling fan", "type": "bool", "default": True},
    ],
}


def mode_entry(mode_id: str) -> dict | None:
    return next((m for m in DISCOVERY["modes"] if m["id"] == mode_id), None)


STAGES = [("read", "Reading the plate"), ("plan", "Planning the outline"), ("write", "Writing G-code")]


# ------------------------------------------------------------------------------------------------
# Refusals (SLICE_SERVICE_PROTOCOL.md section 5)
# ------------------------------------------------------------------------------------------------

def refusal_body(style: str, code: str, message: str, action_url: str | None = None,
                 action_label: str | None = None) -> dict:
    """The body of a 401/402/403. `canonical` is the documented form; the others are the looser
    shapes clients also accept, offered so a client can be tested against them."""
    if style == "string":
        body = {"error": message}
        if action_url:
            body["upgradeUrl"] = action_url
        return body
    if style == "detail":
        return {"detail": message}
    err = {"code": code, "message": message}
    if action_url:
        err["action_url"] = action_url
    if action_label:
        err["action_label"] = action_label
    return {"error": err}


def read_refusal(status: int, body: bytes | str, responding_origin: str = "") -> dict | None:
    """How a client reads an error body, exactly as section 5 describes it. Returns
    {refusal, code, message, action_url, action_label}; `refusal` is True for 401/402/403.
    A reference for client authors -- the tests use it to check every accepted shape."""
    if status < 400:
        return None
    text = body.decode("utf-8", "replace") if isinstance(body, bytes) else body
    out = {"refusal": status in (401, 402, 403), "code": "", "message": "",
           "action_url": "", "action_label": ""}
    try:
        j = json.loads(text)
    except ValueError:
        j = None
    if isinstance(j, dict):
        err = j.get("error")
        if isinstance(err, dict):
            out["code"] = str(err.get("code", ""))
            out["message"] = str(err.get("message", ""))
            out["action_url"] = str(err.get("action_url", "") or "")
            out["action_label"] = str(err.get("action_label", "") or "")
        elif isinstance(err, str):
            out["message"] = err
        elif "detail" in j:
            d = j["detail"]
            out["message"] = d if isinstance(d, str) else json.dumps(d)
        if not out["action_url"] and isinstance(j.get("upgradeUrl"), str):
            out["action_url"] = j["upgradeUrl"]
    if not out["message"]:
        out["message"] = text.strip()[:500]
    url = out["action_url"]
    if url and url.startswith("/") and responding_origin:
        url = urljoin(responding_origin, url)
    out["action_url"] = url if re.match(r"(?i)^https?://", url) else ""
    return out


# ------------------------------------------------------------------------------------------------
# Reading the plate: a 3MF (the slicer's courier project) or an STL
# ------------------------------------------------------------------------------------------------

def _local(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def _matrix(text: str | None):
    """A 3MF transform "m00 m01 m02 m10 ... m32" as a function on points."""
    if not text:
        return lambda p: p
    v = [float(x) for x in text.split()]
    if len(v) != 12:
        return lambda p: p
    return lambda p: (p[0] * v[0] + p[1] * v[3] + p[2] * v[6] + v[9],
                      p[0] * v[1] + p[1] * v[4] + p[2] * v[7] + v[10],
                      p[0] * v[2] + p[1] * v[5] + p[2] * v[8] + v[11])


def _corners(b):
    (x0, y0, z0), (x1, y1, z1) = b
    return [(x, y, z) for x in (x0, x1) for y in (y0, y1) for z in (z0, z1)]


def _union(a, b):
    if a is None:
        return b
    if b is None:
        return a
    return (tuple(min(a[0][i], b[0][i]) for i in range(3)), tuple(max(a[1][i], b[1][i]) for i in range(3)))


def _bbox_of(points):
    pts = list(points)
    if not pts:
        return None
    return (tuple(min(p[i] for p in pts) for i in range(3)), tuple(max(p[i] for p in pts) for i in range(3)))


def read_3mf(data: bytes):
    """(bounding box of the build in plate mm, project settings dict). The box is conservative:
    each object's local box is transformed, not each vertex, which is plenty for an outline."""
    zf = zipfile.ZipFile(io.BytesIO(data))
    objects = {}   # (model path, id) -> {"box": local bbox or None, "components": [(path, id, transform)]}
    build = []     # (path, objectid, transform) of the root model's build items
    models = [n for n in zf.namelist() if n.lower().endswith(".model")]
    root_model = next((n for n in models if n.lower() == "3d/3dmodel.model"), models[0] if models else None)
    for name in models:
        tree = ET.parse(zf.open(name))
        for obj in tree.iter():
            if _local(obj.tag) != "object":
                continue
            key = ("/" + name, obj.get("id"))
            verts = [(float(v.get("x")), float(v.get("y")), float(v.get("z")))
                     for v in obj.iter() if _local(v.tag) == "vertex"]
            comps = []
            for c in obj.iter():
                if _local(c.tag) == "component":
                    path = next((v for k, v in c.attrib.items() if _local(k) == "path"), None) or "/" + name
                    comps.append((path, c.get("objectid"), c.get("transform")))
            objects[key] = {"box": _bbox_of(verts), "components": comps}
        for item in tree.iter():
            if _local(item.tag) == "item":
                path = next((v for k, v in item.attrib.items() if _local(k) == "path"), None) or "/" + name
                build.append((name, path, item.get("objectid"), item.get("transform")))

    def box_of(key, depth=0):
        o = objects.get(key)
        if o is None or depth > 16:
            return None
        b = o["box"]
        for path, oid, tf in o["components"]:
            cb = box_of((path, oid), depth + 1)
            if cb is not None:
                b = _union(b, _bbox_of(map(_matrix(tf), _corners(cb))))
        return b

    box = None
    for model, path, oid, tf in build:
        if root_model and model != root_model:
            continue
        b = box_of((path, oid))
        if b is not None:
            box = _union(box, _bbox_of(map(_matrix(tf), _corners(b))))

    settings = {}
    for name in zf.namelist():
        if name.lower() == "metadata/project_settings.config":
            try:
                settings = json.loads(zf.read(name).decode("utf-8"))
            except ValueError:
                settings = {}
    return box, settings


def read_stl(data: bytes):
    n = struct.unpack("<I", data[80:84])[0] if len(data) >= 84 else -1
    if n >= 0 and 84 + 50 * n == len(data):  # binary STL
        pts = []
        for i in range(n):
            off = 84 + 50 * i + 12
            vals = struct.unpack("<9f", data[off:off + 36])
            pts += [vals[0:3], vals[3:6], vals[6:9]]
        return _bbox_of(pts), {}
    pts = [tuple(float(x) for x in m.groups())
           for m in re.finditer(rb"vertex\s+(\S+)\s+(\S+)\s+(\S+)", data)]
    return _bbox_of(pts), {}


def read_plate(filename: str, data: bytes):
    if data[:2] == b"PK" or filename.lower().endswith(".3mf"):
        return read_3mf(data)
    return read_stl(data)


def first_number(value, default: float) -> float:
    """A project_settings.config value: a string, or a list of strings (per extruder)."""
    if isinstance(value, list):
        value = value[0] if value else None
    try:
        return float(str(value).rstrip("%"))
    except (TypeError, ValueError):
        return default


# ------------------------------------------------------------------------------------------------
# The "slicer": an outline around the plate
# ------------------------------------------------------------------------------------------------

def outline_gcode(box, settings: dict, options: dict, mode: str, machine_axes: int, job_id: str) -> str:
    (x0, y0, _z0), (x1, y1, z1) = box
    layer = max(0.05, first_number(settings.get("layer_height"), 0.2))
    first = max(0.05, first_number(settings.get("initial_layer_print_height"), layer))
    nozzle_temp = first_number(settings.get("nozzle_temperature"), 210)
    bed_temp = first_number(settings.get("hot_plate_temp"), 60)
    filament_d = first_number(settings.get("filament_diameter"), 1.75)
    offset = float(options.get("offset_mm", 3))
    gap = float(options.get("outline_gap_mm", 2))
    loops = max(1, min(3, int(options.get("loops", 1))))
    direction = options.get("direction", "alternate" if machine_axes == 5 else "ccw")
    height = min(float(options.get("max_height_mm", 2)), max(z1, first))
    fan = bool(options.get("fan", True))
    width = 0.45
    area = math.pi * (filament_d / 2) ** 2

    out = [
        f"; generated by the reference slicing service (SLICE_SERVICE_PROTOCOL.md v{PROTOCOL}), job {job_id}",
        "; a placeholder print: an outline around the plate's bounding box, not the part itself",
        f"; bounding box X {x0:.2f}..{x1:.2f} Y {y0:.2f}..{y1:.2f} Z 0..{z1:.2f}",
        f"; layer_height = {layer}", f"; machine_axes = {machine_axes}", f"; mode = {mode}",
        "G21 ; mm", "G90 ; absolute XYZ", "M83 ; relative E",
        f"M140 S{bed_temp:.0f}", f"M104 S{nozzle_temp:.0f}", f"M190 S{bed_temp:.0f}", f"M109 S{nozzle_temp:.0f}",
        "G28", "G92 E0",
    ]
    z, n = first, 0
    while z <= height + 1e-6:
        h = first if n == 0 else layer
        e_per_mm = h * width / area
        out += [";LAYER_CHANGE", f";Z:{z:.3f}", f";HEIGHT:{h:.3f}", f"G1 Z{z:.3f} F600", ";TYPE:Skirt",
                f";WIDTH:{width}"]
        if n == 1 and fan:
            out.append("M106 S255")
        ccw = direction == "ccw" or (direction == "alternate" and n % 2 == 0)
        for loop in range(loops):
            d = offset + loop * gap
            pts = [(x0 - d, y0 - d), (x1 + d, y0 - d), (x1 + d, y1 + d), (x0 - d, y1 + d)]
            if not ccw:
                pts.reverse()
            out.append(f"G0 X{pts[0][0]:.3f} Y{pts[0][1]:.3f} F6000")
            for a, b in zip(pts, pts[1:] + pts[:1]):
                length = math.hypot(b[0] - a[0], b[1] - a[1])
                out.append(f"G1 X{b[0]:.3f} Y{b[1]:.3f} E{length * e_per_mm:.5f} F1800")
        z = round(z + layer, 6)
        n += 1
    out += ["M107", "G1 E-1 F1800", f"G0 Z{min(height + 10, 250):.3f} F600", "M104 S0", "M140 S0", "M84",
            f"; layers = {n}", ""]
    return "\n".join(out)


def gcode_3mf(gcode: str) -> bytes:
    """A minimal sliced 3MF around the G-code: the slicer takes Metadata/plate_1.gcode out of it."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml",
                   '<?xml version="1.0" encoding="UTF-8"?>\n'
                   '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
                   '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
                   '<Default Extension="gcode" ContentType="text/x.gcode"/></Types>')
        z.writestr("Metadata/plate_1.gcode", gcode)
        z.writestr("Metadata/plate_1.gcode.md5", hashlib.md5(gcode.encode()).hexdigest().upper())
    return buf.getvalue()


# ------------------------------------------------------------------------------------------------
# Jobs
# ------------------------------------------------------------------------------------------------

class Job:
    def __init__(self, job_id: str):
        self.id = job_id
        self.status = "queued"
        self.progress = 0.0
        self.stage = -1
        self.error = ""
        self.warnings = ["This reference service prints only an outline around the plate."]
        self.result: bytes | None = None
        self.filename = ""
        self.cancelled = threading.Event()
        self.grants: set[str] = set()

    def view(self) -> dict:
        v = {"status": self.status, "warnings": self.warnings}
        if self.status in ("queued", "running"):
            v["progress"] = round(self.progress, 3)
            v["stages"] = [{"id": sid, "label": label,
                            "state": "done" if i < self.stage else "active" if i == self.stage else "pending"}
                           for i, (sid, label) in enumerate(STAGES)]
        if self.status == "failed":
            v["error"] = self.error
        if self.status == "succeeded" and self.result is not None:
            v["result"] = {"filename": self.filename, "size": len(self.result),
                           "sha256": hashlib.sha256(self.result).hexdigest()}
        return v


class Service:
    def __init__(self, stage_seconds=0.5, refuse=False, refuse_style="canonical", refuse_status=402,
                 token=None, as_3mf=False, require_grant=False):
        self.stage_seconds = stage_seconds
        self.refuse = refuse
        self.refuse_style = refuse_style
        self.refuse_status = refuse_status
        self.token = token
        self.as_3mf = as_3mf
        self.require_grant = require_grant
        self.jobs: dict[str, Job] = {}
        self.prepared: set[str] = set()
        self.lock = threading.Lock()

    def run(self, job: Job, box, settings, options, mode, axes):
        try:
            job.status = "running"
            for i, _ in enumerate(STAGES):
                job.stage = i
                steps = 5
                for s in range(steps):
                    if job.cancelled.is_set():
                        job.status, job.error = "failed", "Cancelled by the user."
                        return
                    time.sleep(self.stage_seconds / steps)
                    job.progress = (i + (s + 1) / steps) / len(STAGES)
            gcode = outline_gcode(box, settings, options, mode, axes, job.id)
            if self.as_3mf:
                job.result, job.filename = gcode_3mf(gcode), f"outline-{job.id[:8]}.gcode.3mf"
            else:
                job.result, job.filename = gcode.encode("utf-8"), f"outline-{job.id[:8]}.gcode"
            job.progress, job.status = 1.0, "succeeded"
        except Exception as e:  # a bug here must still end the job, never leave it running
            job.status, job.error = "failed", f"The reference service failed: {e}"


# ------------------------------------------------------------------------------------------------
# HTTP
# ------------------------------------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    server_version = "ReferenceSliceService/2"
    service: Service  # set on the class by make_server

    def log_message(self, fmt, *args):  # quiet unless asked
        if getattr(self.server, "verbose", False):
            super().log_message(fmt, *args)

    # -- responses --------------------------------------------------------------------------
    def send_json(self, status: int, body: dict):
        data = json.dumps(body).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(data)

    def send_error_json(self, status: int, code: str, message: str):
        self.send_json(status, {"error": {"code": code, "message": message}})

    def refuse(self, status: int, code: str, message: str, url=None, label=None):
        self.send_json(status, refusal_body(self.service.refuse_style, code, message, url, label))

    def authorised(self) -> bool:
        if not self.service.token:
            return True
        if self.headers.get("Authorization", "") == "Bearer " + self.service.token:
            return True
        self.refuse(401, "sign_in_required", "Sign in to use the reference slicing service.")
        return False

    # -- request body -----------------------------------------------------------------------
    def read_body(self) -> bytes:
        n = int(self.headers.get("Content-Length") or 0)
        if n > MAX_UPLOAD:
            raise ValueError("upload too large")
        return self.rfile.read(n) if n else b""

    def read_form(self) -> dict:
        """multipart/form-data -> {name: (filename or None, bytes)}, with the stdlib email parser."""
        ctype = self.headers.get("Content-Type", "")
        body = self.read_body()
        if not ctype.lower().startswith("multipart/form-data"):
            raise ValueError("expected multipart/form-data")
        msg = BytesParser(policy=HTTP).parsebytes(b"Content-Type: " + ctype.encode() + b"\r\n\r\n" + body)
        form = {}
        for part in msg.iter_parts():
            name = part.get_param("name", header="content-disposition")
            if name:
                form[name] = (part.get_filename(), part.get_payload(decode=True) or b"")
        return form

    def path_parts(self):
        u = urlsplit(self.path)
        return [p for p in u.path.split("/") if p], parse_qs(u.query)

    # -- routing ----------------------------------------------------------------------------
    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        parts, query = self.path_parts()
        if parts == []:
            return self.send_json(200, DISCOVERY)
        if not self.authorised():
            return
        if parts == ["ui-schema"]:
            return self.send_json(200, UI_SCHEMA)
        if len(parts) == 2 and parts[0] == "jobs":
            job = self.service.jobs.get(parts[1])
            return self.send_json(200, job.view()) if job else self.send_error_json(404, "no_job", "No such job.")
        if len(parts) == 3 and parts[0] == "jobs" and parts[2] == "gcode":
            return self.download(parts[1], (query.get("grant") or [""])[0])
        self.send_error_json(404, "not_found", "No such endpoint.")

    def do_POST(self):
        parts, _ = self.path_parts()
        if not self.authorised():
            return
        if parts in (["slice"], ["prepare"]):
            return self.submit(parts[0])
        if len(parts) == 3 and parts[0] == "jobs" and parts[2] in ("cancel", "grant"):
            self.read_body()
            job = self.service.jobs.get(parts[1])
            if job is None:
                return self.send_error_json(404, "no_job", "No such job.")
            if parts[2] == "cancel":
                job.cancelled.set()
                return self.send_json(200, {"status": "cancelling" if job.status in ("queued", "running") else job.status})
            if job.status != "succeeded":
                return self.send_error_json(409, "not_ready", "The job has no result yet.")
            grant = secrets.token_hex(16)
            job.grants.add(grant)
            return self.send_json(200, {"url": f"jobs/{job.id}/gcode?grant={grant}"})
        self.send_error_json(404, "not_found", "No such endpoint.")

    # -- endpoints --------------------------------------------------------------------------
    def submit(self, what: str):
        try:
            form = self.read_form()
        except ValueError as e:
            return self.send_error_json(400, "bad_request", f"The request could not be read: {e}.")
        if self.service.refuse:
            return self.refuse(self.service.refuse_status, "plan_required",
                               "This slicing service is not included in your plan.",
                               "https://example.com/plans", "See plans")
        if "mesh" not in form:
            return self.send_error_json(400, "no_mesh", "The request carries no plate (form field \"mesh\").")
        filename, mesh = form["mesh"]
        text = {k: v[1].decode("utf-8", "replace") for k, v in form.items() if k != "mesh" and v[0] is None}
        try:
            options = json.loads(text["options"]) if text.get("options") else {}
            if not isinstance(options, dict):
                raise ValueError("options must be a JSON object")
        except ValueError as e:
            return self.send_error_json(400, "bad_options", f"The options could not be read: {e}.")
        try:
            axes = int(text.get("machine_axes") or 3)
        except ValueError:
            axes = 3
        mode = text.get("mode") or DISCOVERY["modes"][0]["id"]  # no mode sent: the default one
        entry = mode_entry(mode)
        if entry is None:
            return self.send_error_json(400, "unknown_mode", f"This service has no mode \"{mode}\".")
        if str(entry.get("show_if", {}).get("machine_axes", axes)) != str(axes):
            return self.send_error_json(400, "mode_not_offered",
                                        f"The mode \"{mode}\" needs a {entry['show_if']['machine_axes']}-axis machine.")
        # What the mode fixes wins over the form (the slicer sends it anyway; a service checks).
        options = {**options, **entry.get("options", {})}
        try:
            box, settings = read_plate(filename or "", mesh)
        except Exception as e:
            return self.send_error_json(422, "unreadable_plate", f"The plate could not be read: {e}.")
        if box is None:
            return self.send_error_json(422, "empty_plate", "The plate holds no geometry.")

        if what == "prepare":
            key = hashlib.sha256(mesh).hexdigest()
            with self.service.lock:
                seen = key in self.service.prepared
                self.service.prepared.add(key)
            return self.send_json(200, {"status": "skipped" if seen else "queued"})

        job = Job(uuid.uuid4().hex)
        with self.service.lock:
            self.service.jobs[job.id] = job
        accepted = {"job_id": job.id, "status": "queued", "warnings": []}
        threading.Thread(target=self.service.run, args=(job, box, settings, options, mode, axes),
                         daemon=True).start()
        self.send_json(202, accepted)

    def download(self, job_id: str, grant: str):
        job = self.service.jobs.get(job_id)
        if job is None:
            return self.send_error_json(404, "no_job", "No such job.")
        if job.status != "succeeded" or job.result is None:
            return self.send_error_json(409, "not_ready", "The job has no result yet.")
        if grant:
            if grant not in job.grants:
                return self.refuse(403, "grant_used", "This download link has already been used.")
            if self.command != "HEAD":
                job.grants.discard(grant)  # one use
        elif self.service.require_grant:
            return self.refuse(403, "grant_required", "Ask for a download grant first.")
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream" if job.filename.endswith(".3mf") else "text/plain")
        self.send_header("Content-Length", str(len(job.result)))
        self.send_header("Content-Disposition", f'attachment; filename="{job.filename}"')
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(job.result)


def make_server(host="127.0.0.1", port=8000, **service_args) -> ThreadingHTTPServer:
    service = Service(**service_args)
    handler = type("BoundHandler", (Handler,), {"service": service})
    srv = ThreadingHTTPServer((host, port), handler)
    srv.daemon_threads = True
    srv.service = service
    return srv


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--stage-seconds", type=float, default=0.5, help="time each fake stage takes")
    ap.add_argument("--refuse", action="store_true", help="answer every /slice and /prepare with a refusal")
    ap.add_argument("--refuse-status", type=int, default=402, choices=(401, 402, 403))
    ap.add_argument("--refuse-style", default="canonical", choices=("canonical", "string", "detail"),
                    help="body shape of a refusal; canonical is the documented form")
    ap.add_argument("--token", help="require Authorization: Bearer <token>")
    ap.add_argument("--gcode-3mf", action="store_true", help="return results as .gcode.3mf")
    ap.add_argument("--require-grant", action="store_true", help="refuse downloads without a grant")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    srv = make_server(a.host, a.port, stage_seconds=a.stage_seconds, refuse=a.refuse,
                      refuse_style=a.refuse_style, refuse_status=a.refuse_status, token=a.token,
                      as_3mf=a.gcode_3mf, require_grant=a.require_grant)
    srv.verbose = a.verbose
    print(f"reference slicing service (protocol {PROTOCOL}) on http://{a.host}:{srv.server_address[1]}")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
