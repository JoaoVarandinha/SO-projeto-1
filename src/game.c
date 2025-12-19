#include "board.h"
#include "display.h"
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <stdbool.h>
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

void *display_thread(void* arg) {
    board_t *board = (board_t*)arg;
    
    while (true) {

        sleep_ms(board->tempo);
        
        pthread_rwlock_wrlock(&board->board_lock);
        if (board->shutdown_threads) {
            pthread_rwlock_unlock(&board->board_lock);
            pthread_exit(NULL);
        }
        screen_refresh(board, DRAW_MENU);
        pthread_rwlock_unlock(&board->board_lock);


        char c = get_input();
        if (c != '\0') {
            pthread_mutex_lock(&board->info.info_lock);
            board->info.move_input = c;
            pthread_mutex_unlock(&board->info.info_lock);
        }
    }
}

void *pacman_thread(void* arg) {
    board_t* board = (board_t*)arg;
    pacman_t* pacman = &board->pacmans[0];

    int *play_result = malloc(sizeof(int));

    while (true) {
        if (!pacman->alive) {
            *play_result = LOAD_BACKUP;
            return (void*) play_result;
        }

        sleep_ms(board->tempo);

            command_t* play;
            command_t c;
            if (pacman->n_moves == 0) { // if is user input

                while (true) {
                    pthread_mutex_lock(&board->info.info_lock);
                    if (board->info.move_input != '\0') {
                        c.command = board->info.move_input;
                        board->info.move_input = '\0';
                        pthread_mutex_unlock(&board->info.info_lock);
                        break;
                    }
                    pthread_mutex_unlock(&board->info.info_lock);
                    if (!pacman->alive) {
                        *play_result = LOAD_BACKUP;
                        return (void*) play_result;
                    }
                    sleep(board->tempo / 2);
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
            *play_result = LOAD_BACKUP;
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
            *play_result = QUIT_GAME;
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
        pthread_rwlock_unlock(&board->board_lock);
    }
}

int play_board(board_t* game_board) {
    pacman_t* pacman = &game_board->pacmans[0];
    command_t* play;
    command_t c;

    if (pacman->n_moves == 0) { // if is user input
        c.command = get_input();

        if (c.command == '\0')
            return CONTINUE_PLAY;

        c.turns = 1;
        play = &c;
    }
    else { // else if the moves are pre-defined in the file
        // avoid buffer overflow wrapping around with modulo of n_moves
        // this ensures that we always access a valid move for the pacman
        play = &pacman->moves[pacman->current_move%pacman->n_moves];
    }

    debug("KEY %c\n", play->command);

    if (play->command == 'Q') {
        return QUIT_GAME;
    }

    if (play->command == 'G') {
        pacman->current_move++;
        return CREATE_BACKUP;
    }

    int result = move_pacman(game_board, 0, play);
    if (result == REACHED_PORTAL) {
        // Next level
        return NEXT_LEVEL;
    }

    if (result == DEAD_PACMAN) {
        return QUIT_GAME;
    }

    for (int i = 0; i < game_board->n_ghosts; i++) {
        ghost_t* ghost = &game_board->ghosts[i];
        // avoid buffer overflow wrapping around with modulo of n_moves
        // this ensures that we always access a valid move for the ghost
        move_ghost(game_board, i, &ghost->moves[ghost->current_move%ghost->n_moves]);
    }

    if (!game_board->pacmans[0].alive) {
        return QUIT_GAME;
    }      

    return CONTINUE_PLAY;  
}

int play_board_threads(board_t* board) {
    pthread_t display_tid, pac_tid, ghost_tid[MAX_GHOSTS];

    board->shutdown_threads = 0;

    pthread_create(&display_tid, NULL, display_thread, board);
    pthread_create(&pac_tid, NULL, pacman_thread, board); //Start pacman thread
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
    int accumulated_points = 0;
    bool end_game = false;
    pid_t pid = -1;
    board_t game_board;

    // Random seed for any random movements
    srand((unsigned int)time(NULL));

    open_debug_file("debug.log");

    terminal_init();

    if (argc != 2) {

        printf("Usage: %s <level_directory>\n", argv[0]);

        while (!end_game) {
            load_static_level(&game_board, accumulated_points);

            draw_board(&game_board, DRAW_MENU);
            refresh_screen();

            while (true) {
                int result = play_board(&game_board); 

                if (result == NEXT_LEVEL) {
                    screen_refresh(&game_board, DRAW_WIN);
                    sleep_ms(game_board.tempo);
                    break;
                }

                if (result == QUIT_GAME) {
                    screen_refresh(&game_board, DRAW_GAME_OVER); 
                    sleep_ms(game_board.tempo);
                    end_game = true;
                    break;
                }
        
                screen_refresh(&game_board, DRAW_MENU); 

                accumulated_points = game_board.pacmans[0].points;      
            }
            print_board(&game_board);
            unload_level(&game_board);
        }

    } else {

        strcpy(game_board.dir_name, argv[1]);
        strcat(game_board.dir_name, "/");
        DIR* dir = opendir(argv[1]);
        if (!dir) {
            perror("Error opening directory");
            exit(EXIT_FAILURE);
        }
        struct dirent* entry;
        int len;

        while (!end_game && (entry = readdir(dir)) != NULL) {
            len = strlen(entry->d_name);
            if (len <= 4 || strcmp(entry->d_name + len - 4, LEVEL) != 0) continue;

            strcpy(game_board.pacman_file, "");
            strcpy(game_board.ghosts_files[0], "");

            read_file(&game_board, entry->d_name, LEVEL, 0);
            strcpy(game_board.level_name, entry->d_name);
            
            load_file_pacman(&game_board,accumulated_points);
            load_file_ghost(&game_board);
            
            draw_board(&game_board, DRAW_MENU);
            refresh_screen();

            pthread_rwlock_init(&game_board.board_lock, NULL);
            pthread_mutex_init(&game_board.info.info_lock, NULL);

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
                            //screen_refresh(&game_board, DRAW_MENU);
                            //sleep_ms(game_board.tempo);
                            //apagar as 2 linhas de cima e fzr:
                            continue;
                        } else {
                            int status;
                            wait(&status);
                            if (WIFEXITED(status)) {
                                if (WEXITSTATUS(status) == QUIT_GAME) {
                                    end_game = true;
                                    break; 
                                } else if (WEXITSTATUS(status) == LOAD_BACKUP) {
                                    //apagar o sleep q vem a seguir para dentro do else if e fzr:
                                    screen_refresh(&game_board, DRAW_MENU);
                                    //sleep_ms(game_board.tempo);
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
    }

    terminal_cleanup();

    close_debug_file();

    return 0;
}