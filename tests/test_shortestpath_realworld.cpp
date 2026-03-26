#include "network/gline.h"
#include "network/gpoint.h"
#include "network/gsegment.h"
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
#include <queue>

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
    void testDiag_AntwerpenSnapping();
    void testDiag_LevelStructure();
    void testDiag_RouteSantosParanagua();
    void testDiag_RouteNYBoston();
    void testDiag_BoundaryVisibility();
    void testDiag_FailingRouteWaypoints();
    void testDiag_CrossHoleVisibility();
    void testDiag_OuterRingVisibility();
    void testDiag_StraitVisibility();
    void testDiag_ExportLevelsGeoJSON();

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
    QVERIFY2(startDistKm < 100.0,
             qPrintable(QString("Path start %1 km from origin (max 100)")
                            .arg(startDistKm)));

    double endDistKm =
        result.points.last()->distance(*goal).value() / 1000.0;
    QVERIFY2(endDistKm < 100.0,
             qPrintable(QString("Path end %1 km from dest (max 100)")
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

    // Export path as GeoJSON for visual verification
    if (result.isValid() && !result.points.isEmpty())
    {
        QString filename = QString("route_%1_%2.geojson")
                               .arg(originName, destName);
        QFile geoFile(filename);
        if (geoFile.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            QTextStream out(&geoFile);
            out << "{\n\"type\": \"FeatureCollection\",\n\"features\": [{\n"
                << "  \"type\": \"Feature\",\n"
                << "  \"properties\": {\n"
                << "    \"route\": \"" << routeName << "\",\n"
                << "    \"distance_km\": "
                << computeTotalPathLengthKm(result) << ",\n"
                << "    \"points\": " << result.points.size() << ",\n"
                << "    \"time_ms\": " << elapsed << "\n"
                << "  },\n"
                << "  \"geometry\": {\n"
                << "    \"type\": \"LineString\",\n"
                << "    \"coordinates\": [\n";

            for (int i = 0; i < result.points.size(); ++i)
            {
                double lon = result.points[i]->getLongitude().value();
                double lat = result.points[i]->getLatitude().value();
                out << "      [" << lon << ", " << lat << "]";
                if (i + 1 < result.points.size()) out << ",";
                out << "\n";
            }

            out << "    ]\n  }\n}]\n}\n";
            geoFile.close();
            qDebug() << "  GeoJSON exported:" << filename;
        }
    }

    verifyPathValid(result, start, goal, minKm, maxKm);
}

// ---------------------------------------------------------------------------
// Setup / Teardown
// ---------------------------------------------------------------------------

void RealWorldShortestPathTest::initTestCase()
{
    QString dataPath =
        QCoreApplication::applicationDirPath() + "/data/ne_10m_ocean.shp";

    if (!QFile::exists(dataPath)) {
        qWarning() << "Ocean data not found at:" << dataPath;
        mDataAvailable = false;
        QSKIP("Real-world ocean data (ne_10m_ocean.shp) not available");
        return;
    }

    qDebug() << "Loading ocean network from:" << dataPath;
    QElapsedTimer timer;
    timer.start();

    mNetwork = std::make_shared<OptimizedNetwork>(dataPath, "GlobalOcean");

    // Verify polygons were loaded (catches MultiPolygon regression)
    auto hvg = mNetwork->getVisibilityGraph();
    if (!hvg || hvg->getPolygons().isEmpty()) {
        qWarning() << "Network loaded but no polygons found — "
                      "possible wkbMultiPolygon handling bug";
        mDataAvailable = false;
        QSKIP("Ocean network failed to load polygons");
        return;
    }

    qDebug() << "Network loaded in" << timer.elapsed() / 1000.0 << "s"
             << "with" << hvg->getPolygons().size() << "polygons";
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
                 320, 550);
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
    qWarning() << "Dataset polygons:" << hvg->getPolygons().size();
    for (const auto &poly : hvg->getPolygons()) {
        qWarning() << "  Polygon: outer=" << poly->outer().size()
                    << "vertices, holes=" << poly->inners().size();
    }

    for (const auto &port : ports) {
        auto pt = makePoint(port.lon, port.lat, port.name);

        // Check containment in each polygon
        bool inAnyPoly = false;
        int holeIdx = -1;
        for (int pi = 0; pi < hvg->getPolygons().size(); ++pi) {
            const auto &poly = hvg->getPolygons()[pi];
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
        for (const auto &poly : hvg->getPolygons()) {
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

void RealWorldShortestPathTest::testDiag_AntwerpenSnapping()
{
    skipIfNoData();

    auto hvg = mNetwork->getVisibilityGraph();
    QVERIFY(hvg);

    auto antwerpen = makePoint(4.40, 51.22, "Antwerpen");
    auto zeebrugge = makePoint(3.20, 51.33, "Zeebrugge");

    qWarning() << "\n=== ANTWERPEN SNAPPING DIAGNOSTIC ===";

    // 1. Check point containment at L0 (original polygons)
    for (int pi = 0; pi < hvg->getPolygons().size(); ++pi) {
        const auto &poly = hvg->getPolygons()[pi];
        bool inExterior = poly->isPointWithinExteriorRing(*antwerpen);
        int hole = poly->findContainingHoleIndex(*antwerpen);
        bool inPoly = poly->isPointWithinPolygon(*antwerpen);
        qWarning() << "  Antwerpen in L0 poly" << pi
                   << ": inExterior=" << inExterior
                   << "holeIdx=" << hole
                   << "inPolygon=" << inPoly;
    }

    // 2. Direct visibility check
    auto directLine = std::make_shared<GLine>(antwerpen, zeebrugge);
    bool directVisible = hvg->isSegmentVisible(directLine, 0);
    qWarning() << "  Direct Antwerpen->Zeebrugge visible:" << directVisible;

    // 3. Full path result analysis
    auto result = hvg->findShortestPath(antwerpen, zeebrugge);
    qWarning() << "  Path valid:" << result.isValid()
               << "points:" << result.points.size()
               << "lines:" << result.lines.size();

    if (result.isValid() && !result.points.isEmpty()) {
        auto &first = result.points.first();
        auto &last  = result.points.last();
        double startDist = first->distance(*antwerpen).value() / 1000.0;
        double endDist   = last->distance(*zeebrugge).value() / 1000.0;

        qWarning() << "  First point:"
                   << QString("(%1, %2)")
                          .arg(first->getLongitude().value())
                          .arg(first->getLatitude().value())
                   << "dist from Antwerpen:" << QString::number(startDist, 'f', 1) << "km";
        qWarning() << "  Last point:"
                   << QString("(%1, %2)")
                          .arg(last->getLongitude().value())
                          .arg(last->getLatitude().value())
                   << "dist from Zeebrugge:" << QString::number(endDist, 'f', 1) << "km";

        for (int i = 0; i < result.points.size(); ++i) {
            qWarning() << "    pt[" << i << "]:"
                       << QString("(%1, %2)")
                              .arg(result.points[i]->getLongitude().value())
                              .arg(result.points[i]->getLatitude().value());
        }
    }
}

// ---------------------------------------------------------------------------
// Diagnostic: Level Structure
// ---------------------------------------------------------------------------

void RealWorldShortestPathTest::testDiag_LevelStructure()
{
    skipIfNoData();

    auto hvg = mNetwork->getVisibilityGraph();
    QVERIFY(hvg);

    // Compute avgSpacing (same algorithm as computeDynamicParameters)
    double totalLength = 0.0;
    int edgeCount = 0;
    for (const auto &poly : hvg->getPolygons()) {
        if (!poly) continue;
        const auto &outer = poly->outer();
        int sz = outer.size();
        if (sz < 2) continue;
        for (int i = 0; i < sz; ++i) {
            const auto &a = outer[i];
            const auto &b = outer[(i + 1) % sz];
            if (!a || !b) continue;
            totalLength += GSegment::haversineRaw(
                a->getLongitude().value(), a->getLatitude().value(),
                b->getLongitude().value(), b->getLatitude().value());
            ++edgeCount;
        }
    }
    double avgSpacing = (edgeCount > 0)
        ? std::max(1000.0, totalLength / edgeCount)
        : 10000.0;

    qWarning() << "\n=== LEVEL STRUCTURE DIAGNOSTIC ===";
    qWarning() << "avgSpacing =" << avgSpacing / 1000.0 << "km"
               << "(from" << edgeCount << "outer ring edges)";

    // Expected dynamic parameters
    constexpr double TOLERANCE_FACTORS[] = {0.0, 0.2, 1.0, 5.0};
    constexpr double DISTANCE_FACTORS[]  = {5.0, 20.0, 50.0, 200.0};
    // Old hardcoded proven values
    constexpr double OLD_MAX_DIST[]      = {50000.0, 2000000.0,
                                            5000000.0, 2000000.0};

    auto diag = hvg->getDiagnostics();

    qWarning() << "L0 adj ready:" << diag.level0AdjReady;
    qWarning() << "Polygon graph polygons:" << diag.polygonGraphPolygonCount;

    for (int i = 0; i < 4; ++i) {
        const auto &l = diag.levels[i];
        double expectedMaxDist = DISTANCE_FACTORS[i] * avgSpacing;
        qWarning().noquote()
            << QString("  L%1: vertices=%2, edges=%3, "
                       "tolerance=%4 km, maxDist=%5 km "
                       "(expected=%6 km, old=%7 km)")
                   .arg(i)
                   .arg(l.vertexCount)
                   .arg(l.edgeCount)
                   .arg(l.toleranceMeters / 1000.0, 0, 'f', 1)
                   .arg(l.maxDistanceMeters / 1000.0, 0, 'f', 1)
                   .arg(expectedMaxDist / 1000.0, 0, 'f', 1)
                   .arg(OLD_MAX_DIST[i] / 1000.0, 0, 'f', 1);
    }
}

// ---------------------------------------------------------------------------
// Diagnostic: Route trace helper
// ---------------------------------------------------------------------------

static void traceRoute(
    const std::shared_ptr<HierarchicalVisibilityGraph> &hvg,
    const std::shared_ptr<GPoint> &start,
    const std::shared_ptr<GPoint> &goal,
    const QString &routeName)
{
    qWarning().noquote() << "\n=== ROUTE TRACE:" << routeName << "===";

    // 1. Containment for both points
    auto checkContainment = [&](const std::shared_ptr<GPoint> &pt,
                                const QString &name) {
        auto polys = hvg->findAllContainingPolygons(pt);
        bool inWater = !polys.isEmpty();

        int holeIdx = -1;
        int polyIdx = -1;
        double nearestVertDist = std::numeric_limits<double>::max();
        std::shared_ptr<GPoint> nearestVert;

        for (int pi = 0; pi < hvg->getPolygons().size(); ++pi) {
            const auto &poly = hvg->getPolygons()[pi];
            if (poly->isPointWithinExteriorRing(*pt)) {
                int h = poly->findContainingHoleIndex(*pt);
                if (h >= 0) { holeIdx = h; polyIdx = pi; }
            }
            // Find nearest vertex (outer + holes)
            for (const auto &v : poly->outer()) {
                double d = pt->fastDistance(*v).value();
                if (d < nearestVertDist) {
                    nearestVertDist = d;
                    nearestVert = v;
                }
            }
            for (const auto &hole : poly->inners()) {
                for (const auto &v : hole) {
                    double d = pt->fastDistance(*v).value();
                    if (d < nearestVertDist) {
                        nearestVertDist = d;
                        nearestVert = v;
                    }
                }
            }
        }

        qWarning().noquote()
            << QString("  %1 (%2,%3): inWater=%4, poly=%5, hole=%6, "
                       "nearestVert=%7 km at (%8,%9)")
                   .arg(name)
                   .arg(pt->getLongitude().value())
                   .arg(pt->getLatitude().value())
                   .arg(inWater)
                   .arg(polyIdx)
                   .arg(holeIdx)
                   .arg(nearestVertDist / 1000.0, 0, 'f', 1)
                   .arg(nearestVert ? nearestVert->getLongitude().value() : 0)
                   .arg(nearestVert ? nearestVert->getLatitude().value() : 0);

        // 2. Check visibility from nearest vertex to a few neighbours
        //    (check both outer ring and hole vertices)
        if (nearestVert) {
            int visCount = 0;
            int checkCount = 0;
            for (const auto &poly : hvg->getPolygons()) {
                // Check outer ring vertices
                for (const auto &v : poly->outer()) {
                    if (*v == *nearestVert) continue;
                    double d = nearestVert->fastDistance(*v).value();
                    if (d > 50000.0) continue;
                    ++checkCount;
                    if (hvg->isVisible(nearestVert, v, 0))
                        ++visCount;
                    if (checkCount >= 50) break;
                }
                // Check hole vertices
                if (checkCount < 50) {
                    for (const auto &hole : poly->inners()) {
                        for (const auto &v : hole) {
                            if (*v == *nearestVert) continue;
                            double d = nearestVert->fastDistance(*v).value();
                            if (d > 50000.0) continue;
                            ++checkCount;
                            if (hvg->isVisible(nearestVert, v, 0))
                                ++visCount;
                            if (checkCount >= 50) break;
                        }
                        if (checkCount >= 50) break;
                    }
                }
                if (checkCount >= 50) break;
            }
            qWarning().noquote()
                << QString("    visibility from nearest vert: %1/%2 "
                           "visible (of %3 checked within 50km)")
                       .arg(visCount).arg(checkCount).arg(checkCount);
        }
    };

    checkContainment(start, routeName.section("->", 0, 0).trimmed());
    checkContainment(goal, routeName.section("->", 1, 1).trimmed());

    // 3. Direct visibility
    auto directLine = std::make_shared<GLine>(start, goal);
    bool directVis = hvg->isSegmentVisible(directLine, 0);
    qWarning() << "  Direct visibility:" << directVis;

    // 4. findShortestPath
    QElapsedTimer timer;
    timer.start();
    auto result = hvg->findShortestPath(start, goal);
    qint64 elapsed = timer.elapsed();

    qWarning().noquote()
        << QString("  Result: valid=%1, points=%2, lines=%3, time=%4 ms")
               .arg(result.isValid())
               .arg(result.points.size())
               .arg(result.lines.size())
               .arg(elapsed);

    if (result.isValid()) {
        double totalKm = 0.0;
        for (const auto &line : result.lines)
            totalKm += line->length().value() / 1000.0;
        qWarning() << "  Path length:" << totalKm << "km";
    }
}

void RealWorldShortestPathTest::testDiag_RouteSantosParanagua()
{
    skipIfNoData();
    auto hvg = mNetwork->getVisibilityGraph();
    QVERIFY(hvg);

    // Also check containment for additional ports
    qWarning() << "\n=== ADDITIONAL PORT CONTAINMENT ===";
    struct Port { const char *name; double lon, lat; };
    Port extraPorts[] = {
        {"Santos",    -46.31, -23.96},
        {"Paranagua", -48.51, -25.50},
        {"Singapore", 103.85,   1.29},
        {"Colombo",    79.84,   6.93},
        {"Jeddah",     39.17,  21.49},
        {"Busan",     129.04,  35.10},
        {"CapeTown",   18.42, -33.92},
        {"PortSaid",   32.30,  31.26},
    };
    for (const auto &p : extraPorts) {
        auto pt = makePoint(p.lon, p.lat, p.name);
        bool inWater = false;
        int holeIdx = -1;
        int polyIdx = -1;
        for (int pi = 0; pi < hvg->getPolygons().size(); ++pi) {
            if (hvg->getPolygons()[pi]->isPointWithinPolygon(*pt)) {
                inWater = true;
                polyIdx = pi;
                break;
            }
            if (hvg->getPolygons()[pi]->isPointWithinExteriorRing(*pt)) {
                int h = hvg->getPolygons()[pi]->findContainingHoleIndex(*pt);
                if (h >= 0) { holeIdx = h; polyIdx = pi; }
            }
        }
        qWarning().noquote()
            << QString("  %1 (%2,%3): inWater=%4, poly=%5, hole=%6")
                   .arg(p.name).arg(p.lon).arg(p.lat)
                   .arg(inWater).arg(polyIdx).arg(holeIdx);
    }

    auto start = makePoint(-46.31, -23.96, "Santos");
    auto goal  = makePoint(-48.51, -25.50, "Paranagua");
    traceRoute(hvg, start, goal, "Santos -> Paranagua");
}

void RealWorldShortestPathTest::testDiag_RouteNYBoston()
{
    skipIfNoData();
    auto hvg = mNetwork->getVisibilityGraph();
    QVERIFY(hvg);

    auto start = makePoint(-74.01, 40.71, "NewYork");
    auto goal  = makePoint(-71.06, 42.36, "Boston");
    traceRoute(hvg, start, goal, "NewYork -> Boston");
}

// ---------------------------------------------------------------------------
// Diagnostic: Failing Route Waypoints
// ---------------------------------------------------------------------------

void RealWorldShortestPathTest::testDiag_FailingRouteWaypoints()
{
    skipIfNoData();
    auto hvg = mNetwork->getVisibilityGraph();
    QVERIFY(hvg);

    struct Route { const char *name; double sLon, sLat, eLon, eLat; };
    Route routes[] = {
        {"NY-Colon",           -74.01, 40.71, -79.91,  9.35},
        {"Rotterdam-Algeciras",  4.48, 51.92,  -5.45, 36.14},
        {"Colombo-Jeddah",      79.84,  6.93,  39.17, 21.49},
        {"Singapore-Busan",    103.85,  1.29, 129.04, 35.10},
    };

    qWarning() << "\n=== FAILING ROUTE WAYPOINTS ===";

    for (const auto &r : routes) {
        auto s = makePoint(r.sLon, r.sLat, "S");
        auto g = makePoint(r.eLon, r.eLat, "G");

        QElapsedTimer timer;
        timer.start();
        auto result = mNetwork->findShortestPath(
            s, g, PathFindingAlgorithm::AStar);
        qint64 ms = timer.elapsed();

        if (!result.isValid()) {
            qWarning().noquote()
                << QString("  %1: INVALID (%2 ms)").arg(r.name).arg(ms);
            continue;
        }

        double totalKm = computeTotalPathLengthKm(result);
        int n = result.points.size();

        qWarning().noquote()
            << QString("  %1: %2 km, %3 pts, %4 ms")
                   .arg(r.name).arg(totalKm, 0, 'f', 0)
                   .arg(n).arg(ms);

        // Print all waypoints
        for (int i = 0; i < n; ++i) {
            qWarning().noquote()
                << QString("    pt[%1]: (%2, %3)")
                       .arg(i)
                       .arg(result.points[i]->getLongitude().value(), 0, 'f', 4)
                       .arg(result.points[i]->getLatitude().value(), 0, 'f', 4);
        }
    }

    // Export all failing routes as GeoJSON for visualization
    QString geojson = "{\n  \"type\": \"FeatureCollection\",\n  \"features\": [\n";
    bool firstFeature = true;

    for (const auto &r : routes) {
        auto s = makePoint(r.sLon, r.sLat, "S");
        auto g = makePoint(r.eLon, r.eLat, "G");
        auto result = mNetwork->findShortestPath(
            s, g, PathFindingAlgorithm::AStar);
        if (!result.isValid()) continue;

        if (!firstFeature) geojson += ",\n";
        firstFeature = false;

        // Line feature
        geojson += QString(
            "    {\"type\": \"Feature\", \"properties\": {\"name\": \"%1\", "
            "\"km\": %2, \"pts\": %3}, \"geometry\": {\"type\": \"LineString\", "
            "\"coordinates\": [\n")
            .arg(r.name)
            .arg(computeTotalPathLengthKm(result), 0, 'f', 1)
            .arg(result.points.size());

        for (int i = 0; i < result.points.size(); ++i) {
            if (i > 0) geojson += ",\n";
            geojson += QString("      [%1, %2]")
                .arg(result.points[i]->getLongitude().value(), 0, 'f', 6)
                .arg(result.points[i]->getLatitude().value(), 0, 'f', 6);
        }
        geojson += "\n    ]}}\n";
    }
    geojson += "  ]\n}\n";

    QString path = QCoreApplication::applicationDirPath()
                   + "/diagnostic_failing_routes.geojson";
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(geojson.toUtf8());
        file.close();
        qWarning() << "GeoJSON written to:" << path;
    }
}

// ---------------------------------------------------------------------------
// Diagnostic: Cross-Hole Visibility (Manhattan → mainland)
// ---------------------------------------------------------------------------

void RealWorldShortestPathTest::testDiag_CrossHoleVisibility()
{
    skipIfNoData();
    auto hvg = mNetwork->getVisibilityGraph();
    QVERIFY(hvg);
    QVERIFY(hvg->getPolygons().size() >= 1);

    const auto &poly = hvg->getPolygons()[0];
    const auto &holes = poly->inners();

    // Hole 475 = Manhattan (19 vertices)
    // Hole 35 = US mainland (66K vertices)
    if (475 >= holes.size() || 35 >= holes.size()) {
        QSKIP("Required holes not found");
    }

    const auto &manhattan = holes[475];
    const auto &mainland  = holes[35];

    qWarning() << "\n=== CROSS-HOLE VISIBILITY: Manhattan (475) → Mainland (35) ===";
    qWarning() << "Manhattan vertices:" << manhattan.size()
               << ", Mainland vertices:" << mainland.size();

    int totalVisToMainland = 0;
    int totalVisToOuter = 0;
    int totalCheckedMainland = 0;
    int totalCheckedOuter = 0;

    for (int mi = 0; mi < manhattan.size(); ++mi) {
        const auto &mv = manhattan[mi];
        if (!mv) continue;

        int visMainland = 0, checkedMainland = 0;
        int visOuter = 0, checkedOuter = 0;

        // Check visibility to nearest mainland vertices within 50km
        for (const auto &v : mainland) {
            if (!v) continue;
            double d = mv->fastDistance(*v).value();
            if (d > 50000.0) continue;
            ++checkedMainland;
            if (hvg->isVisible(mv, v, 0))
                ++visMainland;
            if (checkedMainland >= 20) break;
        }

        // Check visibility to nearest outer ring vertices within 50km
        for (const auto &v : poly->outer()) {
            if (!v) continue;
            double d = mv->fastDistance(*v).value();
            if (d > 50000.0) continue;
            ++checkedOuter;
            if (hvg->isVisible(mv, v, 0))
                ++visOuter;
            if (checkedOuter >= 20) break;
        }

        qWarning().noquote()
            << QString("  Manhattan[%1] (%2,%3): "
                       "mainland=%4/%5 visible, outer=%6/%7 visible")
                   .arg(mi)
                   .arg(mv->getLongitude().value(), 0, 'f', 4)
                   .arg(mv->getLatitude().value(), 0, 'f', 4)
                   .arg(visMainland).arg(checkedMainland)
                   .arg(visOuter).arg(checkedOuter);

        totalVisToMainland += visMainland;
        totalVisToOuter += visOuter;
        totalCheckedMainland += checkedMainland;
        totalCheckedOuter += checkedOuter;
    }

    qWarning().noquote()
        << QString("  TOTAL: mainland=%1/%2, outer=%3/%4")
               .arg(totalVisToMainland).arg(totalCheckedMainland)
               .arg(totalVisToOuter).arg(totalCheckedOuter);
}

// ---------------------------------------------------------------------------
// Diagnostic: Boundary Visibility
// ---------------------------------------------------------------------------

void RealWorldShortestPathTest::testDiag_BoundaryVisibility()
{
    skipIfNoData();

    auto hvg = mNetwork->getVisibilityGraph();
    QVERIFY(hvg);
    QVERIFY(hvg->getPolygons().size() >= 1);

    const auto &poly = hvg->getPolygons()[0];
    const auto &holes = poly->inners();

    // Test consecutive boundary vertex visibility on specific holes
    int holeIndices[] = {35, 475};   // Boston's hole, NY's hole
    const char *holeNames[] = {"hole35(US-mainland)", "hole475(NYC-area)"};

    qWarning() << "\n=== BOUNDARY VISIBILITY DIAGNOSTIC ===";

    for (int h = 0; h < 2; ++h) {
        int hIdx = holeIndices[h];
        if (hIdx >= holes.size()) {
            qWarning() << "  Hole" << hIdx << "out of range";
            continue;
        }

        const auto &hole = holes[hIdx];
        int n = hole.size();
        qWarning().noquote()
            << QString("\n  %1 (hole %2): %3 vertices")
                   .arg(holeNames[h]).arg(hIdx).arg(n);

        // Check visibility for first 20 consecutive pairs
        int pairs = std::min(20, n - 1);
        int visibleCount = 0;
        int midpointFailCount = 0;

        for (int i = 0; i < pairs; ++i) {
            const auto &v1 = hole[i];
            const auto &v2 = hole[(i + 1) % n];
            if (!v1 || !v2) continue;

            double dist = v1->fastDistance(*v2).value() / 1000.0;
            bool vis = hvg->isVisible(v1, v2, 0);

            if (vis) {
                ++visibleCount;
            } else {
                // Check WHY it failed — is it the midpoint check?
                double midLon = (v1->getLongitude().value()
                                 + v2->getLongitude().value()) / 2.0;
                double midLat = (v1->getLatitude().value()
                                 + v2->getLatitude().value()) / 2.0;
                auto midPt = std::make_shared<GPoint>(
                    units::angle::degree_t(midLon),
                    units::angle::degree_t(midLat));
                bool midInPoly = poly->isPointWithinPolygon(*midPt);
                bool midInHole = poly->findContainingHoleIndex(*midPt) >= 0;
                bool midInExterior = poly->isPointWithinExteriorRing(*midPt);

                if (!midInPoly) ++midpointFailCount;

                qWarning().noquote()
                    << QString("    pair[%1]: (%2,%3)->(%4,%5) "
                               "dist=%6km vis=%7 "
                               "midInPoly=%8 midInExterior=%9 "
                               "midInHole=%10")
                           .arg(i)
                           .arg(v1->getLongitude().value(), 0, 'f', 4)
                           .arg(v1->getLatitude().value(), 0, 'f', 4)
                           .arg(v2->getLongitude().value(), 0, 'f', 4)
                           .arg(v2->getLatitude().value(), 0, 'f', 4)
                           .arg(dist, 0, 'f', 2)
                           .arg(vis)
                           .arg(midInPoly)
                           .arg(midInExterior)
                           .arg(midInHole);
            }
        }

        qWarning().noquote()
            << QString("  SUMMARY: %1/%2 consecutive pairs visible, "
                       "%3 midpoint containment failures")
                   .arg(visibleCount).arg(pairs).arg(midpointFailCount);
    }
}

// ---------------------------------------------------------------------------
// Diagnostic: Outer Ring Visibility (Asia-crossing bug investigation)
// ---------------------------------------------------------------------------

void RealWorldShortestPathTest::testDiag_OuterRingVisibility()
{
    skipIfNoData();
    auto hvg = mNetwork->getVisibilityGraph();
    QVERIFY(hvg);
    QVERIFY(hvg->getPolygons().size() >= 1);

    // The problematic segment from Singapore→PortSaid path
    // Bangladesh coast → Caspian coast (crosses entire South/Central Asia)
    auto v1 = makePoint(87.907, 22.425, "Bangladesh");
    auto v2 = makePoint(54.767, 41.037, "Caspian");

    // Midpoint = inland Pakistan
    double midLon = (87.907 + 54.767) / 2.0;
    double midLat = (22.425 + 41.037) / 2.0;
    GPoint midPt{units::angle::degree_t{midLon},
                 units::angle::degree_t{midLat}};

    qWarning() << "\n=== OUTER RING VISIBILITY DIAGNOSTIC ===";
    qWarning() << "Segment: (87.907, 22.425) → (54.767, 41.037)";
    qWarning() << "Midpoint:" << midLon << midLat << "(should be inland Pakistan)";

    // --- Test 1: Polygon topology ---
    for (int pi = 0; pi < hvg->getPolygons().size(); ++pi)
    {
        const auto &poly = hvg->getPolygons()[pi];
        qWarning() << "\nPolygon" << pi << ":"
                   << "outer=" << poly->outer().size() << "vertices,"
                   << "holes=" << poly->inners().size();

        // Test midpoint containment
        bool midInPoly = poly->isPointWithinPolygon(midPt);
        bool midInExterior = poly->isPointWithinExteriorRing(midPt);
        int midHoleIdx = poly->findContainingHoleIndex(midPt);

        qWarning() << "  Midpoint: inPolygon=" << midInPoly
                   << "inExterior=" << midInExterior
                   << "holeIdx=" << midHoleIdx;

        // Test outer ring crossing
        bool crossesOuter = poly->segmentCrossesOuterRing(v1, v2);
        qWarning() << "  segmentCrossesOuterRing:" << crossesOuter;

        // Test hole crossing
        auto segLine = std::make_shared<GLine>(v1, v2, FastConstruct);
        bool validWater = !poly->segmentCrossesHoles(segLine);
        qWarning() << "  !segmentCrossesHoles:" << validWater;

        // Test multiple intermediate points
        qWarning() << "  Intermediate point checks:";
        for (int k = 1; k <= 9; ++k)
        {
            double t = k / 10.0;
            double pLon = 87.907 * (1.0 - t) + 54.767 * t;
            double pLat = 22.425 * (1.0 - t) + 41.037 * t;
            GPoint pt{units::angle::degree_t{pLon},
                      units::angle::degree_t{pLat}};
            bool inPoly = poly->isPointWithinPolygon(pt);
            int hIdx = poly->findContainingHoleIndex(pt);
            qWarning().noquote()
                << QString("    t=%1: (%2, %3) inPoly=%4 holeIdx=%5")
                       .arg(t, 0, 'f', 1)
                       .arg(pLon, 0, 'f', 2)
                       .arg(pLat, 0, 'f', 2)
                       .arg(inPoly)
                       .arg(hIdx);
        }
    }

    // --- Test 2: Full visibility check ---
    bool visible = hvg->isVisible(v1, v2, 0);
    qWarning() << "\nisVisible(Bangladesh, Caspian):" << visible;
    qWarning() << "(Should be FALSE — segment crosses Asia)";

    // --- Test 3: Check vertex ring info ---
    // Find nearest L0 vertices to our test points
    const auto &lvl = hvg->getPolygons();  // L0 uses same polygons
    auto qt = hvg->getLevel0Quadtree();
    if (qt)
    {
        auto nearV1 = qt->findNearestNeighborPoint(v1);
        auto nearV2 = qt->findNearestNeighborPoint(v2);
        if (nearV1 && nearV2)
        {
            qWarning() << "\nNearest L0 vertex to Bangladesh:"
                       << nearV1->getLongitude().value()
                       << nearV1->getLatitude().value();
            qWarning() << "Nearest L0 vertex to Caspian:"
                       << nearV2->getLongitude().value()
                       << nearV2->getLatitude().value();

            // Check visibility between actual L0 vertices
            bool visL0 = hvg->isVisible(nearV1, nearV2, 0);
            qWarning() << "isVisible(nearL0_1, nearL0_2):" << visL0;

            // Check outer ring crossing for actual L0 vertices
            for (int pi = 0; pi < hvg->getPolygons().size(); ++pi)
            {
                bool crosses = hvg->getPolygons()[pi]->segmentCrossesOuterRing(
                    nearV1, nearV2);
                if (crosses)
                    qWarning() << "  L0 vertices cross outer ring of polygon"
                               << pi;
            }

            // --- Investigate bearing and ring membership ---
            qWarning() << "\n=== Water arc / bearing investigation ===";

            double dLon = nearV2->getLongitude().value()
                          - nearV1->getLongitude().value();
            if (dLon > 180.0) dLon -= 360.0;
            if (dLon < -180.0) dLon += 360.0;
            double dLat = nearV2->getLatitude().value()
                          - nearV1->getLatitude().value();
            double bearingToV2 = std::atan2(dLon, dLat) * 180.0 / M_PI;
            qWarning() << "Bearing from Bangladesh to Caspian:"
                       << bearingToV2 << "degrees (0=N, 90=E, -90=W)";
            qWarning() << "(NW direction ~ -45 to -90 degrees)";
            qWarning() << "The edge points"
                       << ((bearingToV2 > -135 && bearingToV2 < -20)
                           ? "NW/W (should be LAND → computeWaterArc should block)"
                           : "other direction");

            // Check: what polygon/hole is each vertex on?
            for (int pi = 0; pi < hvg->getPolygons().size(); ++pi)
            {
                const auto& poly = hvg->getPolygons()[pi];
                bool v1OnOuter = false, v2OnOuter = false;
                for (const auto& ov : poly->outer())
                {
                    if (ov && *ov == *nearV1) v1OnOuter = true;
                    if (ov && *ov == *nearV2) v2OnOuter = true;
                }
                if (v1OnOuter || v2OnOuter)
                {
                    qWarning().noquote()
                        << QString("  Poly %1: v1_onOuter=%2 v2_onOuter=%3")
                               .arg(pi).arg(v1OnOuter).arg(v2OnOuter);
                }

                int hIdx = 0;
                for (const auto& hole : poly->inners())
                {
                    bool v1OnHole = false, v2OnHole = false;
                    for (const auto& hv : hole)
                    {
                        if (hv && *hv == *nearV1) v1OnHole = true;
                        if (hv && *hv == *nearV2) v2OnHole = true;
                    }
                    if (v1OnHole || v2OnHole)
                    {
                        qWarning().noquote()
                            << QString("  Poly %1 hole %2 (%3 verts): "
                                       "v1_onHole=%4 v2_onHole=%5")
                                   .arg(pi).arg(hIdx).arg(hole.size())
                                   .arg(v1OnHole).arg(v2OnHole);
                    }
                    ++hIdx;
                    // Early exit after checking first few hundred holes
                    if (hIdx > 500 && !v1OnHole && !v2OnHole) break;
                }
            }
        }
    }

    // The segment SHOULD NOT be visible
    QVERIFY2(!visible,
             "Segment through Asia should NOT be visible — "
             "outer ring visibility gap detected");
}

// ---------------------------------------------------------------------------
// Diagnostic: Strait Visibility (verify cross-strait edges are NOT rejected)
// ---------------------------------------------------------------------------

void RealWorldShortestPathTest::testDiag_StraitVisibility()
{
    skipIfNoData();
    auto hvg = mNetwork->getVisibilityGraph();
    QVERIFY(hvg);

    struct StraitTest {
        const char* name;
        double lon1, lat1, lon2, lat2;
        bool shouldBeVisible;
    };

    StraitTest tests[] = {
        // Cross-strait edges (SHOULD be visible — valid water)
        {"Gibraltar Strait",           -5.6, 35.9,   -5.4, 36.1,  true},
        {"English Channel",            1.3, 51.0,    1.5, 50.8,   true},
        {"Open ocean (Arabian Sea)",   65.0, 15.0,   66.0, 15.5,  true},

        // Through-land edges (should NOT be visible)
        {"Bangladesh→Caspian",         87.907, 22.425, 54.767, 41.037, false},
        {"Across Florida",             -80.7, 28.3,   -82.0, 27.0, false},
    };

    qWarning() << "\n=== STRAIT VISIBILITY DIAGNOSTIC ===";

    int passed = 0, failed = 0;
    for (const auto& t : tests)
    {
        auto p1 = makePoint(t.lon1, t.lat1, "A");
        auto p2 = makePoint(t.lon2, t.lat2, "B");
        bool vis = hvg->isVisible(p1, p2, 0);

        bool ok = (vis == t.shouldBeVisible);
        if (ok) passed++; else failed++;

        qWarning().noquote()
            << QString("  %1: isVisible=%2 expected=%3 %4")
                   .arg(t.name, -30)
                   .arg(vis)
                   .arg(t.shouldBeVisible)
                   .arg(ok ? "✓" : "✗ MISMATCH");

        // Show details for mismatches
        if (!ok)
        {
            for (int pi = 0; pi < hvg->getPolygons().size(); ++pi)
            {
                bool crossOuter = hvg->getPolygons()[pi]->segmentCrossesOuterRing(p1, p2);
                auto seg = std::make_shared<GLine>(p1, p2, FastConstruct);
                bool crossHoles = hvg->getPolygons()[pi]->segmentCrossesHoles(seg);
                qWarning().noquote()
                    << QString("    Poly %1: crossOuter=%2 crossHoles=%3")
                           .arg(pi).arg(crossOuter).arg(crossHoles);
            }

            double midLon = (t.lon1 + t.lon2) / 2.0;
            double midLat = (t.lat1 + t.lat2) / 2.0;
            GPoint midPt{units::angle::degree_t{midLon},
                         units::angle::degree_t{midLat}};
            for (int pi = 0; pi < hvg->getPolygons().size(); ++pi)
            {
                bool inPoly = hvg->getPolygons()[pi]->isPointWithinPolygon(midPt);
                if (inPoly)
                    qWarning() << "    Midpoint in polygon" << pi;
            }
        }
    }

    qWarning().noquote()
        << QString("\n  RESULT: %1 passed, %2 failed").arg(passed).arg(failed);

    QVERIFY2(failed == 0, "Some strait visibility checks failed — see details above");
}

// ---------------------------------------------------------------------------
// Diagnostic: Export graph levels + corridors as GeoJSON for QGIS inspection
// ---------------------------------------------------------------------------

void RealWorldShortestPathTest::testDiag_ExportLevelsGeoJSON()
{
    skipIfNoData();
    auto hvg = mNetwork->getVisibilityGraph();
    QVERIFY(hvg);

    // Helper: export a level's edges as GeoJSON LineStrings
    auto exportLevelEdges = [&](int levelIdx, const QString& filename)
    {
        const auto& lvl = hvg->getLevel(levelIdx);
        QFile f(filename);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        QTextStream out(&f);

        out << "{\"type\":\"FeatureCollection\",\"features\":[\n";

        bool first = true;
        int edgeCount = 0;
        int n = lvl.vertices.size();

        for (int i = 0; i < n; ++i)
        {
            for (int j : lvl.adjacency[i])
            {
                if (j <= i) continue;  // undirected: only one direction
                double lon1 = lvl.vertices[i]->getLongitude().value();
                double lat1 = lvl.vertices[i]->getLatitude().value();
                double lon2 = lvl.vertices[j]->getLongitude().value();
                double lat2 = lvl.vertices[j]->getLatitude().value();

                // Skip antimeridian-wrapping edges for cleaner display
                if (std::abs(lon2 - lon1) > 180.0) continue;

                if (!first) out << ",\n";
                first = false;
                out << "{\"type\":\"Feature\","
                    << "\"properties\":{\"level\":" << levelIdx
                    << ",\"from\":" << i << ",\"to\":" << j << "},"
                    << "\"geometry\":{\"type\":\"LineString\","
                    << "\"coordinates\":["
                    << "[" << lon1 << "," << lat1 << "],"
                    << "[" << lon2 << "," << lat2 << "]"
                    << "]}}";
                edgeCount++;
            }
        }

        out << "\n]}\n";
        f.close();
        qWarning() << "Exported" << edgeCount << "edges for level"
                   << levelIdx << "to" << filename;
    };

    // Helper: export a level's vertices as GeoJSON Points
    auto exportLevelVertices = [&](int levelIdx, const QString& filename)
    {
        const auto& lvl = hvg->getLevel(levelIdx);
        QFile f(filename);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        QTextStream out(&f);

        out << "{\"type\":\"FeatureCollection\",\"features\":[\n";
        bool first = true;
        int n = lvl.vertices.size();

        for (int i = 0; i < n; ++i)
        {
            double lon = lvl.vertices[i]->getLongitude().value();
            double lat = lvl.vertices[i]->getLatitude().value();
            int polyId = (i < static_cast<int>(lvl.vertexPolygonId.size()))
                ? lvl.vertexPolygonId[i] : -1;
            int holeId = (i < static_cast<int>(lvl.vertexHoleId.size()))
                ? lvl.vertexHoleId[i] : -1;
            int degree = (i < static_cast<int>(lvl.adjacency.size()))
                ? static_cast<int>(lvl.adjacency[i].size()) : 0;

            if (!first) out << ",\n";
            first = false;
            out << "{\"type\":\"Feature\","
                << "\"properties\":{\"idx\":" << i
                << ",\"poly\":" << polyId
                << ",\"hole\":" << holeId
                << ",\"deg\":" << degree << "},"
                << "\"geometry\":{\"type\":\"Point\","
                << "\"coordinates\":[" << lon << "," << lat << "]}}";
        }

        out << "\n]}\n";
        f.close();
        qWarning() << "Exported" << n << "vertices for level"
                   << levelIdx << "to" << filename;
    };

    // Export L0 edges (only edges with degree > 2 to reduce size)
    qWarning() << "\n=== Exporting graph levels as GeoJSON ===";

    // L0: too many edges — export only non-boundary edges (degree > 2)
    {
        const auto& lvl = hvg->getLevel(0);
        QFile f("level0_express_edges.geojson");
        if (f.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            QTextStream out(&f);
            out << "{\"type\":\"FeatureCollection\",\"features\":[\n";
            bool first = true;
            int edgeCount = 0;

            for (int i = 0; i < lvl.vertices.size(); ++i)
            {
                if (lvl.adjacency[i].size() <= 2) continue;  // skip boundary-only
                for (int j : lvl.adjacency[i])
                {
                    if (j <= i) continue;
                    double lon1 = lvl.vertices[i]->getLongitude().value();
                    double lat1 = lvl.vertices[i]->getLatitude().value();
                    double lon2 = lvl.vertices[j]->getLongitude().value();
                    double lat2 = lvl.vertices[j]->getLatitude().value();
                    if (std::abs(lon2 - lon1) > 180.0) continue;

                    if (!first) out << ",\n";
                    first = false;
                    out << "{\"type\":\"Feature\","
                        << "\"properties\":{\"from\":" << i
                        << ",\"to\":" << j << "},"
                        << "\"geometry\":{\"type\":\"LineString\","
                        << "\"coordinates\":["
                        << "[" << lon1 << "," << lat1 << "],"
                        << "[" << lon2 << "," << lat2 << "]"
                        << "]}}";
                    edgeCount++;
                }
            }
            out << "\n]}\n";
            f.close();
            qWarning() << "Exported" << edgeCount
                       << "L0 express edges (degree>2) to level0_express_edges.geojson";
        }
    }

    // L1, L2 full edge export
    exportLevelEdges(1, "level1_edges.geojson");
    exportLevelEdges(2, "level2_edges.geojson");

    // Export L0 edges in the Singapore→PortSaid region
    // Region: lon [30, 110], lat [-10, 35] (Indian Ocean + Red Sea)
    qWarning() << "\n=== Exporting L0 edges in Singapore→PortSaid region ===";
    {
        const auto& lvl = hvg->getLevel(0);
        double regionMinLon = 30, regionMaxLon = 110;
        double regionMinLat = -10, regionMaxLat = 35;

        QFile f("level0_region_SG_PS.geojson");
        if (f.open(QIODevice::WriteOnly | QIODevice::Text))
        {
            QTextStream out(&f);
            out << "{\"type\":\"FeatureCollection\",\"features\":[\n";
            bool first = true;
            int edgeCount = 0;
            int vertCount = 0;
            int disconnectedVerts = 0;

            // Count vertices and edges in region
            for (int i = 0; i < lvl.vertices.size(); ++i)
            {
                double lon = lvl.vertices[i]->getLongitude().value();
                double lat = lvl.vertices[i]->getLatitude().value();
                if (lon < regionMinLon || lon > regionMaxLon
                    || lat < regionMinLat || lat > regionMaxLat)
                    continue;

                vertCount++;
                int degree = static_cast<int>(lvl.adjacency[i].size());
                if (degree <= 2) disconnectedVerts++;

                for (int j : lvl.adjacency[i])
                {
                    if (j <= i) continue;
                    double lon2 = lvl.vertices[j]->getLongitude().value();
                    double lat2 = lvl.vertices[j]->getLatitude().value();
                    if (std::abs(lon2 - lon) > 180.0) continue;

                    // Only export non-boundary edges (express + bridges)
                    // to keep file manageable
                    bool isBoundaryEdge = (std::abs(j - i) == 1
                        || (lvl.adjacency[i].size() == 2
                            && lvl.adjacency[j].size() == 2));

                    // Export ALL edges for smaller area, or just express for large
                    double edgeLen = std::sqrt((lon2-lon)*(lon2-lon)
                                              + (lat2-lat)*(lat2-lat));

                    // Export edges longer than 0.1 degrees (~10km)
                    if (edgeLen < 0.1) continue;

                    if (!first) out << ",\n";
                    first = false;

                    double lenKm = GSegment::haversineRaw(
                        lon, lat, lon2, lat2) / 1000.0;

                    out << "{\"type\":\"Feature\","
                        << "\"properties\":{\"len_km\":"
                        << QString::number(lenKm, 'f', 1)
                        << ",\"from\":" << i << ",\"to\":" << j << "},"
                        << "\"geometry\":{\"type\":\"LineString\","
                        << "\"coordinates\":["
                        << "[" << lon << "," << lat << "],"
                        << "[" << lon2 << "," << lat2 << "]"
                        << "]}}";
                    edgeCount++;
                }
            }
            out << "\n]}\n";
            f.close();
            qWarning() << "Region [30-110, -10-35]:"
                       << vertCount << "vertices,"
                       << edgeCount << "long edges (>10km),"
                       << disconnectedVerts << "boundary-only vertices";
        }
    }

    // Export L0 connectivity analysis: find gap locations
    qWarning() << "\n=== L0 Connectivity Analysis ===";
    {
        const auto& lvl = hvg->getLevel(0);
        int n = lvl.vertices.size();

        // BFS to find connected components in the SG-PS region
        double rMinLon = 30, rMaxLon = 110;
        double rMinLat = -10, rMaxLat = 35;

        std::vector<bool> inRegion(n, false);
        int regionCount = 0;
        for (int i = 0; i < n; ++i)
        {
            double lon = lvl.vertices[i]->getLongitude().value();
            double lat = lvl.vertices[i]->getLatitude().value();
            if (lon >= rMinLon && lon <= rMaxLon
                && lat >= rMinLat && lat <= rMaxLat)
            {
                inRegion[i] = true;
                regionCount++;
            }
        }

        // BFS for components within region
        std::vector<int> compId(n, -1);
        int numComps = 0;
        for (int i = 0; i < n; ++i)
        {
            if (!inRegion[i] || compId[i] >= 0) continue;
            int cid = numComps++;
            std::queue<int> q;
            q.push(i);
            compId[i] = cid;
            int compSize = 0;
            while (!q.empty())
            {
                int u = q.front(); q.pop();
                compSize++;
                for (int v : lvl.adjacency[u])
                {
                    if (compId[v] < 0 && inRegion[v])
                    {
                        compId[v] = cid;
                        q.push(v);
                    }
                }
            }
            if (compSize > 10)
                qWarning().noquote()
                    << QString("  Component %1: %2 vertices")
                           .arg(cid).arg(compSize);
        }
        qWarning() << "Total:" << regionCount << "vertices in region,"
                   << numComps << "connected components";

        // Check which component Singapore and PortSaid are in
        auto sgPt = makePoint(103.82, 1.35, "SG");
        auto psPt = makePoint(32.28, 31.27, "PS");
        auto nearSG = hvg->getLevel0Quadtree()->findNearestNeighborPoint(sgPt);
        auto nearPS = hvg->getLevel0Quadtree()->findNearestNeighborPoint(psPt);

        if (nearSG && nearPS)
        {
            auto sgIt = lvl.vertexIndex.find(nearSG);
            auto psIt = lvl.vertexIndex.find(nearPS);
            if (sgIt != lvl.vertexIndex.end() && psIt != lvl.vertexIndex.end())
            {
                int sgComp = compId[sgIt->second];
                int psComp = compId[psIt->second];
                qWarning() << "Singapore vertex:" << sgIt->second
                           << "component:" << sgComp;
                qWarning() << "PortSaid vertex:" << psIt->second
                           << "component:" << psComp;
                qWarning() << "Same component?"
                           << (sgComp == psComp ? "YES" : "NO — DISCONNECTED!");
            }
        }
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
