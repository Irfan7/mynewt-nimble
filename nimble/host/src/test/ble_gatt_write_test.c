/**
 * Copyright (c) 2015 Runtime Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string.h>
#include <errno.h>
#include "testutil/testutil.h"
#include "nimble/ble.h"
#include "host/ble_hs_test.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "ble_hs_priv.h"
#include "ble_att_cmd.h"
#include "ble_gatt_priv.h"
#include "ble_hs_conn.h"
#include "ble_hs_test_util.h"

static int ble_gatt_write_test_cb_called;

static uint8_t ble_gatt_write_test_attr_value[BLE_ATT_ATTR_MAX_LEN];
static struct ble_gatt_error ble_gatt_write_test_error;

static void
ble_gatt_write_test_init(void)
{
    int i;

    ble_hs_test_util_init();
    ble_gatt_write_test_cb_called = 0;

    for (i = 0; i < sizeof ble_gatt_write_test_attr_value; i++) {
        ble_gatt_write_test_attr_value[i] = i;
    }
}

static int
ble_gatt_write_test_cb_good(uint16_t conn_handle, struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg)
{
    int *attr_len;

    attr_len = arg;

    TEST_ASSERT(conn_handle == 2);
    if (attr_len != NULL) {
        TEST_ASSERT(error == NULL);
        TEST_ASSERT(attr->handle == 100);
        TEST_ASSERT(attr->value_len == *attr_len);
        TEST_ASSERT(attr->value == ble_gatt_write_test_attr_value);
    } else {
        TEST_ASSERT(error != NULL);
        ble_gatt_write_test_error = *error;
    }

    ble_gatt_write_test_cb_called = 1;

    return 0;
}

static void
ble_gatt_write_test_rx_rsp(struct ble_hs_conn *conn)
{
    struct ble_l2cap_chan *chan;
    uint8_t op;
    int rc;

    chan = ble_hs_conn_chan_find(conn, BLE_L2CAP_CID_ATT);
    TEST_ASSERT_FATAL(chan != NULL);

    op = BLE_ATT_OP_WRITE_RSP;
    rc = ble_hs_test_util_l2cap_rx_payload_flat(conn, chan, &op, 1);
    TEST_ASSERT(rc == 0);
}

static void
ble_gatt_write_test_rx_prep_rsp(struct ble_hs_conn *conn, uint16_t attr_handle,
                                uint16_t offset,
                                void *attr_data, uint16_t attr_data_len)
{
    struct ble_att_prep_write_cmd rsp;
    struct ble_l2cap_chan *chan;
    uint8_t buf[512];
    int rc;

    chan = ble_hs_conn_chan_find(conn, BLE_L2CAP_CID_ATT);
    TEST_ASSERT_FATAL(chan != NULL);

    rsp.bapc_handle = attr_handle;
    rsp.bapc_offset = offset;
    rc = ble_att_prep_write_rsp_write(buf, sizeof buf, &rsp);
    TEST_ASSERT_FATAL(rc == 0);

    memcpy(buf + BLE_ATT_PREP_WRITE_CMD_BASE_SZ, attr_data, attr_data_len);

    rc = ble_hs_test_util_l2cap_rx_payload_flat(
        conn, chan, buf, BLE_ATT_PREP_WRITE_CMD_BASE_SZ + attr_data_len);
    TEST_ASSERT(rc == 0);
}

static void
ble_gatt_write_test_rx_exec_rsp(struct ble_hs_conn *conn)
{
    struct ble_l2cap_chan *chan;
    uint8_t op;
    int rc;

    chan = ble_hs_conn_chan_find(conn, BLE_L2CAP_CID_ATT);
    TEST_ASSERT_FATAL(chan != NULL);

    op = BLE_ATT_OP_EXEC_WRITE_RSP;
    rc = ble_hs_test_util_l2cap_rx_payload_flat(conn, chan, &op, 1);
    TEST_ASSERT(rc == 0);
}

static void
ble_gatt_write_test_misc_long_good(int attr_len)
{
    struct ble_hs_conn *conn;
    int off;
    int len;
    int rc;

    ble_gatt_write_test_init();

    conn = ble_hs_test_util_create_conn(2, ((uint8_t[]){2,3,4,5,6,7,8,9}),
                                        NULL, NULL);

    rc = ble_gattc_write_long(conn->bhc_handle, 100,
                              ble_gatt_write_test_attr_value,
                              attr_len, ble_gatt_write_test_cb_good,
                              &attr_len);
    TEST_ASSERT(rc == 0);

    off = 0;
    while (off < attr_len) {
        /* Send the pending ATT Prep Write Command. */
        ble_hs_test_util_tx_all();

        /* Receive Prep Write response. */
        len = BLE_ATT_MTU_DFLT - BLE_ATT_PREP_WRITE_CMD_BASE_SZ;
        if (off + len > attr_len) {
            len = attr_len - off;
        }
        ble_gatt_write_test_rx_prep_rsp(conn, 100, off,
                                        ble_gatt_write_test_attr_value + off,
                                        len);

        /* Verify callback hasn't gotten called. */
        TEST_ASSERT(!ble_gatt_write_test_cb_called);

        off += len;
    }

    /* Receive Exec Write response. */
    ble_hs_test_util_tx_all();
    ble_gatt_write_test_rx_exec_rsp(conn);

    /* Verify callback got called. */
    TEST_ASSERT(ble_gatt_write_test_cb_called);
}

typedef void ble_gatt_write_test_long_fail_fn(struct ble_hs_conn *conn,
                                              int off, int len);

static void
ble_gatt_write_test_misc_long_bad(int attr_len,
                                  ble_gatt_write_test_long_fail_fn *cb)
{
    struct ble_hs_conn *conn;
    int fail_now;
    int off;
    int len;
    int rc;

    ble_gatt_write_test_init();

    conn = ble_hs_test_util_create_conn(2, ((uint8_t[]){2,3,4,5,6,7,8,9}),
                                        NULL, NULL);

    rc = ble_gattc_write_long(conn->bhc_handle, 100,
                              ble_gatt_write_test_attr_value,
                              attr_len, ble_gatt_write_test_cb_good, NULL);
    TEST_ASSERT(rc == 0);

    fail_now = 0;
    off = 0;
    while (off < attr_len) {
        /* Send the pending ATT Prep Write Command. */
        ble_hs_test_util_tx_all();

        /* Receive Prep Write response. */
        len = BLE_ATT_MTU_DFLT - BLE_ATT_PREP_WRITE_CMD_BASE_SZ;
        if (off + len >= attr_len) {
            len = attr_len - off;
            fail_now = 1;
        }
        if (!fail_now) {
            ble_gatt_write_test_rx_prep_rsp(
                conn, 100, off, ble_gatt_write_test_attr_value + off,
                len);
        } else {
            cb(conn, off, len);
            break;
        }

        /* Verify callback hasn't gotten called. */
        TEST_ASSERT(!ble_gatt_write_test_cb_called);

        off += len;
    }

    /* Verify callback was called. */
    TEST_ASSERT(ble_gatt_write_test_cb_called);
    TEST_ASSERT(ble_gatt_write_test_error.status == BLE_HS_EBADDATA);
    TEST_ASSERT(ble_gatt_write_test_error.att_handle == 0);
}

static void
ble_gatt_write_test_misc_long_fail_handle(struct ble_hs_conn *conn,
                                          int off, int len)
{
    ble_gatt_write_test_rx_prep_rsp(
        conn, 99, off, ble_gatt_write_test_attr_value + off,
        len);
}

static void
ble_gatt_write_test_misc_long_fail_offset(struct ble_hs_conn *conn,
                                          int off, int len)
{
    ble_gatt_write_test_rx_prep_rsp(
        conn, 100, off + 1, ble_gatt_write_test_attr_value + off,
        len);
}

static void
ble_gatt_write_test_misc_long_fail_value(struct ble_hs_conn *conn,
                                         int off, int len)
{
    ble_gatt_write_test_rx_prep_rsp(
        conn, 100, off, ble_gatt_write_test_attr_value + off + 1,
        len);
}

static void
ble_gatt_write_test_misc_long_fail_length(struct ble_hs_conn *conn,
                                          int off, int len)
{
    ble_gatt_write_test_rx_prep_rsp(
        conn, 100, off, ble_gatt_write_test_attr_value + off,
        len - 1);
}

TEST_CASE(ble_gatt_write_test_no_rsp)
{
    int attr_len;
    int rc;

    ble_gatt_write_test_init();

    ble_hs_test_util_create_conn(2, ((uint8_t[]){2,3,4,5,6,7,8,9}),
                                        NULL, NULL);

    attr_len = 4;
    rc = ble_gattc_write_no_rsp(2, 100, ble_gatt_write_test_attr_value,
                                attr_len, ble_gatt_write_test_cb_good,
                                &attr_len);
    TEST_ASSERT(rc == 0);

    /* Send the pending ATT Write Command. */
    ble_hs_test_util_tx_all();

    /* No response expected; verify callback got called. */
    TEST_ASSERT(ble_gatt_write_test_cb_called);
}

TEST_CASE(ble_gatt_write_test_rsp)
{
    struct ble_hs_conn *conn;
    int attr_len;
    int rc;

    ble_gatt_write_test_init();

    conn = ble_hs_test_util_create_conn(2, ((uint8_t[]){2,3,4,5,6,7,8,9}),
                                        NULL, NULL);

    attr_len = 4;
    rc = ble_gattc_write(2, 100, ble_gatt_write_test_attr_value,
                         attr_len, ble_gatt_write_test_cb_good, &attr_len);
    TEST_ASSERT(rc == 0);

    /* Send the pending ATT Write Command. */
    ble_hs_test_util_tx_all();

    /* Response not received yet; verify callback not called. */
    TEST_ASSERT(!ble_gatt_write_test_cb_called);

    /* Receive write response. */
    ble_gatt_write_test_rx_rsp(conn);

    /* Verify callback got called. */
    TEST_ASSERT(ble_gatt_write_test_cb_called);
}

TEST_CASE(ble_gatt_write_test_long_good)
{
    /*** 1 prep write req/rsp. */
    ble_gatt_write_test_misc_long_good(
        BLE_ATT_MTU_DFLT - BLE_ATT_PREP_WRITE_CMD_BASE_SZ);

    /*** 2 prep write reqs/rsps. */
    ble_gatt_write_test_misc_long_good(
        BLE_ATT_MTU_DFLT - BLE_ATT_PREP_WRITE_CMD_BASE_SZ + 1);

    /*** Maximum reqs/rsps. */
    ble_gatt_write_test_misc_long_good(BLE_ATT_ATTR_MAX_LEN);
}

TEST_CASE(ble_gatt_write_test_long_bad_handle)
{
    /*** 1 prep write req/rsp. */
    ble_gatt_write_test_misc_long_bad(
        BLE_ATT_MTU_DFLT - BLE_ATT_PREP_WRITE_CMD_BASE_SZ,
        ble_gatt_write_test_misc_long_fail_handle);

    /*** 2 prep write reqs/rsps. */
    ble_gatt_write_test_misc_long_bad(
        BLE_ATT_MTU_DFLT - BLE_ATT_PREP_WRITE_CMD_BASE_SZ + 1,
        ble_gatt_write_test_misc_long_fail_handle);

    /*** Maximum reqs/rsps. */
    ble_gatt_write_test_misc_long_bad(
        BLE_ATT_ATTR_MAX_LEN,
        ble_gatt_write_test_misc_long_fail_handle);
}

TEST_CASE(ble_gatt_write_test_long_bad_offset)
{
    /*** 1 prep write req/rsp. */
    ble_gatt_write_test_misc_long_bad(
        BLE_ATT_MTU_DFLT - BLE_ATT_PREP_WRITE_CMD_BASE_SZ,
        ble_gatt_write_test_misc_long_fail_offset);

    /*** 2 prep write reqs/rsps. */
    ble_gatt_write_test_misc_long_bad(
        BLE_ATT_MTU_DFLT - BLE_ATT_PREP_WRITE_CMD_BASE_SZ + 1,
        ble_gatt_write_test_misc_long_fail_offset);

    /*** Maximum reqs/rsps. */
    ble_gatt_write_test_misc_long_bad(
        BLE_ATT_ATTR_MAX_LEN,
        ble_gatt_write_test_misc_long_fail_offset);
}

TEST_CASE(ble_gatt_write_test_long_bad_value)
{
    /*** 1 prep write req/rsp. */
    ble_gatt_write_test_misc_long_bad(
        BLE_ATT_MTU_DFLT - BLE_ATT_PREP_WRITE_CMD_BASE_SZ,
        ble_gatt_write_test_misc_long_fail_value);

    /*** 2 prep write reqs/rsps. */
    ble_gatt_write_test_misc_long_bad(
        BLE_ATT_MTU_DFLT - BLE_ATT_PREP_WRITE_CMD_BASE_SZ + 1,
        ble_gatt_write_test_misc_long_fail_value);

    /*** Maximum reqs/rsps. */
    ble_gatt_write_test_misc_long_bad(
        BLE_ATT_ATTR_MAX_LEN,
        ble_gatt_write_test_misc_long_fail_value);
}

TEST_CASE(ble_gatt_write_test_long_bad_length)
{
    /*** 1 prep write req/rsp. */
    ble_gatt_write_test_misc_long_bad(
        BLE_ATT_MTU_DFLT - BLE_ATT_PREP_WRITE_CMD_BASE_SZ,
        ble_gatt_write_test_misc_long_fail_length);

    /*** 2 prep write reqs/rsps. */
    ble_gatt_write_test_misc_long_bad(
        BLE_ATT_MTU_DFLT - BLE_ATT_PREP_WRITE_CMD_BASE_SZ + 1,
        ble_gatt_write_test_misc_long_fail_length);

    /*** Maximum reqs/rsps. */
    ble_gatt_write_test_misc_long_bad(
        BLE_ATT_ATTR_MAX_LEN,
        ble_gatt_write_test_misc_long_fail_length);
}

TEST_SUITE(ble_gatt_write_test_suite)
{
    ble_gatt_write_test_no_rsp();
    ble_gatt_write_test_rsp();
    ble_gatt_write_test_long_good();
    ble_gatt_write_test_long_bad_handle();
    ble_gatt_write_test_long_bad_offset();
    ble_gatt_write_test_long_bad_value();
    ble_gatt_write_test_long_bad_length();
}

int
ble_gatt_write_test_all(void)
{
    ble_gatt_write_test_suite();

    return tu_any_failed;
}
