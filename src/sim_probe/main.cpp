// SIM800L probe (test only, read-only on the modem: status queries, no settings changed).
// Wiring per carrier: SIM TXD -> RX1 (GPIO16), SIM RXD -> TX1 (GPIO17). GPIO17 is also the carrier's
// AS3935 interrupt line, so its idle level is checked first.
#include <Arduino.h>

static const int RX_PIN = 16, TX_PIN = 17;

static String sendAT(const char* cmd, uint32_t waitMs = 1500) {
  while (Serial1.available()) Serial1.read();
  Serial1.print(cmd);
  Serial1.print("\r\n");
  String out;
  uint32_t t0 = millis();
  while (millis() - t0 < waitMs) {
    while (Serial1.available()) out += char(Serial1.read());
    delay(5);
  }
  out.replace("\r", "");
  out.replace("\n", " | ");
  Serial.printf("  %-14s -> %s\n", cmd, out.length() ? out.c_str() : "(no reply)");
  return out;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== SIM800L probe ===");
  for (int mode : {INPUT, INPUT_PULLUP, INPUT_PULLDOWN}) {
    pinMode(TX_PIN, mode);
    delay(20);
    int highs = 0;
    for (int i = 0; i < 20; i++) highs += digitalRead(TX_PIN), delay(2);
    Serial.printf("GPIO17 as %s: high %d/20\n", mode == INPUT ? "INPUT" : mode == INPUT_PULLUP ? "PULLUP" : "PULLDOWN", highs);
  }
  pinMode(RX_PIN, INPUT);
  int rxHigh = 0;
  for (int i = 0; i < 20; i++) rxHigh += digitalRead(RX_PIN), delay(2);
  Serial.printf("GPIO16 (SIM TXD) idle: high %d/20 (UART idle should be high)\n", rxHigh);

  long found = 0;
  for (long baud : {9600L, 115200L, 57600L, 38400L, 19200L}) {
    Serial1.begin(baud, SERIAL_8N1, RX_PIN, TX_PIN);
    Serial.printf("baud %ld:\n", baud);
    for (int i = 0; i < 3 && !found; i++)
      if (sendAT("AT", 800).indexOf("OK") >= 0) found = baud;
    if (found) break;
    Serial1.end();
  }
  if (!found) {
    Serial.println("RESULT: modem did not answer AT at any baud rate");
    return;
  }
  Serial.printf("RESULT: modem answers at %ld baud\n", found);
  for (const char* c : {"ATI", "AT+GSN", "AT+CPIN?", "AT+CCID", "AT+CSQ", "AT+CREG?", "AT+COPS?", "AT+CBC", "AT+CGATT?"})
    sendAT(c, 2000);
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last > 20000) {
    last = millis();
    Serial.println("status:");
    sendAT("AT+CSQ");
    sendAT("AT+CREG?");
    sendAT("AT+CBC");
  }
}
