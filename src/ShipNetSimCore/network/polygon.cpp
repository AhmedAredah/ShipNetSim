/**
 * @file polygon.cpp
 * @brief Implementation of the Polygon class.
 *
 * This file contains the implementation for the Polygon class, providing
 * geodetic polygon operations including area/perimeter calculations,
 * point-in-polygon testing, line intersection detection, and boundary
 * transformations. All geodetic operations use the WGS84 ellipsoid
 * exclusively, which is the international standard for maritime navigation.
 *
 * @note This implementation enforces WGS84 for all geodetic operations.
 *
 * @author Ahmed Aredah
 * @date 10.12.2023
 */

#include "polygon.h"
#include "gsegment.h"
#include "../utils/gdal_compat.h"
#include "../utils/utils.h"
#include "qdebug.h"
#include <GeographicLib/Geodesic.hpp>
#include <GeographicLib/PolygonArea.hpp>
#include <QVector>
#include <cmath>
#include <limits>
#include <sstream>
#include <ogrsf_frmts.h>

namespace ShipNetSimCore
{

// =============================================================================
// Anonymous Namespace - Internal Helper Functions
// =============================================================================

namespace
{

/**
 * @brief Get the WGS84 Geodesic calculator (cached static instance).
 *
 * WGS84 is the international standard for maritime navigation.
 * Using the cached static instance avoids object construction overhead.
 *
 * @return Reference to the cached WGS84 Geodesic calculator
 */
inline const GeographicLib::Geodesic& wgs84Geodesic()
{
    return GeographicLib::Geodesic::WGS84();
}

/**
 * @brief Create a GPoint from OGR ring coordinates at given index.
 *
 * @param ring The OGR ring to extract coordinates from
 * @param index Point index in the ring
 * @return Shared pointer to new GPoint (using default WGS84)
 */
std::shared_ptr<GPoint> pointFromRing(const OGRLinearRing *ring, int index)
{
    return std::make_shared<GPoint>(units::angle::degree_t(ring->getX(index)),
                                    units::angle::degree_t(ring->getY(index)));
}

/**
 * @brief Check if two line endpoints match within tolerance.
 *
 * @param lon1 First longitude
 * @param lat1 First latitude
 * @param lon2 Second longitude
 * @param lat2 Second latitude
 * @param epsilon Tolerance for comparison
 * @return true if points are within epsilon of each other
 */
bool pointsMatch(double lon1, double lat1, double lon2, double lat2,
                 double epsilon = 1e-9)
{
    return std::abs(lon1 - lon2) < epsilon && std::abs(lat1 - lat2) < epsilon;
}

/**
 * @brief Tolerance for vertex proximity comparisons in meters.
 */
constexpr double VERTEX_TOLERANCE_METERS = 0.1;

/**
 * @brief Number of sample points for segment-through-hole testing.
 */
constexpr int HOLE_SAMPLING_COUNT = 10;

/**
 * @brief Threshold for detecting large longitude jumps (antimeridian crossing).
 */
constexpr double LONGITUDE_JUMP_THRESHOLD = 180.0;

/**
 * @brief Tolerance for full-span polygon detection in degrees.
 */
constexpr double FULL_SPAN_TOLERANCE = 2.0;

}  // anonymous namespace

// =============================================================================
// Constructors
// =============================================================================

Polygon::Polygon() {}

Polygon::Polygon(const QVector<std::shared_ptr<GPoint>>          &boundary,
                 const QVector<QVector<std::shared_ptr<GPoint>>> &holes,
                 const QString                                    ID)
    : mOutterBoundary(boundary)
    , mInnerHoles(holes)
    , mUserID(ID)
{
    setOuterPoints(boundary);
    setInnerHolesPoints(holes);
}

// =============================================================================
// Static Shapefile Loading
// =============================================================================

QVector<std::shared_ptr<Polygon>>
Polygon::loadFromShapefile(const QString& filepath)
{
    QVector<std::shared_ptr<Polygon>> polygons;

    GDALDataset* poDS = static_cast<GDALDataset*>(
        GDALOpenEx(filepath.toStdString().c_str(), GDAL_OF_VECTOR,
                   nullptr, nullptr, nullptr));
    if (!poDS)
    {
        qWarning() << "Polygon::loadFromShapefile: Failed to open"
                   << filepath;
        return polygons;
    }

    OGRLayer* poLayer = poDS->GetLayer(0);
    if (!poLayer)
    {
        qWarning() << "Polygon::loadFromShapefile: No layers found";
        GDALClose(poDS);
        return polygons;
    }

    const OGRSpatialReference* poSRS = poLayer->GetSpatialRef();
    if (poSRS)
    {
        OGRSpatialReference wgs84SRS;
        wgs84SRS.SetWellKnownGeogCS("WGS84");
        if (!poSRS->IsSameGeogCS(&wgs84SRS))
        {
            qWarning() << "Polygon::loadFromShapefile:"
                       << "CRS is not WGS84 — results may be incorrect";
        }
    }

    poLayer->ResetReading();
    int shapeID = 0;

    auto processOGRPolygon = [&](OGRPolygon* poPolygon) {
        OGRLinearRing* poExteriorRing = poPolygon->getExteriorRing();

        QVector<std::shared_ptr<GPoint>> exteriorRing;
        if (poExteriorRing)
        {
            int nPoints = poExteriorRing->getNumPoints();
            for (int i = 0; i < nPoints; i++)
            {
                exteriorRing.push_back(
                    std::make_shared<GPoint>(
                        units::angle::degree_t(poExteriorRing->getX(i)),
                        units::angle::degree_t(poExteriorRing->getY(i))));
            }
            shapeID++;
        }

        int nInteriorRings = poPolygon->getNumInteriorRings();
        QVector<QVector<std::shared_ptr<GPoint>>> innerHoles(nInteriorRings);

        for (int i = 0; i < nInteriorRings; i++)
        {
            OGRLinearRing* poInteriorRing = poPolygon->getInteriorRing(i);
            int nPoints = poInteriorRing->getNumPoints();
            for (int j = 0; j < nPoints; j++)
            {
                innerHoles[i].push_back(
                    std::make_shared<GPoint>(
                        units::angle::degree_t(poInteriorRing->getX(j)),
                        units::angle::degree_t(poInteriorRing->getY(j))));
            }
        }

        polygons.push_back(std::make_shared<Polygon>(
            exteriorRing, innerHoles, QString::number(shapeID)));
    };

    OGRFeature* poFeature;
    while ((poFeature = poLayer->GetNextFeature()) != nullptr)
    {
        OGRGeometry* poGeometry = poFeature->GetGeometryRef();
        if (poGeometry)
        {
            auto geomType = wkbFlatten(poGeometry->getGeometryType());
            if (geomType == wkbPolygon)
            {
                processOGRPolygon(static_cast<OGRPolygon*>(poGeometry));
            }
            else if (geomType == wkbMultiPolygon)
            {
                OGRMultiPolygon* poMulti =
                    static_cast<OGRMultiPolygon*>(poGeometry);
                for (int g = 0; g < poMulti->getNumGeometries(); g++)
                {
                    processOGRPolygon(static_cast<OGRPolygon*>(
                        poMulti->getGeometryRef(g)));
                }
            }
        }
        OGRFeature::DestroyFeature(poFeature);
    }

    GDALClose(poDS);
    return polygons;
}

// =============================================================================
// Static Helper Methods
// =============================================================================

void Polygon::validateRing(const OGRLinearRing &ring, const QString &description)
{
    // Check for sufficient number of points
    if (ring.getNumPoints() < 3)
    {
        throw std::runtime_error(description.toStdString() +
                                 " is degenerate: requires at "
                                 "least 3 unique points.");
    }

    // If only 3 points are provided, check for collinearity
    if (ring.getNumPoints() == 3)
    {
        const OGRSpatialReference *SR = ring.getSpatialReference();

        GPoint p0 = GPoint(units::angle::degree_t(ring.getX(0)),
                           units::angle::degree_t(ring.getY(0)), *SR);
        GPoint p1 = GPoint(units::angle::degree_t(ring.getX(1)),
                           units::angle::degree_t(ring.getY(1)), *SR);
        GPoint p2 = GPoint(units::angle::degree_t(ring.getX(2)),
                           units::angle::degree_t(ring.getY(2)), *SR);

        if (GLine::orientation(std::make_shared<GPoint>(p0),
                               std::make_shared<GPoint>(p1),
                               std::make_shared<GPoint>(p2)))
        {
            throw std::runtime_error(description.toStdString() +
                                     " is degenerate: points are collinear.");
        }
    }
}

// =============================================================================
// Boundary Accessors and Mutators
// =============================================================================

void Polygon::setOuterPoints(const QVector<std::shared_ptr<GPoint>> &newOuter)
{
    mOutterBoundary = newOuter;

    // Create a ring (outer boundary)
    OGRLinearRing outerRing;

    // Temporarily store interior rings
    std::vector<OGRLinearRing *> tempInteriorRings;
    for (int i = 0; i < mPolygon.getNumInteriorRings(); ++i)
    {
        OGRLinearRing *interiorRing = static_cast<OGRLinearRing *>(
            mPolygon.getInteriorRing(i)->clone());
        tempInteriorRings.push_back(interiorRing);
    }

    // Clear the polygon and update the exterior ring
    mPolygon.empty();

    for (auto &p : newOuter)
    {
        auto pp = p->getGDALPoint();
        outerRing.addPoint(&pp);
    }
    outerRing.closeRings();
    outerRing.assignSpatialReference(
        newOuter[0]->getGDALPoint().getSpatialReference());

    mPolygon.addRing(&outerRing);
    mPolygon.assignSpatialReference(
        newOuter[0]->getGDALPoint().getSpatialReference());

    // Restore interior rings
    for (auto *interiorRing : tempInteriorRings)
    {
        mPolygon.addRingDirectly(interiorRing);  // Takes ownership
    }

    validateRing(outerRing, "Outer boundary");

    mHoleIndex.invalidate();
    mOuterRingIndex.invalidate();
}

const QVector<std::shared_ptr<GPoint>>& Polygon::outer() const
{
    return mOutterBoundary;
}

void Polygon::setInnerHolesPoints(
    QVector<QVector<std::shared_ptr<GPoint>>> newInners)
{
    mInnerHoles = newInners;

    // Temporarily store exterior ring
    OGRLinearRing *tempExteriorRing =
        static_cast<OGRLinearRing *>(mPolygon.getExteriorRing()->clone());

    // Clear the polygon and restore exterior ring
    mPolygon.empty();
    mPolygon.addRingDirectly(tempExteriorRing);  // Takes ownership

    // Add new interior rings
    for (const auto &hole : newInners)
    {
        OGRLinearRing holeRing;

        for (auto &p : hole)
        {
            auto pp = p->getGDALPoint();
            holeRing.addPoint(&pp);
        }
        holeRing.closeRings();
        holeRing.assignSpatialReference(
            hole[0]->getGDALPoint().getSpatialReference());

        mPolygon.addRing(&holeRing);
        mPolygon.assignSpatialReference(
            hole[0]->getGDALPoint().getSpatialReference());

        validateRing(holeRing, "Hole");
    }

    mHoleIndex.invalidate();
}

const QVector<QVector<std::shared_ptr<GPoint>>>& Polygon::inners() const
{
    return mInnerHoles;
}

// =============================================================================
// Antimeridian Handling
// =============================================================================

bool Polygon::crossesAntimeridian() const
{
    // Use cached value if available
    if (mCrossesAntimeridianCache >= 0)
    {
        return mCrossesAntimeridianCache == 1;
    }

    auto *ring = mPolygon.getExteriorRing();
    if (!ring || ring->getNumPoints() < 2)
    {
        mCrossesAntimeridianCache = 0;
        return false;
    }

    // Calculate longitude bounds
    double minLon = ring->getX(0);
    double maxLon = ring->getX(0);
    for (int i = 1; i < ring->getNumPoints(); ++i)
    {
        double lon = ring->getX(i);
        minLon     = std::min(minLon, lon);
        maxLon     = std::max(maxLon, lon);
    }

    // CASE 1: Full-span polygons (approximately -180 to +180)
    // These cover the entire globe and do NOT cross the antimeridian
    bool isFullSpan = (minLon <= -180.0 + FULL_SPAN_TOLERANCE) &&
                      (maxLon >= 180.0 - FULL_SPAN_TOLERANCE);
    if (isFullSpan)
    {
        mCrossesAntimeridianCache = 0;
        return false;
    }

    // CASE 2: Detect antimeridian crossing via edge direction analysis
    // Look at the first edge that has a large longitude jump (> 180 degrees)
    // The direction determines if the polygon wraps around the antimeridian
    int numPoints = ring->getNumPoints();
    for (int i = 0; i < numPoints - 1; ++i)
    {
        double lon1    = ring->getX(i);
        double lon2    = ring->getX(i + 1);
        double lonDiff = std::abs(lon2 - lon1);

        if (lonDiff > LONGITUDE_JUMP_THRESHOLD)
        {
            // Positive -> Negative = crosses antimeridian
            // Negative -> Positive = goes around prime meridian
            if (lon1 > 0.0 && lon2 < 0.0)
            {
                mCrossesAntimeridianCache = 1;
                return true;
            }
            else
            {
                mCrossesAntimeridianCache = 0;
                return false;
            }
        }
    }

    mCrossesAntimeridianCache = 0;
    return false;
}

// =============================================================================
// Point Containment Tests
// =============================================================================

void Polygon::ensureHoleIndex() const
{
    if (!mHoleIndex.isBuilt())
        mHoleIndex.build(mPolygon);
}

void Polygon::ensureOuterRingIndex() const
{
    if (!mOuterRingIndex.isBuilt())
        mOuterRingIndex.build(mOutterBoundary);
}

void Polygon::ensureRingPositionMap() const
{
    if (mRingPosBuilt) return;
    mRingPosBuilt = true;

    int numHoles = mPolygon.getNumInteriorRings();
    if (numHoles == 0) return;

    // Pre-size: estimate total hole vertices for hash map reservation
    int totalVerts = 0;
    for (int h = 0; h < numHoles; ++h)
    {
        const OGRLinearRing* ring = mPolygon.getInteriorRing(h);
        if (ring)
            totalVerts += ring->getNumPoints() - 1;  // -1: closing pt
    }
    mRingPositionMap.reserve(totalVerts);

    for (int h = 0; h < numHoles; ++h)
    {
        const OGRLinearRing* ring = mPolygon.getInteriorRing(h);
        if (!ring) continue;

        int ringSize = ring->getNumPoints() - 1;  // unique vertices
        if (ringSize < 2) continue;

        for (int i = 0; i < ringSize; ++i)
        {
            CoordKey key = CoordKey::from(ring->getX(i), ring->getY(i));
            mRingPositionMap[key] = {h, i, ringSize};
        }
    }
}

bool Polygon::areVerticesAdjacentOnHole(
    double lon1, double lat1,
    double lon2, double lat2,
    int holeIndex) const
{
    ensureRingPositionMap();

    auto it1 = mRingPositionMap.find(CoordKey::from(lon1, lat1));
    auto it2 = mRingPositionMap.find(CoordKey::from(lon2, lat2));

    if (it1 == mRingPositionMap.end() || it2 == mRingPositionMap.end())
        return false;  // Not a known ring vertex

    if (it1->second.holeIdx != holeIndex || it2->second.holeIdx != holeIndex)
        return false;  // Not on the expected hole

    int diff = std::abs(it1->second.pos - it2->second.pos);
    int ringSize = it1->second.ringSize;

    // Adjacent: consecutive positions, or wrap-around (first↔last)
    return (diff == 1 || diff == ringSize - 1);
}

const OuterRingSpatialIndex& Polygon::outerRingIndex() const
{
    ensureOuterRingIndex();
    return mOuterRingIndex;
}

bool Polygon::segmentCrossesOuterRing(
    const std::shared_ptr<GPoint> &v1,
    const std::shared_ptr<GPoint> &v2) const
{
    ensureOuterRingIndex();
    return mOuterRingIndex.doesSegmentCross(
        v1->getLongitude().value(), v1->getLatitude().value(),
        v2->getLongitude().value(), v2->getLatitude().value());
}

bool Polygon::isPointWithinExteriorRing(const GPoint &pointToCheck) const
{
    auto           r = mPolygon.getExteriorRing();
    const OGRPoint p = pointToCheck.getGDALPoint();

    // Check boundary first, then interior
    if (r->isPointOnRingBoundary(&p, TRUE))
    {
        return true;
    }
    if (r->isPointInRing(&p, TRUE))
    {
        return true;
    }

    return false;
}

bool Polygon::isPointWithinInteriorRings(const GPoint &pointToCheck) const
{
    return findContainingHoleIndex(pointToCheck) >= 0;
}

int Polygon::findContainingHoleIndex(const GPoint &pointToCheck) const
{
    ensureHoleIndex();

    double lon = pointToCheck.getLongitude().value();
    double lat = pointToCheck.getLatitude().value();

    int           result = -1;
    const OGRPoint ogrPt(lon, lat);

    mHoleIndex.forEachCandidate(lon, lat, [&](int holeIdx) -> bool {
        // Fast ray-casting check first
        if (isPointInHoleByCoords(lon, lat, holeIdx))
        {
            result = holeIdx;
            return true;
        }
        // Fallback: OGR boundary check for points on ring edges/vertices
        auto *ring = mPolygon.getInteriorRing(holeIdx);
        if (ring && ring->isPointOnRingBoundary(&ogrPt, TRUE))
        {
            result = holeIdx;
            return true;
        }
        return false;
    });
    return result;
}

bool Polygon::isPointOnHoleBoundary(const GPoint &pt, int holeIndex) const
{
    if (holeIndex < 0 || holeIndex >= mPolygon.getNumInteriorRings())
        return false;
    const OGRLinearRing *ring = mPolygon.getInteriorRing(holeIndex);
    if (!ring)
        return false;
    const OGRPoint ogrPt = pt.getGDALPoint();
    return ring->isPointOnRingBoundary(&ogrPt, TRUE);
}

bool Polygon::isPointWithinPolygon(const GPoint &pointToCheck) const
{
    // Handle antimeridian-crossing polygons specially
    if (crossesAntimeridian())
    {
        double pointLon     = pointToCheck.getLongitude().value();
        double pointLat     = pointToCheck.getLatitude().value();
        double normPointLon = AngleUtils::normalizeLongitude360(pointLon);

        auto *ring = mPolygon.getExteriorRing();
        if (!ring)
        {
            return false;
        }

        // Create normalized ring for testing
        OGRLinearRing normRing;
        for (int i = 0; i < ring->getNumPoints(); ++i)
        {
            double normLon = AngleUtils::normalizeLongitude360(ring->getX(i));
            normRing.addPoint(normLon, ring->getY(i));
        }

        OGRPoint normPoint(normPointLon, pointLat);

        // Lambda to check if point is in any normalized hole.
        // Uses precomputed bboxes for latitude prefiltering.
        auto isInNormalizedHole = [this, normPointLon, pointLat]() -> bool {
            ensureHoleIndex();

            for (int h = 0; h < mPolygon.getNumInteriorRings(); ++h)
            {
                // Latitude-only bbox prefilter (longitude may be shifted
                // for antimeridian, so only filter by latitude)
                if (mHoleIndex.hasHoles())
                {
                    const auto &bbox = mHoleIndex.holeBBox(h);
                    if (pointLat < bbox.MinY || pointLat > bbox.MaxY)
                        continue;
                }

                auto         *hole = mPolygon.getInteriorRing(h);
                OGRLinearRing normHole;

                for (int i = 0; i < hole->getNumPoints(); ++i)
                {
                    double normLon =
                        AngleUtils::normalizeLongitude360(hole->getX(i));
                    normHole.addPoint(normLon, hole->getY(i));
                }

                OGRPoint np(normPointLon, pointLat);
                if (normHole.isPointOnRingBoundary(&np, TRUE)
                    || normHole.isPointInRing(&np, TRUE))
                {
                    return true;
                }
            }
            return false;
        };

        // Check boundary
        if (normRing.isPointOnRingBoundary(&normPoint, TRUE))
        {
            return !isInNormalizedHole();
        }

        // Check interior
        if (normRing.isPointInRing(&normPoint, TRUE))
        {
            return !isInNormalizedHole();
        }

        return false;
    }

    // Standard case: polygon doesn't cross antimeridian
    if (isPointWithinInteriorRings(pointToCheck))
    {
        return false;
    }

    // Fast ray-casting on cached coordinates (OGR-compatible algorithm)
    ensureOuterRingIndex();
    double lon = pointToCheck.getLongitude().value();
    double lat = pointToCheck.getLatitude().value();
    if (mOuterRingIndex.containsPoint(lon, lat))
        return true;

    // Fallback: OGR boundary check for points on ring edges/vertices
    return isPointWithinExteriorRing(pointToCheck);
}

bool Polygon::ringsContain(std::shared_ptr<GPoint> point) const
{
    auto           r = mPolygon.getExteriorRing();
    const OGRPoint p = point->getGDALPoint();

    // Check exterior ring boundary
    if (r->isPointOnRingBoundary(&p, TRUE))
    {
        return true;
    }

    // Check ALL interior ring boundaries (not in hot path —
    // correctness over performance for boundary membership checks)
    for (int i = 0; i < mPolygon.getNumInteriorRings(); ++i)
    {
        if (mPolygon.getInteriorRing(i)->isPointOnRingBoundary(&p, TRUE))
            return true;
    }
    return false;
}

// =============================================================================
// Geometric Calculations
// =============================================================================

units::area::square_meter_t Polygon::area() const
{
    const auto& geod = wgs84Geodesic();

    // Calculate exterior ring area
    GeographicLib::PolygonArea poly(geod);

    const auto *exteriorRing = mPolygon.getExteriorRing();
    for (int i = 0; i < exteriorRing->getNumPoints(); ++i)
    {
        poly.AddPoint(exteriorRing->getY(i), exteriorRing->getX(i));
    }

    double area, perimeter;
    poly.Compute(false, true, perimeter, area);

    // Subtract hole areas
    const int numInteriorRings = mPolygon.getNumInteriorRings();
    for (int j = 0; j < numInteriorRings; ++j)
    {
        const auto *interiorRing = mPolygon.getInteriorRing(j);
        GeographicLib::PolygonArea holePoly(geod);

        for (int k = 0; k < interiorRing->getNumPoints(); ++k)
        {
            holePoly.AddPoint(interiorRing->getY(k), interiorRing->getX(k));
        }

        double holeArea, holePerimeter;
        holePoly.Compute(false, true, holePerimeter, holeArea);
        area -= holeArea;
    }

    return units::area::square_meter_t(area);
}

units::length::meter_t Polygon::perimeter() const
{
    const auto& geod = wgs84Geodesic();

    GeographicLib::PolygonArea poly(geod);

    const auto *exteriorRing = mPolygon.getExteriorRing();
    for (int i = 0; i < exteriorRing->getNumPoints(); ++i)
    {
        poly.AddPoint(exteriorRing->getY(i), exteriorRing->getX(i));
    }

    double area, perimeter;
    poly.Compute(false, true, perimeter, area);

    return units::length::meter_t(perimeter);
}

units::length::meter_t Polygon::getMaxClearWidth(const GLine &line) const
{
    units::length::meter_t leftClearWidth =
        std::numeric_limits<units::length::meter_t>::max();
    units::length::meter_t rightClearWidth =
        std::numeric_limits<units::length::meter_t>::max();

    auto calculateClearWidths = [&leftClearWidth, &rightClearWidth,
                                 &line](const OGRLinearRing *ring) {
        int                        numPoints = ring->getNumPoints();
        const OGRSpatialReference *SR        = ring->getSpatialReference();

        for (int i = 0; i < numPoints; ++i)
        {
            OGRPoint vertexA, vertexB;
            ring->getPoint(i, &vertexA);
            ring->getPoint((i + 1) % numPoints, &vertexB);

            auto pointA = std::make_shared<GPoint>(
                units::angle::degree_t(vertexA.getX()),
                units::angle::degree_t(vertexA.getY()), *SR);

            auto pointB = std::make_shared<GPoint>(
                units::angle::degree_t(vertexB.getX()),
                units::angle::degree_t(vertexB.getY()), *SR);

            GLine edge(pointA, pointB);

            units::length::meter_t dist1 =
                edge.getPerpendicularDistance(*(line.startPoint()));
            units::length::meter_t dist2 =
                edge.getPerpendicularDistance(*(line.endPoint()));

            Line::LocationToLine isLeft1 = line.getlocationToLine(pointA);
            Line::LocationToLine isLeft2 = line.getlocationToLine(pointB);

            if (isLeft1 == Line::LocationToLine::left)
            {
                leftClearWidth = std::min(leftClearWidth, dist1);
            }
            else if (isLeft1 == Line::LocationToLine::right)
            {
                rightClearWidth = std::min(rightClearWidth, dist1);
            }

            if (isLeft2 == Line::LocationToLine::left)
            {
                leftClearWidth = std::min(leftClearWidth, dist2);
            }
            else if (isLeft2 == Line::LocationToLine::right)
            {
                rightClearWidth = std::min(rightClearWidth, dist2);
            }
        }
    };

    // Calculate for outer boundary
    const OGRLinearRing *outerRing =
        static_cast<const OGRLinearRing *>(mPolygon.getExteriorRing());
    calculateClearWidths(outerRing);

    // Calculate for each hole
    int numInteriorRings = mPolygon.getNumInteriorRings();
    for (int i = 0; i < numInteriorRings; ++i)
    {
        const OGRLinearRing *hole = mPolygon.getInteriorRing(i);
        calculateClearWidths(hole);
    }

    return rightClearWidth + leftClearWidth;
}

// =============================================================================
// Line and Segment Operations
// =============================================================================

bool Polygon::intersects(const std::shared_ptr<GLine> line)
{
    const OGRLineString &gdalLine = line->getGDALLine();

    // Quick check: if no intersection at all, return false
    if (!gdalLine.Intersects(&mPolygon))
    {
        return false;
    }

    // Compute actual intersection geometry
    OGRGeometry *intersection = gdalLine.Intersection(&mPolygon);
    if (!intersection)
    {
        return false;
    }

    bool               result   = false;
    OGRwkbGeometryType geomType = wkbFlatten(intersection->getGeometryType());

    if (geomType == wkbPoint || geomType == wkbMultiPoint)
    {
        // Intersection is just point(s) - check if any are NOT line endpoints
        const double eps      = 1e-9;
        double       startLon = line->startPoint()->getLongitude().value();
        double       startLat = line->startPoint()->getLatitude().value();
        double       endLon   = line->endPoint()->getLongitude().value();
        double       endLat   = line->endPoint()->getLatitude().value();

        auto checkIfEndpoint = [&](double x, double y) -> bool {
            return pointsMatch(x, y, startLon, startLat, eps) ||
                   pointsMatch(x, y, endLon, endLat, eps);
        };

        if (geomType == wkbPoint)
        {
            OGRPoint *pt = static_cast<OGRPoint *>(intersection);
            result       = !checkIfEndpoint(pt->getX(), pt->getY());
        }
        else
        {
            OGRMultiPoint *mpt = static_cast<OGRMultiPoint *>(intersection);
            for (int i = 0; i < mpt->getNumGeometries(); ++i)
            {
                OGRPoint *pt =
                    static_cast<OGRPoint *>(mpt->getGeometryRef(i));
                if (!checkIfEndpoint(pt->getX(), pt->getY()))
                {
                    result = true;
                    break;
                }
            }
        }
    }
    else
    {
        // Intersection is a line, polygon, or geometry collection
        result = true;
    }

    delete intersection;
    return result;
}

bool Polygon::segmentCrossesHoles(const std::shared_ptr<GLine> &segment) const
{
    return isSegmentDiagonalThroughHole(segment);
}

bool Polygon::isSegmentDiagonalThroughHole(
    const std::shared_ptr<GLine> &segment) const
{
    ensureHoleIndex();
    if (!mHoleIndex.hasHoles())
        return false;

    // Pre-compute segment bounding box
    double segMinLon = std::min(
        segment->startPoint()->getLongitude().value(),
        segment->endPoint()->getLongitude().value());
    double segMaxLon = std::max(
        segment->startPoint()->getLongitude().value(),
        segment->endPoint()->getLongitude().value());
    double segMinLat = std::min(
        segment->startPoint()->getLatitude().value(),
        segment->endPoint()->getLatitude().value());
    double segMaxLat = std::max(
        segment->startPoint()->getLatitude().value(),
        segment->endPoint()->getLatitude().value());

    return mHoleIndex.forEachCandidateInBBox(
        segMinLon, segMaxLon, segMinLat, segMaxLat,
        [&](int holeIndex) -> bool {
            return isSegmentPassingThroughHole(segment, holeIndex)
                   || isSegmentCrossingHoleBoundary(segment, holeIndex);
        });
}

bool Polygon::isSegmentPassingThroughHole(
    const std::shared_ptr<GLine> &segment, int holeIndex) const
{
    if (holeIndex >= mPolygon.getNumInteriorRings())
    {
        return false;
    }

    const OGRLinearRing *hole = mPolygon.getInteriorRing(holeIndex);
    if (!hole)
    {
        return false;
    }

    const OGRPoint startOGR = segment->startPoint()->getGDALPoint();
    const OGRPoint endOGR   = segment->endPoint()->getGDALPoint();

    bool startOnBoundary = hole->isPointOnRingBoundary(&startOGR, TRUE);
    bool endOnBoundary   = hole->isPointOnRingBoundary(&endOGR, TRUE);

    // Both endpoints on the same hole boundary: only exempt if they are
    // ADJACENT ring vertices (prev/next). Adjacent vertices form actual
    // polygon boundary edges whose midpoints naturally fall inside the
    // hole. Non-adjacent same-hole chords cut through the hole interior
    // (land) and must be checked via sampling.
    if (startOnBoundary && endOnBoundary)
    {
        double sLon = segment->startPoint()->getLongitude().value();
        double sLat = segment->startPoint()->getLatitude().value();
        double eLon = segment->endPoint()->getLongitude().value();
        double eLat = segment->endPoint()->getLatitude().value();

        if (areVerticesAdjacentOnHole(sLon, sLat, eLon, eLat, holeIndex))
            return false;  // Adjacent boundary edge — exempt
        // Non-adjacent: fall through to interior sampling
    }

    // Sample points along the segment
    auto   startPoint = segment->startPoint();
    auto   endPoint   = segment->endPoint();
    double startLon   = startPoint->getLongitude().value();
    double endLon     = endPoint->getLongitude().value();
    double startLat   = startPoint->getLatitude().value();
    double endLat     = endPoint->getLatitude().value();

    // Handle antimeridian crossing
    double lonDiff              = endLon - startLon;
    bool   crossesAntimeridian  = std::abs(lonDiff) > 180.0;
    double adjustedEndLon       = endLon;

    if (crossesAntimeridian)
    {
        if (lonDiff > 180.0)
        {
            adjustedEndLon = endLon - 360.0;
        }
        else if (lonDiff < -180.0)
        {
            adjustedEndLon = endLon + 360.0;
        }
    }

    for (int i = 1; i < HOLE_SAMPLING_COUNT; ++i)
    {
        double t = static_cast<double>(i) / HOLE_SAMPLING_COUNT;

        double lat = startLat * (1.0 - t) + endLat * t;
        double lon = startLon * (1.0 - t) + adjustedEndLon * t;

        // Normalize longitude using centralized function with epsilon tolerance
        lon = AngleUtils::normalizeLongitude(lon);

        // Optimized: pass coordinates directly instead of creating GPoint
        if (isPointInHoleByCoords(lon, lat, holeIndex))
        {
            return true;
        }
    }

    return false;
}

bool Polygon::isSegmentCrossingHoleBoundary(
    const std::shared_ptr<GLine> &segment, int holeIndex) const
{
    if (holeIndex >= mPolygon.getNumInteriorRings())
    {
        return false;
    }

    const OGRLinearRing *hole = mPolygon.getInteriorRing(holeIndex);
    if (!hole)
    {
        return false;
    }

    // Extract query segment coordinates once
    GSegment querySeg(segment);
    int numPoints = hole->getNumPoints();

    for (int i = 0; i < numPoints - 1; ++i)
    {
        double eLon1 = hole->getX(i);
        double eLat1 = hole->getY(i);
        double eLon2 = hole->getX(i + 1);
        double eLat2 = hole->getY(i + 1);
        GSegment holeSeg(eLon1, eLat1, eLon2, eLat2);

        if (querySeg.intersects(holeSeg, false))
        {
            // Coordinate-based endpoint check (no geodesic calls)
            constexpr double COORD_TOL = 1e-9;
            bool startOnEdge =
                (std::abs(querySeg.startLon() - eLon1) < COORD_TOL &&
                 std::abs(querySeg.startLat() - eLat1) < COORD_TOL) ||
                (std::abs(querySeg.startLon() - eLon2) < COORD_TOL &&
                 std::abs(querySeg.startLat() - eLat2) < COORD_TOL);
            bool endOnEdge =
                (std::abs(querySeg.endLon() - eLon1) < COORD_TOL &&
                 std::abs(querySeg.endLat() - eLat1) < COORD_TOL) ||
                (std::abs(querySeg.endLon() - eLon2) < COORD_TOL &&
                 std::abs(querySeg.endLat() - eLat2) < COORD_TOL);

            // Both endpoints on same hole edge = valid edge traversal
            if (startOnEdge && endOnEdge)
            {
                continue;
            }

            // One endpoint on hole edge - check if intersection is at vertex
            if (startOnEdge || endOnEdge)
            {
                if (!isIntersectionAtVertex(querySeg, holeSeg))
                {
                    return true;
                }
            }
            else
            {
                // Neither endpoint on hole edge = invalid crossing
                return true;
            }
        }
    }

    return false;
}

bool Polygon::isIntersectionAtVertex(
    const std::shared_ptr<GLine> &segment1,
    const std::shared_ptr<GLine> &segment2) const
{
    GSegment s1(segment1);
    GSegment s2(segment2);
    return isIntersectionAtVertex(s1, s2);
}

bool Polygon::isIntersectionAtVertex(
    const GSegment &seg1,
    const GSegment &seg2) const
{
    // Coordinate-based vertex proximity check
    // VERTEX_TOLERANCE_METERS ~= 0.1m ~= 0.000001 degrees at equator
    constexpr double COORD_TOL = 0.000002;  // ~0.2m at equator

    auto coordsNear = [](double lon1, double lat1,
                         double lon2, double lat2, double tol) {
        return std::abs(lon1 - lon2) < tol && std::abs(lat1 - lat2) < tol;
    };

    return coordsNear(seg1.startLon(), seg1.startLat(),
                      seg2.startLon(), seg2.startLat(), COORD_TOL) ||
           coordsNear(seg1.startLon(), seg1.startLat(),
                      seg2.endLon(), seg2.endLat(), COORD_TOL) ||
           coordsNear(seg1.endLon(), seg1.endLat(),
                      seg2.startLon(), seg2.startLat(), COORD_TOL) ||
           coordsNear(seg1.endLon(), seg1.endLat(),
                      seg2.endLon(), seg2.endLat(), COORD_TOL);
}

bool Polygon::isPointInHoleByCoords(double lon, double lat,
                                    int    holeIndex) const
{
    if (holeIndex >= mPolygon.getNumInteriorRings())
    {
        return false;
    }

    const OGRLinearRing *hole = mPolygon.getInteriorRing(holeIndex);
    if (!hole)
    {
        return false;
    }

    int numPoints = hole->getNumPoints();
    if (numPoints < 4)  // Need at least 3 unique points + closing point
    {
        return false;
    }

    // OGR-compatible ray-casting using translated coordinates.
    // Translating to the test point (comparing with 0) avoids
    // catastrophic cancellation that occurs when comparing two
    // large, nearly-equal values in the absolute-coordinate formula.
    // See OGR's OGRLinearRing::isPointInRing() for reference.
    int crossings = 0;

    double prevDx = hole->getX(0) - lon;
    double prevDy = hole->getY(0) - lat;

    // Iterate through edges (numPoints - 1 because last point duplicates first)
    for (int i = 1; i < numPoints; ++i)
    {
        double x1 = hole->getX(i) - lon;
        double y1 = hole->getY(i) - lat;
        double x2 = prevDx;
        double y2 = prevDy;

        if (((y1 > 0) && (y2 <= 0)) || ((y2 > 0) && (y1 <= 0)))
        {
            double xIntersect = (x1 * y2 - x2 * y1) / (y2 - y1);
            if (0.0 < xIntersect)
                crossings++;
        }

        prevDx = x1;
        prevDy = y1;
    }

    return (crossings % 2) == 1;
}

bool Polygon::isPointInHole(const std::shared_ptr<GPoint> &point,
                            int                            holeIndex) const
{
    // Delegate to optimized coordinate-based implementation
    return isPointInHoleByCoords(point->getLongitude().value(),
                                 point->getLatitude().value(),
                                 holeIndex);
}

// =============================================================================
// Bounding Box Operations
// =============================================================================

void Polygon::getEnvelope(double &minLon, double &maxLon, double &minLat,
                          double &maxLat) const
{
    OGREnvelope envelope;
    mPolygon.getEnvelope(&envelope);

    minLon = envelope.MinX;
    maxLon = envelope.MaxX;
    minLat = envelope.MinY;
    maxLat = envelope.MaxY;
}

bool Polygon::segmentBoundsIntersect(
    const std::shared_ptr<GLine> &segment) const
{
    double polyMinLon, polyMaxLon, polyMinLat, polyMaxLat;
    getEnvelope(polyMinLon, polyMaxLon, polyMinLat, polyMaxLat);

    double segStartLon = segment->startPoint()->getLongitude().value();
    double segEndLon   = segment->endPoint()->getLongitude().value();
    double segStartLat = segment->startPoint()->getLatitude().value();
    double segEndLat   = segment->endPoint()->getLatitude().value();

    double segMinLon = std::min(segStartLon, segEndLon);
    double segMaxLon = std::max(segStartLon, segEndLon);
    double segMinLat = std::min(segStartLat, segEndLat);
    double segMaxLat = std::max(segStartLat, segEndLat);

    // Check for non-overlap
    if (segMaxLon < polyMinLon || segMinLon > polyMaxLon ||
        segMaxLat < polyMinLat || segMinLat > polyMaxLat)
    {
        return false;
    }

    return true;
}

// =============================================================================
// Boundary Transformations
// =============================================================================

std::unique_ptr<OGRLinearRing>
Polygon::offsetBoundary(const OGRLinearRing &ring, bool inward,
                        units::length::meter_t offset) const
{
    const OGRSpatialReference           *currentSR = mPolygon.getSpatialReference();
    std::shared_ptr<OGRSpatialReference> targetSR =
        Point::getDefaultProjectionReference();

    // Create coordinate transformations
    OGRCoordinateTransformation *coordProjTransform =
        OGRCreateCoordinateTransformation(currentSR, targetSR.get());
    if (!coordProjTransform)
    {
        DESTROY_COORD_TRANSFORM(coordProjTransform);
        throw std::runtime_error(
            "Failed to create coordinate transformation.");
    }

    OGRCoordinateTransformation *coordReprojTransform =
        OGRCreateCoordinateTransformation(targetSR.get(), currentSR);
    if (!coordReprojTransform)
    {
        DESTROY_COORD_TRANSFORM(coordProjTransform);
        DESTROY_COORD_TRANSFORM(coordReprojTransform);
        throw std::runtime_error(
            "Failed to create coordinate transformation.");
    }

    // Transform ring to projected CRS
    OGRLinearRing *transformedRing =
        static_cast<OGRLinearRing *>(ring.clone());
    transformedRing->transform(coordProjTransform);

    // Apply buffer operation
    double       actualOffset     = inward ? -offset.value() : offset.value();
    OGRGeometry *bufferedGeometry = transformedRing->Buffer(actualOffset);
    delete transformedRing;

    // Transform back to WGS84
    bufferedGeometry->transform(coordReprojTransform);

    // Extract the linear ring
    OGRPolygon                    *bufferedPolygon =
        dynamic_cast<OGRPolygon *>(bufferedGeometry);
    std::unique_ptr<OGRLinearRing> newRing(nullptr);

    if (bufferedPolygon)
    {
        newRing.reset(static_cast<OGRLinearRing *>(
            bufferedPolygon->getExteriorRing()->clone()));
    }
    else
    {
        DESTROY_COORD_TRANSFORM(coordProjTransform);
        DESTROY_COORD_TRANSFORM(coordReprojTransform);
        delete bufferedGeometry;
        throw std::runtime_error("Buffered geometry is not a polygon.");
    }

    // Clean up
    DESTROY_COORD_TRANSFORM(coordProjTransform);
    DESTROY_COORD_TRANSFORM(coordReprojTransform);
    delete bufferedGeometry;

    return newRing;
}

void Polygon::transformOuterBoundary(bool                   inward,
                                     units::length::meter_t offset)
{
    const OGRLinearRing *currentOuterRing =
        static_cast<OGRLinearRing *>(mPolygon.getExteriorRing());

    std::unique_ptr<OGRLinearRing> newOuterRing =
        offsetBoundary(*currentOuterRing, inward, offset);

    // Create new polygon with transformed outer ring
    OGRPolygon newPolygon;
    newPolygon.addRing(newOuterRing.get());

    // Copy interior rings
    int numInteriorRings = mPolygon.getNumInteriorRings();
    for (int i = 0; i < numInteriorRings; ++i)
    {
        newPolygon.addRing(mPolygon.getInteriorRing(i));
    }

    mPolygon = newPolygon;
}

void Polygon::transformInnerHolesBoundaries(bool                   inward,
                                            units::length::meter_t offset)
{
    // Start with original outer boundary
    OGRPolygon newPolygon;
    newPolygon.addRing(mPolygon.getExteriorRing());

    // Offset each interior ring
    int numInteriorRings = mPolygon.getNumInteriorRings();
    for (int i = 0; i < numInteriorRings; ++i)
    {
        const OGRLinearRing *currentInnerRing = mPolygon.getInteriorRing(i);

        std::unique_ptr<OGRLinearRing> newInnerRing =
            offsetBoundary(*currentInnerRing, inward, offset);

        newPolygon.addRing(newInnerRing.get());
    }

    mPolygon = newPolygon;
}

// =============================================================================
// Simplification
// =============================================================================

std::shared_ptr<Polygon> Polygon::simplify(double toleranceMeters) const
{
    // Convert meters to approximate degrees for GDAL's Simplify()
    // At the equator: 1 degree ≈ 111,000 meters
    // This is an approximation; accuracy varies with latitude
    constexpr double METERS_PER_DEGREE = 111000.0;
    double toleranceDegrees = toleranceMeters / METERS_PER_DEGREE;

    // Use GDAL's SimplifyPreserveTopology (GEOS TopologyPreservingSimplifier)
    // instead of Simplify (Douglas-Peucker) which can produce invalid geometry
    // (self-intersecting rings), causing fallback to original polygon.
    std::unique_ptr<OGRGeometry> simplified(
        mPolygon.SimplifyPreserveTopology(toleranceDegrees));

    if (!simplified || simplified->getGeometryType() != wkbPolygon)
    {
        // If simplification fails, return a copy of the original
        return std::make_shared<Polygon>(mOutterBoundary, mInnerHoles, mUserID);
    }

    OGRPolygon* simplifiedPoly = static_cast<OGRPolygon*>(simplified.get());

    // Extract outer boundary points from simplified polygon
    const OGRLinearRing* extRing = simplifiedPoly->getExteriorRing();
    QVector<std::shared_ptr<GPoint>> newOuter;

    if (extRing)
    {
        int numPoints = extRing->getNumPoints();
        // Skip last point if it's the same as first (closed ring)
        if (numPoints > 1 &&
            extRing->getX(0) == extRing->getX(numPoints - 1) &&
            extRing->getY(0) == extRing->getY(numPoints - 1))
        {
            numPoints--;
        }

        for (int i = 0; i < numPoints; ++i)
        {
            newOuter.append(std::make_shared<GPoint>(
                units::angle::degree_t(extRing->getX(i)),
                units::angle::degree_t(extRing->getY(i))));
        }
    }

    // Extract inner holes from simplified polygon
    QVector<QVector<std::shared_ptr<GPoint>>> newHoles;
    int numInteriorRings = simplifiedPoly->getNumInteriorRings();

    for (int h = 0; h < numInteriorRings; ++h)
    {
        const OGRLinearRing* holeRing = simplifiedPoly->getInteriorRing(h);
        QVector<std::shared_ptr<GPoint>> holePoints;

        if (holeRing)
        {
            int numPoints = holeRing->getNumPoints();
            // Skip last point if it's the same as first (closed ring)
            if (numPoints > 1 &&
                holeRing->getX(0) == holeRing->getX(numPoints - 1) &&
                holeRing->getY(0) == holeRing->getY(numPoints - 1))
            {
                numPoints--;
            }

            for (int i = 0; i < numPoints; ++i)
            {
                holePoints.append(std::make_shared<GPoint>(
                    units::angle::degree_t(holeRing->getX(i)),
                    units::angle::degree_t(holeRing->getY(i))));
            }
        }

        if (holePoints.size() >= 3)
        {
            newHoles.append(holePoints);
        }
    }

    return std::make_shared<Polygon>(newOuter, newHoles, mUserID + "_simplified");
}

int Polygon::outerVertexCount() const
{
    return mOutterBoundary.size();
}

// =============================================================================
// String Representation
// =============================================================================

QString Polygon::toString(const QString &format, int decimalPercision) const
{
    QString perimeterStr =
        QString::number(perimeter().value(), 'f', decimalPercision);
    QString areaStr = QString::number(area().value(), 'f', decimalPercision);

    QString result = format;

    // Replace placeholders (case-insensitive)
    result.replace("%perimeter", perimeterStr, Qt::CaseInsensitive);
    result.replace("%area", areaStr, Qt::CaseInsensitive);

    return result;
}

}  // namespace ShipNetSimCore
