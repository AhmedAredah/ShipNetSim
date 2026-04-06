/**
 * @file benchmark_diagnose.cpp
 * @brief Diagnostic: instrument the no-cache hierarchical path to find bottlenecks.
 *
 * Runs ONE route (Antwerpen→Zeebrugge, 84km) and prints per-level timing,
 * corridor sizes, number of A* expansions, visibility checks per expansion,
 * and per-step breakdown.
 */

#include "network/gpoint.h"
#include "network/hierarchicalvisibilitygraph.h"
#include "network/optimizednetwork.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTextStream>
#include <cmath>
#include <cstdio>

using namespace ShipNetSimCore;

static std::shared_ptr<GPoint> makePoint(double lon, double lat,
                                          const QString &id) {
    return std::make_shared<GPoint>(units::angle::degree_t(lon),
                                    units::angle::degree_t(lat), id);
}

static double pathLengthKm(const ShortestPathResult &r) {
    double m = 0;
    for (auto &l : r.lines) m += l->length().value();
    return m / 1000.0;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // Set up no-cache shapefile
    QString srcPath = QCoreApplication::applicationDirPath() +
                      "/data/ne_10m_ocean.shp";
    if (!QFile::exists(srcPath))
        srcPath = "/home/ahmed/Documents/dev/ShipNetSim/src/data/"
                  "ne_10m_ocean.shp";

    QString tmpDir = QDir::tempPath() + "/shipnetsim_nocache";
    QString tmpShp = tmpDir + "/ne_10m_ocean.shp";

    // Copy shapefile without cache
    QDir().mkpath(tmpDir);
    for (auto ext : {"shp","shx","dbf","prj","cpg"}) {
        QString src = QFileInfo(srcPath).absolutePath() + "/" +
                      QFileInfo(srcPath).completeBaseName() + "." + ext;
        QString dst = tmpDir + "/" +
                      QFileInfo(srcPath).completeBaseName() + "." + ext;
        QFile::remove(dst);
        QFile::copy(src, dst);
    }
    QFile::remove(tmpDir + "/ne_10m_ocean.hvg_adj");

    QTextStream out(stdout);
    out << "=== DIAGNOSTIC: NO-CACHE BOTTLENECK ANALYSIS ===\n\n";
    out.flush();

    // Load
    QElapsedTimer loadTimer;
    loadTimer.start();
    auto net = std::make_shared<OptimizedNetwork>(tmpShp, "Ocean");
    double loadSec = loadTimer.elapsed() / 1000.0;

    auto hvg = net->getVisibilityGraph();
    if (!hvg || hvg->getPolygons().isEmpty()) {
        out << "ERROR: no polygons\n";
        return 1;
    }

    auto diag = hvg->getDiagnostics();
    out << "Loaded in " << loadSec << " s\n";
    out << "L0: " << diag.levels[0].vertexCount << " verts, "
        << diag.levels[0].edgeCount << " edges\n";
    out << "L0 adj ready: " << (diag.level0AdjReady ? "YES" : "NO") << "\n";
    out << "Coarse adj ready: " << (diag.coarseAdjReady ? "YES" : "NO")
        << "\n\n";

    // ===============================================================
    // Diagnose corridor sizes at each level
    // ===============================================================
    out << "=== CORRIDOR SIZE ANALYSIS ===\n";
    out << "(How many vertices does buildCorridor admit at each level?)\n\n";

    struct RouteInfo {
        const char *name;
        double lonA, latA, lonB, latB;
    };

    RouteInfo routes[] = {
        {"Antwerpen->Zeebrugge", 4.40, 51.22, 3.20, 51.33},
        {"Santos->Paranagua", -46.31, -23.96, -48.51, -25.50},
        {"Brisbane->Newcastle", 153.03, -27.47, 151.78, -32.93},
    };

    for (auto &ri : routes) {
        out << "\n--- Route: " << ri.name << " ---\n";
        out.flush();

        auto start = makePoint(ri.lonA, ri.latA, "start");
        auto goal  = makePoint(ri.lonB, ri.latB, "goal");

        QElapsedTimer timer;
        timer.start();
        auto result = hvg->findShortestPath(start, goal);
        double sec = timer.elapsed() / 1000.0;

        if (result.isValid()) {
            double km = pathLengthKm(result);
            out << "  Result: " << km << " km, " << result.points.size()
                << " pts, " << sec << " s\n";
        } else {
            out << "  FAILED after " << sec << " s\n";
        }
        out.flush();
    }

    // ===============================================================
    // Detailed: measure isVisible cost at L0
    // ===============================================================
    out << "\n\n=== VISIBILITY CHECK COST ANALYSIS ===\n";
    out << "(How expensive is a single isVisible call at L0?)\n\n";

    // Pick two nearby coastal vertices from L0
    const auto &lvl0 = hvg->getLevel(0);
    int testPairs = 0;
    double totalVisMs = 0;
    int visTrue = 0, visFalse = 0;

    // Sample 100 vertex pairs at different distances
    QElapsedTimer visTimer;
    for (int i = 0; i < 1000 && testPairs < 100; i += 10) {
        if (i >= lvl0.vertices.size()) break;
        for (int j = i + 1; j < i + 20 && j < lvl0.vertices.size(); ++j) {
            visTimer.start();
            bool vis = hvg->isVisible(lvl0.vertices[i], lvl0.vertices[j], 0);
            double ms = visTimer.elapsed();
            totalVisMs += ms;
            testPairs++;
            if (vis) visTrue++; else visFalse++;
            if (testPairs >= 100) break;
        }
    }

    double avgVisMs = testPairs > 0 ? totalVisMs / testPairs : 0;
    out << "  Tested " << testPairs << " pairs\n";
    out << "  Avg isVisible time: " << avgVisMs << " ms\n";
    out << "  Visible: " << visTrue << ", Not visible: " << visFalse << "\n";
    out << "  At 500 checks/expansion: "
        << (500 * avgVisMs) << " ms/expansion\n";
    out << "  At 50 expansions: "
        << (50 * 500 * avgVisMs / 1000.0) << " s total\n\n";

    // ===============================================================
    // Detailed: measure candidate building cost
    // ===============================================================
    out << "=== CANDIDATE BUILDING COST ===\n";
    out << "(How expensive is iterating corridor vertex indices?)\n\n";

    // Simulate what getVisibleNodesForPoint does in lazy mode
    // Build a sample corridor with known size
    for (int corridorSize : {1000, 5000, 10000, 20000, 50000}) {
        // Create a dummy corridor with corridorSize vertices
        std::unordered_set<int> allowedIndices;
        for (int k = 0; k < corridorSize && k < lvl0.vertices.size(); ++k) {
            allowedIndices.insert(k);
        }

        auto testNode = lvl0.vertices[0];

        visTimer.start();
        // This is what getVisibleNodesForPoint does:
        QVector<std::shared_ptr<GPoint>> candidates;
        candidates.reserve(static_cast<int>(allowedIndices.size()));
        for (int idx : allowedIndices) {
            if (idx < lvl0.vertices.size())
                candidates.append(lvl0.vertices[idx]);
        }
        double buildMs = visTimer.elapsed();

        // partial_sort
        visTimer.start();
        int sortLimit = std::min(500, static_cast<int>(candidates.size()));
        std::partial_sort(
            candidates.begin(),
            candidates.begin() + sortLimit,
            candidates.end(),
            [&testNode](const auto &a, const auto &b) {
                return testNode->fastDistance(*a) <
                       testNode->fastDistance(*b);
            });
        double sortMs = visTimer.elapsed();

        char buf[256];
        snprintf(buf, sizeof(buf),
                 "  Corridor %6d verts: build=%.1fms sort=%.1fms "
                 "total=%.1fms\n",
                 corridorSize, buildMs, sortMs, buildMs + sortMs);
        out << buf;
    }

    out << "\n  Per-expansion cost breakdown (corridor=10000):\n";
    out << "    1. Iterate corridor indices & copy shared_ptrs\n";
    out << "    2. partial_sort 500 nearest\n";
    out << "    3. isVisible × up to 500\n";
    out << "    Total per expansion: iterate + sort + 500×isVisible\n\n";

    // ===============================================================
    // Quantify: how many expansions does L0 A* do?
    // ===============================================================
    out << "=== EXPANSION COUNT ESTIMATE ===\n";
    out << "(From qDebug output in searchAtLevel)\n";
    out << "See stderr for A* expansion counts per route.\n\n";

    out << "DONE\n";
    out.flush();
    return 0;
}
