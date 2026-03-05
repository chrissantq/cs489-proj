#include <cstdlib>

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

// other constants
#define DEBOUNCE 200 // ignore button inputs within 200ms of last

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
*   0 -> relays
*   1 -> microphone
*/
void * devtab[NDEVS][NDEV_INST] = {0};

byte chipstate = 0b00000000; // current state of the 74hc595 chip
int noise_avg = 512;
volatile int devnum = 0; // not volatile rn, prob will be later
volatile bool toggled = false;
volatile unsigned long lastPressTime = 0;

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
