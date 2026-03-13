/**
 * @file gsegment.cpp
 * @brief Implementation of the GSegment lightweight segment class.
 *
 * All geometric operations use pure 2D math in lon/lat space.
 * No GDAL, no GeographicLib, no heap allocations.
 *
 * @author Ahmed Aredah
 * @date 2026-03-13
 */

#include "gsegment.h"
#include "gpoint.h"
#include "gline.h"

#include <algorithm>
#include <cmath>

namespace ShipNetSimCore
{

// =============================================================================
// Constructors from GPoint / GLine
// =============================================================================

GSegment::GSegment(const GPoint& start, const GPoint& end)
    : mLon1(start.getLongitude().value())
    , mLat1(start.getLatitude().value())
    , mLon2(end.getLongitude().value())
    , mLat2(end.getLatitude().value())
{
}

GSegment::GSegment(const std::shared_ptr<GPoint>& start,
                   const std::shared_ptr<GPoint>& end)
    : mLon1(start->getLongitude().value())
    , mLat1(start->getLatitude().value())
    , mLon2(end->getLongitude().value())
    , mLat2(end->getLatitude().value())
{
}

GSegment::GSegment(const GLine& line)
    : mLon1(line.startPoint()->getLongitude().value())
    , mLat1(line.startPoint()->getLatitude().value())
    , mLon2(line.endPoint()->getLongitude().value())
    , mLat2(line.endPoint()->getLatitude().value())
{
}

GSegment::GSegment(const std::shared_ptr<GLine>& line)
    : mLon1(line->startPoint()->getLongitude().value())
    , mLat1(line->startPoint()->getLatitude().value())
    , mLon2(line->endPoint()->getLongitude().value())
    , mLat2(line->endPoint()->getLatitude().value())
{
}

// =============================================================================
// Internal Helpers
// =============================================================================

double GSegment::cross2D(double ox, double oy,
                         double ax, double ay,
                         double bx, double by)
{
    return (ax - ox) * (by - oy) - (ay - oy) * (bx - ox);
}

bool GSegment::onSegment(double px, double py,
                         double qx, double qy,
                         double rx, double ry)
{
    // Given that p, q, r are collinear, check if q lies on segment p-r
    return qx <= std::max(px, rx) && qx >= std::min(px, rx) &&
           qy <= std::max(py, ry) && qy >= std::min(py, ry);
}

GSegment::Orientation GSegment::orientation(double px, double py,
                                            double qx, double qy,
                                            double rx, double ry)
{
    double val = cross2D(px, py, qx, qy, rx, ry);
    if (std::abs(val) < 1e-12)
        return Collinear;
    return (val > 0) ? CounterClockwise : Clockwise;
}

// =============================================================================
// Segment-Segment Intersection
// =============================================================================

bool GSegment::intersects(const GSegment& other,
                          bool ignoreEndpoints) const
{
    double p1x = mLon1, p1y = mLat1;
    double q1x = mLon2, q1y = mLat2;
    double p2x = other.mLon1, p2y = other.mLat1;
    double q2x = other.mLon2, q2y = other.mLat2;

    // If ignoring endpoints and segments share an endpoint, return false
    if (ignoreEndpoints)
    {
        auto near = [](double ax, double ay, double bx, double by) {
            return std::abs(ax - bx) < 1e-10 && std::abs(ay - by) < 1e-10;
        };
        if (near(p1x, p1y, p2x, p2y) || near(p1x, p1y, q2x, q2y) ||
            near(q1x, q1y, p2x, p2y) || near(q1x, q1y, q2x, q2y))
        {
            return false;
        }
    }

    Orientation o1 = orientation(p1x, p1y, q1x, q1y, p2x, p2y);
    Orientation o2 = orientation(p1x, p1y, q1x, q1y, q2x, q2y);
    Orientation o3 = orientation(p2x, p2y, q2x, q2y, p1x, p1y);
    Orientation o4 = orientation(p2x, p2y, q2x, q2y, q1x, q1y);

    // General case
    if (o1 != o2 && o3 != o4)
        return true;

    // Collinear special cases
    if (o1 == Collinear && onSegment(p1x, p1y, p2x, p2y, q1x, q1y))
        return true;
    if (o2 == Collinear && onSegment(p1x, p1y, q2x, q2y, q1x, q1y))
        return true;
    if (o3 == Collinear && onSegment(p2x, p2y, p1x, p1y, q2x, q2y))
        return true;
    if (o4 == Collinear && onSegment(p2x, p2y, q1x, q1y, q2x, q2y))
        return true;

    return false;
}

// =============================================================================
// Segment-AABB Intersection (Liang-Barsky)
// =============================================================================

bool GSegment::intersectsAABB(double minLon, double minLat,
                              double maxLon, double maxLat) const
{
    // Check if either endpoint is inside
    if (mLon1 >= minLon && mLon1 <= maxLon &&
        mLat1 >= minLat && mLat1 <= maxLat)
        return true;
    if (mLon2 >= minLon && mLon2 <= maxLon &&
        mLat2 >= minLat && mLat2 <= maxLat)
        return true;

    // Segment bounding box quick reject
    double sMinLon = std::min(mLon1, mLon2);
    double sMaxLon = std::max(mLon1, mLon2);
    double sMinLat = std::min(mLat1, mLat2);
    double sMaxLat = std::max(mLat1, mLat2);

    if (sMaxLon < minLon || sMinLon > maxLon ||
        sMaxLat < minLat || sMinLat > maxLat)
        return false;

    // Liang-Barsky parametric clipping
    double dx = mLon2 - mLon1;
    double dy = mLat2 - mLat1;

    double p[4] = { -dx, dx, -dy, dy };
    double q[4] = { mLon1 - minLon, maxLon - mLon1,
                    mLat1 - minLat, maxLat - mLat1 };

    double tMin = 0.0;
    double tMax = 1.0;

    for (int i = 0; i < 4; ++i)
    {
        if (std::abs(p[i]) < 1e-15)
        {
            // Parallel to this edge
            if (q[i] < 0.0)
                return false;  // Outside and parallel
        }
        else
        {
            double t = q[i] / p[i];
            if (p[i] < 0.0)
            {
                if (t > tMin) tMin = t;
            }
            else
            {
                if (t < tMax) tMax = t;
            }
            if (tMin > tMax)
                return false;
        }
    }

    return true;
}

// =============================================================================
// Bounding Box
// =============================================================================

void GSegment::boundingBox(double& minLon, double& minLat,
                           double& maxLon, double& maxLat) const
{
    minLon = std::min(mLon1, mLon2);
    maxLon = std::max(mLon1, mLon2);
    minLat = std::min(mLat1, mLat2);
    maxLat = std::max(mLat1, mLat2);
}

// =============================================================================
// Haversine Length
// =============================================================================

double GSegment::approxLengthMeters() const
{
    constexpr double DEG_TO_RAD = M_PI / 180.0;
    constexpr double EARTH_RADIUS = 6371000.0;

    double lat1r = mLat1 * DEG_TO_RAD;
    double lat2r = mLat2 * DEG_TO_RAD;
    double dLat = lat2r - lat1r;
    double dLon = (mLon2 - mLon1) * DEG_TO_RAD;

    // Wrap dLon for antimeridian
    if (dLon > M_PI)  dLon -= 2.0 * M_PI;
    if (dLon < -M_PI) dLon += 2.0 * M_PI;

    double a = std::sin(dLat * 0.5) * std::sin(dLat * 0.5) +
               std::cos(lat1r) * std::cos(lat2r) *
               std::sin(dLon * 0.5) * std::sin(dLon * 0.5);

    return EARTH_RADIUS * 2.0 * std::asin(std::sqrt(a));
}

// =============================================================================
// Endpoint Utilities
// =============================================================================

bool GSegment::isEndpoint(double lon, double lat, double tolerance) const
{
    return (std::abs(mLon1 - lon) < tolerance &&
            std::abs(mLat1 - lat) < tolerance) ||
           (std::abs(mLon2 - lon) < tolerance &&
            std::abs(mLat2 - lat) < tolerance);
}

bool GSegment::sharesEndpoint(const GSegment& other, double tolerance) const
{
    return isEndpoint(other.mLon1, other.mLat1, tolerance) ||
           isEndpoint(other.mLon2, other.mLat2, tolerance);
}

bool GSegment::crossesAntimeridian() const
{
    return std::abs(mLon1 - mLon2) > 180.0;
}

}  // namespace ShipNetSimCore
