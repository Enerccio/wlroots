#include <wlr/types/wlr_cursor.h>
#include <stdlib.h>
#include <assert.h>
#include <limits.h>
#include <wlr/util/log.h>
#include <wayland-server.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_input_device.h>

struct wlr_cursor_device {
	struct wlr_cursor *cursor;
	struct wlr_input_device *device;
	struct wl_list link;
	struct wlr_output *mapped_output;
	struct wlr_box *mapped_box;

	struct wl_listener motion;
	struct wl_listener motion_absolute;
	struct wl_listener button;
	struct wl_listener axis;

	struct wl_listener touch_down;
	struct wl_listener touch_up;
	struct wl_listener touch_motion;
	struct wl_listener touch_cancel;

	struct wl_listener tablet_tool_axis;
	struct wl_listener tablet_tool_proximity;
	struct wl_listener tablet_tool_tip;
	struct wl_listener tablet_tool_button;

	struct wl_listener destroy;
};

struct wlr_cursor_state {
	struct wlr_cursor *cursor;
	struct wl_list devices;
	struct wlr_output_layout *layout;
	struct wlr_xcursor *xcursor;
	struct wlr_output *mapped_output;
	struct wlr_box *mapped_box;

	struct wl_listener layout_change;
	struct wl_listener layout_destroy;
};

struct wlr_cursor *wlr_cursor_create() {
	struct wlr_cursor *cur = calloc(1, sizeof(struct wlr_cursor));
	if (!cur) {
		wlr_log(L_ERROR, "Failed to allocate wlr_cursor");
		return NULL;
	}

	cur->state = calloc(1, sizeof(struct wlr_cursor_state));
	if (!cur->state) {
		wlr_log(L_ERROR, "Failed to allocate wlr_cursor_state");
		free(cur);
		return NULL;
	}

	cur->state->cursor = cur;
	cur->state->mapped_output = NULL;

	wl_list_init(&cur->state->devices);

	// pointer signals
	wl_signal_init(&cur->events.motion);
	wl_signal_init(&cur->events.motion_absolute);
	wl_signal_init(&cur->events.button);
	wl_signal_init(&cur->events.axis);

	// touch signals
	wl_signal_init(&cur->events.touch_up);
	wl_signal_init(&cur->events.touch_down);
	wl_signal_init(&cur->events.touch_motion);
	wl_signal_init(&cur->events.touch_cancel);

	// tablet tool signals
	wl_signal_init(&cur->events.tablet_tool_tip);
	wl_signal_init(&cur->events.tablet_tool_axis);
	wl_signal_init(&cur->events.tablet_tool_button);
	wl_signal_init(&cur->events.tablet_tool_proximity);

	cur->x = 100;
	cur->y = 100;

	return cur;
}

static void wlr_cursor_detach_output_layout(struct wlr_cursor *cur) {
	if (!cur->state->layout) {
		return;
	}

	wl_list_remove(&cur->state->layout_destroy.link);
	wl_list_remove(&cur->state->layout_change.link);

	cur->state->layout = NULL;
}

void wlr_cursor_destroy(struct wlr_cursor *cur) {
	wlr_cursor_detach_output_layout(cur);

	struct wlr_cursor_device *device, *device_tmp = NULL;
	wl_list_for_each_safe(device, device_tmp, &cur->state->devices, link) {
		wl_list_remove(&device->link);
		free(device);
	}

	free(cur);
}

void wlr_cursor_set_xcursor(struct wlr_cursor *cur, struct wlr_xcursor *xcur) {
	cur->state->xcursor = xcur;
}

static struct wlr_cursor_device *get_cursor_device(struct wlr_cursor *cur,
		struct wlr_input_device *device) {
	struct wlr_cursor_device *c_device, *ret = NULL;
	wl_list_for_each(c_device, &cur->state->devices, link) {
		if (c_device->device == device) {
			ret = c_device;
			break;
		}
	}

	return ret;
}

static void wlr_cursor_warp_unchecked(struct wlr_cursor *cur,
		double x, double y) {
	assert(cur->state->layout);

	struct wlr_output_layout_output *l_output;
	wl_list_for_each(l_output, &cur->state->layout->outputs, link) {
		double output_x = x;
		double output_y = y;

		wlr_output_layout_output_coords(cur->state->layout,
			l_output->output, &output_x, &output_y);
		wlr_output_move_cursor(l_output->output,
			output_x - l_output->output->cursor.hotspot_x,
			output_y - l_output->output->cursor.hotspot_y);
	}

	cur->x = x;
	cur->y = y;
}

/**
 * Get the most specific mapping box for the device in this order:
 *
 * 1. device geometry mapping
 * 2. device output mapping
 * 3. cursor geometry mapping
 * 4. cursor output mapping
 *
 * Absolute movement for touch and pen devices will be relative to this box and
 * pointer movement will be constrained to this box.
 *
 * If none of these are set, returns NULL and absolute movement should be
 * relative to the extents of the layout.
 */
static struct wlr_box *get_mapping(struct wlr_cursor *cur,
		struct wlr_input_device *dev) {
	assert(cur->state->layout);
	struct wlr_cursor_device *c_device = get_cursor_device(cur, dev);

	if (c_device) {
		if (c_device->mapped_box) {
			return c_device->mapped_box;
		}
		if (c_device->mapped_output) {
			return wlr_output_layout_get_box(cur->state->layout,
				c_device->mapped_output);
		}
	}

	if (cur->state->mapped_box) {
		return cur->state->mapped_box;
	}
	if (cur->state->mapped_output) {
		return wlr_output_layout_get_box(cur->state->layout,
			cur->state->mapped_output);
	}

	return NULL;
}

bool wlr_cursor_warp(struct wlr_cursor *cur, struct wlr_input_device *dev,
		double x, double y) {
	assert(cur->state->layout);
	bool result = false;

	struct wlr_box *mapping = get_mapping(cur, dev);

	if (mapping) {
		if (wlr_box_contains_point(mapping, x, y)) {
			wlr_cursor_warp_unchecked(cur, x, y);
			result = true;
		}
	} else if (wlr_output_layout_contains_point(cur->state->layout, NULL,
				x, y)) {
		wlr_cursor_warp_unchecked(cur, x, y);
		result = true;
	}

	return result;
}

void wlr_cursor_warp_absolute(struct wlr_cursor *cur,
		struct wlr_input_device *dev, double x_mm, double y_mm) {
	assert(cur->state->layout);

	struct wlr_box *mapping = get_mapping(cur, dev);
	if (!mapping) {
		mapping = wlr_output_layout_get_box(cur->state->layout, NULL);
	}

	double x = mapping->width * x_mm + mapping->x;
	double y = mapping->height * y_mm + mapping->y;

	wlr_cursor_warp_unchecked(cur, x, y);
}

void wlr_cursor_move(struct wlr_cursor *cur, struct wlr_input_device *dev,
		double delta_x, double delta_y) {
	assert(cur->state->layout);

	double x = cur->x + delta_x;
	double y = cur->y + delta_y;

	struct wlr_box *mapping = get_mapping(cur, dev);

	if (mapping) {
		double closest_x, closest_y;
		if (!wlr_box_contains_point(mapping, x, y)) {
			wlr_box_closest_point(mapping, x, y, &closest_x,
				&closest_y);
			x = closest_x;
			y = closest_y;
		}
	} else {
		if (!wlr_output_layout_contains_point(cur->state->layout, NULL, x, y)) {
			double layout_x, layout_y;
			wlr_output_layout_closest_point(cur->state->layout, NULL, x, y,
				&layout_x, &layout_y);
			x = layout_x;
			y = layout_y;
		}
	}

	wlr_cursor_warp_unchecked(cur, x, y);
}

static void handle_pointer_motion(struct wl_listener *listener, void *data) {
	struct wlr_event_pointer_motion *event = data;
	struct wlr_cursor_device *device =
		wl_container_of(listener, device, motion);
	wl_signal_emit(&device->cursor->events.motion, event);
}

static void handle_pointer_motion_absolute(struct wl_listener *listener,
		void *data) {
	struct wlr_event_pointer_motion_absolute *event = data;
	struct wlr_cursor_device *device =
		wl_container_of(listener, device, motion_absolute);
	wl_signal_emit(&device->cursor->events.motion_absolute, event);
}

static void handle_pointer_button(struct wl_listener *listener, void *data) {
	struct wlr_event_pointer_button *event = data;
	struct wlr_cursor_device *device =
		wl_container_of(listener, device, button);
	wl_signal_emit(&device->cursor->events.button, event);
}

static void handle_pointer_axis(struct wl_listener *listener, void *data) {
	struct wlr_event_pointer_axis *event = data;
	struct wlr_cursor_device *device = wl_container_of(listener, device, axis);
	wl_signal_emit(&device->cursor->events.axis, event);
}

static void handle_touch_up(struct wl_listener *listener, void *data) {
	struct wlr_event_touch_up *event = data;
	struct wlr_cursor_device *device;
	device = wl_container_of(listener, device, touch_up);
	wl_signal_emit(&device->cursor->events.touch_up, event);
}

static void handle_touch_down(struct wl_listener *listener, void *data) {
	struct wlr_event_touch_down *event = data;
	struct wlr_cursor_device *device;
	device = wl_container_of(listener, device, touch_down);
	wl_signal_emit(&device->cursor->events.touch_down, event);
}

static void handle_touch_motion(struct wl_listener *listener, void *data) {
	struct wlr_event_touch_motion *event = data;
	struct wlr_cursor_device *device;
	device = wl_container_of(listener, device, touch_motion);
	wl_signal_emit(&device->cursor->events.touch_motion, event);
}

static void handle_touch_cancel(struct wl_listener *listener, void *data) {
	struct wlr_event_touch_cancel *event = data;
	struct wlr_cursor_device *device;
	device = wl_container_of(listener, device, touch_cancel);
	wl_signal_emit(&device->cursor->events.touch_cancel, event);
}

static void handle_tablet_tool_tip(struct wl_listener *listener, void *data) {
	struct wlr_event_tablet_tool_tip *event = data;
	struct wlr_cursor_device *device;
	device = wl_container_of(listener, device, tablet_tool_tip);
	wl_signal_emit(&device->cursor->events.tablet_tool_tip, event);
}

static void handle_tablet_tool_axis(struct wl_listener *listener, void *data) {
	struct wlr_event_tablet_tool_axis *event = data;
	struct wlr_cursor_device *device;
	device = wl_container_of(listener, device, tablet_tool_axis);
	wl_signal_emit(&device->cursor->events.tablet_tool_axis, event);
}

static void handle_tablet_tool_button(struct wl_listener *listener,
		void *data) {
	struct wlr_event_tablet_tool_button *event = data;
	struct wlr_cursor_device *device;
	device = wl_container_of(listener, device, tablet_tool_button);
	wl_signal_emit(&device->cursor->events.tablet_tool_button, event);
}

static void handle_tablet_tool_proximity(struct wl_listener *listener,
		void *data) {
	struct wlr_event_tablet_tool_proximity *event = data;
	struct wlr_cursor_device *device;
	device = wl_container_of(listener, device, tablet_tool_proximity);
	wl_signal_emit(&device->cursor->events.tablet_tool_proximity, event);
}

static void handle_device_destroy(struct wl_listener *listener, void *data) {
	struct wlr_cursor_device *c_device;
	c_device = wl_container_of(listener, c_device, destroy);
	wlr_cursor_detach_input_device(c_device->cursor, c_device->device);
}

static struct wlr_cursor_device *wlr_cursor_device_create(
		struct wlr_cursor *cursor, struct wlr_input_device *device) {
	struct wlr_cursor_device *c_device =
		calloc(1, sizeof(struct wlr_cursor_device));
	if (!c_device) {
		wlr_log(L_ERROR, "Failed to allocate wlr_cursor_device");
		return NULL;
	}

	c_device->cursor = cursor;
	c_device->device = device;

	// listen to events
	wl_signal_add(&device->events.destroy, &c_device->destroy);
	c_device->destroy.notify = handle_device_destroy;

	if (device->type == WLR_INPUT_DEVICE_POINTER) {
		wl_signal_add(&device->pointer->events.motion, &c_device->motion);
		c_device->motion.notify = handle_pointer_motion;

		wl_signal_add(&device->pointer->events.motion_absolute,
			&c_device->motion_absolute);
		c_device->motion_absolute.notify = handle_pointer_motion_absolute;

		wl_signal_add(&device->pointer->events.button, &c_device->button);
		c_device->button.notify = handle_pointer_button;

		wl_signal_add(&device->pointer->events.axis, &c_device->axis);
		c_device->axis.notify = handle_pointer_axis;
	} else if (device->type == WLR_INPUT_DEVICE_TOUCH) {
		wl_signal_add(&device->touch->events.motion, &c_device->touch_motion);
		c_device->touch_motion.notify = handle_touch_motion;

		wl_signal_add(&device->touch->events.down, &c_device->touch_down);
		c_device->touch_down.notify = handle_touch_down;

		wl_signal_add(&device->touch->events.up, &c_device->touch_up);
		c_device->touch_up.notify = handle_touch_up;

		wl_signal_add(&device->touch->events.cancel, &c_device->touch_cancel);
		c_device->touch_cancel.notify = handle_touch_cancel;
	} else if (device->type == WLR_INPUT_DEVICE_TABLET_TOOL) {
		wl_signal_add(&device->tablet_tool->events.tip,
			&c_device->tablet_tool_tip);
		c_device->tablet_tool_tip.notify = handle_tablet_tool_tip;

		wl_signal_add(&device->tablet_tool->events.proximity,
			&c_device->tablet_tool_proximity);
		c_device->tablet_tool_proximity.notify = handle_tablet_tool_proximity;

		wl_signal_add(&device->tablet_tool->events.axis,
			&c_device->tablet_tool_axis);
		c_device->tablet_tool_axis.notify = handle_tablet_tool_axis;

		wl_signal_add(&device->tablet_tool->events.button,
			&c_device->tablet_tool_button);
		c_device->tablet_tool_button.notify = handle_tablet_tool_button;
	}

	wl_list_insert(&cursor->state->devices, &c_device->link);

	return c_device;
}

void wlr_cursor_attach_input_device(struct wlr_cursor *cur,
		struct wlr_input_device *dev) {
	if (dev->type != WLR_INPUT_DEVICE_POINTER &&
			dev->type != WLR_INPUT_DEVICE_TOUCH &&
			dev->type != WLR_INPUT_DEVICE_TABLET_TOOL) {
		wlr_log(L_ERROR, "only device types of pointer, touch or tablet tool"
				"are supported");
		return;
	}

	// make sure it is not already attached
	struct wlr_cursor_device *_dev;
	wl_list_for_each(_dev, &cur->state->devices, link) {
		if (_dev->device == dev) {
			return;
		}
	}

	wlr_cursor_device_create(cur, dev);
}

static void wlr_cursor_device_destroy(struct wlr_cursor_device *c_device) {
	struct wlr_input_device *dev = c_device->device;
	if (dev->type == WLR_INPUT_DEVICE_POINTER) {
		wl_list_remove(&c_device->motion.link);
		wl_list_remove(&c_device->motion_absolute.link);
		wl_list_remove(&c_device->button.link);
		wl_list_remove(&c_device->axis.link);
	} else if (dev->type == WLR_INPUT_DEVICE_TOUCH) {
		wl_list_remove(&c_device->touch_down.link);
		wl_list_remove(&c_device->touch_up.link);
		wl_list_remove(&c_device->touch_motion.link);
		wl_list_remove(&c_device->touch_cancel.link);
	} else if (dev->type == WLR_INPUT_DEVICE_TABLET_TOOL) {
		wl_list_remove(&c_device->tablet_tool_axis.link);
		wl_list_remove(&c_device->tablet_tool_proximity.link);
		wl_list_remove(&c_device->tablet_tool_tip.link);
		wl_list_remove(&c_device->tablet_tool_button.link);
	}

	wl_list_remove(&c_device->link);
	wl_list_remove(&c_device->destroy.link);
	free(c_device);

}

void wlr_cursor_detach_input_device(struct wlr_cursor *cur,
		struct wlr_input_device *dev) {
	struct wlr_cursor_device *c_device, *tmp = NULL;
	wl_list_for_each_safe(c_device, tmp, &cur->state->devices, link) {
		if (c_device->device == dev) {
			wlr_cursor_device_destroy(c_device);
		}
	}
}

static void handle_layout_destroy(struct wl_listener *listener, void *data) {
	struct wlr_cursor_state *state =
		wl_container_of(listener, state, layout_change);
	wlr_cursor_detach_output_layout(state->cursor);
}

static void handle_layout_change(struct wl_listener *listener, void *data) {
	struct wlr_cursor_state *state =
		wl_container_of(listener, state, layout_change);
	struct wlr_output_layout *layout = data;
	if (!wlr_output_layout_contains_point(layout, NULL, state->cursor->x,
			state->cursor->y)) {
		// the output we were on has gone away so go to the closest boundary
		// point
		double x, y;
		wlr_output_layout_closest_point(layout, NULL, state->cursor->x,
			state->cursor->y, &x, &y);

		wlr_cursor_warp_unchecked(state->cursor, x, y);
	}
}

void wlr_cursor_attach_output_layout(struct wlr_cursor *cur,
		struct wlr_output_layout *l) {
	wlr_cursor_detach_output_layout(cur);

	if (l == NULL) {
		return;
	}

	wl_signal_add(&l->events.change, &cur->state->layout_change);
	cur->state->layout_change.notify = handle_layout_change;

	wl_signal_add(&l->events.destroy, &cur->state->layout_destroy);
	cur->state->layout_destroy.notify = handle_layout_destroy;

	cur->state->layout = l;
}

void wlr_cursor_map_to_output(struct wlr_cursor *cur,
		struct wlr_output *output) {
	cur->state->mapped_output = output;
}

void wlr_cursor_map_input_to_output(struct wlr_cursor *cur,
		struct wlr_input_device *dev, struct wlr_output *output) {
	struct wlr_cursor_device *c_device = get_cursor_device(cur, dev);
	if (!c_device) {
		wlr_log(L_ERROR, "Cannot map device \"%s\" to output"
			"(not found in this cursor)", dev->name);
		return;
	}

	c_device->mapped_output = output;
}

void wlr_cursor_map_to_region(struct wlr_cursor *cur,
		struct wlr_box *box) {
	if (box && wlr_box_empty(box)) {
		wlr_log(L_ERROR, "cannot map cursor to an empty region");
		return;
	}

	cur->state->mapped_box = box;
}

void wlr_cursor_map_input_to_region(struct wlr_cursor *cur,
		struct wlr_input_device *dev, struct wlr_box *box) {
	if (box && wlr_box_empty(box)) {
		wlr_log(L_ERROR, "cannot map device \"%s\" input to an empty region",
			dev->name);
		return;
	}

	struct wlr_cursor_device *c_device = get_cursor_device(cur, dev);
	if (!c_device) {
		wlr_log(L_ERROR, "Cannot map device \"%s\" to geometry (not found in"
			"this cursor)", dev->name);
		return;
	}

	c_device->mapped_box = box;
}
