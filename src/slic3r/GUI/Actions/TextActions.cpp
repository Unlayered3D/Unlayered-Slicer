// Automation actions: text.add / text.edit / text.listFonts / svg.add / svg.edit.
// Owner: work package S3 (parts). See docs/HLSD/bridge-actions.md for how to write an action and
// docs/bridge-actions/parts.md for these.
//
// The Text and SVG tools create their volumes in a background job that places them under the
// mouse (or the screen center) and then opens the tool. Neither is reproducible from a request, so
// these runners build the same embossed volume synchronously -- the same font loading, glyph
// shapes, union and extrusion as EmbossJob's create path -- and place it on a named side of the
// object by casting a ray at it. The volume carries the same text_configuration / emboss_shape the
// tools write, so the user can open it in the Text or SVG tool afterwards and keep editing.

#include "ActionCatalog.hpp"
#include "ActionRuntime.hpp"
#include "Register.hpp"
#include "PartsCommon.hpp"
#include "PartsCommonGui.hpp"

#include <algorithm>
#include <memory>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/convert.hpp>

#include <wx/font.h>
#include <wx/fontenum.h>

#include "libslic3r/Emboss.hpp"
#include "libslic3r/EmbossShape.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/NSVGUtils.hpp"
#include "libslic3r/TextConfiguration.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/WebAppBridge.hpp"
#include "slic3r/Utils/WxFontUtils.hpp"

namespace Slic3r { namespace GUI { namespace Actions {

using nlohmann::json;
namespace fs = boost::filesystem;

namespace {

// EmbossJob's SAFE_SURFACE_OFFSET: how far a raised text sinks into / an engraved one stands out
// of the surface so the two always overlap.
constexpr float SAFE_SURFACE_OFFSET = 0.015f; // [mm]

// ---------------------------------------------------------------- fonts

struct LoadedFont
{
    EmbossStyle                     style;
    Emboss::FontFileWithCache       font;
};

// The Text tool's Bold / Italic buttons (GLGizmoEmboss::draw_bold_button / draw_italic_button):
// switch to the face's bold or italic variant, and when the font has none, fall back to synthetic
// boldness (20 font units) or skew (0.2), so the text still turns bold or italic.
bool load_font_from_wx(wxFont wx_font, bool bold, bool italic, LoadedFont& out, std::string& error)
{
    std::unique_ptr<Emboss::FontFile> ff = WxFontUtils::create_font_file(wx_font);
    if (ff == nullptr) {
        error = "the font has no TrueType outlines that can be embossed";
        return false;
    }
    std::optional<float> boldness, skew;
    if (bold) {
        wxFont f = wx_font;
        if (std::unique_ptr<Emboss::FontFile> nf = WxFontUtils::set_bold(f, *ff); nf != nullptr) {
            wx_font = f;
            ff      = std::move(nf);
        } else
            boldness = 20.f;
    }
    if (italic) {
        wxFont f = wx_font;
        if (std::unique_ptr<Emboss::FontFile> nf = WxFontUtils::set_italic(f, *ff); nf != nullptr) {
            wx_font = f;
            ff      = std::move(nf);
        } else
            skew = 0.2f;
    }
    out.style               = WxFontUtils::create_emboss_style(wx_font);
    out.style.prop.boldness = boldness;
    out.style.prop.skew     = skew;
    out.font                = Emboss::FontFileWithCache(std::move(ff));
    return true;
}

// `face` empty = the Text tool's default style (wxNORMAL_FONT).
bool load_font(const std::string& face, bool bold, bool italic, LoadedFont& out, std::string& error)
{
    if (face.empty()) {
        wxFont f = *wxNORMAL_FONT;
        f.SetWeight(wxFONTWEIGHT_NORMAL);
        f.SetStyle(wxFONTSTYLE_NORMAL);
        return load_font_from_wx(f, bold, italic, out, error);
    }
    wxFont wx_font(wxFontInfo().FaceName(wxString::FromUTF8(face)).Encoding(wxFONTENCODING_SYSTEM));
    if (!wx_font.IsOk() || !wx_font.GetFaceName().IsSameAs(wxString::FromUTF8(face), false)) {
        error = "no installed font is named \"" + face + "\" (text.listFonts lists them)";
        return false;
    }
    return load_font_from_wx(wx_font, bold, italic, out, error);
}

// The Text tool's Advanced section (draw_advanced) on a style's properties. "" when fine, else the
// param at fault ("path") with `error` set.
std::string apply_text_props(const json& p, const Emboss::FontFile& font, FontProp& prop, std::string& error)
{
    if (p.contains("charGap"))
        prop.char_gap = p["charGap"].get<int>() == 0 ? std::optional<int>() : std::optional<int>(p["charGap"].get<int>());
    if (p.contains("lineGap"))
        prop.line_gap = p["lineGap"].get<int>() == 0 ? std::optional<int>() : std::optional<int>(p["lineGap"].get<int>());
    if (p.contains("boldness"))
        prop.boldness = p["boldness"].get<double>() == 0. ? std::optional<float>() : std::optional<float>(p["boldness"].get<float>());
    if (p.contains("skew"))
        prop.skew = p["skew"].get<double>() == 0. ? std::optional<float>() : std::optional<float>(p["skew"].get<float>());
    if (p.contains("alignH")) {
        const std::string h = p["alignH"].get<std::string>();
        prop.align.first    = h == "left" ? FontProp::HorizontalAlign::left : h == "right" ? FontProp::HorizontalAlign::right : FontProp::HorizontalAlign::center;
    }
    if (p.contains("alignV")) {
        const std::string v = p["alignV"].get<std::string>();
        prop.align.second   = v == "top" ? FontProp::VerticalAlign::top : v == "bottom" ? FontProp::VerticalAlign::bottom : FontProp::VerticalAlign::center;
    }
    if (p.contains("fontIndex")) {
        const unsigned i = p["fontIndex"].get<unsigned>();
        if (i >= font.infos.size()) {
            error = "the font file holds " + std::to_string(font.infos.size()) + " face(s); fontIndex is 0-based";
            return "/params/fontIndex";
        }
        prop.collection_number = i == 0 ? std::optional<unsigned int>() : std::optional<unsigned int>(i);
    }
    return {};
}

// ---------------------------------------------------------------- meshes

// The extruded mesh of an emboss shape in its own frame: XY is the shape plane, +Z points out of
// the surface. A raised (part / modifier) shape spans z = [0, depth], an engraved (negative) one
// z = [-depth, 0], each overlapping the surface by SAFE_SURFACE_OFFSET (EmbossJob try_create_mesh).
TriangleMesh extrude_shape(EmbossShape& shape, bool is_outside)
{
    ExPolygons shapes = union_with_delta(shape, Emboss::UNION_DELTA, Emboss::UNION_MAX_ITERATIN);
    if (shapes.empty())
        return {};
    const double scale  = shape.scale;
    const double depth  = shape.projection.depth / scale;
    auto         proj_z = std::make_unique<Emboss::ProjectZ>(depth);
    const float  offset = is_outside ? -SAFE_SURFACE_OFFSET : (SAFE_SURFACE_OFFSET - float(shape.projection.depth));
    const Transform3d tr = Eigen::Translation<double, 3>(0., 0., double(offset)) * Eigen::Scaling(scale);
    Emboss::ProjectTransform project(std::move(proj_z), tr);
    return TriangleMesh(Emboss::polygons2model(shapes, project));
}

bool build_text(LoadedFont& lf, const std::string& text, double depth, bool is_outside, EmbossShape& shape, TriangleMesh& mesh,
                std::string& error)
{
    shape                        = EmbossShape{};
    shape.projection.depth       = depth;
    shape.projection.use_surface = false;
    shape.scale                  = Emboss::get_text_shape_scale(lf.style.prop, *lf.font.font_file);
    const std::wstring wtext     = boost::nowide::widen(text);
    shape.shapes_with_ids        = Emboss::text2vshapes(lf.font, wtext, lf.style.prop, []() { return false; });
    mesh                         = extrude_shape(shape, is_outside);
    if (mesh.empty()) {
        error = "the text produced no shape with this font (only spaces, or glyphs the font lacks)";
        return false;
    }
    return true;
}

std::string volume_name_for(const std::string& text)
{
    std::string name = text;
    std::replace(name.begin(), name.end(), '\n', ' ');
    return name;
}

// Adds an embossed volume with `mesh` (shape frame, see extrude_shape) to object `obj_idx`, placed
// by `placement` (plate frame). Takes no snapshot.
ModelVolume* add_emboss_volume(Model& model, int obj_idx, TriangleMesh&& mesh, ModelVolumeType type, const parts::SurfacePlacement& placement,
                               const std::string& name, const EmbossShape& shape, const std::optional<TextConfiguration>& tc)
{
    ModelObject& object = *model.objects[obj_idx];
    // As EmbossJob's create_volume: add_volume centers a mesh, so add a stand-in and set the real
    // mesh afterwards to keep the shape frame's origin on the surface.
    ModelVolume* v = object.add_volume(make_cube(1., 1., 1.), type);
    v->set_mesh(std::move(mesh));
    v->calculate_convex_hull();
    v->config.set_key_value("extruder", new ConfigOptionInt(0));
    // do not allow model reload from disk
    v->source.is_from_builtin_objects = true;
    v->name                           = name;
    v->set_transformation(Geometry::Transformation(object.instances.front()->get_matrix().inverse() * placement.world));
    v->emboss_shape = shape;
    v->emboss_shape->fix_3mf_tr.reset();
    if (tc.has_value())
        v->text_configuration = *tc;
    object.invalidate_bounding_box();
    Slic3r::save_object_mesh(object);
    return v;
}

InvokeResult finish_emboss_volume(Model& model, int obj_idx, ModelVolume* v, const parts::SurfacePlacement& placement)
{
    ModelObject& object = *model.objects[obj_idx];
    if (v->is_model_part() && object.auto_drop)
        object.ensure_on_bed();
    parts::refresh_object_after_volume_change(obj_idx, v);
    json r = {{"volume", parts::volume_result_json(model, obj_idx, parts::volume_index_in(object, v))},
              {"surfaceHit", placement.hit},
              {"positionMm", parts::vec3_json(placement.position)},
              {"normal", parts::vec3_json(placement.normal)}};
    if (!placement.hit)
        r["hint"] = "the ray toward that side missed the object; the shape sits on its bounding box";
    return InvokeResult::ok(r);
}

parts::SurfacePlacement placement_from(const ModelObject& object, const json& p)
{
    const Vec3d n = parts::side_normal(p.value("side", "front")).value_or(Vec3d(0., -1., 0.));
    Vec2d       offset(0., 0.);
    if (auto it = p.find("offsetMm"); it != p.end() && it->is_array() && it->size() == 2)
        offset = Vec2d((*it)[0].get<double>(), (*it)[1].get<double>());
    return parts::place_on_side(object, n, offset, p.value("rotationDeg", 0.));
}

ModelVolumeType emboss_type(const json& p) { return parts::volume_type_from_name(p.value("type", "part")); }

// ---------------------------------------------------------------- text.add

InvokeResult run_text_add(InvokeContext& ctx)
{
    if (ctx.targets.objects.size() != 1)
        return InvokeResult::error(WAB_BAD_PARAMS, "text.add needs exactly one object", {{"path", "/target"}});
    Model&            model   = ctx.model();
    const int         obj_idx = ctx.targets.objects.front();
    const json&       p       = ctx.params;
    const std::string text    = p.value("text", "");
    if (text.find_first_not_of(" \t\r\n") == std::string::npos)
        return InvokeResult::error(WAB_BAD_PARAMS, "text is empty", {{"path", "/params/text"}});
    const ModelVolumeType type = emboss_type(p);

    LoadedFont  lf;
    std::string error;
    if (!load_font(p.value("font", ""), p.value("bold", false), p.value("italic", false), lf, error))
        return InvokeResult::error(WAB_BAD_PARAMS, error, {{"path", "/params/font"}});
    lf.style.prop.size_in_mm = float(p.value("heightMm", 10.));
    if (std::string path = apply_text_props(p, *lf.font.font_file, lf.style.prop, error); !path.empty())
        return InvokeResult::error(WAB_BAD_PARAMS, error, {{"path", path}});

    EmbossShape  shape;
    TriangleMesh mesh;
    if (!build_text(lf, text, p.value("depthMm", 1.5), type == ModelVolumeType::MODEL_PART, shape, mesh, error))
        return InvokeResult::error(WAB_BAD_PARAMS, error, {{"path", "/params/text"}});

    const parts::SurfacePlacement placement = placement_from(*model.objects[obj_idx], p);
    ctx.snapshot("Add Emboss text Volume");
    Plater::SuppressSnapshots suppress(ctx.plater());
    ModelVolume* v = add_emboss_volume(model, obj_idx, std::move(mesh), type, placement, volume_name_for(text), shape,
                                       TextConfiguration{lf.style, text});
    return finish_emboss_volume(model, obj_idx, v, placement);
}

// ---------------------------------------------------------------- text.edit

InvokeResult run_text_edit(InvokeContext& ctx)
{
    Model& model = ctx.model();
    if (ctx.targets.volumes.size() != 1)
        return InvokeResult::error(WAB_BAD_PARAMS, "text.edit needs exactly one text part", {{"path", "/target"}});
    const int    obj_idx = ctx.targets.volumes.front().first;
    const int    vol_idx = ctx.targets.volumes.front().second;
    ModelObject& object  = *model.objects[obj_idx];
    ModelVolume& volume  = *object.volumes[vol_idx];
    if (!volume.text_configuration.has_value() || !volume.emboss_shape.has_value())
        return InvokeResult::error(WAB_BAD_PARAMS, "that part is not a text", {{"path", "/target"}});
    if (volume.emboss_shape->projection.use_surface)
        return InvokeResult::error(WAB_BAD_PARAMS, "this text follows the surface; edit it in the Text tool", {{"path", "/target"}});

    const json& p = ctx.params;
    static const char* const EDITABLE[] = {"text", "heightMm", "depthMm", "font", "bold", "italic", "charGap", "lineGap",
                                           "boldness", "skew", "alignH", "alignV", "fontIndex"};
    if (std::none_of(std::begin(EDITABLE), std::end(EDITABLE), [&p](const char* k) { return p.contains(k); }))
        return InvokeResult::error(WAB_BAD_PARAMS, "nothing to change", {{"path", "/params"}, {"needs", "text"}});

    const TextConfiguration& old_tc = *volume.text_configuration;
    const std::string        text   = p.value("text", old_tc.text);
    if (text.find_first_not_of(" \t\r\n") == std::string::npos)
        return InvokeResult::error(WAB_BAD_PARAMS, "text is empty", {{"path", "/params/text"}});

    LoadedFont  lf;
    std::string error;
    const wxFont old_font = WxFontUtils::create_wxFont(old_tc.style);
    if (p.contains("font") || p.contains("bold") || p.contains("italic")) {
        // Bold / italic as the text has them now: a bold or italic face, or the synthetic fallback.
        const bool bold   = p.value("bold", old_tc.style.prop.boldness.has_value() || (old_font.IsOk() && WxFontUtils::is_bold(old_font)));
        const bool italic = p.value("italic", old_tc.style.prop.skew.has_value() || (old_font.IsOk() && WxFontUtils::is_italic(old_font)));
        bool       loaded = false;
        if (p.contains("font"))
            loaded = load_font(p["font"].get<std::string>(), bold, italic, lf, error);
        else if (old_font.IsOk()) {
            wxFont f = old_font;
            f.SetWeight(wxFONTWEIGHT_NORMAL);
            f.SetStyle(wxFONTSTYLE_NORMAL);
            loaded = load_font_from_wx(f, bold, italic, lf, error);
        } else
            error = "the text's font is not installed here";
        if (!loaded)
            return InvokeResult::error(WAB_BAD_PARAMS, error, {{"path", "/params/font"}});
        // Keep what the new face does not define (the Text tool keeps them across a font change):
        // size, gaps, alignment, per-glyph and, unless bold / italic was just decided, the
        // synthetic boldness / skew.
        FontProp& np    = lf.style.prop;
        const FontProp& op = old_tc.style.prop;
        np.size_in_mm   = op.size_in_mm;
        np.char_gap     = op.char_gap;
        np.line_gap     = op.line_gap;
        np.align        = op.align;
        np.per_glyph    = op.per_glyph;
        if (!p.contains("bold"))
            np.boldness = op.boldness.has_value() ? op.boldness : np.boldness;
        if (!p.contains("italic"))
            np.skew = op.skew.has_value() ? op.skew : np.skew;
        if (!p.contains("font"))
            np.collection_number = op.collection_number;
    } else {
        std::unique_ptr<Emboss::FontFile> ff = old_font.IsOk() ? WxFontUtils::create_font_file(old_font) : nullptr;
        if (ff == nullptr)
            return InvokeResult::error(WAB_BAD_PARAMS, "the text's font is not installed here; give font", {{"path", "/params/font"}});
        lf.style = old_tc.style;
        lf.font  = Emboss::FontFileWithCache(std::move(ff));
    }
    if (p.contains("heightMm"))
        lf.style.prop.size_in_mm = float(p["heightMm"].get<double>());
    if (std::string path = apply_text_props(p, *lf.font.font_file, lf.style.prop, error); !path.empty())
        return InvokeResult::error(WAB_BAD_PARAMS, error, {{"path", path}});
    const double depth = p.value("depthMm", volume.emboss_shape->projection.depth);

    EmbossShape  shape;
    TriangleMesh mesh;
    if (!build_text(lf, text, depth, volume.is_model_part(), shape, mesh, error))
        return InvokeResult::error(WAB_BAD_PARAMS, error, {{"path", "/params/text"}});

    ctx.snapshot("Emboss attribute change");
    Plater::SuppressSnapshots suppress(ctx.plater());
    // As EmbossJob's update_volume: undo the matrix fix a 3MF load applied, then swap the mesh.
    if (volume.emboss_shape->fix_3mf_tr.has_value())
        volume.set_transformation(volume.get_matrix() * volume.emboss_shape->fix_3mf_tr->inverse());
    volume.set_mesh(std::move(mesh));
    volume.set_new_unique_id();
    volume.calculate_convex_hull();
    volume.name         = volume_name_for(text);
    volume.emboss_shape = shape;
    volume.emboss_shape->fix_3mf_tr.reset();
    volume.text_configuration = TextConfiguration{lf.style, text};
    object.invalidate_bounding_box();
    if (ObjectList* list = parts::object_list())
        list->update_name_in_list(obj_idx, vol_idx);
    ctx.plater()->changed_object(object);
    return InvokeResult::ok({{"volume", parts::volume_result_json(model, obj_idx, vol_idx)}});
}

// ---------------------------------------------------------------- text.listFonts

InvokeResult run_text_list_fonts(InvokeContext& ctx)
{
    const std::string query = ctx.params.value("query", "");
    const int         limit = ctx.params.value("limit", 100);
    const wxArrayString      wx_names = wxFontEnumerator::GetFacenames(wxFONTENCODING_SYSTEM);
    std::vector<std::string> names;
    for (const wxString& n : wx_names)
        if (!n.StartsWith("@")) // vertical CJK variants on Windows
            names.emplace_back(n.ToUTF8().data());
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return boost::algorithm::ilexicographical_compare(a, b); });
    names.erase(std::unique(names.begin(), names.end()), names.end());
    json   out   = json::array();
    size_t total = 0;
    for (const std::string& u : names) {
        if (!query.empty() && !boost::algorithm::icontains(u, query))
            continue;
        ++total;
        if (int(out.size()) < limit)
            out.push_back(u);
    }
    return InvokeResult::ok({{"fonts", out}, {"total", total}, {"default", std::string(wxNORMAL_FONT->GetFaceName().ToUTF8().data())}})
        .with_effects({});
}

// ---------------------------------------------------------------- svg.add

// GLGizmoSVG's get_tesselation_tolerance(1.): 0.1 mm, in the scaled units the SVG is read in.
double svg_tesselation_tolerance()
{
    const double tol_mm = .1;
    return (tol_mm * tol_mm) / SCALING_FACTOR / SCALING_FACTOR;
}

InvokeResult run_svg_add(InvokeContext& ctx)
{
    if (ctx.targets.objects.size() != 1)
        return InvokeResult::error(WAB_BAD_PARAMS, "svg.add needs exactly one object", {{"path", "/target"}});
    if (ctx.host == nullptr || !ctx.host->download)
        return InvokeResult::error(WAB_BAD_PARAMS, "downloads are unavailable in this tab", {{"path", "/params/url"}});
    Model&          model     = ctx.model();
    const ObjectID  object_id = model.objects[ctx.targets.objects.front()]->id();
    const json      p         = ctx.params;

    // The undo point is taken now, so undoing this turn also removes what the download adds.
    ctx.snapshot("Add SVG");
    std::shared_ptr<Deferred> done = ctx.defer();
    ctx.host->download(
        p.value("url", ""), {"svg"},
        [done, object_id, p](const std::string& path) {
            Model&    model   = wxGetApp().model();
            const int obj_idx = parts::object_index_by_object_id(model, object_id);
            if (obj_idx < 0)
                return parts::finish_later(done, InvokeResult::error(WAB_NOT_FOUND, "the object was removed while the file downloaded"));
            if (!parts::gizmos_closed())
                return parts::finish_later(done, InvokeResult::error(WAB_BUSY, "a gizmo was opened while the file downloaded"));

            EmbossShape          shape;
            EmbossShape::SvgFile svg;
            svg.path = path;
            if (init_image(svg) == nullptr)
                return parts::finish_later(done, InvokeResult::error(WAB_UNSUPPORTED_TYPE, "the SVG could not be parsed"));
            NSVGLineParams params{svg_tesselation_tolerance()};
            shape.shapes_with_ids = create_shape_with_ids(*svg.image, params);
            if (shape.shapes_with_ids.empty())
                return parts::finish_later(done, InvokeResult::error(WAB_UNSUPPORTED_TYPE, "the SVG holds no filled or stroked path to emboss"));
            center(shape.shapes_with_ids);
            const BoundingBox bb      = get_extents(shape.shapes_with_ids);
            const double      natural = std::max(bb.size().x(), bb.size().y()) * SCALING_FACTOR; // mm at the file's own size
            const double      wanted  = p.value("sizeMm", 30.);
            shape.scale               = natural > EPSILON ? SCALING_FACTOR * wanted / natural : SCALING_FACTOR;
            shape.projection.depth       = p.value("depthMm", 1.5);
            shape.projection.use_surface = false;
            shape.svg_file               = std::move(svg);

            const ModelVolumeType type = emboss_type(p);
            TriangleMesh          mesh = extrude_shape(shape, type == ModelVolumeType::MODEL_PART);
            if (mesh.empty())
                return parts::finish_later(done, InvokeResult::error(WAB_UNSUPPORTED_TYPE, "the SVG produced no shape"));

            const parts::SurfacePlacement placement = placement_from(*model.objects[obj_idx], p);
            std::string name = fs::path(path).stem().string();
            if (name.empty())
                name = "SVG shape";
            Plater::SuppressSnapshots suppress(wxGetApp().plater());
            ModelVolume* v = add_emboss_volume(model, obj_idx, std::move(mesh), type, placement, name, shape, std::nullopt);
            parts::finish_later(done, finish_emboss_volume(model, obj_idx, v, placement));
        },
        [done](int code, const std::string& message) { parts::finish_later(done, InvokeResult::error(code, message)); });
    return InvokeResult::started();
}

// ---------------------------------------------------------------- svg.edit

// The SVG tool's edits on one SVG volume, after any new file is loaded: size (uniform, re-tessellated,
// sizeMm), width / height (the tool's W / H fields: the volume is scaled along its own X / Y), mirror,
// depth, a new file (url) and Bake. Takes no snapshot.
InvokeResult apply_svg_edit(Model& model, int obj_idx, int vol_idx, const json& p, const std::string& new_file)
{
    ModelObject& object = *model.objects[obj_idx];
    ModelVolume& volume = *object.volumes[vol_idx];
    if (p.value("bake", false)) {
        // "Bake into model as uneditable part": the mesh stays, the SVG source goes.
        volume.emboss_shape.reset();
        if (ObjectList* list = parts::object_list())
            list->update_name_in_list(obj_idx, vol_idx);
        wxGetApp().plater()->changed_object(object);
        return InvokeResult::ok({{"volume", parts::volume_result_json(model, obj_idx, vol_idx)}, {"baked", true}});
    }

    EmbossShape shape = *volume.emboss_shape;
    if (!new_file.empty()) {
        // "Change file": the new drawing at the same scale, centred like a new SVG part.
        EmbossShape::SvgFile svg;
        svg.path = new_file;
        if (init_image(svg) == nullptr)
            return InvokeResult::error(WAB_UNSUPPORTED_TYPE, "the SVG could not be parsed");
        NSVGLineParams params{svg_tesselation_tolerance()};
        ExPolygonsWithIds shapes = create_shape_with_ids(*svg.image, params);
        if (shapes.empty())
            return InvokeResult::error(WAB_UNSUPPORTED_TYPE, "the SVG holds no filled or stroked path to emboss");
        center(shapes);
        shape.shapes_with_ids = std::move(shapes);
        shape.final_shape     = {};
        shape.svg_file        = std::move(svg);
    }
    if (shape.shapes_with_ids.empty())
        return InvokeResult::error(WAB_BAD_PARAMS, "the SVG part holds no shape", {{"path", "/target"}});
    const BoundingBox bb = get_extents(shape.shapes_with_ids);
    if (p.contains("sizeMm")) {
        const double current = std::max(bb.size().x(), bb.size().y()) * shape.scale;
        if (current > EPSILON)
            shape.scale *= p["sizeMm"].get<double>() / current;
    }
    if (p.contains("depthMm"))
        shape.projection.depth = p["depthMm"].get<double>();
    TriangleMesh mesh = extrude_shape(shape, volume.is_model_part());
    if (mesh.empty())
        return InvokeResult::error(WAB_INTERNAL, "the SVG produced no shape");

    if (volume.emboss_shape->fix_3mf_tr.has_value())
        volume.set_transformation(volume.get_matrix() * volume.emboss_shape->fix_3mf_tr->inverse());
    // W / H and mirror act on the volume in its own frame, as the tool's selection.scale /
    // selection.mirror do with a local transformation.
    Vec3d local(1., 1., 1.);
    const Transform3d m = volume.get_matrix();
    const double      sx = m.linear().col(0).norm(), sy = m.linear().col(1).norm();
    if (p.contains("widthMm") && bb.size().x() > 0)
        local.x() = p["widthMm"].get<double>() / (bb.size().x() * shape.scale * sx);
    if (p.contains("heightMm") && bb.size().y() > 0)
        local.y() = p["heightMm"].get<double>() / (bb.size().y() * shape.scale * sy);
    const std::string mirror = p.value("mirror", "");
    if (mirror == "x")
        local.x() = -local.x();
    else if (mirror == "y")
        local.y() = -local.y();
    if (!local.isApprox(Vec3d(1., 1., 1.)))
        volume.set_transformation(m * Geometry::scale_transform(local));
    volume.set_mesh(std::move(mesh));
    volume.set_new_unique_id();
    volume.calculate_convex_hull();
    volume.emboss_shape = shape;
    volume.emboss_shape->fix_3mf_tr.reset();
    if (!new_file.empty()) {
        std::string name = fs::path(new_file).stem().string();
        volume.name      = name.empty() ? std::string("SVG shape") : name;
        if (ObjectList* list = parts::object_list())
            list->update_name_in_list(obj_idx, vol_idx);
    }
    object.invalidate_bounding_box();
    wxGetApp().plater()->changed_object(object);
    return InvokeResult::ok({{"volume", parts::volume_result_json(model, obj_idx, vol_idx)}});
}

InvokeResult run_svg_edit(InvokeContext& ctx)
{
    Model& model = ctx.model();
    if (ctx.targets.volumes.size() != 1)
        return InvokeResult::error(WAB_BAD_PARAMS, "svg.edit needs exactly one SVG part", {{"path", "/target"}});
    const int    obj_idx = ctx.targets.volumes.front().first;
    const int    vol_idx = ctx.targets.volumes.front().second;
    ModelObject& object  = *model.objects[obj_idx];
    ModelVolume& volume  = *object.volumes[vol_idx];
    if (!volume.is_svg() || !volume.emboss_shape->svg_file.has_value())
        return InvokeResult::error(WAB_BAD_PARAMS, "that part is not an SVG", {{"path", "/target"}});
    const json& p    = ctx.params;
    const bool  bake = p.value("bake", false);
    if (volume.emboss_shape->projection.use_surface && !bake)
        return InvokeResult::error(WAB_BAD_PARAMS, "this SVG follows the surface; edit it in the SVG tool (bake works)", {{"path", "/target"}});
    static const char* const EDITS[] = {"sizeMm", "widthMm", "heightMm", "depthMm", "mirror", "url"};
    const bool any_edit = std::any_of(std::begin(EDITS), std::end(EDITS), [&p](const char* k) { return p.contains(k); });
    if (bake && any_edit)
        return InvokeResult::error(WAB_BAD_PARAMS, "bake takes no other change: edit first, then bake", {{"path", "/params/bake"}});
    if (p.contains("bake") && !bake)
        return InvokeResult::error(WAB_BAD_PARAMS, "bake must be true", {{"path", "/params/bake"}});
    if (!bake && !any_edit)
        return InvokeResult::error(WAB_BAD_PARAMS, "nothing to change", {{"path", "/params"}, {"needs", "sizeMm"}});
    if (p.contains("sizeMm") && (p.contains("widthMm") || p.contains("heightMm")))
        return InvokeResult::error(WAB_BAD_PARAMS, "give sizeMm (uniform) or widthMm / heightMm, not both", {{"path", "/params/sizeMm"}});

    if (!p.contains("url")) {
        ctx.snapshot(bake ? "Bake SVG" : "Emboss attribute change");
        Plater::SuppressSnapshots suppress(ctx.plater());
        return apply_svg_edit(model, obj_idx, vol_idx, p, {});
    }
    // "Change file": download it first; the undo point is taken now, as svg.add does.
    if (ctx.host == nullptr || !ctx.host->download)
        return InvokeResult::error(WAB_BAD_PARAMS, "downloads are unavailable in this tab", {{"path", "/params/url"}});
    const ObjectID volume_id = volume.id();
    const ObjectID object_id = object.id();
    ctx.snapshot("Change SVG file");
    std::shared_ptr<Deferred> done = ctx.defer();
    ctx.host->download(
        p.value("url", ""), {"svg"},
        [done, object_id, volume_id, p](const std::string& path) {
            Model&    model   = wxGetApp().model();
            const int obj_idx = parts::object_index_by_object_id(model, object_id);
            const int vol_idx = obj_idx < 0 ? -1 : parts::volume_index_by_object_id(*model.objects[obj_idx], volume_id);
            if (vol_idx < 0)
                return parts::finish_later(done, InvokeResult::error(WAB_NOT_FOUND, "the SVG part was removed while the file downloaded"));
            if (!parts::gizmos_closed())
                return parts::finish_later(done, InvokeResult::error(WAB_BUSY, "a gizmo was opened while the file downloaded"));
            Plater::SuppressSnapshots suppress(wxGetApp().plater());
            parts::finish_later(done, apply_svg_edit(model, obj_idx, vol_idx, p, path));
        },
        [done](int code, const std::string& message) { parts::finish_later(done, InvokeResult::error(code, message)); });
    return InvokeResult::started();
}

// ---------------------------------------------------------------- schema pieces

json emboss_type_schema()
{
    return {{"type", "string"},
            {"enum", {"part", "negative", "modifier"}},
            {"default", "part"},
            {"description", "part = raised, negative = engraved into the surface, modifier = a settings region."}};
}

// The Text tool's Advanced section, shared by text.add and text.edit.
void add_text_style_properties(json& props)
{
    props["charGap"]   = {{"type", "integer"}, {"minimum", -20000}, {"maximum", 20000},
                          {"description", "Extra space between letters, font units (0 = none)."}};
    props["lineGap"]   = {{"type", "integer"}, {"minimum", -20000}, {"maximum", 20000},
                          {"description", "Extra space between lines, font units (0 = none)."}};
    props["boldness"]  = {{"type", "number"}, {"minimum", -500000}, {"maximum", 500000},
                          {"description", "Synthetic boldness, font units (the tool's slider spans about +-half the letter ascent; "
                                          "bold on a face without a bold variant uses 20; 0 = none)."}};
    props["skew"]      = {{"type", "number"}, {"minimum", -100}, {"maximum", 100},
                          {"description", "Italic strength, a ratio (the tool's slider spans -1..1; italic on a face without an italic "
                                          "variant uses 0.2; 0 = none)."}};
    props["alignH"]    = {{"type", "string"}, {"enum", {"left", "center", "right"}}, {"description", "Horizontal alignment of the lines."}};
    props["alignV"]    = {{"type", "string"}, {"enum", {"top", "center", "bottom"}}, {"description", "Vertical alignment about the anchor."}};
    props["fontIndex"] = {{"type", "integer"}, {"minimum", 0}, {"maximum", 255},
                          {"description", "Which face of a font collection file (.ttc), 0-based (the tool's Collection)."}};
}

json placement_properties()
{
    return {{"side", {{"type", "string"},
                      {"enum", {"front", "back", "left", "right", "top"}},
                      {"default", "front"},
                      {"description", "Which side of the object it goes on (front faces -Y, the user)."}}},
            {"offsetMm", {{"type", "array"},
                          {"items", {{"type", "number"}}},
                          {"minItems", 2},
                          {"maxItems", 2},
                          {"description", "Shift from the side's center [along, up] in mm."}}},
            {"rotationDeg", {{"type", "number"}, {"minimum", -360}, {"maximum", 360}, {"default", 0}, {"description", "Rotation about the surface normal."}}}};
}

} // namespace

void register_text_actions(Catalog& c)
{
    // text.add
    {
        ActionDescriptor d;
        d.id      = "text.add";
        d.label   = "Add text";
        d.summary = "Emboss (raise) or engrave text on a side of an object, with font, height and depth. The text stays "
                    "editable (text.edit, or the Text tool).";
        d.target  = {{"kind", "object"}, {"required", true}};
        json props = placement_properties();
        props["text"]     = {{"type", "string"}, {"minLength", 1}, {"maxLength", 1000}, {"description", "The text; \\n starts a new line."}};
        props["heightMm"] = {{"type", "number"}, {"minimum", 0.1}, {"maximum", 1000}, {"default", 10}, {"description", "Letter height (font size) in mm."}};
        props["depthMm"]  = {{"type", "number"}, {"minimum", 0.01}, {"maximum", 1000}, {"default", 1.5}, {"description", "How far it stands out or cuts in, mm."}};
        props["type"]     = emboss_type_schema();
        props["font"]     = {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}, {"description", "Installed font face name (text.listFonts); default: the Text tool's."}};
        props["bold"]     = {{"type", "boolean"}, {"default", false},
                             {"description", "The face's bold variant, or synthetic boldness when it has none (as the tool)."}};
        props["italic"]   = {{"type", "boolean"}, {"default", false},
                             {"description", "The face's italic variant, or a skew when it has none (as the tool)."}};
        add_text_style_properties(props);
        d.params  = {{"type", "object"}, {"additionalProperties", false}, {"required", {"text"}}, {"properties", props}};
        d.effects = {"geometry", "structure"};
        d.ui      = {"GUI_Factories.cpp::menu::expr:item_name@append_menu_itemm_add_", "GLGizmosManager.cpp::gizmo::GLGizmoEmboss"};
        d.examples = json::array({{{"target", {{"object", {{"index", 0}}}}}, {"params", {{"text", "ABC"}, {"heightMm", 5}, {"depthMm", 0.6}}}},
                                  {{"target", {{"object", {{"index", 0}}}}},
                                   {"params", {{"text", "v2"}, {"type", "negative"}, {"side", "top"}, {"heightMm", 6}, {"depthMm", 0.4}}}}});
        c.add(d, run_text_add);
    }

    // text.edit
    {
        ActionDescriptor d;
        d.id      = "text.edit";
        d.label   = "Edit text";
        d.summary = "Change an embossed text part: its text, font, bold/italic, height, depth, letter and line spacing, "
                    "boldness, skew or alignment; what is not given is kept (model.getTree shows the current values). It keeps its place.";
        d.target  = {{"kind", "volume"}, {"required", true}};
        json props = {{"text", {{"type", "string"}, {"minLength", 1}, {"maxLength", 1000}}},
                      {"heightMm", {{"type", "number"}, {"minimum", 0.1}, {"maximum", 1000}}},
                      {"depthMm", {{"type", "number"}, {"minimum", 0.01}, {"maximum", 1000}}},
                      {"font", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
                      {"bold", {{"type", "boolean"}}},
                      {"italic", {{"type", "boolean"}}}};
        add_text_style_properties(props);
        d.params  = {{"type", "object"}, {"additionalProperties", false}, {"properties", props}};
        d.effects = {"geometry"};
        d.ui      = {"GUI_Factories.cpp::menu::expr:name@append_menu_item_edit_text"};
        d.examples = json::parse(R"([{"setup": [{"id": "text.add", "target": {"object": {"index": 0}}, "params": {"text": "ABC", "heightMm": 5}}],
           "target": {"volume": {"object": {"index": 0}, "name": "ABC"}}, "params": {"text": "XYZ"}}])");
        c.add(d, run_text_edit, [](const InvokeContext& ctx) -> std::string {
            for (const auto& ov : ctx.targets.volumes)
                if (!ctx.model().objects[ov.first]->volumes[ov.second]->is_text())
                    return "the part is not a text";
            return {};
        });
    }

    // text.listFonts
    {
        ActionDescriptor d;
        d.id      = "text.listFonts";
        d.label   = "Fonts";
        d.summary = "List the installed font face names text.add and text.edit accept, optionally filtered.";
        d.risk    = Risk::Read;
        d.target  = {{"kind", "none"}, {"required", false}};
        d.params  = {{"type", "object"},
                    {"additionalProperties", false},
                    {"properties",
                     {{"query", {{"type", "string"}, {"maxLength", 64}, {"description", "Case-insensitive substring."}}},
                      {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 500}, {"default", 100}}}}}};
        d.effects  = {};
        d.examples = json::array({{{"target", nullptr}, {"params", {{"query", "arial"}}}}});
        c.add(d, run_text_list_fonts);
    }

    // svg.add
    {
        ActionDescriptor d;
        d.id      = "svg.add";
        d.label   = "Add SVG";
        d.summary = "Emboss or engrave an SVG graphic (from a URL) on a side of an object, at a given size and depth.";
        d.target  = {{"kind", "object"}, {"required", true}};
        json props = placement_properties();
        props["url"]     = {{"type", "string"}, {"minLength", 1}, {"maxLength", 2048}, {"description", "http(s) URL of the .svg; the tab's download rules apply."}};
        props["sizeMm"]  = {{"type", "number"}, {"minimum", 0.1}, {"maximum", 1000}, {"default", 30}, {"description", "Largest side of the graphic, mm."}};
        props["depthMm"] = {{"type", "number"}, {"minimum", 0.01}, {"maximum", 1000}, {"default", 1.5}};
        props["type"]    = emboss_type_schema();
        d.params  = {{"type", "object"}, {"additionalProperties", false}, {"required", {"url"}}, {"properties", props}};
        d.effects = {"geometry", "structure"};
        d.long_running = true;
        d.ui      = {"GLGizmosManager.cpp::gizmo::GLGizmoSVG"};
        d.examples = json::array({{{"target", {{"object", {{"index", 0}}}}},
                                   {"params", {{"url", "http://127.0.0.1:8799/fixtures/logo.svg"}, {"sizeMm", 20}, {"depthMm", 0.6}}}}});
        c.add(d, run_svg_add);
    }

    // svg.edit
    {
        ActionDescriptor d;
        d.id      = "svg.edit";
        d.label   = "Edit SVG";
        d.summary = "Change an embossed SVG part: resize it (largest side, or width / height separately), mirror it, change its "
                    "depth, swap in another SVG file (url), or bake it into a plain part. It keeps its place.";
        d.target  = {{"kind", "volume"}, {"required", true}};
        d.params  = {{"type", "object"},
                    {"additionalProperties", false},
                    {"properties",
                     {{"sizeMm", {{"type", "number"}, {"minimum", 0.1}, {"maximum", 1000}, {"description", "Largest side, mm (uniform)."}}},
                      {"widthMm", {{"type", "number"}, {"minimum", 0.1}, {"maximum", 1000},
                                   {"description", "Width along the drawing's own X, mm (the tool's W with the ratio unlocked)."}}},
                      {"heightMm", {{"type", "number"}, {"minimum", 0.1}, {"maximum", 1000}, {"description", "Height along its own Y, mm."}}},
                      {"depthMm", {{"type", "number"}, {"minimum", 0.01}, {"maximum", 1000}}},
                      {"mirror", {{"type", "string"}, {"enum", {"x", "y"}}, {"description", "Flip the drawing along its own X or Y axis."}}},
                      {"url", {{"type", "string"}, {"minLength", 1}, {"maxLength", 2048},
                               {"description", "Change file: another .svg (the tab's download rules apply) at the same scale."}}},
                      {"bake", {{"type", "boolean"},
                                {"description", "true: keep the mesh as an uneditable part and drop the SVG (the tool's Bake). Alone."}}}}}};
        d.effects = {"geometry"};
        d.long_running = true; // with url
        d.ui      = {"GUI_Factories.cpp::menu::expr:name@append_menu_item_edit_svg"};
        d.examples = json::parse(R"([{"setup": [{"id": "svg.add", "target": {"object": {"index": 0}},
                      "params": {"url": "http://127.0.0.1:8799/fixtures/logo.svg", "sizeMm": 12}}],
           "target": {"volume": {"object": {"index": 0}, "name": "logo"}}, "params": {"sizeMm": 15}},
          {"setup": [{"id": "svg.add", "target": {"object": {"index": 0}},
                      "params": {"url": "http://127.0.0.1:8799/fixtures/logo.svg", "sizeMm": 12}}],
           "target": {"volume": {"object": {"index": 0}, "name": "logo"}}, "params": {"widthMm": 20, "mirror": "x"}}])");
        c.add(d, run_svg_edit, [](const InvokeContext& ctx) -> std::string {
            for (const auto& ov : ctx.targets.volumes)
                if (!ctx.model().objects[ov.first]->volumes[ov.second]->is_svg())
                    return "the part is not an SVG";
            return {};
        });
    }
}

}}} // namespace Slic3r::GUI::Actions
