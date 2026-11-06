// P4OS USB host test: a serial port and a keyboard on one device.
// Prints a line a second; echoes what it gets, upper case; types a line
// as a keyboard ONLY when it receives '!' over serial. Built with -DRAPIDO
// it also blinks the "L" LED every 100 ms and says so in each line, to
// tell it from the plain one after the Programador writes it.
#include <Keyboard.h>

unsigned long n = 0, last = 0;

void setup() {
#ifdef RAPIDO
  pinMode(LED_BUILTIN, OUTPUT);
#endif
  Serial.begin(115200);
  Keyboard.begin();
}

void loop() {
#ifdef RAPIDO
  digitalWrite(LED_BUILTIN, (millis() / 100) & 1);
#endif
  if (millis() - last >= 1000) {
    last = millis();
#ifdef RAPIDO
    Serial.print("P4OS Leonardo rapido ");
#else
    Serial.print("P4OS Leonardo ");
#endif
    Serial.println(n++);
  }
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '!') Keyboard.print("hola desde el Leonardo");
    else Serial.write(toupper(c));
  }
}
