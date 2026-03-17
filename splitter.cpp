#include <arpa/inet.h>
#include <cassert>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <fstream>
#include <print>
#include <vector>

// header

struct run_segment
{
    ssize_t frames{ -1 };
};

constexpr const auto NUM_SEGMENTS = 10;

struct run
{
    uint64_t started_at{ 0 };
    bool did_finish{ false };
    run_segment segments[NUM_SEGMENTS]{};

    uint32_t total_frames() const;
};

struct running_run
{
    run current_run{};
    uint32_t current_segment{ 0 };
    uint32_t current_frame{ 0 };
    bool finished{ false };

    void split();
    void frame();
};

struct database
{
    static database instance;

    std::vector<run> runs{};
    run best_segments{};
    std::optional<run> pb{};

    const run& get_best_segments() { return best_segments; }
    const std::optional<run>& get_pb() { return pb; }
    void save_run(const run& run);
    void update_best_segments(const run& run);
    void update_pb_if_necessary(const run& run);

    void save_to_disk();
    void load_from_disk();
};

// impl

uint32_t run::total_frames() const
{
    uint32_t total = 0;
    for(int i = 0; i < NUM_SEGMENTS; i++) {
        if (segments[i].frames > 0)
            total += segments[i].frames;
    }
    return total;
}

database database::instance;

void running_run::frame()
{
    current_frame++;
    std::println(stderr, "frame");
}

void running_run::split()
{
    std::println(stderr, "split");
    auto& s = current_run.segments[current_segment];
    s.frames = current_frame;
    current_frame = 0;

    if (current_segment < NUM_SEGMENTS)
        current_segment++;
    if (current_segment == NUM_SEGMENTS)
        finished = true;
}

void database::update_best_segments(const run& run)
{
    for(auto i = 0u; i < sizeof(run::segments) / sizeof(run_segment); i++) {
        if (best_segments.segments[i].frames < 0 || best_segments.segments[i].frames > run.segments[i].frames)
            best_segments.segments[i].frames = run.segments[i].frames;
    }
}

void database::update_pb_if_necessary(const run& run)
{
    if (!pb.has_value() || run.total_frames() < pb.value().total_frames())
        pb = run;
}

void database::save_run(const run& r)
{
    std::println(stderr, "saving run");

    runs.push_back(r);
    const auto& new_run = runs.back();
    update_best_segments(new_run);

    if (r.did_finish)
        update_pb_if_necessary(new_run);

    save_to_disk();
}

void database::save_to_disk()
{
    std::ofstream out("splits.bin", std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        std::println(stderr, "Failed to open splits.bin for writing");
        return;
    }

    // 1. Number of runs
    uint64_t count = runs.size();
    out.write(reinterpret_cast<const char*>(&count), sizeof(count));

    // 2. Each run
    for (const auto& r : runs)
    {
        // started_at
        out.write(reinterpret_cast<const char*>(&r.started_at), sizeof(r.started_at));

        // did_finish (1 byte)
        uint8_t finish_byte = r.did_finish ? 1 : 0;
        out.write(reinterpret_cast<const char*>(&finish_byte), sizeof(finish_byte));

        // 10 segments
        for (int i = 0; i < NUM_SEGMENTS; ++i) {
            out.write(reinterpret_cast<const char*>(&r.segments[i].frames),
                      sizeof(r.segments[i].frames));
        }
    }

    out.close();

    if (out.good()) {
        std::println(stderr, "Saved {} runs to splits.bin", count);
    } else {
        std::println(stderr, "Error writing to splits.bin");
    }
}

void database::load_from_disk()
{
    std::ifstream in("splits.bin", std::ios::binary);
    if (!in.is_open()) {
        std::println(stderr, "No splits.bin found - starting with empty database");
        runs.clear();
        return;
    }

    runs.clear();

    // 1. Read count
    uint64_t count = 0;
    in.read(reinterpret_cast<char*>(&count), sizeof(count));

    if (in.fail() || count > 1'000'000) {  // arbitrary sane limit
        std::println(stderr, "Corrupted or invalid splits.bin (bad count)");
        return;
    }

    runs.reserve(count);
    for (uint64_t i = 0; i < count; ++i)
    {
        run r;

        // started_at
        in.read(reinterpret_cast<char*>(&r.started_at), sizeof(r.started_at));

        // did_finish
        uint8_t finish_byte = 0;
        in.read(reinterpret_cast<char*>(&finish_byte), sizeof(finish_byte));
        r.did_finish = (finish_byte != 0);

        for (int s = 0; s < NUM_SEGMENTS; ++s) {
            in.read(reinterpret_cast<char*>(&r.segments[s].frames),
                    sizeof(r.segments[s].frames));
            if (best_segments.segments[s].frames < 0 || (r.segments[s].frames < best_segments.segments[s].frames && r.segments[s].frames > 0))
                best_segments.segments[s].frames = r.segments[s].frames;
        }

        if (in.fail()) {
            std::println(stderr, "Corrupted data at run #{}", i + 1);
            break;
        }

        runs.emplace_back(std::move(r));

        if (r.did_finish && (!pb.has_value() ||  r.total_frames() < pb.value().total_frames()))
            pb = r;
    }

    in.close();

    if (in.eof() || in.fail()) {
        std::println(stderr, "Warning: splits.bin ended unexpectedly");
    }

    std::println(stderr, "Loaded {} runs from splits.bin", runs.size());
}

std::optional<run> comparing_to;
running_run current_run;

constexpr const char* BOLD_YELLOW = "\033[1;33m";
constexpr const char* GREEN  = "\033[32m";
constexpr const char* RED    = "\033[31m";
constexpr const char* GRAY   = "\033[90m";
constexpr const char* CYAN   = "\033[36m";
constexpr const char* RESET  = "\033[0m";
constexpr const char* BOLD   = "\033[1m";

void print_table()
{
    const auto& r = current_run.current_run;
    uint32_t total = 0;
    uint32_t cmp_total = 0;

    for (auto i = 0u; i < NUM_SEGMENTS; ++i) {
        std::print("{:>2} | ", i + 1);

        const bool is_current_segment = i == current_run.current_segment;
        const bool is_finished_segment = i < current_run.current_segment;
        const bool is_gold = is_finished_segment && (r.segments[i].frames < database::instance.get_best_segments().segments[i].frames || database::instance.get_best_segments().segments[i].frames < 0);

        total += r.segments[i].frames;

        // Total
        if (!is_finished_segment)
            std::print("{:17} | ", "");
        else
            std::print("{:<8.2f} {:>8} | ", (double)total / 30.0, total);

        uint32_t segment_frames = 0;
        if (is_finished_segment)
            segment_frames = static_cast<uint32_t>(r.segments[i].frames);
        else if (is_current_segment)
            segment_frames = current_run.current_frame;

        // Segment
        if (segment_frames > 0 || is_current_segment)
            std::print("{}{:<8.2f} {:>8}{} | ",
                       is_gold ? BOLD_YELLOW : RESET,
                       static_cast<double>(segment_frames) / 30.0,
                       segment_frames,
                       RESET);
        else
            std::print("{:17} | ", "");

        // Comparison
        if (!comparing_to.has_value()) {
            std::println();
            continue;
        }

        const auto& cmp = comparing_to.value();
        const auto cmp_frames = cmp.segments[i].frames;
        if (cmp_frames < 0) {
            std::println();
            continue;
        }

        cmp_total += cmp.segments[i].frames;

        if (is_current_segment || is_finished_segment)
        {
            const auto total_diff = static_cast<int32_t>(total + (is_current_segment ? current_run.current_frame : 0)) - static_cast<int32_t>(cmp_total);
            const auto total_diff_sec = (double)total_diff / 30.0;
            const auto diff_frames = static_cast<int32_t>(segment_frames) - static_cast<int32_t>(cmp_frames);
            const double diff_sec = static_cast<double>(diff_frames) / 30.0;

            const char* segment_color = diff_frames < 0 ? GREEN : 
                                        diff_frames > 0 ? RED :
                                        RESET;

            const char* total_color = total_diff < 0 ? GREEN :
                                      total_diff > 0 ? RED :
                                      RESET;

            if (is_gold)
                segment_color = BOLD_YELLOW;

            std::print("{}{:<+8.2f} {}{:^+8.2f} {:>+8}{}", total_color, total_diff_sec, segment_color, diff_sec, diff_frames, RESET);
        }

        std::println();
    }
}

void print_run()
{
    std::println("\033[2J\033[H");  // clear screen + home

    std::println(" {}#{} | {}{:^17}{} | {}{:^17}{} | {}{:^17}{} ", 
                 CYAN, RESET,
                 CYAN, "Total", RESET,
                 CYAN, "Segment", RESET,
                 CYAN, "Comparison", RESET);

    print_table();

    std::println("------------------------------------------------------------------------------");

    uint32_t total_frames = current_run.current_frame;
    for (uint32_t i = 0; i < current_run.current_segment; ++i)
        total_frames += current_run.current_run.segments[i].frames;

    std::println("Time:  {:.2f} | {}", (double)(total_frames) / 30.0, total_frames);
    std::println("seg {}/{}  frame {}", current_run.current_segment + 1, NUM_SEGMENTS, current_run.current_frame);

    const auto sob_frames = database::instance.get_best_segments().total_frames();
    const auto sob_time = sob_frames / 30.0;

    std::println("sob: {:.2f} {:4}", sob_time, sob_frames);
}

void update_comparing_to()
{
    comparing_to = database::instance.get_pb();
}

void reset()
{
    if (current_run.current_run.segments[0].frames > -1)
        database::instance.save_run(current_run.current_run);

    update_comparing_to();

    current_run = running_run{};
    current_run.current_run.started_at = (uint64_t)std::time(NULL);
}

void split()
{
    if (current_run.finished)
        return;

    current_run.split();
    print_run();
    if (current_run.finished) {
        current_run.current_run.did_finish = true;
        database::instance.save_run(current_run.current_run);
        update_comparing_to();
    }
}

void frame()
{
    if (current_run.finished)
        return;

    current_run.frame();
    print_run();
}

void process_received(const char* buffer, uint32_t received)
{
    for(auto i = 0u; i < received; i++) {
        if (buffer[i] == '1')
            split();
        else if (buffer[i] == '2')
            reset();
        else if (buffer[i] == '3')
            frame();
    }
}

void socket_thread()
{
    int server_fd{-1}, client_fd{-1};
    struct sockaddr_in server_addr{}, client_addr{};
    socklen_t client_len = sizeof(client_addr);
    char buffer[64]{ 0 };

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) exit(1);

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(12345);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        close(server_fd);
        std::println(stderr, "can't bind");
        exit(1);
    }

    if (listen(server_fd, 1) == -1) {
        close(server_fd);
        std::println(stderr, "can't listen");
        exit(1);
    }

    std::println(stderr, "listening");

    while (1) {
        client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd == -1)
            continue;

        std::println(stderr, "accepted client");

        while(1) {
            ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);
            if (n > 0) {
                process_received(buffer, static_cast<uint32_t>(n));
            } else break;
        }
    }

    close(server_fd);
}

int main()
{
    database::instance.load_from_disk();
    comparing_to = database::instance.get_pb();

    socket_thread();
}
