/******************************************************************************
 *
 *  Copyright (C) 2012 Marvell International Ltd.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

#define LOG_TAG "bt_mrvl"

#include <utils/Log.h>
#include <assert.h>
#include <time.h>
#include <sched.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/mman.h>

#include "bt_vendor_lib.h"
#include "bt_hci_bdroid.h"

#include "marvell_wireless.h"

#define HCI_CMD_MARVELL_WRITE_PCM_SETTINGS      0xFC07
#define HCI_CMD_MARVELL_WRITE_PCM_SYNC_SETTINGS 0xFC28
#define HCI_CMD_MARVELL_WRITE_PCM_LINK_SETTINGS 0xFC29
#define HCI_CMD_MARVELL_SET_SCO_DATA_PATH       0xFC1D
#define HCI_CMD_MARVELL_WRITE_BD_ADDRESS        0xFC22

#define WRITE_PCM_SETTINGS_SIZE            1
#define WRITE_PCM_SYNC_SETTINGS_SIZE       3
#define WRITE_PCM_LINK_SETTINGS_SIZE       2
#define SET_SCO_DATA_PATH_SIZE             1
#define WRITE_BD_ADDRESS_SIZE              8


#define HCI_CMD_PREAMBLE_SIZE 3

#define HCI_EVT_CMD_CMPL_OPCODE 3

#define STREAM_TO_UINT16(u16, p) \
do { \
	u16 = ((uint16_t)(*(p)) + (((uint16_t)(*((p) + 1))) << 8)); \
	(p) += 2; \
} while (0)

#define UINT16_TO_STREAM(p, u16) \
do { \
	*(p)++ = (uint8_t)(u16); \
	*(p)++ = (uint8_t)((u16) >> 8); \
} while (0)

struct bt_evt_param_t {
	uint16_t cmd;
	uint8_t cmd_ret_param;
};

/* ioctl command to release the read thread before driver close */
#define MBTCHAR_IOCTL_RELEASE _IO('M', 1)

#define VERSION "M002"

static const char mchar_port[] = "/dev/mbtchar0";
static int mchar_fd = -1;

/***********************************************************
 *  Externs
 ***********************************************************
 */
extern unsigned char vnd_local_bd_addr[6];
extern bt_vendor_callbacks_t *bt_vendor_cbacks;

/***********************************************************
 *  Local variables
 ***********************************************************
 */
static uint8_t write_pcm_settings[WRITE_PCM_SETTINGS_SIZE] = {
	0x02
};

static uint8_t write_pcm_sync_settings[WRITE_PCM_SYNC_SETTINGS_SIZE] = {
	0x03,
	0x00,
	0x03
};

static uint8_t write_pcm_link_settings[WRITE_PCM_LINK_SETTINGS_SIZE] = {
	0x03,
	0x00
};

static uint8_t set_sco_data_path[SET_SCO_DATA_PATH_SIZE] = {
	0x01
};

static uint8_t write_bd_address[WRITE_BD_ADDRESS_SIZE] = {
	0xFE, /* Parameter ID */
	0x06, /* bd_addr length */
	0x00, /* 6th byte of bd_addr */
	0x00, /* 5th */
	0x00, /* 4th */
	0x00, /* 3rd */
	0x00, /* 2nd */
	0x00  /* 1st */
};

/***********************************************************
 *  Local functions
 ***********************************************************
 */

static char *cmd_to_str(uint16_t cmd)
{
	switch (cmd) {
	case HCI_CMD_MARVELL_WRITE_PCM_SETTINGS:
		return "write_pcm_settings";
	case HCI_CMD_MARVELL_WRITE_PCM_SYNC_SETTINGS:
		return "write_pcm_sync_settings";
	case HCI_CMD_MARVELL_WRITE_PCM_LINK_SETTINGS:
		return "write_pcm_link_settings";
	case HCI_CMD_MARVELL_SET_SCO_DATA_PATH:
		return "set_sco_data_path";
	case HCI_CMD_MARVELL_WRITE_BD_ADDRESS:
		return "write_bd_address";
	default:
		break;
	}

	return "unknown command";
}

static void populate_bd_addr_params(uint8_t *params, uint8_t *addr)
{
	assert(params && addr);

	*params++ = addr[5];
	*params++ = addr[4];
	*params++ = addr[3];
	*params++ = addr[2];
	*params++ = addr[1];
	*params   = addr[0];
}

static HC_BT_HDR *build_cmd_buf(uint16_t cmd, uint8_t pl_len, uint8_t *payload)
{
	HC_BT_HDR *p_buf = NULL;
	uint16_t cmd_len = HCI_CMD_PREAMBLE_SIZE + pl_len;
	uint8_t *p = NULL;

	assert(payload);

	if (bt_vendor_cbacks)
		p_buf = (HC_BT_HDR *) bt_vendor_cbacks->alloc(BT_HC_HDR_SIZE + cmd_len);

	if (!p_buf)
		return NULL;

	p_buf->event = MSG_STACK_TO_HC_HCI_CMD;
	p_buf->offset = 0;
	p_buf->layer_specific = 0;
	p_buf->len = cmd_len;

	p = (uint8_t *) (p_buf + 1);

	/* opcode */
	UINT16_TO_STREAM(p, cmd);

	/* length of payload */
	*p = pl_len;
	++p;

	/* payload */
	memcpy(p, payload, pl_len);

	return p_buf;
}

static void parse_evt_buf(HC_BT_HDR *p_evt_buf,
		struct bt_evt_param_t *evt_params)
{
	uint8_t *p = (uint8_t *) (p_evt_buf + 1) + HCI_EVT_CMD_CMPL_OPCODE;

	assert(p_evt_buf && evt_params);

	/* opcode */
	STREAM_TO_UINT16(evt_params->cmd, p);

	/* command return parameter */
	evt_params->cmd_ret_param = *p;
}

static void hw_mrvl_config_start_cb(void *p_mem)
{
	HC_BT_HDR *p_evt_buf = (HC_BT_HDR *) p_mem;
	struct bt_evt_param_t evt_params;

	assert(p_mem);

	memset(&evt_params, 0, sizeof(evt_params));

	parse_evt_buf(p_evt_buf, &evt_params);

	/* free the buffer */
	if (bt_vendor_cbacks)
		bt_vendor_cbacks->dealloc(p_evt_buf);

	switch (evt_params.cmd) {
	case HCI_CMD_MARVELL_WRITE_BD_ADDRESS:
		/* fw config succeeds */
		ALOGI("FW config succeeds!");
		if (bt_vendor_cbacks)
			bt_vendor_cbacks->fwcfg_cb(BT_VND_OP_RESULT_SUCCESS);
		return;

	default:
		ALOGE("Received event for unexpected cmd (0x%04hX). Fail.",
			evt_params.cmd);
		break;
	} /* end of switch (evt_params.cmd) */

	if (bt_vendor_cbacks) {
		ALOGE("Vendor lib fwcfg aborted");
		bt_vendor_cbacks->fwcfg_cb(BT_VND_OP_RESULT_FAIL);
	}
}

static void hw_mrvl_sco_config_cb(void *p_mem)
{
	HC_BT_HDR *p_evt_buf = (HC_BT_HDR *) p_mem;
	struct bt_evt_param_t evt_params;
	uint16_t cmd = 0;
	HC_BT_HDR *p_buf = NULL;

	assert(bt_vendor_cbacks && p_mem);

	memset(&evt_params, 0, sizeof(evt_params));

	parse_evt_buf(p_evt_buf, &evt_params);

	/* free the buffer */
	bt_vendor_cbacks->dealloc(p_evt_buf);

	switch (evt_params.cmd) {
	case HCI_CMD_MARVELL_WRITE_PCM_SETTINGS:
		/* Send HCI_CMD_MARVELL_WRITE_PCM_SYNC_SETTINGS */
		cmd = HCI_CMD_MARVELL_WRITE_PCM_SYNC_SETTINGS;
		p_buf = build_cmd_buf(cmd,
				WRITE_PCM_SYNC_SETTINGS_SIZE,
				write_pcm_sync_settings);
		break;

	case HCI_CMD_MARVELL_WRITE_PCM_SYNC_SETTINGS:
		/* Send HCI_CMD_MARVELL_WRITE_PCM_LINK_SETTINGS */
		cmd = HCI_CMD_MARVELL_WRITE_PCM_LINK_SETTINGS;
		p_buf = build_cmd_buf(cmd,
				WRITE_PCM_LINK_SETTINGS_SIZE,
				write_pcm_link_settings);
		break;

	case HCI_CMD_MARVELL_WRITE_PCM_LINK_SETTINGS:
		/* Send HCI_CMD_MARVELL_SET_SCO_DATA_PATH */
		cmd = HCI_CMD_MARVELL_SET_SCO_DATA_PATH;
		p_buf = build_cmd_buf(cmd,
				SET_SCO_DATA_PATH_SIZE,
				set_sco_data_path);
		break;

	case HCI_CMD_MARVELL_SET_SCO_DATA_PATH:
		/* sco config succeeds */
		ALOGI("SCO PCM config succeeds!");
		if (bt_vendor_cbacks)
			bt_vendor_cbacks->scocfg_cb(BT_VND_OP_RESULT_SUCCESS);
		return;

	default:
		ALOGE("Received event for unexpected cmd (0x%04hX). Fail.",
			evt_params.cmd);
		break;
	} /* switch (evt_params.cmd) */

	if (p_buf) {
		ALOGI("Sending hci command 0x%04hX (%s)", cmd, cmd_to_str(cmd));
		if (bt_vendor_cbacks->xmit_cb(cmd, p_buf, hw_mrvl_sco_config_cb))
			return;
		else
			bt_vendor_cbacks->dealloc(p_buf);
	}

	ALOGE("Vendor lib scocfg aborted");
	bt_vendor_cbacks->scocfg_cb(BT_VND_OP_RESULT_FAIL);
}

/***********************************************************
 *  Global functions
 ***********************************************************
 */
void hw_mrvl_config_start(void)
{
	HC_BT_HDR *p_buf = NULL;
	uint16_t cmd = 0;

	assert(bt_vendor_cbacks);

	ALOGI("Start HW config ...");
	/* Start with HCI_CMD_MARVELL_WRITE_BD_ADDRESS */
	ALOGI("Setting bd addr to %02hhX:%02hhX:%02hhX:%02hhX:%02hhX:%02hhX",
		vnd_local_bd_addr[0], vnd_local_bd_addr[1], vnd_local_bd_addr[2],
		vnd_local_bd_addr[3], vnd_local_bd_addr[4], vnd_local_bd_addr[5]);
	populate_bd_addr_params(write_bd_address + 2, vnd_local_bd_addr);

	cmd   = HCI_CMD_MARVELL_WRITE_BD_ADDRESS;
	p_buf = build_cmd_buf(cmd,
			WRITE_BD_ADDRESS_SIZE,
			write_bd_address);

	if (p_buf) {
		ALOGI("Sending hci command 0x%04hX (%s)", cmd, cmd_to_str(cmd));
		if (bt_vendor_cbacks->xmit_cb(cmd, p_buf, hw_mrvl_config_start_cb))
			return;
		else
			bt_vendor_cbacks->dealloc(p_buf);
	}

	ALOGE("Vendor lib fwcfg aborted");
	bt_vendor_cbacks->fwcfg_cb(BT_VND_OP_RESULT_FAIL);
}


void hw_mrvl_sco_config(void)
{
	HC_BT_HDR *p_buf = NULL;
	uint16_t cmd = 0;

	assert(bt_vendor_cbacks);

	ALOGI("Start SCO config ...");
	/* Start with HCI_CMD_MARVELL_WRITE_PCM_SETTINGS */
	cmd   = HCI_CMD_MARVELL_WRITE_PCM_SETTINGS;
	p_buf = build_cmd_buf(cmd,
			WRITE_PCM_SETTINGS_SIZE,
			write_pcm_settings);

	if (p_buf) {
		ALOGI("Sending hci command 0x%04hX (%s)", cmd, cmd_to_str(cmd));
		if (bt_vendor_cbacks->xmit_cb(cmd, p_buf, hw_mrvl_sco_config_cb))
			return;
		else
			bt_vendor_cbacks->dealloc(p_buf);
	}

	ALOGE("Vendor lib scocfg aborted");
	bt_vendor_cbacks->scocfg_cb(BT_VND_OP_RESULT_FAIL);
}

int bt_vnd_mrvl_if_init(const bt_vendor_callbacks_t *p_cb,
		unsigned char *local_bdaddr)
{
	ALOGI("Marvell BT Vendor Lib: ver %s", VERSION);
	bt_vendor_cbacks = (bt_vendor_callbacks_t *) p_cb;
	memcpy(vnd_local_bd_addr, local_bdaddr, sizeof(vnd_local_bd_addr));
	return 0;
}

int bt_vnd_mrvl_if_op(bt_vendor_opcode_t opcode, void *param)
{
	int ret = 0;
	int *power_state = NULL;
	int local_st = 0;
	int retry=20;

	ALOGD("opcode = %d", opcode);
	switch (opcode) {
	case BT_VND_OP_POWER_CTRL:
		power_state = (int *)param;
		if (BT_VND_PWR_OFF == *power_state) {
			ALOGD("Power off");
			bluetooth_disable();
		} else if (BT_VND_PWR_ON == *power_state) {
			ALOGD("Power on");
			bluetooth_enable();
		} else {
			ret = -1;
		}
		break;
	case BT_VND_OP_FW_CFG:
		hw_mrvl_config_start();
		break;
	case BT_VND_OP_SCO_CFG:
		hw_mrvl_sco_config();
		break;
	case BT_VND_OP_USERIAL_OPEN:
		do {
			mchar_fd = open(mchar_port, O_RDWR|O_NOCTTY);
			if(mchar_fd < 0)
				usleep(200000);
			else
				break;
			retry--;
			if(!retry)
				break;
		}while(1);
		if (mchar_fd < 0) {
			ALOGE("Fail to open port %s", mchar_port);
			ret = -1;
		} else {
			ALOGD("open port %s success", mchar_port);
			ret = 1;
		}
		((int *)param)[0] = mchar_fd;
		break;
	case BT_VND_OP_USERIAL_CLOSE:
		if (mchar_fd < 0) {
			ret = -1;
		} else {
			/* mbtchar port is blocked on read. Release the port
			 * before we close it.
			 */
			ioctl(mchar_fd, MBTCHAR_IOCTL_RELEASE, &local_st);
			/* Give it sometime before we close the mbtchar */
			usleep(1000);
			ALOGD("close port %s", mchar_port);
			if (close(mchar_fd) < 0) {
				ALOGE("Fail to close port %s", mchar_port);
				ret = -1;
			}
		}
		break;
	case BT_VND_OP_GET_LPM_IDLE_TIMEOUT:
		break;
	case BT_VND_OP_LPM_SET_MODE:
		/* TODO: Enable or disable LPM mode on BT Controller.
		 * ret = xx;
		 */
		if (bt_vendor_cbacks)
			bt_vendor_cbacks->lpm_cb(ret);

		break;
	case BT_VND_OP_LPM_WAKE_SET_STATE:
		break;
	default:
		ret = -1;
		break;
	} /* switch (opcode) */

	return ret;
}

void bt_vnd_mrvl_if_cleanup(void)
{
	return;
}

