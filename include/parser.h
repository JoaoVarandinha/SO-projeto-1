#ifndef PARSER_H
#define PARSER_H

#include "board.h"
#define MAX_INSTRUCTION_LENGTH 256

int read_line(int fd, char* buf);
void read_file(board_t* board, char* filename, char* filetype, int num);
void process_instruction(board_t* board, char* instruction, char* filetype, int* num);
void process_level_instruction(board_t* board, char* instruction, int* num);
void process_pacman_instruction(board_t* board, char* instruction);
void process_ghost_instruction(board_t* board, char* instruction, int num);

#endif
