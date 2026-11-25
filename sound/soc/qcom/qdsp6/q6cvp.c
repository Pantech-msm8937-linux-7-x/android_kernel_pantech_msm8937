// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2012-2017, The Linux Foundation. All rights reserved.
// Copyright (c) 2020, Stephan Gerhold

#include <linux/module.h>
#include <linux/of.h>
#include <linux/soc/qcom/apr.h>
#include "q6cvp.h"
#include "q6voice-common.h"

#define VSS_IVOCPROC_DIRECTION_RX	0
#define VSS_IVOCPROC_DIRECTION_TX	1
#define VSS_IVOCPROC_DIRECTION_RX_TX	2

#define VSS_IVOCPROC_PORT_ID_NONE	0xFFFF

#define VSS_IVOCPROC_TOPOLOGY_ID_NONE			0x00010F70
#define VSS_IVOCPROC_TOPOLOGY_ID_TX_SM_ECNS		0x00010F71
#define VSS_IVOCPROC_TOPOLOGY_ID_TX_DM_FLUENCE		0x00010F72

#define VSS_IVOCPROC_TOPOLOGY_ID_RX_DEFAULT		0x00010F77

#define VSS_IVOCPROC_VOCPROC_MODE_EC_INT_MIXING		0x00010F7C
#define VSS_IVOCPROC_VOCPROC_MODE_EC_EXT_MIXING		0x00010F7D

#define VSS_ICOMMON_CAL_NETWORK_ID_NONE			0x0001135E

#define VSS_IVOCPROC_CMD_ENABLE				0x000100C6
#define VSS_IVOCPROC_CMD_DISABLE			0x000110E1

#define VSS_IVOCPROC_CMD_CREATE_FULL_CONTROL_SESSION_V2	0x000112BF
#define VSS_IVOCPROC_CMD_CREATE_FULL_CONTROL_SESSION_V3	0x00013169
#define VSS_IVOCPROC_CMD_TOPOLOGY_SET_DEV_CHANNELS	0x00013199
#define VSS_IVOCPROC_CMD_TOPOLOGY_COMMIT		0x00013198
#define VSS_ICOMMON_CMD_SET_PARAM_V2			0x0001133D

#define VSS_MODULE_CVD_GENERIC				0x0001316E
#define VSS_PARAM_TX_PORT_ENDPOINT_MEDIA_INFO		0x00013253
#define VSS_PARAM_RX_PORT_ENDPOINT_MEDIA_INFO		0x00013254

#define VSS_NUM_CHANNELS_MAX				32
#define PCM_CHANNEL_FL					1
#define PCM_CHANNEL_FR					2

/*
 * The media format of the AFE ports the vocproc runs on: the back ends of
 * the sound cards run them at 48 kHz in stereo.
 */
#define Q6CVP_DEV_RATE					48000
#define Q6CVP_DEV_CHANNELS				2
#define Q6CVP_DEV_BITS					16

struct vss_ivocproc_cmd_create_full_control_session_v2_cmd {
	struct apr_hdr hdr;

	/*
	 * Vocproc direction. The supported values:
	 * VSS_IVOCPROC_DIRECTION_RX
	 * VSS_IVOCPROC_DIRECTION_TX
	 * VSS_IVOCPROC_DIRECTION_RX_TX
	 */
	u16 direction;

	/*
	 * Tx device port ID to which the vocproc connects. If a port ID is
	 * not being supplied, set this to #VSS_IVOCPROC_PORT_ID_NONE.
	 */
	u16 tx_port_id;

	/*
	 * Tx path topology ID. If a topology ID is not being supplied, set
	 * this to #VSS_IVOCPROC_TOPOLOGY_ID_NONE.
	 */
	u32 tx_topology_id;

	/*
	 * Rx device port ID to which the vocproc connects. If a port ID is
	 * not being supplied, set this to #VSS_IVOCPROC_PORT_ID_NONE.
	 */
	u16 rx_port_id;

	/*
	 * Rx path topology ID. If a topology ID is not being supplied, set
	 * this to #VSS_IVOCPROC_TOPOLOGY_ID_NONE.
	 */
	u32 rx_topology_id;

	/* Voice calibration profile ID. */
	u32 profile_id;

	/*
	 * Vocproc mode. The supported values:
	 * VSS_IVOCPROC_VOCPROC_MODE_EC_INT_MIXING
	 * VSS_IVOCPROC_VOCPROC_MODE_EC_EXT_MIXING
	 */
	u32 vocproc_mode;

	/*
	 * Port ID to which the vocproc connects for receiving echo
	 * cancellation reference signal. If a port ID is not being supplied,
	 * set this to #VSS_IVOCPROC_PORT_ID_NONE. This parameter value is
	 * ignored when the vocproc_mode parameter is set to
	 * VSS_IVOCPROC_VOCPROC_MODE_EC_INT_MIXING.
	 */
	u16 ec_ref_port_id;

	/*
	 * Session name string used to identify a session that can be shared
	 * with passive controllers (optional).
	 */
	char name[20];
} __packed;

struct vss_ivocproc_cmd_topology_set_dev_channels_cmd {
	struct apr_hdr hdr;
	u16 tx_num_channels;
	u16 rx_num_channels;
} __packed;

struct vss_param_endpoint_media_format_info {
	u32 port_id;
	u16 num_channels;
	u16 bits_per_sample;
	u32 sample_rate;
	u8 channel_mapping[VSS_NUM_CHANNELS_MAX];
} __packed;

/* VSS_ICOMMON_CMD_SET_PARAM_V2 with one in-band parameter, header v1 */
struct vss_icommon_cmd_set_param_media_format_cmd {
	struct apr_hdr hdr;
	u32 mem_handle;
	u64 mem_address;
	u32 mem_size;
	u32 module_id;
	u32 param_id;
	u16 param_size;
	u16 reserved;
	struct vss_param_endpoint_media_format_info info;
} __packed;

static int q6cvp_set_dev_channels(struct q6voice_session *cvp)
{
	struct vss_ivocproc_cmd_topology_set_dev_channels_cmd cmd = { };

	cmd.hdr.pkt_size = sizeof(cmd);
	cmd.hdr.opcode = VSS_IVOCPROC_CMD_TOPOLOGY_SET_DEV_CHANNELS;
	/* One microphone, one speaker channel for the vocproc */
	cmd.tx_num_channels = 1;
	cmd.rx_num_channels = 1;

	return q6voice_common_send(cvp, &cmd.hdr);
}

static int q6cvp_set_media_format(struct q6voice_session *cvp, u32 param_id,
				  u16 port)
{
	struct vss_icommon_cmd_set_param_media_format_cmd cmd = { };

	cmd.hdr.pkt_size = sizeof(cmd);
	cmd.hdr.opcode = VSS_ICOMMON_CMD_SET_PARAM_V2;
	cmd.mem_size = sizeof(cmd) - offsetof(typeof(cmd), module_id);
	cmd.module_id = VSS_MODULE_CVD_GENERIC;
	cmd.param_id = param_id;
	cmd.param_size = sizeof(cmd.info);
	cmd.info.port_id = port;
	cmd.info.num_channels = Q6CVP_DEV_CHANNELS;
	cmd.info.bits_per_sample = Q6CVP_DEV_BITS;
	cmd.info.sample_rate = Q6CVP_DEV_RATE;
	cmd.info.channel_mapping[0] = PCM_CHANNEL_FL;
	cmd.info.channel_mapping[1] = PCM_CHANNEL_FR;

	return q6voice_common_send(cvp, &cmd.hdr);
}

static int q6cvp_topology_commit(struct q6voice_session *cvp)
{
	struct apr_hdr hdr = { };

	hdr.pkt_size = APR_HDR_SIZE;
	hdr.opcode = VSS_IVOCPROC_CMD_TOPOLOGY_COMMIT;

	return q6voice_common_send(cvp, &hdr);
}

/*
 * CVD 2.2 and later take the vocproc topology in pieces: the device
 * channels and the media format of the device ports before a commit. Left
 * out, the vocproc assumes its narrowband defaults and stays silent on the
 * receive path of wideband calls (VoLTE) while narrowband ones play.
 */
static void q6cvp_configure(struct q6voice_session *cvp, u16 tx_port,
			    u16 rx_port)
{
	int ret;

	ret = q6cvp_set_dev_channels(cvp);
	if (ret)
		dev_warn(cvp->dev, "failed to set device channels: %d\n", ret);

	ret = q6cvp_set_media_format(cvp, VSS_PARAM_RX_PORT_ENDPOINT_MEDIA_INFO,
				     rx_port);
	if (!ret)
		ret = q6cvp_set_media_format(cvp,
					     VSS_PARAM_TX_PORT_ENDPOINT_MEDIA_INFO,
					     tx_port);
	if (ret)
		dev_info(cvp->dev, "no device media format (CVD before 2.3?): %d\n",
			 ret);

	ret = q6cvp_topology_commit(cvp);
	if (ret)
		dev_warn(cvp->dev, "failed to commit the topology: %d\n", ret);
}

struct q6voice_session *q6cvp_session_create(enum q6voice_path_type path,
					     u16 tx_port, u16 rx_port)
{
	/* Zeroed: the optional session name stays empty */
	struct vss_ivocproc_cmd_create_full_control_session_v2_cmd cmd = { };
	struct q6voice_session *cvp;

	cmd.hdr.pkt_size = sizeof(cmd);
	cmd.hdr.opcode = VSS_IVOCPROC_CMD_CREATE_FULL_CONTROL_SESSION_V3;

	/* TODO: Implement calibration */
	cmd.tx_topology_id = VSS_IVOCPROC_TOPOLOGY_ID_TX_SM_ECNS;
	cmd.rx_topology_id = VSS_IVOCPROC_TOPOLOGY_ID_RX_DEFAULT;

	cmd.direction = VSS_IVOCPROC_DIRECTION_RX_TX;
	cmd.tx_port_id = tx_port;
	cmd.rx_port_id = rx_port;
	cmd.profile_id = VSS_ICOMMON_CAL_NETWORK_ID_NONE;
	cmd.vocproc_mode = VSS_IVOCPROC_VOCPROC_MODE_EC_INT_MIXING;
	cmd.ec_ref_port_id = VSS_IVOCPROC_PORT_ID_NONE;

	/* V3 (CVD 2.2 and later) takes the same payload */
	cvp = q6voice_session_create(Q6VOICE_SERVICE_CVP, path, &cmd.hdr);
	if (!IS_ERR(cvp)) {
		q6cvp_configure(cvp, tx_port, rx_port);
		return cvp;
	}

	cmd.hdr.opcode = VSS_IVOCPROC_CMD_CREATE_FULL_CONTROL_SESSION_V2;
	return q6voice_session_create(Q6VOICE_SERVICE_CVP, path, &cmd.hdr);
}
EXPORT_SYMBOL_GPL(q6cvp_session_create);

int q6cvp_enable(struct q6voice_session *cvp, bool state)
{
	struct apr_pkt cmd;

	cmd.hdr.pkt_size = APR_HDR_SIZE;
	cmd.hdr.opcode = state ? VSS_IVOCPROC_CMD_ENABLE : VSS_IVOCPROC_CMD_DISABLE;

	return q6voice_common_send(cvp, &cmd.hdr);
}
EXPORT_SYMBOL_GPL(q6cvp_enable);

static int q6cvp_probe(struct apr_device *adev)
{
	return q6voice_common_probe(adev, Q6VOICE_SERVICE_CVP);
}

static const struct of_device_id q6cvp_device_id[]  = {
	{ .compatible = "qcom,q6cvp" },
	{},
};
MODULE_DEVICE_TABLE(of, q6cvp_device_id);

static struct apr_driver qcom_q6cvp_driver = {
	.probe = q6cvp_probe,
	.remove = q6voice_common_remove,
	.callback = q6voice_common_callback,
	.driver = {
		.name = "qcom-q6cvp",
		.of_match_table = of_match_ptr(q6cvp_device_id),
	},
};

module_apr_driver(qcom_q6cvp_driver);

MODULE_AUTHOR("Stephan Gerhold <stephan@gerhold.net>");
MODULE_DESCRIPTION("Q6 Core Voice Processor");
MODULE_LICENSE("GPL v2");
