#include <events/connection.hpp>
#include <events/signal_handler/signal_handler.hpp>

#include <catch2/catch_test_macros.hpp>

#include <utility>


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

TEST_CASE("connection: copy shares disconnect capability", "[connection]") {
	auto sigh = events::signal_handler<void()>{};
	int calls = 0;
	auto conn1 = sigh.connect([&] { ++calls; });
	auto conn2 = conn1; // copy

	CHECK(static_cast<bool>(conn1));
	CHECK(static_cast<bool>(conn2));

	conn2.disconnect();
	// Both should be able to tell it's disconnected after either one disconnects
	CHECK_FALSE(static_cast<bool>(conn2));
	// conn1 still holds the function but the callback was erased from the colony
	sigh.publish(); // should not invoke the callback
	CHECK(calls == 0);
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
