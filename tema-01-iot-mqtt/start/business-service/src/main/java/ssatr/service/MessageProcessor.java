package ssatr.service;

import java.util.concurrent.BlockingQueue;

/**
 * Preia mesajele de un anumit tip ("telemetry", "alarm" sau "status") din coada
 * inbox, le salvează și actualizează dashboard-ul.
 *
 * Formatul mesajelor (câmpuri separate prin ";"):
 *   telemetry:  deviceId;timestamp;temperatura
 *   alarm:      deviceId;timestamp;temperatura;prag
 *   status:     deviceId;timestamp;text
 */
public class MessageProcessor implements Runnable {

    private final String kind;
    private final BlockingQueue<String> inbox;
    private final CsvStorage storage;
    private final Dashboard dashboard;

    public MessageProcessor(String kind, BlockingQueue<String> inbox, CsvStorage storage, Dashboard dashboard) {
        this.kind = kind;
        this.inbox = inbox;
        this.storage = storage;
        this.dashboard = dashboard;
    }

    @Override
    public void run() {
        try {
            while (true) {
                String message = inbox.take();
                storage.append(kind, message);

                try {
                    process(message);
                } catch (RuntimeException e) {
                    System.out.println("[" + kind + "] mesaj invalid ignorat: " + message
                            + " (" + e.getClass().getSimpleName() + ")");
                }
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }
    private void process(String message) {
        String[] parts = message.split(";");
        String deviceId = parts[0];

        switch (kind) {
            case "telemetry": {
                double temperature = Double.parseDouble(parts[2]);
                dashboard.updateTelemetry(deviceId, temperature);
                break;
            }
            case "alarm": {
                double temperature = Double.parseDouble(parts[2]);
                double threshold = Double.parseDouble(parts[3]);
                dashboard.updateAlarm(deviceId);
                System.out.printf("[ALARMA] %s: temperatura %.2f depaseste pragul %.2f%n",
                        deviceId, temperature, threshold);
                break;
            }
            case "status": {
                dashboard.updateStatus(deviceId);
                System.out.println("[STATUS] " + deviceId + ": " + parts[2]);
                break;
            }
            default:
                System.out.println("[" + kind + "] tip necunoscut");
        }
    }
}
