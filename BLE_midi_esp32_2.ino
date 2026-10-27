#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <math.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include <BLEMIDI_Transport.h>
#include <hardware/BLEMIDI_ESP32.h>
#include <MIDI.h>

// =====================================================
// BLE MIDI
// =====================================================

BLEMIDI_CREATE_INSTANCE("ESP33", MIDI);

// =====================================================
// OLED SSD1306
// =====================================================

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET   -1
#define OLED_ADDRESS 0x3C

Adafruit_SSD1306 display(
  SCREEN_WIDTH,
  SCREEN_HEIGHT,
  &Wire,
  OLED_RESET
);

// =====================================================
// ИСХОДНЫЕ GPIO — НЕ ИЗМЕНЕНЫ
// =====================================================

// Потенциометры
const uint8_t potPins[6] = {
  39, 34, 35, 32, 33, 27
};

// Кнопки
const uint8_t buttonPins[8] = {
  13, 19, 23, 18, 5, 17, 16, 4
};

// Кнопки навигации меню
const uint8_t BUTTON_UP_INDEX   = 0; // GPIO 13
const uint8_t BUTTON_DOWN_INDEX = 1; // GPIO 19

// =====================================================
// НАСТРОЙКИ ПОТЕНЦИОМЕТРОВ
// =====================================================

// Коэффициент IIR-фильтра.
// Меньше значение = сильнее сглаживание, но медленнее реакция.
const float FILTER_ALPHA = 0.20f;

// Гистерезис в единицах ADC.
const float ADC_HYSTERESIS = 8.0f;

// Минимальное изменение MIDI CC перед отправкой.
const uint8_t MIDI_CC_THRESHOLD = 2;

// Период опроса потенциометров.
const uint32_t POT_POLL_INTERVAL_MS = 5;

// =====================================================
// НАСТРОЙКИ КНОПОК
// =====================================================

const uint32_t DEBOUNCE_MS = 25;
const uint32_t MENU_HOLD_MS = 2000;

// =====================================================
// НАСТРОЙКИ MIDI ПО УМОЛЧАНИЮ
// =====================================================

// Эти значения являются настройками по умолчанию.
// Они не были заданы в первоначальном исходном коде.
uint8_t potCC[6] = {
  20, 21, 22, 23, 24, 25
};

uint8_t buttonNote[8] = {
  36, 37, 38, 39, 40, 41, 42, 43
};

uint8_t midiChannel = 1;
uint8_t midiVelocity = 100;

// =====================================================
// ФИЛЬТРАЦИЯ ПОТЕНЦИОМЕТРОВ
// =====================================================

float filteredADC[6] = {0};
float acceptedADC[6] = {0};

uint8_t lastSentCC[6] = {
  255, 255, 255, 255, 255, 255
};

bool potInitialized[6] = {
  false, false, false, false, false, false
};

uint32_t lastPotPoll = 0;

// =====================================================
// СОСТОЯНИЕ КНОПОК
// =====================================================

bool rawButtonState[8] = {false};
bool stableButtonState[8] = {false};

uint32_t lastDebounceTime[8] = {0};
uint32_t buttonPressTime[8] = {0};

// Кнопка UP была удержана до входа в меню.
bool upHoldConsumed = false;

// =====================================================
// СОСТОЯНИЕ МЕНЮ
// =====================================================

enum OperatingMode {
  MODE_MIDI,
  MODE_MENU
};

OperatingMode operatingMode = MODE_MIDI;

enum MenuPage {
  PAGE_MAIN,
  PAGE_MIDI_CHANNEL,
  PAGE_POT_CC,
  PAGE_BUTTON_NOTE
};

MenuPage menuPage = PAGE_MAIN;

uint8_t menuSelection = 0;
uint8_t selectedPot = 0;
uint8_t selectedButton = 0;

// Временные настройки, редактируемые в меню.
uint8_t editMidiChannel = 1;
uint8_t editPotCC[6];
uint8_t editButtonNote[8];

// Состояние для отображения экрана.
bool oledAvailable = false;
uint32_t lastDisplayUpdate = 0;

// =====================================================
// СОХРАНЕНИЕ НАСТРОЕК
// =====================================================

Preferences preferences;

void loadSettings() {
  preferences.begin("ble-midi", true);

  midiChannel = preferences.getUChar("channel", 1);

  if (midiChannel < 1 || midiChannel > 16) {
    midiChannel = 1;
  }

  for (uint8_t i = 0; i < 6; i++) {
    char key[12];
    snprintf(key, sizeof(key), "potcc%u", i);

    potCC[i] = preferences.getUChar(
      key,
      20 + i
    );

    if (potCC[i] > 127) {
      potCC[i] = 20 + i;
    }
  }

  for (uint8_t i = 0; i < 8; i++) {
    char key[12];
    snprintf(key, sizeof(key), "note%u", i);

    buttonNote[i] = preferences.getUChar(
      key,
      36 + i
    );

    if (buttonNote[i] > 127) {
      buttonNote[i] = 36 + i;
    }
  }

  preferences.end();
}

void saveSettings() {
  preferences.begin("ble-midi", false);

  preferences.putUChar("channel", midiChannel);

  for (uint8_t i = 0; i < 6; i++) {
    char key[12];
    snprintf(key, sizeof(key), "potcc%u", i);
    preferences.putUChar(key, potCC[i]);
  }

  for (uint8_t i = 0; i < 8; i++) {
    char key[12];
    snprintf(key, sizeof(key), "note%u", i);
    preferences.putUChar(key, buttonNote[i]);
  }

  preferences.end();
}

// =====================================================
// OLED
// =====================================================

void showMessage(const char* line1, const char* line2 = "") {
  if (!oledAvailable) {
    return;
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.println(line1);

  display.setCursor(0, 16);
  display.println(line2);

  display.display();
}

void drawMidiScreen() {
  if (!oledAvailable) {
    return;
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.println("ESP32 BLE MIDI");

  display.setCursor(0, 14);
  display.print("Mode: MIDI");

  display.setCursor(0, 26);
  display.print("Channel: ");
  display.println(midiChannel);

  display.setCursor(0, 38);
  display.print("CC: ");

  for (uint8_t i = 0; i < 6; i++) {
    display.print(potCC[i]);

    if (i < 5) {
      display.print(",");
    }
  }

  display.setCursor(0, 52);
  display.println("Hold UP 2s: Menu");

  display.display();
}

void drawMenuScreen() {
  if (!oledAvailable) {
    return;
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.println("SETTINGS");

  display.setCursor(0, 12);

  if (menuPage == PAGE_MAIN) {
    const char* items[] = {
      "MIDI Channel",
      "Potentiometer CC",
      "Button Notes",
      "Exit"
    };

    for (uint8_t i = 0; i < 4; i++) {
      display.setCursor(0, 12 + i * 10);

      display.print(i == menuSelection ? ">" : " ");
      display.println(items[i]);
    }

    display.setCursor(0, 54);
    display.println("UP: select  DOWN: next");
  }
  else if (menuPage == PAGE_MIDI_CHANNEL) {
    display.println("MIDI Channel");

    display.setCursor(0, 26);
    display.print("Channel: ");
    display.println(editMidiChannel);

    display.setCursor(0, 44);
    display.println("UP: +  DOWN: -");

    display.setCursor(0, 54);
    display.println("Hold UP: save/back");
  }
  else if (menuPage == PAGE_POT_CC) {
    display.println("Potentiometer CC");

    display.setCursor(0, 22);
    display.print("Pot: ");
    display.println(selectedPot + 1);

    display.setCursor(0, 34);
    display.print("CC: ");
    display.println(editPotCC[selectedPot]);

    display.setCursor(0, 48);
    display.println("UP:+ DOWN: next");

    display.setCursor(0, 56);
    display.println("Hold UP: save/back");
  }
  else if (menuPage == PAGE_BUTTON_NOTE) {
    display.println("Button MIDI Note");

    display.setCursor(0, 22);
    display.print("Button: ");
    display.println(selectedButton + 1);

    display.setCursor(0, 34);
    display.print("Note: ");
    display.println(editButtonNote[selectedButton]);

    display.setCursor(0, 48);
    display.println("UP:+ DOWN: next");

    display.setCursor(0, 56);
    display.println("Hold UP: save/back");
  }

  display.display();
}

void updateDisplay() {
  if (millis() - lastDisplayUpdate < 150) {
    return;
  }

  lastDisplayUpdate = millis();

  if (operatingMode == MODE_MIDI) {
    drawMidiScreen();
  } else {
    drawMenuScreen();
  }
}

// =====================================================
// MIDI: POTENTIOMETERS
// =====================================================

uint8_t adcToMidi(float adcValue) {
  int value = (int)(adcValue + 0.5f);

  value = constrain(value, 0, 4095);

  return (uint8_t)map(value, 0, 4095, 0, 127);
}

void processPots() {
  if (millis() - lastPotPoll < POT_POLL_INTERVAL_MS) {
    return;
  }

  lastPotPoll = millis();

  for (uint8_t i = 0; i < 6; i++) {
    int raw = analogRead(potPins[i]);

    // Первоначальная инициализация.
    if (!potInitialized[i]) {
      filteredADC[i] = raw;
      acceptedADC[i] = raw;
      potInitialized[i] = true;

      uint8_t value = adcToMidi(filteredADC[i]);

      MIDI.sendControlChange(
        potCC[i],
        value,
        midiChannel
      );

      lastSentCC[i] = value;
      continue;
    }

    // 1. Сглаживание ADC.
    filteredADC[i] +=
      FILTER_ALPHA * ((float)raw - filteredADC[i]);

    // 2. Гистерезис на стороне ADC.
    if (fabsf(filteredADC[i] - acceptedADC[i])
        >= ADC_HYSTERESIS) {
      acceptedADC[i] = filteredADC[i];
    }

    // 3. Перевод в MIDI CC.
    uint8_t midiValue = adcToMidi(acceptedADC[i]);

    // 4. Отправка только при существенном изменении.
    if (lastSentCC[i] == 255 ||
        abs((int)midiValue - (int)lastSentCC[i])
        >= MIDI_CC_THRESHOLD) {

      MIDI.sendControlChange(
        potCC[i],
        midiValue,
        midiChannel
      );

      lastSentCC[i] = midiValue;
    }
  }
}

// =====================================================
// MIDI: BUTTONS
// =====================================================

void sendButtonNoteOn(uint8_t index) {
  MIDI.sendNoteOn(
    buttonNote[index],
    midiVelocity,
    midiChannel
  );
}

void sendButtonNoteOff(uint8_t index) {
  MIDI.sendNoteOff(
    buttonNote[index],
    0,
    midiChannel
  );
}

// =====================================================
// ВХОД В МЕНЮ
// =====================================================

void enterMenu() {
  if (operatingMode == MODE_MENU) {
    return;
  }

  operatingMode = MODE_MENU;
  menuPage = PAGE_MAIN;
  menuSelection = 0;

  editMidiChannel = midiChannel;

  for (uint8_t i = 0; i < 6; i++) {
    editPotCC[i] = potCC[i];
  }

  for (uint8_t i = 0; i < 8; i++) {
    editButtonNote[i] = buttonNote[i];
  }

  // UP уже удерживается — не использовать его
  // повторно как обычное нажатие меню.
  upHoldConsumed = true;

  showMessage("Settings", "Menu opened");
}

// =====================================================
// ВЫХОД ИЗ МЕНЮ С СОХРАНЕНИЕМ
// =====================================================

void applyAndExitMenu() {
  if (menuPage == PAGE_MAIN) {
    // На главном экране долгий UP означает выход
    // из меню и сохранение настроек.
    midiChannel = editMidiChannel;

    for (uint8_t i = 0; i < 6; i++) {
      potCC[i] = editPotCC[i];
    }

    for (uint8_t i = 0; i < 8; i++) {
      buttonNote[i] = editButtonNote[i];
    }

    saveSettings();

    operatingMode = MODE_MIDI;
    menuPage = PAGE_MAIN;

    showMessage("Settings saved", "MIDI mode");
    return;
  }

  // Сохраняем текущую страницу и возвращаемся
  // на предыдущий уровень меню.
  if (menuPage == PAGE_MIDI_CHANNEL) {
    midiChannel = editMidiChannel;
  }
  else if (menuPage == PAGE_POT_CC) {
    for (uint8_t i = 0; i < 6; i++) {
      potCC[i] = editPotCC[i];
    }
  }
  else if (menuPage == PAGE_BUTTON_NOTE) {
    for (uint8_t i = 0; i < 8; i++) {
      buttonNote[i] = editButtonNote[i];
    }
  }

  saveSettings();

  menuPage = PAGE_MAIN;
  menuSelection = 0;
}

// =====================================================
// ОБРАБОТКА МЕНЮ
// =====================================================

void menuUpShortPress() {
  if (menuPage == PAGE_MAIN) {
    switch (menuSelection) {
      case 0:
        menuPage = PAGE_MIDI_CHANNEL;
        break;

      case 1:
        menuPage = PAGE_POT_CC;
        selectedPot = 0;
        break;

      case 2:
        menuPage = PAGE_BUTTON_NOTE;
        selectedButton = 0;
        break;

      case 3:
        applyAndExitMenu();
        break;
    }

    return;
  }

  if (menuPage == PAGE_MIDI_CHANNEL) {
    if (editMidiChannel < 16) {
      editMidiChannel++;
    } else {
      editMidiChannel = 1;
    }
  }
  else if (menuPage == PAGE_POT_CC) {
    if (editPotCC[selectedPot] < 127) {
      editPotCC[selectedPot]++;
    } else {
      editPotCC[selectedPot] = 0;
    }
  }
  else if (menuPage == PAGE_BUTTON_NOTE) {
    if (editButtonNote[selectedButton] < 127) {
      editButtonNote[selectedButton]++;
    } else {
      editButtonNote[selectedButton] = 0;
    }
  }
}

void menuDownShortPress() {
  if (menuPage == PAGE_MAIN) {
    menuSelection = (menuSelection + 1) % 4;
    return;
  }

  if (menuPage == PAGE_MIDI_CHANNEL) {
    if (editMidiChannel > 1) {
      editMidiChannel--;
    } else {
      editMidiChannel = 16;
    }
  }
  else if (menuPage == PAGE_POT_CC) {
    selectedPot = (selectedPot + 1) % 6;
  }
  else if (menuPage == PAGE_BUTTON_NOTE) {
    selectedButton = (selectedButton + 1) % 8;
  }
}

// =====================================================
// ОБРАБОТКА СОБЫТИЙ КНОПОК
// =====================================================

void processButtons() {
  uint32_t now = millis();

  for (uint8_t i = 0; i < 8; i++) {
    bool reading = digitalRead(buttonPins[i]) == HIGH;

    if (reading != rawButtonState[i]) {
      rawButtonState[i] = reading;
      lastDebounceTime[i] = now;
    }

    if (now - lastDebounceTime[i] >= DEBOUNCE_MS) {
      if (stableButtonState[i] != rawButtonState[i]) {
        stableButtonState[i] = rawButtonState[i];

        if (stableButtonState[i]) {
          buttonPressTime[i] = now;

          // В обычном MIDI-режиме нажатие UP
          // откладывается, чтобы распознать удержание.
          if (operatingMode == MODE_MIDI) {
            if (i != BUTTON_UP_INDEX) {
              sendButtonNoteOn(i);
            }
          }
          else {
            // В меню GPIO 13 и 19 используются
            // для навигации, остальные — MIDI.
            if (i != BUTTON_UP_INDEX &&
                i != BUTTON_DOWN_INDEX) {
              sendButtonNoteOn(i);
            }
          }
        }
        else {
          uint32_t heldTime = now - buttonPressTime[i];

          if (operatingMode == MODE_MIDI) {
            if (i == BUTTON_UP_INDEX) {
              if (heldTime >= MENU_HOLD_MS) {
                enterMenu();
              }
              else {
                // Короткое нажатие UP — MIDI-кнопка.
                sendButtonNoteOn(i);
                sendButtonNoteOff(i);
              }
            }
            else {
              sendButtonNoteOff(i);
            }
          }
          else {
            if (i == BUTTON_UP_INDEX) {
              if (heldTime < MENU_HOLD_MS) {
                if (!upHoldConsumed) {
                  menuUpShortPress();
                }
              }

              upHoldConsumed = false;
            }
            else if (i == BUTTON_DOWN_INDEX) {
              if (heldTime < MENU_HOLD_MS) {
                menuDownShortPress();
              }
            }
            else {
              sendButtonNoteOff(i);
            }
          }
        }
      }
    }
  }

  // Если UP удерживается 2 секунды в MIDI-режиме,
  // войти в меню сразу, не ожидая отпускания.
  if (operatingMode == MODE_MIDI &&
      stableButtonState[BUTTON_UP_INDEX] &&
      !upHoldConsumed &&
      now - buttonPressTime[BUTTON_UP_INDEX] >= MENU_HOLD_MS) {

    enterMenu();
  }
}

// =====================================================
// SETUP
// =====================================================

void setup() {
  Serial.begin(115200);

  // Исходные кнопки активны по HIGH.
  // GPIO не меняются.
  for (uint8_t i = 0; i < 8; i++) {
    pinMode(buttonPins[i], INPUT);
  }

  // Настройка pull-down, как в исходном проекте.
  // Для ESP32 используется gpio_set_pull_mode.
  for (uint8_t i = 0; i < 8; i++) {
    gpio_set_pull_mode(
      (gpio_num_t)buttonPins[i],
      GPIO_PULLDOWN_ONLY
    );
  }

  analogReadResolution(12);

  // Включаем максимальную ширину измерения для ADC.
  for (uint8_t i = 0; i < 6; i++) {
    analogSetPinAttenuation(
      potPins[i],
      ADC_11db
    );
  }

  Wire.begin(21, 22);

  oledAvailable = display.begin(
    SSD1306_SWITCHCAPVCC,
    OLED_ADDRESS
  );

  if (oledAvailable) {
    display.clearDisplay();
    display.display();
  }

  loadSettings();

  MIDI.begin(MIDI_CHANNEL_OMNI);

  showMessage("ESP32 BLE MIDI", "Starting...");

  delay(500);

  Serial.println("ESP32 BLE MIDI Controller ready");
}

// =====================================================
// LOOP
// =====================================================

void loop() {
  processButtons();

  // Потенциометры продолжают работать и в меню.
  processPots();

  updateDisplay();

  delay(1);
}