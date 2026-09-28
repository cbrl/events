#include <events/connection.hpp>
#include <events/signal_handler/signal_handler.hpp>

#include <catch2/catch_test_macros.hpp>

#include <utility>
#include <vector>


TEST_CASE("connection: default-constructed is empty", "[connection]") {
	auto conn = events::connection{};
	CHECK_FALSE(static_cast<bool>(conn));
}

TEST_CASE("connection: obtained from signal_handler is non-empty", "[connection]") {
	auto sigh = events::signal_handler<void()>{};
	auto conn = sigh.connect([] {});
	CHECK(static_cast<bool>(conn));
}

TEST_CASE("connection: disconnect makes it empty", "[connection]") {
	auto sigh = events::signal_handler<void()>{};
	auto conn = sigh.connect([] {});
	REQUIRE(static_cast<bool>(conn));
	conn.disconnect();
	CHECK_FALSE(static_cast<bool>(conn));
}

TEST_CASE("connection: disconnect is idempotent", "[connection]") {
	auto sigh = events::signal_handler<void()>{};
	auto conn = sigh.connect([] {});
	conn.disconnect();
	conn.disconnect(); // second disconnect must not crash
	CHECK_FALSE(static_cast<bool>(conn));
}

TEST_CASE("connection: disconnect removes the callback", "[connection]") {
	auto sigh = events::signal_handler<void(int&)>{};
	int count = 0;
	auto conn = sigh.connect([](int& c) { ++c; });
	sigh.publish(count);
	REQUIRE(count == 1);

	conn.disconnect();
	sigh.publish(count);
	CHECK(count == 1); // not incremented
}

TEST_CASE("connection: copies refer to the same callback", "[connection]") {
	auto sigh = events::signal_handler<void()>{};
	int calls = 0;
	auto conn1 = sigh.connect([&] { ++calls; });
	auto conn2 = conn1; // copy

	CHECK(conn1.connected());
	CHECK(conn2.connected());

	conn2.disconnect();
	CHECK_FALSE(conn2.connected());
	CHECK_FALSE(conn1.connected());

	sigh.publish(); // should not invoke the callback
	CHECK(calls == 0);

	conn1.disconnect(); // disconnecting through the other copy is a no-op
	CHECK(sigh.size() == 0);
}

TEST_CASE("connection: move transfers ownership", "[connection]") {
	auto sigh = events::signal_handler<void()>{};
	auto conn1 = sigh.connect([] {});
	auto conn2 = std::move(conn1);
	CHECK(static_cast<bool>(conn2));
}

TEST_CASE("connection: disconnect on default-constructed is safe", "[connection]") {
	auto conn = events::connection{};
	conn.disconnect(); // must not crash
	CHECK_FALSE(static_cast<bool>(conn));
}


// ---- scoped_connection ----

TEST_CASE("scoped_connection: auto-disconnects on destruction", "[scoped_connection]") {
	auto sigh = events::signal_handler<void(int&)>{};
	int count = 0;

	{
		auto scoped = events::scoped_connection{sigh.connect([](int& c) { ++c; })};
		sigh.publish(count);
		REQUIRE(count == 1);
	} // scoped goes out of scope here

	sigh.publish(count);
	CHECK(count == 1); // callback should be disconnected
}

TEST_CASE("scoped_connection: default-constructed is empty", "[scoped_connection]") {
	auto scoped = events::scoped_connection{};
	CHECK_FALSE(static_cast<bool>(scoped));
}

TEST_CASE("scoped_connection: move transfers ownership", "[scoped_connection]") {
	auto sigh = events::signal_handler<void()>{};
	auto scoped1 = events::scoped_connection{sigh.connect([] {})};
	REQUIRE(static_cast<bool>(scoped1));

	auto scoped2 = std::move(scoped1);
	CHECK(static_cast<bool>(scoped2));
}

TEST_CASE("scoped_connection: explicit disconnect", "[scoped_connection]") {
	auto sigh = events::signal_handler<void(int&)>{};
	int count = 0;
	auto scoped = events::scoped_connection{sigh.connect([](int& c) { ++c; })};

	scoped.disconnect();
	sigh.publish(count);
	CHECK(count == 0);
}

TEST_CASE("scoped_connection: assignment from connection", "[scoped_connection]") {
	auto sigh = events::signal_handler<void()>{};
	auto scoped = events::scoped_connection{};
	auto conn = sigh.connect([] {});

	scoped = conn;
	CHECK(static_cast<bool>(scoped));
}

TEST_CASE("scoped_connection: is non-copyable", "[scoped_connection]") {
	STATIC_CHECK_FALSE(std::is_copy_constructible_v<events::scoped_connection>);
	STATIC_CHECK_FALSE(std::is_copy_assignable_v<events::scoped_connection>);
}

TEST_CASE("connection: moved-from connection is empty", "[connection]") {
	auto sigh = events::signal_handler<void()>{};
	auto conn1 = sigh.connect([] {});

	auto conn2 = std::move(conn1);
	CHECK_FALSE(static_cast<bool>(conn1)); //NOLINT(bugprone-use-after-move)
	CHECK(static_cast<bool>(conn2));

	auto conn3 = events::connection{};
	conn3 = std::move(conn2);
	CHECK_FALSE(static_cast<bool>(conn2)); //NOLINT(bugprone-use-after-move)
	CHECK(static_cast<bool>(conn3));

	// Disconnecting through the moved-from handles must not affect the callback
	conn1.disconnect(); //NOLINT(bugprone-use-after-move)
	conn2.disconnect(); //NOLINT(bugprone-use-after-move)
	CHECK(sigh.size() == 1);
}

TEST_CASE("scoped_connection: moved-from temporary does not disconnect", "[scoped_connection]") {
	auto sigh = events::signal_handler<void(int&)>{};
	int count = 0;

	auto scoped = std::vector<events::scoped_connection>{};
	scoped.push_back(events::scoped_connection{sigh.connect([](int& c) { ++c; })});

	sigh.publish(count);
	CHECK(count == 1);
	CHECK(sigh.size() == 1);
}

TEST_CASE("scoped_connection: move assignment disconnects the previous connection", "[scoped_connection]") {
	auto sigh = events::signal_handler<void()>{};
	int first = 0;
	int second = 0;

	auto scoped = events::scoped_connection{sigh.connect([&] { ++first; })};
	scoped = events::scoped_connection{sigh.connect([&] { ++second; })};

	sigh.publish();
	CHECK(first == 0);
	CHECK(second == 1);
	CHECK(sigh.size() == 1);
}

TEST_CASE("scoped_connection: assignment from connection disconnects the previous connection", "[scoped_connection]") {
	auto sigh = events::signal_handler<void()>{};
	int first = 0;
	int second = 0;

	{
		auto scoped = events::scoped_connection{sigh.connect([&] { ++first; })};
		scoped = sigh.connect([&] { ++second; });

		sigh.publish();
		CHECK(first == 0);
		CHECK(second == 1);
		CHECK(sigh.size() == 1);
	}

	CHECK(sigh.size() == 0);
}

TEST_CASE("scoped_connection: release keeps the callback connected", "[scoped_connection]") {
	auto sigh = events::signal_handler<void()>{};
	auto released = events::connection{};

	{
		auto scoped = events::scoped_connection{sigh.connect([] {})};
		released = scoped.release();
		CHECK_FALSE(static_cast<bool>(scoped));
	}

	CHECK(sigh.size() == 1);
	released.disconnect();
	CHECK(sigh.size() == 0);
}


// ---- Lifetime ----

TEST_CASE("connection: may outlive its signal handler", "[connection]") {
	auto conn = events::connection{};
	auto scoped = events::scoped_connection{};

	{
		auto sigh = events::signal_handler<void()>{};
		conn = sigh.connect([] {});
		scoped = sigh.connect([] {});
		CHECK(conn.connected());
		CHECK(scoped.connected());
	}

	CHECK_FALSE(conn.connected());
	CHECK_FALSE(scoped.connected());
	conn.disconnect(); // no-op
	scoped.disconnect(); // no-op
}

TEST_CASE("connection: remains valid when its signal handler is moved", "[connection]") {
	auto sigh = events::signal_handler<void()>{};
	int calls = 0;
	auto conn = sigh.connect([&] { ++calls; });

	auto moved = std::move(sigh);
	CHECK(conn.connected());

	moved.publish();
	CHECK(calls == 1);

	conn.disconnect();
	moved.publish();
	CHECK(calls == 1);
	CHECK(moved.size() == 0);

	auto assigned = events::signal_handler<void()>{};
	auto conn2 = moved.connect([&] { ++calls; });
	assigned = std::move(moved);
	conn2.disconnect();
	CHECK(assigned.size() == 0);
}

TEST_CASE("connection: a stale copy doesn't disconnect a newer callback", "[connection]") {
	auto sigh = events::signal_handler<void()>{};
	int b_calls = 0;

	auto conn_a = sigh.connect([] {});
	auto copy_a = conn_a;
	conn_a.disconnect();

	auto conn_b = sigh.connect([&] { ++b_calls; });
	copy_a.disconnect(); // must not affect B

	sigh.publish();
	CHECK(b_calls == 1);
	CHECK(conn_b.connected());
	CHECK(sigh.size() == 1);
}

TEST_CASE("connection: copying a signal handler doesn't share connections", "[connection]") {
	auto sigh = events::signal_handler<void()>{};
	auto conn = sigh.connect([] {});

	auto copy = sigh;
	CHECK(copy.size() == 1);

	conn.disconnect();
	CHECK(sigh.size() == 0);
	CHECK(copy.size() == 1);
}
