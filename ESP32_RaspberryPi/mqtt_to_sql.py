import pymysql
import paho.mqtt.client as mqtt

# ================= MQTT LOCAL ON PI =================
MQTT_BROKER = "localhost"
MQTT_PORT = 1883
MQTT_SUB_TOPICS = [("nha/#", 1), ("baochay/#", 1)]

# ================= DATABASE =================
DB_HOST = "localhost"
DB_USER = "fireuser"
DB_PASS = "1234567"
DB_NAME = "fire_iot"

ROOM_NAME_MAP = {
    "hanhlang": "gara",
    "hallway": "gara",
}

ROOM_COLUMNS = {
    "nhietdo": "nhietdo",
    "doam": "doam",
    "gas": "gas",
    "khoi": "khoi",
    "khoi_raw": "khoi_raw",
    "khoi_tang": "khoi_tang",
    "khoi_nen": "khoi_nen",
    "lua": "lua",
    "canhbao_muc": "canhbao_muc",
    "canhbao_trangthai": "canhbao_trangthai",
    "muc": "muc_canh_bao",
    "muc_canh_bao": "muc_canh_bao",
    "trangthai": "trangthai",
    "status": "status",
    "node": "node",
    "device": "device",
}

NUMERIC_INT_FIELDS = {
    "gas", "khoi", "khoi_raw", "khoi_tang", "khoi_nen", "lua", "canhbao_muc"
}

NUMERIC_FLOAT_FIELDS = {
    "nhietdo", "doam"
}

SENSOR_READING_METRICS = {
    "canhbao_trangthai", "canhbao_muc", "muc", "muc_canh_bao"
}

def get_db_connection():
    return pymysql.connect(
        host=DB_HOST,
        user=DB_USER,
        password=DB_PASS,
        database=DB_NAME,
        charset="utf8mb4",
        autocommit=True
    )

def normalize_room_name(room_name: str):
    if room_name is None:
        return None
    room_name = room_name.strip().lower()
    return ROOM_NAME_MAP.get(room_name, room_name)

def normalize_topic(topic: str):
    parts = topic.split("/")
    if len(parts) >= 2 and parts[0] in {"nha", "baochay"}:
        parts[1] = normalize_room_name(parts[1])
    return "/".join(parts)

def parse_topic(topic: str):
    parts = topic.split("/")

    if len(parts) == 3 and parts[0] == "nha":
        return normalize_room_name(parts[1]), parts[2]

    if len(parts) == 3 and parts[0] == "baochay":
        return normalize_room_name(parts[1]), f"{parts[0]}_{parts[2]}"

    return None, None

def normalize_value(metric_name: str, payload_text: str):
    value = payload_text.strip()

    if metric_name in NUMERIC_INT_FIELDS:
        try:
            return int(float(value))
        except ValueError:
            return None

    if metric_name in NUMERIC_FLOAT_FIELDS:
        try:
            return float(value)
        except ValueError:
            return None

    return value

def extract_payload_num(metric_name: str, payload_text: str):
    value = normalize_value(metric_name, payload_text)
    if isinstance(value, (int, float)):
        return float(value)
    return None

def map_system_status(raw_status):
    if raw_status is None:
        return "SAFE"

    value = str(raw_status).strip().lower()

    if value in {"binh_thuong", "an_toan", "safe", "0"}:
        return "SAFE"
    if value in {"canh_bao", "warning", "1"}:
        return "WARNING"
    if value in {"nguy_hiem", "danger", "2"}:
        return "DANGER"

    return "SAFE"

def ensure_device_exists(conn, room_name):
    if room_name is None:
        return

    device_id = f"ROOM_{room_name.upper()}"

    with conn.cursor() as cur:
        cur.execute("""
            INSERT INTO devices (device_id, room_name, last_seen, node_status)
            VALUES (%s, %s, NOW(), 'offline')
            ON DUPLICATE KEY UPDATE
                room_name = VALUES(room_name),
                last_seen = NOW()
        """, (device_id, room_name))

def save_topic_history(conn, topic, room_name, metric_name, payload_text, qos, retained):
    topic = normalize_topic(topic)
    room_name = normalize_room_name(room_name)
    payload_num = extract_payload_num(metric_name, payload_text)

    with conn.cursor() as cur:
        cur.execute("""
            INSERT INTO mqtt_topic_history
            (topic, room_name, metric_name, payload_text, payload_num, qos, retained, received_at)
            VALUES (%s, %s, %s, %s, %s, %s, %s, NOW())
        """, (topic, room_name, metric_name, payload_text, payload_num, qos, int(retained)))

def upsert_topic_latest(conn, topic, room_name, metric_name, payload_text, qos, retained):
    topic = normalize_topic(topic)
    room_name = normalize_room_name(room_name)
    payload_num = extract_payload_num(metric_name, payload_text)

    with conn.cursor() as cur:
        cur.execute("""
            INSERT INTO mqtt_topic_latest
            (topic, room_name, metric_name, payload_text, payload_num, qos, retained, updated_at)
            VALUES (%s, %s, %s, %s, %s, %s, %s, NOW())
            ON DUPLICATE KEY UPDATE
                room_name = VALUES(room_name),
                metric_name = VALUES(metric_name),
                payload_text = VALUES(payload_text),
                payload_num = VALUES(payload_num),
                qos = VALUES(qos),
                retained = VALUES(retained),
                updated_at = NOW()
        """, (topic, room_name, metric_name, payload_text, payload_num, qos, int(retained)))

def upsert_room_state(conn, room_name, metric_name, payload_text):
    room_name = normalize_room_name(room_name)

    if room_name is None or metric_name not in ROOM_COLUMNS:
        return

    column_name = ROOM_COLUMNS[metric_name]
    value = normalize_value(metric_name, payload_text)

    with conn.cursor() as cur:
        cur.execute("""
            INSERT INTO room_state_current (room_name, updated_at)
            VALUES (%s, NOW())
            ON DUPLICATE KEY UPDATE updated_at = NOW()
        """, (room_name,))

        cur.execute(
            f"UPDATE room_state_current SET `{column_name}` = %s, updated_at = NOW() WHERE room_name = %s",
            (value, room_name)
        )

def insert_sensor_reading_snapshot(conn, room_name, metric_name):
    room_name = normalize_room_name(room_name)

    if room_name is None:
        return

    if metric_name not in SENSOR_READING_METRICS:
        return

    ensure_device_exists(conn, room_name)

    with conn.cursor() as cur:
        cur.execute("""
            SELECT nhietdo, gas, khoi, lua, canhbao_trangthai
            FROM room_state_current
            WHERE room_name = %s
            LIMIT 1
        """, (room_name,))
        row = cur.fetchone()

        if not row:
            return

        temperature, gas_value, smoke_value, flame_value, raw_status = row

        if temperature is None and gas_value is None and smoke_value is None and flame_value is None:
            return

        device_id = f"ROOM_{room_name.upper()}"
        system_status = map_system_status(raw_status)

        cur.execute("""
            INSERT INTO sensor_readings
            (device_id, recorded_at, temperature, gas_value, smoke_value, flame_value, system_status)
            VALUES (%s, NOW(), %s, %s, %s, %s, %s)
        """, (device_id, temperature, gas_value, smoke_value, flame_value, system_status))

def upsert_device_status(conn, room_name, metric_name, payload_text):
    room_name = normalize_room_name(room_name)

    if room_name is None or metric_name not in {"trangthai", "status", "node", "device"}:
        return

    state = payload_text.strip().lower()
    node_status = "online" if state == "online" else "offline"
    device_id = f"ROOM_{room_name.upper()}"

    with conn.cursor() as cur:
        cur.execute("""
            INSERT INTO devices (device_id, room_name, last_seen, node_status)
            VALUES (%s, %s, NOW(), %s)
            ON DUPLICATE KEY UPDATE
                room_name = VALUES(room_name),
                last_seen = NOW(),
                node_status = VALUES(node_status)
        """, (device_id, room_name, node_status))

def on_connect(client, userdata, flags, reason_code, properties=None):
    print("MQTT LOCAL connected, rc =", reason_code)
    for topic, qos in MQTT_SUB_TOPICS:
        client.subscribe(topic, qos=qos)
        print("Subscribed to:", topic)

def on_message(client, userdata, msg):
    topic = normalize_topic(msg.topic)
    payload_text = msg.payload.decode("utf-8", errors="replace").strip()
    room_name, metric_name = parse_topic(topic)

    conn = None
    try:
        conn = get_db_connection()

        save_topic_history(conn, topic, room_name, metric_name, payload_text, msg.qos, msg.retain)
        upsert_topic_latest(conn, topic, room_name, metric_name, payload_text, msg.qos, msg.retain)
        upsert_room_state(conn, room_name, metric_name, payload_text)
        upsert_device_status(conn, room_name, metric_name, payload_text)
        insert_sensor_reading_snapshot(conn, room_name, metric_name)

        print(f"Saved topic={topic}, payload={payload_text}")

    except Exception as e:
        print("Error:", e)

    finally:
        if conn:
            conn.close()

client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
client.on_connect = on_connect
client.on_message = on_message

client.connect(MQTT_BROKER, MQTT_PORT, 60)
client.loop_forever()