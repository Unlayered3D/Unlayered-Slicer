#ifndef slic3r_GCodeViewer_hpp_
#define slic3r_GCodeViewer_hpp_

#include "3DScene.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "IMSlider.hpp"
#include "GLModel.hpp"
#include "I18N.hpp"

#include <boost/iostreams/device/mapped_file.hpp>

#include "LibVGCode/LibVGCodeWrapper.hpp"
// needed for tech VGCODE_ENABLE_COG_AND_TOOL_MARKERS
#include <libvgcode/include/Types.hpp>

#include <array>
#include <cstdint>
#include <float.h>
#include <set>
#include <unordered_set>

namespace Slic3r {

class Print;
class TriangleMesh;
class PresetBundle;

namespace GUI {

class PartPlateList;
class OpenGLManager;

static const float GCODE_VIEWER_SLIDER_SCALE = 0.6f;
static const float SLIDER_DEFAULT_RIGHT_MARGIN  = 10.0f;
static const float SLIDER_DEFAULT_BOTTOM_MARGIN = 10.0f;
// ORCA: match right margin to the vertical slider window width to prevent overlap.
static inline const float SLIDER_RIGHT_MARGIN = IMSlider::vertical_slider_window_width();
static const float SLIDER_BOTTOM_MARGIN = 64.0f;
class GCodeViewer
{
public:
    enum class EViewType : unsigned char;
    struct SequentialView
    {
#if ENABLE_ACTUAL_SPEED_DEBUG
        struct ActualSpeedImguiWidget
        {
            std::pair<float, float> y_range = { 0.0f, 0.0f };
            std::vector<std::pair<float, ColorRGBA>> levels;
            struct Item
            {
              float pos{ 0.0f };
              float speed{ 0.0f };
              bool internal{ false };
            };
            std::vector<Item> data;
            int plot(const char* label, const std::array<float, 2>& frame_size = { 0.0f, 0.0f });
        };
#endif // ENABLE_ACTUAL_SPEED_DEBUG

        class Marker
        {
            GLModel m_model;
            // UNLAYERED_MOD: translucent four-sided pyramid standing on the nozzle tip, sized from the
            // same HotendEnvelope HotendCollisionCheck reads and turned by the same tool_head_frame(),
            // so what is drawn is the envelope actually being tested. It leaves out the check's 0.5 mm
            // lateral margin, sub-millimetre against a 40 mm envelope. Built lazily in render():
            // set_envelope() is called from load(), where there is no guarantee of a current GL context.
            GLModel m_envelope;
            HotendEnvelope m_envelope_shape;
            bool m_envelope_enabled{ false };
            Vec3f m_world_position;
            // for seams, the position of the marker is on the last endpoint of the toolpath containing it
            // the offset is used to show the correct value of tool position in the "ToolPosition" window
            // see implementation of render() method
            Vec3f m_world_offset;
            float m_z_offset{ 0.0f };
            // z offset of the model
            float m_model_z_offset{ 0.5f };
            // UNLAYERED_MOD: unit direction the nozzle points away from the surface along. +Z for an
            // ordinary planar print; the chunk's tool axis on a multiplanar / 5-axis one, which tilts
            // the hotend model about its tip so it stays on the toolpath.
            Vec3f m_tool_axis{ 0.0f, 0.0f, 1.0f };
            bool m_visible{ true };
            bool m_is_dark = false;
            float m_scale_factor{ 1.0f };
#if ENABLE_ACTUAL_SPEED_DEBUG
            ActualSpeedImguiWidget m_actual_speed_imgui_widget;
#endif // ENABLE_ACTUAL_SPEED_DEBUG

        public:
            float m_scale = 1.0f;

            void init(std::string filename);

            const BoundingBoxf3& get_bounding_box() const { return m_model.get_bounding_box(); }

            void set_world_position(const Vec3f& position) { m_world_position = position; }
            void set_world_offset(const Vec3f& offset) { m_world_offset = offset; }
            void set_z_offset(float z_offset) { m_z_offset = z_offset; }
            // UNLAYERED_MOD
            void set_tool_axis(const Vec3f& axis) { m_tool_axis = (axis.squaredNorm() > 0.0f) ? axis.normalized() : Vec3f::UnitZ(); }
            const Vec3f& get_tool_axis() const { return m_tool_axis; }
            // UNLAYERED_MOD
            void set_envelope(bool enabled, const HotendEnvelope& envelope);

#if ENABLE_ACTUAL_SPEED_DEBUG
            void set_actual_speed_y_range(const std::pair<float, float>& y_range) {
                m_actual_speed_imgui_widget.y_range = y_range;
            }
            void set_actual_speed_levels(const std::vector<std::pair<float, ColorRGBA>>& levels) {
                m_actual_speed_imgui_widget.levels = levels;
            }
            void set_actual_speed_data(const std::vector<ActualSpeedImguiWidget::Item>& data) {
                m_actual_speed_imgui_widget.data = data;
            }
#endif // ENABLE_ACTUAL_SPEED_DEBUG

            bool is_visible() const { return m_visible; }
            void set_visible(bool visible) { m_visible = visible; }

            void render(int canvas_width, int canvas_height, const libvgcode::EViewType& view_type);
            void render_position_window(const libvgcode::Viewer* viewer, int canvas_width, int canvas_height, const libvgcode::EViewType& view_type);
            void on_change_color_mode(bool is_dark) { m_is_dark = is_dark; }
        };

        class GCodeWindow
        {
            struct Line
            {
                std::string command;
                std::string parameters;
                std::string comment;
            };
            bool m_is_dark = false;
            uint64_t m_selected_line_id{ 0 };
            size_t m_last_lines_size{ 0 };
            std::string m_filename;
            boost::iostreams::mapped_file_source m_file;
            // map for accessing data in file by line number
            std::vector<size_t> m_lines_ends;
            // current visible lines
            std::vector<Line> m_lines;

        public:
            float m_scale = 1.0f;
            GCodeWindow() = default;
            ~GCodeWindow() { stop_mapping_file(); }
            void load_gcode(const std::string& filename, const std::vector<size_t> &lines_ends);
            void reset() {
                stop_mapping_file();
                m_lines_ends.clear();
                m_lines_ends.shrink_to_fit();
                m_lines.clear();
                m_lines.shrink_to_fit();
                m_filename.clear();
                m_filename.shrink_to_fit();
            }

            //BBS: GUI refactor: add canvas size
            //void render(float top, float bottom, uint64_t curr_line_id) const;
            void render(float top, float bottom, float right, uint64_t curr_line_id) const;
            void on_change_color_mode(bool is_dark) { m_is_dark = is_dark; }

            void stop_mapping_file();
        };

        Marker marker;
        GCodeWindow gcode_window;
        float m_scale = 1.0;
        bool m_show_marker = false;
        void render(const bool has_render_path, float legend_height, const libvgcode::Viewer* viewer, uint32_t gcode_id, int canvas_width, int canvas_height, int right_margin, const libvgcode::EViewType& view_type);
    };
    struct ExtruderFilament
    {
        std::string   type;
        std::string   hex_color;
        unsigned char filament_id;
        bool is_support_filament;
    };
    // helper to render shells
    struct Shells
    {
        GLVolumeCollection volumes;
        bool               visible{false};
        // BBS: always load shell when preview
        int  print_id{-1};
        int  print_modify_count{-1};
        bool previewing{false};
    };
    //BBS
    ConflictResultOpt m_conflict_result;
    // UNLAYERED_MOD — the loaded G-code was sliced for virtual 5-axis (plain 3-axis) output. The
    // layers tilt but the machine has no rotary axes, so the physical nozzle stays vertical and the
    // sequential-view hotend marker must not be tilted by the per-move tool axis.
    bool              m_virtual_5axis{ false };
    GCodeCheckResult  m_gcode_check_result;
    FilamentPrintableResult filament_printable_reuslt;
    Shells            m_shells;

private:
    std::vector<int> m_plater_extruder;
    bool m_gl_data_initialized{ false };
    unsigned int m_last_result_id{ 0 };
    //BBS: save m_gcode_result as well
    const GCodeProcessorResult* m_gcode_result{ nullptr };
    std::array<unsigned int, static_cast<size_t>(EMoveType::Count)> m_move_type_counts{};
    std::array<std::array<float, static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Count)>, static_cast<size_t>(EMoveType::Count)> m_move_type_times{};
    std::array<float, static_cast<size_t>(EMoveType::Count)> m_move_type_distances{};
    //BBS: add only gcode mode
    bool m_only_gcode_in_preview {false};

    //BBS: extruder dispensing filament
    std::vector<ExtruderFilament> m_left_extruder_filament;
    std::vector<ExtruderFilament> m_right_extruder_filament;
    size_t m_nozzle_nums;

    // bounding box of toolpaths
    BoundingBoxf3 m_paths_bounding_box;
    // bounding box of toolpaths + marker tools
    BoundingBoxf3 m_max_bounding_box;
    //BBS: add shell bounding box
    BoundingBoxf3 m_shell_bounding_box;
    float m_max_print_height{ 0.0f };
    float m_z_offset{ 0.0f };
    // BELT_MOD: the loaded file was written for a belt printer (GCodeProcessorResult::belt_printer).
    // The upright ("designed") view maps every vertex through belt_to_bed at load time, so the
    // toolpaths sit on the model shells in the bed frame; the raw view shows the machine-frame
    // coordinates as written, for checking the transform itself. The choice survives reloads.
    bool m_belt_view_enabled{ false };
    bool m_belt_show_designed{ true };
    // See GCodeProcessorResult::tool_axis_fixed.
    bool  m_tool_axis_fixed{ false };
    Vec3f m_fixed_tool_axis{ Vec3f::UnitZ() };

    ConfigOptionMode m_user_mode;
    bool m_fold = {false};

    size_t m_extruders_count;
    std::vector<float> m_filament_diameters;
    std::vector<float> m_filament_densities;
    SequentialView m_sequential_view;
    IMSlider* m_moves_slider;
    IMSlider* m_layers_slider;
#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
    // whether or not to render the cog model with fixed screen size
    bool m_cog_marker_fixed_screen_size{ true };
    float m_cog_marker_size{ 1.0f };
    bool m_tool_marker_fixed_screen_size{ false };
    float m_tool_marker_size{ 1.0f };
#endif // VGCODE_ENABLE_COG_AND_TOOL_MARKERS

    /*BBS GUI refactor, store displayed items in color scheme combobox */
    std::vector<libvgcode::EViewType> view_type_items;
    std::vector<std::string> view_type_items_str;
    int       m_view_type_sel = 0;
    int       m_last_extruder_count_default_applied{0};  // 0=unset, 1=single, 2+=multi
    std::vector<EMoveType> options_items;

    bool m_legend_visible{ true };
    bool m_legend_enabled{ true };

    float m_legend_height;
    PrintEstimatedStatistics m_print_statistics;
    std::array<float, 2> m_detected_point_sizes = { 0.0f, 0.0f };
    GCodeProcessorResult::SettingsIds m_settings_ids;

    std::vector<CustomGCode::Item> m_custom_gcode_per_print_z;

    bool m_contained_in_bed{ true };
mutable bool m_no_render_path { false };
    bool m_is_dark = false;

    libvgcode::Viewer m_viewer;
    bool m_loaded_as_preview{ false };

public:
    GCodeViewer();
    ~GCodeViewer();

    void on_change_color_mode(bool is_dark);
    float m_scale = 1.0;
    void set_scale(float scale = 1.0);
    void init(ConfigOptionMode mode, Slic3r::PresetBundle* preset_bundle);
    void update_by_mode(ConfigOptionMode mode);

    // extract rendering data from the given parameters
    //BBS: add only gcode mode
    void load_as_gcode(const GCodeProcessorResult& gcode_result, const Print& print, const std::vector<std::string>& str_tool_colors,
        const std::vector<std::string>& str_color_print_colors, const BuildVolume& build_volume,
        const std::vector<BoundingBoxf3>& exclude_bounding_box, ConfigOptionMode mode, bool only_gcode = false);
    void load_as_preview(libvgcode::GCodeInputData&& data);
    // Preference "preview_color_vision": (re)applies the extrusion-role and range palettes for
    // normal vision or one of the common deficiencies. Called on load and when the preference changes.
    void apply_color_vision_palette();
    void update_shells_color_by_extruder(const DynamicPrintConfig* config);
    void set_shell_transparency(float alpha = 0.15f);

    void reset();
    //BBS: always load shell at preview
    void reset_shell();
    void load_shells(const Print& print, bool initialized, bool force_previewing = false);
    void set_shells_on_preview(bool is_previewing) { m_shells.previewing = is_previewing; }
    //BBS: add all plates filament statistics
    void render_all_plates_stats(const std::vector<const GCodeProcessorResult*>& gcode_result_list, bool show = true) const;
    //BBS: GUI refactor: add canvas width and height
    void render(int canvas_width, int canvas_height, int right_margin);
    //BBS
    // void _render_calibration_thumbnail_internal(ThumbnailData& thumbnail_data, const ThumbnailsParams& thumbnail_params, PartPlateList& partplate_list, OpenGLManager& opengl_manager);
    // void _render_calibration_thumbnail_framebuffer(ThumbnailData& thumbnail_data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params, PartPlateList& partplate_list, OpenGLManager& opengl_manager);
    // void render_calibration_thumbnail(ThumbnailData& thumbnail_data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params, PartPlateList& partplate_list, OpenGLManager& opengl_manager);
    bool has_data() const { return !m_viewer.get_extrusion_roles().empty(); }

    bool can_export_toolpaths() const;
    std::vector<int> get_plater_extruder();

    const float                get_max_print_height() const { return m_max_print_height; }
    const BoundingBoxf3& get_paths_bounding_box() const { return m_paths_bounding_box; }
    const BoundingBoxf3& get_max_bounding_box() const { return m_max_bounding_box; }
    const BoundingBoxf3& get_shell_bounding_box() const { return m_shell_bounding_box; }
    std::vector<double> get_layers_zs() const {
        const std::vector<float> zs = m_viewer.get_layers_zs();
        std::vector<double> ret;
        std::transform(zs.begin(), zs.end(), std::back_inserter(ret), [](float z) { return static_cast<double>(z); });
        return ret;
    }
    std::vector<float> get_layers_times() const { return m_viewer.get_layers_estimated_times(); }

    // UNLAYERED_MOD: per-layer "printed with the tool straight up". Empty for an ordinary planar
    // print, so consumers can skip the tilted-layer handling entirely.
    std::vector<bool> get_layers_planar() const {
        if (m_viewer.all_layers_planar())
            return {};
        const size_t count = m_viewer.get_layers_count();
        std::vector<bool> ret(count);
        for (size_t i = 0; i < count; ++i)
            ret[i] = m_viewer.is_layer_planar(i);
        return ret;
    }
    // Z extent of the given inclusive layer range. A tilted layer occupies a slab, so clipping the
    // shells to a single Z band per layer would cut them apart.
    std::array<double, 2> get_layers_z_extent(size_t first, size_t last) const {
        const size_t count = m_viewer.get_layers_count();
        if (count == 0)
            return { 0.0, 0.0 };
        first = std::min(first, count - 1);
        last  = std::min(last, count - 1);
        double z_min = m_viewer.get_layer_z_min(first);
        double z_max = m_viewer.get_layer_z_max(first);
        for (size_t i = first + 1; i <= last; ++i) {
            z_min = std::min(z_min, double(m_viewer.get_layer_z_min(i)));
            z_max = std::max(z_max, double(m_viewer.get_layer_z_max(i)));
        }
        return { z_min, z_max };
    }

    const std::array<size_t,2> &get_layers_z_range() const { return m_viewer.get_layers_view_range(); }

    const SequentialView& get_sequential_view() const { return m_sequential_view; }
    void update_sequential_view_current(unsigned int first, unsigned int last);

    /* BBS IMSlider */
    IMSlider *get_moves_slider() { return m_moves_slider; }
    IMSlider *get_layers_slider() { return m_layers_slider; }
    void enable_moves_slider(bool enable) const;
    void update_moves_slider(bool set_to_max = false);
    void update_layers_slider_mode();

    const libvgcode::Interval& get_gcode_view_full_range() const { return m_viewer.get_view_full_range(); }
    const libvgcode::Interval& get_gcode_view_enabled_range() const { return m_viewer.get_view_enabled_range(); }
    const libvgcode::Interval& get_gcode_view_visible_range() const { return m_viewer.get_view_visible_range(); }
    const libvgcode::PathVertex& get_gcode_vertex_at(size_t id) const { return m_viewer.get_vertex_at(id); }

    bool is_contained_in_bed() const { return m_contained_in_bed; }
    // BELT_MOD: see m_belt_view_enabled. Toggling only takes effect once the preview is reloaded
    // (Plater::refresh_belt_view), because the mapping is baked into the vertex positions.
    bool is_belt_view() const { return m_belt_view_enabled; }
    bool is_belt_show_designed() const { return m_belt_show_designed; }
    void toggle_belt_show_designed() { if (m_belt_view_enabled) m_belt_show_designed = ! m_belt_show_designed; }
    // Are the loaded positions in the bed frame, so that heights and bed bounds mean what they say?
    bool positions_in_bed_frame() const { return ! m_belt_view_enabled || m_belt_show_designed; }
    //BBS: add only gcode mode
    bool is_only_gcode_in_preview() const { return m_only_gcode_in_preview; }

    void set_view_type(libvgcode::EViewType type) {
        // Declarative cut planes: per-section visibility is only reachable from the Section legend,
        // so it must not survive a switch to another view -- otherwise toolpaths would silently
        // stay hidden in Speed/Feature with no control to bring them back.
        if (type != libvgcode::EViewType::CutSegment)
            m_viewer.show_all_cut_segments();
        m_viewer.set_view_type(type);
    }
    void reset_visible(libvgcode::EViewType type) {
        if (type == libvgcode::EViewType::FeatureType) {
            auto roles = m_viewer.get_extrusion_roles();
            for (size_t i = 0; i < roles.size(); ++i) {
                auto role = roles[i];
                if (!m_viewer.is_extrusion_role_visible(role)) {
                    m_viewer.toggle_extrusion_role_visibility(role);
                }
            }
        }
    }

    libvgcode::EViewType get_view_type() const { return m_viewer.get_view_type(); }

    // Automation (Actions/PreviewActions.cpp): what the legend's controls do, callable outside the
    // ImGui frame. Each setter does what the matching legend click does (the toggle, then
    // update_moves_slider()) and nothing when the state already matches.
    // The colouring combo's pick: also moves the combo's own selection (m_view_type_sel). False when
    // the combo does not offer `type` for the loaded G-code (Tool never; Section only with >1 section).
    bool select_view_type(libvgcode::EViewType type);
    std::vector<libvgcode::EViewType> get_offered_view_types() const;
    // Option rows (Travel, Wipe, Retract, Unretract, Seams, Filament changes).
    const std::vector<libvgcode::EOptionType>& get_options() const { return m_viewer.get_options(); }
    bool is_option_visible(libvgcode::EOptionType type) const { return m_viewer.is_option_visible(type); }
    void set_option_visible(libvgcode::EOptionType type, bool visible);
    // Feature-type rows.
    std::vector<libvgcode::EGCodeExtrusionRole> get_extrusion_roles() const { return m_viewer.get_extrusion_roles(); }
    bool is_extrusion_role_visible(libvgcode::EGCodeExtrusionRole role) const { return m_viewer.is_extrusion_role_visible(role); }
    void set_extrusion_role_visible(libvgcode::EGCodeExtrusionRole role, bool visible);
    // Section rows (Section view only; set_view_type to any other view shows them all again).
    std::vector<int16_t> get_used_cut_segments_ids() const { return m_viewer.get_used_cut_segments_ids(); }
    bool is_cut_segment_visible(int16_t segment_id) const { return m_viewer.is_cut_segment_visible(segment_id); }
    void set_cut_segment_visible(int16_t segment_id, bool visible);
    // "Show stealth mode" / "Show normal mode".
    libvgcode::ETimeMode get_time_mode() const { return m_viewer.get_time_mode(); }
    std::vector<libvgcode::ETimeMode> get_time_modes() const { return m_viewer.get_time_modes(); }
    void set_time_mode(libvgcode::ETimeMode mode);
    // The legend's colour scale for a range view (min, max).
    std::array<float, 2> get_color_range(libvgcode::EViewType type) const { return m_viewer.get_color_range(type).get_range(); }
    // The tool-position window's vertex.
    size_t get_current_vertex_id() const { return m_viewer.get_current_vertex_id(); }
    size_t get_vertices_count() const { return m_viewer.get_vertices_count(); }
    float  get_estimated_time_at(size_t id) const { return m_viewer.get_estimated_time_at(id); }

    // ORCA: darken the layers not scrubbed to while using the preview layer slider
    void set_dim_previous_layers(bool value) { m_viewer.set_dim_previous_layers(value); }
    bool is_dim_previous_layers() const { return m_viewer.is_dim_previous_layers(); }
    // ORCA: brightness of those darkened layers, 1.0 = unchanged, 0.0 = black
    void set_dim_previous_layers_brightness(float value) { m_viewer.set_dim_previous_layers_brightness(value); }
    float get_dim_previous_layers_brightness() const { return m_viewer.get_dim_previous_layers_brightness(); }

    void set_layers_z_range(const std::array<unsigned int, 2>& layers_z_range);

    bool is_legend_shown() const { return m_legend_visible && m_legend_enabled; }
    void show_legend(bool show) { m_legend_visible = show; }
    void enable_legend(bool enable) { m_legend_enabled = enable; }
    float get_legend_height() { return m_legend_height; }

    void export_toolpaths_to_obj(const char* filename) const;

    size_t get_extruders_count() { return m_extruders_count; }
    void push_combo_style();
    void pop_combo_style();

    void invalidate_legend() { /*TODO: m_legend_resizer.reset();*/ }

#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
    float get_cog_marker_scale_factor() const { return m_viewer.get_cog_marker_scale_factor(); }
    void set_cog_marker_scale_factor(float factor) { return m_viewer.set_cog_marker_scale_factor(factor); }
#endif // VGCODE_ENABLE_COG_AND_TOOL_MARKERS

private:
    //BBS: always load shell at preview
    //void load_shells(const Print& print);
    void render_toolpaths();
    void render_shells(int canvas_width, int canvas_height);

    //BBS: GUI refactor: add canvas size
    void render_legend(float &legend_height, int canvas_width, int canvas_height, int right_margin);
    void render_legend_color_arr_recommen(float window_padding);
    void render_slider(int canvas_width, int canvas_height);
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GCodeViewer_hpp_

