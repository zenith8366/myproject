int led = 3;
int button = 2;   // 建议拼写改成 button

void setup() {
  pinMode(led, OUTPUT);
  pinMode(button, INPUT_PULLUP);  // 关键：启用内部上拉
}

void loop() {
  if (digitalRead(button) == LOW) {
    digitalWrite(led, HIGH);   // 按下按钮，LED 亮
  } else {
    digitalWrite(led, LOW);    // 松开按钮，LED 灭
  }
}
