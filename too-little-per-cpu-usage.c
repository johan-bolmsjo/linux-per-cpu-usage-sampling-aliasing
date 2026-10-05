/*
 * too-little-per-cpu-usage.c
 *
 * Demonstrates how Linux can report almost no per-processor usage for a single
 * thread that spins almost continuously. The thread spins and then sleeps for a
 * short window at a fixed phase. When the sleep window covers the kernel's
 * per-processor sampling instant, the sample charges the idle task instead of
 * the running thread, so the per-processor busy counter stays near zero while
 * the thread's real processor time stays high. The sampling phase is not known
 * up front. An in-program feedback loop finds it and then shortens the sleep
 * until the report can no longer be kept near zero.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/timerfd.h>

#define NANOSECONDS_PER_SECOND 1000000000LL
#define NANOSECONDS_PER_MICROSECOND 1000LL
#define NANOSECONDS_PER_MILLISECOND 1000000LL

#define DEFAULT_PROCESSOR 0
#define DEFAULT_TIMERFD_PERIOD_NANOSECONDS (1LL * NANOSECONDS_PER_MILLISECOND)
#define DEFAULT_SLEEP_DURATION_NANOSECONDS (250LL * NANOSECONDS_PER_MICROSECOND)
#define DEFAULT_SETTLE_DURATION_SECONDS 0.5
#define DEFAULT_REPORT_DURATION_SECONDS 1.0

// The reported per-processor usage must stay below this percentage for the
// effect to be considered intact.
#define MAXIMUM_ACCEPTABLE_REPORTED_PERCENT 5.0

// A small floor that keeps the suppression ratio finite when the reported usage
// is zero. It is expressed as a percentage of wall time.
#define MINIMUM_REPORTED_USAGE_PERCENT 0.01

// The feedback loop first samples the whole timerfd period and then narrows the
// search window around the best phase. The number of coarse samples grows with
// the ratio of the period to the sleep duration so that a narrow plateau is not
// stepped over.
#define MINIMUM_COARSE_PHASE_STEP_COUNT 32
#define MAXIMUM_COARSE_PHASE_STEP_COUNT 128
#define PHASE_REFINEMENT_ROUND_COUNT 3
#define PHASE_REFINEMENT_STEP_COUNT 9
#define PHASE_REFINEMENT_WINDOW_DIVISOR 4

// The phase is found with the configured sleep duration. The sleep duration is
// then halved and the phase recalibrated until the report rises above the
// acceptable percentage or the minimum duration is reached. Below roughly 5 us
// the sleep is too short to stay synchronized with the sampling instant and the
// report starts to flicker.
#define SLEEP_REDUCTION_FACTOR 2
#define MINIMUM_SLEEP_DURATION_NANOSECONDS 5000LL

// Two measurements closer than this percentage are treated as equivalent and
// the one that consumes more processor time wins.
#define MEASUREMENT_COMPARISON_EPSILON 0.01

// The effect is re-searched when the reported usage rises above this multiple of
// the best usage found so far.
#define RETRIGGER_USAGE_MULTIPLE 2.0

// ProcessorTimeSample holds the accumulated time counters that /proc/stat
// reports for one processor.
struct ProcessorTimeSample {
    unsigned long long busy_ticks;
    unsigned long long idle_ticks;
    unsigned long long total_ticks;
};

// TaskTimeSample holds the accumulated user and system time that
// /proc/self/stat reports for the calling process.
struct TaskTimeSample {
    unsigned long long user_ticks;
    unsigned long long system_ticks;
};

// DemonstrationContext holds the settings that stay fixed for the whole run.
struct DemonstrationContext {
    int processor;
    long long period_nanoseconds;
    long long sleep_duration_nanoseconds;
    long long settle_duration_nanoseconds;
    long long report_duration_nanoseconds;
    double background_processor_usage_percent;
};

// TrialMeasurement holds the usage that one evaluation produced.
// reported_processor_usage_percent is the raw per-processor value,
// induced_processor_usage_percent subtracts the background usage, and
// thread_usage_percent is the processor time the thread actually consumed.
struct TrialMeasurement {
    long long phase_nanoseconds;
    long long sleep_duration_nanoseconds;
    double reported_processor_usage_percent;
    double induced_processor_usage_percent;
    double task_usage_percent;
    double thread_usage_percent;
    double score;
    unsigned long sleep_count;
};

// g_stop_requested is set by the termination signal handler to end the current
// evaluation and the program cleanly.
static volatile sig_atomic_t g_stop_requested = 0;

// handle_stop_signal records that the program received a termination signal.
static void handle_stop_signal(int signal_number)
{
    (void)signal_number;
    g_stop_requested = 1;
}

// monotonic_nanoseconds returns the current CLOCK_MONOTONIC time in
// nanoseconds.
static long long monotonic_nanoseconds(void)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (long long)time.tv_sec * NANOSECONDS_PER_SECOND + time.tv_nsec;
}

// thread_processor_nanoseconds returns the processor time consumed by the
// calling thread in nanoseconds.
static long long thread_processor_nanoseconds(void)
{
    struct timespec time;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time);
    return (long long)time.tv_sec * NANOSECONDS_PER_SECOND + time.tv_nsec;
}

// sleep_nanoseconds suspends the calling thread for the requested duration.
static void sleep_nanoseconds(long long duration_nanoseconds)
{
    struct timespec request;
    request.tv_sec = duration_nanoseconds / NANOSECONDS_PER_SECOND;
    request.tv_nsec = duration_nanoseconds % NANOSECONDS_PER_SECOND;

    struct timespec remaining;
    while (nanosleep(&request, &remaining) != 0 && errno == EINTR) {
        request = remaining;
    }
}

// spin_until consumes processor time until the requested CLOCK_MONOTONIC time.
// The volatile accumulator and the clock reads keep the loop from being
// optimized away.
static void spin_until(long long deadline_nanoseconds)
{
    volatile unsigned long accumulator = 0;
    while (monotonic_nanoseconds() < deadline_nanoseconds) {
        accumulator += 1;
    }
}

// sleep_with_timerfd suspends the calling thread for the requested duration
// using the timerfd. It returns zero on success and a negative value on
// failure.
static int sleep_with_timerfd(int timer_fd, long long duration_nanoseconds)
{
    struct itimerspec timer_specification;
    memset(&timer_specification, 0, sizeof timer_specification);
    timer_specification.it_value.tv_sec = duration_nanoseconds / NANOSECONDS_PER_SECOND;
    timer_specification.it_value.tv_nsec = duration_nanoseconds % NANOSECONDS_PER_SECOND;

    if (timerfd_settime(timer_fd, 0, &timer_specification, NULL) != 0) {
        return -1;
    }

    uint64_t expiration_count;
    ssize_t read_result = read(timer_fd, &expiration_count, sizeof expiration_count);
    if (read_result != (ssize_t)sizeof expiration_count && errno != EINTR) {
        return -1;
    }
    return 0;
}

// print_usage writes the command line summary to standard output.
static void print_usage(const char *program_name)
{
    printf("Usage: %s [OPTIONS] [PROCESSOR]\n", program_name);
    printf("\n");
    printf("Demonstrates how Linux can report almost no per-processor usage for a\n");
    printf("single thread that consumes almost all of a processor. The thread spins\n");
    printf("and sleeps for a short window at a fixed phase. A feedback loop\n");
    printf("synchronizes the sleep window with the kernel sampling instant and then\n");
    printf("shortens the sleep while the report stays near zero. The demonstration\n");
    printf("runs forever.\n");
    printf("\n");
    printf("Options:\n");
    printf("  -c, --cpu PROCESSOR      processor core to run on (default %d)\n", DEFAULT_PROCESSOR);
    printf("  -p, --period DURATION    timerfd period, for example 1ms (default 1ms)\n");
    printf("  -l, --sleep DURATION     starting sleep duration per period\n");
    printf("                           (default 250us; shortened automatically)\n");
    printf("  -s, --settle SECONDS     settling time per phase (default %.1f)\n",
           DEFAULT_SETTLE_DURATION_SECONDS);
    printf("  -r, --report SECONDS     reporting interval (default %.1f)\n",
           DEFAULT_REPORT_DURATION_SECONDS);
    printf("  -h, --help               display this message and exit\n");
    printf("\n");
    printf("DURATION accepts a number with an optional ns, us or ms suffix.\n");
    printf("A positional PROCESSOR argument is accepted as an alternative to\n");
    printf("--cpu.\n");
}

// parse_processor converts the processor argument text into a processor index.
static int parse_processor(const char *text)
{
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 0 || value > INT_MAX) {
        fprintf(stderr, "invalid processor: %s\n", text);
        exit(EXIT_FAILURE);
    }
    return (int)value;
}

// parse_duration_seconds converts a duration argument into seconds.
static double parse_duration_seconds(const char *text)
{
    char *end = NULL;
    double value = strtod(text, &end);
    if (end == text || *end != '\0' || value <= 0.0 || !isfinite(value)) {
        fprintf(stderr, "invalid duration: %s\n", text);
        exit(EXIT_FAILURE);
    }
    return value;
}

// parse_nanoseconds converts a duration argument into nanoseconds. The text is
// a number with an optional ns, us or ms suffix.
static long long parse_nanoseconds(const char *text)
{
    char *end = NULL;
    double value = strtod(text, &end);
    if (end == text || !isfinite(value) || value <= 0.0) {
        fprintf(stderr, "invalid duration: %s\n", text);
        exit(EXIT_FAILURE);
    }

    long long multiplier = 1;
    if (strcmp(end, "ns") == 0 || *end == '\0') {
        multiplier = 1;
    } else if (strcmp(end, "us") == 0) {
        multiplier = NANOSECONDS_PER_MICROSECOND;
    } else if (strcmp(end, "ms") == 0) {
        multiplier = NANOSECONDS_PER_MILLISECOND;
    } else {
        fprintf(stderr, "invalid duration suffix: %s\n", text);
        exit(EXIT_FAILURE);
    }

    long long result = (long long)(value * (double)multiplier);
    if (result <= 0) {
        fprintf(stderr, "invalid duration: %s\n", text);
        exit(EXIT_FAILURE);
    }
    return result;
}

// parse_command_line reads the options and positional argument into the
// demonstration context. It returns zero on success and a negative value when
// the arguments are invalid.
static int parse_command_line(int argc, char *argv[], struct DemonstrationContext *context)
{
    static const struct option long_options[] = {
        { "cpu", required_argument, NULL, 'c' },
        { "period", required_argument, NULL, 'p' },
        { "sleep", required_argument, NULL, 'l' },
        { "settle", required_argument, NULL, 's' },
        { "report", required_argument, NULL, 'r' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };

    int processor = DEFAULT_PROCESSOR;
    long long period_nanoseconds = DEFAULT_TIMERFD_PERIOD_NANOSECONDS;
    long long sleep_duration_nanoseconds = DEFAULT_SLEEP_DURATION_NANOSECONDS;
    double settle_duration_seconds = DEFAULT_SETTLE_DURATION_SECONDS;
    double report_duration_seconds = DEFAULT_REPORT_DURATION_SECONDS;

    int option;
    while ((option = getopt_long(argc, argv, "c:p:l:s:r:h", long_options, NULL)) != -1) {
        switch (option) {
        case 'c':
            processor = parse_processor(optarg);
            break;
        case 'p':
            period_nanoseconds = parse_nanoseconds(optarg);
            break;
        case 'l':
            sleep_duration_nanoseconds = parse_nanoseconds(optarg);
            break;
        case 's':
            settle_duration_seconds = parse_duration_seconds(optarg);
            break;
        case 'r':
            report_duration_seconds = parse_duration_seconds(optarg);
            break;
        case 'h':
            print_usage(argv[0]);
            exit(EXIT_SUCCESS);
        default:
            print_usage(argv[0]);
            return -1;
        }
    }

    if (optind < argc) {
        processor = parse_processor(argv[optind]);
    }

    context->processor = processor;
    context->period_nanoseconds = period_nanoseconds;
    context->sleep_duration_nanoseconds = sleep_duration_nanoseconds;
    context->settle_duration_nanoseconds =
        (long long)(settle_duration_seconds * (double)NANOSECONDS_PER_SECOND);
    context->report_duration_nanoseconds =
        (long long)(report_duration_seconds * (double)NANOSECONDS_PER_SECOND);
    context->background_processor_usage_percent = 0.0;
    return 0;
}

// set_processor_affinity pins the calling thread to the selected processor.
static int set_processor_affinity(int processor)
{
    cpu_set_t processor_set;
    CPU_ZERO(&processor_set);
    CPU_SET(processor, &processor_set);

    if (sched_setaffinity(0, sizeof processor_set, &processor_set) != 0) {
        fprintf(stderr, "failed to set affinity to processor %d: %s\n",
                processor, strerror(errno));
        return -1;
    }
    return 0;
}

// read_processor_time_sample reads the accumulated time counters for the
// selected processor from /proc/stat. The guest fields are already included in
// the user and nice fields, so they are not added to the total.
static int read_processor_time_sample(int processor, struct ProcessorTimeSample *sample)
{
    FILE *file = fopen("/proc/stat", "r");
    if (file == NULL) {
        return -1;
    }

    char processor_prefix[32];
    snprintf(processor_prefix, sizeof processor_prefix, "cpu%d ", processor);

    char line[1024];
    bool found = false;
    while (fgets(line, sizeof line, file) != NULL) {
        if (strncmp(line, processor_prefix, strlen(processor_prefix)) != 0) {
            continue;
        }

        unsigned long long user = 0;
        unsigned long long nice = 0;
        unsigned long long system = 0;
        unsigned long long idle = 0;
        unsigned long long iowait = 0;
        unsigned long long irq = 0;
        unsigned long long softirq = 0;
        unsigned long long steal = 0;

        int matched = sscanf(line, "%*s %llu %llu %llu %llu %llu %llu %llu %llu",
                             &user, &nice, &system, &idle, &iowait, &irq, &softirq,
                             &steal);
        if (matched < 8) {
            fclose(file);
            return -1;
        }

        sample->idle_ticks = idle + iowait;
        sample->busy_ticks = user + nice + system + irq + softirq + steal;
        sample->total_ticks = sample->idle_ticks + sample->busy_ticks;
        found = true;
        break;
    }

    fclose(file);
    return found ? 0 : -1;
}

// skip_stat_token advances the cursor past the next whitespace separated field.
static char *skip_stat_token(char *cursor)
{
    while (*cursor == ' ') {
        cursor++;
    }
    while (*cursor != ' ' && *cursor != '\0' && *cursor != '\n') {
        cursor++;
    }
    return cursor;
}

// read_task_time_sample reads the accumulated user and system time for the
// calling process from /proc/self/stat. The command name can contain spaces and
// parentheses, so parsing starts after the final closing parenthesis.
static int read_task_time_sample(struct TaskTimeSample *sample)
{
    FILE *file = fopen("/proc/self/stat", "r");
    if (file == NULL) {
        return -1;
    }

    char buffer[4096];
    size_t length = fread(buffer, 1, sizeof buffer - 1, file);
    buffer[length] = '\0';
    fclose(file);

    char *closing = strrchr(buffer, ')');
    if (closing == NULL) {
        return -1;
    }

    char *cursor = closing + 2;
    sample->user_ticks = 0;
    sample->system_ticks = 0;

    // Field 3 is the state and field 14 and 15 are the user and system time.
    for (int field = 3; field <= 15; field++) {
        if (field == 14) {
            sample->user_ticks = strtoull(cursor, NULL, 10);
        } else if (field == 15) {
            sample->system_ticks = strtoull(cursor, NULL, 10);
        }
        cursor = skip_stat_token(cursor);
    }

    return 0;
}

// measure_background_processor_usage measures the processor usage of the
// selected processor while the calling thread sleeps. It estimates the usage
// caused by other activity on the processor.
static double measure_background_processor_usage(int processor)
{
    struct ProcessorTimeSample before;
    struct ProcessorTimeSample after;
    if (read_processor_time_sample(processor, &before) != 0) {
        return 0.0;
    }

    sleep_nanoseconds(200LL * NANOSECONDS_PER_MILLISECOND);

    if (read_processor_time_sample(processor, &after) != 0) {
        return 0.0;
    }

    unsigned long long busy_ticks = after.busy_ticks - before.busy_ticks;
    unsigned long long total_ticks = after.total_ticks - before.total_ticks;
    if (total_ticks == 0) {
        return 0.0;
    }
    return 100.0 * (double)busy_ticks / (double)total_ticks;
}

// compute_trial_score expresses the goal as the real processor time consumed in
// excess of the reported per-processor usage. A larger score means more real
// work for less reported usage.
static double compute_trial_score(double reported_processor_usage_percent,
                                  double thread_usage_percent)
{
    return thread_usage_percent - reported_processor_usage_percent;
}

// run_evaluation spins and sleeps for the requested duration at the given phase
// and sleep duration. The sleep window is anchored to the absolute period grid
// so that it stays at a fixed phase. It records the per-processor usage reported
// by /proc/stat and the per-task usage reported by /proc/self/stat.
static int run_evaluation(const struct DemonstrationContext *context,
                          long long phase_nanoseconds,
                          long long sleep_duration_nanoseconds,
                          long long duration_nanoseconds,
                          struct TrialMeasurement *measurement)
{
    struct ProcessorTimeSample processor_before;
    struct ProcessorTimeSample processor_after;
    struct TaskTimeSample task_before;
    struct TaskTimeSample task_after;

    if (read_processor_time_sample(context->processor, &processor_before) != 0) {
        return -1;
    }
    if (read_task_time_sample(&task_before) != 0) {
        return -1;
    }

    long long start_wall = monotonic_nanoseconds();
    long long start_thread = thread_processor_nanoseconds();

    int timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (timer_fd < 0) {
        return -1;
    }

    long long next_sleep_start =
        (start_wall / context->period_nanoseconds) * context->period_nanoseconds +
        phase_nanoseconds;
    while (next_sleep_start <= start_wall) {
        next_sleep_start += context->period_nanoseconds;
    }

    unsigned long sleep_count = 0;
    while (!g_stop_requested) {
        spin_until(next_sleep_start);

        if (sleep_with_timerfd(timer_fd, sleep_duration_nanoseconds) != 0) {
            break;
        }
        sleep_count++;

        next_sleep_start += context->period_nanoseconds;
        if (next_sleep_start <= monotonic_nanoseconds()) {
            long long behind = monotonic_nanoseconds() - next_sleep_start;
            next_sleep_start += ((behind / context->period_nanoseconds) + 1) *
                                context->period_nanoseconds;
        }

        if (monotonic_nanoseconds() - start_wall >= duration_nanoseconds) {
            break;
        }
    }

    long long end_wall = monotonic_nanoseconds();
    long long end_thread = thread_processor_nanoseconds();
    close(timer_fd);

    if (read_processor_time_sample(context->processor, &processor_after) != 0) {
        return -1;
    }
    if (read_task_time_sample(&task_after) != 0) {
        return -1;
    }

    unsigned long long processor_busy_ticks =
        processor_after.busy_ticks - processor_before.busy_ticks;
    unsigned long long processor_total_ticks =
        processor_after.total_ticks - processor_before.total_ticks;
    unsigned long long task_ticks =
        (task_after.user_ticks + task_after.system_ticks) -
        (task_before.user_ticks + task_before.system_ticks);

    long long wall_nanoseconds = end_wall - start_wall;
    if (wall_nanoseconds <= 0) {
        wall_nanoseconds = 1;
    }

    double reported_processor_usage_percent =
        processor_total_ticks > 0
            ? 100.0 * (double)processor_busy_ticks / (double)processor_total_ticks
            : 0.0;

    double induced_processor_usage_percent =
        reported_processor_usage_percent - context->background_processor_usage_percent;
    if (induced_processor_usage_percent < 0.0) {
        induced_processor_usage_percent = 0.0;
    }

    double wall_seconds = (double)wall_nanoseconds / (double)NANOSECONDS_PER_SECOND;
    double clock_ticks_per_second = (double)sysconf(_SC_CLK_TCK);
    double task_usage_percent =
        100.0 * (double)task_ticks / (clock_ticks_per_second * wall_seconds);
    double thread_usage_percent =
        100.0 * (double)(end_thread - start_thread) / (double)wall_nanoseconds;

    measurement->phase_nanoseconds = phase_nanoseconds;
    measurement->sleep_duration_nanoseconds = sleep_duration_nanoseconds;
    measurement->reported_processor_usage_percent = reported_processor_usage_percent;
    measurement->induced_processor_usage_percent = induced_processor_usage_percent;
    measurement->task_usage_percent = task_usage_percent;
    measurement->thread_usage_percent = thread_usage_percent;
    measurement->sleep_count = sleep_count;
    measurement->score =
        compute_trial_score(induced_processor_usage_percent, thread_usage_percent);
    return 0;
}

// initialize_measurement prepares a measurement so that any real measurement
// replaces it.
static void initialize_measurement(struct TrialMeasurement *measurement)
{
    memset(measurement, 0, sizeof *measurement);
    measurement->score = -HUGE_VAL;
    measurement->thread_usage_percent = 0.0;
}

// is_better_measurement reports whether the candidate improves on the current
// best. A clearly larger score wins. When the scores are equivalent the
// candidate that consumed more processor time wins.
static bool is_better_measurement(const struct TrialMeasurement *candidate,
                                  const struct TrialMeasurement *current)
{
    if (candidate->score > current->score + MEASUREMENT_COMPARISON_EPSILON) {
        return true;
    }
    if (candidate->score < current->score - MEASUREMENT_COMPARISON_EPSILON) {
        return false;
    }
    return candidate->thread_usage_percent > current->thread_usage_percent;
}

// consider_phase runs one evaluation at the given phase and sleep duration and
// keeps it when it improves the best measurement.
static void consider_phase(const struct DemonstrationContext *context,
                           long long phase_nanoseconds,
                           long long sleep_duration_nanoseconds,
                           struct TrialMeasurement *best)
{
    struct TrialMeasurement candidate;
    if (run_evaluation(context, phase_nanoseconds, sleep_duration_nanoseconds,
                       context->settle_duration_nanoseconds, &candidate) != 0) {
        return;
    }
    printf("  sleep %6lld ns phase %9lld ns: per-CPU %6.2f%%, per-task %6.2f%%, "
           "real %6.2f%%, sleeps %lu\n",
           sleep_duration_nanoseconds, phase_nanoseconds,
           candidate.reported_processor_usage_percent, candidate.task_usage_percent,
           candidate.thread_usage_percent, candidate.sleep_count);
    fflush(stdout);
    if (is_better_measurement(&candidate, best)) {
        *best = candidate;
    }
}

// coarse_phase_step_count chooses how many phases to sample in the first search
// round. The step is kept below the sleep duration so that a narrow plateau is
// always sampled.
static int coarse_phase_step_count(const struct DemonstrationContext *context,
                                   long long sleep_duration_nanoseconds)
{
    long long count = context->period_nanoseconds / sleep_duration_nanoseconds;
    count += 1;
    if (count < MINIMUM_COARSE_PHASE_STEP_COUNT) {
        count = MINIMUM_COARSE_PHASE_STEP_COUNT;
    }
    if (count > MAXIMUM_COARSE_PHASE_STEP_COUNT) {
        count = MAXIMUM_COARSE_PHASE_STEP_COUNT;
    }
    return (int)count;
}

// narrow_phase_window samples phases across a window centered on the current
// best phase and then narrows the window.
static void narrow_phase_window(const struct DemonstrationContext *context,
                                long long sleep_duration_nanoseconds,
                                long long initial_window_nanoseconds,
                                struct TrialMeasurement *best)
{
    long long window = initial_window_nanoseconds;
    for (int round = 0; round < PHASE_REFINEMENT_ROUND_COUNT; round++) {
        long long center = best->phase_nanoseconds;
        for (int step = 0; step < PHASE_REFINEMENT_STEP_COUNT; step++) {
            long long offset =
                -window + (2 * window) * step / (PHASE_REFINEMENT_STEP_COUNT - 1);
            consider_phase(context, center + offset, sleep_duration_nanoseconds, best);
        }
        window = window / PHASE_REFINEMENT_WINDOW_DIVISOR;
        if (window < 1) {
            window = 1;
        }
    }
}

// search_full_period finds the phase with the given sleep duration. It samples
// the whole timerfd period so that the plateau is not missed, then narrows the
// window around the best phase.
static struct TrialMeasurement search_full_period(const struct DemonstrationContext *context,
                                                  long long sleep_duration_nanoseconds)
{
    struct TrialMeasurement best;
    initialize_measurement(&best);

    long long period = context->period_nanoseconds;
    int step_count = coarse_phase_step_count(context, sleep_duration_nanoseconds);

    printf("Searching the full period with sleep %lld ns:\n", sleep_duration_nanoseconds);
    for (int step = 0; step < step_count; step++) {
        long long phase = period * step / step_count;
        consider_phase(context, phase, sleep_duration_nanoseconds, &best);
    }

    narrow_phase_window(context, sleep_duration_nanoseconds, period / step_count, &best);
    return best;
}

// refine_phase recalibrates the phase for a shorter sleep. The search window is
// the previous sleep duration, which is wide enough to contain the sampling
// instant, and the step is half the new sleep duration.
static struct TrialMeasurement refine_phase(const struct DemonstrationContext *context,
                                            long long sleep_duration_nanoseconds,
                                            long long center_phase_nanoseconds,
                                            long long window_nanoseconds)
{
    struct TrialMeasurement best;
    initialize_measurement(&best);

    for (int step = 0; step < PHASE_REFINEMENT_STEP_COUNT; step++) {
        long long offset =
            -window_nanoseconds + (2 * window_nanoseconds) * step /
                                      (PHASE_REFINEMENT_STEP_COUNT - 1);
        consider_phase(context, center_phase_nanoseconds + offset,
                       sleep_duration_nanoseconds, &best);
    }

    return best;
}

// reduce_sleep halves the sleep duration repeatedly, recalibrating the phase
// each time, until the report rises above the acceptable percentage or the
// minimum sleep duration is reached. It returns the best measurement that kept
// the effect.
static struct TrialMeasurement reduce_sleep(const struct DemonstrationContext *context,
                                            struct TrialMeasurement best)
{
    while (best.sleep_duration_nanoseconds > MINIMUM_SLEEP_DURATION_NANOSECONDS) {
        long long candidate_sleep =
            best.sleep_duration_nanoseconds / SLEEP_REDUCTION_FACTOR;
        if (candidate_sleep < MINIMUM_SLEEP_DURATION_NANOSECONDS) {
            candidate_sleep = MINIMUM_SLEEP_DURATION_NANOSECONDS;
        }

        struct TrialMeasurement candidate =
            refine_phase(context, candidate_sleep, best.phase_nanoseconds,
                         best.sleep_duration_nanoseconds);

        printf("Reduced sleep: sleep %lld ns, phase %lld ns, per-CPU %.2f%%, "
               "real %.2f%%.\n",
               candidate.sleep_duration_nanoseconds, candidate.phase_nanoseconds,
               candidate.reported_processor_usage_percent,
               candidate.thread_usage_percent);

        if (candidate.induced_processor_usage_percent <=
            MAXIMUM_ACCEPTABLE_REPORTED_PERCENT) {
            best = candidate;
        } else {
            printf("Effect lost at sleep %lld ns; keeping sleep %lld ns.\n",
                   candidate.sleep_duration_nanoseconds, best.sleep_duration_nanoseconds);
            break;
        }
    }

    return best;
}

// optimize_phase_and_sleep finds the phase with the configured wide sleep and
// then reduces the sleep to the shortest duration that keeps the effect.
static struct TrialMeasurement optimize_phase_and_sleep(
    const struct DemonstrationContext *context)
{
    struct TrialMeasurement best =
        search_full_period(context, context->sleep_duration_nanoseconds);
    if (best.score == -HUGE_VAL) {
        return best;
    }
    printf("Wide search: sleep %lld ns, phase %lld ns, per-CPU %.2f%%, real %.2f%%.\n",
           best.sleep_duration_nanoseconds, best.phase_nanoseconds,
           best.reported_processor_usage_percent, best.thread_usage_percent);

    return reduce_sleep(context, best);
}

// recover_phase_and_sleep responds to a degraded effect by widening the sleep by
// one step so that it again spans the sampling instant jitter, recalibrating the
// phase, and then reducing the sleep again. It avoids a full search when only
// the phase has drifted and falls back to one when the widening does not help.
static struct TrialMeasurement recover_phase_and_sleep(
    const struct DemonstrationContext *context,
    const struct TrialMeasurement *current)
{
    long long widened_sleep =
        current->sleep_duration_nanoseconds * SLEEP_REDUCTION_FACTOR;
    if (widened_sleep > context->sleep_duration_nanoseconds) {
        widened_sleep = context->sleep_duration_nanoseconds;
    }

    printf("Widening sleep from %lld ns to %lld ns and recalibrating.\n",
           current->sleep_duration_nanoseconds, widened_sleep);
    struct TrialMeasurement recovered =
        refine_phase(context, widened_sleep, current->phase_nanoseconds, widened_sleep);

    if (recovered.induced_processor_usage_percent <=
        MAXIMUM_ACCEPTABLE_REPORTED_PERCENT) {
        return reduce_sleep(context, recovered);
    }

    printf("Widening did not recover the effect; searching the full period again.\n");
    return optimize_phase_and_sleep(context);
}

// print_measurement writes one measurement to standard output, contrasting the
// per-processor usage with the real thread usage.
static void print_measurement(const struct TrialMeasurement *measurement)
{
    double reported_usage = measurement->reported_processor_usage_percent;
    if (reported_usage < MINIMUM_REPORTED_USAGE_PERCENT) {
        reported_usage = MINIMUM_REPORTED_USAGE_PERCENT;
    }
    double suppression = measurement->thread_usage_percent / reported_usage;
    printf("sleep %lld ns: per-CPU %.2f%%, per-task %.2f%%, real %.2f%%, "
           "suppression %.0fx, sleeps %lu\n",
           measurement->sleep_duration_nanoseconds,
           measurement->reported_processor_usage_percent,
           measurement->task_usage_percent,
           measurement->thread_usage_percent,
           suppression,
           measurement->sleep_count);
    fflush(stdout);
}

// run_forever holds the optimal phase and sleep and reports the effect at the
// requested interval. It re-searches when the report rises above the best seen.
static void run_forever(const struct DemonstrationContext *context,
                        struct TrialMeasurement *best)
{
    printf("\nHolding sleep %lld ns at phase %lld ns. The effect stays active until "
           "interrupted.\n\n",
           best->sleep_duration_nanoseconds, best->phase_nanoseconds);

    while (!g_stop_requested) {
        struct TrialMeasurement measurement;
        if (run_evaluation(context, best->phase_nanoseconds,
                           best->sleep_duration_nanoseconds,
                           context->report_duration_nanoseconds, &measurement) != 0) {
            continue;
        }
        print_measurement(&measurement);

        if (measurement.reported_processor_usage_percent >
            best->reported_processor_usage_percent * RETRIGGER_USAGE_MULTIPLE +
                MAXIMUM_ACCEPTABLE_REPORTED_PERCENT) {
            printf("Effect degraded, widening the sleep and recalibrating...\n");
            struct TrialMeasurement search_result =
                recover_phase_and_sleep(context, best);
            if (search_result.score > -HUGE_VAL) {
                *best = search_result;
                printf("Locked on sleep %lld ns at phase %lld ns.\n",
                       best->sleep_duration_nanoseconds, best->phase_nanoseconds);
            }
        }
    }
}

// main parses the arguments, pins the thread to the requested processor, runs
// the feedback loop and then holds the optimal phase forever.
int main(int argc, char *argv[])
{
    struct DemonstrationContext context;
    if (parse_command_line(argc, argv, &context) != 0) {
        return EXIT_FAILURE;
    }

    long online_processor_count = sysconf(_SC_NPROCESSORS_ONLN);
    if (context.processor < 0 || context.processor >= online_processor_count) {
        fprintf(stderr, "processor %d is not online (%ld processors available)\n",
                context.processor, online_processor_count);
        return EXIT_FAILURE;
    }

    if (set_processor_affinity(context.processor) != 0) {
        return EXIT_FAILURE;
    }

    signal(SIGINT, handle_stop_signal);
    signal(SIGTERM, handle_stop_signal);

    context.background_processor_usage_percent =
        measure_background_processor_usage(context.processor);

    struct TrialMeasurement best = optimize_phase_and_sleep(&context);
    if (best.score == -HUGE_VAL) {
        fprintf(stderr, "feedback loop did not produce a usable phase\n");
        return EXIT_FAILURE;
    }

    printf("\nEquilibrium at sleep %lld ns and phase %lld ns: per-CPU %.2f%%, "
           "per-task %.2f%%, real %.2f%%. That is %.1f%% of a processor consumed "
           "while %.2f%% is reported.\n",
           best.sleep_duration_nanoseconds, best.phase_nanoseconds,
           best.reported_processor_usage_percent, best.task_usage_percent,
           best.thread_usage_percent, best.thread_usage_percent,
           best.reported_processor_usage_percent);

    run_forever(&context, &best);
    return EXIT_SUCCESS;
}
