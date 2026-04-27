//Прошивка моего ЧПУ станка
#include <WiFi.h> // Для ESP32 (или <ESP8266WiFi.h> для 8266)
#include <ESPAsyncWebServer.h> // Библиотеки для асинхронного сервера
#include <AsyncTCP.h>
#include "SPI.h"
#include "SD.h"

// Стандартные пины для ESP32 к SD: MOSI - 23, MISO - 19, SCK - 18
const int chipSelect = 5; // Пин CS (можешь поменять на свой)
//сервер----------------------
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
//вайфай----------------------
const char* ssid = "NanoNC";
const char* password = "12345678";
//моторы----------------------
struct Stepper {
    int sPin;//пин шаги
    int dPin;//пин направление
    int sPin2; int dPin2;  // Дублирующий мотор (0 если нет)
    volatile uint32_t dt;//время фазы
    volatile uint32_t tm;//таймер до смены фазы
    volatile long trgSteps;//целевое количество шагов
    volatile long curSteps;//текущее количество шагов
    volatile bool HL;//текущая фаза уровня шага
    volatile long curPos;//текущая позиция в шагах
    int kf;//количество шагов на мм
    int dir; //tекущее направление: 1 (вперед/HIGH) или -1 (назад/LOW)
};
Stepper M[3] = {
    {12, 13, 0, 0, 500, 0, 0, 0, false, 0, 400, 1}, //это X
    {14, 27, 25, 26, 500, 0, 0, 0, false, 0, 400, 1}, //это Y1 Y2
    {17, 16, 0, 0, 500, 0, 0, 0, false, 0, 400, 1}  //это Z
};
const int spindlePin = 4; // Пин шпинделя
String modalG = "G1"; // По умолчанию модальный код G1
int maxF=2000;
const int enablePin = 15; // Пин включения драйверов
float curF = 300.0, lastT[3] = {0,0,0};
hw_timer_t * timer = NULL;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED; //защита для обмена данными между параллельными задачами
//для прикладной работы с файлами флешки
File workFile;               // Дескриптор открытого файла
bool isFileRunning = false;  // Флаг активного выполнения
bool isFilePaused = false;   // Флаг паузы
String currentFileName = ""; // Имя текущего файла
size_t fileSize = 0;
// Глобальная переменная для хранения списка файлов
struct cachedFilesJSON {
    String type = "files";
    String list = "[]";      // Строка с массивом файлов
    String activeFile = "";  // Имя активного файла (или пусто)
    String error = "";
};
cachedFilesJSON state; // Глобальный объект состояния

//Функция проверки: закончили ли моторы движение
bool allAxesDone() {
    bool done = false;
    portENTER_CRITICAL(&mux);
    // Если у всех трех моторов текущие шаги сравнялись с целевыми
    if (M[0].curSteps >= M[0].trgSteps && 
        M[1].curSteps >= M[1].trgSteps && 
        M[2].curSteps >= M[2].trgSteps) {
        done = true;
    }
    portEXIT_CRITICAL(&mux);
    return done;
}

//аппаратный таймер
void IRAM_ATTR onTimer() {
    for (int i = 0; i < 3; i++) {
        if (M[i].curSteps < M[i].trgSteps) {//проверяем: есть ли еще шаги до цели?
            M[i].tm++; // Каждый тик прерывания = 1 мкс
            if (M[i].tm >= M[i].dt) {
                M[i].tm = 0;
                M[i].HL = !M[i].HL;//инвертируем уровень
                digitalWrite(M[i].sPin, M[i].HL);//запись в регистр
                if (M[i].sPin2 > 0) digitalWrite(M[i].sPin2, M[i].HL);// Дубликат мотора
                if (M[i].HL) { // Считаем по переднему фронту
                    M[i].curSteps++;// Прогресс текущей команды g-кода
                    M[i].curPos += M[i].dir;// Обновление "одометра" станка
                }
            }
        } else if (M[i].HL) {// Сбрасываем пин, если закончили движение
            M[i].HL = false;
            digitalWrite(M[i].sPin, LOW);
            if (M[i].sPin2 > 0) digitalWrite(M[i].sPin2, LOW);
        }
    }
}

//извекатель значения после ключа (X, Y, Z, F)
float getVal(String s, char k, float def){
    int p = s.indexOf(k);
    if (p == -1) return def;
    const char* startPtr = s.c_str() + p + 1; 
    char* endPtr;
    float val = strtof(startPtr, &endPtr);
    return (startPtr == endPtr) ? def : val;
}

// Функция для аппаратного сброса
void emergencyStop() {
    portENTER_CRITICAL(&mux);
    for (int i = 0; i < 3; i++) {
        M[i].trgSteps = 0; // Цель = 0
        M[i].curSteps = 0; // Текущий прогресс = 0
        M[i].tm = 0;       // Сброс таймера
        M[i].HL = false;   // Сброс фазы
        digitalWrite(M[i].sPin, LOW); // Явно выключаем пин шагов
    }
    portEXIT_CRITICAL(&mux);
}

// Функция обработки команд g-кода
void processCommand(String req, bool isAbsolute = false) {
    req.toUpperCase();
    // 1. Проверка на G0/G1 для изменения модальности
    if (req.indexOf("G0") != -1) modalG = "G0";
    else if (req.indexOf("G1") != -1) modalG = "G1";
    // Явная проверка на STOP
    if (req.indexOf("STOP") != -1) {
        emergencyStop();
        return;
    }
    // --- ОБРАБОТЧИК G92 (Сброс координат) ---
    if (req.indexOf("G92") != -1) {
      portENTER_CRITICAL(&mux);
      // Если в команде есть X, Y или Z — обнуляем соответствующие оси
      // Если просто "G92", обнуляем всё сразу
      if (req.indexOf('X') != -1 || (req.indexOf('Y') == -1 && req.indexOf('Z') == -1)) M[0].curPos = 0;
      if (req.indexOf('Y') != -1 || (req.indexOf('X') == -1 && req.indexOf('Z') == -1)) M[1].curPos = 0;
      if (req.indexOf('Z') != -1 || (req.indexOf('X') == -1 && req.indexOf('Y') == -1)) M[2].curPos = 0;
      portEXIT_CRITICAL(&mux);
      
      ws.textAll("Origin set to zero"); 
      return; 
    }

    if (req.indexOf("G0") == -1 && req.indexOf("G1") == -1 && modalG == "") return;
    float targetF = (modalG == "G0") ? maxF : getVal(req, 'F', curF); // G0 игнорирует F из строки
    long dS[3];
    float distSq = 0;
    long maxS = 0;
    char keys[] = {'X', 'Y', 'Z'};
    for (int i = 0; i < 3; i++) {
        float val = getVal(req, keys[i], -9999.0); // -9999 как признак отсутствия координаты
        if (val == -9999.0) {
            dS[i] = 0; // Ось не двигается
        } else {
            if (isAbsolute) {
                // АБСОЛЮТНЫЕ: считаем разницу с текущей позицией
                float currentMM = (float)M[i].curPos / M[i].kf;
                dS[i] = (long)round((val - currentMM) * M[i].kf);
            } else {
                // ОТНОСИТЕЛЬНЫЕ: просто сдвиг
                dS[i] = (long)round(val * M[i].kf);
            }
        }
        long steps = abs(dS[i]);
        if (steps > maxS) maxS = steps;
        float mm = (float)steps / M[i].kf;
        distSq += mm * mm;
    }
    if (maxS <= 0) return;
    float timerFactor = (sqrt(distSq) * 3000000.0) / targetF;
    portENTER_CRITICAL(&mux);
    for (int i = 0; i < 3; i++) {
        M[i].curSteps = 0;
        M[i].trgSteps = abs(dS[i]);
        M[i].dir = (dS[i] >= 0) ? 1 : -1;
        digitalWrite(M[i].dPin, M[i].dir > 0);
        if (M[i].dPin2 > 0) digitalWrite(M[i].dPin2, M[i].dir > 0);
        if (M[i].trgSteps > 0) {
            M[i].dt = (uint32_t)(timerFactor / M[i].trgSteps);
            if (M[i].dt < 1) M[i].dt = 1;
        }
    }
    portEXIT_CRITICAL(&mux);
}

//собираю строку json
String getJSON(){
  return "{\"type\": \""+state.type+"\", \"list\": "+state.list+", \"activeFile\":\""+state.activeFile+"\", \"error\": \""+state.error+"\"}";
}

//Функция сканирования файлов
String getFilesJSON(bool forceUpdate = false) {
    // Если программа работает, отдаем кэш, чтобы не прерывать чтение файла
    if (isFileRunning && !forceUpdate && state.list != "[]") {
        return getJSON();
    }
    SD.end(); 
    delay(100);
    // Принудительно проверяем, доступна ли карта, перед открытием
    if (!SD.begin(chipSelect)) {
        state.type="files"; state.list="[]";
        state.error="SD Card error"; state.activeFile="";
        return getJSON();
    }

    File root = SD.open("/");
    if (!root || !root.isDirectory()) {
        state.type="files"; state.list="[]";
        state.error="SD Card error"; state.activeFile="";
        return getJSON();
    }
    state.type="files";
    state.error="";
    state.list = "[";
    File file = root.openNextFile();
    bool first = true;
    while (file) {
        if (!file.isDirectory()) {
            if (!first) state.list += ",";
            String fileName = String(file.name());
            if (fileName.startsWith("/")) {
                fileName = fileName.substring(1);
            }
            state.list += "\"" + fileName + "\"";
            first = false;
        }
        file = root.openNextFile();
    }
    
    root.close(); 
    state.list += "]";
    return getJSON();
}

//обработчик запросов вебсокета
void handleWebSocketMessage(void *arg, uint8_t *data, size_t len) {
  AwsFrameInfo *info = (AwsFrameInfo*)arg;
  if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
    data[len] = 0;
    String message = (char*)data;
    
    // проверка команды запроса файлов
    if (message == "LIST_FILES") {
        ws.textAll(getFilesJSON()); 
    }else if (message.startsWith("START")) {
        // 1. Получаем чистое имя файла
        String filename = "/" + message.substring(6); 
        filename.trim(); 

        // 2. Если мы были на паузе и это тот же файл — просто продолжаем
        if (isFilePaused && currentFileName == filename) {
            isFilePaused = false;
            isFileRunning = true;
        } else {
            // 3. Если новый запуск — закрываем старый файл (если был) и открываем новый
            if (workFile) workFile.close();
            workFile = SD.open(filename, "r");
            
            if (workFile) {
                fileSize = workFile.size(); // Запоминаем размер
                currentFileName = filename;
                state.activeFile=currentFileName.substring(1);
                isFileRunning = true;
                isFilePaused = false;
                Serial.println("File opened: " + filename);
            } else {
                ws.textAll("ERROR: File not found");
            }
        }
    }else if (message == "PAUSE") {
        isFileRunning = false;
        isFilePaused = true;
    }else if (message == "STOP") {
        emergencyStop(); // Наша функция из первой части
        if (workFile) workFile.close();
        isFileRunning = false;
        isFilePaused = false;
        currentFileName = "";
        state.activeFile="";
    }else {
        processCommand(message, false); 
    }

  }
}
void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
             void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_DATA) handleWebSocketMessage(arg, data, len);
}

//выдаю index страницу
const char index_html[] PROGMEM = R"rawliteral(


<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
<title>NanoNC</title>
<link rel='icon' type='image/x-icon' href='data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAMAAABEpIrGAAAAz1BMVEUAAAB3iJlRbXd3jZ1BVWFNaXVAVmFOaXNTbXl3j5pAVWBNaHRUbXl4j5t3j5pAVmBOaHVTbnl4j5pAVWBOaXQ2Rk43SFA5S1M9UFk/UltBVmBEWmRHXWdKYGpMZG9OZnFOaXVQaXRRZ3FUbnped4JpgIprgY1uhZB0iZJ0i5d4j5t+kpuAlqGInaeJm6ORpK2UpKuZq7SerbSisrqptryqucCzwMa7x82+yM3M1dne4+blUgDl6uzmXA/pci/rfT/wnW/zs4/1vp/99O////9fI8McAAAARXRSTlMADy8vPz9fX6+vv7+/v8/f39/f7+////////////////////////////////////////////////////////////////+QgRlhAAABC0lEQVQ4jbXT6VLCMBQF4LhvKIvWpVWpBgSvbEIgQMCi5P2fydwsGGxhYEbPn/ak36SZtCHkL7J3cg6L0EVyx7sWlACyAKVF83wHVgFqZ9gI9DnnfQc6jLGOAwdnBTU8kJiBAV1duuouf7pPruIYoCm/PoT4lE0EVVuqlIbhJYkRcDkVQkwlR8BsYQhCBwTGAVN80JOJGkpkD0HLlpYHYDKfJbP5xCxyrMuY+qAxUuseNQyoDFUZVpYAQPu9/bNR9be6vvogcycRFNaDPDlaDw43/lgqtRfMI6acAWr3kU6gc11OgefIB8FdCkTLINge/HrFbQq8Pvjg5mmrn/ZiFci5k1XKBkV3sv4534kCc7X/EOSVAAAAAElFTkSuQmCC' />
<style>
:root {
  --red: #ff3b30;
  --green: #4cd964;
  --blue: #5ac8fa;
  --bg: #050505;
  --panel: #1c1c1e;
  --border: #3a3a3c;
  --axis: rgba(255, 255, 255, 0.15);
}
* {
  box-sizing: border-box;
}
body {
    margin: 0;
    padding: 0;
    background: var(--bg);
    color: #fff;
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    display: grid;
    /* Уменьшаем высоту списка файлов (1fr -> 0.8fr) и добавляем отступ снизу */
    grid-template-rows: auto auto 1fr auto 100px; 
    height: 100dvh; /* Используем dvh (dynamic viewport height) для мобилок */
    gap: 10px; /* Немного уменьшил зазор */
    overflow: hidden;
    user-select: none;
    touch-action: none;
    padding-bottom: 20px; /* Поднимает кнопку СТОП выше системного меню */
}

/* Header */
.header {
  margin: 15px 15px 0px 15px;
  display: flex;
  justify-content: space-between;
  align-items: center;
  height: 60px;
}
#display-tx { color: var(--green); font-size: 15px; font-family: monospace; margin-bottom: 5px;}
#display-rx { color: var(--blue); font-size: 15px; opacity: 0.8; font-family: monospace;}
.zero-btn {
  background: rgba(0, 0, 0, 0.1);
  border: 1px solid var(--green);
  border-radius: 12px;
  color: var(--green);
  width: 40px;
  height: 40px;
  display: flex;
  align-items: center;
  justify-content: center;
  transition: transform 0.05s; /* Добавляем плавность для эффекта сжатия */
}
.zero-btn:active {
  transform: scale(0.85);
}
/* Z-Axis Section */
.control-section {
  position: relative;
  display: flex;
  justify-content: center;
  align-items: center;
  height: 60px;
  border-radius: 12px;
}
.track {
  height: 1px;
  width: 85%;
  background: linear-gradient(90deg, transparent, #ffffffa8, transparent);
  border-radius: 4px;
  position: relative;
  display: flex;
  align-items: center;
}

.thumb {
  width: 50px;
  height: 50px;
  position: absolute;
  top: 50%;
  left: 50%;
  transform: translate(-50%, -50%);
  z-index: 10;
  touch-action: none;
}
/* Joystick Area */
.joystick-container {
    display: flex;
    justify-content: center;
    align-items: center;
    width: 100%;
    /* Ограничиваем высоту, чтобы не вытесняла список файлов */
    max-height: 350px; 
    min-height: 180px;
}
.joystick-area {
    /* Делаем область джойстика чуть компактнее */
    width: min(90vw, 250px);
    height: min(90vw, 250px);
    aspect-ratio: 1 / 1;
    position: relative;
}
.joy-axis-h, .joy-axis-v {
  position: absolute;
  background: linear-gradient(90deg, transparent, #ffffffa8, transparent);
}
.joy-axis-h { width: 100%; height: 1px; top: 50%; left: 0; transform: translateY(-50%); }
.joy-axis-v { height: 100%; width: 1px; left: 50%; top: 0; transform: translateX(-50%); background: linear-gradient(0deg, transparent, #ffffffa8, transparent); }
.joy-center-dot {
  position: absolute;
  width: 8px; height: 8px;
  background: var(--border);
  border-radius: 50%;
  top: 50%; left: 50%;
  transform: translate(-50%, -50%);
}
.joy-stick {
  width: 30%;
  position: absolute;
  top: 50%; left: 50%;
  transform: translate(-50%, -50%);
  z-index: 50;
  touch-action: none;
}
.thumb svg, .joy-stick svg { width: 100%; height: 100%; pointer-events: none; }
/* File List */
.file-list-container {
  margin: 15px 15px 0px 15px;
  background: var(--panel);
  border: 1px solid var(--border);
  border-radius: 16px;
  overflow-y: auto;
}
.file-item {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 5px 16px;
  border-bottom: 1px solid var(--border);
}
.file-item.selected {
  background: rgba(90, 200, 250, 0.05);
  border-left: 4px solid var(--blue);
}
.file-name { font-size: 16px; color: #efeff4; font-family: monospace; }
.play-btn {
  width: 40px; height: 40px;
  border-radius: 30%;
  border: 1px solid #48484a;
  display: flex; align-items: center; justify-content: center;
  color: var(--green); background: transparent;
}
.play-btn svg { width: 30px; height: 30px; fill: currentColor; }
/* Stop Button */
.stop-btn {
    margin: 0 15px 10px 15px; /* Увеличили нижний отступ */
    height: 60px; /* Чуть уменьшили высоту, чтобы не съедала место */
    background: #b50000;
    color: #fff;
    border: none;
    border-radius: 16px;
    font-size: 20px;
    font-weight: 900;
    text-transform: uppercase;
}

.stop-btn:active { transform: scale(0.98); background: #e00000; }

/* Плавный переход при возврате в ноль */
.smooth { transition: transform 0.12s cubic-bezier(0.25, 1.5, 0.5, 1), left 0.12s cubic-bezier(0.25, 1.5, 0.5, 1); }

/* Маски блокировки */
.locked {
    filter: grayscale(1) opacity(0.4);
    pointer-events: none;
}

.locked::after {
    content: '🔒';
    position: absolute;
    top: 50%;
    left: 50%;
    transform: translate(-50%, -50%);
    font-size: 24px;
    z-index: 100;
}

/* Прогресс-бар как подложка для активного файла */
.file-item.running {
    position: relative;
    background: rgba(90, 200, 250, 0.05);
}

.file-item.running::before {
    content: '';
    position: absolute;
    left: 0;
    top: 0;
    height: 100%;
    width: var(--progress, 0%); /* Ширина управляется через JS */
    background: rgba(90, 200, 250, 0.2); /* Цвет полоски прогресса */
    z-index: 0;
    transition: width 0.3s linear;
}

.file-item > * {
    position: relative; /* Чтобы текст и кнопки были над полоской прогресса */
    z-index: 1;
}

</style>
</head>
<body>
<svg style="position: absolute; width: 0; height: 0;">
  <defs>
    <linearGradient id="rubyBase" x1="0%" y1="0%" x2="100%" y2="100%">
      <stop offset="0%" style="stop-color:#d60000" />
      <stop offset="100%" style="stop-color:#610000" />
    </linearGradient>
  </defs>
  <symbol id="ruby-crystal" viewBox="0 0 100 100">
    <path d="M30,5 C10,5 5,10 5,30 L5,70 C5,90 10,95 30,95 L70,95 C90,95 95,90 95,70 L95,30 C95,10 90,5 70,5 Z" fill="#4d0000" />
    <path d="M30,5 C10,5 5,10 5,30 L5,70 C5,90 10,95 30,95 L70,95 C90,95 95,90 95,70 L95,30 C95,10 90,5 70,5 Z" fill="url(#rubyBase)" fill-opacity="0.95" />
    <path d="M30,5 C10,5 5,10 5,30 L5,70 C5,90 10,95 30,95 L70,95 C90,95 95,90 95,70 L95,30 C95,10 90,5 70,5 Z" fill="none" stroke="#ff4d4d" stroke-width="1.2" stroke-opacity="0.8" />
    <path d="M35,15 C20,15 15,20 15,35 L15,65 C15,80 20,85 35,85 L65,85 C80,85 85,80 85,65 L85,35 C85,20 80,15 65,15 Z" fill="none" stroke="white" stroke-width="1.8" stroke-opacity="0.25" />
    <path d="M30,12 C20,12 15,15 15,25 L15,45 C45,35 55,35 85,45 L85,25 C85,15 80,12 70,12 Z" fill="white" fill-opacity="0.5" />
    <rect x="30" y="15" width="40" height="2.5" rx="1" fill="white" fill-opacity="0.6" />
    <ellipse cx="50" cy="85" rx="30" ry="6" fill="white" fill-opacity="0.25" />
  </symbol>
</svg>

<header class="header">
  <div class="status-box">
    <div id="display-tx">TX: Ready</div>
    <div id="display-rx">RX: Offline</div>
  </div>
  <button class="zero-btn" id="zero-btn" onclick="send('G92 X0 Y0 Z0')">
    <svg viewBox="0 0 100 100" style="width: 100%; height: 100%; display: block; filter: drop-shadow(0 0 5px rgba(76, 217, 100, 0.4));">
      <circle cx="50" cy="50" r="35" stroke="currentColor" stroke-width="4" fill="none"/>
      <line x1="50" y1="5" x2="50" y2="95" stroke="currentColor" stroke-width="4" stroke-linecap="round"/>
      <line x1="5" y1="50" x2="95" y2="50" stroke="currentColor" stroke-width="4" stroke-linecap="round"/>
    </svg>
  </button>
</header>

<section class="control-section">
  <div class="track" id="z-track">
    <div class="thumb" id="z-thumb">
      <svg><use href="#ruby-crystal"/></svg>
    </div>
  </div>
</section>

<div class="joystick-container">
  <div class="joystick-area" id="joy-bounds">
    <div class="joy-axis-h"></div>
    <div class="joy-axis-v"></div>
    <div class="joy-stick" id="joy-stick">
      <svg><use href="#ruby-crystal"/></svg>
    </div>
  </div>
</div>

<div class="file-list-container" id="file-list"></div>

<button class="stop-btn" onclick="stopExecution()">СТОП</button>

<script>
let ws, isXY = false, isZ = false;
let vx=0, vy=0, vz=0, vf=0, vfz=0;
let activeFile = null;
let pausedFile = null;
const SNAP = 0.1;
let files = []; 

function init() {
  const joy = document.getElementById('joy-stick');
  const zThumb = document.getElementById('z-thumb');
  renderFiles();

  joy?.addEventListener('pointerdown', e => {
    if (activeFile !== null) return;
    isXY = true;
    joy.classList.remove('smooth');
    joy.setPointerCapture(e.pointerId);
  });

  zThumb?.addEventListener('pointerdown', e => {
    if (activeFile !== null) return;
    isZ = true;
    zThumb.classList.remove('smooth');
    zThumb.setPointerCapture(e.pointerId);
  });

  window.addEventListener('pointermove', e => {
    if (isXY && joy) {
      const bounds = document.getElementById('joy-bounds');
      const r = bounds.getBoundingClientRect();
      const cx = r.left + r.width / 2;
      const cy = r.top + r.height / 2;
      
      let dx = e.clientX - cx;
      let dy = e.clientY - cy;
      
      const max = (r.width / 2) - (joy.offsetWidth / 2); 
      const dist = Math.sqrt(dx*dx + dy*dy);

      if (dist > max) {
        dx *= max / dist;
        dy *= max / dist;
      }

      let rawX = dx / max;
      let rawY = -dy / max;
      
      if (Math.abs(rawY) < SNAP) { 
        rawY = 0; 
        dy = 0; 
      }
      else if (Math.abs(rawX) < SNAP) { 
        rawX = 0; 
        dx = 0; 
      }

      vx = rawX.toFixed(2);
      vy = rawY.toFixed(2);
      vf = Math.round((dist / max) * 100);

      joy.style.transform = `translate(calc(-50% + ${dx}px), calc(-50% + ${dy}px))`;
    }

    if (isZ && zThumb) {
      const r = document.getElementById('z-track').getBoundingClientRect();
      let x = e.clientX - r.left;
      x = Math.max(0, Math.min(x, r.width));
      zThumb.style.left = (x / r.width) * 100 + '%';
      
      let rawZ = (x / r.width - 0.5) * 2;
      vz = rawZ.toFixed(2);
	  if (Math.abs(vz) < 0.1) {
		vz = 0;
		vfz = 0; // Скорость 0 в мертвой зоне
	  } else {
		// Рассчитываем скорость Z пропорционально отклонению (от 0 до 100)
		vfz = Math.round(Math.abs(rawZ) * 100); 
	  }
    }
  });

  window.addEventListener('pointerup', (e) => {
    if (isXY && joy) {
      isXY = false;
      joy.classList.add('smooth');
      joy.style.transform = 'translate(-50%, -50%)'; 
      vx = 0; vy = 0; vf = 0;
	  send(`STOP`);
    }
    if (isZ && zThumb) {
      isZ = false;
      vz = 0;
	  vfz = 0; // Сброс скорости
      zThumb.classList.add('smooth');
      zThumb.style.left = '50%';
	  send(`STOP`);
    }
  });

  connect();
}

function updateControlsLock() {
    // Находим сами ползунки, а не маски
    const joy = document.getElementById('joy-stick');
    const zThumb = document.getElementById('z-thumb');
    const zeroBtn = document.getElementById('zero-btn');

    if (activeFile !== null) {
        // Добавляем класс блокировки прямо на кристаллы
        joy?.classList.add('locked');
        zThumb?.classList.add('locked');
        zeroBtn?.classList.add('locked');
    } else {
        // Убираем блокировку
        joy?.classList.remove('locked');
        zThumb?.classList.remove('locked');
        zeroBtn?.classList.remove('locked');
    }
}
//обновление списка файлов принудительно
function refreshFileList() {
    const listContainer = document.getElementById('file-list');
    // Показываем, что идет процесс
    document.getElementById('file-rfr').innerHTML = 'Обновление...';
    send('LIST_FILES');
}

function renderFiles() {
    const listContainer = document.getElementById('file-list');
    if (!listContainer) return;

    // Если файлов нет — выводим сообщение-заглушку
    if (files.length === 0) {
        listContainer.innerHTML = `
            <div style="padding: 30px; text-align: center; color: rgb(255 255 255 / 70%); font-family: monospace;">
                <div onclick="refreshFileList()" style="font-size: 24px; margin-bottom: 10px;">🔄</div>
                <span id="file-rfr">ФАЙЛЫ НЕ НАЙДЕНЫ</span><br>
                <small style="font-size: 10px; opacity: 0.6; text-transform: uppercase;">Загрузите G-Code на SD карту</small>
            </div>`;
        return;
    }

    const currentLockFile = activeFile || pausedFile;

    listContainer.innerHTML = files.map(f => {
        const isDisabled = currentLockFile !== null && currentLockFile !== f;
        const isSelected = currentLockFile === f;
        const isRunning = activeFile === f;
        
        // Формируем стили и классы
        const opacityStyle = isDisabled ? 'opacity: 0.3; pointer-events: none;' : '';
        const runningClass = isRunning ? 'running' : '';
        const selectedClass = isSelected ? 'selected' : '';

        return `
            <div id="file-${f.replace(/[^a-z0-9]/gi, '_')}" 
                 class="file-item ${selectedClass} ${runningClass}" 
                 style="${opacityStyle}" 
                 onclick="selectFile(this, '${f}')">
                
                <div class="file-name">${f}</div>
                
                <button class="play-btn" ${isDisabled ? 'disabled' : ''} onclick="togglePlay(event, '${f}')">
                    ${isRunning 
                        ? '<svg viewBox="0 0 24 24"><path d="M6 19h4V5H6v14zm8-14v14h4V5h-4z"/></svg>' // Иконка Pause
                        : '<svg viewBox="0 0 24 24"><path d="M8 5v14l11-7z"/></svg>' // Иконка Play
                    }
                </button>
            </div>
        `;
    }).join('');
}


function selectFile(el, name) {
  if (activeFile !== null || pausedFile !== null) return;
  document.querySelectorAll('.file-item').forEach(i => i.classList.remove('selected'));
  el.classList.add('selected');
}

function togglePlay(e, name) {
  e.stopPropagation();
  if (activeFile === name) {
    activeFile = null;
    pausedFile = name;
    send(`PAUSE`);
  } else {
    activeFile = name;
    pausedFile = null;
    send(`START ${name}`);
  }
  renderFiles();
  updateControlsLock();
}

function stopExecution() {
  activeFile = null;
  pausedFile = null;
  send('STOP');
  renderFiles();
  updateControlsLock();
}

function send(cmd) {
  const display = document.getElementById('display-tx');
  if(cmd!="LIST_FILES"){
    if (display) display.textContent = "TX: " + cmd;
  }
  if (ws?.readyState === 1) ws.send(cmd);
}

setInterval(() => {
	if (isXY && (vx != 0 || vy != 0)) {
	send(`G1 X${Math.round(vx*10)} Y${Math.round(vy*10)} F${vf*10}`);
	}
    if (isZ && vz != 0) {
        // Умножаем vfz на нужный коэффициент (например, на 5, чтобы макс. скорость была F500)
        let zSpeed = vfz * 5; 
        if (zSpeed < 50) zSpeed = 50; // Минимальный порог скорости
        send(`G1 Z${Math.round(vz*10)} F${zSpeed}`);
    }
}, 100);

function connect() {
    // Адрес ESP32 в режиме точки доступа (SoftAP)
    ws = new WebSocket(`ws://192.168.4.1/ws`);

    ws.onopen = () => {
        const rxDisplay = document.getElementById('display-rx');
        if (rxDisplay) rxDisplay.textContent = "RX: Connected";
        // При подключении сразу запрашиваем список файлов
        send('LIST_FILES');
    };

    ws.onmessage = e => {
        const msg = e.data;
        if (msg.startsWith("RX:")) {
          const rxDisplay = document.getElementById('display-rx');
          if (rxDisplay) rxDisplay.textContent = msg;
        }
        // 1. ОБРАБОТКА ПРОГРЕССА ВЫПОЛНЕНИЯ 
        else if (msg.startsWith("TX:")) {
            try {
                const txDisplay = document.getElementById('display-tx');
                if (txDisplay) txDisplay.textContent = msg;
                // Если выполнение завершено
                if (msg.includes("Done")) {
                    activeFile = null;
                    pausedFile = null;
                    send('LIST_FILES');
                }
            } catch (err) { console.error("Error parsing progress:", err); }
        }
        // 3. ОБРАБОТКА СПИСКА ФАЙЛОВ (JSON)
        else {
            try {
                const data = JSON.parse(msg);
                if (data.type === "files" && Array.isArray(data.list)) {
                    files = data.list; //alert(msg);
                    // Если прошивка присылает активный файл прямо в JSON
                    if (data.activeFile!="" && data.activeFile!="Done") {
                        activeFile = data.activeFile;
                    }else if(data.activeFile=="Done"){
                        document.getElementById('display-tx').textContent="TX:Обработка завершена!";
                        activeFile = "";
                    }
                    renderFiles();
                    updateControlsLock();
                }
            } catch (err) {
                //alert(err);
            }
        }
    };

    ws.onclose = () => {
        const rx = document.getElementById('display-rx');
        if (rx) rx.textContent = "RX: Offline";
        // Пытаемся переподключиться через 2 секунды
        setTimeout(connect, 2000);
    };

    ws.onerror = err => {
        console.error("WebSocket Error:", err);
    };
}


if (document.readyState !== 'loading') { init(); }
else { document.addEventListener('DOMContentLoaded', init); }
</script>
</body>
</html>



    )rawliteral";
    
//Инициализация
void setup() {
    Serial.begin(115200);
    if (!SD.begin(chipSelect)) {
        Serial.println("Ошибка: Карта памяти не найдена! Проверьте слот SD.");
    } else {
        Serial.println("Карта памяти успешно подключена.");
    }
    for (int i = 0; i < 3; i++) {
        pinMode(M[i].sPin, OUTPUT);
        pinMode(M[i].dPin, OUTPUT);
        if (M[i].sPin2 > 0) {
            pinMode(M[i].sPin2, OUTPUT);
            pinMode(M[i].dPin2, OUTPUT);
        }
        M[i].tm = 0; 
    }
    pinMode(enablePin, OUTPUT);
    digitalWrite(enablePin, LOW); // Включаем ток на моторах (LOW = ON)
    pinMode(spindlePin, OUTPUT);
    digitalWrite(spindlePin, LOW); // Шпиндель выключен по умолчанию
    //Настройка аппаратного таймера: 1 000 000 Гц = 1 тик в микросекунду
    timer = timerBegin(1000000);
    timerAttachInterrupt(timer, &onTimer);
    timerAlarm(timer, 10, true, 0);// Срабатывание каждую 1 мкс=1 тик
    //настройка вайфая
    WiFi.softAP(ssid, password);
    //Настройка WebSocket
    ws.onEvent(onEvent);
    server.addHandler(&ws);
    // Главная страница (уже с JS для WebSocket)
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
      // Отправляем данные напрямую из flash-памяти (P = PROGMEM)
      request->send_P(200, "text/html", index_html);
    });
    server.begin();
    //сообшаю о программе в сериал порт
    Serial.println("--- NanoNC v1.0 Booting ---");
    Serial.println("Status: Vector Logic & Hardware Timer Ready");
    Serial.println("Format: G1 X10 Y20 Z5 F1000");
}

//Основной цикл (Логика и Парсинг)
void loop(){
    //Блок чтения команд из сериал порта
    if (Serial.available()){
        processCommand(Serial.readStringUntil('\n'), true);// true = абсолютные координаты
        Serial.println("ok");
    }
    ws.cleanupClients(); // Удаление "мусора" из памяти вебсокета

    // БЛОК ВЫПОЛНЕНИЯ ФАЙЛА
    if (isFileRunning && !isFilePaused) {
        if (!SD.exists(currentFileName)) { // Проверяем, видит ли система файл
            emergencyStop();               // Останавливаем станок
            workFile.close();              // Закрываем дескриптор
            isFileRunning = false;         // Сбрасываем флаги
            ws.textAll("ERROR: SD Card removed"); // Уведомляем интерфейс
            return;                        // Выходим из итерации
        }
        if (allAxesDone()) { // Ждем, пока оси остановятся
            if (workFile && workFile.available()) {
                int progress = (workFile.position() * 100) / fileSize;// Считаем прогресс в процентах
                String line = workFile.readStringUntil('\n');
                line.trim();
                if (line.length() > 0 && line[0] != ';') { // Пропуск пустых строк и комментариев
                    processCommand(line, true); // Выполняем строку
                    ws.textAll("TX: " + String(progress) + "% " + line);// Отправляем прогресс и текущую строку 
                }
            } else {
                // Файл закончился
                if (workFile) workFile.close();
                isFileRunning = false;
                state.activeFile="Done";
                ws.textAll("TX:100% Done"); // Сообщаем интерфейсу об окончании
            }
        }
    }

    //Блок вывода координат (работает всегда)
    static uint32_t lastPrint = 0;
    if (millis() - lastPrint > 200) { // Каждые 200 мс
        lastPrint = millis();
        portENTER_CRITICAL(&mux);// Блокируем для безопасного чтения, так как Ядро 0 может менять эти цифры
        float x = (float)M[0].curPos / M[0].kf;
        float y = (float)M[1].curPos / M[1].kf;
        float z = (float)M[2].curPos / M[2].kf;
        portEXIT_CRITICAL(&mux);
        String posData = "RX: X" + String(x) + " Y" + String(y) + " Z" + String(z);
        Serial.println(posData); //вывожу координаты в сериал порт
        ws.textAll(posData); //вывожу координаты в вебсокет
    }
}

