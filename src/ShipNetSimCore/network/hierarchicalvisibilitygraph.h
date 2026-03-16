#ifndef HIERARCHICALVISIBILITYGRAPH_H
#define HIERARCHICALVISIBILITYGRAPH_H

#include "gline.h"
#include "gsegment.h"
#include "polygon.h"
#include "quadtree.h"
#include "seaport.h"
#include <QHash>
#include <QReadWriteLock>
#include <QVector>
#include <array>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace ShipNetSimCore
{

struct ShortestPathResult
{
    QVector<std::shared_ptr<GLine>>  lines;
    QVector<std::shared_ptr<GPoint>> points;

    bool isValid() const;
};

/**
 * @struct RingRange
 * @brief Describes a contiguous range of vertices belonging to one polygon ring.
 *
 * Populated during level construction to enable O(n) boundary edge
 * identification without re-scanning polygon structures.
 */
struct RingRange
{
    int startIdx;   ///< First vertex index in GraphLevel::vertices (inclusive)
    int count;      ///< Number of vertices in this ring
};

struct GraphLevel
{
    int    levelIndex;
    double toleranceMeters;
    QVector<std::shared_ptr<Polygon>> polygons;
    std::unique_ptr<Quadtree> quadtree;
    QVector<std::shared_ptr<GPoint>> vertices;
    std::vector<std::vector<int>> adjacency;
    std::unordered_map<std::shared_ptr<GPoint>, int,
        GPoint::Hash, GPoint::Equal> vertexIndex;
    std::vector<int> vertexPolygonId;
    mutable QReadWriteLock lock;

    /// Ring layout metadata — one outer ring range per polygon
    std::vector<RingRange> outerRings;
    /// Ring layout metadata — hole ring ranges per polygon
    std::vector<std::vector<RingRange>> holeRings;
};

/**
 * @struct PolygonGraph
 * @brief Coarse polygon-level routing graph for instant L3 pathfinding.
 *
 * Represents connectivity between polygons via representative vertices.
 * With 2 ocean polygons this is trivial; scales for multi-polygon datasets.
 */
struct PolygonGraph
{
    struct Edge
    {
        int    targetPoly;
        double distance;
    };
    /// adjacency[polyIdx] = list of edges to other polygons
    std::vector<std::vector<Edge>> adjacency;
    /// One representative vertex per polygon (nearest outer vertex to center)
    std::vector<std::shared_ptr<GPoint>> representatives;
    int numPolygons = 0;
};

struct Corridor
{
    double minLon, maxLon, minLat, maxLat;
    std::unordered_set<int> allowedVertexIndices;

    // Pre-computed adjacency within this corridor (for Level 0)
    QVector<std::shared_ptr<GPoint>> vertices;
    std::vector<std::vector<int>> adjacency;
    std::unordered_map<std::shared_ptr<GPoint>, int,
        GPoint::Hash, GPoint::Equal> vertexIndex;
    bool hasAdjacency = false;

    /// Edges penalized by iterative path validation (+1e9 cost).
    /// Key: edgeKey(u, v) where u, v are corridor vertex indices.
    std::unordered_set<long long> penalizedEdges;

    /** @brief Compute a unique symmetric key for an edge (u, v). */
    static long long edgeKey(int u, int v)
    {
        return static_cast<long long>(std::min(u, v)) * 1000000LL
               + std::max(u, v);
    }

    bool containsPoint(double lon, double lat) const
    {
        return lon >= minLon && lon <= maxLon &&
               lat >= minLat && lat <= maxLat;
    }
};

class HierarchicalVisibilityGraph : public QObject
{
    Q_OBJECT

public:
    static constexpr int NUM_LEVELS = 4;

    HierarchicalVisibilityGraph();

    HierarchicalVisibilityGraph(
        const QVector<std::shared_ptr<Polygon>>& polygons);

    ~HierarchicalVisibilityGraph();

    ShortestPathResult
    findShortestPath(const std::shared_ptr<GPoint>& start,
                     const std::shared_ptr<GPoint>& goal);

    ShortestPathResult
    findShortestPath(QVector<std::shared_ptr<GPoint>> mustTraversePoints);

    void loadSeaPortsPolygonCoordinates(
        QVector<std::shared_ptr<SeaPort>>& seaPorts);

    void addManualVisibleLine(const std::shared_ptr<GLine>& line);
    void clearManualLines();

    GPoint getMinMapPoint();
    GPoint getMaxMapPoint();

    bool isVisible(const std::shared_ptr<GPoint>& p1,
                   const std::shared_ptr<GPoint>& p2,
                   int level = 0) const;

    bool isSegmentVisible(const std::shared_ptr<GLine>& segment,
                          int level = 0) const;

    bool isSegmentVisible(const GLine& segment,
                          int level = 0) const;

    void clear();

    void setPolygons(const QVector<std::shared_ptr<Polygon>>& newPolygons);

    Quadtree* getLevel0Quadtree() const;

    void buildLevel0Adjacency();
    void buildLevel0AdjacencyAsync(const QString& cachePath = QString());
    bool saveAdjacencyCache(const QString& filePath) const;
    bool loadAdjacencyCache(const QString& filePath);

    bool enableWrapAround;

    std::unordered_set<std::shared_ptr<GLine>, GLine::Hash, GLine::Equal>
        manualLinesSet;
    std::unordered_map<std::shared_ptr<GPoint>,
        QVector<std::shared_ptr<GPoint>>,
        GPoint::Hash, GPoint::Equal> manualConnections;
    QVector<std::shared_ptr<GPoint>> manualPoints;
    std::unordered_set<std::shared_ptr<GPoint>,
        GPoint::Hash, GPoint::Equal> manualPointsSet;

    QVector<std::shared_ptr<Polygon>> polygons;

    std::shared_ptr<Polygon>
    findContainingPolygon(const std::shared_ptr<GPoint>& point) const;

    QVector<std::shared_ptr<Polygon>>
    findAllContainingPolygons(const std::shared_ptr<GPoint>& point) const;

    QVector<std::shared_ptr<GPoint>>
    connectWrapAroundPoints(
        const std::shared_ptr<GPoint>& point,
        const std::shared_ptr<GPoint>& goalPoint = nullptr);

    static bool shouldCrossAntimeridian(double startLon, double goalLon);

signals:
    void pathFindingProgress(int segmentIndex, int totalSegments,
                             double elapsedSeconds);

private:
    std::array<GraphLevel, NUM_LEVELS> mLevels;
    PolygonGraph mPolygonGraph;

    std::atomic<bool> mLevel0AdjReady{false};
    QString mLevel0CachePath;

    mutable QReadWriteLock mManualLock;

    static constexpr double PORTAL_ZONE_DEGREES = 30.0;
    static constexpr double PORTAL_LAT_TOLERANCE = 10.0;

    // Hierarchy shape factors — resolution-independent multipliers
    // applied to the data's average vertex spacing. Derived from proven
    // ne_10m values (avgSpacing ≈ 10km): L1=2km, L2=10km, L3=50km
    // tolerances; L0=50km, L1=200km, L2=500km, L3=2000km max distances.
    static constexpr double TOLERANCE_FACTORS[NUM_LEVELS] = {
        0.0, 0.2, 1.0, 5.0
    };
    static constexpr double DISTANCE_FACTORS[NUM_LEVELS] = {
        5.0, 20.0, 50.0, 200.0
    };

    // Dynamic level parameters computed from input data resolution
    double mLevelTolerances[NUM_LEVELS] = {};
    double mLevelMaxDistance[NUM_LEVELS] = {};

    // Per-level cap on cross-polygon edges per vertex
    static constexpr int LEVEL_MAX_CROSS_CHECKS[NUM_LEVELS] = {
        10, 20, 30, 50
    };

    /** @brief Compute dynamic level parameters from polygon vertex spacing. */
    void computeDynamicParameters();

    void buildAllLevels();
    void buildLevel(int idx);
    void buildAdjacencyForLevel(int idx);

    // -----------------------------------------------------------------
    // Phase-based adjacency construction helpers
    // -----------------------------------------------------------------

    /** @brief Phase 1: Add consecutive ring edges — O(n), no visibility. */
    void addBoundaryEdges(GraphLevel& level);

    /** @brief Phase 2: Add spatial-grid edges — O(n×k), skip-vis for L1+. */
    void addSpatialGridEdges(GraphLevel& level, int levelIdx,
                             double maxDist);

    /** @brief Phase 3: Add antimeridian bridging edges. */
    void addAntimeridianEdges(GraphLevel& level, int levelIdx,
                              double maxDist);

    // -----------------------------------------------------------------
    // Polygon graph (coarse L3 routing)
    // -----------------------------------------------------------------

    void buildPolygonGraph();

    ShortestPathResult polygonGraphSearch(
        const std::shared_ptr<GPoint>& start,
        const std::shared_ptr<GPoint>& goal);

    // -----------------------------------------------------------------
    // On-demand visibility with distance limits
    // -----------------------------------------------------------------

    /**
     * @brief Find nearest visible nodes with distance sorting and caps.
     *
     * Sorts candidates by fastDistance to @p node, checks at most
     * @p maxCheck nearest, and stops after finding @p maxFound visible.
     * Reusable by both within-polygon and between-polygon queries.
     */
    QVector<std::shared_ptr<GPoint>> findNearestVisibleNodes(
        const std::shared_ptr<GPoint>& node,
        QVector<std::shared_ptr<GPoint>>& candidates,
        int level,
        int maxCheck = 100,
        int maxFound = 10);

    /**
     * @brief Inject a point into a GraphLevel as a first-class vertex.
     *
     * Ensures that an L0-snapped user point exists as a graph vertex
     * at a coarse level, with adjacency to nearby ring vertices.
     * Enables consistent corridor guidance across all levels.
     *
     * @param point The L0-snapped point to inject
     * @param level The target GraphLevel index (1-3)
     * @return The vertex index in the level, or -1 if injection failed
     */
    int injectPointIntoLevel(const std::shared_ptr<GPoint>& point,
                             int level);

    /** @brief Core visibility implementation using GSegment for hot-path geometry. */
    bool isSegmentVisibleImpl(
        const GSegment& seg,
        const std::shared_ptr<GPoint>& startPt,
        const std::shared_ptr<GPoint>& endPt,
        int level) const;

    std::shared_ptr<GPoint> snapToWater(
        const std::shared_ptr<GPoint>& point, int level) const;

    ShortestPathResult aStarAtLevel(
        const std::shared_ptr<GPoint>& start,
        const std::shared_ptr<GPoint>& goal,
        int level,
        const Corridor* corridor = nullptr,
        const std::shared_ptr<GPoint>& preSnappedStart = nullptr,
        const std::shared_ptr<GPoint>& preSnappedGoal = nullptr);

    Corridor buildCorridor(
        const ShortestPathResult& coarsePath,
        int targetLevel,
        double expansion);

    void precomputeCorridorAdjacency(
        Corridor& corridor,
        const std::shared_ptr<GPoint>& start,
        const std::shared_ptr<GPoint>& goal,
        const Corridor* previousCorridor = nullptr);

    ShortestPathResult hierarchicalSearch(
        const std::shared_ptr<GPoint>& start,
        const std::shared_ptr<GPoint>& goal);

    ShortestPathResult findShortestPathHelper(
        QVector<std::shared_ptr<GPoint>> mustTraversePoints);

    bool isVisibleInSimplifiedPolygon(
        const std::shared_ptr<GPoint>& v1,
        const std::shared_ptr<GPoint>& v2,
        const std::shared_ptr<Polygon>& poly) const;

    ShortestPathResult reconstructPath(
        const std::unordered_map<
            std::shared_ptr<GPoint>, std::shared_ptr<GPoint>,
            GPoint::Hash, GPoint::Equal>& cameFrom,
        std::shared_ptr<GPoint> current,
        int level);

    QVector<std::shared_ptr<GPoint>> getVisibleNodesForPoint(
        const std::shared_ptr<GPoint>& node,
        int level,
        const Corridor* corridor = nullptr);

    QVector<std::shared_ptr<GPoint>>
    getVisibleNodesWithinPolygon(
        const std::shared_ptr<GPoint>& node,
        const std::shared_ptr<Polygon>& polygon);

    QVector<std::shared_ptr<GPoint>>
    getVisibleNodesBetweenPolygons(
        const std::shared_ptr<GPoint>& node,
        const QVector<std::shared_ptr<Polygon>>& allPolygons);
};

}; // namespace ShipNetSimCore
#endif // HIERARCHICALVISIBILITYGRAPH_H
