/*
    Copyright (C) 1996-1997  Id Software, Inc.
    Copyright (C) 1997       Greg Lewis

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

    See file, 'COPYING', for details.
*/

#include <qbsp/brush.hh>
#include <qbsp/portals.hh>
#include <qbsp/csg4.hh>
#include <qbsp/map.hh>
#include <qbsp/merge.hh>
#include <qbsp/brushbsp.hh>
#include <qbsp/qbsp.hh>
#include <qbsp/writebsp.hh>

#include <map>
#include <list>

static bool ShouldOmitFace(face_t *f)
{
    if (!options.includeskip.value() && map.mtexinfos.at(f->texinfo).flags.is_skip)
        return true;
    if (map.mtexinfos.at(f->texinfo).flags.is_hint)
        return true;

    // HACK: to save a few faces, don't output the interior faces of sky brushes
    if (f->contents.is_sky(options.target_game)) {
        return true;
    }

    return false;
}

static void MergeNodeFaces (node_t *node)
{
    node->facelist = MergeFaceList(node->facelist);
}

/*
===============
SubdivideFace

If the face is >256 in either texture direction, carve a valid sized
piece off and insert the remainder in the next link
===============
*/
std::list<face_t *> SubdivideFace(face_t *f)
{
    vec_t mins, maxs;
    vec_t v;
    int axis;
    qbsp_plane_t plane;
    face_t *front, *back;
    const maptexinfo_t *tex;
    vec_t subdiv;
    vec_t extent;
    int lmshift;

    /* special (non-surface cached) faces don't need subdivision */
    tex = &map.mtexinfos.at(f->texinfo);

    if (tex->flags.is_skip || tex->flags.is_hint || !options.target_game->surf_is_subdivided(tex->flags)) {
        return {f};
    }
    // subdivision is pretty much pointless other than because of lightmap block limits
    // one lightmap block will always be added at the end, for smooth interpolation

    // engines that do support scaling will support 256*256 blocks (at whatever scale).
    lmshift = f->lmshift;
    if (lmshift > 4)
        lmshift = 4; // no bugging out with legacy lighting

    // legacy engines support 18*18 max blocks (at 1:16 scale).
    // the 18*18 limit can be relaxed in certain engines, and doing so will generally give a performance boost.
    subdiv = min(options.subdivide.value(), 255 << lmshift);

    //      subdiv += 8;

    // floating point precision from clipping means we should err on the low side
    // the bsp is possibly going to be used in both engines that support scaling and those that do not. this means we
    // always over-estimate by 16 rather than 1<<lmscale

    std::list<face_t *> surfaces{f};

    for (axis = 0; axis < 2; axis++) {
        // we'll transfer faces that are chopped down to size to this list
        std::list<face_t *> chopped;

        while (!surfaces.empty()) {
            f = surfaces.front();
            surfaces.pop_front();

            mins = VECT_MAX;
            maxs = -VECT_MAX;

            qvec3d tmp = tex->vecs.row(axis).xyz();

            for (int32_t i = 0; i < f->w.size(); i++) {
                v = qv::dot(f->w[i], tmp);
                if (v < mins)
                    mins = v;
                if (v > maxs)
                    maxs = v;
            }

            extent = ceil(maxs) - floor(mins);
            //          extent = maxs - mins;
            if (extent <= subdiv) {
                // this face is already good
                chopped.push_back(f);
                continue;
            }

            // split it
            plane.normal = tmp;
            v = qv::normalizeInPlace(plane.normal);

            // ericw -- reverted this, was causing https://github.com/ericwa/ericw-tools/issues/160
            //            if (subdiv > extent/2)      /* if we're near a boundary, just split the difference, this
            //            should balance the load slightly */
            //                plane.dist = (mins + subdiv/2) / v;
            //            else
            //                plane.dist = (mins + subdiv) / v;
            plane.dist = (mins + subdiv - 16) / v;

            std::tie(front, back) = SplitFace(f, plane);
            if (!front || !back) {
                //logging::print("didn't split\n");
                // FError("Didn't split the polygon");
            }

            if (front) {
                surfaces.push_back(front);
            }
            if (back) {
                chopped.push_front(back);
            }
        }

        // we've finished chopping on this axis, but we may need to chop on other axes
        Q_assert(surfaces.empty());

        surfaces = std::move(chopped);
    }

    return surfaces;
}

static void SubdivideNodeFaces(node_t *node)
{
    std::list<face_t *> result;

    // subdivide each face and push the results onto subdivided
    for (face_t *face : node->facelist) {
        result.splice(result.end(), SubdivideFace(face));
    }

    node->facelist = result;
}

static void FreeNode(node_t *node)
{
    FreeTreePortals_r(node);
    for (face_t *f : node->facelist) {
        delete f;
    }
    delete node;
}

void FreeNodes(node_t *node)
{
    if (node->planenum != PLANENUM_LEAF) {
        FreeNodes(node->children[0]);
        FreeNodes(node->children[1]);
    }
    FreeNode(node);
}

//===========================================================================

// This is a kludge.   Should be pEdgeFaces[2].
static std::map<int, const face_t *> pEdgeFaces0;
static std::map<int, const face_t *> pEdgeFaces1;

//============================================================================

struct hashvert_t
{
    qvec3d point;
    size_t num;
};

using vertidx_t = size_t;
using edgeidx_t = size_t;
static std::map<std::pair<vertidx_t, vertidx_t>, std::list<edgeidx_t>> hashedges;
static std::map<qvec3i, std::list<hashvert_t>> hashverts;

inline void InitHash()
{
    pEdgeFaces0.clear();
    pEdgeFaces1.clear();
    hashverts.clear();
    hashedges.clear();
}

inline void AddHashEdge(size_t v1, size_t v2, size_t i)
{
    hashedges[std::make_pair(v1, v2)].push_front(i);
}

inline qvec3i HashVec(const qvec3d &vec)
{
    return {floor(vec[0]), floor(vec[1]), floor(vec[2])};
}

inline void AddHashVert(const hashvert_t &hv)
{
    // insert each vert at floor(pos[axis]) and floor(pos[axis]) + 1 (for each axis)
    // so e.g. a vert at (0.99, 0.99, 0.99) shows up if we search at (1.01, 1.01, 1.01)
    // this is a bit wasteful, since it inserts 8 copies of each vert.

    for (int x = 0; x <= 1; x++) {
        for (int y = 0; y <= 1; y++) {
            for (int z = 0; z <= 1; z++) {
                const qvec3i h{floor(hv.point[0]) + x, floor(hv.point[1]) + y, floor(hv.point[2]) + z};
                hashverts[h].push_front(hv);
            }
        }
    }
}

/*
=============
GetVertex
=============
*/
inline size_t GetVertex(qvec3d vert)
{
    for (auto &v : vert) {
        double rounded = Q_rint(v);
        if (fabs(v - rounded) < ZERO_EPSILON)
            v = rounded;
    }

    const auto h = HashVec(vert);
    auto it = hashverts.find(h);
    if (it != hashverts.end()) {
        for (hashvert_t &hv : it->second) {
            if (fabs(hv.point[0] - vert[0]) < POINT_EPSILON && fabs(hv.point[1] - vert[1]) < POINT_EPSILON &&
                fabs(hv.point[2] - vert[2]) < POINT_EPSILON) {

                return hv.num;
            }
        }
    }

    const size_t global_vert_num = map.bsp.dvertexes.size();

    AddHashVert({vert, global_vert_num});

    /* emit a vertex */
    map.bsp.dvertexes.emplace_back(vert);

    return global_vert_num;
}

//===========================================================================

/*
==================
GetEdge

Don't allow four way edges (FIXME: What is this?)

Returns a global edge number, possibly negative to indicate a backwards edge.
==================
*/
inline size_t GetEdge(mapentity_t *entity, const qvec3d &p1, const qvec3d &p2, const face_t *face)
{
    if (!face->contents.is_valid(options.target_game, false))
        FError("Face with invalid contents");

    size_t v1 = GetVertex(p1);
    size_t v2 = GetVertex(p2);

    // search for an existing edge from v2->v1
    const std::pair<int, int> edge_hash_key = std::make_pair(v2, v1);

    auto it = hashedges.find(edge_hash_key);
    if (it != hashedges.end()) {
        for (const int i : it->second) {
            if (pEdgeFaces1[i] == NULL && pEdgeFaces0[i]->contents.native == face->contents.native) {
                pEdgeFaces1[i] = face;
                return -i;
            }
        }
    }

    /* emit an edge */
    size_t i = map.bsp.dedges.size();
    map.bsp.dedges.emplace_back(bsp2_dedge_t{static_cast<uint32_t>(v1), static_cast<uint32_t>(v2)});

    AddHashEdge(v1, v2, i);

    pEdgeFaces0[i] = face;
    return i;
}

static void FindFaceFragmentEdges(mapentity_t *entity, face_t *face, face_fragment_t *fragment)
{
    fragment->outputnumber = std::nullopt;

    if (fragment->w.size() > MAXEDGES) {
        FError("Internal error: face->numpoints > MAXEDGES");
    }

    fragment->edges.resize(fragment->w.size());

    for (size_t i = 0; i < fragment->w.size(); i++) {
        const qvec3d &p1 = fragment->w[i];
        const qvec3d &p2 = fragment->w[(i + 1) % fragment->w.size()];
        fragment->edges[i] = GetEdge(entity, p1, p2, face);
    }
}

/*
==================
FindFaceEdges
==================
*/
static void FindFaceEdges(mapentity_t *entity, face_t *face)
{
    if (ShouldOmitFace(face))
        return;

    FindFaceFragmentEdges(entity, face, face);

    for (auto &fragment : face->fragments) {
        FindFaceFragmentEdges(entity, face, &fragment);
    }
}

/*
================
MakeFaceEdges_r
================
*/
static int MakeFaceEdges_r(mapentity_t *entity, node_t *node, int progress)
{
    if (node->planenum == PLANENUM_LEAF)
        return progress;

    for (face_t *f : node->facelist) {
        FindFaceEdges(entity, f);
    }

    logging::percent(progress, splitnodes, entity);
    progress = MakeFaceEdges_r(entity, node->children[0], progress);
    progress = MakeFaceEdges_r(entity, node->children[1], progress);

    return progress;
}

/*
==============
EmitFaceFragment
==============
*/
static void EmitFaceFragment(mapentity_t *entity, face_t *face, face_fragment_t *fragment)
{
    int i;

    // emit a region
    Q_assert(!fragment->outputnumber.has_value());
    fragment->outputnumber = map.bsp.dfaces.size();

    mface_t &out = map.bsp.dfaces.emplace_back();

    // emit lmshift
    map.exported_lmshifts.push_back(face->lmshift);
    Q_assert(map.bsp.dfaces.size() == map.exported_lmshifts.size());

    out.planenum = ExportMapPlane(face->planenum);
    out.side = face->planeside;
    out.texinfo = ExportMapTexinfo(face->texinfo);
    for (i = 0; i < MAXLIGHTMAPS; i++)
        out.styles[i] = 255;
    out.lightofs = -1;

    // emit surfedges
    out.firstedge = static_cast<int32_t>(map.bsp.dsurfedges.size());
    std::copy(fragment->edges.cbegin(), fragment->edges.cbegin() + fragment->w.size(),
        std::back_inserter(map.bsp.dsurfedges));
    fragment->edges.clear();

    out.numedges = static_cast<int32_t>(map.bsp.dsurfedges.size()) - out.firstedge;
}

/*
==============
EmitFace
==============
*/
static void EmitFace(mapentity_t *entity, face_t *face)
{
    if (ShouldOmitFace(face))
        return;

    EmitFaceFragment(entity, face, face);

    for (auto &fragment : face->fragments) {
        EmitFaceFragment(entity, face, &fragment);
    }
}

/*
==============
GrowNodeRegion
==============
*/
static void GrowNodeRegion(mapentity_t *entity, node_t *node)
{
    if (node->planenum == PLANENUM_LEAF)
        return;

    node->firstface = static_cast<int>(map.bsp.dfaces.size());

    for (face_t *face : node->facelist) {
        //Q_assert(face->planenum == node->planenum);

        // emit a region
        EmitFace(entity, face);
    }

    node->numfaces = static_cast<int>(map.bsp.dfaces.size()) - node->firstface;

    GrowNodeRegion(entity, node->children[0]);
    GrowNodeRegion(entity, node->children[1]);
}

static void CountFace(mapentity_t *entity, face_t *f, size_t &facesCount, size_t &vertexesCount)
{
    if (ShouldOmitFace(f))
        return;

    if (f->lmshift != 4)
        map.needslmshifts = true;

    facesCount++;
    vertexesCount += f->w.size();
}

/*
==============
CountData_r
==============
*/
static void CountData_r(mapentity_t *entity, node_t *node, size_t &facesCount, size_t &vertexesCount)
{
    if (node->planenum == PLANENUM_LEAF)
        return;

    for (face_t *f : node->facelist) {
        CountFace(entity, f, facesCount, vertexesCount);
    }

    CountData_r(entity, node->children[0], facesCount, vertexesCount);
    CountData_r(entity, node->children[1], facesCount, vertexesCount);
}

/*
================
MakeFaceEdges
================
*/
int MakeFaceEdges(mapentity_t *entity, node_t *headnode)
{
    int firstface;

    logging::print(logging::flag::PROGRESS, "---- {} ----\n", __func__);

    Q_assert(entity->firstoutputfacenumber == -1);
    entity->firstoutputfacenumber = static_cast<int>(map.bsp.dfaces.size());

    size_t facesCount = 0, vertexesCount = 0;
    CountData_r(entity, headnode, facesCount, vertexesCount);

    // Accessory data
    InitHash();

    firstface = static_cast<int>(map.bsp.dfaces.size());
    MakeFaceEdges_r(entity, headnode, 0);
    logging::percent(splitnodes, splitnodes, entity == map.world_entity());

    pEdgeFaces0.clear();
    pEdgeFaces1.clear();

    logging::print(logging::flag::PROGRESS, "---- GrowRegions ----\n");
    GrowNodeRegion(entity, headnode);

    return firstface;
}

//===========================================================================

static int c_nodefaces;

/*
================
AddMarksurfaces_r

Adds the given face to the markfaces lists of all descendant leafs of `node`.

fixme-brushbsp: all leafs in a cluster can share the same marksurfaces, right?
================
*/
static void AddMarksurfaces_r(face_t *face, face_t *face_copy, node_t *node)
{
    if (node->planenum == PLANENUM_LEAF) {
        node->markfaces.push_back(face);
        return;
    }

    const qbsp_plane_t &splitplane = map.planes.at(node->planenum);

    auto [frontFragment, backFragment] = SplitFace(face_copy, splitplane);
    if (frontFragment) {
        AddMarksurfaces_r(face, frontFragment, node->children[0]);
    }
    if (backFragment) {
        AddMarksurfaces_r(face, backFragment, node->children[1]);
    }
}

/*
================
MakeMarkFaces

Populates the `markfaces` vectors of all leafs
================
*/
void MakeMarkFaces(mapentity_t* entity, node_t* node)
{
    if (node->planenum == PLANENUM_LEAF) {
        return;
    }

    // for the faces on this splitting node..
    for (face_t *face : node->facelist) {
        // add this face to all descendant leafs it touches
        
        // make a copy we can clip
        face_t *face_copy = CopyFace(face);

        if (face->planeside == 0) {
            AddMarksurfaces_r(face, face_copy, node->children[0]);
        } else {
            AddMarksurfaces_r(face, face_copy, node->children[1]);
        }
    }

    // process child nodes recursively
    MakeMarkFaces(entity, node->children[0]);
    MakeMarkFaces(entity, node->children[1]);
}

struct makefaces_stats_t {
    int	c_nodefaces;
    int c_merge;
    int c_subdivide;
};

/*
============
FaceFromPortal

pside is which side of portal (equivalently, which side of the node) we're in.
Typically, we're in an empty leaf and the other side of the portal is a solid wall.

see also FindPortalSide which populates p->side
============
*/
static face_t *FaceFromPortal(portal_t *p, int pside)
{
    side_t *side = p->side;
    if (!side)
        return nullptr;	// portal does not bridge different visible contents

    face_t *f = new face_t{};

    f->texinfo = side->texinfo;
    f->planenum = side->planenum;
    f->planeside = static_cast<planeside_t>(pside);
    f->portal = p;
    f->lmshift = side->lmshift;

    bool make_face = options.target_game->directional_visible_contents(p->nodes[pside]->contents, p->nodes[!pside]->contents);
    if (!make_face) {
        // content type / game rules requested to skip generating a face on this side
        logging::print("skipped face for {} -> {} portal\n",
            p->nodes[pside]->contents.to_string(options.target_game),
            p->nodes[!pside]->contents.to_string(options.target_game));
        return nullptr;
    }

    if (!p->nodes[pside]->contents.is_empty(options.target_game)) {
        bool our_contents_mirrorinside = options.target_game->contents_are_mirrored(p->nodes[pside]->contents);
        if (!our_contents_mirrorinside) {
            if (side->planeside != pside) {

                return nullptr;
            }
        }
    }


    if (pside)
    {
        f->w = p->winding->flip();
        f->contents = p->nodes[1]->contents;
    }
    else
    {
        f->w = *p->winding;
        f->contents = p->nodes[0]->contents;
    }

    UpdateFaceSphere(f);

    return f;
}

/*
===============
MakeFaces_r

If a portal will make a visible face,
mark the side that originally created it

  solid / empty : solid
  solid / water : solid
  water / empty : water
  water / water : none
===============
*/
static void MakeFaces_r(node_t *node, makefaces_stats_t& stats)
{
    // recurse down to leafs
    if (node->planenum != PLANENUM_LEAF)
    {
        MakeFaces_r(node->children[0], stats);
        MakeFaces_r(node->children[1], stats);

        // merge together all visible faces on the node
        if (!options.nomerge.value())
            MergeNodeFaces(node);
        if (options.subdivide.boolValue())
            SubdivideNodeFaces(node);

        return;
    }

    // solid leafs never have visible faces
    if (node->contents.is_any_solid(options.target_game))
        return;

    // see which portals are valid

    // (Note, this is happening per leaf, so we can potentially generate faces
    // for the same portal once from one leaf, and once from the neighbouring one)
    int s;
    for (portal_t *p = node->portals; p; p = p->next[s])
    {
        // 1 means node is on the back side of planenum
        s = (p->nodes[1] == node);

        face_t *f = FaceFromPortal(p, s);
        if (f)
        {
            stats.c_nodefaces++;
            p->face[s] = f;
            p->onnode->facelist.push_back(f);
        }
    }
}

/*
============
MakeFaces
============
*/
void MakeFaces(node_t *node)
{
    logging::print("--- {} ---\n", __func__);

    makefaces_stats_t stats{};

    MakeFaces_r(node, stats);

    logging::print(logging::flag::STAT, "{} makefaces\n", stats.c_nodefaces);
    logging::print(logging::flag::STAT, "{} merged\n", stats.c_merge);
    logging::print(logging::flag::STAT, "{} subdivided\n", stats.c_subdivide);
}