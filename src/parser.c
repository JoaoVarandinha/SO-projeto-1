#include "board.h"
#include "parser.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

int read_line(int fd, char* buf) {
    char c;
    ssize_t n;

    while (1) {
        int i = 0;

        while ((n = read(fd, &c, 1)) == 1) {
            if (c == '\r') continue;
            if (c == '\n') break;
            if (c == '#') {
                while (read(fd, &c, 1) == 1 && c != '\n');
                continue;
            }
            buf[i++] = c;
        }

        if (n == -1) {
            perror("Error reading file");
            exit(EXIT_FAILURE);
        };

        if (n == 0 && i == 0) return 0;

        // skip empty lines
        if (i == 0) continue;
        
        buf[i] = '\0';

        return i;
    }
}

void read_file(board_t* board, char* filename, char* filetype, int num) {
    //Go to correct directory and find the file before opening it
    char dirfilename[MAX_FILENAME + MAX_FILENAME];
    sprintf(dirfilename, "%s/%s", board->dir_name, filename);

    int fd = open(dirfilename, O_RDONLY);
    if (fd == -1) {
        perror("Error opening file");
        exit(EXIT_FAILURE);
    }

    char buf[MAX_INSTRUCTION_LENGTH];

    while (read_line(fd, buf) > 0) {
        process_instruction(board, buf, filetype, &num);
    }

    close(fd);
}

void process_instruction(board_t* board, char* instruction, char* filetype, int* num) {
    switch (filetype[1]) {
        case 'l': process_level_instruction(board, instruction, num); return;
        case 'p': process_pacman_instruction(board, instruction); return;
        case 'm': process_ghost_instruction(board, instruction, *num); return;
    }
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
            board->n_pacmans = 1;
            return;
        }   

        case 'M': {
            char* filename = strtok(instruction, " ");
            while ((filename = strtok(NULL, " ")) != NULL) {
                strcpy(board->ghosts_files[board->n_ghosts++], filename);
            }
            return;
        }

        default: {
            for (int i = 0; i < board->width; i++) {
                pthread_mutex_init(&board->board[(*num) * board->width + i].pos_lock, NULL);
                switch (instruction[i]) {
                    case 'X': board->board[(*num) * board->width + i].content = 'W';
                              break;
                    case 'o': board->board[(*num) * board->width + i].content = ' ';
                              board->board[(*num) * board->width + i].has_dot = 1;
                              break;
                    case '@': board->board[(*num) * board->width + i].content = ' ';
                              board->board[(*num) * board->width + i].has_portal = 1;
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