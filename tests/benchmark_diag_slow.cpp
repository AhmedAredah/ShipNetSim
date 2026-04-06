/**
 * @file benchmark_diag_slow.cpp
 * @brief Diagnose the two slowest no-cache routes: NY→Boston and Rotterdam→Algeciras.
 *        Prints corridor sizes and phased adjacency timing per level.
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

static std::shared_ptr<GPoint> makePoint(double lon, double lat, const QString &id) {
    return std::make_shared<GPoint>(units::angle::degree_t(lon),
                                    units::angle::degree_t(lat), id);
}
static double pathLengthKm(const ShortestPathResult &r) {
    double m = 0; for (auto &l : r.lines) m += l->length().value(); return m/1000.0;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QString tmpShp = QDir::tempPath() + "/shipnetsim_nocache/ne_10m_ocean.shp";
    if (!QFile::exists(tmpShp)) {
        // Copy from source
        QString src = QCoreApplication::applicationDirPath() + "/data/ne_10m_ocean.shp";
        if (!QFile::exists(src))
            src = "/home/ahmed/Documents/dev/ShipNetSim/src/data/ne_10m_ocean.shp";
        QString tmpDir = QDir::tempPath() + "/shipnetsim_nocache";
        QDir().mkpath(tmpDir);
        for (auto ext : {"shp","shx","dbf","prj","cpg"}) {
            QString s = QFileInfo(src).absolutePath() + "/" + QFileInfo(src).completeBaseName() + "." + ext;
            QString d = tmpDir + "/" + QFileInfo(src).completeBaseName() + "." + ext;
            QFile::remove(d); QFile::copy(s, d);
        }
        QFile::remove(tmpDir + "/ne_10m_ocean.hvg_adj");
    }

    fprintf(stderr, "Loading...\n");
    QElapsedTimer lt; lt.start();
    auto net = std::make_shared<OptimizedNetwork>(tmpShp, "Ocean");
    fprintf(stderr, "Loaded in %.1fs\n\n", lt.elapsed()/1000.0);
    auto hvg = net->getVisibilityGraph();
    if (!hvg || hvg->getPolygons().isEmpty()) { fprintf(stderr, "No polygons\n"); return 1; }

    // Print hierarchy info
    auto diag = hvg->getDiagnostics();
    for (int i = 0; i < 4; i++)
        fprintf(stderr, "L%d: %d verts, %lld edges\n",
                i, diag.levels[i].vertexCount, diag.levels[i].edgeCount);
    fprintf(stderr, "\n");

    struct Route { const char *name; double lA, aA, lB, aB; };
    Route routes[] = {
        {"Antwerpen->Zeebrugge", 4.40, 51.22, 3.20, 51.33},
        {"Rotterdam->Algeciras", 4.48, 51.92, -5.45, 36.14},
        {"NewYork->Colon", -74.01, 40.71, -79.91, 9.35},
    };

    for (auto &r : routes) {
        auto s = makePoint(r.lA, r.aA, "start");
        auto g = makePoint(r.lB, r.aB, "goal");
        fprintf(stderr, "=== %s ===\n", r.name);
        QElapsedTimer t; t.start();
        auto res = hvg->findShortestPath(s, g);
        double sec = t.elapsed()/1000.0;
        if (res.isValid())
            fprintf(stderr, "DONE: %.1fs, %.0fkm, %d pts\n\n",
                    sec, pathLengthKm(res), res.points.size());
        else
            fprintf(stderr, "FAILED after %.1fs\n\n", sec);
    }
    return 0;
}
