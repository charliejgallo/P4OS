// P4OS USB host test: a serial port and a keyboard on one device.
// Prints a line a second; echoes what it gets, upper case; types a line
// as a keyboard ONLY when it receives '!' over serial.
#include <Keyboard.h>

unsigned long n = 0, last = 0;

void setup() {
  Serial.begin(115200);
  Keyboard.begin();
}

void loop() {
  if (millis() - last >= 1000) {
    last = millis();
    Serial.print("P4OS Leonardo ");
    Serial.println(n++);
  }
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '!') Keyboard.print("hola desde el Leonardo");
    else Serial.write(toupper(c));
  }
}
