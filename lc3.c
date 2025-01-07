// https://www.jmeiners.com/lc3-vm/supplies/lc3-isa.pdf

#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/termios.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

// Registers
enum {
    R_R0 = 0,  // General purpose (0-7)
    R_R1,
    R_R2,
    R_R3,
    R_R4,
    R_R5,
    R_R6,
    R_R7,
    R_PC,    // Program counter
    R_COND,  // Condition flags
    R_COUNT,
};

// Instruction set (opcodes)
enum {
    OP_BR = 0,  // Branch
    OP_ADD,     // Add
    OP_LD,      // Load
    OP_ST,      // Store
    OP_JSR,     // Jump register
    OP_AND,     // Bitwise AND
    OP_LDR,     // Load register
    OP_STR,     // Store register
    OP_RTI,     // Unused
    OP_NOT,     // Bitwise NOT
    OP_LDI,     // Load indirect
    OP_STI,     // Store indirect
    OP_JMP,     // Jump
    OP_RES,     // Reserved (unused)
    OP_LEA,     // Load effective address
    OP_TRAP,    // Execute trap
};

// Condition flags
enum {
    FL_POS = 1 << 0,  // P
    FL_ZRO = 1 << 1,  // Z
    FL_NEG = 1 << 2,  // N
};

// TRAP Codes
enum {
    TRAP_GETC = 0x20,   // Get a characted from the terminal without echo
    TRAP_OUT = 0x21,    // Output a character
    TRAP_PUTS = 0x22,   // Output a word string
    TRAP_IN = 0x23,     // Get a character from the terminal with echo
    TRAP_PUTSP = 0x24,  // Output a byte string
    TRAP_HALT = 0x25,   // Halt the program
};

// Memory Mapped Registers
enum {
    MR_KBSR = 0xFE00,  // Keyboard status
    MR_KBDR = 0xFE02   // Keyboard data
};

#define MEMORY_MAX (1 << 16)
// Memory Storage
uint16_t memory[MEMORY_MAX];
// Register Storage
uint16_t reg[R_COUNT];

struct termios original_tio;

// Disable terminal input buffering and disable echo
void disable_input_buffering() {
    tcgetattr(STDIN_FILENO, &original_tio);
    struct termios new_tio = original_tio;
    new_tio.c_lflag &= ~ICANON & ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
}

// Restore original terminal settings
void restore_input_buffering() { tcsetattr(STDIN_FILENO, TCSANOW, &original_tio); }

uint16_t check_key() {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    return select(1, &readfds, NULL, NULL, &timeout) != 0;
}

// Handle SIGINT
void handle_interrupt() {
    restore_input_buffering();
    printf("\n");
    exit(-2);
}

// Sign-extend a `bit_count`-bit integer to 16 bits
uint16_t sign_extend(uint16_t x, int bit_count) {
    if ((x >> (bit_count - 1)) & 1) {
        x |= (0xFFFF << bit_count);
    }
    return x;
}

// Swap bytes in a word (change endianness)
uint16_t swap16(uint16_t x) { return (x << 8) | (x >> 8); }

// Update the R_COND register based on the value of `r`
void update_flags(uint16_t r) {
    if (reg[r] == 0) {
        reg[R_COND] = FL_ZRO;
    } else if (reg[r] >> 15) {
        // `1` in the leftmost bit indicates negative
        reg[R_COND] = FL_NEG;
    } else {
        reg[R_COND] = FL_POS;
    }
}

// Read an image file from `file`
// All bytes, including the origin address
// are read as big-endian and converted to little-endian
void read_image_file(FILE* file) {
    // Read the origin first to know where to place the image
    uint16_t origin;
    fread(&origin, sizeof(origin), 1, file);
    origin = swap16(origin);

    uint16_t max_read = MEMORY_MAX - origin;
    uint16_t* p = memory + origin;
    size_t read = fread(p, sizeof(uint16_t), max_read, file);

    // Swap all read bytes to little endian
    while (read-- > 0) {
        *p = swap16(*p);
        ++p;
    }
}

// Read image from a file under `image_path`
int read_image(const char* image_path) {
    FILE* file = fopen(image_path, "rb");
    if (!file) return 0;
    read_image_file(file);
    fclose(file);
    return 1;
}

uint16_t mem_read(uint16_t addr) {
    if (addr == MR_KBSR) {
        if (check_key()) {
            memory[MR_KBSR] = (1 << 15);
            memory[MR_KBDR] = getchar();
        } else {
            memory[MR_KBSR] = 0;
        }
    }
    return memory[addr];
}

void mem_write(uint16_t addr, uint16_t val) { memory[addr] = val; }

int main(int argc, const char* argv[]) {
    if (argc < 2) {
        printf("lc3 [image-file1] ...\n");
        exit(2);
    }
    for (int j = 1; j < argc; ++j) {
        if (!read_image(argv[j])) {
            printf("failed to load image: %s\n", argv[j]);
            exit(1);
        }
    }

    // Setup
    signal(SIGINT, handle_interrupt);
    disable_input_buffering();

    // Exactly one condition flag should be set at any given time
    reg[R_COND] = FL_ZRO;

    // Set PC to the starting position
    enum { PC_START = 0x3000 };
    reg[R_PC] = PC_START;

    int running = 1;
    while (running) {
        // FETCH
        uint16_t inst = mem_read(reg[R_PC]++);
        uint16_t op = inst >> 12;

        switch (op) {
            case OP_ADD: {
                // Destination register (DR)
                uint16_t r0 = (inst >> 9) & 0x7;
                // First operand (SR1)
                uint16_t r1 = (inst >> 6) & 0x7;
                // Whether we are in immediate mode
                uint16_t imm_flag = (inst >> 5) & 0x1;
                if (imm_flag) {
                    uint16_t imm5 = sign_extend(inst & 0x1F, 5);
                    reg[r0] = reg[r1] + imm5;
                } else {
                    uint16_t r2 = inst & 0x7;
                    reg[r0] = reg[r1] + reg[r2];
                }
                update_flags(r0);
                break;
            }
            case OP_AND: {
                // Destination register (DR)
                uint16_t r0 = (inst >> 9) & 0x7;
                // First operand (SR1)
                uint16_t r1 = (inst >> 6) & 0x7;
                // Whether we are in immediate mode
                uint16_t imm_flag = (inst >> 5) & 0x1;
                if (imm_flag) {
                    uint16_t imm5 = sign_extend(inst & 0x1F, 5);
                    reg[r0] = reg[r1] & imm5;
                } else {
                    uint16_t r2 = inst & 0x7;
                    reg[r0] = reg[r1] & reg[r2];
                }
                update_flags(r0);
                break;
            }
            case OP_NOT: {
                // Destination register (DR)
                uint16_t r0 = (inst >> 9) & 0x7;
                // Source register (SR1)
                uint16_t r1 = (inst >> 6) & 0x7;
                reg[r0] = ~reg[r1];
                update_flags(r0);
                break;
            }
            case OP_BR: {
                // 3-bit condition mask
                uint16_t cond_flag = (inst >> 9) & 0x7;
                // PCoffset9
                uint16_t pc_offset = sign_extend(inst & 0x1FF, 9);
                // If the mask AND reg[R_COND] match...
                if (cond_flag & reg[R_COND]) {
                    // ...jump to `reg[PC] + pc_offset`
                    reg[R_PC] = reg[R_PC] + pc_offset;
                }
                break;
            }
            case OP_JMP: {
                // Destination (BaseR)
                uint16_t r1 = (inst >> 6) & 0x7;
                reg[R_PC] = reg[r1];
                break;
            }
            case OP_JSR: {
                reg[R_R7] = reg[R_PC];
                // Whether we are in immediate mode
                uint16_t imm_flag = (inst >> 11) & 0x1;
                if (imm_flag) {  // JSR
                    // Immediate 11-bit value (PCoffset11)
                    uint16_t imm11 = sign_extend(inst & 0x7FF, 11);
                } else {  // JSRR
                    // Base Register (BaseR)
                    uint16_t r1 = (inst >> 6) & 0x7;
                    reg[R_PC] = reg[r1];
                }
                break;
            }
            case OP_LD: {
                // Destination register (DR)
                uint16_t r0 = (inst >> 9) & 0x7;
                // PCoffset9
                uint16_t pc_offset = sign_extend(inst & 0x1FF, 9);
                // Add pc_offset to the current PC...
                uint16_t ptr = reg[R_PC] + pc_offset;
                // ...and follow it to the final location
                reg[r0] = mem_read(ptr);
                update_flags(r0);
                break;
            }
            case OP_LDI: {
                // Destination register (DR)
                uint16_t r0 = (inst >> 9) & 0x7;
                // PCoffset9
                uint16_t pc_offset = sign_extend(inst & 0x1FF, 9);
                // Add pc_offset to the current PC, load an address from memory...
                uint16_t ptr = mem_read(reg[R_PC] + pc_offset);
                // ...and follow it to the final location
                reg[r0] = mem_read(ptr);
                update_flags(r0);
                break;
            }
            case OP_LDR: {
                // Destination register (DR)
                uint16_t r0 = (inst >> 9) & 0x7;
                // Base register (BaseR)
                uint16_t r1 = (inst >> 6) & 0x7;
                // 6-bit offset (offset6)
                uint16_t offset = sign_extend(inst & 0x3F, 6);
                uint16_t ptr = reg[r1] + offset;
                reg[r0] = mem_read(ptr);
                update_flags(r0);
                break;
            }
            case OP_LEA: {
                // Destination register (DR)
                uint16_t r0 = (inst >> 9) & 0x7;
                // PCoffset9
                uint16_t pc_offset = sign_extend(inst & 0x1FF, 9);
                reg[r0] = reg[R_PC] + pc_offset;
                update_flags(r0);
                break;
            }
            case OP_ST: {
                // Source register (SR)
                uint16_t r0 = (inst >> 9) & 0x7;
                // PCoffset9
                uint16_t pc_offset = sign_extend(inst & 0x1FF, 9);
                mem_write(reg[R_PC] + pc_offset, reg[r0]);
                break;
            }
            case OP_STI: {
                // Source register (SR)
                uint16_t r0 = (inst >> 9) & 0x7;
                // PCoffset9
                uint16_t pc_offset = sign_extend(inst & 0x1FF, 9);
                // Follow the pointer and use its value
                uint16_t ptr = mem_read(reg[R_PC] + pc_offset);
                mem_write(ptr, reg[r0]);
                break;
            }
            case OP_STR: {
                // Source register (SR)
                uint16_t r0 = (inst >> 9) & 0x7;
                // Base register (BaseR)
                uint16_t r1 = (inst >> 6) & 0x7;
                // 6-bit offset (offset6)
                uint16_t offset = sign_extend(inst & 0x3F, 6);
                uint16_t ptr = reg[r1] + offset;
                mem_write(ptr, reg[r0]);
                break;
            }
            case OP_TRAP: {
                reg[R_R7] = reg[R_PC];
                uint16_t trap_opcode = inst & 0xFF;
                switch (trap_opcode) {
                    case TRAP_GETC: {
                        // Read a single character from the keyboard. The character is
                        // not echoed onto the console. Its ASCII code is copied into
                        // R0. The high eight bits of R0 are cleared.
                        char c = getchar();
                        reg[R_R0] = (uint16_t)c;
                        update_flags(R_R0);
                        break;
                    }
                    case TRAP_OUT: {
                        // Write a character in R0[7:0] to the console display.
                        putc(reg[R_R0], stdout);
                        fflush(stdout);
                        break;
                    }
                    case TRAP_PUTS: {
                        // Write a string of ASCII characters to the console display.
                        // The characters are contained in consecutive memory locations,
                        // one character per memory location, starting with the address
                        // specified in R0. Writing terminates with the occurrence of
                        // x0000 in a memory location.
                        uint16_t* c = memory + reg[R_R0];
                        // One char per word
                        while (*c) {
                            putc((char)*c, stdout);
                            ++c;
                        }
                        fflush(stdout);
                        break;
                    }
                    case TRAP_IN: {
                        // Print a prompt on the screen and read a single character from
                        // the keyboard. The character is echoed onto the console
                        // monitor, and its ASCII code is copied into R0. The high eight
                        // bits of R0 are cleared.
                        printf("Enter a character: ");
                        char c = getchar();
                        putc(c, stdout);
                        fflush(stdout);
                        reg[R_R0] = (uint16_t)c;
                        update_flags(R_R0);
                        break;
                    }
                    case TRAP_PUTSP: {
                        // Write a string of ASCII characters to the console. The
                        // characters are contained in consecutive memory locations, two
                        // characters per memory location, starting with the address
                        // specified in R0. The ASCII code contained in bits [7:0] of a
                        // memory location is written to the console first. Then the
                        // ASCII code contained in bits [15:8] of that memory location
                        // is written to the console. (A character string consisting of
                        // an odd number of characters to be written will have x00 in
                        // bits [15:8] of the memory location containing the last
                        // character to be written.) Writing terminates with the
                        // occurrence of x0000 in a memory location
                        uint16_t* c = memory + reg[R_R0];
                        // Two chars per word
                        while (*c) {
                            char char1 = (*c) & 0xFF;
                            putc(char1, stdout);
                            char char2 = (*c) >> 8;
                            if (char2) putc(char2, stdout);
                            ++c;
                        }
                        fflush(stdout);
                        break;
                    }
                    case TRAP_HALT: {
                        // Halt execution and print a message on the console.
                        puts("HALT");
                        fflush(stdout);
                        running = 0;
                        break;
                    }
                    default: {
                        abort();
                        break;
                    }
                }
                break;
            }
            case OP_RES:
            case OP_RTI:
            default: {
                abort();
                break;
            }
        }
    }

    // Shutdown
    restore_input_buffering();

    return 0;
}
