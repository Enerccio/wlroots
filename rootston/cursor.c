#define _XOPEN_SOURCE 500
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
// TODO: BSD et al
#include <linux/input-event-codes.h>
#include <wayland-server.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/util/log.h>
#include "rootston/config.h"
#include "rootston/input.h"
#include "rootston/desktop.h"

const struct roots_input_event *get_input_event(struct roots_input *input,
		uint32_t serial) {
	size_t len = sizeof(input->input_events) / sizeof(*input->input_events);
	for (size_t i = 0; i < len; ++i) {
		if (input->input_events[i].cursor
				&& input->input_events[i].serial == serial) {
			return &input->input_events[i];
		}
	}
	return NULL;
}

void view_begin_move(struct roots_input *input, struct wlr_cursor *cursor,
		struct roots_view *view) {
	input->mode = ROOTS_CURSOR_MOVE;
	input->offs_x = cursor->x;
	input->offs_y = cursor->y;
	input->view_x = view->x;
	input->view_y = view->y;
	wlr_seat_pointer_clear_focus(input->wl_seat);
}

void view_begin_resize(struct roots_input *input, struct wlr_cursor *cursor,
		struct roots_view *view, uint32_t edges) {
	input->mode = ROOTS_CURSOR_RESIZE;
	input->offs_x = cursor->x;
	input->offs_y = cursor->y;
	input->view_x = view->x;
	input->view_y = view->y;
	struct wlr_box size;
	view_get_size(view, &size);
	input->view_width = size.width;
	input->view_height = size.height;
	input->resize_edges = edges;
	wlr_seat_pointer_clear_focus(input->wl_seat);
}

void view_begin_rotate(struct roots_input *input, struct wlr_cursor *cursor,
		struct roots_view *view) {
	input->mode = ROOTS_CURSOR_ROTATE;
	input->offs_x = cursor->x;
	input->offs_y = cursor->y;
	input->view_rotation = view->rotation;
	wlr_seat_pointer_clear_focus(input->wl_seat);
}

static void cursor_set_xcursor_image(struct roots_input *input,
		struct wlr_xcursor_image *image) {
	struct roots_output *output;
	wl_list_for_each(output, &input->server->desktop->outputs, link) {
		if (!wlr_output_set_cursor(output->wlr_output, image->buffer,
				image->width, image->width, image->height,
				image->hotspot_x, image->hotspot_y)) {
			wlr_log(L_DEBUG, "Failed to set hardware cursor");
			return;
		}
	}
}

void cursor_update_position(struct roots_input *input, uint32_t time) {
	struct roots_desktop *desktop = input->server->desktop;
	struct roots_view *view;
	struct wlr_surface *surface;
	double sx, sy;
	switch (input->mode) {
	case ROOTS_CURSOR_PASSTHROUGH:
		view = view_at(desktop, input->cursor->x, input->cursor->y, &surface,
			&sx, &sy);
		if (view != input->client_cursor_view) {
			wlr_log(L_DEBUG, "Switching to compositor cursor");
			cursor_set_xcursor_image(input, input->xcursor->images[0]);
			input->client_cursor_view = NULL;
		}
		if (view) {
			wlr_seat_pointer_notify_enter(input->wl_seat, surface, sx, sy);
			wlr_seat_pointer_notify_motion(input->wl_seat, time, sx, sy);
		} else {
			wlr_seat_pointer_clear_focus(input->wl_seat);
		}
		break;
	case ROOTS_CURSOR_MOVE:
		if (input->active_view) {
			int dx = input->cursor->x - input->offs_x,
				dy = input->cursor->y - input->offs_y;
			input->active_view->x = input->view_x + dx;
			input->active_view->y = input->view_y + dy;
		}
		break;
	case ROOTS_CURSOR_RESIZE:
		if (input->active_view) {
			int dx = input->cursor->x - input->offs_x,
				dy = input->cursor->y - input->offs_y;
			int width = input->view_width,
				height = input->view_height;
			if (input->resize_edges & ROOTS_CURSOR_RESIZE_EDGE_TOP) {
				input->active_view->y = input->view_y + dy;
				height -= dy;
			}
			if (input->resize_edges & ROOTS_CURSOR_RESIZE_EDGE_BOTTOM) {
				height += dy;
			}
			if (input->resize_edges & ROOTS_CURSOR_RESIZE_EDGE_LEFT) {
				input->active_view->x = input->view_x + dx;
				width -= dx;
			}
			if (input->resize_edges & ROOTS_CURSOR_RESIZE_EDGE_RIGHT) {
				width += dx;
			}
			view_resize(input->active_view, width, height);
		}
		break;
	case ROOTS_CURSOR_ROTATE:
		if (input->active_view) {
			struct roots_view *view = input->active_view;
			int ox = view->x + view->wlr_surface->current->width/2,
				oy = view->y + view->wlr_surface->current->height/2;
			int ux = input->offs_x - ox,
				uy = input->offs_y - oy;
			int vx = input->cursor->x - ox,
				vy = input->cursor->y - oy;
			float angle = atan2(vx*uy - vy*ux, vx*ux + vy*uy);
			int steps = 12;
			angle = round(angle/M_PI*steps) / (steps/M_PI);
			view->rotation = input->view_rotation + angle;
		}
		break;
	}
}

void set_view_focus(struct roots_input *input, struct roots_desktop *desktop,
		struct roots_view *view) {
	if (input->active_view == view) {
		return;
	}
	input->active_view = view;
	input->mode = ROOTS_CURSOR_PASSTHROUGH;
	if (!view) {
		return;
	}
	input->last_active_view = view;

	size_t index = 0;
	for (size_t i = 0; i < desktop->views->length; ++i) {
		struct roots_view *_view = desktop->views->items[i];
		if (_view != view) {
			view_activate(_view, false);
		} else {
			index = i;
		}
	}
	view_activate(view, true);
	// TODO: list_swap
	list_del(desktop->views, index);
	list_add(desktop->views, view);
}

static void handle_cursor_motion(struct wl_listener *listener, void *data) {
	struct roots_input *input = wl_container_of(listener, input, cursor_motion);
	struct wlr_event_pointer_motion *event = data;
	wlr_cursor_move(input->cursor, event->device,
			event->delta_x, event->delta_y);
	cursor_update_position(input, (uint32_t)(event->time_usec / 1000));
}

static void handle_cursor_motion_absolute(struct wl_listener *listener,
		void *data) {
	struct roots_input *input = wl_container_of(listener,
			input, cursor_motion_absolute);
	struct wlr_event_pointer_motion_absolute *event = data;
	wlr_cursor_warp_absolute(input->cursor, event->device,
		event->x_mm / event->width_mm, event->y_mm / event->height_mm);
	cursor_update_position(input, (uint32_t)(event->time_usec / 1000));
}

static void handle_cursor_axis(struct wl_listener *listener, void *data) {
	struct roots_input *input =
		wl_container_of(listener, input, cursor_axis);
	struct wlr_event_pointer_axis *event = data;
	wlr_seat_pointer_notify_axis(input->wl_seat, event->time_sec,
		event->orientation, event->delta);
}

static bool is_meta_pressed(struct roots_input *input) {
	uint32_t meta_key = input->server->config->keyboard.meta_key;
	if (meta_key == 0) {
		return false;
	}

	struct roots_keyboard *keyboard;
	wl_list_for_each(keyboard, &input->keyboards, link) {
		uint32_t modifiers =
			wlr_keyboard_get_modifiers(keyboard->device->keyboard);
		if ((modifiers ^ meta_key) == 0) {
			return true;
		}
	}
	return false;
}

static void do_cursor_button_press(struct roots_input *input,
		struct wlr_cursor *cursor, struct wlr_input_device *device,
		uint32_t time, uint32_t button, uint32_t state) {
	struct roots_desktop *desktop = input->server->desktop;
	struct wlr_surface *surface;
	double sx, sy;
	struct roots_view *view = view_at(desktop,
		input->cursor->x, input->cursor->y, &surface, &sx, &sy);

	if (state == WLR_BUTTON_PRESSED && view && is_meta_pressed(input)) {
		set_view_focus(input, desktop, view);

		switch (button) {
		case BTN_LEFT:
			view_begin_move(input, cursor, view);
			break;
		case BTN_RIGHT:
			view_begin_resize(input, cursor, view,
				ROOTS_CURSOR_RESIZE_EDGE_RIGHT |
				ROOTS_CURSOR_RESIZE_EDGE_BOTTOM);
			break;
		case BTN_MIDDLE:
			view_begin_rotate(input, cursor, view);
		}
		return;
	}

	uint32_t serial = wlr_seat_pointer_notify_button(input->wl_seat, time, button,
		state);

	int i;
	switch (state) {
	case WLR_BUTTON_RELEASED:
		set_view_focus(input, desktop, NULL);
		break;
	case WLR_BUTTON_PRESSED:
		i = input->input_events_idx;
		input->input_events[i].serial = serial;
		input->input_events[i].cursor = cursor;
		input->input_events[i].device = device;
		input->input_events_idx = (i + 1)
			% (sizeof(input->input_events) / sizeof(input->input_events[0]));
		set_view_focus(input, desktop, view);
		if (view) {
			wlr_seat_keyboard_notify_enter(input->wl_seat, surface);
		}
		break;
	}
}

static void handle_cursor_button(struct wl_listener *listener, void *data) {
	struct roots_input *input = wl_container_of(listener, input, cursor_button);
	struct wlr_event_pointer_button *event = data;
	do_cursor_button_press(input, input->cursor, event->device,
			(uint32_t)(event->time_usec / 1000), event->button, event->state);
}

static void handle_tool_axis(struct wl_listener *listener, void *data) {
	struct roots_input *input = wl_container_of(listener, input, cursor_tool_axis);
	struct wlr_event_tablet_tool_axis *event = data;
	if ((event->updated_axes & WLR_TABLET_TOOL_AXIS_X) &&
			(event->updated_axes & WLR_TABLET_TOOL_AXIS_Y)) {
		wlr_cursor_warp_absolute(input->cursor, event->device,
			event->x_mm / event->width_mm, event->y_mm / event->height_mm);
		cursor_update_position(input, (uint32_t)(event->time_usec / 1000));
	}
}

static void handle_tool_tip(struct wl_listener *listener, void *data) {
	struct roots_input *input = wl_container_of(listener, input, cursor_tool_tip);
	struct wlr_event_tablet_tool_tip *event = data;
	do_cursor_button_press(input, input->cursor, event->device,
			(uint32_t)(event->time_usec / 1000), BTN_LEFT, event->state);
}

static void handle_pointer_grab_end(struct wl_listener *listener, void *data) {
	struct roots_input *input =
		wl_container_of(listener, input, pointer_grab_end);
	cursor_update_position(input, 0);
}

static void handle_request_set_cursor(struct wl_listener *listener,
		void *data) {
	struct roots_input *input = wl_container_of(listener, input,
		request_set_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;

	struct wlr_surface *focused_surface = NULL;
	double sx, sy;
	struct roots_view *focused_view = view_at(input->server->desktop,
		input->cursor->x, input->cursor->y, &focused_surface, &sx, &sy);
	bool ok = focused_surface != NULL;
	if (focused_surface != NULL) {
		struct wl_client *focused_client =
			wl_resource_get_client(focused_surface->resource);
		ok = event->client == focused_client;
	}
	if (!ok) {
		wlr_log(L_DEBUG, "Denying request to set cursor outside view");
		return;
	}

	wlr_log(L_DEBUG, "Setting client cursor");

	struct roots_output *output;
	wl_list_for_each(output, &input->server->desktop->outputs, link) {
		wlr_output_set_cursor_surface(output->wlr_output, event->surface,
			event->hotspot_x, event->hotspot_y);
	}

	input->client_cursor_view = focused_view;
}

void cursor_initialize(struct roots_input *input) {
	struct wlr_cursor *cursor = input->cursor;

	wl_list_init(&input->cursor_motion.link);
	wl_signal_add(&cursor->events.motion, &input->cursor_motion);
	input->cursor_motion.notify = handle_cursor_motion;

	wl_list_init(&input->cursor_motion_absolute.link);
	wl_signal_add(&cursor->events.motion_absolute,
		&input->cursor_motion_absolute);
	input->cursor_motion_absolute.notify = handle_cursor_motion_absolute;

	wl_list_init(&input->cursor_button.link);
	wl_signal_add(&cursor->events.button, &input->cursor_button);
	input->cursor_button.notify = handle_cursor_button;

	wl_list_init(&input->cursor_axis.link);
	wl_signal_add(&cursor->events.axis, &input->cursor_axis);
	input->cursor_axis.notify = handle_cursor_axis;

	wl_list_init(&input->cursor_tool_axis.link);
	wl_signal_add(&cursor->events.tablet_tool_axis, &input->cursor_tool_axis);
	input->cursor_tool_axis.notify = handle_tool_axis;

	wl_list_init(&input->cursor_tool_tip.link);
	wl_signal_add(&cursor->events.tablet_tool_tip, &input->cursor_tool_tip);
	input->cursor_tool_tip.notify = handle_tool_tip;

	wl_signal_add(&input->wl_seat->events.pointer_grab_end, &input->pointer_grab_end);
	input->pointer_grab_end.notify = handle_pointer_grab_end;

	wl_list_init(&input->request_set_cursor.link);
	wl_signal_add(&input->wl_seat->events.request_set_cursor,
		&input->request_set_cursor);
	input->request_set_cursor.notify = handle_request_set_cursor;
}

static void reset_device_mappings(struct roots_config *config,
		struct wlr_cursor *cursor, struct wlr_input_device *device) {
	wlr_cursor_map_input_to_output(cursor, device, NULL);
	struct device_config *dconfig;
	if ((dconfig = config_get_device(config, device))) {
		wlr_cursor_map_input_to_region(cursor, device, dconfig->mapped_box);
	}
}

static void set_device_output_mappings(struct roots_config *config,
		struct wlr_cursor *cursor, struct wlr_output *output,
		struct wlr_input_device *device) {
	struct device_config *dconfig;
	dconfig = config_get_device(config, device);
	if (dconfig && dconfig->mapped_output &&
			strcmp(dconfig->mapped_output, output->name) == 0) {
		wlr_cursor_map_input_to_output(cursor, device, output);
	}
}

void cursor_load_config(struct roots_config *config,
		struct wlr_cursor *cursor,
		struct roots_input *input,
		struct roots_desktop *desktop) {
	struct roots_pointer *pointer;
	struct roots_touch *touch;
	struct roots_tablet_tool *tablet_tool;
	struct roots_output *output;

	// reset mappings
	wlr_cursor_map_to_output(cursor, NULL);
	wl_list_for_each(pointer, &input->pointers, link) {
		reset_device_mappings(config, cursor, pointer->device);
	}
	wl_list_for_each(touch, &input->touch, link) {
		reset_device_mappings(config, cursor, touch->device);
	}
	wl_list_for_each(tablet_tool, &input->tablet_tools, link) {
		reset_device_mappings(config, cursor, tablet_tool->device);
	}

	// configure device to output mappings
	const char *mapped_output = config->cursor.mapped_output;
	wl_list_for_each(output, &desktop->outputs, link) {
		if (mapped_output && strcmp(mapped_output, output->wlr_output->name) == 0) {
			wlr_cursor_map_to_output(cursor, output->wlr_output);
		}

		wl_list_for_each(pointer, &input->pointers, link) {
			set_device_output_mappings(config, cursor, output->wlr_output,
				pointer->device);
		}
		wl_list_for_each(tablet_tool, &input->tablet_tools, link) {
			set_device_output_mappings(config, cursor, output->wlr_output,
				tablet_tool->device);
		}
		wl_list_for_each(touch, &input->touch, link) {
			set_device_output_mappings(config, cursor, output->wlr_output,
				touch->device);
		}
	}
}
