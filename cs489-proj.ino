#include <cmath>

// analog pins
#define MICPIN A0

// digital pins
#define TOGGLEPIN 2 // right now button to toggle relay, eventually
                    // the voice commands can hopefully do this
#define DATAPIN 8 
#define CLOCKPIN 11
#define LATCHPIN 12
#define AUDIOPIN 9

// devtab and devid constants
#define NDEVS 10     // max number of unique types of devices
#define NDEV_INST 5  // max number of each device type
#define RELAY 0

// other constants
#define DEBOUNCE 200 // ignore button inputs within 200ms of last

/*
*   dev table
*   for now just store the one thing used by dev, later
*   change to store pointers to struct for specific dev
*
*   rows: device; cols: dev num
*   0 -> relays
*/
int devtab[NDEVS][NDEV_INST] = {{0}};

byte chipstate = 0b00000000; // current state of the 74hc595 chip
volatile int devnum = 0; // not volatile rn, prob will be later
volatile bool toggled = false;
volatile unsigned long lastPressTime = 0;

void setup() {
  Serial.begin(9600);
  pinMode(DATAPIN, OUTPUT);
  pinMode(CLOCKPIN, OUTPUT);
  pinMode(LATCHPIN, OUTPUT);
  pinMode(AUDIOPIN, OUTPUT);
  pinMode(TOGGLEPIN, INPUT_PULLUP);
  // interrupt when button is pressed
  attachInterrupt(digitalPinToInterrupt(TOGGLEPIN), toggleISR, FALLING);

  // setup device table
  // set relay to respective bit value to toggle
  // 74hc595 chip output (00000010 through 00010000)
  for (int i = 0; i < 4; i++) {
    devtab[RELAY][i] = std::pow(2, i+1);
  }

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
void updateRelay(int devnum) {
  chipstate ^= devtab[RELAY][devnum]; // toggle relay
  // push out new state
  digitalWrite(LATCHPIN, LOW);
  shiftOut(DATAPIN, CLOCKPIN, MSBFIRST, chipstate);
  digitalWrite(LATCHPIN, HIGH);
  // play little tone on speaker
  tone(AUDIOPIN, 100, 150);
  delay(150);
  tone(AUDIOPIN, 200, 50);
  delay(50);
}

void loop() {
  if (toggled) {
    toggled = false;
    devnum = (devnum + 1) % 4; // for now just cycle between the relays
                             // later, will be chosen based off command
    updateRelay(devnum);
  }
}
