/*
        Monitoring of water consumption



Connected to a pulse emitter placed on top of the flowmeter, the module
counts the pulses generated at each one-litre consumption (by a common-collector
linked to the GPIO-25 with a 2.2k pull-up resistor and filtred by a 0.1 uF capacitor).
The program makes different calculations on consumption (day, since
last reading), makes a leakage diagnosis (continuous consumption,
consumption above a given threshold within the last 24 hours) and displays the
information on an OLED display. A mini web server also allows access to
this information, as well as to initialize the counter readings.
In the event of a leakage, an email is sent via SMTP2GO.



Things to customize :
- WiFi SSID and password
- Port #
- SMTP2GO SMTP username/password
- sender and recipient email addresses
- password (hashed) for OTA (Over The Air) update
- Credentials for config page (ID & pwd)
- On the router : reserved IP and port redirection
- Bookmarks on PC and/or mobile phone



Board: ESP32 Dev Module
*/


//----------------------------------------------------------------------
// Global variables and includes
//----------------------------------------------------------------------

#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <WiFiClient.h>
#include <time.h>
#include <TimeLib.h>
#include <math.h>
#include <Preferences.h>
#include <Wire.h>
#include "SSD1306.h"
#include "OLEDDisplayUi.h"
#include "images.h"


const byte interruptPin = 25;
volatile int Total_Counter = 0;         // Pulse counter. Should be a copy of the flowmeter
int Last_Counter = 0;                   // Last value of the counter read by water provider
float Difference = 0.;                  // Volume consummed since last reading
boolean Day_Reset = true;               // Management of the daily counter reset
int Day_Counter = 0;                    // Daily consumption
int conso[24] = {0};                    // Table of the last 24hr consumption
int conso24 = 0;                        // Total consumption of the las 24hr
boolean save_conso = true ;             // Management of recording of consumption
boolean fuite1 = false;                 // Leak type 1 : continuous consumption during 24hr
boolean fuite2 = false;                 // Leak type 2 : consumption in excess of the 24hr threshold
int W_Threshold = 800;                  // Alert threshold, by default 800 l/day
int Prec_Counter = 0;                   // Management of counter
int nb_SMS = 0;                         // Number of sent SMS (email) (max 10 in a month)
boolean Day_SMS = false;                // Only one SMS (email) per day in case of alert
boolean SMS_Reset = false;              // Management of SMS (email) per day
boolean tst_SMS = false;                // for debug purpose
boolean tst_Clear = false;              //     - " -
boolean tst_Init = false;               //     - " -

String leakReason = "Kein Leck erkannt";
String leakDetail1 = "";
String leakDetail2 = "";

unsigned long lastNvsSave = 0;
const unsigned long nvsSaveInterval = 30000;
bool stateDirty = false;


portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;  // for critical section updated under interrupt level


//----------------------------------------------------------------------
// Constants to customize
//----------------------------------------------------------------------

const char* ssid     = "YOUR WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";


const char* www_username = "YOUR_WEB_USERNAME";
const char* www_password = "YOUR_WEB_PASSWORD";


// SMTP2GO
const char* smtpHost = "mail.smtp2go.com";
const uint16_t smtpPort = 2525;
const char* smtpUser = "YOUR_SMTP2GO_USERNAME";
const char* smtpPass = "YOUR_SMTP2GO_PASSWORD";
const char* mailFrom  = "sender@yourdomain.tld";
const char* mailTo    = "recipient@yourdomain.tld";


//----------------------------------------------------------------------
// Libraries / objects
//----------------------------------------------------------------------

boolean WiFi_Checked = false;           // To check wifi connection every minute


// Date and Time management
//-------------------------
char heure[] = "00:00:00";
char jour[] = "xx/xx/xxxx";
int hours = 0;
int mins = 0;
int secs = 0;
int qtm = 0;
int mois = 0;
int an = 0;
struct tm timeinfo;


// WebServer
//----------
WiFiClient client;
WiFiClient smtpClient;


String siteheading  = "Wasserverbrauchs-Monitor";                         // Main title
String sitetitle    = "Wasserverbrauchs-Monitor";                         // Thumbnail title
String yourfootnote = "Wasserverbrauchs-Monitor - Mein Haus";             // Footer
String siteversion  = "v1.7.2";                                           // Site version (also in "about" page)


#define sitewidth 768                                                     // page width
String webpage = "";                                                      // variable to store the html code


float field1 = 9999.999;
float field2 = 9999.999;
int   field3 = 800;


WebServer server(8065);                                                   // start webserver on port 8065 (can be customized)


// Display management
//-------------------
SSD1306 display(0x3c, 5, 4);
OLEDDisplayUi ui(&display);


Preferences prefs;


//----------------------------------------------------------------------
// SMTP helpers
//----------------------------------------------------------------------

String base64Encode(const String &in) {
  const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String out;
  int val = 0, valb = -6;
  for (uint8_t c : in) {
    val = (val << 8) + c;
    valb += 8;
    while (valb >= 0) {
      out += b64[(val >> valb) & 0x3F];
      valb -= 6;
    }
  }
  if (valb > -6) out += b64[((val << 8) >> (valb + 8)) & 0x3F];
  while (out.length() % 4) out += '=';
  return out;
}


bool smtpReadResponse(uint16_t expected, unsigned long timeout = 5000) {
  unsigned long start = millis();
  while (millis() - start < timeout) {
    while (smtpClient.available()) {
      String line = smtpClient.readStringUntil('\n');
      line.trim();
      if (line.length() >= 3) {
        int code = line.substring(0, 3).toInt();
        Serial.println(line);
        if (code == expected) return true;
      }
    }
    delay(10);
  }
  return false;
}


//----------------------------------------------------------------------
// Leak diagnosis helpers
//----------------------------------------------------------------------

void buildLeakReport() {
  leakReason = "Kein Leck erkannt";
  leakDetail1 = "";
  leakDetail2 = "";

  conso24 = 0;
  fuite1 = true;
  fuite2 = false;

  for (uint8_t i = 0; i < 24; i++) {
    uint8_t j = i + 1;
    if (j == 24) j = 0;
    if (conso[i] == 0 && conso[j] == 0) fuite1 = false;
    conso24 += conso[i];
  }

  if (conso24 > W_Threshold) fuite2 = true;

  if (fuite1) {
    leakDetail1 = "Lecktyp 1: Permanenter Verbrauch erkannt, es gibt keinen zweistündigen Zeitraum ohne Verbrauch während der letzten 24 Stunden.";
  }
  if (fuite2) {
    leakDetail2 = "Lecktyp 2: Der Verbrauch während der letzten 24 Stunden liegt über dem angegebenen Schwellenwert (" + String(W_Threshold) + " Liter). 24h: " + String(conso24) + " Liter.";
  }

  if (fuite1 && fuite2) {
    leakReason = leakDetail1 + "<br>" + leakDetail2;
  } else if (fuite1) {
    leakReason = leakDetail1;
  } else if (fuite2) {
    leakReason = leakDetail2;
  }
}


bool alarmActive() {
  return (fuite1 || fuite2);
}


bool sendAlertEmail() {
  if (!smtpClient.connect(smtpHost, smtpPort)) {
    Serial.println("SMTP connect failed");
    return false;
  }

  if (!smtpReadResponse(220)) return false;

  smtpClient.print("EHLO esp32\r\n");
  if (!smtpReadResponse(250)) return false;

  smtpClient.print("AUTH LOGIN\r\n");
  if (!smtpReadResponse(334)) return false;

  smtpClient.print(base64Encode(smtpUser) + "\r\n");
  if (!smtpReadResponse(334)) return false;

  smtpClient.print(base64Encode(smtpPass) + "\r\n");
  if (!smtpReadResponse(235)) return false;

  smtpClient.print("MAIL FROM:<");
  smtpClient.print(mailFrom);
  smtpClient.print(">\r\n");
  if (!smtpReadResponse(250)) return false;

  smtpClient.print("RCPT TO:<");
  smtpClient.print(mailTo);
  smtpClient.print(">\r\n");
  if (!smtpReadResponse(250)) return false;

  smtpClient.print("DATA\r\n");
  if (!smtpReadResponse(354)) return false;

  String subject = "Wasserleck Alarm";
  String body = "Alarm des Wasserverbrauchs-Monitors.\r\n";
  if (fuite1) body += "Lecktyp 1: Permanenter Verbrauch erkannt, es gibt keinen zweistündigen Zeitraum ohne Verbrauch während der letzten 24 Stunden.\r\n";
  if (fuite2) body += "Lecktyp 2: Der Verbrauch während der letzten 24 Stunden liegt über dem angegebenen Schwellenwert (" + String(W_Threshold) + " Liter). 24h: " + String(conso24) + " Liter.\r\n";
  body += "Zeit: " + GetTime() + "\r\n";

  smtpClient.print("From: ");
  smtpClient.print(mailFrom);
  smtpClient.print("\r\nTo: ");
  smtpClient.print(mailTo);
  smtpClient.print("\r\nSubject: ");
  smtpClient.print(subject);
  smtpClient.print("\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n");
  smtpClient.print(body);
  smtpClient.print("\r\n.\r\n");

  bool ok = smtpReadResponse(250);
  smtpClient.print("QUIT\r\n");
  smtpClient.stop();
  return ok;
}


//----------------------------------------------------------------------
// OLED overlays and frames
//----------------------------------------------------------------------

// Time displayed on the 3 pages of the OLED display (overlay)
//----------------------------------------------------------------------
void msOverlay(OLEDDisplay *display, OLEDDisplayUiState* state) {
  display->setTextAlignment(TEXT_ALIGN_RIGHT);
  display->setFont(ArialMT_Plain_10);
  display->drawString(128, 0, heure);
}


// Page 1
//----------------------------------------------------------------------
void drawFrame1(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  char line[20] = "xxx liter";
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->setFont(ArialMT_Plain_16);
  display->drawString(64 + x, 10 + y, jour);

  if ((Total_Counter - Day_Counter) < 1000) {
    sprintf(line, "%03d liter", (Total_Counter - Day_Counter));
  } else {
    sprintf(line, "%8.3f m3", (Total_Counter - Day_Counter) / 1000.);
  }
  display->drawString(64 + x, 26 + y, line);

  if (!fuite1 && !fuite2) {
    display->drawString(64 + x, 42 + y, "Kein Leck");
  } else {
    display->drawString(64 + x, 42 + y, "<< Leck >>");
  }
}


// Page 2
//----------------------------------------------------------------------
void drawFrame2(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  char line[20] = "xxx 9999.999 m3";
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->setFont(ArialMT_Plain_16);

  float value = Total_Counter / 1000.;
  sprintf(line, "Tot. %8.3f m3", value);
  display->drawString(0 + x, 10 + y, line);

  value = Last_Counter / 1000.;
  sprintf(line, "Abl. %8.3f m3", value);
  display->drawString(0 + x, 26 + y, line);

  Difference = (Total_Counter - Last_Counter) / 1000.;
  sprintf(line, "Diff. %8.3f m3", Difference);
  display->drawString(0 + x, 42 + y, line);
}


// Page 3
//----------------------------------------------------------------------
void drawFrame3(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  String IPaddress = WiFi.localIP().toString();
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->setFont(ArialMT_Plain_16);
  display->drawString(0 + x, 10 + y, "WiFi");
  display->setFont(ArialMT_Plain_10);

  if (WiFi.status() != WL_CONNECTED) {
    display->drawString(0 + x, 26 + y, "nicht verbunden");
  } else {
    display->drawString(0 + x, 26 + y, ssid);
    display->drawString(0 + x, 42 + y, IPaddress);
  }
}


// General configuration of the display
//--------------------------------------
// This table contains pointers to the functions displaying the 3 sliding pages
FrameCallback frames[] = { drawFrame1, drawFrame2, drawFrame3 };
int frameCount = 3;
OverlayCallback overlays[] = { msOverlay };
int overlaysCount = 1;


//----------------------------------------------------------------------
// Interrupt
//----------------------------------------------------------------------

// interrupt subroutine to count pulses
// the emitter pulses are 500ms long, so the software hold pulses of less than 900ms (spurious)
//---------------------------------------------------------------------------------------------
void IRAM_ATTR handleInterrupt() {
  static unsigned long last_interrupt = 0;
  unsigned long interrupt = millis();
  if ((interrupt - last_interrupt) > 600) {
    portENTER_CRITICAL_ISR(&mux);
    Total_Counter++;
    portEXIT_CRITICAL_ISR(&mux);
    last_interrupt = interrupt;
  }
}


//----------------------------------------------------------------------
// Time helpers
//----------------------------------------------------------------------

void StartTime() {
  configTime(0, 0, "0.pool.ntp.org", "1.pool.ntp.org", "2.pool.ntp.org");
  setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 3);
  tzset();
  delay(200);
}


void readLocalTime() {
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time");
    StartTime();
  }
}


String GetTime() {
  readLocalTime();
  char output[50];
  strftime(output, 50, "%d.%m.%y um %H:%M:%S Uhr", &timeinfo);
  return output;
}


//----------------------------------------------------------------------
// HTML helpers
//----------------------------------------------------------------------

void append_HTML_header() {
  webpage = "";
  webpage += "<!DOCTYPE html><html><head>";
  webpage += "<meta http-equiv='refresh' content='60'/>";
  webpage += "<meta http-equiv='Content-Type' content='text/html; charset=UTF-8' />";
  webpage += "<meta http-equiv='Cache-Control' content='no-cache, no-store, must-revalidate' />";
  webpage += "<meta http-equiv='Expires' content='0' />";
  webpage += "<meta http-equiv='pragma' content='no-cache' />";

  webpage += "<style>";
  webpage += "body {width:" + String(sitewidth) + "px;margin:0 auto;font-family:arial;font-size:14px;text-align:left;color:blue;background-color:white;}";
  webpage += "h1 {background-color:white;margin:16px 0px;}";
  webpage += "h3 {color:blue;font-size:24px;width:auto;}";
  webpage += ".navbar{overflow:hidden;background-color:blue;position:fixed;top:0;width:" + String(sitewidth) + "px;margin-left:0px;}";
  webpage += ".navbar a {float:left;display:block;color:white;text-align:left;padding:10px 12px;text-decoration:none;font-size:17px;}";
  webpage += ".main{padding:0px;margin:16px 0px;height:1000px;width:" + String(sitewidth) + "px;}";
  webpage += ".style1{text-align:center;font-size:16px;background-color:white;}";
  webpage += ".style2{text-align:left;font-size:16px;background-color:white;width:auto;margin:0 auto;}";
  webpage += "</style>";

  webpage += "</head><body>";
  webpage += "<br><title>" + sitetitle + "</title><br>";
  webpage += "<div class='main'><h1>" + siteheading + " " + siteversion + "</h1>";
  webpage += "<div class='navbar'>";
  webpage += " <a href='/user'>Übersicht</a>";
  webpage += " <a href='/userinput'>Konfiguration</a>";
  webpage += " <a href='/about'>Über</a>";
  webpage += "</div>";
}


void append_HTML_footer() {
  webpage += "<footer><p class='style1'>" + yourfootnote + "<br>";
  webpage += "&copy; Denis Lafourcade 2019<br>";
  webpage += "<span style='display:block; margin-top:4px;'>Debugging und Weiterentwicklung: Hans Schneider 2026</span>";
  webpage += "</p></footer>";
  webpage += "</div></body></html>";
}


//----------------------------------------------------------------------
// Web pages
//----------------------------------------------------------------------

boolean isValidNumber(String str) {
  str.trim();
  if (str.length() == 0) return false;
  if (!(str.charAt(0) == '+' || str.charAt(0) == '-' || isDigit(str.charAt(0)))) return false;
  for (byte i = 1; i < str.length(); i++) {
    if (!(isDigit(str.charAt(i)) || str.charAt(i) == '.')) return false;
  }
  return true;
}


//----------------------------------------------------------------------
void handleNotFound() {
  String message = "Page not found!\n";
  server.send(404, "text/plain", message);
}


//----------------------------------------------------------------------
void About() {
  append_HTML_header();
  webpage += "<H3>Wasserverbrauchs-Monitor</H3>";
  webpage += "<p class='style2'>";
  webpage += "Diese Seite erlaubt die individuelle Kontrolle des Wasserzählers. Nach Eingabe des laufenden Wertes des Wasserzählers, ";
  webpage += "des Wertes der letzten Ablesung und der Alarmschwelle addiert das System die Impulse, die vom Sensor auf dem ";
  webpage += "Wasserzähler empfangen werden (kapazitiver Sensor) und zeigt permanent an:<br><br>";
  webpage += "- das Datum und den täglichen Verbrauch<br>";
  webpage += "- den laufenden Wert des Wasserzählers<br>";
  webpage += "- den Verbrauch seit der letzten Ablesung<br>";
  webpage += "- die Verbindung zum WiFi Netzwerk mit der IP Addresse<br><br>";
  webpage += "Weiterhin diagnostiziert das System durch die Analyse des stündlichen Verbrauchs während der letzten 24 Stunden ";
  webpage += "die Möglichkeit eines Lecks. Kriterien dafür sind:<br><br>";
  webpage += "- es gibt keinen zweistündigen Zeitraum ohne Verbrauch während der letzten 24 Stunden (Leck 1)<br>";
  webpage += "- der Verbrauch während der letzten 24 Stunden liegt über dem angegebenen Schwellenwert (Leck 2)<br><br>";
  webpage += "Im Falle eines Lecks wird eine Benachrichtigung per E-Mail über SMTP2GO geschickt.";
  webpage += "</p><br>";
  webpage += "<p>Am " + GetTime() + "</p>";
  String Uptime = (String(millis() / 1000 / 60 / 60)) + ":";
  Uptime += (((millis() / 1000 / 60 % 60) < 10) ? "0" + String(millis() / 1000 / 60 % 60) : String(millis() / 1000 / 60 % 60)) + ":";
  Uptime += ((millis() / 1000 % 60) < 10) ? "0" + String(millis() / 1000 % 60) : String(millis() / 1000 % 60);
  webpage += "<p>Betriebszeit: " + Uptime + "</p>";
  webpage += "Version 1.7.2 - 27. Juli 2026<br>";
  webpage += "</p>";
  append_HTML_footer();
  server.send(200, "text/html", webpage);
}


//----------------------------------------------------------------------
// Shared state helpers
//----------------------------------------------------------------------

void syncRuntimeFromInputs() {
  field1 = (float)(Total_Counter / 1000.);
  field2 = (float)(Last_Counter / 1000.);
  field3 = W_Threshold;
}


void saveStateToNVS() {
  prefs.begin("Compteur", false);
  prefs.putInt("Total_Counter", Total_Counter);
  prefs.putInt("Last_Counter", Last_Counter);
  prefs.putInt("Day_Counter", Day_Counter);
  prefs.putBytes("Conso", &conso, 96);
  prefs.putInt("nb_SMS", nb_SMS);
  prefs.putInt("W_Threshold", W_Threshold);
  prefs.end();
}


bool applyFormUpdates() {
  bool changed = false;

  String field1_response = String(field1, 3);
  String field2_response = String(field2, 3);
  String field3_response = String(field3);

  if (server.args() > 0) {
    for (uint8_t i = 0; i < server.args(); i++) {
      String Argument_Name = server.argName(i);
      String client_response = server.arg(i);
      if (Argument_Name == "field1") field1_response = client_response;
      if (Argument_Name == "field2") field2_response = client_response;
      if (Argument_Name == "field3") field3_response = client_response;
    }

    field1_response.trim();
    field2_response.trim();
    field3_response.trim();

    if (isValidNumber(field1_response)) {
      float newField1 = field1_response.toFloat();
      if (newField1 != field1) {
        field1 = newField1;
        Total_Counter = round(field1 * 1000.);
        Day_Counter = Total_Counter;
        Prec_Counter = Total_Counter;
        changed = true;
      }
    }

    if (isValidNumber(field2_response)) {
      float newField2 = field2_response.toFloat();
      if (newField2 != field2) {
        field2 = newField2;
        Last_Counter = round(field2 * 1000.);
        changed = true;
      }
    }

    if (isValidNumber(field3_response)) {
      int newField3 = field3_response.toInt();
      if (newField3 != field3) {
        field3 = newField3;
        W_Threshold = field3;
        changed = true;
      }
    }
  }

  return changed;
}


//----------------------------------------------------------------------
void Saisie() {
  if (!server.authenticate(www_username, www_password)) {
    server.requestAuthentication();
    return;
  }

  syncRuntimeFromInputs();
  bool changed = applyFormUpdates();
  if (changed) {
    saveStateToNVS();
    lastNvsSave = millis();
    stateDirty = false;
  }

  append_HTML_header();
  webpage += "<h3>Geben Sie einen oder mehrere Werte ein und klicken Sie auf 'Eingabe'</h3>";
  webpage += "<form method=POST>";
  webpage += "<table style='font-family:arial,sans-serif;font-size:16px;border-collapse:collapse;text-align:center;width:90%;margin-left:auto;margin-right:auto;'>";
  webpage += "<tr>";
  webpage += "<th style='border:0px solid black;text-align:left;padding:2px;'>Laufender Wert des Zählers (m3)</th>";
  webpage += "<th style='border:0px solid black;text-align:left;padding:2px;'>Letzter Ablesewert (m3)</th>";
  webpage += "<th style='border:0px solid black;text-align:left;padding:2px;'>Alarmschwelle 24 Stunden  (l)</th>";
  webpage += "</tr>";
  webpage += "<tr>";
  webpage += "<td style='border:0px solid black;text-align:left;padding:2px;'><input type='text' name='field1' value=" + String(field1, 3) + "></td>";
  webpage += "<td style='border:0px solid black;text-align:left;padding:2px;'><input type='text' name='field2' value=" + String(field2, 3) + "></td>";
  webpage += "<td style='border:0px solid black;text-align:left;padding:2px;'><input type='text' name='field3' value=" + String(field3) + "></td>";
  webpage += "</tr>";
  webpage += "</table><br><br>";
  webpage += "<br><br><input type='submit' value='Eingabe'><br><br>";
  webpage += "</form></body>";
  append_HTML_footer();

  server.send(200, "text/html", webpage);
}


void Visualisation() {
  float total_Conso = 0.;
  int jour_Conso = 0;

  syncRuntimeFromInputs();

  total_Conso = (float)((Total_Counter - Last_Counter) / 1000.);
  jour_Conso = Total_Counter - Day_Counter;

  append_HTML_header();
  webpage += "<meta http-equiv='refresh' content='10'/>";
  webpage += "<h3>Laufende Werte vom " + GetTime() + "</h3>";
  webpage += "<p class='style2'> </p>";
  webpage += "<table border='1' cellspacing='0' cellpading='10' width='40%'>";
  webpage += "<tr><td style='background-color:cyan'>Gesamtzählerstand</td><td align='right'>" + String(field1, 3) + " m3</td></tr>";
  webpage += "<tr><td style='background-color:cyan'>Letzte Ablesung</td><td align='right'>" + String(field2, 3) + " m3</td></tr>";
  webpage += "<tr><td style='background-color:cyan'>Verbrauch seit Ablesung</td><td align='right'>" + String(total_Conso, 3) + " m3</td></tr>";
  webpage += "<tr><td style='background-color:cyan'>Täglicher Zähler</td><td align='right'>" + String(jour_Conso) + " liter</td></tr>";
  webpage += "</table><br>";
  webpage += "<p style='font-size:17px'> Verbrauch während der letzten 24 Stunden: </p>";
  webpage += "<table border='1' cellspacing='0' cellpading='10' width='100%'>";
  webpage += "<tr align='center' style='background-color:cyan'><td>00h-01h</td><td>01h-02h</td><td>02h-03h</td><td>03h-04h</td><td>04h-05h</td><td>05h-06h</td>";
  webpage += "<td>06h-07h</td><td>07h-08h</td><td>08h-09h</td><td>09h-10h</td><td>10h-11h</td><td>11h-12h</td></tr>";
  webpage += "<tr align='right'><td>" + String(conso[0]) + "</td><td>" + String(conso[1]) + "</td><td>" + String(conso[2]) + "</td>";
  webpage += "<td>" + String(conso[3]) + "</td><td>" + String(conso[4]) + "</td><td>" + String(conso[5]) + "</td>";
  webpage += "<td>" + String(conso[6]) + "</td><td>" + String(conso[7]) + "</td><td>" + String(conso[8]) + "</td>";
  webpage += "<td>" + String(conso[9]) + "</td><td>" + String(conso[10]) + "</td><td>" + String(conso[11]) + "</td></tr><br>";
  webpage += "<tr align='center' style='background-color:cyan'><td>12h-13h</td><td>13h-14h</td><td>14h-15h</td><td>15h-16h</td><td>16h-17h</td><td>17h-18h</td>";
  webpage += "<td>18h-19h</td><td>19h-20h</td><td>20h-21h</td><td>21h-22h</td><td>22h-23h</td><td>23h-24h</td></tr>";
  webpage += "<tr align='right'><td>" + String(conso[12]) + "</td><td>" + String(conso[13]) + "</td><td>" + String(conso[14]) + "</td>";
  webpage += "<td>" + String(conso[15]) + "</td><td>" + String(conso[16]) + "</td><td>" + String(conso[17]) + "</td>";
  webpage += "<td>" + String(conso[18]) + "</td><td>" + String(conso[19]) + "</td><td>" + String(conso[20]) + "</td>";
  webpage += "<td>" + String(conso[21]) + "</td><td>" + String(conso[22]) + "</td><td>" + String(conso[23]) + "</td></tr>";
  webpage += "</table><br>";

//  webpage += "<h2>Diagnose: </h2>";
//  webpage += "<class='style2'>" + leakReason;
//  webpage += leakReason;
//  webpage += "<h2>Diagnose: " + leakReason + "</h2>";
  webpage += "<h2>Diagnose: <span style='font-size: 0.8em; font-weight: normal;'>" + leakReason + "</span></h2>";
  webpage += "</p><br>";

  if (fuite1 || fuite2) {
//    webpage += "<h3><br><p align='center'>--- WARNUNG ---</p></h3>";
    webpage += "<h3><p align='center'>--- WARNUNG ---</p></h3>";
    webpage += "<h3><p align='center'>--- Mögliches Leck - Überprüfen Sie Ihre Hausinstallation ---</p></h3><br>";
  }
  append_HTML_footer();
  server.send(200, "text/html", webpage);
}


//----------------------------------------------------------------------
// Setup
//----------------------------------------------------------------------

void setup() 
{
  Serial.begin(115200);
  Serial.println();
  Serial.println();


  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  uint8_t i = 0;
  while (WiFi.status() != WL_CONNECTED && i < 60)  // wait 30s max the wifi connection
  {
    delay(500);
    Serial.print(".");
    i++;
  }
  Serial.println("");
  Serial.println("WiFi connected.");
  Serial.println(WiFi.localIP());


  // initialise and read time
  StartTime();
  readLocalTime();


  //initialise interrupts management
  pinMode(interruptPin, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(interruptPin), handleInterrupt, FALLING);


  ui.setTargetFPS(25);                    // 25 frames/s. Display update measured at 4ms, so 36ms left for the rest
  ui.setActiveSymbol(activeSymbol);
  ui.setInactiveSymbol(inactiveSymbol);
  ui.setIndicatorPosition(BOTTOM);
  ui.setIndicatorDirection(LEFT_RIGHT);
  ui.setFrameAnimation(SLIDE_LEFT);
  ui.setFrames(frames, frameCount);
  ui.setOverlays(overlays, overlaysCount);
  ui.init();
  display.flipScreenVertically();         // otherwise downunder


  server.on("/", Visualisation);          // default page
  server.on("/user", Visualisation);      // current values page
  server.on("/userinput", Saisie);        // config page
  server.on("/about", About);             // about page
  server.onNotFound(handleNotFound);      // error
  server.begin();


  // Read EEPROM stored values if any, default if first time
  prefs.begin("Compteur", false);
  Total_Counter = prefs.getInt("Total_Counter", 0);
  Last_Counter  = prefs.getInt("Last_Counter", 0);
  Day_Counter   = prefs.getInt("Day_Counter", 0);
  uint8_t nB    = prefs.getBytes("Conso", &conso, 96);
  nb_SMS        = prefs.getInt("nb_SMS", 0);
  W_Threshold   = prefs.getInt("W_Threshold", 800);
  prefs.end();


  Prec_Counter = Total_Counter;


  // Initialise OTA
  //---------------
  ArduinoOTA.setPort(8266);
  ArduinoOTA.setHostname("Waterflow");
  ArduinoOTA.setPasswordHash("set password hash here"); // use e.g. https://hash-generieren.de/


  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else // U_SPIFFS
      type = "filesystem";
    Serial.println("Starting update" + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if      (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });


  ArduinoOTA.begin();
}


//----------------------------------------------------------------------
// Loop
//----------------------------------------------------------------------

void loop() {
  ui.update();


  readLocalTime();
  hours = timeinfo.tm_hour;
  mins  = timeinfo.tm_min;
  secs  = timeinfo.tm_sec;
  qtm   = timeinfo.tm_mday;
  mois  = timeinfo.tm_mon + 1;
  an    = timeinfo.tm_year + 1900;


  sprintf(heure, "%02d:%02d:%02d", hours, mins, secs);
  sprintf(jour, "%02d.%02d.%02d", qtm, mois, an);


  // WebServer handling
  server.handleClient();


  // OTA handling
  ArduinoOTA.handle();


  // Hourly counter incrementation
  // Hourly counter incrementation
  if (Prec_Counter != Total_Counter) {
    conso[hours] += Total_Counter - Prec_Counter;
    Prec_Counter = Total_Counter;
    stateDirty = true;
  }

  if (stateDirty && (millis() - lastNvsSave >= nvsSaveInterval)) {
    saveStateToNVS();
    lastNvsSave = millis();
    stateDirty = false;
  }


  // Every minute : check WiFi connection and try to reconnect if needed
  //--------------------------------------------------------------------
  if (secs == 0 && !WiFi_Checked) {
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.mode(WIFI_STA);
      WiFi.begin(ssid, password);
    }
    WiFi_Checked = true;
  }
  if (secs > 0 && WiFi_Checked) WiFi_Checked = false;


  // Every hour : compute hourly consumption, analyse 24hr consumption for leakage detection,
  // NTP update, NVS (Non Volatile Storage) backup of essential data,
  // re-init display, which from time to time freezes for no known reason
  //-----------------------------------------------------------------------
  if (mins == 0 && !save_conso) {
    save_conso = true;

    buildLeakReport();

    // Save main data in NVS before modifying the current hour bucket
    saveStateToNVS();
    lastNvsSave = millis();
    stateDirty = false;

    // Send alert after report is complete, so mail and web report stay consistent
    if (alarmActive() && nb_SMS < 10 && !Day_SMS) {
      if (sendAlertEmail()) {
        Serial.println("Email sent");
        nb_SMS++;
        Day_SMS = true;
      } else {
        Serial.println("Email sending failed");
      }
    }

    // reset of the consumption of the incoming hour
    conso[hours] = 0;
    stateDirty = true;

    // Reinit display to cure random freezes
    ui.init();
    display.flipScreenVertically();
  }


  if (mins != 0 && save_conso) save_conso = false;


  // Every day : Reset daily counter at midnight
  //-------------------------------------------------------
  if (hours == 0 && Day_Reset) {
    Day_Counter = Total_Counter;
    Day_Reset = false;
    Day_SMS = false;
  } else if (hours > 0) {
    Day_Reset = true;
  }


  // Every month : reset SMS (email) number (10 max for IFTTT)
  //--------------------------------------------------
  if (qtm == 1 && !SMS_Reset) {
    nb_SMS = 0;
    SMS_Reset = true;
  }
  if (qtm > 1) SMS_Reset = false;
}