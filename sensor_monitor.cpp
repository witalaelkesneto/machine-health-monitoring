#include <iostream>
#include <cstdlib>
#include <chrono>
#include <ctime>
#include <thread>
#include <unistd.h>
#include "json.hpp"      // json handling
#include "mqtt/client.h" // paho mqtt
#include <iomanip>
#include <fstream>
#include <string>
#include <sstream>
#include <vector>
#include <curl/curl.h>

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

size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

json callAPI(){
    CURL* curl;
    CURLcode res;
    std::string readBuffer;
    
    curl = curl_easy_init();
    if(curl) {
        std::string url = "http://api.openweathermap.org/data/2.5/weather?lat=-19.9034387508784&lon=-43.92571570405504&appid=cd7a6eb0ae1ae79f393f3f245e09df51&units=metric";
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        res = curl_easy_perform(curl);
        if(res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        }
        curl_easy_cleanup(curl);
    }

    return nlohmann::json::parse(readBuffer);
}

int getHumidity()
{
    json data = callAPI();
    int humidity = data["main"]["humidity"];
    
    return humidity;
}

double getTemperature()
{
    json data = callAPI();
    double temp = data["main"]["temp"];
    
    return temp;
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
    std::cout << "Initial Message: " + message.dump() << std::endl;
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
        double value = 0;

        if (sensor.id == "humidity")
        {
            value = getHumidity();
        }
        if (sensor.id == "temperature")
        {
            value = getTemperature();
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

        std::cout << "Data Message: " + jsonData.dump() << std::endl;

        std::this_thread::sleep_for(std::chrono::milliseconds(sensor.interval));
    }
}

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        std::cerr << "Usage: " << argv[0] << " <interval> <sensor_interval>" << std::endl;
        return EXIT_FAILURE;
    }

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
    sensors.push_back(Sensor{"humidity", "int", std::atoi(argv[2])});
    sensors.push_back(Sensor{"temperature", "double", std::atoi(argv[2])});

    std::thread t1(periodicalMessage, machineId, sensors, std::ref(client), std::atoi(argv[1]));

    std::vector<std::thread> threads;
    for (auto &sensor : sensors)
    {
        threads.emplace_back(sensorData, machineId, sensor, std::ref(client));
    }

    for (auto &thread : threads)
    {
        thread.join();
    }

    return 0;
}