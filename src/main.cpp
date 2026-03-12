#include <Arduino.h>

int myFunction(int, int);

void setup() {
  Serial.begin(921600);
  Serial.println("\nStart Program");
  int result = myFunction(2, 3);
  Serial.println(result);
}

void loop() {

}

int myFunction(int x, int y) {
  return x + y;
}