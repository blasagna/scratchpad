/*
 * One custom service with three notify characteristics. The accelerometer
 * stream is batched: the batch size is derived from the negotiated ATT MTU, so
 * each notification fills as much of a connection interval as the link allows.
 */

#include "ble.h"
#include "accel.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(ble, LOG_LEVEL_DBG);

/* f1b7 0001..0004 -9c4e-4a1d-9a6b-2f0c1d4e7a30 */
#define UUID_SERVICE_VAL BT_UUID_128_ENCODE(0xf1b70001, 0x9c4e, 0x4a1d, 0x9a6b, 0x2f0c1d4e7a30)
#define UUID_ACCEL_VAL   BT_UUID_128_ENCODE(0xf1b70002, 0x9c4e, 0x4a1d, 0x9a6b, 0x2f0c1d4e7a30)
#define UUID_TEMP_VAL    BT_UUID_128_ENCODE(0xf1b70003, 0x9c4e, 0x4a1d, 0x9a6b, 0x2f0c1d4e7a30)
#define UUID_BUTTON_VAL  BT_UUID_128_ENCODE(0xf1b70004, 0x9c4e, 0x4a1d, 0x9a6b, 0x2f0c1d4e7a30)

static const struct bt_uuid_128 uuid_service = BT_UUID_INIT_128(UUID_SERVICE_VAL);
static const struct bt_uuid_128 uuid_accel = BT_UUID_INIT_128(UUID_ACCEL_VAL);
static const struct bt_uuid_128 uuid_temp = BT_UUID_INIT_128(UUID_TEMP_VAL);
static const struct bt_uuid_128 uuid_button = BT_UUID_INIT_128(UUID_BUTTON_VAL);

/* Accelerometer payload: uint32 t_ms, uint8 count, then count * {int16 x,y,z}. */
#define ACCEL_HDR_LEN    5
#define ACCEL_SAMPLE_LEN 6

/* Cap the batch so latency stays bounded at 100 ms even when the MTU would
 * allow far more. At 247 the MTU alone would permit 39 samples -- 390 ms.
 */
#define ACCEL_MAX_BATCH 10

/* How long to keep waiting for the next sample while filling a batch. Samples
 * arrive every 10 ms, so this only expires when the stream itself has stalled.
 */
#define ACCEL_FILL_TIMEOUT_MS 15

#define BLE_TX_STACK_SIZE 1024
#define BLE_TX_PRIORITY   7

static struct bt_conn *default_conn;
static atomic_t accel_notify_on;
static atomic_t temp_notify_on;
static atomic_t button_notify_on;

static void accel_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	atomic_set(&accel_notify_on, value == BT_GATT_CCC_NOTIFY);

	/* Report the MTU here rather than on connect: the central negotiates it
	 * moments after connecting, so by subscribe time it has settled, and it is
	 * what decides how many samples fit in each notification.
	 */
	if (value == BT_GATT_CCC_NOTIFY && default_conn != NULL) {
		LOG_INF("accel notifications on, ATT MTU %u", bt_gatt_get_mtu(default_conn));
	} else {
		LOG_INF("accel notifications off");
	}
}

static void temp_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	atomic_set(&temp_notify_on, value == BT_GATT_CCC_NOTIFY);
	LOG_INF("temperature notifications %s", value == BT_GATT_CCC_NOTIFY ? "on" : "off");
}

static void button_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	atomic_set(&button_notify_on, value == BT_GATT_CCC_NOTIFY);
	LOG_INF("button notifications %s", value == BT_GATT_CCC_NOTIFY ? "on" : "off");
}

BT_GATT_SERVICE_DEFINE(mb_svc, BT_GATT_PRIMARY_SERVICE(&uuid_service),

		       BT_GATT_CHARACTERISTIC(&uuid_accel.uuid, BT_GATT_CHRC_NOTIFY,
					      BT_GATT_PERM_NONE, NULL, NULL, NULL),
		       BT_GATT_CCC(accel_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

		       BT_GATT_CHARACTERISTIC(&uuid_temp.uuid, BT_GATT_CHRC_NOTIFY,
					      BT_GATT_PERM_NONE, NULL, NULL, NULL),
		       BT_GATT_CCC(temp_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

		       BT_GATT_CHARACTERISTIC(&uuid_button.uuid, BT_GATT_CHRC_NOTIFY,
					      BT_GATT_PERM_NONE, NULL, NULL, NULL),
		       BT_GATT_CCC(button_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE), );

/* Value attributes sit one past their characteristic declaration. */
#define ATTR_ACCEL  (&mb_svc.attrs[2])
#define ATTR_TEMP   (&mb_svc.attrs[5])
#define ATTR_BUTTON (&mb_svc.attrs[8])

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, UUID_SERVICE_VAL),
};

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_ERR("connection failed (0x%02x)", err);
		return;
	}

	default_conn = bt_conn_ref(conn);
	LOG_INF("connected");

	/* Deliberately no bt_gatt_exchange_mtu() here. Initiating the exchange from
	 * the peripheral looks like a way to avoid depending on the central asking,
	 * but it kills the link against BlueZ: the request draws no response, and
	 * 30 s later the ATT transaction times out and takes the connection with it
	 * (observed as "MTU exchange failed (0x0e)" then disconnect reason 0x16).
	 * Centrals negotiate the MTU themselves on connect, so just use what they
	 * agree to -- CONFIG_BT_L2CAP_TX_MTU sets our side of that ceiling.
	 */
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	LOG_INF("disconnected (0x%02x)", reason);

	atomic_set(&accel_notify_on, 0);
	atomic_set(&temp_notify_on, 0);
	atomic_set(&button_notify_on, 0);

	if (default_conn) {
		bt_conn_unref(default_conn);
		default_conn = NULL;
	}
}

static int advertise(void)
{
	return bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
}

/* Restart from `recycled` rather than `disconnected`: by the time this runs the
 * connection object has been freed, so advertising cannot fail with -ENOMEM.
 */
static void recycled(void)
{
	int err = advertise();

	if (err) {
		LOG_ERR("could not resume advertising (%d)", err);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled,
};

/* Samples that fit in one notification on the given link. */
static uint8_t accel_batch_size(struct bt_conn *conn)
{
	uint16_t mtu = bt_gatt_get_mtu(conn);
	uint16_t payload;

	if (mtu <= 3 + ACCEL_HDR_LEN + ACCEL_SAMPLE_LEN) {
		return 1;
	}

	payload = mtu - 3;
	return (uint8_t)MIN((payload - ACCEL_HDR_LEN) / ACCEL_SAMPLE_LEN, ACCEL_MAX_BATCH);
}

static void ble_tx_thread(void *p1, void *p2, void *p3)
{
	uint8_t buf[ACCEL_HDR_LEN + ACCEL_MAX_BATCH * ACCEL_SAMPLE_LEN];

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		struct accel_sample sample;
		struct bt_conn *conn;
		uint8_t want;
		uint8_t count = 0;
		size_t len = ACCEL_HDR_LEN;
		int err;

		if (default_conn == NULL || !atomic_get(&accel_notify_on)) {
			k_sleep(K_MSEC(100));
			continue;
		}

		/* Block for the first sample of the batch, then keep taking until
		 * the batch is full or the stream goes quiet.
		 */
		if (k_msgq_get(&accel_msgq, &sample, K_MSEC(200)) != 0) {
			continue;
		}

		/* Re-read after the wait: the peer may have gone away while we were
		 * blocked, and both calls below would dereference the pointer.
		 */
		conn = default_conn;
		if (conn == NULL) {
			continue;
		}

		want = accel_batch_size(conn);
		sys_put_le32(sample.t_ms, &buf[0]);

		for (;;) {
			sys_put_le16((uint16_t)sample.x, &buf[len]);
			sys_put_le16((uint16_t)sample.y, &buf[len + 2]);
			sys_put_le16((uint16_t)sample.z, &buf[len + 4]);
			len += ACCEL_SAMPLE_LEN;
			count++;

			if (count >= want) {
				break;
			}
			if (k_msgq_get(&accel_msgq, &sample, K_MSEC(ACCEL_FILL_TIMEOUT_MS)) != 0) {
				break;
			}
		}

		buf[4] = count;

		err = bt_gatt_notify(conn, ATTR_ACCEL, buf, len);
		if (err == -ENOMEM) {
			/* Controller buffers are full; let them drain. */
			k_sleep(K_MSEC(5));
		} else if (err) {
			LOG_WRN("accel notify failed (%d)", err);
			k_sleep(K_MSEC(50));
		}
	}
}

K_THREAD_DEFINE(ble_tx_tid, BLE_TX_STACK_SIZE, ble_tx_thread, NULL, NULL, NULL, BLE_TX_PRIORITY, 0,
		0);

void ble_notify_temp(int16_t centi_c)
{
	struct bt_conn *conn = default_conn;
	uint8_t buf[2];
	int err;

	if (conn == NULL || !atomic_get(&temp_notify_on)) {
		return;
	}

	sys_put_le16((uint16_t)centi_c, buf);

	err = bt_gatt_notify(conn, ATTR_TEMP, buf, sizeof(buf));
	if (err) {
		LOG_WRN("temperature notify failed (%d)", err);
	}
}

void ble_notify_button(uint8_t button, uint8_t pressed, uint32_t t_ms)
{
	struct bt_conn *conn = default_conn;
	uint8_t buf[6];
	int err;

	if (conn == NULL || !atomic_get(&button_notify_on)) {
		return;
	}

	buf[0] = button;
	buf[1] = pressed;
	sys_put_le32(t_ms, &buf[2]);

	err = bt_gatt_notify(conn, ATTR_BUTTON, buf, sizeof(buf));
	if (err) {
		LOG_WRN("button notify failed (%d)", err);
	}
}

int ble_start(void)
{
	int err;

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed (%d)", err);
		return err;
	}

	err = advertise();
	if (err) {
		LOG_ERR("advertising failed to start (%d)", err);
		return err;
	}

	LOG_INF("advertising as \"%s\"", CONFIG_BT_DEVICE_NAME);
	return 0;
}
