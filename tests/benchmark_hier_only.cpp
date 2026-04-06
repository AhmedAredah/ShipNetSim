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
    return std::make_shared<GPoint>(units::angle::degree_t(lon), units::angle::degree_t(lat), id);
}
static double pathLengthKm(const ShortestPathResult &r) {
    double m = 0; for (auto &l : r.lines) m += l->length().value(); return m/1000.0;
}
static double havKm(double lo1,double la1,double lo2,double la2){
    double dLa=(la2-la1)*M_PI/180, dLo=(lo2-lo1)*M_PI/180;
    if(dLo>M_PI)dLo-=2*M_PI; if(dLo<-M_PI)dLo+=2*M_PI;
    double a=sin(dLa/2)*sin(dLa/2)+cos(la1*M_PI/180)*cos(la2*M_PI/180)*sin(dLo/2)*sin(dLo/2);
    return 6371.0*2*asin(sqrt(a));
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

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    QString tmpDir = QDir::tempPath()+"/shipnetsim_nocache";
    QString tmpShp = tmpDir+"/ne_10m_ocean.shp";
    if (!QFile::exists(tmpShp)) {
        fprintf(stderr,"No-cache shapefile not found: %s\n", tmpShp.toUtf8().constData());
        return 1;
    }
    // Verify no cache
    QFile::remove(tmpDir+"/ne_10m_ocean.hvg_adj");

    QTextStream out(stdout);
    out << "=== HIERARCHICAL-ONLY NO-CACHE BENCHMARK ===\n";
    out.flush();

    QElapsedTimer lt; lt.start();
    auto net = std::make_shared<OptimizedNetwork>(tmpShp, "Ocean");
    out << "Loaded in " << lt.elapsed()/1000.0 << " s\n\n"; out.flush();

    auto hvg = net->getVisibilityGraph();
    if (!hvg || hvg->getPolygons().isEmpty()) { fprintf(stderr,"No polygons\n"); return 1; }

    char buf[256];
    snprintf(buf,sizeof(buf),"%-28s %8s %10s %8s %5s\n","Route","d_h(km)","t(s)","L(km)","Pts");
    out << buf;
    snprintf(buf,sizeof(buf),"%-28s %8s %10s %8s %5s\n","----------------------------","--------","----------","--------","-----");
    out << buf; out.flush();

    for (int i=0; i<12; ++i) {
        auto &r=R[i];
        QString name=QString("%1 -> %2").arg(r.from,r.to);
        double h=havKm(r.lA,r.aA,r.lB,r.aB);
        auto s=makePoint(r.lA,r.aA,r.from), g=makePoint(r.lB,r.aB,r.to);

        out << "Running " << name << "...\n"; out.flush();
        QElapsedTimer t; t.start();
        auto res=hvg->findShortestPath(s,g);
        double sec=t.elapsed()/1000.0;
        double km=res.isValid()?pathLengthKm(res):-1;
        int pts=res.isValid()?res.points.size():0;

        snprintf(buf,sizeof(buf),"%-28s %8.0f %10.1f %8.0f %5d\n",
                 name.toUtf8().constData(), h, sec, km, pts);
        out << buf; out.flush();
    }
    out << "\nDONE\n"; out.flush();
    return 0;
}
