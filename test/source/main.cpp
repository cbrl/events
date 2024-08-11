#include <events/dispatcher/event_dispatcher.hpp>
#include <events/dispatcher/synchronized_event_dispatcher.hpp>
#include <events/dispatcher/async_event_dispatcher.hpp>

#include <chrono>
#include <format>
#include <iostream>
#include <numeric>
#include <thread>


template<size_t ThreadCount>
auto threaded_test_impl(auto&& function)
    -> std::pair<std::chrono::steady_clock::duration, std::chrono::steady_clock::duration> {
	using namespace std::chrono;

	auto threads = std::array<std::thread, ThreadCount>{};
	auto enqueue_times = std::array<steady_clock::duration, ThreadCount>{};
	auto dispatch_times = std::array<steady_clock::duration, ThreadCount>{};

	for (size_t i = 0; i < ThreadCount; ++i) {
		threads.at(i) = std::thread{[&, i] { function(enqueue_times.at(i), dispatch_times.at(i)); }};
	}

	for (auto& thread : threads) {
		thread.join();
	}

	return {
	    std::accumulate(enqueue_times.begin(), enqueue_times.end(), steady_clock::duration{}),
	    std::accumulate(dispatch_times.begin(), dispatch_times.end(), steady_clock::duration{})};
}

template<size_t EventCount, size_t ThreadCount>
auto threaded_test(auto& dispatcher) -> void {
	using namespace std::chrono;

	constexpr auto total_events = ThreadCount * EventCount;

	std::cout << std::format("{}: {} events on {} threads\n", typeid(dispatcher).name(), EventCount, ThreadCount);

	auto count = std::make_shared<std::atomic_size_t>(0ull);

	auto handle = events::scoped_connection{dispatcher.template connect<int>([count](int) { ++(*count); })};

	auto const [bulk_enqueue_time, bulk_dispatch_time] =
	    threaded_test_impl<ThreadCount>([&](steady_clock::duration& enqueue_time,
	                                        steady_clock::duration& dispatch_time) {
		    auto const enqueue_begin = steady_clock::now();
		    for (size_t n = 0; n < EventCount; ++n) {
			    dispatcher.template enqueue<int>(0);
		    }
		    auto const enqueue_end = steady_clock::now();

		    auto const dispatch_begin = steady_clock::now();
		    if constexpr (requires { dispatcher.async_dispatch(); }) {
			    //dispatcher.async_dispatch(boost::asio::use_future).get();
			    dispatcher.async_dispatch();
		    }
		    else {
			    dispatcher.dispatch();
		    }
		    auto const dispatch_end = steady_clock::now();

		    enqueue_time = enqueue_end - enqueue_begin;
		    dispatch_time = dispatch_end - dispatch_begin;
	    });

	auto const bulk_time = duration_cast<milliseconds>(bulk_enqueue_time + bulk_dispatch_time);
	std::cout << std::format("    bulk dispatch {} of {}: {}\n", count->load(), total_events, bulk_time);
	std::cout << std::format("        enqueue:  {}\n", duration_cast<milliseconds>(bulk_enqueue_time));
	std::cout << std::format("        dispatch: {}\n", duration_cast<milliseconds>(bulk_dispatch_time));

	*count = 0;
	auto const [single_enqueue_time, single_dispatch_time] =
	    threaded_test_impl<ThreadCount>([&](steady_clock::duration& enqueue_time,
	                                        steady_clock::duration& dispatch_time) {
		    for (size_t n = 0; n < EventCount; ++n) {
			    auto const enqueue_begin = steady_clock::now();
			    dispatcher.template enqueue<int>(0);
			    auto const enqueue_end = steady_clock::now();
			    enqueue_time += enqueue_end - enqueue_begin;

			    auto const dispatch_begin = steady_clock::now();
			    if constexpr (requires { dispatcher.async_dispatch(); }) {
				    //dispatcher.async_dispatch(boost::asio::use_future).get();
				    dispatcher.async_dispatch();
			    }
			    else {
				    dispatcher.dispatch();
			    }
			    auto const dispatch_end = steady_clock::now();
			    dispatch_time += dispatch_end - dispatch_begin;
		    }
	    });

	auto const single_time = duration_cast<milliseconds>(single_enqueue_time + single_dispatch_time);
	std::cout << std::format("    single dispatch {} of {}: {}\n", count->load(), total_events, single_time);
	std::cout << std::format("        enqueue:  {}\n", duration_cast<milliseconds>(single_enqueue_time));
	std::cout << std::format("        dispatch: {}\n", duration_cast<milliseconds>(single_dispatch_time)) << std::flush;
}

auto main() -> int {
	static constexpr auto thread_count = 5ull;
	static constexpr auto event_count = 100'000ull;

	try {
		auto ctx = boost::asio::io_context{};

		auto threads = std::array<std::jthread, 3>{};
		for (auto& thread : threads) {
			thread = std::jthread{[&ctx](std::stop_token stop) {
				auto work = boost::asio::make_work_guard(ctx);

				auto on_stop = std::stop_callback{stop, [&work] { work.reset(); }};

				while (!stop.stop_requested()) {
					try {
						if (ctx.run_one() == 0) {
							std::this_thread::yield();
						}
					}
					catch (std::exception const& e) {
						(void)e;
					}
				}
			}};
		}

		auto dispatcher = events::event_dispatcher{};
		auto sync_dispatcher = events::synchronized_event_dispatcher{};
		auto async_dispatcher = events::async_event_dispatcher{ctx};

		threaded_test<event_count * thread_count, 1>(dispatcher);
		threaded_test<event_count, thread_count>(sync_dispatcher);
		threaded_test<event_count, thread_count>(async_dispatcher);

		for (auto& thread : threads) {
			thread.request_stop();
		}
		for (auto& thread : threads) {
			thread.join();
		}
	}
	catch (std::exception const& ex) {
		std::cout << std::format("Unhandled exception: {}\n", ex.what());
		return 1;
	}

	return 0;
}
