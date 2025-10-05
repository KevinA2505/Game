# Domino Concurrente

Proyecto experimental en C que modela un simulador de dominó desde la perspectiva de un sistema operativo. El objetivo es estudiar la coordinación de hilos y la sincronización entre distintos "módulos" equivalentes a componentes clásicos de un kernel: planificador, procesos (jugadores), validadores de eventos y estructuras compartidas.

## Características principales
- **Simulación multi-hilo:** cada jugador, el planificador y el validador se ejecutan en hilos dedicados que comparten el estado global del juego.
- **Planificador configurable:** se bosquejan políticas FCFS, SJF (por cantidad de jugadas o puntos) y Round-Robin con cuantum ajustable.
- **Cola de movimientos sincronizada:** productor/consumidor basado en `pthread_mutex_t` y `pthread_cond_t` para desacoplar a los jugadores del validador de jugadas.
- **Estructura de estado centralizada:** la mesa, las manos, el pozo y los extremos activos se representan dentro de `game_state_t`, protegida por un mutex.

> ⚠️ El proyecto se encuentra en una etapa temprana: muchos bloques contienen `TODO` con la lógica pendiente para completar la simulación.

## Estructura del código
```
src/
└── domino.c        # Implementación principal con hilos, estados y colas de movimientos
```

### Componentes destacados
- **`validator_thread`**: recibe movimientos desde la cola y debería validar y aplicar las jugadas sobre la mesa.
- **`scheduler_thread`**: asigna CPU a los jugadores según la política definida, simulando un planificador de procesos.
- **`player_thread`**: cada jugador intenta colocar una ficha válida o roba del pozo cuando corresponde.
- **Utilidades** (`shuffle`, `can_play`, `draw_from_pool`, etc.): facilitan la generación de fichas y la mecánica de turnos.

## Estado actual y próximos pasos
1. **Lógica del validador:** aplicar movimientos, actualizar extremos del tren y detectar fin de partida (victoria o bloqueo).
2. **Planificador completo:** implementar selección por política y condición de salida cuando todos los PCBs finalicen.
3. **Ciclo de vida de jugadores:** marcar `TERMINATED` al quedar sin fichas o al detectar que no hay más jugadas posibles.
4. **Utilidades clave:** completar `can_play` y `draw_from_pool`, así como la generación y reparto inicial de fichas en `main`.
5. **Documentación adicional:** describir decisiones de diseño una vez implementada la lógica restante.

## Requisitos
- Compilador C compatible con C11 (`gcc` o `clang`).
- Biblioteca de hilos POSIX (`pthread`).
- Sistema tipo Unix para las funciones de sincronización empleadas.

## Compilación y ejecución
```
mkdir -p build
cc -std=c11 -Wall -Wextra -pthread src/domino.c -o build/domino
./build/domino
```
> El binario resultante ejecuta los hilos y queda bloqueado esperando a que el planificador finalice. Al no estar implementadas todas las salidas, puede requerir interrupción manual (`Ctrl+C`).

## Contribuir
1. Crea un fork o una rama de trabajo.
2. Implementa la lógica pendiente asegurando el uso correcto de mutexes y condiciones.
3. Añade pruebas o scripts que faciliten la validación del comportamiento concurrente.
4. Documenta los cambios relevantes y abre un Pull Request describiendo el enfoque.

## Licencia
Este proyecto se distribuye con fines educativos. Si planeas reutilizarlo o publicarlo, verifica la licencia aplicable o acuerda una con el mantenedor.
