/* Spa Bluez5 Device
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>

#include <spa/support/log.h>
#include <spa/utils/type.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>
#include <spa/node/node.h>
#include <spa/support/loop.h>
#include <spa/support/plugin.h>
#include <spa/monitor/device.h>
#include <spa/monitor/utils.h>
#include <spa/monitor/event.h>
#include <spa/pod/filter.h>
#include <spa/pod/parser.h>
#include <spa/param/param.h>
#include <spa/param/audio/raw.h>
#include <spa/debug/pod.h>

#include "defs.h"
#include "a2dp-codecs.h"

#define NAME  "bluez5-device"

#define MAX_DEVICES	64

static const char default_device[] = "";

struct props {
	char device[64];
};

static void reset_props(struct props *props)
{
	strncpy(props->device, default_device, 64);
}

struct node {
	uint32_t id;
	unsigned int active:1;
	unsigned int mute:1;
	uint32_t n_channels;
	uint32_t channels[SPA_AUDIO_MAX_CHANNELS];
	float volumes[SPA_AUDIO_MAX_CHANNELS];
};

struct impl {
	struct spa_handle handle;
	struct spa_device device;

	struct spa_log *log;

	uint32_t info_all;
	struct spa_device_info info;
#define IDX_EnumProfile		0
#define IDX_Profile		1
#define IDX_EnumRoute		2
#define IDX_Route		3
	struct spa_param_info params[4];

	struct spa_hook_list hooks;

	struct props props;

	struct spa_bt_device *bt_dev;

	uint32_t profile;
	struct node nodes[2];
};

static void init_node(struct impl *this, struct node *node, uint32_t id)
{
	uint32_t i;

	spa_zero(*node);
	node->id = id;
	for (i = 0; i < SPA_AUDIO_MAX_CHANNELS; i++)
		node->volumes[i] = 1.0;
}

static void emit_node(struct impl *this, struct spa_bt_transport *t,
		uint32_t id, const char *factory_name)
{
	struct spa_bt_device *device = this->bt_dev;
	struct spa_device_object_info info;
	struct spa_dict_item items[5];
	char transport[32], str_id[32];

	snprintf(transport, sizeof(transport), "pointer:%p", t);
	items[0] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_TRANSPORT, transport);
	items[1] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_PROFILE, spa_bt_profile_name(t->profile));
	items[2] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_CODEC,
			t->a2dp_codec ? t->a2dp_codec->name : "unknown");
	snprintf(str_id, sizeof(str_id), "%d", id);
	items[3] = SPA_DICT_ITEM_INIT("card.profile.device", str_id);
	items[4] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_ADDRESS, device->address);

	info = SPA_DEVICE_OBJECT_INFO_INIT();
	info.type = SPA_TYPE_INTERFACE_Node;
	info.factory_name = factory_name;
	info.change_mask = SPA_DEVICE_OBJECT_CHANGE_MASK_PROPS;
	info.props = &SPA_DICT_INIT_ARRAY(items);

	spa_device_emit_object_info(&this->hooks, id, &info);

	this->nodes[id].active = true;
	this->nodes[id].n_channels = t->n_channels;
	memcpy(this->nodes[id].channels, t->channels,
			t->n_channels * sizeof(uint32_t));
}

static struct spa_bt_transport *find_transport(struct impl *this, int profile)
{
	struct spa_bt_device *device = this->bt_dev;
	struct spa_bt_transport *t;

	spa_list_for_each(t, &device->transport_list, device_link) {
		if (t->profile & device->connected_profiles &&
		    (t->profile & profile) == t->profile)
			return t;
	}
	return NULL;
}

static int emit_nodes(struct impl *this)
{
	struct spa_bt_transport *t;

	switch (this->profile) {
	case 0:
		break;
	case 1:
		if (this->bt_dev->connected_profiles & SPA_BT_PROFILE_A2DP_SOURCE) {
			t = find_transport(this, SPA_BT_PROFILE_A2DP_SOURCE);
			if (t)
				emit_node(this, t, 0, SPA_NAME_API_BLUEZ5_A2DP_SOURCE);
		}

		if (this->bt_dev->connected_profiles & SPA_BT_PROFILE_A2DP_SINK) {
			t = find_transport(this, SPA_BT_PROFILE_A2DP_SINK);
			if (t)
				emit_node(this, t, 1, SPA_NAME_API_BLUEZ5_A2DP_SINK);
		}
		break;
	case 2:
		if (this->bt_dev->connected_profiles &
		    (SPA_BT_PROFILE_HEADSET_HEAD_UNIT | SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY) ) {
			int i;

			for (i = SPA_BT_PROFILE_HSP_HS ; i <= SPA_BT_PROFILE_HFP_AG ; i <<= 1) {
				t = find_transport(this, i);
				if (t)
					break;
			}
			if (t == NULL)
				break;
			emit_node(this, t, 0, SPA_NAME_API_BLUEZ5_SCO_SOURCE);
			emit_node(this, t, 1, SPA_NAME_API_BLUEZ5_SCO_SINK);
		}
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static const struct spa_dict_item info_items[] = {
	{ SPA_KEY_DEVICE_API, "bluez5" },
	{ SPA_KEY_MEDIA_CLASS, "Audio/Device" },
};

static void emit_info(struct impl *this, bool full)
{
	if (full)
		this->info.change_mask = this->info_all;
	if (this->info.change_mask) {
		this->info.props = &SPA_DICT_INIT_ARRAY(info_items);

		spa_device_emit_info(&this->hooks, &this->info);
		this->info.change_mask = 0;
	}
}

static int set_profile(struct impl *this, uint32_t profile)
{
	uint32_t i;

	if (this->profile == profile)
		return 0;

	for (i = 0; i < 2; i++) {
		if (this->nodes[i].active) {
			spa_device_emit_object_info(&this->hooks, i, NULL);
			this->nodes[i].active = false;
		}
	}
	this->profile = profile;

	emit_nodes(this);

	this->info.change_mask |= SPA_DEVICE_CHANGE_MASK_PARAMS;
	this->params[IDX_Profile].flags ^= SPA_PARAM_INFO_SERIAL;
	this->params[IDX_Route].flags ^= SPA_PARAM_INFO_SERIAL;
	this->params[IDX_EnumRoute].flags ^= SPA_PARAM_INFO_SERIAL;
	emit_info(this, false);

	return 0;
}

static int impl_add_listener(void *object,
			struct spa_hook *listener,
			const struct spa_device_events *events,
			void *data)
{
	struct impl *this = object;
	struct spa_hook_list save;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(events != NULL, -EINVAL);

	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	if (events->info)
		emit_info(this, true);

	if (events->object_info)
		emit_nodes(this);

	spa_hook_list_join(&this->hooks, &save);

	return 0;
}

static int impl_sync(void *object, int seq)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_device_emit_result(&this->hooks, seq, 0, 0, NULL);

	return 0;
}

static uint32_t profile_direction_mask(struct impl *this, uint32_t index)
{
	struct spa_bt_device *device = this->bt_dev;
	uint32_t profile, mask;
	bool have_output = false, have_input = false;

	switch (index) {
	case 1:
		profile = device->connected_profiles &
				(SPA_BT_PROFILE_A2DP_SINK |
				 SPA_BT_PROFILE_A2DP_SOURCE);
		if (profile == SPA_BT_PROFILE_A2DP_SINK)
			have_output = true;
		else if (profile == SPA_BT_PROFILE_A2DP_SOURCE)
			have_input = true;
		else
			have_output = have_input = true;
		break;
	case 2:
		profile = device->connected_profiles &
				(SPA_BT_PROFILE_HEADSET_HEAD_UNIT |
				 SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY);
		if (profile == 0)
			break;
		have_output = have_input = true;
		break;
	}

	mask = 0;
	if (have_output)
		mask |= 1 << SPA_DIRECTION_OUTPUT;
	if (have_input)
		mask |= 1 << SPA_DIRECTION_INPUT;
	return mask;
}

static struct spa_pod *build_profile(struct impl *this, struct spa_pod_builder *b,
		uint32_t id, uint32_t index)
{
	struct spa_bt_device *device = this->bt_dev;
	struct spa_pod_frame f[2];
	const char *name, *desc;
	uint32_t n_source = 0, n_sink = 0;

	switch (index) {
	case 0:
		name = "off";
		desc = "Off";
		break;
	case 1:
	{
		uint32_t profile = device->connected_profiles &
		      (SPA_BT_PROFILE_A2DP_SINK | SPA_BT_PROFILE_A2DP_SOURCE);
		if (profile == 0) {
			return NULL;
		} else if (profile == SPA_BT_PROFILE_A2DP_SINK) {
			desc = "High Fidelity Playback (A2DP Sink)";
			name = "a2dp-sink";
		} else if (profile == SPA_BT_PROFILE_A2DP_SOURCE) {
			desc = "High Fidelity Capture (A2DP Source)";
			name = "a2dp-source";
		} else {
			desc = "High Fidelity Duplex (A2DP Source/Sink)";
			name = "a2dp-duplex";
		}
		if (profile & SPA_BT_PROFILE_A2DP_SOURCE)
			n_source++;
		if (profile & SPA_BT_PROFILE_A2DP_SINK)
			n_sink++;
		break;
	}
	case 2:
	{
		uint32_t profile = device->connected_profiles &
		      (SPA_BT_PROFILE_HEADSET_HEAD_UNIT | SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY);
		if (profile == 0) {
			return NULL;
		} else if (profile == SPA_BT_PROFILE_HEADSET_HEAD_UNIT) {
			desc = "Headset Head Unit (HSP/HFP)";
			name = "headset-head-unit";
		} else if (profile == SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY) {
			desc = "Headset Audio Gateway (HSP/HFP)";
			name = "headset-audio-gateway";
		} else {
			desc = "Headset Audio (HSP/HFP)";
			name = "headset-audio";
		}
		n_source++;
		n_sink++;
		break;
	}
	default:
		errno = -EINVAL;
		return NULL;
	}

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_ParamProfile, id);
	spa_pod_builder_add(b,
		SPA_PARAM_PROFILE_index,   SPA_POD_Int(index),
		SPA_PARAM_PROFILE_name, SPA_POD_String(name),
		SPA_PARAM_PROFILE_description, SPA_POD_String(desc),
		0);
	if (n_source > 0 || n_sink > 0) {
		spa_pod_builder_prop(b, SPA_PARAM_PROFILE_classes, 0);
		spa_pod_builder_push_struct(b, &f[1]);
		if (n_source > 0) {
			spa_pod_builder_add_struct(b,
				SPA_POD_String("Audio/Source"),
				SPA_POD_Int(n_source));
		}
		if (n_sink > 0) {
			spa_pod_builder_add_struct(b,
				SPA_POD_String("Audio/Sink"),
				SPA_POD_Int(n_sink));
		}
		spa_pod_builder_pop(b, &f[1]);
	}
	return spa_pod_builder_pop(b, &f[0]);
}

static struct spa_pod *build_route(struct impl *this, struct spa_pod_builder *b,
		uint32_t id, uint32_t port, uint32_t dev, uint32_t profile)
{
	struct spa_bt_device *device = this->bt_dev;
	struct spa_pod_frame f[2];
	enum spa_direction direction;
	const char *name_prefix, *description, *port_type;
	enum spa_param_availability available;
	enum spa_bt_form_factor ff;
	char name[128];
	uint32_t i;

	ff = spa_bt_form_factor_from_class(device->bluetooth_class);

	switch (ff) {
	case SPA_BT_FORM_FACTOR_HEADSET:
		name_prefix = "headset";
		description = "Headset";
		port_type = "headset";
		break;
	case SPA_BT_FORM_FACTOR_HANDSFREE:
		name_prefix = "handsfree";
		description = "Handsfree";
		port_type = "handsfree";
		break;
	case SPA_BT_FORM_FACTOR_MICROPHONE:
		name_prefix = "microphone";
		description = "Microphone";
		port_type = "mic";
		break;
	case SPA_BT_FORM_FACTOR_SPEAKER:
		name_prefix = "speaker";
		description = "Speaker";
		port_type = "speaker";
		break;
	case SPA_BT_FORM_FACTOR_HEADPHONE:
		name_prefix = "headphone";
		description = "Headphone";
		port_type = "headphones";
		break;
	case SPA_BT_FORM_FACTOR_PORTABLE:
		name_prefix = "portable";
		description = "Portable";
		port_type = "portable";
		break;
	case SPA_BT_FORM_FACTOR_CAR:
		name_prefix = "car";
		description = "Car";
		port_type = "car";
		break;
	case SPA_BT_FORM_FACTOR_HIFI:
		name_prefix = "hifi";
		description = "HiFi";
		port_type = "hifi";
		break;
	case SPA_BT_FORM_FACTOR_PHONE:
		name_prefix = "phone";
		description = "Phone";
		port_type = "phone";
		break;
	case SPA_BT_FORM_FACTOR_UNKNOWN:
	default:
		name_prefix = "bluetooth";
		description = "Bluetooth";
		port_type = "bluetooth";
		break;
	}

	switch (port) {
	case 0:
		direction = SPA_DIRECTION_INPUT;
		snprintf(name, sizeof(name), "%s-input", name_prefix);
		break;
	case 1:
		direction = SPA_DIRECTION_OUTPUT;
		snprintf(name, sizeof(name), "%s-output", name_prefix);
		break;
	default:
		errno = -EINVAL;
		return NULL;
	}

	available = profile_direction_mask(this, this->profile) & (1 << direction) ?
			SPA_PARAM_AVAILABILITY_yes : SPA_PARAM_AVAILABILITY_no;
	if (dev != SPA_ID_INVALID && available == SPA_PARAM_AVAILABILITY_no)
		return NULL;

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_ParamRoute, id);
	spa_pod_builder_add(b,
		SPA_PARAM_ROUTE_index, SPA_POD_Int(port),
		SPA_PARAM_ROUTE_direction,  SPA_POD_Id(direction),
		SPA_PARAM_ROUTE_name,  SPA_POD_String(name),
		SPA_PARAM_ROUTE_description,  SPA_POD_String(description),
		SPA_PARAM_ROUTE_priority,  SPA_POD_Int(0),
		SPA_PARAM_ROUTE_available,  SPA_POD_Id(available),
		0);
	spa_pod_builder_prop(b, SPA_PARAM_ROUTE_info, 0);
	spa_pod_builder_push_struct(b, &f[1]);
	spa_pod_builder_int(b, 1);
	spa_pod_builder_add(b,
			SPA_POD_String("port.type"),
			SPA_POD_String(port_type),
			NULL);
	spa_pod_builder_pop(b, &f[1]);
	spa_pod_builder_prop(b, SPA_PARAM_ROUTE_profiles, 0);
	spa_pod_builder_push_array(b, &f[1]);
	for (i = 0; i < 3; i++) {
		if (profile_direction_mask(this, i) & (1 << direction))
			spa_pod_builder_int(b, i);
	}
	spa_pod_builder_pop(b, &f[1]);

	if (dev != SPA_ID_INVALID) {
		struct node *node = &this->nodes[dev];

		spa_pod_builder_prop(b, SPA_PARAM_ROUTE_device, 0);
		spa_pod_builder_int(b, dev);

		spa_pod_builder_prop(b, SPA_PARAM_ROUTE_props, 0);
		spa_pod_builder_push_object(b, &f[1], SPA_TYPE_OBJECT_Props, id);

		spa_pod_builder_prop(b, SPA_PROP_mute, 0);
		spa_pod_builder_bool(b, node->mute);

		spa_pod_builder_prop(b, SPA_PROP_channelVolumes, 0);
		spa_pod_builder_array(b, sizeof(float), SPA_TYPE_Float,
				node->n_channels, node->volumes);

		spa_pod_builder_prop(b, SPA_PROP_channelMap, 0);
		spa_pod_builder_array(b, sizeof(uint32_t), SPA_TYPE_Id,
				node->n_channels, node->channels);

		spa_pod_builder_pop(b, &f[1]);
	}

	spa_pod_builder_prop(b, SPA_PARAM_ROUTE_devices, 0);
	spa_pod_builder_push_array(b, &f[1]);
	/* port and device indexes are the same, 0=source, 1=sink */
	spa_pod_builder_int(b, port);
	spa_pod_builder_pop(b, &f[1]);

	if (profile != SPA_ID_INVALID) {
		spa_pod_builder_prop(b, SPA_PARAM_ROUTE_profile, 0);
		spa_pod_builder_int(b, profile);
	}
	return spa_pod_builder_pop(b, &f[0]);
}

static int impl_enum_params(void *object, int seq,
			    uint32_t id, uint32_t start, uint32_t num,
			    const struct spa_pod *filter)
{
	struct impl *this = object;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_result_device_params result;
	uint32_t count = 0;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	result.id = id;
	result.next = start;
      next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_EnumProfile:
	{
		switch (result.index) {
		case 0: case 1: case 2:
			param = build_profile(this, &b, id, result.index);
			if (param == NULL)
				goto next;
			break;
		default:
			return 0;
		}
		break;
	}
	case SPA_PARAM_Profile:
	{
		switch (result.index) {
		case 0:
			param = build_profile(this, &b, id, this->profile);
			if (param == NULL)
				return 0;
			break;
		default:
			return 0;
		}
		break;
	}
	case SPA_PARAM_EnumRoute:
	{
		switch (result.index) {
		case 0: case 1:
			param = build_route(this, &b, id, result.index,
					SPA_ID_INVALID, SPA_ID_INVALID);
			break;
		default:
			return 0;
		}
		break;
	}
	case SPA_PARAM_Route:
	{
		switch (result.index) {
		case 0: case 1:
			param = build_route(this, &b, id, result.index,
					result.index, this->profile);
			if (param == NULL)
				goto next;
			break;
		default:
			return 0;
		}
		break;
	}
	default:
		return -ENOENT;
	}

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_device_emit_result(&this->hooks, seq, 0,
			SPA_RESULT_TYPE_DEVICE_PARAMS, &result);

	if (++count != num)
		goto next;

	return 0;
}

static int node_set_volume(struct impl *this, struct node *node, float volumes[], uint32_t n_volumes)
{
	struct spa_event *event;
	uint8_t buffer[4096];
	struct spa_pod_builder b = { 0 };
	struct spa_pod_frame f[1];

	spa_log_info(this->log, "node %p volume %f", node, volumes[0]);

	node->n_channels = n_volumes;
	memcpy(node->volumes, volumes, sizeof(float) * SPA_AUDIO_MAX_CHANNELS);

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	spa_pod_builder_push_object(&b, &f[0],
			SPA_TYPE_EVENT_Device, SPA_DEVICE_EVENT_ObjectConfig);
	spa_pod_builder_prop(&b, SPA_EVENT_DEVICE_Object, 0);
	spa_pod_builder_int(&b, node->id);
	spa_pod_builder_prop(&b, SPA_EVENT_DEVICE_Props, 0);
	spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_Props, SPA_EVENT_DEVICE_Props,
	SPA_PROP_channelVolumes, SPA_POD_Array(sizeof(float),
			SPA_TYPE_Float, n_volumes, volumes),
	SPA_PROP_channelMap, SPA_POD_Array(sizeof(uint32_t),
			SPA_TYPE_Id, node->n_channels, node->channels));
	event = spa_pod_builder_pop(&b, &f[0]);

	spa_device_emit_event(&this->hooks, event);

	return 0;
}

static int node_set_mute(struct impl *this, struct node *node, bool mute)
{
	struct spa_event *event;
	uint8_t buffer[4096];
	struct spa_pod_builder b = { 0 };
	struct spa_pod_frame f[1];

	spa_log_info(this->log, "node %p mute %d", node, mute);

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	spa_pod_builder_push_object(&b, &f[0],
			SPA_TYPE_EVENT_Device, SPA_DEVICE_EVENT_ObjectConfig);
	spa_pod_builder_prop(&b, SPA_EVENT_DEVICE_Object, 0);
	spa_pod_builder_int(&b, node->id);
	spa_pod_builder_prop(&b, SPA_EVENT_DEVICE_Props, 0);

	spa_pod_builder_add_object(&b,
	SPA_TYPE_OBJECT_Props, SPA_EVENT_DEVICE_Props,
			SPA_PROP_mute, SPA_POD_Bool(mute));
	event = spa_pod_builder_pop(&b, &f[0]);

	spa_device_emit_event(&this->hooks, event);

	return 0;
}

static int apply_device_props(struct impl *this, struct node *node, struct spa_pod *props)
{
	float volume = 0;
	bool mute = 0;
	struct spa_pod_prop *prop;
	struct spa_pod_object *obj = (struct spa_pod_object *) props;
	int changed = 0;
	float volumes[SPA_AUDIO_MAX_CHANNELS];
	uint32_t channels[SPA_AUDIO_MAX_CHANNELS];
	uint32_t n_volumes = 0, n_channels = 0;

	if (!spa_pod_is_object_type(props, SPA_TYPE_OBJECT_Props))
		return -EINVAL;

	SPA_POD_OBJECT_FOREACH(obj, prop) {
		switch (prop->key) {
		case SPA_PROP_volume:
			if (spa_pod_get_float(&prop->value, &volume) == 0) {
				node_set_volume(this, node, &volume, 1);
				changed++;
			}
			break;
		case SPA_PROP_mute:
			if (spa_pod_get_bool(&prop->value, &mute) == 0) {
				node_set_mute(this, node, mute);
				changed++;
			}
			break;
		case SPA_PROP_channelVolumes:
			if ((n_volumes = spa_pod_copy_array(&prop->value, SPA_TYPE_Float,
					volumes, SPA_AUDIO_MAX_CHANNELS)) > 0) {
				changed++;
			}
			break;
		case SPA_PROP_channelMap:
			if ((n_channels = spa_pod_copy_array(&prop->value, SPA_TYPE_Id,
					channels, SPA_AUDIO_MAX_CHANNELS)) > 0) {
				changed++;
			}
			break;
		}
	}
	if (n_volumes > 0)
		node_set_volume(this, node, volumes, n_volumes);

	return changed;
}

static int impl_set_param(void *object,
			  uint32_t id, uint32_t flags,
			  const struct spa_pod *param)
{
	struct impl *this = object;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	switch (id) {
	case SPA_PARAM_Profile:
	{
		uint32_t id;

		if ((res = spa_pod_parse_object(param,
				SPA_TYPE_OBJECT_ParamProfile, NULL,
				SPA_PARAM_PROFILE_index, SPA_POD_Int(&id))) < 0) {
			spa_log_warn(this->log, "can't parse profile");
			spa_debug_pod(0, NULL, param);
			return res;
		}
		set_profile(this, id);
		break;
	}
	case SPA_PARAM_Route:
	{
		uint32_t id, device;
		struct spa_pod *props = NULL;
		struct node *node;

		if ((res = spa_pod_parse_object(param,
				SPA_TYPE_OBJECT_ParamRoute, NULL,
				SPA_PARAM_ROUTE_index, SPA_POD_Int(&id),
				SPA_PARAM_ROUTE_device, SPA_POD_Int(&device),
				SPA_PARAM_ROUTE_props, SPA_POD_OPT_Pod(&props))) < 0) {
			spa_log_warn(this->log, "can't parse route");
			spa_debug_pod(0, NULL, param);
			return res;
		}
		if (device > 2 || !this->nodes[device].active)
			return -EINVAL;

		node = &this->nodes[device];
		if (props) {
			apply_device_props(this, node, props);
			this->info.change_mask |= SPA_DEVICE_CHANGE_MASK_PARAMS;
			this->params[IDX_Route].flags ^= SPA_PARAM_INFO_SERIAL;
			emit_info(this, false);
		}
		break;
	}
	default:
		return -ENOENT;
	}
	return 0;
}

static const struct spa_device_methods impl_device = {
	SPA_VERSION_DEVICE_METHODS,
	.add_listener = impl_add_listener,
	.sync = impl_sync,
	.enum_params = impl_enum_params,
	.set_param = impl_set_param,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (strcmp(type, SPA_TYPE_INTERFACE_Device) == 0)
		*interface = &this->device;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct impl);
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct impl *this;
	const char *str;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);

	if (info && (str = spa_dict_lookup(info, SPA_KEY_API_BLUEZ5_DEVICE)))
		sscanf(str, "pointer:%p", &this->bt_dev);

	if (this->bt_dev == NULL) {
		spa_log_error(this->log, "a device is needed");
		return -EINVAL;
	}
	this->device.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Device,
			SPA_VERSION_DEVICE,
			&impl_device, this);

	spa_hook_list_init(&this->hooks);

	reset_props(&this->props);

	init_node(this, &this->nodes[0], 0);
	init_node(this, &this->nodes[1], 1);

	this->info = SPA_DEVICE_INFO_INIT();
	this->info_all = SPA_DEVICE_CHANGE_MASK_PROPS |
		SPA_DEVICE_CHANGE_MASK_PARAMS;

	this->params[IDX_EnumProfile] = SPA_PARAM_INFO(SPA_PARAM_EnumProfile, SPA_PARAM_INFO_READ);
	this->params[IDX_Profile] = SPA_PARAM_INFO(SPA_PARAM_Profile, SPA_PARAM_INFO_READWRITE);
	this->params[IDX_EnumRoute] = SPA_PARAM_INFO(SPA_PARAM_EnumRoute, SPA_PARAM_INFO_READ);
	this->params[IDX_Route] = SPA_PARAM_INFO(SPA_PARAM_Route, SPA_PARAM_INFO_READWRITE);
	this->info.params = this->params;
	this->info.n_params = 4;

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_Device,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
			 const struct spa_interface_info **info,
			 uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	if (*index >= SPA_N_ELEMENTS(impl_interfaces))
		return 0;

	*info = &impl_interfaces[(*index)++];
	return 1;
}

static const struct spa_dict_item handle_info_items[] = {
	{ SPA_KEY_FACTORY_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ SPA_KEY_FACTORY_DESCRIPTION, "A bluetooth device" },
	{ SPA_KEY_FACTORY_USAGE, SPA_KEY_API_BLUEZ5_DEVICE"=<device>" },
};

static const struct spa_dict handle_info = SPA_DICT_INIT_ARRAY(handle_info_items);

const struct spa_handle_factory spa_bluez5_device_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_BLUEZ5_DEVICE,
	&handle_info,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
