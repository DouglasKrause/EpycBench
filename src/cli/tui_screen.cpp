#include "cli/tui.hpp"
#include "platform_linux/presentation_files.hpp"
#include "platform_linux/workbench_terminal.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <thread>
#include <utility>

namespace epycbench::cli::tui {
namespace {
using namespace ftxui;
std::string printable(std::string str) {
    for (auto& c : str)
        if (static_cast<unsigned char>(c) < 32U || c == '\x7f')
            c = ' ';
    return str;
}
std::string folded(std::string str) {
    for (auto& c : str)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return str;
}
class WorkbenchScreen {
  public:
    explicit WorkbenchScreen(Session& session) : s_(session) {}
    void run() {
        auto screen = ScreenInteractive::Fullscreen();
        screen.ForceHandleCtrlC(false);
        auto input = Input(&buffer_, "Enter a value");
        auto root = CatchEvent(Renderer(input, [&] { return render(input); }), [&](Event e) {
            try {
                return event(std::move(e), input);
            } catch (const std::exception& ex) {
                s_.message = ex.what();
                return true;
            }
        });
        platform_linux::WorkbenchScreenSignals signals;
        Loop loop(&screen, root);
        // A running study gets a short input interval between attempts. The Loop
        // is destroyed (including both FTXUI listener threads) before dispatch.
        if (s_.running) {
            loop.RunOnce();
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            while (s_.running && !loop.HasQuitted() && !signals.exit_requested() &&
                   std::chrono::steady_clock::now() < until) {
                loop.RunOnce();
                std::this_thread::sleep_for(std::chrono::milliseconds(12));
            }
        }
        while (!s_.quit && !s_.after_loop && !loop.HasQuitted() && !signals.exit_requested())
            loop.RunOnceBlocking();
        if (loop.HasQuitted() || signals.exit_requested()) {
            s_.after_loop = {};
            s_.running = false;
            s_.quit = true;
        } else if (s_.after_loop) {
            // Stop producers, then dispatch everything they already consumed.
            // Destroying a live Loop here could discard a queued pause or Ctrl+C.
            screen.Exit();
            // The last producer can retire without posting another event.
            while (!loop.HasQuitted()) {
                loop.RunOnce();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (signals.exit_requested()) {
                s_.after_loop = {};
                s_.quit = true;
            }
        }
    }

  private:
    Session& s_;
    std::vector<int> filtered_;
    std::vector<Box> boxes_;
    struct TilePosition {
        int line;
        int column;
    };
    std::vector<std::optional<TilePosition>> tile_positions_;
    std::string buffer_, query_, payload_;
    std::uint64_t revision_{std::numeric_limits<std::uint64_t>::max()};
    bool searching_{};
    int pressed_{-1};
    void refresh_filter() {
        filtered_.clear();
        for (std::size_t i = 0; i < s_.rows.size(); ++i) {
            const auto& r = s_.rows[i];
            if (!searching_ ||
                folded(r.label + " " + r.panel).find(folded(query_)) != std::string::npos)
                filtered_.push_back(static_cast<int>(i));
        }
        if (!filtered_.empty() && !std::ranges::contains(filtered_, s_.selected))
            s_.selected = filtered_.front();
    }
    void sync() {
        if (revision_ != s_.revision) {
            revision_ = s_.revision;
            buffer_ = s_.entry ? s_.entry->value : "";
            query_.clear();
            pressed_ = -1;
            payload_.clear();
            boxes_.clear();
            tile_positions_.clear();
            searching_ = false;
        }
        refresh_filter();
    }
    Element render(const Component& input) {
        sync();
        const auto dims = Terminal::Size();
        const bool wide = dims.dimx >= 85;
        Elements header{text("  EpycBench") | bold | color(Color::CyanLight),
                        text("   " + printable(s_.title)) | bold};
        Elements area{hbox(std::move(header)), separator()};
        if (s_.authoring && s_.target)
            area.push_back(text(" Target  " + printable(s_.topology_label)) |
                           color(Color::CyanLight));
        if (s_.entry) {
            area.push_back(input->Render() | border);
        } else {
            if (searching_)
                area.push_back(text(" Find  " + printable(query_)) | bold);
            boxes_.assign(s_.rows.size(), Box{1, 0, 1, 0});
            tile_positions_.assign(s_.rows.size(), std::nullopt);
            Elements items, tools;
            std::string last_item, last_tool;
            const int map_width = dims.dimx - dims.dimx / 3 - 1;
            Elements tiles;
            std::optional<std::size_t> tile_line;
            int tile_width = 0;
            int visual_line = 0;
            const auto flush_tiles = [&] {
                if (tiles.empty()) return;
                tiles.push_back(filler());
                items.push_back(hbox(std::move(tiles)));
                tiles.clear();
                tile_width = 0;
                ++visual_line;
            };
            for (int i : filtered_) {
                const auto index = static_cast<std::size_t>(i);
                const auto& row = s_.rows[index];
                const bool palette =
                    wide && (row.panel == "Assignments" || row.panel.starts_with("Installed "));
                auto& column = palette ? tools : items;
                auto& last = palette ? last_tool : last_item;
                if (row.panel != last) {
                    if (!palette) flush_tiles();
                    last = row.panel;
                    if (!last.empty())
                        column.push_back(text(" " + printable(last)) | bold |
                                         color(Color::CyanLight));
                }
                const bool active = i == s_.selected;
                const auto* tile = wide && row.topology_tile ? &*row.topology_tile : nullptr;
                const auto label = std::string(active ? " › " : "   ") +
                                   printable(row.topology_tile ? row.topology_tile->label : row.label) +
                                   (tile ? " " : "");
                auto line = text(label);
                if (tile) {
                    const int indent = 2 * static_cast<int>(std::min(tile->indent, 4U));
                    const int width = std::min(string_width(label), map_width - indent - 1);
                    if (tile_line != tile->line || tile_width + width + 1 > map_width)
                        flush_tiles();
                    if (tiles.empty()) {
                        tiles.push_back(text(std::string(static_cast<std::size_t>(indent), ' ')));
                        tile_width = indent;
                    }
                    tile_line = tile->line;
                    tile_positions_[index] = TilePosition{visual_line, tile_width + width / 2};
                    tile_width += width + 1;
                    line = line | size(WIDTH, EQUAL, width) | bgcolor(Color::GrayDark);
                } else if (wide && !row.panel.empty() && !row.detail.empty()) {
                    line = hbox({line, text("   " + printable(row.detail)) | dim | xflex});
                }
                if (!payload_.empty() && row.accepts)
                    line = row.accepts(payload_) ? line | color(Color::GreenLight) : line | dim;
                if (active)
                    line = line | inverted | focus;
                if (tile) {
                    tiles.push_back(line | reflect(boxes_[index]));
                    tiles.push_back(text(" "));
                } else {
                    if (!palette) flush_tiles();
                    column.push_back(line | reflect(boxes_[index]));
                }
            }
            flush_tiles();
            if (items.empty() && tools.empty())
                items.push_back(text(" No matches"));
            const bool split = !tools.empty() && !items.empty();
            if (items.empty())
                items.swap(tools);
            auto list = vbox(std::move(items)) | vscroll_indicator | yframe | flex;
            std::string detail;
            if (!filtered_.empty())
                detail = printable(s_.rows[static_cast<std::size_t>(s_.selected)].detail);
            if (split) {
                area.push_back(hbox({vbox(std::move(tools)) | vscroll_indicator | yframe |
                                         size(WIDTH, EQUAL, dims.dimx / 3),
                                     separator(), list}) |
                               flex);
                if (!detail.empty())
                    area.push_back(paragraph(detail) | size(HEIGHT, LESS_THAN, 3) | dim);
            } else if (wide && !detail.empty()) {
                area.push_back(
                    hbox({list | xflex, separator(),
                          paragraph(detail) | size(WIDTH, LESS_THAN, dims.dimx / 3) | xflex}) |
                    flex);
            } else {
                area.push_back(list);
                if (!detail.empty())
                    area.push_back(paragraph(detail) | size(HEIGHT, LESS_THAN, 3) | dim);
            }
        }
        area.push_back(filler());
        if (!s_.message.empty())
            area.push_back(paragraph(printable(s_.message)) | color(Color::YellowLight) |
                           size(HEIGHT, LESS_THAN, 4));
        if (!payload_.empty())
            area.push_back(
                text(" Carrying " + printable(payload_) + " · choose a highlighted target") |
                color(Color::GreenLight));
        area.push_back(separator());
        std::string help = s_.entry ? " Enter  Apply   Ctrl+U  Clear   Esc  Back"
                                    : " ↑↓  Move   Enter  Open   /  Find   Esc  Back";
        if (!s_.entry &&
            std::ranges::any_of(s_.rows, [](const Row& r) { return !r.payload.empty(); }))
            help += "   Space  Pick up / drop";
        if (wide && std::ranges::any_of(s_.rows, [](const Row& row) { return row.topology_tile.has_value(); }))
            help += "   ←→  Map";
        if (s_.running)
            help = " P  Pause after this attempt";
        area.push_back(text(help) | dim);
        return vbox(std::move(area));
    }
    void move(int amount) {
        if (filtered_.empty())
            return;
        const auto pos = std::ranges::find(filtered_, s_.selected) - filtered_.begin();
        const auto next =
            std::clamp(static_cast<int>(pos) + amount, 0, static_cast<int>(filtered_.size() - 1));
        s_.selected = filtered_[static_cast<std::size_t>(next)];
    }
    bool move_on_map(bool horizontal, int direction) {
        const auto selected = static_cast<std::size_t>(s_.selected);
        if (selected >= tile_positions_.size() || !tile_positions_[selected])
            return false;
        const auto from = *tile_positions_[selected];
        auto nearest = std::pair{std::numeric_limits<int>::max(), std::numeric_limits<int>::max()};
        auto next = s_.selected;
        for (const auto index : filtered_) {
            const auto& to = tile_positions_[static_cast<std::size_t>(index)];
            if (!to) continue;
            const int lines = to->line - from.line, columns = to->column - from.column;
            const int along = horizontal ? columns : lines;
            if (along * direction <= 0 || (horizontal && lines != 0)) continue;
            const auto distance = std::pair{std::abs(along), std::abs(horizontal ? lines : columns)};
            if (distance < nearest) {
                nearest = distance;
                next = index;
            }
        }
        if (next == s_.selected && !horizontal && direction < 0) {
            for (const auto index : filtered_) {
                if (index >= s_.selected) break;
                if (!tile_positions_[static_cast<std::size_t>(index)]) next = index;
            }
        }
        s_.selected = next;
        return true;
    }
    void activate() {
        if (filtered_.empty())
            return;
        const Row row = s_.rows[static_cast<std::size_t>(s_.selected)];
        if (!payload_.empty()) {
            auto carried = std::exchange(payload_, "");
            if (row.accepts && !row.accepts(carried)) {
                s_.message = row.rejection ? row.rejection(carried)
                                           : row.label + " cannot accept this item. " + row.detail;
            } else if (row.drop)
                row.drop(std::move(carried));
            else
                s_.message = "This item cannot receive that placement.";
        } else if (row.activate)
            row.activate();
    }
    bool event(Event e, const Component& input) {
        sync();
        if (s_.quit)
            return true;
        if (e == Event::CtrlC) {
            s_.after_loop = {};
            s_.running = false;
            s_.quit = true;
            return true;
        }
        if (s_.running && (e == Event::p || e == Event::P || e == Event::Escape)) {
            s_.running = false;
            s_.after_loop = {};
            s_.message = "Paused. The remaining attempt budget is available.";
            return true;
        }
        if (s_.after_loop)
            return true;
        if (e == Event::Escape) {
            if (searching_) {
                query_.clear();
                searching_ = false;
            } else if (!payload_.empty()) {
                payload_.clear();
                pressed_ = -1;
            } else if (s_.back) {
                s_.finish_field(s_.back);
            }
            return true;
        }
        if (s_.entry) {
            if (e == Event::Return) {
                auto apply = s_.entry->accept;
                s_.finish_field([&] { apply(buffer_); });
                return true;
            }
            if (e == Event::CtrlU) {
                buffer_.clear();
                s_.field_text(buffer_);
                return true;
            }
            const bool handled = input->OnEvent(e);
            s_.field_text(buffer_);
            return handled;
        }
        if (e.is_mouse()) {
            const auto m = e.mouse();
            if (m.button == Mouse::WheelDown || m.button == Mouse::WheelUp) {
                move(m.button == Mouse::WheelDown ? 3 : -3);
                return true;
            }
            int hit = -1;
            for (const int i : filtered_)
                if (boxes_.size() > static_cast<std::size_t>(i) &&
                    boxes_[static_cast<std::size_t>(i)].Contain(m.x, m.y)) {
                    hit = i;
                    break;
                }
            if (m.motion == Mouse::Pressed && m.button == Mouse::Left) {
                pressed_ = hit;
                if (hit >= 0)
                    s_.selected = hit;
            } else if (m.motion == Mouse::Moved && pressed_ >= 0 && hit != pressed_) {
                payload_ = s_.rows[static_cast<std::size_t>(pressed_)].payload;
                if (hit >= 0)
                    s_.selected = hit;
            } else if (m.motion == Mouse::Released && pressed_ >= 0) {
                if (hit >= 0) {
                    s_.selected = hit;
                    if (hit != pressed_)
                        payload_ = s_.rows[static_cast<std::size_t>(pressed_)].payload;
                    if (hit == pressed_ || !payload_.empty())
                        activate();
                }
                pressed_ = -1;
                payload_.clear();
            }
            return true;
        }
        if (searching_) {
            if (e == Event::Backspace) {
                if (!query_.empty())
                    query_.pop_back();
                return true;
            }
            if (e.is_character()) {
                query_ += e.character();
                refresh_filter();
                return true;
            }
        }
        if (e == Event::Tab)
            move(1);
        else if (e == Event::TabReverse)
            move(-1);
        else if (e == Event::ArrowDown || e == Event::ArrowUp) {
            const int direction = e == Event::ArrowDown ? 1 : -1;
            if (!move_on_map(false, direction)) move(direction);
        } else if (e == Event::ArrowLeft || e == Event::ArrowRight)
            return move_on_map(true, e == Event::ArrowRight ? 1 : -1);
        else if (e == Event::PageDown)
            move(10);
        else if (e == Event::PageUp)
            move(-10);
        else if (e == Event::Home && !filtered_.empty())
            s_.selected = filtered_.front();
        else if (e == Event::End && !filtered_.empty())
            s_.selected = filtered_.back();
        else if (e == Event::Return)
            activate();
        else if (e == Event::Character('/')) {
            searching_ = true;
            query_.clear();
        } else if (e == Event::Character(' ') && !filtered_.empty()) {
            if (payload_.empty())
                payload_ = s_.rows[static_cast<std::size_t>(s_.selected)].payload;
            else
                activate();
        } else
            return false;
        return true;
    }
};
} // namespace

int launch(std::optional<Path> path, topology::cli::Environment& environment,
           const std::optional<Path>& modules, std::ostream& error,
           std::optional<Path> supplied_workers) {
    if (!platform_linux::presentation_files::interactive_terminal()) {
        error << "epycbench tui requires interactive input and output terminals\n";
        return 2;
    }
    try {
        Session session(environment, modules);
        session.supplied_workers = std::move(supplied_workers);
        session.home();
        if (path)
            session.analysis(*path);
        while (!session.quit) {
            WorkbenchScreen(session).run();
            if (session.after_loop && !session.quit) {
                auto work = std::exchange(session.after_loop, {});
                work();
            }
        }
        return 0;
    } catch (const std::exception& ex) {
        error << "epycbench tui: " << ex.what() << '\n';
        return 3;
    }
}
} // namespace epycbench::cli::tui
