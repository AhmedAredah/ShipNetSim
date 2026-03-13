#include "network/gsegment.h"
#include "network/gpoint.h"
#include "network/gline.h"
#include <QDebug>
#include <QTest>

using namespace ShipNetSimCore;

class GSegmentTest : public QObject
{
    Q_OBJECT

private slots:
    // Construction tests
    void testDefaultConstructor();
    void testRawCoordConstructor();
    void testGPointConstructor();
    void testGLineConstructor();

    // Intersection tests
    void testCrossingSegments();
    void testNonCrossingSegments();
    void testParallelSegments();
    void testCollinearOverlapping();
    void testCollinearNonOverlapping();
    void testSharedEndpoint_Ignored();
    void testSharedEndpoint_NotIgnored();
    void testTTouchingEndpoint();

    // AABB tests
    void testAABBContainedEndpoint();
    void testAABBCrossing();
    void testAABBMiss();
    void testAABBParallelInside();

    // Bounding box
    void testBoundingBox();

    // Length
    void testApproxLength();
    void testApproxLengthAntimeridian();

    // Orientation
    void testOrientationCCW();
    void testOrientationCW();
    void testOrientationCollinear();

    // Endpoint utilities
    void testIsEndpoint();
    void testSharesEndpoint();
    void testCrossesAntimeridian();

    // Consistency with GLine::intersects
    void testConsistencyWithGLine();
};

void GSegmentTest::testDefaultConstructor()
{
    GSegment seg;
    QCOMPARE(seg.startLon(), 0.0);
    QCOMPARE(seg.startLat(), 0.0);
    QCOMPARE(seg.endLon(), 0.0);
    QCOMPARE(seg.endLat(), 0.0);
}

void GSegmentTest::testRawCoordConstructor()
{
    GSegment seg(1.0, 2.0, 3.0, 4.0);
    QCOMPARE(seg.startLon(), 1.0);
    QCOMPARE(seg.startLat(), 2.0);
    QCOMPARE(seg.endLon(), 3.0);
    QCOMPARE(seg.endLat(), 4.0);
}

void GSegmentTest::testGPointConstructor()
{
    auto p1 = std::make_shared<GPoint>(
        units::angle::degree_t(10.0), units::angle::degree_t(20.0));
    auto p2 = std::make_shared<GPoint>(
        units::angle::degree_t(30.0), units::angle::degree_t(40.0));

    GSegment seg(p1, p2);
    QCOMPARE(seg.startLon(), 10.0);
    QCOMPARE(seg.startLat(), 20.0);
    QCOMPARE(seg.endLon(), 30.0);
    QCOMPARE(seg.endLat(), 40.0);
}

void GSegmentTest::testGLineConstructor()
{
    auto p1 = std::make_shared<GPoint>(
        units::angle::degree_t(10.0), units::angle::degree_t(20.0));
    auto p2 = std::make_shared<GPoint>(
        units::angle::degree_t(30.0), units::angle::degree_t(40.0));
    auto line = std::make_shared<GLine>(p1, p2);

    GSegment seg(line);
    QCOMPARE(seg.startLon(), 10.0);
    QCOMPARE(seg.startLat(), 20.0);
    QCOMPARE(seg.endLon(), 30.0);
    QCOMPARE(seg.endLat(), 40.0);
}

void GSegmentTest::testCrossingSegments()
{
    // X-shaped crossing
    GSegment a(0.0, 0.0, 2.0, 2.0);
    GSegment b(0.0, 2.0, 2.0, 0.0);
    QVERIFY(a.intersects(b, false));
    QVERIFY(a.intersects(b, true));
}

void GSegmentTest::testNonCrossingSegments()
{
    GSegment a(0.0, 0.0, 1.0, 0.0);
    GSegment b(0.0, 1.0, 1.0, 1.0);
    QVERIFY(!a.intersects(b, false));
    QVERIFY(!a.intersects(b, true));
}

void GSegmentTest::testParallelSegments()
{
    GSegment a(0.0, 0.0, 2.0, 0.0);
    GSegment b(0.0, 0.1, 2.0, 0.1);
    QVERIFY(!a.intersects(b, false));
}

void GSegmentTest::testCollinearOverlapping()
{
    GSegment a(0.0, 0.0, 2.0, 0.0);
    GSegment b(1.0, 0.0, 3.0, 0.0);
    QVERIFY(a.intersects(b, false));
}

void GSegmentTest::testCollinearNonOverlapping()
{
    GSegment a(0.0, 0.0, 1.0, 0.0);
    GSegment b(2.0, 0.0, 3.0, 0.0);
    QVERIFY(!a.intersects(b, false));
}

void GSegmentTest::testSharedEndpoint_Ignored()
{
    // Segments meeting at (1,1)
    GSegment a(0.0, 0.0, 1.0, 1.0);
    GSegment b(1.0, 1.0, 2.0, 0.0);
    QVERIFY(!a.intersects(b, true));  // ignoreEndpoints=true
}

void GSegmentTest::testSharedEndpoint_NotIgnored()
{
    GSegment a(0.0, 0.0, 1.0, 1.0);
    GSegment b(1.0, 1.0, 2.0, 0.0);
    QVERIFY(a.intersects(b, false));  // ignoreEndpoints=false
}

void GSegmentTest::testTTouchingEndpoint()
{
    // T-junction: segment b touches segment a at midpoint
    GSegment a(0.0, 0.0, 2.0, 0.0);
    GSegment b(1.0, -1.0, 1.0, 0.0);
    QVERIFY(a.intersects(b, false));
}

void GSegmentTest::testAABBContainedEndpoint()
{
    GSegment seg(0.5, 0.5, 5.0, 5.0);
    QVERIFY(seg.intersectsAABB(0.0, 0.0, 1.0, 1.0));
}

void GSegmentTest::testAABBCrossing()
{
    // Segment crosses box from left to right
    GSegment seg(-1.0, 0.5, 2.0, 0.5);
    QVERIFY(seg.intersectsAABB(0.0, 0.0, 1.0, 1.0));
}

void GSegmentTest::testAABBMiss()
{
    GSegment seg(5.0, 5.0, 6.0, 6.0);
    QVERIFY(!seg.intersectsAABB(0.0, 0.0, 1.0, 1.0));
}

void GSegmentTest::testAABBParallelInside()
{
    // Segment entirely inside the box
    GSegment seg(0.2, 0.5, 0.8, 0.5);
    QVERIFY(seg.intersectsAABB(0.0, 0.0, 1.0, 1.0));
}

void GSegmentTest::testBoundingBox()
{
    GSegment seg(3.0, 1.0, 1.0, 4.0);
    double minLon, minLat, maxLon, maxLat;
    seg.boundingBox(minLon, minLat, maxLon, maxLat);
    QCOMPARE(minLon, 1.0);
    QCOMPARE(maxLon, 3.0);
    QCOMPARE(minLat, 1.0);
    QCOMPARE(maxLat, 4.0);
}

void GSegmentTest::testApproxLength()
{
    // ~111km per degree at equator
    GSegment seg(0.0, 0.0, 1.0, 0.0);
    double len = seg.approxLengthMeters();
    QVERIFY(len > 110000.0 && len < 112000.0);
}

void GSegmentTest::testApproxLengthAntimeridian()
{
    // Segment crossing antimeridian: 179 to -179 should be ~2 degrees
    GSegment seg(179.0, 0.0, -179.0, 0.0);
    double len = seg.approxLengthMeters();
    QVERIFY(len > 220000.0 && len < 224000.0);
}

void GSegmentTest::testOrientationCCW()
{
    auto o = GSegment::orientation(0, 0, 1, 0, 0.5, 1);
    QCOMPARE(o, GSegment::CounterClockwise);
}

void GSegmentTest::testOrientationCW()
{
    auto o = GSegment::orientation(0, 0, 1, 0, 0.5, -1);
    QCOMPARE(o, GSegment::Clockwise);
}

void GSegmentTest::testOrientationCollinear()
{
    auto o = GSegment::orientation(0, 0, 1, 0, 2, 0);
    QCOMPARE(o, GSegment::Collinear);
}

void GSegmentTest::testIsEndpoint()
{
    GSegment seg(1.0, 2.0, 3.0, 4.0);
    QVERIFY(seg.isEndpoint(1.0, 2.0));
    QVERIFY(seg.isEndpoint(3.0, 4.0));
    QVERIFY(!seg.isEndpoint(2.0, 3.0));
}

void GSegmentTest::testSharesEndpoint()
{
    GSegment a(0.0, 0.0, 1.0, 1.0);
    GSegment b(1.0, 1.0, 2.0, 0.0);
    GSegment c(5.0, 5.0, 6.0, 6.0);
    QVERIFY(a.sharesEndpoint(b));
    QVERIFY(!a.sharesEndpoint(c));
}

void GSegmentTest::testCrossesAntimeridian()
{
    GSegment normal(10.0, 0.0, 20.0, 0.0);
    QVERIFY(!normal.crossesAntimeridian());

    GSegment crossing(170.0, 0.0, -170.0, 0.0);
    QVERIFY(crossing.crossesAntimeridian());
}

void GSegmentTest::testConsistencyWithGLine()
{
    // Verify that GSegment::intersects gives the same result as GLine::intersects
    auto p1 = std::make_shared<GPoint>(
        units::angle::degree_t(0.0), units::angle::degree_t(0.0));
    auto p2 = std::make_shared<GPoint>(
        units::angle::degree_t(2.0), units::angle::degree_t(2.0));
    auto p3 = std::make_shared<GPoint>(
        units::angle::degree_t(0.0), units::angle::degree_t(2.0));
    auto p4 = std::make_shared<GPoint>(
        units::angle::degree_t(2.0), units::angle::degree_t(0.0));

    GLine lineA(p1, p2);
    GLine lineB(p3, p4);
    GSegment segA(lineA);
    GSegment segB(lineB);

    // Both should agree on crossing intersection
    bool glineResult = lineA.intersects(lineB, true);
    bool gsegResult = segA.intersects(segB, true);
    QCOMPARE(gsegResult, glineResult);

    // Non-intersecting case
    auto p5 = std::make_shared<GPoint>(
        units::angle::degree_t(10.0), units::angle::degree_t(10.0));
    auto p6 = std::make_shared<GPoint>(
        units::angle::degree_t(11.0), units::angle::degree_t(11.0));
    GLine lineC(p5, p6);
    GSegment segC(lineC);

    glineResult = lineA.intersects(lineC, true);
    gsegResult = segA.intersects(segC, true);
    QCOMPARE(gsegResult, glineResult);
}

QTEST_MAIN(GSegmentTest)
#include "test_gsegment.moc"
