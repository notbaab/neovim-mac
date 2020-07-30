//
//  ui.cpp
//  Neovim
//
//  Created by Jay Sandhu on 4/8/20.
//  Copyright © 2020 Jay Sandhu. All rights reserved.
//

#include <algorithm>
#include <utility>
#include <iostream>
#include <type_traits>

#include "log.h"
#include "ui.hpp"

namespace nvim {
namespace {

/// A table of highlight attributes.
/// The Neovim UI API predefines highlight groups in a table and refers to them
/// by their index. We store the highlight table as a vector of cell_attributes.
/// The default highlight group is stored at index 0.
using highlight_table = std::vector<cell_attributes>;

/// Returns the highlight group with the given ID.
/// If the highlight ID is not defined, returns the default highlight group.
inline const cell_attributes* hl_get_entry(const highlight_table &table,
                                           size_t hlid) {
    if (hlid < table.size()) {
        return &table[hlid];
    }

    return &table[0];
}

/// Create new entry for the given id.
/// If the ID has been used before, the old entry is replaced.
/// Any gaps created in the table are filled by default initialized entries.
/// Note: ID 0 is reserved for the default highlight group.
inline cell_attributes* hl_new_entry(highlight_table &table, size_t hlid) {
    const size_t table_size = table.size();

    if (hlid == table_size) {
        table.push_back(table[0]);
        return &table.back();
    }

    if (hlid < table_size) {
        table[hlid] = table[0];
        return &table[hlid];
    }

    cell_attributes default_attrs = table[0];
    table.resize(hlid, default_attrs);
    return &table.back();
}

void log_grid_out_of_bounds(const grid *grid, const char *event,
                            size_t row, size_t col) {
    os_log_error(rpc, "Redraw error: Grid index out of bounds - "
                      "Event=%s, Grid=%zux%zu, Index=[row=%zu, col=%zu]",
                      event, grid->width(), grid->height(), row, col);
}

/// Type checking wrapper that:
/// Allows narrowing integer conversions.
/// Allows msg::object pass through.
template<typename T>
bool is(const msg::object &object) {
    if constexpr (!std::is_same_v<T, msg::boolean> && std::is_integral_v<T>) {
        return object.is<msg::integer>();
    } else if constexpr (std::is_same_v<T, msg::object>) {
        return true;
    } else {
        return object.is<T>();
    }
}

/// Object unwrapping wrapper that:
/// Allows narrowing integer conversions.
/// Allows msg::object pass through.
template<typename T>
T get(const msg::object &object) {
    if constexpr (!std::is_same_v<T, msg::boolean> && std::is_integral_v<T>) {
        return object.get<msg::integer>().as<T>();
    } else if constexpr (std::is_same_v<T, msg::object>) {
        return object;
    } else {
        return object.get<T>();
    }
}

template<typename ...Ts, size_t ...Indexes>
void call(ui_controller &controller,
          void(ui_controller::*member_function)(Ts...),
          const msg::array &array,
          std::integer_sequence<size_t, Indexes...>) {
    (controller.*member_function)(get<Ts>(array[Indexes])...);
}

/// Invokes member function with an array of arguments.
/// If object is an array of objects whose types match the member function's
/// signature, the member function is invoked. Otherwise a type error is logged.
template<typename ...Ts>
void apply_one(ui_controller *controller,
               void(ui_controller::*member_function)(Ts...),
               const msg::string &name, const msg::object &object) {
    if (object.is<msg::array>()) {
        msg::array args = object.get<msg::array>();
        
        constexpr size_t size = sizeof...(Ts);
        size_t index = 0;
        
        if (size <= args.size() && (is<Ts>(args[index++]) && ...)) {
            return call(*controller, member_function, args,
                        std::make_integer_sequence<size_t, size>());
        }
    }
    
    os_log_error(rpc, "Redraw error: Argument type error - "
                      "Event=%.*s, ArgTypes=%s",
                      (int)name.size(), name.data(),
                      msg::type_string(object).c_str());
}

/// Invokes member function once for each parameter tuple in array.
template<typename ...Ts>
void apply(ui_controller *controller,
           void(ui_controller::*member_function)(Ts...),
           const msg::string &name, const msg::array &array) {
    for (const msg::object &tuple : array) {
        apply_one(controller, member_function, name, tuple);
    }
}

} // namespace

grid* ui_controller::get_grid(size_t index) {
    // We don't support ext_multigrid, so index should always be 1.
    // If it isn't, we don't exaclty fail gracefully.
    if (index != 1) {
        std::abort();
    }
    
    return writing;
}

void ui_controller::redraw_event(const msg::object &event_object) {
    const msg::array *event = event_object.get_if<msg::array>();
    
    if (!event || !event->size() || !event->at(0).is<msg::string>()) {
        return os_log_error(rpc, "Redraw error: Event type error - Type=%s",
                            msg::type_string(event_object).c_str());
    }

    // Neovim update events are arrays where:
    //  - The first element is the event name
    //  - The remainining elements are an array of argument tuples.
    msg::string name = event->at(0).get<msg::string>();
    msg::array args = event->subarray(1);
    
    if (name == "grid_line") {
        return apply(this, &ui_controller::grid_line, name, args);
    } else if (name == "grid_resize") {
        return apply(this, &ui_controller::grid_resize, name, args);
    } else if (name == "grid_scroll") {
        return apply(this, &ui_controller::grid_scroll, name, args);
    } else if (name == "flush") {
        return apply(this, &ui_controller::flush, name, args);
    } else if (name == "grid_clear") {
        return apply(this, &ui_controller::grid_clear, name, args);
    } else if (name == "hl_attr_define") {
        return apply(this, &ui_controller::hl_attr_define, name, args);
    } else if (name == "default_colors_set") {
        return apply(this, &ui_controller::default_colors_set, name, args);
    } else if (name == "mode_info_set") {
        return apply(this, &ui_controller::mode_info_set, name, args);
    } else if (name == "mode_change") {
        return apply(this, &ui_controller::mode_change, name, args);
    } else if (name == "grid_cursor_goto") {
        return apply(this, &ui_controller::grid_cursor_goto, name, args);
    } else if (name == "set_title") {
        return apply(this, &ui_controller::set_title, name, args);
    }

    // When options change, we should inform the delegate. Neovim tends to
    // send redundant option change events, so only call the delegate if the
    // options actually changed.
    if (name == "option_set") {
        std::lock_guard lock(option_lock);
        options oldopts = opts;
        apply(this, &ui_controller::set_option, name, args);

        if (opts != oldopts) {
            window.options_set();
        }
        
        return;
    }

    // The following events are ignored for now.
    if (name == "mouse_on" || name == "mouse_off"  ||
        name == "set_icon" || name == "hl_group_set") {
        return;
    }
    
    os_log_info(rpc, "Redraw info: Unhandled event - Name=%.*s Args=%s",
                (int)std::min(name.size(), 128ul), name.data(),
                msg::to_string(args).c_str());
}

void ui_controller::redraw(msg::array events) {
    for (const msg::object &event : events) {
        redraw_event(event);
    }
}

void ui_controller::grid_resize(size_t grid_id, size_t width, size_t height) {
    grid *grid = get_grid(grid_id);
    grid->grid_width = width;
    grid->grid_height = height;
    grid->cells.resize(width * height);
}

template<typename ...Ts>
static bool type_check(const msg::array &array) {
    size_t index = 0;
    return array.size() == sizeof...(Ts) && (array[index++].is<Ts>() && ...);
}

/// Represents a cell update from the grid_line event.
struct cell_update {
    msg::string text;
    const cell_attributes *hlattr;
    size_t repeat;
    
    cell_update(): hlattr(nullptr), repeat(0) {}

    /// Set the cell_update from a msg::object.
    /// @param object   An object from the cells array in a grid_line event.
    /// @param hltable  The highlight table.
    /// @returns True if object type checked correctly, otherwise false.
    bool set(const msg::object &object, const highlight_table &hltable) {
        if (!object.is<msg::array>()) {
            return false;
        }
        
        msg::array array = object.get<msg::array>();
        
        if (type_check<msg::string>(array)) {
            text = array[0].get<msg::string>();
            repeat = 1;
            return true;
        }
        
        if (type_check<msg::string, msg::integer>(array)) {
            text = array[0].get<msg::string>();
            hlattr = hl_get_entry(hltable, array[1].get<msg::integer>());
            repeat = 1;
            return true;
        }
            
        if (type_check<msg::string, msg::integer, msg::integer>(array)) {
            text = array[0].get<msg::string>();
            hlattr = hl_get_entry(hltable, array[1].get<msg::integer>());
            repeat = array[2].get<msg::integer>();
            return true;
        }
        
        return false;
    }
};

void ui_controller::grid_line(size_t grid_id, size_t row,
                              size_t col, msg::array cells) {
    grid *grid = get_grid(grid_id);
    
    if (row >= grid->height() || col >= grid->width()) {
        return log_grid_out_of_bounds(grid, "grid_line", row, col);
    }
    
    cell *rowbegin = grid->get(row, 0);
    cell *cell = rowbegin + col;
    
    size_t remaining = grid->width() - col;
    cell_update update;
    
    for (const msg::object &object : cells) {
        if (!update.set(object, hltable)) {
            return os_log_error(rpc, "Redraw error: Cell update type error - "
                                     "Event=grid_line, Type=%s",
                                     msg::type_string(object).c_str());
        }
        
        if (update.repeat > remaining) {
            return os_log_error(rpc, "Redraw error: Row overflow - "
                                     "Event=grid_line");
        }

        // Empty cells are the right cell of a double width char.
        if (update.text.size() == 0) {
            // This should never happen. We'll be defensive about it.
            if (cell == rowbegin) {
                return;
            }
            
            nvim::cell *left = cell - 1;
            cell->attrs = left->attrs;
            left->attrs.flags |= cell_attributes::doublewidth;

            // Double width chars never repeat.
            cell += 1;
            remaining -= 1;
        } else {
            *cell = nvim::cell(update.text, update.hlattr);

            for (int i=1; i<update.repeat; ++i) {
                cell[i] = *cell;
            }

            cell += update.repeat;
            remaining -= update.repeat;
        }
    }
}

void ui_controller::grid_clear(size_t grid_id) {
    grid *grid = get_grid(grid_id);

    cell empty;
    empty.attrs.background = hltable[0].background;

    for (cell &cell : grid->cells) {
        cell = empty;
    }
}

void ui_controller::grid_cursor_goto(size_t grid_id, size_t row, size_t col) {
    grid *grid = get_grid(grid_id);
    
    if (row >= grid->height() || col >= grid->width()) {
        return os_log_error(rpc, "Redraw error: Cursor out of bounds - "
                                 "Event=grid_cursor_goto, "
                                 "Grid=[%zu, %zu], Row=%zu, Col=%zu",
                                 grid->width(), grid->height(), row, col);
    }
    
    grid->cursor_row = row;
    grid->cursor_col = col;
}

void ui_controller::grid_scroll(size_t grid_id, size_t top, size_t bottom,
                                size_t left, size_t right, long rows) {
    if (bottom < top || right < left) {
        os_log_error(rpc, "Redraw error: Invalid args - "
                          "Event=grid_scroll, "
                          "Args=[top=%zu, bottom=%zu, left=%zu, right=%zu]",
                          top, bottom, left, right);
        return;
    }
    
    grid *grid = get_grid(grid_id);
    size_t height = bottom - top;
    size_t width = right - left;
    
    if (bottom > grid->height() || right > grid->width()) {
        return log_grid_out_of_bounds(grid, "grid_scroll", bottom, right);
    }
    
    long count;
    long row_width;
    cell *dest;
    
    if (rows >= 0) {
        dest = grid->get(top, left);
        row_width = grid->width();
        count = height - rows;
    } else {
        dest = grid->get(bottom - 1, left);
        row_width = -grid->width();
        count = height + rows;
    }

    cell *src = dest + ((long)grid->width() * rows);
    size_t copy_size = sizeof(cell) * width;
    
    for (long i=0; i<count; ++i) {
        memcpy(dest, src, copy_size);
        dest += row_width;
        src += row_width;
    }
}

void ui_controller::flush() {
    grid *completed = writing;
    completed->draw_tick += 1;
    
    writing = complete.exchange(completed);
    *writing = *completed;

    if (flush_wait) {
        dispatch_semaphore_signal(flush_wait);
        flush_wait = nullptr;
    } else {
        window.redraw();
    }
}

static inline void adjust_defaults(const cell_attributes &def,
                                   cell_attributes &attrs) {
    bool reversed = attrs.flags & cell_attributes::reverse;
    
    if (attrs.foreground.is_default()) {
        attrs.foreground = reversed ? def.background : def.foreground;
    }
    
    if (attrs.background.is_default()) {
        attrs.background = reversed ? def.foreground : def.background;
    }
    
    if (attrs.special.is_default()) {
        attrs.special = def.special;
    }
}

void ui_controller::default_colors_set(uint32_t fg, uint32_t bg, uint32_t sp) {
    cell_attributes &def = hltable[0];
    def.foreground = rgb_color(fg, rgb_color::default_tag);
    def.background = rgb_color(bg, rgb_color::default_tag);
    def.special = rgb_color(sp, rgb_color::default_tag);
    def.flags = 0;
    
    for (cell_attributes &attrs : hltable) {
        adjust_defaults(def, attrs);
    }
    
    for (cell &cell : writing->cells) {
        adjust_defaults(def, cell.attrs);
    }
}

static inline void set_rgb_color(rgb_color &color, const msg::object &object) {
    if (!object.is<msg::integer>()) {
        return os_log_error(rpc, "Redraw error: RGB type error - "
                                 "Event=hl_attr_define, Type=%s",
                                 msg::type_string(object).c_str());
    }
    
    uint32_t rgb = object.get<msg::integer>().as<uint32_t>();
    color = rgb_color(rgb);
}

void ui_controller::hl_attr_define(size_t hlid, msg::map definition) {
    cell_attributes *attrs = hl_new_entry(hltable, hlid);
    
    for (const msg::pair &pair : definition) {
        if (!pair.first.is<msg::string>()) {
            os_log_error(rpc, "Redraw error: Map key type error - "
                              "Event=hl_attr_define, Type=%s",
                              msg::type_string(pair.first).c_str());
            continue;
        }

        msg::string name = pair.first.get<msg::string>();

        if (name == "foreground") {
            set_rgb_color(attrs->foreground, pair.second);
        } else if (name == "background") {
            set_rgb_color(attrs->background, pair.second);
        } else if (name == "underline") {
            attrs->flags |= cell_attributes::underline;
        } else if (name == "bold") {
            attrs->flags |= cell_attributes::bold;
        } else if (name == "italic") {
            attrs->flags |= cell_attributes::italic;
        } else if (name == "strikethrough") {
            attrs->flags |= cell_attributes::strikethrough;
        } else if (name == "undercurl") {
            attrs->flags |= cell_attributes::undercurl;
        } else if (name == "special") {
            set_rgb_color(attrs->special, pair.second);
        } else if (name == "reverse") {
            attrs->flags |= cell_attributes::reverse;
        } else {
            os_log_info(rpc, "Redraw info: Ignoring highlight attribute - "
                             "Event=hl_attr_define, Name=%.*s",
                             (int)name.size(), name.data());
        }
    }
    
    if (attrs->flags & cell_attributes::reverse) {
        std::swap(attrs->background, attrs->foreground);
    }
}

static inline cursor_shape to_cursor_shape(const msg::object &object) {
    if (object.is<msg::string>()) {
        msg::string name = object.get<msg::string>();
        
        if (name == "block") {
            return cursor_shape::block;
        } else if (name == "vertical") {
            return cursor_shape::vertical;
        } else if (name == "horizontal") {
            return cursor_shape::horizontal;
        }
    }
    
    os_log_error(rpc, "Redraw error: Unknown cursor shape - "
                      "Event=mode_info_set CursorShape=%s",
                      msg::to_string(object).c_str());

    return cursor_shape::block;
};

static inline void set_color_attrs(cursor_attributes *cursor_attrs,
                                   const highlight_table &hltable,
                                   const msg::object &object) {
    if (!object.is<msg::integer>()) {
        os_log_error(rpc, "Redraw error: Highlight id type error - "
                          "Event=mode_info_set, Type=%s",
                          msg::type_string(object).c_str());
        return;
    }
    
    const size_t hlid = object.get<msg::integer>();
    const cell_attributes *hl_attrs = hl_get_entry(hltable, hlid);
    cursor_attrs->special = hl_attrs->special;
    
    if (hlid != 0) {
        cursor_attrs->foreground = hl_attrs->foreground;
        cursor_attrs->background = hl_attrs->background;
    } else {
        cursor_attrs->foreground = hl_attrs->background;
        cursor_attrs->background = hl_attrs->foreground;
    }
}

template<typename T>
static inline T to(const msg::object &object) {
    if (is<T>(object)) {
        return get<T>(object);
    }
    
    return {};
}

static mode_info to_mode_info(const highlight_table &hl_table,
                              const msg::map &map) {
    mode_info info = {};
    
    for (const msg::pair &pair : map) {
        if (!pair.first.is<msg::string>()) {
            os_log_error(rpc, "Redraw error: Map key type error - "
                              "Event=mode_info_set, Type=%s",
                              msg::type_string(pair.first).c_str());
            continue;
        }
        
        msg::string name = pair.first.get<msg::string>();
        
        if (name == "cursor_shape") {
            info.cursor_attrs.shape = to_cursor_shape(pair.second);
        } else if (name == "cell_percentage") {
            info.cursor_attrs.percentage = to<uint16_t>(pair.second);
        } else if (name == "blinkwait") {
            info.cursor_attrs.blinkwait = to<uint16_t>(pair.second);
        } else if (name == "blinkon") {
            info.cursor_attrs.blinkon = to<uint16_t>(pair.second);
        } else if (name == "blinkoff") {
            info.cursor_attrs.blinkoff = to<uint16_t>(pair.second);
        } else if (name == "name") {
            info.mode_name = to<msg::string>(pair.second);
        } else if (name == "attr_id") {
            set_color_attrs(&info.cursor_attrs, hl_table, pair.second);
        }
    }

    if (info.cursor_attrs.blinkwait &&
        info.cursor_attrs.blinkoff  &&
        info.cursor_attrs.blinkon) {
        info.cursor_attrs.blinks = true;
    }
    
    return info;
}

void ui_controller::mode_info_set(bool enabled, msg::array property_maps) {
    mode_info_table.clear();
    mode_info_table.reserve(property_maps.size());
    current_mode = 0;
    
    for (const msg::object &object : property_maps) {
        if (!object.is<msg::map>()) {
            os_log_error(rpc, "Redraw error: Cursor property map type error - "
                              "Event=mode_info_set, Type=%s",
                              msg::type_string(object).c_str());
            continue;
        }
        
        msg::map map = object.get<msg::map>();
        mode_info_table.push_back(to_mode_info(hltable, map));
    }
}

void ui_controller::mode_change(msg::string name, size_t index) {
    if (index >= mode_info_table.size()) {
        return os_log_error(rpc, "Redraw error: Mode index out of bounds - "
                                 "Event=mode_change, TableSize=%zu, Index=%zu",
                                 mode_info_table.size(), index);
    }
    
    writing->cursor_attrs = mode_info_table[index].cursor_attrs;
}

void ui_controller::set_title(msg::string new_title) {
    {
        std::lock_guard lock(option_lock);
        option_title = new_title;
    }

    window.title_set();
}

std::string ui_controller::get_title() {
    std::lock_guard lock(option_lock);
    return option_title;
}

std::string ui_controller::get_font_string() {
    std::lock_guard lock(option_lock);
    return option_guifont;
}

nvim::options ui_controller::get_options() {
    std::lock_guard lock(option_lock);
    return opts;
}

/// Makes a font object from a Vim font string.
/// If size is not given in fontstr, default_size is used.
static font make_font(std::string_view fontstr, double default_size) {
    size_t index = fontstr.size();
    size_t multiply = 1;
    size_t size = 0;
    
    while (index) {
        index -= 1;
        char digit = fontstr[index];
        
        if (isdigit(digit)) {
            size = size + (multiply * (digit - '0'));
            multiply *= 10;
        } else {
            break;
        }
    }
    
    if (size && index && fontstr[index] == 'h' && fontstr[index - 1] == ':') {
        return font{std::string(fontstr.substr(0, index - 1)), double(size)};
    } else {
        return font{std::string(fontstr), default_size};
    }
}

static inline size_t find_unescaped_comma(std::string_view string, size_t pos) {
    for (;;) {
        pos = string.find(',', pos);

        if (pos == std::string_view::npos) {
            return pos;
        }

        // TODO: We're probably not handling multiple backslashes properly.
        //       Replace with a more robust solution.
        if (pos != 0 && string[pos - 1] != '\\') {
            return pos;
        }
        
        pos += 1;
    }
}

std::vector<font> ui_controller::get_fonts(double default_size) {
    std::lock_guard lock(option_lock);
    std::vector<font> fonts;
    
    if (!option_guifont.size()) {
        return fonts;
    }

    std::string_view fontopt = option_guifont;
    size_t index = 0;

    for (;;) {
        size_t pos = find_unescaped_comma(fontopt, index);

        if (pos == std::string_view::npos) {
            auto fontstr = fontopt.substr(index);
            fonts.push_back(make_font(fontstr, default_size));
            break;
        }

        auto fontstr = fontopt.substr(index, pos - index);
        fonts.push_back(make_font(fontstr, default_size));
        
        index = fontopt.find_first_not_of(' ', pos + 1);

        if (pos == std::string::npos) {
            break;
        }
    }
    
    return fonts;
}

static inline void set_font_option(std::string &opt_guifont,
                                   const msg::object &value,
                                   window_controller &window) {
    if (!value.is<msg::string>()) {
        return os_log_info(rpc, "Redraw info: Option type error - "
                                "Option=guifont Type=%s",
                                msg::type_string(value).c_str());
    }

    opt_guifont = value.get<msg::string>();
    window.font_set();
}

static inline void set_ext_option(bool &opt, const msg::object &value) {
    if (!value.is<msg::boolean>()) {
        return os_log_info(rpc, "Redraw info: Option type error - "
                                "Option=ext Type=%s",
                                msg::type_string(value).c_str());
    }

    opt = value.get<msg::boolean>();
}

void ui_controller::set_option(msg::string name, msg::object value) {
    if (name == "guifont") {
        set_font_option(option_guifont, value, window);
    } else if (name == "ext_cmdline")  {
        set_ext_option(opts.ext_cmdline, value);
    } else if (name == "ext_hlstate")  {
        set_ext_option(opts.ext_hlstate, value);
    } else if (name == "ext_linegrid")  {
        set_ext_option(opts.ext_linegrid, value);
    } else if (name == "ext_messages")  {
        set_ext_option(opts.ext_messages, value);
    } else if (name == "ext_multigrid")  {
        set_ext_option(opts.ext_multigrid, value);
    } else if (name == "ext_popupmenu")  {
        set_ext_option(opts.ext_popupmenu, value);
    } else if (name == "ext_tabline")  {
        set_ext_option(opts.ext_tabline, value);
    } else if (name == "ext_termcolors")  {
        set_ext_option(opts.ext_termcolors, value);
    }
}

} // namespace ui
