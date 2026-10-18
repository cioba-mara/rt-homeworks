package ssatr.device;

import java.util.concurrent.BlockingQueue;
import java.util.Locale;
/**
 * TASK SPORADIC: tratează alarmele de temperatură.
 *
 * Evenimentele sosesc în coada alarmEvents (puse de taskul periodic).
 * Intervalul minim între sosiri este MIN_INTERARRIVAL_MS: între două eliberări
 * ale taskului (două alarme publicate) trebuie să treacă cel puțin atât.
 * Intervalul este impus de cod: evenimentele care sosesc mai des sunt
 * respinse, ca să nu fie inundat brokerul.
 */
public class SporadicAlarmTask implements Runnable {

    static final long MIN_INTERARRIVAL_MS = 5000;

    private final String deviceId;
    private final MqttConnection mqtt;
    private final BlockingQueue<Double> alarmEvents;
    private final PeriodicTelemetryTask periodic;

    public SporadicAlarmTask(String deviceId, MqttConnection mqtt, BlockingQueue<Double> alarmEvents,
                             PeriodicTelemetryTask periodic) {
        this.deviceId = deviceId;
        this.mqtt = mqtt;
        this.alarmEvents = alarmEvents;
        this.periodic = periodic;
    }

    @Override
    public void run() {
        long lastReleaseNs = 0;
        boolean firstAlarm = true;

        try {
            while (true) {
                double temperature = alarmEvents.take();
                long arrivalNs = System.nanoTime();

                if (!firstAlarm) {
                    long sinceLastMs = (arrivalNs - lastReleaseNs) / 1_000_000;
                    if (sinceLastMs < MIN_INTERARRIVAL_MS) {
                        System.out.printf("[sporadic] alarma (%.2f) RESPINSA: au trecut doar %d ms (interval minim %d ms)%n",
                                temperature, sinceLastMs, MIN_INTERARRIVAL_MS);
                        continue;
                    }
                }

                lastReleaseNs = arrivalNs;
                firstAlarm = false;

                double threshold = periodic.getThreshold();
                String message = deviceId + ";" + System.currentTimeMillis() + ";"
                        + String.format(Locale.US, "%.2f", temperature) + ";"
                        + String.format(Locale.US, "%.2f", threshold);
                mqtt.publish(Config.alarmTopic(deviceId), message);
                System.out.println("[sporadic] alarma publicata: " + message);
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }
}
/**
 * Am ales citirea lui din PeriodicTelemetryTask (periodic.getThreshold()), din trei motive:
 * coada rămâne simplă (BlockingQueue<Double>, cum e deja în DeviceSimulator);
 * există o singură sursă pentru prag, cea modificată de SET_THRESHOLD;
 * threshold este volatile, deci citirea din alt thread este sigură.
 */
