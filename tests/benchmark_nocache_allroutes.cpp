/**
 * @file benchmark_nocache_allroutes.cpp
 * @brief Benchmark all 12 paper routes WITHOUT cache: hierarchical vs flat A* vs flat Dijkstra.
 *
 * Results are printed immediately after each method completes.
 * Flat methods are skipped if hierarchical exceeds a time threshold.
 */

#include "network/gline.h"
#include "network/gpoint.h"
#include "network/hierarchicalvisibilitygraph.h"
#include "network/optimizednetwork.h"
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QTextStream>
#include <cmath>

using namespace ShipNetSimCore;

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

static double haversineKm(double lon1, double lat1, double lon2, double lat2)
{
    double dLat = (lat2 - lat1) * M_PI / 180.0;
    double dLon = (lon2 - lon1) * M_PI / 180.0;
    double a = std::sin(dLat / 2) * std::sin(dLat / 2) +
               std::cos(lat1 * M_PI / 180.0) * std::cos(lat2 * M_PI / 180.0) *
               std::sin(dLon / 2) * std::sin(dLon / 2);
    return 6371.0 * 2.0 * std::asin(std::sqrt(a));
}

struct Route {
    const char *from;
    const char *to;
    double lonA, latA, lonB, latB;
};

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QString dataPath = "/tmp/nocache_ocean/ne_10m_ocean.shp";
    if (!QFile::exists(dataPath)) {
        qCritical() << "No-cache shapefile not found:" << dataPath;
        return 1;
    }

    // Skip flat methods if hierarchical takes longer than this
    const double FLAT_SKIP_SEC = 300.0;

    QTextStream out(stdout);

    out << "Loading shapefile (NO CACHE)...\n";
    out.flush();
    QElapsedTimer loadTimer;
    loadTimer.start();
    auto network = std::make_shared<OptimizedNetwork>(dataPath, "Ocean");
    double loadSec = loadTimer.elapsed() / 1000.0;
    out << "Loaded in " << loadSec << " s\n\n";
    out.flush();

    auto hvg = network->getVisibilityGraph();
    if (!hvg || hvg->getPolygons().isEmpty()) {
        qCritical() << "Failed to load polygons";
        return 1;
    }

    Route routes[] = {
        {"Antwerpen",  "Zeebrugge",   4.40,  51.22,   3.20,  51.33},
        {"Santos",     "Paranagua",  -46.31, -23.96, -48.51, -25.50},
        {"NewYork",    "Boston",     -74.01,  40.71, -71.06,  42.36},
        {"Brisbane",   "Newcastle",  153.03, -27.47, 151.78, -32.93},
        {"Rotterdam",  "Algeciras",    4.48,  51.92,  -5.45,  36.14},
        {"NewYork",    "Colon",      -74.01,  40.71, -79.91,   9.35},
        {"Colombo",    "Jeddah",      79.85,   6.93,  39.17,  21.49},
        {"Singapore",  "Busan",      103.82,   1.35, 129.08,  35.18},
        {"Rotterdam",  "NewYork",      4.48,  51.92, -74.01,  40.71},
        {"Santos",     "CapeTown",   -46.31, -23.96,  18.42, -33.92},
        {"CapeTown",   "Mumbai",      18.42, -33.92,  72.88,  19.08},
        {"Yokohama",   "LongBeach",  139.64,  35.44,-118.19,  33.77},
    };

    int numRoutes = sizeof(routes) / sizeof(routes[0]);

    for (int r = 0; r < numRoutes; ++r)
    {
        const auto &rt = routes[r];
        QString routeName = QString("%1 -> %2").arg(rt.from, rt.to);
        double hav = haversineKm(rt.lonA, rt.latA, rt.lonB, rt.latB);

        auto start = makePoint(rt.lonA, rt.latA, rt.from);
        auto goal  = makePoint(rt.lonB, rt.latB, rt.to);

        out << "\n========================================\n";
        out << QString("Route %1/%2: %3  (haversine: %4 km)\n")
               .arg(r + 1).arg(numRoutes).arg(routeName)
               .arg(hav, 0, 'f', 0);
        out << "========================================\n";
        out.flush();

        // --- 1. Hierarchical A* ---
        out << "  [Hierarchical A*] running...\n";
        out.flush();
        QElapsedTimer timer;
        timer.start();
        auto hierResult = hvg->findShortestPath(start, goal);
        double hierSec = timer.elapsed() / 1000.0;
        double hierKm = hierResult.isValid() ? pathLengthKm(hierResult) : -1;
        out << QString("  [Hierarchical A*] %1 s | %2 km | %3 pts | %4\n")
               .arg(hierSec, 8, 'f', 1)
               .arg(hierKm, 8, 'f', 0)
               .arg(hierResult.isValid() ? hierResult.points.size() : 0, 4)
               .arg(hierResult.isValid() ? "OK" : "FAIL");
        out.flush();

        bool skipFlat = (hierSec > FLAT_SKIP_SEC);

        // --- 2. Flat A* ---
        if (skipFlat)
        {
            out << QString("  [Flat A*]          SKIPPED (hier > %1s)\n")
                   .arg(FLAT_SKIP_SEC, 0, 'f', 0);
        }
        else
        {
            out << "  [Flat A*]          running...\n";
            out.flush();
            timer.restart();
            auto flatAResult = hvg->findShortestPathFlatAStar(start, goal);
            double flatASec = timer.elapsed() / 1000.0;
            double flatAKm = flatAResult.isValid()
                ? pathLengthKm(flatAResult) : -1;
            out << QString("  [Flat A*]          %1 s | %2 km | %3 pts | %4\n")
                   .arg(flatASec, 8, 'f', 1)
                   .arg(flatAKm, 8, 'f', 0)
                   .arg(flatAResult.isValid() ? flatAResult.points.size() : 0, 4)
                   .arg(flatAResult.isValid() ? "OK" : "FAIL");
            out.flush();
        }

        // --- 3. Flat Dijkstra ---
        if (skipFlat)
        {
            out << QString("  [Flat Dijkstra]    SKIPPED (hier > %1s)\n")
                   .arg(FLAT_SKIP_SEC, 0, 'f', 0);
        }
        else
        {
            out << "  [Flat Dijkstra]    running...\n";
            out.flush();
            timer.restart();
            auto flatDResult = hvg->findShortestPathFlat(start, goal);
            double flatDSec = timer.elapsed() / 1000.0;
            double flatDKm = flatDResult.isValid()
                ? pathLengthKm(flatDResult) : -1;
            out << QString("  [Flat Dijkstra]    %1 s | %2 km | %3 pts | %4\n")
                   .arg(flatDSec, 8, 'f', 1)
                   .arg(flatDKm, 8, 'f', 0)
                   .arg(flatDResult.isValid() ? flatDResult.points.size() : 0, 4)
                   .arg(flatDResult.isValid() ? "OK" : "FAIL");
            out.flush();
        }

        out.flush();
    }

    out << "\n========================================\n";
    out << "DONE\n";
    out.flush();

    return 0;
}
