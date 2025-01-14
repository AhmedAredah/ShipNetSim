#include <QCoreApplication>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include "SimulationServer.h"
#include "utils/shipscommon.h"
#include "utils/logger.h"


int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);

    // Attach the logger first thing:
    ShipNetSimCore::Logger::attach("ShipNetSimServer");

    qRegisterMetaType<ShipsResults>("ShipsResults");

    // Set up the command-line parser
    QCommandLineParser parser;
    parser.setApplicationDescription("ShipNetSim Server with RabbitMQ");
    parser.addHelpOption();

    // Add hostname option
    QCommandLineOption hostnameOption(
        QStringList() << "n" << "hostname",
        "RabbitMQ server hostname (default: localhost).",
        "hostname",
        "localhost");
    parser.addOption(hostnameOption);

    // Add port option (default: 5672)
    QCommandLineOption portOption(
        QStringList() << "p" << "port",
        "RabbitMQ server port (default: 5672).",
        "port",
        "5672");
    parser.addOption(portOption);

    // Process the command-line arguments
    parser.process(app);

    // Retrieve the hostname and port from CLI arguments or use default values
    QString hostname = parser.value(hostnameOption);
    int port = parser.value(portOption).toInt();

    // Start the simulation server
    SimulationServer server;
    server.startRabbitMQServer(hostname.toStdString(), port);

    // Detach logger and start event loop.
    ShipNetSimCore::Logger::detach();
    return app.exec();
}
