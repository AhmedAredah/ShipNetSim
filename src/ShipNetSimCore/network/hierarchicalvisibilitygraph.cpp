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

    computeDynamicParameters();
    buildAllLevels();
}

// =============================================================================
// Dynamic Level Parameters
// =============================================================================

void HierarchicalVisibilityGraph::computeDynamicParameters()
{
    // Compute average outer ring edge length as data resolution metric.
    // Only outer rings are sampled — holes are typically smaller and
    // would bias the estimate downward.
    double totalLength = 0.0;
    int edgeCount = 0;

    for (const auto& poly : polygons)
    {
        if (!poly) continue;
        const auto& outer = poly->outer();
        int sz = outer.size();
        if (sz < 2) continue;

        for (int i = 0; i < sz; ++i)
        {
            const auto& a = outer[i];
            const auto& b = outer[(i + 1) % sz];
            if (!a || !b) continue;
            totalLength += GSegment::haversineRaw(
                a->getLongitude().value(), a->getLatitude().value(),
                b->getLongitude().value(), b->getLatitude().value());
            ++edgeCount;
        }
    }

    // Floor at 1km to prevent degenerate parameters
    mAvgSpacing = (edgeCount > 0)
        ? std::max(1000.0, totalLength / edgeCount)
        : 10000.0;  // default to ne_10m-like resolution

    for (int i = 0; i < NUM_LEVELS; ++i)
    {
        mLevelTolerances[i] = TOLERANCE_FACTORS[i] * mAvgSpacing;
        mLevelMaxDistance[i] = LEVEL_MAX_DISTANCE[i];
    }

    qDebug() << "Dynamic level parameters: avgSpacing ="
             << mAvgSpacing / 1000.0 << "km";
    for (int i = 0; i < NUM_LEVELS; ++i)
    {
        qDebug() << "  L" << i
                 << ": tolerance =" << mLevelTolerances[i] / 1000.0 << "km"
                 << ", maxDist =" << mLevelMaxDistance[i] / 1000.0 << "km";
    }
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
    level.toleranceMeters = mLevelTolerances[idx];

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
    level.vertexHoleId.reserve(estimatedVertices);
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
            level.vertexHoleId.push_back(-1);  // outer ring

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
        for (int hIdx = 0; hIdx < holes.size(); ++hIdx)
        {
            const auto& hole = holes[hIdx];
            int holeStart = vertexIdx;
            int holeCount = 0;
            for (const auto& vertex : hole)
            {
                if (!vertex) continue;
                level.vertices.append(vertex);
                level.vertexIndex[vertex] = vertexIdx;
                level.vertexPolygonId.push_back(pi);
                level.vertexHoleId.push_back(hIdx);

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

// (LEVEL_MAX_DISTANCE removed — replaced by dynamic mLevelMaxDistance member)

void HierarchicalVisibilityGraph::buildAdjacencyForLevel(int idx)
{
    auto& level = mLevels[idx];
    int n = level.vertices.size();
    level.adjacency.resize(n);

    if (n == 0) return;

    QElapsedTimer timer;
    timer.start();

    // Level 0: data-adaptive phased adjacency.
    //   Phase 1: boundary edges — O(n), ring connectivity
    //   Phase 2: local all-visible — O(n×k), avgSpacing × multiplier
    //   Phase 3: Yao visibility spanner — O(n×k×log n), no distance limit
    //   Phase 4: Borůvka bridging — O(C×R×log n), guaranteed connectivity
    // Level 0: ring-aware phased adjacency.
    //   Phase 1: boundary edges — O(n), ring connectivity
    //   Phase 2: intra-ring express — O(S×k), highways on large rings
    //   Phase 3: inter-ring bridges — O(P×k), cross-coastline connections
    //   Phase 4: Borůvka bridging — O(C×R×k), connectivity guarantee
    //   Phase 5: antimeridian edges — cross-±180°
    if (idx == 0)
    {
        // Phase 1: boundary edges — guarantees ring connectivity
        addBoundaryEdges(level);
        qInfo().noquote()
            << QString("  Phase 1 (boundary): done in %1ms")
                   .arg(timer.elapsed());

        // Phase 2: intra-ring express edges on large rings
        addIntraRingExpressEdges(level, 0);

        // Phase 3: inter-ring bridges between nearby ring pairs
        addInterRingBridges(level, 0);

        // Phase 4: Borůvka bridging — ensure single connected component
        // (antimeridian wrapping is handled generically in connectOctantNearest
        //  via wrapped Quadtree queries + wrapped bearing computation)
        bridgeConnectedComponents(level, 0);

        long long totalEdges = 0;
        for (const auto& adj : level.adjacency)
            totalEdges += adj.size();
        qInfo().noquote()
            << QString("L0 adjacency complete: %1 directed edges in %2s")
                   .arg(totalEdges)
                   .arg(timer.elapsed() / 1000.0, 0, 'f', 1);
        return;
    }

    // Levels 1-3: phased approach (fast, skip-vis for corridor guidance)
    double maxDist = mLevelMaxDistance[idx];

    qDebug() << "Building adjacency for level" << idx
             << "with" << n << "vertices (phased, maxDist="
             << maxDist / 1000.0 << "km)";

    // Phase 1: boundary edges — O(n), no visibility check
    addBoundaryEdges(level);
    qDebug() << "  Phase 1 (boundary edges) done in"
             << timer.elapsed() << "ms";

    // Phase 2: spatial grid edges — O(n × k), with visibility checking
    // to ensure coarse-level edges don't cross land (correct corridor guidance)
    if (maxDist > 0.0)
    {
        addSpatialGridEdges(level, idx, maxDist, true);
        qDebug() << "  Phase 2 (spatial grid, vis-checked) done in"
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
// Shared Helper: Octant-Nearest Expanding Search
// =============================================================================

std::optional<std::pair<double, double>>
HierarchicalVisibilityGraph::computeWaterArc(
    int vertexIdx, int level,
    const std::vector<double>& lons,
    const std::vector<double>& lats) const
{
    auto [prevIdx, nextIdx] = getRingNeighbors(vertexIdx, level);
    if (prevIdx < 0) return std::nullopt;

    double vLon = lons[vertexIdx];
    double vLat = lats[vertexIdx];

    // Wrap longitude differences for correct antimeridian bearing
    double dLonPrev = lons[prevIdx] - vLon;
    if (dLonPrev >  180.0) dLonPrev -= 360.0;
    if (dLonPrev < -180.0) dLonPrev += 360.0;
    double dLonNext = lons[nextIdx] - vLon;
    if (dLonNext >  180.0) dLonNext -= 360.0;
    if (dLonNext < -180.0) dLonNext += 360.0;

    double bPrev = std::atan2(dLonPrev, lats[prevIdx] - vLat);
    double bNext = std::atan2(dLonNext, lats[nextIdx] - vLat);

    // Normalize diff to [-π, π]
    double diff = bNext - bPrev;
    while (diff >  M_PI) diff -= 2.0 * M_PI;
    while (diff < -M_PI) diff += 2.0 * M_PI;

    double bis1 = bPrev + diff / 2.0;
    while (bis1 >  M_PI) bis1 -= 2.0 * M_PI;
    while (bis1 < -M_PI) bis1 += 2.0 * M_PI;

    // Test which bisector points toward water
    const auto& lvl = mLevels[level];
    double cosLat = std::max(std::cos(vLat * M_PI / 180.0), 0.01);
    double testDeg = mAvgSpacing / (METERS_PER_DEGREE_LAT * 3.0);
    double testLon = vLon + std::sin(bis1) * testDeg / cosLat;
    double testLat = vLat + std::cos(bis1) * testDeg;
    GPoint testPt{units::angle::degree_t{testLon},
                  units::angle::degree_t{testLat}};

    int polyId = lvl.vertexPolygonId[vertexIdx];
    bool bis1IsWater = (polyId >= 0
        && polyId < lvl.polygons.size()
        && lvl.polygons[polyId]->isPointWithinPolygon(testPt));

    double waterCenter, waterHalfArc;
    if (bis1IsWater)
    {
        waterCenter  = bis1;
        waterHalfArc = std::abs(diff) / 2.0;
    }
    else
    {
        waterCenter = bis1 + M_PI;
        while (waterCenter >  M_PI) waterCenter -= 2.0 * M_PI;
        waterHalfArc = M_PI - std::abs(diff) / 2.0;
    }

    if (waterHalfArc < 0.01) return std::nullopt;

    return std::make_pair(waterCenter, waterHalfArc);
}

int HierarchicalVisibilityGraph::connectOctantNearest(
    GraphLevel& level, int levelIdx,
    int sourceIdx,
    const std::vector<double>& lons,
    const std::vector<double>& lats,
    double initRadiusDeg, double maxSearchDeg,
    const std::function<bool(int)>& candidateFilter,
    std::optional<std::pair<double, double>> waterArc)
{
    if (!level.quadtree) return 0;

    double vLon = lons[sourceIdx];
    double vLat = lats[sourceIdx];
    double coneWidth = 2.0 * M_PI / YAO_CONE_COUNT;
    int edgesAdded = 0;

    // Helper: wrap longitude difference to [-180, 180] for antimeridian
    auto wrapDLon = [](double dLon) -> double {
        if (dLon >  180.0) dLon -= 360.0;
        if (dLon < -180.0) dLon += 360.0;
        return dLon;
    };

    // Determine which octants already have neighbors (skip-covered)
    std::array<bool, YAO_CONE_COUNT> covered = {};
    for (int nb : level.adjacency[sourceIdx])
    {
        double dLon = wrapDLon(lons[nb] - vLon);
        double bearing = std::atan2(dLon, lats[nb] - vLat);
        if (bearing < 0) bearing += 2.0 * M_PI;
        covered[static_cast<int>(bearing / coneWidth) % YAO_CONE_COUNT] = true;
    }

    // If water arc is specified, also mark land-facing cones as covered
    if (waterArc.has_value())
    {
        double wc = waterArc->first;
        double whw = waterArc->second;
        for (int cone = 0; cone < YAO_CONE_COUNT; ++cone)
        {
            if (covered[cone]) continue;
            double coneMid = (cone + 0.5) * coneWidth;
            // Normalize to [-π, π] relative to water center
            double d = coneMid - wc;
            while (d >  M_PI) d -= 2.0 * M_PI;
            while (d < -M_PI) d += 2.0 * M_PI;
            if (std::abs(d) > whw)
                covered[cone] = true;  // land-facing → skip
        }
    }

    for (int cone = 0; cone < YAO_CONE_COUNT; ++cone)
    {
        if (covered[cone]) continue;

        double coneMin = cone * coneWidth;
        double coneMax = coneMin + coneWidth;
        double radiusDeg = initRadiusDeg;
        bool found = false;

        while (!found && radiusDeg <= maxSearchDeg)
        {
            double cosLat = std::max(
                std::cos(vLat * M_PI / 180.0), 0.01);
            double lonR = radiusDeg / cosLat;

            QRectF bbox(vLon - lonR, vLat - radiusDeg,
                        2.0 * lonR, 2.0 * radiusDeg);
            auto candidates =
                level.quadtree->findVerticesInRange(bbox);

            // Antimeridian wrapping: if bbox extends past ±180°,
            // query the wrapped portion on the other side
            if (vLon + lonR > 180.0)
            {
                double w = vLon + lonR - 180.0;
                QRectF wrapped(-180.0, vLat - radiusDeg,
                               w, 2.0 * radiusDeg);
                candidates += level.quadtree->findVerticesInRange(
                    wrapped);
            }
            if (vLon - lonR < -180.0)
            {
                double w = -180.0 - (vLon - lonR);
                QRectF wrapped(180.0 - w, vLat - radiusDeg,
                               w, 2.0 * radiusDeg);
                candidates += level.quadtree->findVerticesInRange(
                    wrapped);
            }

            double bestDist = std::numeric_limits<double>::max();
            int bestIdx = -1;

            for (const auto& c : candidates)
            {
                auto it = level.vertexIndex.find(c);
                if (it == level.vertexIndex.end()) continue;
                int j = it->second;
                if (j == sourceIdx) continue;
                if (!candidateFilter(j)) continue;

                // Wrapped bearing for correct antimeridian octant
                double dLon = wrapDLon(lons[j] - vLon);
                double bearing = std::atan2(dLon, lats[j] - vLat);
                if (bearing < 0) bearing += 2.0 * M_PI;
                if (bearing < coneMin || bearing >= coneMax)
                    continue;

                double dist = GSegment::haversineRaw(
                    vLon, vLat, lons[j], lats[j]);
                if (dist < bestDist)
                {
                    bestDist = dist;
                    bestIdx = j;
                }
            }

            if (bestIdx >= 0
                && isVisible(level.vertices[sourceIdx],
                             level.vertices[bestIdx], levelIdx))
            {
                level.adjacency[sourceIdx].push_back(bestIdx);
                level.adjacency[bestIdx].push_back(sourceIdx);
                edgesAdded++;
                found = true;
            }

            radiusDeg *= 2.0;
        }
    }

    return edgesAdded;
}

// =============================================================================
// Phase 2 (L0): Intra-Ring Express Edges
// =============================================================================

void HierarchicalVisibilityGraph::addIntraRingExpressEdges(
    GraphLevel& level, int levelIdx)
{
    int n = level.vertices.size();
    if (n == 0 || !level.quadtree) return;

    // Pre-extract coordinates
    std::vector<double> lons(n), lats(n);
    double envMinLon = 180, envMaxLon = -180;
    double envMinLat = 90,  envMaxLat = -90;
    for (int i = 0; i < n; ++i)
    {
        lons[i] = level.vertices[i]->getLongitude().value();
        lats[i] = level.vertices[i]->getLatitude().value();
        envMinLon = std::min(envMinLon, lons[i]);
        envMaxLon = std::max(envMaxLon, lons[i]);
        envMinLat = std::min(envMinLat, lats[i]);
        envMaxLat = std::max(envMaxLat, lats[i]);
    }

    double maxSearchDeg = std::sqrt(
        std::pow(envMaxLon - envMinLon, 2)
        + std::pow(envMaxLat - envMinLat, 2)) * 0.5;
    double initRadiusDeg = mAvgSpacing / METERS_PER_DEGREE_LAT;

    // Accept any vertex (intra-ring express has no ring filter)
    auto acceptAll = [](int) { return true; };

    long long edgesAdded = 0;
    int totalSampled = 0;
    int totalExpressVertices = 0;
    QElapsedTimer timer;
    timer.start();
    qint64 lastReport = 0;
    QTextStream progress(stderr);

    // Count total express vertices for progress reporting
    for (size_t pi = 0; pi < level.outerRings.size(); ++pi)
    {
        auto countRing = [&](const RingRange& ring) {
            if (ring.count <= EXPRESS_VERTEX_STEP) return;
            totalExpressVertices += (ring.count + EXPRESS_VERTEX_STEP - 1)
                                   / EXPRESS_VERTEX_STEP;
        };
        countRing(level.outerRings[pi]);
        for (const auto& hr : level.holeRings[pi])
            countRing(hr);
    }

    for (size_t pi = 0; pi < level.outerRings.size(); ++pi)
    {
        auto processRing = [&](const RingRange& ring)
        {
            if (ring.count <= EXPRESS_VERTEX_STEP) return;

            for (int r = 0; r < ring.count; r += EXPRESS_VERTEX_STEP)
            {
                int vIdx = ring.startIdx + r;
                auto arc = computeWaterArc(vIdx, levelIdx, lons, lats);
                edgesAdded += connectOctantNearest(
                    level, levelIdx, vIdx,
                    lons, lats, initRadiusDeg, maxSearchDeg,
                    acceptAll, arc);
                totalSampled++;

                qint64 nowMs = timer.elapsed();
                if (nowMs - lastReport >= 5000)
                {
                    lastReport = nowMs;
                    double pct = totalExpressVertices > 0
                        ? 100.0 * totalSampled / totalExpressVertices
                        : 0.0;
                    progress << "\r  Express: "
                             << QString::number(pct, 'f', 1) << "% | "
                             << totalSampled << "/" << totalExpressVertices
                             << " sampled | "
                             << edgesAdded << " edges | "
                             << QString::number(nowMs / 1000.0, 'f', 0)
                             << "s   ";
                    progress.flush();
                    emit pathFindingProgress(
                        totalSampled, totalExpressVertices,
                        nowMs / 1000.0);
                }
            }
        };

        processRing(level.outerRings[pi]);
        for (const auto& hr : level.holeRings[pi])
            processRing(hr);
    }
    progress << "\n";
    progress.flush();

    qInfo().noquote()
        << QString("  Phase 2 (express): %1 sampled, %2 edges in %3s")
               .arg(totalSampled)
               .arg(edgesAdded)
               .arg(timer.elapsed() / 1000.0, 0, 'f', 1);
}

// =============================================================================
// Phase 3 (L0): Inter-Ring Bridges
// =============================================================================

void HierarchicalVisibilityGraph::addInterRingBridges(
    GraphLevel& level, int levelIdx)
{
    int n = level.vertices.size();
    if (n == 0 || !level.quadtree) return;

    // Pre-extract coordinates
    std::vector<double> lons(n), lats(n);
    for (int i = 0; i < n; ++i)
    {
        lons[i] = level.vertices[i]->getLongitude().value();
        lats[i] = level.vertices[i]->getLatitude().value();
    }

    double initRadiusDeg = mAvgSpacing / METERS_PER_DEGREE_LAT;
    double maxSearchDeg = 90.0;  // half-globe

    long long edgesAdded = 0;
    long long ringPairsChecked = 0;
    QElapsedTimer timer;
    timer.start();
    QTextStream progress(stderr);

    // Collect all rings with their extreme vertices (N/S/E/W)
    struct RingInfo
    {
        int polyIdx;
        int holeIdx;  // -1 for outer ring
        RingRange range;
        int northIdx, southIdx, eastIdx, westIdx;
        double minLon, maxLon, minLat, maxLat;
    };
    std::vector<RingInfo> rings;

    for (size_t pi = 0; pi < level.outerRings.size(); ++pi)
    {
        auto addRing = [&](const RingRange& rr, int hIdx)
        {
            if (rr.count < 2) return;
            RingInfo ri;
            ri.polyIdx = static_cast<int>(pi);
            ri.holeIdx = hIdx;
            ri.range = rr;
            ri.minLon = 180; ri.maxLon = -180;
            ri.minLat = 90;  ri.maxLat = -90;
            ri.northIdx = ri.southIdx = ri.eastIdx = ri.westIdx =
                rr.startIdx;

            for (int r = 0; r < rr.count; ++r)
            {
                int idx = rr.startIdx + r;
                if (lats[idx] > ri.maxLat)
                { ri.maxLat = lats[idx]; ri.northIdx = idx; }
                if (lats[idx] < ri.minLat)
                { ri.minLat = lats[idx]; ri.southIdx = idx; }
                if (lons[idx] > ri.maxLon)
                { ri.maxLon = lons[idx]; ri.eastIdx = idx; }
                if (lons[idx] < ri.minLon)
                { ri.minLon = lons[idx]; ri.westIdx = idx; }
            }
            rings.push_back(ri);
        };

        addRing(level.outerRings[pi], -1);
        for (int hIdx = 0;
             hIdx < static_cast<int>(level.holeRings[pi].size()); ++hIdx)
            addRing(level.holeRings[pi][hIdx], hIdx);
    }

    qDebug() << "  Inter-ring: collected" << rings.size() << "rings";

    // For each ring's representative vertices, bridge to other rings
    for (size_t rA = 0; rA < rings.size(); ++rA)
    {
        const auto& ringA = rings[rA];
        std::array<int, 4> repsA = {
            ringA.northIdx, ringA.southIdx,
            ringA.eastIdx, ringA.westIdx
        };

        // Filter: only accept vertices on a DIFFERENT ring
        auto differentRing = [&](int j) -> bool {
            int jPoly = level.vertexPolygonId[j];
            int jHole = (j < static_cast<int>(level.vertexHoleId.size()))
                ? level.vertexHoleId[j] : -2;
            return (jPoly != ringA.polyIdx || jHole != ringA.holeIdx);
        };

        for (int repIdx : repsA)
        {
            edgesAdded += connectOctantNearest(
                level, levelIdx, repIdx,
                lons, lats, initRadiusDeg, maxSearchDeg,
                differentRing);
        }

        qint64 nowMs = timer.elapsed();
        if (nowMs > 0 && (rA % 100 == 0 || rA + 1 == rings.size()))
        {
            progress << "\r  Inter-ring: "
                     << (rA + 1) << "/" << rings.size()
                     << " rings | " << edgesAdded << " edges | "
                     << QString::number(nowMs / 1000.0, 'f', 0) << "s   ";
            progress.flush();
        }
    }
    progress << "\n";
    progress.flush();

    qInfo().noquote()
        << QString("  Phase 3 (inter-ring): %1 edges from %2 rings in %3s")
               .arg(edgesAdded)
               .arg(rings.size())
               .arg(timer.elapsed() / 1000.0, 0, 'f', 1);
}

// =============================================================================
// Phase 4 (L0): Borůvka Connectivity Bridging
// =============================================================================

void HierarchicalVisibilityGraph::bridgeConnectedComponents(
    GraphLevel& level, int levelIdx)
{
    int n = level.vertices.size();
    if (n == 0 || !level.quadtree) return;

    // Pre-extract coordinates
    std::vector<double> lons(n), lats(n);
    for (int i = 0; i < n; ++i)
    {
        lons[i] = level.vertices[i]->getLongitude().value();
        lats[i] = level.vertices[i]->getLatitude().value();
    }

    QTextStream progress(stderr);

    for (int iteration = 0; iteration < 20; ++iteration)
    {
        // BFS to label connected components
        std::vector<int> compId(n, -1);
        int numComponents = 0;
        for (int i = 0; i < n; ++i)
        {
            if (compId[i] >= 0) continue;
            int cid = numComponents++;
            std::queue<int> q;
            q.push(i);
            compId[i] = cid;
            while (!q.empty())
            {
                int u = q.front(); q.pop();
                for (int v : level.adjacency[u])
                {
                    if (compId[v] < 0)
                    {
                        compId[v] = cid;
                        q.push(v);
                    }
                }
            }
        }

        progress << "\r  Borůvka: iter " << iteration
                 << ", " << numComponents << " components   ";
        progress.flush();

        if (numComponents <= 1) break;

        // For each component, find geographic extreme vertices (N/S/E/W)
        struct CompInfo
        {
            int northIdx = -1, southIdx = -1, eastIdx = -1, westIdx = -1;
            double maxLat = -90, minLat = 90, maxLon = -180, minLon = 180;
        };
        std::vector<CompInfo> comps(numComponents);

        for (int i = 0; i < n; ++i)
        {
            int c = compId[i];
            if (lats[i] > comps[c].maxLat)
            {
                comps[c].maxLat = lats[i];
                comps[c].northIdx = i;
            }
            if (lats[i] < comps[c].minLat)
            {
                comps[c].minLat = lats[i];
                comps[c].southIdx = i;
            }
            if (lons[i] > comps[c].maxLon)
            {
                comps[c].maxLon = lons[i];
                comps[c].eastIdx = i;
            }
            if (lons[i] < comps[c].minLon)
            {
                comps[c].minLon = lons[i];
                comps[c].westIdx = i;
            }
        }

        // For each representative, bridge to a different component
        double initR = mAvgSpacing / METERS_PER_DEGREE_LAT;
        int bridgesAdded = 0;

        for (int c = 0; c < numComponents; ++c)
        {
            std::array<int, 4> reps = {
                comps[c].northIdx, comps[c].southIdx,
                comps[c].eastIdx, comps[c].westIdx
            };

            // Filter: only accept vertices in a DIFFERENT component
            auto differentComp = [&](int j) -> bool {
                return compId[j] != c;
            };

            for (int repIdx : reps)
            {
                if (repIdx < 0) continue;
                bridgesAdded += connectOctantNearest(
                    level, levelIdx, repIdx,
                    lons, lats, initR, 180.0, differentComp);
            }
        }

        progress << "\r  Borůvka: iter " << iteration
                 << ", +" << bridgesAdded << " bridges          ";
        progress.flush();

        if (bridgesAdded == 0) break;  // no more visible connections
    }
    progress << "\n";
    progress.flush();

    qInfo().noquote() << "  Phase 4 (Borůvka): connectivity ensured";
}

// =============================================================================
// Spatial Grid Edges (L1/L2/L3)
// =============================================================================

void HierarchicalVisibilityGraph::addSpatialGridEdges(
    GraphLevel& level, int levelIdx, double maxDist,
    bool checkVisibility)
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
    double cellSize = std::max(maxDist / (METERS_PER_DEGREE_LAT * 10.0), 0.5);
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
        std::ceil(maxDist / (METERS_PER_DEGREE_LAT * cellSize))) + 1;
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
                    else if (levelIdx > 0 || checkVisibility)
                    {
                        // Same polygon: nearest per octant
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

        // Add cross-polygon edges
        int crossCount = 0;
        for (const auto& [polyJ, cand] : bestPerPoly)
        {
            if (crossCount >= maxCross) break;
            if (!checkVisibility
                || isVisible(level.vertices[i],
                             level.vertices[cand.idx], levelIdx))
                addEdgeIfNew(i, cand.idx);
            crossCount++;
        }

        // Add same-polygon octant edges
        for (const auto& oct : octants)
        {
            if (oct.idx < 0) continue;
            if (!checkVisibility
                || isVisible(level.vertices[i],
                             level.vertices[oct.idx], levelIdx))
                addEdgeIfNew(i, oct.idx);
        }
    }
}

// =============================================================================
// Phase 3: Antimeridian Edges
// =============================================================================

void HierarchicalVisibilityGraph::addAntimeridianEdges(
    GraphLevel& level, int levelIdx, double maxDist,
    bool checkVisibility)
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

            if (checkVisibility
                && !isVisible(level.vertices[ei],
                              level.vertices[cand.idx], levelIdx))
                continue;

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

    // O(1) fast path: if point is a known L0 vertex (e.g., snapped
    // from a hole boundary or outer ring), get polygon directly.
    auto sit = lvl.vertexIndex.find(start);
    if (sit != lvl.vertexIndex.end()
        && sit->second < static_cast<int>(lvl.vertexPolygonId.size()))
        startPolyIdx = lvl.vertexPolygonId[sit->second];

    auto git = lvl.vertexIndex.find(goal);
    if (git != lvl.vertexIndex.end()
        && git->second < static_cast<int>(lvl.vertexPolygonId.size()))
        goalPolyIdx = lvl.vertexPolygonId[git->second];

    // Fallback: standard polygon containment for in-water user points
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
        return ShortestPathResult();  // fallback to searchAtLevel

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
    lvl.vertexHoleId.push_back(-2);  // injected point, unknown hole

    // Extend adjacency to accommodate the new vertex
    lvl.adjacency.resize(newIdx + 1);

    // Set polygon ownership for fast visibility lookups
    point->addOwningPolygon(lvl.polygons[bestPolyIdx]);

    // Connect to nearest vertices in the same polygon.
    // This gives the injected point adjacency edges so A* can
    // route through it, similar to Phase 2 spatial grid edges.

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

ShortestPathResult HierarchicalVisibilityGraph::searchAtLevel(
    const std::shared_ptr<GPoint>& start,
    const std::shared_ptr<GPoint>& goal,
    int level,
    const Corridor* corridor,
    const std::shared_ptr<GPoint>& preSnappedStart,
    const std::shared_ptr<GPoint>& preSnappedGoal,
    bool useHeuristic,
    int maxExpansions)
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

    // Heuristic: haversine for A* mode, zero for Dijkstra mode
    auto heuristic = [endLat, endLon, useHeuristic](
        const std::shared_ptr<GPoint>& pt) -> double {
        if (!useHeuristic) return 0.0;  // Dijkstra: uniform exploration
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
    int expandedCount = 0;

    while (!openSet.empty())
    {
        if (++iterationCount % 64 == 0)
        {
            if (QThread::currentThread()->isInterruptionRequested())
            {
                qWarning() << "Search cancelled by thread interruption";
                return ShortestPathResult();
            }

            // Expansion budget check
            if (maxExpansions > 0 && expandedCount > maxExpansions)
            {
                qDebug() << "searchAtLevel" << level
                         << ": expansion budget exceeded"
                         << expandedCount << ">" << maxExpansions;
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
        ++expandedCount;

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
    QElapsedTimer hsTimer;
    hsTimer.start();
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
    qDebug() << "  snap:" << hsTimer.elapsed() << "ms";

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
    // Use snapped points so in-hole / outside-polygon vertices are
    // recognized via vertexIndex lookup (original user points aren't
    // polygon vertices and fail both containment and ringsContain).
    auto result3 = polygonGraphSearch(
        snappedStart ? snappedStart : start,
        snappedGoal ? snappedGoal : goal);
    qDebug() << "  L3 polyGraph:" << (result3.isValid() ? "valid" : "INVALID")
             << hsTimer.elapsed() << "ms";

    if (!result3.isValid())
    {
        result3 = searchAtLevel(start, goal, 3, nullptr,
                                snappedStart, snappedGoal);
        qDebug() << "  L3 A*:" << (result3.isValid() ? "valid" : "INVALID")
                 << hsTimer.elapsed() << "ms";
    }

    if (!result3.isValid())
    {
        qDebug() << "  L3 failed → unconstrained L0";
        return searchAtLevel(start, goal, 0, nullptr,
                            snappedStart, snappedGoal);
    }
    bestCoarseResult = result3;

    // --- Level 2 ---
    double expansion2 = CORRIDOR_EXPANSION[2];
    auto corridor2 = buildCorridor(result3, 2, expansion2);
    auto result2 = searchAtLevel(start, goal, 2, &corridor2,
                                snappedStart, snappedGoal);
    qDebug() << "  L2 corridor:" << corridor2.allowedVertexIndices.size()
             << "verts," << (result2.isValid() ? "valid" : "INVALID")
             << hsTimer.elapsed() << "ms";

    if (!result2.isValid())
    {
        auto widerCorridor2 = buildCorridor(result3, 2, expansion2 * 3.0);
        result2 = searchAtLevel(start, goal, 2, &widerCorridor2,
                               snappedStart, snappedGoal);
        qDebug() << "  L2 wider:" << widerCorridor2.allowedVertexIndices.size()
                 << "verts," << (result2.isValid() ? "valid" : "INVALID")
                 << hsTimer.elapsed() << "ms";
    }

    if (result2.isValid())
    {
        bestCoarseResult = std::move(result2);
    }

    // --- Level 1 ---
    double expansion1 = CORRIDOR_EXPANSION[1];
    auto corridor1 = buildCorridor(bestCoarseResult, 1, expansion1);
    auto result1 = searchAtLevel(start, goal, 1, &corridor1,
                                snappedStart, snappedGoal);
    qDebug() << "  L1 corridor:" << corridor1.allowedVertexIndices.size()
             << "verts," << (result1.isValid() ? "valid" : "INVALID")
             << hsTimer.elapsed() << "ms";

    if (!result1.isValid())
    {
        auto widerCorridor1 = buildCorridor(bestCoarseResult, 1,
                                            expansion1 * 3.0);
        result1 = searchAtLevel(start, goal, 1, &widerCorridor1,
                               snappedStart, snappedGoal);
        qDebug() << "  L1 wider:" << widerCorridor1.allowedVertexIndices.size()
                 << "verts," << (result1.isValid() ? "valid" : "INVALID")
                 << hsTimer.elapsed() << "ms";
    }

    if (result1.isValid())
    {
        bestCoarseResult = std::move(result1);
    }

    // --- Level 0 (original polygons — only valid final result) ---

    // L0 corridor expansion must be proportional to express vertex spacing
    // so the tube corridor captures express + bridge vertices.
    // EXPRESS_VERTEX_STEP × avgSpacing = express spacing on the ring.
    double expansion0 = mAvgSpacing * EXPRESS_VERTEX_STEP;
    bool hasLevel0Adj = mLevel0AdjReady.load(std::memory_order_acquire);
    qDebug() << "  L0 hasAdj:" << hasLevel0Adj << hsTimer.elapsed() << "ms";

    if (hasLevel0Adj)
    {
        // Level 0 adjacency is pre-computed — corridors act only as
        // geographic filters via allowedVertexIndices. No per-query
        // precomputeCorridorAdjacency needed.
        // Tube corridors follow the coarse path tightly, capturing
        // coastline vertices along the route for connected A* search.
        auto corridor0 = buildCorridor(bestCoarseResult, 0, expansion0);
        qDebug() << "  L0 1x corridor:" << corridor0.allowedVertexIndices.size()
                 << "verts," << hsTimer.elapsed() << "ms, starting A*...";
        auto result0 = searchAtLevel(start, goal, 0, &corridor0,
                                    snappedStart, snappedGoal);
        qDebug() << "  L0 1x:" << (result0.isValid() ? "valid" : "INVALID")
                 << hsTimer.elapsed() << "ms";

        if (result0.isValid())
            return result0;

        auto widerCorridor0 = buildCorridor(bestCoarseResult, 0,
                                            expansion0 * 3.0);
        qDebug() << "  L0 3x corridor:" << widerCorridor0.allowedVertexIndices.size()
                 << "verts," << hsTimer.elapsed() << "ms, starting A*...";
        result0 = searchAtLevel(start, goal, 0, &widerCorridor0,
                               snappedStart, snappedGoal);
        qDebug() << "  L0 3x:" << (result0.isValid() ? "valid" : "INVALID")
                 << hsTimer.elapsed() << "ms";

        if (result0.isValid())
            return result0;

        auto veryWideCorridor0 = buildCorridor(bestCoarseResult, 0,
                                               expansion0 * 10.0);
        qDebug() << "  L0 10x corridor:" << veryWideCorridor0.allowedVertexIndices.size()
                 << "verts," << hsTimer.elapsed() << "ms, starting A*...";
        result0 = searchAtLevel(start, goal, 0, &veryWideCorridor0,
                               snappedStart, snappedGoal);
        qDebug() << "  L0 10x:" << (result0.isValid() ? "valid" : "INVALID")
                 << hsTimer.elapsed() << "ms";

        if (result0.isValid())
            return result0;

        // Tube corridors can't bridge this route (e.g., ocean crossing
        // where no cached L0 edges span the gap). Try directional
        // visibility bridge before falling back to unconstrained A*.
        {
            qDebug() << "  L0 cached failed → directional visibility bridge"
                     << hsTimer.elapsed() << "ms";
            auto result0bridge = bridgeViaDirectionalVisibility(
                bestCoarseResult, start, goal, snappedStart, snappedGoal);
            qDebug() << "  L0 bridge:"
                     << (result0bridge.isValid() ? "valid" : "INVALID")
                     << hsTimer.elapsed() << "ms";
            if (result0bridge.isValid())
                return result0bridge;
        }
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
                auto result = searchAtLevel(start, goal, 0, &corr,
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
    qDebug() << "  All corridors failed → unconstrained L0 A*"
             << hsTimer.elapsed() << "ms, starting...";
    int aStarBudget = static_cast<int>(mLevels[0].vertices.size() / 2);
    if (aStarBudget < 200000) aStarBudget = 200000;

    auto finalResult = searchAtLevel(start, goal, 0, nullptr,
                                    snappedStart, snappedGoal,
                                    true, aStarBudget);
    qDebug() << "  unconstrained L0 A* (budget" << aStarBudget << "):"
             << (finalResult.isValid() ? "valid" : "INVALID")
             << hsTimer.elapsed() << "ms";

    if (!finalResult.isValid()) {
        qDebug() << "  A* budget exceeded → Dijkstra fallback";
        finalResult = searchAtLevel(start, goal, 0, nullptr,
                                    snappedStart, snappedGoal,
                                    false, -1);
        qDebug() << "  Dijkstra L0:"
                 << (finalResult.isValid() ? "valid" : "INVALID")
                 << hsTimer.elapsed() << "ms";
    }

    return finalResult;
}

// =============================================================================
// Corridor Construction
// =============================================================================

Corridor HierarchicalVisibilityGraph::buildCorridor(
    const ShortestPathResult& coarsePath,
    int targetLevel,
    double expansion,
    bool useTubeFilter)
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
                               ? expansion / (METERS_PER_DEGREE_LAT * cosLat)
                               : 180.0;
        double latExpand = expansion / METERS_PER_DEGREE_LAT;

        corridor.minLon = std::min(corridor.minLon, lon - lonExpand);
        corridor.maxLon = std::max(corridor.maxLon, lon + lonExpand);
        corridor.minLat = std::min(corridor.minLat, lat - latExpand);
        corridor.maxLat = std::max(corridor.maxLat, lat + latExpand);
    }

    // Antimeridian-crossing corridors: if waypoints span both sides of
    // ±180° (bbox covers >180° longitude), the gap at ±180° would exclude
    // critical antimeridian vertices. Expand to full longitude range and
    // let tube/distance filtering handle the geographic constraint.
    if (corridor.maxLon - corridor.minLon > 180.0)
    {
        corridor.minLon = -180.0;
        corridor.maxLon =  180.0;
    }

    // Use tube filtering only when enabled and coarse path has 3+ waypoints
    // (curved route). For 2-point paths (direct line), the tube would
    // be too narrow for routes that need to detour around land.
    // Disabled at L0 (useTubeFilter=false) because coarse paths may cut
    // through land due to missing holes at simplified levels.
    bool useTube = useTubeFilter && (coarsePath.points.size() >= 3);

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
// Ring Neighbor Lookup
// =============================================================================

std::pair<int,int> HierarchicalVisibilityGraph::getRingNeighbors(
    int vIdx, int level) const
{
    const auto& lvl = mLevels[level];

    if (vIdx < 0
        || vIdx >= static_cast<int>(lvl.vertexPolygonId.size())
        || vIdx >= static_cast<int>(lvl.vertexHoleId.size()))
    {
        return {-1, -1};
    }

    int polyId = lvl.vertexPolygonId[vIdx];
    int holeId = lvl.vertexHoleId[vIdx];

    if (polyId < 0 || polyId >= static_cast<int>(lvl.outerRings.size()))
        return {-1, -1};

    RingRange ring;
    if (holeId < 0)
    {
        ring = lvl.outerRings[polyId];
    }
    else
    {
        if (holeId >= static_cast<int>(lvl.holeRings[polyId].size()))
            return {-1, -1};
        ring = lvl.holeRings[polyId][holeId];
    }

    if (ring.count < 2) return {-1, -1};

    int posInRing = vIdx - ring.startIdx;
    int prevIdx = ring.startIdx + (posInRing - 1 + ring.count) % ring.count;
    int nextIdx = ring.startIdx + (posInRing + 1) % ring.count;

    return {prevIdx, nextIdx};
}

// =============================================================================
// Directional Visibility Bridge
// =============================================================================

ShortestPathResult HierarchicalVisibilityGraph::bridgeViaDirectionalVisibility(
    const ShortestPathResult& coarsePath,
    const std::shared_ptr<GPoint>& start,
    const std::shared_ptr<GPoint>& goal,
    const std::shared_ptr<GPoint>& snappedStart,
    const std::shared_ptr<GPoint>& snappedGoal)
{
    QElapsedTimer timer;
    timer.start();

    const auto& lvl = mLevels[0];
    if (!lvl.quadtree) return {};
    if (!mLevel0AdjReady.load(std::memory_order_acquire)) return {};

    auto sStart = snappedStart ? snappedStart : snapToWater(start, 0);
    auto sGoal  = snappedGoal  ? snappedGoal  : snapToWater(goal, 0);
    if (!sStart || !sGoal) return {};

    // ------------------------------------------------------------------
    // 1. Snap coarse waypoints to nearest L0 vertices
    // ------------------------------------------------------------------
    QVector<int> waypointLvlIdx;  // indices in lvl.vertices

    auto appendWaypoint = [&](const std::shared_ptr<GPoint>& pt) {
        if (!pt) return;
        auto it = lvl.vertexIndex.find(pt);
        if (it == lvl.vertexIndex.end()) return;
        if (!waypointLvlIdx.isEmpty() && waypointLvlIdx.last() == it->second)
            return;  // dedup consecutive
        waypointLvlIdx.append(it->second);
    };

    appendWaypoint(sStart);
    for (const auto& wp : coarsePath.points)
    {
        auto nearest = lvl.quadtree->findNearestNeighborPoint(wp);
        if (nearest) appendWaypoint(nearest);
    }
    appendWaypoint(sGoal);

    if (waypointLvlIdx.size() < 2) return {};

    qDebug() << "  Bridge: snapped" << waypointLvlIdx.size()
             << "waypoints in" << timer.elapsed() << "ms";

    // ------------------------------------------------------------------
    // 2. Directional visibility search from each waypoint
    // ------------------------------------------------------------------
    // Store discovered bridge edges as level-index pairs.
    struct BridgeEdge { int from; int to; };
    QVector<BridgeEdge> bridgeEdges;

    // Set of level indices for bridge vertices (to add to corridor bbox).
    std::unordered_set<int> bridgeVertexLvlIdx;

    // Helper: normalise angle to [-pi, pi]
    auto normAngle = [](double a) -> double {
        while (a >  M_PI) a -= 2.0 * M_PI;
        while (a < -M_PI) a += 2.0 * M_PI;
        return a;
    };

    // Directional search from sourceVIdx toward targetVIdx.
    // Finds visible L0 vertices in the water-facing angular range
    // that are closest to the search bearing, and records bridge edges.
    auto searchDirectional = [&](int sourceVIdx, int targetVIdx)
    {
        // --- Ring neighbor lookup ---
        auto [prevIdx, nextIdx] = getRingNeighbors(sourceVIdx, 0);
        if (prevIdx < 0) return;  // not a ring vertex (injected point)

        double vLon = lvl.vertices[sourceVIdx]->getLongitude().value();
        double vLat = lvl.vertices[sourceVIdx]->getLatitude().value();
        double prevLon = lvl.vertices[prevIdx]->getLongitude().value();
        double prevLat = lvl.vertices[prevIdx]->getLatitude().value();
        double nextLon = lvl.vertices[nextIdx]->getLongitude().value();
        double nextLat = lvl.vertices[nextIdx]->getLatitude().value();

        // --- Bearings to ring neighbors (fast planar atan2) ---
        double bPrev = std::atan2(prevLon - vLon, prevLat - vLat);
        double bNext = std::atan2(nextLon - vLon, nextLat - vLat);

        // --- Determine water arc ---
        double diff = normAngle(bNext - bPrev);
        double bis1 = normAngle(bPrev + diff / 2.0);

        // Cast a short test ray at bis1 to check if it points into water
        double cosLat  = std::cos(vLat * M_PI / 180.0);
        double safecos = std::max(cosLat, 0.01);
        double testDeg = 100.0 / METERS_PER_DEGREE_LAT;  // ~100 m
        double testLon = vLon + std::sin(bis1) * testDeg / safecos;
        double testLat = vLat + std::cos(bis1) * testDeg;
        GPoint testPt{units::angle::degree_t{testLon},
                      units::angle::degree_t{testLat}};

        int polyId = lvl.vertexPolygonId[sourceVIdx];
        if (polyId < 0 || polyId >= lvl.polygons.size()) return;
        bool bis1IsWater =
            lvl.polygons[polyId]->isPointWithinPolygon(testPt);

        double waterCenter, waterHalfArc;
        if (bis1IsWater)
        {
            waterCenter  = bis1;
            waterHalfArc = std::abs(diff) / 2.0;
        }
        else
        {
            waterCenter  = normAngle(bis1 + M_PI);
            waterHalfArc = M_PI - std::abs(diff) / 2.0;
        }

        // Ensure half-arc is at least a small sliver (avoid 0-width)
        if (waterHalfArc < 0.01) return;

        // --- Goal-directed bearing ---
        double tLon = lvl.vertices[targetVIdx]->getLongitude().value();
        double tLat = lvl.vertices[targetVIdx]->getLatitude().value();
        double targetBearing = std::atan2(tLon - vLon, tLat - vLat);

        double dt = normAngle(targetBearing - waterCenter);
        double bestBearing;
        if (std::abs(dt) <= waterHalfArc)
        {
            bestBearing = targetBearing;
        }
        else
        {
            bestBearing = (dt > 0)
                ? normAngle(waterCenter + waterHalfArc)
                : normAngle(waterCenter - waterHalfArc);
        }

        // --- Sample angles ---
        const int NUM_SAMPLES = 8;
        double spreadHalf = std::min(waterHalfArc, M_PI / 4.0);

        // Search radius: proportional to distance to target, cap 500 km
        double wpDist = GSegment::haversineRaw(vLon, vLat, tLon, tLat);
        double searchRadius = std::min(std::max(wpDist, 200000.0), 500000.0);
        double halfAngle = 15.0 * M_PI / 180.0;

        // Single quadtree query for the search radius bounding box
        double rDegLat = searchRadius / METERS_PER_DEGREE_LAT;
        double rDegLon = searchRadius / (METERS_PER_DEGREE_LAT * safecos);
        QRectF sectorBbox(vLon - rDegLon, vLat - rDegLat,
                          2.0 * rDegLon, 2.0 * rDegLat);
        auto candidates = lvl.quadtree->findVerticesInRange(sectorBbox);

        for (int s = 0; s < NUM_SAMPLES; ++s)
        {
            double frac = (NUM_SAMPLES == 1) ? 0.0
                : (s - (NUM_SAMPLES - 1) / 2.0)
                  / ((NUM_SAMPLES - 1) / 2.0);
            double sampleBearing = bestBearing + frac * spreadHalf;

            // Verify sample is within the water arc
            double ds = normAngle(sampleBearing - waterCenter);
            if (std::abs(ds) > waterHalfArc) continue;

            // Find nearest L0 vertex in this angular sector
            double bestDist = std::numeric_limits<double>::max();
            int    bestCandLvlIdx = -1;

            for (const auto& c : candidates)
            {
                auto cIt = lvl.vertexIndex.find(c);
                if (cIt == lvl.vertexIndex.end()) continue;
                if (cIt->second == sourceVIdx)    continue;

                double cLon = c->getLongitude().value();
                double cLat = c->getLatitude().value();
                double cBearing = std::atan2(cLon - vLon, cLat - vLat);

                double bDiff = normAngle(cBearing - sampleBearing);
                if (std::abs(bDiff) > halfAngle) continue;

                double dist = GSegment::haversineRaw(
                    vLon, vLat, cLon, cLat);
                if (dist < bestDist)
                {
                    bestDist = dist;
                    bestCandLvlIdx = cIt->second;
                }
            }

            if (bestCandLvlIdx >= 0
                && isVisible(lvl.vertices[sourceVIdx],
                             lvl.vertices[bestCandLvlIdx], 0))
            {
                bridgeEdges.append({sourceVIdx, bestCandLvlIdx});
                bridgeVertexLvlIdx.insert(bestCandLvlIdx);
            }
        }
    };

    // Search from each waypoint toward the next AND from next toward prev
    // (bidirectional bridging for each gap).
    for (int w = 0; w + 1 < waypointLvlIdx.size(); ++w)
    {
        searchDirectional(waypointLvlIdx[w],     waypointLvlIdx[w + 1]);
        searchDirectional(waypointLvlIdx[w + 1], waypointLvlIdx[w]);
    }

    qDebug() << "  Bridge: found" << bridgeEdges.size()
             << "bridge edges in" << timer.elapsed() << "ms";

    if (bridgeEdges.isEmpty()) return {};

    // ------------------------------------------------------------------
    // 3. Build corridor with cached L0 edges + bridge edges
    // ------------------------------------------------------------------

    // Start from a generous bbox corridor (no tube).
    Corridor corridor = buildCorridor(coarsePath, 0,
                                      CORRIDOR_EXPANSION[0] * 10.0, false);

    // Ensure all bridge vertices and waypoints are included.
    for (int idx : bridgeVertexLvlIdx)
        corridor.allowedVertexIndices.insert(idx);
    for (int idx : waypointLvlIdx)
        corridor.allowedVertexIndices.insert(idx);

    // Build corridor vertex list with a lvl→corr index map for fast
    // edge mapping (avoids shared_ptr hash lookups per edge).
    std::unordered_map<int, int> lvlToCorr;
    lvlToCorr.reserve(corridor.allowedVertexIndices.size() + 2);

    for (int lvlIdx : corridor.allowedVertexIndices)
    {
        if (lvlIdx >= lvl.vertices.size()) continue;
        int corrIdx = corridor.vertices.size();
        corridor.vertices.append(lvl.vertices[lvlIdx]);
        corridor.vertexIndex[lvl.vertices[lvlIdx]] = corrIdx;
        lvlToCorr[lvlIdx] = corrIdx;
    }

    // Add start/goal if not yet present.
    auto addEndpoint = [&](const std::shared_ptr<GPoint>& pt) {
        if (!pt) return;
        if (corridor.vertexIndex.count(pt)) return;
        auto it = lvl.vertexIndex.find(pt);
        int corrIdx = corridor.vertices.size();
        corridor.vertices.append(pt);
        corridor.vertexIndex[pt] = corrIdx;
        if (it != lvl.vertexIndex.end())
            lvlToCorr[it->second] = corrIdx;
    };
    addEndpoint(sStart);
    addEndpoint(sGoal);

    int n = corridor.vertices.size();
    corridor.adjacency.resize(n);

    // Copy cached L0 edges between corridor vertices.
    for (auto& [lvlIdx, corrIdx] : lvlToCorr)
    {
        if (lvlIdx >= static_cast<int>(lvl.adjacency.size())) continue;
        for (int neighborLvlIdx : lvl.adjacency[lvlIdx])
        {
            auto nit = lvlToCorr.find(neighborLvlIdx);
            if (nit != lvlToCorr.end())
            {
                corridor.adjacency[corrIdx].push_back(nit->second);
            }
        }
    }

    // Add bridge edges (new connections discovered by directional vis).
    for (const auto& be : bridgeEdges)
    {
        auto fromIt = lvlToCorr.find(be.from);
        auto toIt   = lvlToCorr.find(be.to);
        if (fromIt != lvlToCorr.end() && toIt != lvlToCorr.end())
        {
            corridor.adjacency[fromIt->second].push_back(toIt->second);
            corridor.adjacency[toIt->second].push_back(fromIt->second);
        }
    }

    corridor.hasAdjacency = true;
    corridor.minLon = -180.0;
    corridor.maxLon =  180.0;
    corridor.minLat =  -90.0;
    corridor.maxLat =   90.0;

    qDebug() << "  Bridge corridor:" << n << "vertices,"
             << bridgeEdges.size() << "bridge edges, built in"
             << timer.elapsed() << "ms";

    // ------------------------------------------------------------------
    // 4. A* on the bridge corridor
    // ------------------------------------------------------------------
    return searchAtLevel(start, goal, 0, &corridor, sStart, sGoal);
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
        if (commonPolygon->segmentCrossesHoles(segLine))
            return false;

        // Verify midpoint is inside the water polygon — prevents
        // segments that exit the polygon through concavities in the
        // outer ring (e.g., crossing land between two coastal vertices
        // where no polygon edges block the path).
        //
        // Exception: boundary edge midpoints naturally fall inside the
        // hole they trace (chord midpoint of a convex polygon is always
        // interior). When both endpoints are on the same hole boundary,
        // the midpoint being inside that hole is expected — not a
        // through-land violation.
        double midLon = (startPt->getLongitude().value()
                         + endPt->getLongitude().value()) / 2.0;
        double midLat = (startPt->getLatitude().value()
                         + endPt->getLatitude().value()) / 2.0;
        auto midDeg = [](double v) { return units::angle::degree_t(v); };
        GPoint midPoint(midDeg(midLon), midDeg(midLat));
        if (!commonPolygon->isPointWithinPolygon(midPoint))
        {
            int midHoleIdx =
                commonPolygon->findContainingHoleIndex(midPoint);
            if (midHoleIdx < 0)
                return false;  // Outside exterior ring

            // O(1) check: both endpoints on the same hole boundary
            // AND adjacent on the ring (prev/next)? Only adjacent ring
            // vertices form boundary edges whose midpoints naturally
            // fall inside the hole. Non-adjacent same-hole chords
            // cross the hole interior (land).
            auto sit = lvl.vertexIndex.find(startPt);
            auto eit = lvl.vertexIndex.find(endPt);
            bool startOnMidHole = sit != lvl.vertexIndex.end()
                && sit->second < static_cast<int>(lvl.vertexHoleId.size())
                && lvl.vertexHoleId[sit->second] == midHoleIdx;
            bool endOnMidHole = eit != lvl.vertexIndex.end()
                && eit->second < static_cast<int>(lvl.vertexHoleId.size())
                && lvl.vertexHoleId[eit->second] == midHoleIdx;
            if (!startOnMidHole || !endOnMidHole)
                return false;  // Not same-hole boundary — through land

            // Same hole — verify adjacency via ring topology
            auto [prevIdx, nextIdx] = getRingNeighbors(sit->second, level);
            if (prevIdx < 0
                || (eit->second != prevIdx && eit->second != nextIdx))
                return false;  // Same hole but not adjacent — through land
        }
    }
    else
    {
        auto segLine = std::make_shared<GLine>(startPt, endPt, FastConstruct);
        for (const auto& polygon : lvl.polygons)
        {
            if (!polygon->segmentBoundsIntersect(segLine))
                continue;
            // Check both hole crossings AND outer ring crossings.
            // Without the outer ring check, segments between vertices
            // on different polygons can cross a polygon's boundary
            // undetected (e.g., Bangladesh→Caspian crossing Asia).
            if (polygon->segmentCrossesHoles(segLine))
                return false;
            if (polygon->segmentCrossesOuterRing(startPt, endPt))
                return false;
        }

        // Cross-polygon midpoint check: if no polygon claims the
        // midpoint as water, the segment goes through land.
        // This catches segments between vertices on different polygons
        // where the chord flies over a continent without crossing
        // any polygon edge (e.g., Bangladesh→Caspian over Central Asia).
        double midLon = (startPt->getLongitude().value()
                         + endPt->getLongitude().value()) / 2.0;
        double midLat = (startPt->getLatitude().value()
                         + endPt->getLatitude().value()) / 2.0;
        GPoint midPt{units::angle::degree_t{midLon},
                     units::angle::degree_t{midLat}};

        bool midInAnyPolygon = false;
        for (const auto& polygon : lvl.polygons)
        {
            if (polygon->isPointWithinPolygon(midPt))
            {
                midInAnyPolygon = true;
                break;
            }
        }
        if (!midInAnyPolygon)
            return false;
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

            const double COORD_TOL = SIMPLIFIED_COORD_TOL;
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

    // Distance-sorted visibility with caps
    auto visibleNodes = findNearestVisibleNodes(
        node, candidates, 0,
        ON_DEMAND_MAX_CHECK, ON_DEMAND_MAX_FOUND);

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

    // Distance-sorted visibility with caps
    auto visibleNodes = findNearestVisibleNodes(
        node, tasks, 0, ON_DEMAND_MAX_CHECK, ON_DEMAND_MAX_FOUND);

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

    computeDynamicParameters();
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

HVGDiagnostics HierarchicalVisibilityGraph::getDiagnostics() const
{
    HVGDiagnostics diag;
    for (int i = 0; i < NUM_LEVELS; ++i)
    {
        auto& info = diag.levels[i];
        const auto& lvl = mLevels[i];
        info.vertexCount = lvl.vertices.size();
        long long directedEdges = 0;
        for (const auto& adj : lvl.adjacency)
            directedEdges += adj.size();
        info.edgeCount = directedEdges / 2;
        info.toleranceMeters = mLevelTolerances[i];
        info.maxDistanceMeters = mLevelMaxDistance[i];
    }
    diag.level0AdjReady =
        mLevel0AdjReady.load(std::memory_order_acquire);
    diag.polygonGraphPolygonCount = mPolygonGraph.numPolygons;
    return diag;
}

}; // namespace ShipNetSimCore
