#include <events/dispatcher/event_dispatcher.hpp>
#include <events/dispatcher/synchronized_event_dispatcher.hpp>
#include <events/signal_handler/signal_handler.hpp>
#include <events/signal_handler/synchronized_signal_handler.hpp>

#include <tabulate/table.hpp>
#include <fmt/color.h>
#include <fmt/core.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using clock_type = std::chrono::steady_clock;
using nanoseconds_f64 = std::chrono::duration<double, std::nano>;

// ============================================================================
// Benchmark configuration
// ============================================================================

static constexpr std::array callback_counts   = {1, 10, 100};
static constexpr std::array thread_counts     = {1, 2, 4, 8};
static constexpr std::array event_type_counts = {1, 5, 10};

// event_dispatcher: events enqueued between calls to dispatch().
// synchronized_event_dispatcher: events enqueued (by all producers together) in one round.
static constexpr std::array event_counts = {100, 1'000, 10'000, 100'000};

// Each result is the median of this many samples, taken after a warm-up
static constexpr int sample_count = 9;

// Short workloads are repeated until one sample takes at least this long, so that the timer's resolution and overhead
// (and thread wake-up latency) don't dominate the results
static constexpr auto min_sample_time = std::chrono::milliseconds{5};

// The synchronized_event_dispatcher benchmark gives up after this long (which would mean that events were lost)
static constexpr auto pipeline_timeout = std::chrono::seconds{30};

static constexpr int max_event_types = 10;
static_assert(std::ranges::max(event_type_counts) <= max_event_types);


// ============================================================================
// Measurement
// ============================================================================

// Every callback increments this counter. It's a side effect that can't be optimized away, threads don't contend for it,
// and it lets each benchmark check that every callback was invoked.
thread_local std::uint64_t invocations = 0;

struct stats {
	double median_ns = 0.0;  ///< Median of the samples, per operation
	double spread = 0.0;     ///< Interquartile range / median. Unlike the full range, one disturbed sample barely affects it.
};

auto summarize(std::vector<double> per_op_ns) -> stats {
	std::ranges::sort(per_op_ns);
	auto const median = per_op_ns[per_op_ns.size() / 2];
	auto const lower_quartile = per_op_ns[per_op_ns.size() / 4];
	auto const upper_quartile = per_op_ns[(per_op_ns.size() * 3) / 4];
	auto const spread = (median > 0.0) ? ((upper_quartile - lower_quartile) / median) : 0.0;
	return {median, spread};
}

auto verify_invocations(std::string_view benchmark, std::uint64_t actual, std::uint64_t expected) -> void {
	if (actual != expected) {
		throw std::runtime_error{fmt::format("{}: {} callback invocations, expected {}", benchmark, actual, expected)};
	}
}

// Finds how many repetitions of `run` fill a sample of at least min_sample_time. `run(repetitions)` returns the time it
// took. The calibration runs double as the warm-up.
template<typename RunT>
auto calibrate(RunT&& run) -> std::int64_t {
	auto repetitions = std::int64_t{1};

	while (true) {
		auto const elapsed = nanoseconds_f64{run(repetitions)};
		if (elapsed >= min_sample_time) {
			run(repetitions);  // warm up once more at the final size
			return repetitions;
		}

		// Aim slightly past the target, growing by 2x to 100x per step
		auto const factor = (elapsed.count() > 0.0) ? std::clamp(1.2 * (nanoseconds_f64{min_sample_time} / elapsed), 2.0, 100.0)
		                                            : 100.0;
		repetitions = static_cast<std::int64_t>(static_cast<double>(repetitions) * factor);
	}
}

// Measures the time per operation of `run(repetitions)`, which performs repetitions * ops_per_repetition operations and
// returns the time it took
template<typename RunT>
auto measure(RunT&& run, std::int64_t ops_per_repetition) -> stats {
	auto const repetitions = calibrate(run);
	auto const ops = static_cast<double>(repetitions * ops_per_repetition);

	auto per_op_ns = std::vector<double>{};
	per_op_ns.reserve(sample_count);
	for (auto s = 0; s < sample_count; ++s) {
		per_op_ns.push_back(nanoseconds_f64{run(repetitions)}.count() / ops);
	}

	return summarize(std::move(per_op_ns));
}


// ============================================================================
// Signal handler benchmarks
// ============================================================================

template<typename SignalHandlerT>
auto connect_callbacks(SignalHandlerT& sigh, int num_callbacks) -> std::vector<events::connection> {
	auto conns = std::vector<events::connection>{};
	for (auto c = 0; c < num_callbacks; ++c) {
		conns.push_back(sigh.connect([](int) { ++invocations; }));
	}
	return conns;
}

// One thread publishes repeatedly. Result: time per publish.
auto bench_signal_handler(int num_callbacks) -> stats {
	auto sigh = events::signal_handler<void(int)>{};
	[[maybe_unused]] auto const conns = connect_callbacks(sigh, num_callbacks);

	return measure([&](std::int64_t publishes) {
		auto const before = invocations;
		auto const start = clock_type::now();

		for (auto i = std::int64_t{0}; i < publishes; ++i) {
			sigh.publish(static_cast<int>(i));
		}

		auto const elapsed = nanoseconds_f64{clock_type::now() - start};
		verify_invocations("signal_handler", invocations - before, static_cast<std::uint64_t>(publishes * num_callbacks));
		return elapsed;
	}, 1);
}

// All threads publish at the same time. Result: wall time (until the last thread is done) per publish, counting the
// publishes of all threads.
auto bench_synchronized_signal_handler(int num_callbacks, int num_threads) -> stats {
	auto sigh = events::synchronized_signal_handler<void(int)>{};
	[[maybe_unused]] auto const conns = connect_callbacks(sigh, num_callbacks);

	auto const thread_count = static_cast<std::size_t>(num_threads);

	return measure([&](std::int64_t publishes_per_thread) {
		auto start = clock_type::time_point{};
		auto sync_point = std::barrier{num_threads, [&start]() noexcept { start = clock_type::now(); }};
		auto end_times = std::vector<clock_type::time_point>(thread_count);
		auto counts = std::vector<std::uint64_t>(thread_count);

		auto threads = std::vector<std::thread>{};
		threads.reserve(thread_count);
		for (auto t = std::size_t{0}; t < thread_count; ++t) {
			threads.emplace_back([&, t] {
				sync_point.arrive_and_wait();
				auto const before = invocations;

				for (auto i = std::int64_t{0}; i < publishes_per_thread; ++i) {
					sigh.publish(static_cast<int>(i));
				}

				end_times[t] = clock_type::now();
				counts[t] = invocations - before;
			});
		}
		for (auto& thread : threads) {
			thread.join();
		}

		verify_invocations(
			"synchronized_signal_handler",
			std::accumulate(counts.begin(), counts.end(), std::uint64_t{0}),
			static_cast<std::uint64_t>(publishes_per_thread * num_threads * num_callbacks)
		);
		return nanoseconds_f64{*std::ranges::max_element(end_times) - start};
	}, num_threads);
}


// ============================================================================
// Event dispatcher benchmarks
// ============================================================================

template<int N>
struct bench_event {
	int v;
};

// Connects num_callbacks callbacks to each of the first num_types event types
template<typename DispatcherT>
auto connect_event_types(DispatcherT& dispatcher, int num_types, int num_callbacks) -> std::vector<events::connection> {
	auto conns = std::vector<events::connection>{};

	auto const connect_n = [&]<int N>(bench_event<N> /*tag*/) {
		if (N < num_types) {
			for (auto c = 0; c < num_callbacks; ++c) {
				conns.push_back(dispatcher.template connect<bench_event<N>>([](bench_event<N> const&) { ++invocations; }));
			}
		}
	};

	[&]<int... N>(std::integer_sequence<int, N...>) {
		(connect_n(bench_event<N>{}), ...);
	}(std::make_integer_sequence<int, max_event_types>{});

	return conns;
}

// Enqueues num_events events, spread evenly across the first num_types event types
template<typename DispatcherT>
auto enqueue_events(DispatcherT& dispatcher, int num_events, int num_types) -> void {
	for (auto i = 0; i < num_events; ++i) {
		switch (i % num_types) {
			case 0: dispatcher.enqueue(bench_event<0>{i}); break;
			case 1: dispatcher.enqueue(bench_event<1>{i}); break;
			case 2: dispatcher.enqueue(bench_event<2>{i}); break;
			case 3: dispatcher.enqueue(bench_event<3>{i}); break;
			case 4: dispatcher.enqueue(bench_event<4>{i}); break;
			case 5: dispatcher.enqueue(bench_event<5>{i}); break;
			case 6: dispatcher.enqueue(bench_event<6>{i}); break;
			case 7: dispatcher.enqueue(bench_event<7>{i}); break;
			case 8: dispatcher.enqueue(bench_event<8>{i}); break;
			case 9: dispatcher.enqueue(bench_event<9>{i}); break;
			default: break;
		}
	}
}

struct dispatcher_result {
	stats enqueue;   ///< Per event
	stats dispatch;  ///< Per event
	stats total;     ///< Per event
};

// Repeatedly enqueues a batch of events, then dispatches them
auto bench_event_dispatcher(int batch_size, int num_callbacks, int num_types) -> dispatcher_result {
	auto dispatcher = events::event_dispatcher{};
	[[maybe_unused]] auto const conns = connect_event_types(dispatcher, num_types, num_callbacks);

	struct phase_times {
		nanoseconds_f64 enqueue{};
		nanoseconds_f64 dispatch{};
	};

	// Timing each phase costs two clock reads per batch, which is small next to even the smallest batch
	auto const run = [&](std::int64_t cycles) {
		auto times = phase_times{};
		auto const before = invocations;
		auto previous = clock_type::now();

		for (auto c = std::int64_t{0}; c < cycles; ++c) {
			enqueue_events(dispatcher, batch_size, num_types);
			auto const enqueued = clock_type::now();
			dispatcher.dispatch();
			auto const dispatched = clock_type::now();

			times.enqueue += enqueued - previous;
			times.dispatch += dispatched - enqueued;
			previous = dispatched;
		}

		verify_invocations("event_dispatcher", invocations - before, static_cast<std::uint64_t>(cycles * batch_size * num_callbacks));
		return times;
	};

	auto const cycles = calibrate([&](std::int64_t count) {
		auto const times = run(count);
		return times.enqueue + times.dispatch;
	});
	auto const num_events = static_cast<double>(cycles * batch_size);

	auto enqueue = std::vector<double>{};
	auto dispatch = std::vector<double>{};
	auto total = std::vector<double>{};
	for (auto s = 0; s < sample_count; ++s) {
		auto const times = run(cycles);
		enqueue.push_back(times.enqueue.count() / num_events);
		dispatch.push_back(times.dispatch.count() / num_events);
		total.push_back((times.enqueue + times.dispatch).count() / num_events);
	}

	return {summarize(std::move(enqueue)), summarize(std::move(dispatch)), summarize(std::move(total))};
}

struct pipeline_result {
	int events = 0;    ///< Number of events in one round
	stats enqueue;     ///< Wall time until every producer was done, per event
	stats end_to_end;  ///< Wall time until every callback was invoked, per event
};

// Producer threads enqueue events while one consumer thread calls dispatch() in a loop, until every callback was
// invoked. The threads repeat this in rounds, which start and end with every thread waiting at a barrier.
auto bench_synchronized_event_dispatcher(int num_events, int num_callbacks, int num_types, int num_producers) -> pipeline_result {
	auto const events_per_producer = num_events / num_producers;
	auto const total_events = events_per_producer * num_producers;
	auto const expected_per_round = static_cast<std::uint64_t>(total_events) * static_cast<std::uint64_t>(num_callbacks);
	auto const producer_count = static_cast<std::size_t>(num_producers);

	auto dispatcher = events::synchronized_event_dispatcher{};
	[[maybe_unused]] auto const conns = connect_event_types(dispatcher, num_types, num_callbacks);

	struct phase_times {
		nanoseconds_f64 enqueue{};
		nanoseconds_f64 end_to_end{};
	};

	auto const run = [&](std::int64_t rounds) {
		auto totals = phase_times{};
		auto start = clock_type::time_point{};
		auto producer_end = std::vector<clock_type::time_point>(producer_count);
		auto consumer_end = clock_type::time_point{};
		auto delivered = std::uint64_t{0};

		// The completion functions run while every thread is waiting, so they can read the other threads' results
		auto round_start = std::barrier{num_producers + 1, [&start]() noexcept { start = clock_type::now(); }};
		auto round_end = std::barrier{num_producers + 1, [&]() noexcept {
			totals.enqueue += *std::ranges::max_element(producer_end) - start;
			totals.end_to_end += consumer_end - start;
		}};

		auto threads = std::vector<std::thread>{};
		threads.reserve(producer_count + 1);

		for (auto p = std::size_t{0}; p < producer_count; ++p) {
			threads.emplace_back([&, p] {
				for (auto r = std::int64_t{0}; r < rounds; ++r) {
					round_start.arrive_and_wait();
					enqueue_events(dispatcher, events_per_producer, num_types);
					producer_end[p] = clock_type::now();
					round_end.arrive_and_wait();
				}
			});
		}

		threads.emplace_back([&] {
			// If events are lost, the remaining rounds end immediately and the check below fails
			auto const deadline = clock_type::now() + pipeline_timeout;

			for (auto r = std::int64_t{0}; r < rounds; ++r) {
				round_start.arrive_and_wait();
				auto const before = invocations;  // Callbacks only run on this thread

				while (((invocations - before) < expected_per_round) && (clock_type::now() < deadline)) {
					dispatcher.dispatch();
				}

				consumer_end = clock_type::now();
				delivered += invocations - before;
				round_end.arrive_and_wait();
			}
		});

		for (auto& thread : threads) {
			thread.join();
		}

		verify_invocations("synchronized_event_dispatcher", delivered, expected_per_round * static_cast<std::uint64_t>(rounds));
		return totals;
	};

	auto const rounds = calibrate([&](std::int64_t count) { return run(count).end_to_end; });
	auto const num_events_measured = static_cast<double>(rounds * total_events);

	auto enqueue = std::vector<double>{};
	auto end_to_end = std::vector<double>{};
	for (auto s = 0; s < sample_count; ++s) {
		auto const times = run(rounds);
		enqueue.push_back(times.enqueue.count() / num_events_measured);
		end_to_end.push_back(times.end_to_end.count() / num_events_measured);
	}

	return {total_events, summarize(std::move(enqueue)), summarize(std::move(end_to_end))};
}


// ============================================================================
// Formatting and tables
// ============================================================================

auto format_time(double ns) -> std::string {
	if (ns >= 1e9) {
		return fmt::format("{:.2f} s", ns / 1e9);
	}
	if (ns >= 1e6) {
		return fmt::format("{:.2f} ms", ns / 1e6);
	}
	if (ns >= 1e3) {
		return fmt::format("{:.2f} us", ns / 1e3);
	}
	return fmt::format("{:.2f} ns", ns);
}

auto events_per_second(double ns_per_event) -> double {
	return (ns_per_event > 0.0) ? (1e9 / ns_per_event) : 0.0;
}

auto format_throughput(double ns_per_event) -> std::string {
	auto const eps = events_per_second(ns_per_event);
	if (eps >= 1'000'000.0) {
		return fmt::format("{:.2f}M/s", eps / 1'000'000.0);
	}
	if (eps >= 1'000.0) {
		return fmt::format("{:.2f}K/s", eps / 1'000.0);
	}
	return fmt::format("{:.0f}/s", eps);
}

auto format_spread(double spread) -> std::string {
	return fmt::format("{:.0f}%", spread * 100.0);
}

auto throughput_color(double ns_per_event) -> tabulate::Color {
	auto const eps = events_per_second(ns_per_event);
	if (eps >= 10'000'000.0) return tabulate::Color::green;
	if (eps >= 1'000'000.0)  return tabulate::Color::yellow;
	return tabulate::Color::red;
}

// Noisy results (e.g. from other programs using the CPU) are highlighted
auto spread_color(double spread) -> tabulate::Color {
	if (spread > 0.15) return tabulate::Color::red;
	if (spread > 0.05) return tabulate::Color::yellow;
	return tabulate::Color::none;
}

auto print_section_header(std::string_view title, std::string_view description) -> void {
	fmt::print(fmt::emphasis::bold | fg(fmt::color::cornflower_blue), "\n{}\n{}\n", title, std::string(title.size(), '='));
	fmt::print(fg(fmt::color::light_gray), "{}\n\n", description);
}

auto make_table(tabulate::Table::Row_t const& headers) -> tabulate::Table {
	auto table = tabulate::Table{};
	table.add_row(headers);
	for (auto i = std::size_t{0}; i < headers.size(); ++i) {
		table[0][i].format().font_color(tabulate::Color::cyan).font_style({tabulate::FontStyle::bold});
	}
	return table;
}

// Adds a row whose last two cells are the throughput and the spread
auto add_result_row(tabulate::Table& table, tabulate::Table::Row_t const& cells, stats const& result) -> void {
	table.add_row(cells);
	auto& row = table[table.size() - 1];
	row[cells.size() - 2].format().font_color(throughput_color(result.median_ns));
	row[cells.size() - 1].format().font_color(spread_color(result.spread));
}

auto print_table(tabulate::Table& table) -> void {
	table.format().border_top(" ").border_bottom(" ").border_left(" ").border_right(" ").corner(" ");
	std::cout << table << "\n";
}


// ============================================================================
// Sections
// ============================================================================

auto run_signal_handler_benchmarks() -> void {
	print_section_header(
		"Signal Handler (single-threaded)",
		"One thread publishes repeatedly. Each publish invokes every callback."
	);

	auto table = make_table({"Callbacks", "Per Publish", "Throughput", "Spread"});
	for (auto const callbacks : callback_counts) {
		auto const r = bench_signal_handler(callbacks);
		add_result_row(table, {fmt::format("{}", callbacks), format_time(r.median_ns), format_throughput(r.median_ns), format_spread(r.spread)}, r);
	}
	print_table(table);
}

auto run_synchronized_signal_handler_benchmarks() -> void {
	print_section_header(
		"Synchronized Signal Handler (concurrent publish)",
		"All threads publish at the same time. Per Publish is the wall time divided by the publishes of all threads, so it\n"
		"decreases with more threads if publishing scales."
	);

	auto table = make_table({"Callbacks", "Threads", "Per Publish", "Throughput", "Spread"});
	for (auto const callbacks : callback_counts) {
		for (auto const threads : thread_counts) {
			auto const r = bench_synchronized_signal_handler(callbacks, threads);
			add_result_row(
				table,
				{fmt::format("{}", callbacks), fmt::format("{}", threads), format_time(r.median_ns), format_throughput(r.median_ns), format_spread(r.spread)},
				r
			);
		}
	}
	print_table(table);
}

auto run_event_dispatcher_benchmarks() -> void {
	print_section_header(
		"Event Dispatcher (single-threaded enqueue + dispatch)",
		"Repeatedly enqueues a batch of events (spread across the event types), then dispatches them. Times are per event,\n"
		"and dispatching an event invokes every callback for its type."
	);

	auto table = make_table({"Batch", "Callbacks", "Event Types", "Enqueue", "Dispatch", "Per Event", "Throughput", "Spread"});
	for (auto const batch : event_counts) {
		for (auto const callbacks : callback_counts) {
			for (auto const types : event_type_counts) {
				auto const r = bench_event_dispatcher(batch, callbacks, types);
				add_result_row(
					table,
					{
						fmt::format("{}", batch),
						fmt::format("{}", callbacks),
						fmt::format("{}", types),
						format_time(r.enqueue.median_ns),
						format_time(r.dispatch.median_ns),
						format_time(r.total.median_ns),
						format_throughput(r.total.median_ns),
						format_spread(r.total.spread)
					},
					r.total
				);
			}
		}
	}
	print_table(table);
}

auto run_synchronized_event_dispatcher_benchmarks() -> void {
	print_section_header(
		"Synchronized Event Dispatcher (concurrent enqueue + dispatch)",
		"In each round, producer threads enqueue the events while one consumer thread calls dispatch() in a loop. Enqueue\n"
		"is the wall time until every producer was done, and End-to-End until every callback was invoked (both per round).\n"
		"Small rounds are dominated by the latency of waking up the threads."
	);

	auto table = make_table({"Events", "Callbacks", "Event Types", "Producers", "Enqueue", "End-to-End", "Per Event", "Throughput", "Spread"});
	for (auto const num_events : event_counts) {
		for (auto const callbacks : callback_counts) {
			for (auto const types : event_type_counts) {
				for (auto const producers : thread_counts) {
					auto const r = bench_synchronized_event_dispatcher(num_events, callbacks, types, producers);
					add_result_row(
						table,
						{
							fmt::format("{}", r.events),
							fmt::format("{}", callbacks),
							fmt::format("{}", types),
							fmt::format("{}", producers),
							format_time(r.enqueue.median_ns * r.events),
							format_time(r.end_to_end.median_ns * r.events),
							format_time(r.end_to_end.median_ns),
							format_throughput(r.end_to_end.median_ns),
							format_spread(r.end_to_end.spread)
						},
						r.end_to_end
					);
				}
			}
		}
	}
	print_table(table);
}

auto run_comparison_summary() -> void {
	constexpr auto callbacks = 10;
	constexpr auto threads = 2;
	constexpr auto types = 10;
	constexpr auto num_events = 10'000;

	print_section_header(
		"Comparison Summary",
		"Per Event is the time per publish for the signal handlers."
	);

	auto table = make_table({"Component", "Configuration", "Per Event", "Throughput", "Spread"});
	auto const add = [&](std::string_view name, std::string const& configuration, stats const& r) {
		add_result_row(table, {std::string{name}, configuration, format_time(r.median_ns), format_throughput(r.median_ns), format_spread(r.spread)}, r);
	};

	add("signal_handler", fmt::format("{} callbacks", callbacks), bench_signal_handler(callbacks));
	add("synchronized_signal_handler", fmt::format("{} callbacks, {} threads", callbacks, threads), bench_synchronized_signal_handler(callbacks, threads));
	add(
		"event_dispatcher",
		fmt::format("batches of {}, {} callbacks, {} types", num_events, callbacks, types),
		bench_event_dispatcher(num_events, callbacks, types).total
	);
	add(
		"synchronized_event_dispatcher",
		fmt::format("{} events, {} callbacks, {} types, {} producers", num_events, callbacks, types, threads),
		bench_synchronized_event_dispatcher(num_events, callbacks, types, threads).end_to_end
	);

	print_table(table);
}


// ============================================================================
// Main
// ============================================================================

struct section {
	std::string_view name;
	void (*run)();
};

static constexpr std::array sections = {
	section{"signal_handler", run_signal_handler_benchmarks},
	section{"synchronized_signal_handler", run_synchronized_signal_handler_benchmarks},
	section{"event_dispatcher", run_event_dispatcher_benchmarks},
	section{"synchronized_event_dispatcher", run_synchronized_event_dispatcher_benchmarks},
	section{"summary", run_comparison_summary},
};

auto print_usage() -> void {
	fmt::print("Usage: events_benchmark [section...]\n\nSections (all of them by default):\n");
	for (auto const& s : sections) {
		fmt::print("  {}\n", s.name);
	}
}

auto main(int argc, char** argv) -> int {
	auto selected = std::vector<section>{};
	for (auto i = 1; i < argc; ++i) {
		auto const arg = std::string_view{argv[i]};  //NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
		if ((arg == "-h") || (arg == "--help")) {
			print_usage();
			return 0;
		}

		auto const it = std::ranges::find(sections, arg, &section::name);
		if (it == sections.end()) {
			fmt::print(fg(fmt::color::red), "Unknown section: {}\n\n", arg);
			print_usage();
			return 1;
		}
		selected.push_back(*it);
	}
	if (selected.empty()) {
		selected.assign(sections.begin(), sections.end());
	}

	fmt::print(fmt::emphasis::bold | fg(fmt::color::gold),
	           "\n  Events Library Performance Metrics\n"
	           "  -----------------------------------\n\n");

#ifndef NDEBUG
	fmt::print(fmt::emphasis::bold | fg(fmt::color::red), "  Warning: this is a debug build. The results aren't representative.\n\n");
#endif

	fmt::print(fg(fmt::color::light_gray),
	           "  Configuration:\n"
	           "    Event counts:      {}\n"
	           "    Callback counts:   {}\n"
	           "    Thread counts:     {}\n"
	           "    Event type counts: {}\n"
	           "    Hardware threads:  {}\n"
	           "    Samples:           median of {}, each at least {} ms\n"
	           "    Spread:            interquartile range / median (high values mean noisy results)\n",
	           fmt::join(event_counts, ", "),
	           fmt::join(callback_counts, ", "),
	           fmt::join(thread_counts, ", "),
	           fmt::join(event_type_counts, ", "),
	           std::thread::hardware_concurrency(),
	           sample_count,
	           min_sample_time.count());

	try {
		for (auto const& s : selected) {
			s.run();
		}
	}
	catch (std::exception const& ex) {
		fmt::print(fg(fmt::color::red), "\nError: {}\n", ex.what());
		return 1;
	}

	return 0;
}
