#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <utility>

namespace Slic3r::CurvedTransition {

// Coordinate field used by native mesh slicing and physical path emission.
// Geometry bounds are separate from machine calibration and material validation.
struct ModelDomain {
    double xmin, xmax, ymin, ymax, top_max, slope_max;
    double slope() const { return slope_max; }
};
struct Options {
    bool enabled = false;
    double flat_height_mm = 0.2;
    double reference_top_mm = 4.;
    double top_min_mm = 3.;
    double top_max_mm = 5.;
    double max_slope_deg = 5.;
};

struct TopSample {
    double z, dx, dy; // physical roof z and its derivatives in placed XY coordinates
};

struct Gradient { double dx, dy; };

// Preview classification only; it is never a collision or print-quality claim.
enum class LayerMode : unsigned char { Planar=0, Transition=1, Curved=2 };

struct SpacingBounds {
    double min_vertical_mm;
    double max_vertical_mm;
    // Lower bound on separation between Lipschitz graphs, away from the domain
    // boundary. Not an extrusion bead model or proof of nozzle clearance.
    double min_graph_distance_mm;
};

class Field {
public:
    using TopFunction = std::function<TopSample(double, double)>;

    Field(Options options, TopFunction top) : m_options(options), m_top(std::move(top))
    {
        if (!m_options.enabled)
            return;
        if (!m_top || !finite(m_options.flat_height_mm) || !finite(m_options.reference_top_mm) ||
            !finite(m_options.top_min_mm) || !finite(m_options.top_max_mm) ||
            !finite(m_options.max_slope_deg) || m_options.flat_height_mm < 0. ||
            m_options.top_min_mm <= m_options.flat_height_mm ||
            m_options.reference_top_mm <= m_options.flat_height_mm ||
            m_options.reference_top_mm < m_options.top_min_mm ||
            m_options.reference_top_mm > m_options.top_max_mm ||
            m_options.top_max_mm < m_options.top_min_mm ||
            m_options.max_slope_deg < 0. || m_options.max_slope_deg >= 90.)
            throw std::invalid_argument("Invalid curved-transition bounds or target roof");

        // q = (T - R)/(T - z0). q <= 1/3 keeps the local layer slope
        // between zero and the roof slope for the quintic blend below.
        const double q_max = (m_options.top_max_mm - m_options.reference_top_mm) /
                             (m_options.top_max_mm - m_options.flat_height_mm);
        const double q_min = (m_options.top_min_mm - m_options.reference_top_mm) /
                             (m_options.top_min_mm - m_options.flat_height_mm);
        if (q_max > 1. / 3. + 1e-12)
            throw std::invalid_argument("Not enough height for a blend without slope overshoot");
        // max S'(t) = 15/8. These are global bounds for d(slice_z)/d(world_z).
        m_jacobian_min = 1. - std::max(0., q_max) * 15. / 8.;
        m_jacobian_max = 1. - std::min(0., q_min) * 15. / 8.;
        m_slope_max = std::tan(m_options.max_slope_deg * pi / 180.);
    }

    virtual ~Field() = default;
    virtual bool enabled() const { return m_options.enabled; }
    virtual double reference_top_mm() const { return m_options.reference_top_mm; }
    virtual double slope_bound() const { return m_slope_max; }
    virtual LayerMode layer_mode(double x,double y,double physical_z) const {
        if(!enabled() || physical_z<=m_options.flat_height_mm) return LayerMode::Planar;
        return physical_z<roof(x,y).z?LayerMode::Transition:LayerMode::Curved;
    }
    // Physical/logical height difference bounds used across independent objects.
    virtual double max_displacement_mm() const {
        return std::max(std::abs(m_options.top_min_mm-m_options.reference_top_mm),
                        std::abs(m_options.top_max_mm-m_options.reference_top_mm));
    }
    // True only when the ENTIRE box is identity mapped, not a sample of points.
    virtual bool identity_on_box(double,double,double,double,double,double zmax) const {
        return zmax<=m_options.flat_height_mm;
    }
    // Mapped supports are opt-in to a field whose support/bridge paths are
    // validated after native generation. Other fields retain planar contacts.
    virtual bool maps_supports() const { return false; }
    virtual bool affine_on_box(double x0,double x1,double y0,double y1,double z0,double z1) const {
        return identity_on_box(x0,x1,y0,y1,z0,z1);
    }
    virtual double inverse_hessian_bound_box(double,double,double,double,double,double) const {
        return inverse_hessian_bound();
    }
    // Hessian norm of the inverse map over the physical model. Zero is reserved
    // for a boundary known to map exactly (the certified ramp).
    virtual double inverse_hessian_bound() const { return 0.; }

    // Contract: the callback's height bounds and Lipschitz slope bound must be
    // established over the WHOLE model domain by its caller. Checking individual
    // samples here catches mistakes but cannot certify an arbitrary callback.
    TopSample roof(double x, double y) const
    {
        if (!m_options.enabled)
            throw std::logic_error("An inactive field has no roof");
        if (!finite(x) || !finite(y))
            throw std::invalid_argument("Non-finite XY coordinate");
        const TopSample r = m_top(x, y);
        if (!finite(r.z) || !finite(r.dx) || !finite(r.dy) ||
            r.z < m_options.top_min_mm - 1e-9 || r.z > m_options.top_max_mm + 1e-9 ||
            std::hypot(r.dx, r.dy) > m_slope_max + 1e-10)
            throw std::invalid_argument("Roof sample exceeds the declared height or slope bound");
        return r;
    }

    // Coordinates of the physical solid in a flat slicing domain. XY is unchanged.
    virtual double slice_z(double x, double y, double z) const
    {
        require_finite(z);
        if (!m_options.enabled || z <= m_options.flat_height_mm)
            return z;
        const TopSample r = roof(x, y);
        const double t = (z - m_options.flat_height_mm) / (r.z - m_options.flat_height_mm);
        return z - (r.z - m_options.reference_top_mm) * smooth(t);
    }

    // Inverse: constant slice_z becomes a genuinely curved physical layer even
    // for a linear wedge roof, since its transition depth depends on XY.
    virtual double world_z(double x, double y, double w) const
    {
        require_finite(w);
        if (!m_options.enabled || w <= m_options.flat_height_mm)
            return w;
        const TopSample r = roof(x, y);
        if (w >= m_options.reference_top_mm)
            return w + r.z - m_options.reference_top_mm;
        double lo = m_options.flat_height_mm, hi = r.z;
        // A monotone bracket, not Newton iteration: cannot jump across layers.
        for (int i = 0; i < 60; ++i) {
            const double mid = lo + (hi - lo) * 0.5;
            const double t = (mid - m_options.flat_height_mm) / (r.z - m_options.flat_height_mm);
            const double mapped = mid - (r.z - m_options.reference_top_mm) * smooth(t);
            if (mapped < w) lo = mid; else hi = mid;
        }
        return lo + (hi - lo) * 0.5;
    }

    virtual Gradient gradient(double x, double y, double w) const
    {
        require_finite(w);
        if (!m_options.enabled || w <= m_options.flat_height_mm)
            return {0., 0.};
        const TopSample r = roof(x, y);
        if (w >= m_options.reference_top_mm)
            return {r.dx, r.dy};
        const double z = world_z(x, y, w);
        const double t = (z - m_options.flat_height_mm) / (r.z - m_options.flat_height_mm);
        const double q = (r.z - m_options.reference_top_mm) / (r.z - m_options.flat_height_mm);
        const double factor = (smooth(t) - q * t * smooth_derivative(t)) /
                              (1. - q * smooth_derivative(t));
        return {r.dx * factor, r.dy * factor};
    }

    virtual double jacobian_min() const { return m_jacobian_min; }
    virtual double jacobian_max() const { return m_jacobian_max; }

    SpacingBounds spacing_bounds(double step) const
    {
        if (!finite(step) || step <= 0.)
            throw std::invalid_argument("Layer step must be finite and positive");
        const double min_vertical = step / jacobian_max();
        return {min_vertical, step / jacobian_min(),
                min_vertical / std::sqrt(1. + slope_bound() * slope_bound())};
    }

    // Geometry/process feasibility ONLY. Limits are supplied by a calibrated
    // process, not inferred from nozzle diameter, defaults or a CAD assembly.
    void require_spacing(double step, double min_distance, double max_vertical) const
    {
        if (!finite(min_distance) || !finite(max_vertical) || min_distance <= 0. ||
            max_vertical < min_distance)
            throw std::invalid_argument("Invalid layer-spacing limits");
        const SpacingBounds b = spacing_bounds(step);
        if (b.min_graph_distance_mm < min_distance - 1e-10 ||
            b.max_vertical_mm > max_vertical + 1e-10)
            throw std::invalid_argument("Blend cannot certify the requested layer-spacing limits");
    }

private:
    static constexpr double pi = 3.14159265358979323846;
    static bool finite(double v) { return std::isfinite(v); }
    static void require_finite(double v) {
        if (!finite(v)) throw std::invalid_argument("Non-finite height coordinate");
    }
    static double smooth(double t) {
        if (t <= 0.) return 0.;
        if (t >= 1.) return 1.;
        return t * t * t * (10. + t * (-15. + 6. * t));
    }
    static double smooth_derivative(double t) {
        if (t <= 0. || t >= 1.) return 0.;
        return 30. * t * t * (1. - t) * (1. - t);
    }

    Options m_options;
    TopFunction m_top;
    double m_jacobian_min = 1., m_jacobian_max = 1., m_slope_max = 0.;
};

// Used while a planar object, skirt or brim shares a plate with curved objects.
// The writer still uses the plate-wide raised travel path, but no deformation.
class Identity final : public Field {
public:
    Identity():Field(Options{}, {}) {}
    bool enabled() const override {return true;}
    double slope_bound() const override {return 0.;}
    double max_displacement_mm() const override {return 0.;}
    double jacobian_min() const override {return 1.;}
    double jacobian_max() const override {return 1.;}
    double world_z(double,double,double w) const override {return w;}
    double slice_z(double,double,double z) const override {return z;}
    LayerMode layer_mode(double,double,double) const override {return LayerMode::Planar;}
    Gradient gradient(double,double,double) const override {return {0.,0.};}
    bool identity_on_box(double,double,double,double,double,double) const override {return true;}
};

} // namespace Slic3r::CurvedTransition
