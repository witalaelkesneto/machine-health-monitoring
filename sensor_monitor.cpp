#include <iostream>
#include <cstdlib>
#include <chrono>
#include <ctime>
#include <thread>
#include <unistd.h>
#include "json.hpp"      // json handling
#include "mqtt/client.h" // paho mqtt
#include <iomanip>
#include "sys/types.h"
#include "sys/sysinfo.h"
#include <fstream>
#include <string>
#include <sstream>
#include <vector>

#define QOS 1
#define BROKER_ADDRESS "tcp://localhost:1883"
#define TOPIC_SENSOR_MONITOR "/sensor_monitors"

using json = nlohmann::json;

struct Sensor
{
    std::string id;
    std::string type;
    long interval;
};

int getRAM()
{
    struct sysinfo memInfo;
    sysinfo(&memInfo);
    int usedRAM = memInfo.totalram - memInfo.freeram;

    usedRAM *= memInfo.mem_unit;
    return usedRAM;
}

double getCPU()
{
    std::ifstream file("/proc/stat");
    std::string line;
    getline(file, line);
    file.close();

    std::istringstream ss(line);
    std::string cpu;
    long user, nice, system, idle;
    ss >> cpu >> user >> nice >> system >> idle;

    static long prevTotal = 0, prevIdle = 0;
    long total = user + nice + system + idle;
    long idleDelta = idle - prevIdle;
    long totalDelta = total - prevTotal;

    double cpuUsage = (1.0 - static_cast<double>(idleDelta) / totalDelta) * 100.0;

    prevTotal = total;
    prevIdle = idle;

    return cpuUsage;
}

void initialMessage(const std::string &machine_id, const std::vector<Sensor> &sensors, mqtt::client &client)
{
    json message;
    message["machine_id"] = machine_id;
    std::vector<json> jsonSensors;
    for (auto &sensor : sensors)
    {
        json jsonSensor;
        jsonSensor["sensor_id"] = sensor.id;
        jsonSensor["data_type"] = sensor.type;
        jsonSensor["data_interval"] = sensor.interval;
        jsonSensors.push_back(jsonSensor);
    }
    message["sensors"] = jsonSensors;
    mqtt::message initialMessage(TOPIC_SENSOR_MONITOR, message.dump(), QOS, false);
    client.publish(initialMessage);
}

void periodicalMessage(const std::string &machine_id, const std::vector<Sensor> &sensors, mqtt::client &client, int interval)
{
    while (true)
    {
        initialMessage(machine_id, sensors, client);
        std::this_thread::sleep_for(std::chrono::seconds(interval));
    }
}

void sensorData(const std::string &machine_id, const Sensor &sensor, mqtt::client &client)
{
    std::string topic = "/sensors/" + machine_id + "/" + sensor.id;

    while (true)
    {
        int value;

        if (sensor.id == "ram")
        {
            value = getRAM();
        }
        if (sensor.id == "cpu")
        {
            value = getCPU();
        }
        auto now = std::chrono::system_clock::now();
        std::time_t now_c = std::chrono::system_clock::to_time_t(now);
        std::tm *now_tm = std::localtime(&now_c);
        std::stringstream ss;
        ss << std::put_time(now_tm, "%FT%TZ");
        std::string timestamp = ss.str();

        json jsonData;
        jsonData["timestamp"] = timestamp;
        jsonData["value"] = value;

        mqtt::message message(topic, jsonData.dump(), QOS, false);
        client.publish(message);

        std::this_thread::sleep_for(std::chrono::milliseconds(sensor.interval));
    }
}

int main(int argc, char *argv[])
{
    std::string clientId = "sensor-monitor";
    mqtt::client client(BROKER_ADDRESS, clientId);

    mqtt::connect_options connOpts;
    connOpts.set_keep_alive_interval(20);
    connOpts.set_clean_session(true);

    try
    {
        client.connect(connOpts);
    }
    catch (mqtt::exception &e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    std::clog << "connected to the broker" << std::endl;

    char hostname[1024];
    gethostname(hostname, 1024);
    std::string machineId(hostname);

    std::vector<Sensor> sensors;
    sensors.push_back(Sensor{"cpu", "int", std::atoi(argv[2])});
    sensors.push_back(Sensor{"ram", "int", std::atoi(argv[2])});

    std::thread t1(periodicalMessage, machineId, sensors, client, argv[1]);

    std::vector<std::thread> threads;
    for(auto &sensor : sensors){
        threads.emplace_back(sensorData, machineId, sensor, client);
    }

    for (auto &thread : threads){
        thread.join();
    }

    return 0;
}
