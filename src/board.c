#include "board.h"
#include "parser.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#include <fcntl.h>
#include <string.h>

FILE * debugfile;

// Helper private function to find and kill pacman at specific position
static int find_and_kill_pacman(board_t* board, int new_x, int new_y) {

    for (int p = 0; p < board->n_pacmans; p++) {
        pacman_t* pac = &board->pacmans[p];
        pthread_mutex_lock(&pac->pac_lock);
        if (pac->pos_x == new_x && pac->pos_y == new_y && pac->alive) {
            kill_pacman(board, p);
            pthread_mutex_unlock(&pac->pac_lock);
            return DEAD_PACMAN;
        }
        pthread_mutex_unlock(&pac->pac_lock);
    }

    return VALID_MOVE;
}

// Helper private function for getting board position index
static inline int get_board_index(board_t* board, int x, int y) {
    return y * board->width + x;
}

// Helper private function for checking valid position
static inline int is_valid_position(board_t* board, int x, int y) {
    return (x >= 0 && x < board->width) && (y >= 0 && y < board->height); // Inside of the board boundaries
}

void sleep_ms(int milliseconds) {
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

int move_pacman(board_t* board, int pacman_index, command_t* command) {
    pacman_t* pac = &board->pacmans[pacman_index];

    pthread_mutex_lock(&pac->pac_lock);
    if (pacman_index < 0 || !pac->alive) {
        return DEAD_PACMAN; // Invalid or dead pacman
    }
    pthread_mutex_unlock(&pac->pac_lock);
    
    int new_x = pac->pos_x;
    int new_y = pac->pos_y;

    // check passo
    if (pac->waiting > 0) {
        pac->waiting -= 1;
        return VALID_MOVE;
    }
    pac->waiting = pac->passo;

    char direction = command->command;

    if (direction == 'R') {
        char directions[] = {'W', 'S', 'A', 'D'};
        direction = directions[rand() % 4];
    }

    // Calculate new position based on direction
    switch (direction) {
        case 'W': // Up
            new_y--;
            break;
        case 'S': // Down
            new_y++;
            break;
        case 'A': // Left
            new_x--;
            break;
        case 'D': // Right
            new_x++;
            break;
        case 'T': // Wait
            if (command->turns_left == 1) {
                pac->current_move += 1; // move on
                command->turns_left = command->turns;
            }
            else command->turns_left -= 1;
            return  VALID_MOVE;
        default:
            return INVALID_MOVE; // Invalid direction
    }

    // Logic for the WASD movement
    pac->current_move+=1;

    // Check boundaries
    if (!is_valid_position(board, new_x, new_y)) {
        return INVALID_MOVE;
    }

    int new_index = get_board_index(board, new_x, new_y);
    int old_index = get_board_index(board, pac->pos_x, pac->pos_y);

    if (new_index < old_index) {
        pthread_mutex_lock(&board->board[new_index].pos_lock);
        pthread_mutex_lock(&board->board[old_index].pos_lock);
    } else {
        pthread_mutex_lock(&board->board[old_index].pos_lock);
        pthread_mutex_lock(&board->board[new_index].pos_lock);
    }

    char target_content = board->board[new_index].content;

    if (board->board[new_index].has_portal) {

        board->board[old_index].content = ' ';
        board->board[new_index].content = 'P';

        pthread_mutex_unlock(&board->board[new_index].pos_lock);
        pthread_mutex_unlock(&board->board[old_index].pos_lock);
        return REACHED_PORTAL;
    }

    // Check for walls
    if (target_content == 'W') {
        pthread_mutex_unlock(&board->board[new_index].pos_lock);
        pthread_mutex_unlock(&board->board[old_index].pos_lock);
        return INVALID_MOVE;
    }

    // Check for ghosts
    if (target_content == 'M') {
        pthread_mutex_lock(&pac->pac_lock);
        kill_pacman(board, pacman_index);
        pthread_mutex_unlock(&pac->pac_lock);
        pthread_mutex_unlock(&board->board[new_index].pos_lock);
        pthread_mutex_unlock(&board->board[old_index].pos_lock);
        return DEAD_PACMAN;
    }

    // Collect points
    if (board->board[new_index].has_dot) {
        pac->points++;
        board->board[new_index].has_dot = 0;
    }

    pac->pos_x = new_x;
    pac->pos_y = new_y;

    board->board[old_index].content = ' ';
    board->board[new_index].content = 'P';

    pthread_mutex_unlock(&board->board[new_index].pos_lock);
    pthread_mutex_unlock(&board->board[old_index].pos_lock);

    return VALID_MOVE;
}

// Helper private function for charged ghost movement in one direction
static int move_ghost_charged_direction(board_t* board, ghost_t* ghost, char direction, int* new_x, int* new_y) {

    int x = ghost->pos_x;
    int y = ghost->pos_y;

    *new_x = x;
    *new_y = y;

    int result;
    
    switch (direction) {
        case 'W': // Up
            if (y == 0) return INVALID_MOVE;

            for (int i = 0; i <= y; i++) {
                pthread_mutex_lock(&board->board[i * board->width + x].pos_lock);
            }

            *new_y = 0; // In case there is no colision
            for (int i = y - 1; i >= 0; i--) {
                char target_content = board->board[get_board_index(board, x, i)].content;
                if (target_content == 'W' || target_content == 'M') {
                    *new_y = i + 1; // stop before colision
                    result = VALID_MOVE;
                    break;
                }
                else if (target_content == 'P') {
                    *new_y = i;
                    result = find_and_kill_pacman(board, *new_x, *new_y);
                    break;
                }
            }
            for (int i = 0; i <= y; i++) {
                pthread_mutex_unlock(&board->board[i * board->width + x].pos_lock);
            }
            break;

        case 'S': // Down
            if (y == board->height - 1) return INVALID_MOVE;

            for (int i = y; i < board->height; i++) {
                pthread_mutex_lock(&board->board[i * board->width + x].pos_lock);
            }

            *new_y = board->height - 1; // In case there is no colision
            for (int i = y + 1; i < board->height; i++) {
                char target_content = board->board[get_board_index(board, x, i)].content;
                if (target_content == 'W' || target_content == 'M') {
                    *new_y = i - 1; // stop before colision
                    result = VALID_MOVE;
                    break;
                }
                if (target_content == 'P') {
                    *new_y = i;
                    result = find_and_kill_pacman(board, *new_x, *new_y);
                    break;
                }
            }
            for (int i = y; i < board->height; i++) {
                pthread_mutex_unlock(&board->board[i * board->width + x].pos_lock);
            }
            break;

        case 'A': // Left
            if (x == 0) return INVALID_MOVE;

            for (int j = 0; j <= x; j++) {
                pthread_mutex_lock(&board->board[y * board->width + j].pos_lock);
            }

            *new_x = 0; // In case there is no colision
            for (int j = x - 1; j >= 0; j--) {
                char target_content = board->board[get_board_index(board, j, y)].content;
                if (target_content == 'W' || target_content == 'M') {
                    *new_x = j + 1; // stop before colision
                    result = VALID_MOVE;
                    break;
                }
                if (target_content == 'P') {
                    *new_x = j;
                    result = find_and_kill_pacman(board, *new_x, *new_y);
                    break;
                }
            }
            for (int j = 0; j <= x; j++) {
                pthread_mutex_unlock(&board->board[y * board->width + j].pos_lock);
            }
            break;

        case 'D': // Right
            if (x == board->width - 1) return INVALID_MOVE;

            for (int j = x; j < board->width; j++) {
                pthread_mutex_lock(&board->board[y * board->width + j].pos_lock);
            }

            *new_x = board->width - 1; // In case there is no colision
            for (int j = x + 1; j < board->width; j++) {
                char target_content = board->board[get_board_index(board, j, y)].content;
                if (target_content == 'W' || target_content == 'M') {
                    *new_x = j - 1; // stop before colision
                    result = VALID_MOVE;
                    break;
                }
                if (target_content == 'P') {
                    *new_x = j;
                    result = find_and_kill_pacman(board, *new_x, *new_y);
                    break;
                }
            }
            for (int j = x; j < board->width; j++) {
                pthread_mutex_unlock(&board->board[y * board->width + j].pos_lock);
            }
            break;
        default:
            debug("DEFAULT CHARGED MOVE - direction = %c\n", direction);
            return INVALID_MOVE;
    }
    return result;
}   

int move_ghost_charged(board_t* board, int ghost_index, char direction) {
    ghost_t* ghost = &board->ghosts[ghost_index];

    int old_x = ghost->pos_x;
    int old_y = ghost->pos_y;

    int new_x = old_x;
    int new_y = old_y;

    ghost->charged = 0; //uncharge

    int result = move_ghost_charged_direction(board, ghost, direction, &new_x, &new_y);
    if (result == INVALID_MOVE) {
        debug("DEFAULT CHARGED MOVE - direction = %c\n", direction);
        return INVALID_MOVE;
    }

    // Get board indices
    int old_index = get_board_index(board, old_x, old_y);
    int new_index = get_board_index(board, new_x, new_y);

    if (new_index == old_index) {
        pthread_mutex_lock(&board->board[new_index].pos_lock);
    } else if (new_index < old_index) {
        pthread_mutex_lock(&board->board[new_index].pos_lock);
        pthread_mutex_lock(&board->board[old_index].pos_lock);
    } else {
        pthread_mutex_lock(&board->board[old_index].pos_lock);
        pthread_mutex_lock(&board->board[new_index].pos_lock);
    }


    // Update board - clear old position (restore what was there)
    board->board[old_index].content = ' '; // Or restore the dot if ghost was on one

    // Update ghost position
    ghost->pos_x = new_x;
    ghost->pos_y = new_y;
    
    // Update board - set new position
    board->board[new_index].content = 'M';

    if (new_index == old_index) {
        pthread_mutex_unlock(&board->board[new_index].pos_lock);
    } else {
        pthread_mutex_unlock(&board->board[new_index].pos_lock);
        pthread_mutex_unlock(&board->board[old_index].pos_lock);
    }
    return result;
}

int move_ghost(board_t* board, int ghost_index, command_t* command) {
    ghost_t* ghost = &board->ghosts[ghost_index];

    int new_x = ghost->pos_x;
    int new_y = ghost->pos_y;

    // check passo
    if (ghost->waiting > 0) {
        ghost->waiting -= 1;
        return VALID_MOVE;
    }
    ghost->waiting = ghost->passo;

    char direction = command->command;
    
    if (direction == 'R') {
        char directions[] = {'W', 'S', 'A', 'D'};
        direction = directions[rand() % 4];
    }

    // Calculate new position based on direction
    switch (direction) {
        case 'W': // Up
            new_y--;
            break;
        case 'S': // Down
            new_y++;
            break;
        case 'A': // Left
            new_x--;
            break;
        case 'D': // Right
            new_x++;
            break;
        case 'C': // Charge
            ghost->current_move += 1;
            ghost->charged = 1;
            return VALID_MOVE;
        case 'T': // Wait
            if (command->turns_left == 1) {
                ghost->current_move += 1; // move on
                command->turns_left = command->turns;
            }
            else command->turns_left -= 1;
            return VALID_MOVE;
        default:
            return INVALID_MOVE; // Invalid direction
    }

    // Logic for the WASD movement
    ghost->current_move++;
    if (ghost->charged) {
        int result = move_ghost_charged(board, ghost_index, direction);
        return result;
    }

    // Check boundaries
    if (!is_valid_position(board, new_x, new_y)) {
        return INVALID_MOVE;
    }

    // Check board position
    int new_index = get_board_index(board, new_x, new_y);
    int old_index = get_board_index(board, ghost->pos_x, ghost->pos_y);


    if (new_index < old_index) {
        pthread_mutex_lock(&board->board[new_index].pos_lock);
        pthread_mutex_lock(&board->board[old_index].pos_lock);
    } else {
        pthread_mutex_lock(&board->board[old_index].pos_lock);
        pthread_mutex_lock(&board->board[new_index].pos_lock);
    }

    char target_content = board->board[new_index].content;

    // Check for walls and ghosts
    if (target_content == 'W' || target_content == 'M') {
        pthread_mutex_unlock(&board->board[new_index].pos_lock);
        pthread_mutex_unlock(&board->board[old_index].pos_lock);
        return INVALID_MOVE;
    }

    int result = VALID_MOVE;
    // Check for pacman
    if (target_content == 'P') {
        result = find_and_kill_pacman(board, new_x, new_y);
    }

    // Update ghost position
    ghost->pos_x = new_x;
    ghost->pos_y = new_y;
    
    // Update board - clear old position (restore what was there)
    board->board[old_index].content = ' '; // Or restore the dot if ghost was on one
    // Update board - set new position
    board->board[new_index].content = 'M';

    pthread_mutex_unlock(&board->board[new_index].pos_lock);
    pthread_mutex_unlock(&board->board[old_index].pos_lock);

    return result;
}

void kill_pacman(board_t* board, int pacman_index) {
    debug("Killing %d pacman\n\n", pacman_index);
    pacman_t* pac = &board->pacmans[pacman_index];


    int index = pac->pos_y * board->width + pac->pos_x;

    // Remove pacman from the board
    board->board[index].content = ' ';

    // Mark pacman as dead
    pac->alive = 0;

    return;
}


void load_pacman(board_t* board, int points) {
    board->pacmans = calloc(1, sizeof(pacman_t));
    pacman_t* pac = &board->pacmans[0];

    pac->points = points;
    pac->alive = 1;
    pthread_mutex_init(&pac->pac_lock, NULL);

    int idx;
    if (board->n_pacmans == 0) {
        board->n_pacmans = 1;
        for (idx = 0; idx < board->width * board->height; idx++) {
            if (board->board[idx].content == ' ' && board->board[idx].has_dot) break;
        }
        board->pacmans[0].pos_x = idx % board->width;
        board->pacmans[0].pos_y = idx / board->width;
    } else {
        read_file(board, board->pacman_file, PACMAN, 0);
        idx = pac->pos_y * board->width + pac->pos_x;
    }
    board->board[idx].content = 'P';
}

void load_ghosts(board_t* board) {
    if (board->n_ghosts == 0) {
        exit(EXIT_FAILURE);
    } 
    board->ghosts = calloc(board->n_ghosts, sizeof(ghost_t));
    for (int i = 0; i < board->n_ghosts; i++) {
        read_file(board, board->ghosts_files[i], GHOST, i);
        ghost_t* ghost = &board->ghosts[i];
        board->board[ghost->pos_y * board->width + ghost->pos_x].content = 'M';
    }
}

void load_level(board_t* board, char* filename) {
    board->n_pacmans = 0;
    board->n_ghosts = 0;
    strcpy(board->level_name, filename);
    pthread_rwlock_init(&board->board_lock, NULL);

    read_file(board, filename, LEVEL, 0);
}

void unload_level(board_t * board) {
    pthread_rwlock_destroy(&board->board_lock);
    pthread_mutex_destroy(&board->pacmans[0].pac_lock);
    for (int i = 0; i < board->width * board->height; i++) {
        pthread_mutex_destroy(&board->board[i].pos_lock);
    }
    free(board->board);
    free(board->pacmans);
    free(board->ghosts);
    return;
}

void open_debug_file(char *filename) {
    debugfile = fopen(filename, "w");
}

void close_debug_file() {
    fclose(debugfile);
}

void debug(const char * format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(debugfile, format, args);
    va_end(args);

    fflush(debugfile);
}

void print_board(board_t *board) {
    if (!board || !board->board) {
        debug("[%d] Board is empty or not initialized.\n", getpid());
        return;
    }

    // Large buffer to accumulate the whole output
    char buffer[8192];
    size_t offset = 0;

    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "=== [%d] LEVEL INFO ===\n"
                       "Dimensions: %d x %d\n"
                       "Tempo: %d\n"
                       "Pacman file: %s\n",
                       getpid(), board->height, board->width, board->tempo, board->pacman_file);

    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "Monster files (%d):\n", board->n_ghosts);

    for (int i = 0; i < board->n_ghosts; i++) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                           "  - %s\n", board->ghosts_files[i]);
    }

    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\n=== BOARD ===\n");

    for (int y = 0; y < board->height; y++) {
        for (int x = 0; x < board->width; x++) {
            int idx = y * board->width + x;
            if (offset < sizeof(buffer) - 2) {
                buffer[offset++] = board->board[idx].content;
            }
        }
        if (offset < sizeof(buffer) - 2) {
            buffer[offset++] = '\n';
        }
    }

    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "==================\n");

    buffer[offset] = '\0';

    debug("%s", buffer);
}
