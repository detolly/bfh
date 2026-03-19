#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <cstdint>
#include <print>
#include <vector>

#include <zlib.h>

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
    bool update_best_segments(const run& run);
    bool update_pb_if_necessary(const run& run);

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

bool database::update_best_segments(const run& run)
{
    bool did_update = false;
    for(auto i = 0u; i < NUM_SEGMENTS; i++) {
        if (best_segments.segments[i].frames < 0 || (best_segments.segments[i].frames > run.segments[i].frames && run.segments[i].frames > 0)) {
            best_segments.segments[i].frames = run.segments[i].frames;
            did_update = true;
        }
    }
    return did_update;
}

bool database::update_pb_if_necessary(const run& run)
{
    if (!pb.has_value() || run.total_frames() < pb.value().total_frames()) {
        pb = run;
        return true;
    }

    return false;
}

void database::save_run(const run& r)
{
    std::println(stderr, "saving run");

    runs.push_back(r);
    const auto& new_run = runs.back();

    bool should_save = update_best_segments(new_run);

    if (r.did_finish)
        should_save = update_pb_if_necessary(new_run) || should_save;

    if (should_save)
        save_to_disk();
}

constexpr static const char* FILENAME = "runs.db.gz";

void database::save_to_disk()
{
    gzFile file = gzopen(FILENAME, "wb9");  // wb = write binary, 9 = best compression
    if (!file) {
        std::println(stderr, "Failed to create compressed file {}", FILENAME);
        return;
    }

    uint64_t count = runs.size();

    // 1. Number of runs
    gzwrite(file, &count, sizeof(count));

    // 2. Each run
    for (const auto& r : runs)
    {
        // started_at
        gzwrite(file, &r.started_at, sizeof(r.started_at));

        // did_finish (1 byte)
        uint8_t finish_byte = r.did_finish ? 1 : 0;
        gzwrite(file, &finish_byte, sizeof(finish_byte));

        // segments
        for (int i = 0; i < NUM_SEGMENTS; ++i) {
            gzwrite(file, &r.segments[i].frames, sizeof(r.segments[i].frames));
        }
    }

    int err = gzclose(file);
    if (err != Z_OK) {
        std::println(stderr, "Error closing compressed file {} (gzerror = {})", 
                     FILENAME, gzerror(file, &err));
        return;
    }

    std::println(stderr, "Saved {} runs to {}", count, FILENAME);
}

// ────────────────────────────────────────────────

void database::load_from_disk()
{
    gzFile file = gzopen(FILENAME, "rb");
    if (!file) {
        std::println(stderr, "No {} found - starting with empty database", FILENAME);
        runs.clear();
        return;
    }

    runs.clear();

    uint64_t count = 0;
    if (gzread(file, &count, sizeof(count)) != sizeof(count)) {
        std::println(stderr, "Failed to read run count from {}", FILENAME);
        gzclose(file);
        return;
    }

    if (count > 1'000'000) {
        std::println(stderr, "Unreasonable run count ({} > 1M) - assuming corruption", count);
        gzclose(file);
        return;
    }

    runs.resize(count);

    for (uint64_t i = 0; i < count; ++i)
    {
        run& r = runs[i];

        // started_at
        if (gzread(file, &r.started_at, sizeof(r.started_at)) != sizeof(r.started_at)) {
            std::println(stderr, "Corrupted data at run #{} in {}", i + 1, FILENAME);
            exit(1);
        }

        // did_finish
        uint8_t finish_byte = 0;
        if (gzread(file, &finish_byte, sizeof(finish_byte)) != sizeof(finish_byte)) {
            std::println(stderr, "Corrupted data at run #{} in {}", i + 1, FILENAME);
            exit(1);
        }
        r.did_finish = (finish_byte != 0);

        // segments
        for (int s = 0; s < NUM_SEGMENTS; ++s) {
            if (gzread(file, &r.segments[s].frames, sizeof(r.segments[s].frames)) != sizeof(r.segments[s].frames)) {
                std::println(stderr, "Corrupted data at run #{} in {}", i + 1, FILENAME);
                exit(1);
            }

            // update best segments
            if (best_segments.segments[s].frames < 0 ||
                (r.segments[s].frames < best_segments.segments[s].frames &&
                 r.segments[s].frames > 0))
            {
                best_segments.segments[s].frames = r.segments[s].frames;
            }
        }

        // update personal best
        if (r.did_finish && (!pb.has_value() || r.total_frames() < pb.value().total_frames()))
            pb = r;
    }

    gzclose(file);

    std::println(stderr, "Loaded {} runs from {}", runs.size(), FILENAME);
}

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
    uint32_t pb_frames_total = 0;
    uint32_t sob_frames_total = 0;

    const auto pb = database::instance.pb;
    const auto sob = database::instance.best_segments;

    bool pb_scuffed = false;
    bool sob_scuffed = false;

    for (auto i = 0u; i < NUM_SEGMENTS; ++i) {

        // Current segment
        std::print("| {:>2} | ", i + 1);

        auto segment_frames = r.segments[i].frames;
        const bool is_current_segment = i == current_run.current_segment;
        const bool is_finished_segment = i < current_run.current_segment;
        const bool is_gold = is_finished_segment && (segment_frames < database::instance.get_best_segments().segments[i].frames || database::instance.get_best_segments().segments[i].frames < 0);

        if (segment_frames > -1)
            total += segment_frames;

        if (is_current_segment)
            total += current_run.current_frame;

        segment_frames = (is_current_segment ? (ssize_t)current_run.current_frame : segment_frames);

        // Total
        if (segment_frames > -1)
            std::print("{:<8.2f} {:>8} | ", (double)total / 30.0, total);
        else
            std::print("{:17} | ", "");

        // Segment
        if (segment_frames > -1)
            std::print("{}{:<8.2f} {:>8}{} | ",
                       is_gold ? BOLD_YELLOW : RESET,
                       static_cast<double>(segment_frames) / 30.0,
                       segment_frames,
                       RESET);
        else
            std::print("{:17} | ", "");

        // PB compare
        if (!pb_scuffed && pb.has_value() && pb.value().segments[i].frames > -1 && (is_finished_segment || is_current_segment)) {
            const auto& pb_run = pb.value();

            const auto pb_segment_frames = pb_run.segments[i].frames;
            pb_frames_total += pb_run.segments[i].frames;

            const auto total_diff = static_cast<int32_t>(total) - static_cast<int32_t>(pb_frames_total);
            const auto total_diff_sec = (double)total_diff / 30.0;
            const auto diff_frames = static_cast<int32_t>(segment_frames) - static_cast<int32_t>(pb_segment_frames);
            const double diff_sec = static_cast<double>(diff_frames) / 30.0;

            const char* segment_color = diff_frames < 0 ? GREEN : 
                                        diff_frames > 0 ? RED :
                                        RESET;

            const char* total_color = total_diff < 0 ? GREEN :
                                      total_diff > 0 ? RED :
                                      RESET;

            if (is_gold)
                segment_color = BOLD_YELLOW;

            std::print("{}{:<+8.2f} {}{:^+8.2f} {:>+8}{} | ", total_color, total_diff_sec, segment_color, diff_sec, diff_frames, RESET);
        } else {
            std::print("{:26} | ", " ");
            pb_scuffed = true;
        }

        // golds compare
        if (!sob_scuffed && sob.segments[i].frames > -1 && (is_finished_segment || is_current_segment)) {
            const auto sob_segment_frames = sob.segments[i].frames;
            sob_frames_total += sob.segments[i].frames;

            const auto total_diff = static_cast<int32_t>(total) - static_cast<int32_t>(sob_frames_total);
            const auto total_diff_sec = (double)total_diff / 30.0;
            const auto diff_frames = static_cast<int32_t>(segment_frames) - static_cast<int32_t>(sob_segment_frames);
            const double diff_sec = static_cast<double>(diff_frames) / 30.0;

            const char* segment_color = diff_frames < 0 ? GREEN : 
                                        diff_frames > 0 ? RED :
                                        RESET;

            const char* total_color = total_diff < 0 ? GREEN :
                                      total_diff > 0 ? RED :
                                      RESET;

            if (is_gold)
                segment_color = BOLD_YELLOW;

            std::print("{}{:<+8.2f} {}{:^+8.2f} {:>+8}{} | ", total_color, total_diff_sec, segment_color, diff_sec, diff_frames, RESET);
        } else {
            std::print("{:26} | ", " ");
            sob_scuffed = true;
        }

        std::print("\n");
    }
}

void print_run()
{
    std::println("\033[2J\033[H");  // clear screen + home

    constexpr static const char* line = "--------------------------------------------------------------------------------------------------------";

    std::println(line);

    std::println("| {}{:>2}{} | {}{:^17}{} | {}{:^17}{} | {}{:^26}{} | {}{:^26}{} |", 
                 CYAN, "#", RESET,
                 CYAN, "Total", RESET,
                 CYAN, "Segment", RESET,
                 CYAN, "PB", RESET,
                 BOLD_YELLOW, "SOB", RESET);

    print_table();

    std::println(line);

    uint32_t total_frames = current_run.current_frame;
    for (uint32_t i = 0; i < current_run.current_segment; ++i)
        total_frames += current_run.current_run.segments[i].frames;

    std::println("Time:  {:.2f} | {}", (double)(total_frames) / 30.0, total_frames);
    std::println("seg {}/{}  frame {}", current_run.current_segment + 1, NUM_SEGMENTS, current_run.current_frame);

    const auto sob_frames = database::instance.get_best_segments().total_frames();
    const auto sob_time = sob_frames / 30.0;

    std::println("sob: {:.2f} {:4}", sob_time, sob_frames);
}

void reset()
{
    if (current_run.current_run.segments[0].frames > -1)
        database::instance.save_run(current_run.current_run);

    current_run = running_run{};
    current_run.current_run.started_at = (uint64_t)std::time(NULL);
}

void split()
{
    if (current_run.finished) {
        std::println(stderr, "warning: tried to split when finished.");
        return;
    }

    current_run.split();
    print_run();
    if (current_run.finished) {
        current_run.current_run.did_finish = true;
        database::instance.save_run(current_run.current_run);
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

    while (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        std::println(stderr, "can't bind");
        sleep(2);
    }

    if (listen(server_fd, 1) == -1) {
        close(server_fd);
        std::println(stderr, "can't listen");
        return;
    }

    std::println(stderr, "listening");

    client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
    if (client_fd == -1) {
        std::println(stderr, "client broken");
        return;
    }

    std::println(stderr, "accepted client");

    while(1) {
        ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);
        if (n > 0) {
            process_received(buffer, static_cast<uint32_t>(n));
        } else break;
    }

    close(server_fd);
}

int main()
{
    database::instance.load_from_disk();
    socket_thread();
    database::instance.save_to_disk();
}
