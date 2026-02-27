// analog pins
#define MICPIN A0

// digital pins
#define TOGGLEPIN 2 // right now button to toggle relay, eventually
                    // the voice commands can hopefully do this
#define DATAPIN 8 
#define CLOCKPIN 11
#define LATCHPIN 12
#define AUDIOPIN 9

// other constants
#define DEBOUNCE 200 // ignore button inputs within 200ms of last

byte relay_states[] = {0b00000000, 0b00000010};
volatile int state = 0;
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
}

// toggle button state
void toggleISR() {
  unsigned long now = millis();
  if (now - lastPressTime > DEBOUNCE) {
    state ^= 1; // toggle state between 0 and 1
    toggled = true;
    lastPressTime = now;
  }
}

// update the state
void updateRelay() {
  // push out new state
  digitalWrite(LATCHPIN, LOW);
  shiftOut(DATAPIN, CLOCKPIN, MSBFIRST, relay_states[state]);
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
    updateRelay();
  }
}
