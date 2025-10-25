// prime_threads.cpp
// C++17
// MSVC: cl /std:c++17 /O2 /EHsc prime_threads.cpp
// g++  : g++ -std=gnu++17 -O2 -pthread prime_threads.cpp -o prime_threads

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using u64 = unsigned long long;

/* ---------- time ---------- */
static inline auto nowtp() { return std::chrono::system_clock::now(); }

static std::string ts_ms(std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    auto t = system_clock::to_time_t(tp);
    auto ms = duration_cast<milliseconds>(tp.time_since_epoch()) % 1000;
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "."
        << std::setw(3) << std::setfill('0') << ms.count();
    return os.str();
}

/* ---------- config ---------- */
struct Config {
    int         threads = 8;
    u64         max_value = 50000;
    std::string division = "range";      // "range" | "per_number"
    std::string printing = "immediate";  // "immediate" | "deferred"
    bool        skip_even = true;
    bool        use_6k = false;
    int         log_every = -1;           // for B1 immediate; -1 = no CHECK lines
    bool        list_primes = false;
    bool        table_sum = true;
};

static std::string trim(std::string s) {
    auto a = s.find_first_not_of(" \t\r\n");
    auto b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, b - a + 1);
}
static Config parse_cfg(std::istream& in) {
    Config c;
    std::string line;
    while (std::getline(in, line)) {
        size_t c1 = line.find('#'), c2 = line.find(';');
        size_t cut = std::min(c1 == std::string::npos ? line.size() : c1,
            c2 == std::string::npos ? line.size() : c2);
        line = trim(line.substr(0, cut));
        if (line.empty()) continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq));
        std::string v = trim(line.substr(eq + 1));

        if (k == "threads")       c.threads = std::max(1, std::stoi(v));
        else if (k == "max_value")     c.max_value = static_cast<u64>(std::stoull(v));
        else if (k == "division")      c.division = v;
        else if (k == "printing")      c.printing = v;
        else if (k == "skip_even")     c.skip_even = (v == "1" || v == "true" || v == "True");
        else if (k == "use_6k")        c.use_6k = (v == "1" || v == "true" || v == "True");
        else if (k == "log_every")     c.log_every = std::stoi(v);
        else if (k == "list_primes")   c.list_primes = (v == "1" || v == "true" || v == "True");
        else if (k == "table_summary") c.table_sum = (v == "1" || v == "true" || v == "True");
    }
    return c;
}
static Config load_cfg(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::cerr << "WARN: can't open " << path << ", using defaults.\n";
        return {};
    }
    return parse_cfg(in);
}

/* ---------- logger ---------- */
enum class PrintMode { IMMEDIATE, DEFERRED };

struct Ev {
    std::chrono::system_clock::time_point tp;
    int         tid;
    std::string tag;  // RUN/START/PRIME/FIN
    std::string msg;
};

struct Logger {
    PrintMode mode{ PrintMode::IMMEDIATE };
    std::mutex m;
    std::vector<Ev> buf;
    int w_time = 23, w_tid = 2, w_tag = 6;

    explicit Logger(PrintMode pm) : mode(pm) {}

    void set_width(int T) {
        int w = 1, x = std::max(1, T - 1);
        while (x >= 10) { ++w; x /= 10; }
        w_tid = std::max(2, w);
    }

    void add(int tid, std::string tag, std::string msg) {
        Ev e{ nowtp(), tid, std::move(tag), std::move(msg) };
        if (mode == PrintMode::IMMEDIATE) {
            std::ostringstream os;
            os << std::left << std::setw(w_time) << ts_ms(e.tp) << "  "
                << "T" << std::right << std::setw(w_tid) << std::setfill('0') << (tid >= 0 ? tid : 0)
                << std::setfill(' ') << "  "
                << std::left << std::setw(w_tag) << e.tag << "  "
                << e.msg;
            std::lock_guard<std::mutex> lk(m);
            std::cout << os.str() << "\n";
        }
        else {
            std::lock_guard<std::mutex> lk(m);
            buf.push_back(std::move(e));
        }
    }

    void run(const std::string& s) { add(-1, "RUN", s); }
    void start(int tid, const std::string& s) { add(tid, "START", s); }
    void prime(int tid, u64 n) { add(tid, "PRIME", "n=" + std::to_string(n)); }
    void finish(int tid, const std::string& s) { add(tid, "FIN", s); }

    void line(const Ev& e, const std::string& body) {
        std::ostringstream os;
        os << std::left << std::setw(w_time) << ts_ms(e.tp) << "  "
            << "T" << std::right << std::setw(w_tid) << std::setfill('0') << (e.tid >= 0 ? e.tid : 0)
            << std::setfill(' ') << "  " << body;
        std::cout << os.str() << "\n";
    }

    // A2: after compute, print in three blocks
    void flush_deferred() {
        std::lock_guard<std::mutex> lk(m);
        std::vector<Ev> starts, fins, primes;
        for (auto& e : buf) {
            if (e.tag == "START") starts.push_back(e);
            else if (e.tag == "FIN") fins.push_back(e);
            else if (e.tag == "PRIME") primes.push_back(e);
        }
        auto by_tid_time = [](const Ev& a, const Ev& b) {
            if (a.tid != b.tid) return a.tid < b.tid;
            return a.tp < b.tp;
            };
        std::sort(starts.begin(), starts.end(), by_tid_time);
        std::sort(fins.begin(), fins.end(), by_tid_time);
        std::sort(primes.begin(), primes.end(), by_tid_time);

        std::cout << "=== Thread Starts ===\n";
        for (auto& e : starts) line(e, "Thread " + std::to_string(e.tid) + " started (" + e.msg + ")");

        std::cout << "\n=== Thread Finishes ===\n";
        for (auto& e : fins)   line(e, "Thread " + std::to_string(e.tid) + " finished (" + e.msg + ")");

        std::cout << "\n=== Results (Primes) ===\n";
        for (auto& e : primes) line(e, "Thread " + std::to_string(e.tid) + " | Prime: " + e.msg.substr(2));

        buf.clear();
    }
};

/* ---------- primality ---------- */
static inline bool is_6kpm1(u64 d) { return (d % 6 == 1) || (d % 6 == 5); }

// B1 single-thread primality
static bool prime_single(u64 n, const Config& c) {
    if (n < 2) return false;
    if (n == 2) return true;
    if (c.skip_even && n % 2 == 0) return false;

    u64 lim = (u64)std::sqrt((long double)n);
    if (c.use_6k) {
        for (u64 d = 3; d <= lim; ++d) {
            if (!is_6kpm1(d)) continue;
            if (n % d == 0) return false;
        }
    }
    else {
        for (u64 d = 3; d <= lim; d += 2) {
            if (n % d == 0) return false;
        }
    }
    return true;
}

// B2: split divisors among T threads (no CHECK logs to keep it fast)
static bool prime_parallel(u64 n, const Config& c, int T) {
    if (n < 2) return false;
    if (n == 2) return true;
    if (c.skip_even && n % 2 == 0) return false;

    u64 lim = (u64)std::sqrt((long double)n);
    std::vector<u64> divs;
    if (!c.skip_even) divs.push_back(2);
    for (u64 d = 3; d <= lim; d += 2) {
        if (c.use_6k && !is_6kpm1(d)) continue;
        divs.push_back(d);
    }
    if (divs.empty()) return true;

    std::atomic<bool> found(false);
    std::vector<std::thread> ths; ths.reserve(T);

    auto worker = [&](int /*tid*/, size_t L, size_t R) {
        for (size_t i = L; i < R && !found.load(std::memory_order_relaxed); ++i) {
            if (n % divs[i] == 0) { found.store(true, std::memory_order_relaxed); break; }
        }
        };

    for (int t = 0; t < T; ++t) {
        size_t L = (size_t)((divs.size() * 1ULL * t) / T);
        size_t R = (size_t)((divs.size() * 1ULL * (t + 1)) / T);
        ths.emplace_back(worker, t, L, R);
    }
    for (auto& th : ths) th.join();
    return !found.load(std::memory_order_relaxed);
}

/* ---------- runs ---------- */
struct Result {
    std::vector<u64> primes;
    u64 processed = 0;
    std::vector<u64> primes_per_thread;
    std::vector<u64> proc_per_thread;
};

// B1: contiguous numeric ranges per thread
static Result run_B1(const Config& c, Logger& log) {
    Result r;
    const int T = std::max(1, c.threads);
    u64 N = c.max_value;
    r.primes_per_thread.assign(T, 0);
    r.proc_per_thread.assign(T, 0);

    log.run("Variant=A" + std::string(c.printing == "immediate" ? "1" : "2") + "B1  threads=" + std::to_string(T) + "  max=" + std::to_string(N));

    auto chunk = [&](int t)->std::pair<u64, u64> {
        u64 lo = (N * 1ULL * t) / T + 1;
        u64 hi = (N * 1ULL * (t + 1)) / T;
        if (lo < 2) lo = 2;
        return { lo,hi };
        };

    std::mutex mx;
    std::vector<std::thread> ths; ths.reserve(T);

    for (int tid = 0; tid < T; ++tid) {
        auto [lo, hi] = chunk(tid);
        ths.emplace_back([&, tid, lo, hi] {
            {
                std::ostringstream os; os << "range=[" << lo << "-" << hi << "]";
                log.start(tid, os.str());
            }
            std::vector<u64> mine;
            u64 done = 0;

            for (u64 n = lo; n <= hi; ++n) {
                // optional CHECKs only for B1+immediate (if log_every>=0)
                if (c.printing == "immediate" && c.log_every >= 0) {
                    if (c.log_every == 0 || (done % c.log_every) == 0) {
                        u64 lim = (u64)std::sqrt((long double)n);
                        std::ostringstream os; os << "testing n=" << n << " up to " << lim;
                        log.add(tid, "CHECK", os.str());
                    }
                }
                if (prime_single(n, c)) { log.prime(tid, n); mine.push_back(n); }
                ++done;
            }
            {
                std::lock_guard<std::mutex> lk(mx);
                r.primes.insert(r.primes.end(), mine.begin(), mine.end());
                r.primes_per_thread[tid] = mine.size();
                r.proc_per_thread[tid] = done;
                r.processed += done;
            }
            std::ostringstream os;
            os << "range=[" << lo << "-" << hi << "], processed=" << done << ", primes=" << mine.size();
            log.finish(tid, os.str());
            });
    }
    for (auto& th : ths) th.join();
    return r;
}

// B2: per-number, share divisors among threads; owner chosen round-robin (balanced)
static Result run_B2(const Config& c, Logger& log) {
    Result r;
    const int T = std::max(1, c.threads);
    u64 N = c.max_value;

    log.run("Variant=A" + std::string(c.printing == "immediate" ? "1" : "2") + "B2  threads=" + std::to_string(T) + "  max=" + std::to_string(N));

    for (int tid = 0; tid < T; ++tid) log.start(tid, "owner mode");

    std::vector<u64> proc_by(T, 0), primes_by(T, 0);
    int next_owner = 0; // round-robin owner assignment for nicer per-thread balance

    for (u64 n = 2; n <= N; ++n) {
        if (c.skip_even && n > 2 && (n % 2 == 0)) { ++r.processed; continue; }

        const int owner = next_owner;
        next_owner = (next_owner + 1) % T;

        proc_by[owner]++;

        // no CHECK lines in B2 (keeps it fast/clean)
        bool is_p = prime_parallel(n, c, T);
        if (is_p) { log.prime(owner, n); r.primes.push_back(n); primes_by[owner]++; }
        ++r.processed;
    }

    r.proc_per_thread = proc_by;
    r.primes_per_thread = primes_by;

    for (int tid = 0; tid < T; ++tid) {
        std::ostringstream os; os << "owner processed=" << proc_by[tid] << ", primes=" << primes_by[tid];
        log.finish(tid, os.str());
    }
    return r;
}

/* ---------- variant picker ---------- */
struct Variant { const char* key; const char* div; const char* print; const char* label; };
static std::string lower(std::string s) { for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch); return s; }
static const Variant VARS[] = {
    {"a1b1","range","immediate","A1B1 (Immediate + Range)"},
    {"a2b1","range","deferred", "A2B1 (Deferred  + Range)"},
    {"a1b2","per_number","immediate","A1B2 (Immediate + Per-number)"},
    {"a2b2","per_number","deferred", "A2B2 (Deferred  + Per-number)"}
};
static int find_var(std::string tok) { tok = lower(tok); for (int i = 0; i < 4; ++i) if (tok == VARS[i].key) return i; return -1; }
static int ask_variant() {
    while (true) {
        std::cout << "=== Variant Picker ===\n";
        for (int i = 0; i < 4; ++i) std::cout << " " << (i + 1) << ") " << VARS[i].label << "  [" << VARS[i].key << "]\n";
        std::cout << "Choose 1-4, or Q: ";
        std::string s; if (!std::getline(std::cin, s)) return -1;
        s = trim(s);
        if (s.empty()) continue;
        if (s.size() == 1 && (s[0] == 'q' || s[0] == 'Q')) return -1;
        if (s.size() == 1 && std::isdigit((unsigned char)s[0])) {
            int k = s[0] - '0'; if (1 <= k && k <= 4) return k - 1;
        }
        int k = find_var(s); if (k >= 0) return k;
        std::cout << "Invalid.\n\n";
    }
}

/* ---------- summaries ---------- */
static void print_summary(const Config& c, const Result& r) {
    std::cout << "\n=== Summary ===\n";
    std::cout << "Division:  " << c.division << "   Printing: " << c.printing << "\n";
    std::cout << "Processed: " << r.processed << " numbers\n";
    std::cout << "Primes:    " << r.primes.size() << "\n";
}

static void print_table(const Config& c, const Result& r) {
    const int T = std::max(1, c.threads);

    auto range_of = [&](int t)->std::pair<u64, u64> {
        u64 lo = (c.max_value * 1ULL * t) / T + 1;
        u64 hi = (c.max_value * 1ULL * (t + 1)) / T;
        if (lo < 2) lo = 2;
        return { lo,hi };
        };

    std::cout << "\n=== Per-thread ===\n";
    std::cout << std::left << std::setw(8) << "Thread"
        << std::left << std::setw(20) << (c.division == "range" ? "Range" : "Owner")
        << std::right << std::setw(14) << "Processed"
        << std::right << std::setw(10) << "Primes" << "\n";

    for (int t = 0; t < T; ++t) {
        std::string where = (c.division == "range")
            ? (std::to_string(range_of(t).first) + "-" + std::to_string(range_of(t).second))
            : "owner";
        u64 proc = (t < (int)r.proc_per_thread.size()) ? r.proc_per_thread[t] : 0;
        u64 p = (t < (int)r.primes_per_thread.size()) ? r.primes_per_thread[t] : 0;

        std::cout << std::left << std::setw(8) << t
            << std::left << std::setw(20) << where
            << std::right << std::setw(14) << proc
            << std::right << std::setw(10) << p << "\n";
    }

    if (c.list_primes && !r.primes.empty()) {
        std::cout << "\nPrimes:\n";
        for (size_t i = 0; i < r.primes.size(); ++i)
            std::cout << r.primes[i] << (i + 1 < r.primes.size() ? ' ' : '\n');
    }
}

/* ---------- main ---------- */
int main(int argc, char** argv) {
    Config cfg = load_cfg("config.ini");

    int vidx = (argc >= 2 ? find_var(argv[1]) : -1);
    if (vidx < 0) {
        vidx = ask_variant();
        if (vidx < 0) { std::cout << "Goodbye.\n"; return 0; }
    }
    cfg.division = VARS[vidx].div;
    cfg.printing = VARS[vidx].print;

    PrintMode pm = (cfg.printing == "deferred") ? PrintMode::DEFERRED : PrintMode::IMMEDIATE;
    Logger log(pm);
    log.set_width(std::max(1, cfg.threads));

    log.run("Program started");

    Result r;
    if (cfg.division == "range") r = run_B1(cfg, log);
    else                       r = run_B2(cfg, log);

    log.run("Program finished");

    if (pm == PrintMode::DEFERRED) log.flush_deferred();
    print_summary(cfg, r);
    if (cfg.table_sum) print_table(cfg, r);

    return 0;
}