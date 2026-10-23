#pragma once
#include "CurvedTransition.hpp"
#include "TriangleMesh.hpp"
#include "Exception.hpp"
#include <map>

namespace Slic3r::CurvedTransition {

// Conforming subdivision of the ORIGINAL physical facets before changing their
// Z coordinates. A global inverse-Hessian bound certifies interpolation error:
// |F(p)-linear(F(vertices))| <= H * diameter^2 / 2 on each refined facet.
// Shared edge midpoints prevent T junctions and preserve closed connectivity.
inline indexed_triangle_set map_mesh(indexed_triangle_set mesh,const Field& field,
                                     const std::function<void()>& cancel,double physical_tolerance=.01)
{
    const double hessian=field.inverse_hessian_bound();
    if (hessian>0.) {
        const double max_edge2=2.*physical_tolerance*field.jacobian_min()/hessian;
        if (!(max_edge2>0.)) throw SlicingError("Curved blend has invalid mesh refinement bounds");
        bool done=false;
        for (int iteration=0;iteration<24;++iteration) {
            cancel();
            std::map<std::pair<int,int>,int> midpoints;
            auto split=[&](int a,int b,double edge_limit2) {
                auto key=std::minmax(a,b);
                const std::pair<int,int> edge{key.first,key.second};
                const auto found=midpoints.find(edge);
                if (found!=midpoints.end()) return found->second;
                if ((mesh.vertices[a]-mesh.vertices[b]).cast<double>().squaredNorm()<=edge_limit2) return -1;
                if (mesh.vertices.size()>=2000000) throw SlicingError("Curved blend mesh refinement exceeds two million vertices");
                const int id=int(mesh.vertices.size());
                const Vec3f midpoint=(mesh.vertices[a]+mesh.vertices[b])*.5f;
                mesh.vertices.push_back(midpoint);
                midpoints.emplace(edge,id); return id;
            };
            // Mark every edge before emitting faces. A neighbouring identity
            // facet must use an edge split requested by a curved facet too.
            for(const auto& f:mesh.indices) {
                const auto lo=mesh.vertices[f[0]].cwiseMin(mesh.vertices[f[1]]).cwiseMin(mesh.vertices[f[2]]);
                const auto hi=mesh.vertices[f[0]].cwiseMax(mesh.vertices[f[1]]).cwiseMax(mesh.vertices[f[2]]);
                const double local=field.inverse_hessian_bound_box(lo.x(),hi.x(),lo.y(),hi.y(),lo.z(),hi.z());
                if(local<=0.) continue;
                const double limit=2.*physical_tolerance*field.jacobian_min()/local;
                split(f[0],f[1],limit);split(f[1],f[2],limit);split(f[2],f[0],limit);
            }
            auto midpoint=[&](int a,int b) {
                const auto key=std::minmax(a,b);const auto it=midpoints.find({key.first,key.second});
                return it==midpoints.end()?-1:it->second;
            };
            decltype(mesh.indices) faces; faces.reserve(mesh.indices.size());
            for (const auto& f:mesh.indices) {
                const int a=f[0],b=f[1],c=f[2];
                const int ab=midpoint(a,b),bc=midpoint(b,c),ca=midpoint(c,a);
                const int mask=(ab>=0?1:0)+(bc>=0?2:0)+(ca>=0?4:0);
                auto add=[&](int x,int y,int z){faces.emplace_back(x,y,z);};
                switch(mask) {
                case 0:add(a,b,c);break;
                case 1:add(a,ab,c);add(ab,b,c);break;
                case 2:add(b,bc,a);add(bc,c,a);break;
                case 4:add(c,ca,b);add(ca,a,b);break;
                case 3:add(b,bc,ab);add(a,ab,c);add(ab,bc,c);break;
                case 5:add(a,ab,ca);add(ab,b,c);add(ab,c,ca);break;
                case 6:add(c,ca,bc);add(a,b,ca);add(b,bc,ca);break;
                case 7:add(a,ab,ca);add(ab,b,bc);add(ca,bc,c);add(ab,bc,ca);break;
                }
                if (faces.size()>4000000) throw SlicingError("Curved blend mesh refinement exceeds four million facets");
            }
            mesh.indices=std::move(faces);
            if (midpoints.empty()) {done=true;break;}
        }
        if (!done) throw SlicingError("Curved blend mesh refinement did not converge");
    }
    for (auto& p:mesh.vertices) p.z()=float(field.slice_z(p.x(),p.y(),p.z()));
    return mesh;
}
} // namespace Slic3r::CurvedTransition
