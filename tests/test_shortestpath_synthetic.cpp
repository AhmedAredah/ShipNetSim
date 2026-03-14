#include "network/gline.h"
#include "network/gpoint.h"
#include "network/hierarchicalvisibilitygraph.h"
#include "network/polygon.h"
#include <QDebug>
#include <QTest>
#include <cmath>

using namespace ShipNetSimCore;

class SyntheticShortestPathTest : public QObject
{
    Q_OBJECT

private slots:
    // 2-point API tests (exercise full hierarchical search L3→L0)
    void testOpenWaterDirect();
    void testAroundRectObstacle();
    void testAroundLShapedObstacle();
    void testThroughNarrowChannel();
    void testBlockedByWall();
    void testMultipleObstacles();
    void testDiagonalPath();

    // Edge / degenerate cases
    void testStartEqualsGoal();
    void testStartOnBoundaryVertex();
    void testGoalInsideHole();

    // Multi-waypoint API tests
    void testEmptyWaypoints();
    void testSingleWaypoint();
    void testMultiWaypoint4Points();
    void testMultiWaypointReversed();

    // API asymmetry documentation
    void testDirectVisibilityShortcut();

private:
    std::shared_ptr<GPoint> makePoint(double lon, double lat,
                                      const QString &id);
    double computeTotalPathLengthKm(const ShortestPathResult &result);
    bool verifyPathContinuity(const ShortestPathResult &result);
    void verifyPathValid(const ShortestPathResult &result,
                         const std::shared_ptr<GPoint> &start,
                         const std::shared_ptr<GPoint> &goal,
                         double minKm, double maxKm);

    std::shared_ptr<Polygon>
    createPolygonWithHoles(
        const QVector<std::shared_ptr<GPoint>> &outer,
        const QVector<QVector<std::shared_ptr<GPoint>>> &holes,
        const QString &id);
};

// ---------------------------------------------------------------------------
// Static helpers for creating subdivided polygon rings.
// The HVG needs enough vertices for graph building — simple 4-vertex
// rectangles are too sparse. We subdivide edges every ~0.5 degrees.
// ---------------------------------------------------------------------------

static int gPointCounter = 0;

static std::shared_ptr<GPoint>
smp(double lon, double lat, const QString &prefix)
{
    return std::make_shared<GPoint>(
        units::angle::degree_t(lon), units::angle::degree_t(lat),
        QString("%1_%2").arg(prefix).arg(gPointCounter++));
}

static void subdivideEdge(QVector<std::shared_ptr<GPoint>> &ring,
                           double lon0, double lat0,
                           double lon1, double lat1,
                           const QString &prefix, double step = 0.5)
{
    double dLon = lon1 - lon0;
    double dLat = lat1 - lat0;
    double len  = std::sqrt(dLon * dLon + dLat * dLat);
    int n = std::max(1, static_cast<int>(std::ceil(len / step)));

    for (int i = 0; i < n; ++i) {
        double t   = static_cast<double>(i) / n;
        double lon = lon0 + t * dLon;
        double lat = lat0 + t * dLat;
        ring.append(smp(lon, lat, prefix));
    }
}

// Clockwise hole ring with subdivided edges
static QVector<std::shared_ptr<GPoint>>
makeRectHole(double lonMin, double lonMax, double latMin, double latMax,
             const QString &prefix, double step = 0.5)
{
    QVector<std::shared_ptr<GPoint>> h;
    subdivideEdge(h, lonMin, latMin, lonMin, latMax, prefix, step);
    subdivideEdge(h, lonMin, latMax, lonMax, latMax, prefix, step);
    subdivideEdge(h, lonMax, latMax, lonMax, latMin, prefix, step);
    subdivideEdge(h, lonMax, latMin, lonMin, latMin, prefix, step);
    h.append(h.first());
    return h;
}

// Counter-clockwise outer ring with subdivided edges
static QVector<std::shared_ptr<GPoint>>
makeRectOuter(double lonMin, double lonMax, double latMin, double latMax,
              const QString &prefix, double step = 0.5)
{
    QVector<std::shared_ptr<GPoint>> b;
    subdivideEdge(b, lonMin, latMin, lonMax, latMin, prefix, step);
    subdivideEdge(b, lonMax, latMin, lonMax, latMax, prefix, step);
    subdivideEdge(b, lonMax, latMax, lonMin, latMax, prefix, step);
    subdivideEdge(b, lonMin, latMax, lonMin, latMin, prefix, step);
    b.append(b.first());
    return b;
}

// ---------------------------------------------------------------------------
// Member helpers
// ---------------------------------------------------------------------------

std::shared_ptr<GPoint>
SyntheticShortestPathTest::makePoint(double lon, double lat,
                                     const QString &id)
{
    return std::make_shared<GPoint>(units::angle::degree_t(lon),
                                    units::angle::degree_t(lat), id);
}

double SyntheticShortestPathTest::computeTotalPathLengthKm(
    const ShortestPathResult &result)
{
    double totalM = 0.0;
    for (const auto &line : result.lines) {
        totalM += line->length().value();
    }
    return totalM / 1000.0;
}

bool SyntheticShortestPathTest::verifyPathContinuity(
    const ShortestPathResult &result)
{
    if (result.lines.size() < 2)
        return true;
    for (int i = 0; i < result.lines.size() - 1; ++i) {
        auto end   = result.lines[i]->endPoint();
        auto start = result.lines[i + 1]->startPoint();
        if (*end != *start) {
            qWarning() << "Path discontinuity at segment" << i
                        << "end:" << end->getLongitude().value()
                        << end->getLatitude().value()
                        << "next start:" << start->getLongitude().value()
                        << start->getLatitude().value();
            return false;
        }
    }
    return true;
}

void SyntheticShortestPathTest::verifyPathValid(
    const ShortestPathResult &result,
    const std::shared_ptr<GPoint> &start,
    const std::shared_ptr<GPoint> &goal, double minKm, double maxKm)
{
    QVERIFY2(result.isValid(), "Path result is not valid");
    QVERIFY2(result.points.size() >= 2,
             "Path must have at least 2 points");
    QCOMPARE(result.lines.size(), result.points.size() - 1);

    double startDistKm =
        result.points.first()->distance(*start).value() / 1000.0;
    QVERIFY2(startDistKm < 50.0,
             qPrintable(QString("Path start %1 km from origin (max 50)")
                            .arg(startDistKm)));

    double endDistKm =
        result.points.last()->distance(*goal).value() / 1000.0;
    QVERIFY2(endDistKm < 50.0,
             qPrintable(QString("Path end %1 km from dest (max 50)")
                            .arg(endDistKm)));

    double totalKm = computeTotalPathLengthKm(result);
    QVERIFY2(totalKm >= minKm,
             qPrintable(QString("Path too short: %1 km < min %2 km")
                            .arg(totalKm).arg(minKm)));
    QVERIFY2(totalKm <= maxKm,
             qPrintable(QString("Path too long: %1 km > max %2 km")
                            .arg(totalKm).arg(maxKm)));

    QVERIFY2(verifyPathContinuity(result), "Path is not continuous");
}

std::shared_ptr<Polygon>
SyntheticShortestPathTest::createPolygonWithHoles(
    const QVector<std::shared_ptr<GPoint>> &outer,
    const QVector<QVector<std::shared_ptr<GPoint>>> &holes,
    const QString &id)
{
    return std::make_shared<Polygon>(outer, holes, id);
}

// ---------------------------------------------------------------------------
// 2-point API tests — exercise full hierarchical search (L3→L2→L1→L0)
// ---------------------------------------------------------------------------

void SyntheticShortestPathTest::testOpenWaterDirect()
{
    auto outer = makeRectOuter(0, 10, 0, 10, "Outer");
    auto poly = std::make_shared<Polygon>(
        outer, QVector<QVector<std::shared_ptr<GPoint>>>(), "OpenWater");

    QVector<std::shared_ptr<Polygon>> polys;
    polys.append(poly);
    HierarchicalVisibilityGraph hvg(polys);

    auto start = makePoint(2, 5, "S");
    auto goal  = makePoint(8, 5, "G");

    auto result = hvg.findShortestPath(start, goal);

    // BUG 2 fixed: goal pre-connected to graph via getVisibleNodesForPoint
    verifyPathValid(result, start, goal, 400, 800);
}

void SyntheticShortestPathTest::testAroundRectObstacle()
{
    auto outer = makeRectOuter(0, 10, 0, 10, "Outer");
    auto hole  = makeRectHole(4, 6, 3, 7, "Hole");

    QVector<QVector<std::shared_ptr<GPoint>>> holes;
    holes.append(hole);
    auto poly = createPolygonWithHoles(outer, holes, "WithRect");

    QVector<std::shared_ptr<Polygon>> polys;
    polys.append(poly);
    HierarchicalVisibilityGraph hvg(polys);

    auto start = makePoint(2, 5, "S");
    auto goal  = makePoint(8, 5, "G");

    auto result = hvg.findShortestPath(start, goal);

    // BUG 2 fixed: goal pre-connected to graph via getVisibleNodesForPoint
    double straightKm = start->distance(*goal).value() / 1000.0;
    double pathKm     = computeTotalPathLengthKm(result);
    QVERIFY2(pathKm > straightKm,
             "Path should detour around obstacle");
    verifyPathValid(result, start, goal, straightKm * 0.9,
                    straightKm * 3.0);
}

void SyntheticShortestPathTest::testAroundLShapedObstacle()
{
    auto outer = makeRectOuter(0, 10, 0, 10, "Outer");

    QVector<std::shared_ptr<GPoint>> lHole;
    subdivideEdge(lHole, 3, 4, 3, 6, "L");
    subdivideEdge(lHole, 3, 6, 5, 6, "L");
    subdivideEdge(lHole, 5, 6, 5, 8, "L");
    subdivideEdge(lHole, 5, 8, 7, 8, "L");
    subdivideEdge(lHole, 7, 8, 7, 4, "L");
    subdivideEdge(lHole, 7, 4, 3, 4, "L");
    lHole.append(lHole.first());

    QVector<QVector<std::shared_ptr<GPoint>>> holes;
    holes.append(lHole);
    auto poly = createPolygonWithHoles(outer, holes, "WithL");

    QVector<std::shared_ptr<Polygon>> polys;
    polys.append(poly);
    HierarchicalVisibilityGraph hvg(polys);

    auto start = makePoint(4, 7, "S");
    auto goal  = makePoint(8, 5, "G");

    auto result = hvg.findShortestPath(start, goal);

    // BUG 2 fixed: goal pre-connected to graph via getVisibleNodesForPoint
    verifyPathValid(result, start, goal, 200, 1500);
}

void SyntheticShortestPathTest::testThroughNarrowChannel()
{
    auto outer = makeRectOuter(0, 10, 0, 10, "Outer");
    auto hole1 = makeRectHole(4, 6, 0.5, 4.85, "H1");
    auto hole2 = makeRectHole(4, 6, 5.15, 9.5, "H2");

    QVector<QVector<std::shared_ptr<GPoint>>> holes;
    holes.append(hole1);
    holes.append(hole2);
    auto poly = createPolygonWithHoles(outer, holes, "NarrowChan");

    QVector<std::shared_ptr<Polygon>> polys;
    polys.append(poly);
    HierarchicalVisibilityGraph hvg(polys);

    auto start = makePoint(2, 5, "S");
    auto goal  = makePoint(8, 5, "G");

    auto result = hvg.findShortestPath(start, goal);

    // BUG 2 fixed: goal pre-connected to graph via getVisibleNodesForPoint
    verifyPathValid(result, start, goal, 400, 1000);
}

void SyntheticShortestPathTest::testBlockedByWall()
{
    auto outer = makeRectOuter(0, 10, 0, 10, "Outer");
    auto wall = makeRectHole(0.5, 9.5, 4.5, 5.5, "Wall");

    QVector<QVector<std::shared_ptr<GPoint>>> holes;
    holes.append(wall);
    auto poly = createPolygonWithHoles(outer, holes, "WallBlock");

    QVector<std::shared_ptr<Polygon>> polys;
    polys.append(poly);
    HierarchicalVisibilityGraph hvg(polys);

    auto start = makePoint(5, 2, "S");
    auto goal  = makePoint(5, 8, "G");

    auto result = hvg.findShortestPath(start, goal);

    // BUG 2 fixed: goal pre-connected to graph via getVisibleNodesForPoint
    double straightKm = start->distance(*goal).value() / 1000.0;
    double pathKm     = computeTotalPathLengthKm(result);
    QVERIFY2(pathKm > straightKm * 1.1,
             "Path should detour around wall");
    verifyPathValid(result, start, goal, straightKm * 0.9,
                    straightKm * 5);
}

void SyntheticShortestPathTest::testMultipleObstacles()
{
    auto outer = makeRectOuter(0, 10, 0, 10, "Outer");
    auto h1    = makeRectHole(1.5, 3, 1.5, 3.5, "H1");
    auto h2    = makeRectHole(4.5, 6, 5, 7, "H2");
    auto h3    = makeRectHole(7, 8.5, 2, 4, "H3");

    QVector<QVector<std::shared_ptr<GPoint>>> holes;
    holes.append(h1);
    holes.append(h2);
    holes.append(h3);
    auto poly = createPolygonWithHoles(outer, holes, "Multi");

    QVector<std::shared_ptr<Polygon>> polys;
    polys.append(poly);
    HierarchicalVisibilityGraph hvg(polys);

    auto start = makePoint(0.5, 0.5, "S");
    auto goal  = makePoint(9.5, 9.5, "G");

    auto result = hvg.findShortestPath(start, goal);

    // BUG 2 fixed: goal pre-connected to graph via getVisibleNodesForPoint
    verifyPathValid(result, start, goal, 800, 2500);
}

void SyntheticShortestPathTest::testDiagonalPath()
{
    auto outer = makeRectOuter(0, 10, 0, 10, "Outer");
    auto hole = makeRectHole(6, 8, 2, 4, "OffCenter");
    QVector<QVector<std::shared_ptr<GPoint>>> holes;
    holes.append(hole);
    auto poly = createPolygonWithHoles(outer, holes, "Diag");

    QVector<std::shared_ptr<Polygon>> polys;
    polys.append(poly);
    HierarchicalVisibilityGraph hvg(polys);

    auto start = makePoint(1, 1, "S");
    auto goal  = makePoint(9, 9, "G");

    auto result = hvg.findShortestPath(start, goal);

    // BUG 2 fixed: goal pre-connected to graph via getVisibleNodesForPoint
    double straightKm = start->distance(*goal).value() / 1000.0;
    verifyPathValid(result, start, goal, straightKm * 0.9,
                    straightKm * 2.0);
}

// ---------------------------------------------------------------------------
// Edge / degenerate cases
// ---------------------------------------------------------------------------

void SyntheticShortestPathTest::testStartEqualsGoal()
{
    auto outer = makeRectOuter(0, 10, 0, 10, "Outer");
    auto poly = std::make_shared<Polygon>(
        outer, QVector<QVector<std::shared_ptr<GPoint>>>(), "Simple");
    QVector<std::shared_ptr<Polygon>> polys;
    polys.append(poly);
    HierarchicalVisibilityGraph hvg(polys);

    auto point = makePoint(5, 5, "P");
    auto result = hvg.findShortestPath(point, point);

    // aStarAtLevel returns early for start==goal (line 402-407)
    // Result has 1 point, no lines — isValid() returns false but no crash
    qDebug() << "StartEqualsGoal: valid=" << result.isValid()
             << "points=" << result.points.size();
}

void SyntheticShortestPathTest::testStartOnBoundaryVertex()
{
    auto outer = makeRectOuter(0, 10, 0, 10, "Outer");
    auto hole  = makeRectHole(4, 6, 4, 6, "Hole");
    QVector<QVector<std::shared_ptr<GPoint>>> holes;
    holes.append(hole);
    auto poly = createPolygonWithHoles(outer, holes, "BndTest");

    QVector<std::shared_ptr<Polygon>> polys;
    polys.append(poly);
    HierarchicalVisibilityGraph hvg(polys);

    // Start exactly on a hole boundary vertex — this IS a graph vertex,
    // so it will be in the vertex index and use pre-computed adjacency
    auto start = makePoint(4, 4, "S");
    // Goal is also a boundary vertex (on outer edge)
    auto goal  = makePoint(10, 5, "G");

    auto result = hvg.findShortestPath(start, goal);

    // Both start and goal are boundary vertices → should be in the graph
    // → A* can find them without the 50km proximity check
    if (result.isValid()) {
        verifyPathValid(result, start, goal, 400, 1200);
        qDebug() << "StartOnBoundary: valid path,"
                 << computeTotalPathLengthKm(result) << "km";
    } else {
        qDebug() << "StartOnBoundary: invalid (start/goal may not "
                    "exactly match graph vertices)";
    }
}

void SyntheticShortestPathTest::testGoalInsideHole()
{
    auto outer = makeRectOuter(0, 10, 0, 10, "Outer");
    auto hole  = makeRectHole(4, 6, 4, 6, "Hole");
    QVector<QVector<std::shared_ptr<GPoint>>> holes;
    holes.append(hole);
    auto poly = createPolygonWithHoles(outer, holes, "LandGoal");

    QVector<std::shared_ptr<Polygon>> polys;
    polys.append(poly);
    HierarchicalVisibilityGraph hvg(polys);

    auto start = makePoint(2, 2, "S");
    auto goal  = makePoint(5, 5, "G"); // inside hole (land)

    // Must not crash — goal snaps to nearest hole vertex via snapToWater
    auto result = hvg.findShortestPath(start, goal);
    qDebug() << "GoalInsideHole: valid=" << result.isValid()
             << "points=" << result.points.size();
}

// ---------------------------------------------------------------------------
// Multi-waypoint API tests — uses findShortestPathHelper() which has
// isSegmentVisible shortcut that bypasses hierarchical search
// ---------------------------------------------------------------------------

void SyntheticShortestPathTest::testEmptyWaypoints()
{
    auto outer = makeRectOuter(0, 10, 0, 10, "Outer");
    auto poly = std::make_shared<Polygon>(
        outer, QVector<QVector<std::shared_ptr<GPoint>>>(), "Simple");
    QVector<std::shared_ptr<Polygon>> polys;
    polys.append(poly);
    HierarchicalVisibilityGraph hvg(polys);

    QVector<std::shared_ptr<GPoint>> empty;
    auto result = hvg.findShortestPath(empty);
    qDebug() << "EmptyWaypoints: valid=" << result.isValid();
}

void SyntheticShortestPathTest::testSingleWaypoint()
{
    auto outer = makeRectOuter(0, 10, 0, 10, "Outer");
    auto poly = std::make_shared<Polygon>(
        outer, QVector<QVector<std::shared_ptr<GPoint>>>(), "Simple");
    QVector<std::shared_ptr<Polygon>> polys;
    polys.append(poly);
    HierarchicalVisibilityGraph hvg(polys);

    QVector<std::shared_ptr<GPoint>> single;
    single.append(makePoint(5, 5, "P"));
    auto result = hvg.findShortestPath(single);
    qDebug() << "SingleWaypoint: valid=" << result.isValid();
}

void SyntheticShortestPathTest::testMultiWaypoint4Points()
{
    auto outer = makeRectOuter(0, 10, 0, 10, "Outer");
    auto hole  = makeRectHole(4, 6, 4, 6, "Hole");
    QVector<QVector<std::shared_ptr<GPoint>>> holes;
    holes.append(hole);
    auto poly = createPolygonWithHoles(outer, holes, "MW");

    QVector<std::shared_ptr<Polygon>> polys;
    polys.append(poly);
    HierarchicalVisibilityGraph hvg(polys);

    QVector<std::shared_ptr<GPoint>> waypoints;
    waypoints.append(makePoint(1, 1, "W1"));
    waypoints.append(makePoint(1, 9, "W2"));
    waypoints.append(makePoint(9, 9, "W3"));
    waypoints.append(makePoint(9, 1, "W4"));

    auto result = hvg.findShortestPath(waypoints);

    QVERIFY2(result.isValid(), "Multi-waypoint path should be valid");
    QVERIFY2(verifyPathContinuity(result),
             "Multi-waypoint path should be continuous");
    double km = computeTotalPathLengthKm(result);
    QVERIFY2(km > 100, "Multi-waypoint path should have meaningful length");
    qDebug() << "MultiWaypoint4: " << km << "km,"
             << result.points.size() << "points";
}

void SyntheticShortestPathTest::testMultiWaypointReversed()
{
    auto outer = makeRectOuter(0, 10, 0, 10, "Outer");
    auto hole  = makeRectHole(4, 6, 4, 6, "Hole");
    QVector<QVector<std::shared_ptr<GPoint>>> holes;
    holes.append(hole);
    auto poly = createPolygonWithHoles(outer, holes, "MWR");

    QVector<std::shared_ptr<Polygon>> polys;
    polys.append(poly);
    HierarchicalVisibilityGraph hvg(polys);

    QVector<std::shared_ptr<GPoint>> forward;
    forward.append(makePoint(1, 1, "W1"));
    forward.append(makePoint(1, 9, "W2"));
    forward.append(makePoint(9, 9, "W3"));
    forward.append(makePoint(9, 1, "W4"));

    QVector<std::shared_ptr<GPoint>> reversed;
    reversed.append(makePoint(9, 1, "W4r"));
    reversed.append(makePoint(9, 9, "W3r"));
    reversed.append(makePoint(1, 9, "W2r"));
    reversed.append(makePoint(1, 1, "W1r"));

    auto fwdResult = hvg.findShortestPath(forward);
    auto revResult = hvg.findShortestPath(reversed);

    QVERIFY2(fwdResult.isValid(), "Forward path should be valid");
    QVERIFY2(revResult.isValid(), "Reversed path should be valid");

    double fwdKm = computeTotalPathLengthKm(fwdResult);
    double revKm = computeTotalPathLengthKm(revResult);

    double ratio = fwdKm / revKm;
    QVERIFY2(ratio > 0.9 && ratio < 1.1,
             qPrintable(QString("Forward/reverse ratio %1 (fwd=%2, rev=%3)")
                            .arg(ratio).arg(fwdKm).arg(revKm)));
    qDebug() << "MultiWaypointReversed: fwd=" << fwdKm
             << "km, rev=" << revKm << "km";
}

// ---------------------------------------------------------------------------
// API asymmetry documentation test
// ---------------------------------------------------------------------------

void SyntheticShortestPathTest::testDirectVisibilityShortcut()
{
    // Documents the asymmetry between 2-point and multi-waypoint APIs.
    // Multi-waypoint checks isSegmentVisible first and bypasses
    // hierarchical search when direct line is visible.
    auto outer = makeRectOuter(0, 10, 0, 10, "Outer");
    auto poly = std::make_shared<Polygon>(
        outer, QVector<QVector<std::shared_ptr<GPoint>>>(), "Shortcut");

    QVector<std::shared_ptr<Polygon>> polys;
    polys.append(poly);
    HierarchicalVisibilityGraph hvg(polys);

    auto start = makePoint(2, 5, "S");
    auto goal  = makePoint(8, 5, "G");

    // 2-point API: now has direct visibility check before hierarchicalSearch
    auto result2pt = hvg.findShortestPath(start, goal);

    // Multi-waypoint API: checks isSegmentVisible first → direct line
    QVector<std::shared_ptr<GPoint>> waypoints{start, goal};
    auto resultMW = hvg.findShortestPath(waypoints);

    // Both APIs should succeed after BUG 2 fix
    QVERIFY2(resultMW.isValid(),
             "Multi-waypoint should find direct visible path");
    QVERIFY2(result2pt.isValid(),
             "2-point should find direct visible path (Part A fix)");

    // Both should produce similar path lengths
    double km2pt = computeTotalPathLengthKm(result2pt);
    double kmMW  = computeTotalPathLengthKm(resultMW);
    double ratio = km2pt / kmMW;
    QVERIFY2(ratio > 0.8 && ratio < 1.2,
             qPrintable(QString("API length mismatch: 2pt=%1km, mw=%2km")
                            .arg(km2pt).arg(kmMW)));
    qDebug() << "DirectVisibilityShortcut: 2pt=" << km2pt
             << "km, mw=" << kmMW << "km";
}

QTEST_MAIN(SyntheticShortestPathTest)
#include "test_shortestpath_synthetic.moc"
