#include <events/signal_handler/signal_handler.hpp>
#include <events/connection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>


// ---- Basic functionality ----

TEST_CASE("signal_handler: starts with zero size", "[signal_handler]") {
	auto sigh = events::signal_handler<void()>{};
	CHECK(sigh.size() == 0);
}

TEST_CASE("signal_handler: connect increases size", "[signal_handler]") {
	auto sigh = events::signal_handler<void()>{};
	auto c1 = sigh.connect([] {});
	CHECK(sigh.size() == 1);
	auto c2 = sigh.connect([] {});
	CHECK(sigh.size() == 2);
}

TEST_CASE("signal_handler: disconnect decreases size", "[signal_handler]") {
	auto sigh = events::signal_handler<void()>{};
	auto c = sigh.connect([] {});
	REQUIRE(sigh.size() == 1);
	c.disconnect();
	CHECK(sigh.size() == 0);
}

TEST_CASE("signal_handler: disconnect_all clears all callbacks", "[signal_handler]") {
	auto sigh = events::signal_handler<void()>{};
	auto c1 = sigh.connect([] {});
	auto c2 = sigh.connect([] {});
	auto c3 = sigh.connect([] {});
	REQUIRE(sigh.size() == 3);
	sigh.disconnect_all();
	CHECK(sigh.size() == 0);
}

TEST_CASE("signal_handler: publish invokes all callbacks", "[signal_handler]") {
	auto sigh = events::signal_handler<void(int&)>{};
	auto c1 = sigh.connect([](int& n) { n += 1; });
	auto c2 = sigh.connect([](int& n) { n += 10; });
	auto c3 = sigh.connect([](int& n) { n += 100; });

	int value = 0;
	sigh.publish(value);
	CHECK(value == 111);
}

TEST_CASE("signal_handler: publish with no callbacks is safe", "[signal_handler]") {
	auto sigh = events::signal_handler<void(int)>{};
	sigh.publish(42); // must not crash
}

TEST_CASE("signal_handler: publish forwards arguments", "[signal_handler]") {
	auto sigh = events::signal_handler<void(std::string const&, int)>{};
	std::string captured_str;
	int captured_int = 0;

	auto conn = sigh.connect([&](std::string const& s, int n) {
		captured_str = s;
		captured_int = n;
	});

	sigh.publish("hello", 42);
	CHECK(captured_str == "hello");
	CHECK(captured_int == 42);
}


// ---- Return values ----

TEST_CASE("signal_handler: publish with return value collects results", "[signal_handler]") {
	auto sigh = events::signal_handler<int(int)>{};
	auto c1 = sigh.connect([](int n) { return n * 2; });
	auto c2 = sigh.connect([](int n) { return n * 10; });

	auto results = sigh.publish(5);
	REQUIRE(results.size() == 2);
	CHECK(results[0] == 10);
	CHECK(results[1] == 50);
}

TEST_CASE("signal_handler: publish with return value and no callbacks returns empty", "[signal_handler]") {
	auto sigh = events::signal_handler<int()>{};
	auto results = sigh.publish();
	CHECK(results.empty());
}

TEST_CASE("signal_handler: publish_range returns lazy range", "[signal_handler]") {
	auto sigh = events::signal_handler<int(int)>{};
	auto c1 = sigh.connect([](int n) { return n + 1; });
	auto c2 = sigh.connect([](int n) { return n + 2; });
	auto c3 = sigh.connect([](int n) { return n + 3; });

	auto range = sigh.publish_range(10);
	auto results = std::vector<int>{};
	for (auto val : range) {
		results.push_back(val);
	}
	REQUIRE(results.size() == 3);
	CHECK(results[0] == 11);
	CHECK(results[1] == 12);
	CHECK(results[2] == 13);
}


// ---- Copy and move semantics ----

TEST_CASE("signal_handler: copy constructor duplicates callbacks", "[signal_handler]") {
	auto sigh1 = events::signal_handler<void(int&)>{};
	auto conn = sigh1.connect([](int& n) { ++n; });

	auto sigh2 = sigh1; // copy

	int value = 0;
	sigh2.publish(value);
	CHECK(value == 1); // copy should have the callback
}

TEST_CASE("signal_handler: copy does not share connections", "[signal_handler]") {
	auto sigh1 = events::signal_handler<void(int&)>{};
	auto conn = sigh1.connect([](int& n) { ++n; });

	auto sigh2 = sigh1;

	// Disconnecting from original should not affect copy
	conn.disconnect();

	int value = 0;
	sigh2.publish(value);
	CHECK(value == 1);
}

TEST_CASE("signal_handler: move constructor transfers callbacks", "[signal_handler]") {
	auto sigh1 = events::signal_handler<void(int&)>{};
	auto conn = sigh1.connect([](int& n) { ++n; });

	auto sigh2 = std::move(sigh1);

	int value = 0;
	sigh2.publish(value);
	CHECK(value == 1);
}


// ---- Reentrancy ----

TEST_CASE("signal_handler: disconnect during publish is safe", "[signal_handler][reentrancy]") {
	// plf::colony iteration is stable, so erasing during iteration may or may not
	// skip elements, but it must not crash.
	auto sigh = events::signal_handler<void()>{};

	events::connection self_conn;
	int call_count = 0;

	self_conn = sigh.connect([&] {
		++call_count;
		self_conn.disconnect();
	});

	auto other_conn = sigh.connect([&] {
		++call_count;
	});

	sigh.publish(); // should not crash
	// The self-disconnecting callback was invoked at least once
	CHECK(call_count >= 1);
}

TEST_CASE("signal_handler: connect during publish is safe", "[signal_handler][reentrancy]") {
	auto sigh = events::signal_handler<void()>{};

	int outer_count = 0;
	int inner_count = 0;
	events::connection inner_conn;

	auto outer_conn = sigh.connect([&] {
		++outer_count;
		if (outer_count == 1) {
			inner_conn = sigh.connect([&] { ++inner_count; });
		}
	});

	sigh.publish();
	// The outer callback was invoked
	CHECK(outer_count >= 1);

	// Whether the inner callback is invoked during the same publish depends on colony behavior.
	// But a second publish should definitely invoke both.
	outer_count = 0;
	inner_count = 0;
	sigh.publish();
	CHECK(outer_count == 1);
	CHECK(inner_count == 1);
}


// ---- Edge cases with various types ----

struct heavy_event {
	int id;
	std::string data;
	std::vector<double> values;
};

TEST_CASE("signal_handler: works with complex argument types", "[signal_handler]") {
	auto sigh = events::signal_handler<void(heavy_event const&)>{};

	heavy_event captured{};
	auto conn = sigh.connect([&](heavy_event const& ev) {
		captured = ev;
	});

	heavy_event event{42, "test", {1.0, 2.0, 3.0}};
	sigh.publish(event);

	CHECK(captured.id == 42);
	CHECK(captured.data == "test");
	REQUIRE(captured.values.size() == 3);
	CHECK(captured.values[0] == 1.0);
}

TEST_CASE("signal_handler: works with multiple argument types", "[signal_handler]") {
	auto sigh = events::signal_handler<void(int, double, std::string)>{};

	int ci = 0;
	double cd = 0;
	std::string cs;

	auto conn = sigh.connect([&](int i, double d, std::string s) {
		ci = i;
		cd = d;
		cs = std::move(s);
	});

	sigh.publish(1, 2.5, std::string("hello"));
	CHECK(ci == 1);
	CHECK(cd == 2.5);
	CHECK(cs == "hello");
}

TEST_CASE("signal_handler: return value with string", "[signal_handler]") {
	auto sigh = events::signal_handler<std::string(int)>{};
	auto c1 = sigh.connect([](int n) { return std::to_string(n); });
	auto c2 = sigh.connect([](int n) { return std::to_string(n * 2); });

	auto results = sigh.publish(7);
	REQUIRE(results.size() == 2);
	CHECK(results[0] == "7");
	CHECK(results[1] == "14");
}


// ---- Ordering ----

TEST_CASE("signal_handler: callbacks are invoked in connection order", "[signal_handler]") {
	auto sigh = events::signal_handler<void(std::vector<int>&)>{};

	auto c1 = sigh.connect([](std::vector<int>& v) { v.push_back(1); });
	auto c2 = sigh.connect([](std::vector<int>& v) { v.push_back(2); });
	auto c3 = sigh.connect([](std::vector<int>& v) { v.push_back(3); });

	std::vector<int> order;
	sigh.publish(order);
	REQUIRE(order.size() == 3);
	CHECK(order[0] == 1);
	CHECK(order[1] == 2);
	CHECK(order[2] == 3);
}


// ---- Stress ----

TEST_CASE("signal_handler: many connects and disconnects", "[signal_handler]") {
	auto sigh = events::signal_handler<void()>{};

	std::vector<events::connection> conns;
	for (int i = 0; i < 1000; ++i) {
		conns.push_back(sigh.connect([] {}));
	}
	CHECK(sigh.size() == 1000);

	for (auto& c : conns) {
		c.disconnect();
	}
	CHECK(sigh.size() == 0);
}

TEST_CASE("signal_handler: interleaved connect and disconnect", "[signal_handler]") {
	auto sigh = events::signal_handler<void()>{};

	// Connect 5, disconnect 3, connect 5, disconnect 3, ...
	std::vector<events::connection> conns;
	for (int round = 0; round < 10; ++round) {
		for (int i = 0; i < 5; ++i) {
			conns.push_back(sigh.connect([] {}));
		}
		for (int i = 0; i < 3 && !conns.empty(); ++i) {
			conns.back().disconnect();
			conns.pop_back();
		}
	}

	// 10 rounds: each round adds 5, removes 3 => net +2 per round => 20
	CHECK(sigh.size() == 20);
}
