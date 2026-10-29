#include "ActionTargets.hpp"

#include <algorithm>
#include <set>

#include "libslic3r/Model.hpp"

#include "../3DScene.hpp"
#include "../GLCanvas3D.hpp"
#include "../GUI_App.hpp"
#include "../GUI_ObjectList.hpp"
#include "../PartPlate.hpp"
#include "../Plater.hpp"
#include "../Selection.hpp"
#include "../WebAppBridge.hpp"

namespace Slic3r { namespace GUI { namespace Actions {

using json = nlohmann::json;

int object_index_by_id(const Model& model, size_t id)
{
    for (size_t i = 0; i < model.objects.size(); ++i)
        if (model.objects[i]->id().id == id)
            return int(i);
    return -1;
}

std::pair<int, int> volume_index_by_id(const Model& model, size_t id)
{
    for (size_t i = 0; i < model.objects.size(); ++i)
        for (size_t v = 0; v < model.objects[i]->volumes.size(); ++v)
            if (model.objects[i]->volumes[v]->id().id == id)
                return {int(i), int(v)};
    return {-1, -1};
}

std::pair<int, int> instance_index_by_id(const Model& model, size_t id)
{
    for (size_t i = 0; i < model.objects.size(); ++i)
        for (size_t k = 0; k < model.objects[i]->instances.size(); ++k)
            if (model.objects[i]->instances[k]->id().id == id)
                return {int(i), int(k)};
    return {-1, -1};
}

json object_ref_json(const Model& model, int obj_idx)
{
    if (obj_idx < 0 || obj_idx >= int(model.objects.size()))
        return nullptr;
    const ModelObject* o = model.objects[obj_idx];
    return {{"id", o->id().id}, {"index", obj_idx}, {"name", o->name}};
}

json volume_ref_json(const Model& model, int obj_idx, int vol_idx)
{
    if (obj_idx < 0 || obj_idx >= int(model.objects.size()) || vol_idx < 0 ||
        vol_idx >= int(model.objects[obj_idx]->volumes.size()))
        return nullptr;
    const ModelVolume* v = model.objects[obj_idx]->volumes[vol_idx];
    return {{"id", v->id().id}, {"index", vol_idx}, {"name", v->name}, {"object", object_ref_json(model, obj_idx)}};
}

namespace {

const char* const OBJECT_REF_HELP = "an object reference is {\"id\"}, {\"index\"} or {\"name\"}";
const char* const VOLUME_REF_HELP = "a part reference is {\"id\"} or {\"object\", \"index\"|\"name\"}";

struct Resolver
{
    const Model& model;
    std::string& message;
    json&        data;

    int bad(const std::string& path, const std::string& why)
    {
        message      = why;
        data["path"] = path;
        return WAB_BAD_PARAMS;
    }
    int missing(const std::string& path, const std::string& why)
    {
        message         = why;
        data["missing"] = path;
        return WAB_NOT_FOUND;
    }

    int object(const json& ref, const std::string& path, int& out)
    {
        out = -1;
        if (!ref.is_object() || ref.size() != 1)
            return bad(path, OBJECT_REF_HELP);
        if (ref.contains("id")) {
            if (!ref["id"].is_number_unsigned())
                return bad(path + "/id", "id must be a non-negative integer");
            out = object_index_by_id(model, ref["id"].get<size_t>());
        } else if (ref.contains("index")) {
            if (!ref["index"].is_number_integer())
                return bad(path + "/index", "index must be an integer");
            const int i = ref["index"].get<int>();
            out         = (i >= 0 && i < int(model.objects.size())) ? i : -1;
        } else if (ref.contains("name")) {
            if (!ref["name"].is_string())
                return bad(path + "/name", "name must be a string");
            const std::string name = ref["name"].get<std::string>();
            for (size_t i = 0; i < model.objects.size(); ++i)
                if (model.objects[i]->name == name) {
                    if (out >= 0)
                        return bad(path + "/name", "more than one object is named '" + name + "'; use its id");
                    out = int(i);
                }
        } else {
            return bad(path, OBJECT_REF_HELP);
        }
        if (out < 0)
            return missing(path, "no such object");
        return WAB_OK;
    }

    int volume(const json& ref, const std::string& path, std::pair<int, int>& out)
    {
        out = {-1, -1};
        if (!ref.is_object())
            return bad(path, VOLUME_REF_HELP);
        if (ref.contains("id")) {
            if (ref.size() != 1 || !ref["id"].is_number_unsigned())
                return bad(path, VOLUME_REF_HELP);
            out = volume_index_by_id(model, ref["id"].get<size_t>());
            return out.first < 0 ? missing(path, "no such part") : WAB_OK;
        }
        if (!ref.contains("object") || ref.size() != 2)
            return bad(path, VOLUME_REF_HELP);
        int obj = -1;
        if (const int code = object(ref["object"], path + "/object", obj); code != WAB_OK)
            return code;
        const ModelObject* o = model.objects[obj];
        if (ref.contains("index")) {
            if (!ref["index"].is_number_integer())
                return bad(path + "/index", "index must be an integer");
            const int v = ref["index"].get<int>();
            if (v < 0 || v >= int(o->volumes.size()))
                return missing(path, "no such part");
            out = {obj, v};
        } else if (ref.contains("name") && ref["name"].is_string()) {
            const std::string name = ref["name"].get<std::string>();
            for (size_t v = 0; v < o->volumes.size() && out.first < 0; ++v)
                if (o->volumes[v]->name == name)
                    out = {obj, int(v)};
            if (out.first < 0)
                return missing(path, "no such part");
        } else {
            return bad(path, VOLUME_REF_HELP);
        }
        return WAB_OK;
    }

    int instance(const json& ref, const std::string& path, std::pair<int, int>& out)
    {
        out = {-1, -1};
        if (ref.is_object() && ref.size() == 1 && ref.contains("id") && ref["id"].is_number_unsigned()) {
            out = instance_index_by_id(model, ref["id"].get<size_t>());
            return out.first < 0 ? missing(path, "no such instance") : WAB_OK;
        }
        if (!ref.is_object() || ref.size() != 2 || !ref.contains("object") || !ref.contains("index") ||
            !ref["index"].is_number_integer())
            return bad(path, "an instance reference is {\"id\"} or {\"object\", \"index\"}");
        int obj = -1;
        if (const int code = object(ref["object"], path + "/object", obj); code != WAB_OK)
            return code;
        const int k = ref["index"].get<int>();
        if (k < 0 || k >= int(model.objects[obj]->instances.size()))
            return missing(path, "no such instance");
        out = {obj, k};
        return WAB_OK;
    }
};

// "objects": [...] or the singular sugar "object": {...}; null when neither is present.
const json* list_or_single(const json& target, const char* plural, const char* singular, json& holder, std::string& path)
{
    if (target.contains(plural)) {
        path = std::string("/target/") + plural;
        return &target[plural];
    }
    if (target.contains(singular)) {
        holder = json::array({target[singular]});
        path   = std::string("/target/") + singular;
        return &holder;
    }
    return nullptr;
}

template<class T> void sort_unique(std::vector<T>& v)
{
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

} // namespace

bool target_accepts(const json& spec, const std::string& kind)
{
    if (!spec.is_object())
        return false;
    if (spec.value("kind", std::string()) == kind)
        return true;
    const auto it = spec.find("accepts");
    if (it == spec.end() || !it->is_array())
        return false;
    for (const json& k : *it)
        if (k.is_string() && k.get<std::string>() == kind)
            return true;
    return false;
}

int resolve_targets(const Model& model, int plate_count, const json& spec, const json& target, const ResolvedTargets* selection,
                    ResolvedTargets& out, std::string& message, json& data)
{
    out                     = ResolvedTargets{};
    data                    = json::object();
    const std::string kind  = spec.is_object() ? spec.value("kind", std::string("none")) : std::string("none");
    const bool        required = spec.is_object() ? spec.value("required", kind != "none") : false;
    Resolver          r{model, message, data};

    if (!target.is_null() && !target.is_object())
        return r.bad("/target", "target must be an object");

    if (target.is_object()) {
        static const std::set<std::string> keys = {"objects", "object", "volumes", "volume", "instances",
                                                   "instance", "plate", "selection"};
        for (auto it = target.begin(); it != target.end(); ++it)
            if (keys.count(it.key()) == 0)
                return r.bad("/target/" + it.key(), "unknown target key '" + it.key() + "'");

        if (target.contains("selection")) {
            if (!target["selection"].is_boolean())
                return r.bad("/target/selection", "selection must be true or false");
            if (target["selection"].get<bool>()) {
                if (selection == nullptr)
                    return r.bad("/target/selection", "the selection is not available here");
                out = *selection;
                out.plate = -1;
            }
        }
        std::string path;
        json        holder_o, holder_v, holder_i;
        if (const json* list = list_or_single(target, "objects", "object", holder_o, path)) {
            if (!list->is_array() || list->empty() || list->size() > 1000)
                return r.bad(path, "must be a non-empty list of object references");
            for (size_t i = 0; i < list->size(); ++i) {
                int obj = -1;
                if (const int code = r.object((*list)[i], path + "/" + std::to_string(i), obj); code != WAB_OK)
                    return code;
                out.objects.push_back(obj);
            }
        }
        if (const json* list = list_or_single(target, "volumes", "volume", holder_v, path)) {
            if (!list->is_array() || list->empty() || list->size() > 1000)
                return r.bad(path, "must be a non-empty list of part references");
            for (size_t i = 0; i < list->size(); ++i) {
                std::pair<int, int> v;
                if (const int code = r.volume((*list)[i], path + "/" + std::to_string(i), v); code != WAB_OK)
                    return code;
                out.volumes.push_back(v);
            }
        }
        if (const json* list = list_or_single(target, "instances", "instance", holder_i, path)) {
            if (!list->is_array() || list->empty() || list->size() > 1000)
                return r.bad(path, "must be a non-empty list of instance references");
            for (size_t i = 0; i < list->size(); ++i) {
                std::pair<int, int> k;
                if (const int code = r.instance((*list)[i], path + "/" + std::to_string(i), k); code != WAB_OK)
                    return code;
                out.instances.push_back(k);
            }
        }
        if (target.contains("plate")) {
            if (!target["plate"].is_number_integer())
                return r.bad("/target/plate", "plate must be an integer index");
            const int p = target["plate"].get<int>();
            if (p < 0 || p >= plate_count)
                return r.missing("/target/plate",
                                 "no plate " + std::to_string(p) + " (" + std::to_string(plate_count) + " plates)");
            out.plate = p;
        }
    }

    sort_unique(out.objects);
    sort_unique(out.volumes);
    sort_unique(out.instances);

    // Implied targets, as the typed methods do: a plate holding exactly one object implies it; one
    // object with a single part implies the part; objects imply all of their instances.
    if ((kind == "objects" || kind == "object") && out.objects.empty() && out.volumes.empty() && out.instances.empty() &&
        model.objects.size() == 1)
        out.objects.push_back(0);
    if ((kind == "volumes" || kind == "volume") && out.volumes.empty() && out.objects.empty() && model.objects.size() == 1 &&
        model.objects.front()->volumes.size() == 1)
        out.volumes.push_back({0, 0});
    if ((kind == "volumes" || kind == "volume") && out.volumes.empty() && out.objects.size() == 1 &&
        model.objects[out.objects.front()]->volumes.size() == 1) {
        out.volumes.push_back({out.objects.front(), 0});
        out.objects.clear();
    }
    if (kind == "instances" && out.instances.empty() && !out.objects.empty()) {
        for (int o : out.objects)
            for (size_t k = 0; k < model.objects[o]->instances.size(); ++k)
                out.instances.push_back({o, int(k)});
        out.objects.clear();
    }

    if (!required)
        return WAB_OK;
    if (kind == "objects" || kind == "object") {
        // An object action that also acts on parts or single instances (spec "accepts") is
        // satisfied by those; the runner then works on whatever was named and selected.
        const bool other = (target_accepts(spec, "volumes") && !out.volumes.empty()) ||
                           (target_accepts(spec, "instances") && !out.instances.empty());
        if (out.objects.empty() && other)
            return WAB_OK;
        if (out.objects.empty())
            return r.bad("/target/objects", model.objects.empty() ? "the plate is empty" :
                                            target_accepts(spec, "volumes") || target_accepts(spec, "instances") ?
                                                                            "name the object(s), part(s) or instance(s) in the target" :
                                                                            "name the object(s) in target.objects");
        if (kind == "object" && out.objects.size() != 1)
            return r.bad("/target/objects", "this action takes exactly one object");
    } else if (kind == "volumes" || kind == "volume") {
        if (out.volumes.empty())
            return r.bad("/target/volumes", "name the part(s) in target.volumes");
        if (kind == "volume" && out.volumes.size() != 1)
            return r.bad("/target/volumes", "this action takes exactly one part");
    } else if (kind == "instances") {
        if (out.instances.empty())
            return r.bad("/target/instances", "name the instance(s) in target.instances");
    } else if (kind == "plate") {
        if (out.plate < 0)
            return r.bad("/target/plate", "name the plate in target.plate");
    }
    return WAB_OK;
}

// ---------------------------------------------------------------------------------------------
// GUI side
// ---------------------------------------------------------------------------------------------

ResolvedTargets current_selection()
{
    ResolvedTargets t;
    Plater*         plater = wxGetApp().plater();
    if (plater == nullptr || plater->canvas3D() == nullptr)
        return t;
    const Selection& sel = plater->canvas3D()->get_selection();
    t.plate              = plater->get_partplate_list().get_curr_plate_index();
    if (sel.is_empty() || sel.is_wipe_tower())
        return t;
    const Model& model = plater->model();
    if (sel.is_single_full_object() || sel.is_multiple_full_object()) {
        for (const auto& entry : sel.get_content())
            t.objects.push_back(entry.first);
    } else if (sel.is_any_volume()) {
        std::set<std::pair<int, int>> vols;
        for (unsigned int idx : sel.get_volume_idxs())
            if (const GLVolume* v = sel.get_volume(idx); v != nullptr && v->object_idx() >= 0 && v->volume_idx() >= 0)
                vols.insert({v->object_idx(), v->volume_idx()});
        t.volumes.assign(vols.begin(), vols.end());
    } else {
        for (const auto& entry : sel.get_content())
            for (int k : entry.second)
                t.instances.push_back({entry.first, k});
        // Every instance of an object selected is the object.
        std::set<int> objs;
        for (const auto& p : t.instances)
            objs.insert(p.first);
        bool whole = !objs.empty();
        for (int o : objs) {
            size_t n = 0;
            for (const auto& p : t.instances)
                n += p.first == o ? 1 : 0;
            whole = whole && o >= 0 && o < int(model.objects.size()) && n == model.objects[o]->instances.size();
        }
        if (whole) {
            t.objects.assign(objs.begin(), objs.end());
            t.instances.clear();
        }
    }
    return t;
}

void select_targets(const ResolvedTargets& t)
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr || plater->canvas3D() == nullptr)
        return;
    if (t.objects.empty() && t.volumes.empty() && t.instances.empty())
        return;
    Selection& sel   = plater->canvas3D()->get_selection();
    bool       first = true;
    for (int o : t.objects) {
        sel.add_object(unsigned(o), first);
        first = false;
    }
    for (const auto& v : t.volumes) {
        sel.add_volume(unsigned(v.first), unsigned(v.second), 0, first);
        first = false;
    }
    for (const auto& k : t.instances) {
        sel.add_instance(unsigned(k.first), unsigned(k.second), first);
        first = false;
    }
    if (ObjectList* list = wxGetApp().obj_list(); list != nullptr)
        list->update_selections();
}

}}} // namespace Slic3r::GUI::Actions
