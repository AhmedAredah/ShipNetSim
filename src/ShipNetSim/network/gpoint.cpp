#include "gpoint.h"
#include "point.h"
#include "qendian.h"
#include <ogr_spatialref.h>
#include <ogr_geometry.h>
#include <GeographicLib/Geodesic.hpp>

std::shared_ptr<OGRSpatialReference> GPoint::spatialRef = nullptr;

GPoint::GPoint() : mOGRPoint(0.0, 0.0), mIsPort(false),
    mDwellTime(units::time::second_t(0.0)) {}

GPoint::GPoint( units::angle::degree_t lon, units::angle::degree_t lat,
               OGRSpatialReference crc)
    : mIsPort(false), mDwellTime(units::time::second_t(0.0))
{
    // Initialize mOGRPoint with longitude and latitude
    setLatitude(lat);
    setLongitude(lon);

    OGRSpatialReference* SR = nullptr;
    if (!crc.IsEmpty()) {
        if (!crc.IsGeographic())
        {
            qFatal("Spatial reference must be geodetic!");
        }
        // Clone the passed spatial reference and assign
        // it to spatialRef pointer
        SR = crc.Clone();
    } else {
        // If no spatial reference is passed, create a
        // new spatial reference for WGS84
        SR = getDefaultReprojectionReference()->Clone();
    }

    // Assign the spatial reference to mOGRPoint.
    // mOGRPoint does not take ownership of the spatialRef.
    mOGRPoint.assignSpatialReference(SR);
}

// Constructor with optional CRS, defaults to an empty spatial reference
GPoint::GPoint(units::angle::degree_t lon,
               units::angle::degree_t lat,
               QString ID,
               OGRSpatialReference crc)
    : mUserID(ID), mIsPort(false), mDwellTime(units::time::second_t(0.0))
{
    // Initialize mOGRPoint with longitude and latitude
    setLatitude(lat);
    setLongitude(lon);

    OGRSpatialReference* SR = nullptr;
    if (!crc.IsEmpty()) {
        if (!crc.IsGeographic())
        {
            qFatal("Spatial reference must be geodetic!");
        }
        // Clone the passed spatial reference and assign
        // it to spatialRef pointer
        SR = crc.Clone();
    } else {
        // If no spatial reference is passed, create a
        // new spatial reference for WGS84
        SR = getDefaultReprojectionReference()->Clone();
    }

    // Assign the spatial reference to mOGRPoint.
    // mOGRPoint does not take ownership of the spatialRef.
    mOGRPoint.assignSpatialReference(SR);
}

std::shared_ptr<OGRSpatialReference> GPoint::getDefaultReprojectionReference()
{
    // Initialize spatialRef with WGS84 if it hasn't been initialized yet
    if (!GPoint::spatialRef) {
        GPoint::spatialRef = std::make_shared<OGRSpatialReference>();
        OGRErr err = spatialRef->SetWellKnownGeogCS("WGS84");
        if (err != OGRERR_NONE) {
            qFatal("Failed to set WGS84 spatial reference");
        }
    }
    return GPoint::spatialRef;
}

void GPoint::setDefaultReprojectionReference(std::string wellknownCS)
{
    if (!spatialRef) {
        GPoint::spatialRef = std::make_shared<OGRSpatialReference>();
    }

    // Temporary spatial reference for validation
    std::shared_ptr<OGRSpatialReference> tempRef =
        std::make_shared<OGRSpatialReference>();
    OGRErr err = tempRef->SetWellKnownGeogCS(wellknownCS.c_str());
    if (err != OGRERR_NONE) {
        // Exit the function on failure
        qFatal("Failed to interpret the provided spatial reference: %",
               qPrintable(QString::fromStdString(wellknownCS)));
    }

    // Check if the spatial reference is geodetic
    if (!tempRef->IsGeographic()) {
        // Exit the function if not geodetic
        qFatal("The provided spatial reference is not geodetic: %",
               qPrintable(QString::fromStdString(wellknownCS)));
    }

    // If validation passed, assign the validated spatial
    // reference to the class's static member
    GPoint::spatialRef = tempRef;

}

OGRPoint GPoint::getGDALPoint() const { return mOGRPoint; }

// Project this point to a given projected CRS
Point GPoint::projectTo(OGRSpatialReference* targetSR) const  {
    // Ensure the target Spatial Reference is valid and is a projected CRS
    if (!targetSR || !targetSR->IsProjected()) {
        qFatal("Target Spatial Reference "
               "is not valid or not a projected CRS.");
    }

    const OGRSpatialReference* currentSR = mOGRPoint.getSpatialReference();
    if (currentSR == nullptr) {
        qFatal("Current Spatial Reference is not set.");
    }

    // Create a coordinate transformation from the current
    // geographic CRS to the target projected CRS
    OGRCoordinateTransformation* coordTransform =
        OGRCreateCoordinateTransformation(currentSR, targetSR);
    if (!coordTransform) {
        qFatal("Failed to create coordinate transformation.");
    }

    double x = mOGRPoint.getX();
    double y = mOGRPoint.getY();

    // Transform the point's coordinates from geographic to projected CRS
    if (!coordTransform->Transform(1, &x, &y)) {
        OCTDestroyCoordinateTransformation(coordTransform);
        qFatal("Failed to transform point coordinates.");
    }

    OCTDestroyCoordinateTransformation(coordTransform);

    // Create and return the transformed point.
    return Point(units::length::meter_t(x),
                 units::length::meter_t(y),
                 mUserID, *targetSR);
}

GPoint GPoint::pointAtDistanceAndHeading(units::length::meter_t distance,
                                         units::angle::degree_t heading) const
{
    // Ensure the spatial reference is set for both points
    const OGRSpatialReference* thisSR = mOGRPoint.getSpatialReference();

    double semiMajorAxis = thisSR->GetSemiMajor();
    double flattening = 1.0 / thisSR->GetInvFlattening();

    // Create a GeographicLib::Geodesic object with the ellipsoid parameters
    const GeographicLib::Geodesic geod(semiMajorAxis, flattening);

    double newLat, newLon;
    geod.Direct(this->getLatitude().value(),
                this->getLongitude().value(),
                heading.value(), distance.value(), newLat, newLon);

    // Create a new GPoint with the calculated latitude and longitude.
    // Assuming the spatial reference (SR) of the new point is the same
    // as the current point.
    const OGRSpatialReference* currentSR = mOGRPoint.getSpatialReference();
    OGRSpatialReference* newSR = nullptr;
    if (currentSR != nullptr) {
        newSR = currentSR->Clone();
    }

    return GPoint(units::angle::degree_t(newLat),
                  units::angle::degree_t(newLon), "NewPoint", *newSR);
}

void GPoint::transformDatumTo(OGRSpatialReference* targetSR)
{
    if (targetSR && targetSR->IsGeographic())
    {
        const OGRSpatialReference* currentSR = mOGRPoint.getSpatialReference();
        if (currentSR && !currentSR->IsSame(targetSR)) {
            OGRCoordinateTransformation* coordTransform =
                OGRCreateCoordinateTransformation(currentSR, targetSR);
            if (coordTransform) {
                double x = mOGRPoint.getX(), y = mOGRPoint.getY();
                if (coordTransform->Transform(1, &x, &y)) {
                    // Update the internal OGRPoint coordinates
                    mOGRPoint.setX(x);
                    mOGRPoint.setY(y);

                    // Update point's spatial reference to the target
                    mOGRPoint.assignSpatialReference(targetSR);
                }
                OCTDestroyCoordinateTransformation(coordTransform);
            }
        }
    }
    else
    {
        qFatal("Target spatial reference is not geodetic!");
    }

}


units::length::meter_t GPoint::distance(const GPoint& other) const
{
    // Ensure the spatial reference is set for both points
    const OGRSpatialReference* thisSR = mOGRPoint.getSpatialReference();
    const OGRSpatialReference* otherSR = other.mOGRPoint.getSpatialReference();

    if (thisSR == nullptr || otherSR == nullptr) {
        throw std::runtime_error("Spatial reference not "
                                 "set for one or both points.");
    }

    if (!thisSR->IsSame(otherSR))
    {
        qFatal("Mismatch geodetic datums!");
    }


    double semiMajorAxis = thisSR->GetSemiMajor();
    double flattening = 1.0 / thisSR->GetInvFlattening();

    // Create a GeographicLib::Geodesic object with the ellipsoid parameters
    const GeographicLib::Geodesic geod(semiMajorAxis, flattening);

    double distance;
    // Calculate the distance
    geod.Inverse(this->getLatitude().value(), this->getLongitude().value(),
                 other.getLatitude().value(), other.getLongitude().value(),
                 distance);

    return units::length::meter_t(distance);
}

units::angle::degree_t GPoint::getLatitude() const {
    return units::angle::degree_t(mOGRPoint.getY());
}

units::angle::degree_t GPoint::getLongitude() const {
    return units::angle::degree_t(mOGRPoint.getX());
}

bool GPoint::isPort() const { return mIsPort; }

units::time::second_t GPoint::getDwellTime() const { return mDwellTime; }

void GPoint::setLatitude(units::angle::degree_t lat) {
    double normalizedLat = lat.value();

    // Normalize latitude to the range [-90, 90]
    // If latitude goes beyond 90 or -90, it flips direction.
    while (normalizedLat > 90.0 || normalizedLat < -90.0) {
        if (normalizedLat > 90.0) {
            normalizedLat = 180.0 - normalizedLat;
        } else if (normalizedLat < -90.0) {
            normalizedLat = -180.0 - normalizedLat;
        }
    }

    mOGRPoint.setY(normalizedLat); // Update the internal OGRPoint as well
}

void GPoint::setLongitude(units::angle::degree_t lon) {
    double normalizedLon = lon.value();

    // Normalize longitude to the range [-180, 180]
    while (normalizedLon > 180.0 || normalizedLon < -180.0) {
        if (normalizedLon > 180.0) {
            normalizedLon -= 360.0;
        } else if (normalizedLon < -180.0) {
            normalizedLon += 360.0;
        }
    }

    mOGRPoint.setX(normalizedLon); // Update the internal OGRPoint as well
}

void GPoint::MarkAsNonPort()
{
    mIsPort = false;
    mDwellTime = units::time::second_t(0.0);
}

void GPoint::MarkAsPort(units::time::second_t dwellTime)
{
    mIsPort = true;
    mDwellTime = dwellTime;
}

// Implementation of getMiddlePoint
GPoint GPoint::getMiddlePoint(const GPoint& endPoint) const
{
    auto midLat =
        units::angle::degree_t((this->getLatitude().value() +
                                endPoint.getLatitude().value()) / 2.0);
    auto midLon =
        units::angle::degree_t((this->getLongitude().value() +
                                endPoint.getLongitude().value()) / 2.0);
    return GPoint(midLat, midLon, "Midpoint", *mOGRPoint.getSpatialReference());
}

// Function to convert the point to a string representation.
QString GPoint::toString() const
{
    QString str;
     // Convert coordinate value to string in decimal format
    QString xStr = QString::number(getLatitude().value(), 'f', 3);
    QString yStr = QString::number(getLongitude().value(), 'f', 3);


    // Format the string as "Point userID(x, y)".
    if (mUserID.isEmpty() ||
        mUserID == "temporary point")  // Format the string as "(x, y)".
    {
        str =
            QString("(%1; %2)").arg(xStr, yStr);
    }
    else  // Format the string as "Point userID(x, y)".
    {
        str = QString("Point %1(%2; %3)").arg(mUserID, xStr, yStr);
    }
    return str;  // Return the formatted string.
}

// Implementation of the nested Hash structure
std::size_t GPoint::Hash::operator()(const std::shared_ptr<GPoint>& p) const
{
    if (!p) return 0; // Handle null pointers

    auto x_value = p->getLatitude().value();
    auto y_value = p->getLongitude().value();

    return std::hash<decltype(x_value)>()(x_value) ^
           std::hash<decltype(y_value)>()(y_value);
}

bool GPoint::operator==(const GPoint &other) const
{
    // Return true if both x and y coordinates are the same.
    return getLatitude() == other.getLatitude() &&
           getLongitude() == other.getLongitude();
}

// Implementation of the nested Equal structure
bool GPoint::Equal::operator()(const std::shared_ptr<GPoint>& lhs,
                              const std::shared_ptr<GPoint>& rhs) const
{
    if (!lhs || !rhs) return false; // Handle null pointers
    return *lhs == *rhs;
}

void GPoint::serialize(std::ostream& out) const
{
    if (!out) {
        throw std::runtime_error("Output stream is not ready for writing.");
    }

    // Serialize x-coordinate
    std::uint64_t xNet =
        qToBigEndian(std::bit_cast<std::uint64_t>(getLongitude().value()));
    out.write(reinterpret_cast<const char*>(&xNet), sizeof(xNet));

    // Serialize y-coordinate
    std::uint64_t yNet =
        qToBigEndian(std::bit_cast<std::uint64_t>(getLatitude().value()));
    out.write(reinterpret_cast<const char*>(&yNet), sizeof(yNet));

    // Serialize mUserID as string length followed by string data
    std::string userIDStr = mUserID.toStdString();
    std::uint64_t userIDLen =
        qToBigEndian(static_cast<std::uint64_t>(userIDStr.size()));
    out.write(reinterpret_cast<const char*>(&userIDLen),
              sizeof(userIDLen));
    out.write(userIDStr.c_str(), userIDStr.size());

    // Serialize mIsPort
    out.write(reinterpret_cast<const char*>(&mIsPort),
              sizeof(mIsPort));

    // Serialize mDwellTime
    std::uint64_t dwellTimeNet =
        qToBigEndian(std::bit_cast<std::uint64_t>(mDwellTime.value()));
    out.write(reinterpret_cast<const char*>(&dwellTimeNet),
              sizeof(dwellTimeNet));

    // Check for write failures
    if (!out) {
        throw std::runtime_error("Failed to write point"
                                 " data to output stream.");
    }
}

void GPoint::deserialize(std::istream& in)
{
    if (!in)
    {
        throw std::runtime_error("Input stream is not ready for reading.");
    }

    // Deserialize x-coordinate
    std::uint64_t xNet;
    in.read(reinterpret_cast<char*>(&xNet), sizeof(xNet));
    if (!in)
    {
        throw std::runtime_error("Failed to read x-coordinate"
                                 " from input stream.");
    }
    mOGRPoint.setX(qFromBigEndian(std::bit_cast<double>(xNet)));

    // Deserialize y-coordinate
    std::uint64_t yNet;
    in.read(reinterpret_cast<char*>(&yNet), sizeof(yNet));
    if (!in) {
        throw std::runtime_error("Failed to read y-coordinate "
                                 "from input stream.");
    }
    mOGRPoint.setY(qFromBigEndian(std::bit_cast<double>(yNet)));

    // Deserialize userID
    std::uint64_t userIDLen;
    in.read(reinterpret_cast<char*>(&userIDLen), sizeof(userIDLen));
    userIDLen = qFromBigEndian(userIDLen);
    std::string userIDStr(userIDLen, '\0');
    in.read(&userIDStr[0], userIDLen);
    if (!in)
    {
        throw std::runtime_error("Failed to read userID "
                                 "from input stream.");
    }
    mUserID = QString::fromStdString(userIDStr);

    // Deserialize mIsPort
    in.read(reinterpret_cast<char*>(&mIsPort), sizeof(mIsPort));
    if (!in)
    {
        throw std::runtime_error("Failed to read mIsPort "
                                 "flag from input stream.");
    }

    // Deserialize mDwellTime
    std::uint64_t dwellTimeNet;
    in.read(reinterpret_cast<char*>(&dwellTimeNet),
            sizeof(dwellTimeNet));
    if (!in) {
        throw std::runtime_error("Failed to read dwell "
                                 "time from input stream.");
    }
    mDwellTime = units::time::second_t(
        qFromBigEndian(std::bit_cast<double>(dwellTimeNet)));
}


// Definition of the stream insertion operator
std::ostream& operator<<(std::ostream& os, const GPoint& point) {
    os << "Point(ID: " << point.mUserID.toStdString() <<
        ", Lat: " << point.getLatitude().value() <<
        ", Lon: " << point.getLongitude().value() << ")";
    return os;
}