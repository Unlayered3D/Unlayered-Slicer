#!/usr/bin/env python3
"""Coverage gate for the automation action catalog (docs/HLSD/bridge-actions.md).

Scans the GUI sources for every user-reachable control -- menu and context-menu items, side
buttons, main-toolbar items, gizmos, gizmo buttons, Preferences rows, settings-tab buttons and
sidebar controls, object-list columns, height-range rows, Parameter Table columns and the plate
icons on the bed -- and checks that each one is accounted for in a per-domain manifest under
resources/actions/surface/<domain>.json:

    { "<file>::<kind>::<label|member>": {"action": "object.mirror"}
                                      | {"actions": ["object.delete", "plate.clear"]}
                                      | {"coveredBy": "slicer.setSettings"}
                                      | {"excluded": "<reason>"}
                                      | {"todo": true}, ... }

Fails (exit 1) when a scanned key is in no manifest, or an action id a manifest names is not in
the catalog snapshot. With --strict it also fails on any remaining "todo" and on stale keys (in a
manifest, no longer in the source).

    python tools/check_action_coverage.py                      # report + basic gate
    python tools/check_action_coverage.py --strict             # release gate
    python tools/check_action_coverage.py --init-manifests     # add new keys as todo (by owner)
    python tools/check_action_coverage.py --json report.json   # machine-readable report
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from collections import Counter, OrderedDict, defaultdict

EXCLUSION_REASONS = {
    "forbidden-security",   # credentials, tokens, consent, plugins, process exec, app lifecycle
    "os-dialog-replaced",   # an OS open dialog; the action takes a url instead
    "cosmetic-help",        # help, tips, about, tutorials, links
    "duplicate-alias",      # the same command reachable elsewhere under another key
    "sla-only",             # SLA printers only (not built for FFF)
    "hidden-dead",          # not reachable in this build (hidden gizmo, unused dialog)
    "deferred-printer",     # printer-interactive flows not automated yet
    "not-allowlisted",      # a harmless preference left out of the automation allowlist (D4)
}

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# --------------------------------------------------------------------------------------------
# Source preprocessing
# --------------------------------------------------------------------------------------------


def strip_comments(text: str) -> str:
    """Blank out // and /* */ comments, keeping string literals and every newline."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            # raw string R"( ... )"
            if c == '"' and i > 0 and text[i - 1] == 'R':
                m = re.match(r'"([^(\s]*)\(', text[i:])
                if m:
                    end = text.find(')' + m.group(1) + '"', i)
                    end = n if end < 0 else end + len(m.group(1)) + 2
                    out.append(text[i:end])
                    i = end
                    continue
            j = i + 1
            while j < n and text[j] != c:
                if text[j] == '\\':
                    j += 1
                elif text[j] == '\n':
                    break
                j += 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', text[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def call_args(text: str, open_paren: int) -> list[str]:
    """Top-level comma-separated arguments of the call whose '(' is at open_paren."""
    depth, i, start, args = 0, open_paren, open_paren + 1, []
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == '\\' else 1
            i = j
        elif c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
            if depth == 0:
                args.append(text[start:i])
                return [a.strip() for a in args]
        elif c == ',' and depth == 1:
            args.append(text[start:i])
            start = i + 1
        elif c == '<' and depth >= 1:
            pass
        i += 1
    return [a.strip() for a in args]


LIT = r'"((?:[^"\\]|\\.)*)"'
LABEL_RES = [
    re.compile(r'_L_CONTEXT\s*\(\s*' + LIT),
    re.compile(r'_u8L_CONTEXT\s*\(\s*' + LIT),
    re.compile(r'\b_u8L\s*\(\s*' + LIT),
    re.compile(r'\b_L\s*\(\s*' + LIT),
    re.compile(r'\b_\s*\(\s*L\s*\(\s*' + LIT),
    re.compile(r'\b_\s*\(\s*' + LIT),
    re.compile(r'\bL\s*\(\s*' + LIT),
]
DESC_RE = re.compile(r'm_desc(?:\.at\s*\(\s*|\s*\[\s*)' + LIT)


def labels_in(expr: str) -> list[str]:
    """Every translatable literal in expr, in source order (a ternary yields both branches)."""
    found = []
    for rx in LABEL_RES:
        for m in rx.finditer(expr):
            found.append((m.start(), m.group(1)))
    for m in DESC_RE.finditer(expr):
        found.append((m.start(), 'desc:' + m.group(1)))
    found.sort()
    # _L("x") also matches the bare L("x") pattern at a later offset: keep the first per literal end
    seen, out = set(), []
    for _, lab in found:
        if lab not in seen:
            seen.add(lab)
            out.append(lab)
    return out


def norm_label(label: str) -> str:
    label = label.replace('\\t', '\t').split('\t')[0]       # drop "\tCtrl+X" accelerators
    label = label.replace('&&', '\0').replace('&', '').replace('\0', '&')
    label = label.replace('...', '').replace('…', '').strip()
    return re.sub(r'\s+', ' ', label)


def norm_expr(expr: str, func: str = '') -> str:
    e = 'expr:' + re.sub(r'\s+', '', expr)[:60]
    return e + '@' + func if func else e


def line_of(text: str, pos: int) -> int:
    return text.count('\n', 0, pos) + 1


FUNC_LINE = re.compile(r'^[A-Za-z_][\w:<>,*& ]*?\b(\w+)\s*\(')


def enclosing_function(text: str, pos: int) -> str:
    """Name of the function whose unindented definition precedes pos; '' if none."""
    end = pos
    while end > 0:
        nl = text.rfind('\n', 0, end)
        line = text[nl + 1:end]
        end = nl
        if line[:1].isalpha() or line[:1] == '_':
            m = FUNC_LINE.match(line)
            if m and not line.rstrip().endswith(';') and m.group(1) not in ('if', 'for', 'while', 'switch', 'return'):
                return m.group(1)
        if nl < 0:
            break
    return ''


# --------------------------------------------------------------------------------------------
# Scanners. Each yields (key, file, line, kind, label, extra)
# --------------------------------------------------------------------------------------------

MENU_CALL = re.compile(r'(?<![\w:])(append_menu_item|append_menu_check_item|append_menu_radio_item|append_submenu)\s*\(')
SIDEBUTTON = re.compile(r'new\s+SideButton\s*\(')
TOOLBAR_ITEM = re.compile(r'\bitem\.name\s*=\s*"([^"]+)"')
GIZMO = re.compile(r'm_gizmos\.emplace_back\s*\(\s*new\s+(GLGizmo\w+)')
# Every row helper, including the bare label row and combo base the Theme section builds by hand.
# Calls with a plain variable title (the helpers calling each other) are skipped below.
PREF_ITEM = re.compile(r'(?<![\w:])(create_item_(?!title\b)\w+)\s*\(')
TRANSLATED_VAR = re.compile(r'\s*_(?:L|u8L)?\s*\(\s*\w+\s*\)\s*')
TAB_BUTTON = re.compile(r'add_scaled_button\s*\([^,]+,\s*&(m_\w+)')
SIDEBAR_BIND = re.compile(r'([A-Za-z_]\w*(?:->\w+)*)->Bind\s*\(\s*wxEVT_(BUTTON|COMBOBOX)')
# ImGui controls of the G-code preview (layer/moves sliders, legend, plate thumbnails). They are
# drawn every frame from data, so no call pattern finds them; each is listed with a source anchor
# (a literal that must still occur in the file) and is emitted as <file>::imgui::<label> only while
# the anchor is there, so a removed control turns into a stale manifest key under --strict.
IMGUI_PREVIEW_CONTROLS = {
    'IMSlider.cpp': [
        ('Layer slider', 'bool IMSlider::vertical_slider'),
        ('Jump to layer', 'render_go_to_layer_dialog'),
        ('One layer mode', 'switch_one_layer_mode()'),
        ('Layer time tooltip', 'ltEstimatedTime'),
        ('Add Pause', '"Add Pause"'),
        ('Add Custom G-code', '"Add Custom G-code"'),
        ('Add Custom Template', '"Add Custom Template"'),
        ('Change Filament', '"Change Filament"'),
        ('Delete tick', 'delete_tick'),
        ('Edit Custom G-code', '"Edit Custom G-code"'),
        ('Tick tooltip', 'show_tooltip'),
        ('Moves slider', 'bool IMSlider::horizontal_slider'),
    ],
    'GCodeViewer.cpp': [
        ('Tool position window', 'render_position_window'),
        ('Actual speed profile', 'Actual speed profile'),
        ('G-code window', 'GCodeWindow::render'),
        ('Legend fold', 'm_fold'),
        ('Color scheme combo', 'BBLBeginCombo'),
        ('Feature type rows', 'toggle_extrusion_role_visibility'),
        ('Option rows', 'toggle_option_visibility'),
        ('Section rows', 'toggle_cut_segment_visibility'),
        ('Colour range scale', 'append_range('),
        ('Filament table', 'get_used_filament_from_volume'),
        ('Show stealth mode', 'Show stealth mode'),
        ('Regroup filament', 'EVT_OPEN_FILAMENT_MAP_SETTINGS_DIALOG'),
        ('Set to Optimal', 'fmmAutoForFlush'),
        ('Wiki Guide', 'Wiki Guide'),
        ('Show raw machine G-code', 'Show raw machine G-code'),
        ('Statistics of All Plates', 'render_all_plates_stats'),
    ],
    'GLCanvas3D.cpp': [
        ('Sliced plate thumbnail', 'EVT_GLTOOLBAR_SELECT_SLICED_PLATE'),
        ('All plates stats', 'EVT_GLTOOLBAR_SLICE_ALL'),
    ],
}
GIZMO_BUTTON = re.compile(r'(m_imgui->button|ImGui::Button|ImGuiWrapper::button|imgui->button)\s*\(')
# Device tab (StatusPanel.cpp, Monitor.cpp): each control is a handler connected or bound to a
# widget, plus the temperature inputs, which call their on_set_*_temp from a lambda.
DEVICE_HANDLER = re.compile(r'(?:(?<!Dis)Connect\s*\([^;]*?Handler\s*\(\s*|\bBind\s*\([^;]*?&)'
                            r'(?:StatusPanel|StatusBasePanel|MonitorPanel|AddMachinePanel)::(on_\w+)')
DEVICE_TEMP_CALL = re.compile(r'(?<![:\w])(on_set_(?:bed|nozzle|chamber)_temp)\s*\(')
# Not controls: focus bookkeeping, web request / navigation state, size and timer, and the
# notifications the panel refreshes itself from.
DEVICE_NOT_CONTROL = re.compile(r'_(kill|set)_focus$|^on_(webrequest_state|webview_navigating|size|timer|fan_changed|ams_switch)$')


# Settings-tab, ParamsPanel and preset-dialog controls that no menu, toolbar or add_scaled_button
# call marks (presets audit, section 3.10). Each is named by a regex over the comment-stripped
# source and emitted as <file>::control::<label> only while that code is there, so a control that
# goes away turns stale in --strict.
PRESET_CONTROLS = {
    'Tab.cpp': [
        (r'\bm_mode_icon\s*->\s*Bind\b', 'settings mode switch'),
        (r'\bm_extruder_switch\b', 'extruder switch'),
        (r'\bm_variant_combo\b', 'variant combo'),
        (r'\bm_extruder_sync\s*->\s*Bind\b', 'copy to other extruder'),
        (r'\bm_detach_preset_btn\b', 'detach from parent'),
        (r'\bcompatible_widget_create\s*\(', 'compatible printers/prints'),
        (r'\bcreate_bed_shape_widget\s*\(', 'bed shape'),
        (r'Option\s+option\s*\(\s*def\s*,\s*"extruders_count"', 'extruders count'),
        (r'\bappend_browse_line\s*\(\s*"sparse_infill_svg_path"', 'browse sparse_infill_svg_path'),
        (r'\bappend_browse_line\s*\(\s*"infill_mesh_path"', 'browse infill_mesh_path'),
        (r'\bvoid\s+Tab::edit_custom_gcode\b', 'edit custom G-code'),
    ],
    'ParamsPanel.cpp': [
        (r'\bm_compare_btn\s*->\s*Bind\b', 'compare presets'),
        (r'\bm_mode_region\s*->\s*Bind\b', 'global/objects switch'),
        (r'\bm_setting_btn\s*->\s*Bind\b', 'object settings table'),
        (r'\bm_mode_icon\s*->\s*Bind\b', 'settings mode switch'),
        (r'\bm_search_btn\s*->\s*Bind\b', 'search'),
    ],
    'SavePresetDialog.cpp': [
        (r'\bm_input_ctrl\s*->\s*Bind\b', 'preset name'),
        (r'\bm_radio_group\s*->\s*Bind\b', 'user preset / project'),
        (r'\bdetach_checkbox\s*->\s*Bind\b', 'detach from parent'),
        (r'\bSavePresetDialog::update_physical_printers\b', 'physical printer'),
    ],
    'UnsavedChangesDialog.cpp': [
        (r'add_btn\s*\(\s*&m_save_btn\b', 'unsaved changes: save'),
        (r'add_btn\s*\(\s*&m_transfer_btn\b', 'unsaved changes: transfer'),
        (r'add_btn\s*\(\s*&m_discard_btn\b', 'unsaved changes: discard'),
        (r'\bm_cancel_btn\b', 'unsaved changes: cancel'),
        (r'\bm_show_all_presets\s*->\s*Bind\b', 'compare: show all presets'),
        (r'\bm_use_for_transfer\s*->\s*Bind\b', 'compare: transfer values'),
        (r'SimpleEvent\s*\(\s*EVT_DIFF_DIALOG_TRANSFER\b', 'compare: transfer'),
    ],
    'PresetBundleDialog.cpp': [
        (r'strCmd\s*==\s*"request_bundles"', 'list bundles'),
        (r'action\s*==\s*"delete_bundle"', 'delete bundle'),
        (r'action\s*==\s*"open_folder"', 'open folder'),
        (r'action\s*==\s*"unsubscribe_bundle"', 'unsubscribe'),
        (r'strCmd\s*==\s*"update_bundle"', 'update bundle'),
        (r'strCmd\s*==\s*"refresh_bundles"', 'refresh bundles'),
        (r'strCmd\s*==\s*"set_auto_update"', 'auto update'),
        (r'strCmd\s*==\s*"open_bundle_on_cloud"', 'open on cloud'),
        (r'strCmd\s*==\s*"export_page"', 'export bundle'),
    ],
    'ExportPresetBundleDialog.cpp': [
        (r'\bvoid\s+ExportPresetBundleDialog::OnExportData\b', 'export bundle'),
    ],
    'BedShapeDialog.cpp': [
        (r'\bvoid\s+BedShapePanel::update_shape\b', 'shape (rectangle/circle/custom)'),
        (r'\bvoid\s+BedShapePanel::load_stl\b', 'load shape from STL'),
        (r'\bvoid\s+BedShapePanel::load_texture\b', 'load texture'),
        (r'\bvoid\s+BedShapePanel::load_model\b', 'load model'),
    ],
    'CreatePresetsDialog.cpp': [
        (r'\bCreateFilamentPresetDialog::create_dialog_buttons\b', 'create filament'),
        (r'\bCreatePrinterPresetDialog::create_page2_dialog_buttons\b', 'create printer'),
        (r'\bm_button_bed_stl\s*->\s*Bind\b', 'create printer: bed model'),
        (r'\bm_button_bed_svg\s*->\s*Bind\b', 'create printer: bed texture'),
        (r'\bExportConfigsDialog::archive_printer_preset_to_file\b', 'export configs'),
        (r'\bEditFilamentPresetDialog::delete_preset\b', 'edit filament: delete'),
        (r'\bCreatePresetForPrinterDialog::create_dialog_buttons\b', 'edit filament: add for printer'),
        (r'\bCreatePresetSuccessfulDialog::CreatePresetSuccessfulDialog\b', 'created notice'),
    ],
}


def scan_file(rel: str, text: str):
    base = os.path.basename(rel)
    out = []

    def emit(kind, label, pos, extra=None):
        out.append((f'{base}::{kind}::{label}', rel, line_of(text, pos), kind, label, extra or {}))

    for m in MENU_CALL.finditer(text):
        # Skip the definitions in wxExtensions.cpp ("wxMenuItem* append_menu_item(wxMenu* menu, ...")
        prefix = text[max(0, m.start() - 16):m.start()]
        if re.search(r'wxMenuItem\s*\*\s*$', prefix):
            continue
        args = call_args(text, m.end() - 1)
        kind = 'submenu' if m.group(1) == 'append_submenu' else 'menu'
        label_arg = args[3] if kind == 'submenu' and len(args) > 3 else (args[2] if len(args) > 2 else '')
        label_arg = re.split(r'\+\s*"\\t"', label_arg)[0]   # drop the + "\t" + <accelerator> tail
        labs = labels_in(label_arg)
        menu_var = args[0] if args else ''
        if not labs:
            labs = [norm_expr(label_arg, enclosing_function(text, m.start()))]
        for lab in labs:
            emit(kind, norm_label(lab), m.start(), {'menu': menu_var})

    if base == 'MainFrame.cpp':
        for m in SIDEBUTTON.finditer(text):
            args = call_args(text, m.end() - 1)
            labs = labels_in(args[1]) if len(args) > 1 else []
            if not labs:
                if len(args) > 1 and args[1] == '""':
                    # an icon-only dropdown arrow next to a labelled button: name it by the member
                    lhs = re.search(r'(\w+)\s*=\s*$', text[max(0, m.start() - 60):m.start()])
                    labs = ['member:' + (lhs.group(1) if lhs else 'anonymous')]
                else:
                    labs = [norm_expr(args[1] if len(args) > 1 else '')]
            for lab in labs:
                emit('sidebutton', norm_label(lab), m.start())

    if base == 'GLCanvas3D.cpp':
        for m in TOOLBAR_ITEM.finditer(text):
            if 'seperator' in m.group(1) or 'separator' in m.group(1):
                continue
            emit('toolbar', m.group(1), m.start())

    if base == 'GLGizmosManager.cpp':
        for m in GIZMO.finditer(text):
            emit('gizmo', m.group(1), m.start())

    if base == 'Preferences.cpp':
        for m in PREF_ITEM.finditer(text):
            prefix = text[max(0, m.start() - 24):m.start()]
            if 'PreferencesDialog::' in prefix:
                continue
            args = call_args(text, m.end() - 1)
            labs = labels_in(args[0]) if args else []
            if not labs and args and TRANSLATED_VAR.fullmatch(args[0]):
                # rows built in a loop from a table of translatable names (Interface colours)
                labs = [norm_expr(args[0], enclosing_function(text, m.start()))]
            if not labs:
                continue  # internal helper call with a variable title
            emit('pref', norm_label(labs[0]), m.start(), {'helper': m.group(1)})

    if base == 'Tab.cpp':
        for m in TAB_BUTTON.finditer(text):
            emit('tabbutton', m.group(1), m.start())

    for anchor, label in PRESET_CONTROLS.get(base, ()):
        m = re.search(anchor, text)
        if m:
            emit('control', label, m.start())

    if base == 'Plater.cpp':
        for m in SIDEBAR_BIND.finditer(text):
            member = re.sub(r'^p->', '', m.group(1))
            emit('sidebar', member, m.start(), {'event': m.group(2)})

    # Direct interactions outside menus (the object list, its height ranges, the Parameter Table and
    # the plate icons on the bed), so the coverage gate sees them too.
    if base == 'GUI_ObjectList.cpp':
        # Every column of the object list is a control: a click, an in-place editor or a drag grip.
        seen = set()
        for m in re.finditer(r'\b(col[A-Z]\w*)\b', text):
            if m.group(1) != 'colCount' and m.group(1) not in seen:
                seen.add(m.group(1))
                emit('column', m.group(1), m.start())

    if base == 'GUI_ObjectLayers.cpp':
        # The height-range rows: min/max editors and the +/- buttons.
        for m in re.finditer(r'new\s+LayerRangeEditor\([^;]*?\b(et[A-Z]\w*)', text):
            emit('layerctl', m.group(1), m.start())
        for m in re.finditer(r'obj_list\(\)->((?:add|del)_layer_range\w*)\(', text):
            emit('layerctl', m.group(1), m.start())

    if base == 'GUI_ObjectTable.cpp':
        # The Parameter Table's columns: the members of ObjectGridTable::GridColType (header).
        hpp = os.path.join(ROOT, os.path.dirname(rel), 'GUI_ObjectTable.hpp')
        cols = []
        if os.path.exists(hpp):
            with open(hpp, encoding='utf-8', errors='replace') as fh:
                h = strip_comments(fh.read())
            e = re.search(r'enum\s+GridColType\s*\{([^}]*)\}', h)
            if e:
                cols = [c for c in re.findall(r'\b(col_\w+)\s*=', e.group(1)) if c != 'col_max']
        for c in cols:
            m = re.search(r'\b' + c + r'\b', text)
            if m:
                emit('tablecol', c, m.start())

    if base == 'Plater.cpp':
        # The icons on each plate on the bed (Plater::select_plate_by_hover_id, by hover action).
        start = text.find('int Plater::select_plate_by_hover_id')
        if start >= 0:
            end = text.find('\nint Plater::', start + 10)
            body = text[start:end if end > 0 else len(text)]
            for m in re.finditer(r'\baction\s*==\s*(\d+|PartPlate::\w+)', body):
                emit('bedicon', 'hover:' + m.group(1).replace('PartPlate::', ''), start + m.start())
    for label, anchor in IMGUI_PREVIEW_CONTROLS.get(base, []):
        pos = text.find(anchor)
        if pos >= 0:
            emit('imgui', label, pos)
    if base in ('StatusPanel.cpp', 'Monitor.cpp'):
        for rx in (DEVICE_HANDLER, DEVICE_TEMP_CALL):
            for m in rx.finditer(text):
                if not DEVICE_NOT_CONTROL.search(m.group(1)):
                    emit('device', m.group(1), m.start())

    if rel.replace('\\', '/').split('/')[-2] == 'Gizmos':
        for m in GIZMO_BUTTON.finditer(text):
            args = call_args(text, m.end() - 1)
            labs = labels_in(args[0]) if args else []
            if not labs:
                pure = re.fullmatch(r'\s*(?:u8)?' + LIT + r'\s*', args[0]) if args else None
                labs = [pure.group(1)] if pure else [norm_expr(args[0] if args else '', enclosing_function(text, m.start()))]
            for lab in labs:
                emit('gizmoctl', norm_label(lab), m.start())
    return out


def scan(src_dir: str):
    items = []
    for dirpath, _, files in os.walk(src_dir):
        for f in sorted(files):
            if not f.endswith('.cpp'):
                continue
            path = os.path.join(dirpath, f)
            rel = os.path.relpath(path, ROOT).replace('\\', '/')
            with open(path, encoding='utf-8', errors='replace') as fh:
                text = strip_comments(fh.read())
            items.extend(scan_file(rel, text))
    # Fold duplicates (platform #ifdef branches, the same label twice in one file).
    folded = OrderedDict()
    for key, rel, line, kind, label, extra in items:
        e = folded.setdefault(key, {'key': key, 'file': rel, 'lines': [], 'kind': kind, 'label': label, 'extra': extra})
        e['lines'].append(line)
    return folded


# --------------------------------------------------------------------------------------------
# Initial owner assignment (PLAN section 1); used only by --init-manifests for new keys.
# --------------------------------------------------------------------------------------------

def _any(label, *words):
    l = label.lower()
    return any(w in l for w in words)


GIZMO_DOMAIN = {
    'GLGizmoMove3D': 'object', 'GLGizmoRotate3D': 'object', 'GLGizmoScale3D': 'object', 'GLGizmoFlatten': 'object',
    'GLGizmoCut3D': 'part', 'GLGizmoMeshBoolean': 'part', 'GLGizmoCutChunks3D': 'part', 'GLGizmoRotateLayers3D': 'part',
    'GLGizmoSkeleton': 'part', 'GLGizmoEmboss': 'part', 'GLGizmoSVG': 'part', 'GLGizmoSimplify': 'part',
    'GLGizmoCutPlanes': 'core',
    'GLGizmoFdmSupports': 'paint', 'GLGizmoSeam': 'paint', 'GLGizmoFuzzySkin': 'paint', 'GLGizmoMmuSegmentation': 'paint',
    'GLGizmoBrimEars': 'paint', 'GLGizmoMeasure': 'paint', 'GLGizmoAssembly': 'paint',
}
GIZMO_FILE_DOMAIN = {
    'GLGizmoMove.cpp': 'object', 'GLGizmoRotate.cpp': 'object', 'GLGizmoScale.cpp': 'object', 'GLGizmoFlatten.cpp': 'object',
    'GLGizmoCut.cpp': 'part', 'GLGizmoAdvancedCut.cpp': 'part', 'GLGizmoMeshBoolean.cpp': 'part', 'GLGizmoCutChunks.cpp': 'part',
    'GLGizmoRotateLayers.cpp': 'part', 'GLGizmoSkeleton.cpp': 'part', 'GLGizmoEmboss.cpp': 'part', 'GLGizmoSVG.cpp': 'part',
    'GLGizmoText.cpp': 'part', 'GLGizmoSimplify.cpp': 'part', 'GLGizmoCutPlanes.cpp': 'core',
    'GLGizmoFdmSupports.cpp': 'paint', 'GLGizmoSeam.cpp': 'paint', 'GLGizmoFuzzySkin.cpp': 'paint',
    'GLGizmoMmuSegmentation.cpp': 'paint', 'GLGizmoBrimEars.cpp': 'paint', 'GLGizmoMeasure.cpp': 'paint',
    'GLGizmoAssembly.cpp': 'paint', 'GLGizmoPainterBase.cpp': 'paint',
}
TOOLBAR_DOMAIN = {
    'add': 'project', 'addplate': 'plate', 'orient': 'plate', 'arrange': 'plate', 'more': 'object', 'fewer': 'object',
    'splitobjects': 'object', 'splitvolumes': 'part', 'splitchunks': 'part', 'layersediting': 'objectSettings',
    'assembly_view': 'view',
}


def owner_of(item) -> str:
    base = os.path.basename(item['file'])
    kind, label = item['kind'], item['label']
    menu = item['extra'].get('menu', '')
    if kind == 'imgui':
        return 'preview'
    if kind == 'gizmo':
        return GIZMO_DOMAIN.get(label, 'part')
    if kind == 'gizmoctl':
        return GIZMO_FILE_DOMAIN.get(base, 'part')
    if kind == 'toolbar':
        return TOOLBAR_DOMAIN.get(label, 'view')
    if kind == 'pref':
        return 'prefs'
    if kind in ('tabbutton', 'control'):
        return 'preset'
    if kind == 'sidebar':
        return 'filament'
    if kind == 'device':
        return 'printer'
    if kind == 'sidebutton':
        return 'plate' if _any(label, 'slice all') else 'core'
    if kind == 'bedicon':
        return 'plate'
    if kind in ('column', 'tablecol', 'layerctl'):
        return 'object'
    if base == 'MainFrame.cpp':
        if menu in ('editMenu',):
            return 'core' if _any(label, 'undo', 'redo') else 'object'
        if menu in ('viewMenu', 'view_menu'):
            return 'view'
        if 'calib' in menu.lower():
            return 'calib'
        if menu == 'helpMenu':
            return 'app'
        if menu in ('fileMenu', 'import_menu', 'export_menu', 'publish_menu'):
            if _any(label, 'quit', 'exit', 'new window'):
                return 'app'
            return 'project'
        if _any(label, 'preference', 'plugin', 'sync', 'bundle', 'show agent', 'sidebar'):
            return 'app'
        if _any(label, 'calib', 'temperature', 'flow', 'pressure', 'retraction', 'cornering', 'input shaping', 'vfa'):
            return 'calib'
        if _any(label, 'undo', 'redo'):
            return 'core'
        return 'app'
    if base == 'GUI_AuxiliaryList.cpp':
        return 'project'
    if base == 'PresetComboBoxes.cpp':
        return 'filament' if _any(label, 'color', 'colour') else 'preset'
    if base == 'GUI_Factories.cpp':
        exact = {
            'Reload from disk': 'object', 'Reload All': 'plate', 'Auto Rotate': 'plate', 'Arrange': 'plate',
            'Add Models': 'plate', 'Select All': 'plate', 'Delete All': 'plate', 'Auto orientation': 'plate',
            'Decompose Color': 'filament', '(Lost color)': 'filament', 'Merge with': 'filament', 'Edit': 'filament',
            'Show Labels': 'view', 'Export as STLs': 'project', 'Export as one STL': 'project',
            'Export as DRCs': 'project', 'Export as one DRC': 'project', 'Fill bed with copies': 'object',
        }
        if label in exact:
            return exact[label]
        func = label.split('@')[1] if label.startswith('expr:') and '@' in label else ''
        if 'extruder' in func or 'filament' in func:
            return 'filament'
        if 'convert_unit' in func or 'auto_drop' in func:
            return 'object'
        if 'process' in func:
            return 'objectSettings'
        if 'setting' in func or 'categor' in func:
            return 'objectSettings'
        if 'plate' in func:
            return 'plate'
        if 'order' in func:
            return 'object'
        if menu in ('extruder_selection_menu',) or _any(label, 'filament', 'extruder'):
            return 'filament'
        if _any(label, 'process setting', 'settings', 'height range', 'layer', 'parameter table', 'flush'):
            if _any(label, 'copy', 'paste'):
                return 'object'
            return 'objectSettings'
        if menu == 'flush_options_menu':
            return 'objectSettings'
        if _any(label, 'plate', 'all plates', 'lock'):
            return 'plate'
        if _any(label, 'part', 'modifier', 'blocker', 'enforcer', 'cube', 'cylinder', 'sphere', 'cone', 'disc', 'torus',
                'text', 'svg', 'primitive', 'handy', 'change type', 'boolean', 'fix model', 'simplify', 'subdivision',
                'cut info', 'replace', 'load', 'infill mesh', 'negative', 'expr:'):
            return 'part'
        return 'object'
    return 'object'


# --------------------------------------------------------------------------------------------
# Manifests, catalog, report
# --------------------------------------------------------------------------------------------

def load_manifests(manifest_dir: str):
    entries, owner = {}, {}
    if not os.path.isdir(manifest_dir):
        return entries, owner
    for f in sorted(os.listdir(manifest_dir)):
        if not f.endswith('.json'):
            continue
        domain = f[:-5]
        with open(os.path.join(manifest_dir, f), encoding='utf-8') as fh:
            data = json.load(fh)
        for k, v in data.items():
            if k.startswith('$'):
                continue
            if k in entries:
                raise SystemExit(f'error: {k} is in both {owner[k]}.json and {domain}.json')
            entries[k] = v
            owner[k] = domain
    return entries, owner


def load_catalog_ids(path: str):
    if not path or not os.path.exists(path):
        return None
    with open(path, encoding='utf-8') as fh:
        data = json.load(fh)
    actions = data.get('actions', data if isinstance(data, list) else [])
    ids = set()
    for a in actions:
        ids.add(a['id'])
        if a.get('bridgeMethod'):
            ids.add(a['bridgeMethod'])
    return ids


def entry_status(v) -> str:
    if not isinstance(v, dict):
        return 'invalid'
    if 'action' in v or 'actions' in v:
        return 'action'
    if 'coveredBy' in v:
        return 'coveredBy'
    if 'excluded' in v:
        return 'excluded'
    if v.get('todo'):
        return 'todo'
    return 'invalid'


def write_manifest(path: str, domain: str, data: dict):
    header = OrderedDict()
    header['$domain'] = domain
    header['$doc'] = ('Coverage manifest; see tools/check_action_coverage.py. Each key is <file>::<kind>::<label>; the '
                      'value is {"action"} | {"actions"} | {"coveredBy"} | {"excluded": reason} | {"todo": true}.')
    header.update(sorted(data.items()))
    # One entry per line: each manifest belongs to one work package, and line-per-key diffs merge.
    lines = [f'  {json.dumps(k, ensure_ascii=False)}: {json.dumps(v, ensure_ascii=False)}' for k, v in header.items()]
    with open(path, 'w', encoding='utf-8', newline='\n') as fh:
        fh.write('{\n' + ',\n'.join(lines) + '\n}\n')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--src', default=os.path.join(ROOT, 'src', 'slic3r', 'GUI'))
    ap.add_argument('--manifest', default=os.path.join(ROOT, 'resources', 'actions', 'surface'))
    ap.add_argument('--catalog', default=os.path.join(ROOT, 'resources', 'actions', 'catalog.snapshot.json'))
    ap.add_argument('--strict', action='store_true', help='also fail on todo entries and stale keys')
    ap.add_argument('--init-manifests', action='store_true', help='add unmapped keys as todo under their owner domain')
    ap.add_argument('--json', help='write a machine-readable report here')
    ap.add_argument('--list', choices=['todo', 'missing', 'stale', 'excluded'], help='print the keys in one bucket')
    ap.add_argument('--domain', help='with --list: only this manifest')
    args = ap.parse_args(argv)

    scanned = scan(args.src)
    entries, owner = load_manifests(args.manifest)

    if args.init_manifests:
        per_domain = defaultdict(dict)
        for k, v in entries.items():
            per_domain[owner[k]][k] = v
        added = Counter()
        for key, item in scanned.items():
            if key not in entries:
                d = owner_of(item)
                per_domain[d][key] = {'todo': True}
                added[d] += 1
        os.makedirs(args.manifest, exist_ok=True)
        for d, data in per_domain.items():
            write_manifest(os.path.join(args.manifest, d + '.json'), d, data)
        print('added todo keys:', dict(sorted(added.items())))
        entries, owner = load_manifests(args.manifest)

    catalog_ids = load_catalog_ids(args.catalog)

    missing = [k for k in scanned if k not in entries]
    stale = [k for k in entries if k not in scanned]
    invalid, unknown_actions, bad_reasons = [], [], []
    by_status = Counter()
    by_kind = defaultdict(Counter)
    by_reason = Counter()
    by_domain = defaultdict(Counter)
    for key, item in scanned.items():
        v = entries.get(key)
        st = 'missing' if v is None else entry_status(v)
        by_status[st] += 1
        by_kind[item['kind']][st] += 1
        if v is not None:
            by_domain[owner[key]][st] += 1
        if st == 'invalid':
            invalid.append(key)
        if st == 'excluded':
            by_reason[v['excluded']] += 1
            if v['excluded'] not in EXCLUSION_REASONS:
                bad_reasons.append((key, v['excluded']))
        if st == 'action' and catalog_ids is not None:
            ids = v.get('actions') or [v.get('action')]
            for a in ids:
                if a not in catalog_ids:
                    unknown_actions.append((key, a))
        if st == 'coveredBy' and catalog_ids is not None and v['coveredBy'] not in catalog_ids:
            unknown_actions.append((key, v['coveredBy']))

    total = len(scanned)
    covered = by_status['action'] + by_status['coveredBy'] + by_status['excluded']
    print(f'scanned surface items: {total}   covered: {covered} ({100.0 * covered / max(total, 1):.1f}%)   '
          f'todo: {by_status["todo"]}   missing: {len(missing)}   stale: {len(stale)}')
    print('\nby kind:')
    for kind in sorted(by_kind):
        c = by_kind[kind]
        print(f'  {kind:<11} ' + '  '.join(f'{s}={c[s]}' for s in ('action', 'coveredBy', 'excluded', 'todo', 'missing') if c[s]))
    print('\nby manifest:')
    for d in sorted(by_domain):
        c = by_domain[d]
        print(f'  {d:<15} ' + '  '.join(f'{s}={c[s]}' for s in ('action', 'coveredBy', 'excluded', 'todo') if c[s]))
    if by_reason:
        print('\nexcluded by reason:')
        for r, n in sorted(by_reason.items()):
            print(f'  {r:<20} {n}')
    if catalog_ids is None:
        print(f'\nnote: no catalog snapshot at {args.catalog}; action ids not checked')

    if args.list:
        bucket = {'missing': missing, 'stale': stale,
                  'todo': [k for k in scanned if entries.get(k, {}).get('todo')],
                  'excluded': [k for k in scanned if 'excluded' in entries.get(k, {})]}[args.list]
        for k in bucket:
            if args.domain and owner.get(k) != args.domain:
                continue
            item = scanned.get(k)
            where = f"{item['file']}:{item['lines'][0]}" if item else ''
            print(f'{k}\t{where}')

    failures = []
    if missing:
        failures.append(f'{len(missing)} scanned keys are in no manifest (run --init-manifests, then map them)')
    if invalid:
        failures.append(f'{len(invalid)} manifest entries have no action/coveredBy/excluded/todo')
    if bad_reasons:
        failures.append(f'{len(bad_reasons)} exclusions use an unknown reason: ' + ', '.join(sorted({r for _, r in bad_reasons})))
    if unknown_actions:
        failures.append(f'{len(unknown_actions)} manifest action ids are not in the catalog: ' +
                        ', '.join(sorted({a for _, a in unknown_actions}))[:400])
    if args.strict:
        if by_status['todo']:
            failures.append(f'{by_status["todo"]} todo entries remain')
        if stale:
            failures.append(f'{len(stale)} stale manifest keys (no longer in the source)')

    if args.json:
        report = {
            'total': total, 'covered': covered, 'byStatus': dict(by_status),
            'byKind': {k: dict(v) for k, v in by_kind.items()}, 'byDomain': {k: dict(v) for k, v in by_domain.items()},
            'excludedByReason': dict(by_reason), 'missing': missing, 'stale': stale,
            'unknownActions': unknown_actions, 'failures': failures,
        }
        with open(args.json, 'w', encoding='utf-8') as fh:
            json.dump(report, fh, indent=2)

    if failures:
        print('\nFAIL:')
        for f in failures:
            print('  - ' + f)
        return 1
    print('\nOK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
