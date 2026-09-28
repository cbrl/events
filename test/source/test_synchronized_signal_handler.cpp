#include <events/signal_handler/synchronized_signal_handler.hpp>
#include <events/connection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <random>
#include <string>
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
	// B was disconnected before the publish reached it, so it's skipped even though it's in the snapshot
	CHECK(callback_a_calls == 1);
	CHECK(callback_b_calls == 0);

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


// ---- Connections and lifetime ----

TEST_CASE("synchronized_signal_handler: a stale copy doesn't disconnect a newer callback", "[synchronized_signal_handler]") {
	auto sigh = events::synchronized_signal_handler<void()>{};
	int b_calls = 0;

	auto conn_a = sigh.connect([] {});
	auto copy_a = conn_a;
	conn_a.disconnect();
	CHECK_FALSE(copy_a.connected());

	auto conn_b = sigh.connect([&] { ++b_calls; });
	copy_a.disconnect(); // must not affect B

	sigh.publish();
	CHECK(b_calls == 1);
	CHECK(sigh.size() == 1);
}

TEST_CASE("synchronized_signal_handler: connection may outlive the handler", "[synchronized_signal_handler]") {
	auto conn = events::connection{};
	auto scoped = events::scoped_connection{};

	{
		auto sigh = events::synchronized_signal_handler<void()>{};
		conn = sigh.connect([] {});
		scoped = sigh.connect([] {});
		CHECK(conn.connected());
	}

	CHECK_FALSE(conn.connected());
	CHECK_FALSE(scoped.connected());
	conn.disconnect(); // no-op
}

TEST_CASE("synchronized_signal_handler: disconnect_all during publish stops the remaining callbacks", "[synchronized_signal_handler][reentrancy]") {
	auto sigh = events::synchronized_signal_handler<void()>{};
	int calls = 0;

	auto a = sigh.connect([&] {
		++calls;
		sigh.disconnect_all();
	});
	auto b = sigh.connect([&] { ++calls; });

	sigh.publish();
	CHECK(calls == 1);
	CHECK(sigh.size() == 0);
}

TEST_CASE("synchronized_signal_handler: a callback can disconnect itself during publish", "[synchronized_signal_handler][reentrancy]") {
	auto sigh = events::synchronized_signal_handler<void()>{};
	events::connection self_conn;
	std::size_t observed_size = 0;

	self_conn = sigh.connect([&self_conn, &observed_size, payload = std::string(200, 'x')] {
		self_conn.disconnect();
		observed_size = payload.size();
	});

	sigh.publish();
	CHECK(observed_size == 200);
	CHECK(sigh.size() == 0);
}

TEST_CASE("synchronized_signal_handler: a callback's destructor may use the handler", "[synchronized_signal_handler][reentrancy]") {
	auto sigh = events::synchronized_signal_handler<void()>{};

	// The outer callback owns a scoped_connection to the inner one. Destroying the outer callback must not happen
	// while the handler's mutex is locked.
	auto inner = std::make_shared<events::scoped_connection>(sigh.connect([] {}));
	auto outer = sigh.connect([inner] {});
	inner.reset();

	outer.disconnect();
	CHECK(sigh.size() == 0);

	auto inner2 = std::make_shared<events::scoped_connection>(sigh.connect([] {}));
	auto outer2 = sigh.connect([inner2] {});
	inner2.reset();

	sigh.disconnect_all();
	CHECK(sigh.size() == 0);
}

TEST_CASE("synchronized_signal_handler: move construction transfers the callbacks", "[synchronized_signal_handler]") {
	auto sigh1 = events::synchronized_signal_handler<void()>{};
	int calls = 0;
	auto conn = sigh1.connect([&] { ++calls; });

	auto sigh2 = std::move(sigh1);
	CHECK(sigh1.size() == 0); //NOLINT(bugprone-use-after-move)
	CHECK(sigh2.size() == 1);

	// Connections from the moved-from handler no longer refer to the callback
	CHECK_FALSE(conn.connected());
	conn.disconnect();
	sigh2.publish();
	CHECK(calls == 1);

	// The moved-from handler is still usable
	auto conn2 = sigh1.connect([&] { ++calls; }); //NOLINT(bugprone-use-after-move)
	sigh1.publish();
	CHECK(calls == 2);
}

TEST_CASE("synchronized_signal_handler: assignment disconnects the previous callbacks", "[synchronized_signal_handler]") {
	auto target = events::synchronized_signal_handler<void()>{};
	auto source = events::synchronized_signal_handler<void()>{};
	int target_calls = 0;
	int source_calls = 0;

	auto target_conn = target.connect([&] { ++target_calls; });
	auto source_conn = source.connect([&] { ++source_calls; });

	target = source;
	CHECK_FALSE(target_conn.connected());
	CHECK(source_conn.connected());
	target.publish();
	CHECK(target_calls == 0);
	CHECK(source_calls == 1);

	// A stale connection of the target must not affect the copied callbacks, even though ids may overlap
	target_conn.disconnect();
	CHECK(target.size() == 1);

	auto moved_from = events::synchronized_signal_handler<void()>{};
	auto moved_conn = moved_from.connect([&] { ++source_calls; });
	target = std::move(moved_from);
	CHECK(target.size() == 1);
	CHECK_FALSE(moved_conn.connected());
	target.publish();
	CHECK(source_calls == 2);
}

TEST_CASE("synchronized_signal_handler: concurrent connect, disconnect, and publish", "[synchronized_signal_handler][threaded]") {
	auto sigh = events::synchronized_signal_handler<void()>{};
	std::atomic<bool> running{true};
	std::atomic<int> calls{0};
	std::atomic<int> failures{0}; // Catch2 assertions aren't thread-safe

	auto publishers = std::vector<std::thread>{};
	for (int t = 0; t < 2; ++t) {
		publishers.emplace_back([&] {
			while (running.load(std::memory_order_relaxed)) {
				sigh.publish();
			}
		});
	}

	auto mutators = std::vector<std::thread>{};
	for (int t = 0; t < 4; ++t) {
		mutators.emplace_back([&] {
			for (int i = 0; i < 2'000; ++i) {
				auto conn = sigh.connect([&calls] { calls.fetch_add(1, std::memory_order_relaxed); });
				auto copy = conn;
				if (!copy.connected()) {
					failures.fetch_add(1, std::memory_order_relaxed);
				}
				conn.disconnect();
				if (copy.connected()) {
					failures.fetch_add(1, std::memory_order_relaxed);
				}
				copy.disconnect();
			}
		});
	}

	for (auto& t : mutators) {
		t.join();
	}
	running.store(false, std::memory_order_relaxed);
	for (auto& t : publishers) {
		t.join();
	}

	CHECK(failures.load() == 0);
	CHECK(sigh.size() == 0);
}


// ---- Model test ----

TEST_CASE("synchronized_signal_handler: random connects and disconnects match a reference model", "[synchronized_signal_handler]") {
	auto rng = std::mt19937{12345};
	auto sigh = events::synchronized_signal_handler<void(std::vector<int>&)>{};

	struct entry {
		int value;
		events::connection conn;
	};
	auto model = std::vector<entry>{};
	int next_value = 0;

	for (int step = 0; step < 20'000; ++step) {
		auto const op = std::uniform_int_distribution<int>{0, 99}(rng);

		if (op < 45) {
			int const value = next_value++;
			model.push_back({value, sigh.connect([value](std::vector<int>& out) { out.push_back(value); })});
		}
		else if (op < 90) {
			if (!model.empty()) {
				auto const index = std::uniform_int_distribution<std::size_t>{0, model.size() - 1}(rng);
				model[index].conn.disconnect();
				model.erase(model.begin() + static_cast<std::ptrdiff_t>(index));
			}
		}
		else if (op < 99) {
			auto invoked = std::vector<int>{};
			sigh.publish(invoked);

			auto expected = std::vector<int>{};
			for (auto const& e : model) {
				expected.push_back(e.value);
			}

			REQUIRE(invoked == expected);
			REQUIRE(sigh.size() == model.size());
		}
		else {
			sigh.disconnect_all();
			for (auto const& e : model) {
				REQUIRE_FALSE(e.conn.connected());
			}
			model.clear();
		}
	}

	REQUIRE(sigh.size() == model.size());
}
