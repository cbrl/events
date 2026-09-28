#include <events/dispatcher/synchronized_event_dispatcher.hpp>
#include <events/connection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>


struct sync_test_event {
	int value;
};

struct sync_other_event {
	std::string message;
};


// ---- Basic functionality ----

TEST_CASE("synchronized_event_dispatcher: connect and send", "[synchronized_event_dispatcher]") {
	auto dispatcher = events::synchronized_event_dispatcher{};
	int received = 0;

	auto conn = dispatcher.connect<sync_test_event>([&](sync_test_event const& e) {
		received = e.value;
	});

	dispatcher.send(sync_test_event{42});
	CHECK(received == 42);
}

TEST_CASE("synchronized_event_dispatcher: enqueue and dispatch", "[synchronized_event_dispatcher]") {
	auto dispatcher = events::synchronized_event_dispatcher{};
	std::vector<int> received;

	auto conn = dispatcher.connect<sync_test_event>([&](sync_test_event const& e) {
		received.push_back(e.value);
	});

	dispatcher.enqueue(sync_test_event{1});
	dispatcher.enqueue(sync_test_event{2});
	dispatcher.enqueue(sync_test_event{3});

	CHECK(received.empty());

	dispatcher.dispatch();
	REQUIRE(received.size() == 3);
	CHECK(received[0] == 1);
	CHECK(received[1] == 2);
	CHECK(received[2] == 3);
}

TEST_CASE("synchronized_event_dispatcher: multiple event types", "[synchronized_event_dispatcher]") {
	auto dispatcher = events::synchronized_event_dispatcher{};
	int int_received = 0;
	std::string str_received;

	auto c1 = dispatcher.connect<sync_test_event>([&](sync_test_event const& e) {
		int_received = e.value;
	});

	auto c2 = dispatcher.connect<sync_other_event>([&](sync_other_event const& e) {
		str_received = e.message;
	});

	dispatcher.send(sync_test_event{99});
	dispatcher.send(sync_other_event{"world"});

	CHECK(int_received == 99);
	CHECK(str_received == "world");
}

TEST_CASE("synchronized_event_dispatcher: queue_size", "[synchronized_event_dispatcher]") {
	auto dispatcher = events::synchronized_event_dispatcher{};
	auto conn = dispatcher.connect<sync_test_event>([](sync_test_event const&) {});

	CHECK(dispatcher.queue_size() == 0);

	dispatcher.enqueue(sync_test_event{1});
	dispatcher.enqueue(sync_test_event{2});
	CHECK(dispatcher.queue_size() == 2);
	CHECK(dispatcher.queue_size<sync_test_event>() == 2);
	CHECK(dispatcher.queue_size<sync_other_event>() == 0);

	dispatcher.dispatch();
	CHECK(dispatcher.queue_size() == 0);
}

TEST_CASE("synchronized_event_dispatcher: enqueue range", "[synchronized_event_dispatcher]") {
	auto dispatcher = events::synchronized_event_dispatcher{};
	int total = 0;

	auto conn = dispatcher.connect<sync_test_event>([&](sync_test_event const& e) {
		total += e.value;
	});

	auto events_vec = std::vector<sync_test_event>{{1}, {2}, {3}};
	dispatcher.enqueue<sync_test_event>(events_vec);

	dispatcher.dispatch();
	CHECK(total == 6);
}

TEST_CASE("synchronized_event_dispatcher: send range", "[synchronized_event_dispatcher]") {
	auto dispatcher = events::synchronized_event_dispatcher{};
	int total = 0;

	auto conn = dispatcher.connect<sync_test_event>([&](sync_test_event const& e) {
		total += e.value;
	});

	auto events_vec = std::vector<sync_test_event>{{10}, {20}};
	dispatcher.send<sync_test_event>(events_vec);
	CHECK(total == 30);
}

TEST_CASE("synchronized_event_dispatcher: send and enqueue construct events the same way", "[synchronized_event_dispatcher]") {
	auto dispatcher = events::synchronized_event_dispatcher{};
	auto sizes = std::vector<std::size_t>{};

	auto conn = dispatcher.connect<std::vector<int>>([&](std::vector<int> const& v) { sizes.push_back(v.size()); });

	dispatcher.send<std::vector<int>>(3u, 7);
	dispatcher.enqueue<std::vector<int>>(3u, 7);
	dispatcher.dispatch();

	CHECK(sizes == std::vector<std::size_t>{3, 3});
}

TEST_CASE("synchronized_event_dispatcher: enqueue and send accept non-common ranges", "[synchronized_event_dispatcher]") {
	auto dispatcher = events::synchronized_event_dispatcher{};
	int total = 0;

	auto conn = dispatcher.connect<sync_test_event>([&](sync_test_event const& e) {
		total += e.value;
	});

	auto source = std::vector<sync_test_event>{{1}, {2}, {3}, {100}};
	auto small = source | std::views::take_while([](sync_test_event const& e) { return e.value < 10; });

	dispatcher.enqueue<sync_test_event>(small);
	dispatcher.dispatch();
	CHECK(total == 6);

	dispatcher.send<sync_test_event>(small);
	CHECK(total == 12);
}

TEST_CASE("synchronized_event_dispatcher: disconnect", "[synchronized_event_dispatcher]") {
	auto dispatcher = events::synchronized_event_dispatcher{};
	int count = 0;

	auto conn = dispatcher.connect<sync_test_event>([&](sync_test_event const&) { ++count; });
	conn.disconnect();

	dispatcher.send(sync_test_event{1});
	CHECK(count == 0);
}


// ---- Thread safety ----

TEST_CASE("synchronized_event_dispatcher: concurrent enqueue and dispatch",
          "[synchronized_event_dispatcher][threaded]") {
	auto dispatcher = events::synchronized_event_dispatcher{};
	std::atomic<int> total{0};

	auto conn = dispatcher.connect<sync_test_event>([&](sync_test_event const& e) {
		total.fetch_add(e.value, std::memory_order_relaxed);
	});

	constexpr int num_threads = 4;
	constexpr int events_per_thread = 5'000;

	auto threads = std::vector<std::thread>{};
	threads.reserve(num_threads);

	for (int t = 0; t < num_threads; ++t) {
		threads.emplace_back([&dispatcher] {
			for (int i = 0; i < events_per_thread; ++i) {
				dispatcher.enqueue(sync_test_event{1});
				if ((i % 100) == 0) {
					dispatcher.dispatch();
				}
			}
			dispatcher.dispatch(); // flush remaining
		});
	}

	for (auto& t : threads) {
		t.join();
	}

	// Final dispatch to catch any remaining events
	dispatcher.dispatch();

	CHECK(total.load() == num_threads * events_per_thread);
}

TEST_CASE("synchronized_event_dispatcher: concurrent send",
          "[synchronized_event_dispatcher][threaded]") {
	auto dispatcher = events::synchronized_event_dispatcher{};
	std::atomic<int> total{0};

	auto conn = dispatcher.connect<sync_test_event>([&](sync_test_event const& e) {
		total.fetch_add(e.value, std::memory_order_relaxed);
	});

	constexpr int num_threads = 4;
	constexpr int sends_per_thread = 5'000;

	auto threads = std::vector<std::thread>{};
	threads.reserve(num_threads);

	for (int t = 0; t < num_threads; ++t) {
		threads.emplace_back([&dispatcher] {
			for (int i = 0; i < sends_per_thread; ++i) {
				dispatcher.send(sync_test_event{1});
			}
		});
	}

	for (auto& t : threads) {
		t.join();
	}

	CHECK(total.load() == num_threads * sends_per_thread);
}

namespace {
struct sync_id_event {
	int id;
};
}  //namespace

TEST_CASE("synchronized_event_dispatcher: concurrent connect from multiple threads",
          "[synchronized_event_dispatcher][threaded]") {
	auto dispatcher = events::synchronized_event_dispatcher{};

	constexpr int num_threads = 4;
	constexpr int connects_per_thread = 1'000;

	auto threads = std::vector<std::thread>{};
	threads.reserve(num_threads);

	std::atomic<int> test_calls{0};
	std::atomic<int> id_calls{0};

	for (int t = 0; t < num_threads; ++t) {
		threads.emplace_back([&dispatcher, &test_calls, &id_calls] {
			for (int i = 0; i < connects_per_thread; ++i) {
				// Destroying a connection object doesn't disconnect the callback. The threads race to create the
				// dispatcher for sync_id_event, which doesn't exist yet.
				[[maybe_unused]] auto const c1 = dispatcher.connect<sync_test_event>([&test_calls](sync_test_event const&) {
					test_calls.fetch_add(1, std::memory_order_relaxed);
				});
				[[maybe_unused]] auto const c2 = dispatcher.connect<sync_id_event>([&id_calls](sync_id_event const&) {
					id_calls.fetch_add(1, std::memory_order_relaxed);
				});
			}
		});
	}

	for (auto& t : threads) {
		t.join();
	}

	dispatcher.send(sync_test_event{1});
	dispatcher.send(sync_id_event{1});
	CHECK(test_calls.load() == num_threads * connects_per_thread);
	CHECK(id_calls.load() == num_threads * connects_per_thread);
}


// ---- Reentrancy ----

TEST_CASE("synchronized_event_dispatcher: enqueue during dispatch",
          "[synchronized_event_dispatcher][reentrancy]") {
	auto dispatcher = events::synchronized_event_dispatcher{};
	int dispatch_count = 0;

	auto conn = dispatcher.connect<sync_test_event>([&](sync_test_event const& e) {
		++dispatch_count;
		if (e.value < 3) {
			dispatcher.enqueue(sync_test_event{e.value + 1});
		}
	});

	dispatcher.enqueue(sync_test_event{1});
	dispatcher.dispatch();
	CHECK(dispatch_count == 1);

	dispatcher.dispatch(); // processes {2}
	CHECK(dispatch_count == 2);

	dispatcher.dispatch(); // processes {3}
	CHECK(dispatch_count == 3);

	dispatcher.dispatch(); // nothing left
	CHECK(dispatch_count == 3);
}

TEST_CASE("synchronized_event_dispatcher: connect new event type during dispatch",
          "[synchronized_event_dispatcher][reentrancy]") {
	auto dispatcher = events::synchronized_event_dispatcher{};
	int test_count = 0;
	int other_count = 0;
	events::connection other_conn;

	auto conn = dispatcher.connect<sync_test_event>([&](sync_test_event const&) {
		++test_count;
		if (test_count == 1) {
			other_conn = dispatcher.connect<sync_other_event>(
			    [&](sync_other_event const&) { ++other_count; });
		}
	});

	dispatcher.enqueue(sync_test_event{1});
	dispatcher.dispatch();
	CHECK(test_count == 1);

	dispatcher.send(sync_other_event{"test"});
	CHECK(other_count == 1);
}

TEST_CASE("synchronized_event_dispatcher: concurrent enqueue during dispatch from different threads",
          "[synchronized_event_dispatcher][threaded][reentrancy]") {
	auto dispatcher = events::synchronized_event_dispatcher{};
	std::atomic<int> total{0};

	auto conn = dispatcher.connect<sync_test_event>([&](sync_test_event const& e) {
		total.fetch_add(e.value, std::memory_order_relaxed);
	});

	constexpr int num_enqueue_threads = 4;
	constexpr int events_per_thread = 2'000;
	std::atomic<bool> stop{false};

	// Enqueue threads
	auto enqueuers = std::vector<std::thread>{};
	enqueuers.reserve(num_enqueue_threads);
	for (int t = 0; t < num_enqueue_threads; ++t) {
		enqueuers.emplace_back([&dispatcher] {
			for (int i = 0; i < events_per_thread; ++i) {
				dispatcher.enqueue(sync_test_event{1});
			}
		});
	}

	// Dispatch thread
	auto dispatch_thread = std::thread{[&dispatcher, &stop] {
		while (!stop.load(std::memory_order_relaxed)) {
			dispatcher.dispatch();
			std::this_thread::yield();
		}
		dispatcher.dispatch(); // one final dispatch
	}};

	for (auto& t : enqueuers) {
		t.join();
	}
	stop.store(true, std::memory_order_relaxed);
	dispatch_thread.join();

	// Final dispatch to get any remaining events
	dispatcher.dispatch();

	CHECK(total.load() == num_enqueue_threads * events_per_thread);
}


// ---- Move semantics ----

TEST_CASE("synchronized_event_dispatcher: move preserves state", "[synchronized_event_dispatcher]") {
	auto dispatcher1 = events::synchronized_event_dispatcher{};
	int received = 0;

	auto conn = dispatcher1.connect<sync_test_event>([&](sync_test_event const& e) {
		received = e.value;
	});

	dispatcher1.enqueue(sync_test_event{77});

	auto dispatcher2 = std::move(dispatcher1);
	CHECK(dispatcher1.queue_size() == 0); //NOLINT(bugprone-use-after-move,hicpp-invalid-access-moved)
	CHECK(dispatcher2.queue_size() == 1);

	dispatcher2.dispatch();
	CHECK(received == 77);
	CHECK(conn.connected());
}

TEST_CASE("synchronized_event_dispatcher: a moved-from dispatcher is empty and can be reused", "[synchronized_event_dispatcher]") {
	auto dispatcher1 = events::synchronized_event_dispatcher{};
	int received1 = 0;
	int received2 = 0;

	auto conn1 = dispatcher1.connect<sync_test_event>([&](sync_test_event const& e) { received1 += e.value; });
	dispatcher1.enqueue(sync_test_event{1});

	auto dispatcher2 = events::synchronized_event_dispatcher{};
	dispatcher2 = std::move(dispatcher1);

	// dispatcher1 no longer shares anything with dispatcher2
	auto conn2 = dispatcher1.connect<sync_test_event>([&](sync_test_event const& e) { received2 += e.value; }); //NOLINT(bugprone-use-after-move,hicpp-invalid-access-moved)
	dispatcher1.enqueue(sync_test_event{10});
	CHECK(dispatcher1.queue_size() == 1);
	CHECK(dispatcher2.queue_size() == 1);

	dispatcher1.dispatch();
	CHECK(received1 == 0);
	CHECK(received2 == 10);

	dispatcher2.dispatch();
	CHECK(received1 == 1);
	CHECK(received2 == 10);
}

TEST_CASE("synchronized_event_dispatcher: move assignment disconnects the previous callbacks and discards their events", "[synchronized_event_dispatcher]") {
	auto dispatcher = events::synchronized_event_dispatcher{};
	int received = 0;

	auto conn = dispatcher.connect<sync_test_event>([&](sync_test_event const&) { ++received; });
	dispatcher.enqueue(sync_test_event{1});

	dispatcher = events::synchronized_event_dispatcher{};
	CHECK_FALSE(conn.connected());
	CHECK(dispatcher.queue_size() == 0);

	dispatcher.enqueue(sync_test_event{1});
	dispatcher.dispatch();
	CHECK(received == 0);
}


// ---- Edge cases ----

TEST_CASE("synchronized_event_dispatcher: dispatch with no enqueued events", "[synchronized_event_dispatcher]") {
	auto dispatcher = events::synchronized_event_dispatcher{};
	auto conn = dispatcher.connect<sync_test_event>([](sync_test_event const&) {});
	dispatcher.dispatch();
	CHECK(dispatcher.queue_size() == 0);
}

TEST_CASE("synchronized_event_dispatcher: send with no callbacks", "[synchronized_event_dispatcher]") {
	auto dispatcher = events::synchronized_event_dispatcher{};
	dispatcher.send(sync_test_event{1});
	CHECK(dispatcher.queue_size() == 0);
}


// ---- Lifetime ----

TEST_CASE("synchronized_event_dispatcher: connection may outlive the dispatcher", "[synchronized_event_dispatcher]") {
	auto conn = events::connection{};
	auto scoped = events::scoped_connection{};

	{
		auto dispatcher = events::synchronized_event_dispatcher{};
		conn = dispatcher.connect<sync_test_event>([](sync_test_event const&) {});
		scoped = dispatcher.connect<sync_test_event>([](sync_test_event const&) {});
		CHECK(conn.connected());
	}

	CHECK_FALSE(conn.connected());
	CHECK_FALSE(scoped.connected());
	conn.disconnect(); // no-op
}

namespace {
struct run_on_destroy {
	std::function<void()> action;

	run_on_destroy() = default;
	run_on_destroy(run_on_destroy const&) = delete;
	run_on_destroy(run_on_destroy&&) = delete;
	~run_on_destroy() {
		action();
	}
	auto operator=(run_on_destroy const&) -> run_on_destroy& = delete;
	auto operator=(run_on_destroy&&) -> run_on_destroy& = delete;
};
}  //namespace

TEST_CASE("synchronized_event_dispatcher: move assignment may destroy callbacks that use the dispatcher", "[synchronized_event_dispatcher]") {
	auto dispatcher = events::synchronized_event_dispatcher{};

	// Destroying this callback enqueues an event on the same dispatcher
	auto guard = std::make_shared<run_on_destroy>();
	guard->action = [&dispatcher] { dispatcher.enqueue(sync_other_event{"from destructor"}); };
	auto conn = dispatcher.connect<sync_test_event>([guard](sync_test_event const&) {});
	guard.reset();

	dispatcher = events::synchronized_event_dispatcher{};
	CHECK_FALSE(conn.connected());
	CHECK(dispatcher.queue_size<sync_other_event>() == 1);
}


// ---- Delivery order ----

namespace {
struct sync_first_event {};
struct sync_second_event {};
struct sync_third_event {};
}  //namespace

TEST_CASE("synchronized_event_dispatcher: event types are dispatched in the order they were first used", "[synchronized_event_dispatcher]") {
	auto dispatcher = events::synchronized_event_dispatcher{};
	auto order = std::string{};

	dispatcher.send(sync_third_event{});  // first use of sync_third_event, with no callbacks yet
	auto c1 = dispatcher.connect<sync_first_event>([&](sync_first_event const&) { order += '1'; });
	dispatcher.enqueue(sync_second_event{});
	auto c2 = dispatcher.connect<sync_second_event>([&](sync_second_event const&) { order += '2'; });
	auto c3 = dispatcher.connect<sync_third_event>([&](sync_third_event const&) { order += '3'; });

	dispatcher.enqueue(sync_first_event{});
	dispatcher.enqueue(sync_third_event{});
	dispatcher.dispatch();

	CHECK(order == "312");
}

TEST_CASE("synchronized_event_dispatcher: events of any type enqueued during dispatch are delivered by the next dispatch", "[synchronized_event_dispatcher][reentrancy]") {
	auto dispatcher = events::synchronized_event_dispatcher{};
	auto log = std::vector<std::string>{};

	SECTION("sync_test_event first") {
		dispatcher.enqueue(sync_test_event{0});
		dispatcher.enqueue(sync_other_event{"a"});
	}
	SECTION("sync_other_event first") {
		dispatcher.enqueue(sync_other_event{"a"});
		dispatcher.enqueue(sync_test_event{0});
	}

	// Each callback enqueues an event of the other type
	auto c1 = dispatcher.connect<sync_test_event>([&](sync_test_event const& e) {
		log.push_back("test " + std::to_string(e.value));
		if (e.value == 0) {
			dispatcher.enqueue(sync_other_event{"b"});
		}
	});
	auto c2 = dispatcher.connect<sync_other_event>([&](sync_other_event const& e) {
		log.push_back("other " + e.message);
		if (e.message == "a") {
			dispatcher.enqueue(sync_test_event{1});
		}
	});

	dispatcher.dispatch();
	CHECK(log.size() == 2);
	CHECK(dispatcher.queue_size() == 2);

	dispatcher.dispatch();
	CHECK(log.size() == 4);
	CHECK(dispatcher.queue_size() == 0);

	std::ranges::sort(log);
	CHECK(log == std::vector<std::string>{"other a", "other b", "test 0", "test 1"});
}


// ---- Exceptions ----

TEST_CASE("synchronized_event_dispatcher: events are not lost when a callback throws", "[synchronized_event_dispatcher][exceptions]") {
	auto dispatcher = events::synchronized_event_dispatcher{};
	auto received = std::vector<int>{};
	int other_count = 0;

	auto c1 = dispatcher.connect<sync_test_event>([&](sync_test_event const& e) {
		if (e.value == 2) {
			dispatcher.enqueue(sync_test_event{10});  // enqueued during the failed dispatch
			throw std::runtime_error{"callback failed"};
		}
		received.push_back(e.value);
	});
	auto c2 = dispatcher.connect<sync_other_event>([&](sync_other_event const&) { ++other_count; });

	dispatcher.enqueue(sync_test_event{1});
	dispatcher.enqueue(sync_test_event{2});
	dispatcher.enqueue(sync_test_event{3});
	dispatcher.enqueue(sync_test_event{4});
	dispatcher.enqueue(sync_other_event{"x"});

	CHECK_THROWS_AS(dispatcher.dispatch(), std::runtime_error);
	CHECK(received == std::vector<int>{1});
	CHECK(other_count == 0);

	// The event whose callback threw is discarded. The undelivered events stay queued, in front of newer ones.
	CHECK(dispatcher.queue_size<sync_test_event>() == 3);
	CHECK(dispatcher.queue_size<sync_other_event>() == 1);
	dispatcher.enqueue(sync_test_event{5});

	dispatcher.dispatch();
	CHECK(received == std::vector<int>{1, 3, 4, 10, 5});
	CHECK(other_count == 1);
	CHECK(dispatcher.queue_size() == 0);
}


// ---- Clearing ----

TEST_CASE("synchronized_event_dispatcher: clear", "[synchronized_event_dispatcher]") {
	auto dispatcher = events::synchronized_event_dispatcher{};
	int count = 0;
	auto conn = dispatcher.connect<sync_test_event>([&](sync_test_event const&) { ++count; });

	dispatcher.clear<sync_test_event>();   // nothing to clear
	dispatcher.clear<sync_third_event>();  // unknown type

	dispatcher.enqueue(sync_test_event{1});
	dispatcher.enqueue(sync_test_event{2});
	dispatcher.enqueue(sync_other_event{"x"});

	dispatcher.clear<sync_test_event>();
	CHECK(dispatcher.queue_size<sync_test_event>() == 0);
	CHECK(dispatcher.queue_size<sync_other_event>() == 1);

	dispatcher.enqueue(sync_test_event{3});
	dispatcher.clear();
	CHECK(dispatcher.queue_size() == 0);

	dispatcher.dispatch();
	CHECK(count == 0);
	CHECK(conn.connected());
}

TEST_CASE("synchronized_event_dispatcher: clear during dispatch discards the undelivered events", "[synchronized_event_dispatcher][reentrancy]") {
	auto dispatcher = events::synchronized_event_dispatcher{};
	auto received = std::vector<int>{};
	int other_count = 0;

	auto c1 = dispatcher.connect<sync_test_event>([&](sync_test_event const& e) {
		received.push_back(e.value);
		if (e.value == 2) {
			dispatcher.clear();
		}
	});
	auto c2 = dispatcher.connect<sync_other_event>([&](sync_other_event const&) { ++other_count; });

	dispatcher.enqueue(sync_test_event{1});
	dispatcher.enqueue(sync_test_event{2});
	dispatcher.enqueue(sync_test_event{3});
	dispatcher.enqueue(sync_other_event{"x"});
	dispatcher.dispatch();

	CHECK(received == std::vector<int>{1, 2});
	CHECK(other_count == 0);
	CHECK(dispatcher.queue_size() == 0);
}


// ---- More reentrancy ----

TEST_CASE("synchronized_event_dispatcher: nested dispatch delivers each event once", "[synchronized_event_dispatcher][reentrancy]") {
	auto dispatcher = events::synchronized_event_dispatcher{};
	auto received = std::vector<int>{};
	auto other_received = std::vector<std::string>{};

	auto c1 = dispatcher.connect<sync_test_event>([&](sync_test_event const& e) {
		received.push_back(e.value);
		if (e.value == 1) {
			dispatcher.dispatch();  // delivers sync_other_event{"a"}, which the outer dispatch hasn't reached yet
		}
	});
	auto c2 = dispatcher.connect<sync_other_event>([&](sync_other_event const& e) { other_received.push_back(e.message); });

	dispatcher.enqueue(sync_test_event{1});
	dispatcher.enqueue(sync_test_event{2});
	dispatcher.enqueue(sync_other_event{"a"});
	dispatcher.dispatch();

	CHECK(received == std::vector<int>{1, 2});
	CHECK(other_received == std::vector<std::string>{"a"});
	CHECK(dispatcher.queue_size() == 0);
}

TEST_CASE("synchronized_event_dispatcher: destroying the dispatcher during dispatch", "[synchronized_event_dispatcher][reentrancy]") {
	auto dispatcher = std::make_unique<events::synchronized_event_dispatcher>();
	auto* const raw = dispatcher.get();
	auto received = std::vector<int>{};
	int other_count = 0;

	auto c1 = dispatcher->connect<sync_test_event>([&](sync_test_event const& e) {
		received.push_back(e.value);
		dispatcher.reset();
	});
	auto c2 = dispatcher->connect<sync_other_event>([&](sync_other_event const&) { ++other_count; });

	dispatcher->enqueue(sync_test_event{1});
	dispatcher->enqueue(sync_test_event{2});
	dispatcher->enqueue(sync_other_event{"x"});
	raw->dispatch();

	CHECK(received == std::vector<int>{1});
	CHECK(other_count == 0);
	CHECK_FALSE(c1.connected());
	CHECK_FALSE(c2.connected());
}

TEST_CASE("synchronized_event_dispatcher: assigning to the dispatcher during dispatch", "[synchronized_event_dispatcher][reentrancy]") {
	auto dispatcher = events::synchronized_event_dispatcher{};
	auto received = std::vector<int>{};

	auto conn = dispatcher.connect<sync_test_event>([&](sync_test_event const& e) {
		received.push_back(e.value);
		dispatcher = events::synchronized_event_dispatcher{};
		dispatcher.enqueue(sync_test_event{100});  // goes to the new, empty state
	});

	dispatcher.enqueue(sync_test_event{1});
	dispatcher.enqueue(sync_test_event{2});
	dispatcher.dispatch();

	CHECK(received == std::vector<int>{1});
	CHECK_FALSE(conn.connected());
	CHECK(dispatcher.queue_size() == 1);
}


// ---- More thread safety ----

TEST_CASE("synchronized_event_dispatcher: concurrent dispatches deliver each event exactly once", "[synchronized_event_dispatcher][threaded]") {
	auto dispatcher = events::synchronized_event_dispatcher{};

	constexpr int num_producers = 3;
	constexpr int events_per_producer = 3'000;
	constexpr int total = num_producers * events_per_producer;
	constexpr int num_dispatchers = 3;

	auto seen = std::vector<std::atomic<int>>(total);
	std::atomic<int> delivered{0};

	auto const record = [&](int id) {
		seen[static_cast<std::size_t>(id)].fetch_add(1, std::memory_order_relaxed);
		delivered.fetch_add(1, std::memory_order_relaxed);
	};
	auto c1 = dispatcher.connect<sync_test_event>([&](sync_test_event const& e) { record(e.value); });
	auto c2 = dispatcher.connect<sync_id_event>([&](sync_id_event const& e) { record(e.id); });

	auto threads = std::vector<std::thread>{};
	for (int p = 0; p < num_producers; ++p) {
		threads.emplace_back([&dispatcher, p] {
			for (int i = 0; i < events_per_producer; ++i) {
				auto const id = (p * events_per_producer) + i;
				if ((id % 2) == 0) {
					dispatcher.enqueue(sync_test_event{id});
				}
				else {
					dispatcher.enqueue(sync_id_event{id});
				}
			}
		});
	}
	for (int d = 0; d < num_dispatchers; ++d) {
		threads.emplace_back([&dispatcher, &delivered] {
			while (delivered.load(std::memory_order_relaxed) < total) {
				dispatcher.dispatch();
				std::this_thread::yield();
			}
		});
	}

	for (auto& t : threads) {
		t.join();
	}

	CHECK(delivered.load() == total);
	CHECK(std::ranges::all_of(seen, [](std::atomic<int> const& count) { return count.load() == 1; }));
	CHECK(dispatcher.queue_size() == 0);
}

TEST_CASE("synchronized_event_dispatcher: assignment while other threads use the dispatcher", "[synchronized_event_dispatcher][threaded]") {
	// Mostly useful under ASan and TSan: other threads may still hold the per-type dispatchers of a replaced state
	auto dispatcher = events::synchronized_event_dispatcher{};
	std::atomic<bool> stop{false};
	std::atomic<int> iterations{0};

	auto workers = std::vector<std::thread>{};
	for (int t = 0; t < 3; ++t) {
		workers.emplace_back([&] {
			while (!stop.load(std::memory_order_relaxed)) {
				auto const conn = events::scoped_connection{dispatcher.connect<sync_test_event>([](sync_test_event const&) {})};
				dispatcher.enqueue(sync_test_event{1});
				dispatcher.enqueue(sync_other_event{"x"});
				dispatcher.dispatch();
				dispatcher.send(sync_test_event{2});
				[[maybe_unused]] auto const size = dispatcher.queue_size();
				iterations.fetch_add(1, std::memory_order_relaxed);
			}
		});
	}

	for (int i = 0; i < 200 || iterations.load(std::memory_order_relaxed) < 100; ++i) {
		dispatcher = events::synchronized_event_dispatcher{};
		std::this_thread::yield();
	}

	stop.store(true, std::memory_order_relaxed);
	for (auto& t : workers) {
		t.join();
	}

	// Still fully usable afterwards
	dispatcher = events::synchronized_event_dispatcher{};
	int received = 0;
	auto conn = dispatcher.connect<sync_test_event>([&](sync_test_event const& e) { received += e.value; });
	dispatcher.enqueue(sync_test_event{5});
	dispatcher.dispatch();
	CHECK(received == 5);
}
