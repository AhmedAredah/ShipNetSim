/**
 * @file benchmark_nocache.cpp
 * @brief Benchmark all 12 paper routes WITHOUT pre-computed adjacency cache.
 *
 * Tests three methods per route:
 *   1. Hierarchical A* (lazy corridor-filtered visibility)
 *   2. Flat A* (haversine heuristic, on-demand L0 visibility)
 *   3. Flat Dijkstra (h=0, on-demand L0 visibility)
 *
 * Flat methods are skipped when hierarchical A* exceeds a time threshold
 * (configurable via FLAT_TIMEOUT_SEC env var, default 600s) because flat
 * methods are always slower and would take hours/days on longer routes.
 *
 * The shapefile is copied to a temp directory WITHOUT the .hvg_adj cache
 * to ensure clean no-cache operation.
 *
 * Outputs:
 *   - Per-route results (Table 5 in paper)
 *   - CSV summary for easy import
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

/// Copy shapefile components (shp, shx, dbf, prj, cpg) to dest dir,
/// deliberately excluding .hvg_adj cache.
static bool copyShapefileWithoutCache(const QString &srcShp,
                                       const QString &destDir)
{
    QDir dir;
    if (!dir.mkpath(destDir)) return false;

    QFileInfo info(srcShp);
    QString base = info.absolutePath() + "/" + info.completeBaseName();
    QString destBase = destDir + "/" + info.completeBaseName();

    QStringList exts = {"shp", "shx", "dbf", "prj", "cpg"};
    for (const auto &ext : exts) {
        QString src = base + "." + ext;
        QString dst = destBase + "." + ext;
        if (QFile::exists(src)) {
            QFile::remove(dst); // remove old copy if any
            if (!QFile::copy(src, dst)) {
                qWarning() << "Failed to copy" << src << "to" << dst;
                return false;
            }
        }
    }

    // Ensure NO cache file exists
    QString cacheFile = destBase + ".hvg_adj";
    QFile::remove(cacheFile);

    return true;
}

// -----------------------------------------------------------------------
// Route definitions (12 paper routes — identical to benchmark_paper.cpp)
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

    // Configurable flat-method timeout threshold (seconds).
    // If hierarchical takes longer than this, flat methods are skipped.
    double flatTimeoutSec = 600.0;
    QByteArray envTimeout = qgetenv("FLAT_TIMEOUT_SEC");
    if (!envTimeout.isEmpty())
        flatTimeoutSec = envTimeout.toDouble();

    // Locate source shapefile
    QString srcDataPath =
        QCoreApplication::applicationDirPath() + "/data/ne_10m_ocean.shp";
    if (!QFile::exists(srcDataPath)) {
        srcDataPath = "/home/ahmed/Documents/dev/ShipNetSim/src/data/"
                      "ne_10m_ocean.shp";
    }
    if (!QFile::exists(srcDataPath)) {
        qCritical() << "Ocean shapefile not found.";
        return 1;
    }

    // Copy to temp dir WITHOUT cache
    QString tmpDir = QDir::tempPath() + "/shipnetsim_nocache";
    QString tmpShp = tmpDir + "/ne_10m_ocean.shp";

    QTextStream out(stdout);

    out << "=== BENCHMARK: NO-CACHE MODE ===\n";
    out << "Flat method timeout threshold: " << flatTimeoutSec << " s\n";
    out << "Copying shapefile to " << tmpDir << " (without .hvg_adj)...\n";
    out.flush();

    if (!copyShapefileWithoutCache(srcDataPath, tmpDir)) {
        qCritical() << "Failed to copy shapefile to temp dir";
        return 1;
    }

    // Verify no cache file
    QString cacheCheck = tmpDir + "/ne_10m_ocean.hvg_adj";
    if (QFile::exists(cacheCheck)) {
        qCritical() << "Cache file still exists after copy — aborting";
        return 1;
    }

    out << "Loading shapefile (NO CACHE)...\n";
    out.flush();
    QElapsedTimer loadTimer;
    loadTimer.start();
    auto network = std::make_shared<OptimizedNetwork>(tmpShp, "Ocean");
    double loadSec = loadTimer.elapsed() / 1000.0;

    auto hvg = network->getVisibilityGraph();
    if (!hvg || hvg->getPolygons().isEmpty()) {
        qCritical() << "Failed to load polygons";
        return 1;
    }

    auto diag = hvg->getDiagnostics();
    out << "Loaded in " << loadSec << " s"
        << " | L0 adj: " << (diag.level0AdjReady ? "YES" : "NO")
        << " | Coarse adj: " << (diag.coarseAdjReady ? "YES" : "NO")
        << "\n\n";
    out.flush();

    // ===================================================================
    // Run all 12 routes
    // ===================================================================

    struct Result {
        QString route;
        QString category;
        double havKm;
        double hierTimeSec;
        double hierDistKm;
        bool   hierValid;
        double flatATimeSec;
        double flatADistKm;
        bool   flatAValid;
        bool   flatASkipped;
        double flatDTimeSec;
        double flatDDistKm;
        bool   flatDValid;
        bool   flatDSkipped;
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
        res.flatASkipped = false;
        res.flatDSkipped = false;

        // --- 1. Hierarchical A* ---
        {
            out << "  [Hierarchical A*] running...\n";
            out.flush();
            QElapsedTimer timer;
            timer.start();
            auto result = hvg->findShortestPath(start, goal);
            res.hierTimeSec = timer.elapsed() / 1000.0;
            res.hierValid = result.isValid();
            res.hierDistKm = res.hierValid ? pathLengthKm(result) : -1;

            out << QString("  [Hierarchical A*] %1 s | %2 km | %3 pts"
                           " | %4\n")
                   .arg(res.hierTimeSec, 10, 'f', 1)
                   .arg(res.hierDistKm, 8, 'f', 0)
                   .arg(res.hierValid ? result.points.size() : 0, 5)
                   .arg(res.hierValid ? "OK" : "FAIL");
            out.flush();
        }

        bool skipFlat = (res.hierTimeSec > flatTimeoutSec);

        // --- 2. Flat A* ---
        if (skipFlat) {
            out << QString("  [Flat A*]          SKIPPED (hier > %1 s)\n")
                   .arg(flatTimeoutSec, 0, 'f', 0);
            res.flatASkipped = true;
            res.flatAValid = false;
            res.flatATimeSec = -1;
            res.flatADistKm = -1;
        } else {
            out << "  [Flat A*]          running...\n";
            out.flush();
            QElapsedTimer timer;
            timer.start();
            auto result = hvg->findShortestPathFlatAStar(start, goal);
            res.flatATimeSec = timer.elapsed() / 1000.0;
            res.flatAValid = result.isValid();
            res.flatADistKm = res.flatAValid ? pathLengthKm(result) : -1;

            out << QString("  [Flat A*]          %1 s | %2 km | %3 pts"
                           " | %4\n")
                   .arg(res.flatATimeSec, 10, 'f', 1)
                   .arg(res.flatADistKm, 8, 'f', 0)
                   .arg(res.flatAValid ? result.points.size() : 0, 5)
                   .arg(res.flatAValid ? "OK" : "FAIL");
            out.flush();
        }

        // --- 3. Flat Dijkstra ---
        if (skipFlat) {
            out << QString("  [Flat Dijkstra]    SKIPPED (hier > %1 s)\n")
                   .arg(flatTimeoutSec, 0, 'f', 0);
            res.flatDSkipped = true;
            res.flatDValid = false;
            res.flatDTimeSec = -1;
            res.flatDDistKm = -1;
        } else {
            out << "  [Flat Dijkstra]    running...\n";
            out.flush();
            QElapsedTimer timer;
            timer.start();
            auto result = hvg->findShortestPathFlat(start, goal);
            res.flatDTimeSec = timer.elapsed() / 1000.0;
            res.flatDValid = result.isValid();
            res.flatDDistKm = res.flatDValid ? pathLengthKm(result) : -1;

            out << QString("  [Flat Dijkstra]    %1 s | %2 km | %3 pts"
                           " | %4\n")
                   .arg(res.flatDTimeSec, 10, 'f', 1)
                   .arg(res.flatDDistKm, 8, 'f', 0)
                   .arg(res.flatDValid ? result.points.size() : 0, 5)
                   .arg(res.flatDValid ? "OK" : "FAIL");
            out.flush();
        }

        results.append(res);
    }

    // ===================================================================
    // Summary table (Paper Table 5)
    // ===================================================================

    out << "\n\n";
    out << "============================================================"
           "=======================================\n";
    out << "SUMMARY TABLE (Paper Table 5: No-cache comparison)\n";
    out << "============================================================"
           "=======================================\n";
    out << QString("%-28s %8s | %10s | %10s | %10s\n")
           .arg("Route", "d_h(km)", "Hier(s)", "FlatA(s)", "FlatD(s)");
    out << QString("%-28s %8s | %10s | %10s | %10s\n")
           .arg("----------------------------", "--------",
                "----------", "----------", "----------");
    for (const auto &r : results) {
        auto fmtTime = [](double t, bool skipped, bool valid) -> QString {
            if (skipped) return "DNF";
            if (!valid)  return "FAIL";
            return QString::number(t, 'f', 1);
        };
        out << QString("%-28s %8.0f | %10s | %10s | %10s\n")
               .arg(r.route)
               .arg(r.havKm)
               .arg(fmtTime(r.hierTimeSec, false, r.hierValid))
               .arg(fmtTime(r.flatATimeSec, r.flatASkipped, r.flatAValid))
               .arg(fmtTime(r.flatDTimeSec, r.flatDSkipped, r.flatDValid));
    }

    // ===================================================================
    // CSV export
    // ===================================================================
    {
        QString outputDir = QCoreApplication::applicationDirPath() +
                            "/benchmark_output";
        QDir().mkpath(outputDir);
        QString csvPath = outputDir + "/benchmark_nocache_results.csv";
        QFile csvFile(csvPath);
        if (csvFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream csv(&csvFile);
            csv << "Route,Category,d_h_km,"
                   "Hier_t_s,Hier_L_km,Hier_valid,"
                   "FlatA_t_s,FlatA_L_km,FlatA_valid,FlatA_skipped,"
                   "FlatD_t_s,FlatD_L_km,FlatD_valid,FlatD_skipped\n";
            for (const auto &r : results) {
                csv << r.route << "," << r.category << ","
                    << r.havKm << ","
                    << r.hierTimeSec << "," << r.hierDistKm << ","
                    << r.hierValid << ","
                    << r.flatATimeSec << "," << r.flatADistKm << ","
                    << r.flatAValid << "," << r.flatASkipped << ","
                    << r.flatDTimeSec << "," << r.flatDDistKm << ","
                    << r.flatDValid << "," << r.flatDSkipped << "\n";
            }
            csvFile.close();
            out << "\nCSV exported: " << csvPath << "\n";
        }
    }

    out << "\nDONE\n";
    out.flush();

    return 0;
}
