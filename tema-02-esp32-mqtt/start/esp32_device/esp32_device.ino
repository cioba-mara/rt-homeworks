// Device-ul IoT din tema 01, de data aceasta pe un ESP32-C3 Super Mini.
//
// Aceeași logică și același protocol ca device-simulator din tema 01, cu trei
// taskuri FreeRTOS în loc de trei thread-uri Java:
//
//   task periodic --(coada de alarme)--> task sporadic
//        |                                     |
//        +---- telemetry ---> MQTT <--- alarm -+
//                              |
//                          commands --> command_received() --(coada de comenzi)--> task aperiodic ---> status
//
// Gata implementate (nu trebuie modificate):
//   config.h         setările: Wi-Fi, broker, STUDENT, valorile inițiale; DEVICE_ID
//                    se citește din adresa MAC a plăcii
//   connection.h/cpp Wi-Fi, ora exactă (SNTP), clientul MQTT, mqtt_publish(), epoch_ms()
//
// Așa cum este, schița se conectează la Wi-Fi și la broker și afișează pe
// Serial comenzile primite. Taskurile le scrieți voi: TODO 0 ... TODO 3.
// Exemplele de pornire sunt în laboratorul 0 din rt-labs-esp32c3; vezi
// exemple/README.md din folderul temei.
//
// Placa: ESP32C3 Dev Module, USB CDC On Boot: Enabled. Serial Monitor: 115200 baud.

#include "config.h"
#include "connection.h"

// Temperatura cipului, în °C, de la senzorul intern al ESP32-C3.
// Este temperatura siliciului, nu a camerei: imediat după pornirea Wi-Fi-ului
// poate fi în jur de 50 °C, apoi scade în câteva minute spre 35-40 °C.
// Rezoluția este de aproximativ 1 °C. Ca să provocați alarme, puneți pragul
// cu SET_THRESHOLD cu un grad sub valoarea citită.
float read_temperature(void) {
    return temperatureRead();
}

// TODO 1: TASK PERIODIC, cu perioada T = period_ms (inițial PERIOD_MS_INITIAL).

// Evenimentul de alarmă: se pune în coadă prin valoare (xQueueSend copiază elementul).
struct AlarmEvent {
    float temperature;
    float threshold;
};

// O comandă primită: textul însuși, nu un pointer (text din command_received()
// este valid doar în timpul apelului).
struct CommandMessage {
    char text[COMMAND_MAX_LEN];
};

QueueHandle_t alarm_queue;
QueueHandle_t command_queue;

// Modificate de taskul aperiodic, citite de cel periodic: variabile globale volatile.
volatile uint32_t period_ms = PERIOD_MS_INITIAL;
volatile float threshold = THRESHOLD_INITIAL;

void periodic_task(void* param) {
    (void)param;
    TickType_t last_wake = xTaskGetTickCount();  // r_0: prima eliberare este acum
    for (;;) {
        float t = read_temperature();
        char text[80];
        snprintf(text, sizeof(text), "%s;%lld;%.2f", DEVICE_ID, (long long)epoch_ms(), t);
        mqtt_publish("telemetry", text);
        Serial.printf("[periodic] %s\n", text);

        if (t > threshold) {
            AlarmEvent ev = {t, threshold};
            xQueueSend(alarm_queue, &ev, 0);  // timeout 0: nu blochează niciodată
        }

        // așteptare absolută: următoarea eliberare = last_wake + T; last_wake se
        // actualizează singur; period_ms se citește la fiecare job
        xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(period_ms));
    }
}

// TODO 2: TASK SPORADIC, tratează alarmele de temperatură.
void sporadic_task(void* param) {
    (void)param;
    uint32_t last_release_ms = 0;  // momentul ultimei alarme PUBLICATE (timp monoton)
    bool first_alarm = true;       // la prima alarmă nu există "ultima eliberare"

    for (;;) {
        AlarmEvent ev;
        // așteaptă (blocat, fără să consume procesor) următorul eveniment
        if (xQueueReceive(alarm_queue, &ev, portMAX_DELAY) != pdTRUE) continue;
        uint32_t arrival_ms = millis();

        if (!first_alarm) {
            uint32_t since_last = arrival_ms - last_release_ms;
            if (since_last < MIN_INTERARRIVAL_MS) {
                Serial.printf("[sporadic] alarma (%.2f) RESPINSA: au trecut doar %lu ms (interval minim %lu ms)\n",
                              ev.temperature, (unsigned long)since_last, (unsigned long)MIN_INTERARRIVAL_MS);
                continue;  // evenimentul respins nu se mai procesează
            }
        }

        // eveniment acceptat: acesta este jobul eliberat
        last_release_ms = arrival_ms;
        first_alarm = false;

        char text[96];
        snprintf(text, sizeof(text), "%s;%lld;%.2f;%.2f", DEVICE_ID, (long long)epoch_ms(), ev.temperature,
                 ev.threshold);
        mqtt_publish("alarm", text);
        Serial.printf("[sporadic] alarma publicata: %s\n", text);
    }
}

// TODO 3: TASK APERIODIC, execută comenzile primite de la business service.
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

// limitele acceptate pentru SET_INTERVAL (xTaskDelayUntil nu acceptă perioada 0)
const uint32_t PERIOD_MS_MIN = 10;
const uint32_t PERIOD_MS_MAX = 3600000;

// Execută o comandă și scrie în result confirmarea ("OK ..." sau "ERROR ...").
void execute_command(const char* command, char* result, size_t size) {
    char buf[COMMAND_MAX_LEN];
    strncpy(buf, command, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* save = NULL;
    char* name = strtok_r(buf, " \t\r\n", &save);
    char* arg = strtok_r(NULL, " \t\r\n", &save);
    char* extra = strtok_r(NULL, " \t\r\n", &save);

    if (name == NULL) {
        snprintf(result, size, "ERROR comanda goala");
        return;
    }
    for (char* p = name; *p != '\0'; p++) *p = (char)toupper((unsigned char)*p);

    if (strcmp(name, "SET_INTERVAL") == 0) {
        if (arg == NULL || extra != NULL) {
            snprintf(result, size, "ERROR SET_INTERVAL cere un parametru");
            return;
        }
        char* end = NULL;
        long ms = strtol(arg, &end, 10);
        if (end == arg || *end != '\0') {
            snprintf(result, size, "ERROR parametru invalid");
            return;
        }
        if (ms < (long)PERIOD_MS_MIN || ms > (long)PERIOD_MS_MAX) {
            snprintf(result, size, "ERROR perioada trebuie sa fie intre %lu si %lu ms",
                     (unsigned long)PERIOD_MS_MIN, (unsigned long)PERIOD_MS_MAX);
            return;
        }
        period_ms = (uint32_t)ms;
        snprintf(result, size, "OK SET_INTERVAL %ld", ms);

    } else if (strcmp(name, "SET_THRESHOLD") == 0) {
        if (arg == NULL || extra != NULL) {
            snprintf(result, size, "ERROR SET_THRESHOLD cere un parametru");
            return;
        }
        char* end = NULL;
        float value = strtof(arg, &end);
        if (end == arg || *end != '\0') {
            snprintf(result, size, "ERROR parametru invalid");
            return;
        }
        if (!(value > -100.0f && value < 1000.0f)) {  // respinge și NaN / infinit
            snprintf(result, size, "ERROR prag invalid");
            return;
        }
        threshold = value;
        snprintf(result, size, "OK SET_THRESHOLD %.2f", value);

    } else if (strcmp(name, "RESET") == 0) {
        period_ms = PERIOD_MS_INITIAL;
        threshold = THRESHOLD_INITIAL;
        snprintf(result, size, "OK RESET");

    } else if (strcmp(name, "STATUS") == 0) {
        snprintf(result, size, "OK STATUS interval=%lu prag=%.2f", (unsigned long)period_ms, (double)threshold);

    } else {
        snprintf(result, size, "ERROR comanda necunoscuta: %s", name);
    }
}

void aperiodic_task(void* param) {
    (void)param;
    for (;;) {
        CommandMessage cmd;
        // așteaptă (blocat) următoarea comandă; fiecare comandă eliberează un job
        if (xQueueReceive(command_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;

        char result[96];
        execute_command(cmd.text, result, sizeof(result));
        // ';' separă câmpurile mesajului, deci nu trebuie să apară în text
        for (char* p = result; *p != '\0'; p++) {
            if (*p == ';') *p = ',';
        }

        char text[160];
        snprintf(text, sizeof(text), "%s;%lld;%s", DEVICE_ID, (long long)epoch_ms(), result);
        mqtt_publish("status", text);
        Serial.printf("[aperiodic] %s -> %s\n", cmd.text, result);
    }
}

// Apelată de clientul MQTT pentru fiecare comandă primită (vezi connection.h).
void command_received(const char* text) {
    Serial.printf("[mqtt] comandă primită: %s\n", text);
    // TODO 3: comanda se copiază în coadă, fără blocare (timeout 0)
    CommandMessage msg;
    strncpy(msg.text, text, sizeof(msg.text) - 1);
    msg.text[sizeof(msg.text) - 1] = '\0';
    if (xQueueSend(command_queue, &msg, 0) != pdTRUE) {
        Serial.println("[mqtt] coada de comenzi este plina, comanda pierduta");
    }
}

void setup() {
    Serial.begin(115200);
    // Fără așteptare la scriere: cu placa la USB, dar fără Serial Monitor deschis,
    // fiecare Serial.printf() ar aștepta până la 2 s să fie citit și ar întârzia
    // taskul care scrie. Așa, textul pe care nu-l citește nimeni se pierde.
    Serial.setTxTimeoutMs(0);
    uint32_t t = millis();
    while (!Serial && millis() - t < 3000) delay(10);  // așteptăm Serial Monitor, cel mult 3 s
    delay(200);
    Serial.println();
    Serial.printf("=== tema 02: STUDENT %s, DEVICE_ID %s ===\n", STUDENT, DEVICE_ID);

    wifi_connect();
    time_sync();

    // TODO 0 (a): cozile, înainte de mqtt_start() (de acolo încolo pot sosi comenzi)
    alarm_queue = xQueueCreate(10, sizeof(AlarmEvent));
    command_queue = xQueueCreate(10, sizeof(CommandMessage));

    mqtt_start();

    // TODO 0 (b): taskurile, după mqtt_start(). Priorități: periodic > sporadic > aperiodic
    xTaskCreate(periodic_task, "periodic", 4096, NULL, 5, NULL);
    xTaskCreate(sporadic_task, "sporadic", 4096, NULL, 4, NULL);
    xTaskCreate(aperiodic_task, "aperiodic", 4096, NULL, 3, NULL);
}

void loop() {
    // Totul se face în taskuri: loop() nu are nimic de făcut.
    delay(1000);
}