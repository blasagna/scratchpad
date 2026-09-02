/*
 * Copyright (c) 2026 Bob DiMaiolo
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ble.hpp"

#include "codec.hpp"
#include "rpc.hpp"
#include "streams.hpp"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ble, LOG_LEVEL_INF);

namespace ble
{
namespace
{

/*
 * One custom 128-bit vendor primary service, f5e5xxxx-4a75-4b21-9d3e-6b1c2a7e0000.
 * Four notify characteristics, one per rate class, so a host can subscribe to
 * only what it needs, plus the RPC pair.
 */
#define FEATHER_UUID(id)                                                                           \
	BT_UUID_128_ENCODE(0xf5e50000 + (id), 0x4a75, 0x4b21, 0x9d3e, 0x6b1c2a7e0000)

static const bt_uuid_128 svc_uuid = BT_UUID_INIT_128(FEATHER_UUID(0));
static const bt_uuid_128 imu_uuid = BT_UUID_INIT_128(FEATHER_UUID(1));
static const bt_uuid_128 magn_uuid = BT_UUID_INIT_128(FEATHER_UUID(2));
static const bt_uuid_128 env_uuid = BT_UUID_INIT_128(FEATHER_UUID(3));
static const bt_uuid_128 events_uuid = BT_UUID_INIT_128(FEATHER_UUID(4));
static const bt_uuid_128 rpc_request_uuid = BT_UUID_INIT_128(FEATHER_UUID(5));
static const bt_uuid_128 rpc_response_uuid = BT_UUID_INIT_128(FEATHER_UUID(6));

/* The characteristics, in the order they appear in the service below. */
enum Characteristic {
	kCharImu,
	kCharMagn,
	kCharEnv,
	kCharEvents,
	kCharRpcResponse,
	kCharCount,
};

bool subscribed_char[kCharCount];
bt_conn *current_conn;

Characteristic characteristic_for(uint8_t stream_id)
{
	switch (stream_id) {
	case codec::kStreamImu:
		return kCharImu;
	case codec::kStreamMagn:
		return kCharMagn;
	case codec::kStreamEnv:
		return kCharEnv;
	default:
		/* Battery and button share the events characteristic: both are
		 * rare and neither needs its own subscription.
		 */
		return kCharEvents;
	}
}

/*
 * Recompute how many IMU samples fit in one notification.
 *
 * bt_gatt_get_mtu() reports the ATT MTU; 3 bytes go to the notification's own
 * opcode and handle, and 10 to the batch header. At CONFIG_BT_L2CAP_TX_MTU=247
 * this is 19 samples -- about 11 notifications per second at 208 Hz, and
 * roughly 91 ms of latency, which is the cap the design budgets for.
 */
void recompute_imu_batch(bt_conn *conn)
{
	if (conn == nullptr) {
		return;
	}

	const uint16_t mtu = bt_gatt_get_mtu(conn);
	if (mtu <= 3 + codec::kBatchHeaderBytes + codec::kImuSampleBytes) {
		streams::set_imu_batch_samples(1);
		return;
	}

	const uint16_t usable = mtu - 3 - codec::kBatchHeaderBytes;
	streams::set_imu_batch_samples(static_cast<uint8_t>(usable / codec::kImuSampleBytes));
	LOG_INF("ATT MTU %u -> %u IMU samples per notification", mtu, streams::imu_batch_samples());
}

void ccc_changed(Characteristic which, const bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	subscribed_char[which] = (value == BT_GATT_CCC_NOTIFY);

	if (which == kCharImu && subscribed_char[which]) {
		recompute_imu_batch(current_conn);
	}
}

void imu_ccc_changed(const bt_gatt_attr *attr, uint16_t value)
{
	ccc_changed(kCharImu, attr, value);
}
void magn_ccc_changed(const bt_gatt_attr *attr, uint16_t value)
{
	ccc_changed(kCharMagn, attr, value);
}
void env_ccc_changed(const bt_gatt_attr *attr, uint16_t value)
{
	ccc_changed(kCharEnv, attr, value);
}
void events_ccc_changed(const bt_gatt_attr *attr, uint16_t value)
{
	ccc_changed(kCharEvents, attr, value);
}
void rpc_ccc_changed(const bt_gatt_attr *attr, uint16_t value)
{
	ccc_changed(kCharRpcResponse, attr, value);
}

/*
 * An RPC request arrives as a write. Every opcode answers from cached state --
 * `get battery` reads battery.cpp's last sample rather than the ADC -- so this
 * runs to completion on the Bluetooth RX thread without blocking it.
 */
ssize_t rpc_write(bt_conn *conn, const bt_gatt_attr *attr, const void *buf, uint16_t len,
		  uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	uint8_t response[codec::kRpcMaxFrameBytes];
	const size_t response_len =
		rpc::handle(static_cast<const uint8_t *>(buf), len, response, sizeof(response));

	if (response_len > 0) {
		notify_rpc(response, response_len);
	}

	return len;
}

BT_GATT_SERVICE_DEFINE(feather_svc, BT_GATT_PRIMARY_SERVICE(&svc_uuid),

		       BT_GATT_CHARACTERISTIC(&imu_uuid.uuid, BT_GATT_CHRC_NOTIFY,
					      BT_GATT_PERM_NONE, nullptr, nullptr, nullptr),
		       BT_GATT_CCC(imu_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

		       BT_GATT_CHARACTERISTIC(&magn_uuid.uuid, BT_GATT_CHRC_NOTIFY,
					      BT_GATT_PERM_NONE, nullptr, nullptr, nullptr),
		       BT_GATT_CCC(magn_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

		       BT_GATT_CHARACTERISTIC(&env_uuid.uuid, BT_GATT_CHRC_NOTIFY,
					      BT_GATT_PERM_NONE, nullptr, nullptr, nullptr),
		       BT_GATT_CCC(env_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

		       BT_GATT_CHARACTERISTIC(&events_uuid.uuid, BT_GATT_CHRC_NOTIFY,
					      BT_GATT_PERM_NONE, nullptr, nullptr, nullptr),
		       BT_GATT_CCC(events_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

		       BT_GATT_CHARACTERISTIC(&rpc_request_uuid.uuid,
					      BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
					      BT_GATT_PERM_WRITE, nullptr, rpc_write, nullptr),

		       BT_GATT_CHARACTERISTIC(&rpc_response_uuid.uuid, BT_GATT_CHRC_NOTIFY,
					      BT_GATT_PERM_NONE, nullptr, nullptr, nullptr),
		       BT_GATT_CCC(rpc_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE), );

/*
 * The value attribute of each characteristic, which is what bt_gatt_notify()
 * wants.
 *
 * Resolved by UUID rather than by index into the service above. Counting
 * attributes by hand off the macro expansion works and survives exactly until
 * someone inserts a characteristic; and it cannot be guarded with a BUILD_ASSERT
 * on the array length, because `feather_svc.attrs` is a pointer rather than an
 * array and ARRAY_SIZE quietly answers 0. bt_gatt_find_by_uuid() matches the
 * *value* attribute for a characteristic UUID -- the declaration carries
 * BT_UUID_GATT_CHRC instead -- so this asks the question directly.
 */
const bt_gatt_attr *value_attrs[kCharCount];

int resolve_value_attrs()
{
	const bt_uuid *uuids[kCharCount] = {
		&imu_uuid.uuid,    &magn_uuid.uuid,         &env_uuid.uuid,
		&events_uuid.uuid, &rpc_response_uuid.uuid,
	};

	for (size_t i = 0; i < kCharCount; i++) {
		value_attrs[i] =
			bt_gatt_find_by_uuid(feather_svc.attrs, feather_svc.attr_count, uuids[i]);
		if (value_attrs[i] == nullptr) {
			LOG_ERR("characteristic %zu is not in the service", i);
			return -ENOENT;
		}
	}

	return 0;
}

const bt_gatt_attr *value_attr(Characteristic which)
{
	return value_attrs[which];
}

const bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

const bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, FEATHER_UUID(0)),
};

void connected_cb(bt_conn *conn, uint8_t err)
{
	if (err != 0) {
		LOG_ERR("connection failed (0x%02x)", err);
		return;
	}

	current_conn = bt_conn_ref(conn);
	LOG_INF("connected");
}

void disconnected_cb(bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);

	LOG_INF("disconnected (0x%02x)", reason);

	if (current_conn != nullptr) {
		bt_conn_unref(current_conn);
		current_conn = nullptr;
	}

	for (bool &flag : subscribed_char) {
		flag = false;
	}

	/* Back to the MTU-independent default until the next host negotiates. */
	streams::set_imu_batch_samples(streams::kMaxImuBatchSamples);

	/* Advertising is NOT restarted here. See recycled_cb(). */
}

/*
 * Restart advertising once the connection object has actually been freed.
 *
 * Doing this from disconnected_cb() is the obvious thing, and it does not work:
 * the connection object is still held at that point, so a *connectable*
 * advertiser has no slot to take and bt_le_adv_start() returns -ENOMEM. The
 * board then advertises never again while continuing to stream perfectly over
 * USB, which is what made it look fine. `recycled` is Zephyr's hook for exactly
 * this; its own documentation calls it "the event to listen for to start a new
 * connection or connectable advertiser". Measured: the error was -12, and the
 * second BLE connection to this board was the one that found it.
 */
void recycled_cb()
{
	const int ret =
		bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (ret != 0) {
		LOG_ERR("could not restart advertising (%d)", ret);
		return;
	}

	LOG_INF("advertising again");
}

/*
 * BlueZ raises the MTU shortly after connecting, and often after the host has
 * already subscribed. Recomputing here as well as in the CCC callback is what
 * keeps a subscription that arrived first from being stuck at 23 bytes.
 */
void mtu_updated_cb(bt_conn *conn, uint16_t tx, uint16_t rx)
{
	ARG_UNUSED(tx);
	ARG_UNUSED(rx);

	recompute_imu_batch(conn);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected_cb,
	.disconnected = disconnected_cb,
	.recycled = recycled_cb,
};

bt_gatt_cb gatt_callbacks = {
	.att_mtu_updated = mtu_updated_cb,
};

} /* namespace */

bool connected()
{
	return current_conn != nullptr;
}

bool subscribed(uint8_t stream_id)
{
	return current_conn != nullptr && subscribed_char[characteristic_for(stream_id)];
}

void notify(const uint8_t *batch, size_t len)
{
	codec::BatchHeader header;

	if (current_conn == nullptr || !codec::unpack_batch_header(batch, len, header)) {
		return;
	}

	const Characteristic which = characteristic_for(header.stream_id);
	if (!subscribed_char[which] || value_attr(which) == nullptr) {
		return;
	}

	const int ret = bt_gatt_notify(current_conn, value_attr(which), batch, len);
	if (ret != 0) {
		LOG_DBG("notify on stream %u failed (%d)", header.stream_id, ret);
	}
}

void notify_rpc(const uint8_t *frame, size_t len)
{
	if (current_conn == nullptr || !subscribed_char[kCharRpcResponse] ||
	    value_attr(kCharRpcResponse) == nullptr) {
		return;
	}

	const int ret = bt_gatt_notify(current_conn, value_attr(kCharRpcResponse), frame, len);
	if (ret != 0) {
		LOG_WRN("rpc response notify failed (%d)", ret);
	}
}

int start()
{
	int ret = bt_enable(nullptr);
	if (ret != 0) {
		LOG_ERR("bt_enable failed (%d)", ret);
		return ret;
	}

	ret = resolve_value_attrs();
	if (ret != 0) {
		return ret;
	}

	bt_gatt_cb_register(&gatt_callbacks);

	ret = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (ret != 0) {
		LOG_ERR("advertising failed to start (%d)", ret);
		return ret;
	}

	LOG_INF("advertising as \"%s\"", CONFIG_BT_DEVICE_NAME);

	return 0;
}

} /* namespace ble */
