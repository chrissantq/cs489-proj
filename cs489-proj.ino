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
#define MICID 0
#define RELAYID 1
#define WIFIID 2

// audio and packet constants
#define PACKET_SAMPLES 320 // audio sample size to send via udp
#define MAGIC 0xA5B4
#define HEADER_SIZE 8 // 2 magic + 2 seq + 2 n samples + 2 checksum
#define SEND_BUF_SIZE (PACKET_SAMPLES * 2 + HEADER_SIZE) // 648 bytes
#define RECV_BUF_SIZE 120 // recv buf smaller, just simple commands

// other constants
#define DEBOUNCE 200 // ignore button inputs within 200ms of last

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

// wifi_t bool macros
#define SEND 0
#define RECV 1
#define SET_RDY(x, pos) ((x) |= (1 << (pos)))
#define CLEAR_RDY(x, pos) ((x) &= ~(1 << (pos)))
#define CHECK_RDY(x, pos) ((x & (1 << (pos))))

// pseudodevice used to hold server information and other wifi data
typedef struct wifi_dev {
  int devnum;
  int serverIP[4]; // server ip (store each part separate in arr)
  int serverPort;  // server port
  volatile uint8_t recv_buf[RECV_BUF_SIZE];    // recv data buffer
  volatile uint8_t send_buf[2][SEND_BUF_SIZE];    // send data buffer
  volatile int active_buf; // which buf ISR is writing to
  volatile int ready_buf; // which buf is ready to send
  volatile int sending;   // which buf is actively sending
  volatile int s_idx;    // index in send buffer
  volatile int r_idx;    // index in recv buffer
  volatile int pkt_rdy; // if packet is ready to send
                        // pos 0 = send, pos 1 = recv
  volatile uint16_t seq;
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

/*
 *  CMD table
 *  rows: cmd type (system, relay, bluetooth, etc)
 *  cols: cmd number
 *
 *  CMD ID = [row]*10 + [col]
 *  0: system
 *  1: relays
 *  2: bluetooth
 *
 */

// timer to interrupt for audio sampling
FspTimer audioTimer;

// wifi connections
WiFiUDP udp;
const char * ssid = "McDonald's Free Wifi";
const char * pass = "AKAK608Waldro";
const int local_port = 1223;

byte chipstate = 0b00000000; // current state of the 74hc595 chip
volatile float baseline = 2048.0f;            // base room volume level
volatile int baseline_int = 0;
volatile int devnum = 0;     // not volatile rn, prob will be later
volatile bool toggled = false;
volatile unsigned long lastPressTime = 0;

// dev ptrs
wifi_t * whisper = NULL;
mic_t * mic = NULL;

// set up wifi device(s)
void wifi_setup() {

  // whisper server (localhost:1223 for now)
  whisper = (wifi_t*)malloc(sizeof(wifi_t));
  whisper->devnum = 0;
  whisper->serverIP[0] = 192; whisper->serverIP[1] = 168;
  whisper->serverIP[2] = 1; whisper->serverIP[3] = 63;
  whisper->serverPort = 1223;
  memset((void*)whisper->recv_buf, 0, sizeof(whisper->recv_buf));
  memset((void*)whisper->send_buf, 0, sizeof(whisper->send_buf));
  whisper->s_idx = 0;
  whisper->r_idx = 0;
  whisper->pkt_rdy = 0;
  whisper->seq = 0;
  whisper->active_buf = 0;
  whisper->ready_buf = 0;
  whisper->sending = -1;
  devtab[WIFIID][0] = whisper;

}

// connect to wifi
void wifi_connect() {

  Serial.print("Connecting to WiFi...");
  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected!");
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
  mic = (mic_t*)malloc(sizeof(mic_t));
  mic->devnum = 0;
  mic->threshold = 300;
  mic->pin = A0;
  mic->reading = 0;
  memset(mic->buffer, 0, sizeof(mic->buffer));
  devtab[MICID][mic->devnum] = mic;

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

  // init devices
  Serial.println("Initializing devices...");
  devinit();
  Serial.println("Devices initialized.");

  // "kick" adc
  analogReadResolution(12);
  analogRead(mic->pin);

  // set up continuous ADC register reading instead of analogRead()
  R_ADC0->ADANSA[0] = (1 << 9); // ADC channel select reg A, group 0 (AN009 for A0)
  R_ADC0->ADCER = 0x0000; // set 12 bit resolution (bits 2:1 = 00) and r-aligned output (bit 15 = 0)
  R_ADC0->ADCSR = (0b10 << 13) | (1 << 15); // set ADCS to 10 (cont scan)
                                            // set ADST bit (starts ADC)

  // wait a moment for ADC to stabilize and get initial baseline
  delay(10);
  baseline = (float)R_ADC0->ADDR[9];
  baseline_int = (int)baseline;

  // audio timer setup
  uint8_t timer_type = GPT_TIMER;
  int8_t timer_idx = FspTimer::get_available_timer(timer_type);
  audioTimer.begin(TIMER_MODE_PERIODIC, timer_type, timer_idx,
                   16000, 0, audioSampleISR);
  audioTimer.setup_overflow_irq();
  audioTimer.open();
  audioTimer.start();

  Serial.println("Audio timer started.");

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

  // read and trigger next reading immediately
  int reading = (int)R_ADC0->ADDR[9]; // ADC writes latest res for AN009 (A0)

  if (!mic || !whisper) return;

  mic->reading = reading;

  // get pcm (<< 4 for gain, gives more range)
  int pcm_val = (reading - baseline_int) << 4;

  // clamp values
  if (pcm_val > 32767) pcm_val = 32767;
  else if (pcm_val < -32768) pcm_val = -32768;
  int16_t pcm = (int16_t)pcm_val;

  // direct buffer access
  int s_idx = whisper->s_idx;
  int active = whisper->active_buf;

  // direct assign to buffer (both little endian)
  *(int16_t*)&whisper->send_buf[active][HEADER_SIZE + s_idx] = pcm;
  s_idx += 2;

  // swap buffer and send, reset send index
  if (s_idx >= PACKET_SAMPLES * 2) {
    int next = active ^ 1;
    if (next != whisper->sending) {
      whisper->ready_buf = active; // hand off full buffer
      whisper->active_buf = next; // switch to other buffer
      SET_RDY(whisper->pkt_rdy, SEND);
    }
    s_idx = 0;
  }
  whisper->s_idx = s_idx;
}

// polls the microphone for a clap
/*
void micDetection(mic_t* mic) {
  if (abs(mic->reading - noise_avg) > mic->threshold) {
    delay(10); // detect short burst of sound (ie clap)
    mic->reading = analogRead(mic->pin);
    if (abs(mic->reading - noise_avg) < (mic->threshold / 2)) {
      toggled = true;
    }
  }
}
*/

// function to send udp packet of data from send_buf to
// corresponding server destination
void sendUDP(wifi_t* dest) {

  int buf;

  // disable interrupts to grab data
  noInterrupts();
  if (!CHECK_RDY(dest->pkt_rdy, SEND)) {
    interrupts();
    return;
  }
  buf = dest->ready_buf;
  dest->sending = buf;
  CLEAR_RDY(dest->pkt_rdy, SEND);
  interrupts();


  uint16_t magic = MAGIC;
  uint16_t seq = dest->seq++;
  uint16_t n = PACKET_SAMPLES;

  // checksum: xor all bytes
  uint8_t checksum = 0;
  for (int i = HEADER_SIZE; i < SEND_BUF_SIZE; i++) {
    checksum ^= (uint8_t)dest->send_buf[buf][i];
  }
  uint16_t checksum16 = checksum;

  // assemble header
  memcpy((void*)&dest->send_buf[buf][0], &magic, 2);
  memcpy((void*)&dest->send_buf[buf][2], &seq, 2);
  memcpy((void*)&dest->send_buf[buf][4], &n, 2);
  memcpy((void*)&dest->send_buf[buf][6], &checksum16, 2);


  // send packet
  IPAddress ip(dest->serverIP[0], dest->serverIP[1],
               dest->serverIP[2], dest->serverIP[3]);
  udp.beginPacket(ip, dest->serverPort);
  udp.write((uint8_t*)dest->send_buf[buf], SEND_BUF_SIZE);
  udp.endPacket();

  dest->sending = -1; // release buffer
}

// receive data from src to the receive buffer
void recvUDP(wifi_t* src) {
  int pktSize = udp.parsePacket();
  if (pktSize) {
    int len = udp.read((uint8_t*)src->recv_buf, RECV_BUF_SIZE - 1);
    if (len > 0) {
      src->recv_buf[len] = '\0';
      src->r_idx = len;
      SET_RDY(src->pkt_rdy, RECV);
    }
  }
}

// parse cmd recv from whisper packet
// return command id number
int parseCmd(wifi_t* whisper) {
 
  //TODO: parse command

}


// execute command
#define SYSCMDID 0
// reuse RELAYID 1
#define BTCMDID 2
void doCmd(int cmdId) {

  int type = cmdId / 10;
  int num = cmdId % 10;
  
  switch (type) {
    case SYSCMDID:
      // add system pseudodevice to devtab similar to relays
      break;
    case RELAYID: {
      relay_t * relay = (relay_t*)devtab[RELAYID][num];
      updateRelay(relay);
      break;
    }
    case BTCMDID:
      // idk how these will be implemented if at all 
      break;
    default:
      // light up red led to show fail or something
      break;
  }

}

void loop() {

  if (!whisper || !mic) return;

  if (toggled) {
    toggled = false;
    relay_t *relay = (relay_t*)devtab[RELAYID][devnum];
    updateRelay(relay);
    devnum = (devnum + 1) % 4; // for now just cycle between the relays
                             // later, will be chosen based off command
  }

  baseline = (baseline * 0.999f) + (mic->reading * 0.001f); 
  baseline_int = (int)baseline;


  // send if ready
  if (CHECK_RDY(whisper->pkt_rdy, SEND)) {
    sendUDP(whisper);
    Serial.println("sent");
  }

  recvUDP(whisper); // check for packet (if r_len > 0)

  // if command received, process and execute
  if (CHECK_RDY(whisper->pkt_rdy, RECV)) {
    int cmdId = parseCmd(whisper);
    doCmd(cmdId);
  }

}
