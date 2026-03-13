/**
 * @file gsegment.h
 * @brief Lightweight 2D segment for fast geometric queries.
 *
 * GSegment is a value type holding four doubles (lon1, lat1, lon2, lat2).
 * It provides segment-segment intersection (cross-product orientation test),
 * AABB intersection (Liang-Barsky), bounding box, and haversine length.
 *
 * Designed as the hot-path counterpart to GLine: no heap allocations,
 * no shared_ptr, no OGR/GDAL, no GeographicLib. 32 bytes, trivially copyable.
 *
 * @author Ahmed Aredah
 * @date 2026-03-13
 */

#ifndef GSEGMENT_H
#define GSEGMENT_H

#include "../export.h"
#include <cmath>
#include <memory>

namespace ShipNetSimCore
{

// Forward declarations — avoid pulling in heavy headers
class GPoint;
class GLine;

/**
 * @class GSegment
 * @brief Lightweight 2D line segment for fast geometric operations.
 *
 * Value type: 32 bytes, no heap allocation, trivially copyable,
 * cache-line friendly. Constructable from raw coordinates, GPoint,
 * or GLine objects.
 */
class SHIPNETSIM_EXPORT GSegment
{
public:
    // =========================================================================
    // Constructors
    // =========================================================================

    /** @brief Default: degenerate zero-length segment at origin. */
    GSegment() : mLon1(0), mLat1(0), mLon2(0), mLat2(0) {}

    /** @brief From raw coordinates (zero overhead). */
    GSegment(double lon1, double lat1, double lon2, double lat2)
        : mLon1(lon1), mLat1(lat1), mLon2(lon2), mLat2(lat2) {}

    /** @brief From GPoints (extracts coordinates, no shared_ptr retained). */
    GSegment(const GPoint& start, const GPoint& end);

    /** @brief From shared GPoints. */
    GSegment(const std::shared_ptr<GPoint>& start,
             const std::shared_ptr<GPoint>& end);

    /** @brief From existing GLine (extracts coordinates). */
    explicit GSegment(const GLine& line);

    /** @brief From shared GLine. */
    explicit GSegment(const std::shared_ptr<GLine>& line);

    // =========================================================================
    // Coordinate Access
    // =========================================================================

    double startLon() const { return mLon1; }
    double startLat() const { return mLat1; }
    double endLon()   const { return mLon2; }
    double endLat()   const { return mLat2; }

    // =========================================================================
    // Geometric Queries
    // =========================================================================

    /**
     * @brief 2D segment-segment intersection (cross-product orientation test).
     *
     * Uses the standard four-orientation algorithm. Handles collinear
     * overlapping segments correctly.
     *
     * @param other The other segment to test against
     * @param ignoreEndpoints If true, shared endpoints don't count
     * @return true if segments intersect
     */
    bool intersects(const GSegment& other,
                    bool ignoreEndpoints = true) const;

    /**
     * @brief Test if this segment intersects an axis-aligned bounding box.
     *
     * Uses Liang-Barsky parametric clipping for exact segment-vs-AABB.
     *
     * @param minLon Left edge
     * @param minLat Bottom edge
     * @param maxLon Right edge
     * @param maxLat Top edge
     * @return true if segment intersects or is inside the AABB
     */
    bool intersectsAABB(double minLon, double minLat,
                        double maxLon, double maxLat) const;

    /**
     * @brief Compute this segment's bounding box.
     */
    void boundingBox(double& minLon, double& minLat,
                     double& maxLon, double& maxLat) const;

    /**
     * @brief Haversine approximate length in meters.
     *
     * No geodesic Inverse — uses the spherical Earth approximation.
     */
    double approxLengthMeters() const;

    // =========================================================================
    // Orientation Test
    // =========================================================================

    enum Orientation { Collinear, Clockwise, CounterClockwise };

    /**
     * @brief Orientation of three points in 2D lon/lat space.
     *
     * Uses cross-product of vectors (p→q) and (p→r).
     */
    static Orientation orientation(double px, double py,
                                   double qx, double qy,
                                   double rx, double ry);

    // =========================================================================
    // Endpoint Utilities
    // =========================================================================

    /**
     * @brief Check if (lon, lat) is near one of this segment's endpoints.
     */
    bool isEndpoint(double lon, double lat,
                    double tolerance = 1e-6) const;

    /**
     * @brief Check if this segment shares an endpoint with another.
     */
    bool sharesEndpoint(const GSegment& other,
                        double tolerance = 1e-6) const;

    /**
     * @brief True if segment spans more than 180 degrees of longitude.
     */
    bool crossesAntimeridian() const;

    /**
     * @brief Haversine distance between two lon/lat points in meters.
     *
     * No heap allocation, no GPoint construction. Pure arithmetic.
     */
    static double haversineRaw(double lon1, double lat1,
                               double lon2, double lat2);

private:
    double mLon1, mLat1, mLon2, mLat2;  // 32 bytes total

    /** @brief 2D cross product: (a - o) x (b - o). */
    static double cross2D(double ox, double oy,
                          double ax, double ay,
                          double bx, double by);

    /** @brief Check if point (qx,qy) lies on segment (px,py)-(rx,ry) given collinearity. */
    static bool onSegment(double px, double py,
                          double qx, double qy,
                          double rx, double ry);
};

}  // namespace ShipNetSimCore

#endif  // GSEGMENT_H
