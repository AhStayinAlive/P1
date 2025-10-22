// prime_threads.cpp
// Build (MSVC): cl /std:c++17 /O2 /EHsc prime_threads.cpp
// Build (g++):  g++ -std=gnu++17 -O2 -pthread prime_threads.cpp -o prime_threads

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using u64 = unsigned long long;

// ---------- time stamp ----------
static std::string now_stamp() {
    using namespace std::chrono;
    auto tp = system_clock::now();
    std::time_t t = system_clock::to_time_t(tp);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream os;
    os << "[" << std::put_time(&tm, "%H:%M:%S") << "]";
    return os.str();
}

// ---------- config ----------
struct Config {
    int         threads = 8;          // x
    u64         max_value = 50000;      // y
    std::string division = "range";    // "range" | "per_number"
    std::string printing = "immediate";// "immediate" | "deferred"
    bool        skip_even = true;
    bool        use_6k = false;      // 6k ± 1 stepping for divisors
    int         log_every = 0;          // 0=every check; N>0=every Nth; -1=range summary
    bool        list_primes = false;      // dump primes at end
};

// small trimming helper
static std::string trim(std::string s) {
    auto a = s.find_first_not_of(" \t\r\n");
    auto b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, b - a + 1);
}

// parse key=val lines from a stream into config
static Config parse_config_stream(std::istream& in) {
    Config c;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (line[0] == '#' || line[0] == ';') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq));
        std::string v = trim(line.substr(eq + 1));
        if (k == "threads") c.threads = std::max(1, std::stoi(v));
        else if (k == "max_value") c.max_value = static_cast<u64>(std::stoull(v));
        else if (k == "division") c.division = v;
        else if (k == "printing") c.printing = v;
        else if (k == "skip_even") c.skip_even = (v == "1" || v == "true" || v == "True");
        else if (k == "use_6k") c.use_6k = (v == "1" || v == "true" || v == "True");
        else if (k == "log_every") c.log_every = std::stoi(v);
        else if (k == "list_primes") c.list_primes = (v == "1" || v == "true" || v == "True");
    }
    return c;
}

// standard load (prints a warning if file missing)
static Config load_config(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::cerr << now_stamp() << " [----] [WARN] Could not open config file: " << path
            << " -> using defaults.\n";
        return Config{};
    }
    return parse_config_stream(in);
}

// silent try-load (for menu listing)
static bool try_load_config(const std::string& path, Config& out) {
    std::ifstream in(path);
    if (!in) return false;
    out = parse_config_stream(in);
    return true;
}

static bool file_exists(const std::string& p) {
    std::ifstream f(p);
    return f.good();
}

// ---------- logger ----------
enum class PrintMode { IMMEDIATE, DEFERRED };

struct Logger {
    PrintMode mode;
    std::mutex m;
    std::vector<std::string> buf;
    int thread_pad = 2; // width for T## zero-padding

    explicit Logger(PrintMode pm) : mode(pm) {}

    void set_thread_width(int T) {
        if (T <= 1) { thread_pad = 1; return; }
        int w = 0; int n = T - 1;
        while (n > 0) { ++w; n /= 10; }
        thread_pad = std::max(2, w);
    }

    std::string tid_label(int tid) {
        if (tid < 0) return "----";
        std::ostringstream os;
        os << 'T' << std::setw(thread_pad) << std::setfill('0') << tid;
        return os.str();
    }

    void raw(const std::string& s) {
        std::lock_guard<std::mutex> lk(m);
        if (mode == PrintMode::IMMEDIATE) std::cout << s << "\n";
        else                               buf.push_back(s);
    }

    void ev(int tid, const std::string& tag, const std::string& msg) {
        std::ostringstream os;
        os << now_stamp() << " [" << tid_label(tid) << "] [" << tag << "] " << msg;
        raw(os.str());
    }

    void run(const std::string& msg) { ev(-1, "RUN", msg); }
    void info(const std::string& msg) { ev(-1, "INFO", msg); }
    void start(int tid, const std::string& msg) { ev(tid, "START", msg); }
    void check(int tid, const std::string& msg) { ev(tid, "CHECK", msg); }
    void prime(int tid, const std::string& msg) { ev(tid, "PRIME", msg); }
    void finish(int tid, const std::string& msg) { ev(tid, "FINISH", msg); }

    void flush() {
        std::lock_guard<std::mutex> lk(m);
        for (auto& s : buf) std::cout << s << "\n";
        buf.clear();
    }
};

// ---------- primality helpers ----------
static inline bool is_6kpm1(u64 d) { return (d % 6 == 1) || (d % 6 == 5); }

// single-thread primality test (for range-split scheme)
static bool is_prime_single(u64 n, const Config& cfg, int tid, Logger& log, int& counter) {
    if (n < 2) return false;
    if (n == 2) return true;
    if (cfg.skip_even && n % 2 == 0) return false;

    u64 limit = (u64)std::sqrt((long double)n);
    auto should_log = [&](u64 /*d*/) -> bool {
        if (cfg.log_every == 0) return true;
        if (cfg.log_every > 0)  return (++counter % cfg.log_every) == 0;
        return false; // -1 -> we do "range summary" elsewhere (not here)
        };

    if (!cfg.skip_even) {
        if (should_log(2)) log.check(tid, "d=2");
        if (n % 2 == 0) return false;
    }

    if (cfg.use_6k) {
        for (u64 d = 3; d <= limit; ++d) {
            if (!is_6kpm1(d)) continue;
            if (should_log(d)) log.check(tid, "d=" + std::to_string(d));
            if (n % d == 0) return false;
        }
    }
    else {
        for (u64 d = 3; d <= limit; d += 2) {
            if (should_log(d)) log.check(tid, "d=" + std::to_string(d));
            if (n % d == 0) return false;
        }
    }
    return true;
}

// parallel divisibility test for a *single* n (per-number scheme)
static bool is_prime_parallel(u64 n, const Config& cfg, Logger& log) {
    if (n < 2) return false;
    if (n == 2) return true;
    if (cfg.skip_even && n % 2 == 0) return false;

    const int T = std::max(1, cfg.threads);
    u64 limit = (u64)std::sqrt((long double)n);

    std::vector<u64> cand;
    if (!cfg.skip_even) cand.push_back(2);
    for (u64 d = 3; d <= limit; d += 2) {
        if (cfg.use_6k && !is_6kpm1(d)) continue;
        cand.push_back(d);
    }
    if (cand.empty()) return true;

    std::atomic<bool> found(false);
    std::vector<std::thread> ths; ths.reserve(T);

    auto worker = [&](int tid, size_t L, size_t R) {
        if (L >= R) return;
        if (cfg.log_every == -1) {
            u64 a = cand[L], b = cand[R - 1];
            log.check(tid, "range d=[" + std::to_string(a) + ".." + std::to_string(b) + "] for n=" + std::to_string(n));
        }
        int counter = 0;
        for (size_t i = L; i < R && !found.load(std::memory_order_relaxed); ++i) {
            u64 d = cand[i];
            if (cfg.log_every >= 0) {
                bool do_log = (cfg.log_every == 0) ? true : ((++counter % cfg.log_every) == 0);
                if (do_log) log.check(tid, "d=" + std::to_string(d) + " (n=" + std::to_string(n) + ")");
            }
            if (n % d == 0) {
                found.store(true, std::memory_order_relaxed);
                break;
            }
        }
        };

    for (int t = 0; t < T; ++t) {
        size_t L = (size_t)((cand.size() * 1ULL * t) / T);
        size_t R = (size_t)((cand.size() * 1ULL * (t + 1)) / T);
        ths.emplace_back(worker, t, L, R);
    }
    for (auto& th : ths) th.join();
    return !found.load(std::memory_order_relaxed);
}

// ---------- drivers ----------
struct RunResult {
    std::vector<u64> primes;
    u64 processed = 0;
    std::vector<u64> primes_per_thread; // only meaningful for range-split
};

static RunResult run_division_range(const Config& cfg, Logger& log) {
    RunResult rr;
    const int T = std::max(1, cfg.threads);
    u64 N = cfg.max_value;
    rr.primes_per_thread.assign(T, 0);

    log.run("Start (division=range, printing=" + cfg.printing + "), N=" + std::to_string(N) + ", T=" + std::to_string(T));

    auto block_of = [&](int t)->std::pair<u64, u64> {
        u64 start = (N * 1ULL * t) / T + 1;
        u64 end = (N * 1ULL * (t + 1)) / T;
        if (start < 2) start = 2;
        return { start, end }; // inclusive
        };

    std::mutex primes_m;
    std::vector<std::thread> ths; ths.reserve(T);

    for (int t = 0; t < T; ++t) {
        auto [lo, hi] = block_of(t);
        ths.emplace_back([&, t, lo, hi] {
            log.start(t, "range=[" + std::to_string(lo) + "-" + std::to_string(hi) + "]");
            std::vector<u64> local;
            int check_counter = 0;
            for (u64 n = lo; n <= hi; ++n) {
                bool prime = is_prime_single(n, cfg, t, log, check_counter);
                if (prime) {
                    log.prime(t, "n=" + std::to_string(n));
                    local.push_back(n);
                }
            }
            {
                std::lock_guard<std::mutex> lk(primes_m);
                rr.primes.insert(rr.primes.end(), local.begin(), local.end());
                rr.processed += (hi >= lo ? (hi - lo + 1) : 0);
                rr.primes_per_thread[t] += local.size();
            }
            log.finish(t, "range=[" + std::to_string(lo) + "-" + std::to_string(hi) + "]");
            });
    }
    for (auto& th : ths) th.join();
    log.run("End");
    return rr;
}

static RunResult run_division_per_number(const Config& cfg, Logger& log) {
    RunResult rr;
    const int T = std::max(1, cfg.threads);
    u64 N = cfg.max_value;

    log.run("Start (division=per_number, printing=" + cfg.printing + "), N=" + std::to_string(N) + ", T=" + std::to_string(T));

    for (u64 n = 2; n <= N; ++n) {
        if (cfg.skip_even && n > 2 && n % 2 == 0) { rr.processed++; continue; }
        bool prime = is_prime_parallel(n, cfg, log);
        if (prime) {
            // log under thread 0 so every line has a thread id
            log.prime(0, "n=" + std::to_string(n));
            rr.primes.push_back(n);
        }
        rr.processed++;
    }
    log.run("End");
    return rr;
}

// ---------- preset & menu ----------
struct Preset {
    std::string key;      // range_immediate, ...
    std::string filename; // key + ".ini"
};

static std::string to_lower_copy(std::string s) {
    for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}
static std::string normalize_token(std::string s) {
    s = to_lower_copy(s);
    if (s.size() >= 4 && s.substr(s.size() - 4) == ".ini") s.erase(s.size() - 4);
    for (auto& ch : s) if (ch == '-' || ch == ' ') ch = '_';
    return s;
}

static const std::vector<Preset> PRESETS = {
    {"range_immediate",        "range_immediatee.ini"},
    {"range_deferred",         "range_deferred.ini"},
    {"per_number_immediate",   "per_number_immediate.ini"},
    {"per_number_deferred",    "per_number_deferred.ini"},
};

static void show_preset_status() {
    std::cout << "=== Prime Threads (Menu) ===\n";
    std::cout << "Looking for preset config files in current folder:\n\n";
    std::cout << std::left << std::setw(26) << "Filename"
        << std::setw(10) << "Status"
        << "Details\n";
    std::cout << std::string(70, '-') << "\n";

    for (const auto& p : PRESETS) {
        Config cfg;
        bool ok = try_load_config(p.filename, cfg);
        std::ostringstream details;
        if (ok) {
            details << "threads=" << cfg.threads
                << "  max_value=" << cfg.max_value
                << "  division=" << cfg.division
                << "  printing=" << cfg.printing;
        }
        else {
            details << "(missing or unreadable)";
        }
        std::cout << std::left << std::setw(26) << p.filename
            << std::setw(10) << (ok ? "OK" : "MISSING")
            << details.str() << "\n";
    }
    std::cout << std::string(70, '-') << "\n\n";
}

static std::string pick_config_interactively() {
    while (true) {
        show_preset_status();
        std::cout << "Choose a preset [1-4], or type a filename, or Q to quit:\n";
        for (size_t i = 0; i < PRESETS.size(); ++i) {
            std::cout << " " << (i + 1) << ") " << PRESETS[i].key << "  -> " << PRESETS[i].filename << "\n";
        }
        std::cout << "> ";
        std::string choice;
        if (!std::getline(std::cin, choice)) return {};
        choice = trim(choice);
        if (choice.empty()) continue;
        if (choice.size() == 1 && (choice[0] == 'q' || choice[0] == 'Q')) return {};

        // numeric 1..4
        if (choice.size() == 1 && std::isdigit((unsigned char)choice[0])) {
            int idx = choice[0] - '0';
            if (1 <= idx && idx <= (int)PRESETS.size()) {
                std::string fn = PRESETS[idx - 1].filename;
                if (!file_exists(fn)) {
                    std::cout << "File not found: " << fn << "\n\n";
                    continue;
                }
                return fn;
            }
        }
        // token or filename
        std::string tok = normalize_token(choice);
        for (const auto& p : PRESETS) {
            if (tok == p.key) {
                if (!file_exists(p.filename)) {
                    std::cout << "File not found: " << p.filename << "\n\n";
                    goto prompt_again;
                }
                return p.filename;
            }
        }
        // treat as filename
        if (!file_exists(choice)) {
            std::cout << "File not found: " << choice << "\n\n";
            continue;
        }
        return choice;

    prompt_again:
        continue;
    }
}

static void print_summary(const Config& cfg, const RunResult& rr) {
    std::cout << "\n=== Summary ===\n";
    std::cout << "Division: " << cfg.division << "   Printing: " << cfg.printing << "\n";
    std::cout << "Processed: " << rr.processed << " numbers\n";
    std::cout << "Primes:    " << rr.primes.size() << "\n";
    if (!rr.primes_per_thread.empty()) {
        std::cout << "Primes per thread: ";
        for (size_t i = 0; i < rr.primes_per_thread.size(); ++i) {
            std::cout << (i ? ", " : "") << "T" << i << "=" << rr.primes_per_thread[i];
        }
        std::cout << "\n";
    }
}

// ---------- main ----------
int main(int argc, char** argv) {
    // Command-line shortcut still works:
    //   prime_threads.exe range_immediate
    //   prime_threads.exe per_number_deferred.ini
    std::string cfg_file;

    if (argc >= 2) {
        std::string tok = normalize_token(argv[1]);
        bool matched = false;
        for (const auto& p : PRESETS) {
            if (tok == p.key) { cfg_file = p.filename; matched = true; break; }
        }
        if (!matched) cfg_file = argv[1]; // treat as filename
    }

    while (true) {
        if (cfg_file.empty()) {
            cfg_file = pick_config_interactively();
            if (cfg_file.empty()) {
                std::cout << "Goodbye!\n";
                return 0;
            }
        }

        // Load & run
        Config cfg = load_config(cfg_file);
        PrintMode pm = (cfg.printing == "deferred") ? PrintMode::DEFERRED : PrintMode::IMMEDIATE;

        Logger log(pm);
        log.set_thread_width(std::max(1, cfg.threads));
        log.info("Using config file: " + cfg_file);

        RunResult rr;
        if (cfg.division == "per_number") rr = run_division_per_number(cfg, log);
        else                               rr = run_division_range(cfg, log);

        if (pm == PrintMode::DEFERRED) log.flush();
        print_summary(cfg, rr);

        // Optional: list all primes
        if (cfg.list_primes && !rr.primes.empty()) {
            std::cout << "Primes list:\n";
            for (size_t i = 0; i < rr.primes.size(); ++i) {
                std::cout << rr.primes[i] << (i + 1 < rr.primes.size() ? ' ' : '\n');
            }
        }

        // Ask to run again
        std::cout << "\nRun another config? (y/n) ";
        std::string ans; std::getline(std::cin, ans);
        if (!ans.empty() && (ans[0] == 'y' || ans[0] == 'Y')) {
            cfg_file.clear(); // go back to menu
            std::cout << "\n";
            continue;
        }
        break;
    }

    return 0;
}
