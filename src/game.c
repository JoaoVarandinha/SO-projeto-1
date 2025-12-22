#include "board.h"
#include "display.h"
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>

#define CONTINUE_PLAY 0
#define NEXT_LEVEL 1
#define QUIT_GAME 2
#define LOAD_BACKUP 3
#define CREATE_BACKUP 4

typedef struct {
    board_t* board;
    int ghost_idx;
} ghost_thread_args;

void screen_refresh(board_t * game_board, int mode) {
    debug("REFRESH\n");
    draw_board(game_board, mode);
    refresh_screen();
}

int pacman_alive(board_t* board) {
    pacman_t* pacman = &board->pacmans[0];
    pthread_mutex_lock(&pacman->pac_lock);
    int status = pacman->alive;
    pthread_mutex_unlock(&pacman->pac_lock);
    return status;
}

void *display_thread(void* arg) {
    board_t *board = (board_t*)arg;
    
    sleep_ms(board->tempo / 2);

    while (true) {
        sleep_ms(board->tempo);

        pthread_rwlock_wrlock(&board->board_lock);
        if (board->shutdown_threads) {
            pthread_rwlock_unlock(&board->board_lock);
            pthread_exit(NULL);
        }
        screen_refresh(board, DRAW_MENU);
        pthread_rwlock_unlock(&board->board_lock);
    }
}

void *pacman_thread(void* arg) {
    board_t* board = (board_t*)arg;
    pacman_t* pacman = &board->pacmans[0];

    int *play_result = malloc(sizeof(int));

    while (true) {
        if (!pacman_alive(board)) {
            *play_result = LOAD_BACKUP;
            return (void*) play_result;
        }

        sleep_ms(board->tempo);

        command_t* play;
        command_t c;
        if (pacman->n_moves == 0) { // if is user input

            c.command = get_input();

            if(c.command == '\0') {
                continue;
            }

            c.turns = 1;
            play = &c;
        } else { // else if the moves are pre-defined in the file
            // avoid buffer overflow wrapping around with modulo of n_moves
            // this ensures that we always access a valid move for the pacman
            play = &pacman->moves[pacman->current_move%pacman->n_moves];
        }

        debug("KEY %c\n", play->command);

        if (play->command == 'Q') {
            *play_result = QUIT_GAME;
            return (void*) play_result;
        }

        if (play->command == 'G') {
            pacman->current_move++;
            *play_result = CREATE_BACKUP;
            return (void*) play_result;
        }

        pthread_rwlock_rdlock(&board->board_lock);

        int result = move_pacman(board, 0, play);

        if (result == REACHED_PORTAL) {
            // Next level
            *play_result = NEXT_LEVEL;
            break;
        }

        if (result == DEAD_PACMAN) {
            *play_result = LOAD_BACKUP;
            break;
        }
        pthread_rwlock_unlock(&board->board_lock);
    }
    pthread_rwlock_unlock(&board->board_lock);

    return (void*) play_result;
}

void *ghost_thread(void* arg) {
    ghost_thread_args* args = (ghost_thread_args*)arg;
    board_t* board = args->board;
    int ghost_idx = args->ghost_idx;

    free(args);

    ghost_t* ghost = &board->ghosts[ghost_idx];

    while (true) {
        sleep_ms(board->tempo);

        pthread_rwlock_rdlock(&board->board_lock);

        if (board->shutdown_threads) {
            pthread_rwlock_unlock(&board->board_lock);
            pthread_exit(NULL);
        }
        
        move_ghost(board, ghost_idx, &ghost->moves[ghost->current_move%ghost->n_moves]);

        if (!pacman_alive(board)) {
            pthread_rwlock_unlock(&board->board_lock);
            pthread_exit(NULL);
        }
        pthread_rwlock_unlock(&board->board_lock);
    }
}

int play_board_threads(board_t* board) {
    pthread_t display_tid, pac_tid, ghost_tid[MAX_GHOSTS];

    board->shutdown_threads = 0;

    pthread_create(&display_tid, NULL, display_thread, board);
    pthread_create(&pac_tid, NULL, pacman_thread, board);
    for (int i = 0; i < board->n_ghosts; i++) {
        ghost_thread_args* args = calloc(1, sizeof(*args));

        args->board = board;
        args->ghost_idx = i;

        pthread_create(&ghost_tid[i], NULL, ghost_thread, args);
    }

    int *play_result;
    pthread_join(pac_tid, (void**)&play_result);

    pthread_rwlock_wrlock(&board->board_lock);
    board->shutdown_threads = 1;
    pthread_rwlock_unlock(&board->board_lock);

    
    pthread_join(display_tid, NULL);
    for (int i = 0; i < board->n_ghosts; i++) {
        pthread_join(ghost_tid[i], NULL);
    }

    int result = *play_result;
    free(play_result);

    return result;
}


int main(int argc, char** argv) {
    if (argc != 2) {
        printf("Usage: %s <level_directory>\n", argv[0]);
        return -1;
    }

    // Random seed for any random movements
    srand((unsigned int)time(NULL));

    open_debug_file("debug.log");

    terminal_init();
    
    int accumulated_points = 0;
    bool end_game = false;
    pid_t pid = -1;
    board_t game_board;

    DIR* dir = opendir(argv[1]);
    if (!dir) {
        perror("Error opening directory");
        exit(EXIT_FAILURE);
    }

    strcpy(game_board.dir_name, argv[1]);
    
    struct dirent* entry;
    while (!end_game && (entry = readdir(dir)) != NULL) {
        int len = strlen(entry->d_name);
        if (len <= 4 || strcmp(entry->d_name + len - 4, LEVEL) != 0) continue;

        load_level(&game_board, entry->d_name);
        load_pacman(&game_board,accumulated_points);
        load_ghosts(&game_board);
        
        draw_board(&game_board, DRAW_MENU);
        refresh_screen();

        while (true) {

            int result = play_board_threads(&game_board);

            if (result == NEXT_LEVEL) {
                screen_refresh(&game_board, DRAW_WIN);
                sleep_ms(game_board.tempo);
                break;
            }

            if (result == LOAD_BACKUP) {
                if (pid == 0) {
                    unload_level(&game_board);
                    exit(LOAD_BACKUP);
                } else {
                    result = QUIT_GAME;
                }
            }

            if (result == QUIT_GAME) {
                if (pid == 0) {
                    unload_level(&game_board);
                    exit(QUIT_GAME);
                }
                screen_refresh(&game_board, DRAW_GAME_OVER); 
                sleep_ms(game_board.tempo);
                end_game = true;
                break;
            }

            if (result == CREATE_BACKUP) {
                if (pid != 0) {
                    pid = fork();
                    if (pid == -1) {
                            perror("Error forking");
                            exit(EXIT_FAILURE);
                    } else if (pid == 0) {
                        screen_refresh(&game_board, DRAW_MENU);
                        continue;
                    } else {
                        int status;
                        wait(&status);
                        if (WIFEXITED(status)) {
                            if (WEXITSTATUS(status) == QUIT_GAME) {
                                end_game = true;
                                break; 
                            } else if (WEXITSTATUS(status) == LOAD_BACKUP) {
                                screen_refresh(&game_board, DRAW_MENU);
                                continue;
                            }
                        } else {
                            perror("Error waiting for child");
                            exit(EXIT_FAILURE);
                        }
                    }
                }
            }
        }

    accumulated_points = game_board.pacmans[0].points;

    print_board(&game_board);
    unload_level(&game_board);

    }

    if (pid == 0) exit(QUIT_GAME);

    closedir(dir);

    terminal_cleanup();

    close_debug_file();

    return 0;
}