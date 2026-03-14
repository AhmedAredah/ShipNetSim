/**
 * @file outerringspatialindex.cpp
 * @brief Implementation of OuterRingSpatialIndex.
 *
 * @author Ahmed Aredah
 * @date 13.03.2026
 */

#include "outerringspatialindex.h"

namespace ShipNetSimCore
{

// =============================================================================
// Lifecycle
// =============================================================================

void OuterRingSpatialIndex::build(
    const QVector<std::shared_ptr<GPoint>> &outerBoundary)
{
    invalidate();
    mBuilt = true;

    int n = outerBoundary.size();
    if (n < 3)
        return;

    // Step 1: Extract coordinates into raw double arrays
    mLons.resize(n);
    mLats.resize(n);
    double minLon = std::numeric_limits<double>::max();
    double maxLon = std::numeric_limits<double>::lowest();
    double minLat = std::numeric_limits<double>::max();
    double maxLat = std::numeric_limits<double>::lowest();

    for (int i = 0; i < n; ++i)
    {
        mLons[i] = outerBoundary[i]->getLongitude().value();
        mLats[i] = outerBoundary[i]->getLatitude().value();
        minLon   = std::min(minLon, mLons[i]);
        maxLon   = std::max(maxLon, mLons[i]);
        minLat   = std::min(minLat, mLats[i]);
        maxLat   = std::max(maxLat, mLats[i]);
    }

    // Step 2: Configure grid (1-degree cells)
    mCellSize = 1.0;
    mMinLon   = minLon;
    mMinLat   = minLat;
    mCols     = std::max(
        1,
        static_cast<int>(std::ceil((maxLon - minLon) / mCellSize)));
    mRows = std::max(
        1,
        static_cast<int>(std::ceil((maxLat - minLat) / mCellSize)));

    mEdgeGrid.resize(mRows * mCols);

    // Step 3: Insert each edge into overlapping grid cells
    for (int i = 0; i < n; ++i)
    {
        int next = (i + 1) % n;

        double edgeMinLon = std::min(mLons[i], mLons[next]);
        double edgeMaxLon = std::max(mLons[i], mLons[next]);
        double edgeMinLat = std::min(mLats[i], mLats[next]);
        double edgeMaxLat = std::max(mLats[i], mLats[next]);

        int colMin = std::max(
            0, static_cast<int>((edgeMinLon - mMinLon) / mCellSize));
        int colMax = std::min(
            mCols - 1,
            static_cast<int>((edgeMaxLon - mMinLon) / mCellSize));
        int rowMin = std::max(
            0, static_cast<int>((edgeMinLat - mMinLat) / mCellSize));
        int rowMax = std::min(
            mRows - 1,
            static_cast<int>((edgeMaxLat - mMinLat) / mCellSize));

        for (int r = rowMin; r <= rowMax; ++r)
        {
            for (int c = colMin; c <= colMax; ++c)
            {
                mEdgeGrid[r * mCols + c].push_back(i);
            }
        }
    }

    // Pre-allocate visited buffer
    mVisited.resize(n, false);
}

void OuterRingSpatialIndex::invalidate()
{
    mLons.clear();
    mLats.clear();
    mEdgeGrid.clear();
    mVisited.clear();
    mCols     = 0;
    mRows     = 0;
    mMinLon   = 0.0;
    mMinLat   = 0.0;
    mCellSize = 0.0;
    mBuilt    = false;
}

// =============================================================================
// Spatial Queries
// =============================================================================

bool OuterRingSpatialIndex::doesSegmentCross(double ax, double ay,
                                             double bx, double by) const
{
    int n = static_cast<int>(mLons.size());
    if (n < 3)
        return false;

    // Compute segment bbox
    double segMinLon = std::min(ax, bx);
    double segMaxLon = std::max(ax, bx);
    double segMinLat = std::min(ay, by);
    double segMaxLat = std::max(ay, by);

    // Grid cell range
    int colMin = std::max(
        0, static_cast<int>((segMinLon - mMinLon) / mCellSize));
    int colMax = std::min(
        mCols - 1,
        static_cast<int>((segMaxLon - mMinLon) / mCellSize));
    int rowMin = std::max(
        0, static_cast<int>((segMinLat - mMinLat) / mCellSize));
    int rowMax = std::min(
        mRows - 1,
        static_cast<int>((segMaxLat - mMinLat) / mCellSize));

    // Clamp to valid range
    if (colMin > colMax || rowMin > rowMax)
        return false;

    // Cross-product helpers
    auto sign = [](double v) -> int {
        if (v > 1e-12)
            return 1;
        if (v < -1e-12)
            return -1;
        return 0;
    };

    auto cross = [](double ox, double oy, double px, double py,
                    double qx, double qy) -> double {
        return (px - ox) * (qy - oy) - (py - oy) * (qx - ox);
    };

    constexpr double EPS = 1e-9;

    // Deduplicate edges across cells
    mVisited.assign(n, false);

    for (int r = rowMin; r <= rowMax; ++r)
    {
        for (int c = colMin; c <= colMax; ++c)
        {
            for (int edgeIdx : mEdgeGrid[r * mCols + c])
            {
                if (mVisited[edgeIdx])
                    continue;
                mVisited[edgeIdx] = true;

                int    next = (edgeIdx + 1) % n;
                double cx   = mLons[edgeIdx];
                double cy   = mLats[edgeIdx];
                double dx   = mLons[next];
                double dy   = mLats[next];

                // Skip edges sharing an endpoint with query segment
                bool aIsEndpoint =
                    (std::abs(ax - cx) < EPS && std::abs(ay - cy) < EPS)
                    || (std::abs(ax - dx) < EPS
                        && std::abs(ay - dy) < EPS);
                bool bIsEndpoint =
                    (std::abs(bx - cx) < EPS && std::abs(by - cy) < EPS)
                    || (std::abs(bx - dx) < EPS
                        && std::abs(by - dy) < EPS);

                if (aIsEndpoint || bIsEndpoint)
                    continue;

                // Cross-product intersection test
                int d1 = sign(cross(cx, cy, dx, dy, ax, ay));
                int d2 = sign(cross(cx, cy, dx, dy, bx, by));
                int d3 = sign(cross(ax, ay, bx, by, cx, cy));
                int d4 = sign(cross(ax, ay, bx, by, dx, dy));

                if (d1 != d2 && d3 != d4 && d1 != 0 && d2 != 0
                    && d3 != 0 && d4 != 0)
                {
                    return true;
                }
            }
        }
    }

    return false;
}

bool OuterRingSpatialIndex::containsPoint(double lon, double lat) const
{
    int n = static_cast<int>(mLons.size());
    if (n < 3)
        return false;

    // OGR-compatible ray-casting using translated coordinates.
    // Translating to the test point (comparing with 0) avoids
    // catastrophic cancellation that occurs when comparing two
    // large, nearly-equal values in the absolute-coordinate formula.
    // See OGR's OGRLinearRing::isPointInRing() for reference.
    int crossings = 0;

    double prevDx = mLons[0] - lon;
    double prevDy = mLats[0] - lat;

    for (int i = 1; i <= n; ++i)
    {
        int    idx = i % n;
        double x1  = mLons[idx] - lon;
        double y1  = mLats[idx] - lat;
        double x2  = prevDx;
        double y2  = prevDy;

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

}  // namespace ShipNetSimCore
