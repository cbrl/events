#include <events/dispatcher/event_dispatcher.hpp>
#include <events/dispatcher/synchronized_event_dispatcher.hpp>
#include <events/signal_handler/signal_handler.hpp>
#include <events/signal_handler/synchronized_signal_handler.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <type_traits>
#include <utility>


namespace {

using pmr_allocator = std::pmr::polymorphic_allocator<std::byte>;

// Counts allocations so tests can check that memory really comes from the supplied allocator
class counting_resource final : public std::pmr::memory_resource {
public:
	std::size_t allocations = 0;

private:
	auto do_allocate(std::size_t bytes, std::size_t alignment) -> void* override {
		++allocations;
		return std::pmr::new_delete_resource()->allocate(bytes, alignment);
	}

	auto do_deallocate(void* ptr, std::size_t bytes, std::size_t alignment) -> void override {
		std::pmr::new_delete_resource()->deallocate(ptr, bytes, alignment);
	}

	[[nodiscard]] auto do_is_equal(std::pmr::memory_resource const& other) const noexcept -> bool override {
		return this == &other;
	}
};

// A stateful allocator that propagates on move assignment (unlike polymorphic_allocator)
template<typename T>
struct tagged_allocator {
	using value_type = T;
	using propagate_on_container_move_assignment = std::true_type;
	using is_always_equal = std::false_type;

	tagged_allocator() noexcept = default;

	explicit tagged_allocator(int t) noexcept : tag(t) {
	}

	template<typename U>
	tagged_allocator(tagged_allocator<U> const& other) noexcept : tag(other.tag) { //NOLINT(google-explicit-constructor,hicpp-explicit-conversions)
	}

	[[nodiscard]] auto allocate(std::size_t count) -> T* {
		return std::allocator<T>{}.allocate(count);
	}

	auto deallocate(T* ptr, std::size_t count) noexcept -> void {
		std::allocator<T>{}.deallocate(ptr, count);
	}

	int tag = 0;
};

template<typename T, typename U>
auto operator==(tagged_allocator<T> const& lhs, tagged_allocator<U> const& rhs) noexcept -> bool {
	return lhs.tag == rhs.tag;
}

struct alloc_event {
	int value;
};

}  //namespace


// ---- Signal handlers ----

TEST_CASE("signal_handler: callbacks are allocated with the supplied allocator", "[allocator][signal_handler]") {
	auto resource = counting_resource{};
	auto sigh = events::signal_handler<void(int&), pmr_allocator>{pmr_allocator{&resource}};
	CHECK(sigh.get_allocator().resource() == &resource);

	auto conn = sigh.connect([](int& n) { ++n; });
	CHECK(resource.allocations > 0);

	int value = 0;
	sigh.publish(value);
	CHECK(value == 1);

	auto copy = events::signal_handler<void(int&), pmr_allocator>{sigh, pmr_allocator{&resource}};
	CHECK(copy.get_allocator().resource() == &resource);
	copy.publish(value);
	CHECK(value == 2);

	auto moved = std::move(sigh);
	CHECK(moved.get_allocator().resource() == &resource);
	moved.publish(value);
	CHECK(value == 3);
}

TEST_CASE("synchronized_signal_handler: callbacks are allocated with the supplied allocator", "[allocator][synchronized_signal_handler]") {
	auto resource = counting_resource{};
	auto sigh = events::synchronized_signal_handler<void(int&), pmr_allocator>{pmr_allocator{&resource}};
	CHECK(sigh.get_allocator().resource() == &resource);

	auto conn = sigh.connect([](int& n) { ++n; });
	CHECK(resource.allocations > 0);

	int value = 0;
	sigh.publish(value);
	CHECK(value == 1);

	auto copy = events::synchronized_signal_handler<void(int&), pmr_allocator>{sigh, pmr_allocator{&resource}};
	CHECK(copy.get_allocator().resource() == &resource);
	copy.publish(value);
	CHECK(value == 2);

	conn.disconnect();
	CHECK(sigh.size() == 0);
	CHECK(copy.size() == 1);
}


// ---- Event dispatchers ----

TEST_CASE("event_dispatcher: move constructor keeps the allocator", "[allocator][event_dispatcher]") {
	auto resource = counting_resource{};
	auto dispatcher1 = events::basic_event_dispatcher<pmr_allocator>{pmr_allocator{&resource}};
	int received = 0;

	auto conn = dispatcher1.connect<alloc_event>([&](alloc_event const& e) { received = e.value; });
	dispatcher1.enqueue(alloc_event{5});
	CHECK(resource.allocations > 0);

	auto dispatcher2 = std::move(dispatcher1);
	CHECK(dispatcher2.get_allocator().resource() == &resource);

	dispatcher2.dispatch();
	CHECK(received == 5);
}

TEST_CASE("event_dispatcher: allocator-extended move constructor uses the given allocator", "[allocator][event_dispatcher]") {
	using allocator = tagged_allocator<void>;

	auto dispatcher1 = events::basic_event_dispatcher<allocator>{allocator{1}};
	int received = 0;

	auto conn = dispatcher1.connect<alloc_event>([&](alloc_event const& e) { received = e.value; });
	dispatcher1.enqueue(alloc_event{7});

	auto dispatcher2 = events::basic_event_dispatcher<allocator>{std::move(dispatcher1), allocator{2}};
	CHECK(dispatcher2.get_allocator().tag == 2);

	dispatcher2.dispatch();
	CHECK(received == 7);
}

TEST_CASE("synchronized_event_dispatcher: move constructor keeps the allocator", "[allocator][synchronized_event_dispatcher]") {
	auto resource = counting_resource{};
	auto dispatcher1 = events::basic_synchronized_event_dispatcher<pmr_allocator>{pmr_allocator{&resource}};
	int received = 0;

	auto conn = dispatcher1.connect<alloc_event>([&](alloc_event const& e) { received = e.value; });
	dispatcher1.enqueue(alloc_event{5});
	CHECK(resource.allocations > 0);

	auto dispatcher2 = std::move(dispatcher1);
	CHECK(dispatcher2.get_allocator().resource() == &resource);

	dispatcher2.dispatch();
	CHECK(received == 5);
}

TEST_CASE("synchronized_event_dispatcher: allocator-extended move constructor uses the given allocator", "[allocator][synchronized_event_dispatcher]") {
	using allocator = tagged_allocator<void>;

	auto dispatcher1 = events::basic_synchronized_event_dispatcher<allocator>{allocator{1}};
	int received = 0;

	auto conn = dispatcher1.connect<alloc_event>([&](alloc_event const& e) { received = e.value; });
	dispatcher1.enqueue(alloc_event{7});

	auto dispatcher2 = events::basic_synchronized_event_dispatcher<allocator>{std::move(dispatcher1), allocator{2}};
	CHECK(dispatcher2.get_allocator().tag == 2);

	dispatcher2.dispatch();
	CHECK(received == 7);
}
