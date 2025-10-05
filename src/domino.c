// domino.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <linux/time.h>

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
typedef struct { int id; game_state_t *g; pcb_t *pcb; } player_ctx_t;

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
    srand((unsigned)time(NULL));

    game_state_t g = {0}; pthread_mutex_init(&g.mtx, NULL);
    g.table_id = 0;

    // TODO: generar 28 fichas, barajar, repartir 7 por jugador, resto al pozo, setear extremos si hay doble inicial, etc.

    // PCBs y jugadores
    pcb_t pcbs[MAX_PLAYERS]={0};
    player_ctx_t pctx[MAX_PLAYERS];
    pthread_t th_players[MAX_PLAYERS];

    for(int i=0;i<MAX_PLAYERS;i++){
        pcbs[i].pid=i; pcbs[i].st=READY; pcbs[i].pol=RR;
        pthread_mutex_init(&pcbs[i].mtx,NULL);
        pthread_cond_init(&pcbs[i].run_cv,NULL);
        pctx[i]=(player_ctx_t){ .id=i, .g=&g, .pcb=&pcbs[i] };
        pthread_create(&th_players[i], NULL, player_thread, &pctx[i]);
    }

    // HVU
    pthread_t th_v; pthread_create(&th_v, NULL, validator_thread, &g);

    // Planificador
    sched_ctx_t sc = { .pcbs=pcbs, .n=MAX_PLAYERS, .pol=RR, .quantum_ms=Q_DEFAULT_MS };
    pthread_t th_s; pthread_create(&th_s, NULL, scheduler_thread, &sc);

    // Espera (en un prototipo simple puede unirse o bloquear hasta condición de fin)
    pthread_join(th_s, NULL);
    pthread_join(th_v, NULL);
    for(int i=0;i<MAX_PLAYERS;i++) pthread_join(th_players[i], NULL);

    return 0;
}
