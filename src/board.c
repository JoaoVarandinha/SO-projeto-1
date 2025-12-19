#include "board.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <pthread.h>

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
                    return VALID_MOVE;
                }
                else if (target_content == 'P') {
                    *new_y = i;
                    return find_and_kill_pacman(board, *new_x, *new_y);
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
                    return VALID_MOVE;
                }
                if (target_content == 'P') {
                    *new_y = i;
                    return find_and_kill_pacman(board, *new_x, *new_y);
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
                    return VALID_MOVE;
                }
                if (target_content == 'P') {
                    *new_x = j;
                    return find_and_kill_pacman(board, *new_x, *new_y);
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
                    return VALID_MOVE;
                }
                if (target_content == 'P') {
                    *new_x = j;
                    return find_and_kill_pacman(board, *new_x, *new_y);
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
    return VALID_MOVE;
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
        pthread_rwlock_unlock(&board->board_lock);
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


// Static Loading
void load_static_pacman(board_t* board) {
    int i;
    for (i = 0; i < board->width * board->height; i++) {
            if (board->board[i].content == ' ' && board->board[i].has_dot) break;
    }
    board->board[i].content = 'P'; // Pacman
    board->pacmans[0].pos_x = i % board->width;
    board->pacmans[0].pos_y = i / board->width;
    return;
}

void load_file_pacman(board_t* board, int points) {
    board->n_pacmans = 1;
    board->pacmans = calloc(board->n_pacmans, sizeof(pacman_t));
    pacman_t* pac = &board->pacmans[0];

    pac->points = points;
    pac->alive = 1;
    pthread_mutex_init(&pac->pac_lock, NULL);

    if (strcmp(board->pacman_file, "") == 0) {
        load_static_pacman(board);
    } else {
        read_file(board, board->pacman_file, PACMAN, 0);
        board->board[pac->pos_y * board->width + pac->pos_x].content = 'P';
    }
    return;
}


// Static Loading
void load_static_ghost(board_t* board) {
    // Ghost 0
    board->board[3 * board->width + 1].content = 'M'; // Monster
    board->ghosts[0].pos_x = 1;
    board->ghosts[0].pos_y = 3;
    board->ghosts[0].passo = 0;
    board->ghosts[0].waiting = 0;
    board->ghosts[0].current_move = 0;
    board->ghosts[0].n_moves = 16;
    for (int i = 0; i < 8; i++) {
        board->ghosts[0].moves[i].command = 'D';
        board->ghosts[0].moves[i].turns = 1; 
    }
    for (int i = 8; i < 16; i++) {
        board->ghosts[0].moves[i].command = 'A';
        board->ghosts[0].moves[i].turns = 1; 
    }

    // Ghost 1
    board->board[2 * board->width + 4].content = 'M'; // Monster
    board->ghosts[1].pos_x = 4;
    board->ghosts[1].pos_y = 2;
    board->ghosts[1].passo = 1;
    board->ghosts[1].waiting = 1;
    board->ghosts[1].current_move = 0;
    board->ghosts[1].n_moves = 1;
    board->ghosts[1].moves[0].command = 'R'; // Random
    board->ghosts[1].moves[0].turns = 1;

    return;
}

void load_file_ghost(board_t* board) {
    board->ghosts = calloc(board->n_ghosts, sizeof(ghost_t));
    if (strcmp(board->ghosts_files[0], "") == 0) {
        load_static_ghost(board);
    } else {
        for (int i = 0; i < board->n_ghosts; i++) {
            read_file(board, board->ghosts_files[i], GHOST, i);
            ghost_t* ghost = &board->ghosts[i];
            board->board[ghost->pos_y * board->width + ghost->pos_x].content = 'M';
        }
    }
    return;
}


void process_level_instruction(board_t* board, char* instruction, int* num) {
    switch (instruction[0]) {

        case 'D': {
            sscanf(instruction, "DIM %d %d", &board->width, &board->height);
            board->board = calloc(board->width * board->height, sizeof(board_pos_t));
            return;
        }

        case 'T': {
            sscanf(instruction, "TEMPO %d", &board->tempo);
            return;
        }

        case 'P': {
            sscanf(instruction, "PAC %s", board->pacman_file);
            return;
        }   

        case 'M': {
            int counter = 0;
            char* filename = strtok(instruction, " ");
            while ((filename = strtok(NULL, " ")) != NULL) {
                strcpy(board->ghosts_files[counter], filename);
                counter++;
            }

            board->n_ghosts = counter;
            return;
        }

        default: {
            for (int i = 0; i < board->width; i++) {
                pthread_mutex_init(&board->board[(*num) * board->width + i].pos_lock, NULL);
                switch (instruction[i]) {
                    case 'X': board->board[(*num) * board->width + i].content = 'W';
                              break;
                    case 'o': board->board[(*num) * board->width + i].content = ' ';
                              board->board[(*num) * board->width + i].has_dot = TRUE;
                              break;
                    case '@': board->board[(*num) * board->width + i].content = ' ';
                              board->board[(*num) * board->width + i].has_portal = TRUE;
                              break;
                }
            }
            (*num)++;
            return;
        }
    }
}

void process_pacman_instruction(board_t* board, char* instruction) {
    pacman_t* pac = &board->pacmans[0];
    switch (instruction[0]) {
        case 'P': {
            switch (instruction[1]) {
                case 'A': {
                    sscanf(instruction, "PASSO %d", &pac->passo);
                    return;
                }
                case 'O': {
                    sscanf(instruction, "POS %d %d", &pac->pos_x, &pac->pos_y);
                    return;
                }
            }
            exit(EXIT_FAILURE);
        }

        default: {
            command_t cmd;
            cmd.command = instruction[0];
            cmd.turns = 1;
            if (instruction[0] == 'T') {
                sscanf(instruction, "T %d", &cmd.turns);
                cmd.turns_left = cmd.turns;
            }

            pac->moves[pac->n_moves++] = cmd;

            return;
        }
    }
}

void process_ghost_instruction(board_t* board, char* instruction, int num) {
    ghost_t* ghost = &board->ghosts[num];
    switch (instruction[0]) {
        case 'P': {
            switch (instruction[1]) {
                case 'A': {
                    sscanf(instruction, "PASSO %d", &ghost->passo);
                    return;
                }
                case 'O': {
                    sscanf(instruction, "POS %d %d", &ghost->pos_x, &ghost->pos_y);
                    return;
                }
            }
            exit(EXIT_FAILURE);
        }

        default: {
            command_t cmd;
            cmd.command = instruction[0];
            cmd.turns = 1;
            if (instruction[0] == 'T') {
                sscanf(instruction, "T %d", &cmd.turns);
                cmd.turns_left = cmd.turns;
            }
            ghost->moves[ghost->n_moves++] = cmd;

            return;
        }

    }
}

void process_instruction(board_t* board, char* instruction, char* filetype, int* num) {
    if (instruction[0] == '#' || strcmp(instruction, "") == 0) return;

    switch (filetype[1]) {
        case 'l': process_level_instruction(board, instruction, num); return;
        case 'p': process_pacman_instruction(board, instruction); return;
        case 'm': process_ghost_instruction(board, instruction, *num); return;
    }
}

void read_file(board_t* board, char* filename, char* filetype, int num) {
    //Go to correct directory and find the file before opening it
    char dirfilename[MAX_FILENAME + MAX_DIRLENGTH];
    sprintf(dirfilename, "%s%s", board->dir_name, filename);
    int file = open(dirfilename, O_RDONLY);
    if (file == -1) {
        perror("Error opening file");
        exit(EXIT_FAILURE);
    }

    ssize_t bytesRead;
    char buf[MAXLINELENGTH]; //Initialize buffer
    char instruction[MAXLINELENGTH + MAXLINELENGTH];
    char leftovers[MAXLINELENGTH] = ""; //Initialize leftovers buffer
    int len = sizeof(buf); //Get amount of bytes to read
    while (TRUE) {
        int done = 0; //Set read bytes to 0
        while (done < len) { //Read until buffer is full or EOF is reached
            bytesRead = read(file, buf + done, len - done);
            if (bytesRead == -1) { //Error if can't read
                perror("Error reading level file");
                close(file);
                return;
            }
            if (bytesRead == 0) break; //EOF reached
            
            done += bytesRead;
        }

        if (done == 0) break; //EOF reached
        
        int start = 0;
        for (int i = 0; i < done; i++) {
            if (buf[i] == '\n') { //Search for tokens
                strcpy(instruction, leftovers); //Copy the previous leftovers
                strncat(instruction, buf + start, i - start); //Append the new token to the leftovers

                process_instruction(board, instruction, filetype, &num); //Process the instruction

                leftovers[0] = '\0'; //Reset leftovers from previous buffer
                start = i + 1; //Move to next token
            }
        }

        if (start < done) { // Check for leftovers
            int curr_leftover_len = strlen(leftovers);
            int new_leftover_len = done - start; //Get the length of the leftovers
            strncat(leftovers, buf + start, new_leftover_len); //Copy the leftovers to the leftovers variable
            leftovers[curr_leftover_len + new_leftover_len] = '\0'; //Add null terminator
        }
    }

    if (leftovers[0] != '\0') { //Check for leftovers after file ends
        process_instruction(board, leftovers, filetype, &num); //Process leftovers
    }
    close(file);
}


int load_static_level(board_t *board, int points) {
    board->height = 5;
    board->width = 10;
    board->tempo = 10;

    board->n_ghosts = 2;
    board->n_pacmans = 1;

    board->board = calloc(board->width * board->height, sizeof(board_pos_t));
    board->pacmans = calloc(board->n_pacmans, sizeof(pacman_t));
    board->ghosts = calloc(board->n_ghosts, sizeof(ghost_t));

    sprintf(board->level_name, "Static Level");

    for (int i = 0; i < board->height; i++) {
        for (int j = 0; j < board->width; j++) {
            if (i == 0 || j == 0 || j == (board->width - 1)) {
                board->board[i * board->width + j].content = 'W';
            }
            else if (i == 4 && j == 8) {
                board->board[i * board->width + j].content = ' ';
                board->board[i * board->width + j].has_portal = 1;
            }
            else {
                board->board[i * board->width + j].content = ' ';
                board->board[i * board->width + j].has_dot = 1;
            }
        }
    }

    load_static_ghost(board);
    board->pacmans[0].points = points;
    load_static_pacman(board);

    return 0;
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
