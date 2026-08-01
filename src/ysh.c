#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <pwd.h>
#include <signal.h>
#include <errno.h>
#include <glob.h>

#include <readline/readline.h>
#include <readline/history.h>
#include <readline/keymaps.h> 
#include <ctype.h>

#define MAX_ARGS 512
#define MAX_ALIASES 64
#define MAX_BINDS 32

typedef struct {
    char *name;
    char *value;
} Alias;

typedef struct {
    char *keyseq;  
    char *command; 
} Bind;

Alias aliases[MAX_ALIASES];
int alias_count = 0;

Bind binds[MAX_BINDS];
int bind_count = 0;

char history_path[512] = {0};

static char *ysh_clipboard = NULL;

static int selection_mark = -1;

char color_mistake[64]        = "\x1b[1;31m";
char color_non_mistake[64]    = "\x1b[1;32m";
char color_element_string[64] = "\x1b[1;33m";
char color_element_int[64]    = "\x1b[1;36m";
char color_operators[64]      = "\x1b[1;35m";

const char *builtins[] = {
    "cd",
    "exit",
    "help",
    "alias",
    "export",
    "yshbind",
    "yshlisthistory",
    "yshcleanhistory",
    "yshsetlight",
    "source",
    ".",
    NULL
};

void execute_line(char *line);
const char *get_home_dir(void);
void ysh_redisplay(void);
void print_binds(void);
void print_aliases(void);
int execute_single_command(char *cmd_str);
int handle_custom_bind(int count, int key);

const char* parse_color_name(const char *name) {
    if (strcasecmp(name, "red") == 0)     return "\x1b[1;31m";
    if (strcasecmp(name, "green") == 0)   return "\x1b[1;32m";
    if (strcasecmp(name, "yellow") == 0)  return "\x1b[1;33m";
    if (strcasecmp(name, "blue") == 0)    return "\x1b[1;34m";
    if (strcasecmp(name, "magenta") == 0) return "\x1b[1;35m";
    if (strcasecmp(name, "cyan") == 0)    return "\x1b[1;36m";
    if (strcasecmp(name, "white") == 0)   return "\x1b[1;37m";
    if (strcasecmp(name, "reset") == 0)   return "\x1b[0m";

    if (strcasecmp(name, "rosewater") == 0)   return "\x1b[38;2;245;224;220m";
    if (strcasecmp(name, "flamingo") == 0)    return "\x1b[38;2;242;205;205m";
    if (strcasecmp(name, "pink") == 0)        return "\x1b[38;2;245;194;231m";
    if (strcasecmp(name, "mauve") == 0)       return "\x1b[38;2;203;166;247m";
    if (strcasecmp(name, "red_mocha") == 0)   return "\x1b[38;2;243;139;168m";
    if (strcasecmp(name, "maroon") == 0)      return "\x1b[38;2;235;160;172m";
    if (strcasecmp(name, "peach") == 0)       return "\x1b[38;2;250;179;135m";
    if (strcasecmp(name, "green_mocha") == 0) return "\x1b[38;2;166;227;161m";
    if (strcasecmp(name, "teal") == 0)        return "\x1b[38;2;148;226;213m";
    if (strcasecmp(name, "sapphire") == 0)    return "\x1b[38;2;116;199;236m";
    if (strcasecmp(name, "lavender") == 0)    return "\x1b[38;2;180;191;231m";
    
    return name;
}

void handle_sigint(int sig) {
    (void)sig;
    selection_mark = -1;
    rl_crlf();
    rl_replace_line("", 0);
    rl_on_new_line();
    rl_redisplay();
}

void setup_signals(void) {
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
}

void set_alias(const char *name, const char *value) {
    for (int i = 0; i < alias_count; i++) {
        if (strcmp(aliases[i].name, name) == 0) {
            free(aliases[i].value);
            aliases[i].value = strdup(value);
            return;
        }
    }
    if (alias_count < MAX_ALIASES) {
        aliases[alias_count].name = strdup(name);
        aliases[alias_count].value = strdup(value);
        alias_count++;
    }
}

char *expand_alias(char *cmd) {
    for (int i = 0; i < alias_count; i++) {
        if (strcmp(cmd, aliases[i].name) == 0) {
            return aliases[i].value;
        }
    }
    return NULL;
}

void print_aliases(void) {
    for (int i = 0; i < alias_count; i++) {
        printf("alias %s='%s'\n", aliases[i].name, aliases[i].value);
    }
}

void print_binds(void) {
    for (int i = 0; i < bind_count; i++) {
        printf("yshbind %s \"%s\"\n", binds[i].keyseq, binds[i].command);
    }
}

void trim_quotes(char *str) {
    size_t len = strlen(str);
    if (len >= 2 && ((str[0] == '"' && str[len - 1] == '"') || (str[0] == '\'' && str[len - 1] == '\''))) {
        memmove(str, str + 1, len - 2);
        str[len - 2] = '\0';
    }
}

char *parse_key_notation(const char *input) {
    char *seq = malloc(16);
    if (!seq) return NULL;

    if (strncmp(input, "^", 1) == 0 && strlen(input) == 2) {
        snprintf(seq, 16, "C-%c", tolower((unsigned char)input[1]));
        return seq;
    }
    if (strncasecmp(input, "C-", 2) == 0) {
        snprintf(seq, 16, "C-%c", tolower((unsigned char)input[2]));
        return seq;
    }
    if (strncasecmp(input, "M-", 2) == 0) {
        snprintf(seq, 16, "M-%c", tolower((unsigned char)input[2]));
        return seq;
    }

    strncpy(seq, input, 15);
    seq[15] = '\0';
    return seq;
}

int handle_custom_bind(int count, int key) {
    (void)count;
    (void)key;

    if (!rl_executing_keyseq) return 0;

    char pressed_key[16] = {0};
    const unsigned char *seq = (const unsigned char *)rl_executing_keyseq;

    if (seq[0] == '\x1b' && seq[1] != '\0') {
        snprintf(pressed_key, sizeof(pressed_key), "M-%c", tolower(seq[1]));
    } else if (seq[0] < 32 && seq[0] > 0) {
        snprintf(pressed_key, sizeof(pressed_key), "C-%c", seq[0] + 96);
    } else {
        snprintf(pressed_key, sizeof(pressed_key), "%c", seq[0]);
    }

    for (int i = 0; i < bind_count; i++) {
        if (strcmp(binds[i].keyseq, pressed_key) == 0) {
            rl_crlf();

            char *cmd_copy = strdup(binds[i].command);
            execute_line(cmd_copy);
            free(cmd_copy);

            rl_replace_line("", 0);
            rl_point = 0;
            rl_end = 0;
            rl_on_new_line();
            
            ysh_redisplay(); 
            return 0;
        }
    }

    return 0;
}

void add_yshbind(const char *keyspec, const char *command) {
    char *parsed_seq = parse_key_notation(keyspec);
    if (!parsed_seq) return;

    if (strcmp(parsed_seq, "C-m") == 0 || strcmp(parsed_seq, "C-i") == 0 || strcmp(parsed_seq, "C-h") == 0) {
        fprintf(stderr, "ysh: yshbind: cannot override critical key sequence '%s'\n", keyspec);
        free(parsed_seq);
        return;
    }

    char rl_spec[16] = {0};
    if (strncmp(parsed_seq, "C-", 2) == 0) {
        snprintf(rl_spec, sizeof(rl_spec), "\\C-%c", parsed_seq[2]);
    } else if (strncmp(parsed_seq, "M-", 2) == 0) {
        snprintf(rl_spec, sizeof(rl_spec), "\\e%c", parsed_seq[2]);
    } else {
        strncpy(rl_spec, parsed_seq, sizeof(rl_spec) - 1);
    }

    for (int i = 0; i < bind_count; i++) {
        if (strcmp(binds[i].keyseq, parsed_seq) == 0) {
            free(binds[i].command);
            binds[i].command = strdup(command);
            free(parsed_seq);

            rl_bind_keyseq(rl_spec, handle_custom_bind);
            return;
        }
    }

    if (bind_count < MAX_BINDS) {
        binds[bind_count].keyseq = parsed_seq;
        binds[bind_count].command = strdup(command);

        rl_bind_keyseq(rl_spec, handle_custom_bind);
        bind_count++;
    } else {
        free(parsed_seq);
    }
}

int ysh_shift_left(int count, int key) {
    (void)count; (void)key;
    if (rl_point > 0) {
        if (selection_mark == -1) {
            selection_mark = rl_point;
        }
        rl_point--;
        if (rl_point == selection_mark) {
            selection_mark = -1;
        }
    }
    return 0;
}

int ysh_shift_right(int count, int key) {
    (void)count; (void)key;
    if (rl_point < rl_end) {
        if (selection_mark == -1) {
            selection_mark = rl_point;
        }
        rl_point++;
        if (rl_point == selection_mark) {
            selection_mark = -1;
        }
    }
    return 0;
}

int ysh_move_beginning(int count, int key) {
    (void)count; (void)key;
    selection_mark = -1;
    rl_point = 0;
    return 0;
}

int ysh_move_end(int count, int key) {
    (void)count; (void)key;
    selection_mark = -1;
    rl_point = rl_end;
    return 0;
}

int ysh_copy(int count, int key) {
    (void)count; (void)key;
    if (selection_mark != -1 && selection_mark != rl_point) {
        int start = (selection_mark < rl_point) ? selection_mark : rl_point;
        int end   = (selection_mark < rl_point) ? rl_point : selection_mark;
        int len   = end - start;

        if (ysh_clipboard) free(ysh_clipboard);
        ysh_clipboard = malloc(len + 1);
        if (ysh_clipboard) {
            strncpy(ysh_clipboard, rl_line_buffer + start, len);
            ysh_clipboard[len] = '\0';

            FILE *p = popen("xclip -selection clipboard 2>/dev/null || wl-copy 2>/dev/null", "w");
            if (p) {
                fputs(ysh_clipboard, p);
                pclose(p);
            }
        }
        selection_mark = -1;
    }
    return 0;
}

int ysh_paste(int count, int key) {
    (void)count; (void)key;
    char *text_to_paste = NULL;

    FILE *p = popen("xclip -selection clipboard -o 2>/dev/null || wl-paste -n 2>/dev/null", "r");
    if (p) {
        char buf[2048] = {0};
        size_t n = fread(buf, 1, sizeof(buf) - 1, p);
        pclose(p);
        if (n > 0) {
            buf[n] = '\0';
            text_to_paste = strdup(buf);
        }
    }

    if (!text_to_paste && ysh_clipboard) {
        text_to_paste = strdup(ysh_clipboard);
    }

    if (text_to_paste) {
        rl_insert_text(text_to_paste);
        free(text_to_paste);
    }
    selection_mark = -1;
    return 0;
}

void setup_custom_navigation(void) {
    rl_bind_key(2, ysh_move_beginning); // Ctrl+B
    rl_bind_key(14, ysh_move_end);       // Ctrl+N

    rl_bind_keyseq("\\e[1;5D", rl_backward_word);
    rl_bind_keyseq("\\e[1;5C", rl_forward_word);
    rl_bind_keyseq("\\e[5D", rl_backward_word);
    rl_bind_keyseq("\\e[5C", rl_forward_word);

    rl_bind_keyseq("\\e[1;2D", ysh_shift_left);
    rl_bind_keyseq("\\e[1;2C", ysh_shift_right);

    rl_bind_keyseq("\\e[67;5u", ysh_copy);  // Ctrl+Shift+C
    rl_bind_keyseq("\\e[86;5u", ysh_paste); // Ctrl+Shift+V
    rl_bind_keyseq("\\e[1;6C", ysh_copy);
    rl_bind_keyseq("\\e[1;6V", ysh_paste);
    rl_bind_keyseq("\\ec", ysh_copy);        // Alt+C
    rl_bind_keyseq("\\ev", ysh_paste);       // Alt+V
    rl_bind_key(25, ysh_paste);              // Ctrl+Y (Yank)
}

int is_builtin(const char *cmd) {
    for (int i = 0; builtins[i] != NULL; i++) {
        if (strcmp(cmd, builtins[i]) == 0) return 1;
    }
    return 0;
}

int is_in_path(const char *cmd) {
    if (strchr(cmd, '/')) {
        return access(cmd, X_OK) == 0;
    }
    char *path_env = getenv("PATH");
    if (!path_env) return 0;

    char path_copy[1024];
    strncpy(path_copy, path_env, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

    char *dir = strtok(path_copy, ":");
    while (dir) {
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir, cmd);
        if (access(full_path, X_OK) == 0) {
            return 1;
        }
        dir = strtok(NULL, ":");
    }
    return 0;
}

int is_number(const char *str) {
    if (!str || *str == '\0') return 0;
    for (int i = 0; str[i] != '\0'; i++) {
        if (str[i] < '0' || str[i] > '9') return 0;
    }
    return 1;
}

void ysh_redisplay(void) {
    rl_redisplay();

    if (!rl_line_buffer) {
        fflush(stdout);
        return;
    }

    int visible_prompt_len = 0;
    if (rl_prompt) {
        int ignore = 0;
        for (const char *p = rl_prompt; *p; p++) {
            if (*p == '\001') { ignore = 1; continue; }
            if (*p == '\002') { ignore = 0; continue; }
            if (!ignore) visible_prompt_len++;
        }
    }

    printf("\r");
    if (visible_prompt_len > 0) {
        printf("\x1b[%dC", visible_prompt_len);
    }
    printf("\x1b[K");

    int sel_start = -1, sel_end = -1;
    if (selection_mark != -1 && selection_mark != rl_point) {
        if (selection_mark < rl_point) {
            sel_start = selection_mark;
            sel_end = rl_point;
        } else {
            sel_start = rl_point;
            sel_end = selection_mark;
        }
    }

    char *syntax_on = getenv("YSH_SYNTAX_LIGHTER");
    int use_syntax = (syntax_on && strcmp(syntax_on, "1") == 0);

    int in_quote = 0;
    char quote_char = 0;
    int first_word = 1;
    size_t len = strlen(rl_line_buffer);

    size_t i = 0;
    while (i < len) {
        if ((int)i == sel_start) {
            printf("\x1b[7m");
        }

        if (use_syntax) {
            if ((rl_line_buffer[i] == '"' || rl_line_buffer[i] == '\'') && !in_quote) {
                in_quote = 1;
                quote_char = rl_line_buffer[i];
                printf("%s%c", color_element_string, rl_line_buffer[i++]);
                if ((int)i == sel_end) printf("\x1b[27m");
                continue;
            } else if (in_quote && rl_line_buffer[i] == quote_char) {
                in_quote = 0;
                printf("%c\x1b[0m", rl_line_buffer[i++]);
                if ((int)i == sel_start) printf("\x1b[7m");
                if ((int)i == sel_end) printf("\x1b[27m");
                continue;
            } else if (in_quote) {
                printf("%c", rl_line_buffer[i++]);
                if ((int)i == sel_end) printf("\x1b[27m");
                continue;
            }

            if (rl_line_buffer[i] == '|' || rl_line_buffer[i] == '&' || 
                rl_line_buffer[i] == '>' || rl_line_buffer[i] == '<') {
                printf("%s", color_operators);
                if ((rl_line_buffer[i] == '|' && rl_line_buffer[i+1] == '|') ||
                    (rl_line_buffer[i] == '&' && rl_line_buffer[i+1] == '&') ||
                    (rl_line_buffer[i] == '>' && rl_line_buffer[i+1] == '>')) {
                    printf("%c%c\x1b[0m", rl_line_buffer[i], rl_line_buffer[i+1]);
                    i += 2;
                } else {
                    printf("%c\x1b[0m", rl_line_buffer[i++]);
                }
                if ((int)i >= sel_start && (int)i < sel_end) printf("\x1b[7m");
                if ((int)i == sel_end) printf("\x1b[27m");
                first_word = 1;
                continue;
            }

            if (rl_line_buffer[i] == ' ' || rl_line_buffer[i] == '\t') {
                printf("%c", rl_line_buffer[i++]);
                if ((int)i == sel_end) printf("\x1b[27m");
                continue;
            }

            size_t start = i;
            while (i < len && rl_line_buffer[i] != ' ' && rl_line_buffer[i] != '\t' && 
                   rl_line_buffer[i] != '"' && rl_line_buffer[i] != '\'' &&
                   rl_line_buffer[i] != '|' && rl_line_buffer[i] != '&' &&
                   rl_line_buffer[i] != '>' && rl_line_buffer[i] != '<') {
                i++;
            }

            char word[256];
            size_t word_len = i - start;
            if (word_len >= sizeof(word)) word_len = sizeof(word) - 1;
            strncpy(word, rl_line_buffer + start, word_len);
            word[word_len] = '\0';

            if (first_word) {
                if (is_builtin(word) || expand_alias(word) != NULL || is_in_path(word)) {
                    printf("%s", color_non_mistake);
                } else {
                    printf("%s", color_mistake);
                }
                first_word = 0;
            } else {
                if (word[0] == '-' || is_number(word)) {
                    printf("%s", color_element_int);
                }
            }

            for (size_t w = start; w < i; w++) {
                if ((int)w == sel_start) printf("\x1b[7m");
                printf("%c", rl_line_buffer[w]);
                if ((int)(w + 1) == sel_end) printf("\x1b[27m");
            }
            printf("\x1b[0m");
            if ((int)i >= sel_start && (int)i < sel_end) printf("\x1b[7m");
        } else {
            printf("%c", rl_line_buffer[i++]);
        }

        if ((int)i == sel_end) {
            printf("\x1b[27m\x1b[0m");
        }
    }
    printf("\x1b[0m");

    size_t move_back = len - rl_point;
    if (move_back > 0) {
        printf("\x1b[%zuD", move_back);
    }

    fflush(stdout);
}

void process_prompt_escapes(const char *src, char *dest, size_t max_len) {
    size_t i = 0, j = 0;
    while (src[i] != '\0' && j < max_len - 1) {
        if ((src[i] == '\\' && src[i+1] == '0' && src[i+2] == '3' && src[i+3] == '3') ||
            (src[i] == '\\' && src[i+1] == 'e') ||
            (src[i] == '\\' && src[i+1] == 'x' && src[i+2] == '1' && src[i+3] == 'b')) {
            
            int skip = (src[i+1] == 'e') ? 2 : 4;
            i += skip;

            if (j < max_len - 3) {
                dest[j++] = '\001';
                dest[j++] = '\x1b';
            }

            while (src[i] != '\0' && j < max_len - 2) {
                dest[j++] = src[i];
                if (src[i] == 'm') { i++; break; }
                i++;
            }

            if (j < max_len - 1) {
                dest[j++] = '\002';
            }
        } 
        else if (src[i] == '\\' && src[i+1] == 'w') {
            char cwd[256];
            if (getcwd(cwd, sizeof(cwd))) {
                const char *home = get_home_dir();
                char formatted_cwd[256];
                
                if (home && strncmp(cwd, home, strlen(home)) == 0) {
                    snprintf(formatted_cwd, sizeof(formatted_cwd), "~%s", cwd + strlen(home));
                } else {
                    strncpy(formatted_cwd, cwd, sizeof(formatted_cwd) - 1);
                    formatted_cwd[sizeof(formatted_cwd) - 1] = '\0';
                }

                size_t len = strlen(formatted_cwd);
                if (j + len < max_len) {
                    strcpy(dest + j, formatted_cwd);
                    j += len;
                }
            }
            i += 2;
        }
        else if (src[i] == '\\' && src[i+1] == 'u') {
            char *user = getenv("USER");
            if (!user) user = "user";
            size_t len = strlen(user);
            if (j + len < max_len) {
                strcpy(dest + j, user);
                j += len;
            }
            i += 2;
        } 
        else {
            dest[j++] = src[i++];
        }
    }
    dest[j] = '\0';
}

void print_history_cmd(int limit_last, int limit_first) {
    HIST_ENTRY **the_list = history_list();
    if (!the_list) return;

    int total = 0;
    while (the_list[total]) total++;

    int start = 0;
    int end = total;

    if (limit_last > 0 && limit_last < total) {
        start = total - limit_last;
    }
    else if (limit_first > 0) {
        if (limit_first < total) {
            end = limit_first;
        }
    }

    for (int i = start; i < end; i++) {
        printf("%5d  %s\n", i + history_base, the_list[i]->line);
    }
}

void clean_history_cmd(void) {
    clear_history();
    if (history_path[0] != '\0') {
        write_history(history_path);
    }
}

int execute_source_command(const char *filename) {
    if (!filename) {
        fprintf(stderr, "ysh: source: filename argument required\n");
        return 1;
    }

    char resolved_path[1024];
    if (filename[0] == '~' && (filename[1] == '/' || filename[1] == '\0')) {
        const char *home = get_home_dir();
        snprintf(resolved_path, sizeof(resolved_path), "%s%s", home ? home : "", filename + 1);
    } else {
        strncpy(resolved_path, filename, sizeof(resolved_path) - 1);
        resolved_path[sizeof(resolved_path) - 1] = '\0';
    }

    FILE *file = fopen(resolved_path, "r");
    if (!file) {
        fprintf(stderr, "ysh: source: %s: %s\n", resolved_path, strerror(errno));
        return 1;
    }

    char *line = NULL;
    size_t len = 0;
    while (getline(&line, &len, file) != -1) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '#' && line[1] == '!') continue;
        execute_line(line);
    }

    free(line);
    fclose(file);
    return 0;
}

static void expand_wildcards(char **raw_args, int raw_count, char **expanded_args, int *expanded_count) {
    *expanded_count = 0;

    for (int i = 0; i < raw_count; i++) {
        if (strchr(raw_args[i], '*') || strchr(raw_args[i], '?')) {
            glob_t glob_result;
            memset(&glob_result, 0, sizeof(glob_result));

            int res = glob(raw_args[i], GLOB_NOCHECK | GLOB_TILDE, NULL, &glob_result);
            if (res == 0) {
                for (size_t j = 0; j < glob_result.gl_pathc; j++) {
                    if (*expanded_count < MAX_ARGS - 1) {
                        expanded_args[(*expanded_count)++] = strdup(glob_result.gl_pathv[j]);
                    }
                }
                globfree(&glob_result);
                continue;
            }
        }

        if (*expanded_count < MAX_ARGS - 1) {
            expanded_args[(*expanded_count)++] = strdup(raw_args[i]);
        }
    }
    expanded_args[*expanded_count] = NULL;
}

char *expand_variables_and_substitutions(const char *input) {
    size_t in_len = strlen(input);
    char *result = malloc(8192);
    if (!result) return strdup(input);
    
    size_t r = 0;
    size_t i = 0;

    while (i < in_len && r < 8180) {
        if (input[i] == '$' && input[i+1] == '(') {
            i += 2;
            size_t cmd_start = i;
            int depth = 1;

            while (i < in_len && depth > 0) {
                if (input[i] == '(') depth++;
                else if (input[i] == ')') depth--;
                if (depth > 0) i++;
            }

            if (depth == 0) {
                size_t cmd_len = i - cmd_start;
                char sub_cmd[1024] = {0};
                if (cmd_len >= sizeof(sub_cmd)) cmd_len = sizeof(sub_cmd) - 1;
                strncpy(sub_cmd, input + cmd_start, cmd_len);
                i++;

                FILE *fp = popen(sub_cmd, "r");
                if (fp) {
                    char buf[1024];
                    while (fgets(buf, sizeof(buf), fp)) {
                        size_t blen = strlen(buf);
                        while (blen > 0 && (buf[blen-1] == '\n' || buf[blen-1] == '\r')) {
                            buf[--blen] = '\0';
                        }
                        if (r + blen < 8180) {
                            strcpy(result + r, buf);
                            r += blen;
                        }
                    }
                    pclose(fp);
                }
                continue;
            } else {
                i = cmd_start - 2;
            }
        }

        if (input[i] == '$') {
            i++;
            char var_name[256] = {0};
            size_t v = 0;

            if (input[i] == '{') {
                i++;
                while (i < in_len && input[i] != '}' && v < sizeof(var_name) - 1) {
                    var_name[v++] = input[i++];
                }
                if (input[i] == '}') i++;
            } else {
                while (i < in_len && (isalnum((unsigned char)input[i]) || input[i] == '_') && v < sizeof(var_name) - 1) {
                    var_name[v++] = input[i++];
                }
            }

            if (v > 0) {
                char *val = getenv(var_name);
                if (val) {
                    size_t vlen = strlen(val);
                    if (r + vlen < 8180) {
                        strcpy(result + r, val);
                        r += vlen;
                    }
                }
                continue;
            } else {
                result[r++] = '$';
                continue;
            }
        }

        result[r++] = input[i++];
    }

    result[r] = '\0';
    return result;
}

int execute_single_command(char *cmd_str) {
    char *raw_args[MAX_ARGS];
    int raw_arg_count = 0;

    char *input_file = NULL;
    char *output_file = NULL;
    int append_mode = 0;

    char *token = strtok(cmd_str, " \t\n\r");
    while (token != NULL) {
        if (strcmp(token, "<") == 0) {
            token = strtok(NULL, " \t\n\r");
            if (token) input_file = token;
        } else if (strcmp(token, ">") == 0) {
            token = strtok(NULL, " \t\n\r");
            if (token) { output_file = token; append_mode = 0; }
        } else if (strcmp(token, ">>") == 0) {
            token = strtok(NULL, " \t\n\r");
            if (token) { output_file = token; append_mode = 1; }
        } else {
            if (raw_arg_count < MAX_ARGS - 1) {
                raw_args[raw_arg_count++] = token;
            }
        }
        token = strtok(NULL, " \t\n\r");
    }
    raw_args[raw_arg_count] = NULL;

    if (raw_arg_count == 0) return 0;

    char *args[MAX_ARGS];
    int arg_count = 0;
    expand_wildcards(raw_args, raw_arg_count, args, &arg_count);

    if (arg_count == 0) return 0;

    int ret_val = 0;

    if (strcmp(args[0], "source") == 0 || strcmp(args[0], ".") == 0) {
        ret_val = execute_source_command(args[1]);
        goto cleanup;
    }

    if (strcmp(args[0], "yshsetlight") == 0) {
        if (arg_count < 3) {
            fprintf(stderr, "ysh: yshsetlight: usage: yshsetlight <element> <color>\n");
            ret_val = 1;
            goto cleanup;
        }

        char *target = args[1];
        char *val = args[2];
        trim_quotes(val);
        const char *parsed = parse_color_name(val);

        if (strcmp(target, "mistake") == 0) {
            strncpy(color_mistake, parsed, sizeof(color_mistake) - 1);
        } else if (strcmp(target, "non-mistake") == 0) {
            strncpy(color_non_mistake, parsed, sizeof(color_non_mistake) - 1);
        } else if (strcmp(target, "element_string") == 0) {
            strncpy(color_element_string, parsed, sizeof(color_element_string) - 1);
        } else if (strcmp(target, "element_int") == 0) {
            strncpy(color_element_int, parsed, sizeof(color_element_int) - 1);
        } else if (strcmp(target, "operators") == 0) {
            strncpy(color_operators, parsed, sizeof(color_operators) - 1);
        } else {
            fprintf(stderr, "ysh: yshsetlight: unknown target element '%s'\n", target);
            ret_val = 1;
            goto cleanup;
        }
        ret_val = 0;
        goto cleanup;
    }

    if (strcmp(args[0], "yshcleanhistory") == 0) {
        clean_history_cmd();
        ret_val = 0;
        goto cleanup;
    }

    if (strcmp(args[0], "yshlisthistory") == 0) {
        int count_last = -1;
        int count_first = -1;

        if (arg_count >= 3 && strcmp(args[1], "-c") == 0) {
            count_last = atoi(args[2]);
        } else if (arg_count >= 3 && strcmp(args[1], "-l") == 0) {
            count_first = atoi(args[2]);
        }
        print_history_cmd(count_last, count_first);
        ret_val = 0;
        goto cleanup;
    }

    if (strcmp(args[0], "yshbind") == 0) {
        if (arg_count == 1) {
            print_binds();
        } else if (arg_count >= 3) {
            char full_cmd[512] = {0};
            for (int i = 2; i < arg_count; i++) {
                strcat(full_cmd, args[i]);
                if (i < arg_count - 1) strcat(full_cmd, " ");
            }
            trim_quotes(full_cmd);
            add_yshbind(args[1], full_cmd);
        }
        ret_val = 0;
        goto cleanup;
    }

    if (strcmp(args[0], "alias") == 0) {
        if (arg_count == 1) {
            print_aliases();
        } else {
            char full_alias_expr[512] = {0};
            for (int i = 1; i < arg_count; i++) {
                strcat(full_alias_expr, args[i]);
                if (i < arg_count - 1) strcat(full_alias_expr, " ");
            }

            char *eq = strchr(full_alias_expr, '=');
            if (eq) {
                *eq = '\0';
                char *name = full_alias_expr;
                char *val = eq + 1;
                trim_quotes(val);
                set_alias(name, val);
            }
        }
        ret_val = 0;
        goto cleanup;
    }

    char *expanded = expand_alias(args[0]);
    if (expanded) {
        char new_line[1024] = {0};
        snprintf(new_line, sizeof(new_line), "%s", expanded);
        for (int i = 1; i < arg_count; i++) {
            strcat(new_line, " ");
            strcat(new_line, args[i]);
        }
        ret_val = execute_single_command(new_line);
        goto cleanup;
    }

    if (strcmp(args[0], "cd") == 0) {
        char *target = args[1];
        char expanded_path[1024];

        if (!target) {
            target = (char *)get_home_dir();
        } else if (target[0] == '~' && (target[1] == '/' || target[1] == '\0')) {
            const char *home = get_home_dir();
            snprintf(expanded_path, sizeof(expanded_path), "%s%s", home ? home : "", target + 1);
            target = expanded_path;
        }

        if (target && chdir(target) != 0) {
            fprintf(stderr, "ysh: cd: %s: %s\n", target, strerror(errno));
            ret_val = 1;
            goto cleanup;
        }
        ret_val = 0;
        goto cleanup;
    }

    if (strcmp(args[0], "exit") == 0) {
        if (history_path[0] != '\0') write_history(history_path);
        exit(0);
    }

    if (strcmp(args[0], "help") == 0) {
        printf("ysh - Simple C Shell\nBuilt-in: cd, exit, help, alias, export, yshbind, yshlisthistory, yshcleanhistory, yshsetlight, source, .\nOperators: |, ||, &&, >, >>, <\nWildcard: *\nVariables: NAME=val, $NAME, $(cmd)\nShortcuts: Shift+Arrows (select), Ctrl+Shift+C (copy), Ctrl+Shift+V (paste)\n");
        ret_val = 0;
        goto cleanup;
    }

    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);

        if (input_file) {
            int fd = open(input_file, O_RDONLY);
            if (fd < 0) {
                fprintf(stderr, "ysh: %s: %s\n", input_file, strerror(errno));
                exit(1);
            }
            dup2(fd, STDIN_FILENO);
            close(fd);
        }
        if (output_file) {
            int flags = O_WRONLY | O_CREAT | (append_mode ? O_APPEND : O_TRUNC);
            int fd = open(output_file, flags, 0644);
            if (fd < 0) {
                fprintf(stderr, "ysh: %s: %s\n", output_file, strerror(errno));
                exit(1);
            }
            dup2(fd, STDOUT_FILENO);
            close(fd);
        }

        execvp(args[0], args);

        if (errno == ENOENT) {
            fprintf(stderr, "ysh: %s: command not found\n", args[0]);
            exit(127);
        } else if (errno == EACCES) {
            fprintf(stderr, "ysh: %s: Permission denied\n", args[0]);
            exit(126);
        } else {
            fprintf(stderr, "ysh: %s: %s\n", args[0], strerror(errno));
            exit(1);
        }
    } else if (pid < 0) {
        fprintf(stderr, "ysh: fork failed: %s\n", strerror(errno));
        ret_val = -1;
        goto cleanup;
    } else {
        int status;
        waitpid(pid, &status, 0);

        if (WIFSIGNALED(status)) {
            if (WTERMSIG(status) == SIGINT) {
                printf("\n");
            }
            ret_val = 128 + WTERMSIG(status);
            goto cleanup;
        }

        ret_val = WEXITSTATUS(status);
        goto cleanup;
    }

cleanup:
    for (int i = 0; i < arg_count; i++) {
        free(args[i]);
    }
    return ret_val;
}

int execute_pipeline(char *line) {
    char *cmds[MAX_ARGS];
    int num_cmds = 0;

    char *cur = line;
    cmds[num_cmds++] = cur;
    while ((cur = strchr(cur, '|')) != NULL) {
        if (*(cur + 1) == '|') {
            cur += 2;
            continue;
        }
        *cur = '\0';
        cur++;
        cmds[num_cmds++] = cur;
    }

    if (num_cmds == 1) {
        return execute_single_command(cmds[0]);
    }

    int pipefds[2 * (num_cmds - 1)];
    for (int i = 0; i < (num_cmds - 1); i++) {
        if (pipe(pipefds + i*2) < 0) {
            fprintf(stderr, "ysh: pipe failed: %s\n", strerror(errno));
            return -1;
        }
    }

    int status = 0;
    for (int i = 0; i < num_cmds; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            if (i != 0) {
                dup2(pipefds[(i - 1) * 2], STDIN_FILENO);
            }
            if (i != num_cmds - 1) {
                dup2(pipefds[i * 2 + 1], STDOUT_FILENO);
            }
            for (int j = 0; j < 2 * (num_cmds - 1); j++) {
                close(pipefds[j]);
            }
            exit(execute_single_command(cmds[i]));
        }
    }

    for (int i = 0; i < 2 * (num_cmds - 1); i++) {
        close(pipefds[i]);
    }

    for (int i = 0; i < num_cmds; i++) {
        wait(&status);
    }

    return WEXITSTATUS(status);
}

void execute_line(char *raw_line) {
    if (!raw_line) return;

    while (*raw_line == ' ' || *raw_line == '\t') raw_line++;
    if (*raw_line == '\0' || *raw_line == '#') return;

    char *line = expand_variables_and_substitutions(raw_line);

    char *eq = strchr(line, '=');
    if (eq && eq != line) {
        int valid_name = 1;
        for (char *p = line; p < eq; p++) {
            if (!isalnum((unsigned char)*p) && *p != '_') {
                valid_name = 0;
                break;
            }
        }
        if (valid_name) {
            *eq = '\0';
            char *var = line;
            char *val = eq + 1;
            trim_quotes(val);
            if (strcmp(var, "PS1") == 0) {
                setenv("YSH_PS1", val, 1);
            } else {
                setenv(var, val, 1);
            }
            free(line);
            return;
        }
    }

    if (strncmp(line, "export", 6) == 0 && (line[6] == ' ' || line[6] == '\t' || line[6] == '\0')) {
        char *ptr = line + 6;
        while (*ptr == ' ' || *ptr == '\t') ptr++;
        if (*ptr != '\0') {
            char *eq_ptr = strchr(ptr, '=');
            if (eq_ptr) {
                *eq_ptr = '\0';
                char *var = ptr;
                char *val = eq_ptr + 1;
                trim_quotes(val);
                if (strcmp(var, "PS1") == 0) {
                    setenv("YSH_PS1", val, 1);
                } else {
                    setenv(var, val, 1);
                }
            }
        }
        free(line);
        return;
    }

    char *ptr = line;
    int last_status = 0;
    int next_op = 0;

    while (*ptr) {
        char *start = ptr;
        while (*ptr) {
            if (*ptr == '&' && *(ptr+1) == '&') {
                *ptr = '\0';
                ptr += 2;
                next_op = 1;
                break;
            } else if (*ptr == '|' && *(ptr+1) == '|') {
                *ptr = '\0';
                ptr += 2;
                next_op = 2;
                break;
            }
            ptr++;
        }

        last_status = execute_pipeline(start);

        if (next_op == 1 && last_status != 0) break;
        if (next_op == 2 && last_status == 0) break;
        next_op = 0;
    }

    free(line);
}

const char *get_home_dir(void) {
    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    return home;
}

void load_yshrc(void) {
    const char *home = get_home_dir();
    if (!home) return;

    char rc_path[512];
    snprintf(rc_path, sizeof(rc_path), "%s/.yshrc", home);

    FILE *file = fopen(rc_path, "r");
    if (!file) return;

    char *line = NULL;
    size_t len = 0;
    while (getline(&line, &len, file) != -1) {
        line[strcspn(line, "\r\n")] = 0;
        execute_line(line);
    }
    free(line);
    fclose(file);
}

void init_history(void) {
    using_history();
    const char *home = get_home_dir();
    if (home) {
        snprintf(history_path, sizeof(history_path), "%s/.ysh_history", home);
        read_history(history_path);
    }
}

void build_prompt(char *out_prompt, size_t max_size) {
    char *ps1 = getenv("YSH_PS1");
    if (ps1 && strlen(ps1) > 0) {
        process_prompt_escapes(ps1, out_prompt, max_size);
        return;
    }

    char cwd[512];
    if (!getcwd(cwd, sizeof(cwd))) {
        strcpy(cwd, "?");
    }

    const char *home = get_home_dir();
    char formatted_cwd[512];
    if (home && strncmp(cwd, home, strlen(home)) == 0) {
        snprintf(formatted_cwd, sizeof(formatted_cwd), "~%s", cwd + strlen(home));
    } else {
        strncpy(formatted_cwd, cwd, sizeof(formatted_cwd) - 1);
        formatted_cwd[sizeof(formatted_cwd) - 1] = '\0';
    }

    char *user = getenv("USER");
    if (!user) user = "user";

    snprintf(out_prompt, max_size, 
             "\001\033[1;32m\002%s\001\033[0m\002:\001\033[1;34m\002%s\001\033[0m\002$ ", 
             user, formatted_cwd);
}

char *command_generator(const char *text, int state) {
    static int list_index, len;
    static DIR *dir = NULL;
    static char *path_copy = NULL;
    static char *dir_path = NULL;
    char *name;

    if (!state) {
        list_index = 0;
        len = strlen(text);
        if (dir) { closedir(dir); dir = NULL; }
        if (path_copy) { free(path_copy); path_copy = NULL; }
        
        char *path_env = getenv("PATH");
        if (path_env) path_copy = strdup(path_env);
        if (path_copy) dir_path = strtok(path_copy, ":");
    }

    while ((name = (char *)builtins[list_index])) {
        list_index++;
        if (strncmp(name, text, len) == 0) {
            return strdup(name);
        }
    }

    for (int i = 0; i < alias_count; i++) {
        if (strncmp(aliases[i].name, text, len) == 0) {
            return strdup(aliases[i].name);
        }
    }

    while (dir_path != NULL) {
        if (!dir) {
            dir = opendir(dir_path);
        }

        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (strncmp(entry->d_name, text, len) == 0) {
                    if (entry->d_name[0] != '.') {
                        return strdup(entry->d_name);
                    }
                }
            }
            closedir(dir);
            dir = NULL;
        }
        dir_path = strtok(NULL, ":");
    }

    return NULL;
}

char **ysh_completion(const char *text, int start, int end) {
    (void)end;
    char **matches = NULL;

    if (start == 0) {
        matches = rl_completion_matches(text, command_generator);
    }
    
    return matches;
}

int run_script_file(const char *filename, int argc, char **argv) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        fprintf(stderr, "ysh: %s: %s\n", filename, strerror(errno));
        return 1;
    }

    for (int i = 2; i < argc; i++) {
        char var_name[16];
        snprintf(var_name, sizeof(var_name), "%d", i - 1);
        setenv(var_name, argv[i], 1);
    }

    char *line = NULL;
    size_t len = 0;
    while (getline(&line, &len, file) != -1) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '#' && line[1] == '!') continue;
        execute_line(line);
    }

    free(line);
    fclose(file);
    return 0;
}

int main(int argc, char **argv) {
    system("stty werase undef 2>/dev/null");

    setup_signals();

    rl_variable_bind("convert-meta", "off");
    rl_variable_bind("input-meta", "on");
    rl_variable_bind("output-meta", "on");
    
    setup_custom_navigation();

    rl_attempted_completion_function = ysh_completion;
    rl_redisplay_function = ysh_redisplay;

    init_history();

    load_yshrc();

    if (argc > 1) {
        return run_script_file(argv[1], argc, argv);
    }
    
    char prompt[1024];
    char *input;

    while (1) {
        build_prompt(prompt, sizeof(prompt));

        selection_mark = -1;
        input = readline(prompt);

        if (!input) {
            printf("exit\n");
            if (history_path[0] != '\0') {
                write_history(history_path);
            }
            break;
        }

        if (*input) {
            add_history(input);
            if (history_path[0] != '\0') {
                append_history(1, history_path);
            }
            execute_line(input);
        }

        free(input);
    }

    if (ysh_clipboard) free(ysh_clipboard);
    return 0;
}
