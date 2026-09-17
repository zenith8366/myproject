/*
  Genshin Impact Main Theme - 单音蜂鸣器版
  来源：用户上传的三页《原神主题曲》简谱

  简谱信息：
    1 = D
    3/4
    ♩ = 82

  硬件：
    Button        -> D2 与 GND
    Passive Buzzer -> D8 与 GND

  按键：
    按下一次 -> 从头播放
    使用 INPUT_PULLUP，不需要外接上拉电阻

  说明：
    - 只取简谱上方旋律声部，不播放下方钢琴伴奏。
    - 每一个音符都直接绑定毫秒时长。
    - 连音/延音通过较长的 duration 实现。
    - 简谱中的装饰音用短时值近似。
*/

#define BUTTON_PIN 2
#define BUZZER_PIN 8

// --------------------
// 音符频率
// 1=D
// --------------------
#define REST 0

#define D4   294
#define E4   330
#define FS4  370
#define G4   392
#define A4   440
#define B4   494
#define CS5  554

#define D5   587
#define E5   659
#define FS5  740
#define G5   784
#define A5   880
#define B5   988
#define CS6  1109
#define D6   1175

// --------------------
// 82 BPM 对应时值
// 四分音符 = 60000 / 82 = 731.7 ms
// --------------------
#define Q     732
#define E     366
#define S     183
#define DQ    1098
#define H     1464
#define DH    2196

// 简谱装饰音
#define GRACE 60

struct Note {
  uint16_t freq;
  uint16_t duration;
};

// ============================================================
// 上方旋律声部
// 每一项 = {音高, 毫秒}
// ============================================================
const Note melody[] = {

  // ====== 01-04 ======
  {REST, Q}, {REST, Q}, {REST, Q},
  {REST, Q}, {REST, Q}, {REST, Q},
  {REST, Q}, {REST, Q}, {REST, Q},
  {REST, Q}, {REST, Q}, {REST, Q},

  // ====== 05 ======
  {G5, Q}, {REST, Q}, {A5, E}, {B5, E},

  // ====== 06 ======
  {CS6, Q}, {REST, Q},
  {D6, GRACE}, {CS6, GRACE}, {B5, Q - GRACE * 2},
  {A5, E},

  // ====== 07 ======
  {B5, Q}, {REST, Q}, {A5, E}, {G5, E},

  // ====== 08 ======
  {A5, Q}, {E5, Q}, {REST, Q},

  // ====== 09 ======
  {G5, Q}, {REST, Q}, {G5, E}, {A5, E},

  // ====== 10 ======
  {FS5, Q},
  {G5, GRACE}, {FS5, GRACE}, {E5, DQ - GRACE * 2},
  {D5, E},

  // ====== 11 ======
  {E5, Q}, {B5, Q}, {REST, Q},

  // ====== 12 ======
  {REST, Q}, {REST, Q}, {D5, Q},

  // ====== 13 ======
  {G5, Q}, {REST, Q}, {A5, E}, {B5, E},

  // ====== 14 ======
  {CS6, Q}, {REST, Q},
  {D6, GRACE}, {CS6, GRACE}, {B5, Q - GRACE * 2},
  {A5, E},

  // ====== 15 ======
  {B5, DQ}, {A5, E}, {A5, E}, {G5, E},

  // ====== 16 ======
  {A5, Q}, {E5, H},

  // ====== 17 ======
  {G5, Q}, {REST, Q},
  {B5, GRACE}, {A5, E - GRACE}, // 6装饰音 -> 5
  {G5, E},

  // ====== 18 ======
  {FS5, Q}, {E5, DQ}, {D5, E},

  // ====== 19 ======
  {E5, DH},

  // ====== 20 ======
  // 2 延续后接 6,7
  {E5, H}, {B5, E}, {CS6, E},

  // ====== 21 ======
  {D5, Q}, {REST, Q}, {D5, E}, {E5, E},

  // ====== 22 ======
  {CS6, Q}, {B5, Q}, {A5, Q},

  // ====== 23 ======
  // 22末尾5 -> 23开头5，按连音处理
  {A5, H}, {B5, Q}, {FS5, Q},

  // ====== 24 ======
  {CS6, Q}, {D6, GRACE}, {CS6, GRACE},
  {B5, Q - GRACE * 2}, {A5, Q},

  // ====== 25 ======
  {B5, Q}, {REST, Q}, {A5, E}, {G5, E},

  // ====== 26 ======
  {FS5, Q}, {REST, Q},
  {G5, GRACE}, {FS5, GRACE}, {E5, Q - GRACE * 2},
  {D5, E},

  // ====== 27 ======
  {E5, DH},

  // ====== 28 ======
  {E5, H}, {B5, E}, {CS6, E},

  // ====== 29 ======
  {D5, Q}, {REST, Q}, {D5, E}, {E5, E},

  // ====== 30 ======
  {CS6, Q}, {B5, Q}, {A5, Q},

  // ====== 31 ======
  {A5, Q}, {B5, Q}, {A5, Q},

  // ====== 32 ======
  {CS6, Q},
  {D6, GRACE}, {CS6, GRACE}, {B5, Q - GRACE * 2},
  {A5, Q},

  // ====== 33 ======
  {B5, DQ}, {A5, E}, {A5, E}, {G5, E},

  // ====== 34 ======
  {FS5, Q},
  {G5, GRACE}, {FS5, GRACE}, {E5, DQ - GRACE * 2},
  {D5, E},

  // ====== 35 ======
  {E5, DH},

  // ====== 36 ======
  {REST, Q}, {REST, Q}, {D5, Q},

  // ====== 37 ======
  {G5, Q}, {REST, Q}, {A5, E}, {B5, E},

  // ====== 38 ======
  {CS6, Q}, {REST, Q},
  {D6, GRACE}, {CS6, GRACE}, {B5, Q - GRACE * 2},
  {A5, E},

  // ====== 39 ======
  {B5, Q}, {REST, Q}, {A5, E}, {G5, E},

  // ====== 40 ======
  {A5, Q}, {E5, Q}, {REST, Q},

  // ====== 41 ======
  {G5, Q}, {REST, Q},
  {B5, GRACE}, {A5, E - GRACE},
  {G5, E},

  // ====== 42 ======
  {FS5, Q}, {E5, DQ}, {D5, E},

  // ====== 43-44 ======
  {E5, Q},
  {B5, H},       // 43末6 -> 44开6 连音
  {REST, Q},
  {D5, Q},

  // ====== 45 ======
  {G5, Q}, {REST, Q}, {A5, E}, {B5, E},

  // ====== 46 ======
  {CS6, Q}, {REST, Q},
  {D6, GRACE}, {CS6, GRACE}, {B5, Q - GRACE * 2},
  {A5, E},

  // ====== 47 ======
  {B5, Q}, {REST, Q}, {A5, E}, {G5, E},

  // ====== 48 ======
  {A5, Q}, {E5, Q}, {REST, Q},

  // ====== 49 ======
  {G5, Q}, {REST, Q}, {G5, E}, {A5, E},

  // ====== 50 ======
  {FS5, Q},
  {G5, GRACE}, {FS5, GRACE}, {E5, DQ - GRACE * 2},
  {D5, E},

  // ====== 51-52 ======
  // 最后一个2跨小节延长，按简谱连音合并成持续音
  {E5, DH + DH}
};

const uint16_t melodyLength =
  sizeof(melody) / sizeof(melody[0]);


// ============================================================
// 播放一个音符
// ============================================================
void playNote(uint16_t frequency, uint16_t durationMs)
{
  if (frequency == REST) {
    noTone(BUZZER_PIN);
    delay(durationMs);
    return;
  }

  tone(BUZZER_PIN, frequency);
  delay(durationMs);
  noTone(BUZZER_PIN);
}


// ============================================================
// 播放整首
// ============================================================
void playMelody()
{
  for (uint16_t i = 0; i < melodyLength; i++) {
    playNote(
      melody[i].freq,
      melody[i].duration
    );
  }

  noTone(BUZZER_PIN);
}


// ============================================================
// 按键：D2 -> 按钮 -> GND
// ============================================================
bool lastButtonState = HIGH;

void setup()
{
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);

  noTone(BUZZER_PIN);
}


void loop()
{
  bool currentButtonState = digitalRead(BUTTON_PIN);

  // 检测从 HIGH -> LOW，即刚刚按下
  if (lastButtonState == HIGH &&
      currentButtonState == LOW)
  {
    delay(20);  // 消抖

    if (digitalRead(BUTTON_PIN) == LOW)
    {
      playMelody();
    }
  }

  lastButtonState = currentButtonState;
}