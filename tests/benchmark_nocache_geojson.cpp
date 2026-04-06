/**
 * @file benchmark_nocache_geojson.cpp
 * @brief Run all 12 routes no-cache, export GeoJSON for visual validation.
 *        Only runs hierarchical — exports each path immediately.
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

static auto makePoint(double lon, double lat, const QString &id) {
    return std::make_shared<GPoint>(units::angle::degree_t(lon),
                                    units::angle::degree_t(lat), id);
}
static double pathLenKm(const ShortestPathResult &r) {
    double m = 0; for (auto &l : r.lines) m += l->length().value(); return m/1000.0;
}

static void exportGeoJSON(const ShortestPathResult &res,
                           const QString &name, double km, double sec,
                           const QString &dir)
{
    if (!res.isValid() || res.points.isEmpty()) return;
    QString safe = name; safe.replace(" ","").replace("->","_");
    QFile f(dir + "/nocache_" + safe + ".geojson");
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    QTextStream o(&f);
    o << "{\"type\":\"FeatureCollection\",\"features\":[{\"type\":\"Feature\","
      << "\"properties\":{\"route\":\"" << name << "\","
      << "\"distance_km\":" << km << ",\"time_s\":" << sec << ","
      << "\"points\":" << res.points.size() << "},"
      << "\"geometry\":{\"type\":\"LineString\",\"coordinates\":[\n";
    for (int i = 0; i < res.points.size(); ++i) {
        o << "[" << res.points[i]->getLongitude().value()
          << "," << res.points[i]->getLatitude().value() << "]";
        if (i + 1 < res.points.size()) o << ",";
        o << "\n";
    }
    o << "]}}]}\n";
}

struct Route { const char *from,*to; double lA,aA,lB,aB; };
static const Route R[]={
    {"Antwerpen","Zeebrugge",4.40,51.22,3.20,51.33},
    {"Santos","Paranagua",-46.31,-23.96,-48.51,-25.50},
    {"NewYork","Boston",-74.01,40.71,-71.06,42.36},
    {"Brisbane","Newcastle",153.03,-27.47,151.78,-32.93},
    {"Rotterdam","Algeciras",4.48,51.92,-5.45,36.14},
    {"NewYork","Colon",-74.01,40.71,-79.91,9.35},
    {"Colombo","Jeddah",79.85,6.93,39.17,21.49},
    {"Singapore","Busan",103.82,1.35,129.08,35.18},
    {"Rotterdam","NewYork",4.48,51.92,-74.01,40.71},
    {"Santos","CapeTown",-46.31,-23.96,18.42,-33.92},
    {"CapeTown","Mumbai",18.42,-33.92,72.88,19.08},
    {"Yokohama","LongBeach",139.64,35.44,-118.19,33.77},
};

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // Prepare no-cache shapefile
    QString src = QCoreApplication::applicationDirPath() + "/data/ne_10m_ocean.shp";
    if (!QFile::exists(src))
        src = "/home/ahmed/Documents/dev/ShipNetSim/src/data/ne_10m_ocean.shp";
    QString tmpDir = QDir::tempPath() + "/shipnetsim_nocache";
    QDir().mkpath(tmpDir);
    for (auto ext : {"shp","shx","dbf","prj","cpg"}) {
        QString s = QFileInfo(src).absolutePath() + "/" +
                    QFileInfo(src).completeBaseName() + "." + ext;
        QString d = tmpDir + "/" + QFileInfo(src).completeBaseName() + "." + ext;
        QFile::remove(d); QFile::copy(s, d);
    }
    QFile::remove(tmpDir + "/ne_10m_ocean.hvg_adj");

    QString outDir = "/home/ahmed/Documents/dev/ShipNetSim/paper/geojson";
    QDir().mkpath(outDir);

    fprintf(stderr, "Loading (no cache)...\n");
    QElapsedTimer lt; lt.start();
    auto net = std::make_shared<OptimizedNetwork>(
        tmpDir + "/ne_10m_ocean.shp", "Ocean");
    fprintf(stderr, "Loaded in %.1fs\n\n", lt.elapsed()/1000.0);
    auto hvg = net->getVisibilityGraph();
    if (!hvg || hvg->getPolygons().isEmpty()) return 1;

    // Also verify land crossings
    auto countLandCross = [&](const ShortestPathResult &r) -> int {
        int bad = 0;
        for (auto &line : r.lines) {
            if (line->length().value() < 2000.0) continue;
            if (!hvg->isSegmentVisible(line)) bad++;
        }
        return bad;
    };

    for (int i = 0; i < 12; ++i) {
        auto &r = R[i];
        QString name = QString("%1 -> %2").arg(r.from, r.to);
        auto s = makePoint(r.lA, r.aA, r.from);
        auto g = makePoint(r.lB, r.aB, r.to);

        fprintf(stderr, "Route %d/12: %s ...\n", i+1, name.toUtf8().constData());
        QElapsedTimer t; t.start();
        auto res = hvg->findShortestPath(s, g);
        double sec = t.elapsed()/1000.0;

        if (res.isValid()) {
            double km = pathLenKm(res);
            int land = countLandCross(res);
            fprintf(stderr, "  %.1fs | %.0fkm | %lld pts | land:%d\n",
                    sec, km, (long long)res.points.size(), land);
            exportGeoJSON(res, name, km, sec, outDir);
        } else {
            fprintf(stderr, "  FAILED after %.1fs\n", sec);
        }
    }
    fprintf(stderr, "\nGeoJSON exported to: %s\n", outDir.toUtf8().constData());
    return 0;
}
