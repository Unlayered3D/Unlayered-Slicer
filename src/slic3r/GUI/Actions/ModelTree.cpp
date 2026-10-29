// slicer.getModelTree (ModelTree.hpp). Owner: work package S2 (objects).
//
// The tree names every entity by its ObjectID ("id"), which survives edits to OTHER objects
// (deleting object 0 renumbers every index but no id), so a page can hold on to "the left one"
// across turns. Indices are reported too, because the older typed methods take them.
//
// Frames: instance offsets, rotations, every bboxMm and the cut planes are in the plate (world)
// frame, in mm and degrees. Cut planes are stored object-local (DECLARATIVE_CUT_PLANES.md) and
// mapped the way GLGizmoCutPlanes shows them, so they read back as slicer.setCutPlanes took them.

#include "ModelTree.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "libslic3r/Geometry.hpp"
#include "libslic3r/Emboss.hpp" // get_extents of emboss shapes
#include "libslic3r/EmbossShape.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TextConfiguration.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "../PartPlate.hpp"
#include "ActionCatalog.hpp"
#include "CutPlaneRules.hpp"

namespace Slic3r { namespace GUI { namespace Actions {

namespace {

// Large projects stay answerable: past this many objects the tree is cut and says so.
constexpr size_t MAX_TREE_OBJECTS = 500;

// Rounded so that float noise from re-deriving the same transform does not change treeVersion.
double r4(double v)
{
    if (!std::isfinite(v))
        return 0.;
    const double r = std::round(v * 1e4) / 1e4;
    return r == 0. ? 0. : r; // no "-0"
}

nlohmann::json vec3_json(const Vec3d& v) { return nlohmann::json::array({r4(v.x()), r4(v.y()), r4(v.z())}); }

nlohmann::json bbox_json(const BoundingBoxf3& bb)
{
    if (!bb.defined)
        return nullptr;
    return {{"min", vec3_json(bb.min)}, {"max", vec3_json(bb.max)}, {"size", vec3_json(bb.size())}};
}

std::vector<std::string> sorted_keys(const ModelConfig& config)
{
    std::vector<std::string> keys = config.keys();
    std::sort(keys.begin(), keys.end());
    return keys;
}

// The filament slot (1-based) a config assigns, or 0 when it does not set one (inherits).
int config_filament(const ModelConfig& config)
{
    if (!config.has("extruder"))
        return 0;
    const ConfigOption* opt = config.option("extruder");
    return opt != nullptr ? opt->getInt() : 0;
}

} // namespace

const char* volume_type_name(ModelVolumeType type)
{
    switch (type) {
    case ModelVolumeType::MODEL_PART: return "part";
    case ModelVolumeType::NEGATIVE_VOLUME: return "negative";
    case ModelVolumeType::PARAMETER_MODIFIER: return "modifier";
    case ModelVolumeType::SUPPORT_BLOCKER: return "blocker";
    case ModelVolumeType::SUPPORT_ENFORCER: return "enforcer";
    case ModelVolumeType::INFILL_MESH: return "infillMesh";
    default: return "invalid";
    }
}

nlohmann::json cut_planes_json(const ModelObject& object)
{
    nlohmann::json planes = nlohmann::json::array();
    if (!object.instances.empty()) {
        const Transform3d m = object.instances.front()->get_matrix();
        for (const CutPlane& cp : object.cut_planes.planes) {
            nlohmann::json plane = {{"point", vec3_json(m * cp.point)},
                                    {"normal", vec3_json(CutPlaneRules::world_normal(m, cp.normal))},
                                    {"enabled", cp.enabled}};
            if (cp.has_build_normal) {
                const Vec3d b  = CutPlaneRules::world_normal(m, cp.build_normal);
                double      bd = 0., cd = 0.;
                Geometry::bc_from_build_normal(b, bd, cd);
                plane["buildNormal"]    = vec3_json(b);
                plane["buildAnglesDeg"] = {{"b", r4(bd)}, {"c", r4(cd)}};
            }
            planes.push_back(std::move(plane));
        }
    }
    return {{"enabled", object.cut_planes.enabled},
            {"active", object.cut_planes.active()},
            {"userOrdered", object.cut_planes.user_ordered},
            {"frame", "world"},
            {"planes", planes}};
}

nlohmann::json build_model_tree(const Model& model, const PartPlateList* plates_c, int plate)
{
    // find_instance is not const, but only reads.
    PartPlateList* plates = const_cast<PartPlateList*>(plates_c);

    nlohmann::json objects   = nlohmann::json::array();
    bool           truncated = false;
    for (size_t o = 0; o < model.objects.size(); ++o) {
        const ModelObject* mo = model.objects[o];
        if (mo == nullptr)
            continue;

        // The plate of each instance; an object is on a plate when any of its instances is.
        std::vector<int> inst_plates(mo->instances.size(), -1);
        if (plates != nullptr)
            for (size_t i = 0; i < mo->instances.size(); ++i)
                inst_plates[i] = plates->find_instance(int(o), int(i));
        if (plate >= 0 && plates != nullptr && std::find(inst_plates.begin(), inst_plates.end(), plate) == inst_plates.end())
            continue;
        if (objects.size() >= MAX_TREE_OBJECTS) {
            truncated = true;
            break;
        }

        nlohmann::json obj = {{"id", mo->id().id},
                              {"index", int(o)},
                              {"name", mo->name},
                              {"printable", mo->printable},
                              {"filament", config_filament(mo->config)},
                              {"overrides", sorted_keys(mo->config)},
                              {"isCut", mo->is_cut()}};
        if (plates != nullptr)
            obj["plate"] = inst_plates.empty() ? -1 : inst_plates.front();
        if (!mo->instances.empty())
            obj["bboxMm"] = bbox_json(mo->instance_bounding_box(0));

        if (!mo->cut_planes.planes.empty())
            obj["cutPlanes"] = cut_planes_json(*mo);

        // Brim ears (the Brim ears tool / object.brimEars), where instance 0 puts them on the plate.
        if (!mo->brim_points.empty() && !mo->instances.empty()) {
            const Transform3d m    = mo->instances.front()->get_matrix();
            nlohmann::json    ears = nlohmann::json::array();
            for (const BrimPoint& bp : mo->brim_points) {
                const Vec3d w = m * bp.pos.cast<double>();
                ears.push_back({{"x", r4(w.x())}, {"y", r4(w.y())}, {"radiusMm", r4(bp.head_front_radius)}});
            }
            obj["brimEars"] = ears;
        }

        nlohmann::json ranges = nlohmann::json::array();
        for (const auto& [range, config] : mo->layer_config_ranges)
            ranges.push_back({{"zMin", r4(range.first)}, {"zMax", r4(range.second)}, {"overrides", sorted_keys(config)}});
        obj["layerRanges"] = ranges;

        nlohmann::json instances = nlohmann::json::array();
        for (size_t i = 0; i < mo->instances.size(); ++i) {
            const ModelInstance* mi   = mo->instances[i];
            const Vec3d          rot  = mi->get_rotation();
            nlohmann::json       inst = {{"id", mi->id().id},
                                         {"index", int(i)},
                                         {"offset", vec3_json(mi->get_offset())},
                                         {"rotationDeg", vec3_json(Vec3d(Geometry::rad2deg(rot.x()), Geometry::rad2deg(rot.y()),
                                                                         Geometry::rad2deg(rot.z())))},
                                         {"scale", vec3_json(mi->get_scaling_factor())},
                                         {"mirrored", mi->is_left_handed()},
                                         {"printable", mi->printable},
                                         {"autoDrop", mi->auto_drop},
                                         {"bboxMm", bbox_json(mo->instance_bounding_box(i))}};
            if (plates != nullptr)
                inst["plate"] = inst_plates[i];
            instances.push_back(std::move(inst));
        }
        obj["instances"] = instances;

        const Transform3d inst0   = mo->instances.empty() ? Transform3d::Identity() : mo->instances.front()->get_matrix();
        nlohmann::json    volumes = nlohmann::json::array();
        for (size_t v = 0; v < mo->volumes.size(); ++v) {
            const ModelVolume* mv  = mo->volumes[v];
            nlohmann::json     vol = {{"id", mv->id().id},
                                      {"index", int(v)},
                                      {"name", mv->name},
                                      {"type", volume_type_name(mv->type())},
                                      {"filament", config_filament(mv->config)},
                                      {"overrides", sorted_keys(mv->config)},
                                      {"paint", {{"support", !mv->supported_facets.empty()},
                                                 {"seam", !mv->seam_facets.empty()},
                                                 {"fuzzy", !mv->fuzzy_skin_facets.empty()},
                                                 {"color", !mv->mmu_segmentation_facets.empty()}}},
                                      {"isText", mv->is_text()},
                                      {"isSvg", mv->is_svg()}};
            if (mv->mesh_ptr() != nullptr && !mo->instances.empty())
                vol["bboxMm"] = bbox_json(mv->mesh().transformed_bounding_box(inst0 * mv->get_matrix()));
            // What the Text / SVG tool shows for an embossed part, so text.edit / svg.edit can be
            // given only what changes.
            if (mv->text_configuration.has_value()) {
                const TextConfiguration& tc = *mv->text_configuration;
                const FontProp&          fp = tc.style.prop;
                nlohmann::json t = {{"text", tc.text},
                                    {"font", fp.face_name.value_or(tc.style.name)},
                                    {"heightMm", r4(fp.size_in_mm)},
                                    {"charGap", fp.char_gap.value_or(0)},
                                    {"lineGap", fp.line_gap.value_or(0)},
                                    {"boldness", r4(fp.boldness.value_or(0.f))},
                                    {"skew", r4(fp.skew.value_or(0.f))},
                                    {"alignH", fp.align.first == FontProp::HorizontalAlign::left    ? "left" :
                                               fp.align.first == FontProp::HorizontalAlign::right   ? "right" : "center"},
                                    {"alignV", fp.align.second == FontProp::VerticalAlign::top      ? "top" :
                                               fp.align.second == FontProp::VerticalAlign::bottom   ? "bottom" : "center"},
                                    {"fontIndex", fp.collection_number.value_or(0)}};
                if (fp.style.has_value())
                    t["fontStyle"] = *fp.style;
                if (fp.weight.has_value())
                    t["fontWeight"] = *fp.weight;
                if (mv->emboss_shape.has_value()) {
                    t["depthMm"]    = r4(mv->emboss_shape->projection.depth);
                    t["useSurface"] = mv->emboss_shape->projection.use_surface;
                }
                vol["text"] = std::move(t);
            } else if (mv->emboss_shape.has_value() && mv->emboss_shape->svg_file.has_value()) {
                const EmbossShape& es = *mv->emboss_shape;
                nlohmann::json     g  = {{"depthMm", r4(es.projection.depth)}, {"useSurface", es.projection.use_surface}};
                if (!es.shapes_with_ids.empty()) {
                    const BoundingBox bb = get_extents(es.shapes_with_ids);
                    const Vec3d       sc = mv->get_scaling_factor();
                    g["sizeMm"] = nlohmann::json::array({r4(bb.size().x() * es.scale * std::abs(sc.x())), r4(bb.size().y() * es.scale * std::abs(sc.y()))});
                }
                g["mirrored"] = mv->is_left_handed();
                vol["svg"]    = std::move(g);
            }
            volumes.push_back(std::move(vol));
        }
        obj["volumes"] = volumes;
        objects.push_back(std::move(obj));
    }

    nlohmann::json tree = {{"objects", objects},
                           {"objectCount", model.objects.size()},
                           {"truncated", truncated},
                           {"treeVersion", tree_version(model)}};
    if (plate >= 0)
        tree["plate"] = plate;
    if (plates != nullptr) {
        tree["currentPlate"] = plates->get_curr_plate_index();
        tree["plateCount"]   = plates->get_plate_count();
    }
    return tree;
}

std::string tree_version(const Model& model)
{
    // Everything the tree reports that can change, as one compact canonical string, hashed.
    // Cheaper than hashing the tree JSON (no per-volume mesh bounding boxes): a volume's box only
    // changes with its matrix or its mesh, which the matrix and the mesh statistics stand for.
    std::string s;
    s.reserve(256 * (model.objects.size() + 1));
    auto num = [&s](double v) {
        char buf[40];
        snprintf(buf, sizeof(buf), "%.4f,", r4(v));
        s += buf;
    };
    auto mat = [&num](const Transform3d& m) {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                num(m(r, c));
    };
    auto keys = [&s](const ModelConfig& config) {
        for (const std::string& k : sorted_keys(config)) {
            s += k;
            s += '=';
            if (const ConfigOption* opt = config.option(k); opt != nullptr)
                s += opt->serialize();
            s += ';';
        }
    };
    for (const ModelObject* mo : model.objects) {
        if (mo == nullptr)
            continue;
        s += "O" + std::to_string(mo->id().id) + "|" + mo->name + "|";
        s += mo->printable ? "p" : "n";
        s += mo->is_cut() ? "c|" : "-|";
        keys(mo->config);
        for (const CutPlane& cp : mo->cut_planes.planes) {
            num(cp.point.x());
            num(cp.point.y());
            num(cp.point.z());
            num(cp.normal.x());
            num(cp.normal.y());
            num(cp.normal.z());
            s += cp.enabled ? "e" : "d";
            if (cp.has_build_normal) {
                s += "b";
                num(cp.build_normal.x());
                num(cp.build_normal.y());
                num(cp.build_normal.z());
            }
        }
        s += mo->cut_planes.enabled ? "E" : "D";
        s += mo->cut_planes.user_ordered ? "U" : "A";
        for (const BrimPoint& bp : mo->brim_points) {
            s += "B";
            num(bp.pos.x());
            num(bp.pos.y());
            num(bp.pos.z());
            num(bp.head_front_radius);
        }
        for (const auto& [range, config] : mo->layer_config_ranges) {
            num(range.first);
            num(range.second);
            keys(config);
        }
        for (const ModelInstance* mi : mo->instances) {
            s += "I" + std::to_string(mi->id().id);
            s += mi->printable ? "p" : "n";
            s += mi->auto_drop ? "a" : "-";
            mat(mi->get_matrix());
        }
        for (const ModelVolume* mv : mo->volumes) {
            s += "V" + std::to_string(mv->id().id) + "|" + mv->name + "|" + std::to_string(int(mv->type())) + "|";
            keys(mv->config);
            mat(mv->get_matrix());
            if (mv->mesh_ptr() != nullptr) {
                s += std::to_string(mv->mesh().facets_count()) + ",";
                num(mv->mesh().stats().volume);
            }
            // Paint by content, not by timestamp: an undo restores the same strokes, and the version
            // must come back with them.
            for (const FacetsAnnotation* fa : {&mv->supported_facets, &mv->seam_facets, &mv->fuzzy_skin_facets, &mv->mmu_segmentation_facets}) {
                const auto& data = fa->get_data();
                s += "t" + std::to_string(data.triangles_to_split.size()) + "." + std::to_string(data.bitstream.size()) + "." +
                     std::to_string(std::hash<std::vector<bool>>{}(data.bitstream));
            }
        }
        s += '\n';
    }
    return fnv1a_hex(s);
}

}}} // namespace Slic3r::GUI::Actions
