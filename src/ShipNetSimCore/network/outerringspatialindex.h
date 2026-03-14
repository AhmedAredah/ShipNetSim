/**
 * @file outerringspatialindex.h
 * @brief Grid-based spatial index for polygon outer ring edge and
 *        containment queries.
 *
 * Pre-caches outer ring coordinates as raw doubles and builds a spatial
 * grid over edges. Replaces O(n) edge crossing loops and OGR-based
 * containment tests with O(k) grid-accelerated checks and inline
 * ray-casting on cached coordinates.
 *
 * @author Ahmed Aredah
 * @date 13.03.2026
 */

#ifndef OUTERRINGSPATIALINDEX_H
#define OUTERRINGSPATIALINDEX_H

#include "gpoint.h"
#include <QVector>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace ShipNetSimCore
{

/**
 * @class OuterRingSpatialIndex
 * @brief Spatial index for fast outer ring edge crossing and containment
 * queries.
 *
 * Stores the outer ring vertices as contiguous raw double arrays (no
 * shared_ptr overhead) and indexes edges into a uniform grid for O(k)
 * spatial queries. Provides:
 *
 * - doesSegmentCross(): grid-accelerated edge crossing test
 * - containsPoint(): inline ray-casting on cached coordinates
 *
 * Designed as a private member of Polygon, built lazily on first query.
 * Thread safety: not thread-safe.
 */
class OuterRingSpatialIndex
{

    // =========================================================================
    // Public Interface
    // =========================================================================
public:
    OuterRingSpatialIndex() = default;

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    /**
     * @brief Build the index from outer boundary vertices.
     *
     * Extracts coordinates into raw double arrays and builds a uniform
     * grid over the edges. Safe to call multiple times (rebuilds).
     *
     * @param outerBoundary The outer ring vertices
     */
    void build(const QVector<std::shared_ptr<GPoint>> &outerBoundary);

    /**
     * @brief Clear the index. Must call build() again before queries.
     */
    void invalidate();

    // -------------------------------------------------------------------------
    // State Queries
    // -------------------------------------------------------------------------

    bool isBuilt() const { return mBuilt; }
    int  numVertices() const { return static_cast<int>(mLons.size()); }

    /** @brief Access cached longitude array. */
    const std::vector<double> &lons() const { return mLons; }

    /** @brief Access cached latitude array. */
    const std::vector<double> &lats() const { return mLats; }

    // -------------------------------------------------------------------------
    // Spatial Queries
    // -------------------------------------------------------------------------

    /**
     * @brief Test if a segment crosses any edge of the outer ring.
     *
     * Uses the edge grid for spatial filtering, then cross-product
     * intersection test on candidate edges. Skips edges that share
     * an endpoint with the query segment (within tolerance 1e-9).
     *
     * @param ax Segment start longitude
     * @param ay Segment start latitude
     * @param bx Segment end longitude
     * @param by Segment end latitude
     * @return true if the segment crosses any outer ring edge
     */
    bool doesSegmentCross(double ax, double ay, double bx,
                          double by) const;

    /**
     * @brief Test if a point is inside the outer ring (ray-casting).
     *
     * Inline ray-casting on cached double arrays. No OGR overhead.
     *
     * @param lon Point longitude
     * @param lat Point latitude
     * @return true if point is inside the outer ring
     */
    bool containsPoint(double lon, double lat) const;

    // =========================================================================
    // Private Implementation
    // =========================================================================
private:
    /// Cached outer ring coordinates (contiguous for cache efficiency)
    std::vector<double> mLons;
    std::vector<double> mLats;

    /// Edge spatial grid: cell → list of edge indices
    std::vector<std::vector<int>> mEdgeGrid;

    int    mCols     = 0;
    int    mRows     = 0;
    double mMinLon   = 0.0;
    double mMinLat   = 0.0;
    double mCellSize = 0.0;
    bool   mBuilt    = false;

    /// Reusable deduplication buffer for multi-cell queries
    mutable std::vector<bool> mVisited;
};

}  // namespace ShipNetSimCore

#endif  // OUTERRINGSPATIALINDEX_H
