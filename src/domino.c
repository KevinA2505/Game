// domino.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <stdbool.h>
#include <ctype.h>

#define MAX_PLAYERS 4
#define MAX_TILES   28
#define Q_DEFAULT_MS 50

typedef enum { FCFS, SJF_PLAYERS, SJF_POINTS, RR } policy_t;
typedef enum { NEW, READY, RUNNING, IO_WAIT, TERMINATED } pstate_t;

typedef struct { int a, b; } tile_t;

typedef struct {
    int pid;
    pstate_t st;
    policy_t pol;
    long arrival_ms, first_run_ms, finish_ms;
    int cpu_bursts;
    pthread_mutex_t mtx;
    pthread_cond_t  run_cv;
    int can_run;
} pcb_t;

typedef struct {
    int player_id, table_id;
    tile_t t;
    int side; // -1 izq, +1 der
} move_t;

typedef struct table_runtime_t table_runtime_t;

typedef struct {
    // extremos, tren, manos, pozo...
    tile_t train[64]; int train_len;
    int left_end, right_end;
    tile_t hands[MAX_PLAYERS][14]; int hand_len[MAX_PLAYERS];
    tile_t pool[28]; int pool_len;
    int turn, table_id, finished;
    int player_count;
    int human_player;
    int winner;
    int blocked;
    int passes_in_row;
    move_t history[256]; int history_len;
    pthread_mutex_t mtx; // sección crítica del estado
} game_state_t;

/* ===== Prototipos ===== */
// Utilidades
static void shuffle(tile_t *v, int n);
static void build_shuffled_deck(tile_t *deck, int *out_len);
static void setup_game_state(game_state_t *g, int table_id, int player_count, int human_player);
static int  can_play(const game_state_t *g, int pid, tile_t *out, int *side);
static int  draw_from_pool(game_state_t *g, int pid);
// Cola de movimientos (mutex + cond)
static void moveq_init(void);
static void moveq_push(const move_t *m);
static int  moveq_pop_for_table(int table_id, move_t *out);
// Validación (HVU)
static void *validator_thread(void *arg);
// Planificador (HPCS)
static void *scheduler_thread(void *arg);
// Mesa (HJ)
static void *table_thread(void *arg);
// Jugador
static void *player_thread(void *arg);
static void print_table_state(table_runtime_t *table, int force);
static void *reporter_thread(void *arg);
// Ayudas
static long now_ms(void);
static void msleep(int ms);

/* ===== Cola de movimientos ===== */
static pthread_mutex_t q_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  q_cv  = PTHREAD_COND_INITIALIZER;
static move_t qbuf[256]; static int qh=0, qt=0, qn=0;
static pthread_mutex_t io_mtx = PTHREAD_MUTEX_INITIALIZER;

static void moveq_init(void){ qh=qt=qn=0; }
static void moveq_push(const move_t *m){
    pthread_mutex_lock(&q_mtx);
    qbuf[qt] = *m; qt = (qt+1)%256; qn++;
    pthread_cond_broadcast(&q_cv);
    pthread_mutex_unlock(&q_mtx);
}
static int moveq_pop_for_table(int table_id, move_t *out){
    pthread_mutex_lock(&q_mtx);
    for(;;){
        while(qn==0) pthread_cond_wait(&q_cv, &q_mtx);
        int found = -1;
        for(int i=0;i<qn;i++){
            int pos = (qh + i) % 256;
            if(qbuf[pos].table_id == table_id){
                found = pos;
                break;
            }
        }
        if(found >= 0){
            *out = qbuf[found];
            int last_pos = (qh + qn - 1) % 256;
            while(found != last_pos){
                int next = (found + 1) % 256;
                qbuf[found] = qbuf[next];
                found = next;
            }
            qt = (qt + 255) % 256;
            qn--;
            pthread_mutex_unlock(&q_mtx);
            return 1;
        }
        pthread_cond_wait(&q_cv, &q_mtx);
    }
    pthread_mutex_unlock(&q_mtx);
    return 0;
}

/* ===== Validator (HVU) ===== */
static int compute_hand_points(const game_state_t *g, int pid){
    int sum = 0;
    for(int i=0;i<g->hand_len[pid];++i){
        tile_t t = g->hands[pid][i];
        sum += t.a + t.b;
    }
    return sum;
}

static int next_active_player(const game_state_t *g, int current){
    if(g->player_count <= 0) return -1;
    for(int i=1;i<=g->player_count;i++){
        int idx = (current + i) % g->player_count;
        if(g->hand_len[idx] >= 0){
            return idx;
        }
    }
    return current;
}

static void append_history(game_state_t *g, const move_t *mv){
    if(g->history_len >= 256) return;
    g->history[g->history_len++] = *mv;
}

static void finish_round(game_state_t *g, int winner, int blocked){
    g->finished = 1;
    g->winner = winner;
    g->blocked = blocked;
    g->turn = -1;
}

static void *validator_thread(void *arg){
    game_state_t *g = (game_state_t*)arg;
    while(!g->finished){
        move_t mv; moveq_pop_for_table(g->table_id, &mv);
        pthread_mutex_lock(&g->mtx);
        if(g->finished){
            pthread_mutex_unlock(&g->mtx);
            break;
        }

        if(mv.player_id < 0 || mv.player_id >= g->player_count){
            pthread_mutex_unlock(&g->mtx);
            continue;
        }

        int pid = mv.player_id;

        if(mv.side == 0){
            move_t logged = mv;
            logged.t = (tile_t){ .a=-1, .b=-1 };
            g->passes_in_row++;
            append_history(g, &logged);
        }else{
            int target = (mv.side < 0) ? g->left_end : g->right_end;
            int idx = -1;
            tile_t tile = mv.t;
            for(int i=0;i<g->hand_len[pid];++i){
                tile_t cur = g->hands[pid][i];
                if((cur.a == tile.a && cur.b == tile.b) || (cur.a == tile.b && cur.b == tile.a)){
                    idx = i;
                    tile = cur;
                    break;
                }
            }
            if(idx >= 0){
                tile_t placed = tile;
                int valid = 0;
                if(g->train_len == 0){
                    valid = 1;
                }else if(mv.side < 0){
                    if(target == -1 || tile.b == target){
                        valid = 1;
                        placed = tile;
                    }else if(tile.a == target){
                        valid = 1;
                        placed = (tile_t){ tile.b, tile.a };
                    }
                }else{
                    if(target == -1 || tile.a == target){
                        valid = 1;
                        placed = tile;
                    }else if(tile.b == target){
                        valid = 1;
                        placed = (tile_t){ tile.b, tile.a };
                    }
                }

                if(valid){
                    if(mv.side < 0){
                        int len = g->train_len;
                        if(len > 63) len = 63;
                        if(len > 0){
                            memmove(&g->train[1], &g->train[0], sizeof(tile_t)*len);
                        }
                        g->train[0] = placed;
                        g->left_end = placed.a;
                        if(g->train_len == 0){
                            g->right_end = placed.b;
                        }
                        if(g->train_len < 64) g->train_len = (len < 64 ? len + 1 : 64);
                    }else{
                        if(g->train_len < 64){
                            g->train[g->train_len] = placed;
                        }
                        g->right_end = placed.b;
                        if(g->train_len == 0){
                            g->left_end = placed.a;
                        }
                        if(g->train_len < 64) g->train_len++;
                    }

                    for(int j=idx;j<g->hand_len[pid]-1;++j){
                        g->hands[pid][j] = g->hands[pid][j+1];
                    }
                    if(g->hand_len[pid] > 0) g->hand_len[pid]--;

                    move_t logged = mv;
                    logged.t = placed;
                    append_history(g, &logged);
                    g->passes_in_row = 0;

                    if(g->hand_len[pid] == 0){
                        finish_round(g, pid, 0);
                    }
                }
            }
        }

        if(!g->finished){
            if(g->passes_in_row >= g->player_count){
                int best_pid = -1;
                int best_score = 1<<30;
                for(int i=0;i<g->player_count;++i){
                    int sc = compute_hand_points(g, i);
                    if(sc < best_score){
                        best_score = sc;
                        best_pid = i;
                    }
                }
                finish_round(g, best_pid, 1);
            }
        }

        if(!g->finished){
            int next = next_active_player(g, pid);
            g->turn = next;
        }

        pthread_mutex_unlock(&g->mtx);
    }
    return NULL;
}

/* ===== Scheduler (HPCS) ===== */
typedef struct { pcb_t *pcbs; int n; policy_t pol; int quantum_ms; game_state_t *game; } sched_ctx_t;

static void wake_all_players(pcb_t *pcbs, int n){
    for(int i=0;i<n;i++){
        pthread_mutex_lock(&pcbs[i].mtx);
        pcbs[i].can_run = 1;
        pthread_cond_signal(&pcbs[i].run_cv);
        pthread_mutex_unlock(&pcbs[i].mtx);
    }
}

static void *scheduler_thread(void *arg){
    sched_ctx_t *sc = (sched_ctx_t*)arg;
    int last_turn = 0;
    while(1){
        int active = 0;
        for(int i=0;i<sc->n;i++){
            if(sc->pcbs[i].st != TERMINATED){
                active = 1;
                break;
            }
        }
        if(!active) break;

        game_state_t *g = sc->game;
        pthread_mutex_lock(&g->mtx);
        int finished = g->finished;
        int turn = g->turn;
        pthread_mutex_unlock(&g->mtx);

        if(finished){
            wake_all_players(sc->pcbs, sc->n);
            msleep(10);
            continue;
        }

        if(turn < 0 || turn >= sc->n){
            msleep(sc->quantum_ms);
            continue;
        }

        pcb_t *p = &sc->pcbs[turn];
        if(p->st == TERMINATED){
            pthread_mutex_lock(&g->mtx);
            int next = next_active_player(g, turn);
            g->turn = next;
            pthread_mutex_unlock(&g->mtx);
            msleep(5);
            continue;
        }

        pthread_mutex_lock(&p->mtx);
        p->st = RUNNING;
        p->can_run = 1;
        pthread_cond_signal(&p->run_cv);
        pthread_mutex_unlock(&p->mtx);

        msleep(sc->quantum_ms);

        pthread_mutex_lock(&p->mtx);
        if(p->st != TERMINATED){
            p->can_run = 0;
            if(p->st == RUNNING) p->st = READY;
        }
        pthread_mutex_unlock(&p->mtx);

        last_turn = turn;
    }
    wake_all_players(sc->pcbs, sc->n);
    return NULL;
}

/* ===== Player thread ===== */
typedef struct {
    int id;
    game_state_t *g;
    pcb_t *pcb;
    bool is_human;
} player_ctx_t;

struct table_runtime_t {
    game_state_t state;
    pcb_t *pcbs;
    player_ctx_t *pctx;
    pthread_t *player_threads;
    pthread_t validator_thread;
    pthread_t scheduler_thread;
    sched_ctx_t scheduler_ctx;
    int seats;
};

typedef struct {
    table_runtime_t *tables;
    int table_count;
    int interval_ms;
} reporter_ctx_t;

static void tile_to_string(tile_t t, char *buf, size_t n){
    if(t.a < 0 || t.b < 0){
        snprintf(buf, n, "PASS");
    }else{
        snprintf(buf, n, "[%d|%d]", t.a, t.b);
    }
}

static void describe_train(const tile_t *train, int len, char *buf, size_t n){
    buf[0] = '\0';
    size_t used = 0;
    for(int i=0;i<len;i++){
        char tmp[16];
        tile_to_string(train[i], tmp, sizeof(tmp));
        int written = snprintf(buf+used, (used<n)? n-used:0, "%s%s", (i==0)?"":"-", tmp);
        if(written < 0) break;
        used += written;
        if(used >= n) break;
    }
    if(len==0 && n>0){
        snprintf(buf, n, "(vacío)");
    }
}

static int human_take_turn(player_ctx_t *cx){
    game_state_t *g = cx->g;
    int pid = cx->id;
    while(1){
        pthread_mutex_lock(&io_mtx);
        pthread_mutex_lock(&g->mtx);
        if(g->finished || g->turn != pid){
            pthread_mutex_unlock(&g->mtx);
            pthread_mutex_unlock(&io_mtx);
            return 0;
        }
        tile_t train_copy[64];
        int train_len = g->train_len;
        if(train_len > 64) train_len = 64;
        memcpy(train_copy, g->train, sizeof(tile_t)*train_len);
        tile_t hand_copy[14];
        int hand_len = g->hand_len[pid];
        if(hand_len > 14) hand_len = 14;
        memcpy(hand_copy, g->hands[pid], sizeof(tile_t)*hand_len);
        int left = g->left_end;
        int right = g->right_end;
        int pool_len = g->pool_len;
        pthread_mutex_unlock(&g->mtx);
        char train_buf[512];
        describe_train(train_copy, train_len, train_buf, sizeof(train_buf));
        printf("\n[Humano] Mesa %d - Turno Jugador %d\n", g->table_id+1, pid+1);
        printf("Tren: %s (izq=%d, der=%d)\n", train_buf, left, right);
        printf("Fichas en mano:\n");
        for(int i=0;i<hand_len;i++){
            char tile_buf[16];
            tile_to_string(hand_copy[i], tile_buf, sizeof(tile_buf));
            printf("  %2d: %s\n", i+1, tile_buf);
        }
        printf("Fichas en pozo: %d\n", pool_len);
        printf("Opciones: [j] jugar, [c] comprar, [p] pasar\n> ");
        fflush(stdout);
        char line[64];
        if(!fgets(line, sizeof(line), stdin)){
            clearerr(stdin);
            pthread_mutex_unlock(&io_mtx);
            return 0;
        }
        char opt = tolower((unsigned char)line[0]);
        int action_done = 0;
        switch(opt){
            case 'j': {
                if(hand_len == 0){
                    printf("No tiene fichas para jugar.\n");
                    break;
                }
                int tile_index = -1;
                while(1){
                    printf("Seleccione el índice de la ficha (1-%d): ", hand_len);
                    fflush(stdout);
                    if(!fgets(line, sizeof(line), stdin)){
                        clearerr(stdin);
                        break;
                    }
                    if(sscanf(line, "%d", &tile_index) == 1){
                        tile_index -= 1;
                        if(tile_index >=0 && tile_index < hand_len) break;
                    }
                    printf("Índice inválido.\n");
                }
                if(tile_index < 0 || tile_index >= hand_len) break;
                int selected_side = 0;
                while(selected_side == 0){
                    printf("Seleccione el lado (i=izquierda, d=derecha): ");
                    fflush(stdout);
                    if(!fgets(line, sizeof(line), stdin)){
                        clearerr(stdin);
                        break;
                    }
                    char c = tolower((unsigned char)line[0]);
                    if(c == 'i') selected_side = -1;
                    else if(c == 'd') selected_side = 1;
                    else printf("Opción inválida.\n");
                }
                if(selected_side == 0) break;

                pthread_mutex_lock(&g->mtx);
                if(g->finished || g->turn != pid){
                    pthread_mutex_unlock(&g->mtx);
                    pthread_mutex_unlock(&io_mtx);
                    return 0;
                }
                if(tile_index >= g->hand_len[pid]){
                    pthread_mutex_unlock(&g->mtx);
                    printf("La ficha seleccionada ya no está disponible.\n");
                    break;
                }
                tile_t tile = g->hands[pid][tile_index];
                int target = (selected_side < 0) ? g->left_end : g->right_end;
                if(target != -1 && !(tile.a == target || tile.b == target)){
                    pthread_mutex_unlock(&g->mtx);
                    printf("La ficha no encaja en ese lado.\n");
                    break;
                }
                move_t mv = { .player_id = pid, .table_id = g->table_id, .t = tile, .side = selected_side };
                moveq_push(&mv);
                pthread_mutex_unlock(&g->mtx);
                char tile_buf[16];
                tile_to_string(tile, tile_buf, sizeof(tile_buf));
                printf("Jugada enviada: %s al lado %s.\n", tile_buf, (selected_side<0)?"izquierdo":"derecho");
                action_done = 1;
                break;
            }
            case 'c': {
                pthread_mutex_lock(&g->mtx);
                if(draw_from_pool(g, pid)){
                    tile_t new_tile = g->hands[pid][g->hand_len[pid]-1];
                    pthread_mutex_unlock(&g->mtx);
                    char tile_buf[16];
                    tile_to_string(new_tile, tile_buf, sizeof(tile_buf));
                    printf("Robó la ficha %s.\n", tile_buf);
                }else{
                    int pool_empty = (g->pool_len == 0);
                    pthread_mutex_unlock(&g->mtx);
                    if(pool_empty){
                        printf("No quedan fichas en el pozo.\n");
                    }else{
                        printf("No fue posible robar una ficha.\n");
                    }
                }
                break;
            }
            case 'p': {
                pthread_mutex_lock(&g->mtx);
                int pool_empty = (g->pool_len == 0);
                tile_t dummy; int dummy_side;
                int can = can_play(g, pid, &dummy, &dummy_side);
                if(!pool_empty || can){
                    pthread_mutex_unlock(&g->mtx);
                    printf("No puede pasar: aún tiene jugadas o el pozo no está vacío.\n");
                    break;
                }
                move_t mv = { .player_id = pid, .table_id = g->table_id, .t = { .a=-1, .b=-1 }, .side = 0 };
                moveq_push(&mv);
                pthread_mutex_unlock(&g->mtx);
                printf("Se registró el pase de turno.\n");
                action_done = 1;
                break;
            }
            default:
                printf("Opción no reconocida.\n");
                break;
        }
        pthread_mutex_unlock(&io_mtx);
        if(action_done) return 1;
    }
    return 0;
}

static void *player_thread(void *arg){
    player_ctx_t *cx = (player_ctx_t*)arg;
    pcb_t *pcb = cx->pcb; game_state_t *g = cx->g;
    while(1){
        pthread_mutex_lock(&pcb->mtx);
        while(!pcb->can_run) pthread_cond_wait(&pcb->run_cv, &pcb->mtx);
        pthread_mutex_unlock(&pcb->mtx);

        pthread_mutex_lock(&g->mtx);
        if(g->finished){
            pthread_mutex_unlock(&g->mtx);
            break;
        }
        if(g->turn != cx->id){
            pthread_mutex_unlock(&g->mtx);
            msleep(5);
            continue;
        }
        pthread_mutex_unlock(&g->mtx);

        int performed = 0;
        if(cx->is_human){
            performed = human_take_turn(cx);
        }else{
            tile_t t; int side=0;
            pthread_mutex_lock(&g->mtx);
            int ok = can_play(g, cx->id, &t, &side);
            pthread_mutex_unlock(&g->mtx);

            if(ok){
                move_t mv = { .player_id=cx->id, .table_id=g->table_id, .t=t, .side=side };
                moveq_push(&mv);
                performed = 1;
            }else{
                pthread_mutex_lock(&g->mtx);
                int drew = draw_from_pool(g, cx->id);
                int pool_empty = (g->pool_len == 0);
                pthread_mutex_unlock(&g->mtx);
                if(!drew && pool_empty){
                    move_t pass = { .player_id=cx->id, .table_id=g->table_id, .t={.a=-1,.b=-1}, .side=0 };
                    moveq_push(&pass);
                    performed = 1;
                }else{
                    msleep(5);
                }
            }
        }

        if(performed){
            int wait_loops = 0;
            while(1){
                pthread_mutex_lock(&g->mtx);
                int finished = g->finished;
                int turn_now = g->turn;
                pthread_mutex_unlock(&g->mtx);
                if(finished || turn_now != cx->id) break;
                msleep(2);
                if(++wait_loops > 1000) break;
            }
        }

        pthread_mutex_lock(&g->mtx);
        int finished = g->finished;
        int hand_empty = (g->hand_len[cx->id] == 0);
        pthread_mutex_unlock(&g->mtx);
        if(finished || hand_empty){
            break;
        }
    }
    pthread_mutex_lock(&pcb->mtx);
    pcb->st = TERMINATED;
    pcb->can_run = 1;
    pthread_cond_signal(&pcb->run_cv);
    pthread_mutex_unlock(&pcb->mtx);
    return NULL;
}

static void print_table_state(table_runtime_t *table, int force){
    if(force){
        pthread_mutex_lock(&io_mtx);
    }else{
        if(pthread_mutex_trylock(&io_mtx) != 0) return;
    }

    game_state_t *g = &table->state;
    tile_t train_copy[64]; int train_len = 0;
    int left = -1, right = -1;
    int pool_len = 0;
    int turn = -1;
    int finished = 0;
    int winner = -1;
    int blocked = 0;
    int player_count = 0;
    int human_id = -1;
    int hand_len[MAX_PLAYERS] = {0};
    int points[MAX_PLAYERS] = {0};
    move_t history_buf[16]; int history_len = 0;
    int table_id = 0;

    pthread_mutex_lock(&g->mtx);
    table_id = g->table_id;
    finished = g->finished;
    winner = g->winner;
    blocked = g->blocked;
    player_count = g->player_count;
    human_id = g->human_player;
    turn = g->turn;
    pool_len = g->pool_len;
    left = g->left_end;
    right = g->right_end;
    train_len = g->train_len;
    if(train_len > 64) train_len = 64;
    memcpy(train_copy, g->train, sizeof(tile_t)*train_len);
    for(int i=0;i<player_count && i<MAX_PLAYERS;i++){
        hand_len[i] = g->hand_len[i];
        points[i] = compute_hand_points(g, i);
    }
    int start = 0;
    if(g->history_len > 16) start = g->history_len - 16;
    history_len = g->history_len - start;
    if(history_len < 0) history_len = 0;
    for(int i=0;i<history_len;i++){
        history_buf[i] = g->history[start + i];
    }
    pthread_mutex_unlock(&g->mtx);

    char train_str[512];
    describe_train(train_copy, train_len, train_str, sizeof(train_str));

    printf("\n=== Mesa %d ===\n", table_id + 1);
    printf("Tren: %s (izq=%d, der=%d) | Pozo: %d\n", train_str, left, right, pool_len);
    if(!finished && turn >= 0){
        printf("Turno actual: Jugador %d\n", turn + 1);
    }
    for(int i=0;i<player_count;i++){
        printf("Jugador %d%s: %d fichas", i+1, (i==human_id)?" (Humano)":"", hand_len[i]);
        if(finished){
            printf(" | Puntos: %d", points[i]);
        }
        printf("\n");
    }
    if(history_len > 0){
        printf("Historial reciente:\n");
        for(int i=0;i<history_len;i++){
            move_t mv = history_buf[i];
            char tile_buf[16];
            tile_to_string(mv.t, tile_buf, sizeof(tile_buf));
            if(mv.side == 0){
                printf("  Jugador %d pasó\n", mv.player_id + 1);
            }else{
                printf("  Jugador %d jugó %s al lado %s\n", mv.player_id + 1, tile_buf, (mv.side<0)?"izquierdo":"derecho");
            }
        }
    }
    if(finished){
        if(winner >= 0){
            printf("Resultado: Jugador %d ganó %s.\n", winner + 1, blocked?"por bloqueo":"al quedarse sin fichas");
        }else{
            printf("Resultado: partida finalizada.\n");
        }
    }
    fflush(stdout);
    pthread_mutex_unlock(&io_mtx);
}

/* ===== Mesa (opcional si solo hay una) ===== */
static void *table_thread(void *arg){
    game_state_t *g = (game_state_t*)arg;
    // TODO: orquestar turnos si no usa planificador global
    (void)g;
    return NULL;
}

static void *reporter_thread(void *arg){
    reporter_ctx_t *ctx = (reporter_ctx_t*)arg;
    while(1){
        int all_finished = 1;
        for(int i=0;i<ctx->table_count;i++){
            game_state_t *g = &ctx->tables[i].state;
            pthread_mutex_lock(&g->mtx);
            int finished = g->finished;
            pthread_mutex_unlock(&g->mtx);
            if(!finished) all_finished = 0;
            print_table_state(&ctx->tables[i], 0);
        }
        if(all_finished) break;
        msleep(ctx->interval_ms);
    }
    return NULL;
}

/* ===== Utilidades ===== */
static void shuffle(tile_t *v, int n){
    for(int i=n-1;i>0;i--){
        int j = rand() % (i+1);
        tile_t tmp = v[i]; v[i]=v[j]; v[j]=tmp;
    }
}

static void build_shuffled_deck(tile_t *deck, int *out_len){
    int idx = 0;
    for(int a = 0; a <= 6; ++a){
        for(int b = a; b <= 6; ++b){
            deck[idx++] = (tile_t){ .a = a, .b = b };
        }
    }
    *out_len = idx;
    shuffle(deck, idx);
}

static void setup_game_state(game_state_t *g, int table_id, int player_count, int human_player){
    tile_t deck[MAX_TILES];
    int deck_len = 0;
    build_shuffled_deck(deck, &deck_len);

    g->table_id = table_id;
    g->player_count = player_count;
    g->human_player = human_player;
    g->finished = 0;
    g->train_len = 0;
    g->left_end = -1;
    g->right_end = -1;
    g->pool_len = 0;
    g->turn = 0;
    g->winner = -1;
    g->blocked = 0;
    g->passes_in_row = 0;
    g->history_len = 0;

    for(int pid = 0; pid < MAX_PLAYERS; ++pid){
        g->hand_len[pid] = 0;
    }

    int deck_pos = 0;
    for(int pid = 0; pid < player_count; ++pid){
        for(int j = 0; j < 7 && deck_pos < deck_len; ++j){
            g->hands[pid][j] = deck[deck_pos++];
        }
        g->hand_len[pid] = 7;
    }

    for(int pid = player_count; pid < MAX_PLAYERS; ++pid){
        g->hand_len[pid] = 0;
    }

    while(deck_pos < deck_len && g->pool_len < MAX_TILES){
        g->pool[g->pool_len++] = deck[deck_pos++];
    }

    int start_pid = -1;
    int start_idx = -1;
    tile_t start_tile = {0, 0};
    int best_double = -1;
    for(int pid = 0; pid < player_count; ++pid){
        for(int j = 0; j < g->hand_len[pid]; ++j){
            tile_t t = g->hands[pid][j];
            if(t.a == t.b && t.a > best_double){
                best_double = t.a;
                start_pid = pid;
                start_idx = j;
                start_tile = t;
            }
        }
    }

    if(start_pid == -1){
        int best_sum = -1;
        int best_high = -1;
        for(int pid = 0; pid < player_count; ++pid){
            for(int j = 0; j < g->hand_len[pid]; ++j){
                tile_t t = g->hands[pid][j];
                int sum = t.a + t.b;
                int high = t.a > t.b ? t.a : t.b;
                if(sum > best_sum || (sum == best_sum && high > best_high)){
                    best_sum = sum;
                    best_high = high;
                    start_pid = pid;
                    start_idx = j;
                    start_tile = t;
                }
            }
        }
    }

    if(start_pid >= 0){
        int len = g->hand_len[start_pid];
        for(int j = start_idx; j < len - 1; ++j){
            g->hands[start_pid][j] = g->hands[start_pid][j + 1];
        }
        if(len > 0){
            g->hand_len[start_pid] = len - 1;
        }
        g->train[0] = start_tile;
        g->train_len = 1;
        g->left_end = start_tile.a;
        g->right_end = start_tile.b;
        g->turn = (start_pid + 1) % player_count;
    }
}
static int can_play(const game_state_t *g, int pid, tile_t *out, int *side){
    if(pid < 0 || pid >= g->player_count) return 0;
    if(g->turn != pid) return 0;
    if(g->hand_len[pid] <= 0) return 0;
    int left = g->left_end;
    int right = g->right_end;
    for(int i=0;i<g->hand_len[pid];++i){
        tile_t t = g->hands[pid][i];
        if(left == -1 || t.a == left || t.b == left){
            if(out) *out = t;
            if(side) *side = -1;
            return 1;
        }
        if(right == -1 || t.a == right || t.b == right){
            if(out) *out = t;
            if(side) *side = 1;
            return 1;
        }
    }
    return 0;
}
static int draw_from_pool(game_state_t *g, int pid){
    if(pid < 0 || pid >= g->player_count) return 0;
    if(g->pool_len <= 0) return 0;
    if(g->hand_len[pid] >= 14) return 0;
    tile_t t = g->pool[g->pool_len-1];
    g->pool_len--;
    g->hands[pid][g->hand_len[pid]] = t;
    g->hand_len[pid] += 1;
    return 1;
}
static long now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec*1000L + ts.tv_nsec/1000000L;
}
static void msleep(int ms){ usleep(ms*1000); }

/* ===== main ===== */
int main(int argc, char **argv){
    (void)argc; (void)argv;

    srand((unsigned)time(NULL));

    int keep_playing = 1;
    while(keep_playing){
        int tables_count = 0;
        while(tables_count < 1){
            printf("Ingrese la cantidad de mesas a crear (>=1): ");
            fflush(stdout);
            if(scanf("%d", &tables_count) != 1){
                int ch;
                while((ch = getchar()) != '\n' && ch != EOF){}
                tables_count = 0;
                printf("Entrada inválida. Intente nuevamente.\n");
            }else if(tables_count < 1){
                printf("La cantidad debe ser al menos 1.\n");
            }
        }

        table_runtime_t *tables = calloc(tables_count, sizeof(table_runtime_t));
        if(!tables){
            fprintf(stderr, "Error al reservar memoria para las mesas.\n");
            return 1;
        }

        moveq_init();

        for(int t=0; t<tables_count; ++t){
            table_runtime_t *tbl = &tables[t];
            tbl->seats = rand()%3 + 2;

            printf("Mesa %d: %d asientos disponibles.\n", t+1, tbl->seats);
            bool occupy = false;
            char opt;
            printf("¿Desea ocupar uno de estos asientos? (s/n): ");
            fflush(stdout);
            if(scanf(" %c", &opt) == 1){
                if(opt == 's' || opt == 'S'){
                    occupy = true;
                }
            }else{
                int ch;
                while((ch = getchar()) != '\n' && ch != EOF){}
            }

            int chosen_seat = -1;
            if(occupy){
                while(1){
                    printf("Seleccione el número de asiento (1-%d): ", tbl->seats);
                    fflush(stdout);
                    if(scanf("%d", &chosen_seat) != 1){
                        int ch;
                        while((ch = getchar()) != '\n' && ch != EOF){}
                        printf("Entrada inválida. Intente nuevamente.\n");
                        chosen_seat = -1;
                        continue;
                    }
                    chosen_seat -= 1;
                    if(chosen_seat < 0 || chosen_seat >= tbl->seats){
                        printf("Asiento fuera de rango. Intente nuevamente.\n");
                        chosen_seat = -1;
                    }else{
                        break;
                    }
                }
            }

            tbl->pcbs = calloc(tbl->seats, sizeof(pcb_t));
            tbl->pctx = calloc(tbl->seats, sizeof(player_ctx_t));
            tbl->player_threads = calloc(tbl->seats, sizeof(pthread_t));
            if(!tbl->pcbs || !tbl->pctx || !tbl->player_threads){
                fprintf(stderr, "Error al reservar memoria para la mesa %d.\n", t+1);
                return 1;
            }

            tbl->state = (game_state_t){0};
            pthread_mutex_init(&tbl->state.mtx, NULL);
            setup_game_state(&tbl->state, t, tbl->seats, chosen_seat);

            for(int i=0;i<tbl->seats;i++){
                tbl->pcbs[i].pid=i;
                tbl->pcbs[i].st=READY;
                tbl->pcbs[i].pol=RR;
                tbl->pcbs[i].can_run = 0;
                pthread_mutex_init(&tbl->pcbs[i].mtx,NULL);
                pthread_cond_init(&tbl->pcbs[i].run_cv,NULL);
                tbl->pctx[i] = (player_ctx_t){ .id=i, .g=&tbl->state, .pcb=&tbl->pcbs[i], .is_human=(i==chosen_seat) };
                pthread_create(&tbl->player_threads[i], NULL, player_thread, &tbl->pctx[i]);
            }

            pthread_create(&tbl->validator_thread, NULL, validator_thread, &tbl->state);

            tbl->scheduler_ctx = (sched_ctx_t){ .pcbs=tbl->pcbs, .n=tbl->seats, .pol=RR, .quantum_ms=Q_DEFAULT_MS, .game=&tbl->state };
            pthread_create(&tbl->scheduler_thread, NULL, scheduler_thread, &tbl->scheduler_ctx);
        }

        reporter_ctx_t rep_ctx = { .tables = tables, .table_count = tables_count, .interval_ms = 500 };
        pthread_t rep_thread;
        pthread_create(&rep_thread, NULL, reporter_thread, &rep_ctx);

        for(int t=0; t<tables_count; ++t){
            table_runtime_t *tbl = &tables[t];
            pthread_join(tbl->scheduler_thread, NULL);
            pthread_join(tbl->validator_thread, NULL);
            for(int i=0;i<tbl->seats;i++){
                pthread_join(tbl->player_threads[i], NULL);
            }
        }

        pthread_join(rep_thread, NULL);

        for(int t=0; t<tables_count; ++t){
            table_runtime_t *tbl = &tables[t];
            print_table_state(tbl, 1);
            pthread_mutex_lock(&tbl->state.mtx);
            int winner = tbl->state.winner;
            int blocked = tbl->state.blocked;
            int player_count = tbl->state.player_count;
            int human_id = tbl->state.human_player;
            int scores[MAX_PLAYERS] = {0};
            for(int i=0;i<player_count;i++){
                scores[i] = compute_hand_points(&tbl->state, i);
            }
            pthread_mutex_unlock(&tbl->state.mtx);
            if(winner >= 0){
                printf("Ganador mesa %d: Jugador %d%s (%s).\n", t+1, winner+1, (winner==human_id)?" (Humano)":"", blocked?"bloqueo":"mano limpia");
            }else{
                printf("Mesa %d finalizada sin ganador registrado.\n", t+1);
            }
            printf("Puntajes finales: ");
            for(int i=0;i<player_count;i++){
                printf("J%d=%d%s", i+1, scores[i], (i==player_count-1)?"":" | ");
            }
            printf("\n");

            for(int i=0;i<tbl->seats;i++){
                pthread_mutex_destroy(&tbl->pcbs[i].mtx);
                pthread_cond_destroy(&tbl->pcbs[i].run_cv);
            }
            pthread_mutex_destroy(&tbl->state.mtx);
            free(tbl->pcbs);
            free(tbl->pctx);
            free(tbl->player_threads);
        }
        free(tables);

        int ch;
        while((ch = getchar()) != '\n' && ch != EOF){}

        while(1){
            printf("\n¿Desea jugar otra partida? (s/n): ");
            fflush(stdout);
            char again;
            if(scanf(" %c", &again) != 1){
                while((ch = getchar()) != '\n' && ch != EOF){}
                printf("Entrada inválida.\n");
                continue;
            }
            again = tolower((unsigned char)again);
            while((ch = getchar()) != '\n' && ch != EOF){}
            if(again == 's'){
                keep_playing = 1;
                break;
            }else if(again == 'n'){
                keep_playing = 0;
                break;
            }else{
                printf("Opción inválida.\n");
            }
        }
    }

    return 0;
}
