// quad.cpp
#include "quadtree.h"
#include "gsegment.h"
#include <QtEndian>
#include <algorithm>
#include <limits>
#include <qmutex.h>
#include <qtconcurrentmap.h>
#include <queue>
#include <stack>
#include <thread>
#include <unordered_set>

namespace ShipNetSimCore
{

unsigned int Quadtree::MIN_SEGMENTS_FOR_PARALLEL =
    500 * std::max(1u, std::thread::hardware_concurrency());

Quadtree::Node::Node(Quadtree *tree, Node *parent, int quadrant)
    : host(tree)
    , quadrant(quadrant)
    , isLeaf(true)
    , parent(parent)
    , minLon(-180.0)
    , minLat(-90.0)
    , maxLon(180.0)
    , maxLat(90.0)
{
    std::fill(std::begin(children), std::end(children), nullptr);
}

Quadtree::Node::~Node()
{
    // Delete child nodes, which are the resources owned by this node
    for (Node *&child : children)
    {
        delete child;    // Delete the child node
        child = nullptr; // Set the pointer to nullptr after deletion
    }
    parent = nullptr;
    host   = nullptr;
}

bool Quadtree::Node::isPointWithinNode(
    const std::shared_ptr<GPoint> &point) const
{
    double pLon = point->getLongitude().value();
    double pLat = point->getLatitude().value();
    return pLon >= minLon && pLon <= maxLon
        && pLat >= minLat && pLat <= maxLat;
}

units::length::meter_t Quadtree::Node::distanceFromPointToBoundingBox(
    const std::shared_ptr<GPoint> &point) const
{
    double pLon = point->getLongitude().value();
    double pLat = point->getLatitude().value();
    double cLon = std::clamp(pLon, minLon, maxLon);
    double cLat = std::clamp(pLat, minLat, maxLat);
    if (cLon == pLon && cLat == pLat)
        return units::length::meter_t(0.0); // point inside box
    return units::length::meter_t(
        GSegment::haversineRaw(pLon, pLat, cLon, cLat));
}

void Quadtree::Node::subdivide(Quadtree *tree)
{
    if (!isLeaf)
        return;

    if (line_segments.size() == 0)
    {
        isLeaf = true;
        return;
    }

    createChildren(tree); // create 4 children

    QVector<std::shared_ptr<GLine>> segmentsToKeep;

    for (auto &segment : line_segments)
    {
        // Check if the segment crosses the antimeridian and should be
        // split
        if (isSegmentCrossingAntimeridian(segment))
        {
            auto splitSegments = splitSegmentAtAntimeridian(segment);
            for (auto &splitSegment : splitSegments)
            {
                auto r = distributeSegmentToChildren(splitSegment);
                if (r != nullptr)
                {
                    segmentsToKeep.push_back(r);
                }
            }
        }
        else
        {
            auto r = distributeSegmentToChildren(segment);
            if (r != nullptr)
            {
                segmentsToKeep.push_back(r);
            }
        }
    }

    // Clear the parent node's segments as they are now reassigned
    line_segments.clear();
    segments.clear();
    line_segments = segmentsToKeep;
    // Rebuild segments vector to match
    segments.reserve(segmentsToKeep.size());
    for (const auto &seg : segmentsToKeep)
    {
        segments.emplace_back(GSegment(seg));
    }
    isLeaf = false;

    // Recursively subdivide children if they exceed
    // the maximum number of segments.
    for (int i = 0; i < 4; ++i)
    {
        if (children[i]->line_segments.size()
            > host->MAX_SEGMENTS_PER_NODE)
        {
            children[i]->subdivide(tree);
        }
    }
}

std::shared_ptr<GLine> Quadtree::Node::distributeSegmentToChildren(
    const std::shared_ptr<GLine> &segment)
{
    bool distributed = false;
    for (int i = 0; i < 4; ++i)
    {
        if (children[i]->doesLineSegmentIntersectNode(segment))
        {
            children[i]->line_segments.push_back(segment);
            children[i]->segments.emplace_back(GSegment(segment));
            distributed = true;
        }
    }
    if (!distributed)
    {
        return segment;
    }
    return nullptr;
}

void Quadtree::Node::createChildren(Quadtree *tree)
{
    double centerLon = (minLon + maxLon) / 2.0;
    double centerLat = (minLat + maxLat) / 2.0;

    for (int i = 0; i < 4; ++i)
    {
        children[i]         = new Node(tree, this, i);
        children[i]->isLeaf = true;

        children[i]->minLon = (i % 2 == 0) ? minLon : centerLon;
        children[i]->maxLon = (i % 2 == 0) ? centerLon : maxLon;
        children[i]->minLat = (i < 2) ? centerLat : minLat;
        children[i]->maxLat = (i < 2) ? maxLat : centerLat;
    }
}

Quadtree::Quadtree(const QVector<std::shared_ptr<Polygon>> &polygons)
    : root(this)
{

    auto initializeBoundary =
        [this](const std::shared_ptr<GPoint> point) -> void {
        double lon = point->getLongitude().value();
        double lat = point->getLatitude().value();
        root.minLon = std::min(root.minLon, lon);
        root.minLat = std::min(root.minLat, lat);
        root.maxLon = std::max(root.maxLon, lon);
        root.maxLat = std::max(root.maxLat, lat);
    };

    // Initialize the root node's boundary based on polygons
    for (const std::shared_ptr<Polygon> &polygon : polygons)
    {
        for (const std::shared_ptr<GPoint> &point : polygon->outer())
        {
            initializeBoundary(point);
        }

        // Include inner holes in boundary initialization
        for (const QVector<std::shared_ptr<GPoint>> &hole :
             polygon->inners())
        {
            for (const std::shared_ptr<GPoint> &point : hole)
            {
                initializeBoundary(point);
            }
        }
    }

    // Process each line segment of the polygon
    auto processLineSegment =
        [this](const std::shared_ptr<GPoint> &start,
               const std::shared_ptr<GPoint> &end) {
            auto segment = std::make_shared<GLine>(start, end);
            root.line_segments.push_back(segment);
            root.segments.emplace_back(GSegment(segment));
        };

    // Process each polygon
    for (const std::shared_ptr<Polygon> &polygon : polygons)
    {
        // Insert segments from outer boundary
        const auto &outer = polygon->outer();
        for (qsizetype i = 0; i < outer.size() - 1; ++i)
        {
            processLineSegment(outer[i], outer[i + 1]);
        }

        // Insert segments from inner holes
        for (const auto &hole : polygon->inners())
        {
            for (qsizetype i = 0; i < hole.size() - 1; ++i)
            {
                processLineSegment(hole[i], hole[i + 1]);
            }
        }
    }

    // Subdivide the root node if necessary
    if (root.line_segments.size() > MAX_SEGMENTS_PER_NODE)
    {
        root.subdivide(this);
    }
}

QVector<Quadtree::Node *> Quadtree::findNodesIntersectingLineSegment(
    const std::shared_ptr<GLine> &segment) const
{
    QVector<Quadtree::Node *> intersecting_nodes;
    if (isSegmentCrossingAntimeridian(segment))
    {
        auto splitSegments = splitSegmentAtAntimeridian(segment);
        for (auto &splitSegment : splitSegments)
        {
            findIntersectingNodesHelper(splitSegment, root,
                                        intersecting_nodes);
        }
    }
    else
    {
        findIntersectingNodesHelper(segment, root,
                                    intersecting_nodes);
    }
    return intersecting_nodes;
}

// Add parallel processing for large queries
QVector<Quadtree::Node *>
Quadtree::findNodesIntersectingLineSegmentParallel(
    const std::shared_ptr<GLine> &segment) const
{
    QVector<Node *> intersecting_nodes;

    if (root.line_segments.size() < MIN_SEGMENTS_FOR_PARALLEL)
    {
        return findNodesIntersectingLineSegment(segment);
    }

    // Get initial level nodes for parallel processing
    QVector<Node *> initialNodes;
    if (!root.isLeaf)
    {
        for (auto *child : root.children)
        {
            if (child && child->doesLineSegmentIntersectNode(segment))
            {
                initialNodes.push_back(child);
            }
        }
    }

    // Process nodes in parallel using QtConcurrent
    QMutex mutex;
    auto   processNode = [this, &segment, &mutex,
                        &intersecting_nodes](Node *node) {
        QVector<Node *> threadNodes;
        findIntersectingNodesHelper(segment, *node, threadNodes);

        QMutexLocker locker(&mutex);
        intersecting_nodes.append(threadNodes);
    };

    // Use mapped to process all nodes in parallel
    QFuture<void> future =
        QtConcurrent::map(initialNodes, processNode);

    // Wait for all parallel operations to complete
    future.waitForFinished();

    return intersecting_nodes;
}

void Quadtree::findIntersectingNodesHelper(
    const std::shared_ptr<GLine> &segment, const Node &node,
    QVector<Quadtree::Node *> &intersecting_nodes) const
{
    if (!node.doesLineSegmentIntersectNode(segment))
    {
        return; // No intersection with this node
    }

    if (node.isLeaf)
    {
        // Cast to non-const to match return type
        intersecting_nodes.push_back(const_cast<Node *>(&node));
    }
    else
    {
        for (const Node *child : node.children)
        {
            if (child)
            {
                findIntersectingNodesHelper(segment, *child,
                                            intersecting_nodes);
            }
        }
    }
}

// GSegment overload — iterative, zero allocations
QVector<Quadtree::Node *> Quadtree::findNodesIntersectingSegment(
    const GSegment &seg) const
{
    QVector<Node *> intersecting_nodes;

    if (isSegmentCrossingAntimeridian(seg))
    {
        auto splits = splitSegmentAtAntimeridian(seg);
        for (const auto &s : splits)
        {
            findIntersectingSegmentHelper(s, root,
                                          intersecting_nodes);
        }
    }
    else
    {
        findIntersectingSegmentHelper(seg, root, intersecting_nodes);
    }
    return intersecting_nodes;
}

void Quadtree::findIntersectingSegmentHelper(
    const GSegment &seg, const Node &node,
    QVector<Node *> &intersecting_nodes) const
{
    if (!node.doesSegmentIntersectNode(seg))
        return;

    if (node.isLeaf)
    {
        intersecting_nodes.push_back(const_cast<Node *>(&node));
    }
    else
    {
        for (const Node *child : node.children)
        {
            if (child)
            {
                findIntersectingSegmentHelper(seg, *child,
                                              intersecting_nodes);
            }
        }
    }
}

bool Quadtree::Node::doesLineSegmentIntersectNode(
    const std::shared_ptr<GLine> &segment) const
{
    if (standardIntersectionCheck(segment))
    {
        return true;
    }

    if (isSegmentCrossingAntimeridian(segment))
    {
        auto adjustedSegments = splitSegmentAtAntimeridian(segment);
        for (const auto &adjSegment : adjustedSegments)
        {
            if (standardIntersectionCheck(adjSegment))
            {
                return true;
            }
        }
    }

    return false;
}

// GSegment overload — pure double arithmetic
bool Quadtree::Node::doesSegmentIntersectNode(
    const GSegment &seg) const
{
    double sx = seg.startLon(), sy = seg.startLat();
    double ex = seg.endLon(),   ey = seg.endLat();

    // Check if either endpoint is inside the node's bounding box
    if (sx >= minLon && sx <= maxLon && sy >= minLat && sy <= maxLat)
        return true;
    if (ex >= minLon && ex <= maxLon && ey >= minLat && ey <= maxLat)
        return true;

    // Segment bounding box vs node bounding box — quick reject
    double segMinLon = std::min(sx, ex);
    double segMaxLon = std::max(sx, ex);
    double segMinLat = std::min(sy, ey);
    double segMaxLat = std::max(sy, ey);

    if (segMaxLon < minLon || segMinLon > maxLon ||
        segMaxLat < minLat || segMinLat > maxLat)
        return false;

    // Liang-Barsky parametric clipping
    double dx = ex - sx;
    double dy = ey - sy;

    auto clipTest = [](double p, double q,
                       double &tMin, double &tMax) -> bool {
        if (std::abs(p) < 1e-15)
            return q >= 0.0;
        double t = q / p;
        if (p < 0.0)
        {
            if (t > tMax) return false;
            if (t > tMin) tMin = t;
        }
        else
        {
            if (t < tMin) return false;
            if (t < tMax) tMax = t;
        }
        return true;
    };

    double tMin = 0.0, tMax = 1.0;

    if (!clipTest(-dx, sx - minLon, tMin, tMax)) return false;
    if (!clipTest( dx, maxLon - sx, tMin, tMax)) return false;
    if (!clipTest(-dy, sy - minLat, tMin, tMax)) return false;
    if (!clipTest( dy, maxLat - sy, tMin, tMax)) return false;

    // If Liang-Barsky passes, check antimeridian fallback for
    // stored polygon edges that may cross the antimeridian
    if (tMin <= tMax)
        return true;

    // Antimeridian fallback
    if (Quadtree::isSegmentCrossingAntimeridian(seg))
    {
        auto splits = Quadtree::splitSegmentAtAntimeridian(seg);
        for (const auto &s : splits)
        {
            // Recursive but on a non-crossing segment
            if (doesSegmentIntersectNode(s))
                return true;
        }
    }

    return false;
}

std::vector<std::shared_ptr<GLine>>
Quadtree::splitSegmentAtAntimeridian(
    const std::shared_ptr<GLine> &segment)
{
    std::vector<std::shared_ptr<GLine>> adjustedSegments;

    // Null safety check
    if (!segment || !segment->startPoint() || !segment->endPoint())
    {
        qWarning() << "splitSegmentAtAntimeridian: Invalid segment";
        return adjustedSegments;  // Return empty vector
    }

    // Extract coordinates
    double startLon = segment->startPoint()->getLongitude().value();
    double endLon   = segment->endPoint()->getLongitude().value();
    double startLat = segment->startPoint()->getLatitude().value();
    double endLat   = segment->endPoint()->getLatitude().value();

    // Normalize longitudes to [0, 360) range to simplify calculations
    startLon = fmod((startLon + 360), 360);
    endLon   = fmod((endLon + 360), 360);

    // Determine if crossing occurs from east to west or west to east
    bool crossesFromEastToWest = startLon > 180 && endLon < 180;
    bool crossesFromWestToEast = startLon < 180 && endLon > 180;

    // If no wrap-around, return the original segment
    if (!crossesFromEastToWest && !crossesFromWestToEast)
    {
        // No wrap-around, return the original segment
        adjustedSegments.push_back(segment);
        return adjustedSegments;
    }

    // Calculate intersection latitude using linear interpolation
    double ratio =
        std::abs(startLon - 180.0) / std::abs(endLon - startLon);
    units::angle::degree_t intersectionLat = units::angle::degree_t(
        startLat + ratio * (endLat - startLat));

    // Create the intersection point at the boundary
    std::shared_ptr<GPoint> intersectionPoint =
        std::make_shared<GPoint>(crossesFromEastToWest
                                     ? units::angle::degree_t(180.0)
                                     : units::angle::degree_t(-180.0),
                                 intersectionLat);

    // Create two new segments from the original segment
    if (crossesFromEastToWest)
    {
        // Segment crosses from east to west
        if (*intersectionPoint != *segment->startPoint())
        {
            adjustedSegments.push_back(std::make_shared<GLine>(
                segment->startPoint(), intersectionPoint));
        }
        if (*intersectionPoint != *segment->endPoint())
        {
            adjustedSegments.push_back(std::make_shared<GLine>(
                std::make_shared<GPoint>(
                    units::angle::degree_t(-180.0), intersectionLat),
                segment->endPoint()));
        }
    }
    else
    {
        // Segment crosses from west to east
        if (*intersectionPoint != *segment->startPoint())
        {
            adjustedSegments.push_back(std::make_shared<GLine>(
                segment->startPoint(),
                std::make_shared<GPoint>(
                    units::angle::degree_t(180.0), intersectionLat)));
        }
        if (*intersectionPoint != *segment->endPoint())
        {
            adjustedSegments.push_back(std::make_shared<GLine>(
                intersectionPoint, segment->endPoint()));
        }
    }

    // Handle edge case: If no segments were added (due to zero-length
    // splits), return the original segment
    if (adjustedSegments.empty())
    {
        adjustedSegments.push_back(segment);
    }

    return adjustedSegments;
}

// GSegment overload — lightweight split
std::vector<GSegment>
Quadtree::splitSegmentAtAntimeridian(const GSegment &seg)
{
    std::vector<GSegment> result;

    double startLon = seg.startLon();
    double endLon   = seg.endLon();
    double startLat = seg.startLat();
    double endLat   = seg.endLat();

    // Normalize longitudes to [0, 360) range
    double normStart = fmod((startLon + 360.0), 360.0);
    double normEnd   = fmod((endLon + 360.0), 360.0);

    bool crossesEW = normStart > 180.0 && normEnd < 180.0;
    bool crossesWE = normStart < 180.0 && normEnd > 180.0;

    if (!crossesEW && !crossesWE)
    {
        result.push_back(seg);
        return result;
    }

    // Calculate intersection latitude
    double ratio =
        std::abs(normStart - 180.0) / std::abs(normEnd - normStart);
    double intersectLat = startLat + ratio * (endLat - startLat);

    if (crossesEW)
    {
        // start → 180, -180 → end
        if (std::abs(startLon - 180.0) > 1e-12 ||
            std::abs(startLat - intersectLat) > 1e-12)
            result.emplace_back(startLon, startLat, 180.0, intersectLat);
        if (std::abs(endLon - (-180.0)) > 1e-12 ||
            std::abs(endLat - intersectLat) > 1e-12)
            result.emplace_back(-180.0, intersectLat, endLon, endLat);
    }
    else
    {
        // start → -180, 180 → end (west to east crossing)
        // Actually: start → 180, -180 → end
        if (std::abs(startLon - 180.0) > 1e-12 ||
            std::abs(startLat - intersectLat) > 1e-12)
            result.emplace_back(startLon, startLat, 180.0, intersectLat);
        if (std::abs(endLon - (-180.0)) > 1e-12 ||
            std::abs(endLat - intersectLat) > 1e-12)
            result.emplace_back(-180.0, intersectLat, endLon, endLat);
    }

    if (result.empty())
        result.push_back(seg);

    return result;
}

bool Quadtree::Node::standardIntersectionCheck(
    const std::shared_ptr<GLine> &segment) const
{
    // Pure AABB arithmetic — no GLine/GPoint construction, no geodesic math.
    double sx = segment->startPoint()->getLongitude().value();
    double sy = segment->startPoint()->getLatitude().value();
    double ex = segment->endPoint()->getLongitude().value();
    double ey = segment->endPoint()->getLatitude().value();

    // Check if either endpoint is inside the node's bounding box
    if (sx >= minLon && sx <= maxLon && sy >= minLat && sy <= maxLat)
        return true;
    if (ex >= minLon && ex <= maxLon && ey >= minLat && ey <= maxLat)
        return true;

    // Segment bounding box vs node bounding box — quick reject
    double segMinLon = std::min(sx, ex);
    double segMaxLon = std::max(sx, ex);
    double segMinLat = std::min(sy, ey);
    double segMaxLat = std::max(sy, ey);

    if (segMaxLon < minLon || segMinLon > maxLon ||
        segMaxLat < minLat || segMinLat > maxLat)
        return false;

    double dx = ex - sx;
    double dy = ey - sy;

    auto clipTest = [](double p, double q,
                       double &tMin, double &tMax) -> bool {
        if (std::abs(p) < 1e-15)
            return q >= 0.0;
        double t = q / p;
        if (p < 0.0)
        {
            if (t > tMax) return false;
            if (t > tMin) tMin = t;
        }
        else
        {
            if (t < tMin) return false;
            if (t < tMax) tMax = t;
        }
        return true;
    };

    // Liang-Barsky algorithm for line-segment vs AABB clipping
    double tMin = 0.0, tMax = 1.0;

    if (!clipTest(-dx, sx - minLon, tMin, tMax)) return false;
    if (!clipTest( dx, maxLon - sx, tMin, tMax)) return false;
    if (!clipTest(-dy, sy - minLat, tMin, tMax)) return false;
    if (!clipTest( dy, maxLat - sy, tMin, tMax)) return false;

    return true;
}

bool Quadtree::isSegmentCrossingAntimeridian(
    const std::shared_ptr<GLine> &segment)
{
    if (!segment || !segment->startPoint() || !segment->endPoint())
    {
        qWarning()
            << "Invalid segment or points in antimeridian check";
        return false;
    }

    try
    {
        // Get coordinates and handle poles
        double startLon =
            segment->startPoint()->getLongitude().value();
        double endLon = segment->endPoint()->getLongitude().value();
        double startLat =
            segment->startPoint()->getLatitude().value();
        double endLat = segment->endPoint()->getLatitude().value();

        // Pole handling - if either point is at/near poles,
        // longitude differences don't matter
        const double POLE_THRESHOLD = 89.9; // degrees
        if (std::abs(startLat) > POLE_THRESHOLD
            || std::abs(endLat) > POLE_THRESHOLD)
        {
            return false;
        }

        // If either endpoint is at exactly ±180°, the segment is already
        // at the antimeridian boundary and doesn't need splitting
        const double BOUNDARY_TOLERANCE = 1e-9;
        if (std::abs(std::abs(startLon) - 180.0) < BOUNDARY_TOLERANCE ||
            std::abs(std::abs(endLon) - 180.0) < BOUNDARY_TOLERANCE)
        {
            return false;
        }

        // Normalize longitudes to [-180, 180] range
        auto normalizeLongitude = [](double lon) -> double {
            lon = std::fmod(lon + 180.0, 360.0);
            if (lon < 0)
                lon += 360.0;
            return lon - 180.0;
        };

        startLon = normalizeLongitude(startLon);
        endLon   = normalizeLongitude(endLon);

        // Calculate the shortest distance between longitudes
        double lonDiff = std::abs(endLon - startLon);
        if (lonDiff > 180.0)
        {
            lonDiff = 360.0 - lonDiff;
        }

        // Calculate the actual path distance
        double directDist = std::abs(endLon - startLon);

        const double TOLERANCE = 1e-10;
        return directDist > (lonDiff + TOLERANCE);
    }
    catch (const std::exception &e)
    {
        qWarning() << "Exception in antimeridian check:" << e.what();
        return false;
    }
}

// GSegment overload — replicates full antimeridian logic
bool Quadtree::isSegmentCrossingAntimeridian(const GSegment &seg)
{
    double startLon = seg.startLon();
    double endLon   = seg.endLon();
    double startLat = seg.startLat();
    double endLat   = seg.endLat();

    // Pole handling
    const double POLE_THRESHOLD = 89.9;
    if (std::abs(startLat) > POLE_THRESHOLD
        || std::abs(endLat) > POLE_THRESHOLD)
        return false;

    // Boundary tolerance — prevents infinite recursion
    const double BOUNDARY_TOLERANCE = 1e-9;
    if (std::abs(std::abs(startLon) - 180.0) < BOUNDARY_TOLERANCE ||
        std::abs(std::abs(endLon) - 180.0) < BOUNDARY_TOLERANCE)
        return false;

    // Normalize longitudes to [-180, 180]
    auto normalizeLongitude = [](double lon) -> double {
        lon = std::fmod(lon + 180.0, 360.0);
        if (lon < 0)
            lon += 360.0;
        return lon - 180.0;
    };

    startLon = normalizeLongitude(startLon);
    endLon   = normalizeLongitude(endLon);

    double lonDiff = std::abs(endLon - startLon);
    if (lonDiff > 180.0)
        lonDiff = 360.0 - lonDiff;

    double directDist = std::abs(endLon - startLon);

    const double TOLERANCE = 1e-10;
    return directDist > (lonDiff + TOLERANCE);
}

QVector<std::shared_ptr<GLine>>
Quadtree::getAllSegmentsInNode(const Node *node) const
{
    if (!node || node->isLeaf)
    {
        return node ? node->line_segments
                    : QVector<std::shared_ptr<GLine>>();
    }

    QVector<std::shared_ptr<GLine>> segments;
    for (const Node *child : node->children)
    {
        QVector<std::shared_ptr<GLine>> childSegments =
            getAllSegmentsInNode(child);

        segments +=
            childSegments; // append all the children to segments
    }
    return segments;
}

QVector<Quadtree::Node *>
Quadtree::getAdjacentNodes(const Node *node) const
{
    QVector<Node *> adjacentNodes;
    if (node == nullptr || node->parent == nullptr)
    {
        return adjacentNodes;
    }

    auto addChildren = [&adjacentNodes](const Node *n) {
        if (n != nullptr && !n->isLeaf)
        {
            for (Node *child : n->children)
            {
                if (child != nullptr)
                {
                    adjacentNodes.push_back(child);
                }
            }
        }
    };

    Node *parent   = node->parent;
    int   quadrant = node->quadrant;
    switch (quadrant)
    {
    case 0:
        addChildren(parent->children[1]);
        addChildren(parent->children[2]);
        break;
    case 1:
        addChildren(parent->children[0]);
        addChildren(parent->children[3]);
        break;
    case 2:
        addChildren(parent->children[0]);
        addChildren(parent->children[3]);
        break;
    case 3:
        addChildren(parent->children[1]);
        addChildren(parent->children[2]);
        break;
    }

    return adjacentNodes;
}

bool Quadtree::isNodeAtLeftEdge(const Node *node) const
{
    return std::abs(node->minLon - root.minLon) <= tolerance;
}

bool Quadtree::isNodeAtRightEdge(const Node *node) const
{
    return std::abs(node->maxLon - root.maxLon) <= tolerance;
}

QVector<Quadtree::Node *> Quadtree::findNodesOnRightEdge() const
{
    QVector<Node *>                   rightEdgeNodes;
    std::function<void(const Node *)> findRightEdgeNodes =
        [&](const Node *currentNode) {
            if (currentNode == nullptr)
                return;

            if (isNodeAtRightEdge(currentNode))
            {
                rightEdgeNodes.push_back(
                    const_cast<Node *>(currentNode));
            }

            if (!currentNode->isLeaf)
            {
                for (const Node *child : currentNode->children)
                {
                    findRightEdgeNodes(child);
                }
            }
        };

    findRightEdgeNodes(&root);
    return rightEdgeNodes;
}

QVector<Quadtree::Node *> Quadtree::findNodesOnLeftEdge() const
{
    QVector<Node *>                   leftEdgeNodes;
    std::function<void(const Node *)> findLeftEdgeNodes =
        [&](const Node *currentNode) {
            if (currentNode == nullptr)
                return;

            if (isNodeAtLeftEdge(currentNode))
            {
                leftEdgeNodes.push_back(
                    const_cast<Node *>(currentNode));
            }

            if (!currentNode->isLeaf)
            {
                for (const Node *child : currentNode->children)
                {
                    findLeftEdgeNodes(child);
                }
            }
        };

    findLeftEdgeNodes(&root);
    return leftEdgeNodes;
}

std::shared_ptr<GLine>
Quadtree::findLineSegment(const std::shared_ptr<GPoint> &point1,
                          const std::shared_ptr<GPoint> &point2) const
{
    // Create a line segment to use its bounding box
    auto searchSegment = std::make_shared<GLine>(point1, point2);

    // Find only the nodes that this line segment could possibly be in
    auto intersectingNodes =
        findNodesIntersectingLineSegment(searchSegment);

    // Search only in those nodes
    for (auto *node : intersectingNodes)
    {
        for (const auto &line : node->line_segments)
        {
            // Quick equality check using points
            bool startMatches = (*line->startPoint() == *point1);
            bool endMatches   = (*line->endPoint() == *point2);
            if (startMatches && endMatches)
            {
                return line;
            }

            // Check reverse direction
            if ((*line->startPoint() == *point2)
                && (*line->endPoint() == *point1))
            {
                return line;
            }
        }
    }

    return nullptr;
}

void Quadtree::insertLineSegment(
    const std::shared_ptr<GLine> &segment)
{
    if (!segment)
    {
        throw std::invalid_argument("Null segment");
    }
    // Start with the root node
    if (isSegmentCrossingAntimeridian(segment))
    {
        auto splitSegments = splitSegmentAtAntimeridian(segment);
        for (auto &splitSegment : splitSegments)
        {
            insertLineSegmentHelper(splitSegment, &root);
        }
    }
    else
    {
        insertLineSegmentHelper(segment, &root);
    }
}

void Quadtree::insertLineSegmentHelper(
    const std::shared_ptr<GLine> &segment, Node *node)
{
    if (!node || !node->doesLineSegmentIntersectNode(segment))
        return;

    if (node->isLeaf)
    {
        if (node->line_segments.size() < MAX_SEGMENTS_PER_NODE)
        {
            node->line_segments.push_back(segment);
            node->segments.emplace_back(GSegment(segment));
        }
        else
        {
            // Subdivide the node if it has reached its capacity
            node->subdivide(this);
            // Insert the segment into the appropriate child nodes
            for (int i = 0; i < 4; ++i)
            {
                if (node->children[i]->doesLineSegmentIntersectNode(
                        segment))
                {
                    insertLineSegmentHelper(segment,
                                            node->children[i]);
                }
            }
        }
    }
    else
    {
        for (int i = 0; i < 4; ++i)
        {
            if (node->children[i]->doesLineSegmentIntersectNode(
                    segment))
            {
                insertLineSegmentHelper(segment, node->children[i]);
            }
        }
    }
}

bool Quadtree::deleteLineSegment(
    const std::shared_ptr<GLine> &segment)
{
    return deleteLineSegmentHelper(segment, &root);
}

bool Quadtree::deleteLineSegmentHelper(
    const std::shared_ptr<GLine> &segment, Node *node)
{
    if (!node)
        return false;

    // Check if the segment intersects the node's bounding box
    if (!(*node).doesLineSegmentIntersectNode(segment))
    {
        return false;
    }

    if (node->isLeaf)
    {
        // Look for the segment in the current node's line segments
        auto it = std::find(node->line_segments.begin(),
                            node->line_segments.end(), segment);
        if (it != node->line_segments.end())
        {
            size_t idx = std::distance(node->line_segments.begin(), it);
            node->line_segments.erase(it);
            if (idx < node->segments.size())
            {
                node->segments.erase(node->segments.begin()
                                     + static_cast<ptrdiff_t>(idx));
            }
            return true;
        }
    }
    else
    {
        // If the node is not a leaf, check its children
        for (int i = 0; i < 4; ++i)
        {
            if (deleteLineSegmentHelper(segment, node->children[i]))
            {
                return true;
            }
        }
    }
    return false;
}

int Quadtree::getMaxDepth() const
{
    return getMaxDepthHelper(&root, 0);
}

int Quadtree::getMaxDepthHelper(const Node *node,
                                int         currentDepth) const
{
    if (!node || node->isLeaf)
    {
        return currentDepth;
    }

    int maxDepth = currentDepth;
    for (const Node *child : node->children)
    {
        if (child)
        {
            maxDepth = std::max(
                maxDepth, getMaxDepthHelper(child, currentDepth + 1));
        }
    }
    return maxDepth;
}

// Parallel range query
QVector<std::shared_ptr<GLine>>
Quadtree::rangeQueryParallel(const QRectF &range) const
{
    QVector<std::shared_ptr<GLine>> results;

    // Get candidate nodes
    QVector<Node *>                   candidateNodes;
    std::function<void(const Node *)> gatherNodes =
        [&](const Node *node) {
            if (!node)
                return;

            QRectF nodeBounds(
                QPointF(node->minLon, node->minLat),
                QPointF(node->maxLon, node->maxLat));

            if (range.intersects(nodeBounds))
            {
                if (node->isLeaf)
                {
                    candidateNodes.push_back(
                        const_cast<Node *>(node));
                }
                else
                {
                    for (auto *child : node->children)
                    {
                        gatherNodes(child);
                    }
                }
            }
        };

    gatherNodes(&root);

    // Process nodes in parallel using QtConcurrent
    QMutex mutex;
    auto   processNode = [this, &range, &mutex](Node *node) {
        QVector<std::shared_ptr<GLine>> nodeResults;

        for (const auto &segment : node->line_segments)
        {
            if (segmentIntersectsRange(segment, range))
            {
                nodeResults.push_back(segment);
            }
        }

        return nodeResults;
    };

    // Map each node to its results in parallel
    QFuture<QVector<std::shared_ptr<GLine>>> future =
        QtConcurrent::mapped(candidateNodes, processNode);

    // Wait for all parallel operations to complete
    future.waitForFinished();

    // Combine all results
    const auto &resultList = future.results();
    for (const auto &nodeResults : resultList)
    {
        results.append(nodeResults);
    }

    return results;
}

QVector<std::shared_ptr<GLine>>
Quadtree::rangeQuery(const QRectF &range) const
{
    QVector<std::shared_ptr<GLine>> foundSegments;

    // Initial range query
    rangeQueryHelper(range, &root, foundSegments);

    return foundSegments;
}

void Quadtree::rangeQueryHelper(
    const QRectF &range, // range should be in degrees
    const Node   *node,
    QVector<std::shared_ptr<GLine>> &foundSegments) const
{
    if (!node)
        return;

    // Create bounding box in degrees
    QRectF nodeBoundingBox(
        QPointF(node->minLon, node->minLat),
        QPointF(node->maxLon, node->maxLat));

    if (!range.intersects(nodeBoundingBox))
    {
        return;
    }

    if (node->isLeaf)
    {
        for (const std::shared_ptr<GLine> &segment :
             node->line_segments)
        {
            if (segmentIntersectsRange(segment, range))
            {
                foundSegments.push_back(segment);
            }
        }
    }
    else
    {
        for (const Node *child : node->children)
        {
            rangeQueryHelper(range, child, foundSegments);
        }
    }
}

bool Quadtree::segmentIntersectsRange(
    const std::shared_ptr<GLine> &segment, const QRectF &range) const
{
    // Use GSegment for fast AABB intersection
    GSegment seg(segment);
    return seg.intersectsAABB(range.left(), range.top(),
                              range.right(), range.bottom());
}

QVector<std::shared_ptr<GPoint>>
Quadtree::findVerticesInRange(const QRectF &range) const
{
    // Get segments in the range using existing range query
    auto segments = rangeQuery(range);

    // Extract range bounds for point filtering
    double minLon = range.left();
    double maxLon = range.right();
    double minLat = range.top();
    double maxLat = range.bottom();

    if (minLat > maxLat)
    {
        std::swap(minLat, maxLat);
    }

    std::unordered_set<std::shared_ptr<GPoint>, GPoint::Hash, GPoint::Equal>
        uniqueVertices;

    auto isPointInRange = [&](const std::shared_ptr<GPoint> &point) -> bool {
        double lon = point->getLongitude().value();
        double lat = point->getLatitude().value();
        return lon >= minLon && lon <= maxLon &&
               lat >= minLat && lat <= maxLat;
    };

    for (const auto &segment : segments)
    {
        if (isPointInRange(segment->startPoint()))
        {
            uniqueVertices.insert(segment->startPoint());
        }
        if (isPointInRange(segment->endPoint()))
        {
            uniqueVertices.insert(segment->endPoint());
        }
    }

    QVector<std::shared_ptr<GPoint>> result;
    result.reserve(uniqueVertices.size());
    for (const auto &vertex : uniqueVertices)
    {
        result.append(vertex);
    }

    return result;
}

std::shared_ptr<GLine> Quadtree::findNearestNeighbor(
    const std::shared_ptr<GPoint> &point) const
{
    std::shared_ptr<GLine> nearestSegment = nullptr;
    units::length::meter_t minDistance =
        units::length::meter_t(std::numeric_limits<double>::max());
    findNearestNeighborHelper(point, &root, nearestSegment,
                              minDistance);

    return nearestSegment;
}

void Quadtree::findNearestNeighborHelper(
    const std::shared_ptr<GPoint> &point, const Node *node,
    std::shared_ptr<GLine> &nearestSegment,
    units::length::meter_t &minDistance) const
{
    if (!node)
        return;

    units::length::meter_t distanceToNode =
        distanceFromPointToNode(point, node);
    if (distanceToNode > minDistance)
    {
        return;
    }

    if (node->isLeaf)
    {
        for (const auto &segment : node->line_segments)
        {
            units::length::meter_t distance =
                segment->distanceToPoint(point);
            if (distance < minDistance)
            {
                minDistance    = distance;
                nearestSegment = segment;
            }
        }
    }
    else
    {
        for (const Node *child : node->children)
        {
            findNearestNeighborHelper(point, child, nearestSegment,
                                      minDistance);
        }
    }
}

units::length::meter_t Quadtree::distanceFromPointToNode(
    const std::shared_ptr<GPoint> &point, const Node *node) const
{
    if (!node)
    {
        return units::length::meter_t(
            std::numeric_limits<double>::max());
    }

    double pLon = point->getLongitude().value();
    double pLat = point->getLatitude().value();
    double cLon = std::clamp(pLon, node->minLon, node->maxLon);
    double cLat = std::clamp(pLat, node->minLat, node->maxLat);
    if (cLon == pLon && cLat == pLat)
        return units::length::meter_t(0.0); // point inside box
    return units::length::meter_t(
        GSegment::haversineRaw(pLon, pLat, cLon, cLat));
}

std::shared_ptr<GPoint> Quadtree::findNearestNeighborPoint(
    const std::shared_ptr<GPoint> &point) const
{
    std::shared_ptr<GPoint> nearestPoint = nullptr;
    units::length::meter_t  minDistance(
        std::numeric_limits<double>::max());

    // Create priority queue of nodes sorted by distance to target
    struct NodeDist
    {
        const Node            *node;
        units::length::meter_t distance;

        bool operator>(const NodeDist &other) const
        {
            return distance > other.distance;
        }
    };

    std::priority_queue<NodeDist, std::vector<NodeDist>,
                        std::greater<NodeDist>>
        pq;

    // Start with root
    pq.push({&root, units::length::meter_t(0)});

    while (!pq.empty())
    {
        auto current = pq.top();
        pq.pop();

        // Early exit if this node can't have closer points
        if (current.distance >= minDistance)
        {
            break;
        }

        const Node *node = current.node;

        if (node->isLeaf)
        {
            if (node->line_segments.size()
                > MIN_SEGMENTS_FOR_PARALLEL)
            {
                // Parallel processing for large nodes
                auto futurePoints = QtConcurrent::mapped(
                    node->line_segments,
                    [&point](const std::shared_ptr<GLine> &segment) {
                        struct PointDist
                        {
                            std::shared_ptr<GPoint> point;
                            units::length::meter_t  distance;
                        };

                        PointDist result;
                        result.distance =
                            point->distance(*segment->startPoint());
                        result.point = segment->startPoint();

                        auto endDist =
                            point->distance(*segment->endPoint());
                        if (endDist < result.distance)
                        {
                            result.distance = endDist;
                            result.point    = segment->endPoint();
                        }

                        return result;
                    });

                futurePoints.waitForFinished();

                auto results = futurePoints.results();
                for (const auto &result : results)
                {
                    if (result.distance < minDistance)
                    {
                        minDistance  = result.distance;
                        nearestPoint = result.point;
                    }
                }
            }
            else
            {
                // Sequential processing for small nodes
                for (const auto &segment : node->line_segments)
                {
                    auto startDist =
                        point->distance(*segment->startPoint());
                    auto endDist =
                        point->distance(*segment->endPoint());

                    if (startDist < minDistance)
                    {
                        minDistance  = startDist;
                        nearestPoint = segment->startPoint();
                    }
                    if (endDist < minDistance)
                    {
                        minDistance  = endDist;
                        nearestPoint = segment->endPoint();
                    }
                }
            }
        }
        else
        {
            // Add child nodes to priority queue
            for (const auto &child : node->children)
            {
                if (!child)
                    continue;

                units::length::meter_t childDist =
                    child->distanceFromPointToBoundingBox(point);
                if (childDist < minDistance)
                {
                    pq.push({child, childDist});
                }
            }
        }
    }

    return nearestPoint;
}

void Quadtree::checkAndUpdateMinDistance(
    const std::shared_ptr<GPoint> &targetPoint,
    const std::shared_ptr<GPoint> &point,
    std::shared_ptr<GPoint>       &nearestPoint,
    units::length::meter_t        &minDistance) const
{
    units::length::meter_t distance = targetPoint->distance(*point);
    if (distance < minDistance)
    {
        minDistance  = distance;
        nearestPoint = point;
    }
}

void Quadtree::clearTree()
{
    // Recursively clear the tree starting from the root
    clearTreeHelper(&root);

    // Reset the root node's properties
    root.host     = this;
    root.quadrant = -1;
    root.isLeaf   = true;
    root.line_segments.clear();
    root.segments.clear();
    root.minLon = -180.0;
    root.minLat = -90.0;
    root.maxLon = 180.0;
    root.maxLat = 90.0;

    // Initialize children pointers to nullptr
    std::fill(std::begin(root.children), std::end(root.children),
              nullptr);
}

void Quadtree::clearTreeHelper(Node *node)
{
    if (!node)
        return;

    // Use a safer approach for tree traversal
    std::queue<Node *>  nodeQueue;
    std::vector<Node *> nodesToDelete;

    // Process children of root node
    for (int i = 0; i < 4; ++i)
    {
        if (node->children[i])
        {
            nodeQueue.push(node->children[i]);
            node->children[i] = nullptr; // Clear reference
        }
    }

    // Process all descendants
    while (!nodeQueue.empty())
    {
        Node *current = nodeQueue.front();
        nodeQueue.pop();

        // Queue for deletion
        nodesToDelete.push_back(current);

        // Process children
        for (int i = 0; i < 4; ++i)
        {
            if (current->children[i])
            {
                nodeQueue.push(current->children[i]);
                current->children[i] = nullptr;
            }
        }
    }

    // Delete all nodes
    for (Node *nodeToDelete : nodesToDelete)
    {
        nodeToDelete->line_segments.clear();
        nodeToDelete->segments.clear();
        delete nodeToDelete;
    }

    // Clear root node data
    node->line_segments.clear();
    node->segments.clear();
    std::fill(std::begin(node->children), std::end(node->children),
              nullptr);
}

void Quadtree::serialize(std::ostream &out) const
{
    serializeNode(out, &root);
}

void Quadtree::serializeNode(std::ostream &out,
                             const Node   *node) const
{
    if (!out)
    {
        throw std::runtime_error(
            "Output stream is not ready for writing.");
    }

    // Serialize nullity of the node
    bool isNull = (node == nullptr);
    out.write(reinterpret_cast<const char *>(&isNull),
              sizeof(isNull));
    if (isNull)
    {
        return;
    }

    // Serialize bounds as 4 doubles
    out.write(reinterpret_cast<const char *>(&node->minLon),
              sizeof(double));
    out.write(reinterpret_cast<const char *>(&node->minLat),
              sizeof(double));
    out.write(reinterpret_cast<const char *>(&node->maxLon),
              sizeof(double));
    out.write(reinterpret_cast<const char *>(&node->maxLat),
              sizeof(double));

    // Serialize line segments
    std::uint64_t numSegments =
        static_cast<std::uint64_t>(node->line_segments.size());
    out.write(reinterpret_cast<const char *>(&numSegments),
              sizeof(numSegments));

    for (const auto &segment : node->line_segments)
    {
        segment->startPoint()->serialize(out);
        segment->endPoint()->serialize(out);
    }

    // Serialize leaf status
    out.write(reinterpret_cast<const char *>(&node->isLeaf),
              sizeof(node->isLeaf));

    // Serialize child nodes recursively
    for (const auto &child : node->children)
    {
        serializeNode(out, child);
    }

    // Check for write failures
    if (!out)
    {
        throw std::runtime_error("Failed to write node "
                                 "data to output stream.");
    }
}

void Quadtree::deserialize(std::istream &in)
{
    try
    {
        // Clears the current tree
        clearTree();

        // Directly deserialize into the root node
        deserializeNode(in, &root);
    }
    catch (const std::runtime_error &e)
    {
        // Cleanup in case of an exception
        clearTree(); // Clear any partially constructed tree
        throw e;     // Rethrow the exception after cleanup
    }
}

void Quadtree::deserializeNode(std::istream &in, Node *parentNode)
{
    if (!in.good())
    {
        throw std::runtime_error("Input stream in bad state");
    }

    // 1. Read null marker
    bool isNull;
    in.read(reinterpret_cast<char *>(&isNull), sizeof(isNull));
    if (isNull)
        return;

    // 2. Deserialize bounds as 4 doubles
    in.read(reinterpret_cast<char *>(&parentNode->minLon),
            sizeof(double));
    in.read(reinterpret_cast<char *>(&parentNode->minLat),
            sizeof(double));
    in.read(reinterpret_cast<char *>(&parentNode->maxLon),
            sizeof(double));
    in.read(reinterpret_cast<char *>(&parentNode->maxLat),
            sizeof(double));

    // 3. Deserialize line segments
    std::uint64_t numSegments;
    in.read(reinterpret_cast<char *>(&numSegments),
            sizeof(numSegments));
    parentNode->line_segments.clear();
    parentNode->line_segments.reserve(numSegments);
    parentNode->segments.clear();
    parentNode->segments.reserve(numSegments);

    for (std::uint64_t i = 0; i < numSegments; ++i)
    {
        auto start = std::make_shared<GPoint>();
        auto end   = std::make_shared<GPoint>();
        start->deserialize(in);
        end->deserialize(in);
        auto seg = std::make_shared<GLine>(start, end);
        parentNode->line_segments.emplace_back(seg);
        parentNode->segments.emplace_back(GSegment(seg));
    }

    // 4. Deserialize leaf status
    in.read(reinterpret_cast<char *>(&parentNode->isLeaf),
            sizeof(parentNode->isLeaf));

    // 5. Deserialize children with proper parent relationships
    if (!parentNode->isLeaf)
    {
        for (int i = 0; i < 4; ++i)
        {
            parentNode->children[i] = new Node(this, parentNode, i);
            deserializeNode(in, parentNode->children[i]);

            // Validate quadrant boundaries
            const auto &child = parentNode->children[i];
            if (child->minLon < parentNode->minLon
                || child->maxLon > parentNode->maxLon
                || child->minLat < parentNode->minLat
                || child->maxLat > parentNode->maxLat)
            {
                throw std::runtime_error("Child node boundaries "
                                         "exceed parent limits");
            }
        }
    }
    else
    {
        // Ensure no children exist in leaf nodes
        std::fill(std::begin(parentNode->children),
                  std::end(parentNode->children), nullptr);
    }
}

Quadtree::Quadtree()
    : root(this)
{
}

units::angle::degree_t Quadtree::getMapWidth() const
{
    return units::angle::degree_t(root.maxLon - root.minLon);
}

units::angle::degree_t Quadtree::getMapHeight() const
{
    return units::angle::degree_t(root.maxLat - root.minLat);
}

bool Quadtree::isNearBoundary(
    const std::shared_ptr<GPoint> &point) const
{
    return (std::abs(point->getLongitude().value() - root.minLon)
            < tolerance)
           || (std::abs(point->getLongitude().value() - root.maxLon)
               < tolerance);
}

GPoint Quadtree::getMapMinPoint() const
{
    return GPoint(units::angle::degree_t(root.minLon),
                  units::angle::degree_t(root.minLat));
}

GPoint Quadtree::getMapMaxPoint() const
{
    return GPoint(units::angle::degree_t(root.maxLon),
                  units::angle::degree_t(root.maxLat));
}

}; // namespace ShipNetSimCore
