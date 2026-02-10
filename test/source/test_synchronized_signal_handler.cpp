#include <events/signal_handler/synchronized_signal_handler.hpp>
#include <events/connection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <barrier>
#include <thread>
#include <vector>


// ---- Basic functionality (mirrors signal_handler tests) ----

TEST_CASE("synchronized_signal_handler: starts with zero size", "[synchronized_signal_handler]") {
	auto sigh = events::synchronized_signal_handler<void()>{};
	CHECK(sigh.size() == 0);
}

TEST_CASE("synchronized_signal_handler: connect and publish", "[synchronized_signal_handler]") {
	auto sigh = events::synchronized_signal_handler<void(int&)>{};
	int value = 0;

	auto c1 = sigh.connect([](int& n) { n += 1; });
	auto c2 = sigh.connect([](int& n) { n += 10; });

	sigh.publish(value);
	CHECK(value == 11);
}

TEST_CASE("synchronized_signal_handler: disconnect removes callback", "[synchronized_signal_handler]") {
	auto sigh = events::synchronized_signal_handler<void(int&)>{};
	int value = 0;

	auto conn = sigh.connect([](int& n) { ++n; });
	conn.disconnect();

	sigh.publish(value);
	CHECK(value == 0);
}

TEST_CASE("synchronized_signal_handler: disconnect_all", "[synchronized_signal_handler]") {
	auto sigh = events::synchronized_signal_handler<void()>{};
	auto c1 = sigh.connect([] {});
	auto c2 = sigh.connect([] {});
	REQUIRE(sigh.size() == 2);

	sigh.disconnect_all();
	CHECK(sigh.size() == 0);
}

TEST_CASE("synchronized_signal_handler: return values", "[synchronized_signal_handler]") {
	auto sigh = events::synchronized_signal_handler<int(int)>{};
	auto c1 = sigh.connect([](int n) { return n * 2; });
	auto c2 = sigh.connect([](int n) { return n * 3; });

	auto results = sigh.publish(5);
	REQUIRE(results.size() == 2);
	CHECK(results[0] == 10);
	CHECK(results[1] == 15);
}

TEST_CASE("synchronized_signal_handler: publish with no callbacks is safe", "[synchronized_signal_handler]") {
	auto sigh = events::synchronized_signal_handler<void(int)>{};
	sigh.publish(42); // must not crash
}


// ---- Copy and move ----

TEST_CASE("synchronized_signal_handler: copy constructor shares snapshot", "[synchronized_signal_handler]") {
	auto sigh1 = events::synchronized_signal_handler<void(int&)>{};
	auto conn = sigh1.connect([](int& n) { ++n; });

	auto sigh2 = sigh1; // copy

	int value = 0;
	sigh2.publish(value);
	CHECK(value == 1);
}

TEST_CASE("synchronized_signal_handler: move constructor transfers state", "[synchronized_signal_handler]") {
	auto sigh1 = events::synchronized_signal_handler<void(int&)>{};
	auto conn = sigh1.connect([](int& n) { ++n; });

	auto sigh2 = std::move(sigh1);

	int value = 0;
	sigh2.publish(value);
	CHECK(value == 1);
}


// ---- Thread safety ----

TEST_CASE("synchronized_signal_handler: concurrent publish", "[synchronized_signal_handler][threaded]") {
	auto sigh = events::synchronized_signal_handler<void(int)>{};
	std::atomic<int> total{0};

	auto conn = sigh.connect([&](int n) { total.fetch_add(n, std::memory_order_relaxed); });

	constexpr int num_threads = 8;
	constexpr int publishes_per_thread = 10'000;

	auto threads = std::vector<std::thread>{};
	threads.reserve(num_threads);

	for (int t = 0; t < num_threads; ++t) {
		threads.emplace_back([&sigh] {
			for (int i = 0; i < publishes_per_thread; ++i) {
				sigh.publish(1);
			}
		});
	}

	for (auto& t : threads) {
		t.join();
	}

	CHECK(total.load() == num_threads * publishes_per_thread);
}

TEST_CASE("synchronized_signal_handler: concurrent connect and publish", "[synchronized_signal_handler][threaded]") {
	auto sigh = events::synchronized_signal_handler<void()>{};
	std::atomic<int> call_count{0};

	constexpr int num_threads = 4;
	constexpr int ops_per_thread = 5'000;

	auto threads = std::vector<std::thread>{};
	threads.reserve(num_threads * 2);

	// Threads that connect
	for (int t = 0; t < num_threads; ++t) {
		threads.emplace_back([&sigh, &call_count] {
			std::vector<events::connection> conns;
			for (int i = 0; i < ops_per_thread; ++i) {
				conns.push_back(sigh.connect([&call_count] {
					call_count.fetch_add(1, std::memory_order_relaxed);
				}));
			}
			// Keep connections alive until thread finishes
			for (auto& c : conns) {
				c.disconnect();
			}
		});
	}

	// Threads that publish
	for (int t = 0; t < num_threads; ++t) {
		threads.emplace_back([&sigh] {
			for (int i = 0; i < ops_per_thread; ++i) {
				sigh.publish();
			}
		});
	}

	for (auto& t : threads) {
		t.join();
	}

	CHECK(true); // if we get here, no deadlock or crash
}

TEST_CASE("synchronized_signal_handler: concurrent connect and disconnect", "[synchronized_signal_handler][threaded]") {
	auto sigh = events::synchronized_signal_handler<void()>{};

	constexpr int num_threads = 4;
	constexpr int ops_per_thread = 5'000;

	auto threads = std::vector<std::thread>{};
	threads.reserve(num_threads);

	for (int t = 0; t < num_threads; ++t) {
		threads.emplace_back([&sigh] {
			for (int i = 0; i < ops_per_thread; ++i) {
				auto conn = sigh.connect([] {});
				conn.disconnect();
			}
		});
	}

	for (auto& t : threads) {
		t.join();
	}

	CHECK(sigh.size() == 0);
}


// ---- Reentrancy ----

TEST_CASE("synchronized_signal_handler: connect during publish", "[synchronized_signal_handler][reentrancy]") {
	auto sigh = events::synchronized_signal_handler<void()>{};

	int outer_calls = 0;
	int inner_calls = 0;
	events::connection inner_conn;

	auto outer_conn = sigh.connect([&] {
		++outer_calls;
		if (outer_calls == 1) {
			// Connecting during publish uses copy-on-write, so the new callback
			// should NOT be visible during this iteration.
			inner_conn = sigh.connect([&] { ++inner_calls; });
		}
	});

	sigh.publish();
	CHECK(outer_calls == 1);
	CHECK(inner_calls == 0); // new callback not visible during current publish

	// Second publish should see both callbacks
	sigh.publish();
	CHECK(outer_calls == 2);
	CHECK(inner_calls == 1);
}

TEST_CASE("synchronized_signal_handler: disconnect during publish", "[synchronized_signal_handler][reentrancy]") {
	auto sigh = events::synchronized_signal_handler<void()>{};

	int callback_a_calls = 0;
	int callback_b_calls = 0;
	events::connection conn_b;

	auto conn_a = sigh.connect([&] {
		++callback_a_calls;
		conn_b.disconnect(); // disconnect B while iterating
	});

	conn_b = sigh.connect([&] {
		++callback_b_calls;
	});

	sigh.publish();
	// Because synchronized_signal_handler uses a snapshot, B should still be called
	// during this publish (the snapshot is immutable).
	CHECK(callback_a_calls == 1);
	CHECK(callback_b_calls == 1);

	// After disconnect, B should not be called on the next publish
	callback_a_calls = 0;
	callback_b_calls = 0;
	sigh.publish();
	CHECK(callback_a_calls == 1);
	CHECK(callback_b_calls == 0);
}

TEST_CASE("synchronized_signal_handler: disconnect_all during publish from another thread",
          "[synchronized_signal_handler][threaded][reentrancy]") {
	auto sigh = events::synchronized_signal_handler<void()>{};
	std::atomic<bool> running{true};
	std::atomic<int> call_count{0};

	auto conn = sigh.connect([&] {
		call_count.fetch_add(1, std::memory_order_relaxed);
	});

	// Thread that continuously publishes
	auto publisher = std::thread{[&] {
		while (running.load(std::memory_order_relaxed)) {
			sigh.publish();
		}
	}};

	// Let it run briefly, then disconnect
	std::this_thread::sleep_for(std::chrono::milliseconds{5});
	sigh.disconnect_all();
	running.store(false, std::memory_order_relaxed);

	publisher.join();

	// Should have been called at least once
	CHECK(call_count.load() > 0);
	CHECK(sigh.size() == 0);
}
