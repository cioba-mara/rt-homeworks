package ssatr.service;

import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;

/**
 * Ultima stare cunoscută a fiecărui device: ultima temperatură, momentul
 * ultimului mesaj, numărul de alarme.
 *
 * Este actualizată de thread-urile MessageProcessor și citită de CommandConsole,
 * deci toate metodele sunt synchronized.
 */
public class Dashboard {

    /** Starea unui singur device. */
    private static class DeviceState {
        double lastTemperature = Double.NaN;   // NaN = încă nu s-a primit telemetrie
        long lastMessageMs = 0;                // ora locală a ultimului mesaj primit
        int alarms = 0;
    }

    // cheia este deviceId; TreeMap ține device-urile ordonate alfabetic
    private final Map<String, DeviceState> devices = new TreeMap<>();

    private DeviceState stateOf(String deviceId) {
        return devices.computeIfAbsent(deviceId, id -> new DeviceState());
    }

    public synchronized void updateTelemetry(String deviceId, double temperature) {
        DeviceState state = stateOf(deviceId);
        state.lastTemperature = temperature;
        state.lastMessageMs = System.currentTimeMillis();
    }

    public synchronized void updateAlarm(String deviceId) {
        DeviceState state = stateOf(deviceId);
        state.alarms++;
        state.lastMessageMs = System.currentTimeMillis();
    }

    public synchronized void updateStatus(String deviceId) {
        stateOf(deviceId).lastMessageMs = System.currentTimeMillis();
    }

    /** Afișează un tabel cu starea tuturor device-urilor. */
    public synchronized void print() {
        if (devices.isEmpty()) {
            System.out.println("(niciun device nu a trimis mesaje)");
            return;
        }

        long now = System.currentTimeMillis();
        System.out.printf("%-12s %8s   %-14s %s%n", "DEVICE", "TEMP", "ULTIMUL MESAJ", "ALARME");
        for (Map.Entry<String, DeviceState> entry : devices.entrySet()) {
            DeviceState state = entry.getValue();
            String temp = Double.isNaN(state.lastTemperature)
                    ? "-" : String.format(Locale.US, "%.2f", state.lastTemperature);
            String age = "acum " + (now - state.lastMessageMs) / 1000 + " s";
            System.out.printf("%-12s %8s   %-14s %d%n", entry.getKey(), temp, age, state.alarms);
        }
    }
}