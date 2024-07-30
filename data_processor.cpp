#include <iostream>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <unistd.h>
#include "json.hpp"
#include "mqtt/client.h"
#include <boost/asio.hpp>
#include <string>
#include <map>

#define QOS 1
#define BROKER_ADDRESS "tcp://localhost:1883"
#define GRAPHITE_HOST "graphite"
#define GRAPHITE_PORT 2003

std::string timestampUnix(const std::string &timestamp)
{
    std::tm u = {};
    std::istringstream ss(timestamp);
    ss >> std::get_time(&u, "%Y-%m-%dT%H:%M:%S");
    return std::to_string(mktime(&u));
}

std::string unixTimestamp(std::time_t u)
{
    std::tm *timestamp = std::localtime(&u);
    char buffer[32];
    strftime(buffer, 32, "%Y-%m-%dT%H:%M:%S", timestamp);
    return std::string(buffer);
}

void connectDatabase(const std::string &metric)
{
    boost::asio::io_context io_context;
    boost::asio::ip::tcp::socket socket(io_context);
    boost::asio::ip::tcp::resolver resolver(io_context);
    boost::asio::connect(socket, resolver.resolve(GRAPHITE_HOST, std::to_string(GRAPHITE_PORT)));
    boost::asio::write(socket, boost::asio::buffer(metric));
}

void post_metric(const std::string &machine_id, const std::string &sensor_id, const std::string &timestamp_str, const int value)
{
    std::string metricPath = machine_id + "." + sensor_id;
    std::string metric = metricPath + " " + std::to_string(value) + " " + timestampUnix(timestamp_str) + "\n";
    connectDatabase(metric);
}

std::vector<std::string> split(const std::string &str, char delim)
{
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(str);
    while (std::getline(tokenStream, token, delim))
    {
        tokens.push_back(token);
    }
    return tokens;
}

std::map<std::pair<std::string, std::string>, std::chrono::steady_clock::time_point> lastRegisters;
void updateRegister(const std::string &machine_id, const std::string &sensor_id)
{
    lastRegisters[{machine_id, sensor_id}] = std::chrono::steady_clock::now();
}

std::chrono::steady_clock::time_point initial = std::chrono::steady_clock::now();

void alarmInactivity()
{
    time_t now = time(nullptr);
    std::chrono::steady_clock::time_point final = std::chrono::steady_clock::now();
    std::chrono::duration<double> duration = final - initial;
    initial = final;
    double frequency = duration.count() * 10;
    for (const auto &reg : lastRegisters)
    {
        auto key = reg.first;
        auto value = reg.second;
        std::string machine_id = key.first;
        std::string sensor_id = key.second;
        auto duration2 = std::chrono::steady_clock::now() - value;
        auto durationSeconds = std::chrono::duration_cast<std::chrono::seconds>(duration2).count();
        if(durationSeconds > frequency){
            post_metric(machine_id, ".alarms.inactive" + sensor_id, unixTimestamp(now), 1);
        }  
    }
}

int main(int argc, char *argv[])
{
    std::string clientId = "clientId";
    mqtt::async_client client(BROKER_ADDRESS, clientId);

    // Create an MQTT callback.
    class callback : public virtual mqtt::callback
    {
    public:
        void message_arrived(mqtt::const_message_ptr msg) override
        {
            auto j = nlohmann::json::parse(msg->get_payload());

            std::string topic = msg->get_topic();
            auto topic_parts = split(topic, '/');
            std::string machine_id = topic_parts[2];
            std::string sensor_id = topic_parts[3];
            std::string timestamp = j["timestamp"];
            int value = j["value"];
            post_metric(machine_id, sensor_id, timestamp, value);
        }
    };

    callback cb;
    client.set_callback(cb);

    // Connect to the MQTT broker.
    mqtt::connect_options connOpts;
    connOpts.set_keep_alive_interval(20);
    connOpts.set_clean_session(true);

    try
    {
        client.connect(connOpts);
        client.subscribe("/sensors/#", QOS);
    }
    catch (mqtt::exception &e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    while (true)
    {
        alarmInactivity();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return EXIT_SUCCESS;
}
