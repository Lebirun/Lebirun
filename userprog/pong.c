#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

static void clear_screen(void) {
    int cur = console_getcur();
    console_clear(cur);
}

static void cursor_home(void) {
    console_setcursor(0, 0);
}

#define ROWS 16
#define COLS 40
#define PADDLE_HEIGHT 3
#define PADDLE_WIDTH 1
#define BALL_SIZE 1
#define BOT_FOV 6          
#define UPDATE_INTERVAL 4

#define CELL_EMPTY  0
#define CELL_WALL   1
#define CELL_PADDLE 2
#define CELL_BALL   3

static uint8_t grid[ROWS][COLS];

static struct {
    int x, y;
    unsigned int score;
} player[2];

static struct {
    int x, y;
    int dx, dy;
} ball;

static unsigned int update_interval;
static unsigned int frame_count;
static int pvp_mode;
static int running;

static int abs_val(int x) {
    return (x < 0) ? -x : x;
}

static void init_grid(void) {
    int y, x;
    for (y = 0; y < ROWS; y++) {
        for (x = 0; x < COLS; x++) {
            if (y == 0 || y == ROWS - 1) {
                grid[y][x] = CELL_WALL;
            } else {
                grid[y][x] = CELL_EMPTY;
            }
        }
    }
}

static void clear_moving_objects(void) {
    int y, x;
    for (y = 1; y < ROWS - 1; y++) {
        for (x = 0; x < COLS; x++) {
            grid[y][x] = CELL_EMPTY;
        }
    }
}

static void place_objects(void) {
    int i, py, px;
    
    for (i = 0; i < 2; i++) {
        for (py = 0; py < PADDLE_HEIGHT; py++) {
            int gy = player[i].y + py;
            if (gy > 0 && gy < ROWS - 1) {
                grid[gy][player[i].x] = CELL_PADDLE;
            }
        }
    }
    
    if (ball.y > 0 && ball.y < ROWS - 1 && ball.x >= 0 && ball.x < COLS) {
        grid[ball.y][ball.x] = CELL_BALL;
    }
}

static void draw_game(void) {
    int y, x;
    
    cursor_home();
    
    clear_moving_objects();
    place_objects();
    
    for (y = 0; y < ROWS; y++) {
        for (x = 0; x < COLS; x++) {
            switch (grid[y][x]) {
                case CELL_EMPTY:  putchar(' '); break;
                case CELL_WALL:   putchar('-'); break;
                case CELL_PADDLE: putchar('|'); break;
                case CELL_BALL:   putchar('O'); break;
                default:          putchar('?'); break;
            }
        }
        putchar('\n');
    }
    
    printf("\n  Player 1: %u   |   Player 2: %u\n", player[0].score, player[1].score);
    printf("  [W/S] P1 Move   [I/K] P2 Move   [Q] Quit");
}

static void reset_ball(void) {
    ball.x = COLS / 2;
    ball.y = ROWS / 2;
    ball.dx = (ball.dx > 0) ? -1 : 1;
    ball.dy = 1;
    update_interval = UPDATE_INTERVAL;
}

static void update_ball_pos(void) {
    int next_x, next_y;
    int i;
    
    next_x = ball.x + ball.dx;
    next_y = ball.y + ball.dy;
    
    if (next_y <= 0 || next_y >= ROWS - 1) {
        ball.dy = -ball.dy;
        next_y = ball.y + ball.dy;
    }
    
    for (i = 0; i < 2; i++) {
        int paddle_x = player[i].x;
        int paddle_top = player[i].y;
        int paddle_bottom = player[i].y + PADDLE_HEIGHT - 1;
        
        if ((i == 0 && next_x <= paddle_x + 1 && ball.dx < 0) ||
            (i == 1 && next_x >= paddle_x - 1 && ball.dx > 0)) {
            
            if (next_y >= paddle_top && next_y <= paddle_bottom) {
                ball.dx = -ball.dx;
                next_x = ball.x + ball.dx;
                
                if (update_interval > 2) {
                    update_interval--;
                }
            }
        }
    }
    
    if (next_x <= 0) {
        player[1].score++;
        reset_ball();
        return;
    }
    if (next_x >= COLS - 1) {
        player[0].score++;
        reset_ball();
        return;
    }
    
    ball.x = next_x;
    ball.y = next_y;
}

static void move_paddle(int idx, int direction) {
    int new_y = player[idx].y + direction;
    
    if (new_y >= 1 && new_y + PADDLE_HEIGHT <= ROWS - 1) {
        player[idx].y = new_y;
    }
}

static void ai_move(int idx) {
    int paddle_center, diff;
    
    diff = abs_val(player[idx].x - ball.x);
    if (diff > COLS / BOT_FOV) {
        return;
    }
    
    paddle_center = player[idx].y + PADDLE_HEIGHT / 2;
    
    if (ball.y < paddle_center) {
        move_paddle(idx, -1);
    } else if (ball.y > paddle_center) {
        move_paddle(idx, 1);
    }
}

static void process_input(void) {
    char c;
    int n;
    
    while ((n = read_nb(0, &c, 1)) > 0) {
        switch (c) {
            case 'w':
            case 'W':
                move_paddle(0, -1);
                break;
            case 's':
            case 'S':
                move_paddle(0, 1);
                break;
            
            case 'i':
            case 'I':
                if (pvp_mode) move_paddle(1, -1);
                break;
            case 'k':
            case 'K':
                if (pvp_mode) move_paddle(1, 1);
                break;
            
            case 'q':
            case 'Q':
            case 27:
                running = 0;
                return;
        }
    }
}

static void init_game(void) {
    player[0].x = 2;
    player[0].y = ROWS / 2 - PADDLE_HEIGHT / 2;
    player[0].score = 0;
    
    player[1].x = COLS - 3;
    player[1].y = ROWS / 2 - PADDLE_HEIGHT / 2;
    player[1].score = 0;
    
    ball.x = COLS / 2;
    ball.y = ROWS / 2;
    ball.dx = 1;
    ball.dy = 1;
    
    update_interval = UPDATE_INTERVAL;
    frame_count = 0;
    running = 1;
    pvp_mode = 0;
    
    init_grid();
}

int main(void) {
    clear_screen();
    cursor_home();
    printf("=== PONG for Lebirun OS ===\n\n");
    printf("Controls:\n");
    printf("  W/S  - Move Player 1 (left paddle)\n");
    printf("  I/K  - Move Player 2 (right paddle, PvP only)\n");
    printf("  Q    - Quit game\n\n");
    printf("Press '1' for vs AI, '2' for PvP: \n");
    
    while (1) {
        int c = getchar();
        if (c == '1') {
            pvp_mode = 0;
            printf("vs AI selected\n");
            break;
        } else if (c == '2') {
            pvp_mode = 1;
            printf("PvP selected\n");
            break;
        } else if (c == 'q' || c == 'Q' || c == 27) {
            printf("\nExiting...\n");
            return 0;
        }
    }
    
    printf("\n\nStarting game...\n");
    sleep_ms(500);
    
    init_game();
    
    clear_screen();
    
    while (running) {
        process_input();
        
        if (frame_count % update_interval == 0) {
            update_ball_pos();
            
            if (!pvp_mode) {
                ai_move(1);
            }
        }
        
        draw_game();
        frame_count++;
        
        sleep_ms(30);
    }
    
    clear_screen();
    cursor_home();
    printf("=== GAME OVER ===\n\n");
    printf("Final Score:\n");
    printf("  Player 1: %u\n", player[0].score);
    printf("  Player 2: %u\n", player[1].score);
    
    if (player[0].score > player[1].score) {
        printf("\nPlayer 1 Wins!\n");
    } else if (player[1].score > player[0].score) {
        printf("\nPlayer 2 Wins!\n");
    } else {
        printf("\nIt's a Tie!\n");
    }
    
    return 0;
}