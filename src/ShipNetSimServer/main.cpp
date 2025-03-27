#include <QCoreApplication>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include "SimulationServer.h"
#include "utils/shipscommon.h"

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);

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

    // Add username option
    QCommandLineOption usernameOption(
        QStringList() << "u" << "username",
        "RabbitMQ server username.",
        "username",
        "guest");  // Default to 'guest' if not specified
    parser.addOption(usernameOption);

    // Add password option
    QCommandLineOption passwordOption(
        QStringList() << "pw" << "password",
        "RabbitMQ server password.",
        "password",
        "guest");  // Default to 'guest' if not specified
    parser.addOption(passwordOption);

    // Process the command-line arguments
    parser.process(app);

    // Retrieve the hostname and port from CLI arguments or use default values
    QString hostname = parser.value(hostnameOption);
    int port = parser.value(portOption).toInt();
    QString username = parser.value(usernameOption);
    QString password = parser.value(passwordOption);

    // Start the simulation server
    SimulationServer server;
    server.startRabbitMQServer(hostname.toStdString(),
                               port,
                               username.toStdString(),
                               password.toStdString());

    return app.exec();
}
