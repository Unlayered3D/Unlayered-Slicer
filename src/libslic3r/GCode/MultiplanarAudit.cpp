#include "MultiplanarAudit.hpp"

#include <boost/format.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <sstream>

namespace Slic3r {

namespace {

enum class Kind : uint8_t { Model, Support, Span /* modelled span: bridge, overhang wall, sparse infill */, Skip };

Kind kind_of(ExtrusionRole r)
{
    switch (r) {
    case erSupportMaterial:
    case erSupportMaterialInterface:
    case erSupportTransition:     return Kind::Support;
    case erBridgeInfill:
    case erInternalBridgeInfill:
    case erOverhangPerimeter:
    case erInternalInfill:        return Kind::Span;
    case erSkirt:
    case erBrim:
    case erWipeTower:
    case erCustom:
    case erNone:
    case erMixed:
    case erCount:                 return Kind::Skip;
    default:                      return Kind::Model;
    }
}

struct Sample
{
    Vec3f    p;           // nozzle tip (top of the bead)
    float    bottom;      // p.z - layer thickness
    float    width;
    Kind     kind;
    int      object_label;
    int      segment;
    unsigned layer_id;
    unsigned gcode_id;
};

// Uniform XY grid over the extrusion bounds. Samples are appended in deposition order, so everything
// already in the grid is EARLIER than whatever is being queried.
class Grid
{
public:
    Grid(const Vec2f &min, const Vec2f &max, float cell) : m_min(min), m_cell(cell)
    {
        m_nx = std::max(1, int(std::ceil((max.x() - min.x()) / cell)) + 1);
        m_ny = std::max(1, int(std::ceil((max.y() - min.y()) / cell)) + 1);
        m_cells.resize(size_t(m_nx) * size_t(m_ny));
    }
    void insert(uint32_t idx, const Vec3f &p) { m_cells[cell_of(p)].push_back(idx); }

    // Highest top (z) of earlier samples within `r` of `p` (XY) whose top is not above `z_max`.
    // Returns -inf when none; `kind_out` is the kind of that sample.
    // With `skip` set, samples of that (object, section) are ignored -- used to look only at what an
    // EARLIER section left at a transition, not at the current section's own neighbouring beads.
    float highest_below(const std::vector<Sample> &s, const Vec3f &p, float r, float z_max, Kind &kind_out,
                        const Sample *skip = nullptr) const
    {
        float best = -std::numeric_limits<float>::infinity();
        const int x0 = ix(p.x() - r), x1 = ix(p.x() + r), y0 = iy(p.y() - r), y1 = iy(p.y() + r);
        const float r2 = r * r;
        for (int y = y0; y <= y1; ++ y)
            for (int x = x0; x <= x1; ++ x)
                for (uint32_t i : m_cells[size_t(y) * size_t(m_nx) + size_t(x)]) {
                    const Sample &q = s[i];
                    if (q.p.z() > z_max || q.p.z() <= best)
                        continue;
                    if (skip != nullptr && q.object_label == skip->object_label && q.segment == skip->segment)
                        continue;
                    const float dx = q.p.x() - p.x(), dy = q.p.y() - p.y();
                    if (dx * dx + dy * dy > r2)
                        continue;
                    best = q.p.z();
                    kind_out = q.kind;
                }
        return best;
    }

private:
    int    ix(float x) const { return std::clamp(int((x - m_min.x()) / m_cell), 0, m_nx - 1); }
    int    iy(float y) const { return std::clamp(int((y - m_min.y()) / m_cell), 0, m_ny - 1); }
    size_t cell_of(const Vec3f &p) const { return size_t(iy(p.y())) * size_t(m_nx) + size_t(ix(p.x())); }

    Vec2f m_min;
    float m_cell;
    int   m_nx, m_ny;
    std::vector<std::vector<uint32_t>> m_cells;
};

constexpr float kSpanRadius = 1.5f;   // mm: material at the right height this close counts as a span anchor

double percentile(std::vector<double> v, double q)
{
    if (v.empty()) return 0.;
    std::sort(v.begin(), v.end());
    const size_t i = std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5));
    return v[i];
}

std::string f3(double v) { return (boost::format("%.3f") % v).str(); }

bool has_rotary_motion_word(const std::string &line)
{
    // Firmware accepts compact words, zero-padded commands and optional line numbers. Comments and
    // checksums are not commands: remove them before examining G0/G1/G2/G3 and their axis words.
    std::string code;
    int comment_depth = 0;
    for (char c : line) {
        if (c == '(') { ++ comment_depth; code += ' '; continue; }
        if (c == ')' && comment_depth > 0) { -- comment_depth; continue; }
        if (comment_depth > 0) continue;
        if (c == ';' || c == '*') break;
        code += char(std::toupper(static_cast<unsigned char>(c)));
    }
    auto digit = [](char c) { return c >= '0' && c <= '9'; };
    size_t pos = 0;
    auto skip_space = [&]() {
        while (pos < code.size() && std::isspace(static_cast<unsigned char>(code[pos]))) ++ pos;
    };
    skip_space();
    if (pos < code.size() && code[pos] == 'N') {
        ++ pos;
        const size_t start = pos;
        while (pos < code.size() && digit(code[pos])) ++ pos;
        if (pos == start) return false;
        skip_space();
    }
    if (pos == code.size() || code[pos++] != 'G') return false;
    const size_t start = pos;
    int command = 0;
    while (pos < code.size() && digit(code[pos])) {
        command = std::min(10, command * 10 + code[pos] - '0');
        ++ pos;
    }
    if (pos == start || command > 3 || (pos < code.size() && code[pos] == '.')) return false;
    for (; pos < code.size(); ++ pos) {
        if (code[pos] != 'A' && code[pos] != 'B' && code[pos] != 'C') continue;
        size_t number = pos + 1;
        if (number < code.size() && (code[number] == '+' || code[number] == '-')) ++ number;
        if (number < code.size() && code[number] == '.') ++ number;
        if (number < code.size() && digit(code[number])) return true;
    }
    return false;
}

} // namespace

MultiplanarAuditResult audit_multiplanar_toolpaths(const GCodeProcessorResult &result, const std::string &gcode_path,
                                                   const MultiplanarAuditParams &params)
{
    MultiplanarAuditResult out;
    out.multiplanar = result.tool_axes.size() > 1;

    // ---- sample every extrusion, in order ----
    std::vector<Sample>  samples;
    std::vector<size_t>  move_first;   // first sample index of each sampled move, to query before inserting
    Vec2f lo( std::numeric_limits<float>::max(),  std::numeric_limits<float>::max());
    Vec2f hi(-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max());
    for (size_t i = 1; i < result.moves.size(); ++ i) {
        const auto &m = result.moves[i];
        if (m.type != EMoveType::Extrude || m.delta_extruder <= 0.f)
            continue;
        const Kind k = kind_of(m.extrusion_role);
        if (k == Kind::Skip)
            continue;
        const Vec3f a = result.moves[i - 1].position, b = m.position;
        const float len = (b - a).norm();
        const int   n   = std::max(1, int(std::ceil(len / float(params.sample_step_mm))));
        move_first.push_back(samples.size());

        for (int j = 1; j <= n; ++ j) {
            const Vec3f p = a + (b - a) * (float(j) / float(n));
            samples.push_back({ p, p.z() - std::max(0.01f, m.height), std::max(0.1f, m.width), k,
                                m.object_label_id, m.cut_segment_id, m.layer_id, m.gcode_id });
            lo = lo.cwiseMin(p.head<2>()); hi = hi.cwiseMax(p.head<2>());
        }
    }
    move_first.push_back(samples.size());

    // Separate objects by XY connectivity on a 2 mm grid, so per-object transition and seam figures
    // do not merge objects printed in the same file.
    // Always by position: label ids are not guaranteed to be present or distinct per object, while
    // separately placed objects are always separated in XY.
    const float cell = 2.f;
    const int   nx = samples.empty() ? 1 : std::max(1, int((hi.x() - lo.x()) / cell) + 1);
    const int   ny = samples.empty() ? 1 : std::max(1, int((hi.y() - lo.y()) / cell) + 1);
    std::vector<int> grid(size_t(nx) * size_t(ny), -2);
    auto at = [&](const Vec3f &p) { return size_t(std::clamp(int((p.y() - lo.y()) / cell), 0, ny - 1)) * size_t(nx) + size_t(std::clamp(int((p.x() - lo.x()) / cell), 0, nx - 1)); };
    auto cluster_of = [&](const Vec3f &p) { return samples.empty() ? -1 : grid[at(p)]; };
    if (! samples.empty()) {
        for (const Sample &s : samples) grid[at(s.p)] = -1;
        int label = 0;
        for (size_t c = 0; c < grid.size(); ++ c) {
            if (grid[c] != -1) continue;
            std::vector<size_t> st { c }; grid[c] = label;
            while (! st.empty()) {
                const size_t q = st.back(); st.pop_back();
                const int qx = int(q % size_t(nx)), qy = int(q / size_t(nx));
                for (int dy = -1; dy <= 1; ++ dy) for (int dx = -1; dx <= 1; ++ dx) {
                    const int x = qx + dx, y = qy + dy;
                    if (x < 0 || y < 0 || x >= nx || y >= ny) continue;
                    const size_t r = size_t(y) * size_t(nx) + size_t(x);
                    if (grid[r] == -1) { grid[r] = label; st.push_back(r); }
                }
            }
            ++ label;
        }
        for (Sample &s : samples) s.object_label = grid[at(s.p)];
    }

    // First layer of every (object, section), for the transition window.
    std::map<std::pair<int, int>, unsigned> first_layer;
    for (const Sample &s : samples) {
        auto key = std::make_pair(s.object_label, s.segment);
        auto it  = first_layer.find(key);
        if (it == first_layer.end() || s.layer_id < it->second)
            first_layer[key] = s.layer_id;
    }

    std::map<std::pair<int, int>, TransitionStats> trans;
    std::map<std::pair<int, int>, std::vector<double>> trans_gaps;

    if (! samples.empty()) {
        Grid grid(lo, hi, 0.5f);
        const float tol = float(params.gap_tolerance_mm);
        for (size_t mv = 0; mv + 1 < move_first.size(); ++ mv) {
            const size_t b = move_first[mv], e = move_first[mv + 1];
            for (size_t i = b; i < e; ++ i) {
                const Sample &s = samples[i];
                const float   z_max = s.p.z() - 0.5f * (s.p.z() - s.bottom);   // below this bead's mid-height
                Kind kn = Kind::Model, kw = Kind::Model;
                // Reach: this bead's half-width plus half a typical bead below. Using this bead's width
                // alone missed the bead underneath a narrow line (gap fill, ~0.24 mm) whenever it fell
                // between the beads below and reported a full-layer gap that is not there.
                const float reach    = std::max(float(params.reach_factor) * s.width, 0.5f * s.width + 0.25f);
                const float near_top = grid.highest_below(samples, s.p, reach, z_max, kn);
                const float wide_top = grid.highest_below(samples, s.p, kSpanRadius, z_max, kw);

                auto allowance = [&](Kind below) -> float {
                    if (s.kind == Kind::Support && (below == Kind::Model || below == Kind::Span)) return float(params.support_bottom_z_mm);
                    if (s.kind != Kind::Support && below == Kind::Support)                       return float(params.support_top_z_mm);
                    return 0.f;
                };
                const bool  on_bed   = s.bottom <= float(params.bed_tolerance_mm);
                const float gap_near = std::isfinite(near_top) ? s.bottom - near_top : s.bottom;
                const float gap_wide = std::isfinite(wide_top) ? s.bottom - wide_top : s.bottom;
                const bool  founded  = on_bed || (std::isfinite(near_top) && gap_near <= allowance(kn) + tol);
                const bool  spanned  = ! founded && std::isfinite(wide_top) && gap_wide <= allowance(kw) + tol;

                FoundationStats &fs = s.kind == Kind::Support ? out.support : out.model;
                ++ fs.samples;
                if (! founded) {
                    if (spanned || s.kind == Kind::Span) {
                        ++ fs.bridged;
                    } else {
                        ++ fs.unfounded;
                        const double g = std::min(gap_near, gap_wide);
                        if (g > fs.worst_gap_mm) { fs.worst_gap_mm = g; fs.worst_at = s.p; fs.worst_gcode_id = s.gcode_id; }
                        fs.worst.push_back({ s.p, g, s.object_label, s.segment, int(s.kind), s.gcode_id, s.layer_id });
                    }
                }

                // Transition window: model samples in the first layers of a non-root section.
                if (s.segment > 0 && s.kind != Kind::Support) {
                    const auto key = std::make_pair(s.object_label, s.segment);
                    if (s.layer_id < first_layer[key] + unsigned(std::max(1, params.transition_layers))) {
                        TransitionStats &t = trans[key];
                        t.object_label = s.object_label; t.segment = s.segment;
                        ++ t.samples;
                        const float allow = std::isfinite(near_top) ? allowance(kn) : 0.f;
                        const double g = double(gap_near) - double(allow);
                        trans_gaps[key].push_back(std::max(0., g));
                        if (g > tol)  {
                            ++ t.voids;    t.max_gap_mm     = std::max(t.max_gap_mm, g);
                            t.worst_voids.push_back({ s.p, g, s.object_label, s.segment, int(s.kind), s.gcode_id, s.layer_id });
                            if (t.worst_voids.size() > 64) {
                                std::sort(t.worst_voids.begin(), t.worst_voids.end(), [](const UnfoundedSample &l, const UnfoundedSample &r) { return l.gap_mm > r.gap_mm; });
                                t.worst_voids.resize(10);
                            }
                        }
                        // Overlap: material of an EARLIER section standing inside this bead's own height.
                        Kind ko = Kind::Model;
                        const float other_top = grid.highest_below(samples, s.p, reach,
                                                                   s.p.z() - 0.01f, ko, &s);
                        if (std::isfinite(other_top) && other_top > s.bottom + tol) {
                            ++ t.overlaps;
                            t.max_overlap_mm = std::max(t.max_overlap_mm, double(other_top - s.bottom));
                        }
                    }
                }
            }
            for (size_t i = b; i < e; ++ i)
                grid.insert(uint32_t(i), samples[i].p);
        }
    }
    for (FoundationStats *fs : { &out.model, &out.support }) {
        std::sort(fs->worst.begin(), fs->worst.end(), [](const UnfoundedSample &l, const UnfoundedSample &r) { return l.gap_mm > r.gap_mm; });
        if (fs->worst.size() > 25) fs->worst.resize(25);
    }
    for (auto &[key, t] : trans) {
        std::sort(t.worst_voids.begin(), t.worst_voids.end(), [](const UnfoundedSample &l, const UnfoundedSample &r) { return l.gap_mm > r.gap_mm; });
        if (t.worst_voids.size() > 10) t.worst_voids.resize(10);
        t.p95_gap_mm = percentile(trans_gaps[key], 0.95);
        out.transitions.push_back(t);
    }

    // ---- seams: where each layer's first outer wall starts, per (object, section) ----
    struct LayerSeam { int obj, seg; unsigned layer; Vec3f p; };
    std::vector<LayerSeam> seams;
    {
        int      cur_obj = -2, cur_seg = -2;
        unsigned cur_layer = ~0u;
        bool     taken = false;
        for (size_t i = 1; i < result.moves.size(); ++ i) {
            const auto &m = result.moves[i];
            if (m.type != EMoveType::Extrude || m.delta_extruder <= 0.f)
                continue;
            const int obj = cluster_of(m.position);
            if (obj != cur_obj || m.cut_segment_id != cur_seg || m.layer_id != cur_layer) {
                cur_obj = obj; cur_seg = m.cut_segment_id; cur_layer = m.layer_id; taken = false;
            }
            if (! taken && m.extrusion_role == erExternalPerimeter) {
                seams.push_back({ cur_obj, cur_seg, cur_layer, result.moves[i - 1].position });
                taken = true;
            }
        }
    }
    // A transition is where, for one object, the section changes between that object's consecutive
    // seam records (objects interleave in "by group" order, so neighbours in the file are not enough).
    std::map<int, LayerSeam> last_of;
    for (const LayerSeam &b : seams) {
        auto it = last_of.find(b.obj);
        if (it == last_of.end()) { last_of.emplace(b.obj, b); continue; }
        const LayerSeam a = it->second;
        it->second = b;
        if (a.seg != b.seg && b.seg > 0) {
            SeamStats ss;
            ss.object_label = a.obj; ss.segment_below = a.seg; ss.segment_above = b.seg;
            ss.seam_below = a.p; ss.seam_above = b.p;
            ss.xy_jump_mm = double((b.p.head<2>() - a.p.head<2>()).norm());
            out.seams.push_back(ss);
        }
    }

    for (SeamStats &ss : out.seams) {
        std::vector<double> d;
        for (const LayerSeam &r : seams)
            if (r.obj == ss.object_label && r.seg == ss.segment_above)
                d.push_back(double((r.p.head<2>() - ss.seam_below.head<2>()).norm()));
        ss.layers_above = d.size();
        if (! d.empty()) {
            ss.median_offset_mm = percentile(d, 0.5);
            ss.share_within_2mm = double(std::count_if(d.begin(), d.end(), [](double x) { return x <= 2.; })) / double(d.size());
        }
    }

    // ---- rotary words in the written file ----
    if (! gcode_path.empty()) {
        boost::nowide::ifstream in(gcode_path);
        std::string line;
        while (std::getline(in, line)) {
            if (has_rotary_motion_word(line)) {
                if (out.rotary_words == 0) out.first_rotary_line = line;
                ++ out.rotary_words;
            }
        }
    }
    return out;
}

std::string MultiplanarAuditResult::to_json() const
{
    std::ostringstream ss;
    auto fs = [&ss](const char *name, const FoundationStats &f) {
        ss << "\"" << name << "\":{\"samples\":" << f.samples << ",\"unfounded\":" << f.unfounded << ",\"spanned\":" << f.bridged
           << ",\"unfounded_ratio\":" << f3(f.unfounded_ratio()) << ",\"worst_gap_mm\":" << f3(f.worst_gap_mm)
           << ",\"worst_at\":[" << f3(f.worst_at.x()) << "," << f3(f.worst_at.y()) << "," << f3(f.worst_at.z()) << "]"
           << ",\"worst_gcode_line\":" << f.worst_gcode_id << ",\"worst\":[";
        for (size_t i = 0; i < f.worst.size(); ++ i) {
            const UnfoundedSample &u = f.worst[i];
            ss << (i ? "," : "") << "{\"at\":[" << f3(u.at.x()) << "," << f3(u.at.y()) << "," << f3(u.at.z()) << "],\"gap_mm\":" << f3(u.gap_mm)
               << ",\"object\":" << u.object << ",\"section\":" << u.segment << ",\"layer\":" << u.layer_id << ",\"gcode_line\":" << u.gcode_line << "}";
        }
        ss << "]}";
    };
    ss << "{\"multiplanar\":" << (multiplanar ? "true" : "false") << ",";
    fs("model_foundation", model); ss << ",";
    fs("support_foundation", support); ss << ",\"transitions\":[";
    for (size_t i = 0; i < transitions.size(); ++ i) {
        const TransitionStats &t = transitions[i];
        ss << (i ? "," : "") << "{\"object\":" << t.object_label << ",\"section\":" << t.segment << ",\"samples\":" << t.samples
           << ",\"voids\":" << t.voids << ",\"overlaps\":" << t.overlaps << ",\"max_gap_mm\":" << f3(t.max_gap_mm)
           << ",\"p95_gap_mm\":" << f3(t.p95_gap_mm) << ",\"max_overlap_mm\":" << f3(t.max_overlap_mm) << ",\"worst_voids\":[";
        for (size_t k = 0; k < t.worst_voids.size(); ++ k) {
            const UnfoundedSample &u = t.worst_voids[k];
            ss << (k ? "," : "") << "{\"at\":[" << f3(u.at.x()) << "," << f3(u.at.y()) << "," << f3(u.at.z()) << "],\"gap_mm\":" << f3(u.gap_mm)
               << ",\"layer\":" << u.layer_id << ",\"gcode_line\":" << u.gcode_line << ",\"kind\":" << u.role << "}";
        }
        ss << "]}";
    }
    ss << "],\"seams\":[";
    for (size_t i = 0; i < seams.size(); ++ i) {
        const SeamStats &s = seams[i];
        ss << (i ? "," : "") << "{\"object\":" << s.object_label << ",\"below\":" << s.segment_below << ",\"above\":" << s.segment_above
           << ",\"xy_jump_mm\":" << f3(s.xy_jump_mm) << ",\"layers_above\":" << s.layers_above
           << ",\"median_offset_mm\":" << f3(s.median_offset_mm) << ",\"share_within_2mm\":" << f3(s.share_within_2mm) << ",\"seam_below\":[" << f3(s.seam_below.x()) << "," << f3(s.seam_below.y()) << "," << f3(s.seam_below.z())
           << "],\"seam_above\":[" << f3(s.seam_above.x()) << "," << f3(s.seam_above.y()) << "," << f3(s.seam_above.z()) << "]}";
    }
    ss << "],\"rotary_words\":" << rotary_words << "}";
    return ss.str();
}

std::string MultiplanarAuditResult::to_text() const
{
    std::ostringstream ss;
    auto fs = [&ss](const char *name, const FoundationStats &f) {
        ss << name << ": " << f.samples << " samples, " << f.unfounded << " unfounded (" << f3(100. * f.unfounded_ratio()) << "%), "
           << f.bridged << " spanning; worst gap " << f3(f.worst_gap_mm) << " mm at (" << f3(f.worst_at.x()) << ", "
           << f3(f.worst_at.y()) << ", " << f3(f.worst_at.z()) << ")\n";
    };
    fs("Model foundation", model);
    fs("Support foundation", support);
    for (const TransitionStats &t : transitions)
        ss << "Transition object " << t.object_label << " section " << t.segment << ": " << t.samples << " samples, " << t.voids
           << " voids (max " << f3(t.max_gap_mm) << ", p95 " << f3(t.p95_gap_mm) << " mm), " << t.overlaps << " overlaps (max "
           << f3(t.max_overlap_mm) << " mm)\n";
    for (const SeamStats &s : seams)
        ss << "Seam object " << s.object_label << " section " << s.segment_below << " -> " << s.segment_above << ": first-layer jump "
           << f3(s.xy_jump_mm) << " mm; over " << s.layers_above << " layers above, median offset " << f3(s.median_offset_mm)
           << " mm, " << f3(100. * s.share_within_2mm) << "% within 2 mm of the seam line\n";
    ss << "Rotary axis words: " << rotary_words << "\n";
    return ss.str();
}

} // namespace Slic3r
