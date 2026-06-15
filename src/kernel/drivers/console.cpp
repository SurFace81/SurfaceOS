#include "../../include/drivers/console.h"
#include "../../include/drivers/uart.h"
#include "../../include/stdlib/string.h"
#include "../../include/drivers/commands.h"
#include "../version.h"

#define MAX_COMMANDS 32
#define HISTORY_SIZE 32

struct Command
{
    const char* name;
    command_fn handler;
};

static Command cmd_table[MAX_COMMANDS];
static uint32_t cmd_count = 0;

static list::List<char>* input_buf;
static uint32_t input_pos = 0;

// Temporary buffers for parsing
static char line_buf[CONSOLE_INPUT_MAX];
static char* argv_buf[CONSOLE_MAX_ARGS];

// Command history
static char history[HISTORY_SIZE][CONSOLE_INPUT_MAX];
static uint32_t history_count = 0;
static uint32_t history_write = 0;   // ring buffer write position
static sint32_t history_browse = -1; // current browsing index, -1 = not browsing

// Save current input line to history ring buffer
static void history_push(const char* line)
{
    if (line[0] == '\0')
        return;

    // Don't save duplicates of the last entry
    if (history_count > 0)
    {
        uint32_t last = (history_write + HISTORY_SIZE - 1) % HISTORY_SIZE;
        if (strcmp(history[last], line) == 0)
            return;
    }

    strcpy(history[history_write], line);
    history_write = (history_write + 1) % HISTORY_SIZE;
    if (history_count < HISTORY_SIZE)
        history_count++;
}

// Get entry from history by browse offset (0 = most recent)
static const char* history_get(uint32_t offset)
{
    if (offset >= history_count)
        return nullptr;

    uint32_t idx = (history_write + HISTORY_SIZE - 1 - offset) % HISTORY_SIZE;
    return history[idx];
}

// Erase current input line from screen, replace buffer, redraw
static void replace_input(const char* new_text)
{
    // Erase current visible input (everything after "> ")
    uint32_t old_len = list::size(input_buf);
    for (uint32_t i = 0; i < old_len; i++)
    {
        uint32_t cx = screen::cursor_x();
        uint32_t cy = screen::cursor_y();

        if (cx > 0)
            cx--;
        else if (cy > 0)
        {
            cy--;
            cx = screen::cols() - 1;
        }

        screen::erase_at(cx, cy);
        screen::set_cursor(cx, cy);
    }

    // Clear buffer and fill with new text
    list::clear(input_buf);

    uint32_t new_len = strlen(new_text);
    for (uint32_t i = 0; i < new_len && i < CONSOLE_INPUT_MAX - 1; i++)
        list::add(input_buf, new_text[i]);

    screen::write(new_text);
    input_pos = list::size(input_buf);
}

// Flatten list buffer into a C-string
static void buf_to_str(list::List<char>* buf, char* out, uint32_t max_len)
{
    uint32_t len = list::size(buf);
    if (len >= max_len)
        len = max_len - 1;

    for (uint32_t i = 0; i < len; i++)
        list::get(buf, i, out[i]);
    out[len] = '\0';
}

// Parse a C-string into argc/argv
static int parse_line(const char* line)
{
    // Copy into line_buf so we can insert null terminators
    uint32_t len = strlen(line);
    if (len >= CONSOLE_INPUT_MAX)
        len = CONSOLE_INPUT_MAX - 1;
    memcpy(line_buf, line, len);
    line_buf[len] = '\0';

    int argc = 0;
    bool in_token = false;

    for (uint32_t i = 0; i < len && argc < CONSOLE_MAX_ARGS; i++)
    {
        if (line_buf[i] == ' ')
        {
            line_buf[i] = '\0';
            in_token = false;
        }
        else if (!in_token)
        {
            argv_buf[argc++] = &line_buf[i];
            in_token = true;
        }
    }

    return argc;
}

// Execute parsed command
static void exec(const char* line)
{
    int argc = parse_line(line);
    if (argc == 0)
        return;

    const char* cmd_name = argv_buf[0];

    for (uint32_t i = 0; i < cmd_count; i++)
    {
        if (strcmp(cmd_name, cmd_table[i].name) == 0)
        {
            cmd_table[i].handler(argc, (const char**)argv_buf);
            return;
        }
    }

    screen::printf("\n\rUnknown command: %s", cmd_name);
}

// Keyboard event handler
static void on_key(keyboard_event_t e)
{
    if (e.type != KEY_PRESS)
        return;

    if (e.KeyCode == Keys::BACKSPACE)
    {
        if (input_pos == 0)
            return;

        input_pos--;
        list::remove_at(input_buf, input_pos);

        // Redraw: move screen cursor back, then rewrite everything from input_pos to end
        uint32_t cx = screen::cursor_x();
        uint32_t cy = screen::cursor_y();

        if (cx > 0)
            cx--;
        else if (cy > 0)
        {
            cy--;
            cx = screen::cols() - 1;
        }

        screen::set_cursor(cx, cy);

        // Rewrite from current position to end of buffer, then erase the trailing char
        uint32_t save_cx = screen::cursor_x();
        uint32_t save_cy = screen::cursor_y();

        for (uint32_t i = input_pos; i < list::size(input_buf); i++)
        {
            char c;
            list::get(input_buf, i, c);
            char out[2] = {c, '\0'};
            screen::write(out);
        }
        // Erase the leftover character at the end
        screen::erase_at(screen::cursor_x(), screen::cursor_y());

        // Restore cursor to where it should be
        screen::set_cursor(save_cx, save_cy);
        return;
    }

    if (e.KeyCode == Keys::ENTER)
    {
        char cmd_line[CONSOLE_INPUT_MAX];
        buf_to_str(input_buf, cmd_line, CONSOLE_INPUT_MAX);

        history_push(cmd_line);
        history_browse = -1;

        exec(cmd_line);
        list::clear(input_buf);
        input_pos = 0;
        screen::printf("\n\r> ");
        return;
    }

    // History navigation
    if (e.KeyCode == Keys::ARROW_UP)
    {
        uint32_t next = history_browse + 1;
        if (next < (uint32_t)history_count)
        {
            history_browse = next;
            const char* entry = history_get(history_browse);
            if (entry)
                replace_input(entry);
        }
        return;
    }

    if (e.KeyCode == Keys::ARROW_DOWN)
    {
        if (history_browse > 0)
        {
            history_browse--;
            const char* entry = history_get(history_browse);
            if (entry)
                replace_input(entry);
        }
        else if (history_browse == 0)
        {
            history_browse = -1;
            replace_input("");
        }
        return;
    }

    // Cursor movement within input line
    if (e.KeyCode == Keys::ARROW_LEFT)
    {
        if (input_pos == 0)
            return;

        input_pos--;

        uint32_t cx = screen::cursor_x();
        uint32_t cy = screen::cursor_y();

        if (cx > 0)
            cx--;
        else if (cy > 0)
        {
            cy--;
            cx = screen::cols() - 1;
        }
        screen::set_cursor(cx, cy);
        return;
    }

    if (e.KeyCode == Keys::ARROW_RIGHT)
    {
        if (input_pos >= list::size(input_buf))
            return;

        input_pos++;

        uint32_t cx = screen::cursor_x();
        uint32_t cy = screen::cursor_y();

        cx++;
        if (cx >= screen::cols())
        {
            cx = 0;
            cy++;
        }
        screen::set_cursor(cx, cy);
        return;
    }

    // Delete: remove character under cursor (to the right)
    if (e.KeyCode == Keys::DELETE)
    {
        if (input_pos >= list::size(input_buf))
            return;

        list::remove_at(input_buf, input_pos);

        // Redraw from current position to end, erase trailing char
        uint32_t save_cx = screen::cursor_x();
        uint32_t save_cy = screen::cursor_y();

        for (uint32_t i = input_pos; i < list::size(input_buf); i++)
        {
            char c;
            list::get(input_buf, i, c);
            char out[2] = {c, '\0'};
            screen::write(out);
        }
        screen::erase_at(screen::cursor_x(), screen::cursor_y());

        screen::set_cursor(save_cx, save_cy);
        return;
    }

    // Ignore non-printable characters
    if (e.KeyChar < 0x20 || e.KeyChar == 0x7F)
        return;

    // Don't overflow input buffer
    if (list::size(input_buf) >= CONSOLE_INPUT_MAX - 1)
        return;

    // Insert character at cursor position (overwrite mode on screen,
    // but insert into the buffer so no chars are lost)
    // If typing in the middle, we need to redraw the rest of the line
    bool at_end = (input_pos == list::size(input_buf));

    // Insert into buffer at current position
    if (at_end)
    {
        list::add(input_buf, e.KeyChar);
    }
    else
    {
        // Shift everything right by inserting: add dummy at end, then shift
        char last;
        list::get(input_buf, list::size(input_buf) - 1, last);
        list::add(input_buf, last);
        for (uint32_t i = list::size(input_buf) - 2; i > input_pos; i--)
        {
            char c;
            list::get(input_buf, i - 1, c);
            list::set(input_buf, i, c);
        }
        list::set(input_buf, input_pos, e.KeyChar);
    }

    input_pos++;

    if (at_end)
    {
        // Simple case: just print the character
        char out[2] = {e.KeyChar, '\0'};
        screen::write(out);
    }
    else
    {
        // Redraw from cursor to end of buffer
        uint32_t save_cx = screen::cursor_x();
        uint32_t save_cy = screen::cursor_y();

        for (uint32_t i = input_pos - 1; i < list::size(input_buf); i++)
        {
            char c;
            list::get(input_buf, i, c);
            char out[2] = {c, '\0'};
            screen::write(out);
        }

        // Move cursor to right after the inserted character
        save_cx++;
        if (save_cx >= screen::cols())
        {
            save_cx = 0;
            save_cy++;
        }
        screen::set_cursor(save_cx, save_cy);
    }

    uart::printf("%c", e.KeyChar);
}

// API
namespace console
{
    uint32_t command_count()
    {
        return cmd_count;
    }

    const char* command_name(uint32_t index)
    {
        if (index >= cmd_count)
            return "";
        return cmd_table[index].name;
    }

    void register_command(const char* name, command_fn handler)
    {
        if (cmd_count >= MAX_COMMANDS)
            return;

        cmd_table[cmd_count].name = name;
        cmd_table[cmd_count].handler = handler;
        cmd_count++;
    }

    void init()
    {
        cmd_count = 0;
        history_count = 0;
        history_write = 0;
        history_browse = -1;
        input_pos = 0;
        input_buf = list::create<char>();

        commands::init();

        keyboard::set_keyboard_callback(on_key);
        screen::show_cursor();

        char cpu_name[51];
        cpuid::get_cpu_name(cpu_name);

        char freq[12];
        int_to_str(cpuid::get_base_freq(), freq);

        screen::printf("\n\tSurfaceOS v%s (C) 2025\n\r\tMem: ", VERSION_STRING);
        screen::printf("%u", (uint32_t)(memory::total() / 1048576 + 1));
        screen::printf(" Mb\n\r\tCpu: %s @ %s MHz", cpu_name, freq);
        screen::printf("\n\r------------------------------------------------\n\n\r> ");
    }

} // namespace console