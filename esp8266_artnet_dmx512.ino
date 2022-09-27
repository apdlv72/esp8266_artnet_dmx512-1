/*
  This sketch receives Art-Net data of one DMX universes over WiFi
  and sends it over the serial interface to a MAX485 module.

  It provides an interface between wireless Art-Net and wired DMX512.

*/

// Comment in to enable standalone mode. This means that the setup function won't
// block until the device was configured to connect to a Wifi network but will start
// to receive Artnet data right away on the access point network the WifiManager
// created for this purpose. You can then simply ignore the configuration attempt and
// use the device without a local Wifi network or choose to connect one later.
// Consider setting also a password in standalonemode, otherwise someone else might
// configure your device to connect to a random Wifi.
#define ENABLE_STANDALONE
//#define STANDALONE_PASSWORD "secretsecret"

// Uncomment to send DMX data using the microcontroller's builtin UART.
// This is the original way this sketch used to work and expects the max485 level
// shifter to be connected to the pin that corresondents to Serial1.
// On a Wemos D1 this is e.g. pin D4.
#define ENABLE_UART

// Uncomment to send DMX data via I2S instead of UART.
// I2S allows for better control of number of stop bits and DMX timing to meet the
// requiremeents of sloppy devices. Moreover using DMA reduces strain of the CPU and avoids 
// issues with background activity such as handling WiFi, interrupts etc.
// However - because of the extra timing/pauses for sloppy device, sending DMX over I2S
// will cause throughput to drop from approx 40 packets/s to around 30.
#define ENABLE_I2S

// Enable kind of unit test for new I2S code moving around a knowingly picky device
// (china brand moving head with timing issues)
//#define WITH_TEST_CODE

// Enable OTA (over the air programming in the Arduino GUI, not via the web server)
//#define ENABLE_ARDUINO_OTA
//#define ARDUINO_OTA_PASSWORD "secret"

#include <ESP8266WiFi.h>         // https://github.com/esp8266/Arduino
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiManager.h>         // https://github.com/tzapu/WiFiManager
#include <WiFiClient.h>
#include <ArtnetWifi.h>          // https://github.com/rstephan/ArtnetWifi
#include <FS.h>
#ifdef ENABLE_ARDUINO_OTA
#include <ArduinoOTA.h>
#endif
#ifdef ENABLE_I2S
#include <i2s.h>
#include <i2s_reg.h>
#include "i2s_dmx.h"
#endif

#include "setup_ota.h"
#include "send_break.h"

#define MIN(x,y) (x<y ? x : y)
#define ENABLE_MDNS
#define ENABLE_WEBINTERFACE
// #define COMMON_ANODE

Config config;
ESP8266WebServer server(80);
const char* host = "ARTNET";
const char* version = __DATE__ " / " __TIME__;

#define LED_B 16  // GPIO16/D0
#define LED_G 5   // GPIO05/D1
#define LED_R 4   // GPIO04/D2

#ifdef ENABLE_I2S
#define I2S_PIN 3
#endif

// Artnet settings
ArtnetWifi artnet;
WiFiManager wifiManager;

unsigned long packetCounter = 0, frameCounter = 0, last_packet_received = 0;
float fps = 0;

// Keep track if we already started OTA 
bool arduinoOtaStarted = false;

// Global universe buffer
struct {
  uint16_t universe;
  uint16_t length;
  uint8_t sequence;
  // when using I2S, channel values are stored in i2s_data
#ifdef ENABLE_UART
  uint8_t *data;
#endif
#ifdef ENABLE_I2S
  i2s_packet i2s_data;
  uint16_t   i2s_length;
#endif
} global;
#define WITH_WIFI

// keep track of the timing of the function calls
long tic_loop = 0, tic_fps = 0, tic_packet = 0, tic_web = 0;

#ifdef ENABLE_UART
long tic_uart = 0;
unsigned long uartCounter;
#endif

#ifdef ENABLE_I2S
long tic_i2s = 0;
unsigned long i2sCounter;
#endif

void printBits(byte b) {    
    for (int j = 7; j >= 0; j--) {
        byte bit = (b >> j) & 1;
        Serial.printf("%u", bit);
    }
}

// This will be called for each UDP packet received
void onDmxPacket(uint16_t universe, uint16_t length, uint8_t sequence, uint8_t * data) {

  unsigned long now = millis();
  if (now-last_packet_received > 1000) {
    // print immediate feedback at start of stream
    Serial.print("DMX stream started\n");
  }
  last_packet_received = now;

  // print some feedback once per second
  if ((millis() - tic_fps) > 1000 && frameCounter > 100) {
    Serial.print("packetCounter = ");
    Serial.print(packetCounter++);
    // don't estimate the FPS too frequently
    fps = 1000 * frameCounter / (millis() - tic_fps);
    tic_fps = last_packet_received;
    frameCounter = 0;
    Serial.print(", FPS = ");      Serial.print(fps);
    // print out also some diagnostic info
    Serial.print(", length = ");   Serial.print(length);
    Serial.print(", sequence = "); Serial.print(sequence);
    Serial.print(", universe = "); Serial.print(universe);
    Serial.print(", config.universe = "); Serial.print(universe);
    Serial.println();
  }

  if (universe == config.universe) {

#ifdef ENABLE_I2S
    // TODO optimize i2s such that it can send less than 512 bytes if not required (length<512)
    // we are sending I2S _frames_ (not bytes) and every frame consists of 2 words,
    // so we must ensure an even number of DMX values where every walue is a word
    int even_length = DMX_CHANNELS;
    /* 
     The code below does not work for me. 
     Do not activate before thoroughly testing & debugging with arbitrary DMX sizes for your setup.
     It seems to me like some device do not expect any other DMX size than 512.
     
    even_length = 2 * (length + 1) / 2;
    if (even_length > DMX_CHANNELS) {
      even_length = DMX_CHANNELS;
    }
    int skipped_bytes = 2 * (DMX_CHANNELS - even_length); // divisible by 4
    global.i2s_length = sizeof(global.i2s_data) - skipped_bytes;
    Serial.printf("onDmxPacket: length=%d, even_length=%d, skipped_bytes=%d, i2s_length=%d, sizeof(i2s_data)=%d\n",
                  length, even_length, skipped_bytes, global.i2s_length, sizeof(global.i2s_data));    
    */    
    for (int i = 0; i < even_length; i++) {
      uint16_t hi = i < length ? flipByte(data[i]) : 0;
      // Add stop bits and start bit of next byte unless there
      // is no next byte because the current is the last one.
      uint16_t lo = i == (even_length-1) ? 0b0000000011111111 : 0b0000000011111110;
      // Leave the start-byte (index 0) untouched => +1:
      global.i2s_data.dmx_bytes[i + 1] = (hi << 8) | lo;
    }
#endif

#ifdef ENABLE_UART
    // copy the data from the UDP packet over to the global universe buffer
    global.universe = universe;
    global.sequence = sequence;
    if (length < 512)
      global.length = length;
    for (int i = 0; i < global.length; i++) {
      global.data[i] = data[i];
    }
#endif
  }
} // onDmxpacket

void setup() {

#ifdef ENABLE_UART
  Serial1.begin(250000, SERIAL_8N2);
#endif
  Serial.begin(115200);
  while (!Serial) {
    ;
  }
  Serial.println("setup starting");

  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);

  global.universe = 0;
  global.sequence = 0;
  global.length = 512;
#ifdef ENABLE_UART
  global.data = (uint8_t *)malloc(512);
  for (int i = 0; i < 512; i++)
    global.data[i] = 0;
#endif
#ifdef ENABLE_I2S
  logI2SInfo();
  // Must NOT be called before Serial.begin I2S output is on RX0 pin which would
  // be reset to input mode for serial data rather than output for I2S data.
  initI2S();
#endif

  SPIFFS.begin();

  Serial.println("Loading configuration");
  initialConfig();

  if (loadConfig()) {
    singleYellow();
    delay(1000);
  }
  else {
    singleRed();
    delay(1000);
  }

  if (WiFi.status() != WL_CONNECTED)
    singleRed();

  //wifiManager.resetSettings();
#ifdef ENABLE_STANDALONE
  Serial.println("Starting WiFiManager (non-blocking mode)");
  wifiManager.setConfigPortalBlocking(false);
#else
  Serial.println("Starting WiFiManager (blocking mode)");
#endif
  WiFi.hostname(host);
  wifiManager.setAPStaticIPConfig(IPAddress(192, 168, 1, 1), IPAddress(192, 168, 1, 1), IPAddress(255, 255, 255, 0));
#ifdef STANDALONE_PASSWORD
  wifiManager.autoConnect(host, STANDALONE_PASSWORD);
#else
  wifiManager.autoConnect(host);
#endif  
  Serial.println("connected");

  if (WiFi.status() == WL_CONNECTED)
    singleGreen();

#ifdef ENABLE_ARDUINO_OTA
  initOTA();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Starting Arduino OTA (setup)");
    ArduinoOTA.begin();
    arduinoOtaStarted = true;
  }
#endif
#ifdef ENABLE_WEBINTERFACE
  initServer();
#endif

  // announce the hostname and web server through zeroconf
#ifdef ENABLE_MDNS
  MDNS.begin(host);
  MDNS.addService("http", "tcp", 80);
#endif

  artnet.begin();
  artnet.setArtDmxCallback(onDmxPacket);

  // initialize all timers
  tic_loop   = millis();
  tic_packet = millis();
  tic_fps    = millis();
  tic_web    = 0;

#ifdef ENABLE_UART
  tic_uart    = 0;
#endif
#ifdef ENABLE_I2S
  tic_i2s    = 0;
#endif

  Serial.println("setup done");
} // setup

void loop() {
  // handle those servers only when not receiving DMX data
  long now = millis();
  
  if (now - last_packet_received > 1000) {
    wifiManager.process();
#ifdef ENABLE_ARDUINO_OTA
    if (WiFi.status() == WL_CONNECTED && !arduinoOtaStarted) {
      Serial.println("Starting Arduino OTA (loop)");
      ArduinoOTA.begin();
      arduinoOtaStarted = true;
    }
    ArduinoOTA.handle();
#endif    
  }
  server.handleClient();

  if (WiFi.status() != WL_CONNECTED) {
    singleRed();
    delay(10);
#ifndef ENABLE_STANDALONE
    return;
#endif
  }

  if ((millis() - tic_web) < 5000) {
    singleBlue();
    delay(25);
  }
  else  {
    singleGreen();
    artnet.read();

    // this section gets executed at a maximum rate of around 40Hz
    if ((millis() - tic_loop) > config.delay) {
      tic_loop = millis();
      frameCounter++;

#ifdef ENABLE_UART
      outputSerial();
#endif

#ifdef ENABLE_I2S
      outputI2S();
#endif
    }
  }

#ifdef WITH_TEST_CODE
  testCode();
#endif

  //delay(1);
} // loop

#ifdef ENABLE_UART

void outputSerial() {

  sendBreak();

  Serial1.write(0); // Start-Byte
  // send out the value of the selected channels (up to 512)
  for (int i = 0; i < MIN(global.length, config.channels); i++) {
    Serial1.write(global.data[i]);
  }

  uartCounter++;
  long now = millis();
  if ((now - tic_uart) > 1000 && uartCounter > 100) {
    // don't estimate the FPS too frequently
    float pps = (1000.0 * uartCounter) / (now - tic_uart);
    tic_uart = now;
    uartCounter = 0;
    Serial.printf("UART: %.1f p/s\n", pps);
  }
}

#endif // ENABLE_UART

#ifdef ENABLE_I2S

void initI2S() {
  pinMode(I2S_PIN, OUTPUT); // Override default Serial initiation
  digitalWrite(I2S_PIN, 1); // Set pin high

  memset(&(global.i2s_data), 0x00, sizeof(global.i2s_data));
  memset(&(global.i2s_data.mark_before_break), 0xff, sizeof(global.i2s_data.mark_before_break));

  // 3 bits (12us) MAB. The MAB's LSB 0 acts as the start bit (low) for the null byte
  global.i2s_data.mark_after_break = (uint16_t) 0b000001110;

  // Set LSB to 0 for every byte to act as the start bit of the following byte.
  // Sending 7 stop bits in stead of 2 will please slow/buggy hardware and act
  // as the mark time between slots.
  for (int i = 0; i < DMX_CHANNELS; i++) {
    global.i2s_data.dmx_bytes[i] = (uint16_t) 0b0000000011111110;
  }
  // Set MSB NOT to 0 for the last byte because MBB (mark for break will follow)
  global.i2s_data.dmx_bytes[DMX_CHANNELS] = (uint16_t) 0b0000000011111111;
  global.i2s_length = sizeof(global.i2s_data);

  i2s_begin();
  // 250.000 baud / 32 bits = 7812
  i2s_set_rate(7812);

  // Use this to fine tune frequency: oscilloscope should show 125 kHz square wave
  //memset(&data, 0b01010101, sizeof(data));
}


/*
DMX512:
1111111111111111 1111111111111111 1111111111111111 1111111111111111 1111111111111111 1111111111111111 1111111111111111 1111111111111111 1111111111111111 1111111111111111 
0000000000000000 0000000000000000 0000000000001110 0000000011111110 (null byte)
0000000011111110 0000000011111110 (1st value)
...
0000000011111110 0000000011111110 
0000000011111110 0000000011111111 (512th value)

DMX256:
1111111111111111 1111111111111111 1111111111111111 1111111111111111 1111111111111111 1111111111111111 1111111111111111 1111111111111111 1111111111111111 1111111111111111 
0000000000000000 0000000000000000 0000000000001110 0000000011111110 (null byte)
0000000011111110 0000000011111110 (1st value)
...
0000000011111110 0000000011111111 (256th value)
*/
void debugI2S() {
  int count = sizeof(global.i2s_data) / sizeof(uint16_t);
  uint16_t * words = (uint16_t*) &(global.i2s_data);  
  for (int i=0; i < count; i++) {
    uint16_t b = words[i];
    byte hi = b>>8;
    byte lo = b & 0xff;
    printBits(hi);
    printBits(lo);
    Serial.print(" ");
  }
  Serial.println();      
}

void outputI2S(void) {
  // From the comment in i2s.h:
  // "A frame is just a int16_t for mono, for stereo a frame is two int16_t, one for each channel."
  // Therefore we need to divide by 4 in total
  int frames = sizeof(global.i2s_data) / 4;
  //debugI2S();
  
  i2s_write_buffer((int16_t*) &global.i2s_data, frames);

  i2sCounter++;
  long now = millis();
  if ((now - tic_i2s) > 1000 && i2sCounter > 100) {
    // don't estimate the FPS too frequently
    float pps = (1000.0 * i2sCounter) / (now - tic_i2s);
    tic_i2s = now;
    i2sCounter = 0;
    Serial.printf("I2S: %.1f p/s\n", pps);
  }
}

// Reverse byte order because DMX expects LSB first but I2S sends MSB first.
byte flipByte(byte c) {
  c = ((c >> 1) & 0b01010101) | ((c << 1) & 0b10101010);
  c = ((c >> 2) & 0b00110011) | ((c << 2) & 0b11001100);
  return (c >> 4) | (c << 4) ;
}

#endif // ENABLE_I2S

#ifdef ENABLE_WEBINTERFACE

void initServer() {
  // this serves all URIs that can be resolved to a file on the SPIFFS filesystem
  server.onNotFound(handleNotFound);

  server.on("/", HTTP_GET, []() {
    tic_web = millis();
    handleRedirect("/index");
  });

  server.on("/index", HTTP_GET, []() {
    tic_web = millis();
    handleStaticFile("/index.html");
  });

  server.on("/defaults", HTTP_GET, []() {
    tic_web = millis();
    Serial.println("handleDefaults");
    handleStaticFile("/reload_success.html");
    delay(2000);
    singleRed();
    initialConfig();
    saveConfig();
    WiFiManager wifiManager;
    wifiManager.resetSettings();
    WiFi.hostname(host);
    ESP.restart();
  });

  server.on("/reconnect", HTTP_GET, []() {
    tic_web = millis();
    Serial.println("handleReconnect");
    handleStaticFile("/reload_success.html");
    delay(2000);
    singleRed();
    WiFiManager wifiManager;
    wifiManager.setAPStaticIPConfig(IPAddress(192, 168, 1, 1), IPAddress(192, 168, 1, 1), IPAddress(255, 255, 255, 0));
    wifiManager.startConfigPortal(host);
    Serial.println("connected");
    if (WiFi.status() == WL_CONNECTED)
      singleGreen();
  });

  server.on("/reset", HTTP_GET, []() {
    tic_web = millis();
    Serial.println("handleReset");
    handleStaticFile("/reload_success.html");
    delay(2000);
    singleRed();
    ESP.restart();
  });

  server.on("/monitor", HTTP_GET, [] {
    tic_web = millis();
    handleStaticFile("/monitor.html");
  });

  server.on("/hello", HTTP_GET, [] {
    tic_web = millis();
    handleStaticFile("/hello.html");
  });

  server.on("/settings", HTTP_GET, [] {
    tic_web = millis();
    handleStaticFile("/settings.html");
  });

  server.on("/dir", HTTP_GET, [] {
    tic_web = millis();
    handleDirList();
  });

  server.on("/json", HTTP_PUT, [] {
    tic_web = millis();
    handleJSON();
  });

  server.on("/json", HTTP_POST, [] {
    tic_web = millis();
    handleJSON();
  });

  server.on("/json", HTTP_GET, [] {
    tic_web = millis();
    DynamicJsonDocument root(300);
    CONFIG_TO_JSON(universe, "universe");
    CONFIG_TO_JSON(channels, "channels");
    CONFIG_TO_JSON(delay, "delay");
    root["version"] = version;
    root["uptime"]  = long(millis() / 1000);
    root["packets"] = packetCounter;
    root["fps"]     = fps;
    String str;
    serializeJson(root, str);
    server.send(200, "application/json", str);
  });

  server.on("/update", HTTP_GET, [] {
    tic_web = millis();
    handleStaticFile("/update.html");
  });

  server.on("/update", HTTP_POST, handleUpdate1, handleUpdate2);

  // start the web server
  server.begin();
}

#endif // ifdef ENABLE_WEBINTERFACE

#ifdef COMMON_ANODE

void singleRed() {
  digitalWrite(LED_R, LOW);
  digitalWrite(LED_G, HIGH);
  digitalWrite(LED_B, HIGH);
}

void singleGreen() {
  digitalWrite(LED_R, HIGH);
  digitalWrite(LED_G, LOW);
  digitalWrite(LED_B, HIGH);
}

void singleBlue() {
  digitalWrite(LED_R, HIGH);
  digitalWrite(LED_G, HIGH);
  digitalWrite(LED_B, LOW);
}

void singleYellow() {
  digitalWrite(LED_R, LOW);
  digitalWrite(LED_G, LOW);
  digitalWrite(LED_B, HIGH);
}

void allBlack() {
  digitalWrite(LED_R, HIGH);
  digitalWrite(LED_G, HIGH);
  digitalWrite(LED_B, HIGH);
}

#else

void singleRed() {
  digitalWrite(LED_R, HIGH);
  digitalWrite(LED_G, LOW);
  digitalWrite(LED_B, LOW);
}

void singleGreen() {
  digitalWrite(LED_R, LOW);
  digitalWrite(LED_G, HIGH);
  digitalWrite(LED_B, LOW);
}

void singleBlue() {
  digitalWrite(LED_R, LOW);
  digitalWrite(LED_G, LOW);
  digitalWrite(LED_B, HIGH);
}

void singleYellow() {
  digitalWrite(LED_R, HIGH);
  digitalWrite(LED_G, HIGH);
  digitalWrite(LED_B, LOW);
}

void allBlack() {
  digitalWrite(LED_R, LOW);
  digitalWrite(LED_G, LOW);
  digitalWrite(LED_B, LOW);
}

#endif

#ifdef WITH_TEST_CODE

void testCode() {
  long now = millis();
  uint8_t x = (now / 60) % 240;
  if (x > 120) {
    x = 240 - x;
  }

  //Serial.printf("x: %d\n", x);
#ifdef ENABLE_UART
  global.data[1] =   x; // x 0 - 170
  global.data[2] =   0; // x fine

  global.data[3] =   x; // y: 0: -horz. 120: vert, 240: +horz
  global.data[4] =   0; // y fine

  global.data[5] =  30; // color wheel: red
  global.data[6] =   0; // pattern
  global.data[7] =   0; // strobe
  global.data[8] = 150; // brightness
#endif // ENABLE_UART

#ifdef ENABLE_I2S
  global.i2s_data.dmx_bytes[1] = (flipByte(  x) << 8) | 0b0000000011111110; // x 0 - 170
  global.i2s_data.dmx_bytes[2] = (flipByte(  0) << 8) | 0b0000000011111110; // x fine

  global.i2s_data.dmx_bytes[3] = (flipByte(  x) << 8) | 0b0000000011111110; // y: 0: -horz. 120: vert, 240: +horz
  global.i2s_data.dmx_bytes[4] = (flipByte(  0) << 8) | 0b0000000011111110; // y fine

  global.i2s_data.dmx_bytes[5] = (flipByte( 30) << 8) | 0b0000000011111110; // color wheel: red
  global.i2s_data.dmx_bytes[6] = (flipByte(  0) << 8) | 0b0000000011111110; // pattern
  global.i2s_data.dmx_bytes[7] = (flipByte(  0) << 8) | 0b0000000011111110; // strobe
  global.i2s_data.dmx_bytes[8] = (flipByte(150) << 8) | 0b0000000011111110; // brightness
#endif
}

#endif // WITH_TEST_CODE

#ifdef ENABLE_ARDUINO_OTA

int last_ota_progress = 0;

void initOTA() {
  Serial.println("Initializing Arduino OTA");
  ArduinoOTA.setHostname(host);
  ArduinoOTA.setPassword(ARDUINO_OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    allBlack();
    digitalWrite(LED_B, ON);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    allBlack();
    digitalWrite(LED_R, ON);
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    analogWrite(LED_B, 4096 - (20 * millis()) % 4096);
    if (progress != last_ota_progress) {
      Serial.printf("OTA Progress: %u%%\n", (progress / (total / 100)));
      last_ota_progress = progress;
    }
  });
  ArduinoOTA.onEnd([]()   {
    allBlack();
    digitalWrite(LED_G, ON);
    delay(500);
    allBlack();
  });
  Serial.println("Arduino OTA init complete");
}
#endif // ENABLE_ARDUINO_OTA
