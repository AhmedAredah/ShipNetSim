#include "network/gline.h"
#include "network/gpoint.h"
#include "network/hierarchicalvisibilitygraph.h"
#include "network/optimizednetwork.h"
#include "network/polygon.h"
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTest>
#include <cmath>
#include <limits>

using namespace ShipNetSimCore;

class RealWorldShortestPathTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    // Short routes (<500km)
    void testShort_Antwerpen_Zeebrugge();
    void testShort_Santos_Paranagua();
    void testShort_NewYork_Boston();

    // Medium routes (500-3000km)
    void testMedium_Brisbane_Newcastle();
    void testMedium_Rotterdam_Algeciras();
    void testMedium_NewYork_Colon();

    // Long routes (3000-7000km)
    void testLong_Colombo_Jeddah();
    void testLong_Singapore_Busan();
    void testLong_Rotterdam_NewYork();
    void testLong_Santos_CapeTown();

    // Very long routes (>7000km)
    void testVeryLong_Singapore_PortSaid();
    void testVeryLong_Yokohama_LongBeach();

    // Edge cases on real data
    void testAntimeridian_Yokohama_LongBeach();
    void testNearCoast_Dover_Calais();
    void testVeryClose_OpenOcean();
    void testNarrowStrait_Malacca();
    void testNarrowStrait_SuezApproach();
    void testSameStartEnd();
    void testMultiWaypoint_5Ports();
    void testSymmetry_Rotterdam_NY();
    void testSymmetry_Singapore_Busan();

    // Diagnostic tests — investigate WHY tests fail at a given resolution
    void testDiag_PointContainment();
    void testDiag_FailingRouteDetails();
    void testDiag_ExportGeoJSON();

private:
    std::shared_ptr<GPoint> makePoint(double lon, double lat,
                                      const QString &id);
    double computeTotalPathLengthKm(const ShortestPathResult &result);
    bool verifyPathContinuity(const ShortestPathResult &result);
    void verifyPathValid(const ShortestPathResult &result,
                         const std::shared_ptr<GPoint> &start,
                         const std::shared_ptr<GPoint> &goal,
                         double minKm, double maxKm);
    void verifyNoLandCrossing(const ShortestPathResult &result);
    void logPathDetails(const ShortestPathResult &result,
                        const QString &routeName);
    void skipIfNoData();
    void runRouteTest(const QString &originName, double originLon,
                      double originLat, const QString &destName,
                      double destLon, double destLat, double minKm,
                      double maxKm);

    std::shared_ptr<OptimizedNetwork> mNetwork;
    bool                              mDataAvailable = false;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::shared_ptr<GPoint>
RealWorldShortestPathTest::makePoint(double lon, double lat,
                                     const QString &id)
{
    return std::make_shared<GPoint>(units::angle::degree_t(lon),
                                    units::angle::degree_t(lat), id);
}

double RealWorldShortestPathTest::computeTotalPathLengthKm(
    const ShortestPathResult &result)
{
    double totalM = 0.0;
    for (const auto &line : result.lines) {
        totalM += line->length().value();
    }
    return totalM / 1000.0;
}

bool RealWorldShortestPathTest::verifyPathContinuity(
    const ShortestPathResult &result)
{
    if (result.lines.size() < 2)
        return true;
    for (int i = 0; i < result.lines.size() - 1; ++i) {
        auto end   = result.lines[i]->endPoint();
        auto start = result.lines[i + 1]->startPoint();
        if (*end != *start) {
            qWarning() << "Path discontinuity at segment" << i;
            return false;
        }
    }
    return true;
}

void RealWorldShortestPathTest::verifyPathValid(
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
             qPrintable(
                 QString("Path too short: %1 km < min %2 km")
                     .arg(totalKm).arg(minKm)));
    QVERIFY2(totalKm <= maxKm,
             qPrintable(
                 QString("Path too long: %1 km > max %2 km")
                     .arg(totalKm).arg(maxKm)));

    QVERIFY2(verifyPathContinuity(result), "Path is not continuous");
}

void RealWorldShortestPathTest::verifyNoLandCrossing(
    const ShortestPathResult &result)
{
    if (!mNetwork)
        return;
    auto hvg = mNetwork->getVisibilityGraph();
    if (!hvg)
        return;

    int violations = 0;
    for (int i = 0; i < result.lines.size(); ++i) {
        const auto &line = result.lines[i];
        double lenKm = line->length().value() / 1000.0;
        if (lenKm < 2.0)
            continue;

        if (!hvg->isSegmentVisible(line)) {
            violations++;
            if (violations <= 5) {
                qWarning() << "Land crossing at segment" << i
                           << "length:" << lenKm << "km"
                           << "from:"
                           << line->startPoint()->getLongitude().value()
                           << line->startPoint()->getLatitude().value()
                           << "to:"
                           << line->endPoint()->getLongitude().value()
                           << line->endPoint()->getLatitude().value();
            }
        }
    }
    QVERIFY2(violations == 0,
             qPrintable(QString("%1 segments cross land").arg(violations)));
}

void RealWorldShortestPathTest::logPathDetails(
    const ShortestPathResult &result, const QString &routeName)
{
    double km = computeTotalPathLengthKm(result);
    qDebug().noquote()
        << QString("%1: %2 km, %3 points, %4 segments")
               .arg(routeName)
               .arg(km, 0, 'f', 1)
               .arg(result.points.size())
               .arg(result.lines.size());
}

void RealWorldShortestPathTest::skipIfNoData()
{
    if (!mDataAvailable) {
        QSKIP("Real-world ocean data not available");
    }
}

void RealWorldShortestPathTest::runRouteTest(
    const QString &originName, double originLon, double originLat,
    const QString &destName, double destLon, double destLat, double minKm,
    double maxKm)
{
    skipIfNoData();

    auto start = makePoint(originLon, originLat, originName);
    auto goal  = makePoint(destLon, destLat, destName);

    QElapsedTimer timer;
    timer.start();
    auto result = mNetwork->findShortestPath(
        start, goal, PathFindingAlgorithm::AStar);
    qint64 elapsed = timer.elapsed();

    QString routeName =
        QString("%1 -> %2").arg(originName, destName);
    logPathDetails(result, routeName);
    qDebug() << "  Time:" << elapsed << "ms";

    verifyPathValid(result, start, goal, minKm, maxKm);
}

// ---------------------------------------------------------------------------
// Setup / Teardown
// ---------------------------------------------------------------------------

void RealWorldShortestPathTest::initTestCase()
{
    QString dataPath =
        QCoreApplication::applicationDirPath() + "/data/ne_110m_ocean.shp";

    if (!QFile::exists(dataPath)) {
        qWarning() << "Ocean data not found at:" << dataPath;
        mDataAvailable = false;
        QSKIP("Real-world ocean data (ne_110m_ocean.shp) not available");
        return;
    }

    qDebug() << "Loading ocean network from:" << dataPath;
    QElapsedTimer timer;
    timer.start();

    mNetwork = std::make_shared<OptimizedNetwork>(dataPath, "GlobalOcean");

    // Verify polygons were loaded (catches MultiPolygon regression)
    auto hvg = mNetwork->getVisibilityGraph();
    if (!hvg || hvg->polygons.isEmpty()) {
        qWarning() << "Network loaded but no polygons found — "
                      "possible wkbMultiPolygon handling bug";
        mDataAvailable = false;
        QSKIP("Ocean network failed to load polygons");
        return;
    }

    qDebug() << "Network loaded in" << timer.elapsed() / 1000.0 << "s"
             << "with" << hvg->polygons.size() << "polygons";
    mDataAvailable = true;
}

void RealWorldShortestPathTest::cleanupTestCase()
{
    mNetwork.reset();
    qDebug() << "Real-world tests completed.";
}

// ---------------------------------------------------------------------------
// Short routes
// ---------------------------------------------------------------------------

void RealWorldShortestPathTest::testShort_Antwerpen_Zeebrugge()
{
    runRouteTest("Antwerpen", 4.40, 51.22,
                 "Zeebrugge", 3.20, 51.33,
                 70, 150);
}

void RealWorldShortestPathTest::testShort_Santos_Paranagua()
{
    runRouteTest("Santos", -46.31, -23.96,
                 "Paranagua", -48.51, -25.50,
                 240, 400);
}

void RealWorldShortestPathTest::testShort_NewYork_Boston()
{
    runRouteTest("NewYork", -74.01, 40.71,
                 "Boston", -71.06, 42.36,
                 330, 550);
}

// ---------------------------------------------------------------------------
// Medium routes
// ---------------------------------------------------------------------------

void RealWorldShortestPathTest::testMedium_Brisbane_Newcastle()
{
    runRouteTest("Brisbane", 153.03, -27.47,
                 "Newcastle", 151.78, -32.93,
                 550, 900);
}

void RealWorldShortestPathTest::testMedium_Rotterdam_Algeciras()
{
    runRouteTest("Rotterdam", 4.48, 51.92,
                 "Algeciras", -5.45, 36.14,
                 2200, 3200);
}

void RealWorldShortestPathTest::testMedium_NewYork_Colon()
{
    runRouteTest("NewYork", -74.01, 40.71,
                 "Colon", -79.91, 9.35,
                 3200, 4500);
}

// ---------------------------------------------------------------------------
// Long routes
// ---------------------------------------------------------------------------

void RealWorldShortestPathTest::testLong_Colombo_Jeddah()
{
    runRouteTest("Colombo", 79.85, 6.93,
                 "Jeddah", 39.17, 21.49,
                 4500, 6500);
}

void RealWorldShortestPathTest::testLong_Singapore_Busan()
{
    runRouteTest("Singapore", 103.82, 1.35,
                 "Busan", 129.08, 35.18,
                 4000, 5500);
}

void RealWorldShortestPathTest::testLong_Rotterdam_NewYork()
{
    runRouteTest("Rotterdam", 4.48, 51.92,
                 "NewYork", -74.01, 40.71,
                 5500, 7500);
}

void RealWorldShortestPathTest::testLong_Santos_CapeTown()
{
    runRouteTest("Santos", -46.31, -23.96,
                 "CapeTown", 18.42, -33.92,
                 6000, 8500);
}

// ---------------------------------------------------------------------------
// Very long routes
// ---------------------------------------------------------------------------

void RealWorldShortestPathTest::testVeryLong_Singapore_PortSaid()
{
    runRouteTest("Singapore", 103.82, 1.35,
                 "PortSaid", 32.30, 31.26,
                 7500, 10000);
}

void RealWorldShortestPathTest::testVeryLong_Yokohama_LongBeach()
{
    runRouteTest("Yokohama", 139.64, 35.44,
                 "LongBeach", -118.19, 33.77,
                 8500, 12000);
}

// ---------------------------------------------------------------------------
// Edge cases on real data
// ---------------------------------------------------------------------------

void RealWorldShortestPathTest::testAntimeridian_Yokohama_LongBeach()
{
    skipIfNoData();

    auto start = makePoint(139.64, 35.44, "Yokohama");
    auto goal  = makePoint(-118.19, 33.77, "LongBeach");

    auto result = mNetwork->findShortestPath(
        start, goal, PathFindingAlgorithm::AStar);

    logPathDetails(result, "Antimeridian_Yokohama_LongBeach");
    verifyPathValid(result, start, goal, 8500, 12000);

    bool crossesAntimeridian = false;
    for (const auto &pt : result.points) {
        double lon = pt->getLongitude().value();
        if (std::abs(lon) > 170.0) {
            crossesAntimeridian = true;
            break;
        }
    }
    QVERIFY2(crossesAntimeridian,
             "Yokohama-LongBeach path should cross the antimeridian");
}

void RealWorldShortestPathTest::testNearCoast_Dover_Calais()
{
    runRouteTest("Dover", 1.30, 51.10,
                 "Calais", 1.85, 50.95,
                 30, 100);
}

void RealWorldShortestPathTest::testVeryClose_OpenOcean()
{
    skipIfNoData();

    auto start = makePoint(-30.0, 20.0, "MidAtl1");
    auto goal  = makePoint(-30.005, 20.0, "MidAtl2");

    auto result = mNetwork->findShortestPath(
        start, goal, PathFindingAlgorithm::AStar);

    if (result.isValid()) {
        double km = computeTotalPathLengthKm(result);
        QVERIFY2(km < 10.0,
                 qPrintable(
                     QString("Very close points path too long: %1 km")
                         .arg(km)));
    }
    logPathDetails(result, "VeryClose_OpenOcean");
}

void RealWorldShortestPathTest::testNarrowStrait_Malacca()
{
    runRouteTest("Singapore", 103.82, 1.35,
                 "Penang", 100.35, 5.42,
                 500, 800);
}

void RealWorldShortestPathTest::testNarrowStrait_SuezApproach()
{
    runRouteTest("PortSaid", 32.30, 31.26,
                 "Suez", 32.55, 29.97,
                 100, 300);
}

void RealWorldShortestPathTest::testSameStartEnd()
{
    skipIfNoData();

    auto point = makePoint(4.48, 51.92, "Rotterdam");
    auto result = mNetwork->findShortestPath(
        point, point, PathFindingAlgorithm::AStar);

    if (result.isValid()) {
        double km = computeTotalPathLengthKm(result);
        QVERIFY2(km < 1.0,
                 qPrintable(
                     QString("Same start/end path should be ~0 km, got %1")
                         .arg(km)));
    }
    qDebug() << "SameStartEnd: valid=" << result.isValid();
}

void RealWorldShortestPathTest::testMultiWaypoint_5Ports()
{
    skipIfNoData();

    QVector<std::shared_ptr<GPoint>> waypoints;
    waypoints.append(makePoint(4.48, 51.92, "Rotterdam"));
    waypoints.append(makePoint(-5.45, 36.14, "Algeciras"));
    waypoints.append(makePoint(32.30, 31.26, "PortSaid"));
    waypoints.append(makePoint(39.17, 21.49, "Jeddah"));
    waypoints.append(makePoint(103.82, 1.35, "Singapore"));

    QElapsedTimer timer;
    timer.start();
    auto result = mNetwork->findShortestPath(
        waypoints, PathFindingAlgorithm::AStar);
    qint64 elapsed = timer.elapsed();

    logPathDetails(result, "MultiWaypoint_5Ports");
    qDebug() << "  Time:" << elapsed << "ms";

    QVERIFY2(result.isValid(), "Multi-waypoint 5-port path should be valid");
    QVERIFY2(verifyPathContinuity(result),
             "Multi-waypoint path should be continuous");

    double km = computeTotalPathLengthKm(result);
    QVERIFY2(km > 12000 && km < 25000,
             qPrintable(
                 QString("5-port path length %1 km out of range").arg(km)));
}

void RealWorldShortestPathTest::testSymmetry_Rotterdam_NY()
{
    skipIfNoData();

    auto rotterdam = makePoint(4.48, 51.92, "Rotterdam");
    auto ny        = makePoint(-74.01, 40.71, "NewYork");

    auto fwd = mNetwork->findShortestPath(
        rotterdam, ny, PathFindingAlgorithm::AStar);
    auto rev = mNetwork->findShortestPath(
        ny, rotterdam, PathFindingAlgorithm::AStar);

    QVERIFY2(fwd.isValid(), "R->NY should be valid");
    QVERIFY2(rev.isValid(), "NY->R should be valid");

    double fwdKm = computeTotalPathLengthKm(fwd);
    double revKm = computeTotalPathLengthKm(rev);

    logPathDetails(fwd, "Rotterdam->NY");
    logPathDetails(rev, "NY->Rotterdam");

    double ratio = fwdKm / revKm;
    QVERIFY2(ratio > 0.85 && ratio < 1.15,
             qPrintable(
                 QString("Symmetry violation: R->NY=%1km, NY->R=%2km, "
                         "ratio=%3")
                     .arg(fwdKm).arg(revKm).arg(ratio)));
}

void RealWorldShortestPathTest::testSymmetry_Singapore_Busan()
{
    skipIfNoData();

    auto sg    = makePoint(103.82, 1.35, "Singapore");
    auto busan = makePoint(129.08, 35.18, "Busan");

    auto fwd = mNetwork->findShortestPath(
        sg, busan, PathFindingAlgorithm::AStar);
    auto rev = mNetwork->findShortestPath(
        busan, sg, PathFindingAlgorithm::AStar);

    QVERIFY2(fwd.isValid(), "SG->Busan should be valid");
    QVERIFY2(rev.isValid(), "Busan->SG should be valid");

    double fwdKm = computeTotalPathLengthKm(fwd);
    double revKm = computeTotalPathLengthKm(rev);

    logPathDetails(fwd, "Singapore->Busan");
    logPathDetails(rev, "Busan->Singapore");

    double ratio = fwdKm / revKm;
    QVERIFY2(ratio > 0.85 && ratio < 1.15,
             qPrintable(
                 QString("Symmetry violation: SG->B=%1km, B->SG=%2km, "
                         "ratio=%3")
                     .arg(fwdKm).arg(revKm).arg(ratio)));
}

// ---------------------------------------------------------------------------
// Diagnostic Tests — investigate WHY tests fail at a given resolution
// ---------------------------------------------------------------------------

void RealWorldShortestPathTest::testDiag_PointContainment()
{
    skipIfNoData();

    auto hvg = mNetwork->getVisibilityGraph();
    QVERIFY(hvg);

    // Points that fail on 110m (and potentially others)
    struct TestPort {
        const char *name;
        double lon, lat;
    };
    TestPort ports[] = {
        {"Antwerpen",  4.40,   51.22},
        {"Zeebrugge",  3.20,   51.33},
        {"NewYork",   -74.01,  40.71},
        {"Boston",    -71.06,  42.36},
        {"Yokohama",  139.64,  35.44},
        {"LongBeach", -118.19, 33.77},
        {"Brisbane",  153.03, -27.47},
        {"Newcastle", 151.78, -32.93},
        {"Rotterdam",   4.48,  51.92},
        {"Algeciras",  -5.45,  36.14},
        {"Colon",     -79.91,   9.35},
        {"Dover",       1.30,  51.10},
        {"Calais",      1.85,  50.95},
    };

    qWarning() << "=== POINT CONTAINMENT DIAGNOSTIC ===";
    qWarning() << "Dataset polygons:" << hvg->polygons.size();
    for (const auto &poly : hvg->polygons) {
        qWarning() << "  Polygon: outer=" << poly->outer().size()
                    << "vertices, holes=" << poly->inners().size();
    }

    for (const auto &port : ports) {
        auto pt = makePoint(port.lon, port.lat, port.name);

        // Check containment in each polygon
        bool inAnyPoly = false;
        int holeIdx = -1;
        for (int pi = 0; pi < hvg->polygons.size(); ++pi) {
            const auto &poly = hvg->polygons[pi];
            bool inExterior = poly->isPointWithinExteriorRing(*pt);
            int hole = poly->findContainingHoleIndex(*pt);
            bool inPoly = poly->isPointWithinPolygon(*pt);

            if (inExterior || hole >= 0 || inPoly) {
                qWarning() << "  " << port.name
                           << "poly" << pi
                           << ": inExterior=" << inExterior
                           << "holeIdx=" << hole
                           << "inPolygon=" << inPoly;
            }
            if (inPoly) inAnyPoly = true;
            if (hole >= 0) holeIdx = hole;
        }

        // Find nearest vertex across all polygons
        double minDistKm = std::numeric_limits<double>::max();
        for (const auto &poly : hvg->polygons) {
            for (const auto &v : poly->outer()) {
                double d = pt->fastDistance(*v).value() / 1000.0;
                if (d < minDistKm) minDistKm = d;
            }
            for (const auto &hole : poly->inners()) {
                for (const auto &v : hole) {
                    double d = pt->fastDistance(*v).value() / 1000.0;
                    if (d < minDistKm) minDistKm = d;
                }
            }
        }

        qWarning() << "  " << port.name
                   << QString("(%1,%2)").arg(port.lon).arg(port.lat)
                   << ": inWater=" << inAnyPoly
                   << "onLand(hole)=" << holeIdx
                   << "nearestVertex=" << QString::number(minDistKm, 'f', 1) << "km";
    }
}

void RealWorldShortestPathTest::testDiag_FailingRouteDetails()
{
    skipIfNoData();

    struct Route {
        const char *name;
        double sLon, sLat, eLon, eLat;
    };
    Route routes[] = {
        {"Antwerpen-Zeebrugge",   4.40, 51.22,   3.20, 51.33},
        {"NY-Boston",            -74.01, 40.71, -71.06, 42.36},
        {"Brisbane-Newcastle",   153.03,-27.47, 151.78,-32.93},
        {"Rotterdam-Algeciras",    4.48, 51.92,  -5.45, 36.14},
        {"NY-Colon",             -74.01, 40.71, -79.91,  9.35},
        {"Rotterdam-NY",           4.48, 51.92, -74.01, 40.71},
        {"Yokohama-LongBeach",   139.64, 35.44,-118.19, 33.77},
        {"Dover-Calais",           1.30, 51.10,   1.85, 50.95},
    };

    qWarning() << "\n=== FAILING ROUTE DIAGNOSTIC ===";

    for (const auto &r : routes) {
        auto start = makePoint(r.sLon, r.sLat, "S");
        auto goal = makePoint(r.eLon, r.eLat, "G");

        // Great-circle distance
        double gcKm = start->distance(*goal).value() / 1000.0;

        QElapsedTimer timer;
        timer.start();
        auto result = mNetwork->findShortestPath(
            start, goal, PathFindingAlgorithm::AStar);
        qint64 ms = timer.elapsed();

        if (!result.isValid()) {
            qWarning() << "  " << r.name
                       << ": INVALID (no path found)"
                       << "GC=" << QString::number(gcKm, 'f', 0) << "km"
                       << "time=" << ms << "ms";
            continue;
        }

        double totalKm = computeTotalPathLengthKm(result);
        double ratio = totalKm / gcKm;
        bool continuous = verifyPathContinuity(result);

        double startSnapKm =
            result.points.first()->distance(*start).value() / 1000.0;
        double endSnapKm =
            result.points.last()->distance(*goal).value() / 1000.0;

        qWarning() << "  " << r.name
                   << ": path=" << QString::number(totalKm, 'f', 0) << "km"
                   << "GC=" << QString::number(gcKm, 'f', 0) << "km"
                   << "ratio=" << QString::number(ratio, 'f', 2)
                   << "snap(S)=" << QString::number(startSnapKm, 'f', 1) << "km"
                   << "snap(G)=" << QString::number(endSnapKm, 'f', 1) << "km"
                   << "continuous=" << continuous
                   << "pts=" << result.points.size()
                   << "time=" << ms << "ms";

        // Log first and last path coordinates
        if (result.points.size() >= 2) {
            auto first = result.points.first();
            auto last = result.points.last();
            qWarning() << "    start:"
                       << first->getLongitude().value()
                       << first->getLatitude().value()
                       << "  end:"
                       << last->getLongitude().value()
                       << last->getLatitude().value();
        }
    }
}

void RealWorldShortestPathTest::testDiag_ExportGeoJSON()
{
    skipIfNoData();

    struct Route {
        const char *name;
        double sLon, sLat, eLon, eLat;
    };
    Route routes[] = {
        {"Antwerpen-Zeebrugge",     4.40,  51.22,    3.20,  51.33},
        {"Santos-Paranagua",       -46.31, -23.96,  -48.51, -25.50},
        {"NY-Boston",              -74.01,  40.71,  -71.06,  42.36},
        {"Brisbane-Newcastle",     153.03, -27.47,  151.78, -32.93},
        {"Rotterdam-Algeciras",      4.48,  51.92,   -5.45,  36.14},
        {"NY-Colon",               -74.01,  40.71,  -79.91,   9.35},
        {"Colombo-Jeddah",          79.85,   6.93,   39.17,  21.49},
        {"Singapore-Busan",        103.82,   1.35,  129.08,  35.18},
        {"Rotterdam-NY",             4.48,  51.92,  -74.01,  40.71},
        {"Santos-CapeTown",        -46.31, -23.96,   18.42, -33.92},
        {"Singapore-PortSaid",     103.82,   1.35,   32.30,  31.26},
        {"Yokohama-LongBeach",     139.64,  35.44, -118.19,  33.77},
        {"Dover-Calais",             1.30,  51.10,    1.85,  50.95},
        {"Malacca",                103.82,   1.35,  100.35,   5.42},
        {"SuezApproach",            32.30,  31.26,   32.55,  29.97},
    };

    // Build GeoJSON FeatureCollection
    QString geojson = "{\n  \"type\": \"FeatureCollection\",\n  \"features\": [\n";
    bool firstFeature = true;

    for (const auto &r : routes) {
        auto start = makePoint(r.sLon, r.sLat, "S");
        auto goal  = makePoint(r.eLon, r.eLat, "G");

        auto result = mNetwork->findShortestPath(
            start, goal, PathFindingAlgorithm::AStar);

        // Origin marker
        if (!firstFeature) geojson += ",\n";
        firstFeature = false;
        geojson += QString(
            "    {\"type\":\"Feature\",\"properties\":"
            "{\"name\":\"%1 (origin)\",\"marker-color\":\"#00ff00\"},"
            "\"geometry\":{\"type\":\"Point\","
            "\"coordinates\":[%2,%3]}}")
            .arg(r.name).arg(r.sLon, 0, 'f', 6).arg(r.sLat, 0, 'f', 6);

        // Destination marker
        geojson += QString(
            ",\n    {\"type\":\"Feature\",\"properties\":"
            "{\"name\":\"%1 (dest)\",\"marker-color\":\"#ff0000\"},"
            "\"geometry\":{\"type\":\"Point\","
            "\"coordinates\":[%2,%3]}}")
            .arg(r.name).arg(r.eLon, 0, 'f', 6).arg(r.eLat, 0, 'f', 6);

        if (!result.isValid()) {
            qWarning() << r.name << ": NO PATH (skipping line)";
            continue;
        }

        double totalKm = computeTotalPathLengthKm(result);

        // Path line
        QString coords;
        for (int i = 0; i < result.points.size(); ++i) {
            if (i > 0) coords += ",";
            coords += QString("[%1,%2]")
                .arg(result.points[i]->getLongitude().value(), 0, 'f', 6)
                .arg(result.points[i]->getLatitude().value(), 0, 'f', 6);
        }

        geojson += QString(
            ",\n    {\"type\":\"Feature\",\"properties\":"
            "{\"name\":\"%1\",\"length_km\":%2,\"points\":%3,"
            "\"stroke\":\"#0066ff\",\"stroke-width\":2},"
            "\"geometry\":{\"type\":\"LineString\","
            "\"coordinates\":[%4]}}")
            .arg(r.name)
            .arg(totalKm, 0, 'f', 1)
            .arg(result.points.size())
            .arg(coords);
    }

    geojson += "\n  ]\n}\n";

    // Write to file next to test binary
    QString outPath = QCoreApplication::applicationDirPath()
                      + "/diagnostic_paths.geojson";
    QFile file(outPath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        file.write(geojson.toUtf8());
        file.close();
        qWarning() << "GeoJSON written to:" << outPath;
    } else {
        qWarning() << "Failed to write GeoJSON to:" << outPath;
    }
}

// Custom main: disable the default 5-minute per-function watchdog
// so that long-running real-world pathfinding tests run to completion.
int main(int argc, char *argv[])
{
    if (!qEnvironmentVariableIsSet("QTEST_FUNCTION_TIMEOUT"))
        qputenv("QTEST_FUNCTION_TIMEOUT",
                 QByteArray::number(std::numeric_limits<int>::max()));
    QCoreApplication app(argc, argv);
    QTEST_SET_MAIN_SOURCE_PATH
    RealWorldShortestPathTest tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_shortestpath_realworld.moc"
