#include <cstdlib>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "FspTimer.h"

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
#define MICID 1
#define WIFIID 2

// other constants
#define DEBOUNCE 200 // ignore button inputs within 200ms of last
#define PACKET_SAMPLES 320 // audio sample size to send via udp
#define SEND_BUF_SIZE 640 // send buf big -> PACKET_SAMPLES * 2 bytes
#define RECV_BUF_SIZE 120 // recv buf smaller, just simple command

// device structs
typedef struct relay_dev {
  int devnum;     // specific dev num
  int mask;       // bitmask to toggle state via 74hc595 chip
  bool state;     // true = on, false = off
} relay_t;

typedef struct microphone_dev {
  int16_t buffer[PACKET_SAMPLES]; // store audio input
  int devnum;     // specific device number
  int threshold;  // sound difference to register
  int pin;        // pin device using
  int reading;    // microphone analogread
} mic_t;

// pseudodevice used to hold server information and other wifi data
typedef struct wifi_dev {
  int devnum;
  int serverIP[4]; // server ip (store each part separate in arr)
  int serverPort;  // server port
  volatile uint8_t recv_buf[RECV_BUF_SIZE];    // recv data buffer
  volatile uint8_t send_buf[SEND_BUF_SIZE];    // send data buffer
  volatile int s_idx;    // index in send buffer
  volatile int r_idx;    // index in recv buffer
  volatile bool pkt_rdy; // if packet is ready to send
} wifi_t;

/*
*   dev table
*
*   rows: device; cols: dev num
*   0 -> relays
*   1 -> microphone
*   2 -> wifi pseudodevice
*/
void * devtab[NDEVS][NDEV_INST] = {0};

// timer to interrupt for audio sampling
FspTimer audioTimer;

// wifi connections
WiFiUDP udp;
const char * ssid = "wifi";
const char * pass = "pass";
const int local_port = 12345;

byte chipstate = 0b00000000; // current state of the 74hc595 chip
int noise_avg = 512;         // noise avg detected by mic of room
volatile int devnum = 0;     // not volatile rn, prob will be later
volatile bool toggled = false;
volatile unsigned long lastPressTime = 0;

// set up wifi device(s)
void wifi_setup() {

  // whisper server (localhost:1223 for now)
  wifi_t * whisper = (wifi_t*)malloc(sizeof(wifi_t));
  whisper->devnum = 0;
  whisper->serverIP[0] = 127; whisper->serverIP[1] = 0;
  whisper->serverIP[2] = 0; whisper->serverIP[3] = 1;
  whisper->serverPort = 1223;
  memset((void*)whisper->recv_buf, 0, sizeof(whisper->recv_buf));
  memset((void*)whisper->send_buf, 0, sizeof(whisper->send_buf));
  whisper->s_idx = 0;
  whisper->r_idx = 0;
  whisper->pkt_rdy = false;
  devtab[WIFIID][0] = whisper;

}

// connect to wifi
void wifi_connect() {

  while (WiFi.begin(ssid, pass) != WL_CONNECTED) {
    delay(1000);
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
  microphone->pin = A5;
  microphone->reading = 0;
  memset(microphone->buffer, 0, sizeof(microphone->buffer));
  devtab[MICID][microphone->devnum] = microphone;

  // set up wifi device connection(s);
  wifi_setup();
  wifi_connect();

}

void setup() {
  Serial.begin(115200);
  pinMode(DATAPIN, OUTPUT);
  pinMode(CLOCKPIN, OUTPUT);
  pinMode(LATCHPIN, OUTPUT);
  pinMode(AUDIOPIN, OUTPUT);
  pinMode(TOGGLEPIN, INPUT_PULLUP);

  // interrupt for button
  attachInterrupt(digitalPinToInterrupt(TOGGLEPIN), toggleISR, FALLING);

  // audio timer setup
  uint8_t timer_type = GPT_TIMER;
  int8_t timer_idx = FspTimer::get_available_timer(timer_type);
  audioTimer.begin(TIMER_MODE_PERIODIC, timer_type, timer_idx,
                   16000, 0.0, audioSampleISR);
  audioTimer.start();

  // init devices
  devinit();


}

// toggle button state
void toggleISR() {
  unsigned long now = millis();
  if (now - lastPressTime > DEBOUNCE) {
    toggled = true;
    lastPressTime = now;
  }
}

// update the state
void updateRelay(relay_t* relay) {
  chipstate ^= relay->mask; // toggle relay
  // push out new state
  digitalWrite(LATCHPIN, LOW);
  shiftOut(DATAPIN, CLOCKPIN, MSBFIRST, chipstate);
  digitalWrite(LATCHPIN, HIGH);
  // play little tone on speaker
  tone(AUDIOPIN, 100*(relay->devnum), 150);
  delay(150);
  tone(AUDIOPIN, 200, 50);
  delay(50);
}

// interrupt to record audio every ~62 microsec (16kHz)
void audioSampleISR(timer_callback_args_t* args) {
  mic_t * mic = (mic_t*)devtab[MICID][0];
  wifi_t * whisper = (wifi_t*)devtab[WIFIID][0];
  mic->reading = analogRead(mic->pin);
  // convert reading to pcm (whisper uses this format)
  int center = mic->reading - 2048;
  int16_t pcm = center << 4;
  memcpy((void*)&whisper->send_buf[whisper->s_idx], &pcm, sizeof(pcm));
  whisper->s_idx += 2; // each pcm 2 bytes
  if (whisper->s_idx >= PACKET_SAMPLES) {
    whisper->pkt_rdy = true;
    whisper->s_idx = 0;
  }
}

// polls the microphone for a clap
void micDetection(mic_t* mic) {
  mic->reading = analogRead(mic->pin);
  noise_avg = (noise_avg * 15 + mic->reading) / 16;
  if (abs(mic->reading - noise_avg) > mic->threshold) {
    delay(10); // detect short burst of sound (ie clap)
    mic->reading = analogRead(mic->pin);
    if (abs(mic->reading - noise_avg) < (mic->threshold / 2)) {
      toggled = true;
    }
  }
}

// function to send udp packet of data from send_buf to
// corresponding server destination
void sendUDP(wifi_t* dest) {
  IPAddress ip(dest->serverIP[0], dest->serverIP[1],
               dest->serverIP[2], dest->serverIP[3]);
  udp.beginPacket(ip, dest->serverPort);
  udp.write((uint8_t*)dest->send_buf, sizeof(dest->send_buf));
  udp.endPacket();
}

// recieve data from src to the receive buffer
void recvUDP(wifi_t* src) {
  int pktSize = udp.parsePacket();
  if (pktSize) {
    int len = udp.read((uint8_t*)src->recv_buf, RECV_BUF_SIZE - 1);
    if (len > 0) {
      src->recv_buf[len] = '\0';
      src->r_idx = len;
    }
  }
}

void loop() {
  if (toggled) {
    toggled = false;
    relay_t *relay = (relay_t*)devtab[RELAYID][devnum];
    updateRelay(relay);
    devnum = (devnum + 1) % 4; // for now just cycle between the relays
                             // later, will be chosen based off command
  }

  // poll mic for loud noise (ie clap) to toggle
  // prob switch to interrupts using LM393 chip to be more
  // reliable, this works decently well tho for now
  mic_t * mic = (mic_t*)devtab[MICID][0];
  micDetection(mic);

  wifi_t * whisper = (wifi_t*)devtab[WIFIID][0];
  if (whisper->pkt_rdy) {sendUDP(whisper);} // send if ready
  recvUDP(whisper); // check for packet (if r_len > 0)


}
