#include "BootNormal.hpp"

using namespace HomieInternals;

BootNormal::BootNormal(SharedInterface* sharedInterface)
: Boot(sharedInterface, "normal")
, _sharedInterface(sharedInterface)
, _lastWifiReconnectAttempt(0)
, _lastMqttReconnectAttempt(0)
, _lastSignalSent(0)
, _wifiConnectNotified(false)
, _wifiDisconnectNotified(false)
, _mqttConnectNotified(false)
, _mqttDisconnectNotified(false)
, _flaggedForOta(false)
, _flaggedForReset(false)
{
  if (Config.get().mqtt.ssl) {
    this->_sharedInterface->mqtt = new PubSubClient(this->_wifiClientSecure);
  } else {
    this->_sharedInterface->mqtt = new PubSubClient(this->_wifiClient);
  }

  strcpy(this->_mqttBaseTopic, "devices/");
  strcat(this->_mqttBaseTopic, Helpers.getDeviceId());
}

BootNormal::~BootNormal() {
  delete this->_sharedInterface->mqtt;
}

void BootNormal::_wifiConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(Config.get().wifi.ssid, Config.get().wifi.password);
}

void BootNormal::_mqttConnect() {
  this->_sharedInterface->mqtt->setServer(Config.get().mqtt.host, Config.get().mqtt.port);
  this->_sharedInterface->mqtt->setCallback(std::bind(&BootNormal::_mqttCallback, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
  char topic[24 + 1];
  strcpy(topic, this->_mqttBaseTopic);
  strcat(topic, "/$online");

  char client_id[CONFIG_MAX_LENGTH_WIFI_SSID] = "";
  strcat(client_id, this->_sharedInterface->brand);
  strcat(client_id, "-");
  strcat(client_id, Helpers.getDeviceId());

  bool connectResult;
  if (Config.get().mqtt.auth) {
    connectResult = this->_sharedInterface->mqtt->connect(client_id, Config.get().mqtt.username, Config.get().mqtt.password, topic, 2, true, "false");
  } else {
    connectResult = this->_sharedInterface->mqtt->connect(client_id, topic, 2, true, "false");
  }

  if (connectResult) {
    if (Config.get().mqtt.ssl && !strcmp(Config.get().mqtt.fingerprint, "")) {
      if(!this->_wifiClientSecure.verify(Config.get().mqtt.fingerprint, Config.get().mqtt.host)) {
        Logger.logln("✖ MQTT SSL certificate mismatch");
        this->_sharedInterface->mqtt->disconnect();
        return;
      }
    }

    this->_mqttSetup();
  }
}

void BootNormal::_mqttSetup() {
  Logger.logln("Sending initial informations");

  char topic[27 + 1];
  strcpy(topic, this->_mqttBaseTopic);
  strcat(topic, "/$nodes");

  int nodesLength = 0;
  for (int i = 0; i < this->_sharedInterface->registeredNodes.size(); i++) {
    HomieNode node = this->_sharedInterface->registeredNodes[i];
    nodesLength += strlen(node.id);
    nodesLength += 1; // :
    nodesLength += strlen(node.type);
    nodesLength += 1; // ,
  }
  nodesLength += 1; // Leading \0

  String nodes = String();
  nodes.reserve(nodesLength);
  for (int i = 0; i < this->_sharedInterface->registeredNodes.size(); i++) {
    HomieNode node = this->_sharedInterface->registeredNodes[i];
    nodes += node.id;
    nodes += ":";
    nodes += node.type;
    nodes += ",";
  }
  nodes.remove(nodes.length() - 1, 1); // Remove last ,
  this->_sharedInterface->mqtt->publish(topic, nodes.c_str(), true);

  strcpy(topic, this->_mqttBaseTopic);
  strcat(topic, "/$online");
  this->_sharedInterface->mqtt->publish(topic, "true", true);

  strcpy(topic, this->_mqttBaseTopic);
  strcat(topic, "/$name");
  this->_sharedInterface->mqtt->publish(topic, Config.get().name, true);

  strcpy(topic, this->_mqttBaseTopic);
  strcat(topic, "/$localip");
  IPAddress local_ip = WiFi.localIP();
  char local_ip_str[15 + 1];
  char local_ip_part_str[3 + 1];
  itoa(local_ip[0], local_ip_part_str, 10);
  strcpy(local_ip_str, local_ip_part_str);
  strcat(local_ip_str, ".");
  itoa(local_ip[1], local_ip_part_str, 10);
  strcat(local_ip_str, local_ip_part_str);
  strcat(local_ip_str, ".");
  itoa(local_ip[2], local_ip_part_str, 10);
  strcat(local_ip_str, local_ip_part_str);
  strcat(local_ip_str, ".");
  itoa(local_ip[3], local_ip_part_str, 10);
  strcat(local_ip_str, local_ip_part_str);
  this->_sharedInterface->mqtt->publish(topic, local_ip_str, true);

  strcpy(topic, this->_mqttBaseTopic);
  strcat(topic, "/$fwname");
  this->_sharedInterface->mqtt->publish(topic, this->_sharedInterface->firmware.name, true);

  strcpy(topic, this->_mqttBaseTopic);
  strcat(topic, "/$fwversion");
  this->_sharedInterface->mqtt->publish(topic, this->_sharedInterface->firmware.version, true);

  strcpy(topic, this->_mqttBaseTopic);
  strcat(topic, "/$reset");
  this->_sharedInterface->mqtt->subscribe(topic, 1);

  if (Config.get().ota.enabled) {
    strcpy(topic, this->_mqttBaseTopic);
    strcat(topic, "/$ota");
    this->_sharedInterface->mqtt->subscribe(topic, 1);
  }

  for (int i = 0; i < this->_sharedInterface->registeredNodes.size(); i++) {
    HomieNode node = this->_sharedInterface->registeredNodes[i];
    for (int i = 0; i < node.subscriptions.size(); i++) {
      Subscription subscription = node.subscriptions[i];

      String dynamic_topic;
      dynamic_topic.reserve(strlen(this->_mqttBaseTopic) + 1 + strlen(node.id) + 1 + strlen(subscription.property) + 4 + 1);
      dynamic_topic = this->_mqttBaseTopic;
      dynamic_topic += "/";
      dynamic_topic += node.id;
      dynamic_topic += "/";
      dynamic_topic += subscription.property;
      dynamic_topic += "/set";
      this->_sharedInterface->mqtt->subscribe(dynamic_topic.c_str(), 1);
      this->_sharedInterface->mqtt->loop(); // see knolleary/pubsublient#98
    }
  }
}

void BootNormal::_mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  message.reserve(length);
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  String unified = String(topic);
  unified.remove(0, strlen(this->_mqttBaseTopic) + 1); // Remove /devices/${id}/ - +1 for /
  if (Config.get().ota.enabled && unified == "$ota") {
    if (message != this->_sharedInterface->firmware.version) {
      Logger.log("✴ OTA available (version ");
      Logger.log(message);
      Logger.logln(")");
      this->_flaggedForOta = true;
      Serial.println("Flagged for OTA");
    }
    return;
  } else if (unified == "$reset" && message == "true") {
    this->_sharedInterface->mqtt->publish(topic, "false", true);
    this->_flaggedForReset = true;
    Logger.logln("Flagged for reset by network");
    return;
  }
  unified.remove(unified.length() - 4, 4); // Remove /set
  int separator;
  for (int i = 0; i < unified.length(); i++) {
    if (unified.charAt(i) == '/') {
      separator = i;
    }
  }
  String node = unified.substring(0, separator);
  String property = unified.substring(separator + 1);

  bool handled = this->_sharedInterface->inputHandler(node, property, message);
  if (handled) { return; }

  int homieNodeIndex;
  bool homieNodeFound = false;
  for (int i = 0; i < this->_sharedInterface->registeredNodes.size(); i++) {
    HomieNode homieNode = this->_sharedInterface->registeredNodes[i];
    if (node == homieNode.id) {
      homieNodeFound = true;
      homieNodeIndex = i;
      handled = homieNode.inputHandler(property, message);
      break;
    }
  }

  if (!homieNodeFound) { return; }
  if (handled) { return; }

  HomieNode homieNode = this->_sharedInterface->registeredNodes[homieNodeIndex];

  for (int i = 0; i < homieNode.subscriptions.size(); i++) {
    Subscription subscription = homieNode.subscriptions[i];
    if (property == subscription.property) {
      handled = subscription.inputHandler(message);
      break;
    }
  }

  if (!handled) {
    Logger.logln("No handlers handled the following message:");
    Logger.log("  • Node ID: ");
    Logger.logln(node);
    Logger.log("  • Property: ");
    Logger.logln(property);
    Logger.log("  • Message: ");
    Logger.logln(message);
  }
}

void BootNormal::_handleReset() {
  if (this->_sharedInterface->resetTriggerEnabled) {
    this->_resetDebouncer.update();

    if (this->_resetDebouncer.read() == this->_sharedInterface->resetTriggerState) {
      this->_flaggedForReset = true;
      Logger.logln("Flagged for reset by pin");
    }
  }

  if (this->_sharedInterface->resetFunction()) {
    this->_flaggedForReset = true;
    Logger.logln("Flagged for reset by function");
  }
}

void BootNormal::setup() {
  Boot::setup();

  if (this->_sharedInterface->resetTriggerEnabled) {
    pinMode(this->_sharedInterface->resetTriggerPin, INPUT_PULLUP);

    this->_resetDebouncer.attach(this->_sharedInterface->resetTriggerPin);
    this->_resetDebouncer.interval(this->_sharedInterface->resetTriggerTime);

    this->_sharedInterface->setupFunction();
  }

  Config.log();
}

void BootNormal::loop() {
  Boot::loop();

  this->_handleReset();

  if (this->_flaggedForReset && this->_sharedInterface->resettable) {
    Logger.logln("Device is in a resettable state");
    Config.erase();
    Logger.logln("Configuration erased");

    this->_sharedInterface->eventHandler(HOMIE_ABOUT_TO_RESET);

    Logger.logln("↻ Rebooting in config mode");
    ESP.restart();
  }

  if (this->_flaggedForOta && this->_sharedInterface->resettable) {
    Logger.logln("Device is in a resettable state");
    Config.setOtaMode(true);

    Logger.logln("↻ Rebooting in OTA mode");
    ESP.restart();
  }

  this->_sharedInterface->readyToOperate = false;

  if (WiFi.status() != WL_CONNECTED) {
    this->_wifiConnectNotified = false;
    if (!this->_wifiDisconnectNotified) {
      this->_lastWifiReconnectAttempt = 0;
      this->_sharedInterface->eventHandler(HOMIE_WIFI_DISCONNECTED);
      this->_wifiDisconnectNotified = true;
    }

    unsigned long now = millis();
    if (now - this->_lastWifiReconnectAttempt >= WIFI_RECONNECT_INTERVAL || this->_lastWifiReconnectAttempt == 0) {
      Logger.logln("⌔ Attempting to connect to Wi-Fi");
      this->_lastWifiReconnectAttempt = now;
      if (this->_sharedInterface->useBuiltInLed) {
        Blinker.start(LED_WIFI_DELAY);
      }
      this->_wifiConnect();
    }
    return;
  }

  this->_wifiDisconnectNotified = false;
  if (!this->_wifiConnectNotified) {
    this->_sharedInterface->eventHandler(HOMIE_WIFI_CONNECTED);
    this->_wifiConnectNotified = true;
  }

  if (!this->_sharedInterface->mqtt->connected()) {
    this->_mqttConnectNotified = false;
    if (!this->_mqttDisconnectNotified) {
      this->_lastMqttReconnectAttempt = 0;
      this->_sharedInterface->eventHandler(HOMIE_MQTT_DISCONNECTED);
      this->_mqttDisconnectNotified = true;
    }

    unsigned long now = millis();
    if (now - this->_lastMqttReconnectAttempt >= MQTT_RECONNECT_INTERVAL || this->_lastMqttReconnectAttempt == 0) {
      Logger.logln("⌔ Attempting to connect to MQTT");
      this->_lastMqttReconnectAttempt = now;
      if (this->_sharedInterface->useBuiltInLed) {
        Blinker.start(LED_MQTT_DELAY);
      }
      this->_mqttConnect();
    }
    return;
  } else {
    if (this->_sharedInterface->useBuiltInLed) {
      Blinker.stop();
    }
  }

  this->_mqttDisconnectNotified = false;
  if (!this->_mqttConnectNotified) {
    this->_sharedInterface->eventHandler(HOMIE_MQTT_CONNECTED);
    this->_mqttConnectNotified = true;
  }

  unsigned long now = millis();
  if (now - this->_lastSignalSent >= SIGNAL_QUALITY_SEND_INTERVAL || this->_lastSignalSent == 0) {
    Logger.logln("Sending Wi-Fi signal quality");
    int32_t rssi = WiFi.RSSI();
    byte quality;
    if (rssi <= -100) {
      quality = 0;
    } else if (rssi >= -50) {
      quality = 100;
    } else {
      quality = 2 * (rssi + 100);
    }

    char quality_str[3 + 1];
    itoa(quality, quality_str, 10);

    char topic[24 + 1];
    strcpy(topic, this->_mqttBaseTopic);
    strcat(topic, "/$signal");

    if (this->_sharedInterface->mqtt->publish(topic, quality_str, true)) {
      this->_lastSignalSent = now;
    }
  }

  this->_sharedInterface->readyToOperate = true;
  this->_sharedInterface->loopFunction();

  this->_sharedInterface->mqtt->loop();
}
