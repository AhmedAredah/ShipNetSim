/**
 * @file holespatialindex.cpp
 * @brief Implementation of the HoleSpatialIndex class.
 *
 * Builds a uniform 1-degree grid over a polygon's interior rings
 * for fast spatial queries. The grid maps each cell to the list of
 * hole indices whose bounding boxes overlap that cell.
 *
 * @author Ahmed Aredah
 * @date 13.03.2026
 */

#include "holespatialindex.h"

namespace ShipNetSimCore
{

// =============================================================================
// Lifecycle
// =============================================================================

void HoleSpatialIndex::build(const OGRPolygon &polygon)
{
    // Clear any previous state
    invalidate();
    mBuilt = true;

    int numHoles = polygon.getNumInteriorRings();
    if (numHoles == 0)
        return;

    // Step 1: Compute bounding boxes for all holes
    mBBoxes.resize(numHoles);
    for (int i = 0; i < numHoles; ++i)
    {
        polygon.getInteriorRing(i)->getEnvelope(&mBBoxes[i]);
    }

    // Step 2: Get exterior ring envelope for grid bounds
    OGREnvelope outerEnv;
    polygon.getExteriorRing()->getEnvelope(&outerEnv);

    // Step 3: Configure grid (1-degree cells)
    mCellSize = 1.0;
    mMinLon   = outerEnv.MinX;
    mMinLat   = outerEnv.MinY;
    mCols     = std::max(
        1, static_cast<int>(
               std::ceil((outerEnv.MaxX - outerEnv.MinX) / mCellSize)));
    mRows = std::max(
        1, static_cast<int>(
               std::ceil((outerEnv.MaxY - outerEnv.MinY) / mCellSize)));

    mGrid.resize(mRows * mCols);

    // Step 4: Insert each hole into all overlapping grid cells
    for (int i = 0; i < numHoles; ++i)
    {
        const auto &bbox = mBBoxes[i];

        int colMin = std::max(
            0, static_cast<int>((bbox.MinX - mMinLon) / mCellSize));
        int colMax = std::min(
            mCols - 1,
            static_cast<int>((bbox.MaxX - mMinLon) / mCellSize));
        int rowMin = std::max(
            0, static_cast<int>((bbox.MinY - mMinLat) / mCellSize));
        int rowMax = std::min(
            mRows - 1,
            static_cast<int>((bbox.MaxY - mMinLat) / mCellSize));

        for (int r = rowMin; r <= rowMax; ++r)
        {
            for (int c = colMin; c <= colMax; ++c)
            {
                mGrid[r * mCols + c].push_back(i);
            }
        }
    }

    // Pre-allocate visited buffer for forEachCandidateInBBox
    mVisited.resize(numHoles, false);
}

void HoleSpatialIndex::invalidate()
{
    mBBoxes.clear();
    mGrid.clear();
    mVisited.clear();
    mCols     = 0;
    mRows     = 0;
    mMinLon   = 0.0;
    mMinLat   = 0.0;
    mCellSize = 0.0;
    mBuilt    = false;
}

}  // namespace ShipNetSimCore
