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
    if (game_board->tempo != 0)
        sleep_ms(game_board->tempo);       
}

void set_result(board_t* board, int res) {
    game_info* info = &board->info;

    pthread_rwlock_wrlock(&info->info_lock);
    info->result = res;
    pthread_rwlock_unlock(&info->info_lock);
}

int check_result(board_t* board) {
    game_info* info = &board->info;

    pthread_rwlock_rdlock(&info->info_lock);
    int res = info->result;
    pthread_rwlock_unlock(&info->info_lock);

    return res;
}

void *display_thread(void* arg) {
    board_t *board = (board_t*)arg;
    
    while (check_result(board) == CONTINUE_PLAY) {
        pthread_rwlock_wrlock(&board->board_lock);

        debug("REFRESH\n");
        draw_board(board, DRAW_MENU);
        refresh_screen();

        pthread_rwlock_unlock(&board->board_lock);

        sleep_ms(board->tempo); 
    }
    return NULL;
}

void *pacman_thread(void* arg) {
    board_t* board = (board_t*)arg;
    pacman_t* pacman = &board->pacmans[0];

    while (check_result(board) == CONTINUE_PLAY) {

        command_t* play;
        command_t c;
        if (pacman->n_moves == 0) { // if is user input
            c.command = get_input();

            if (c.command == '\0') {
                set_result(board, CONTINUE_PLAY);
                continue;
            }

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
            set_result(board, QUIT_GAME);
            //return; ???
            continue;
        }

        if (play->command == 'G') {
            pacman->current_move++;
            set_result(board, CREATE_BACKUP);
            //return; ???
            continue;
        }

        int result = move_pacman(board, 0, play);
        if (result == REACHED_PORTAL) {
            // Next level
            set_result(board, NEXT_LEVEL);
            //return; ???
            continue;
        }

        if (result == DEAD_PACMAN) {
            set_result(board, QUIT_GAME);
            //return; ???
            continue;
        }

        set_result(board, CONTINUE_PLAY);

        sleep_ms(board->tempo);

    }

    return NULL;
}

void *ghost_thread(void* arg) {
    ghost_thread_args* args = (ghost_thread_args*)arg;
    board_t* board = args->board;
    ghost_t* ghost = &board->ghosts[args->ghost_idx];

    while (check_result(board) == CONTINUE_PLAY) {
        sleep_ms(board->tempo);
        
        move_ghost(board, args->ghost_idx, &ghost->moves[ghost->current_move%ghost->n_moves]);
    }
    
    return NULL;
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

    pthread_create(&display_tid, NULL, display_thread, board);
    pthread_create(&pac_tid, NULL, pacman_thread, board); //Start pacman thread
    for (int i = 0; i < board->n_ghosts; i++) {
        ghost_thread_args* args = calloc(1, sizeof(*args));

        args->board = board;
        args->ghost_idx = i;

        pthread_create(&ghost_tid[i], NULL, ghost_thread, args);
    }


    pthread_join(display_tid, NULL);
    pthread_join(pac_tid, NULL);
    for (int i = 0; i < board->n_ghosts; i++) {
        pthread_join(ghost_tid[i], NULL);
    }

    return board->info.result;
}


int main(int argc, char** argv) {
    int accumulated_points = 0;
    bool end_game = false;
    pid_t pid = -1;
    board_t game_board;
    game_board.info.result = CONTINUE_PLAY;

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
        DIR* dir = opendir(argv[1]);
        if (!dir) {
            perror("Error opening directory");
            exit(EXIT_FAILURE);
        }
        struct dirent* entry;
        int len;
        pthread_rwlock_init(&game_board.board_lock, NULL);
        pthread_rwlock_init(&game_board.info.info_lock, NULL);

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

            while(true) {

                int result = play_board_threads(&game_board);

                if (result == NEXT_LEVEL) {
                    screen_refresh(&game_board, DRAW_WIN);
                    sleep_ms(game_board.tempo);
                    break;
                }
    
                if (result == QUIT_GAME) {
                    if (pid == 0) {
                        if (game_board.pacmans[0].alive) {
                            exit(QUIT_GAME);
                        } else {
                            exit(LOAD_BACKUP);
                        }
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
                            sleep_ms(game_board.tempo);
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
                                    sleep_ms(game_board.tempo);
                                    continue;
                                }
                            }
                            perror("Error waiting for child");
                            exit(EXIT_FAILURE);
                        }
                    }
                }

            }

            accumulated_points = game_board.pacmans[0].points;
            
            print_board(&game_board);
            unload_level(&game_board);

            if (pid == 0) exit(QUIT_GAME);
    


            for (int i = 0; i < game_board.width * game_board.height; i++) {
                pthread_mutex_destroy(&game_board.board[i].pos_lock);
            }
        }

        closedir(dir);
    }

    terminal_cleanup();

    close_debug_file();

    return 0;
}

