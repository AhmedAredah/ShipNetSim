/**
 * @file SimulationServer.h
 * @brief Defines the SimulationServer class that integrates with a
 * RabbitMQ server to manage simulation tasks as a server.
 *
 * This file contains the SimulationServer class declaration. It includes
 * the necessary ShipNetSim and AMQP (Advanced Message Queuing Protocol)
 * headers to facilitate communication with RabbitMQ for handling
 * simulation tasks in a distributed environment. This class serves as a
 * bridge between the simulation application (ShipNetSim) and RabbitMQ,
 * managing message sending, receiving, and simulation state transitions
 * based on the messages processed.
 */

#ifndef SIMULATIONSERVER_H
#define SIMULATIONSERVER_H

#include "./simulatorapi.h"
#include "qwaitcondition.h"
#include "utils/shipscommon.h"
#include <QObject>
#include <QMap>
#include <QJsonDocument>
#include <QJsonObject>
#include <rabbitmq-c/amqp.h>
#include <rabbitmq-c/tcp_socket.h>
#include <containerLib/container.h>
#ifdef _WIN32
struct timeval {
    // Define timeval structure for Windows platforms where it is not defined.
    long tv_sec;  // seconds
    long tv_usec; // microseconds
};
#else
// Include the timeval definition from sys/time.h for Unix-like systems
#include <sys/time.h>
#endif


using ShipParamsMap = QMap<QString, QString>;
Q_DECLARE_METATYPE(ShipParamsMap)

/**
 * @brief The SimulationServer class integrates a simulation server
 * that communicates via RabbitMQ.
 *
 * This class manages the lifecycle of simulation events, from
 * initialization to teardown, while interacting with RabbitMQ
 * for message handling.
 */
class SimulationServer : public QObject {
    Q_OBJECT

public:
    /**
     * @brief Constructs a SimulationServer with optional parent object.
     * @param parent The parent QObject which manages the lifetime of
     * this instance.
     */
    explicit SimulationServer(QObject *parent = nullptr);

    /**
     * @brief Destructor which cleans up resources, ensuring a proper
     * shutdown of the RabbitMQ connection.
     */
    ~SimulationServer();

    /**
     * @brief Initializes and starts the RabbitMQ server connection.
     * @param hostname The hostname of the RabbitMQ server.
     * @param port The port number on which the RabbitMQ server is running.
     * @param rabbitMQUsername The username for RabbitMQ authentication.
     * @param rabbitMQPassword The password for RabbitMQ authentication.
     */
    void startRabbitMQServer(const std::string &hostname,
                             const int port,
                             const std::string &rabbitMQUsername,
                             const std::string &rabbitMQPassword);

    /**
     * @brief Sends a JSON formatted message to RabbitMQ with a
     * specified routing key.
     * @param routingKey The routing key for the message, determining
     *        where the message should be routed within RabbitMQ.
     * @param message The JSON object to be sent as the message payload.
     */
    void sendRabbitMQMessage(const QString &routingKey,
                             const QJsonObject &message);

    /**
     * @brief Stops the RabbitMQ server and cleans up any connections
     * and resources associated with it.
     */
    void stopRabbitMQServer();  // stop RabbitMQ server cleanly

signals:
    /**
     * @brief Signal emitted when data is received from RabbitMQ.
     * @param message The received JSON object.
     */
    void dataReceived(QJsonObject message);

    /**
     * @brief Signal emitted when a ship reaches its destination.
     * @param shipID The identifier of the ship that reached the destination.
     */
    void shipReachedDestination(const QString &shipID);

    /**
     * @brief Signal emitted when simulation results are available.
     * @param results The simulation results wrapped in a ShipsResults
     * structure.
     */
    void simulationResultsAvailable(ShipsResults &results);

    /**
     * @brief Signal emitted to request stopping the message consuming loop.
     */
    void stopConsuming();

private slots:

    /**
     * @brief Handles received data from RabbitMQ and updates the
     * simulation state accordingly.
     * @param message The received JSON object containing the message.
     * @param envelope The AMQP envelope containing the message details.
     */
    void onDataReceivedFromRabbitMQ(QJsonObject &message,
                                    const amqp_envelope_t &envelope);

    /**
     * @brief Slot activated when the worker thread is ready to
     * resume processing.
     */
    void onWorkerReady();  // resume processing when the worker is ready

    // simulation events

    /**
     * @brief Emitted when a simulation network is fully loaded.
     * @param networkName The name of the network that was loaded.
     */
    void onSimulationNetworkLoaded(QString networkName);

    /**
     * @brief Emitted when a new simulation has been created.
     * @param networkName The name of the simulation network that was created.
     */
    void onSimulationCreated(QString networkName);

    /**
     * @brief Emitted when a simulation is paused.
     * @param networkNames A list of network names that were paused.
     */
    void onSimulationPaused(QVector<QString> networkNames);

    /**
     * @brief Emitted when a simulation is resumed.
     * @param networkNames A list of network names that were resumed.
     */
    void onSimulationResumed(QVector<QString> networkNames);

    /**
     * @brief Emitted when a simulation is restarted.
     * @param networkNames A list of network names that were restarted.
     */
    void onSimulationRestarted(QVector<QString> networkNames);

    /**
     * @brief Emitted when a simulation ends.
     * @param networkNames A list of network names where the simulation
     * has ended.
     */
    void onSimulationEnded(QVector<QString> networkNames);

    /**
     * @brief Emitted when the simulation time advances.
     * @param newSimulationTime A map from network names to their new
     * simulation times.
     */
    void onSimulationAdvanced(
        QMap<QString, units::time::second_t> newSimulationTime);

    /**
     * @brief Emitted to update the simulation progress in percentage.
     * @param progressPercentage The current progress of the simulation
     * in percentage.
     */
    void onSimulationProgressUpdate(int progressPercentage);

    /**
     * @brief Emitted when a new ship is added to a simulator.
     * @param networkName The network name where the ship is added.
     * @param shipIDs A list of IDs of ships that were added.
     */
    void onShipAddedToSimulator(const QString networkName,
                                const QVector<QString> shipIDs);

    /**
     * @brief Emitted when a ship reaches its destination.
     * @param shipStatus A JSON object containing the status of the ship.
     */
    void onShipReachedDestination(const QJsonObject shipStatus);

    /**
     * @brief Emitted when simulation results are available.
     * @param results A map from network names to their respective
     * simulation results.
     */
    void onSimulationResultsAvailable(QMap<QString, ShipsResults>& results);

    /**
     * @brief Emitted when the state of a ship is available.
     * @param shipState A JSON object containing the state of the ship.
     */
    void onShipStateAvailable(const QJsonObject shipState);

    /**
     * @brief Emitted when the state of the simulator is available.
     * @param simulatorState A JSON object containing the state of the
     * simulator.
     */
    void onSimulatorStateAvailable(const QJsonObject simulatorState);


    /**
     * @brief Emitted when containers are added to a ship.
     * @param networkName The network name of the ship.
     * @param shipID The ID of the ship to which containers are added.
     */
    void onContainersAddedToShip(QString networkName, QString shipID);
    void onShipReachedSeaPort(QString networkName, QString shipID,
                              QString seaPortCode, qsizetype containersCount);
    void onPortsAvailable(QMap<QString,
                               QVector<QString>> networkPorts);
    void onContainersUnloaded(QString networkName,
                              QString shipID,
                              QString seaPortName,
                              QJsonArray containers);

    /**
     * @brief Emitted when an error occurs.
     * @param errorMessage The error message describing what went wrong.
     */
    void onErrorOccurred(const QString& errorMessage);
    void onServerReset();

private:
    ///< Hostname of the RabbitMQ server
    std::string mHostname;

    ///< Port number of the RabbitMQ server
    int mPort;

    ///< Username for RabbitMQ authentication
    std::string mRabbitMQUsername;

    ///< Password for RabbitMQ authentication
    std::string mRabbitMQPassword;

    ///< Mutex for protecting access to mWorkerBusy
    QMutex mMutex;

    ///< To control the server run loop
    bool mWorkerBusy;

    ///< Thread handle for the RabbitMQ connection
    QThread *mRabbitMQThread = nullptr;

    ///< Condition variable for pausing/resuming the worker thread
    QWaitCondition mWaitCondition;

    ///< RabbitMQ connection state handle
    amqp_connection_state_t mRabbitMQConnection;

    /**
     * @brief Processes command received via RabbitMQ and executes
     * corresponding actions.
     * @param jsonMessage The JSON object containing the command
     * and associated data.
     */
    void processCommand(QJsonObject &jsonMessage);

    /**
     * @brief Consumes messages from RabbitMQ in a dedicated thread.
     */
    void consumeFromRabbitMQ();

    /**
     * @brief Starts the message consuming process.
     */
    void startConsumingMessages();

    /**
     * @brief Attempts to reconnect to RabbitMQ if the connection is lost.
     */
    void reconnectToRabbitMQ();
    void setupServer();

};

#endif // SIMULATIONSERVER_H
