#include <stdio.h>
#include <emscripten.h>

#include "network.h"
#include "chess.h"
#include "chessfrontend.h"

#include "imwasm.h"

#define NUM_BOARDS 7
char* boards[NUM_BOARDS*2] = {
#include "../include/default.board"
		,
#include "../include/doubleking.board"
		,
#include "../include/fourplayer.board"
		,
#include "../include/twovone.board"
		,
#include "../include/capablanca.board"
		,
#include "../include/heirchess.board"
		,
#include "../include/ultimate.board"
};

#define DEFAULT_SERVADDR "wss://esochess.net"

typedef struct {
	html_ui_t ui;

	char* err;
	char* mp_name;
	char* gname;
	chess_client_t client;
	enum {
		menu_main,
		menu_connect,
		menu_chooseplayer,
		menu_makegame
	} menustate;

	int menu_customboard;
	int menu_multiplayer;
	int which;

	//apparently we need two booleans to determine whether the player has been checkmated and when to disable it
	int mate_change;
	int check_displayed;
} chess_web_t;

chess_web_t g_web;

typedef enum {
	a_multiplayer,
	a_singleplayer,
	a_netmsg,
	a_disconnected,
	a_connect,
	a_makegame_menu,
	a_chooseplayer,
	a_makegame,
	a_boardchange,
	a_setmovecursor,
	a_setmovecursor_now,
	a_joingame,
	a_select,
	a_dropmove,
	a_dragmove,
	a_doai,
	a_checkdisplayed,
	a_undo_move,
	a_back
} action_t;

void handler(void* arg, cur_t cur) {
	chess_web_t* web = arg;

	if (cur.start==NULL) {
		printf("disconnect\n");
		html_send(&web->ui, a_disconnected, NULL);
	} else {
		mp_serv_t msg = chess_client_recvmsg(&web->client, cur);
		printf("recv %u\n", msg);
		html_send(&web->ui, a_netmsg, (void*)msg);
	}
}

void setup_game(chess_web_t* web) {
	web->which=0;
	web->client.select.from[0] = -1;
	web->check_displayed = 0;
	web->mate_change=1;
}

void clearhints(chess_web_t* web) {
	vector_clear(&web->client.hints);
	web->client.select.from[0]=-1;
	web->which=0;
}

void web_moved(html_ui_t* ui, chess_web_t* web, int moved) { //move fx
	player_t* t = vector_get(&web->client.g.players, web->client.player);
	if (!t->mate) {
		web->mate_change=1;
		web->check_displayed=0;
	} else if (web->mate_change) {
		web->mate_change=0;
		web->check_displayed=0;
	}

	if (t->check && !web->check_displayed) {
		html_settimeout(ui, 1000, a_checkdisplayed, NULL);
		if (moved) html_playsound(ui, "img/check.ogg", 0.2);
	} else if (moved) {
		html_playsound(ui, "img/move.ogg", 0.2);
	}
}

void web_move(html_ui_t* ui, chess_web_t* web) { //wrapper level=3
	if (client_make_move(&web->client)) {
		if (web->client.g.m.host == web->client.pnum) {
			html_defer(ui, a_doai, NULL);
		}

		web_moved(ui, web, 1);
	}
}

void update(html_ui_t* ui, html_event_t* ev, chess_web_t* web) {
	web->err=NULL;

	switch (ev->action) {
		case a_netmsg: {
			mp_serv_t msg = *(mp_serv_t*)&ev->custom_data;
			switch (msg) {
				case mp_move_made: {
					if (web->client.g.m.host == web->client.pnum) {
						html_defer(ui, a_doai, NULL);
					}

					web_moved(ui, web, 1);
					break;
				}
				default:;
			}

			break;
		}
		case a_back: {
			switch (web->client.mode) {
				case mode_menu: switch (web->menustate) { //triple-switch
					case menu_makegame: {
						if (web->menu_multiplayer) web->client.mode = mode_gamelist;
						else web->menustate = menu_main;
						break;
					}
					case menu_chooseplayer: {
						game_free(&web->client.g);
						if (web->menu_multiplayer) drop(web->gname);
						web->menustate = menu_makegame;
						break;
					}
					case menu_connect: {
						web->menustate = menu_main; break;
					}
					default:;
				} break;
				case mode_gamelist: {
					chess_client_disconnect(&web->client);
					if (web->mp_name) drop(web->mp_name);
					web->client.mode = mode_menu;
					web->menustate = menu_main;
					break;
				}
				case mode_singleplayer: {
					chess_client_leavegame(&web->client);

					web->client.mode = mode_menu;
					web->menustate = menu_main;
					break;
				}
				case mode_multiplayer: {
					chess_client_leavegame(&web->client);
					chess_client_gamelist(&web->client);
					web->client.mode = mode_gamelist;
					break;
				}
			}

			break;
		}
		case a_multiplayer: {
			web->menustate = menu_connect; break;
		}
		case a_singleplayer: {
			web->menu_multiplayer=0;
			web->menustate = menu_makegame; break;
		}
		case a_connect: {
			char* addr = html_input_value("addr");
			if (strlen(addr)==0) {
				web->err = "address is empty";
				drop(addr);
				return;
			}

			html_local_set("addr", addr);

			web->mp_name = html_input_value("name");
			unsigned len = strlen(web->mp_name);
			if (len!=0) {
				if (len>PLAYERNAME_MAXLEN) {
					drop(web->mp_name);
					web->err = "that name is distastefully tedious. shorten your words, or forget them";
					break;
				}

				html_local_set("name", web->mp_name);
			} else {
				drop(web->mp_name);
				web->mp_name = NULL;
			}

			web->client.net = client_connect(addr, MP_PORT, handler, web);
			drop(addr);

			if (web->client.net->err) {
				web->err = heapstr("failed to connect; check address. err: %s", strerror(web->client.net->err));
				if (web->mp_name) drop(web->mp_name); break;
			}

			chess_client_gamelist(&web->client);

			web->client.mode = mode_gamelist;
			break;
		}
		case a_disconnected: {
			web->err = "disconnected from server";
			break;
		}
		case a_makegame_menu: {
			web->client.mode = mode_menu;
			web->menu_multiplayer=1;
			web->menustate = menu_makegame;
			break;
		}
		case a_boardchange: break;
		case a_chooseplayer: {
			if (web->menu_multiplayer) {
				web->gname = html_input_value("gname");
				unsigned len = strlen(web->gname);
				if (len==0) {
					web->err="game name is empty; if you are not cognizant enough to fill it out, perhaps you are unfit for this game of chess";
					drop(web->gname); break;
				} else if (len>GAMENAME_MAXLEN) {
					web->err = "that game name is too lame. perform a concision.";
					drop(web->gname); break;
				}
			}

			game_flags_t flags = html_checked("winbypieces") ? game_win_by_pieces : 0;

			char* b_i = html_input_value("boards");
			if (streq(b_i, "custom")) {
				char* b = html_input_value("customboard");
				web->client.g = parse_board(b, flags);
				drop(b);
			} else {
				int i = atoi(b_i);
				web->client.g = parse_board(boards[i*2+1], flags);
			}

			drop(b_i);

			vector_t pfrom = html_checkboxes_checked("promote", 1);
			vector_t castleable = html_checkboxes_checked("castleable", 1);

			vector_iterator promote_iter = vector_iterate(&pfrom);
			while (vector_next(&promote_iter)) {
				char ty = (char)strsstr(PIECE_STR, p_empty, *(char**)promote_iter.x);
				vector_pushcpy(&web->client.g.promote_from, &ty);
			}

			char* promoto = html_input_value("promoteto");
			web->client.g.promote_to = (piece_ty)strsstr(PIECE_STR, p_empty, promoto);
			drop(promoto);

			vector_iterator castle_iter = vector_iterate(&castleable);
			while (vector_next(&castle_iter)) {
				char ty = (char)strsstr(PIECE_STR, p_empty, *(char**)castle_iter.x);
				vector_pushcpy(&web->client.g.castleable, &ty);
			}

			vector_free_strings(&pfrom);
			vector_free_strings(&castleable);

			web->menustate = menu_chooseplayer;
			break;
		}
		case a_makegame: {
			char* p_i = html_radio_value("chosenplayer");
			char p = (char)atoi(p_i);
			drop(p_i);

			web->client.player = (char)p;

			vector_t ais = html_checkboxes_checked("isai", 0);
			vector_iterator ai_iter = vector_iterate(&ais);

			char ai_i=-1;
			while (vector_next(&ai_iter)) {
				ai_i = (char)atoi(*(char**)ai_iter.x);
				if (ai_i==web->client.player) break; //passthrough

				player_t* ai = vector_get(&web->client.g.players, ai_i);
				ai->ai=1;
				ai->joined=1;
			}

			vector_free_strings(&ais);

			if (ai_i==web->client.player) {
				web->err = "you arent an ai, fool";
				break;
			}

			chess_client_initgame(&web->client, web->menu_multiplayer?mode_multiplayer:mode_singleplayer, 1);
			if (web->menu_multiplayer) {
				chess_client_makegame(&web->client, web->gname, web->mp_name);
				drop(web->gname);
			}

			setup_game(web);
			break;
		}
		case a_joingame: {
			if (!chess_client_joingame(&web->client, ev->elem->i, web->mp_name)) {
				web->err = "that game is full, take another gamble lmao";
				break;
			}

			setup_game(web);

			break;
		}
		case a_select: {
			player_t* t = vector_get(&web->client.g.players, web->client.player);

			int select[2] = {(int)ev->elem->i, (int)ev->elem->parent->i};
			board_rot_pos(&web->client.g, t->board_rot, select, web->which==0?web->client.select.from:web->client.select.to);

			if (web->which==1) {
				web_move(ui, web);
				clearhints(web);
			} else {
				refresh_hints(&web->client);
				web->which=1;
			}

			break;
		}
		case a_dragmove: {
			player_t* t = vector_get(&web->client.g.players, web->client.player);
			int select[2] = {(int)ev->elem->i, (int)ev->elem->parent->i};
			board_rot_pos(&web->client.g, t->board_rot, select, web->client.select.from);

			refresh_hints(&web->client);
			break;
		}
		case a_dropmove: {
			player_t* t = vector_get(&web->client.g.players, web->client.player);
			int select[2] = {(int)ev->elem->i, (int)ev->elem->parent->i};
			board_rot_pos(&web->client.g, t->board_rot, select, web->client.select.to);

			web_move(ui, web);
			web->which=0;

			break;
		}
		case a_doai: {
			int moved = chess_client_ai(&web->client);
			refresh_hints(&web->client);
			web_moved(ui, web, moved);

			break;
		}
		case a_checkdisplayed: {
			web->check_displayed=1;
			break;
		}
		case a_setmovecursor: {
			chess_client_set_move_cursor(&web->client, ev->elem->i);
			break;
		}
		case a_setmovecursor_now: {
			chess_client_set_move_cursor(&web->client, web->client.g.moves.length);
			break;
		}
		case a_undo_move: {
			chess_client_undo_move(&web->client);
			clearhints(web);

			web->check_displayed=0;

			break;
		}
	}

	if (web->client.net && web->client.net->err) {
		web->err = heapstr("network error; check address. err: %s", strerror(web->client.net->err));
	}
}

void piece_list(html_ui_t* ui, char* id, int multiple, piece_ty selected) {
	html_start_select(ui, id, multiple);
	for (piece_ty ty=0; ty<p_empty; ty++) {
		html_option(ui, PIECE_NAME[ty], PIECE_STR[ty], ty==selected);
	}
	html_end(ui);
}

void render(html_ui_t* ui, chess_web_t* web) {
	if (web->err) {
		html_p(ui, "err", web->err);
	}

	if (web->client.mode!=mode_menu || web->menustate!=menu_main) {
		html_elem_t* back = html_button(ui, "back", "back");
		html_event(ui, back, html_click, a_back);
	}

	switch (web->client.mode) {
		case mode_menu: switch (web->menustate) {
			case menu_main: {
				html_h(ui, 1, "title", "esochess");
				html_p(ui, "p1", "welcome to chess. images by wikimedians Cburnett and spinningspark.");
				html_p(ui, "p2", " also epilepsy warning, some sequences may flash and desist from using a mobile device to access this site; now consider the following modes:");

				html_start_div(ui, "b", 0);

				html_elem_t* s = html_button(ui, "single", "singleplayer");
				html_event(ui, s, html_click, a_singleplayer);
				html_elem_t* m = html_button(ui, "multi", "multiplayer");
				html_event(ui, m, html_click, a_multiplayer);
				html_a(ui, "git", "github", "https://github.com/super-cilious/termchess");

				html_end(ui);
				break;
			}
			case menu_connect: {
				html_p(ui, "p1", "server address:");

				char* localaddr = html_local_get("addr");
				if (localaddr==NULL) localaddr=DEFAULT_SERVADDR;
				char* localname = html_local_get("name");

				html_input(ui, "addr", localaddr);

				html_p(ui, "p2", "decide on an alias, or leave blank for default");
				html_input(ui, "name", localname);

				drop(localaddr);
				drop(localname);

				html_elem_t* c = html_button(ui, "connect", "connect");
				html_event(ui, c, html_click, a_connect);
				break;
			}
			case menu_makegame: {
				if (web->menu_multiplayer) {
					html_p(ui, "p1", "game name:");
					html_input(ui, "gname", "");
				}

				html_p(ui, "gtype", "game type:");

				html_elem_t* s = html_start_select(ui, "boards", 0);
				for (int i=0; i<NUM_BOARDS; i++) {
					char* istr = heapstr("%i", i);
					html_elem_t* e = html_option(ui, boards[i*2], istr, 0);
					drop(istr);
				}

				html_option(ui, "custom", NULL, 0);

				html_end(ui);
				html_event(ui, s, html_onchange, a_boardchange);

				char* b_i = html_input_value("boards");
				if (streq(b_i, "custom")) {
					html_textarea(ui, "customboard", boards[1]);
					html_elem_t* a = html_a(ui, "boardhelp", "board format", "/boardformat.html");
					html_set_attr(a, html_attrib, "target", "_blank");
				}

				drop(b_i);

				html_start_div(ui, "opts", 0);

				html_label(ui, "promo-label", "promote from:");
				piece_list(ui, "promote", 1, p_pawn);
				html_label(ui, "promo-to-label", "to");
				piece_list(ui, "promoteto", 0, p_queen);

				html_br(ui);

				html_label(ui, "castleable-label", "pieces that can castle with king:");
				piece_list(ui, "castleable", 1, p_rook);

				html_br(ui);
				html_label(ui, "winbypieces-label", "win by pieces?");
				html_checkbox(ui, "winbypieces", NULL, NULL, 0);

				html_end(ui);

				html_elem_t* e = html_button(ui, "next", "next");
				html_event(ui, e, html_click, a_chooseplayer);
				break;
			}
			case menu_chooseplayer: {
				html_p(ui, "choose", "now devise your character:");

				html_start_table(ui, "players");
				html_start_tr(ui);

				html_td(ui, "ai");
				html_td(ui, "you");
				html_td(ui, "player name");

				html_end(ui);

				vector_iterator p_iter = vector_iterate(&web->client.g.players);
				while (vector_next(&p_iter)) {
					player_t* p = p_iter.x;

					html_start_tr(ui);
					char* istr = heapstr("%i", p_iter.i);

					html_start_td(ui);
					html_checkbox(ui, NULL, "isai", istr, p_iter.i!=0);
					html_end(ui);

					html_start_td(ui);
					html_radio(ui, NULL, "chosenplayer", istr, p_iter.i==0);
					html_end(ui);

					html_td(ui, p->name);

					drop(istr);
					html_end(ui);
				}

				html_end(ui);

				html_elem_t* b = html_button(ui, "make", "make thy gracious game");
				html_event(ui, b, html_click, a_makegame);

				break;
			}
		} break;
		case mode_gamelist: {
			html_p(ui, "p1", "henceforth, a new match may be made or picked from the following pool");

			html_start_div(ui, "games", 1);
			vector_iterator gl_iter = vector_iterate(&web->client.game_list);
			while (vector_next(&gl_iter)) {
				game_listing_t* gl = gl_iter.x;
				html_elem_t* b = html_button(ui, NULL, gl->name);
				if (gl->full) html_set_attr(b, html_class, NULL, "full");
				html_event(ui, b, html_click, a_joingame);
			}

			html_end(ui);

			html_elem_t* e = html_button(ui, "make", "make game");
			html_event(ui, e, html_click, a_makegame_menu);
			break;
		}
		case mode_multiplayer:
		case mode_singleplayer: {
			player_t* t = vector_get(&web->client.g.players, web->client.player);

			if (web->client.mode == mode_multiplayer) {
				if (web->client.spectating) {
					html_p(ui, "spectating", "(spectating)");
				}

				html_start_div(ui, "spectators", 1);
				vector_iterator spec_iter = vector_iterate(&web->client.g.m.spectators);
				while (vector_next(&spec_iter)) {
					char* name = *(char**)spec_iter.x;
					if (*name==0) html_span(ui, NULL, "anon");
					else html_span(ui, NULL, name);
					if (spec_iter.i!=web->client.g.m.spectators.length-1) {
						html_span(ui, NULL, ", ");
					}
				}

				if (web->client.g.m.spectators.length==1) {
					html_span(ui, NULL, " is spectating");
				} else if (web->client.g.m.spectators.length>1) {
					html_span(ui, NULL, " are spectating");
				}

				html_end(ui);
			}

			html_start_div(ui, "wrapper", 1);

			html_elem_t* rot_divs[4] = {html_div(ui, NULL), html_div(ui, NULL), html_div(ui, NULL), html_div(ui, NULL)};
			for (char i=0; i<4; i++) html_set_attr(rot_divs[i], html_class, NULL, "player");
			html_set_attr(rot_divs[0], html_class, NULL, "bottom");
			html_set_attr(rot_divs[1], html_class, NULL, "right");
			html_set_attr(rot_divs[2], html_class, NULL, "up");
			html_set_attr(rot_divs[3], html_class, NULL, "left");

			vector_iterator t_iter = vector_iterate(&web->client.g.players);
			while (vector_next(&t_iter)) {
				player_t* p = t_iter.x;
				if (web->client.mode==mode_multiplayer && !p->joined) continue;

				int rel_rot = p->board_rot-t->board_rot;
				if (rel_rot<0) rel_rot=4+rel_rot;
				rel_rot %= 4;

				html_start(ui, rot_divs[rel_rot], 1);

				char* name = p->name;
				if (web->client.g.player==t_iter.i)
					name = heapstr(web->client.g.won ? "👑 %s" : "%s's turn", p->name);
				html_p(ui, NULL, name);

				html_end(ui);
			}

			html_start_div(ui, "info", 0);
			html_start_div(ui, "moves", 1);
			vector_iterator move_iter = vector_iterate(&web->client.g.moves);
			while (vector_next(&move_iter)) {
				char* pgn = move_pgn(&web->client.g, move_iter.x);
				html_elem_t* p = html_p(ui, NULL, pgn);
				html_event(ui, p, html_click, a_setmovecursor);

				if (move_iter.i == web->client.move_cursor) {
					html_set_attr(p, html_class, NULL, "current");
				}

				drop(pgn);
			}

			html_end(ui);

			html_event(ui, html_button(ui, "now", "now"), html_click, a_setmovecursor_now);
			if ((web->client.mode==mode_singleplayer && web->client.g.last_player!=-1)
						|| web->client.g.last_player==web->client.player)
				html_event(ui, html_button(ui, "undo", "undo"), html_click, a_undo_move);

			html_end(ui);

			move_t* last_m = vector_get(&web->client.g.moves, web->client.g.moves.length-1);

			html_start_table(ui, "board");
			int pos[2] = {0};
			for (; pos[1]<(t->board_rot%2==1?web->client.g.board_w:web->client.g.board_h); pos[1]++) {
				html_start_tr(ui);

				for (pos[0]=0; pos[0]<(t->board_rot%2==1?web->client.g.board_h:web->client.g.board_w); pos[0]++) {
					int bpos[2];
					board_rot_pos(&web->client.g, t->board_rot, pos, bpos);
					piece_t* p = board_get(&web->client.g, bpos);

					html_elem_t* td = html_start_td(ui);

					int col = p->player%4;
					static char* bpieces[] = {"img/king.svg","img/queen.svg","img/rook.svg","img/bishop.svg","img/knight.svg","img/pawn.svg", "img/archibishop.svg", "img/chancellor.svg", "img/heir.svg"};
					static char* wpieces[] = {"img/wking.svg","img/wqueen.svg","img/wrook.svg","img/wbishop.svg","img/wknight.svg","img/wpawn.svg", "img/warchibishop.svg", "img/wchancellor.svg", "img/wheir.svg"};
					static char* rpieces[] = {"img/rking.svg","img/rqueen.svg","img/rrook.svg","img/rbishop.svg","img/rknight.svg","img/rpawn.svg", "img/rarchibishop.svg", "img/rchancellor.svg", "img/rheir.svg"};
					static char* gpieces[] = {"img/gking.svg","img/gqueen.svg","img/grook.svg","img/gbishop.svg","img/gknight.svg","img/gpawn.svg", "img/garchibishop.svg", "img/gchancellor.svg", "img/gheir.svg"};

					html_elem_t* img;
					switch (p->ty) {
						case p_empty:break;
						case p_blocked:img=html_img(ui, NULL, "img/blocked.svg");break;
						default: {
							if (col==0) img=html_img(ui, NULL, wpieces[p->ty]);
							else if (col==1) img=html_img(ui, NULL, bpieces[p->ty]);
							else if (col==2) img=html_img(ui, NULL, rpieces[p->ty]);
							else if (col==3) img=html_img(ui, NULL, gpieces[p->ty]);
						}
					}

					if (p->ty!=p_empty && p->ty!=p_blocked) {
						char* class;
						switch (col) {
							case 0: class="white"; break; case 1: class="black"; break; case 2: class="red"; break; case 3: class="green"; break;
						}

						html_set_attr(td, html_class, NULL, class);
						html_set_attr(img, html_draggable, NULL, NULL);
						html_event(ui, td, html_drag, a_dragmove);

						//pawns are unidirectional, so rotation is helpful
						if (p->ty==p_pawn) {
							int prot = pawn_rot(p->flags);
							int rel_rot = prot/2 - t->board_rot;
							if (rel_rot!=0 || prot%2==1) {
								if (rel_rot<0) rel_rot=4+rel_rot;
								rel_rot %= 4;

								rel_rot*=2;

								if (t->board_rot>0 && t->board_rot<3) rel_rot-=prot%2;
								else rel_rot+=prot%2;

								char* style = heapstr("transform: rotate(-%ideg);", rel_rot*45);
								html_set_attr(img, html_attrib, "style", style);
							}
						}
					}

					html_set_attr(td, html_class, NULL, (bpos[0]+bpos[1])%2 ? "dark" : "light");

					if (i2eq(bpos, web->client.select.from)) {
						html_set_attr(td, html_class, NULL, "selected");
					} else if (client_hint_search(&web->client, bpos)!=NULL) {
						html_set_attr(td, html_class, NULL, "hint");
					} else if (last_m && i2eq(last_m->from, bpos)) {
						html_set_attr(td, html_class, NULL, "from");
					} else if (last_m && i2eq(last_m->to, bpos)) {
						html_set_attr(td, html_class, NULL, "to");
					}

					html_event(ui, td, html_click, a_select);
					html_event(ui, td, html_drop, a_dropmove);

					html_end(ui);
				}

				html_end(ui);
			}

			html_end(ui); //table
			html_end(ui); //wrapper

			if (!web->check_displayed && t->check) {
				html_start_div(ui, "flash", 0);
				html_p(ui, "flashtxt", t->mate ? "CHECKMATE!" : "CHECK!");
				html_end(ui);
			}

			break;
		}
	}
}

int main() {
	g_web.ui = html_ui_new();

	g_web.client.mode = mode_menu;
	g_web.menustate = menu_main;
	g_web.menu_customboard=0;

	g_web.err = NULL;
	g_web.client.net = NULL;

	html_run(&g_web.ui, (update_t)update, (render_t)render, &g_web);
}