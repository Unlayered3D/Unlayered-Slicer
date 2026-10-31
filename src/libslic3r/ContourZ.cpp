#include "Exception.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Layer.hpp"
#include "Line.hpp"
#include "Point.hpp"
#include "Print.hpp"
#include "SLA/IndexedMesh.hpp"
#include "libslic3r.h"
#include <algorithm>
#include <Eigen/Core>
#include <cfloat>
#include <cmath>
#include <initializer_list>
#include <math.h>
#include <string>
#include <utility>
#include "Config.hpp"
#include "Polyline.hpp"
#include "PrintConfig.hpp"

namespace Slic3r {

// What a layer is contoured against: the mesh, bounded further by a cut-plane segment's clips.
struct ContourZSurface
{
    const sla::IndexedMesh                    &mesh;
    const std::vector<Layer::ContourZCeiling> &ceilings;

    struct Hit
    {
        bool   is_hit;
        double distance;
        Vec3d  normal;
    };

    // Where an upward ray from inside the material first leaves  mesh ∩ every ceiling.
    Hit query_up(const Vec3d &origin) const
    {
        const sla::IndexedMesh::hit_result mesh_hit = mesh.query_ray_hit(origin, Vec3d::UnitZ());
        Hit hit{mesh_hit.is_hit(), mesh_hit.distance(), mesh_hit.is_hit() ? mesh_hit.normal() : Vec3d::UnitZ()};
        for (const Layer::ContourZCeiling &ceiling : ceilings) {
            // A downward-facing plane (a segment's floor) is never left through going up.
            if (ceiling.normal.z() < EPSILON)
                continue;
            const double t = std::max(0., (ceiling.offset - ceiling.normal.dot(origin)) / ceiling.normal.z());
            if (!hit.is_hit || t < hit.distance)
                hit = {true, t, ceiling.normal};
        }
        return hit;
    }
};

static void contour_extrusion_entity(LayerRegion *region, const ContourZSurface &surface, ExtrusionEntity *extr);

static double follow_slope_down(double angle_rad, double dist)
{
	return -dist * std::sin(angle_rad);
}

static double slope_from_normal(const Eigen::Vector3d& normal)
{
    // Ensure the normal is normalized
    Eigen::Vector3d n = normal.normalized();

    // Compute angle between normal and z-axis
    double angle_rad = std::acos(std::abs(n.z()));  // angle between normal and vertical
	return angle_rad;
}

static bool contour_extrusion_path(LayerRegion *region, const ContourZSurface &surface, ExtrusionPath &path)
{

	    //BOOST_LOG_TRIVIAL(info) << "contour_extrusion_path: role=" << path.role();

    // UNLAYERED_MOD: a nonplanar layers shell path already follows the surface.
    // A path contoured before (nonplanar layers: the planar layer under a curved shell) keeps its Z.
    if (path.nonplanar_shell || path.z_contoured)
        return false;

    if (path.role() != erTopSolidInfill && path.role() != erIroning && path.role() != erExternalPerimeter && path.role() != erPerimeter) {
            //BOOST_LOG_TRIVIAL(info) << "contour_extrusion_path: skipping role=" << path.role();

		return false;
	}
	
	Layer *layer = region->layer();
	coordf_t mesh_slice_z = layer->slice_z + surface.mesh.ground_level();
    BOOST_LOG_TRIVIAL(info) << "contour_extrusion_path: mesh_slice_z=" << mesh_slice_z << " ground_level=" << surface.mesh.ground_level();

	coordf_t min_z = region->region().config().zaa_min_z;

	const Points3 &points = path.polyline.points;
	double resolution_mm = 0.1;

	coordf_t height = layer->height;

	double minimize_perimeter_height_angle = region->region().config().zaa_minimize_perimeter_height;

	Pointf3s contoured_points;
	bool was_contoured = false;

    if (points.size() < 2) {
        // Safety check. The loop below does not handle paths with less than two points correctly.
        return false;
    }

    for (Points3::const_iterator it = points.begin(); it != points.end()-1; ++it) {
		Vec2d p1d(unscale_(it->x()), unscale_(it->y()));
		Vec2d p2d(unscale_((it+1)->x()), unscale_((it+1)->y()));
		Linef line(p1d, p2d);

		double length_mm = line.length();
		int num_segments = int(std::ceil(length_mm / resolution_mm));
		Vec2d delta = line.vector();

        if (num_segments == 0) {
            continue;
        }

        for (int i = 0; i < num_segments + 1; i++) {
            Vec2d p = p1d + delta * i / num_segments;

            coordf_t x = p.x();
			coordf_t y = p.y();

			ContourZSurface::Hit hit_up = surface.query_up({x, y, mesh_slice_z});
            //BOOST_LOG_TRIVIAL(info) << "contour_extrusion_path: hit=" << hit_up.is_hit << " distance=" << hit_up.distance;


			double d = hit_up.distance - (layer->print_z - layer->slice_z);

			double max_up = min_z;
			double min_down = -(height - min_z);
			double half_width = path.width / 2.0;
			if (path.role() == erIroning) {
				max_up = height;
				min_down = -(height + 0.1);
			}

            if (is_perimeter(path.role()) && hit_up.is_hit) {
				const Vec3d &normal = hit_up.normal;
                double slope_rad     = slope_from_normal(normal);
                double slope_degrees = slope_rad * 180.0 / M_PI;

                if (d > min_down && minimize_perimeter_height_angle > 0 && minimize_perimeter_height_angle < slope_degrees) {
                    double adjustment = follow_slope_down(slope_rad, half_width);
                    if (adjustment > 0) {
                        throw RuntimeError("ContourZ: got positive adjustment");
                    }
                    d += adjustment;
                    if (d < min_down) {
                        d = min_down;
                    }
                }
            }

            if (d < -height || d > max_up + 0.03) {
                // this point is too far from the mesh edge, probably because this is not a top surface. Do not contour it.
                d = 0;
            }

            if (d < min_down) {
                d = min_down;
            } else if (d > max_up) {
                d = max_up;
            }

            if (is_perimeter(path.role()) && d > 0) {
                // do not increase height of perimeters as this may create an appearance of a seam
                d = 0;
            }

            if (std::abs(d) > EPSILON) {
				was_contoured = true;
			}

            Vec3d new_point = {p.x(), p.y(), d};

            if (contoured_points.size() >= 2 && i != 0) {
                // Normally, if the new point is collinear with the last two points, we do not add
                // it to the list of contoured points. Instead we update the last point to be the
                // new point. This is to avoid creating a large number of very short segments.
                //
                // However, if the new point corresponds to a point in the original path (i == 0),
                // even if it is collinear, we add it anyway. This is to avoid creating a degenerate
                // polygon with only two points, which may cause issues in downstream code.
                double dist = Linef3::distance_to_infinite_squared(new_point, contoured_points[contoured_points.size() - 2],
                                                                   contoured_points[contoured_points.size() - 1]);
                if (dist < EPSILON * EPSILON) {
                    contoured_points[contoured_points.size() - 1] = new_point;
                    continue;
                }
            }

            contoured_points.push_back(new_point);
        }
    }

    if (!was_contoured) {
		return false;
	}

	Polyline3 polyline;
	for (const Vec3d &point : contoured_points) {
		polyline.append(Point3(scale_(point.x()), scale_(point.y()), scale_(point.z())));
	}

	path.polyline = std::move(polyline);
	path.z_contoured = true;
	return true;
}

static void contour_extrusion_multipath(LayerRegion *region, const ContourZSurface &surface, ExtrusionMultiPath &multipath) 
{
	for (ExtrusionPath &path : multipath.paths) {
		contour_extrusion_path(region, surface, path);
	}
}

static void contour_extrusion_loop(LayerRegion *region, const ContourZSurface &surface, ExtrusionLoop &loop) 
{
	for (ExtrusionPath &path : loop.paths) {
		contour_extrusion_path(region, surface, path);
	}
}

static void contour_extrusion_entitiy_collection(LayerRegion *region, const ContourZSurface &surface, ExtrusionEntityCollection &collection)
{
	for (ExtrusionEntity *entity : collection.entities) {
		contour_extrusion_entity(region, surface, entity);
	}
}

static void contour_extrusion_entity(LayerRegion *region, const ContourZSurface &surface, ExtrusionEntity *extr)
{
	const ExtrusionPathSloped *sloped = dynamic_cast<const ExtrusionPathSloped*>(extr);
	if (sloped != nullptr) {
		throw RuntimeError("ExtrusionPathSloped not implemented");
		return;
	}

	ExtrusionMultiPath *multipath = dynamic_cast<ExtrusionMultiPath*>(extr);
	if (multipath != nullptr) {
		contour_extrusion_multipath(region, surface, *multipath);
		return;
	}

	ExtrusionPath *path = dynamic_cast<ExtrusionPath*>(extr);
	if (path != nullptr) {
		contour_extrusion_path(region, surface, *path);
		return;
	}

	ExtrusionLoop *loop = dynamic_cast<ExtrusionLoop*>(extr);
	if (loop != nullptr) {
		contour_extrusion_loop(region, surface, *loop);
		return;
	}

	const ExtrusionLoopSloped *loop_sloped = dynamic_cast<const ExtrusionLoopSloped*>(extr);
	if (loop_sloped != nullptr) {
		throw RuntimeError("ExtrusionLoopSloped not implemented");
		return;
	}

	ExtrusionEntityCollection *collection = dynamic_cast<ExtrusionEntityCollection*>(extr);
	if (collection != nullptr) {
		contour_extrusion_entitiy_collection(region, surface, *collection);
		return;
	}

	throw RuntimeError("ContourZ: ExtrusionEntity type not implemented: " + std::string(typeid(*extr).name()));
	return;
}

static void handle_extrusion_collection(LayerRegion *region, const ContourZSurface &surface, ExtrusionEntityCollection &collection, std::initializer_list<ExtrusionRole> roles) {
    for (ExtrusionEntity* extr : collection.entities) {
        if (!contains(roles, extr->role())) {
			continue;
		}

		contour_extrusion_entity(region, surface, extr);
    }
}

void Layer::make_contour_z(const sla::IndexedMesh &mesh, const std::vector<ContourZCeiling> &ceilings)
{
    const ContourZSurface surface{mesh, ceilings};
	for (LayerRegion *region : this->regions()) {
        if (!region->region().config().zaa_enabled)
            continue;

        handle_extrusion_collection(region, surface, region->fills, {erTopSolidInfill, erIroning, erPerimeter, erExternalPerimeter, erMixed});
        handle_extrusion_collection(region, surface, region->perimeters, {erPerimeter, erExternalPerimeter, erMixed});
    }
}
} // namespace Slic3r
