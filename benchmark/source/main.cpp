#include <events/dispatcher/event_dispatcher.hpp>
#include <events/dispatcher/synchronized_event_dispatcher.hpp>
#include <events/signal_handler/signal_handler.hpp>
#include <events/signal_handler/synchronized_signal_handler.hpp>

#include <tabulate/table.hpp>
#include <fmt/core.h>
#include <fmt/chrono.h>
#include <fmt/color.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <numeric>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono;

using seconds_f64 = duration<double, std::ratio<1>>;
using milliseconds_f64 = duration<double, std::milli>;
using microseconds_f64 = duration<double, std::micro>;

// ============================================================================
// Benchmark configuration
// ============================================================================

static constexpr std::array event_counts     = {100, 1'000, 10'000, 100'000};
static constexpr std::array callback_counts  = {1, 10, 50};
static constexpr std::array thread_counts    = {1, 2, 4, 8};

// How many unique event types to test for dispatchers
static constexpr std::array event_type_counts = {1, 5, 10};


// ============================================================================
// Timing utilities
// ============================================================================

struct timing_result {
	microseconds_f64 total;
	microseconds_f64 per_event;
	double events_per_sec;
	microseconds_f64 enqueue_time{};
};

auto format_duration(microseconds_f64 d) -> std::string {
	if (d >= seconds{1}) {
		return fmt::format("{:.2}", duration_cast<seconds_f64>(d));
	}
	if (d >= milliseconds{1}) {
		return fmt::format("{:.2}", duration_cast<milliseconds_f64>(d));
	}
	return fmt::format("{:.2}", d);
}

auto format_throughput(double eps) -> std::string {
	if (eps >= 1'000'000.0) {
		return fmt::format("{:.2f}M/s", eps / 1'000'000.0);
	}
	if (eps >= 1'000.0) {
		return fmt::format("{:.2f}K/s", eps / 1'000.0);
	}
	return fmt::format("{:.0f}/s", eps);
}

auto format_enqueue_time(microseconds_f64 d) -> std::string {
	if (d.count() == 0.0) return "N/A";
	return format_duration(d);
}


// ============================================================================
// Common helpers
// ============================================================================

auto compute_result(
	microseconds_f64 const total_time,
	int const num_events,
    microseconds_f64 const enq_time = {}
) -> timing_result {
	auto const per = total_time / num_events;
	auto const eps = (total_time.count() > 0) ? (num_events / duration_cast<seconds_f64>(total_time).count()) : 0.0;
	return {total_time, per, eps, enq_time};
}


// ============================================================================
// Signal handler benchmarks
// ============================================================================

auto bench_signal_handler(int num_events, int num_callbacks) -> timing_result {
	auto sigh = events::signal_handler<void(int)>{};
	auto conns = std::vector<events::connection>{};
	conns.reserve(static_cast<size_t>(num_callbacks));

	volatile auto sink = int{0};
	for (auto c = int{0}; c < num_callbacks; ++c) {
		conns.push_back(sigh.connect([&sink](int n) { sink = n; }));
	}

	auto const start = steady_clock::now();
	for (auto i = int{0}; i < num_events; ++i) {
		sigh.publish(i);
	}
	auto const elapsed = steady_clock::now() - start;

	auto const total = duration_cast<microseconds_f64>(elapsed);
	auto const per_event = total / num_events;
	auto const eps = (total.count() > 0) ? (num_events / (total.count() / 1'000'000.0)) : 0.0;

	return {total, per_event, eps};
}

auto bench_synchronized_signal_handler(int num_events, int num_callbacks, int num_threads) -> timing_result {
	// Ensure number of events is divisible by number of threads
	num_events = (num_events / num_threads) * num_threads;

	auto sigh = events::synchronized_signal_handler<void(int)>{};
	auto conns = std::vector<events::connection>{};
	conns.reserve(static_cast<size_t>(num_callbacks));

	auto sink = std::atomic_int{0};
	for (auto c = int{0}; c < num_callbacks; ++c) {
		conns.push_back(sigh.connect([&sink](int n) { sink.store(n, std::memory_order_relaxed); }));
	}

	auto const events_per_thread = num_events / num_threads;

	// Barrier with completion function to record precise start time
	auto wall_start = steady_clock::time_point{};
	auto sync_point = std::barrier{num_threads, [&wall_start]() noexcept {
		wall_start = steady_clock::now();
	}};

	auto thread_end_times = std::vector<steady_clock::time_point>(static_cast<size_t>(num_threads));
	auto threads = std::vector<std::thread>{};
	threads.reserve(static_cast<size_t>(num_threads));
	for (auto t = int{0}; t < num_threads; ++t) {
		threads.emplace_back([&, t, events_per_thread] {
			sync_point.arrive_and_wait();
			for (auto i = int{0}; i < events_per_thread; ++i) {
				sigh.publish(i);
			}
			thread_end_times[static_cast<size_t>(t)] = steady_clock::now();
		});
	}

	for (auto& th : threads) {
		th.join();
	}

	auto const wall_end = *std::max_element(thread_end_times.begin(), thread_end_times.end());
	auto const total_events = events_per_thread * num_threads;
	auto const total = duration_cast<microseconds_f64>(wall_end - wall_start);

	return compute_result(total, total_events);
}


// ============================================================================
// Event dispatcher benchmarks
// ============================================================================

// Simple event types used by the benchmarks
template<int N>
struct bench_event { int v; };

// Helper to connect to N event types on a dispatcher, returns connections
template<typename Dispatcher>
auto connect_event_types(Dispatcher& d, int num_types, int num_callbacks, auto& sink) -> std::vector<events::connection> {
	auto conns = std::vector<events::connection>{};

	auto const connect_n = [&](auto const tag) {
		using E = decltype(tag);
		for (auto c = int{0}; c < num_callbacks; ++c) {
			conns.push_back(d.template connect<E>([&sink](E const& e) {
				sink.store(e.v, std::memory_order_relaxed);
			}));
		}
	};

	// Connect up to num_types distinct event types
	if (num_types > 0) connect_n(bench_event<0>{});
	if (num_types > 1) connect_n(bench_event<1>{});
	if (num_types > 2) connect_n(bench_event<2>{});
	if (num_types > 3) connect_n(bench_event<3>{});
	if (num_types > 4) connect_n(bench_event<4>{});
	if (num_types > 5) connect_n(bench_event<5>{});
	if (num_types > 6) connect_n(bench_event<6>{});
	if (num_types > 7) connect_n(bench_event<7>{});
	if (num_types > 8) connect_n(bench_event<8>{});
	if (num_types > 9) connect_n(bench_event<9>{});

	return conns;
}

// Helper to enqueue distributing events across N types
template<typename Dispatcher>
auto enqueue_events(Dispatcher& d, int const total_events, int const num_types) -> void {
	for (auto i = int{0}; i < total_events; ++i) {
		switch (i % num_types) {
			case 0: d.enqueue(bench_event<0>{i}); break;
			case 1: d.enqueue(bench_event<1>{i}); break;
			case 2: d.enqueue(bench_event<2>{i}); break;
			case 3: d.enqueue(bench_event<3>{i}); break;
			case 4: d.enqueue(bench_event<4>{i}); break;
			case 5: d.enqueue(bench_event<5>{i}); break;
			case 6: d.enqueue(bench_event<6>{i}); break;
			case 7: d.enqueue(bench_event<7>{i}); break;
			case 8: d.enqueue(bench_event<8>{i}); break;
			case 9: d.enqueue(bench_event<9>{i}); break;
			default: break;
		}
	}
}

auto bench_event_dispatcher(int num_events, int num_callbacks, int num_types) -> timing_result {
	auto dispatcher = events::event_dispatcher{};
	auto sink = std::atomic_int{0};
	auto const conns = connect_event_types(dispatcher, num_types, num_callbacks, sink);

	enqueue_events(dispatcher, num_events, num_types);

	auto const start = steady_clock::now();
	dispatcher.dispatch();
	auto const elapsed = steady_clock::now() - start;

	auto const total = duration_cast<microseconds_f64>(elapsed);
	auto const per_event = total / num_events;
	auto const eps = (total.count() > 0) ? (num_events / (total.count() / 1'000'000.0)) : 0.0;

	return {total, per_event, eps};
}

auto bench_synchronized_event_dispatcher(int num_events, int num_callbacks, int num_types, int num_threads) -> timing_result {
	// Ensure number of events is divisible by number of threads
	num_events = (num_events / num_threads) * num_threads;

	auto dispatcher = events::synchronized_event_dispatcher{};
	auto sink = std::atomic_int{0};
	auto const const conns = connect_event_types(dispatcher, num_types, num_callbacks, sink);

	auto const events_per_thread = num_events / num_threads;

	// Barrier: producer threads + dispatch thread
	auto sync_point = std::barrier{num_threads + 1};

	auto producers_running = std::atomic_int{num_threads};

	// Per-producer enqueue time tracking
	auto enqueue_durations = std::vector<microseconds_f64>(static_cast<size_t>(num_threads));

	// Producer threads enqueue events while dispatch thread processes in parallel
	auto producers = std::vector<std::thread>{};
	producers.reserve(static_cast<size_t>(num_threads));
	for (int t = 0; t < num_threads; ++t) {
		producers.emplace_back([&, t, events_per_thread, num_types] {
			sync_point.arrive_and_wait();
			auto enq_start = steady_clock::now();
			enqueue_events(dispatcher, events_per_thread, num_types);
			producers_running.fetch_sub(1, std::memory_order_release);
			enqueue_durations[static_cast<size_t>(t)] =
				duration_cast<microseconds_f64>(steady_clock::now() - enq_start);
		});
	}

	// Dispatch thread processes events as they arrive
	auto dispatch_time = microseconds_f64{};
	auto dispatch_thread = std::thread{[&] {
		sync_point.arrive_and_wait();
		auto const start = steady_clock::now();
		while (producers_running.load(std::memory_order_acquire) > 0) {
			dispatcher.dispatch();
		}
		dispatcher.dispatch(); // Final sweep for remaining events
		dispatch_time = duration_cast<microseconds_f64>(steady_clock::now() - start);
	}};

	for (auto& t : producers) t.join();
	dispatch_thread.join();

	auto const total_events = events_per_thread * num_threads;
	auto const total_enqueue = std::reduce(enqueue_durations.begin(), enqueue_durations.end());

	return compute_result(dispatch_time, total_events, total_enqueue);
}

// ============================================================================
// Table rendering
// ============================================================================

auto make_header_color() -> tabulate::Color {
	return tabulate::Color::cyan;
}

auto throughput_color(double eps) -> tabulate::Color {
	if (eps >= 10'000'000.0) return tabulate::Color::green;
	if (eps >= 1'000'000.0)  return tabulate::Color::yellow;
	return tabulate::Color::red;
}

auto print_section_header(std::string const& title) -> void {
	fmt::print(fmt::emphasis::bold | fg(fmt::color::cornflower_blue),
	           "\n{}\n{}\n\n", title, std::string(title.size(), '='));
}


// ---- signal_handler tables ----

auto run_signal_handler_benchmarks() -> void {
	print_section_header("Signal Handler (single-threaded)");

	auto table = tabulate::Table{};
	table.add_row({"Events", "Callbacks", "Total Time", "Per Event", "Throughput"});
	for (size_t i = 0; i < 5; ++i) {
		table[0][i].format()
			.font_color(make_header_color())
			.font_style({tabulate::FontStyle::bold});
	}

	for (auto const events : event_counts) {
		for (auto const cbs : callback_counts) {
			auto const r = bench_signal_handler(events, cbs);
			table.add_row({
				fmt::format("{}", events),
				fmt::format("{}", cbs),
				format_duration(r.total),
				format_duration(r.per_event),
				format_throughput(r.events_per_sec)
			});

			auto const row = table.size() - 1;
			table[row][4].format().font_color(throughput_color(r.events_per_sec));
		}
	}

	table.format()
		.border_top(" ")
		.border_bottom(" ")
		.border_left(" ")
		.border_right(" ")
		.corner(" ");
	table.column(0).format().width(10);
	table.column(2).format().width(14);
	table.column(3).format().width(14);
	table.column(4).format().width(14);

	std::cout << table << "\n";
}

auto run_synchronized_signal_handler_benchmarks() -> void {
	print_section_header("Synchronized Signal Handler (barrier-synchronized publish)");

	auto table = tabulate::Table{};
	table.add_row({"Events", "Callbacks", "Threads", "Total Time", "Per Event", "Throughput"});
	for (size_t i = 0; i < 6; ++i) {
		table[0][i].format()
			.font_color(make_header_color())
			.font_style({tabulate::FontStyle::bold});
	}

	for (auto const events : event_counts) {
		for (auto const cbs : callback_counts) {
			for (auto const threads : thread_counts) {
				auto const r = bench_synchronized_signal_handler(events, cbs, threads);
				table.add_row({
					fmt::format("{}", events),
					fmt::format("{}", cbs),
					fmt::format("{}", threads),
					format_duration(r.total),
					format_duration(r.per_event),
					format_throughput(r.events_per_sec)
				});

				auto const row = table.size() - 1;
				table[row][5].format().font_color(throughput_color(r.events_per_sec));
			}
		}
	}

	table.format()
		.border_top(" ")
		.border_bottom(" ")
		.border_left(" ")
		.border_right(" ")
		.corner(" ");

	std::cout << table << "\n";
}


// ---- Event dispatcher tables ----

auto run_event_dispatcher_benchmarks() -> void {
	print_section_header("Event Dispatcher (single-threaded enqueue + dispatch)");

	auto table = tabulate::Table{};
	table.add_row({"Events", "Callbacks", "Event Types", "Processing Time", "Per Event", "Throughput"});
	for (size_t i = 0; i < 6; ++i) {
		table[0][i].format()
			.font_color(make_header_color())
			.font_style({tabulate::FontStyle::bold});
	}

	for (auto const events : event_counts) {
		for (auto const cbs : callback_counts) {
			for (auto const types : event_type_counts) {
				auto const r = bench_event_dispatcher(events, cbs, types);
				table.add_row({
					fmt::format("{}", events),
					fmt::format("{}", cbs),
					fmt::format("{}", types),
					format_duration(r.total),
					format_duration(r.per_event),
					format_throughput(r.events_per_sec)
				});

				auto const row = table.size() - 1;
				table[row][5].format().font_color(throughput_color(r.events_per_sec));
			}
		}
	}

	table.format()
		.border_top(" ")
		.border_bottom(" ")
		.border_left(" ")
		.border_right(" ")
		.corner(" ");

	std::cout << table << "\n";
}

auto run_synchronized_event_dispatcher_benchmarks() -> void {
	print_section_header("Synchronized Event Dispatcher (parallel enqueue + dispatch)");

	auto table = tabulate::Table{};
	table.add_row({"Events", "Callbacks", "Event Types", "Threads", "Enqueue Time", "Processing Time", "Per Event", "Throughput"});
	for (size_t i = 0; i < 8; ++i) {
		table[0][i].format()
			.font_color(make_header_color())
			.font_style({tabulate::FontStyle::bold});
	}

	for (auto const events : event_counts) {
		for (auto const cbs : callback_counts) {
			for (auto const types : event_type_counts) {
				for (auto const threads : thread_counts) {
					auto const r = bench_synchronized_event_dispatcher(events, cbs, types, threads);
					table.add_row({
						fmt::format("{}", events),
						fmt::format("{}", cbs),
						fmt::format("{}", types),
						fmt::format("{}", threads),
						format_enqueue_time(r.enqueue_time),
						format_duration(r.total),
						format_duration(r.per_event),
						format_throughput(r.events_per_sec)
					});

					auto const row = table.size() - 1;
					table[row][7].format().font_color(throughput_color(r.events_per_sec));
				}
			}
		}
	}

	table.format()
		.border_top(" ")
		.border_bottom(" ")
		.border_left(" ")
		.border_right(" ")
		.corner(" ");

	std::cout << table << "\n";
}

// ============================================================================
// Summary comparison table
// ============================================================================

auto run_comparison_summary() -> void {
	print_section_header("Comparison Summary (1,000 events, 10 callbacks, 10 event types, 2 threads where applicable)");

	constexpr auto n_events = int{1'000};
	constexpr auto n_cbs = int{10};
	constexpr auto n_threads = int{2};
	constexpr auto n_types = int{10};

	auto table = tabulate::Table{};
	table.add_row({"Component", "Enqueue Time", "Processing Time", "Per Event", "Throughput"});
	for (size_t i = 0; i < 5; ++i) {
		table[0][i].format()
			.font_color(make_header_color())
			.font_style({tabulate::FontStyle::bold});
	}

	auto const add_row = [&](std::string const& name, timing_result const& r) {
		table.add_row({name, format_enqueue_time(r.enqueue_time), format_duration(r.total), format_duration(r.per_event), format_throughput(r.events_per_sec)});
		auto const row = table.size() - 1;
		table[row][4].format().font_color(throughput_color(r.events_per_sec));
	};

	add_row("signal_handler",                bench_signal_handler(n_events, n_cbs));
	add_row("synchronized_signal_handler",   bench_synchronized_signal_handler(n_events, n_cbs, n_threads));
	add_row("event_dispatcher",              bench_event_dispatcher(n_events, n_cbs, n_types));
	add_row("synchronized_event_dispatcher", bench_synchronized_event_dispatcher(n_events, n_cbs, n_types, n_threads));

	table.format()
		.border_top(" ")
		.border_bottom(" ")
		.border_left(" ")
		.border_right(" ")
		.corner(" ");

	std::cout << table << "\n";
}


// ============================================================================
// Main
// ============================================================================

auto main() -> int {
	fmt::print(fmt::emphasis::bold | fg(fmt::color::gold),
	           "\n  Events Library Performance Metrics\n"
	           "  -----------------------------------\n\n");

	fmt::print(fg(fmt::color::light_gray),
	           "  Configuration:\n"
	           "    Event counts:      {}\n"
	           "    Callback counts:   {}\n"
	           "    Thread counts:     {}\n"
	           "    Event type counts: {}\n\n",
	           fmt::join(event_counts, ", "),
	           fmt::join(callback_counts, ", "),
	           fmt::join(thread_counts, ", "),
	           fmt::join(event_type_counts, ", "));

	try {
		run_signal_handler_benchmarks();
		run_synchronized_signal_handler_benchmarks();
		run_event_dispatcher_benchmarks();
		run_synchronized_event_dispatcher_benchmarks();
		run_comparison_summary();
	}
	catch (std::exception const& ex) {
		fmt::print(fg(fmt::color::red), "\nError: {}\n", ex.what());
		return 1;
	}

	return 0;
}
