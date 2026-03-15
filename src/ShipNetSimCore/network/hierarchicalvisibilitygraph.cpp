#include "hierarchicalvisibilitygraph.h"
#include "../utils/utils.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QElapsedTimer>
#include <QFile>
#include <algorithm>
#include <chrono>
#include <functional>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace ShipNetSimCore
{

// =============================================================================
// ShortestPathResult
// =============================================================================

bool ShortestPathResult::isValid() const
{
    return (points.size() >= 2 && lines.size() >= 1
            && lines.size() == (points.size() - 1));
}

// =============================================================================
// Construction
// =============================================================================

HierarchicalVisibilityGraph::HierarchicalVisibilityGraph()
    : enableWrapAround(false)
{}

HierarchicalVisibilityGraph::HierarchicalVisibilityGraph(
    const QVector<std::shared_ptr<Polygon>>& usedPolygons)
    : polygons(usedPolygons)
{
    if (polygons.isEmpty())
    {
        qWarning() << "Warning: Empty polygon list provided to "
                      "HierarchicalVisibilityGraph";
    }

    enableWrapAround = true;

    // Set polygon ownership on vertices for O(1) lookup
    for (const auto& polygon : polygons)
    {
        if (!polygon) continue;
        for (const auto& vertex : polygon->outer())
        {
            if (vertex) vertex->addOwningPolygon(polygon);
        }
        for (const auto& hole : polygon->inners())
        {
            for (const auto& vertex : hole)
            {
                if (vertex) vertex->addOwningPolygon(polygon);
            }
        }
    }

    buildAllLevels();
}

HierarchicalVisibilityGraph::~HierarchicalVisibilityGraph()
{
}

// =============================================================================
// Level Building
// =============================================================================

void HierarchicalVisibilityGraph::buildAllLevels()
{
    for (int i = 0; i < NUM_LEVELS; ++i)
    {
        buildLevel(i);
    }

    // Build adjacency for coarser levels (1-3) sequentially.
    // OGRSpatialReference / PROJ is NOT thread-safe for concurrent access
    // to the same objects, so all adjacency building is single-threaded.
    for (int i = 1; i < NUM_LEVELS; ++i)
        buildAdjacencyForLevel(i);

    // Build polygon-level graph for instant L3 coarse routing
    buildPolygonGraph();
}

void HierarchicalVisibilityGraph::buildLevel(int idx)
{
    auto& level = mLevels[idx];
    level.levelIndex = idx;
    level.toleranceMeters = LEVEL_TOLERANCES[idx];

    if (idx == 0)
    {
        // Level 0: original polygons
        level.polygons = polygons;
    }
    else
    {
        // Levels 1-3: simplified polygons
        for (const auto& poly : polygons)
        {
            if (!poly) continue;
            auto simplified = poly->simplify(level.toleranceMeters);
            if (simplified && simplified->outerVertexCount() >= 3)
            {
                level.polygons.append(simplified);
            }
            else
            {
                // Fall back to original if simplification fails
                level.polygons.append(poly);
            }
        }
    }

    // Create Quadtree for this level
    try
    {
        level.quadtree = std::make_unique<Quadtree>(level.polygons);
    }
    catch (const std::exception& e)
    {
        qCritical() << "Exception creating Quadtree for level" << idx
                     << ":" << e.what();
        level.quadtree = std::make_unique<Quadtree>(
            QVector<std::shared_ptr<Polygon>>());
    }

    // Estimate total vertex count for pre-allocation
    int estimatedVertices = 0;
    for (const auto& poly : level.polygons)
    {
        if (!poly) continue;
        estimatedVertices += poly->outer().size();
        for (const auto& hole : poly->inners())
            estimatedVertices += hole.size();
    }
    level.vertices.reserve(estimatedVertices);
    level.vertexPolygonId.reserve(estimatedVertices);
    level.vertexIndex.reserve(estimatedVertices);

    // Collect all vertices and track ring layout metadata
    int numPolys = level.polygons.size();
    level.outerRings.resize(numPolys);
    level.holeRings.resize(numPolys);

    int vertexIdx = 0;
    for (int pi = 0; pi < numPolys; ++pi)
    {
        const auto& poly = level.polygons[pi];
        if (!poly) continue;

        // --- Outer ring ---
        const auto& outer = poly->outer();
        int outerCount = 0;
        int outerStart = vertexIdx;
        for (const auto& vertex : outer)
        {
            if (!vertex) continue;
            level.vertices.append(vertex);
            level.vertexIndex[vertex] = vertexIdx;
            level.vertexPolygonId.push_back(pi);

            // Set ownership on simplified vertices so isVisible()
            // skips the expensive findAllContainingPolygons() fallback
            if (idx > 0)
                vertex->addOwningPolygon(level.polygons[pi]);

            vertexIdx++;
            outerCount++;
        }
        level.outerRings[pi] = {outerStart, outerCount};

        // --- Hole rings ---
        const auto& holes = poly->inners();
        level.holeRings[pi].reserve(holes.size());
        for (const auto& hole : holes)
        {
            int holeStart = vertexIdx;
            int holeCount = 0;
            for (const auto& vertex : hole)
            {
                if (!vertex) continue;
                level.vertices.append(vertex);
                level.vertexIndex[vertex] = vertexIdx;
                level.vertexPolygonId.push_back(pi);

                if (idx > 0)
                    vertex->addOwningPolygon(level.polygons[pi]);

                vertexIdx++;
                holeCount++;
            }
            level.holeRings[pi].push_back({holeStart, holeCount});
        }
    }

    qDebug() << "Level" << idx << ": tolerance="
             << level.toleranceMeters << "m, vertices="
             << level.vertices.size();
}

// Per-level maximum distance thresholds (meters) for adjacency pre-filter.
// Pairs beyond this distance skip the expensive visibility check.
// Level 0 uses 0.0 (no cutoff — full resolution, pre-computed separately).
static constexpr double LEVEL_MAX_DISTANCE[
    HierarchicalVisibilityGraph::NUM_LEVELS] = {
    0.0,        // Level 0: not used here
    200000.0,   // Level 1: 200 km
    500000.0,   // Level 2: 500 km
    2000000.0   // Level 3: 2000 km
};

void HierarchicalVisibilityGraph::buildAdjacencyForLevel(int idx)
{
    auto& level = mLevels[idx];
    int n = level.vertices.size();
    level.adjacency.resize(n);

    if (n == 0) return;

    QElapsedTimer timer;
    timer.start();

    // Level 0: O(n²) with adaptive distance filter — comprehensive
    // adjacency for the AdjBuilder cache. Only called from
    // buildLevel0Adjacency() (ShipNetSimAdjBuilder tool), never at
    // runtime (runtime uses precomputeCorridorAdjacency instead).
    if (idx == 0)
    {
        // Adaptive distance: for small datasets (coarse resolution),
        // check all pairs (no distance limit) since O(n²) is fast.
        // For large datasets (dense resolution), use 50km limit to
        // keep the build tractable (~54 min for 446K vertices).
        // Threshold: 10K vertices ≈ O(50M) pairs, ~80s with filter.
        double l0MaxDist = (n < 10000) ? 0.0 : 50000.0;

        qDebug() << "Building L0 adjacency (O(n²), maxDist="
                 << l0MaxDist / 1000.0 << "km) with"
                 << n << "vertices";

        // Pre-extract coordinates for fast haversine in the inner loop
        std::vector<double> l0Lons(n), l0Lats(n);
        for (int i = 0; i < n; ++i)
        {
            l0Lons[i] = level.vertices[i]->getLongitude().value();
            l0Lats[i] = level.vertices[i]->getLatitude().value();
        }

        long long totalPairs =
            static_cast<long long>(n) * (n - 1) / 2;
        long long pairsChecked = 0;
        long long edgesAdded = 0;
        qint64 lastReportMs = 0;

        for (int i = 0; i < n; ++i)
        {
            for (int j = i + 1; j < n; ++j)
            {
                // Haversine distance pre-filter (skip if no limit)
                if (l0MaxDist > 0.0)
                {
                    double dist = GSegment::haversineRaw(
                        l0Lons[i], l0Lats[i],
                        l0Lons[j], l0Lats[j]);
                    if (dist > l0MaxDist) continue;
                }

                if (isVisible(level.vertices[i],
                              level.vertices[j], 0))
                {
                    level.adjacency[i].push_back(j);
                    level.adjacency[j].push_back(i);
                    edgesAdded++;
                }
            }

            pairsChecked += (n - i - 1);

            // Progress reporting every 10 seconds
            qint64 nowMs = timer.elapsed();
            if (nowMs - lastReportMs >= 10000)
            {
                lastReportMs = nowMs;
                double pct =
                    100.0 * pairsChecked / totalPairs;
                double elapsedSec = nowMs / 1000.0;
                double etaSec = (pct > 0.01)
                    ? elapsedSec / pct * (100.0 - pct)
                    : 0.0;
                qDebug().noquote()
                    << QString(
                           "  L0: %1% | vertex %2/%3 | "
                           "%4 edges | ETA %5")
                           .arg(pct, 0, 'f', 1)
                           .arg(i + 1)
                           .arg(n)
                           .arg(edgesAdded)
                           .arg(etaSec < 3600
                                    ? QString("%1 min")
                                          .arg(etaSec / 60.0,
                                               0, 'f', 1)
                                    : QString("%1 hr")
                                          .arg(etaSec / 3600.0,
                                               0, 'f', 1));

                emit pathFindingProgress(
                    i + 1, n, elapsedSec);
            }
        }

        long long totalEdges = 0;
        for (const auto& adj : level.adjacency)
            totalEdges += adj.size();
        qDebug() << "L0 adjacency built in" << timer.elapsed() << "ms,"
                 << totalEdges << "directed edges ("
                 << edgesAdded << "unique)";
        return;
    }

    // Levels 1-3: phased approach (fast, skip-vis for corridor guidance)
    double maxDist = LEVEL_MAX_DISTANCE[idx];

    qDebug() << "Building adjacency for level" << idx
             << "with" << n << "vertices (phased, maxDist="
             << maxDist / 1000.0 << "km)";

    // Phase 1: boundary edges — O(n), no visibility check
    addBoundaryEdges(level);
    qDebug() << "  Phase 1 (boundary edges) done in"
             << timer.elapsed() << "ms";

    // Phase 2: spatial grid edges — O(n × k)
    if (maxDist > 0.0)
    {
        addSpatialGridEdges(level, idx, maxDist);
        qDebug() << "  Phase 2 (spatial grid) done in"
                 << timer.elapsed() << "ms";
    }

    // Phase 3: antimeridian bridging edges
    if (maxDist > 500000.0)
    {
        addAntimeridianEdges(level, idx, maxDist);
        qDebug() << "  Phase 3 (antimeridian) done in"
                 << timer.elapsed() << "ms";
    }

    // Count total edges
    long long totalEdges = 0;
    for (const auto& adj : level.adjacency)
        totalEdges += adj.size();

    qDebug() << "Adjacency for level" << idx << "built in"
             << timer.elapsed() << "ms,"
             << totalEdges << "directed edges";
}

// =============================================================================
// Phase 1: Boundary Edges
// =============================================================================

void HierarchicalVisibilityGraph::addBoundaryEdges(GraphLevel& level)
{
    auto connectRing = [&](const RingRange& range) {
        if (range.count < 2) return;
        for (int r = 0; r < range.count; ++r)
        {
            int i = range.startIdx + r;
            int j = range.startIdx + (r + 1) % range.count;
            level.adjacency[i].push_back(j);
            level.adjacency[j].push_back(i);
        }
    };

    for (size_t pi = 0; pi < level.outerRings.size(); ++pi)
    {
        connectRing(level.outerRings[pi]);
        for (const auto& hr : level.holeRings[pi])
            connectRing(hr);
    }
}

// =============================================================================
// Phase 2: Spatial Grid Edges
// =============================================================================

void HierarchicalVisibilityGraph::addSpatialGridEdges(
    GraphLevel& level, int levelIdx, double maxDist)
{
    int n = level.vertices.size();
    if (n == 0) return;

    // Pre-extract coordinates for fast access
    std::vector<double> lons(n), lats(n);
    double minLon = 180.0, maxLon = -180.0;
    double minLat = 90.0,  maxLat = -90.0;
    for (int i = 0; i < n; ++i)
    {
        lons[i] = level.vertices[i]->getLongitude().value();
        lats[i] = level.vertices[i]->getLatitude().value();
        minLon = std::min(minLon, lons[i]);
        maxLon = std::max(maxLon, lons[i]);
        minLat = std::min(minLat, lats[i]);
        maxLat = std::max(maxLat, lats[i]);
    }

    // Build spatial grid
    double cellSize = std::max(maxDist / (111000.0 * 10.0), 0.5);
    int cols = std::max(1, static_cast<int>((maxLon - minLon) / cellSize) + 1);
    int rows = std::max(1, static_cast<int>((maxLat - minLat) / cellSize) + 1);

    // Clamp grid dimensions to prevent excessive memory
    if (static_cast<long long>(cols) * rows > 10000000LL)
    {
        cellSize = std::sqrt((maxLon - minLon) * (maxLat - minLat)
                             / 5000000.0);
        cols = std::max(1,
            static_cast<int>((maxLon - minLon) / cellSize) + 1);
        rows = std::max(1,
            static_cast<int>((maxLat - minLat) / cellSize) + 1);
    }

    std::vector<std::vector<int>> grid(
        static_cast<size_t>(rows) * cols);
    for (int i = 0; i < n; ++i)
    {
        int col = std::clamp(
            static_cast<int>((lons[i] - minLon) / cellSize), 0, cols - 1);
        int row = std::clamp(
            static_cast<int>((lats[i] - minLat) / cellSize), 0, rows - 1);
        grid[static_cast<size_t>(row) * cols + col].push_back(i);
    }

    int searchRadius = static_cast<int>(
        std::ceil(maxDist / (111000.0 * cellSize))) + 1;
    int maxCross = LEVEL_MAX_CROSS_CHECKS[levelIdx];

    // Helper: compute octant index (0-7) for direction from vertex i to j
    auto computeOctant = [](double dLon, double dLat) -> int {
        // Map angle to 0-7: N=0, NE=1, E=2, SE=3, S=4, SW=5, W=6, NW=7
        double angle = std::atan2(dLon, dLat);  // bearing-like (N=0)
        if (angle < 0.0) angle += 2.0 * M_PI;
        return static_cast<int>(angle / (M_PI / 4.0)) % 8;
    };

    // Duplicate-edge prevention set (avoids adding boundary edges again)
    // Using a flat vector of sorted adjacency for fast lookup
    std::vector<std::unordered_set<int>> existingEdges(n);
    for (int i = 0; i < n; ++i)
    {
        for (int j : level.adjacency[i])
            existingEdges[i].insert(j);
    }

    auto addEdgeIfNew = [&](int i, int j) {
        if (existingEdges[i].count(j) == 0)
        {
            level.adjacency[i].push_back(j);
            level.adjacency[j].push_back(i);
            existingEdges[i].insert(j);
            existingEdges[j].insert(i);
        }
    };

    for (int i = 0; i < n; ++i)
    {
        int polyI = level.vertexPolygonId[i];
        int col = std::clamp(
            static_cast<int>((lons[i] - minLon) / cellSize), 0, cols - 1);
        int row = std::clamp(
            static_cast<int>((lats[i] - minLat) / cellSize), 0, rows - 1);

        // Track best cross-polygon candidate per target polygon
        struct Candidate { int idx; double dist; };
        std::unordered_map<int, Candidate> bestPerPoly;

        // Track best same-polygon candidate per octant direction
        std::array<Candidate, 8> octants;
        for (auto& o : octants)
            o = {-1, std::numeric_limits<double>::max()};

        // Search nearby grid cells
        for (int dr = -searchRadius; dr <= searchRadius; ++dr)
        {
            int r = row + dr;
            if (r < 0 || r >= rows) continue;

            for (int dc = -searchRadius; dc <= searchRadius; ++dc)
            {
                int c = col + dc;
                if (c < 0 || c >= cols) continue;

                for (int j : grid[static_cast<size_t>(r) * cols + c])
                {
                    if (j <= i) continue;  // each pair processed once

                    // Haversine distance check
                    double dist = GSegment::haversineRaw(
                        lons[i], lats[i], lons[j], lats[j]);
                    if (dist > maxDist) continue;

                    int polyJ = level.vertexPolygonId[j];

                    if (polyI != polyJ)
                    {
                        // Cross-polygon: keep nearest per target polygon
                        auto it = bestPerPoly.find(polyJ);
                        if (it == bestPerPoly.end()
                            || dist < it->second.dist)
                        {
                            bestPerPoly[polyJ] = {j, dist};
                        }
                    }
                    else if (levelIdx > 0)
                    {
                        // Same polygon, coarse level: nearest per octant
                        double dLon = lons[j] - lons[i];
                        double dLat = lats[j] - lats[i];
                        int oct = computeOctant(dLon, dLat);
                        if (dist < octants[oct].dist)
                        {
                            octants[oct] = {j, dist};
                        }
                    }
                    // L0 same-polygon handled by O(n²) in buildAdjacencyForLevel
                }
            }
        }

        // Add cross-polygon edges (skip-vis — distance only for
        // corridor guidance at coarse levels)
        int crossCount = 0;
        for (const auto& [polyJ, cand] : bestPerPoly)
        {
            if (crossCount >= maxCross) break;
            addEdgeIfNew(i, cand.idx);
            crossCount++;
        }

        // Add same-polygon octant edges (coarse levels only, skip-vis)
        for (const auto& oct : octants)
        {
            if (oct.idx < 0) continue;
            addEdgeIfNew(i, oct.idx);
        }
    }
}

// =============================================================================
// Phase 3: Antimeridian Edges
// =============================================================================

void HierarchicalVisibilityGraph::addAntimeridianEdges(
    GraphLevel& level, int /*levelIdx*/, double maxDist)
{
    int n = level.vertices.size();
    if (n == 0) return;

    constexpr double ZONE = 30.0;  // degrees from antimeridian

    // Collect east-side and west-side vertices
    std::vector<int> eastSide, westSide;
    for (int i = 0; i < n; ++i)
    {
        double lon = level.vertices[i]->getLongitude().value();
        if (lon > 180.0 - ZONE) eastSide.push_back(i);
        if (lon < -180.0 + ZONE) westSide.push_back(i);
    }

    if (eastSide.empty() || westSide.empty()) return;

    // Duplicate-edge prevention
    std::unordered_set<long long> addedEdges;
    auto makeKey = [](int a, int b) -> long long {
        return static_cast<long long>(std::min(a, b)) * 1000000LL
               + std::max(a, b);
    };

    // For each east vertex, find nearest west vertices
    for (int ei : eastSide)
    {
        double eLon = level.vertices[ei]->getLongitude().value();
        double eLat = level.vertices[ei]->getLatitude().value();

        struct Candidate { int idx; double dist; };
        std::unordered_map<int, Candidate> bestPerPoly;

        for (int wi : westSide)
        {
            // Wrap-around distance: treat as if west is east + 360
            double wLon = level.vertices[wi]->getLongitude().value()
                          + 360.0;
            double dist = GSegment::haversineRaw(
                eLon, eLat, wLon,
                level.vertices[wi]->getLatitude().value());
            if (dist > maxDist) continue;

            int polyW = level.vertexPolygonId[wi];
            auto it = bestPerPoly.find(polyW);
            if (it == bestPerPoly.end() || dist < it->second.dist)
            {
                bestPerPoly[polyW] = {wi, dist};
            }
        }

        for (const auto& [polyW, cand] : bestPerPoly)
        {
            long long key = makeKey(ei, cand.idx);
            if (addedEdges.count(key)) continue;
            addedEdges.insert(key);

            level.adjacency[ei].push_back(cand.idx);
            level.adjacency[cand.idx].push_back(ei);
        }
    }
}

// =============================================================================
// Polygon Graph (L3 Coarse Routing)
// =============================================================================

void HierarchicalVisibilityGraph::buildPolygonGraph()
{
    const auto& lvl = mLevels[0];  // use L0 polygons for containment
    int numPolys = lvl.polygons.size();

    mPolygonGraph.numPolygons = numPolys;
    mPolygonGraph.adjacency.clear();
    mPolygonGraph.adjacency.resize(numPolys);
    mPolygonGraph.representatives.clear();
    mPolygonGraph.representatives.resize(numPolys);

    if (numPolys == 0) return;

    // Find representative vertex per polygon: nearest outer vertex
    // to the polygon envelope center.
    for (int pi = 0; pi < numPolys; ++pi)
    {
        const auto& poly = lvl.polygons[pi];
        if (!poly) continue;

        double envMinLon, envMaxLon, envMinLat, envMaxLat;
        poly->getEnvelope(envMinLon, envMaxLon, envMinLat, envMaxLat);
        double centerLon = (envMinLon + envMaxLon) / 2.0;
        double centerLat = (envMinLat + envMaxLat) / 2.0;

        double minDist = std::numeric_limits<double>::max();
        for (const auto& v : poly->outer())
        {
            if (!v) continue;
            double dist = GSegment::haversineRaw(
                v->getLongitude().value(), v->getLatitude().value(),
                centerLon, centerLat);
            if (dist < minDist)
            {
                minDist = dist;
                mPolygonGraph.representatives[pi] = v;
            }
        }
    }

    // Connect all polygon pairs (with 2 polygons this is a single edge)
    for (int pi = 0; pi < numPolys; ++pi)
    {
        if (!mPolygonGraph.representatives[pi]) continue;
        for (int pj = pi + 1; pj < numPolys; ++pj)
        {
            if (!mPolygonGraph.representatives[pj]) continue;

            double dist = mPolygonGraph.representatives[pi]
                              ->fastDistance(
                                  *mPolygonGraph.representatives[pj])
                              .value();

            mPolygonGraph.adjacency[pi].push_back({pj, dist});
            mPolygonGraph.adjacency[pj].push_back({pi, dist});
        }
    }

    qDebug() << "PolygonGraph built:" << numPolys << "polygons";
}

ShortestPathResult HierarchicalVisibilityGraph::polygonGraphSearch(
    const std::shared_ptr<GPoint>& start,
    const std::shared_ptr<GPoint>& goal)
{
    if (mPolygonGraph.numPolygons == 0)
        return ShortestPathResult();

    // Find containing polygons for start and goal
    const auto& lvl = mLevels[0];
    int startPolyIdx = -1, goalPolyIdx = -1;

    for (int pi = 0; pi < mPolygonGraph.numPolygons; ++pi)
    {
        if (pi >= lvl.polygons.size() || !lvl.polygons[pi]) continue;

        if (startPolyIdx < 0
            && lvl.polygons[pi]->isPointWithinPolygon(*start))
        {
            startPolyIdx = pi;
        }
        if (goalPolyIdx < 0
            && lvl.polygons[pi]->isPointWithinPolygon(*goal))
        {
            goalPolyIdx = pi;
        }
        if (startPolyIdx >= 0 && goalPolyIdx >= 0) break;
    }

    // If either point not in any polygon, try ringsContain fallback
    if (startPolyIdx < 0 || goalPolyIdx < 0)
    {
        for (int pi = 0; pi < mPolygonGraph.numPolygons; ++pi)
        {
            if (pi >= lvl.polygons.size() || !lvl.polygons[pi]) continue;
            if (startPolyIdx < 0
                && lvl.polygons[pi]->ringsContain(
                       std::const_pointer_cast<GPoint>(start)))
            {
                startPolyIdx = pi;
            }
            if (goalPolyIdx < 0
                && lvl.polygons[pi]->ringsContain(
                       std::const_pointer_cast<GPoint>(goal)))
            {
                goalPolyIdx = pi;
            }
        }
    }

    if (startPolyIdx < 0 || goalPolyIdx < 0)
        return ShortestPathResult();  // fallback to aStarAtLevel

    // Same polygon: return direct start→goal (no representative detour)
    if (startPolyIdx == goalPolyIdx)
    {
        ShortestPathResult result;
        result.points.append(start);
        result.points.append(goal);
        result.lines.append(
            std::make_shared<GLine>(start, goal, FastConstruct));
        return result;
    }

    // Different polygons: build path through representatives
    // Simple A* on the polygon graph (typically just 2 polygons)
    int numP = mPolygonGraph.numPolygons;
    std::vector<double> gScore(numP,
                               std::numeric_limits<double>::infinity());
    std::vector<int> cameFrom(numP, -1);
    std::vector<bool> closed(numP, false);

    gScore[startPolyIdx] = 0.0;

    using PQEntry = std::pair<double, int>;
    std::priority_queue<PQEntry, std::vector<PQEntry>,
                        std::greater<PQEntry>> openSet;
    openSet.push({0.0, startPolyIdx});

    while (!openSet.empty())
    {
        auto [fScore, current] = openSet.top();
        openSet.pop();

        if (closed[current]) continue;
        closed[current] = true;

        if (current == goalPolyIdx) break;

        for (const auto& edge : mPolygonGraph.adjacency[current])
        {
            if (closed[edge.targetPoly]) continue;
            double tentG = gScore[current] + edge.distance;
            if (tentG < gScore[edge.targetPoly])
            {
                gScore[edge.targetPoly] = tentG;
                cameFrom[edge.targetPoly] = current;
                openSet.push({tentG, edge.targetPoly});
            }
        }
    }

    if (cameFrom[goalPolyIdx] == -1 && startPolyIdx != goalPolyIdx)
        return ShortestPathResult();  // no path in polygon graph

    // Reconstruct polygon path
    std::vector<int> polyPath;
    for (int p = goalPolyIdx; p != -1; p = cameFrom[p])
        polyPath.push_back(p);
    std::reverse(polyPath.begin(), polyPath.end());

    // Build waypoint path: start → representatives → goal
    ShortestPathResult result;
    result.points.append(start);
    for (size_t i = 0; i < polyPath.size(); ++i)
    {
        const auto& rep = mPolygonGraph.representatives[polyPath[i]];
        if (rep && *rep != *start && *rep != *goal)
        {
            result.points.append(rep);
        }
    }
    result.points.append(goal);

    // Create line segments
    for (int i = 0; i < result.points.size() - 1; ++i)
    {
        result.lines.append(std::make_shared<GLine>(
            result.points[i], result.points[i + 1], FastConstruct));
    }

    return result;
}

// =============================================================================
// Distance-Limited Visibility Helper
// =============================================================================

QVector<std::shared_ptr<GPoint>>
HierarchicalVisibilityGraph::findNearestVisibleNodes(
    const std::shared_ptr<GPoint>& node,
    QVector<std::shared_ptr<GPoint>>& candidates,
    int level,
    int maxCheck,
    int maxFound)
{
    int sortLimit = std::min(maxCheck,
                             static_cast<int>(candidates.size()));
    if (sortLimit <= 0)
        return {};

    // Partial sort: bring nearest sortLimit candidates to front
    std::partial_sort(
        candidates.begin(),
        candidates.begin() + sortLimit,
        candidates.end(),
        [&node](const auto& a, const auto& b) {
            return node->fastDistance(*a) < node->fastDistance(*b);
        });

    QVector<std::shared_ptr<GPoint>> visible;
    visible.reserve(maxFound);

    for (int i = 0; i < sortLimit
                    && visible.size() < maxFound; ++i)
    {
        if (isVisible(node, candidates[i], level))
            visible.append(candidates[i]);
    }

    return visible;
}

// =============================================================================
// Vertex Injection into Coarse Levels
// =============================================================================

int HierarchicalVisibilityGraph::injectPointIntoLevel(
    const std::shared_ptr<GPoint>& point, int level)
{
    if (!point || level < 0 || level >= NUM_LEVELS)
        return -1;

    auto& lvl = mLevels[level];

    // Already present as a graph vertex — no injection needed
    auto it = lvl.vertexIndex.find(point);
    if (it != lvl.vertexIndex.end())
        return it->second;

    // Find the containing or nearest polygon at this level
    int bestPolyIdx = -1;
    double bestDist = std::numeric_limits<double>::max();

    for (int pi = 0; pi < lvl.polygons.size(); ++pi)
    {
        const auto& poly = lvl.polygons[pi];
        if (!poly) continue;

        if (poly->isPointWithinPolygon(*point))
        {
            bestPolyIdx = pi;
            break;
        }

        // Track nearest polygon by distance to outer ring vertices
        for (const auto& v : poly->outer())
        {
            if (!v) continue;
            double d = point->fastDistance(*v).value();
            if (d < bestDist)
            {
                bestDist = d;
                bestPolyIdx = pi;
            }
        }
    }

    if (bestPolyIdx < 0)
        return -1;

    // Add point to the level's vertex structures
    int newIdx = lvl.vertices.size();
    lvl.vertices.append(point);
    lvl.vertexIndex[point] = newIdx;
    lvl.vertexPolygonId.push_back(bestPolyIdx);

    // Extend adjacency to accommodate the new vertex
    lvl.adjacency.resize(newIdx + 1);

    // Set polygon ownership for fast visibility lookups
    point->addOwningPolygon(lvl.polygons[bestPolyIdx]);

    // Connect to nearest vertices in the same polygon.
    // This gives the injected point adjacency edges so A* can
    // route through it, similar to Phase 2 spatial grid edges.
    static constexpr int MAX_INJECT_NEIGHBORS = 8;

    struct Candidate { int idx; double dist; };
    std::vector<Candidate> candidates;
    candidates.reserve(256);

    for (int vi = 0; vi < newIdx; ++vi)
    {
        if (lvl.vertexPolygonId[vi] != bestPolyIdx)
            continue;
        double d = point->fastDistance(*lvl.vertices[vi]).value();
        candidates.push_back({vi, d});
    }

    if (candidates.empty())
        return newIdx;

    // Partial sort to find the nearest MAX_INJECT_NEIGHBORS
    int limit = std::min(static_cast<int>(candidates.size()),
                         MAX_INJECT_NEIGHBORS);
    std::partial_sort(
        candidates.begin(),
        candidates.begin() + limit,
        candidates.end(),
        [](const Candidate& a, const Candidate& b) {
            return a.dist < b.dist;
        });

    for (int i = 0; i < limit; ++i)
    {
        lvl.adjacency[newIdx].push_back(candidates[i].idx);
        lvl.adjacency[candidates[i].idx].push_back(newIdx);
    }

    return newIdx;
}

// =============================================================================
// Point Snapping
// =============================================================================

std::shared_ptr<GPoint> HierarchicalVisibilityGraph::snapToWater(
    const std::shared_ptr<GPoint>& point, int level) const
{
    const auto& lvl = mLevels[level];
    const double ptLon = point->getLongitude().value();
    const double ptLat = point->getLatitude().value();

    // Check if point is already in a water polygon
    for (const auto& polygon : lvl.polygons)
    {
        // Envelope pre-filter to skip polygons far from point
        double minLon, maxLon, minLat, maxLat;
        polygon->getEnvelope(minLon, maxLon, minLat, maxLat);
        if (ptLon < minLon || ptLon > maxLon ||
            ptLat < minLat || ptLat > maxLat)
            continue;

        if (polygon->isPointWithinPolygon(*point))
        {
            // Ensure owning polygons are set so isSegmentVisible
            // doesn't need to call findAllContainingPolygons
            if (point->getOwningPolygons().isEmpty())
            {
                point->addOwningPolygon(polygon);
            }
            return point; // Already in water
        }
    }

    // Point is on land - determine why and snap
    for (const auto& polygon : lvl.polygons)
    {
        // Envelope pre-filter
        double minLon, maxLon, minLat, maxLat;
        polygon->getEnvelope(minLon, maxLon, minLat, maxLat);
        if (ptLon < minLon || ptLon > maxLon ||
            ptLat < minLat || ptLat > maxLat)
            continue;

        if (!polygon->isPointWithinExteriorRing(*point))
            continue;

        // Check if inside a hole (island)
        int holeIdx = polygon->findContainingHoleIndex(*point);
        if (holeIdx >= 0)
        {
            // Snap to nearest hole vertex
            auto holeVertices = polygon->inners()[holeIdx];
            units::length::meter_t minDist(
                std::numeric_limits<double>::max());
            std::shared_ptr<GPoint> nearest;

            for (const auto& vertex : holeVertices)
            {
                auto dist = point->fastDistance(*vertex);
                if (dist < minDist)
                {
                    minDist = dist;
                    nearest = vertex;
                }
            }
            if (nearest) return nearest;
        }
        else
        {
            // Inside exterior but not in hole - use nearest neighbor
            if (lvl.quadtree)
                return lvl.quadtree->findNearestNeighborPoint(point);
        }
    }

    // Outside all polygons - find nearest outer vertex
    units::length::meter_t minDist(std::numeric_limits<double>::max());
    std::shared_ptr<GPoint> nearest;

    for (const auto& polygon : lvl.polygons)
    {
        for (const auto& vertex : polygon->outer())
        {
            auto dist = point->fastDistance(*vertex);
            if (dist < minDist)
            {
                minDist = dist;
                nearest = vertex;
            }
        }
    }

    return nearest;
}

// =============================================================================
// A* at Single Level
// =============================================================================

ShortestPathResult HierarchicalVisibilityGraph::aStarAtLevel(
    const std::shared_ptr<GPoint>& start,
    const std::shared_ptr<GPoint>& goal,
    int level,
    const Corridor* corridor,
    const std::shared_ptr<GPoint>& preSnappedStart,
    const std::shared_ptr<GPoint>& preSnappedGoal)
{
    if (!start || !goal)
    {
        qWarning() << "Invalid start or end point for A* at level" << level;
        return ShortestPathResult();
    }

    if (*start == *goal)
    {
        ShortestPathResult result;
        result.points.append(start);
        return result;
    }

    // Use pre-snapped points if provided, otherwise snap
    auto startNav = preSnappedStart ? preSnappedStart
                                    : snapToWater(start, level);
    auto endNav = preSnappedGoal ? preSnappedGoal
                                : snapToWater(goal, level);

    if (!startNav || !endNav)
    {
        qWarning() << "Could not snap points to water at level" << level;
        return ShortestPathResult();
    }

    qDebug() << "A* level" << level << ": from"
             << startNav->toString() << "to" << endNav->toString();

    // --- Integer-based A* for performance ---
    // Avoid shared_ptr hash/refcount overhead by mapping points to ints.
    // Points are stored in a flat vector; all A* structures use int indices.
    QVector<std::shared_ptr<GPoint>> idxToPoint;
    std::unordered_map<std::shared_ptr<GPoint>, int,
                       GPoint::Hash, GPoint::Equal> pointToIdx;

    auto getOrAddIndex = [&](const std::shared_ptr<GPoint>& pt) -> int {
        auto it = pointToIdx.find(pt);
        if (it != pointToIdx.end())
            return it->second;
        int idx = idxToPoint.size();
        idxToPoint.append(pt);
        pointToIdx[pt] = idx;
        return idx;
    };

    int startIdx = getOrAddIndex(startNav);
    int endIdx   = getOrAddIndex(endNav);

    // Pre-cache the goal coordinates for fast heuristic
    const double endLat = endNav->getLatitude().value();
    const double endLon = endNav->getLongitude().value();

    // Heuristic using haversine (avoids validateSpatialReferences overhead)
    auto heuristic = [endLat, endLon](const std::shared_ptr<GPoint>& pt)
        -> double {
        double lat1 = pt->getLatitude().value() * M_PI / 180.0;
        double lat2 = endLat * M_PI / 180.0;
        double dLat = lat2 - lat1;
        double dLon = (pt->getLongitude().value() - endLon) * M_PI / 180.0;
        double a = std::sin(dLat / 2.0) * std::sin(dLat / 2.0) +
                   std::cos(lat1) * std::cos(lat2) *
                   std::sin(dLon / 2.0) * std::sin(dLon / 2.0);
        return 6371000.0 * 2.0 * std::asin(std::sqrt(a));
    };

    // A* data structures using integer indices
    // Dynamic vectors that grow as new points are discovered
    std::vector<double> gScore(2, std::numeric_limits<double>::infinity());
    std::vector<int> cameFrom(2, -1);
    std::vector<bool> closed(2, false);

    auto ensureCapacity = [&](int idx) {
        if (idx >= static_cast<int>(gScore.size()))
        {
            int newSize = idx + 1;
            gScore.resize(newSize, std::numeric_limits<double>::infinity());
            cameFrom.resize(newSize, -1);
            closed.resize(newSize, false);
        }
    };

    // Min-heap: (fScore, vertexIndex)
    std::priority_queue<
        std::pair<double, int>,
        std::vector<std::pair<double, int>>,
        std::greater<std::pair<double, int>>>
        openSet;

    gScore[startIdx] = 0.0;
    openSet.push({heuristic(startNav), startIdx});

    // Pre-connect goal to the graph — mirrors how
    // precomputeCorridorAdjacency adds goal as a vertex.
    // Skip when corridor already has goal with full adjacency.
    std::unordered_set<int> goalReachableFrom;
    bool goalInCorridorAdj = corridor && corridor->hasAdjacency
        && corridor->vertexIndex.count(endNav) > 0;
    if (!goalInCorridorAdj)
    {
        auto goalNeighbors = getVisibleNodesForPoint(endNav, level, corridor);
        for (const auto& gn : goalNeighbors) {
            int gnIdx = getOrAddIndex(gn);
            ensureCapacity(gnIdx);
            goalReachableFrom.insert(gnIdx);
        }
    }

    QElapsedTimer progressTimer;
    progressTimer.start();
    qint64 lastProgressEmit = 0;
    int iterationCount = 0;

    while (!openSet.empty())
    {
        if (++iterationCount % 64 == 0)
        {
            if (QThread::currentThread()->isInterruptionRequested())
            {
                qWarning() << "A* cancelled by thread interruption";
                return ShortestPathResult();
            }

            qint64 nowMs = progressTimer.elapsed();
            if (nowMs - lastProgressEmit >= 1000)
            {
                lastProgressEmit = nowMs;
                emit pathFindingProgress(-1, 0, nowMs / 1000.0);
            }
        }

        auto [currentF, currentIdx] = openSet.top();
        openSet.pop();

        if (closed[currentIdx])
            continue;
        closed[currentIdx] = true;

        if (currentIdx == endIdx)
            break;

        auto current = idxToPoint[currentIdx];

        // Get neighbors
        auto neighbors = getVisibleNodesForPoint(current, level, corridor);

        // If current vertex can reach the goal (pre-computed), add as neighbor
        if (!closed[endIdx] && goalReachableFrom.count(currentIdx))
        {
            neighbors.append(endNav);
        }

        // Wrap-around connections
        if (enableWrapAround && level == 0)
        {
            neighbors.append(connectWrapAroundPoints(current, endNav));
        }

        for (const auto& neighbor : neighbors)
        {
            int neighborIdx = getOrAddIndex(neighbor);
            ensureCapacity(neighborIdx);

            if (closed[neighborIdx])
                continue;

            double edgeCost =
                current->fastDistance(*neighbor).value();

            // Apply penalty for edges flagged by iterative validation
            if (corridor && !corridor->penalizedEdges.empty())
            {
                auto ci = corridor->vertexIndex.find(current);
                auto ni = corridor->vertexIndex.find(neighbor);
                if (ci != corridor->vertexIndex.end()
                    && ni != corridor->vertexIndex.end())
                {
                    if (corridor->penalizedEdges.count(
                            Corridor::edgeKey(ci->second,
                                              ni->second)))
                    {
                        edgeCost += 1e9;
                    }
                }
            }

            double tentG = gScore[currentIdx] + edgeCost;

            if (tentG < gScore[neighborIdx])
            {
                cameFrom[neighborIdx] = currentIdx;
                gScore[neighborIdx] = tentG;
                openSet.push({tentG + heuristic(neighbor), neighborIdx});
            }
        }
    }

    if (cameFrom[endIdx] == -1 && startIdx != endIdx)
    {
        qDebug() << "A* level" << level << ": no path found";
        return ShortestPathResult();
    }

    // Reconstruct path from integer indices
    std::unordered_map<std::shared_ptr<GPoint>, std::shared_ptr<GPoint>,
                       GPoint::Hash, GPoint::Equal> cameFromMap;
    for (int i = 0; i < static_cast<int>(cameFrom.size()); ++i)
    {
        if (cameFrom[i] >= 0)
        {
            cameFromMap[idxToPoint[i]] = idxToPoint[cameFrom[i]];
        }
    }

    return reconstructPath(cameFromMap, endNav, level);
}

// =============================================================================
// Hierarchical Search
// =============================================================================

ShortestPathResult HierarchicalVisibilityGraph::hierarchicalSearch(
    const std::shared_ptr<GPoint>& start,
    const std::shared_ptr<GPoint>& goal)
{
    qDebug() << "=== Hierarchical Search ===";

    if (!start || !goal)
    {
        qWarning() << "hierarchicalSearch: null start or goal";
        return ShortestPathResult();
    }

    // Track the best coarse result available for corridor building.
    // Coarse results are NEVER returned — only used to guide Level 0.
    ShortestPathResult bestCoarseResult;

    // Snap ONCE at L0 (highest resolution, closest vertex match).
    // Reuse these snapped points at ALL levels for consistency.
    auto snappedStart = snapToWater(start, 0);
    auto snappedGoal  = snapToWater(goal, 0);

    // Inject L0-snapped points into coarse levels (L1-L3) so they
    // exist as first-class graph vertices with adjacency at every level.
    // This ensures consistent corridor guidance regardless of
    // simplification-induced vertex loss.
    for (int lvl = 1; lvl < NUM_LEVELS; ++lvl)
    {
        if (snappedStart)
            injectPointIntoLevel(snappedStart, lvl);
        if (snappedGoal)
            injectPointIntoLevel(snappedGoal, lvl);
    }

    // --- Level 3 (coarsest) — use polygon graph for instant routing ---
    auto result3 = polygonGraphSearch(start, goal);

    if (!result3.isValid())
    {
        // Polygon graph failed, try vertex-level L3 A*
        result3 = aStarAtLevel(start, goal, 3, nullptr,
                                snappedStart, snappedGoal);
    }

    if (!result3.isValid())
    {
        qDebug() << "Level 3 failed, falling back to unconstrained level 0";
        return aStarAtLevel(start, goal, 0, nullptr,
                            snappedStart, snappedGoal);
    }
    bestCoarseResult = result3;

    // --- Level 2 ---
    double expansion2 = LEVEL_TOLERANCES[3] * 3.0;
    auto corridor2 = buildCorridor(result3, 2, expansion2);
    auto result2 = aStarAtLevel(start, goal, 2, &corridor2,
                                snappedStart, snappedGoal);

    if (!result2.isValid())
    {
        auto widerCorridor2 = buildCorridor(result3, 2, expansion2 * 3.0);
        result2 = aStarAtLevel(start, goal, 2, &widerCorridor2,
                               snappedStart, snappedGoal);
    }

    if (result2.isValid())
    {
        bestCoarseResult = std::move(result2);
    }

    // --- Level 1 ---
    double expansion1 = LEVEL_TOLERANCES[2] * 3.0;
    auto corridor1 = buildCorridor(bestCoarseResult, 1, expansion1);
    auto result1 = aStarAtLevel(start, goal, 1, &corridor1,
                                snappedStart, snappedGoal);

    if (!result1.isValid())
    {
        auto widerCorridor1 = buildCorridor(bestCoarseResult, 1,
                                            expansion1 * 3.0);
        result1 = aStarAtLevel(start, goal, 1, &widerCorridor1,
                               snappedStart, snappedGoal);
    }

    if (result1.isValid())
    {
        bestCoarseResult = std::move(result1);
    }

    // --- Level 0 (original polygons — only valid final result) ---

    double expansion0 = LEVEL_TOLERANCES[1] * 3.0;
    bool hasLevel0Adj = mLevel0AdjReady.load(std::memory_order_acquire);

    if (hasLevel0Adj)
    {
        // Level 0 adjacency is pre-computed — corridors act only as
        // geographic filters via allowedVertexIndices. No per-query
        // precomputeCorridorAdjacency needed.
        auto corridor0 = buildCorridor(bestCoarseResult, 0, expansion0);
        auto result0 = aStarAtLevel(start, goal, 0, &corridor0,
                                    snappedStart, snappedGoal);

        if (result0.isValid())
            return result0;

        auto widerCorridor0 = buildCorridor(bestCoarseResult, 0,
                                            expansion0 * 3.0);
        result0 = aStarAtLevel(start, goal, 0, &widerCorridor0,
                               snappedStart, snappedGoal);

        if (result0.isValid())
            return result0;

        auto veryWideCorridor0 = buildCorridor(bestCoarseResult, 0,
                                               expansion0 * 10.0);
        result0 = aStarAtLevel(start, goal, 0, &veryWideCorridor0,
                               snappedStart, snappedGoal);

        if (result0.isValid())
            return result0;
    }
    else
    {
        // Pre-compute corridor adjacency with phased approach,
        // then validate with penalty-based iterative pruning.
        auto validateAndSearch =
            [&](Corridor& corr) -> ShortestPathResult
        {
            ShortestPathResult bestResult;
            int bestBadCount = std::numeric_limits<int>::max();

            for (int iter = 0; iter < 20; ++iter)
            {
                auto result = aStarAtLevel(start, goal, 0, &corr,
                                           snappedStart, snappedGoal);
                if (!result.isValid()) break;

                int prevSize = static_cast<int>(
                    corr.penalizedEdges.size());
                int badCount = 0;

                for (int i = 0; i < result.lines.size(); ++i)
                {
                    // Skip short edges (coastline boundary tracing)
                    if (result.lines[i]->length().value() < 2000.0)
                        continue;

                    if (!isSegmentVisible(result.lines[i], 0))
                    {
                        auto si = corr.vertexIndex.find(
                            result.points[i]);
                        auto ei = corr.vertexIndex.find(
                            result.points[i + 1]);
                        if (si != corr.vertexIndex.end()
                            && ei != corr.vertexIndex.end())
                        {
                            corr.penalizedEdges.insert(
                                Corridor::edgeKey(si->second,
                                                  ei->second));
                        }
                        badCount++;
                    }
                }

                if (badCount < bestBadCount)
                {
                    bestResult = result;
                    bestBadCount = badCount;
                }
                if (badCount == 0) break;
                if (static_cast<int>(corr.penalizedEdges.size())
                    == prevSize)
                    break;  // converged
            }
            return bestResult;
        };

        auto corridor0 = buildCorridor(bestCoarseResult, 0, expansion0);
        precomputeCorridorAdjacency(corridor0, start, goal);
        auto result0 = validateAndSearch(corridor0);

        if (result0.isValid())
            return result0;

        auto widerCorridor0 = buildCorridor(bestCoarseResult, 0,
                                            expansion0 * 3.0);
        precomputeCorridorAdjacency(widerCorridor0, start, goal,
                                    &corridor0);
        result0 = validateAndSearch(widerCorridor0);

        if (result0.isValid())
            return result0;

        auto veryWideCorridor0 = buildCorridor(bestCoarseResult, 0,
                                               expansion0 * 10.0);
        precomputeCorridorAdjacency(veryWideCorridor0, start, goal,
                                    &widerCorridor0);
        result0 = validateAndSearch(veryWideCorridor0);

        if (result0.isValid())
            return result0;
    }

    // Final fallback: unconstrained Level 0 A*
    qDebug() << "All corridor attempts failed, "
                "falling back to unconstrained level 0";
    return aStarAtLevel(start, goal, 0, nullptr,
                        snappedStart, snappedGoal);
}

// =============================================================================
// Corridor Construction
// =============================================================================

Corridor HierarchicalVisibilityGraph::buildCorridor(
    const ShortestPathResult& coarsePath,
    int targetLevel,
    double expansion)
{
    Corridor corridor;
    if (coarsePath.points.isEmpty())
        return corridor;

    corridor.minLon = std::numeric_limits<double>::max();
    corridor.maxLon = std::numeric_limits<double>::lowest();
    corridor.minLat = std::numeric_limits<double>::max();
    corridor.maxLat = std::numeric_limits<double>::lowest();

    for (const auto& waypoint : coarsePath.points)
    {
        double lon = waypoint->getLongitude().value();
        double lat = waypoint->getLatitude().value();

        double latRad = lat * M_PI / 180.0;
        double cosLat = std::cos(latRad);
        double lonExpand = (cosLat > 1e-6)
                               ? expansion / (111000.0 * cosLat)
                               : 180.0;
        double latExpand = expansion / 111000.0;

        corridor.minLon = std::min(corridor.minLon, lon - lonExpand);
        corridor.maxLon = std::max(corridor.maxLon, lon + lonExpand);
        corridor.minLat = std::min(corridor.minLat, lat - latExpand);
        corridor.maxLat = std::max(corridor.maxLat, lat + latExpand);
    }

    // Use tube filtering only when coarse path has 3+ waypoints
    // (curved route). For 2-point paths (direct line), the tube would
    // be too narrow for routes that need to detour around land.
    bool useTube = (coarsePath.points.size() >= 3);

    std::vector<double> wpLons, wpLats;
    if (useTube)
    {
        wpLons.reserve(coarsePath.points.size());
        wpLats.reserve(coarsePath.points.size());
        for (const auto& wp : coarsePath.points)
        {
            wpLons.push_back(wp->getLongitude().value());
            wpLats.push_back(wp->getLatitude().value());
        }
    }

    // Populate allowed vertex indices via quadtree range query,
    // with optional per-waypoint tube filter for curved routes.
    const auto& lvl = mLevels[targetLevel];
    if (lvl.quadtree)
    {
        double width  = corridor.maxLon - corridor.minLon;
        double height = corridor.maxLat - corridor.minLat;
        QRectF range(corridor.minLon, corridor.minLat, width, height);

        auto rangeVertices = lvl.quadtree->findVerticesInRange(range);
        for (const auto& v : rangeVertices)
        {
            auto it = lvl.vertexIndex.find(v);
            if (it == lvl.vertexIndex.end()) continue;

            if (useTube)
            {
                double vLon = v->getLongitude().value();
                double vLat = v->getLatitude().value();
                bool inTube = false;
                for (size_t w = 0; w < wpLons.size() && !inTube; ++w)
                {
                    if (GSegment::haversineRaw(vLon, vLat,
                                               wpLons[w], wpLats[w])
                        <= expansion)
                    {
                        inTube = true;
                    }
                }
                if (!inTube) continue;
            }

            corridor.allowedVertexIndices.insert(it->second);
        }
    }
    else
    {
        // Fallback to linear scan
        for (int i = 0; i < lvl.vertices.size(); ++i)
        {
            double vLon = lvl.vertices[i]->getLongitude().value();
            double vLat = lvl.vertices[i]->getLatitude().value();

            if (!corridor.containsPoint(vLon, vLat)) continue;

            if (useTube)
            {
                bool inTube = false;
                for (size_t w = 0; w < wpLons.size() && !inTube; ++w)
                {
                    if (GSegment::haversineRaw(vLon, vLat,
                                               wpLons[w], wpLats[w])
                        <= expansion)
                    {
                        inTube = true;
                    }
                }
                if (!inTube) continue;
            }

            corridor.allowedVertexIndices.insert(i);
        }
    }

    qDebug() << "Corridor for level" << targetLevel << ":"
             << corridor.allowedVertexIndices.size()
             << "allowed vertices out of" << lvl.vertices.size();

    return corridor;
}

void HierarchicalVisibilityGraph::precomputeCorridorAdjacency(
    Corridor& corridor,
    const std::shared_ptr<GPoint>& start,
    const std::shared_ptr<GPoint>& goal,
    const Corridor* previousCorridor)
{
    const auto& lvl = mLevels[0];

    // Track which vertices are inherited from the previous corridor
    int inheritedCount = 0;

    // If we have a previous corridor, carry forward its vertices and adjacency
    if (previousCorridor && previousCorridor->hasAdjacency)
    {
        // Copy all previous corridor vertices first
        for (int i = 0; i < previousCorridor->vertices.size(); ++i)
        {
            corridor.vertices.append(previousCorridor->vertices[i]);
            corridor.vertexIndex[previousCorridor->vertices[i]] = i;
        }
        inheritedCount = corridor.vertices.size();

        // Copy previous adjacency
        corridor.adjacency.resize(inheritedCount);
        for (int i = 0; i < inheritedCount; ++i)
        {
            corridor.adjacency[i] = previousCorridor->adjacency[i];
        }
    }

    // Add new corridor vertices (not already in the corridor)
    for (int idx : corridor.allowedVertexIndices)
    {
        if (idx < lvl.vertices.size())
        {
            const auto& v = lvl.vertices[idx];
            if (corridor.vertexIndex.find(v) == corridor.vertexIndex.end())
            {
                corridor.vertexIndex[v] = corridor.vertices.size();
                corridor.vertices.append(v);
            }
        }
    }

    // Add start and goal if not already present
    auto addIfMissing = [&](const std::shared_ptr<GPoint>& pt) {
        if (!pt) return;
        auto snapped = snapToWater(pt, 0);
        if (!snapped) return;
        if (corridor.vertexIndex.find(snapped) == corridor.vertexIndex.end())
        {
            corridor.vertexIndex[snapped] = corridor.vertices.size();
            corridor.vertices.append(snapped);
        }
    };
    addIfMissing(start);
    addIfMissing(goal);

    int n = corridor.vertices.size();
    corridor.adjacency.resize(n);

    if (n == 0)
    {
        corridor.hasAdjacency = true;
        return;
    }

    int newCount = n - inheritedCount;
    long long totalPairs = static_cast<long long>(n) * (n - 1) / 2;
    long long inheritedPairs = previousCorridor
        ? static_cast<long long>(inheritedCount) * (inheritedCount - 1) / 2
        : 0;

    qDebug() << "Pre-computing corridor adjacency for" << n
             << "vertices (" << newCount << " new,"
             << inheritedCount << " inherited,"
             << (totalPairs - inheritedPairs) << "new pairs)";

    QElapsedTimer timer;
    timer.start();

    // Only compute visibility for pairs involving at least one new vertex.
    // Old-old pairs are already in the inherited adjacency.
    QElapsedTimer progressTimer;
    progressTimer.start();
    int completedVertices = 0;
    qint64 lastProgressEmit = 0;
    int progressInterval = std::max(1, n / 100);

    for (int i = 0; i < n; ++i)
    {
        // For inherited vertices (i < inheritedCount), only check
        // against new vertices (j >= inheritedCount).
        // For new vertices, check against all j > i.
        int jStart = (i < inheritedCount) ? inheritedCount : (i + 1);

        for (int j = jStart; j < n; ++j)
        {
            if (isVisible(corridor.vertices[i], corridor.vertices[j], 0))
            {
                corridor.adjacency[i].push_back(j);
                corridor.adjacency[j].push_back(i);
            }
        }

        ++completedVertices;
        if (completedVertices % progressInterval == 0)
        {
            qint64 nowMs = progressTimer.elapsed();
            if (nowMs - lastProgressEmit >= 1000)
            {
                lastProgressEmit = nowMs;
                emit pathFindingProgress(-1, 0, nowMs / 1000.0);
            }
        }
    }

    corridor.hasAdjacency = true;

    qDebug() << "Corridor adjacency built in" << timer.elapsed() << "ms"
             << "(skipped" << inheritedPairs << "inherited pairs)";
}

// =============================================================================
// Visibility Checking
// =============================================================================

bool HierarchicalVisibilityGraph::isVisible(
    const std::shared_ptr<GPoint>& node1,
    const std::shared_ptr<GPoint>& node2,
    int level) const
{
    if (!node1 || !node2)
        throw std::runtime_error("Point is not valid!\n");

    if (*node1 == *node2)
        return true;

    // Use GSegment for fast haversine — no geodesic Inverse, no OGR
    GSegment seg(node1, node2);

    if (seg.approxLengthMeters() < 1.0)
        return true;

    return isSegmentVisibleImpl(seg, node1, node2, level);
}

bool HierarchicalVisibilityGraph::isSegmentVisible(
    const std::shared_ptr<GLine>& segment,
    int level) const
{
    if (!segment)
        return false;

    // Check manual lines
    if (!manualLinesSet.empty())
    {
        if (manualLinesSet.find(segment) != manualLinesSet.end())
            return true;
    }

    // Handle antimeridian crossing
    if (Quadtree::isSegmentCrossingAntimeridian(segment))
    {
        auto splitSegments =
            Quadtree::splitSegmentAtAntimeridian(segment);
        for (const auto& seg : splitSegments)
        {
            if (!isSegmentVisible(seg, level))
                return false;
        }
        return true;
    }

    GSegment seg(segment);
    return isSegmentVisibleImpl(seg, segment->startPoint(),
                                segment->endPoint(), level);
}

bool HierarchicalVisibilityGraph::isSegmentVisible(
    const GLine& segment,
    int level) const
{
    GSegment seg(segment);
    auto startPt = segment.startPoint();
    auto endPt = segment.endPoint();
    return isSegmentVisibleImpl(seg, startPt, endPt, level);
}

bool HierarchicalVisibilityGraph::isSegmentVisibleImpl(
    const GSegment& seg,
    const std::shared_ptr<GPoint>& startPt,
    const std::shared_ptr<GPoint>& endPt,
    int level) const
{
    const auto& lvl = mLevels[level];
    if (!lvl.quadtree)
        return false;

    if (seg.approxLengthMeters() < 1.0)
        return true;

    // Water polygon validation — needs GLine for polygon API compatibility
    QVector<std::shared_ptr<Polygon>> startPolygons =
        startPt->getOwningPolygons();
    QVector<std::shared_ptr<Polygon>> endPolygons =
        endPt->getOwningPolygons();

    if (startPolygons.isEmpty())
        startPolygons = findAllContainingPolygons(startPt);
    if (endPolygons.isEmpty())
        endPolygons = findAllContainingPolygons(endPt);

    // Find common polygon
    std::shared_ptr<Polygon> commonPolygon = nullptr;
    for (const auto& sp : startPolygons)
    {
        for (const auto& ep : endPolygons)
        {
            if (sp == ep)
            {
                commonPolygon = sp;
                break;
            }
        }
        if (commonPolygon) break;
    }

    if (commonPolygon)
    {
        // Build a fast GLine for polygon water-segment check
        auto segLine = std::make_shared<GLine>(startPt, endPt, FastConstruct);
        if (!commonPolygon->isValidWaterSegment(segLine))
            return false;

        // Verify midpoint is inside the water polygon — prevents
        // segments that exit the polygon through concavities in the
        // outer ring (e.g., crossing land between two coastal vertices
        // where no polygon edges block the path).
        double midLon = (startPt->getLongitude().value()
                         + endPt->getLongitude().value()) / 2.0;
        double midLat = (startPt->getLatitude().value()
                         + endPt->getLatitude().value()) / 2.0;
        GPoint midPoint{units::angle::degree_t{midLon},
                        units::angle::degree_t{midLat}};
        if (!commonPolygon->isPointWithinPolygon(midPoint))
            return false;
    }
    else
    {
        auto segLine = std::make_shared<GLine>(startPt, endPt, FastConstruct);
        for (const auto& polygon : lvl.polygons)
        {
            if (!polygon->segmentBoundsIntersect(segLine))
                continue;
            if (polygon->segmentCrossesHoles(segLine))
                return false;
        }
    }

    // Quadtree edge intersection check — zero-allocation visitor pattern
    double startLon = seg.startLon();
    double endLon = seg.endLon();
    double startLat = seg.startLat();
    double endLat = seg.endLat();
    double segMinLat = std::min(startLat, endLat);
    double segMaxLat = std::max(startLat, endLat);

    // Handle antimeridian split segments
    double segMinLon, segMaxLon;
    double lonDiff = std::abs(endLon - startLon);
    const double ANTI_TOL = 1e-6;
    bool startAtAnti = std::abs(std::abs(startLon) - 180.0) < ANTI_TOL;
    bool endAtAnti = std::abs(std::abs(endLon) - 180.0) < ANTI_TOL;
    bool isAntiSplit = (startAtAnti || endAtAnti) && lonDiff > 90.0;

    if (isAntiSplit)
    {
        double otherLon = startAtAnti ? endLon : startLon;
        if (otherLon < 0)
        {
            segMinLon = -180.0;
            segMaxLon = otherLon;
        }
        else
        {
            segMinLon = otherLon;
            segMaxLon = 180.0;
        }
    }
    else
    {
        segMinLon = std::min(startLon, endLon);
        segMaxLon = std::max(startLon, endLon);
    }

    bool blocked = lvl.quadtree->visitSegmentsAlongSegment(seg,
        [&](const GSegment& edge) -> bool {
            double eLon1 = edge.startLon(), eLat1 = edge.startLat();
            double eLon2 = edge.endLon(),   eLat2 = edge.endLat();

            if (std::abs(eLon1 - eLon2) > 180.0)
                return false;

            double edgeMinLon = std::min(eLon1, eLon2);
            double edgeMaxLon = std::max(eLon1, eLon2);
            double edgeMinLat = std::min(eLat1, eLat2);
            double edgeMaxLat = std::max(eLat1, eLat2);

            if (edgeMaxLon < segMinLon || edgeMinLon > segMaxLon
                || edgeMaxLat < segMinLat || edgeMinLat > segMaxLat)
                return false;

            const double COORD_TOL = 0.00001;
            auto coordsNear = [COORD_TOL](double lon1, double lat1,
                                           double lon2, double lat2) {
                return std::abs(lon1 - lon2) < COORD_TOL
                    && std::abs(lat1 - lat2) < COORD_TOL;
            };

            bool sharesEndpoint =
                coordsNear(eLon1, eLat1, startLon, startLat) ||
                coordsNear(eLon1, eLat1, endLon, endLat) ||
                coordsNear(eLon2, eLat2, startLon, startLat) ||
                coordsNear(eLon2, eLat2, endLon, endLat);

            if (sharesEndpoint)
                return false;

            auto pointOnEdgeFast = [&](double pLon, double pLat) {
                if (pLon < edgeMinLon - COORD_TOL || pLon > edgeMaxLon + COORD_TOL ||
                    pLat < edgeMinLat - COORD_TOL || pLat > edgeMaxLat + COORD_TOL)
                    return false;

                double dx = eLon2 - eLon1;
                double dy = eLat2 - eLat1;
                double dpx = pLon - eLon1;
                double dpy = pLat - eLat1;
                double cross = dx * dpy - dy * dpx;
                double lenSq = dx * dx + dy * dy;
                return (cross * cross) < (COORD_TOL * COORD_TOL * lenSq * 100.0);
            };

            if (pointOnEdgeFast(startLon, startLat) ||
                pointOnEdgeFast(endLon, endLat))
                return false;

            GSegment edgeSeg(eLon1, eLat1, eLon2, eLat2);
            return seg.intersects(edgeSeg, false);
        });
    if (blocked)
        return false;

    return true;
}

bool HierarchicalVisibilityGraph::isVisibleInSimplifiedPolygon(
    const std::shared_ptr<GPoint>& v1,
    const std::shared_ptr<GPoint>& v2,
    const std::shared_ptr<Polygon>& simplifiedPolygon) const
{
    if (!v1 || !v2 || !simplifiedPolygon)
        return false;

    if (*v1 == *v2)
        return true;

    // Edge crossing check (grid-accelerated, O(k) instead of O(n))
    if (simplifiedPolygon->segmentCrossesOuterRing(v1, v2))
        return false;

    // Midpoint-in-polygon check
    double midLon = (v1->getLongitude().value() + v2->getLongitude().value())
                    / 2.0;
    double midLat = (v1->getLatitude().value() + v2->getLatitude().value())
                    / 2.0;
    GPoint midPoint{units::angle::degree_t{midLon},
                    units::angle::degree_t{midLat}};
    if (!simplifiedPolygon->isPointWithinPolygon(midPoint))
        return false;

    return true;
}

// =============================================================================
// Containment
// =============================================================================

std::shared_ptr<Polygon>
HierarchicalVisibilityGraph::findContainingPolygon(
    const std::shared_ptr<GPoint>& point) const
{
    if (!point)
        return nullptr;

    // Use level 0 polygons (original)
    for (const auto& polygon : mLevels[0].polygons)
    {
        if (polygon->isPointWithinPolygon(*point))
            return polygon;
    }
    return nullptr;
}

QVector<std::shared_ptr<Polygon>>
HierarchicalVisibilityGraph::findAllContainingPolygons(
    const std::shared_ptr<GPoint>& point) const
{
    QVector<std::shared_ptr<Polygon>> result;
    if (!point)
        return result;

    double ptLon = point->getLongitude().value();
    double ptLat = point->getLatitude().value();

    for (const auto& polygon : mLevels[0].polygons)
    {
        double minLon, maxLon, minLat, maxLat;
        polygon->getEnvelope(minLon, maxLon, minLat, maxLat);

        if (ptLon < minLon || ptLon > maxLon ||
            ptLat < minLat || ptLat > maxLat)
            continue;

        if (polygon->isPointWithinPolygon(*point)
            || polygon->ringsContain(point))
        {
            result.append(polygon);
        }
    }

    return result;
}

// =============================================================================
// Neighbor Discovery
// =============================================================================

QVector<std::shared_ptr<GPoint>>
HierarchicalVisibilityGraph::getVisibleNodesForPoint(
    const std::shared_ptr<GPoint>& node,
    int level,
    const Corridor* corridor)
{
    QVector<std::shared_ptr<GPoint>> result;
    const auto& lvl = mLevels[level];

    // Fast path: use corridor's pre-computed adjacency (Level 0)
    if (corridor && corridor->hasAdjacency)
    {
        auto cit = corridor->vertexIndex.find(node);
        if (cit != corridor->vertexIndex.end())
        {
            int idx = cit->second;
            if (idx < static_cast<int>(corridor->adjacency.size()))
            {
                for (int neighborIdx : corridor->adjacency[idx])
                {
                    result.append(corridor->vertices[neighborIdx]);
                }
            }
            // Add manual connections and return
            QReadLocker locker(&mManualLock);
            auto mit = manualConnections.find(node);
            if (mit != manualConnections.end())
            {
                result += mit->second;
            }
            return result;
        }
        // Node not in corridor adjacency — fall through to on-demand
    }

    // Check if node is a known vertex with pre-computed adjacency.
    // For Level 0, only use adjacency when the atomic flag confirms the
    // build is complete — otherwise the background thread may be writing
    // to the same vectors (data race -> crash).
    auto it = lvl.vertexIndex.find(node);
    bool adjReady = !lvl.adjacency.empty()
                    && (level > 0
                        || mLevel0AdjReady.load(std::memory_order_acquire));
    if (it != lvl.vertexIndex.end() && adjReady)
    {
        int idx = it->second;
        if (idx < static_cast<int>(lvl.adjacency.size()))
        {
            for (int neighborIdx : lvl.adjacency[idx])
            {
                if (corridor &&
                    corridor->allowedVertexIndices.find(neighborIdx) ==
                        corridor->allowedVertexIndices.end())
                {
                    continue;
                }
                result.append(lvl.vertices[neighborIdx]);
            }
        }
    }
    else
    {
        // On-demand visibility (start/goal or unconstrained Level 0)
        auto containingPolygons = findAllContainingPolygons(node);

        if (containingPolygons.isEmpty())
        {
            result = getVisibleNodesBetweenPolygons(node, lvl.polygons);
        }
        else
        {
            for (const auto& poly : containingPolygons)
            {
                auto nodesInPoly = getVisibleNodesWithinPolygon(node, poly);
                for (const auto& n : nodesInPoly)
                {
                    if (corridor)
                    {
                        double lon = n->getLongitude().value();
                        double lat = n->getLatitude().value();
                        if (!corridor->containsPoint(lon, lat))
                            continue;
                    }
                    result.append(n);
                }
            }
        }
    }

    // Add manual connections
    QReadLocker locker(&mManualLock);
    auto mit = manualConnections.find(node);
    if (mit != manualConnections.end())
    {
        result += mit->second;
    }

    return result;
}

QVector<std::shared_ptr<GPoint>>
HierarchicalVisibilityGraph::getVisibleNodesWithinPolygon(
    const std::shared_ptr<GPoint>& node,
    const std::shared_ptr<Polygon>& polygon)
{
    // Check GPoint visibility cache first
    if (node->hasVisibleNeighborsCache(polygon.get()))
    {
        return node->getVisibleNeighborsInPolygon(polygon.get());
    }

    QVector<std::shared_ptr<GPoint>> candidates;

    const auto& outerPoints = polygon->outer();
    std::copy_if(outerPoints.begin(), outerPoints.end(),
                 std::back_inserter(candidates),
                 [&node](const auto& p) { return *p != *node; });

    for (const auto& hole : polygon->inners())
    {
        std::copy_if(hole.begin(), hole.end(),
                     std::back_inserter(candidates),
                     [&node](const auto& p) { return *p != *node; });
    }

    // Distance-sorted visibility with caps: check 500 nearest, stop at 20
    auto visibleNodes = findNearestVisibleNodes(node, candidates, 0,
                                                500, 20);

    // Add manual connections
    {
        QReadLocker locker(&mManualLock);
        auto it = manualConnections.find(node);
        if (it != manualConnections.end())
        {
            visibleNodes += it->second;
        }
    }

    node->setVisibleNeighborsInPolygon(polygon.get(), visibleNodes);
    return visibleNodes;
}

QVector<std::shared_ptr<GPoint>>
HierarchicalVisibilityGraph::getVisibleNodesBetweenPolygons(
    const std::shared_ptr<GPoint>& node,
    const QVector<std::shared_ptr<Polygon>>& allPolygons)
{
    QVector<std::shared_ptr<GPoint>> tasks;

    double nodeLon = node->getLongitude().value();
    double nodeLat = node->getLatitude().value();

    for (const auto& polygon : allPolygons)
    {
        double minLon, maxLon, minLat, maxLat;
        polygon->getEnvelope(minLon, maxLon, minLat, maxLat);

        if (nodeLon < minLon || nodeLon > maxLon ||
            nodeLat < minLat || nodeLat > maxLat)
            continue;

        bool isPartOfPolygon = polygon->ringsContain(node)
                               || polygon->isPointWithinPolygon(*node);
        if (!isPartOfPolygon)
            continue;

        const auto outerPoints = polygon->outer();
        std::copy_if(outerPoints.begin(), outerPoints.end(),
                     std::back_inserter(tasks),
                     [&node](const auto& p) { return *p != *node; });

        for (const auto& hole : polygon->inners())
        {
            std::copy_if(hole.begin(), hole.end(),
                         std::back_inserter(tasks),
                         [&node](const auto& p) { return *p != *node; });
        }
    }

    // Distance-sorted visibility with caps: check 100 nearest, stop at 10
    auto visibleNodes = findNearestVisibleNodes(node, tasks, 0, 500, 20);

    QReadLocker locker(&mManualLock);
    auto it = manualConnections.find(node);
    if (it != manualConnections.end())
    {
        visibleNodes += it->second;
    }

    return visibleNodes;
}

// =============================================================================
// Path Reconstruction
// =============================================================================

ShortestPathResult HierarchicalVisibilityGraph::reconstructPath(
    const std::unordered_map<
        std::shared_ptr<GPoint>, std::shared_ptr<GPoint>,
        GPoint::Hash, GPoint::Equal>& cameFrom,
    std::shared_ptr<GPoint> current,
    int level)
{
    ShortestPathResult result;
    const auto& lvl = mLevels[level];

    // Build path in reverse (append is O(1)), then reverse at the end
    // instead of prepend which is O(n) per call.
    while (cameFrom.find(current) != cameFrom.end())
    {
        result.points.append(current);
        std::shared_ptr<GPoint> next = cameFrom.at(current);

        std::shared_ptr<GLine> lineSegment;
        if (lvl.quadtree)
            lineSegment = lvl.quadtree->findLineSegment(next, current);

        if (lineSegment
            && *lineSegment->startPoint() == *next
            && *lineSegment->endPoint() == *current)
        {
            // Cached line matches expected direction (next → current)
            result.lines.append(lineSegment);
        }
        else
        {
            // No cached line or wrong direction — create correct one
            result.lines.append(
                std::make_shared<GLine>(next, current));
        }
        current = next;
    }
    result.points.append(current);

    // Reverse both to get correct order — O(n) total
    std::reverse(result.points.begin(), result.points.end());
    std::reverse(result.lines.begin(), result.lines.end());

    return result;
}

// =============================================================================
// Multi-Segment Helper
// =============================================================================

ShortestPathResult HierarchicalVisibilityGraph::findShortestPathHelper(
    QVector<std::shared_ptr<GPoint>> mustTraversePoints)
{
    ShortestPathResult result;

    if (mustTraversePoints.size() < 2)
    {
        if (!mustTraversePoints.isEmpty() && mustTraversePoints.first())
            result.points.append(mustTraversePoints.first());
        return result;
    }

    for (const auto& point : mustTraversePoints)
    {
        if (!point)
        {
            qWarning() << "Null point in path points list";
            return result;
        }
    }

    QElapsedTimer progressTimer;
    progressTimer.start();
    int totalSegments = mustTraversePoints.size() - 1;

    emit pathFindingProgress(0, totalSegments,
                             progressTimer.elapsed() / 1000.0);

    result.points.append(mustTraversePoints.first());

    for (qsizetype i = 0; i < mustTraversePoints.size() - 1; ++i)
    {
        auto startPoint = mustTraversePoints[i];
        auto endPoint = mustTraversePoints[i + 1];

        try
        {
            auto l = std::make_shared<GLine>(startPoint, endPoint);

            if (isSegmentVisible(l, 0))
            {
                // Deduplicate at segment boundaries: only check last point
                // since waypoints are processed sequentially
                if (result.points.isEmpty() ||
                    *(result.points.last()) != *startPoint)
                {
                    result.points.push_back(startPoint);
                }

                if (result.points.isEmpty() ||
                    *(result.points.last()) != *endPoint)
                {
                    result.points.push_back(endPoint);
                }

                result.lines.push_back(l);
            }
            else
            {
                ShortestPathResult twoPointResult =
                    hierarchicalSearch(startPoint, endPoint);

                for (qsizetype j = 1;
                     j < twoPointResult.points.size(); ++j)
                {
                    result.points.append(twoPointResult.points[j]);
                }

                for (auto& line : twoPointResult.lines)
                {
                    result.lines.append(line);
                }
            }
        }
        catch (const std::exception& e)
        {
            qWarning() << "Exception creating line segment:" << e.what();
            continue;
        }

        emit pathFindingProgress(static_cast<int>(i + 1), totalSegments,
                                 progressTimer.elapsed() / 1000.0);
    }

    return result;
}

// =============================================================================
// Wrap-Around / Antimeridian
// =============================================================================

bool HierarchicalVisibilityGraph::shouldCrossAntimeridian(
    double startLon, double goalLon)
{
    double directDiff = std::abs(goalLon - startLon);
    return directDiff > 180.0;
}

QVector<std::shared_ptr<GPoint>>
HierarchicalVisibilityGraph::connectWrapAroundPoints(
    const std::shared_ptr<GPoint>& point,
    const std::shared_ptr<GPoint>& goalPoint)
{
    QVector<std::shared_ptr<GPoint>> wrapAroundPoints;

    const auto& lvl = mLevels[0];
    GPoint mapMin, mapMax;
    double mapWidth;
    bool nearBoundary = false;

    {
        QReadLocker locker(&lvl.lock);
        if (!lvl.quadtree)
            return wrapAroundPoints;

        nearBoundary = lvl.quadtree->isNearBoundary(point);
        mapMin = lvl.quadtree->getMapMinPoint();
        mapMax = lvl.quadtree->getMapMaxPoint();
        mapWidth = (mapMax.getLongitude() - mapMin.getLongitude()).value();
    }

    double pointLon = point->getLongitude().value();

    // Goal-aware antimeridian crossing
    if (goalPoint)
    {
        double goalLon = goalPoint->getLongitude().value();

        if (shouldCrossAntimeridian(pointLon, goalLon))
        {
            double targetLon = (pointLon > 0) ? 180.0 : -180.0;
            double zoneLon = (targetLon > 0)
                                 ? 180.0 - PORTAL_ZONE_DEGREES
                                 : -180.0;
            double pointLat = point->getLatitude().value();
            double latRange = PORTAL_LAT_TOLERANCE * 2;
            QRectF portalZone(zoneLon, pointLat - latRange,
                              PORTAL_ZONE_DEGREES, latRange * 2);

            auto portalVertices = lvl.quadtree->findVerticesInRange(portalZone);

            for (const auto& portalVertex : portalVertices)
            {
                if (isVisible(point, portalVertex, 0))
                {
                    if (!wrapAroundPoints.contains(portalVertex))
                        wrapAroundPoints.append(portalVertex);
                }
            }
        }
    }

    if (!nearBoundary)
        return wrapAroundPoints;

    auto createMirrorPoint = [&](double offset) {
        double newLon = pointLon + offset;
        return std::make_shared<GPoint>(
            units::angle::degree_t(newLon), point->getLatitude());
    };

    QVector<std::shared_ptr<GPoint>> mirrorPoints;

    if ((mapMax.getLongitude() - point->getLongitude()).value() < 1.0)
    {
        mirrorPoints.append(createMirrorPoint(-mapWidth));
    }
    else if ((point->getLongitude() - mapMin.getLongitude()).value() < 1.0)
    {
        mirrorPoints.append(createMirrorPoint(mapWidth));
    }

    QVector<std::shared_ptr<GPoint>> allVisible;
    for (const auto& wrappedPoint : mirrorPoints)
    {
        QVector<std::shared_ptr<GPoint>> wrappedVisible;
        if (auto poly = findContainingPolygon(point))
        {
            wrappedVisible = getVisibleNodesWithinPolygon(wrappedPoint, poly);
        }
        else
        {
            wrappedVisible = getVisibleNodesBetweenPolygons(
                wrappedPoint, lvl.polygons);
        }

        for (auto& p : wrappedVisible)
        {
            double adjustedLon = p->getLongitude().value();
            if (adjustedLon > 180)
                adjustedLon -= 360;
            else if (adjustedLon < -180)
                adjustedLon += 360;

            allVisible.append(std::make_shared<GPoint>(
                units::angle::degree_t(adjustedLon),
                p->getLatitude()));
        }
    }

    for (const auto& candidate : allVisible)
    {
        auto wrapSegment = std::make_shared<GLine>(point, candidate);
        if (isSegmentVisible(wrapSegment, 0))
        {
            if (!wrapAroundPoints.contains(candidate))
                wrapAroundPoints.append(candidate);
        }
    }

    return wrapAroundPoints;
}

// =============================================================================
// Manual Lines, SeaPorts, Clear, SetPolygons
// =============================================================================

void HierarchicalVisibilityGraph::addManualVisibleLine(
    const std::shared_ptr<GLine>& line)
{
    QWriteLocker locker(&mManualLock);

    manualLinesSet.insert(line);
    manualConnections[line->startPoint()].append(line->endPoint());
    manualConnections[line->endPoint()].append(line->startPoint());

    if (manualPointsSet.insert(line->startPoint()).second)
        manualPoints.append(line->startPoint());
    if (manualPointsSet.insert(line->endPoint()).second)
        manualPoints.append(line->endPoint());
}

void HierarchicalVisibilityGraph::clearManualLines()
{
    QWriteLocker locker(&mManualLock);
    manualLinesSet.clear();
    manualConnections.clear();
    manualPoints.clear();
    manualPointsSet.clear();
}

void HierarchicalVisibilityGraph::loadSeaPortsPolygonCoordinates(
    QVector<std::shared_ptr<SeaPort>>& seaPorts)
{
    const auto& lvl = mLevels[0];
    if (!lvl.quadtree)
    {
        qWarning() << "loadSeaPortsPolygonCoordinates: Quadtree not initialized";
        return;
    }
    for (auto& seaPort : seaPorts)
    {
        std::shared_ptr<GPoint> portCoord =
            std::make_shared<GPoint>(seaPort->getPortCoordinate());
        seaPort->setClosestPointOnWaterPolygon(
            lvl.quadtree->findNearestNeighborPoint(portCoord));
    }
}

GPoint HierarchicalVisibilityGraph::getMinMapPoint()
{
    const auto& lvl = mLevels[0];
    if (!lvl.quadtree)
    {
        qWarning() << "getMinMapPoint: Quadtree not initialized";
        return GPoint();
    }
    return lvl.quadtree->getMapMinPoint();
}

GPoint HierarchicalVisibilityGraph::getMaxMapPoint()
{
    const auto& lvl = mLevels[0];
    if (!lvl.quadtree)
    {
        qWarning() << "getMaxMapPoint: Quadtree not initialized";
        return GPoint();
    }
    return lvl.quadtree->getMapMaxPoint();
}

void HierarchicalVisibilityGraph::clear()
{
    mLevel0AdjReady.store(false, std::memory_order_release);

    // Clear vertex ownership and visibility cache for ALL levels
    // (includes simplified vertices at L1-L3 that have ownership set)
    for (int i = 0; i < NUM_LEVELS; ++i)
    {
        for (const auto& vertex : mLevels[i].vertices)
        {
            if (vertex)
            {
                vertex->clearOwningPolygons();
                vertex->clearVisibleNeighborsCache();
            }
        }
    }
    polygons.clear();

    // Clear polygon graph
    mPolygonGraph.adjacency.clear();
    mPolygonGraph.representatives.clear();
    mPolygonGraph.numPolygons = 0;

    // Clear manual lines
    {
        QWriteLocker locker(&mManualLock);
        manualLinesSet.clear();
        manualConnections.clear();
        manualPoints.clear();
        manualPointsSet.clear();
    }

    // Clear all levels
    for (int i = 0; i < NUM_LEVELS; ++i)
    {
        auto& lvl = mLevels[i];
        QWriteLocker locker(&lvl.lock);
        if (lvl.quadtree)
            lvl.quadtree->clearTree();
        lvl.polygons.clear();
        lvl.vertices.clear();
        lvl.adjacency.clear();
        lvl.vertexIndex.clear();
        lvl.vertexPolygonId.clear();
        lvl.outerRings.clear();
        lvl.holeRings.clear();
    }
}

void HierarchicalVisibilityGraph::setPolygons(
    const QVector<std::shared_ptr<Polygon>>& newPolygons)
{
    mLevel0AdjReady.store(false, std::memory_order_release);

    // Clear ownership from ALL level vertices (including simplified)
    for (int i = 0; i < NUM_LEVELS; ++i)
    {
        for (const auto& vertex : mLevels[i].vertices)
        {
            if (vertex)
            {
                vertex->clearOwningPolygons();
                vertex->clearVisibleNeighborsCache();
            }
        }
    }

    // Clear polygon graph
    mPolygonGraph.adjacency.clear();
    mPolygonGraph.representatives.clear();
    mPolygonGraph.numPolygons = 0;

    polygons = newPolygons;

    // Set ownership on new L0 vertices
    for (const auto& polygon : polygons)
    {
        if (!polygon) continue;
        for (const auto& vertex : polygon->outer())
        {
            if (vertex)
                vertex->addOwningPolygon(polygon);
        }
        for (const auto& hole : polygon->inners())
        {
            for (const auto& vertex : hole)
            {
                if (vertex)
                    vertex->addOwningPolygon(polygon);
            }
        }
    }

    // Rebuild all levels
    for (int i = 0; i < NUM_LEVELS; ++i)
    {
        auto& lvl = mLevels[i];
        QWriteLocker locker(&lvl.lock);
        if (lvl.quadtree)
            lvl.quadtree->clearTree();
        lvl.polygons.clear();
        lvl.vertices.clear();
        lvl.adjacency.clear();
        lvl.vertexIndex.clear();
        lvl.vertexPolygonId.clear();
        lvl.outerRings.clear();
        lvl.holeRings.clear();
    }

    buildAllLevels();
}

// =============================================================================
// Level 0 Adjacency Cache
// =============================================================================

void HierarchicalVisibilityGraph::buildLevel0Adjacency()
{
    buildAdjacencyForLevel(0);
    mLevel0AdjReady.store(true, std::memory_order_release);
}

void HierarchicalVisibilityGraph::buildLevel0AdjacencyAsync(
    const QString& cachePath)
{
    mLevel0CachePath = cachePath;
    buildAdjacencyForLevel(0);
    mLevel0AdjReady.store(true, std::memory_order_release);
    if (!mLevel0CachePath.isEmpty())
        saveAdjacencyCache(mLevel0CachePath);
}

bool HierarchicalVisibilityGraph::saveAdjacencyCache(
    const QString& filePath) const
{
    const auto& lvl = mLevels[0];
    int n = lvl.vertices.size();

    if (n == 0 || lvl.adjacency.empty())
    {
        qWarning() << "saveAdjacencyCache: No Level 0 adjacency to save";
        return false;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly))
    {
        qWarning() << "saveAdjacencyCache: Cannot open" << filePath;
        return false;
    }

    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);
    out.setFloatingPointPrecision(QDataStream::DoublePrecision);

    // Magic bytes "HVG0"
    out.writeRawData("HVG0", 4);

    // Version
    qint32 version = 1;
    out << version;

    // Vertex count
    qint32 vertexCount = static_cast<qint32>(n);
    out << vertexCount;

    // SHA-256 of vertex coordinates
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (int i = 0; i < n; ++i)
    {
        double lon = lvl.vertices[i]->getLongitude().value();
        double lat = lvl.vertices[i]->getLatitude().value();
        hash.addData(reinterpret_cast<const char*>(&lon), sizeof(double));
        hash.addData(reinterpret_cast<const char*>(&lat), sizeof(double));
    }
    QByteArray coordHash = hash.result();
    out.writeRawData(coordHash.constData(), 32);

    // Write adjacency lists
    for (int i = 0; i < n; ++i)
    {
        qint32 neighborCount = static_cast<qint32>(lvl.adjacency[i].size());
        out << neighborCount;
        for (int neighbor : lvl.adjacency[i])
        {
            qint32 idx = static_cast<qint32>(neighbor);
            out << idx;
        }
    }

    file.close();
    qDebug() << "Saved Level 0 adjacency cache to" << filePath
             << "(" << file.size() << "bytes)";
    return true;
}

bool HierarchicalVisibilityGraph::loadAdjacencyCache(
    const QString& filePath)
{
    auto& lvl = mLevels[0];
    int n = lvl.vertices.size();

    if (n == 0)
    {
        qWarning() << "loadAdjacencyCache: No Level 0 vertices";
        return false;
    }

    QFile file(filePath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
    {
        qDebug() << "loadAdjacencyCache: Cache file not found:" << filePath;
        return false;
    }

    QDataStream in(&file);
    in.setByteOrder(QDataStream::LittleEndian);
    in.setFloatingPointPrecision(QDataStream::DoublePrecision);

    // Validate magic
    char magic[4];
    if (in.readRawData(magic, 4) != 4 ||
        memcmp(magic, "HVG0", 4) != 0)
    {
        qWarning() << "loadAdjacencyCache: Invalid magic bytes";
        return false;
    }

    // Validate version
    qint32 version;
    in >> version;
    if (version != 1)
    {
        qWarning() << "loadAdjacencyCache: Unsupported version" << version;
        return false;
    }

    // Validate vertex count
    qint32 vertexCount;
    in >> vertexCount;
    if (vertexCount != static_cast<qint32>(n))
    {
        qDebug() << "loadAdjacencyCache: Vertex count mismatch"
                 << "(cache:" << vertexCount << "current:" << n << ")";
        return false;
    }

    // Validate coordinate hash
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (int i = 0; i < n; ++i)
    {
        double lon = lvl.vertices[i]->getLongitude().value();
        double lat = lvl.vertices[i]->getLatitude().value();
        hash.addData(reinterpret_cast<const char*>(&lon), sizeof(double));
        hash.addData(reinterpret_cast<const char*>(&lat), sizeof(double));
    }
    QByteArray expectedHash = hash.result();

    char storedHash[32];
    if (in.readRawData(storedHash, 32) != 32 ||
        memcmp(storedHash, expectedHash.constData(), 32) != 0)
    {
        qDebug() << "loadAdjacencyCache: Coordinate hash mismatch "
                    "(shapefile changed)";
        return false;
    }

    // Read adjacency lists
    lvl.adjacency.resize(n);
    for (int i = 0; i < n; ++i)
    {
        qint32 neighborCount;
        in >> neighborCount;
        if (neighborCount < 0 || neighborCount > n)
        {
            qWarning() << "loadAdjacencyCache: Invalid neighbor count"
                       << neighborCount << "at vertex" << i;
            lvl.adjacency.clear();
            return false;
        }
        lvl.adjacency[i].resize(neighborCount);
        for (int j = 0; j < neighborCount; ++j)
        {
            qint32 idx;
            in >> idx;
            if (idx < 0 || idx >= n)
            {
                qWarning() << "loadAdjacencyCache: Invalid neighbor index"
                           << idx << "at vertex" << i;
                lvl.adjacency.clear();
                return false;
            }
            lvl.adjacency[i][j] = idx;
        }
    }

    file.close();
    mLevel0AdjReady.store(true, std::memory_order_release);
    qDebug() << "Loaded Level 0 adjacency cache from" << filePath
             << "(" << n << "vertices)";
    return true;
}

// =============================================================================
// Public API
// =============================================================================

ShortestPathResult HierarchicalVisibilityGraph::findShortestPath(
    const std::shared_ptr<GPoint>& start,
    const std::shared_ptr<GPoint>& goal)
{
    // Direct visibility check — mirrors the shortcut in
    // findShortestPathHelper (multi-waypoint API).
    // If start and goal can see each other directly, skip hierarchical search.
    if (start && goal && !(*start == *goal)) {
        auto directLine = std::make_shared<GLine>(start, goal);
        if (isSegmentVisible(directLine, 0)) {
            ShortestPathResult result;
            result.points.append(start);
            result.points.append(goal);
            result.lines.append(directLine);
            return result;
        }
    }
    return hierarchicalSearch(start, goal);
}

ShortestPathResult HierarchicalVisibilityGraph::findShortestPath(
    QVector<std::shared_ptr<GPoint>> mustTraversePoints)
{
    return findShortestPathHelper(mustTraversePoints);
}

Quadtree* HierarchicalVisibilityGraph::getLevel0Quadtree() const
{
    return mLevels[0].quadtree.get();
}

}; // namespace ShipNetSimCore
