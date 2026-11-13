/*
 * nRF smoke node (Zephyr). BLE name = BEDROOM1 / BEDROOM2.
 *
 * The node builds this packet itself (sent over BLE as text):
 *   GSM:NOT CONNECTED|WIFI:NOT CONNECTED|SLAVE:BEDROOM1|MSG:SMOKE DETECTED|TIME:hh:mm:ss|DATE:dd/mm/yyyy
 *
 * WIFI:CONNECTED or WIFI:NOT CONNECTED is the gateway's WiFi state. The ESP32
 * writes 1 (connected) or 0 (not connected) to the "WiFi Status" characteristic.
 * If the BLE link drops, the node goes back to NOT CONNECTED by itself.
 *
 * The ESP32 gateway adds "RSSD:<its MAC>|" in front and forwards it to the server.
 * Time comes from the gateway: it writes the current local time to the
 * "Time Sync" characteristic after connecting, then the node counts on its own.
 */
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

/* ============ CHANGE THIS: 1 for board 1, 2 for board 2 ============ */
#define NODE_NUMBER   2
/* ==================================================================== */

#define _STR(x) #x
#define STR(x)  _STR(x)
/* BLE name (seen in nRF Connect / scanners) AND Slave ID in the packet:
 *   NODE_NUMBER 1 -> "BEDROOM1"      NODE_NUMBER 2 -> "BEDROOM2"
 * Also set the same name in prj.conf: CONFIG_BT_DEVICE_NAME="BEDROOM1" or "BEDROOM2" */
#define DEV_NAME    "BEDROOM" STR(NODE_NUMBER)
#define SLAVE_ID    DEV_NAME

/* name of the OTHER bedroom, used in the REMOTE ALERT message */
#if NODE_NUMBER == 1
#define OTHER_NAME  "BEDROOM2"
#else
#define OTHER_NAME  "BEDROOM1"
#endif
#define GSM_STATUS   "NOT CONNECTED"               /* change when a GSM module is added */
#define WIFI_ON_TEXT  "CONNECTED"                  /* gateway wrote 1 */
#define WIFI_OFF_TEXT "NOT CONNECTED"              /* gateway wrote 0 / BLE link lost */

/* ---------------- PINS ---------------- */
#define IR_LED_PIN       12
#define SENSOR_PIN       10
#define SWITCH_PIN       25
#define STATUS_LED_PIN   20
#define BUZZER_PIN        9

/* ---------------- SETTINGS ---------------- */
#define SWITCH_ACTIVE_LEVEL   0
#define LED_ACTIVE_LEVEL      1
#define BUZZER_ACTIVE_LEVEL   1

#define DEBOUNCE_TIME_MS      40
#define SWITCH_BLINK_MS       1000
#define SMOKE_BLINK_MS        300
#define BOTH_BLINK_MS         100
#define REMOTE_BLINK_MS       150

#define HIT_MAX        20
#define HIT_ON         15
#define HIT_OFF         5

#define HEARTBEAT_MS       1000
#define TEXT_REFRESH_MS    1000
#define REMOTE_TIMEOUT_MS  4000
#define BT_CHECK_MS        2000

/* flag bits (byte 3 of the status packet) */
#define F_SMOKE    0x01
#define F_SWITCH   0x02
#define F_BUZZER   0x04
#define F_REMOTE   0x08
#define F_IDLE     0x10
#define F_RAW      0x20

/* ---------------- BLE UUIDs ---------------- */
#define SVC_UUID_VAL     BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)
#define STATUS_UUID_VAL  BT_UUID_128_ENCODE(0x12345679, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)
#define ALARM_UUID_VAL   BT_UUID_128_ENCODE(0x1234567a, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)
#define TEXT_UUID_VAL    BT_UUID_128_ENCODE(0x1234567b, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)
#define TIME_UUID_VAL    BT_UUID_128_ENCODE(0x1234567c, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)
#define WIFI_UUID_VAL    BT_UUID_128_ENCODE(0x1234567d, 0x1234, 0x5678, 0x1234, 0x56789abcdef0)

#define SVC_UUID     BT_UUID_DECLARE_128(SVC_UUID_VAL)
#define STATUS_UUID  BT_UUID_DECLARE_128(STATUS_UUID_VAL)
#define ALARM_UUID   BT_UUID_DECLARE_128(ALARM_UUID_VAL)
#define TEXT_UUID    BT_UUID_DECLARE_128(TEXT_UUID_VAL)   /* now carries the full packet */
#define TIME_UUID    BT_UUID_DECLARE_128(TIME_UUID_VAL)   /* written by the ESP32 */
#define WIFI_UUID    BT_UUID_DECLARE_128(WIFI_UUID_VAL)   /* written by the ESP32 */

static const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));

/* Status packet, 7 bytes: [0-1] board, [2] counter, [3] flags, [4] score, [5-6] uptime */
static uint8_t status_buf[7];
static uint8_t my_seq;

/* Same info inside the advertisement: FF FF 'S' <board> <flags> <score> */
static uint8_t node_data[6] = {0xFF, 0xFF, 'S', NODE_NUMBER, 0, 0};

/* Alert written by the ESP32 about the OTHER board */
static volatile uint8_t  remote_flag;
static volatile uint32_t remote_ts;

/* Gateway WiFi state, written by the ESP32 (0 = not connected, 1 = connected) */
static volatile bool wifi_connected;
static volatile bool wifi_changed;      /* forces the packet to be sent right away */

/* ---------------- CLOCK (set by the ESP32) ---------------- */
static volatile uint32_t time_base;      /* local epoch seconds minus uptime seconds */
static volatile bool     time_synced;

static uint32_t uptime_s(void) { return (uint32_t)(k_uptime_get() / 1000); }

/* days since 1970-01-01 -> year, month, day */
static void civil(int32_t days, int *y, int *m, int *d)
{
    int32_t  z   = days + 719468;
    int32_t  era = z / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp  = (5 * doy + 2) / 153;

    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

/* ---------------- THE PACKET ---------------- */
#define PKT_MAX 112     /* longest packet is 109 characters (86 + 23 message) */
static char status_text[PKT_MAX] = "";

/* Message texts (max 23 characters so the packet fits the BLE MTU).
 * "REMOTE" = the OTHER bedroom has an alarm; this board buzzes as a warning.
 * If this board has no alarm of its own, the message names the bedroom. */
static const char *msg_string(int smoke, int sw, int remote)
{
    if (smoke && sw) return remote ? "SMOKE + SWITCH + REMOTE" : "SMOKE + SWITCH";
    if (smoke)       return remote ? "SMOKE DETECTED + REMOTE" : "SMOKE DETECTED";
    if (sw)          return remote ? "SWITCH PRESSED + REMOTE" : "SWITCH PRESSED";
    if (remote)      return "REMOTE ALERT: " OTHER_NAME;
    return "ALL CLEAR";
}

/* GSM:..|WIFI:..|SLAVE:..|MSG:..|TIME:hh:mm:ss|DATE:dd/mm/yyyy */
static void build_packet(char *out, size_t cap, const char *msg)
{
    const char *wifi = wifi_connected ? WIFI_ON_TEXT : WIFI_OFF_TEXT;

    if (!time_synced) {
        snprintf(out, cap,
                 "GSM:%s|WIFI:%s|SLAVE:%s|MSG:%s|TIME:--:--:--|DATE:--/--/----",
                 GSM_STATUS, wifi, SLAVE_ID, msg);
        return;
    }

    uint32_t t   = uptime_s() + time_base;
    uint32_t sec = t % 86400u;
    int y, m, d;
    civil((int32_t)(t / 86400u), &y, &m, &d);

    snprintf(out, cap,
             "GSM:%s|WIFI:%s|SLAVE:%s|MSG:%s|TIME:%02u:%02u:%02u|DATE:%02d/%02d/%04d",
             GSM_STATUS, wifi, SLAVE_ID, msg,
             (unsigned)(sec / 3600), (unsigned)((sec % 3600) / 60), (unsigned)(sec % 60),
             d, m, y);
}

/* ---------------- GATT callbacks ---------------- */
static ssize_t read_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           void *buf, uint16_t len, uint16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             status_buf, sizeof(status_buf));
}

static ssize_t read_text(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                         void *buf, uint16_t len, uint16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset,
                             status_text, strlen(status_text));
}

static ssize_t write_alarm(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           const void *buf, uint16_t len, uint16_t offset,
                           uint8_t flags)
{
    if (len < 1) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    remote_flag = ((const uint8_t *)buf)[0] ? 1 : 0;
    remote_ts = k_uptime_get_32();
    return len;
}

/* ESP32 writes 4 bytes: local time as seconds since 1970 (little-endian) */
static ssize_t write_time(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          const void *buf, uint16_t len, uint16_t offset,
                          uint8_t flags)
{
    if (len != 4) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    uint32_t epoch = sys_get_le32(buf);
    time_base = epoch - uptime_s();
    time_synced = true;
    printk("Time synced from gateway\n");
    return len;
}

/* ESP32 writes 1 byte: 1 = WiFi connected, 0 = WiFi not connected */
static ssize_t write_wifi(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          const void *buf, uint16_t len, uint16_t offset,
                          uint8_t flags)
{
    if (len < 1) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    bool now_on = ((const uint8_t *)buf)[0] ? true : false;

    if (now_on != wifi_connected) {
        wifi_connected = now_on;
        wifi_changed = true;
    }
    return len;
}

static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    ARG_UNUSED(attr);
    printk("Notifications %s\n", (value == BT_GATT_CCC_NOTIFY) ? "on" : "off");
}

static const struct bt_gatt_cpf text_format = {
    .format = 0x19, .exponent = 0, .unit = 0x2700,
    .name_space = 1, .description = 0,
};

/* Each notify characteristic must be followed directly by its CCC
 * (the ESP32 uses value handle + 1). */
BT_GATT_SERVICE_DEFINE(
    smoke_svc,
    BT_GATT_PRIMARY_SERVICE(SVC_UUID),

    /* 1) raw status bytes */
    BT_GATT_CHARACTERISTIC(STATUS_UUID,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ,
                           read_status, NULL, NULL),
    BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    BT_GATT_CUD("Status Bytes (hex)", BT_GATT_PERM_READ),

    /* 2) alert from the other board (written by the ESP32) */
    BT_GATT_CHARACTERISTIC(ALARM_UUID,
                           BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_WRITE,
                           NULL, write_alarm, NULL),
    BT_GATT_CUD("Alert From Other Board", BT_GATT_PERM_READ),

    /* 3) the full text packet */
    BT_GATT_CHARACTERISTIC(TEXT_UUID,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ,
                           read_text, NULL, NULL),
    BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    BT_GATT_CUD("Packet", BT_GATT_PERM_READ),
    BT_GATT_CPF(&text_format),

    /* 4) time sync (written by the ESP32) */
    BT_GATT_CHARACTERISTIC(TIME_UUID,
                           BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_WRITE,
                           NULL, write_time, NULL),
    BT_GATT_CUD("Time Sync", BT_GATT_PERM_READ),

    /* 5) gateway WiFi status (written by the ESP32): 1 = connected, 0 = not */
    BT_GATT_CHARACTERISTIC(WIFI_UUID,
                           BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_WRITE,
                           NULL, write_wifi, NULL),
    BT_GATT_CUD("WiFi Status", BT_GATT_PERM_READ)
);

/* flags (3) + 128-bit UUID (18) + node data (8) = 29 bytes (max 31) */
static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, SVC_UUID_VAL),
    BT_DATA(BT_DATA_MANUFACTURER_DATA, node_data, sizeof(node_data)),
};

static const struct bt_data sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, DEV_NAME, sizeof(DEV_NAME) - 1),
};

static bool adv_ok;

static int start_adv(void)
{
    int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad),
                              sd, ARRAY_SIZE(sd));

    if (err == -EALREADY) { adv_ok = true;  return 0; }
    if (err == -ENOMEM)   { adv_ok = false; return 0; }
    if (err) {
        printk("Advertising failed: %d\n", err);
        adv_ok = false;
        return err;
    }
    adv_ok = true;
    return 0;
}

static void connected(struct bt_conn *conn, uint8_t err)
{
    ARG_UNUSED(conn);
    if (err) {
        printk("Connect failed (%u)\n", err);
        return;
    }
    printk("Connected\n");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    ARG_UNUSED(conn);
    printk("Disconnected (0x%02x)\n", reason);

    /* no gateway link -> we can no longer know its WiFi state */
    if (wifi_connected) {
        wifi_connected = false;
        wifi_changed = true;
    }
}

static void recycled(void)
{
    start_adv();
}

BT_CONN_CB_DEFINE(connection_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
    .recycled = recycled,
};

/* Update the advertisement and notify subscribed devices */
static void publish_status(uint8_t flags, uint8_t score)
{
    uint16_t up = (uint16_t)(k_uptime_get() / 1000);

    status_buf[0] = NODE_NUMBER & 0xFF;
    status_buf[1] = NODE_NUMBER >> 8;
    status_buf[2] = my_seq;
    status_buf[3] = flags;
    status_buf[4] = score;
    status_buf[5] = up & 0xFF;
    status_buf[6] = up >> 8;

    node_data[4] = flags;
    node_data[5] = score;

    if (adv_ok) {
        bt_le_adv_update_data(ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    }

    bt_gatt_notify_uuid(NULL, STATUS_UUID, smoke_svc.attrs,
                        status_buf, sizeof(status_buf));
}

/* Build the packet for this message and notify it */
static void publish_packet(const char *msg)
{
    build_packet(status_text, sizeof(status_text), msg);

    bt_gatt_notify_uuid(NULL, TEXT_UUID, smoke_svc.attrs,
                        status_text, strlen(status_text));
}

/* ---------------- SENSOR ---------------- */
static int read_sensor_once(void)
{
    int v;

    gpio_pin_set_raw(gpio0, IR_LED_PIN, 1);
    k_busy_wait(1000);
    v = gpio_pin_get_raw(gpio0, SENSOR_PIN);
    gpio_pin_set_raw(gpio0, IR_LED_PIN, 0);

    return v;
}

static int learn_idle_level(void)
{
    int ones = 0, total = 0;

    for (int i = 0; i < 100; i++) {
        int v = read_sensor_once();
        if (v >= 0) {
            total++;
            if (v == 1) ones++;
        }
        k_msleep(10);
    }
    return (ones * 2 > total) ? 1 : 0;
}

static void blink_code(int count, int ms)
{
    k_msleep(500);
    for (int i = 0; i < count; i++) {
        gpio_pin_set(gpio0, STATUS_LED_PIN, LED_ACTIVE_LEVEL);
        k_msleep(ms);
        gpio_pin_set(gpio0, STATUS_LED_PIN, !LED_ACTIVE_LEVEL);
        k_msleep(ms);
    }
    k_msleep(500);
}

/* ---------------- MAIN ---------------- */
int main(void)
{
    int idle_level;
    int hits = 0;
    int smoke_detected = 0;
    int raw = 0;

    int switch_raw, previous_switch, stable_switch;
    int switch_pressed = 0;

    int led_state = 0;
    int blink_interval = 0, previous_blink_interval = 0;
    int buzzer_on = 0;

    int64_t now, switch_change_time, last_blink_time;
    int64_t last_heartbeat, last_text_time = 0, last_bt_check = 0;
    uint8_t prev_state = 0xFF;
    const char *old_text = NULL;

    if (!device_is_ready(gpio0)) return 0;

    if (gpio_pin_configure(gpio0, IR_LED_PIN, GPIO_OUTPUT_INACTIVE) < 0)      return 0;
    if (gpio_pin_configure(gpio0, SENSOR_PIN, GPIO_INPUT | GPIO_PULL_UP) < 0) return 0;
    if (gpio_pin_configure(gpio0, SWITCH_PIN, GPIO_INPUT | GPIO_PULL_UP) < 0) return 0;
    if (gpio_pin_configure(gpio0, STATUS_LED_PIN, GPIO_OUTPUT_INACTIVE) < 0)  return 0;
    if (gpio_pin_configure(gpio0, BUZZER_PIN, GPIO_OUTPUT_INACTIVE) < 0)      return 0;

    /* Calibration: LED on ~1 s, keep the chamber in clean air */
    gpio_pin_set_raw(gpio0, STATUS_LED_PIN, 1);
    idle_level = learn_idle_level();
    gpio_pin_set_raw(gpio0, STATUS_LED_PIN, 0);

    switch_raw = gpio_pin_get(gpio0, SWITCH_PIN);
    if (switch_raw < 0) return 0;
    previous_switch = stable_switch = switch_raw;
    switch_change_time = k_uptime_get();
    switch_pressed = (stable_switch == SWITCH_ACTIVE_LEVEL);

    /* ----- Bluetooth start-up ----- */
    int e_bt = bt_enable(NULL);
    if (e_bt) {
        printk("Bluetooth init failed: %d\n", e_bt);
        while (1) { blink_code(6, 400); }       /* 6 slow blinks = bt_enable failed */
    }

    node_data[4] = idle_level ? F_IDLE : 0;
    int e_adv = start_adv();

    printk("%s: idle=%d adv=%d (0 = ok)\n", DEV_NAME, idle_level, e_adv);

    if (e_adv) blink_code(2, 500);              /* 2 slow blinks = advertising failed */
    else       blink_code(3, 100);              /* 3 quick blinks = all OK            */

    last_blink_time = last_heartbeat = last_bt_check = k_uptime_get();

    while (1) {
        now = k_uptime_get();

        /* ----- smoke sensor with filtering ----- */
        int s = read_sensor_once();
        if (s >= 0) {
            raw = s;
            if (s != idle_level) { if (hits < HIT_MAX) hits++; }
            else                 { if (hits > 0) hits--; }

            if (hits >= HIT_ON)       smoke_detected = 1;
            else if (hits <= HIT_OFF) smoke_detected = 0;
        }

        /* ----- switch with debounce ----- */
        switch_raw = gpio_pin_get(gpio0, SWITCH_PIN);
        if (switch_raw >= 0) {
            if (switch_raw != previous_switch) {
                previous_switch = switch_raw;
                switch_change_time = now;
            }
            if ((now - switch_change_time) >= DEBOUNCE_TIME_MS) {
                stable_switch = previous_switch;
            }
            switch_pressed = (stable_switch == SWITCH_ACTIVE_LEVEL);
        }

        /* ----- alert from the other board (written by the ESP32) ----- */
        int remote_alarm = remote_flag &&
                           ((k_uptime_get_32() - remote_ts) < REMOTE_TIMEOUT_MS);

        /* ----- LED blink pattern ----- */
        if (smoke_detected && switch_pressed) blink_interval = BOTH_BLINK_MS;
        else if (smoke_detected)              blink_interval = SMOKE_BLINK_MS;
        else if (switch_pressed)              blink_interval = SWITCH_BLINK_MS;
        else if (remote_alarm)                blink_interval = REMOTE_BLINK_MS;
        else                                  blink_interval = 0;

        if (blink_interval != previous_blink_interval) {
            last_blink_time = now;
            led_state = (blink_interval != 0);
            previous_blink_interval = blink_interval;
        }

        if (blink_interval > 0) {
            if ((now - last_blink_time) >= blink_interval) {
                led_state = !led_state;
                last_blink_time = now;
            }
        } else {
            led_state = 0;
        }

        gpio_pin_set(gpio0, STATUS_LED_PIN,
                     led_state ? LED_ACTIVE_LEVEL : !LED_ACTIVE_LEVEL);

        /* ----- buzzer ----- */
        buzzer_on = (smoke_detected || switch_pressed || remote_alarm);
        gpio_pin_set(gpio0, BUZZER_PIN,
                     buzzer_on ? BUZZER_ACTIVE_LEVEL : !BUZZER_ACTIVE_LEVEL);

        /* ----- packet: sent on every change and refreshed every second ----- */
        const char *msg = msg_string(smoke_detected, switch_pressed, remote_alarm);

        if (msg != old_text || wifi_changed) {
            old_text = msg;
            wifi_changed = false;
            last_text_time = now;
            publish_packet(msg);
            printk("%s\n", status_text);
        } else if ((now - last_text_time) >= TEXT_REFRESH_MS) {
            last_text_time = now;
            publish_packet(msg);
        }

        /* ----- status packet (advertisement + notification) ----- */
        uint8_t flags = (smoke_detected ? F_SMOKE  : 0) |
                        (switch_pressed ? F_SWITCH : 0) |
                        (buzzer_on      ? F_BUZZER : 0) |
                        (remote_alarm   ? F_REMOTE : 0) |
                        (idle_level     ? F_IDLE   : 0) |
                        (raw            ? F_RAW    : 0);

        uint8_t state = flags & 0x0F;

        if (state != prev_state) {
            prev_state = state;
            my_seq++;
            last_heartbeat = now;
            publish_status(flags, hits);
        } else if ((now - last_heartbeat) >= HEARTBEAT_MS) {
            last_heartbeat = now;
            my_seq++;
            publish_status(flags, hits);
        }

        /* ----- make sure advertising keeps running ----- */
        if ((now - last_bt_check) >= BT_CHECK_MS) {
            last_bt_check = now;
            start_adv();
        }

        k_msleep(10);
    }

    return 0;
}
