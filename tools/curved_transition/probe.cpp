// Standalone geometry experiment: no printer commands, mesh edits or G-code.
#include "../../src/libslic3r/CurvedTransition.hpp"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace Slic3r::CurvedTransition;
namespace {
constexpr double pi = 3.14159265358979323846;
void check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void rejects(F fn, const char *message) {
    bool rejected = false;
    try { fn(); } catch (const std::invalid_argument &) { rejected = true; }
    check(rejected, message);
}
Options wedge_options(double width, double angle, double reference) {
    Options o;
    o.enabled = true;
    o.reference_top_mm = reference;
    o.top_min_mm = 3.;
    o.top_max_mm = 3. + width * std::tan(angle * pi / 180.);
    o.max_slope_deg = std::abs(angle);
    return o;
}
Field wedge(double width, double angle, double reference) {
    const double m = std::tan(angle * pi / 180.);
    return Field(wedge_options(width, angle, reference),
                 [m](double x, double) { return TopSample{3. + m * x, m, 0.}; });
}
void self_test() {
    int groups = 0;
    { // Inactive behavior must be exactly identity, even with no roof provider.
        Field f(Options{}, {});
        for (double z : {-10., 0., 0.2, 1., 5., 100.}) {
            check(f.slice_z(1., 2., z) == z, "Inactive slice mapping changed a height");
            check(f.world_z(1., 2., z) == z, "Inactive physical mapping changed a height");
        }
        ++groups;
    }
    { // Inverse consistency, no crossing, true curvature, and analytical slope.
        const double m = std::tan(4. * pi / 180.);
        const Field f = wedge(40., 4., 4.36);
        double last_error = 0.;
        for (int xi = 0; xi <= 80; ++xi) {
            const double x = xi * 0.5;
            double prior = -1.;
            for (int k = 0; k <= 120; ++k) {
                const double w = 4.36 * k / 120.;
                const double z = f.world_z(x, 0., w);
                last_error = std::max(last_error, std::abs(f.slice_z(x, 0., z) - w));
                check(z > prior, "Curved layers crossed");
                prior = z;
                const Gradient g = f.gradient(x, 0., w);
                check(g.dx >= -1e-12 && g.dx <= m + 1e-10 && g.dy == 0., "Slope overshot roof");
                if (x > 0. && x < 40. && w > 0.2 && w < 4.36) {
                    const double eps = 0.001;
                    const double numerical = (f.world_z(x + eps, 0., w) - f.world_z(x - eps, 0., w)) / (2. * eps);
                    check(std::abs(numerical - g.dx) < 1e-8, "Gradient disagrees with physical geometry");
                }
            }
            check(f.world_z(x, 0., 0.2) == 0.2, "First planar layer moved");
            check(std::abs(f.world_z(x, 0., 4.36) - (3. + m*x)) < 1e-12, "Roof geometry changed");
        }
        check(last_error < 2e-12, "Forward/inverse mapping failed");
        const double curvature = f.world_z(0.,0.,2.) - 2.*f.world_z(20.,0.,2.) + f.world_z(40.,0.,2.);
        check(std::abs(curvature) > 0.02, "Blend degenerated to a fan of flat layers");
        check(std::abs(f.gradient(20.,0.,0.20001).dx) < 1e-10, "Slope jumped at planar boundary");
        check(std::abs(f.gradient(20.,0.,4.35999).dx - m) < 1e-10, "Slope jumped at roof boundary");
        ++groups;
    }
    { // Independent sampled gaps must fall inside the global derivative bounds.
        const Field f = wedge(40., 4., 4.36);
        f.require_spacing(0.16, 0.08, 0.32);
        const auto b = f.spacing_bounds(0.16);
        for (int xi = 0; xi <= 400; ++xi)
            for (int k = 1; k <= 26; ++k) {
                const double x = xi * 0.1, w = 0.2 + 0.16*k;
                const double gap = f.world_z(x,0.,w) - f.world_z(x,0.,w-0.16);
                check(gap >= b.min_vertical_mm-1e-10 && gap <= b.max_vertical_mm+1e-10,
                      "Sampled spacing escaped certified global bounds");
            }
        rejects([&]{f.require_spacing(0.2,0.08,0.32);}, "Too-large layer step was accepted");
        ++groups;
    }
    { // Rotation/mirroring of the roof direction must not change feasibility.
        for (double a : {0., pi/4., pi/2., pi, 3.*pi/2.}) {
            const double m = std::tan(4.*pi/180.), dx = m*std::cos(a), dy = m*std::sin(a);
            const Field f(wedge_options(40.,4.,4.36), [=](double x,double y){return TopSample{3.+dx*x+dy*y,dx,dy};});
            for (double s : {0.,10.,20.,30.,40.}) {
                const double x=s*std::cos(a), y=s*std::sin(a);
                check(std::abs(f.world_z(x,y,2.)-wedge(40.,4.,4.36).world_z(s,0.,2.)) < 1e-12,
                      "A rotated field changed the geometry");
            }
        }
        ++groups;
    }
    { // A truly curved roof, analytic Lipschitz bound and invariant final surface.
        Options o; o.enabled=true; o.reference_top_mm=5.; o.top_min_mm=4.5; o.top_max_mm=5.5;
        const Field f(o, [](double x,double y){return TopSample{5.+0.5*std::sin(x/20.)*std::cos(y/20.),
            0.025*std::cos(x/20.)*std::cos(y/20.), -0.025*std::sin(x/20.)*std::sin(y/20.)};});
        f.require_spacing(0.2,0.08,0.32);
        for (int x=-40;x<=40;x+=2) for(int y=-40;y<=40;y+=2) {
            check(std::abs(f.world_z(x,y,5.)-f.roof(x,y).z)<1e-12,"Curved roof changed");
            double last=-1.;
            for(int k=0;k<=50;++k) {
                const double z=f.world_z(x,y,k*0.1);
                check(z>last,"Curved-roof layers crossed"); last=z;
                const auto g=f.gradient(x,y,k*0.1);
                check(std::hypot(g.dx,g.dy)<=0.025+1e-10,"Curved roof slope overshot");
            }
        }
        ++groups;
    }
    { // Invalid/thin/steep/nonfinite fields fail closed.
        Options o=wedge_options(40.,4.,4.36); o.flat_height_mm=4.;
        rejects([&]{Field f(o,[](double,double){return TopSample{4.,0.,0.};});},"Invalid band accepted");
        rejects([]{wedge(40.,4.,3.);},"Overshooting blend accepted");
        o=wedge_options(40.,4.,4.36); o.max_slope_deg=3.;
        Field steep(o,[](double,double){return TopSample{4.,0.1,0.};});
        rejects([&]{steep.world_z(0.,0.,2.);},"Excessive roof slope accepted");
        Field bad(wedge_options(40.,4.,4.36),[](double,double){return TopSample{100.,0.,0.};});
        rejects([&]{bad.world_z(0.,0.,2.);},"Out-of-bound roof accepted");
        Field f=wedge(40.,4.,4.36);
        rejects([&]{f.world_z(0.,0.,std::numeric_limits<double>::quiet_NaN());},"NaN accepted");
        rejects([&]{f.require_spacing(0.,0.08,0.32);},"Zero layer step accepted");
        o=wedge_options(40.,4.,4.36); o.reference_top_mm=std::numeric_limits<double>::infinity();
        rejects([&]{Field invalid(o,{});},"Infinite field accepted");
        ++groups;
    }
    { // The compact 5-degree coupon fits this geometry-only process window.
        const Field f=wedge(20.,5.,4.);
        f.require_spacing(0.2,0.08,0.32);
        ++groups;
    }
    std::cout << "PASS: " << groups << " geometry test groups; no G-code generated\n";
}
} // namespace

int main(int argc, char **argv) {
    try {
        if (argc==2 && std::string(argv[1])=="--self-test") {self_test();return 0;}
        if (argc!=3 || std::string(argv[1])!="--enable") {
            std::cerr << "Curved-layer geometry prototype (not integrated into the slicer).\n"
                         "Use --self-test or --enable OUTPUT_PREFIX for geometry CSV/JSON only.\n";
            return 2;
        }
        const std::string prefix=argv[2];
        std::ofstream csv(prefix+".csv");
        if(!csv)throw std::runtime_error("Cannot open geometry CSV output");
        csv << "case,layer,slice_z_mm,x_mm,world_z_mm,slope_deg,vertical_gap_mm\n" << std::setprecision(12);
        std::ofstream report(prefix+".json");
        if(!report)throw std::runtime_error("Cannot open audit JSON output");
        report << std::setprecision(12) << "{\"status\":\"geometry_prototype_only\",\"gcode_generated\":false,\"cases\":[";
        bool first=true;
        for (int c=0;c<2;++c) {
            const double width=c==0?40.:20.,angle=c==0?4.:5.,reference=c==0?4.36:4.,step=c==0?0.16:0.2;
            const std::string name=c==0?"4deg_40mm":"5deg_20mm";
            const Field f=wedge(width,angle,reference);
            f.require_spacing(step,0.08,0.32);
            const auto b=f.spacing_bounds(step);
            double min_gap=100.,max_gap=0.,max_slope=0.;
            const int layers=int(std::lround((reference-0.2)/step));
            for(int k=0;k<=layers;++k) for(int xi=0;xi<=400;++xi) {
                const double x=width*xi/400.,w=0.2+step*k,z=f.world_z(x,0.,w);
                const auto g=f.gradient(x,0.,w);
                const double slope=std::atan(std::hypot(g.dx,g.dy))*180./pi;
                const double gap=k?z-f.world_z(x,0.,w-step):0.2;
                if(k) {min_gap=std::min(min_gap,gap);max_gap=std::max(max_gap,gap);}
                max_slope=std::max(max_slope,slope);
                csv << name << ',' << k << ',' << w << ',' << x << ',' << z << ',' << slope << ',' << gap << '\n';
            }
            if(!first)report << ','; first=false;
            report << "{\"case\":\""<<name<<"\",\"reference_top_mm\":"<<reference<<",\"flat_height_mm\":0.2,"
                   << "\"flat_domain_step_mm\":"<<step<<",\"target_angle_deg\":"<<angle<<",\"layer_count\":"<<layers+1
                   << ",\"bound_min_graph_distance_mm\":"<<b.min_graph_distance_mm
                   << ",\"bound_max_vertical_gap_mm\":"<<b.max_vertical_mm
                   << ",\"sampled_min_vertical_gap_mm\":"<<min_gap<<",\"sampled_max_vertical_gap_mm\":"<<max_gap
                   << ",\"sampled_max_slope_deg\":"<<max_slope<<'}';
        }
        report << "]}\n";
        std::cout << "Wrote geometry CSV and JSON only. No model, settings or print toolpaths changed.\n";
        return 0;
    } catch(const std::exception &e) {std::cerr << "Rejected: " << e.what() << '\n';return 1;}
}
