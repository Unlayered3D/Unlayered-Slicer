#include <catch2/catch_all.hpp>
#include "test_helpers.hpp"
#include "auto_planes_fixtures.hpp"
#include "test_utils.hpp"
#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/CurvedPlaneProgram.hpp"
#include "libslic3r/CurvedMesh.hpp"
#include "libslic3r/CurvedRegions.hpp"
#include "libslic3r/CurvedJointPlanning.hpp"
#include "libslic3r/CurvedFixedNozzle.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Format/STL.hpp"
#include <sstream>
#include <regex>
#include <fstream>

using namespace Slic3r;
namespace {
DynamicPrintConfig curved_config()
{
    auto c=DynamicPrintConfig::full_print_config();
    c.set_deserialize_strict({
        {"curved_layer_blend","1"},{"enable_nonplanar","1"},{"axis_selection","virtual"},
        {"print_sequence","by group"},{"nonplanar_hotend_cone_height","125"},
        {"nonplanar_hotend_angle_xpos","7"},{"nonplanar_hotend_angle_xneg","10"},
        {"nonplanar_hotend_angle_ypos","15"},{"nonplanar_hotend_angle_yneg","6"},
        {"nonplanar_gantry_height","14"},{"nonplanar_virtual_max_angle","5"},{"nonplanar_virtual_max_z","100"},
        {"printable_area","-250x-250,250x-250,250x250,-250x250"}, // synthetic fixture permits InfiniteBed arrangement
        {"min_layer_height","0.08"},{"max_layer_height","0.32"},
        {"layer_height","0.2"},{"initial_layer_print_height","0.2"},
        {"sparse_infill_density","100%"},{"wall_generator","classic"},{"seam_slope_type","none"},
        {"enable_support","0"},{"raft_layers","0"},{"enable_prime_tower","0"},{"enable_arc_fitting","0"},
        {"wipe","0"},{"infill_combination","0"},{"skirt_loops","0"},
        {"wipe_on_loops","0"},{"wipe_before_external_loop","0"},
        {"ironing_type","no ironing"},{"fuzzy_skin","disabled_fuzzy"},{"zaa_enabled","0"},
        {"wave_overhangs","0"},{"make_overhang_printable","0"},
        {"layer_change_gcode","G92 E0"},{"before_layer_change_gcode",""},
        {"machine_start_gcode","G28"},{"machine_end_gcode","M104 S0"},{"thumbnails",""},
        {"use_relative_e_distances","1"},{"gcode_comments","1"},
        {"filament_adaptive_volumetric_speed","0"},{"elefant_foot_compensation","0"}
    });
    return c;
}
TriangleMesh ramp(double angle=5.)
{
    return TriangleMesh(AutoPlanesFixtures::heightfield_block(20.,20.,4,[angle](double x){return 3.+x*std::tan(angle*PI/180.);}));
}

TriangleMesh joint_part()
{
    indexed_triangle_set its;
    const std::array<Vec2d,7> outline{{{0.,0.},{2.,0.},{2.,15.},
        {12.,15.+10.*std::tan(28.*PI/180.)},{12.,27.},{6.,27.5},{0.,27.}}};
    for(double y:{0.,6.}) for(const auto& p:outline) its.vertices.emplace_back(float(p.x()),float(y),float(p.y()));
    for(int i=0;i<5;++i) {
        its.indices.emplace_back(6,i,i+1);its.indices.emplace_back(i+1+7,i+7,6+7);
    }
    for(int i=0;i<7;++i) {const int j=(i+1)%7;
        its.indices.emplace_back(i,i+7,j+7);its.indices.emplace_back(i,j+7,j);}
    return TriangleMesh(std::move(its));
}

// Automatic mode may legitimately choose planar. Verify the actual generated
// comparison rather than treating an enabled checkbox as proof of curvature.
void require_best_generated_choice(const PrintObject& object)
{
    INFO(object.layer_strategy_decision());
    REQUIRE(object.layer_strategy_decision().find("Generated benefit/cost choice:")!=std::string::npos);
    REQUIRE_FALSE(object.layer_region_records().empty());
    double best=-INFINITY;bool planar=false;
    std::vector<CurvedTransition::RegionDecision> records;
    for(const auto& line:object.layer_region_records()) {
        CurvedTransition::RegionDecision r;
        REQUIRE(CurvedTransition::RegionDecision::decode(line,r));
        if(r.selected) best=std::max(best,r.gain);
        planar|=r.kind==2;records.push_back(r);
    }
    REQUIRE(planar);REQUIRE(std::isfinite(best));REQUIRE(best>=0.);
    for(const auto& r:records) REQUIRE(r.gain<=best+1e-5);
}

// Synthetic fixed dual-carriage fixture, never a measured Bambu profile.
DynamicPrintConfig fixed_nozzle_config(int active=0)
{
    auto c=curved_config();c.set_num_extruders(2);
    c.set_deserialize_strict({{"nozzle_diameter","0.4,0.4"},{"extruder_offset","0x0,12x-3"},
        {"min_layer_height","0.08,0.1"},{"max_layer_height","0.32,0.3"},
        {"extruder_printable_height","200,80"},{"physical_extruder_map","1,0"},
        {"nonplanar_fixed_nozzle",active==0?"1":"2"},{"nonplanar_clearance_nozzle",active==0?"1":"2"},
        {"nonplanar_fixed_clearance_recorded","1"},{"nonplanar_fixed_clearance_source","SYNTHETIC full carriage with parked tip"},
        {"machine_max_speed_z","10,0,5,0"},{"filament_map",active==0?"1":"2"},
        {"enable_filament_dynamic_map","0"},{"automatic_layer_strategy","1"},{"curved_layer_blend","0"},
        {"change_filament_gcode",""},{"time_lapse_gcode",""},
        {"printer_model","Bambu Lab X2D"},{"machine_start_gcode","G28\nT0 H[initial_no_support_hotend]\nM104 S210"},
        {"layer_change_gcode","M73 L{layer_num+1}\nM991 S0 P{layer_num}\nM106 P2 S100"}});
    c.set_key_value("filament_map_mode",new ConfigOptionEnum<FilamentMapMode>(fmmManual));
    c.set_key_value("printer_extruder_id",new ConfigOptionInts({1,2}));
    c.set_key_value("printer_extruder_variant",new ConfigOptionStrings({"Direct Drive Standard","Direct Drive Standard"}));
    c.set_key_value("extruder_printable_area",new ConfigOptionPointsGroups({
        {Vec2d(0.,0.),Vec2d(200.,0.),Vec2d(200.,200.),Vec2d(0.,200.)},
        {Vec2d(20.,0.),Vec2d(190.,0.),Vec2d(190.,190.),Vec2d(20.,190.)}}));
    return c;
}

TriangleMesh holed_part()
{
    indexed_triangle_set mesh;
    for(int i=0;i<16;++i) {
        const double angle=i*2.*PI/16.;
        for(const auto& rz:std::vector<std::pair<double,double>>{{10.,0.},{10.,12.},{3.,0.},{3.,12.}})
            mesh.vertices.emplace_back(float(rz.first*std::cos(angle)),float(rz.first*std::sin(angle)),float(rz.second));
    }
    auto add=[&](int a,int b,int c){mesh.indices.emplace_back(a,b,c);};
    for(int i=0;i<16;++i) {
        const int a=4*i,b=4*((i+1)%16);
        add(a,b,b+1);add(a,b+1,a+1); // outer
        add(a+2,a+3,b+3);add(a+2,b+3,b+2); // bore
        add(a+1,b+1,b+3);add(a+1,b+3,a+3); // roof
        add(a,a+2,b+2);add(a,b+2,b); // bed
    }
    return TriangleMesh(std::move(mesh));
}

void add_plane(ModelObject& object,double height,double gx,double gy=0.)
{
    CutPlane p; p.point=Vec3d(0.,0.,height); p.normal=Vec3d(-gx,-gy,1.).normalized();
    object.cut_planes.enabled=true; object.cut_planes.planes.push_back(p);
}
}

TEST_CASE("Fixed nozzle limits retain parked hardware declarations and reject mismatched states", "[curved-engine][fixed-nozzle]")
{
    auto dynamic=fixed_nozzle_config(1);
    for(const auto& key:{"nonplanar_fixed_nozzle","nonplanar_clearance_nozzle",
                        "nonplanar_fixed_clearance_recorded","nonplanar_fixed_clearance_source"}) {
        const std::string value=dynamic.option(key)->serialize();dynamic.erase(key);dynamic.set_deserialize_strict(key,value);
    }
    PrintConfig c;c.apply(dynamic,true);
    const auto limits=CurvedTransition::FixedNozzleLimits::from_config(c,1);
    REQUIRE(limits.nozzle==1);REQUIRE(limits.max_z==80.);
    REQUIRE(limits.min_layer==.1);REQUIRE(limits.max_layer==.3);REQUIRE(limits.z_speed==5.);
    SECTION("different active tip") {c.nonplanar_clearance_nozzle.value=1;}
    SECTION("unrecorded") {c.nonplanar_fixed_clearance_recorded.value=false;}
    SECTION("no source") {c.nonplanar_fixed_clearance_source.value=" \t";}
    SECTION("unknown parked-tip side") {c.nonplanar_hotend_angle_xneg.value=0.;}
    SECTION("incomplete route") {c.physical_extruder_map.values={0};}
    SECTION("added nozzle") {c.nozzle_diameter.values.push_back(.4);}
    SECTION("missing height") {c.extruder_printable_height.values[1]=0.;}
    REQUIRE_THROWS(CurvedTransition::FixedNozzleLimits::from_config(c,1));
}

TEST_CASE("Fixed nozzle automatic slicing emits BBL selection and preserves live and imported Preview", "[curved-engine][fixed-nozzle][preview]")
{
    const int active=GENERATE(0,1);auto config=fixed_nozzle_config(active);
    Print print;print.is_BBL_printer()=true;Model model;
    Test::init_print({ramp(2.)},print,model,config);
    model.objects.front()->instances.front()->set_offset(Vec3d(100.,100.,0.));print.apply(model,config);
    print.process();
    ScopedTemporaryFile temp(".gcode");GCodeProcessorResult preview;
    print.export_gcode(temp.string(),&preview,nullptr);
    const auto* object=print.objects().front();
    REQUIRE(object->curved_field());REQUIRE(object->curved_domain());
    REQUIRE(object->curved_nozzle_index()==size_t(active));REQUIRE(preview.tool_axis_fixed);
    REQUIRE(preview.nozzle_group_result);
    const auto live_nozzles=preview.nozzle_group_result->get_nozzles_for_filament(0);
    REQUIRE(live_nozzles.size()==1);REQUIRE(live_nozzles.front().extruder_id==active);
    std::ifstream input(temp.string());const std::string output((std::istreambuf_iterator<char>(input)),{});
    REQUIRE(output.find("T0 H"+std::to_string(live_nozzles.front().extruder_id))!=std::string::npos);
    REQUIRE(output.find("T0 H-1")==std::string::npos);
    REQUIRE(output.find("M104 S210")!=std::string::npos);
    REQUIRE(output.find("M106 P2 S100")!=std::string::npos);
    const auto domain=*object->curved_domain(); // automatic selection may use a PlaneProgram, not a Ramp
    const Vec2d shift=unscale(object->instances().front().shift);
    const auto deposits=[&](const GCodeProcessorResult& result) {
        size_t count=0;
        for(const auto& move:result.moves) {
            if(move.type!=EMoveType::Extrude || move.extrusion_role==erCustom || move.extrusion_role==erSkirt || move.extrusion_role==erBrim) continue;
            ++count;REQUIRE(move.extruder_id==0);
            const Vec2d local=move.position.head<2>().cast<double>()-shift;
            REQUIRE(local.x()>=domain.xmin-.1);REQUIRE(local.x()<=domain.xmax+.1);
            REQUIRE(local.y()>=domain.ymin-.1);REQUIRE(local.y()<=domain.ymax+.1);
        }
        return count;
    };
    const size_t live_count=deposits(preview);REQUIRE(live_count>1000);
    GCodeProcessor replay;replay.process_file(temp.string());
    REQUIRE(replay.get_result().nozzle_group_result);
    const auto imported=replay.get_result().nozzle_group_result->get_nozzles_for_filament(0);
    REQUIRE(imported.size()==1);REQUIRE(imported.front().extruder_id==active);
    REQUIRE(deposits(replay.get_result())==live_count);
    // A changed bound must invalidate the curved slice, not merely re-export it.
    config.set_deserialize_strict({{"extruder_printable_height",active==0?"4,80":"200,4"}});print.apply(model,config);
    const_cast<PrintObject*>(print.objects().front())->slice();REQUIRE_FALSE(print.objects().front()->curved_field());
}

TEST_CASE("Fixed nozzle automatic mode keeps unsupported mappings and footprints planar", "[curved-engine][fixed-nozzle][fallback]")
{
    auto config=fixed_nozzle_config(1);Print print;print.is_BBL_printer()=true;Model model;
    std::string scenario;
    SECTION("mapping to other nozzle") {scenario="mapping";config.set_deserialize_strict({{"filament_map","1"}});}
    SECTION("unmeasured clearance") {scenario="clearance";config.set_deserialize_strict({{"nonplanar_fixed_clearance_recorded","0"}});}
    SECTION("automatic reassignment") {scenario="automatic mapping";config.set_key_value("filament_map_mode",new ConfigOptionEnum<FilamentMapMode>(fmmAutoForFlush));}
    SECTION("manual tags") {scenario="manual change";config.set_deserialize_strict({{"manual_filament_change","1"}});}
    SECTION("two used materials") {
        scenario="two materials";
        config.set_num_filaments(2);
        // set_num_filaments() only visits filament_option_keys(), which this
        // upstream ConfigDef does not initialize. Supply both persistent slots
        // and their variant identities explicitly, as a real project does.
        config.set_deserialize_strict({{"filament_diameter","1.75,1.75"},
            {"filament_colour","#FF0000;#00FF00"},{"filament_map","2,1"},
            {"outer_wall_filament_id","1"},{"internal_solid_filament_id","2"},{"top_surface_filament_id","2"}});
        config.set_key_value("filament_self_index",new ConfigOptionInts({1,2}));
        config.set_key_value("filament_extruder_variant",new ConfigOptionStrings({"Direct Drive Standard","Direct Drive Standard"}));
        REQUIRE(config.opt<ConfigOptionFloats>("filament_diameter")->size()==2);
        REQUIRE((config.opt<ConfigOptionInts>("filament_self_index")->values==std::vector<int>{1,2}));
    }
    SECTION("wrong machine protocol") {scenario="machine protocol";print.is_BBL_printer()=false;}
    SECTION("out of reach") {scenario="footprint";config.set_key_value("extruder_printable_area",new ConfigOptionPointsGroups({
        {Vec2d(0.,0.),Vec2d(200.,0.),Vec2d(200.,200.),Vec2d(0.,200.)},
        {Vec2d(150.,0.),Vec2d(190.,0.),Vec2d(190.,190.),Vec2d(150.,190.)}}));}
    Test::init_print({ramp(2.)},print,model,config);
    model.objects.front()->instances.front()->set_offset(Vec3d(100.,100.,0.));
    if(scenario=="two materials") {
        // Make the two consumed materials explicit on the actual fixture
        // object, then verify its applied region before testing the fallback.
        model.objects.front()->config.set("outer_wall_filament_id",1);
        model.objects.front()->config.set("internal_solid_filament_id",2);
        model.objects.front()->config.set("top_surface_filament_id",2);
    }
    print.apply(model,config);
    if(scenario=="two materials") {
        REQUIRE(print.config().filament_diameter.size()==2);
        REQUIRE(print.objects().front()->printing_region(0).config().internal_solid_filament_id.value==2);
        REQUIRE(print.extruders().size()==2);
    }
    const_cast<PrintObject*>(print.objects().front())->slice();
    INFO("scenario="<<scenario<<" decision="<<print.objects().front()->layer_strategy_decision());
    REQUIRE_FALSE(print.objects().front()->curved_field());
    REQUIRE(print.objects().front()->layer_strategy_decision().find("Automatic selection: planar.")!=std::string::npos);
}

TEST_CASE("Fixed nozzle writer refuses tool changes before mutation and applies offset once", "[curved-engine][fixed-nozzle]")
{
    PrintConfig c;c.apply(fixed_nozzle_config(1),true);
    c.filament_map.values={2,1};c.filament_diameter.values={1.75,1.75};
    GCodeWriter writer;writer.set_is_bbl_machine(true);writer.apply_print_config(c);writer.set_extruders({0,1});
    REQUIRE(writer.set_extruder(0).find("M1020 S0 H1")!=std::string::npos);
    REQUIRE(writer.filament()->extruder_id()==1);
    CurvedTransition::Ramp ramp{0.,20.,0.,20.,3.,.08,0.,3.,4.6};auto field=ramp.field(.2);
    const Vec2d placed(100.,80.);const Vec2d origin=placed-writer.extruder_offset_xy().head<2>();
    REQUIRE_THROWS(writer.set_curved_blend(field,ramp.domain(),origin,2.,5.,0));
    writer.set_curved_blend(field,ramp.domain(),origin,2.,5.,1);writer.set_curved_path(2.,.45,.2);
    writer.travel_to_xyz(Vec3d(origin.x(),origin.y()+10.,2.));
    REQUIRE_FALSE(writer.extrude_to_xy(Vec2d(origin.x()+20.,origin.y()+10.),1.).empty());
    REQUIRE(std::abs(writer.get_position().x()+12.-(placed.x()+20.))<1e-9);
    REQUIRE(std::abs(writer.get_position().z()-field->world_z(20.,10.,2.))<1e-9);
    const auto position=writer.get_position();
    REQUIRE_THROWS(writer.toolchange(1,0));REQUIRE_THROWS(writer.toolchange(0,1));
    REQUIRE(writer.filament()->extruder_id()==1);REQUIRE(writer.get_position()==position);
}

TEST_CASE("Curved layer scripts retain Bambu cooling and progress but reject expanded motion", "[curved-engine][fixed-nozzle][scripts]")
{
    for(const std::string code:{"; comment\nM73 L2\nM991 S0 P1","M106 P2 S100\nM106 P10 S100","M142 P1 R30 S40 U0.3 V1 O45","G92 E0"})
        REQUIRE(CurvedTransition::layer_code_safe(code));
    for(const std::string code:{"G1 X10","T1","M1020 S0 H1","G92 X0","G92 E1","M106 S255 G1X0","M106 SNaN","{if layer_num>0}G1 X0{endif}"})
        REQUIRE_FALSE(CurvedTransition::layer_code_safe(code));
    auto config=fixed_nozzle_config(0);
    config.set_deserialize_strict({{"layer_change_gcode","{if layer_num>=0}G1 X0{endif}"}});
    Print print;print.is_BBL_printer()=true;Model model;Test::init_print({ramp(2.)},print,model,config);
    model.objects.front()->instances.front()->set_offset(Vec3d(100.,100.,0.));print.apply(model,config);
    REQUIRE_THROWS_WITH(Test::gcode(print),Catch::Matchers::ContainsSubstring("Expanded layer code"));
    config.set_deserialize_strict({{"layer_change_gcode","G92 E0"},{"machine_start_gcode","G28\nT0 H-1"}});
    print.apply(model,config);
    REQUIRE_THROWS_WITH(Test::gcode(print),Catch::Matchers::ContainsSubstring("startup to explicitly select"));
    for(const std::string start:{"G28\nT0 H1e30","G28\nT0 H0\nT255",
                                "G28\nT0 H0\nM1020 S0 H1"}) {
        config.set_deserialize_strict({{"machine_start_gcode",start}});print.apply(model,config);
        REQUIRE_THROWS_WITH(Test::gcode(print),Catch::Matchers::ContainsSubstring("startup to explicitly select"));
    }
    config.set_deserialize_strict({{"machine_start_gcode","G28\nT0 H0\nM1020 S0 H0"}});print.apply(model,config);
    REQUIRE_NOTHROW(Test::gcode(print));
    for(const auto& script:std::vector<std::pair<std::string,std::string>>{
            {"filament_start_gcode","T0 H1"},{"process_change_extrusion_role_gcode","G1 X0"},
            {"layer_change_gcode","{position[0] = 1234}"}}) {
        auto unsafe=config;unsafe.set_deserialize_strict({{script.first,script.second}});print.apply(model,unsafe);
        REQUIRE_THROWS_WITH(Test::gcode(print),Catch::Matchers::ContainsSubstring(
            script.first=="layer_change_gcode"?"Expanded layer code":"Expanded curved custom code"));
    }
}

TEST_CASE("Curved planes slice a curved roof without independent sections", "[curved-engine][planes]")
{
    Print print;Model model;
    const auto config=curved_config();
    Test::init_print({TriangleMesh(AutoPlanesFixtures::curved_top_block())},print,model,config);
    auto* object=model.objects.front();
    const auto before=object->volumes.front()->mesh().its.vertices;
    add_plane(*object,1.7,.02);
    print.apply(model,config);
    REQUIRE(print.objects().size()==1);
    REQUIRE_FALSE(print.objects().front()->is_segment());
    const auto output=Test::gcode(print);
    REQUIRE_FALSE(output.empty());
    REQUIRE(object->volumes.front()->mesh().its.vertices==before);
    REQUIRE(print.objects().front()->curved_field());
    REQUIRE_FALSE(print.objects().front()->curved_ramp());
    const auto* program=dynamic_cast<const CurvedTransition::PlaneProgram*>(print.objects().front()->curved_field().get());
    REQUIRE(program);
    REQUIRE(program->blends().size()==1);
}

TEST_CASE("Automatic slicing infers a bounded roof direction without changing the model", "[curved-engine][automatic]")
{
    auto config=curved_config();
    config.set_deserialize_strict({{"automatic_layer_strategy","1"},{"curved_layer_blend","0"}});
    Print print;Model model;Test::init_print({ramp(2.)},print,model,config);
    const auto vertices=model.objects.front()->volumes.front()->mesh().its.vertices;
    REQUIRE(print.validate().string.empty());
    const auto output=Test::gcode(print);
    const auto* object=print.objects().front();
    REQUIRE(object->curved_field());REQUIRE_FALSE(object->curved_ramp());
    REQUIRE_FALSE(model.objects.front()->has_cut_planes());
    REQUIRE(model.objects.front()->volumes.front()->mesh().its.vertices==vertices);
    REQUIRE(output.find("; layer_strategy: Automatic selection: curved.")!=std::string::npos);
    REQUIRE(output.find("curved blend: traverse above printed material")!=std::string::npos);
}

TEST_CASE("Automatic slicing keeps flat roofs planar and reselects after settings change", "[curved-engine][automatic]")
{
    auto config=curved_config();config.set_deserialize_strict({{"automatic_layer_strategy","1"}});
    Print print;Model model;Test::init_print({TriangleMesh(its_make_cube(20.,20.,3.))},print,model,config);
    auto output=Test::gcode(print);
    REQUIRE_FALSE(print.objects().front()->curved_field());
    REQUIRE(output.find("Automatic selection: planar.")!=std::string::npos);
    REQUIRE(output.find("curved blend: traverse above printed material")==std::string::npos);
    Print tilted;Model tilted_model;Test::init_print({ramp(2.)},tilted,tilted_model,config);
    REQUIRE_FALSE(Test::gcode(tilted).empty());REQUIRE(tilted.objects().front()->curved_field());
    config.set_deserialize_strict({{"nonplanar_virtual_max_angle","0.1"}});
    tilted.apply(tilted_model,config);
    REQUIRE_FALSE(Test::gcode(tilted).empty());
    REQUIRE_FALSE(tilted.objects().front()->curved_field());
}

TEST_CASE("Automatic slicing preserves supports and ignores cut sections on a flat roof fallback", "[curved-engine][automatic]")
{
    auto config=curved_config();config.set_deserialize_strict({{"automatic_layer_strategy","1"},{"enable_support","1"}});
    Print print;Model model;Test::init_print({TriangleMesh(its_make_cube(20.,20.,12.))},print,model,config);
    add_plane(*model.objects.front(),6.,.07);print.apply(model,config);
    REQUIRE(print.validate().string.empty());
    REQUIRE_FALSE(Test::gcode(print).empty());
    REQUIRE(print.objects().size()==1);REQUIRE_FALSE(print.objects().front()->is_segment());
    REQUIRE_FALSE(print.objects().front()->curved_field());
    REQUIRE(print.objects().front()->config().enable_support.value);
    REQUIRE(model.objects.front()->cut_planes.planes.size()==1);
    REQUIRE(print.objects().front()->layer_strategy_decision().find("no useful regional roof")!=std::string::npos);
}

TEST_CASE("Automatic cost comparison protects an already flat roof from underside regression", "[curved-engine][automatic][undersides]")
{
    Model input;
    const std::string path=std::string(TEST_DATA_DIR)+"/curved/Centauri_AB_support_surface_coupon.stl";
    REQUIRE(load_stl(path.c_str(),&input));REQUIRE(input.objects.size()==1);
    // Flatten only the high vertices of this fixture copy. The original
    // sloped roof has a separate regression below. A global underside field
    // would make this already flat roof worse, which must enter the score.
    const auto& volume=*input.objects.front()->volumes.front();
    auto flat=volume.mesh().its;
    size_t flattened=0;
    for(auto& v:flat.vertices) {
        // STL import centres mesh vertices and stores the translation on the
        // volume. Restore fixture coordinates before applying the roof cutoff.
        v=(volume.get_matrix()*v.cast<double>()).cast<float>();
        if(v.z()>7.8f) {v.z()=10.f;++flattened;}
    }
    REQUIRE(flattened>0);
    for(const auto& face:flat.indices) {
        const Vec3f normal=(flat.vertices[face[1]]-flat.vertices[face[0]]).cross(
            flat.vertices[face[2]]-flat.vertices[face[0]]);
        if(normal.z()>0.f) REQUIRE(normal.head<2>().norm()<1e-4f);
    }
    const TriangleMesh mesh(std::move(flat));
    auto config=curved_config();config.set_deserialize_strict({{"curved_layer_blend","0"},
        {"automatic_layer_strategy","0"},{"enable_support","1"},{"support_threshold_angle","30"},
        {"support_type","normal(auto)"},{"support_style","grid"}});
    Print print;Model model;Test::init_print({mesh},print,model,config);
    const auto vertices=model.objects.front()->volumes.front()->mesh().its.vertices;
    ScopedTemporaryFile baseline(".gcode"),automatic(".gcode");
    GCodeProcessorResult planar_preview,auto_preview;
    print.process();print.export_gcode(baseline.string(),&planar_preview,nullptr);
    config.set_deserialize_strict({{"automatic_layer_strategy","1"}});print.apply(model,config);
    print.process();print.export_gcode(automatic.string(),&auto_preview,nullptr);
    const auto* o=print.objects().front();
    REQUIRE_FALSE(o->curved_field());REQUIRE(o->config().enable_support.value);
    REQUIRE_FALSE(o->support_layers().empty());
    REQUIRE(model.objects.front()->volumes.front()->mesh().its.vertices==vertices);
    const auto& decision=o->layer_strategy_decision();INFO(decision);
    REQUIRE(decision.find("Automatic selection: planar.")!=std::string::npos);
    require_best_generated_choice(*o);
    REQUIRE(decision.find("Underside check:")!=std::string::npos);
    REQUIRE(decision.find("Best uniform-layer candidate:")!=std::string::npos);
    REQUIRE(decision.find("Generated benefit/cost choice: Planar.")!=std::string::npos);
    // Material and motion checks for an applied underside/joint field live in
    // the joint fixture, where improving the underside also repays roof cost.
    size_t model_tilted=0,support_tilted=0,transition=0,nonplanar=0;
    Vec3f previous=Vec3f::Zero();
    for(const auto& move:auto_preview.moves) {
        if(move.type==EMoveType::Extrude && move.delta_extruder>0. && move.extrusion_role!=erCustom) {
            if(move.layer_mode==1) ++transition;
            if(move.layer_mode==2) ++nonplanar;
            if((move.position.head<2>()-previous.head<2>()).norm()>.01 && std::abs(move.position.z()-previous.z())>.0001) {
                if(is_support(move.extrusion_role)) ++support_tilted;
                else ++model_tilted;
            }
        }
        previous=move.position;
    }
    REQUIRE(model_tilted==0);REQUIRE(support_tilted==0);REQUIRE(transition==0);REQUIRE(nonplanar==0);
    REQUIRE(auto_preview.moves.size()==planar_preview.moves.size());
    GCodeProcessor imported;imported.process_file(automatic.string());
    REQUIRE(imported.get_result().layer_strategy_decisions.size()==1);
    REQUIRE(imported.get_result().layer_strategy_decisions.front()==decision);
    // A support-angle-only edit used to invalidate supports, but not slicing.
    // The new automatic analysis must be recomputed as well.
    config.set_deserialize_strict({{"support_threshold_angle","20"}});print.apply(model,config);
    REQUIRE_FALSE(Test::gcode(print).empty());
    REQUIRE(print.objects().front()->layer_strategy_decision().find("20.0 deg configured angle")!=std::string::npos);
}

TEST_CASE("Automatic coupon roof choices repay generated cost with supports on or off", "[curved-engine][automatic][regions][undersides]")
{
    Model input;
    const std::string path=std::string(TEST_DATA_DIR)+"/curved/Centauri_AB_support_surface_coupon.stl";
    REQUIRE(load_stl(path.c_str(),&input));
    auto config=curved_config();config.set_deserialize_strict({{"curved_layer_blend","0"},
        {"automatic_layer_strategy","1"},{"support_threshold_angle","30"},
        {"support_type","normal(auto)"},{"support_style","grid"}});
    SECTION("supports off") {config.set_deserialize_strict({{"enable_support","0"}});}
    SECTION("normal supports on") {config.set_deserialize_strict({{"enable_support","1"}});}
    Print print;Model model;
    Test::init_print({input.objects.front()->volumes.front()->mesh()},print,model,config);
    const auto original=model.objects.front()->volumes.front()->mesh().its.vertices;
    print.process();const auto* o=print.objects().front();
    INFO(o->layer_strategy_decision());
    require_best_generated_choice(*o);
    REQUIRE(o->config().enable_support.value==config.opt_bool("enable_support"));
    if(!config.opt_bool("enable_support")) REQUIRE(o->support_layers().empty());
    ScopedTemporaryFile output(".gcode");GCodeProcessorResult result;
    print.export_gcode(output.string(),&result,nullptr);
    size_t top_xyz=0;double top_angle=0.;Vec3f previous=Vec3f::Zero();
    for(const auto& move:result.moves) {
        if(move.type==EMoveType::Extrude && move.delta_extruder>0. && move.extrusion_role==erTopSolidInfill) {
            const auto& normal=result.tool_axes[move.tool_axis_id];
            top_angle=std::max(top_angle,std::atan2(double(normal.head<2>().norm()),std::abs(double(normal.z())))*180./PI);
        }
        if(move.type==EMoveType::Extrude && move.delta_extruder>0. && move.extrusion_role==erTopSolidInfill &&
           (move.position.head<2>()-previous.head<2>()).norm()>.01 &&
           std::abs(move.position.z()-previous.z())>.0001) ++top_xyz;
        previous=move.position;
    }
    if(o->curved_field()) REQUIRE(top_xyz>20);
    else REQUIRE(top_xyz==0);
    // A useful local angle can be smaller than the five-degree ceiling.
    // Full-capability and transition-overshoot checks have separate fixtures.
    if(o->curved_field()) REQUIRE(top_angle>.25);
    REQUIRE(top_angle<=5.0001);
    REQUIRE(model.objects.front()->volumes.front()->mesh().its.vertices==original);
}

TEST_CASE("Support-free underside mapping uses actual generated paths rather than a support toggle gate", "[curved-engine][automatic][undersides]")
{
    auto config=curved_config();config.set_deserialize_strict({{"curved_layer_blend","0"},
        {"automatic_layer_strategy","1"},{"enable_support","0"},{"support_threshold_angle","30"}});
    // Sloped roofs supply a real combined benefit; deliberately tilting a flat
    // roof can now correctly lose the generated cost comparison.
    const TriangleMesh mesh=joint_part();
    Print print;Model model;Test::init_print({mesh},print,model,config);
    // This test exercises the OFF branch directly; the native generated-path
    // guard, not an enable-support prerequisite, decides whether it can export.
    print.process();auto* o=const_cast<PrintObject*>(print.objects().front());
    INFO(o->layer_strategy_decision());
    REQUIRE(o->curved_field());REQUIRE(o->curved_field()->maps_supports());
    REQUIRE_FALSE(o->config().enable_support.value);
    REQUIRE_FALSE(Test::gcode(print).empty());
    require_best_generated_choice(*o);
    REQUIRE(o->support_layers().empty());
    REQUIRE(o->layer_strategy_decision().find("supports off; no support paths requested")!=std::string::npos);
    // Inject an air bridge into the changing-direction slab after ordinary
    // generation. Support OFF must not bypass this independent path guard.
    const auto joint=std::dynamic_pointer_cast<const CurvedTransition::JointField>(o->curved_field());
    REQUIRE(joint);
    const auto program=joint->underside();
    REQUIRE(program);const auto blend=program->blends().front();
    Layer* changing=nullptr;
    for(auto* layer:o->layers())
        if(layer->print_z>blend.lo+.2 && layer->print_z<blend.hi-.2) {changing=layer;break;}
    REQUIRE(changing);
    ExtrusionPath bridge(erBridgeInfill,.08,.45f,.2f);
    bridge.polyline=Polyline3(Points3{Point3(scale_(-.5),0.,0.),Point3(scale_(.5),0.,0.)});
    changing->regions().front()->fills.append(bridge);
    REQUIRE_THROWS_WITH(o->refine_curved_regions_for_paths(),
        Catch::Matchers::ContainsSubstring("bridge crosses a changing layer direction"));
}

TEST_CASE("Automatic slicing checks explicit plane transitions and rejects ambiguous manual rotation", "[curved-engine][automatic]")
{
    auto config=curved_config();config.set_deserialize_strict({{"automatic_layer_strategy","1"}});
    Print print;Model model;Test::init_print({TriangleMesh(its_make_cube(20.,20.,18.))},print,model,config);
    add_plane(*model.objects.front(),4.,.035);add_plane(*model.objects.front(),11.,-.035);print.apply(model,config);
    REQUIRE_FALSE(Test::gcode(print).empty());
    INFO(print.objects().front()->layer_strategy_decision());
    REQUIRE(print.objects().front()->curved_field());
    model.objects.front()->cut_planes.planes.back().point.z()=4.1;print.apply(model,config);
    REQUIRE_FALSE(Test::gcode(print).empty());REQUIRE_FALSE(print.objects().front()->curved_field());
    REQUIRE(print.objects().front()->layer_strategy_decision().find("intersecting")!=std::string::npos);
    config.set_deserialize_strict({{"layer_rotation_b","2"}});print.apply(model,config);
    REQUIRE_FALSE(print.validate().string.empty());
}

TEST_CASE("Automatic planning compares generated joint roof underside and planar candidates", "[curved-engine][automatic][joint]")
{
    auto config=curved_config();config.set_deserialize_strict({{"curved_layer_blend","0"},
        {"automatic_layer_strategy","1"},{"enable_support","1"},{"support_threshold_angle","30"},
        {"support_type","normal(auto)"},{"support_style","grid"}});
    // Elevated underside leaves enough foundation space for a joint field.
    // A faceted roof supplies two different finish directions on the same solid.
    Print print;Model model;Test::init_print({joint_part()},print,model,config);
    const auto original=model.objects.front()->volumes.front()->mesh().its.vertices;
    print.process();const auto* object=print.objects().front();
    INFO(object->layer_strategy_decision());
    REQUIRE(object->layer_strategy_decision().find("Generated benefit/cost choice:")!=std::string::npos);
    REQUIRE(object->layer_strategy_decision().find("Joint half budget:")!=std::string::npos);
    REQUIRE(object->layer_strategy_decision().find("Planar:")!=std::string::npos);
    REQUIRE_FALSE(object->layer_region_records().empty());
    double chosen_score=-INFINITY;bool chosen_roof=false,chosen_under=false;
    std::vector<CurvedTransition::RegionDecision> regions;
    for(const auto& record:object->layer_region_records()) {
        CurvedTransition::RegionDecision r;REQUIRE(CurvedTransition::RegionDecision::decode(record,r));
        REQUIRE(r.motion_seconds>=0.);REQUIRE(r.support_mm3>=0.);
        if(r.selected) {chosen_score=r.gain;chosen_roof|=r.kind==0;chosen_under|=r.kind==1;}
        regions.push_back(std::move(r));
    }
    REQUIRE(std::isfinite(chosen_score));REQUIRE(chosen_score>=0.);
    for(const auto& r:regions) if(!r.selected) REQUIRE(r.gain<=chosen_score+1e-5);
    REQUIRE(std::dynamic_pointer_cast<const CurvedTransition::JointField>(object->curved_field()));
    REQUIRE(chosen_roof);REQUIRE(chosen_under);REQUIRE(object->curved_field()->maps_supports());
    ScopedTemporaryFile output(".gcode");GCodeProcessorResult result;print.export_gcode(output.string(),&result,nullptr);
    REQUIRE_FALSE(result.layer_region_records.empty());
    size_t model_xyz=0,support_xyz=0,top_xyz=0;Vec3f previous=Vec3f::Zero();
    for(const auto& move:result.moves) {
        if(move.type==EMoveType::Extrude && move.delta_extruder>0. && move.extrusion_role!=erCustom) {
            const auto& normal=result.tool_axes[move.tool_axis_id];
            REQUIRE(std::atan2(normal.head<2>().norm(),std::abs(normal.z()))*180./PI<=5.001);
            if((move.position.head<2>()-previous.head<2>()).norm()>.01 && std::abs(move.position.z()-previous.z())>.0001) {
                if(is_support(move.extrusion_role)) ++support_xyz;else ++model_xyz;
                if(move.extrusion_role==erTopSolidInfill) ++top_xyz;
            }
        }
        previous=move.position;
    }
    REQUIRE(model_xyz>20);REQUIRE(support_xyz>20);REQUIRE(top_xyz>20);
    REQUIRE(model.objects.front()->volumes.front()->mesh().its.vertices==original);
    REQUIRE(object->config().enable_support.value);
    const auto stable_decision=object->layer_strategy_decision();
    const auto stable_regions=object->layer_region_records();
    print.process();
    REQUIRE(object->layer_strategy_decision()==stable_decision);
    REQUIRE(object->layer_region_records()==stable_regions);
    // Keep the foundation guard exercised on a field actually selected under
    // the new policy, not on an assumed automatic checkbox result.
    auto* mutable_object=const_cast<PrintObject*>(object);
    REQUIRE(mutable_object->support_layers().size()>1);
    mutable_object->support_layers().front()->support_fills.clear();
    REQUIRE_THROWS(mutable_object->refine_curved_regions_for_paths());
}

TEST_CASE("Mixed joint plates reset the prior object's field before each physical brim", "[curved-engine][joint][brim][multi-model]")
{
    const size_t automatic=GENERATE(0,1);
    auto config=curved_config();config.set_deserialize_strict({{"curved_layer_blend","0"},
        {"automatic_layer_strategy","0"},{"print_sequence","by layer"},
        {"brim_type","outer_only"},{"brim_width","5"},{"enable_support","0"},
        {"support_threshold_angle","30"}});
    Print print;Model model;Test::init_print({joint_part(),joint_part()},print,model,config);
    for(size_t i=0;i<2;++i) {
        model.objects[i]->instances.front()->set_offset(Vec3d(80.+60.*i,100.,0.));
        model.objects[i]->config.set_key_value("automatic_layer_strategy",new ConfigOptionBool(i==automatic));
    }
    print.apply(model,config);print.process();
    REQUIRE(std::dynamic_pointer_cast<const CurvedTransition::JointField>(print.objects()[automatic]->curved_field()));
    REQUIRE_FALSE(print.objects()[1-automatic]->curved_field());
    ScopedTemporaryFile output(".gcode");GCodeProcessorResult result;
    REQUIRE_NOTHROW(print.export_gcode(output.string(),&result,nullptr));
    size_t brim=0,curved=0;
    for(const auto& move:result.moves) if(move.type==EMoveType::Extrude && move.delta_extruder>0.) {
        const auto& normal=result.tool_axes[move.tool_axis_id];
        if(move.extrusion_role==erBrim) {
            ++brim;REQUIRE(normal.head<2>().norm()<1e-6f);
            REQUIRE(move.position.z()==Catch::Approx(.2).margin(.001));
        } else if(normal.head<2>().norm()>1e-5f) ++curved;
    }
    REQUIRE(brim>20);REQUIRE(curved>20);
}

TEST_CASE("Automatic slicing selects independent fields for different models and sparse infill", "[curved-engine][automatic][multi-model]")
{
    auto config=curved_config();config.set_deserialize_strict({{"automatic_layer_strategy","1"}});
    SECTION("several objects") {
        config.set_deserialize_strict({{"print_sequence","by layer"}});
        Print print;Model model;Test::init_print({ramp(2.),TriangleMesh(AutoPlanesFixtures::curved_top_block()),ramp(0.)},print,model,config);
        for(size_t i=0;i<model.objects.size();++i) model.objects[i]->instances.front()->set_offset(Vec3d(45.+60.*i,100.,0.));
        print.apply(model,config);
        REQUIRE_FALSE(Test::gcode(print).empty());
        REQUIRE(print.objects()[0]->curved_field());
        require_best_generated_choice(*print.objects()[0]);
        require_best_generated_choice(*print.objects()[1]);
        REQUIRE_FALSE(print.objects()[2]->curved_field());
    }
    SECTION("sparse infill") {
        config.set_deserialize_strict({{"sparse_infill_density","15%"}});
        const TriangleMesh roof(AutoPlanesFixtures::heightfield_block(2.,2.,1,[](double x){return 10.+.05*x;}));
        Print print;Model model;Test::init_print({roof},print,model,config);
        REQUIRE_FALSE(Test::gcode(print).empty());REQUIRE(print.objects().front()->curved_field());
        REQUIRE(std::dynamic_pointer_cast<const CurvedTransition::Regions>(print.objects().front()->curved_field()));
        REQUIRE(print.objects().front()->printing_region(0).config().sparse_infill_density.value==15.);
    }
    SECTION("copies of one model") {
        config.set_deserialize_strict({{"print_sequence","by layer"}});
        Print print;Model model;Test::init_print({ramp(2.)},print,model,config);
        auto* object=model.objects.front();object->instances.front()->set_offset(Vec3d(60.,100.,0.));
        object->add_instance(*object->instances.front())->set_offset(Vec3d(120.,100.,0.));print.apply(model,config);
        REQUIRE_FALSE(Test::gcode(print).empty());
        REQUIRE(print.num_object_instances()==2);
        for(const auto* o:print.objects()) REQUIRE(o->curved_field());
    }
}

TEST_CASE("Regional cost selection retains planar bridges and normal support constraints", "[curved-engine][regions][supports]")
{
    auto config=curved_config();config.set_deserialize_strict({{"automatic_layer_strategy","1"},{"curved_layer_blend","0"},
        {"support_type","normal(auto)"},{"support_style","grid"}});
    const auto mesh=TriangleMesh(AutoPlanesFixtures::heightfield_block(20.,20.,10,
        [](double x){return 10.+.04*x;},[](double x){return x<2. || x>18.?0.:4.;}));
    SECTION("bridge without supports") {}
    SECTION("normal support") {config.set_deserialize_strict({{"enable_support","1"},{"support_on_build_plate_only","1"}});}
    Print print;Model model;Test::init_print({mesh},print,model,config);
    model.objects.front()->instances.front()->set_offset(Vec3d(100.,100.,0.));print.apply(model,config);
    print.process();const auto* o=print.objects().front();
    require_best_generated_choice(*o);
    const auto original=model.objects.front()->volumes.front()->mesh().its.vertices;
    ScopedTemporaryFile output(".gcode");GCodeProcessorResult result;print.export_gcode(output.string(),&result,nullptr);
    REQUIRE(model.objects.front()->volumes.front()->mesh().its.vertices==original);
    size_t tilted=0,bridge=0,support=0;
    for(const auto& move:result.moves) if(move.type==EMoveType::Extrude && move.extrusion_role!=erCustom) {
        if(is_bridge(move.extrusion_role)) {++bridge;REQUIRE(result.tool_axes[move.tool_axis_id].z()==Catch::Approx(1.).margin(1e-6));}
        if(is_support(move.extrusion_role)) {++support;REQUIRE(result.tool_axes[move.tool_axis_id].z()==Catch::Approx(1.).margin(1e-6));}
        if(result.tool_axes[move.tool_axis_id].z()<.999999) ++tilted;
    }
    if(o->curved_field()) REQUIRE(tilted>0);else REQUIRE(tilted==0);
    REQUIRE(bridge+support>0);
    if(config.opt_bool("enable_support")) REQUIRE(support>0);
}

TEST_CASE("Mixed curved plates refuse neighbour collisions instead of disabling clearance", "[curved-engine][multi-model][clearance]")
{
    auto config=curved_config();config.set_deserialize_strict({{"automatic_layer_strategy","1"},{"print_sequence","by layer"}});
    Print print;Model model;Test::init_print({ramp(2.),ramp(2.)},print,model,config);
    model.objects[0]->instances.front()->set_offset(Vec3d(100.,100.,0.));
    model.objects[1]->instances.front()->set_offset(Vec3d(122.,100.,0.));print.apply(model,config);
    REQUIRE_THROWS(Test::gcode(print));
}

TEST_CASE("Curved plate validates generated brim and skirt reach", "[curved-engine][clearance]")
{
    auto config=curved_config();config.set_deserialize_strict({{"automatic_layer_strategy","1"},
        {"printable_area","0x0,100x0,100x100,0x100"},{"brim_type","outer_only"},{"brim_width","6"}});
    // Generate outside the common machine area on purpose. Otherwise the brim
    // generator's independent bed clip can hide the paths this guard must test.
    // Do not depend on Model's shared bed polygon left by another test.
    config.set_key_value("extruder_printable_area",new ConfigOptionPointsGroups({
        {Vec2d(-200.,-200.),Vec2d(200.,-200.),Vec2d(200.,200.),Vec2d(-200.,200.)}}));
    SECTION("brim") {}
    SECTION("skirt") {config.set_deserialize_strict({{"brim_type","no_brim"},{"skirt_loops","1"},{"skirt_distance","6"}});}
    Print print;Model model;Test::init_print({ramp(2.)},print,model,config);
    // The source ramp spans [0,20]; the instance translation is its lower-left
    // location, not its centre. Keep the model on the bed but adhesion outside.
    model.objects.front()->instances.front()->set_offset(Vec3d(2.,50.,0.));print.apply(model,config);
    REQUIRE(model.objects.front()->instance_bounding_box(0).min.x()==Catch::Approx(2.));
    print.process();REQUIRE(print.objects().front()->curved_field());
    REQUIRE_FALSE(print.first_layer_convex_hull().empty());
    const auto adhesion_bounds=get_extents(print.first_layer_convex_hull());
    INFO("first-layer hull x="<<unscale<double>(adhesion_bounds.min.x())<<","<<unscale<double>(adhesion_bounds.max.x()));
    REQUIRE(unscale<double>(adhesion_bounds.min.x())<0.);
    ScopedTemporaryFile output(".gcode");
    REQUIRE_THROWS_WITH(print.export_gcode(output.string(),nullptr,nullptr),
        Catch::Matchers::ContainsSubstring("brim/skirt exceeds"));
}

TEST_CASE("Sequential curved plates include generated adhesion paths in neighbour clearance", "[curved-engine][multi-model][clearance]")
{
    auto config=curved_config();config.set_deserialize_strict({{"automatic_layer_strategy","1"},
        {"print_sequence","by object"},{"brim_type","no_brim"},{"brim_width","50"},
        {"printable_area","-350x-350,350x-350,350x350,-350x350"}});
    config.set_key_value("extruder_printable_area",new ConfigOptionPointsGroups({
        {Vec2d(-350.,-350.),Vec2d(350.,-350.),Vec2d(350.,350.),Vec2d(-350.,350.)}}));
    bool adhesion=false;
    SECTION("model spacing without adhesion") {}
    SECTION("wide brim approaches an earlier part") {
        adhesion=true;config.set_deserialize_strict({{"brim_type","outer_only"}});
    }
    Print print;Model model;Test::init_print({ramp(2.),ramp(2.)},print,model,config);
    model.objects[0]->instances.front()->set_offset(Vec3d(100.,100.,0.));
    model.objects[1]->instances.front()->set_offset(Vec3d(180.,100.,0.));print.apply(model,config);
    print.process();REQUIRE(print.objects()[0]->curved_field());REQUIRE(print.objects()[1]->curved_field());
    if(adhesion) {
        const auto hull=get_extents(print.first_layer_convex_hull());
        REQUIRE(unscale<double>(hull.min.x())>0.);
        REQUIRE(unscale<double>(hull.max.x())<300.);
        REQUIRE(unscale<double>(hull.max.y())<300.);
        ScopedTemporaryFile output(".gcode");
        REQUIRE_THROWS_WITH(print.export_gcode(output.string(),nullptr,nullptr),
            Catch::Matchers::ContainsSubstring("too close"));
    } else REQUIRE_FALSE(Test::gcode(print).empty());
}

TEST_CASE("Full Centauri MCT-1 compares roof benefit and preserves planar support paths", "[curved-engine][regions][totem]")
{
    Model input;
    const std::string path=std::string(TEST_DATA_DIR)+"/curved/MCT-1_Centauri_5deg.stl";
    REQUIRE(load_stl(path.c_str(),&input));REQUIRE(input.objects.size()==1);
    const auto& mesh=input.objects.front()->volumes.front()->mesh();
    REQUIRE(mesh.its.indices.size()==22146);
    auto config=curved_config();config.set_deserialize_strict({{"automatic_layer_strategy","0"},{"curved_layer_blend","0"},
        {"sparse_infill_density","15%"},{"top_shell_thickness","1"},{"enable_support","1"},
        {"support_type","normal(auto)"},{"support_style","grid"},{"support_on_build_plate_only","0"},
        {"support_threshold_angle","60"},{"support_remove_small_overhang","0"}});
    Print print;Model model;Test::init_print({mesh},print,model,config);
    // The imported mesh is centred about Z=0. Preserve ensure_on_bed()'s
    // vertical lift while moving XY; zeroing Z would sink half of the Temple.
    auto* instance=model.objects.front()->instances.front();
    Vec3d offset=instance->get_offset();offset.head<2>()=Vec2d(100.,100.);
    instance->set_offset(offset);
    model.objects.front()->invalidate_bounding_box();
    const auto placed_bounds=model.objects.front()->instance_bounding_box(*instance);
    REQUIRE(placed_bounds.min.z()==Catch::Approx(0.));
    REQUIRE(placed_bounds.size().z()==Catch::Approx(28.));
    print.apply(model,config);
    const auto original=model.objects.front()->volumes.front()->mesh().its.vertices;
    // This regression needs real supports alongside the curved roofs. The
    // Temple's base blocks build-plate-only columns from reaching its roofs.
    // Verify the unmodified planar control generates support before enabling
    // the automatic field; do not infer support from the settings checkbox.
    print.process();
    REQUIRE_FALSE(print.objects().front()->curved_field());
    REQUIRE_FALSE(print.objects().front()->support_layers().empty());
    config.set_deserialize_strict({{"automatic_layer_strategy","1"}});
    print.apply(model,config);
    print.process();
    INFO(print.objects().front()->layer_strategy_decision());
    require_best_generated_choice(*print.objects().front());
    REQUIRE_FALSE(print.objects().front()->support_layers().empty());
    ScopedTemporaryFile output(".gcode");GCodeProcessorResult result;print.export_gcode(output.string(),&result,nullptr);
    REQUIRE(model.objects.front()->volumes.front()->mesh().its.vertices==original);
    size_t curved=0,support=0;
    for(const auto& move:result.moves) if(move.type==EMoveType::Extrude && move.extrusion_role!=erCustom) {
        if(result.tool_axes[move.tool_axis_id].z()<.999999) ++curved;
        if(is_support(move.extrusion_role)) {
            ++support;
            REQUIRE(move.layer_mode==0);
            REQUIRE(result.tool_axes[move.tool_axis_id].head<2>().norm()<1e-6f);
        }
    }
    if(print.objects().front()->curved_field()) REQUIRE(curved>0);else REQUIRE(curved==0);
    REQUIRE(support>0);
}

TEST_CASE("Curved plane project reload preserves world positions when the volume origin moves", "[curved-engine][planes][3mf]")
{
    Model model;
    auto* object=model.add_object();
    auto* volume=object->add_volume(TriangleMesh(its_make_cube(20.,20.,12.)));
    auto* instance=object->add_instance();
    instance->set_offset(Vec3d(128.,128.,0.));
    REQUIRE(volume->get_offset().norm()>1.);
    add_plane(*object,6.,.035);
    const Vec3d world_point=instance->get_matrix()*object->cut_planes.planes.front().point;
    ScopedTemporaryDir backup("curved_3mf");model.set_backup_path(backup.string());
    ScopedTemporaryFile temp(".3mf");const std::string path=temp.string();
    auto config=curved_config();
    PlateData plate;plate.plate_index=0;
    StoreParams params;
    params.path=path.c_str();params.model=&model;params.config=&config;
    params.plate_data_list.push_back(&plate);
    params.strategy=SaveStrategy::Zip64|SaveStrategy::Silence;
    REQUIRE(store_bbs_3mf(params));
    Model loaded;DynamicPrintConfig loaded_config;
    ConfigSubstitutionContext context{ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs plates;std::vector<Preset*> presets;
    bool is_bbl=false,is_orca=false;Semver version;
    REQUIRE(load_bbs_3mf(path.c_str(),&loaded_config,&context,&loaded,&plates,&presets,
                        &is_bbl,&is_orca,&version,nullptr,LoadStrategy::LoadModel|LoadStrategy::LoadConfig));
    release_PlateData_list(plates);
    REQUIRE(loaded.objects.size()==1);
    const auto* restored=loaded.objects.front();
    REQUIRE(restored->cut_planes.planes.size()==1);
    const Vec3d restored_world=restored->instances.front()->get_matrix()*restored->cut_planes.planes.front().point;
    REQUIRE((restored_world-world_point).norm()<1e-6);
    REQUIRE((restored->cut_planes.planes.front().normal-object->cut_planes.planes.front().normal).norm()<1e-6);
    Print print;print.apply(loaded,config);
    REQUIRE(print.validate().string.empty());
    REQUIRE_FALSE(Test::gcode(print).empty());
}

TEST_CASE("Curved plane mesh mapping keeps a through hole and bounded facet error", "[curved-engine][planes][mesh]")
{
    using namespace CurvedTransition;
    const auto mesh=holed_part().its;
    PlaneProgram program({-10.,10.,-10.,10.,12.,.07},.2,.2,{{6.,.07,0.}});
    const auto mapped=map_mesh(mesh,program,[]{});
    REQUIRE(mapped.indices.size()>mesh.indices.size());
    REQUIRE(its_num_open_edges(mapped)==0);
    // Genus-one boundary: refinement preserves the bore's Euler characteristic.
    std::set<std::pair<int,int>> edges;
    for(const auto& f:mapped.indices) for(int i=0;i<3;++i) {
        const auto edge=std::minmax(f[i],f[(i+1)%3]);edges.emplace(edge.first,edge.second);
    }
    REQUIRE(int64_t(mapped.vertices.size())-int64_t(edges.size())+int64_t(mapped.indices.size())==0);
    for(const auto& f:mapped.indices) {
        Vec3d p=Vec3d::Zero(); double physical=0.;
        for(int i=0;i<3;++i) {
            const Vec3d q=mapped.vertices[f[i]].cast<double>();p+=q/3.;
            physical+=program.world_z(q.x(),q.y(),q.z())/3.;
        }
        REQUIRE(std::abs(program.world_z(p.x(),p.y(),p.z())-physical)<=.0101);
    }
    Print print;Model model; const auto config=curved_config();
    Test::init_print({holed_part()},print,model,config);
    add_plane(*model.objects.front(),6.,.07);
    print.apply(model,config); print.process();
    REQUIRE(print.objects().size()==1);
    const auto* object=print.objects().front();
    size_t holes=0;
    for(const auto* layer:object->layers()) for(const auto& poly:layer->lslices) holes+=poly.holes.size();
    REQUIRE(holes>object->layers().size()/2);
    ScopedTemporaryFile temp(".gcode");
    GCodeProcessorResult result;
    print.export_gcode(temp.string(),&result,nullptr);
    const double bound=object->curved_domain()->top_max;
    REQUIRE(bound>12.);
    double actual_top=0.;size_t traverses=0;
    for (size_t i=1;i<result.moves.size();++i) {
        const auto& move=result.moves[i];const auto& prior=result.moves[i-1];
        if (move.type==EMoveType::Extrude && move.extrusion_role!=erCustom) {
            actual_top=std::max(actual_top,double(move.position.z()));
            REQUIRE(move.position.z()<=bound+.002);
        }
        if (move.type==EMoveType::Travel && actual_top>0. &&
            (move.position.head<2>()-prior.position.head<2>()).norm()>.001f) {
            ++traverses;
            REQUIRE(move.position.z()>=actual_top+1.999);
            REQUIRE(prior.position.z()>=actual_top+1.999);
        }
    }
    // This flat CAD roof crosses tilted layer contours: midpoint sampling
    // actually deposits a fraction of a layer above its nominal 12 mm top.
    REQUIRE(actual_top>12.01);
    REQUIRE(traverses>0);
    auto restricted=config;
    restricted.set_deserialize_strict({{"nonplanar_virtual_max_z","14.01"}});
    print.apply(model,restricted);
    REQUIRE_THROWS(Test::gcode(print));
}

TEST_CASE("Curved engine blends two plane changes and refuses intersecting branches", "[curved-engine][planes]")
{
    Print print;Model model; const auto config=curved_config();
    Test::init_print({TriangleMesh(its_make_cube(20.,20.,18.))},print,model,config);
    add_plane(*model.objects.front(),4.,.035);
    add_plane(*model.objects.front(),11.,-.035);
    print.apply(model,config); print.process();
    REQUIRE(print.objects().size()==1);
    const auto* program=dynamic_cast<const CurvedTransition::PlaneProgram*>(print.objects().front()->curved_field().get());
    REQUIRE(program); REQUIRE(program->blends().size()==2);
    REQUIRE_FALSE(Test::gcode(print).empty());
    model.objects.front()->cut_planes.planes.back().point.z()=4.1;
    print.apply(model,config);
    REQUIRE_THROWS(Test::gcode(print));
}

TEST_CASE("Curved blend slices the flattened solid and writes physical paths", "[curved-engine]")
{
    Print print; Model model;
    Test::init_print({ramp()},print,model,curved_config());
    REQUIRE(print.validate().string.empty());
    const auto original_vertices=model.objects.front()->volumes.front()->mesh().its.vertices;
    const std::string gcode=Test::gcode(print);
    const auto* object=print.objects().front();
    REQUIRE(object->curved_field());
    REQUIRE(object->curved_ramp());
    REQUIRE(model.objects.front()->volumes.front()->mesh().its.vertices==original_vertices);
    REQUIRE(std::abs(object->layers().back()->print_z-object->curved_field()->reference_top_mm())<1e-8);
    REQUIRE(gcode.find("curved blend: traverse above printed material")!=std::string::npos);
    REQUIRE(gcode.find("G2 ")==std::string::npos);
    REQUIRE(gcode.find("G3 ")==std::string::npos);
    // Verify every native deposition has XYZ, stays above the foundation and
    // below the true roof, including paths running across X and off-axis infill.
    std::istringstream in(gcode); std::string line;
    size_t extrusions=0; double minz=1e9,maxz=0.;
    const std::regex zword(R"( Z(-?\d*\.?\d+))");
    while(std::getline(in,line)) {
        const auto code=line.substr(0,line.find(';'));
        if (code.rfind("G0",0)==0 || code.rfind("G1",0)==0) {
            REQUIRE(code.find(" B")==std::string::npos);
            REQUIRE(code.find(" C")==std::string::npos);
        }
        if (code.rfind("G1",0)!=0 || code.find(" E")==std::string::npos || code.find(" X")==std::string::npos) continue;
        REQUIRE(code.find(" Y")!=std::string::npos);
        std::smatch z; REQUIRE(std::regex_search(code,z,zword));
        minz=std::min(minz,std::stod(z[1])); maxz=std::max(maxz,std::stod(z[1])); ++extrusions;
    }
    REQUIRE(extrusions>1000);
    REQUIRE(minz>=.199);
    REQUIRE(maxz<=object->curved_ramp()->top_max+.001);
    REQUIRE(maxz>object->curved_field()->reference_top_mm()+.1);
    const std::regex header(R"(; max_z_height: (-?\d*\.?\d+))");
    std::smatch reported;REQUIRE(std::regex_search(gcode,reported,header));
    REQUIRE(std::stod(reported[1])>=maxz-.005);
}

TEST_CASE("Curved blend Preview receives the same physical deposition and bounded travel", "[curved-engine][preview]")
{
    Print print; Model model;
    Test::init_print({ramp()},print,model,curved_config());
    print.process();
    ScopedTemporaryFile temp(".gcode");
    GCodeProcessorResult result;
    print.export_gcode(temp.string(),&result,nullptr);
    REQUIRE(result.virtual_5axis);
    REQUIRE(result.tool_axis_fixed);
    REQUIRE(result.fixed_tool_axis.isApprox(Vec3f(0.f,0.f,1.f),1e-6f));
    REQUIRE(result.tool_axes.size()>1);
    const auto* object=print.objects().front();
    const auto& roof=*object->curved_ramp();
    const auto& field=*object->curved_field();
    const Vec2d shift=unscale(object->instances().front().shift);
    size_t deposits=0, traverses=0, lower_traverses=0; double maxz=0., minh=1e9, maxh=0.;
    for (size_t i=1;i<result.moves.size();++i) {
        const auto& move=result.moves[i];
        const auto& prior=result.moves[i-1];
        if (move.type==EMoveType::Extrude && move.extrusion_role!=erCustom) {
            ++deposits; maxz=std::max(maxz,double(move.position.z()));
            minh=std::min(minh,double(move.height)); maxh=std::max(maxh,double(move.height));
            REQUIRE(move.position.z()>=.199);
            const double x=move.position.x()-shift.x(),y=move.position.y()-shift.y();
            REQUIRE(move.position.z()<=roof.roof(x,y)+.002);
            REQUIRE(move.tool_axis_id<result.tool_axes.size());
            const Vec3f normal=result.tool_axes[move.tool_axis_id];
            REQUIRE(move.delta_extruder>0.);
            const auto grad=field.gradient(x,y,field.slice_z(x,y,move.position.z()));
            REQUIRE(std::hypot(grad.dx,grad.dy)<=roof.slope()+1e-6);
            const Vec3f expected=Vec3f(float(-grad.dx),float(-grad.dy),1.f).normalized();
            REQUIRE((normal-expected).norm()<.002f); // midpoint tag versus rounded endpoint
        }
        if (move.type==EMoveType::Travel &&
            (move.position.head<2>()-prior.position.head<2>()).norm()>.001f &&
            deposits>0) {
            ++traverses;
            REQUIRE(prior.position.z()>=maxz+1.999);
            REQUIRE(move.position.z()>=maxz+1.999);
            REQUIRE(std::abs(move.position.z()-prior.position.z())<.002);
            if(move.position.z()<roof.top_max+1.9) ++lower_traverses;
        }
    }
    REQUIRE(deposits>1000);
    REQUIRE(traverses>0);
    REQUIRE(lower_traverses>0);
    REQUIRE(minh>=print.config().min_layer_height.get_at(0)-.002);
    REQUIRE(maxh<=print.config().max_layer_height.get_at(0)+.002);
    REQUIRE(maxh-minh>.04); // physical spacing changes through the blend
    REQUIRE(maxz>field.reference_top_mm()+.1);
}

TEST_CASE("Curved blend rejects unsupported inputs and excessive tilt", "[curved-engine]")
{
    SECTION("supports") {
        auto config=curved_config(); config.set_deserialize_strict({{"enable_support","1"}});
        Print p; Model m; Test::init_print({ramp()},p,m,config);
        REQUIRE_FALSE(p.validate().string.empty());
        REQUIRE_THROWS(Test::gcode(p));
    }
    SECTION("several manual curved models") {
        auto config=curved_config();config.set_deserialize_strict({{"print_sequence","by layer"}});
        Print p;Model m;Test::init_print({ramp(2.),ramp(3.)},p,m,config);
        m.objects[0]->instances.front()->set_offset(Vec3d(60.,100.,0.));
        m.objects[1]->instances.front()->set_offset(Vec3d(120.,100.,0.));p.apply(m,config);
        REQUIRE(p.validate().string.empty());
        REQUIRE_FALSE(Test::gcode(p).empty());
        REQUIRE(p.objects()[0]->curved_field());REQUIRE(p.objects()[1]->curved_field());
    }
    SECTION("draft shield outside the curved domain") {
        auto config=curved_config(); config.set_deserialize_strict({{"draft_shield","enabled"}});
        Print p; Model m; Test::init_print({ramp()},p,m,config);
        REQUIRE_FALSE(p.validate().string.empty());
        REQUIRE_THROWS(Test::gcode(p));
    }
    SECTION("surface wipe is not a retracted travel") {
        auto config=curved_config(); config.set_deserialize_strict({{"wipe_on_loops","1"}});
        Print p; Model m; Test::init_print({ramp()},p,m,config);
        REQUIRE_FALSE(p.validate().string.empty());
        REQUIRE_THROWS(Test::gcode(p));
    }
    SECTION("curved roof not yet implemented") {
        Print p; Model m;
        Test::init_print({TriangleMesh(AutoPlanesFixtures::curved_top_block())},p,m,curved_config());
        REQUIRE_THROWS(Test::gcode(p));
    }
    SECTION("roof beyond configured limit") {
        Print p; Model m; Test::init_print({ramp(6.)},p,m,curved_config());
        REQUIRE_THROWS(Test::gcode(p));
    }
    SECTION("infeasible layer spacing") {
        auto config=curved_config(); config.set_deserialize_strict({{"min_layer_height","0.18"},{"max_layer_height","0.21"}});
        Print p; Model m; Test::init_print({ramp()},p,m,config);
        REQUIRE_THROWS(Test::gcode(p));
    }
}

TEST_CASE("Curved blend config round trip and disabling restores planar slices", "[curved-engine]")
{
    auto config=curved_config();
    REQUIRE(config.opt_bool("curved_layer_blend"));
    DynamicPrintConfig restored;
    restored.set_deserialize_strict("curved_layer_blend",config.option("curved_layer_blend")->serialize());
    REQUIRE(restored.opt_bool("curved_layer_blend"));
    Print p; Model m; Test::init_print({ramp()},p,m,config);
    const auto curved=Test::gcode(p);
    config.set_deserialize_strict({{"curved_layer_blend","0"}});
    p.apply(m,config);
    const auto planar=Test::gcode(p);
    REQUIRE_FALSE(p.objects().front()->curved_field());
    REQUIRE(planar.find("curved blend: traverse above printed material")==std::string::npos);
    Print fresh; fresh.apply(m,config);
    const auto clean=Test::gcode(fresh);
    auto moves=[](const std::string& text) {
        std::istringstream in(text); std::string line,out;
        while(std::getline(in,line)) if (line.rfind("G1",0)==0 || line.rfind("G0",0)==0 || line.rfind("G2",0)==0 || line.rfind("G3",0)==0)
            out+=line.substr(0,line.find(';'))+"\n";
        return out;
    };
    REQUIRE(moves(planar)==moves(clean));
}

TEST_CASE("Curved slicing keeps instance placement and mirrored cardinal slopes", "[curved-engine][transform]")
{
    for (bool mirror:{false,true}) for (int quarter=0;quarter<4;++quarter) {
        CAPTURE(mirror,quarter);
        auto config=curved_config();
        Print print; Model model;
        Test::init_print({ramp(4.)},print,model,config);
        auto* object=model.objects.front();
        if (mirror) object->volumes.front()->set_mirror(X,-1.);
        object->instances.front()->set_rotation(Z,quarter*PI*.5);
        object->instances.front()->set_offset(Vec3d(40.,50.,0.));
        object->invalidate_bounding_box();
        print.apply(model,config);
        REQUIRE(print.validate().string.empty());
        print.process();
        const auto* sliced=print.objects().front();
        REQUIRE(sliced->curved_field());
        const auto& roof=*sliced->curved_ramp();
        const double sign=mirror?-1.:1.;
        REQUIRE(std::abs(roof.gx-sign*std::tan(4.*PI/180.)*std::cos(quarter*PI*.5))<1e-6);
        REQUIRE(std::abs(roof.gy-sign*std::tan(4.*PI/180.)*std::sin(quarter*PI*.5))<1e-6);
        REQUIRE(std::abs(roof.top_min-3.)<1e-5);
        REQUIRE(std::abs(roof.top_max-(3.+20.*std::tan(4.*PI/180.)))<1e-5);
        REQUIRE(std::abs(sliced->curved_field()->world_z(roof.xmin,roof.ymin,sliced->layers().back()->print_z)-
                         roof.roof(roof.xmin,roof.ymin))<1e-8);
    }
}

TEST_CASE("Curved writer subdivides paths, compensates flow and avoids diagonal travel", "[curved-engine]")
{
    using namespace CurvedTransition;
    Ramp r{0.,20.,0.,20.,3.,.08,0.,3.,4.6};
    auto field=r.field(.2);
    GCodeWriter writer;
    writer.set_extruders({0}); writer.set_extruder(0);
    const Vec2d origin(100.,80.);
    writer.set_curved_blend(field,r.domain(),origin,2.,5.);
    writer.set_curved_path(2.,.45,.2);
    const auto approach=writer.travel_to_xyz(Vec3d(100.,90.,2.));
    REQUIRE(approach.find("Z6.6")!=std::string::npos);
    REQUIRE(std::abs(writer.get_position().z()-field->world_z(0.,10.,2.))<1e-9);
    const double before=writer.filament()->used_filament();
    const auto extrusion=writer.extrude_to_xy(Vec2d(120.,90.),1.);
    REQUIRE(std::count(extrusion.begin(),extrusion.end(),'\n')>100);
    REQUIRE(std::abs(writer.get_position().z()-field->world_z(20.,10.,2.))<1e-9);
    REQUIRE(writer.filament()->used_filament()>before);
    // Independent fine-grid volume integral. A unit nominal E along 20 mm
    // should deliver the mapped bead's area times physical path length.
    double expected=0.;
    const double dx=20./10000;
    for (int i=0;i<10000;++i) {
        const double x=(i+.5)*dx;
        const double h=(field->world_z(x,10.,2.)-field->world_z(x,10.,1.8))/
            std::sqrt(1.+std::pow(field->gradient(x,10.,2.).dx,2));
        const double dz=field->world_z(x+dx*.5,10.,2.)-field->world_z(x-dx*.5,10.,2.);
        expected+=Ramp::bead_area(.45,h)/Ramp::bead_area(.45,.2)*std::hypot(dx,dz)/20.;
    }
    REQUIRE(std::abs((writer.filament()->used_filament()-before)-expected)<.0001);
    REQUIRE(writer.max_extrusion_z()>0.);
    REQUIRE_THROWS(writer.extrude_arc_to_xy(Vec2d(120.,100.),Vec2d(0.,5.),1.,true));
    REQUIRE_THROWS(writer.extrude_to_xyz(Vec3d(120.,100.,3.),1.));
    REQUIRE_THROWS(writer.extrude_to_xy(Vec2d(120.,100.),0.,"surface wipe",true));
    // Classic walls can generate narrow gap-fill paths. Keep upstream's area
    // convention and bound the maximum flow even when its height maximum is
    // not where the concave area expression reaches its maximum.
    writer.set_curved_path(2.,.09,.2);
    REQUIRE(writer.curved_max_flow_scale()>1.);
    writer.travel_to_xyz(Vec3d(100.,90.,2.));
    REQUIRE_FALSE(writer.extrude_to_xy(Vec2d(120.,90.),1.).empty());
    REQUIRE_THROWS(writer.set_curved_path(2.,.02,.2));
    const auto layer_change=writer.travel_to_z(2.2);
    REQUIRE(layer_change.find("F300")!=std::string::npos);
}

TEST_CASE("Affine tilted extrusion uses one exact segment while blends retain bounded chords", "[curved-engine][flow]")
{
    using namespace CurvedTransition;
    const ModelDomain domain{0.,20.,0.,20.,30.,.1};
    const auto field=std::make_shared<PlaneProgram>(domain,.2,.2,
        std::vector<PlaneProgram::Target>{{6.,.03,0.}},.1);
    double w=15.;bool terminal=true;
    SECTION("completed tilt") {}
    SECTION("changing direction") {terminal=false;w=(field->blends().front().lo+field->blends().front().hi)*.5;}
    GCodeWriter writer;writer.set_extruders({0});writer.set_extruder(0);
    writer.set_curved_plate_bound(35.);writer.set_curved_blend(field,domain,Vec2d::Zero(),2.,5.);
    writer.set_curved_path(w,.45,.2);writer.travel_to_xyz(Vec3d(1.,10.,w));
    const auto output=writer.extrude_to_xy(Vec2d(19.,10.),1.);
    size_t count=0;double total_e=0.;std::string line;std::istringstream lines(output);
    const std::regex eword(R"( E(-?\d*\.?\d+))");
    const std::regex xword(R"( X(-?\d*\.?\d+))"),zword(R"( Z(-?\d*\.?\d+))");
    Vec2d previous(1.,field->world_z(1.,10.,w));
    while(std::getline(lines,line)) if(line.rfind("G1 ",0)==0) {
        std::smatch e;if(std::regex_search(line,e,eword)) {
            ++count;total_e+=std::stod(e[1]);
            std::smatch x,z;REQUIRE(std::regex_search(line,x,xword));REQUIRE(std::regex_search(line,z,zword));
            const Vec2d end(std::stod(x[1]),std::stod(z[1]));
            for(int i=0;i<=100;++i) {
                const double t=i/100.;const Vec2d p=previous+t*(end-previous);
                REQUIRE(std::abs(field->world_z(p.x(),10.,w)-p.y())<=.0056);
            }
            previous=end;
        }
    }
    if(terminal) {
        REQUIRE(count==1);
        // Exact tilted-line length and normal bead height cancel here; area
        // includes the existing rounded bead-end term, so integrate its own
        // independently computed constant physical cross section.
        const double h=.2/std::sqrt(1.+.03*.03);
        const double area=.45*h-h*h*(1.-PI/4.);
        const double baseline=.45*.2-.2*.2*(1.-PI/4.);
        REQUIRE(total_e==Catch::Approx(std::sqrt(1.+.03*.03)*area/baseline).margin(2e-5));
    } else {
        REQUIRE(count>1);REQUIRE(count<1000);
        // An independent, much finer integral verifies retained volume, rather
        // than trusting the subdivision routine's own integration samples.
        double expected=0.;constexpr int n=10000;const double dx=18./n;
        for(int i=0;i<n;++i) {
            const double x=1.+(i+.5)*dx;
            const auto gradient=field->gradient(x,10.,w);
            const double h=(field->world_z(x,10.,w)-field->world_z(x,10.,w-.2))/
                std::sqrt(1.+gradient.dx*gradient.dx+gradient.dy*gradient.dy);
            const double dz=field->world_z(x+dx*.5,10.,w)-field->world_z(x-dx*.5,10.,w);
            expected+=Ramp::bead_area(.45,h)/Ramp::bead_area(.45,.2)*std::hypot(dx,dz)/18.;
        }
        REQUIRE(total_e==Catch::Approx(expected).margin(1e-4));
    }
    REQUIRE(writer.get_position().x()==19.);
    REQUIRE(writer.get_position().z()==Catch::Approx(field->world_z(19.,10.,w)).margin(1e-9));
}

TEST_CASE("Curved travel clears prior deposition and batches an XY detour", "[curved-engine][travel]")
{
    using namespace CurvedTransition;
    Ramp r{0.,20.,0.,20.,3.,.08,0.,3.,4.6};
    const auto field=r.field(.2);
    GCodeWriter writer;
    writer.set_extruders({0});writer.set_extruder(0);
    writer.set_curved_plate_bound(30.);
    writer.set_curved_blend(field,r.domain(),Vec2d::Zero(),2.,5.);
    writer.set_curved_path(2.,.45,.2);
    // A custom/unknown start still takes the full-plate route.
    REQUIRE(writer.travel_to_xyz(Vec3d(0.,10.,2.)).find("Z30")!=std::string::npos);
    writer.extrude_to_xy(Vec2d(20.,10.),1.);
    const double printed=writer.max_extrusion_z();
    REQUIRE(printed<r.top_max);
    const std::string route=writer.travel_to_curved_path(
        {Vec3d(20.,12.,2.),Vec3d(0.,12.,2.),Vec3d(0.,10.,2.)},"final approach");
    size_t departures=0,traverses=0,approaches=0;
    std::istringstream lines(route);std::string line;
    const std::regex zword(R"( Z(-?\d*\.?\d+))");
    while(std::getline(lines,line)) {
        if(line.find("vertical departure")!=std::string::npos) ++departures;
        if(line.find("final approach")!=std::string::npos) ++approaches;
        if(line.find("traverse above printed material")!=std::string::npos) {
            ++traverses;std::smatch z;REQUIRE(std::regex_search(line,z,zword));
            const double height=std::stod(z[1]);
            // Include the tilted bead edge and never use the unprinted roof.
            REQUIRE(height>=printed+2.+.45*.5*r.slope()-1e-6);
            REQUIRE(height<r.top_max+2.);
        }
    }
    REQUIRE(departures==1);REQUIRE(traverses==3);REQUIRE(approaches==1);
    REQUIRE(std::abs(writer.get_position().z()-field->world_z(0.,10.,2.))<1e-9);
    REQUIRE(writer.travel_to_xyz(Vec3d(0.,10.,2.)).empty());
    // Changing the mapping must retain material laid by another object/support.
    writer.set_curved_blend(std::make_shared<Identity>(),{0.,20.,0.,20.,28.,0.},Vec2d::Zero(),2.,5.);
    writer.set_curved_path(8.,.45,.2);
    writer.travel_to_xyz(Vec3d(0.,10.,8.));writer.extrude_to_xy(Vec2d(20.,10.),1.);
    writer.set_curved_blend(field,r.domain(),Vec2d::Zero(),2.,5.);
    writer.set_curved_path(2.,.45,.2);
    const auto reentry=writer.travel_to_xyz(Vec3d(0.,10.,2.));
    std::smatch raised;REQUIRE(std::regex_search(reentry,raised,zword));
    REQUIRE(std::stod(raised[1])>=10.018-1e-6);
    REQUIRE(std::stod(raised[1])<10.021);
    writer.travel_to_z(2.2);
    const double staged=writer.get_position().z();
    for(int i=0;i<5;++i) writer.travel_to_z(2.2+.2*i);
    REQUIRE(writer.get_position().z()==staged);
    writer.set_current_position_clear(false);
    REQUIRE(writer.travel_to_xyz(Vec3d(0.,10.,2.)).find("Z30")!=std::string::npos);
}

TEST_CASE("Route-specific writer clearance retains other objects and unknown-motion fallback", "[curved-engine][travel][motion]")
{
    using namespace CurvedTransition;
    PrintConfig config;config.apply(curved_config(),true);
    config.travel_speed_z.values={3.};
    GCodeWriter writer;writer.apply_print_config(config);writer.set_extruders({0});writer.set_extruder(0);
    const auto identity=std::make_shared<Identity>();
    const ModelDomain domain{0.,200.,0.,200.,40.,0.};
    writer.set_curved_plate_bound(45.);writer.set_curved_blend(identity,domain,Vec2d::Zero(),2.,5.);
    REQUIRE(writer.curved_effective_z_speed()==3.);
    writer.set_curved_path(30.,.45,.2);
    writer.travel_to_xyz(Vec3d(190.,190.,30.));writer.extrude_to_xy(Vec2d(195.,190.),1.);
    // A second object's identity mapping must not clear the first object's map.
    writer.set_curved_blend(identity,domain,Vec2d::Zero(),2.,5.);
    writer.set_curved_path(2.,.45,.2);
    writer.travel_to_xyz(Vec3d(10.,10.,2.));writer.extrude_to_xy(Vec2d(15.,10.),1.);
    const std::string clear=writer.travel_to_curved_path({Vec3d(15.,12.,2.),Vec3d(10.,12.,2.)});
    REQUIRE(clear.find("traverse above route envelope")!=std::string::npos);
    const std::regex zword(R"( Z(-?\d*\.?\d+))");std::smatch z;
    REQUIRE(std::regex_search(clear,z,zword));
    const double low=std::stod(z[1]);
    // Remote material still restricts gantry clearance at all XY positions.
    REQUIRE(low>=30.-14.+2.);REQUIRE(low<30.+2.);
    const std::string crossing=writer.travel_to_curved_path({Vec3d(192.,190.,2.)});
    REQUIRE(std::regex_search(crossing,z,zword));
    REQUIRE(std::stod(z[1])>=32.);
    writer.set_current_position_clear(false);
    const auto unknown=writer.travel_to_xyz(Vec3d(10.,10.,2.));
    REQUIRE(unknown.find("traverse above printed material")!=std::string::npos);
    REQUIRE(std::regex_search(unknown,z,zword));REQUIRE(std::stod(z[1])>=45.);
    // A known position cannot restore material omitted during unknown motion.
    writer.extrude_to_xy(Vec2d(15.,10.),1.);
    const auto still_global=writer.travel_to_xyz(Vec3d(10.,12.,2.));
    REQUIRE(still_global.find("traverse above route envelope")==std::string::npos);
    REQUIRE(std::regex_search(still_global,z,zword));REQUIRE(std::stod(z[1])>=32.);
}

TEST_CASE("Route-specific writer clearance covers bead width and off-axis deposition", "[curved-engine][travel][motion]")
{
    using namespace CurvedTransition;
    PrintConfig config;config.apply(curved_config(),true);
    GCodeWriter writer;writer.apply_print_config(config);writer.set_extruders({0});writer.set_extruder(0);
    writer.set_curved_plate_bound(45.);
    writer.set_curved_blend(std::make_shared<Identity>(),{0.,200.,0.,200.,40.,0.},Vec2d::Zero(),2.,5.);
    writer.set_curved_path(20.,4.,.2);
    writer.travel_to_xyz(Vec3d(10.,20.,20.));writer.extrude_to_xy(Vec2d(15.,20.),1.);
    writer.set_curved_path(2.,.45,.2);
    writer.travel_to_xyz(Vec3d(5.,17.,2.));writer.extrude_to_xy(Vec2d(6.,17.),1.);
    const auto route=writer.travel_to_curved_path({Vec3d(18.,17.,2.)});
    const std::regex zword(R"( Z(-?\d*\.?\d+))");std::smatch z;
    REQUIRE(std::regex_search(route,z,zword));
    // The broad bead reaches y=18: a tip-line-only test would miss it.
    const double limit=20.-.5*std::tan(15.*PI/180.)+2.;
    REQUIRE(std::stod(z[1])>=limit);
    REQUIRE(writer.get_position().z()==2.);
}

TEST_CASE("Empty curved layers preserve an already rounded staging height", "[curved-engine][travel]")
{
    GCodeWriter writer;writer.set_extruders({0});writer.set_extruder(0);
    writer.set_curved_blend(std::make_shared<CurvedTransition::Identity>(),
        {0.,20.,0.,20.,28.,0.},Vec2d::Zero(),2.,5.);
    writer.travel_to_xyz_direct(Vec3d(0.,0.,2.007),"existing staging position");
    for(int i=0;i<10;++i) writer.travel_to_z(.2+.2*i);
    REQUIRE(writer.get_position().z()==2.007);
}

TEST_CASE("Curved travel refuses heights above common or active-tip Z limits", "[curved-engine][travel]")
{
    SECTION("Common Virtual ceiling includes printed material and rounding") {
        PrintConfig config;config.nonplanar_virtual_max_z.value=10.;config.printable_height.value=100.;
        GCodeWriter writer;writer.apply_print_config(config);writer.set_extruders({0});writer.set_extruder(0);
        writer.set_curved_blend(std::make_shared<CurvedTransition::Identity>(),
            {0.,20.,0.,20.,8.,0.},Vec2d::Zero(),2.,5.);
        writer.set_curved_path(8.,.45,.2);
        writer.travel_to_xyz_direct(Vec3d(0.,0.,8.),"synthetic material start");
        writer.extrude_to_xy(Vec2d(10.,0.),1.);
        REQUIRE_THROWS(writer.travel_to_xyz(Vec3d(0.,0.,3.)));
        REQUIRE_THROWS(writer.travel_to_z(3.));
    }
    SECTION("Active tip ceiling can be lower than the common ceiling") {
        PrintConfig config;config.apply(fixed_nozzle_config(1),true);
        // Filament 0 is routed to active tip 1 by this fixed-nozzle fixture.
        GCodeWriter writer;writer.apply_print_config(config);writer.set_extruders({0});writer.set_extruder(0);
        writer.set_curved_blend(std::make_shared<CurvedTransition::Identity>(),
            {0.,20.,0.,20.,70.,0.},Vec2d::Zero(),2.,5.,1);
        REQUIRE_THROWS(writer.travel_to_curved_path({Vec3d(0.,0.,78.)}));
    }
}

TEST_CASE("Layer-mode preview follows field boundaries and protected planar regions", "[curved-engine][preview]")
{
    using namespace CurvedTransition;
    const ModelDomain domain{0.,20.,0.,20.,30.,.1};
    const PlaneProgram program(domain,.2,.2,{{10.,.02,0.}},.1);
    const auto blend=program.blends().front();
    REQUIRE(program.layer_mode(10.,10.,blend.lo)==LayerMode::Planar);
    REQUIRE(program.layer_mode(10.,10.,(blend.lo+blend.hi)*.5)==LayerMode::Transition);
    REQUIRE(program.layer_mode(10.,10.,blend.hi)==LayerMode::Curved);
    const Regions regions(domain,{{10.,10.,5.,1.,4.,.02,0.}},.1);
    REQUIRE(regions.layer_mode(10.,10.,.2)==LayerMode::Planar);
    REQUIRE(regions.layer_mode(10.,10.,2.)==LayerMode::Transition);
    REQUIRE(regions.layer_mode(10.,10.,5.)==LayerMode::Curved);
    REQUIRE(regions.layer_mode(0.,0.,5.)==LayerMode::Planar);
    REQUIRE(regions.layer_mode(15.,10.,5.)==LayerMode::Planar);
    REQUIRE(Identity{}.layer_mode(10.,10.,20.)==LayerMode::Planar);
    GCodeWriter writer;writer.set_extruders({0});writer.set_extruder(0);
    writer.set_curved_blend(std::make_shared<PlaneProgram>(program),domain,Vec2d::Zero(),2.,5.);
    std::string output;
    for(double z:{.2,10.,20.}) {
        writer.set_curved_path(z,.45,.2);
        output+=writer.travel_to_xyz(Vec3d(5.,5.,z));
        output+=writer.extrude_to_xy(Vec2d(6.,5.),.2);
    }
    REQUIRE(output.find(";LAYER_MODE:0\n")!=std::string::npos);
    REQUIRE(output.find(";LAYER_MODE:1\n")!=std::string::npos);
    REQUIRE(output.find(";LAYER_MODE:2\n")!=std::string::npos);
}

TEST_CASE("Imported Preview preserves explicit layer modes and bounded selection reports", "[curved-engine][preview]")
{
    ScopedTemporaryFile temp(".gcode");
    {
        std::ofstream out(temp.string());
        out<<"; layer_strategy: Automatic selection: curved. Fixture region\n"
              "; layer_region: v1 4 0 1 10 20 4 3 2.5 0.25 45 12 chosen roof fixture\n"
              "; layer_region: v1 5 0 0 12 22 4 3 2.5 0 50 14 rejected contact fixture\n"
              "; layer_region: v1 6 0 3 10 20 4 3 2.5 0 45 12 invalid boolean\n"
              "M83\nG90\n;TYPE:Outer wall\n;HEIGHT:0.2\n;WIDTH:0.45\n"
              "G1 X0 Y0 Z0.2 F600\n"
              ";LAYER_MODE:1\nG1 X10 Y0 Z0.3 E1\n"
              ";LAYER_MODE:2\nG1 X10 Y10 Z0.4 E1\n"
              ";LAYER_MODE:invalid\nG1 X0 Y10 Z0.4 E1\n"
              ";LAYER_MODE:999\nG1 X0 Y0 Z0.4 E1\n";
    }
    GCodeProcessor::s_IsBBLPrinter=false;
    GCodeProcessor replay;replay.process_file(temp.string());
    std::vector<unsigned> modes;
    for(const auto& move:replay.get_result().moves)
        if(move.type==EMoveType::Extrude) modes.push_back(move.layer_mode);
    REQUIRE(modes==std::vector<unsigned>{1,2,255,255});
    REQUIRE(replay.get_result().layer_strategy_decisions.size()==1);
    REQUIRE(replay.get_result().layer_strategy_decisions.front()=="Automatic selection: curved. Fixture region");
    REQUIRE(replay.get_result().layer_region_records.size()==2);
    CurvedTransition::RegionDecision region;
    REQUIRE(CurvedTransition::RegionDecision::decode(replay.get_result().layer_region_records.front(),region));
    REQUIRE(region.selected);REQUIRE(region.id==4);REQUIRE(region.x==10.);REQUIRE(region.y==20.);
    GCodeProcessorResult copied;copied=replay.get_result();
    REQUIRE(copied.layer_strategy_decisions==replay.get_result().layer_strategy_decisions);
    REQUIRE(copied.layer_region_records==replay.get_result().layer_region_records);
    REQUIRE(copied.moves.size()==replay.get_result().moves.size());
    copied.reset();REQUIRE(copied.layer_strategy_decisions.empty());
    REQUIRE(copied.layer_region_records.empty());
}
