/**
 * @file benchmark_paper.cpp
 * @brief Benchmark all 12 paper routes WITH pre-computed adjacency cache.
 *
 * Runs three methods per route:
 *   1. Hierarchical A* (corridor-constrained)
 *   2. Flat A* (haversine heuristic, full L0 graph)
 *   3. Flat Dijkstra (h=0, full L0 graph)
 *
 * Outputs:
 *   - Hierarchy diagnostics (Table 2 in paper)
 *   - Per-route results for all 3 methods (Tables 3 & 4 in paper)
 *   - Path validation (land-crossing checks)
 *   - GeoJSON export for visual verification
 *   - CSV summary for easy import into paper tables
 */

#include "network/gline.h"
#include "network/gpoint.h"
#include "network/hierarchicalvisibilitygraph.h"
#include "network/optimizednetwork.h"
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTextStream>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <sstream>

using namespace ShipNetSimCore;

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

static std::shared_ptr<GPoint> makePoint(double lon, double lat,
                                          const QString &id)
{
    return std::make_shared<GPoint>(units::angle::degree_t(lon),
                                    units::angle::degree_t(lat), id);
}

static double pathLengthKm(const ShortestPathResult &result)
{
    double m = 0.0;
    for (const auto &line : result.lines)
        m += line->length().value();
    return m / 1000.0;
}

static double haversineKm(double lon1, double lat1,
                           double lon2, double lat2)
{
    constexpr double R = 6371.0;
    double dLat = (lat2 - lat1) * M_PI / 180.0;
    double dLon = (lon2 - lon1) * M_PI / 180.0;
    if (dLon > M_PI)  dLon -= 2.0 * M_PI;
    if (dLon < -M_PI) dLon += 2.0 * M_PI;
    double a = std::sin(dLat / 2) * std::sin(dLat / 2) +
               std::cos(lat1 * M_PI / 180.0) *
               std::cos(lat2 * M_PI / 180.0) *
               std::sin(dLon / 2) * std::sin(dLon / 2);
    return R * 2.0 * std::asin(std::sqrt(a));
}

static bool verifyPathContinuity(const ShortestPathResult &result)
{
    if (result.lines.size() < 2) return true;
    for (int i = 0; i < result.lines.size() - 1; ++i) {
        auto end   = result.lines[i]->endPoint();
        auto start = result.lines[i + 1]->startPoint();
        if (*end != *start) return false;
    }
    return true;
}

static int countLandCrossings(const ShortestPathResult &result,
                               HierarchicalVisibilityGraph *hvg)
{
    int violations = 0;
    for (const auto &line : result.lines) {
        double lenKm = line->length().value() / 1000.0;
        if (lenKm < 2.0) continue;
        if (!hvg->isSegmentVisible(line))
            violations++;
    }
    return violations;
}

static void exportGeoJSON(const ShortestPathResult &result,
                           const QString &routeName,
                           double distKm, double timeSec,
                           const QString &method,
                           const QString &outputDir)
{
    if (!result.isValid() || result.points.isEmpty()) return;

    QString safeName = routeName;
    safeName.replace(" ", "").replace("->", "_");
    QString filename = QString("%1/route_%2_%3.geojson")
                           .arg(outputDir, safeName, method);

    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return;

    QTextStream out(&file);
    out << "{\n\"type\": \"FeatureCollection\",\n\"features\": [{\n"
        << "  \"type\": \"Feature\",\n"
        << "  \"properties\": {\n"
        << "    \"route\": \"" << routeName << "\",\n"
        << "    \"method\": \"" << method << "\",\n"
        << "    \"distance_km\": " << distKm << ",\n"
        << "    \"points\": " << result.points.size() << ",\n"
        << "    \"time_s\": " << timeSec << "\n"
        << "  },\n"
        << "  \"geometry\": {\n"
        << "    \"type\": \"LineString\",\n"
        << "    \"coordinates\": [\n";

    for (int i = 0; i < result.points.size(); ++i) {
        double lon = result.points[i]->getLongitude().value();
        double lat = result.points[i]->getLatitude().value();
        out << "      [" << lon << ", " << lat << "]";
        if (i + 1 < result.points.size()) out << ",";
        out << "\n";
    }

    out << "    ]\n  }\n}]\n}\n";
    file.close();
}

// -----------------------------------------------------------------------
// Route definitions (12 paper routes)
// -----------------------------------------------------------------------

struct Route {
    const char *from;
    const char *to;
    double lonA, latA, lonB, latB;
    const char *category;
};

static const Route ROUTES[] = {
    // Short (<500 km)
    {"Antwerpen",  "Zeebrugge",   4.40,  51.22,   3.20,  51.33, "Short"},
    {"Santos",     "Paranagua",  -46.31, -23.96, -48.51, -25.50, "Short"},
    {"NewYork",    "Boston",     -74.01,  40.71, -71.06,  42.36, "Short"},
    // Medium (500-3500 km)
    {"Brisbane",   "Newcastle",  153.03, -27.47, 151.78, -32.93, "Medium"},
    {"Rotterdam",  "Algeciras",    4.48,  51.92,  -5.45,  36.14, "Medium"},
    {"NewYork",    "Colon",      -74.01,  40.71, -79.91,   9.35, "Medium"},
    // Long (3500-7000 km)
    {"Colombo",    "Jeddah",      79.85,   6.93,  39.17,  21.49, "Long"},
    {"Singapore",  "Busan",      103.82,   1.35, 129.08,  35.18, "Long"},
    {"Rotterdam",  "NewYork",      4.48,  51.92, -74.01,  40.71, "Long"},
    {"Santos",     "CapeTown",   -46.31, -23.96,  18.42, -33.92, "Long"},
    // Very long (>7000 km)
    {"CapeTown",   "Mumbai",      18.42, -33.92,  72.88,  19.08, "VeryLong"},
    {"Yokohama",   "LongBeach",  139.64,  35.44,-118.19,  33.77, "VeryLong"},
};

static constexpr int NUM_ROUTES = sizeof(ROUTES) / sizeof(ROUTES[0]);

// -----------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // Locate data — prefer build-dir copy, fallback to source
    QString dataPath =
        QCoreApplication::applicationDirPath() + "/data/ne_10m_ocean.shp";
    if (!QFile::exists(dataPath)) {
        dataPath = "/home/ahmed/Documents/dev/ShipNetSim/src/data/"
                   "ne_10m_ocean.shp";
    }
    if (!QFile::exists(dataPath)) {
        qCritical() << "Ocean shapefile not found. Tried build/data and "
                        "src/data.";
        return 1;
    }

    // Output directory for GeoJSON files
    QString outputDir = QCoreApplication::applicationDirPath() +
                        "/benchmark_output";
    QDir().mkpath(outputDir);

    QTextStream out(stdout);

    // ===================================================================
    // 1. Load network (with cache)
    // ===================================================================
    out << "=== BENCHMARK: CACHED MODE ===\n";
    out << "Loading: " << dataPath << "\n";
    out.flush();

    QElapsedTimer loadTimer;
    loadTimer.start();
    auto network = std::make_shared<OptimizedNetwork>(dataPath, "Ocean");
    double loadSec = loadTimer.elapsed() / 1000.0;

    auto hvg = network->getVisibilityGraph();
    if (!hvg || hvg->getPolygons().isEmpty()) {
        qCritical() << "Failed to load polygons";
        return 1;
    }
    out << "Loaded in " << loadSec << " s\n\n";

    // ===================================================================
    // 2. Hierarchy diagnostics (for Table 2)
    // ===================================================================
    auto diag = hvg->getDiagnostics();
    out << "--- Hierarchy Diagnostics ---\n";
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "  %-8s %10s %12s %12s %12s\n",
                      "Level", "Vertices", "Edges",
                      "Tol(km)", "MaxDist(km)");
        out << buf;
        for (int i = 0; i < 4; ++i) {
            const auto &lv = diag.levels[i];
            char lvl[8];
            std::snprintf(lvl, sizeof(lvl), "L%d", i);
            std::snprintf(buf, sizeof(buf),
                          "  %-8s %10d %12lld %12.1f %12.1f\n",
                          lvl, lv.vertexCount, lv.edgeCount,
                          lv.toleranceMeters / 1000.0,
                          lv.maxDistanceMeters / 1000.0);
            out << buf;
        }
    }
    out << "  L0 adjacency ready: "
        << (diag.level0AdjReady ? "YES" : "NO") << "\n";
    out << "  Coarse adjacency ready: "
        << (diag.coarseAdjReady ? "YES" : "NO") << "\n";
    out << "  Polygon graph polygons: "
        << diag.polygonGraphPolygonCount << "\n\n";

    // ===================================================================
    // 3. Run all 12 routes × 3 methods
    // ===================================================================

    struct Result {
        QString route;
        QString category;
        double havKm;
        // Hierarchical
        double hierTimeSec;
        double hierDistKm;
        int    hierWaypoints;
        int    hierLandCross;
        bool   hierValid;
        // Flat A*
        double flatATimeSec;
        double flatADistKm;
        int    flatAWaypoints;
        bool   flatAValid;
        // Flat Dijkstra
        double flatDTimeSec;
        double flatDDistKm;
        int    flatDWaypoints;
        bool   flatDValid;
    };

    QVector<Result> results;

    for (int r = 0; r < NUM_ROUTES; ++r)
    {
        const auto &rt = ROUTES[r];
        QString routeName = QString("%1 -> %2").arg(rt.from, rt.to);
        double hav = haversineKm(rt.lonA, rt.latA, rt.lonB, rt.latB);

        auto start = makePoint(rt.lonA, rt.latA, rt.from);
        auto goal  = makePoint(rt.lonB, rt.latB, rt.to);

        out << "\n========================================"
               "========================================\n";
        out << QString("Route %1/%2: %3  [%4]  (haversine: %5 km)\n")
               .arg(r + 1).arg(NUM_ROUTES).arg(routeName)
               .arg(rt.category).arg(hav, 0, 'f', 0);
        out << "========================================"
               "========================================\n";
        out.flush();

        Result res;
        res.route = routeName;
        res.category = rt.category;
        res.havKm = hav;

        // --- Hierarchical A* ---
        {
            out << "  [Hierarchical A*] running...\n";
            out.flush();
            QElapsedTimer timer;
            timer.start();
            auto result = hvg->findShortestPath(start, goal);
            res.hierTimeSec = timer.elapsed() / 1000.0;
            res.hierValid = result.isValid();
            res.hierDistKm = res.hierValid ? pathLengthKm(result) : -1;
            res.hierWaypoints = res.hierValid ? result.points.size() : 0;
            res.hierLandCross = (res.hierValid)
                ? countLandCrossings(result, hvg.get()) : -1;

            bool continuous = res.hierValid && verifyPathContinuity(result);

            out << QString("  [Hierarchical A*] %1 s | %2 km | %3 pts"
                           " | land:%4 | cont:%5 | %6\n")
                   .arg(res.hierTimeSec, 8, 'f', 2)
                   .arg(res.hierDistKm, 8, 'f', 0)
                   .arg(res.hierWaypoints, 5)
                   .arg(res.hierLandCross, 3)
                   .arg(continuous ? "OK" : "FAIL")
                   .arg(res.hierValid ? "OK" : "FAIL");
            out.flush();

            if (res.hierValid)
                exportGeoJSON(result, routeName, res.hierDistKm,
                              res.hierTimeSec, "hier", outputDir);
        }

        // --- Flat A* ---
        {
            out << "  [Flat A*]          running...\n";
            out.flush();
            QElapsedTimer timer;
            timer.start();
            auto result = hvg->findShortestPathFlatAStar(start, goal);
            res.flatATimeSec = timer.elapsed() / 1000.0;
            res.flatAValid = result.isValid();
            res.flatADistKm = res.flatAValid ? pathLengthKm(result) : -1;
            res.flatAWaypoints = res.flatAValid ? result.points.size() : 0;

            out << QString("  [Flat A*]          %1 s | %2 km | %3 pts"
                           " | %4\n")
                   .arg(res.flatATimeSec, 8, 'f', 2)
                   .arg(res.flatADistKm, 8, 'f', 0)
                   .arg(res.flatAWaypoints, 5)
                   .arg(res.flatAValid ? "OK" : "FAIL");
            out.flush();

            if (res.flatAValid)
                exportGeoJSON(result, routeName, res.flatADistKm,
                              res.flatATimeSec, "flatA", outputDir);
        }

        // --- Flat Dijkstra ---
        {
            out << "  [Flat Dijkstra]    running...\n";
            out.flush();
            QElapsedTimer timer;
            timer.start();
            auto result = hvg->findShortestPathFlat(start, goal);
            res.flatDTimeSec = timer.elapsed() / 1000.0;
            res.flatDValid = result.isValid();
            res.flatDDistKm = res.flatDValid ? pathLengthKm(result) : -1;
            res.flatDWaypoints = res.flatDValid ? result.points.size() : 0;

            out << QString("  [Flat Dijkstra]    %1 s | %2 km | %3 pts"
                           " | %4\n")
                   .arg(res.flatDTimeSec, 8, 'f', 2)
                   .arg(res.flatDDistKm, 8, 'f', 0)
                   .arg(res.flatDWaypoints, 5)
                   .arg(res.flatDValid ? "OK" : "FAIL");
            out.flush();

            if (res.flatDValid)
                exportGeoJSON(result, routeName, res.flatDDistKm,
                              res.flatDTimeSec, "flatD", outputDir);
        }

        // --- Detour ratio & quality comparison ---
        if (res.hierValid && res.flatDValid) {
            double hierRho = res.hierDistKm / hav;
            double deltaL = (res.hierDistKm - res.flatDDistKm)
                            / res.flatDDistKm * 100.0;
            out << QString("  Detour ratio (hier): %1 | "
                           "DeltaL vs Dijkstra: %2%\n")
                   .arg(hierRho, 0, 'f', 2)
                   .arg(deltaL, 0, 'f', 2);
        }
        out.flush();

        results.append(res);
    }

    // ===================================================================
    // 4. Summary tables
    // ===================================================================

    out << "\n\n";
    out << "============================================================"
           "========================================================\n";
    out << "SUMMARY TABLE (Paper Table 3: Hierarchical A* on 1:10M)\n";
    out << "============================================================"
           "========================================================\n";
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "%-28s %8s %8s %6s %8s %5s %5s\n",
                      "Route", "d_h(km)", "L(km)", "rho",
                      "t(s)", "W", "Land");
        out << buf;
        std::snprintf(buf, sizeof(buf),
                      "%-28s %8s %8s %6s %8s %5s %5s\n",
                      "----------------------------",
                      "--------", "--------", "------",
                      "--------", "-----", "-----");
        out << buf;
        for (const auto &r : results) {
            double rho = (r.hierDistKm > 0 && r.havKm > 0)
                ? r.hierDistKm / r.havKm : -1;
            std::snprintf(buf, sizeof(buf),
                          "%-28s %8.0f %8.0f %6.2f %8.2f %5d %5d\n",
                          r.route.toUtf8().constData(),
                          r.havKm, r.hierDistKm, rho,
                          r.hierTimeSec, r.hierWaypoints,
                          r.hierLandCross);
            out << buf;
        }
    }

    out << "\n\n";
    out << "============================================================"
           "========================================================\n";
    out << "SUMMARY TABLE (Paper Table 4: Hier vs Flat comparison)\n";
    out << "============================================================"
           "========================================================\n";
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "%-28s | %8s %8s | %8s %8s | %8s %8s | %6s\n",
                      "Route", "Hier(s)", "L(km)",
                      "FlatA(s)", "L(km)",
                      "FlatD(s)", "L(km)", "DL(%)");
        out << buf;
        std::snprintf(buf, sizeof(buf),
                      "%-28s | %8s %8s | %8s %8s | %8s %8s | %6s\n",
                      "----------------------------",
                      "--------", "--------",
                      "--------", "--------",
                      "--------", "--------", "------");
        out << buf;
        for (const auto &r : results) {
            double deltaL = (r.hierValid && r.flatDValid
                             && r.flatDDistKm > 0)
                ? (r.hierDistKm - r.flatDDistKm) / r.flatDDistKm * 100.0
                : 0.0;
            std::snprintf(buf, sizeof(buf),
                          "%-28s | %8.2f %8.0f | %8.2f %8.0f |"
                          " %8.2f %8.0f | %6.2f\n",
                          r.route.toUtf8().constData(),
                          r.hierTimeSec, r.hierDistKm,
                          r.flatATimeSec, r.flatADistKm,
                          r.flatDTimeSec, r.flatDDistKm,
                          deltaL);
            out << buf;
        }
    }

    // ===================================================================
    // 5. CSV export
    // ===================================================================
    {
        QString csvPath = outputDir + "/benchmark_cached_results.csv";
        QFile csvFile(csvPath);
        if (csvFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream csv(&csvFile);
            csv << "Route,Category,d_h_km,"
                   "Hier_t_s,Hier_L_km,Hier_W,Hier_Land,"
                   "FlatA_t_s,FlatA_L_km,FlatA_W,"
                   "FlatD_t_s,FlatD_L_km,FlatD_W,"
                   "Rho,DeltaL_pct\n";
            for (const auto &r : results) {
                double rho = (r.hierDistKm > 0 && r.havKm > 0)
                    ? r.hierDistKm / r.havKm : -1;
                double deltaL = (r.hierValid && r.flatDValid
                                 && r.flatDDistKm > 0)
                    ? (r.hierDistKm - r.flatDDistKm) / r.flatDDistKm * 100.0
                    : 0.0;
                csv << r.route << "," << r.category << ","
                    << r.havKm << ","
                    << r.hierTimeSec << "," << r.hierDistKm << ","
                    << r.hierWaypoints << "," << r.hierLandCross << ","
                    << r.flatATimeSec << "," << r.flatADistKm << ","
                    << r.flatAWaypoints << ","
                    << r.flatDTimeSec << "," << r.flatDDistKm << ","
                    << r.flatDWaypoints << ","
                    << rho << "," << deltaL << "\n";
            }
            csvFile.close();
            out << "\nCSV exported: " << csvPath << "\n";
        }
    }

    out << "\nGeoJSON files in: " << outputDir << "\n";
    out << "DONE\n";
    out.flush();

    return 0;
}
