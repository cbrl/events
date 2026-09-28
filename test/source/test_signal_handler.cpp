#include <events/signal_handler/signal_handler.hpp>
#include <events/connection.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstddef>
#include <memory>
#include <random>
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

TEST_CASE("signal_handler: publish_range passes reference arguments by reference", "[signal_handler]") {
	auto sigh = events::signal_handler<int(int&)>{};
	auto c1 = sigh.connect([](int& n) { return ++n; });
	auto c2 = sigh.connect([](int& n) { return ++n; });

	int value = 0;
	auto results = std::vector<int>{};
	for (auto val : sigh.publish_range(value)) {
		results.push_back(val);
	}

	CHECK(value == 2);
	CHECK(results == std::vector<int>{1, 2});
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

TEST_CASE("signal_handler: a callback can disconnect itself during publish", "[signal_handler][reentrancy]") {
	auto sigh = events::signal_handler<void()>{};

	events::connection self_conn;
	int self_calls = 0;
	int other_calls = 0;

	self_conn = sigh.connect([&] {
		++self_calls;
		self_conn.disconnect();
	});

	auto other_conn = sigh.connect([&] {
		++other_calls;
	});

	sigh.publish();
	CHECK(self_calls == 1);
	CHECK(other_calls == 1);
	CHECK(sigh.size() == 1);

	sigh.publish();
	CHECK(self_calls == 1);
	CHECK(other_calls == 2);
}

TEST_CASE("signal_handler: a self-disconnecting callback stays alive until it returns", "[signal_handler][reentrancy]") {
	auto sigh = events::signal_handler<void()>{};

	events::connection self_conn;
	std::size_t observed_size = 0;

	auto keep = sigh.connect([] {});

	// Large enough that std::function stores it on the heap
	self_conn = sigh.connect([&self_conn, &observed_size, payload = std::string(200, 'x')] {
		self_conn.disconnect();
		observed_size = payload.size();
	});

	sigh.publish();
	CHECK(observed_size == 200);
	CHECK(sigh.size() == 1);
}

TEST_CASE("signal_handler: connect during publish takes effect on the next publish", "[signal_handler][reentrancy]") {
	// Sweep the number of callbacks, since reallocation of the underlying storage depends on it
	for (int count = 1; count <= 64; ++count) {
		CAPTURE(count);

		auto sigh = events::signal_handler<void()>{};
		int outer_calls = 0;
		int inner_calls = 0;

		auto conns = std::vector<events::connection>{};
		for (int i = 0; i < count; ++i) {
			conns.push_back(sigh.connect([&] {
				++outer_calls;
				if (outer_calls == count) {
					conns.push_back(sigh.connect([&] { ++inner_calls; }));
				}
			}));
		}

		sigh.publish();
		CHECK(outer_calls == count);
		CHECK(inner_calls == 0);
		CHECK(sigh.size() == static_cast<std::size_t>(count) + 1);

		sigh.publish();
		CHECK(inner_calls == 1);
	}
}

TEST_CASE("signal_handler: disconnecting a later callback during publish skips it", "[signal_handler][reentrancy]") {
	auto sigh = events::signal_handler<void()>{};
	int b_calls = 0;
	events::connection conn_b;

	auto conn_a = sigh.connect([&] { conn_b.disconnect(); });
	conn_b = sigh.connect([&] { ++b_calls; });

	sigh.publish();
	CHECK(b_calls == 0);
	CHECK(sigh.size() == 1);
}

TEST_CASE("signal_handler: disconnect_all during publish stops the remaining callbacks", "[signal_handler][reentrancy]") {
	auto sigh = events::signal_handler<void()>{};
	int calls = 0;

	auto a = sigh.connect([&] {
		++calls;
		sigh.disconnect_all();
	});
	auto b = sigh.connect([&] { ++calls; });
	auto c = sigh.connect([&] { ++calls; });

	sigh.publish();
	CHECK(calls == 1);
	CHECK(sigh.size() == 0);
	CHECK_FALSE(b.connected());
}

TEST_CASE("signal_handler: destroying the handler during publish is safe", "[signal_handler][reentrancy]") {
	auto sigh = std::make_unique<events::signal_handler<void()>>();
	int calls = 0;

	auto a = sigh->connect([&] {
		++calls;
		sigh.reset();
	});
	auto b = sigh->connect([&] { ++calls; });

	sigh->publish();  // `sigh` is null after this returns
	CHECK(calls == 1);
	CHECK_FALSE(a.connected());
	CHECK_FALSE(b.connected());
}

TEST_CASE("signal_handler: nested publish doesn't see callbacks connected by the outer one", "[signal_handler][reentrancy]") {
	auto sigh = events::signal_handler<void(int)>{};
	int inner_calls = 0;
	events::connection inner_conn;

	auto outer_conn = sigh.connect([&](int depth) {
		if (depth == 0) {
			inner_conn = sigh.connect([&](int) { ++inner_calls; });
			sigh.publish(1);
		}
	});

	sigh.publish(0);
	CHECK(inner_calls == 0);

	sigh.publish(1);
	CHECK(inner_calls == 1);
}

TEST_CASE("signal_handler: a callback's destructor may disconnect other callbacks", "[signal_handler][reentrancy]") {
	auto sigh = events::signal_handler<void()>{};
	int probe_calls = 0;

	auto others = std::vector<events::connection>{};
	for (int i = 0; i < 4; ++i) {
		others.push_back(sigh.connect([] {}));
	}

	// The outer callback owns a scoped_connection to the inner one, so destroying it disconnects the inner callback
	auto inner = std::make_shared<events::scoped_connection>(sigh.connect([] {}));
	auto outer = sigh.connect([inner] {});
	inner.reset();

	outer.disconnect();
	CHECK(sigh.size() == 4);

	auto probe = sigh.connect([&] { ++probe_calls; });
	sigh.publish();
	CHECK(probe_calls == 1);

	// Also while the handler is being destroyed
	auto inner2 = std::make_shared<events::scoped_connection>(sigh.connect([] {}));
	auto outer2 = sigh.connect([inner2] {});
	inner2.reset();
	sigh.disconnect_all();
	CHECK(sigh.size() == 0);
}

TEST_CASE("signal_handler: publish_range defers changes until the range is destroyed", "[signal_handler][reentrancy]") {
	auto sigh = events::signal_handler<int()>{};
	auto c1 = sigh.connect([] { return 1; });
	auto c2 = sigh.connect([] { return 2; });

	auto results = std::vector<int>{};
	{
		auto range = sigh.publish_range();
		auto c3 = sigh.connect([] { return 3; });  // Not part of the range
		c2.disconnect();                            // Skipped by the range

		for (auto val : range) {
			results.push_back(val);
		}
	}

	CHECK(results == std::vector<int>{1});
	CHECK(sigh.publish() == std::vector<int>{1, 3});
}

TEST_CASE("signal_handler: publish_range may outlive the handler", "[signal_handler][reentrancy]") {
	auto sigh = std::make_unique<events::signal_handler<int()>>();
	auto c1 = sigh->connect([] { return 1; });

	auto results = std::vector<int>{};
	{
		auto range = sigh->publish_range();
		sigh.reset();  // Disconnects the callbacks, but the range keeps them alive until it's destroyed

		for (auto val : range) {
			results.push_back(val);
		}
	}

	CHECK(results.empty());
	CHECK_FALSE(c1.connected());
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
	CHECK_THAT(captured.values[0], Catch::Matchers::WithinULP(1.0, 0));
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
	CHECK_THAT(cd, Catch::Matchers::WithinULP(2.5, 0));
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


TEST_CASE("signal_handler: connection order is kept after disconnecting", "[signal_handler]") {
	auto sigh = events::signal_handler<void(std::vector<int>&)>{};

	auto c1 = sigh.connect([](std::vector<int>& v) { v.push_back(1); });
	auto c2 = sigh.connect([](std::vector<int>& v) { v.push_back(2); });
	auto c3 = sigh.connect([](std::vector<int>& v) { v.push_back(3); });
	c1.disconnect();
	auto c4 = sigh.connect([](std::vector<int>& v) { v.push_back(4); });

	std::vector<int> order;
	sigh.publish(order);
	CHECK(order == std::vector<int>{2, 3, 4});
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


// ---- Model test ----

TEST_CASE("signal_handler: random connects and disconnects match a reference model", "[signal_handler]") {
	auto rng = std::mt19937{12345};
	auto sigh = events::signal_handler<void(std::vector<int>&)>{};

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
