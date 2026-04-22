#include <cstdlib>
#include <cstdio>
#include <WiFi.h>
#include <WiFiUdp.h>

// digital pins
#define TOGGLEPIN 2 // right now button to toggle relay, eventually
                    // the voice commands can hopefully do this
#define DATAPIN 8 
#define CLOCKPIN 11
#define LATCHPIN 12
#define AUDIOPIN 9

// devtab constants
#define NDEVS 10     // max number of unique types of devices
#define NDEV_INST 5  // max number of each device type
#define NRELAYS 4    // number of relays

// device ids
#define RELAYID 0
#define WIRELESSID 1
#define MICID 2

// other constants
#define DEBOUNCE 200 // ignore button inputs within 200ms of last
#define CLAP_EXPIRE_TIME 2000 // 2 seconds

// device structs
typedef struct relay_dev {
  int devnum;     // specific dev num
  int mask;       // bitmask to toggle state via 74hc595 chip
  bool state;     // true = on, false = off
} relay_t;

typedef struct microphone_dev {
  int devnum;     // specific device number
  int threshold;  // sound difference to register
  int pin;        // pin device using
  int reading;    // microphone analogread
} mic_t;

/*
*   dev table
*
*   rows: device; cols: dev num
*   0 -> relays devs
*   1 -> wireless devices
*   2 -> microphone
*/
void * devtab[NDEVS][NDEV_INST] = {0};

byte chipstate = 0b00000000; // current state of the 74hc595 chip
bool toggled = false;
unsigned long prevTime = 0;
volatile unsigned long lastPressTime = 0;
volatile uint8_t clap_count = 0;

// Wifi info
WiFiUDP udp;
const char * ssid = "McDonald's Free Wifi";
const char * pass = "AKAK608Waldro";

IPAddress local_IP(192, 168, 1, 23);
IPAddress gateway(192, 168, 1, 25);
IPAddress subnet(255, 255, 255, 0);
const int local_port = 1223;
char app_cmd_buffer[32] = {'\0'};

mic_t * mic = NULL;

// wifi connection
void wifi_connect() {

  WiFi.config(local_IP, gateway, subnet);
  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
  udp.begin(local_port);
}

// device setup function
void devinit() {

  // setup relay devices
  for (int i = 0; i < NRELAYS; i++) {
    relay_t * newRelay = (relay_t*)malloc(sizeof(relay_t));
    newRelay->devnum = i;
    newRelay->mask = 1 << (i+1);
    newRelay->state = false;
    devtab[RELAYID][newRelay->devnum] = newRelay;
  }

  // set up microphone device
  mic_t * microphone = (mic_t*)malloc(sizeof(mic_t));
  microphone->devnum = 0;
  microphone->threshold = 300;
  microphone->pin = A0;
  microphone->reading = 0;
  devtab[MICID][microphone->devnum] = microphone;

}

void setup() {
  Serial.begin(115200);
  pinMode(DATAPIN, OUTPUT);
  pinMode(CLOCKPIN, OUTPUT);
  pinMode(LATCHPIN, OUTPUT);
  pinMode(AUDIOPIN, OUTPUT);
  pinMode(TOGGLEPIN, INPUT_PULLUP);
  // interrupt when button is pressed
  attachInterrupt(digitalPinToInterrupt(TOGGLEPIN), toggleISR, FALLING);

  // init devices
  devinit();
  wifi_connect();

  mic = (mic_t*)devtab[MICID][0];

}

// toggle button state
void toggleISR() {
  unsigned long now = millis();
  if (now - lastPressTime > DEBOUNCE) {
    toggled = true;
    lastPressTime = now;
  }
}

// resets claps
void resetClap() {
  clap_count = 0;
  toggled = false;
}

// update the state
void updateRelay(relay_t* relay) {
  chipstate ^= relay->mask; // toggle relay
  relay->state = !(relay->state);
  // push out new state
  digitalWrite(LATCHPIN, LOW);
  shiftOut(DATAPIN, CLOCKPIN, MSBFIRST, chipstate);
  digitalWrite(LATCHPIN, HIGH);
}

// polls the microphone for a clap
void micDetection() {
  static float baseline = 512.0f;
  static float noise = 20.0f;

  static int lastReading = 0;
  static unsigned long lastTrigger = 0;

  int reading = analogRead(mic->pin);
  mic->reading = reading;

  int diff = abs(reading - baseline);
  int delta = reading - lastReading;

  // update baseline
  if (diff < mic->threshold) {
    float alpha = 0.005f;
    float beta  = 0.05f;

    baseline = (1.0f - alpha) * baseline + alpha * reading;
    noise = (1.0f - beta) * noise + beta * diff;
  }

  // update threshold
  mic->threshold = max((int)(noise * 3), 60);

  // condition 1: sharp spike
  bool spike = abs(delta) > 60 && diff > mic->threshold;
  static bool inSpike = false;

  // condition 2: short duration (returns quickly)
  static int spikeCount = 0;

  if (!inSpike && spike) {
      inSpike = true;
      unsigned long now = millis();
      if (now - lastTrigger > 200) {
          clap_count++;
          prevTime = now;
          lastTrigger = now;
          Serial.println("clap");
      }
  } else if (inSpike && !spike) {
      inSpike = false;
  }

  noise = noise * 0.99 + 1.0;
  lastReading = reading;
}

// detect cmd based on number of claps and silence
void doCmd() {

  if (clap_count > NDEV_INST || clap_count < 1) return;

  relay_t *relay = (relay_t*)devtab[RELAYID][clap_count-1];

  switch (clap_count - 1) {
    case 0:
      updateRelay(relay);
      break;
    case 1:
      updateRelay(relay);
      break;
    case 2:
      updateRelay(relay);
      break;
    case 3:
      updateRelay(relay);
      break;
    default:
      break;


  }
  
  // send the updated state to app
  char sendstate[16] = {'\0'};
  int statechar = relay->state ? 1 : 0;
  snprintf(sendstate, 16, "%d: %d", clap_count, statechar); 
  sendUDP(sendstate, 16, 5000); // to app

  resetClap();

}

// recv packets
void recvUDP(char * buf, int size) {
  int len = udp.read((uint8_t*)buf, size - 1);
  if (len > 0) {
    buf[len] = '\0';
  }
  Serial.println("recv");
}

// send packets (to app for now)
void sendUDP(char * buffer, int size, int port) {
  IPAddress ip(192, 168, 1, 125);
  udp.beginPacket(ip, port);
  udp.write((uint8_t*)buffer, size);
  udp.endPacket();
  Serial.println("sent");
}

// parseAppCmd: parse command received from app
int parseAppCmd() {
  int cmdId = 0;
  int count = sscanf(app_cmd_buffer, " app: %d", &cmdId);
  if (count == 1) {
    return cmdId;
  } else {
    return 0;
  }
}

void loop() {

  // check for incoming pkts
  if (udp.parsePacket()) {
    recvUDP(app_cmd_buffer, sizeof(app_cmd_buffer));
    clap_count = parseAppCmd();
    doCmd();

  }

  // poll for clap
  micDetection();

  // would rather do interrupt but cant get timer to work
  unsigned long curTime = millis();
  if (curTime - prevTime > CLAP_EXPIRE_TIME) {
    doCmd();
    prevTime = curTime;
  }


}
