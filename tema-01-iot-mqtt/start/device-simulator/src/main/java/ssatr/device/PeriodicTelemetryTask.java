package ssatr.device;

import java.util.concurrent.BlockingQueue;
import java.util.Locale;

/**
 * TASK PERIODIC, cu perioada T = periodMs: fiecare job citește senzorul și
 * publică valoarea pe topic-ul de telemetrie.
 *
 * Dacă temperatura depășește pragul, taskul NU tratează el alarma, ci doar
 * semnalează un eveniment în coada alarmEvents. Alarma este tratată de
 * taskul sporadic.
 */
public class PeriodicTelemetryTask implements Runnable {

    private final String deviceId;
    private final TemperatureSensor sensor;
    private final MqttConnection mqtt;
    private final BlockingQueue<Double> alarmEvents;

    // volatile: sunt citite aici, dar modificate de taskul aperiodic (alt thread)
    private volatile long periodMs = 2000;
    private volatile double threshold = 28.0;

    public PeriodicTelemetryTask(String deviceId, TemperatureSensor sensor, MqttConnection mqtt,
                                 BlockingQueue<Double> alarmEvents) {
        this.deviceId = deviceId;
        this.sensor = sensor;
        this.mqtt = mqtt;
        this.alarmEvents = alarmEvents;
    }

    @Override
    public void run() {
        long nextRelease = System.nanoTime();

        while (!Thread.currentThread().isInterrupted()) {
            long waitNs = nextRelease - System.nanoTime();
            if (waitNs > 0) {
                try {
                    Thread.sleep(waitNs / 1_000_000, (int) (waitNs % 1_000_000));
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    return;
                }
            }

            double temperature = sensor.read();
            String message = deviceId + ";" + System.currentTimeMillis() + ";"
                    + String.format(Locale.US, "%.2f", temperature);
            mqtt.publish(Config.telemetryTopic(deviceId), message);

            if (temperature > threshold) {
                alarmEvents.offer(temperature);
            }

            nextRelease += periodMs * 1_000_000L;
        }
    }

    public void setPeriodMs(long periodMs) {
        this.periodMs = periodMs;
    }

    public void setThreshold(double threshold) {
        this.threshold = threshold;
    }

    public long getPeriodMs() {
        return periodMs;
    }

    public double getThreshold() {
        return threshold;
    }
}
