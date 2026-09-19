#include <WiFi.h>
#include <WebServer.h>

WebServer server(80);

const int LED_PIN = 8;      // C3 SuperMini 板载 LED；S3 改成 48

bool ledOn = false;

// ---------- 首页 ----------
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>ESP32</title></head>";
  html += "<body style='text-align:center;font-family:sans-serif;padding-top:50px'>";
  html += "<h1>ESP32 SuperMini</h1>";
  html += "<h2>LED: ";
  html += ledOn ? "<span style='color:green'>ON</span>" : "<span style='color:red'>OFF</span>";
  html += "</h2>";
  html += "<p><a href='/toggle'><button style='font-size:24px;padding:15px 40px'>";
  html += ledOn ? "TURN OFF" : "TURN ON";
  html += "</button></a></p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

// ---------- 切换 LED ----------
void handleToggle() {
  ledOn = !ledOn;
  digitalWrite(LED_PIN, ledOn ? LOW : HIGH);   // 低电平点亮
  server.sendHeader("Location", "/");
  server.send(303);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // 板载 LED 初始化，默认熄灭
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);   // 高电平 = 灭

  // 开启热点
  WiFi.softAP("ESP32", "12345678");
  Serial.println("AP started");
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());   // 192.168.4.1

  // 注册路由
  server.on("/", handleRoot);
  server.on("/toggle", handleToggle);
  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();
}