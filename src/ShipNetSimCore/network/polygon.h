/**
 * @file polygon.h
 * @brief Geodetic Polygon Implementation for WGS84 Ellipsoid.
 *
 * This file contains the Polygon class which represents a two-dimensional
 * polygon on the WGS84 ellipsoid. The polygon supports both an outer boundary
 * and multiple inner holes, enabling accurate representation of complex
 * geographic regions like water bodies with islands.
 *
 * Key features:
 * - Accurate geodetic area and perimeter calculations using GeographicLib
 * - Support for outer boundary and multiple inner holes (rings)
 * - Point-in-polygon testing with hole awareness
 * - Line intersection detection
 * - Segment validation for pathfinding through water regions
 * - Antimeridian crossing support for global-scale polygons
 * - Boundary transformation (offset/buffer operations)
 * - GDAL/OGR integration for spatial operations
 *
 * @author Ahmed Aredah
 * @date 10.12.2023
 */

#ifndef POLYGON_H
#define POLYGON_H

#include "../../third_party/units/units.h"
#include "../export.h"
#include "basegeometry.h"
#include "gdal_priv.h"
#include "gline.h"
#include "gpoint.h"
#include "gsegment.h"
#include "holespatialindex.h"
#include "outerringspatialindex.h"
#include "ogr_spatialref.h"
#include <QVector>
#include <memory>

namespace ShipNetSimCore
{

/**
 * @class Polygon
 * @brief Represents a geodetic polygon with outer boundary and inner holes.
 *
 * The Polygon class models a geographic region defined by an outer boundary
 * and zero or more inner holes. All geometric calculations use geodesic
 * mathematics via GeographicLib, ensuring accuracy for global-scale
 * applications including shipping simulation.
 *
 * The polygon is stored both as QVector of GPoint shared pointers (for
 * convenient access) and as an OGRPolygon (for GDAL spatial operations).
 *
 * @extends BaseGeometry
 */
class SHIPNETSIM_EXPORT Polygon : public BaseGeometry
{
    // =========================================================================
    // Member Variables
    // =========================================================================
private:
    /// Points defining the outer boundary (exterior ring)
    QVector<std::shared_ptr<GPoint>> mOutterBoundary;

    /// Points defining inner holes (interior rings)
    QVector<QVector<std::shared_ptr<GPoint>>> mInnerHoles;

    /// GDAL/OGR polygon representation for spatial operations
    OGRPolygon mPolygon;

    /// User-defined identifier for the polygon
    QString mUserID;

    /// Cache for antimeridian crossing status (-1=unchecked, 0=no, 1=yes)
    mutable int mCrossesAntimeridianCache = -1;

    /// Spatial index for fast interior ring (hole) queries.
    /// Built lazily on first query via ensureHoleIndex().
    mutable HoleSpatialIndex mHoleIndex;

    /// Spatial index for fast outer ring edge and containment queries.
    /// Built lazily on first query via ensureOuterRingIndex().
    mutable OuterRingSpatialIndex mOuterRingIndex;

    // -----------------------------------------------------------------
    // Ring topology: coordinate → ring-position map for adjacency checks
    // -----------------------------------------------------------------

    /** @brief Quantized coordinate key for O(1) ring position lookup. */
    struct CoordKey
    {
        long long lonQ;  ///< round(longitude * 1e9)
        long long latQ;  ///< round(latitude  * 1e9)

        static CoordKey from(double lon, double lat)
        {
            return {static_cast<long long>(std::round(lon * 1e9)),
                    static_cast<long long>(std::round(lat * 1e9))};
        }
        bool operator==(const CoordKey& o) const
        {
            return lonQ == o.lonQ && latQ == o.latQ;
        }
        struct Hash
        {
            size_t operator()(const CoordKey& k) const
            {
                size_t h1 = std::hash<long long>()(k.lonQ);
                size_t h2 = std::hash<long long>()(k.latQ);
                return h1 ^ (h2 * 2654435761ULL);
            }
        };
    };

    /** @brief Position of a vertex within a specific hole ring. */
    struct RingPos
    {
        int holeIdx;   ///< Interior ring index
        int pos;       ///< 0-based position within the ring (excluding closing point)
        int ringSize;  ///< Number of unique vertices in the ring
    };

    mutable bool mRingPosBuilt = false;
    mutable std::unordered_map<CoordKey, RingPos, CoordKey::Hash>
        mRingPositionMap;

    /** @brief Build ring position map lazily on first use. */
    void ensureRingPositionMap() const;

    // =========================================================================
    // Private Helper Methods
    // =========================================================================

    /**
     * @brief Offset a ring boundary by a given distance.
     *
     * Uses GDAL buffer operations to create an offset version of the ring.
     * The operation projects to a planar CRS, applies buffer, and re-projects
     * back to WGS84.
     *
     * @param ring The ring to offset
     * @param inward True to offset inward (shrink), false for outward (expand)
     * @param offset The distance to offset in meters
     * @return Unique pointer to the new offset ring
     * @throws std::runtime_error if transformation fails
     */
    std::unique_ptr<OGRLinearRing>
    offsetBoundary(const OGRLinearRing &ring, bool inward,
                   units::length::meter_t offset) const;

    /**
     * @brief Validate that a ring has sufficient valid points.
     *
     * Checks that the ring has at least 3 points and is not degenerate
     * (i.e., points are not collinear for triangles).
     *
     * @param ring The ring to validate
     * @param description Description for error messages
     * @throws std::runtime_error if ring is degenerate
     */
    static void validateRing(const OGRLinearRing &ring,
                             const QString       &description);

    /**
     * @brief Check if a segment passes through the interior of a hole.
     *
     * Samples points along the segment and tests if any fall inside the hole.
     *
     * @param segment The line segment to check
     * @param holeIndex Index of the hole to test against
     * @return true if segment passes through the hole interior
     */
    bool isSegmentPassingThroughHole(const std::shared_ptr<GLine> &segment,
                                     int holeIndex) const;

    /**
     * @brief Check if a segment crosses a hole boundary at non-vertex points.
     *
     * Detects invalid crossings where a segment intersects hole edges
     * at points that are not shared vertices.
     *
     * @param segment The line segment to check
     * @param holeIndex Index of the hole to test against
     * @return true if segment crosses hole boundary invalidly
     */
    bool isSegmentCrossingHoleBoundary(const std::shared_ptr<GLine> &segment,
                                       int holeIndex) const;

    /**
     * @brief Check if intersection between two segments is at a vertex.
     *
     * Determines if two segments meet at a shared endpoint rather than
     * crossing through each other.
     *
     * @param segment1 First line segment
     * @param segment2 Second line segment
     * @return true if intersection is at a shared vertex
     */
    bool isIntersectionAtVertex(const std::shared_ptr<GLine> &segment1,
                                const std::shared_ptr<GLine> &segment2) const;

    /** @brief Coordinate-based vertex intersection check (no geodesic). */
    bool isIntersectionAtVertex(const GSegment &seg1,
                                const GSegment &seg2) const;

    /**
     * @brief Point-in-polygon test for a specific hole using raw coordinates.
     *
     * Optimized version that performs ray casting directly on OGR ring
     * coordinates without creating intermediate GPoint objects. This is
     * the core implementation used by all point-in-hole tests.
     *
     * @param lon Longitude of the point to test
     * @param lat Latitude of the point to test
     * @param holeIndex Index of the hole to test against
     * @return true if point is inside the hole
     */
    bool isPointInHoleByCoords(double lon, double lat, int holeIndex) const;

    /**
     * @brief Point-in-polygon test for a specific hole.
     *
     * Convenience overload that extracts coordinates from GPoint.
     * Delegates to isPointInHoleByCoords for the actual test.
     *
     * @param point The point to test
     * @param holeIndex Index of the hole to test against
     * @return true if point is inside the hole
     */
    bool isPointInHole(const std::shared_ptr<GPoint> &point,
                       int                            holeIndex) const;

    /**
     * @brief Check if this polygon crosses the antimeridian (+-180 degrees).
     *
     * Analyzes edge directions to determine if the polygon wraps around
     * the antimeridian. Uses vertex ordering convention to distinguish
     * between antimeridian-crossing and prime-meridian-spanning polygons.
     *
     * @return true if the polygon crosses the antimeridian
     */
    bool crossesAntimeridian() const;

    /** @brief Ensure hole spatial index is built (lazy initialization). */
    void ensureHoleIndex() const;

    /** @brief Ensure outer ring spatial index is built. */
    void ensureOuterRingIndex() const;

    /**
     * @brief Access the outer ring spatial index (private — use
     *        segmentCrossesOuterRing() or isPointWithinPolygon() instead).
     */
    const OuterRingSpatialIndex& outerRingIndex() const;

    // =========================================================================
    // Constructors
    // =========================================================================
public:
    /**
     * @brief Default constructor creating an empty polygon.
     */
    Polygon();

    /**
     * @brief Construct a polygon with boundary and optional holes.
     *
     * @param boundary Points defining the outer boundary (will be closed)
     * @param holes Optional vector of inner hole boundaries
     * @param ID Optional user-defined identifier
     */
    Polygon(const QVector<std::shared_ptr<GPoint>>          &boundary,
            const QVector<QVector<std::shared_ptr<GPoint>>> &holes = {},
            const QString                                    ID    = "");

    /**
     * @brief Load polygons from an OGR-compatible shapefile.
     *
     * Single source of truth for polygon loading — used by both
     * OptimizedNetwork and ShipNetSimAdjBuilder to ensure identical
     * polygon data and consistent simplification results.
     *
     * @param filepath Path to the shapefile (.shp)
     * @return Vector of polygons (empty on failure)
     */
    static QVector<std::shared_ptr<Polygon>>
    loadFromShapefile(const QString& filepath);

    // =========================================================================
    // Boundary Accessors and Mutators
    // =========================================================================

    /**
     * @brief Set the outer boundary points.
     *
     * Updates the exterior ring while preserving any existing interior rings.
     *
     * @param newOuter Vector of points defining the new outer boundary
     */
    void setOuterPoints(const QVector<std::shared_ptr<GPoint>> &newOuter);

    /**
     * @brief Get the outer boundary points.
     * @return QVector of shared pointers to the outer boundary points
     */
    const QVector<std::shared_ptr<GPoint>>& outer() const;

    /**
     * @brief Set the inner hole boundaries.
     *
     * Replaces all existing holes with the new set while preserving
     * the exterior ring.
     *
     * @param holes Vector of vectors, each defining a hole boundary
     */
    void setInnerHolesPoints(
        const QVector<QVector<std::shared_ptr<GPoint>>> holes);

    /**
     * @brief Get the inner hole boundaries.
     * @return QVector of QVectors, each containing points of one hole
     */
    const QVector<QVector<std::shared_ptr<GPoint>>>& inners() const;

    // =========================================================================
    // Point Containment Tests
    // =========================================================================

    /**
     * @brief Check if a point is within the exterior ring.
     *
     * Returns true if the point is on the boundary or inside the
     * exterior ring, ignoring any holes.
     *
     * @param pointToCheck The point to test
     * @return true if point is within or on the exterior ring
     */
    bool isPointWithinExteriorRing(const GPoint &pointToCheck) const;

    /**
     * @brief Check if a point is within any interior ring (hole).
     *
     * Returns true if the point is on the boundary or inside any
     * of the holes.
     *
     * @param pointToCheck The point to test
     * @return true if point is within or on any hole
     */
    bool isPointWithinInteriorRings(const GPoint &pointToCheck) const;

    /**
     * @brief Find which interior ring (hole) contains a point.
     *
     * Returns the index of the hole containing the point, or -1 if
     * the point is not in any hole.
     *
     * @param pointToCheck The point to test
     * @return Index of containing hole (0-based), or -1 if not in any hole
     */
    int findContainingHoleIndex(const GPoint &pointToCheck) const;

    /**
     * @brief Check if a point lies on the boundary of a specific hole ring.
     *
     * Uses OGR isPointOnRingBoundary (with tolerance) for an exact check
     * that avoids the ray-casting ambiguity of findContainingHoleIndex.
     *
     * @param pt The point to test
     * @param holeIndex The hole ring index (0-based)
     * @return true if the point is on the hole's ring boundary
     */
    bool isPointOnHoleBoundary(const GPoint &pt, int holeIndex) const;

    /**
     * @brief Check if a point is within the polygon (excluding holes).
     *
     * Returns true if the point is inside the exterior ring but not
     * inside any holes. This is the main point-in-polygon test for
     * determining if a point is in the valid polygon area.
     *
     * Handles antimeridian-crossing polygons automatically.
     *
     * @param pointToCheck The point to test
     * @return true if point is in the valid polygon area
     */
    bool isPointWithinPolygon(const GPoint &pointToCheck) const;

    /**
     * @brief Check if a point lies on any ring boundary.
     *
     * Returns true if the point is on the exterior ring boundary
     * or any hole boundary.
     *
     * @param point The point to check
     * @return true if point is on any ring boundary
     */
    bool ringsContain(std::shared_ptr<GPoint> point) const;

    /**
     * @brief Check if two hole-boundary vertices are adjacent on their ring.
     *
     * Returns true only if the two coordinates are consecutive vertices
     * (prev/next) on the specified hole ring. Used to distinguish genuine
     * boundary edges from non-adjacent chords that cross the hole interior.
     *
     * @param lon1 Longitude of first vertex
     * @param lat1 Latitude of first vertex
     * @param lon2 Longitude of second vertex
     * @param lat2 Latitude of second vertex
     * @param holeIndex The hole ring both vertices belong to
     * @return true if vertices are adjacent (consecutive) on the ring
     */
    bool areVerticesAdjacentOnHole(double lon1, double lat1,
                                   double lon2, double lat2,
                                   int holeIndex) const;

    // =========================================================================
    // Line and Segment Operations
    // =========================================================================

    /**
     * @brief Check if a line intersects the polygon boundary.
     *
     * Returns true if the line crosses the polygon boundary at a
     * non-endpoint location. Touching at line endpoints only does
     * not count as intersection.
     *
     * @param line The line to test
     * @return true if line intersects (crosses) the polygon
     */
    bool intersects(const std::shared_ptr<GLine> line);

    /**
     * @brief Check if a segment is valid for water navigation.
     *
     * For water polygons (where holes represent land/islands), this
     * @brief Check if a segment crosses through any holes.
     *
     * @param segment The segment to check
     * @return true if segment crosses any hole
     */
    bool segmentCrossesHoles(const std::shared_ptr<GLine> &segment) const;

    /**
     * @brief Check if a segment creates an invalid diagonal through a hole.
     *
     * @param segment The segment to check
     * @return true if segment is an invalid diagonal through any hole
     */
    bool
    isSegmentDiagonalThroughHole(const std::shared_ptr<GLine> &segment) const;

    // =========================================================================
    // Geometric Calculations
    // =========================================================================

    /**
     * @brief Calculate the geodetic area of the polygon.
     *
     * Uses GeographicLib for accurate area calculation on the WGS84
     * ellipsoid. The area of holes is subtracted from the total.
     *
     * @return Area in square meters
     */
    units::area::square_meter_t area() const;

    /**
     * @brief Calculate the geodetic perimeter of the exterior ring.
     *
     * Uses GeographicLib for accurate perimeter calculation on the
     * WGS84 ellipsoid. Only calculates the exterior ring perimeter.
     *
     * @return Perimeter in meters
     */
    units::length::meter_t perimeter() const;

    /**
     * @brief Get the maximum clear width for a line inside the polygon.
     *
     * Calculates the sum of the minimum distances from the line to
     * the polygon boundaries on both sides.
     *
     * @param line The reference line
     * @return Total clear width (left + right) in meters
     */
    units::length::meter_t getMaxClearWidth(const GLine &line) const;

    // =========================================================================
    // Bounding Box Operations
    // =========================================================================

    /**
     * @brief Get the bounding box (envelope) of the polygon.
     *
     * @param[out] minLon Minimum longitude
     * @param[out] maxLon Maximum longitude
     * @param[out] minLat Minimum latitude
     * @param[out] maxLat Maximum latitude
     */
    void getEnvelope(double &minLon, double &maxLon, double &minLat,
                     double &maxLat) const;

    /**
     * @brief Check if a segment's bounding box intersects the polygon's.
     *
     * Fast preliminary check before detailed intersection testing.
     *
     * @param segment The segment to check
     * @return true if bounding boxes overlap
     */
    bool segmentBoundsIntersect(const std::shared_ptr<GLine> &segment) const;

    // =========================================================================
    // Boundary Transformations
    // =========================================================================

    /**
     * @brief Offset the outer boundary by a given distance.
     *
     * Expands (outward) or shrinks (inward) the exterior ring.
     *
     * @param inward True to shrink, false to expand
     * @param offset Distance to offset in meters
     */
    void transformOuterBoundary(bool inward, units::length::meter_t offset);

    /**
     * @brief Offset all inner hole boundaries by a given distance.
     *
     * Expands or shrinks all holes simultaneously.
     *
     * @param inward True to shrink holes, false to expand them
     * @param offset Distance to offset in meters
     */
    void transformInnerHolesBoundaries(bool                   inward,
                                       units::length::meter_t offset);

    // =========================================================================
    // Simplification
    // =========================================================================

    /**
     * @brief Create a simplified version of this polygon using Douglas-Peucker.
     *
     * Reduces the number of vertices while preserving the overall shape.
     * Useful for hierarchical pathfinding where a coarse representation
     * is needed for fast global navigation.
     *
     * @param toleranceMeters Simplification tolerance in meters.
     *        Larger values = fewer vertices but less accurate shape.
     *        Typical values: 500-5000 meters for ocean navigation.
     * @return New simplified Polygon with reduced vertex count
     */
    std::shared_ptr<Polygon> simplify(double toleranceMeters) const;

    /**
     * @brief Get the number of vertices in the outer boundary.
     * @return Number of vertices in exterior ring
     */
    int outerVertexCount() const;

    /**
     * @brief Test if a segment crosses any edge of the outer ring.
     *
     * Uses the grid-accelerated OuterRingSpatialIndex for O(k) spatial
     * filtering, then cross-product intersection test on candidate edges.
     *
     * @param v1 Segment start point
     * @param v2 Segment end point
     * @return true if the segment crosses any outer ring edge
     */
    bool segmentCrossesOuterRing(
        const std::shared_ptr<GPoint> &v1,
        const std::shared_ptr<GPoint> &v2) const;

    // =========================================================================
    // String Representation
    // =========================================================================

    /**
     * @brief Convert the polygon to a formatted string.
     *
     * Supported placeholders (case-insensitive):
     * - %perimeter: Perimeter of exterior ring in meters
     * - %area: Area of polygon (excluding holes) in square meters
     *
     * @param format Format string with placeholders
     * @param decimalPercision Number of decimal places for values
     * @return Formatted string representation
     */
    QString toString(const QString &format =
                         "Polygon Perimeter: %perimeter || Area: %area",
                     int decimalPercision = 5) const override;
};

}  // namespace ShipNetSimCore

#endif  // POLYGON_H
