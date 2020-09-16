/**********************************************************************************
* Copyright (c) 2020, Valentin DEBON                                              *
* All rights reserved.                                                            *
*                                                                                 *
* Redistribution and use in source and binary forms, with or without              *
* modification, are permitted provided that the following conditions are met:     *
*     * Redistributions of source code must retain the above copyright            *
*       notice, this list of conditions and the following disclaimer.             *
*     * Redistributions in binary form must reproduce the above copyright         *
*       notice, this list of conditions and the following disclaimer in the       *
*       documentation and/or other materials provided with the distribution.      *
*     * Neither the name of the copyright holder nor the                          *
*       names of its contributors may be used to endorse or promote products      *
*       derived from this software without specific prior written permission.     *
*                                                                                 *
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND *
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED   *
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE          *
* DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY            *
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES      *
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;    *
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND     *
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT      *
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS   *
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.                    *
**********************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <math.h>
#include <err.h>

#define KEYSYM_SPECIAL_NOSYMBOL 0x00
#define KEYSYM_LATIN_1_P 'p'
#define KEYSYM_LATIN_1_Q 'q'
#define KEYSYM_LATIN_1_R 'r'
#define KEYSYM_LATIN_1_S 's'
#define KEYSYM_LATIN_1_Z 'z'
#define KEYSYM_FUNCTION_UP   0xFF52
#define KEYSYM_FUNCTION_DOWN 0xFF54

#define PLAYER_1 0
#define PLAYER_2 1

#define PLAYER_SPEED 1.0
#define BALL_SPEED 0.7

#define ABSCISSA 0
#define ORDINATE 1

#define PAUSE_TEXT "Pause"

#define clamp(value, min, max) ((value) < (min) ? (min) : ((value) > (max) ? (max) : (value)))

#define contains(min, max, value) ((value) >= (min) & (value) <= (max))

struct pong_args {
	const char *display;
	const char *fontname;
	struct timespec frame_duration;
	int64_t seed;
};

struct pong {
	xcb_connection_t *connection;
	xcb_get_keyboard_mapping_reply_t *keyboard_mapping;
	xcb_query_font_reply_t *font_extents;
	xcb_gcontext_t graphic_context;
	xcb_window_t window;
	xcb_atom_t wm_delete_window;

	int16_t offset[2];
	uint16_t square;

	unsigned running : 1;
	unsigned paused : 1;
	unsigned players_score[2];
	double players_position[2];
	double players_speed[2];
	double ball_position[2];
	double ball_angle;
};

struct pong_key_event {
	xcb_keysym_t keysym;
	void (*handler)(struct pong *);
};

static double
pong_ball_angle(void) {
	return 2 * M_PI * random() / 2147483647.0;
}

static void
pong_reset(struct pong *pong) {
	pong->running = 1;
	pong->paused = 1;
	pong->players_score[PLAYER_1] = 0;
	pong->players_score[PLAYER_2] = 0;
	pong->players_position[PLAYER_1] = 0.0;
	pong->players_position[PLAYER_2] = 0.0;
	pong->players_speed[PLAYER_1] = 0.0;
	pong->players_speed[PLAYER_2] = 0.0;
	pong->ball_position[ABSCISSA] = 0.0;
	pong->ball_position[ORDINATE] = 0.0;
	pong->ball_angle = pong_ball_angle();
}

static void
pong_init(struct pong *pong, const struct pong_args *args) {

	/* Game variables */
	pong_reset(pong);

	/* Render variables */
	pong->offset[ABSCISSA] = pong->offset[ORDINATE] = 0;
	pong->square = 500;

	/* Connection */
	int screen_number;

	pong->connection = xcb_connect(args->display, &screen_number);

	if(xcb_connection_has_error(pong->connection) != 0) {
		errx(EXIT_FAILURE, "Unable to connect to X11 display");
	}

	/* Access screen */
	const xcb_setup_t *setup = xcb_get_setup(pong->connection);
	xcb_screen_iterator_t screen_iterator = xcb_setup_roots_iterator(setup);

	while(screen_number > 0) {
		xcb_screen_next(&screen_iterator);
		screen_number--;
	}

	xcb_screen_t *screen = screen_iterator.data;

	/* Create our graphic context */
	pong->graphic_context = xcb_generate_id(pong->connection);
	if(args->fontname != NULL) {
		xcb_font_t const font = xcb_generate_id(pong->connection);
		uint32_t graphic_context_value_list[] = {
			screen->white_pixel, font
		};

		xcb_open_font(pong->connection, font, strlen(args->fontname), args->fontname);

		xcb_create_gc(pong->connection, pong->graphic_context, screen->root,
			XCB_GC_FOREGROUND | XCB_GC_FONT, graphic_context_value_list);

		xcb_close_font(pong->connection, font);
	} else {
		xcb_create_gc(pong->connection, pong->graphic_context, screen->root,
			XCB_GC_FOREGROUND, &screen->white_pixel);
	}

	/* Create our window */
	const uint32_t window_value_list[] = {
		screen->black_pixel,
		XCB_EVENT_MASK_STRUCTURE_NOTIFY
		| XCB_EVENT_MASK_KEY_PRESS
		| XCB_EVENT_MASK_KEY_RELEASE,
	};
	pong->window = xcb_generate_id(pong->connection);
	xcb_create_window(pong->connection, XCB_COPY_FROM_PARENT,
		pong->window, screen->root,
		0, 0,
		pong->square + pong->offset[ABSCISSA],
		pong->square + pong->offset[ORDINATE],
		0, XCB_WINDOW_CLASS_COPY_FROM_PARENT,
		XCB_COPY_FROM_PARENT,
		XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK,
		window_value_list);

	/* Asynchronous requests */

	/* - WM_PROTOCOLS atom cookie */
	xcb_intern_atom_cookie_t wm_protocols_atom_cookie
		= xcb_intern_atom(pong->connection, 1, 12, "WM_PROTOCOLS");

	/* - WM_DELETE_WINDOW atom cookie */
	xcb_intern_atom_cookie_t wm_delete_window_atom_cookie
		= xcb_intern_atom(pong->connection, 0, 16, "WM_DELETE_WINDOW");

	/* - query font cookie */
	xcb_query_font_cookie_t query_font_cookie
		= xcb_query_font(pong->connection, pong->graphic_context);

	/* - keycode to keysyms mapping cookie */
	xcb_get_keyboard_mapping_cookie_t get_keyboard_mapping_cookie
		= xcb_get_keyboard_mapping(pong->connection, setup->min_keycode,
			setup->max_keycode - setup->min_keycode + 1);

	/* Asynchronous replies. */

	/* - WM_PROTOCOLS atom reply */
	xcb_intern_atom_reply_t *wm_protocols_atom_reply
		= xcb_intern_atom_reply(pong->connection, wm_protocols_atom_cookie, NULL);
	if(wm_protocols_atom_reply == NULL) {
		errx(EXIT_FAILURE, "Required WM_PROTOCOLS not available");
	}

	/* - WM_DELETE_WINDOW atom reply */
	xcb_intern_atom_reply_t *wm_delete_window_atom_reply
		= xcb_intern_atom_reply(pong->connection, wm_delete_window_atom_cookie, NULL);
	if(wm_delete_window_atom_reply == NULL) {
		errx(EXIT_FAILURE, "Required WM_DELETE_WINDOW not available");
	}

	pong->wm_delete_window = wm_delete_window_atom_reply->atom;

	/* - query font reply */
	pong->font_extents = xcb_query_font_reply(pong->connection, query_font_cookie, NULL);
	if(pong->font_extents == NULL) {
		errx(EXIT_FAILURE, "Unable to query font extents");
	}

	/* - keycode to keysyms mapping reply */
	pong->keyboard_mapping = xcb_get_keyboard_mapping_reply(pong->connection, get_keyboard_mapping_cookie, NULL);
	if(pong->keyboard_mapping == NULL) {
		errx(EXIT_FAILURE, "Unable to get keyboard mapping");
	}

	/* Final setups before start */
	const uint32_t kb_value_list[] = { XCB_AUTO_REPEAT_MODE_OFF };

	xcb_change_keyboard_control(pong->connection, XCB_KB_AUTO_REPEAT_MODE, kb_value_list);

	xcb_change_property(pong->connection, XCB_PROP_MODE_REPLACE,
		pong->window, wm_protocols_atom_reply->atom,
		XCB_ATOM_ATOM, 32, 1, &pong->wm_delete_window);

	xcb_map_window(pong->connection, pong->window);

	xcb_flush(pong->connection); /* Could be removed as the first render pass would flush anyway */

	free(wm_protocols_atom_reply);
	free(wm_delete_window_atom_reply);
}

static void
pong_deinit(struct pong *pong) {
	const uint32_t kb_value_list[] = { XCB_AUTO_REPEAT_MODE_DEFAULT };

	xcb_change_keyboard_control(pong->connection, XCB_KB_AUTO_REPEAT_MODE, kb_value_list);
	xcb_disconnect(pong->connection);

	free(pong->keyboard_mapping);
	free(pong->font_extents);
}

static void
pong_pause(struct pong *pong) {
	pong->paused = !pong->paused;
}

static void
pong_quit(struct pong *pong) {
	pong->running = 0;
}

static void
pong_player1_up(struct pong *pong) {
	pong->players_speed[PLAYER_1] -= PLAYER_SPEED;
}

static void
pong_player1_down(struct pong *pong) {
	pong->players_speed[PLAYER_1] += PLAYER_SPEED;
}

static void
pong_player2_up(struct pong *pong) {
	pong->players_speed[PLAYER_2] -= PLAYER_SPEED;
}

static void
pong_player2_down(struct pong *pong) {
	pong->players_speed[PLAYER_2] += PLAYER_SPEED;
}

const struct pong_key_event key_presses[] = {
	{ KEYSYM_LATIN_1_P, pong_pause },
	{ KEYSYM_LATIN_1_Q, pong_quit },
	{ KEYSYM_LATIN_1_R, pong_reset },
	{ KEYSYM_LATIN_1_S, pong_player1_down },
	{ KEYSYM_LATIN_1_Z, pong_player1_up },
	{ KEYSYM_FUNCTION_UP, pong_player2_up },
	{ KEYSYM_FUNCTION_DOWN, pong_player2_down },
	{ KEYSYM_SPECIAL_NOSYMBOL, NULL },
};

const struct pong_key_event key_releases[] = {
	{ KEYSYM_LATIN_1_S, pong_player1_up },
	{ KEYSYM_LATIN_1_Z, pong_player1_down },
	{ KEYSYM_FUNCTION_UP, pong_player2_down },
	{ KEYSYM_FUNCTION_DOWN, pong_player2_up },
	{ KEYSYM_SPECIAL_NOSYMBOL, NULL },
};

static void
pong_events_key(struct pong *pong, const xcb_key_press_event_t *key_event, const struct pong_key_event *handlers) {
	xcb_get_keyboard_mapping_reply_t *keyboard_mapping = pong->keyboard_mapping;
	xcb_keysym_t *keysyms = xcb_get_keyboard_mapping_keysyms(keyboard_mapping) /* keysyms associated to our keycode */
		+ (key_event->detail - xcb_get_setup(pong->connection)->min_keycode) * keyboard_mapping->keysyms_per_keycode;

	while(handlers->keysym != KEYSYM_SPECIAL_NOSYMBOL) {
		int index = 0;

		while(index < keyboard_mapping->keysyms_per_keycode) {
			if(keysyms[index] == handlers->keysym) {
				handlers->handler(pong);
				return;
			}
			++index;
		}
		++handlers;
	}
}

static void
pong_events(struct pong *pong) {
	xcb_generic_event_t *event;

	while(event = xcb_poll_for_event(pong->connection), event != NULL) {
		uint8_t const response_type = event->response_type & ~0x80;

		if(response_type != 0) {
			switch(response_type) {
			case XCB_KEY_PRESS:
				pong_events_key(pong, (const xcb_key_press_event_t *)event, key_presses);
				break;
			case XCB_KEY_RELEASE:
				pong_events_key(pong, (const xcb_key_press_event_t *)event, key_releases);
				break;
			case XCB_DESTROY_NOTIFY:
				break;
			case XCB_UNMAP_NOTIFY:
				break;
			case XCB_MAP_NOTIFY:
				break;
			case XCB_REPARENT_NOTIFY:
				break;
			case XCB_CONFIGURE_NOTIFY: {
				const xcb_configure_notify_event_t *configure_notify_event
					= (const xcb_configure_notify_event_t *)event;
				if(configure_notify_event->width < configure_notify_event->height) {
					pong->square = configure_notify_event->width;
					pong->offset[1] = (configure_notify_event->height - pong->square) >> 1;
					pong->offset[0] = 0;
				} else {
					pong->square = configure_notify_event->height;
					pong->offset[0] = (configure_notify_event->width - pong->square) >> 1;
					pong->offset[1] = 0;
				}
			}	break;
			case XCB_GRAVITY_NOTIFY:
				break;
			case XCB_CIRCULATE_NOTIFY:
				break;
			case XCB_CLIENT_MESSAGE: {
				const xcb_client_message_event_t *client_message_event
					= (const xcb_client_message_event_t *)event;

				if(client_message_event->data.data32[0] == pong->wm_delete_window) {
					xcb_destroy_window(pong->connection, pong->window);
					pong_quit(pong);
				}
			}	break;
			}
		} else {
			warnx("Received X11 error");
		}

		free(event);
	}
}

static _Bool
pong_player_collides_ball(const struct pong *pong, int player) {
	static const struct {
		double min, max;
	} players_bound[] = {
		{ .min = -0.8, .max = -0.75 },
		{ .min = 0.7, .max = 0.75 },
	};

	return contains(players_bound[player].min, players_bound[player].max, pong->ball_position[ABSCISSA])
		& contains(pong->players_position[player] - 0.125, pong->players_position[player] + 0.125, pong->ball_position[ORDINATE])
		& ((cos(pong->ball_angle) < 0.0) ^ player);
		/* This last weird condition checks whether we already collided or not. Sometimes the ball was 'sliding' along the player,
			my guess is it teleported at the first frame, and changed angles until it slided out.
			So we check for the good angle too, seemed to have fixed it. */
}

static void
pong_player_scores(struct pong *pong, int player) {
	pong->players_score[player]++;

	pong->ball_position[ABSCISSA] = 0;
	pong->ball_position[ORDINATE] = 0;
	pong->ball_angle = pong_ball_angle();
}

static void
pong_physic(struct pong *pong, const struct timespec *duration) {
	double const elapsed = duration->tv_sec + duration->tv_nsec / 1000000000.0;

	pong->players_position[PLAYER_1] = clamp(pong->players_position[PLAYER_1] + pong->players_speed[PLAYER_1] * elapsed, -0.9, 0.9);
	pong->players_position[PLAYER_2] = clamp(pong->players_position[PLAYER_2] + pong->players_speed[PLAYER_2] * elapsed, -0.9, 0.9);

	pong->ball_position[ABSCISSA] += BALL_SPEED * elapsed * cos(pong->ball_angle);
	pong->ball_position[ORDINATE] += BALL_SPEED * elapsed * sin(pong->ball_angle);

	if(pong->ball_position[ORDINATE] <= -1.0 || pong->ball_position[ORDINATE] >= 0.95) {
		double const raw = asin(-sin(pong->ball_angle));
		pong->ball_angle = cos(pong->ball_angle) < 0.0 ? M_PI - raw : raw;
	}

	if(pong_player_collides_ball(pong, PLAYER_1) || pong_player_collides_ball(pong, PLAYER_2)) {
		double const raw = acos(-cos(pong->ball_angle));
		pong->ball_angle = sin(pong->ball_angle) < 0.0 ? 2 * M_PI - raw : raw;
	} else if(pong->ball_position[ABSCISSA] <= -1.0) {
		pong_player_scores(pong, PLAYER_2);
	} else if(pong->ball_position[ABSCISSA] >= 0.95) {
		pong_player_scores(pong, PLAYER_1);
	}
}

static int16_t
pong_player_position(const struct pong *pong, int player) {
	return pong->offset[ORDINATE] + ((pong->players_position[player] + 1.0) / 2.0) * pong->square;
}

static int16_t
pong_ball_position(const struct pong *pong, int coordinate) {
	return pong->offset[coordinate] + ((pong->ball_position[coordinate] + 1.0) / 2.0) * pong->square;
}

static int
pong_format_score(const struct pong *pong, char *buffer, size_t size) {
	static const char score_format[] = "%d : %d";
	return snprintf(buffer, size, score_format, pong->players_score[PLAYER_1], pong->players_score[PLAYER_2]);
}

static void
pong_render(struct pong *pong) {
	uint16_t const players_width = 0.025 * pong->square, players_height = 0.1 * pong->square, ball_size = 0.025 * pong->square;
	xcb_rectangle_t const rectangles[] = {
		{ pong->offset[ABSCISSA] + 0.100 * pong->square, pong_player_position(pong, PLAYER_1) - players_height / 2, players_width, players_height },
		{ pong->offset[ABSCISSA] + 0.875 * pong->square, pong_player_position(pong, PLAYER_2) - players_height / 2, players_width, players_height },
		{ pong_ball_position(pong, ABSCISSA), pong_ball_position(pong, ORDINATE), ball_size, ball_size }
	};

	xcb_clear_area(pong->connection, 0, pong->window, 0, 0, 0, 0);

	xcb_poly_fill_rectangle(pong->connection, pong->window, pong->graphic_context,
		sizeof(rectangles) / sizeof(*rectangles), rectangles);

	if(pong->square > 250) {
		const xcb_query_font_reply_t *font_extents = pong->font_extents;
		int const length = pong_format_score(pong, NULL, 0);

		if(length > 0) {
			uint16_t const approximate_width = length * font_extents->max_bounds.character_width;
			char buffer[length + 1];
			pong_format_score(pong, buffer, sizeof(buffer));
			xcb_image_text_8(pong->connection, length, pong->window, pong->graphic_context,
				pong->offset[ABSCISSA] + (pong->square - approximate_width) / 2,
				pong->offset[ORDINATE] + (0.1 * pong->square - font_extents->max_bounds.ascent), buffer);
		}

		if(pong->paused != 0) {
			uint16_t const approximate_width = (sizeof(PAUSE_TEXT) - 1) * font_extents->max_bounds.character_width;
			xcb_image_text_8(pong->connection, sizeof(PAUSE_TEXT) - 1, pong->window, pong->graphic_context,
				pong->offset[ABSCISSA] + (pong->square - approximate_width) / 2,
				pong->offset[ORDINATE] + (0.9 * pong->square + font_extents->max_bounds.descent), PAUSE_TEXT);
		}
	}

	xcb_flush(pong->connection);
}

static void
pong_run(struct pong *pong, const struct pong_args *args) {
	while(pong->running != 0) {
		struct timespec duration = args->frame_duration, remainder;

		pong_events(pong);
		pong_render(pong);
		if(pong->paused == 0) {
			pong_physic(pong, &duration);
		}

		while(nanosleep(&duration, &remainder) != 0) {
			duration = remainder;
		}
	}
}

static void
pong_usage(const char *pong_name) {
	fprintf(stderr, "usage: %s [-D display] [-F fontname] [-f fps] [-S seed]\n", pong_name);
	exit(EXIT_FAILURE);
}

static struct pong_args
pong_parse_args(int argc, char **argv) {
	struct pong_args args = {
		.display = NULL,
		.fontname = NULL,
		.frame_duration = {
			.tv_sec = 0,
			.tv_nsec = 16666666 
		},
		.seed = 0, /* Well, zero is as good as any others */
	};
	int c;

	while((c = getopt(argc, argv, "D:F:f:S:")) != -1) {
		switch(c) {
		case 'D':
			args.display = optarg;
			break;
		case 'F':
			args.fontname = optarg;
			break;
		case 'f': {
			char *endptr;
			unsigned long frequency = strtoul(optarg, &endptr, 10);
			if(*endptr != '\0' || frequency == 0) {
				errx(EXIT_FAILURE, "Invalid frequency: %s", optarg);
			}
			double const duration = 1.0 / frequency;
			struct timespec frame_duration;
			frame_duration.tv_sec = duration;
			frame_duration.tv_nsec = duration * 1000000000.0 - frame_duration.tv_sec;

			if(frame_duration.tv_sec == 0 && frame_duration.tv_nsec == 0) {
				errx(EXIT_FAILURE, "Invalid frequency: %lu", frequency);
			}

			args.frame_duration = frame_duration;
		}	break;
		case 'S': {
			char *endptr;
			unsigned long seed = strtoul(optarg, &endptr, 16);
			if(*endptr != '\0' || seed == 0 || seed > (unsigned)-1) {
				errx(EXIT_FAILURE, "Invalid seed (expects non-zero raw hexadecimal): %s", optarg);
			}
			args.seed = seed;
		}	break;
		default:
			pong_usage(*argv);
			break;
		}
	}

	if(argc - optind != 0) {
		pong_usage(*argv);
	}

	return args;
}

int
main(int argc, char **argv) {
	const struct pong_args args = pong_parse_args(argc, argv);
	struct pong pong;

	srandom(args.seed);

	pong_init(&pong, &args);

	pong_run(&pong, &args);

	pong_deinit(&pong);

	return EXIT_SUCCESS;
}

