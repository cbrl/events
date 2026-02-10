#include <events/dispatcher/event_dispatcher.hpp>
#include <events/connection.hpp>

#include <catch2/catch_test_macros.hpp>

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
	dispatcher2.dispatch();
	CHECK(received == 99);
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
	dispatcher.dispatch(); // must not crash
}

TEST_CASE("event_dispatcher: dispatch with no connected callbacks", "[event_dispatcher]") {
	auto dispatcher = events::event_dispatcher{};
	dispatcher.enqueue(test_event{1});
	dispatcher.dispatch(); // must not crash
}

TEST_CASE("event_dispatcher: send with no connected callbacks", "[event_dispatcher]") {
	auto dispatcher = events::event_dispatcher{};
	dispatcher.send(test_event{1}); // must not crash
}
