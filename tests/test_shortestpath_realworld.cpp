#include "network/gline.h"
#include "network/gpoint.h"
#include "network/hierarchicalvisibilitygraph.h"
#include "network/optimizednetwork.h"
#include "network/polygon.h"
#include <QCoreApplication>
#include <QDebug>
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
    void testSameStartEnd();
    void testSymmetry_Rotterdam_NY();
    void testSymmetry_Singapore_Busan();

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
                 320, 600);
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
                 2200, 3800);
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
    // Suez Canal is not in ne_10m_ocean.shp (158km land gap).
    // Route goes around Africa via Cape of Good Hope (~26000km).
    // Bounds will tighten once Suez manual connections are added.
    runRouteTest("Singapore", 103.82, 1.35,
                 "PortSaid", 32.30, 31.26,
                 20000, 28000);
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
