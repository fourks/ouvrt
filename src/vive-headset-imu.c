/*
 * HTC Vive Headset IMU
 * Copyright 2016 Philipp Zabel
 * SPDX-License-Identifier:	LGPL-2.0+
 */
#include <asm/byteorder.h>
#include <errno.h>
#include <json-glib/json-glib.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/fcntl.h>
#include <unistd.h>

#include "vive-headset-imu.h"
#include "vive-hid-reports.h"
#include "vive-config.h"
#include "device.h"
#include "hidraw.h"
#include "imu.h"
#include "json.h"
#include "math.h"

struct _OuvrtViveHeadsetIMUPrivate {
	JsonNode *config;
	uint8_t sequence;
	vec3 acc_bias;
	vec3 acc_scale;
	vec3 gyro_bias;
	vec3 gyro_scale;
};

G_DEFINE_TYPE_WITH_PRIVATE(OuvrtViveHeadsetIMU, ouvrt_vive_headset_imu, \
			   OUVRT_TYPE_DEVICE)

/*
 * Downloads the configuration data stored in the headset
 */
static int vive_headset_imu_get_config(OuvrtViveHeadsetIMU *self)
{
	char *config_json;
	JsonObject *object;

	config_json = ouvrt_vive_get_config(&self->dev);
	if (!config_json)
		return -1;

	self->priv->config = json_from_string(config_json, NULL);
	g_free(config_json);
	if (!self->priv->config) {
		g_print("%s: Parsing JSON configuration data failed\n",
			self->dev.name);
		return -1;
	}

	object = json_node_get_object(self->priv->config);

	json_object_get_vec3_member(object, "acc_bias", &self->priv->acc_bias);
	json_object_get_vec3_member(object, "acc_scale", &self->priv->acc_scale);
	json_object_get_vec3_member(object, "gyro_bias", &self->priv->gyro_bias);
	json_object_get_vec3_member(object, "gyro_scale", &self->priv->gyro_scale);

	return 0;
}

/*
 * Retrieves the headset firmware version
 */
static int vive_headset_get_firmware_version(OuvrtViveHeadsetIMU *self)
{
	struct vive_firmware_version_report report = {
		.id = VIVE_FIRMWARE_VERSION_REPORT_ID,
	};
	uint32_t firmware_version;
	int ret;

	ret = hid_get_feature_report(self->dev.fd, &report, sizeof(report));
	if (ret < 0) {
		g_print("%s: Read error 0x05: %d\n", self->dev.name,
			errno);
		return ret;
	}

	firmware_version = __le32_to_cpu(report.firmware_version);

	g_print("%s: Headset firmware version %u %s@%s FPGA %u.%u\n",
		self->dev.name, firmware_version, report.string1,
		report.string2, report.fpga_version_major,
		report.fpga_version_minor);
	g_print("%s: Hardware revision: %d rev %d.%d.%d\n", self->dev.name,
		report.hardware_revision, report.hardware_version_major,
		report.hardware_version_minor, report.hardware_version_micro);

	return 0;
}

static inline int oldest_sequence_index(uint8_t a, uint8_t b, uint8_t c)
{
	if (a == (uint8_t)(b + 2))
		return 1;
	else if (b == (uint8_t)(c + 2))
		return 2;
	else
		return 0;
}

/*
 * Decodes the periodic sensor message containing IMU sample(s).
 */
static void vive_headset_imu_decode_message(OuvrtViveHeadsetIMU *self,
					    const void *buf, size_t len)
{
	const struct vive_headset_imu_report *report = buf;
	const struct vive_headset_imu_sample *sample = report->sample;
	uint8_t last_seq = self->priv->sequence;
	int i, j;

	(void)len;

	/*
	 * The three samples are updated round-robin. New messages
	 * can contain already seen samples in any place, but the
	 * sequence numbers should always be consecutive.
	 * Start at the sample with the oldest sequence number.
	 */
	i = oldest_sequence_index(sample[0].seq, sample[1].seq, sample[2].seq);

	/* From there, handle all new samples */
	for (j = 3; j; --j, i = (i + 1) % 3) {
		struct raw_imu_sample raw;
		uint8_t seq;

		sample = report->sample + i;
		seq = sample->seq;

		/* Skip already seen samples */
		if (seq == last_seq ||
		    seq == (uint8_t)(last_seq - 1) ||
		    seq == (uint8_t)(last_seq - 2))
			continue;

		raw.accel[0] = (int16_t)__le16_to_cpu(sample->acc[0]);
		raw.acc[1] = (int16_t)__le16_to_cpu(sample->acc[1]);
		raw.acc[2] = (int16_t)__le16_to_cpu(sample->acc[2]);
		raw.gyro[0] = (int16_t)__le16_to_cpu(sample->gyro[0]);
		raw.gyro[1] = (int16_t)__le16_to_cpu(sample->gyro[1]);
		raw.gyro[2] = (int16_t)__le16_to_cpu(sample->gyro[2]);
		raw.time = __le32_to_cpu(sample->time);

		(void)raw;

		self->priv->sequence = seq;
	}
}

static int vive_headset_enable_lighthouse(OuvrtViveHeadsetIMU *self)
{
	unsigned char buf[5] = { 0x04 };
	int ret;

	ret = hid_send_feature_report(self->dev.fd, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	/*
	 * Reset Lighthouse Rx registers? Without this, inactive channels are
	 * not cleared to 0xff.
	 */
	buf[0] = 0x07;
	buf[1] = 0x02;
	return hid_send_feature_report(self->dev.fd, buf, sizeof(buf));
}

/*
 * Opens the IMU device, reads the stored configuration and enables
 * the Lighthouse receiver.
 */
static int vive_headset_imu_start(OuvrtDevice *dev)
{
	OuvrtViveHeadsetIMU *self = OUVRT_VIVE_HEADSET_IMU(dev);
	int fd = self->dev.fd;
	int ret;

	if (fd == -1) {
		fd = open(self->dev.devnode, O_RDWR | O_NONBLOCK);
		if (fd == -1) {
			g_print("%s: Failed to open '%s': %d\n", dev->name,
				dev->devnode, errno);
			return -1;
		}
		dev->fd = fd;
	}

	ret = vive_headset_get_firmware_version(self);
	if (ret < 0) {
		g_print("%s: Failed to get firmware version\n", dev->name);
		return ret;
	}

	ret = vive_headset_imu_get_config(self);
	if (ret < 0) {
		g_print("%s: Failed to read configuration\n", dev->name);
		return ret;
	}

	ret = vive_headset_enable_lighthouse(self);
	if (ret < 0) {
		g_print("%s: Failed to enable Lighthouse Receiver\n",
			dev->name);
		return ret;
	}

	return 0;
}

/*
 * Handles IMU messages.
 */
static void vive_headset_imu_thread(OuvrtDevice *dev)
{
	OuvrtViveHeadsetIMU *self = OUVRT_VIVE_HEADSET_IMU(dev);
	unsigned char buf[64];
	struct pollfd fds;
	int ret;

	while (dev->active) {
		fds.fd = dev->fd;
		fds.events = POLLIN;
		fds.revents = 0;

		ret = poll(&fds, 1, 1000);
		if (ret == -1) {
			g_print("%s: Poll failure: %d\n", dev->name, errno);
			continue;
		}

		if (fds.events & (POLLERR | POLLHUP | POLLNVAL))
			break;

		if (!(fds.revents & POLLIN)) {
			g_print("%s: Poll timeout\n", dev->name);
			continue;
		}

		ret = read(dev->fd, buf, sizeof(buf));
		if (ret == -1) {
			g_print("%s: Read error: %d\n", dev->name, errno);
			continue;
		}
		if (ret != 52 || buf[0] != VIVE_HEADSET_IMU_REPORT_ID) {
			g_print("%s: Error, invalid %d-byte report 0x%02x\n",
				dev->name, ret, buf[0]);
			continue;
		}

		vive_headset_imu_decode_message(self, buf, 52);
	}
}

/*
 * Nothing to do here.
 */
static void vive_headset_imu_stop(OuvrtDevice *dev)
{
	(void)dev;
}

/*
 * Frees the device structure and its contents.
 */
static void ouvrt_vive_headset_imu_finalize(GObject *object)
{
	G_OBJECT_CLASS(ouvrt_vive_headset_imu_parent_class)->finalize(object);
}

static void ouvrt_vive_headset_imu_class_init(OuvrtViveHeadsetIMUClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = ouvrt_vive_headset_imu_finalize;
	OUVRT_DEVICE_CLASS(klass)->start = vive_headset_imu_start;
	OUVRT_DEVICE_CLASS(klass)->thread = vive_headset_imu_thread;
	OUVRT_DEVICE_CLASS(klass)->stop = vive_headset_imu_stop;
}

static void ouvrt_vive_headset_imu_init(OuvrtViveHeadsetIMU *self)
{
	self->dev.type = DEVICE_TYPE_HMD;
	self->priv = ouvrt_vive_headset_imu_get_instance_private(self);
}

/*
 * Allocates and initializes the device structure.
 *
 * Returns the newly allocated Vive Headset IMU device.
 */
OuvrtDevice *vive_headset_imu_new(const char *devnode)
{
	OuvrtViveHeadsetIMU *vive;

	vive = g_object_new(OUVRT_TYPE_VIVE_HEADSET_IMU, NULL);
	if (vive == NULL)
		return NULL;

	vive->dev.devnode = g_strdup(devnode);

	return &vive->dev;
}
