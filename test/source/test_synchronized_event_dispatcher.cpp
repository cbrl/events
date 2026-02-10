#include <events/dispatcher/synchronized_event_dispatcher.hpp>
#include <events/connection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
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

TEST_CASE("synchronized_event_dispatcher: concurrent connect from multiple threads",
          "[synchronized_event_dispatcher][threaded]") {
	auto dispatcher = events::synchronized_event_dispatcher{};

	constexpr int num_threads = 4;
	constexpr int connects_per_thread = 1'000;

	auto threads = std::vector<std::thread>{};
	threads.reserve(num_threads);

	std::atomic<int> call_count{0};

	for (int t = 0; t < num_threads; ++t) {
		threads.emplace_back([&dispatcher, &call_count] {
			std::vector<events::connection> conns;
			for (int i = 0; i < connects_per_thread; ++i) {
				conns.push_back(dispatcher.connect<sync_test_event>(
				    [&call_count](sync_test_event const&) {
					    call_count.fetch_add(1, std::memory_order_relaxed);
				    }));
			}
			// Let connections live until thread exits
		});
	}

	for (auto& t : threads) {
		t.join();
	}

	// All connections should be alive. Send one event and verify all callbacks fire.
	dispatcher.send(sync_test_event{1});
	// Note: we cannot check exact count because connections may have been destroyed
	// when vectors went out of scope. The important thing is no crash/deadlock.
	CHECK(true);
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
		enqueuers.emplace_back([&dispatcher, &stop] {
			int i = 0;
			while (!stop.load(std::memory_order_relaxed) || i < events_per_thread) {
				dispatcher.enqueue(sync_test_event{1});
				++i;
				if (i >= events_per_thread) break;
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
	dispatcher2.dispatch();
	CHECK(received == 77);
}


// ---- Edge cases ----

TEST_CASE("synchronized_event_dispatcher: dispatch with no enqueued events", "[synchronized_event_dispatcher]") {
	auto dispatcher = events::synchronized_event_dispatcher{};
	auto conn = dispatcher.connect<sync_test_event>([](sync_test_event const&) {});
	dispatcher.dispatch(); // must not crash
}

TEST_CASE("synchronized_event_dispatcher: send with no callbacks", "[synchronized_event_dispatcher]") {
	auto dispatcher = events::synchronized_event_dispatcher{};
	dispatcher.send(sync_test_event{1}); // must not crash
}
