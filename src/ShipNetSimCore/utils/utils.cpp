#include "utils.h"

namespace ShipNetSimCore
{
namespace Utils
{

// Definition of the function
QString getFirstExistingPathFromList(
    QVector<QString> filePaths,
    QVector<QString> extensions)
{
    for (const QString& loc: filePaths)
    {
        QFileInfo fileInfo(loc);

        // If the path is relative, resolve it to an absolute path
        QString fullPath;
        if (fileInfo.isRelative())
        {
            fullPath = QDir::current().absoluteFilePath(loc);
        }
        else
        {
            fullPath = loc;
        }

        // Check if the file exists
        if (QFile::exists(fullPath))
        {
            // Get the extension of the file
            QString ext = fileInfo.suffix().toLower();

            // if it has the needed extension or no required extension,
            // return the path
            if (extensions.empty() ||
                extensions.contains(ext, Qt::CaseInsensitive))
            {
                return fullPath;
            }
        }
    }
    return QString("");
};

QString formatString(const QString preString,
					 const QString mainString,
					 const QString postString,
					 const QString filler,
					 int length) {
    // Start by concatenating preString and mainString
    QString result = preString + mainString;

    // Calculate remaining length to fill
    int remainingLength = length - postString.length() - result.length();

    // Append the filler string as many times as needed to reach the
    // desired length excluding postString
    while (remainingLength > 0) {
        // Append only the necessary part of the filler
        result.append(filler.left(remainingLength));
        // Update remaining length
        remainingLength = length - postString.length() - result.length();
    }

    result += postString;  // Append postString after filling

    return result;  // Return the formatted string
};

std::vector<double> linspace_step(double start,
                                  double end,
                                  double step)
{
    std::vector<double> linspaced;
    int numSteps = static_cast<int>(std::ceil((end - start) / step));

    for(int i = 0; i <= numSteps; ++i){
        double currentValue = start + i * step;
        // To avoid floating point errors, we limit the value to 'end'
        if(currentValue > end) currentValue = end;
        linspaced.push_back(currentValue);
    }

    return linspaced;
};

QVector<QPair<QString, QString>>
splitStringStream(const QString& inputString,
                  const QString& delimiter)
{
    QVector<QPair<QString, QString>> result;

    // Split input string by newline characters
    const auto& lines = inputString.split("\n", Qt::SkipEmptyParts);
    for (const auto& line : lines)
    {
        // Split each line by the given delimiter and append to result
        int delimiterPos = line.indexOf(delimiter);
        if (delimiterPos != -1)
        {
            QString first = line.left(delimiterPos);
            QString second = line.mid(delimiterPos + delimiter.size());
            result.append(qMakePair(first, second));
        }
        else
        {
            result.append(qMakePair(line, ""));
        }
    }

    return result;
};

QString getHomeDirectory()
{
    QString homeDir;

// OS-dependent home directory retrieval
#if defined(Q_OS_WIN)
    homeDir = QDir::homePath();
#elif defined(Q_OS_LINUX) || defined(Q_OS_MAC)
    homeDir = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
#endif

    if (!homeDir.isEmpty())
    {
        // Creating a path to ShipNetSim folder in the Documents directory
        const QString documentsDir = QDir(homeDir).filePath("Documents");
        const QString folder = QDir(documentsDir).filePath("ShipNetSim");

        QDir().mkpath(folder); // Create the directory if it doesn't exist

        return folder;
    }

    throw std::runtime_error("Error: Cannot retrieve home directory!");
};

bool stringToBool(const QString& str, bool* ok)
{
    QString lowerStr = str.toLower();
    if (lowerStr == "true" || str == "1") {
        if (ok != nullptr) *ok = true;
        return true;
    } else if (lowerStr == "false" || str == "0") {
        if (ok != nullptr) *ok = true;
        return false;
    } else {
        if (ok != nullptr) *ok = false;
        qWarning() << "Invalid boolean string:" << str;
        return false;
    }
};

}
}