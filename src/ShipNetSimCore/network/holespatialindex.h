/**
 * @file holespatialindex.h
 * @brief Grid-based spatial index for polygon interior ring (hole) queries.
 *
 * This file contains the HoleSpatialIndex class which accelerates
 * point-in-hole and segment-hole intersection queries by partitioning
 * holes into a uniform 1-degree grid. Instead of scanning all holes
 * (O(h)), queries only check holes in the relevant grid cell (O(k),
 * k typically 1-5).
 *
 * @author Ahmed Aredah
 * @date 13.03.2026
 */

#ifndef HOLESPATIALINDEX_H
#define HOLESPATIALINDEX_H

#include "ogr_geometry.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace ShipNetSimCore
{

/**
 * @class HoleSpatialIndex
 * @brief Grid-based spatial index for fast interior ring (hole) queries.
 *
 * Accelerates point-in-hole and segment-hole intersection queries by
 * partitioning holes into a uniform grid. Each grid cell stores indices
 * of holes whose bounding boxes overlap that cell. Queries use a single
 * cell lookup (point) or iterate overlapping cells (segment bbox),
 * then apply per-hole bounding box filtering before the caller performs
 * the actual geometric test (ray-casting, boundary check, etc.).
 *
 * Designed for use as a private member of the Polygon class, built
 * lazily on first query. The visitor pattern (forEachCandidate) avoids
 * heap allocation during queries.
 *
 * Thread safety: not thread-safe. Callers must ensure exclusive access
 * during build() and query operations on the same instance.
 */
class HoleSpatialIndex
{

    // =========================================================================
    // Public Interface
    // =========================================================================
public:
    /**
     * @brief Default constructor. Creates an empty, unbuilt index.
     */
    HoleSpatialIndex() = default;

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------

    /**
     * @brief Build the spatial index from an OGRPolygon's interior rings.
     *
     * Precomputes bounding boxes for all interior rings and populates
     * a uniform grid. If the polygon has no interior rings, the index
     * is marked as built with zero holes.
     *
     * Safe to call multiple times (rebuilds from scratch each time).
     *
     * @param polygon The OGRPolygon whose interior rings to index
     */
    void build(const OGRPolygon &polygon);

    /**
     * @brief Clear the index. Must call build() again before queries.
     */
    void invalidate();

    // -------------------------------------------------------------------------
    // State Queries
    // -------------------------------------------------------------------------

    /** @brief Whether the index has been built. */
    bool isBuilt() const { return mBuilt; }

    /** @brief Whether any holes exist in the index. */
    bool hasHoles() const { return !mBBoxes.empty(); }

    /** @brief Number of indexed holes. */
    int numHoles() const { return static_cast<int>(mBBoxes.size()); }

    /** @brief Access precomputed bounding box for a hole by index. */
    const OGREnvelope &holeBBox(int index) const { return mBBoxes[index]; }

    // -------------------------------------------------------------------------
    // Visitor-Based Queries
    // -------------------------------------------------------------------------

    /**
     * @brief Visit candidate holes whose bbox contains the given point.
     *
     * Performs a grid cell lookup for the point's location, then
     * iterates holes in that cell, filtering by bounding box. For each
     * candidate that passes the bbox test, invokes the callback with
     * the hole index.
     *
     * Zero allocation, inlineable. Follows the same visitor pattern
     * as Quadtree::visitSegmentsAlongSegment.
     *
     * @tparam Func Callback type: bool(int holeIndex)
     * @param lon Query point longitude (degrees)
     * @param lat Query point latitude (degrees)
     * @param func Callback invoked per candidate. Return true to stop.
     * @return true if callback stopped early (found match)
     */
    template <typename Func>
    bool forEachCandidate(double lon, double lat, Func &&func) const
    {
        if (mBBoxes.empty())
            return false;

        int col = static_cast<int>((lon - mMinLon) / mCellSize);
        int row = static_cast<int>((lat - mMinLat) / mCellSize);
        if (col < 0 || col >= mCols || row < 0 || row >= mRows)
            return false;

        for (int i : mGrid[row * mCols + col])
        {
            const auto &bbox = mBBoxes[i];
            if (lon >= bbox.MinX && lon <= bbox.MaxX && lat >= bbox.MinY
                && lat <= bbox.MaxY)
            {
                if (func(i))
                    return true;
            }
        }
        return false;
    }

    /**
     * @brief Visit candidate holes overlapping a bounding box region.
     *
     * Iterates all grid cells that the query bbox covers, collects
     * candidate holes with bbox overlap, and invokes the callback.
     * Deduplicates across cells using a reusable visited buffer.
     *
     * @tparam Func Callback type: bool(int holeIndex)
     * @param minLon Query bbox minimum longitude (degrees)
     * @param maxLon Query bbox maximum longitude (degrees)
     * @param minLat Query bbox minimum latitude (degrees)
     * @param maxLat Query bbox maximum latitude (degrees)
     * @param func Callback invoked per candidate. Return true to stop.
     * @return true if callback stopped early (found match)
     */
    template <typename Func>
    bool forEachCandidateInBBox(double minLon, double maxLon, double minLat,
                                double maxLat, Func &&func) const
    {
        if (mBBoxes.empty())
            return false;

        int colMin =
            std::max(0, static_cast<int>((minLon - mMinLon) / mCellSize));
        int colMax = std::min(
            mCols - 1, static_cast<int>((maxLon - mMinLon) / mCellSize));
        int rowMin =
            std::max(0, static_cast<int>((minLat - mMinLat) / mCellSize));
        int rowMax = std::min(
            mRows - 1, static_cast<int>((maxLat - mMinLat) / mCellSize));

        // Reuse visited buffer to deduplicate across overlapping cells
        mVisited.assign(mBBoxes.size(), false);

        for (int r = rowMin; r <= rowMax; ++r)
        {
            for (int c = colMin; c <= colMax; ++c)
            {
                for (int idx : mGrid[r * mCols + c])
                {
                    if (mVisited[idx])
                        continue;
                    mVisited[idx] = true;

                    const auto &bbox = mBBoxes[idx];
                    if (maxLon < bbox.MinX || minLon > bbox.MaxX
                        || maxLat < bbox.MinY || minLat > bbox.MaxY)
                        continue;

                    if (func(idx))
                        return true;
                }
            }
        }
        return false;
    }

    // =========================================================================
    // Private Implementation
    // =========================================================================
private:
    /// Precomputed bounding boxes, one per interior ring (hole)
    std::vector<OGREnvelope> mBBoxes;

    /// Flat 2D grid: cell at (row, col) = mGrid[row * mCols + col].
    /// Each cell stores indices of holes whose bbox overlaps that cell.
    std::vector<std::vector<int>> mGrid;

    int    mCols     = 0;   ///< Number of grid columns
    int    mRows     = 0;   ///< Number of grid rows
    double mMinLon   = 0.0; ///< Grid origin longitude (degrees)
    double mMinLat   = 0.0; ///< Grid origin latitude (degrees)
    double mCellSize = 0.0; ///< Grid cell size (degrees)
    bool   mBuilt    = false;

    /// Reusable deduplication buffer for forEachCandidateInBBox.
    /// Mutable because queries are logically const.
    mutable std::vector<bool> mVisited;
};

}  // namespace ShipNetSimCore

#endif  // HOLESPATIALINDEX_H
