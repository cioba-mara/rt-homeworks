// Semafor pentru pietoni, cu buton, pe ESP32-C3 Super Mini.
//
// Placa este controlerul semaforului; lămpile sunt desenate de dashboard, care
// le aprinde după mesajele "lights" publicate de placă. Patru activități:
//
//   ISR buton BOOT ----+
//                      +--(coada butonului)--> task sporadic
//   command_received --+   (BUTTON)                 |
//                                                   v
//                                         [ starea semaforului ] <-- mutex
//                                            ^          |
//                                            |          v
//                         task periodic de control   task periodic de ieșire
//                         (fazele, T = 100 ms)       (lămpile, T = 500 ms) --> lights
//
// Gata implementate (nu trebuie modificate):
//   config.h         setările: Wi-Fi, broker, STUDENT, pinii, perioadele, duratele fazelor
//   connection.h/cpp Wi-Fi, ora exactă (SNTP), clientul MQTT, mqtt_publish(), epoch_ms()
//
// Așa cum este, schița se conectează la Wi-Fi și la broker și afișează pe
// Serial comenzile primite. Restul îl scrieți voi: TODO 0 ... TODO 3.
//
// Placa: ESP32C3 Dev Module, USB CDC On Boot: Enabled. Serial Monitor: 115200 baud.

#include "config.h"
#include "connection.h"

#include <string.h>

// Fazele semaforului (vezi tabelul și diagrama din README.md). Numele din
// PHASE_NAMES sunt cele din mesajul "lights", în aceeași ordine ca în enum.
enum Phase { CAR_GREEN, CAR_YELLOW, ALL_RED, PED_GREEN };
const char* const PHASE_NAMES[] = {"CAR_GREEN", "CAR_YELLOW", "ALL_RED", "PED_GREEN"};

// Culorile unei lămpi, cu numele din mesajul "lights".
enum Light { RED, YELLOW, GREEN };
const char* const LIGHT_NAMES[] = {"RED", "YELLOW", "GREEN"};

// TODO 0 (c): STAREA SEMAFORULUI, partajată de taskuri și protejată de un mutex.
struct SemaforState {
    Phase phase;               // faza curentă
    uint32_t phase_entry_ms;   // millis() la intrarea în faza curentă
    bool request;              // cererea pietonului (lampa AȘTEPTAȚI)
};

SemaforState state;            // se citește și se scrie doar cu state_mutex luat
SemaphoreHandle_t state_mutex;

// O apăsare de buton: momentul se ia la intrarea în coadă (în ISR sau în
// command_received()), în ms, și se pune în elementul cozii.
struct ButtonPress {
    uint32_t time_ms;
};

QueueHandle_t button_queue;

// TODO 1: TASK PERIODIC DE CONTROL, cu perioada CONTROL_PERIOD_MS.
void control_task(void* param) {
    (void)param;
    TickType_t last_wake = xTaskGetTickCount();  // r_0: prima eliberare este acum
    for (;;) {
        bool changed = false;
        Phase new_phase = CAR_GREEN;

        xSemaphoreTake(state_mutex, portMAX_DELAY);
        uint32_t now = millis();                        // timp monoton
        uint32_t elapsed = now - state.phase_entry_ms;  // cât durează faza curentă
        bool leave = false;
        Phase next = state.phase;
        switch (state.phase) {
            case CAR_GREEN:   // după verdele minim, doar dacă există o cerere
                leave = (elapsed >= CAR_GREEN_MIN_MS) && state.request;
                next = CAR_YELLOW;
                break;
            case CAR_YELLOW:
                leave = (elapsed >= CAR_YELLOW_MS);
                next = ALL_RED;
                break;
            case ALL_RED:
                leave = (elapsed >= ALL_RED_MS);
                next = PED_GREEN;
                break;
            case PED_GREEN:
                leave = (elapsed >= PED_GREEN_MS);
                next = CAR_GREEN;
                break;
        }
        if (leave) {
            state.phase = next;
            state.phase_entry_ms = now;       // momentul intrării în faza nouă
            if (next == PED_GREEN) {
                state.request = false;        // pietonul a primit ce a cerut
            }
            changed = true;
            new_phase = next;
        }
        xSemaphoreGive(state_mutex);

        // Serial doar după ce mutexul a fost eliberat
        if (changed) {
            Serial.printf("[control] faza noua: %s\n", PHASE_NAMES[new_phase]);
        }

        // așteptare absolută: următoarea eliberare = last_wake + T
        xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(CONTROL_PERIOD_MS));
    }
}

// TODO 2: TASK PERIODIC DE IEȘIRE, cu perioada OUTPUT_PERIOD_MS.
void output_task(void* param) {
    (void)param;
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        // copiem starea cu mutexul luat, cât mai scurt
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        SemaforState s = state;
        xSemaphoreGive(state_mutex);
        uint32_t now = millis();  // după copiere: now >= s.phase_entry_ms

        // culorile lămpilor din faza curentă
        Light cars = RED;
        Light peds = RED;
        uint32_t seconds = 0;  // numărătoarea inversă; 0 = nu se afișează
        switch (s.phase) {
            case CAR_GREEN:
                cars = GREEN;
                peds = RED;
                break;
            case CAR_YELLOW:
                cars = YELLOW;
                peds = RED;
                break;
            case ALL_RED:
                cars = RED;
                peds = RED;
                break;
            case PED_GREEN: {
                cars = RED;
                peds = GREEN;
                uint32_t elapsed = now - s.phase_entry_ms;
                uint32_t remaining = (elapsed >= PED_GREEN_MS) ? 0 : (PED_GREEN_MS - elapsed);
                seconds = (remaining + 999) / 1000;  // rotunjit în sus
                break;
            }
        }

        char text[96];
        snprintf(text, sizeof(text), "%s;%lld;%s;%s;%s;%d;%lu", DEVICE_ID, (long long)epoch_ms(),
                 PHASE_NAMES[s.phase], LIGHT_NAMES[cars], LIGHT_NAMES[peds], s.request ? 1 : 0,
                 (unsigned long)seconds);
        mqtt_publish("lights", text);

        // LED-ul plăcii: aprins pe LOW, cât timp lampa AȘTEPTAȚI este aprinsă
        digitalWrite(LED_PIN, s.request ? LOW : HIGH);

        xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(OUTPUT_PERIOD_MS));
    }
}

// TODO 3: TASK SPORADIC, tratează apăsările butonului pentru pietoni.
void button_task(void* param) {
    (void)param;
    uint32_t last_accepted_ms = 0;  // momentul ultimei apăsări ACCEPTATE
    bool first_press = true;        // la prima apăsare nu există una anterioară

    for (;;) {
        ButtonPress press;
        // așteaptă (blocat, fără să consume procesor) o apăsare
        if (xQueueReceive(button_queue, &press, portMAX_DELAY) != pdTRUE) continue;

        if (!first_press) {
            uint32_t since_last = press.time_ms - last_accepted_ms;
            if (since_last < MIN_INTERARRIVAL_MS) {
                Serial.printf("[buton] apasare RESPINSA: au trecut doar %lu ms (interval minim %lu ms)\n",
                              (unsigned long)since_last, (unsigned long)MIN_INTERARRIVAL_MS);
                continue;  // apăsarea respinsă nu se mai procesează
            }
        }

        // apăsare acceptată: acesta este jobul eliberat
        first_press = false;
        last_accepted_ms = press.time_ms;

        xSemaphoreTake(state_mutex, portMAX_DELAY);
        state.request = true;  // lampa AȘTEPTAȚI
        xSemaphoreGive(state_mutex);

        Serial.println("[buton] cerere inregistrata");
    }
}

// TODO 3: RUTINA DE ÎNTRERUPERE a butonului BOOT, înregistrată în setup()
// Scurtă: doar pune apăsarea în coadă. Fără Serial, mqtt_publish() sau mutex.
void IRAM_ATTR button_isr(void) {
    ButtonPress press;
    press.time_ms = xTaskGetTickCountFromISR() * portTICK_PERIOD_MS;
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(button_queue, &press, &woken);
    portYIELD_FROM_ISR(woken);  // dacă a fost trezit un task mai prioritar, rulează imediat
}

// Apelată de clientul MQTT pentru fiecare comandă primită (vezi connection.h).
void command_received(const char* text) {
    Serial.printf("[mqtt] comandă primită: %s\n", text);
    // TODO 3: "BUTTON" merge în coada butonului, fără blocare (timeout 0)
    if (strcmp(text, "BUTTON") == 0) {
        ButtonPress press;
        press.time_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        xQueueSend(button_queue, &press, 0);
    }
    // celelalte comenzi se ignoră
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
    Serial.printf("=== tema 03: STUDENT %s, DEVICE_ID %s ===\n", STUDENT, DEVICE_ID);

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);  // LED stins
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    wifi_connect();
    time_sync();

    // TODO 0 (a): mutexul stării și coada butonului, înainte de mqtt_start()
    state_mutex = xSemaphoreCreateMutex();
    button_queue = xQueueCreate(10, sizeof(ButtonPress));
    state.phase = CAR_GREEN;
    state.phase_entry_ms = millis();  // momentul intrării în faza inițială
    state.request = false;

    mqtt_start();

    // TODO 0 (b): taskurile, apoi întreruperea butonului.
    // Priorități după Rate Monotonic (perioada mai mică, prioritatea mai mare):
    // control (100 ms) > ieșire (500 ms) > sporadic (tratat ca periodic cu
    // T = MIN_INTERARRIVAL_MS = 1000 ms).
    xTaskCreate(control_task, "control", 4096, NULL, 6, NULL);
    xTaskCreate(output_task, "output", 4096, NULL, 5, NULL);
    xTaskCreate(button_task, "button", 4096, NULL, 4, NULL);
    attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), button_isr, FALLING);
}

void loop() {
    // Totul se face în taskuri: loop() nu are nimic de făcut.
    delay(1000);
}