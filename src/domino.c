// domino.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <linux/time.h>
#include <stdbool.h>

#define MAX_PLAYERS 4
#define MAX_TILES   28
#define Q_DEFAULT_MS 50

typedef enum { FCFS, SJF_PLAYERS, SJF_POINTS, RR } policy_t;
typedef enum { NEW, READY, RUNNING, IO_WAIT, TERMINATED } pstate_t;

typedef struct { int a, b; } tile_t;

typedef struct {
    // extremos, tren, manos, pozo...
    tile_t train[64]; int train_len;
    int left_end, right_end;
    tile_t hands[MAX_PLAYERS][14]; int hand_len[MAX_PLAYERS];
    tile_t pool[28]; int pool_len;
    int turn, table_id, finished;
    int player_count;
    int human_player;
    pthread_mutex_t mtx; // sección crítica del estado
} game_state_t;

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
static int  moveq_pop(move_t *out);
// Validación (HVU)
static void *validator_thread(void *arg);
// Planificador (HPCS)
static void *scheduler_thread(void *arg);
// Mesa (HJ)
static void *table_thread(void *arg);
// Jugador
static void *player_thread(void *arg);
// Ayudas
static long now_ms(void);
static void msleep(int ms);

/* ===== Cola de movimientos ===== */
static pthread_mutex_t q_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  q_cv  = PTHREAD_COND_INITIALIZER;
static move_t qbuf[256]; static int qh=0, qt=0, qn=0;

static void moveq_init(void){ qh=qt=qn=0; }
static void moveq_push(const move_t *m){
    pthread_mutex_lock(&q_mtx);
    qbuf[qt] = *m; qt = (qt+1)%256; qn++;
    pthread_cond_signal(&q_cv);
    pthread_mutex_unlock(&q_mtx);
}
static int moveq_pop(move_t *out){
    pthread_mutex_lock(&q_mtx);
    while(qn==0) pthread_cond_wait(&q_cv, &q_mtx);
    *out = qbuf[qh]; qh=(qh+1)%256; qn--;
    pthread_mutex_unlock(&q_mtx);
    return 1;
}

/* ===== Validator (HVU) ===== */
static void *validator_thread(void *arg){
    game_state_t *g = (game_state_t*)arg;
    while(!g->finished){
        move_t mv; moveq_pop(&mv);
        pthread_mutex_lock(&g->mtx);
        // TODO: validar jugada, actualizar extremos, tren, mano del jugador
        // TODO: detectar fin de partida (mano vacía o bloqueo)
        pthread_mutex_unlock(&g->mtx);
        // opcional: loggear evento
    }
    return NULL;
}

/* ===== Scheduler (HPCS) ===== */
typedef struct { pcb_t *pcbs; int n; policy_t pol; int quantum_ms; } sched_ctx_t;

static void *scheduler_thread(void *arg){
    sched_ctx_t *sc = (sched_ctx_t*)arg;
    while(1){
        // TODO: elegir siguiente PCB listo según sc->pol
        // Ejemplo simplificado: round-robin
        for(int i=0;i<sc->n;i++){
            pcb_t *p = &sc->pcbs[i];
            if(p->st==TERMINATED) continue;
            pthread_mutex_lock(&p->mtx);
            p->can_run = 1; pthread_cond_signal(&p->run_cv);
            pthread_mutex_unlock(&p->mtx);
            msleep(sc->quantum_ms); // tiempo de CPU
            // preempción: retirar CPU
            pthread_mutex_lock(&p->mtx);
            p->can_run = 0;
            pthread_mutex_unlock(&p->mtx);
        }
        // TODO: condición de salida global cuando todos TERMINATED
    }
    return NULL;
}

/* ===== Player thread ===== */
typedef struct {
    int id;
    game_state_t *g;
    pcb_t *pcb;
    bool is_human;
} player_ctx_t;

typedef struct {
    game_state_t state;
    pcb_t *pcbs;
    player_ctx_t *pctx;
    pthread_t *player_threads;
    pthread_t validator_thread;
    pthread_t scheduler_thread;
    sched_ctx_t scheduler_ctx;
    int seats;
} table_runtime_t;

static void *player_thread(void *arg){
    player_ctx_t *cx = (player_ctx_t*)arg;
    pcb_t *pcb = cx->pcb; game_state_t *g = cx->g;
    while(1){
        pthread_mutex_lock(&pcb->mtx);
        while(!pcb->can_run) pthread_cond_wait(&pcb->run_cv, &pcb->mtx);
        pthread_mutex_unlock(&pcb->mtx);

        // “Tiene CPU”: intenta jugar
        tile_t t; int side=0;
        pthread_mutex_lock(&g->mtx);
        int ok = can_play(g, cx->id, &t, &side);
        pthread_mutex_unlock(&g->mtx);

        if(ok){
            move_t mv = { .player_id=cx->id, .table_id=g->table_id, .t=t, .side=side };
            moveq_push(&mv); // HVU valida/aplica
        }else{
            // comer del pozo (E/S breve) o pasar si pozo vacío
            pthread_mutex_lock(&g->mtx);
            int drew = draw_from_pool(g, cx->id);
            pthread_mutex_unlock(&g->mtx);
            if(!drew){
                // pasar: nada que hacer
            }else{
                msleep(5); // simular E/S
            }
        }

        // TODO: comprobar si este jugador ya terminó → pcb->st = TERMINATED y salir
        // si no, ceder CPU (RR) o esperar siguiente turno
    }
    return NULL;
}

/* ===== Mesa (opcional si solo hay una) ===== */
static void *table_thread(void *arg){
    game_state_t *g = (game_state_t*)arg;
    // TODO: orquestar turnos si no usa planificador global
    (void)g;
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
    // TODO: buscar en mano[pid] una ficha que calce con left_end o right_end
    (void)g; (void)pid; (void)out; (void)side;
    return 0;
}
static int draw_from_pool(game_state_t *g, int pid){
    // TODO: mover del pool a la mano del jugador pid
    (void)g; (void)pid;
    return 0;
}
static long now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec*1000L + ts.tv_nsec/1000000L;
}
static void msleep(int ms){ usleep(ms*1000); }

/* ===== main ===== */
int main(int argc, char **argv){
    (void)argc; (void)argv;

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

    srand((unsigned)time(NULL));

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
            pthread_mutex_init(&tbl->pcbs[i].mtx,NULL);
            pthread_cond_init(&tbl->pcbs[i].run_cv,NULL);
            tbl->pctx[i] = (player_ctx_t){ .id=i, .g=&tbl->state, .pcb=&tbl->pcbs[i], .is_human=(i==chosen_seat) };
            pthread_create(&tbl->player_threads[i], NULL, player_thread, &tbl->pctx[i]);
        }

        pthread_create(&tbl->validator_thread, NULL, validator_thread, &tbl->state);

        tbl->scheduler_ctx = (sched_ctx_t){ .pcbs=tbl->pcbs, .n=tbl->seats, .pol=RR, .quantum_ms=Q_DEFAULT_MS };
        pthread_create(&tbl->scheduler_thread, NULL, scheduler_thread, &tbl->scheduler_ctx);
    }

    for(int t=0; t<tables_count; ++t){
        table_runtime_t *tbl = &tables[t];
        pthread_join(tbl->scheduler_thread, NULL);
        pthread_join(tbl->validator_thread, NULL);
        for(int i=0;i<tbl->seats;i++){
            pthread_join(tbl->player_threads[i], NULL);
        }
    }

    for(int t=0; t<tables_count; ++t){
        table_runtime_t *tbl = &tables[t];
        free(tbl->pcbs);
        free(tbl->pctx);
        free(tbl->player_threads);
    }
    free(tables);

    return 0;
}
