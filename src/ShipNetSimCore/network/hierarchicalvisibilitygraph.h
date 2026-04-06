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
#include <functional>
#include <optional>
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
 * @struct HVGDiagnostics
 * @brief Read-only snapshot of HVG internal state for diagnostic purposes.
 */
struct HVGDiagnostics
{
    struct LevelInfo
    {
        int vertexCount = 0;
        long long edgeCount = 0;   ///< Unique undirected edges
        double toleranceMeters = 0.0;
        double maxDistanceMeters = 0.0;
    };
    std::array<LevelInfo, 4> levels;
    bool level0AdjReady = false;
    bool coarseAdjReady = false;
    int polygonGraphPolygonCount = 0;
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
    std::vector<int> vertexHoleId;  ///< Maps vertex idx → hole index (-1 = outer ring)
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

    /// Coarse path waypoints that guided this corridor's creation.
    /// Used to project cross-water spine edges into the corridor.
    QVector<std::shared_ptr<GPoint>> coarseWaypoints;

    /// Edges penalized by iterative path validation (+1e9 cost).
    /// Key: edgeKey(u, v) where u, v are level vertex indices.
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

    /** @brief Flat (unconstrained) search on Level 0 — no hierarchy.
     *  Uses Dijkstra (h=0) for guaranteed optimality on the full L0 graph.
     *  Intended for baseline comparison benchmarks. */
    ShortestPathResult
    findShortestPathFlat(const std::shared_ptr<GPoint>& start,
                         const std::shared_ptr<GPoint>& goal);

    /** @brief Flat (unconstrained) A* on Level 0 — no hierarchy.
     *  Uses haversine heuristic for directional guidance but no corridors.
     *  Intended for baseline comparison benchmarks. */
    ShortestPathResult
    findShortestPathFlatAStar(const std::shared_ptr<GPoint>& start,
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
    void buildAllAdjacency();
    bool saveAdjacencyCache(const QString& filePath) const;
    bool loadAdjacencyCache(const QString& filePath);

    /** @brief Return a diagnostic snapshot of internal state. */
    HVGDiagnostics getDiagnostics() const;

    /** @brief Read-only access to a graph level for diagnostics/export. */
    const GraphLevel& getLevel(int idx) const { return mLevels[idx]; }

    /** @brief Read-only access to the polygon list. */
    const QVector<std::shared_ptr<Polygon>>& getPolygons() const
    { return polygons; }

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
    bool mCoarseAdjReady = false;   ///< true when L1-L3 adjacency available
    QString mLevel0CachePath;

    mutable QReadWriteLock mManualLock;

    // ----- Encapsulated data (formerly public) -----
    bool enableWrapAround = false;

    std::unordered_set<std::shared_ptr<GLine>, GLine::Hash, GLine::Equal>
        manualLinesSet;
    std::unordered_map<std::shared_ptr<GPoint>,
        QVector<std::shared_ptr<GPoint>>,
        GPoint::Hash, GPoint::Equal> manualConnections;
    QVector<std::shared_ptr<GPoint>> manualPoints;
    std::unordered_set<std::shared_ptr<GPoint>,
        GPoint::Hash, GPoint::Equal> manualPointsSet;

    QVector<std::shared_ptr<Polygon>> polygons;

    // ----- Physical / geometric constants -----
    static constexpr double METERS_PER_DEGREE_LAT = 111000.0;
    static constexpr double PORTAL_ZONE_DEGREES = 30.0;
    static constexpr double PORTAL_LAT_TOLERANCE = 10.0;

    // ----- Algorithm constants -----
    static constexpr int MAX_INJECT_NEIGHBORS = 8;
    static constexpr int ON_DEMAND_MAX_CHECK = 500;
    static constexpr int ON_DEMAND_MAX_FOUND = 20;
    static constexpr double SIMPLIFIED_COORD_TOL = 0.00001;

    // =============================================================
    // Data Resolution Parameters — scale with input vertex spacing
    // =============================================================

    /// Simplification tolerance multipliers applied to avgSpacing.
    static constexpr double TOLERANCE_FACTORS[NUM_LEVELS] = {
        0.0, 0.2, 1.0, 5.0
    };

    /// Dynamic tolerances computed from input data resolution.
    double mLevelTolerances[NUM_LEVELS] = {};

    /// Average outer-ring edge length (meters). Set in computeDynamicParameters().
    /// Used by data-adaptive algorithms (local visible, Yao spanner).
    double mAvgSpacing = 10000.0;

    /// Number of angular cones for Yao-style directional searches.
    /// k=8 gives t-spanner with stretch ≤ 1/(1-2sin(π/8)) ≈ 4.6.
    static constexpr int YAO_CONE_COUNT = 8;

    /// Express vertex step: sample every N-th vertex on large rings.
    /// Controls intra-ring express density (lower = denser, slower build).
    static constexpr int EXPRESS_VERTEX_STEP = 100;

    // =============================================================
    // Routing Parameters — absolute values (meters) for global
    // maritime routing. Independent of data resolution.
    // =============================================================

    /// Maximum adjacency edge distance per level.
    static constexpr double LEVEL_MAX_DISTANCE[NUM_LEVELS] = {
        50000.0, 2000000.0, 5000000.0, 2000000.0
    };

    /// Corridor tube width per level. Controls how wide the search
    /// area is around the coarse path. Wider fallbacks (3x, 10x)
    /// are applied automatically when the initial corridor fails.
    static constexpr double CORRIDOR_EXPANSION[NUM_LEVELS] = {
        6000.0, 30000.0, 150000.0, 0.0
    };

    /// Per-level cap on cross-polygon edges per vertex.
    static constexpr int LEVEL_MAX_CROSS_CHECKS[NUM_LEVELS] = {
        10, 20, 30, 50
    };

    /// Runtime copy of max distances (initialized from LEVEL_MAX_DISTANCE).
    double mLevelMaxDistance[NUM_LEVELS] = {};

    /** @brief Compute dynamic level parameters from polygon vertex spacing. */
    void computeDynamicParameters();

    void buildAllLevelVertices();
    void buildCoarseLevelAdjacency();
    void ensureCoarseAdjacency();
    void buildLevel(int idx);
    void buildAdjacencyForLevel(int idx);

    // ----- Cache serialization helpers -----
    bool writeLevelAdjacency(QDataStream& out, int level) const;
    bool readLevelAdjacency(QDataStream& in, int level);

    // -----------------------------------------------------------------
    // Phase-based adjacency construction helpers
    // -----------------------------------------------------------------

    /** @brief Phase 1: Add consecutive ring edges — O(n), no visibility. */
    void addBoundaryEdges(GraphLevel& level);

    /** @brief Phase 2: Add spatial-grid edges — O(n×k).
     *  @param checkVisibility When true, only add edges that pass
     *         isVisible(). When false (default), skip-vis for corridor guidance.
     */
    void addSpatialGridEdges(GraphLevel& level, int levelIdx,
                             double maxDist,
                             bool checkVisibility = false);

    /** @brief Shared helper: find nearest visible vertex in each uncovered
     *  octant via expanding Quadtree search. Applies candidateFilter to
     *  exclude unwanted candidates (same ring, same component, etc.).
     *  @param waterArc Optional angular range (center, halfWidth) in radians.
     *         When set, only cones overlapping this arc are searched.
     *  @return Number of edges added. */
    int connectOctantNearest(
        GraphLevel& level, int levelIdx,
        int sourceIdx,
        const std::vector<double>& lons,
        const std::vector<double>& lats,
        double initRadiusDeg, double maxSearchDeg,
        const std::function<bool(int)>& candidateFilter,
        std::optional<std::pair<double, double>> waterArc = std::nullopt);

    /** @brief Compute water-facing angular arc for a ring boundary vertex.
     *  Uses ring neighbors and isPointWithinPolygon bisector test.
     *  @return (arcCenter, arcHalfWidth) in radians, or nullopt if not a ring vertex. */
    std::optional<std::pair<double, double>> computeWaterArc(
        int vertexIdx, int level,
        const std::vector<double>& lons,
        const std::vector<double>& lats) const;

    /** @brief Phase 2 (L0): Add express edges on large rings.
     *  Samples evenly-spaced vertices on rings larger than YAO_CONE_COUNT
     *  and adds Yao-k octant-nearest visible edges. Creates "highway" edges
     *  along long coastlines so A* doesn't follow every boundary edge.
     *  No distance limit — expanding Quadtree search. */
    void addIntraRingExpressEdges(GraphLevel& level, int levelIdx);

    /** @brief Phase 3 (L0): Bridge nearby ring pairs with directional edges.
     *  For each pair of geographically nearby rings, finds directional
     *  bridges from representative vertices (N/S/E/W extremes).
     *  Uses HoleSpatialIndex + RingRange for ring-pair discovery. */
    void addInterRingBridges(GraphLevel& level, int levelIdx);

    /** @brief Phase 4 (L0): Merge disconnected components using Borůvka-like
     *  nearest-visible edge merging with directional (octant) coverage.
     *  Components halve each iteration → O(log C) iterations.
     *  Guarantees single connected component. */
    void bridgeConnectedComponents(GraphLevel& level, int levelIdx);

    /** @brief Phase 5: Add antimeridian bridging edges.
     *  @param checkVisibility When true (L0), only add edges that pass
     *         isVisible(). When false (L1-L3), skip-vis for corridor guidance.
     */
    void addAntimeridianEdges(GraphLevel& level, int levelIdx,
                              double maxDist,
                              bool checkVisibility = false);

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

    /** @brief Core graph search — A* (useHeuristic=true) or Dijkstra (false).
     *  @param useHeuristic  true = haversine A*, false = Dijkstra (h=0)
     *  @param maxExpansions  -1 = unlimited, >0 = budget (returns invalid if exceeded)
     */
    ShortestPathResult searchAtLevel(
        const std::shared_ptr<GPoint>& start,
        const std::shared_ptr<GPoint>& goal,
        int level,
        const Corridor* corridor = nullptr,
        const std::shared_ptr<GPoint>& preSnappedStart = nullptr,
        const std::shared_ptr<GPoint>& preSnappedGoal = nullptr,
        bool useHeuristic = true,
        int maxExpansions = -1);

    Corridor buildCorridor(
        const ShortestPathResult& coarsePath,
        int targetLevel,
        double expansion,
        bool useTubeFilter = true);

    /** @brief Get ring neighbor vertex indices for a vertex in a GraphLevel.
     *  @return (prevIdx, nextIdx) in GraphLevel::vertices, or (-1,-1) if invalid.
     */
    std::pair<int,int> getRingNeighbors(int vertexIdx, int level) const;

    /** @brief Bridge disconnected L0 segments via directional visibility
     *         from ring topology. Called when cached bbox corridors all fail.
     */
    ShortestPathResult bridgeViaDirectionalVisibility(
        const ShortestPathResult& coarsePath,
        const std::shared_ptr<GPoint>& start,
        const std::shared_ptr<GPoint>& goal,
        const std::shared_ptr<GPoint>& snappedStart,
        const std::shared_ptr<GPoint>& snappedGoal);

    /** @brief Build corridor adjacency via corridor-local grid.
     *
     *  Phase 1: Ring boundary edges (O(n), zero visibility checks)
     *  Phase 2: Corridor-local hash grid cross-ring bridges (O(n×k))
     *  Phase 3: BFS connectivity verification with adaptive expansion
     *  Phase 4: Start/goal endpoint connectivity
     *
     *  Data-adaptive: cell size from mAvgSpacing, works on any dataset.
     *  All spatial indexing uses corridor-local grid, never the level
     *  quadtree — avoids O(n_level) overhead for corridor-scale work.
     */
    void buildCorridorAdjacencyPhased(
        Corridor& corridor,
        const std::shared_ptr<GPoint>& start,
        const std::shared_ptr<GPoint>& goal,
        const std::shared_ptr<GPoint>& snappedStart,
        const std::shared_ptr<GPoint>& snappedGoal,
        int level = 0);

    ShortestPathResult hierarchicalSearch(
        const std::shared_ptr<GPoint>& start,
        const std::shared_ptr<GPoint>& goal);

    ShortestPathResult findShortestPathHelper(
        QVector<std::shared_ptr<GPoint>> mustTraversePoints);

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
