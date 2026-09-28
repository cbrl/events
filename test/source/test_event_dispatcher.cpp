#include <events/dispatcher/event_dispatcher.hpp>
#include <events/connection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <vector>


struct test_event {
	int value;
};

struct other_event {
	std::string message;
};


// ---- Basic functionality ----

TEST_CASE("event_dispatcher: connect and send", "[event_dispatcher]") {
	auto dispatcher = events::event_dispatcher{};
	int received = 0;

	auto conn = dispatcher.connect<test_event>([&](test_event const& e) {
		received = e.value;
	});

	dispatcher.send(test_event{42});
	CHECK(received == 42);
}

TEST_CASE("event_dispatcher: connect and enqueue then dispatch", "[event_dispatcher]") {
	auto dispatcher = events::event_dispatcher{};
	std::vector<int> received;

	auto conn = dispatcher.connect<test_event>([&](test_event const& e) {
		received.push_back(e.value);
	});

	dispatcher.enqueue(test_event{1});
	dispatcher.enqueue(test_event{2});
	dispatcher.enqueue(test_event{3});

	CHECK(received.empty()); // not dispatched yet

	dispatcher.dispatch();
	REQUIRE(received.size() == 3);
	CHECK(received[0] == 1);
	CHECK(received[1] == 2);
	CHECK(received[2] == 3);
}

TEST_CASE("event_dispatcher: enqueue with in-place construction", "[event_dispatcher]") {
	auto dispatcher = events::event_dispatcher{};
	int received = 0;

	auto conn = dispatcher.connect<test_event>([&](test_event const& e) {
		received = e.value;
	});

	dispatcher.enqueue<test_event>(99);
	dispatcher.dispatch();
	CHECK(received == 99);
}

TEST_CASE("event_dispatcher: enqueue range", "[event_dispatcher]") {
	auto dispatcher = events::event_dispatcher{};
	int total = 0;

	auto conn = dispatcher.connect<test_event>([&](test_event const& e) {
		total += e.value;
	});

	auto events_vec = std::vector<test_event>{{1}, {2}, {3}, {4}, {5}};
	dispatcher.enqueue<test_event>(events_vec);

	dispatcher.dispatch();
	CHECK(total == 15);
}

TEST_CASE("event_dispatcher: send range", "[event_dispatcher]") {
	auto dispatcher = events::event_dispatcher{};
	int total = 0;

	auto conn = dispatcher.connect<test_event>([&](test_event const& e) {
		total += e.value;
	});

	auto events_vec = std::vector<test_event>{{10}, {20}, {30}};
	dispatcher.send<test_event>(events_vec);
	CHECK(total == 60);
}


struct int_convertible_event {
	int_convertible_event(int v) : value(v) { //NOLINT(google-explicit-constructor,hicpp-explicit-conversions)
	}
	int value;
};

TEST_CASE("event_dispatcher: send and enqueue construct events the same way", "[event_dispatcher]") {
	auto dispatcher = events::event_dispatcher{};
	auto sizes = std::vector<std::size_t>{};

	auto conn = dispatcher.connect<std::vector<int>>([&](std::vector<int> const& v) { sizes.push_back(v.size()); });

	// vector<int>(3u, 7) has three elements. Brace-initialization would pick the initializer_list constructor instead
	// (and reject 3u as a narrowing conversion).
	dispatcher.send<std::vector<int>>(3u, 7);
	dispatcher.enqueue<std::vector<int>>(3u, 7);
	dispatcher.dispatch();

	CHECK(sizes == std::vector<std::size_t>{3, 3});
}

TEST_CASE("event_dispatcher: enqueue and send accept non-common ranges", "[event_dispatcher]") {
	auto dispatcher = events::event_dispatcher{};
	int total = 0;

	auto conn = dispatcher.connect<test_event>([&](test_event const& e) {
		total += e.value;
	});

	auto source = std::vector<test_event>{{1}, {2}, {3}, {100}};
	auto small = source | std::views::take_while([](test_event const& e) { return e.value < 10; });

	dispatcher.enqueue<test_event>(small);
	dispatcher.dispatch();
	CHECK(total == 6);

	dispatcher.send<test_event>(small);
	CHECK(total == 12);
}

TEST_CASE("event_dispatcher: range elements are converted to the event type", "[event_dispatcher]") {
	auto dispatcher = events::event_dispatcher{};
	int total = 0;

	auto conn = dispatcher.connect<int_convertible_event>([&](int_convertible_event const& e) {
		total += e.value;
	});

	auto values = std::vector<int>{1, 2, 3};
	dispatcher.enqueue<int_convertible_event>(values);
	dispatcher.dispatch();
	CHECK(total == 6);

	dispatcher.send<int_convertible_event>(std::views::iota(1, 4));
	CHECK(total == 12);
}


// ---- Multiple event types ----

TEST_CASE("event_dispatcher: multiple event types", "[event_dispatcher]") {
	auto dispatcher = events::event_dispatcher{};
	int int_received = 0;
	std::string str_received;

	auto c1 = dispatcher.connect<test_event>([&](test_event const& e) {
		int_received = e.value;
	});

	auto c2 = dispatcher.connect<other_event>([&](other_event const& e) {
		str_received = e.message;
	});

	dispatcher.send(test_event{42});
	dispatcher.send(other_event{"hello"});

	CHECK(int_received == 42);
	CHECK(str_received == "hello");
}

TEST_CASE("event_dispatcher: dispatch only sends matching types", "[event_dispatcher]") {
	auto dispatcher = events::event_dispatcher{};
	int test_count = 0;
	int other_count = 0;

	auto c1 = dispatcher.connect<test_event>([&](test_event const&) { ++test_count; });
	auto c2 = dispatcher.connect<other_event>([&](other_event const&) { ++other_count; });

	dispatcher.enqueue(test_event{1});
	dispatcher.enqueue(test_event{2});
	dispatcher.enqueue(other_event{"a"});

	dispatcher.dispatch();
	CHECK(test_count == 2);
	CHECK(other_count == 1);
}


// ---- Queue management ----

TEST_CASE("event_dispatcher: queue_size", "[event_dispatcher]") {
	auto dispatcher = events::event_dispatcher{};
	auto conn = dispatcher.connect<test_event>([](test_event const&) {});

	CHECK(dispatcher.queue_size() == 0);
	CHECK(dispatcher.queue_size<test_event>() == 0);

	dispatcher.enqueue(test_event{1});
	dispatcher.enqueue(test_event{2});
	CHECK(dispatcher.queue_size() == 2);
	CHECK(dispatcher.queue_size<test_event>() == 2);
	CHECK(dispatcher.queue_size<other_event>() == 0);

	dispatcher.dispatch();
	CHECK(dispatcher.queue_size() == 0);
}

TEST_CASE("event_dispatcher: dispatch clears the queue", "[event_dispatcher]") {
	auto dispatcher = events::event_dispatcher{};
	auto conn = dispatcher.connect<test_event>([](test_event const&) {});

	dispatcher.enqueue(test_event{1});
	dispatcher.dispatch();
	CHECK(dispatcher.queue_size() == 0);

	// Double dispatch should not re-process events
	int count = 0;
	auto c2 = dispatcher.connect<test_event>([&](test_event const&) { ++count; });
	dispatcher.dispatch();
	CHECK(count == 0);
}


// ---- Connection management ----

TEST_CASE("event_dispatcher: disconnect removes callback", "[event_dispatcher]") {
	auto dispatcher = events::event_dispatcher{};
	int count = 0;

	auto conn = dispatcher.connect<test_event>([&](test_event const&) { ++count; });
	conn.disconnect();

	dispatcher.send(test_event{1});
	CHECK(count == 0);
}

TEST_CASE("event_dispatcher: multiple callbacks for same event type", "[event_dispatcher]") {
	auto dispatcher = events::event_dispatcher{};
	int total = 0;

	auto c1 = dispatcher.connect<test_event>([&](test_event const& e) { total += e.value; });
	auto c2 = dispatcher.connect<test_event>([&](test_event const& e) { total += e.value * 10; });

	dispatcher.send(test_event{5});
	CHECK(total == 55); // 5 + 50
}


// ---- Move semantics ----

TEST_CASE("event_dispatcher: move preserves connections and queue", "[event_dispatcher]") {
	auto dispatcher1 = events::event_dispatcher{};
	int received = 0;

	auto conn = dispatcher1.connect<test_event>([&](test_event const& e) {
		received = e.value;
	});

	dispatcher1.enqueue(test_event{99});

	auto dispatcher2 = std::move(dispatcher1);
	CHECK(dispatcher1.queue_size() == 0); //NOLINT(bugprone-use-after-move,hicpp-invalid-access-moved)
	CHECK(dispatcher2.queue_size() == 1);

	dispatcher2.dispatch();
	CHECK(received == 99);
	CHECK(conn.connected());
}

TEST_CASE("event_dispatcher: a moved-from dispatcher is empty and can be reused", "[event_dispatcher]") {
	auto dispatcher1 = events::event_dispatcher{};
	int received1 = 0;
	int received2 = 0;

	auto conn1 = dispatcher1.connect<test_event>([&](test_event const& e) { received1 += e.value; });
	dispatcher1.enqueue(test_event{1});

	auto dispatcher2 = events::event_dispatcher{};
	dispatcher2 = std::move(dispatcher1);

	// dispatcher1 no longer shares anything with dispatcher2
	auto conn2 = dispatcher1.connect<test_event>([&](test_event const& e) { received2 += e.value; }); //NOLINT(bugprone-use-after-move,hicpp-invalid-access-moved)
	dispatcher1.enqueue(test_event{10});
	CHECK(dispatcher1.queue_size() == 1);
	CHECK(dispatcher2.queue_size() == 1);

	dispatcher1.dispatch();
	CHECK(received1 == 0);
	CHECK(received2 == 10);

	dispatcher2.dispatch();
	CHECK(received1 == 1);
	CHECK(received2 == 10);
}

TEST_CASE("event_dispatcher: move assignment disconnects the previous callbacks and discards their events", "[event_dispatcher]") {
	auto dispatcher = events::event_dispatcher{};
	int received = 0;

	auto conn = dispatcher.connect<test_event>([&](test_event const&) { ++received; });
	dispatcher.enqueue(test_event{1});

	dispatcher = events::event_dispatcher{};
	CHECK_FALSE(conn.connected());
	CHECK(dispatcher.queue_size() == 0);

	dispatcher.enqueue(test_event{1});
	dispatcher.dispatch();
	CHECK(received == 0);
}


// ---- Reentrancy ----

TEST_CASE("event_dispatcher: enqueue during dispatch", "[event_dispatcher][reentrancy]") {
	auto dispatcher = events::event_dispatcher{};
	int dispatch_count = 0;

	auto conn = dispatcher.connect<test_event>([&](test_event const& e) {
		++dispatch_count;
		if (e.value < 3) {
			dispatcher.enqueue(test_event{e.value + 1});
		}
	});

	dispatcher.enqueue(test_event{1});
	dispatcher.dispatch();

	// The first dispatch processes event{1}, which enqueues event{2}
	CHECK(dispatch_count == 1);
	CHECK(dispatcher.queue_size<test_event>() == 1);

	// Second dispatch processes event{2}, which enqueues event{3}
	dispatcher.dispatch();
	CHECK(dispatch_count == 2);

	// Third dispatch processes event{3}, which does not enqueue (value >= 3)
	dispatcher.dispatch();
	CHECK(dispatch_count == 3);
	CHECK(dispatcher.queue_size<test_event>() == 0);
}

TEST_CASE("event_dispatcher: send during dispatch", "[event_dispatcher][reentrancy]") {
	auto dispatcher = events::event_dispatcher{};
	std::vector<int> received;

	auto conn = dispatcher.connect<test_event>([&](test_event const& e) {
		received.push_back(e.value);
		if (e.value == 1) {
			dispatcher.send(test_event{100}); // immediate send during dispatch
		}
	});

	dispatcher.enqueue(test_event{1});
	dispatcher.enqueue(test_event{2});
	dispatcher.dispatch();

	// Expected order: 1, 100 (sent during 1's callback), 2
	REQUIRE(received.size() == 3);
	CHECK(received[0] == 1);
	CHECK(received[1] == 100);
	CHECK(received[2] == 2);
}

TEST_CASE("event_dispatcher: connect new event type during dispatch", "[event_dispatcher][reentrancy]") {
	auto dispatcher = events::event_dispatcher{};
	int test_count = 0;
	int other_count = 0;
	events::connection other_conn;

	auto conn = dispatcher.connect<test_event>([&](test_event const&) {
		++test_count;
		if (test_count == 1) {
			other_conn = dispatcher.connect<other_event>([&](other_event const&) {
				++other_count;
			});
		}
	});

	dispatcher.enqueue(test_event{1});
	dispatcher.dispatch();
	CHECK(test_count == 1);

	// Now the other_event handler exists, we can send to it
	dispatcher.send(other_event{"hello"});
	CHECK(other_count == 1);
}


// ---- Edge cases ----

TEST_CASE("event_dispatcher: dispatch with no enqueued events is safe", "[event_dispatcher]") {
	auto dispatcher = events::event_dispatcher{};
	auto conn = dispatcher.connect<test_event>([](test_event const&) {});
	dispatcher.dispatch();
	CHECK(dispatcher.queue_size() == 0);
}

TEST_CASE("event_dispatcher: dispatch with no connected callbacks", "[event_dispatcher]") {
	auto dispatcher = events::event_dispatcher{};
	dispatcher.enqueue(test_event{1});
	dispatcher.dispatch();
	CHECK(dispatcher.queue_size() == 0);
}

TEST_CASE("event_dispatcher: send with no connected callbacks", "[event_dispatcher]") {
	auto dispatcher = events::event_dispatcher{};
	dispatcher.send(test_event{1});
	CHECK(dispatcher.queue_size() == 0);
}


// ---- Lifetime ----

TEST_CASE("event_dispatcher: connection may outlive the dispatcher", "[event_dispatcher]") {
	auto conn = events::connection{};
	auto scoped = events::scoped_connection{};

	{
		auto dispatcher = events::event_dispatcher{};
		conn = dispatcher.connect<test_event>([](test_event const&) {});
		scoped = dispatcher.connect<test_event>([](test_event const&) {});
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

TEST_CASE("event_dispatcher: move assignment may destroy callbacks that use the dispatcher", "[event_dispatcher]") {
	auto dispatcher = events::event_dispatcher{};

	// Destroying this callback enqueues an event on the same dispatcher
	auto guard = std::make_shared<run_on_destroy>();
	guard->action = [&dispatcher] { dispatcher.enqueue(other_event{"from destructor"}); };
	auto conn = dispatcher.connect<test_event>([guard](test_event const&) {});
	guard.reset();

	dispatcher = events::event_dispatcher{};
	CHECK_FALSE(conn.connected());
	CHECK(dispatcher.queue_size<other_event>() == 1);
}


// ---- Delivery order ----

namespace {
struct first_event {};
struct second_event {};
struct third_event {};
}  //namespace

TEST_CASE("event_dispatcher: event types are dispatched in the order they were first used", "[event_dispatcher]") {
	auto dispatcher = events::event_dispatcher{};
	auto order = std::string{};

	dispatcher.send(third_event{});  // first use of third_event, with no callbacks yet
	auto c1 = dispatcher.connect<first_event>([&](first_event const&) { order += '1'; });
	dispatcher.enqueue(second_event{});
	auto c2 = dispatcher.connect<second_event>([&](second_event const&) { order += '2'; });
	auto c3 = dispatcher.connect<third_event>([&](third_event const&) { order += '3'; });

	dispatcher.enqueue(first_event{});
	dispatcher.enqueue(third_event{});
	dispatcher.dispatch();

	CHECK(order == "312");
}

TEST_CASE("event_dispatcher: events of any type enqueued during dispatch are delivered by the next dispatch", "[event_dispatcher][reentrancy]") {
	auto dispatcher = events::event_dispatcher{};
	auto log = std::vector<std::string>{};

	// Both processing orders. Before, whether the new event was delivered right away depended on the order of the types.
	SECTION("test_event first") {
		dispatcher.enqueue(test_event{0});
		dispatcher.enqueue(other_event{"a"});
	}
	SECTION("other_event first") {
		dispatcher.enqueue(other_event{"a"});
		dispatcher.enqueue(test_event{0});
	}

	// Each callback enqueues an event of the other type
	auto c1 = dispatcher.connect<test_event>([&](test_event const& e) {
		log.push_back("test " + std::to_string(e.value));
		if (e.value == 0) {
			dispatcher.enqueue(other_event{"b"});
		}
	});
	auto c2 = dispatcher.connect<other_event>([&](other_event const& e) {
		log.push_back("other " + e.message);
		if (e.message == "a") {
			dispatcher.enqueue(test_event{1});
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

TEST_CASE("event_dispatcher: events are not lost when a callback throws", "[event_dispatcher][exceptions]") {
	auto dispatcher = events::event_dispatcher{};
	auto received = std::vector<int>{};
	int other_count = 0;

	auto c1 = dispatcher.connect<test_event>([&](test_event const& e) {
		if (e.value == 2) {
			dispatcher.enqueue(test_event{10});  // enqueued during the failed dispatch
			throw std::runtime_error{"callback failed"};
		}
		received.push_back(e.value);
	});
	auto c2 = dispatcher.connect<other_event>([&](other_event const&) { ++other_count; });

	dispatcher.enqueue(test_event{1});
	dispatcher.enqueue(test_event{2});
	dispatcher.enqueue(test_event{3});
	dispatcher.enqueue(test_event{4});
	dispatcher.enqueue(other_event{"x"});

	CHECK_THROWS_AS(dispatcher.dispatch(), std::runtime_error);
	CHECK(received == std::vector<int>{1});
	CHECK(other_count == 0);

	// The event whose callback threw is discarded. The undelivered events stay queued, in front of newer ones.
	CHECK(dispatcher.queue_size<test_event>() == 3);
	CHECK(dispatcher.queue_size<other_event>() == 1);
	dispatcher.enqueue(test_event{5});

	dispatcher.dispatch();
	CHECK(received == std::vector<int>{1, 3, 4, 10, 5});
	CHECK(other_count == 1);
	CHECK(dispatcher.queue_size() == 0);
}

TEST_CASE("event_dispatcher: callbacks after one that throws are not invoked for that event", "[event_dispatcher][exceptions]") {
	auto dispatcher = events::event_dispatcher{};
	auto log = std::string{};
	bool fail = true;

	auto c1 = dispatcher.connect<test_event>([&](test_event const& e) {
		log += (e.value == 0) ? "a0 " : "a1 ";
		if (fail) {
			fail = false;
			throw std::runtime_error{"callback failed"};
		}
	});
	auto c2 = dispatcher.connect<test_event>([&](test_event const& e) { log += (e.value == 0) ? "b0 " : "b1 "; });

	dispatcher.enqueue(test_event{0});
	dispatcher.enqueue(test_event{1});

	CHECK_THROWS_AS(dispatcher.dispatch(), std::runtime_error);
	CHECK(log == "a0 ");

	dispatcher.dispatch();
	CHECK(log == "a0 a1 b1 ");
}


// ---- Clearing ----

TEST_CASE("event_dispatcher: clear", "[event_dispatcher]") {
	auto dispatcher = events::event_dispatcher{};
	int count = 0;
	auto conn = dispatcher.connect<test_event>([&](test_event const&) { ++count; });

	dispatcher.clear<test_event>();   // nothing to clear
	dispatcher.clear<third_event>();  // unknown type

	dispatcher.enqueue(test_event{1});
	dispatcher.enqueue(test_event{2});
	dispatcher.enqueue(other_event{"x"});

	dispatcher.clear<test_event>();
	CHECK(dispatcher.queue_size<test_event>() == 0);
	CHECK(dispatcher.queue_size<other_event>() == 1);

	dispatcher.enqueue(test_event{3});
	dispatcher.clear();
	CHECK(dispatcher.queue_size() == 0);

	dispatcher.dispatch();
	CHECK(count == 0);
	CHECK(conn.connected());
}

TEST_CASE("event_dispatcher: clear during dispatch discards the undelivered events", "[event_dispatcher][reentrancy]") {
	auto dispatcher = events::event_dispatcher{};
	auto received = std::vector<int>{};
	int other_count = 0;

	auto c1 = dispatcher.connect<test_event>([&](test_event const& e) {
		received.push_back(e.value);
		if (e.value == 2) {
			dispatcher.clear();
		}
	});
	auto c2 = dispatcher.connect<other_event>([&](other_event const&) { ++other_count; });

	dispatcher.enqueue(test_event{1});
	dispatcher.enqueue(test_event{2});
	dispatcher.enqueue(test_event{3});
	dispatcher.enqueue(other_event{"x"});
	dispatcher.dispatch();

	CHECK(received == std::vector<int>{1, 2});
	CHECK(other_count == 0);
	CHECK(dispatcher.queue_size() == 0);
}


// ---- More reentrancy ----

TEST_CASE("event_dispatcher: nested dispatch delivers each event once", "[event_dispatcher][reentrancy]") {
	auto dispatcher = events::event_dispatcher{};
	auto received = std::vector<int>{};
	auto other_received = std::vector<std::string>{};

	auto c1 = dispatcher.connect<test_event>([&](test_event const& e) {
		received.push_back(e.value);
		if (e.value == 1) {
			dispatcher.dispatch();  // delivers other_event{"a"}, which the outer dispatch hasn't reached yet
		}
	});
	auto c2 = dispatcher.connect<other_event>([&](other_event const& e) { other_received.push_back(e.message); });

	dispatcher.enqueue(test_event{1});
	dispatcher.enqueue(test_event{2});
	dispatcher.enqueue(other_event{"a"});
	dispatcher.dispatch();

	CHECK(received == std::vector<int>{1, 2});
	CHECK(other_received == std::vector<std::string>{"a"});
	CHECK(dispatcher.queue_size() == 0);
}

TEST_CASE("event_dispatcher: destroying the dispatcher during dispatch", "[event_dispatcher][reentrancy]") {
	auto dispatcher = std::make_unique<events::event_dispatcher>();
	auto* const raw = dispatcher.get();
	auto received = std::vector<int>{};
	int other_count = 0;

	auto c1 = dispatcher->connect<test_event>([&](test_event const& e) {
		received.push_back(e.value);
		dispatcher.reset();
	});
	auto c2 = dispatcher->connect<other_event>([&](other_event const&) { ++other_count; });

	dispatcher->enqueue(test_event{1});
	dispatcher->enqueue(test_event{2});
	dispatcher->enqueue(other_event{"x"});
	raw->dispatch();

	CHECK(received == std::vector<int>{1});
	CHECK(other_count == 0);
	CHECK_FALSE(c1.connected());
	CHECK_FALSE(c2.connected());
}

TEST_CASE("event_dispatcher: assigning to the dispatcher during dispatch", "[event_dispatcher][reentrancy]") {
	auto dispatcher = events::event_dispatcher{};
	auto received = std::vector<int>{};

	auto conn = dispatcher.connect<test_event>([&](test_event const& e) {
		received.push_back(e.value);
		dispatcher = events::event_dispatcher{};
		dispatcher.enqueue(test_event{100});  // goes to the new, empty state
	});

	dispatcher.enqueue(test_event{1});
	dispatcher.enqueue(test_event{2});
	dispatcher.dispatch();

	CHECK(received == std::vector<int>{1});
	CHECK_FALSE(conn.connected());
	CHECK(dispatcher.queue_size() == 1);
}

TEST_CASE("event_dispatcher: destroying the dispatcher while sending a range", "[event_dispatcher][reentrancy]") {
	auto dispatcher = std::make_unique<events::event_dispatcher>();
	auto* const raw = dispatcher.get();
	int count = 0;

	auto conn = dispatcher->connect<test_event>([&](test_event const&) {
		++count;
		dispatcher.reset();
	});

	auto const events_vec = std::vector<test_event>{{1}, {2}, {3}};
	raw->send<test_event>(events_vec);
	CHECK(count == 1);
}
