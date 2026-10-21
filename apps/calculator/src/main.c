#include <morpthis/morpthis.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

MORPTHIS_DEFINE_API_VERSION();

// A four-function calculator. It shows the widget set, a callback carrying a
// value in its user pointer, and the back button.

static char g_display[32] = "0";
static double g_accumulator;
static char g_pending;
static bool g_fresh = true;
static morpthis_widget_t *g_display_widget;

static void refresh(void) {
	if (g_display_widget) {
		morpthis_widget_set_text(g_display_widget, g_display);
	}
}

static void show(const char *text) {
	snprintf(g_display, sizeof(g_display), "%s", text);
	refresh();
}

static void show_value(double value) {
	snprintf(g_display, sizeof(g_display), "%.10g", value);
	refresh();
}

static double current_value(void) {
	return strtod(g_display, NULL);
}

// Returns false when the operation is undefined; the caller resets then.
static bool apply_pending(void) {
	double value = current_value();

	switch (g_pending) {
	case '+':
		g_accumulator += value;
		break;
	case '-':
		g_accumulator -= value;
		break;
	case '*':
		g_accumulator *= value;
		break;
	case '/':
		if (value == 0.0) {
			morpthis_toast("Cannot divide by zero");
			return false;
		}
		g_accumulator /= value;
		break;
	default:
		g_accumulator = value;
		break;
	}

	g_pending = '\0';
	return true;
}

static void reset(void) {
	g_accumulator = 0.0;
	g_pending = '\0';
	g_fresh = true;
	show("0");
}

static void digit_clicked(morpthis_widget_t *widget, void *user) {
	(void)widget;
	char digit = (char)(intptr_t)user;

	if (g_fresh || strcmp(g_display, "0") == 0) {
		g_display[0] = digit;
		g_display[1] = '\0';
		g_fresh = false;
	} else if (strlen(g_display) < sizeof(g_display) - 2) {
		size_t length = strlen(g_display);
		g_display[length] = digit;
		g_display[length + 1] = '\0';
	}
	refresh();
}

static void dot_clicked(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;

	if (g_fresh) {
		show("0.");
		g_fresh = false;
		return;
	}
	if (!strchr(g_display, '.') && strlen(g_display) < sizeof(g_display) - 2) {
		strcat(g_display, ".");
	}
	refresh();
}

static void operator_clicked(morpthis_widget_t *widget, void *user) {
	(void)widget;
	char op = (char)(intptr_t)user;

	if (g_pending) {
		if (!apply_pending()) {
			reset();
			return;
		}
		show_value(g_accumulator);
	} else {
		g_accumulator = current_value();
	}
	g_pending = op;
	g_fresh = true;
}

static void equals_clicked(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;

	if (!g_pending) {
		return;
	}
	if (!apply_pending()) {
		reset();
		return;
	}
	show_value(g_accumulator);
	g_fresh = true;
}

static void clear_clicked(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	reset();
}

static void back_pressed(morpthis_button_t button, morpthis_input_action_t action, void *user) {
	(void)user;
	if (button == MORPTHIS_BUTTON_BACK && action == MORPTHIS_INPUT_PRESS) {
		morpthis_logi("calc", "exit");
		morpthis_app_quit();
	}
}

static void add_key(morpthis_screen_t *screen, const char *text, int x, int y, int width,
					int height, morpthis_click_cb cb, void *user) {
	morpthis_widget_t *key = morpthis_button_create(screen, text, cb, user);
	morpthis_widget_set_pos(key, x, y);
	morpthis_widget_set_size(key, width, height);
}

int morpthis_app_main(const morpthis_app_t *app) {
	morpthis_logi("calc", "started, api %d", morpthis_app_api_version(app));

	morpthis_screen_t *screen = morpthis_screen_create("Calculator");
	if (!screen) {
		morpthis_loge("calc", "could not create the screen");
		return 1;
	}

	const int width = morpthis_ui_width();
	const int height = morpthis_ui_height();
	const int left = 12;
	const int top = 60;
	const int gap = 4;
	const int key_w = (width - left * 2 - gap * 3) / 4;
	const int key_h = (height - top - 12 - gap * 4) / 5;
	const int step_x = key_w + gap;
	const int step_y = key_h + gap;

	g_display_widget = morpthis_label_create(screen, g_display);
	morpthis_widget_set_pos(g_display_widget, left, 8);
	morpthis_widget_set_size(g_display_widget, width - left * 2, 40);

	add_key(screen, "C", left, top, key_w, key_h, clear_clicked, NULL);
	add_key(screen, "/", left + step_x, top, key_w, key_h, operator_clicked, (void *)(intptr_t)'/');
	add_key(screen, "*", left + step_x * 2, top, key_w, key_h, operator_clicked,
			(void *)(intptr_t)'*');
	add_key(screen, "-", left + step_x * 3, top, key_w, key_h, operator_clicked,
			(void *)(intptr_t)'-');

	add_key(screen, "7", left, top + step_y, key_w, key_h, digit_clicked, (void *)(intptr_t)'7');
	add_key(screen, "8", left + step_x, top + step_y, key_w, key_h, digit_clicked,
			(void *)(intptr_t)'8');
	add_key(screen, "9", left + step_x * 2, top + step_y, key_w, key_h, digit_clicked,
			(void *)(intptr_t)'9');
	add_key(screen, "+", left + step_x * 3, top + step_y, key_w, key_h, operator_clicked,
			(void *)(intptr_t)'+');

	add_key(screen, "4", left, top + step_y * 2, key_w, key_h, digit_clicked,
			(void *)(intptr_t)'4');
	add_key(screen, "5", left + step_x, top + step_y * 2, key_w, key_h, digit_clicked,
			(void *)(intptr_t)'5');
	add_key(screen, "6", left + step_x * 2, top + step_y * 2, key_w, key_h, digit_clicked,
			(void *)(intptr_t)'6');
	add_key(screen, "=", left + step_x * 3, top + step_y * 2, key_w, key_h * 3 + gap * 2,
			equals_clicked, NULL);

	add_key(screen, "1", left, top + step_y * 3, key_w, key_h, digit_clicked,
			(void *)(intptr_t)'1');
	add_key(screen, "2", left + step_x, top + step_y * 3, key_w, key_h, digit_clicked,
			(void *)(intptr_t)'2');
	add_key(screen, "3", left + step_x * 2, top + step_y * 3, key_w, key_h, digit_clicked,
			(void *)(intptr_t)'3');

	add_key(screen, "0", left, top + step_y * 4, key_w * 2 + gap, key_h, digit_clicked,
			(void *)(intptr_t)'0');
	add_key(screen, ".", left + step_x * 2, top + step_y * 4, key_w, key_h, dot_clicked, NULL);

	morpthis_input_set_button_cb(back_pressed, NULL);
	morpthis_screen_load(screen);
	return 0;
}
