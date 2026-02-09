#pragma once

#include <algorithm>
#include <concepts>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <ranges>
#include <shared_mutex>
#include <typeinfo>
#include <typeindex>
#include <vector>

#include <events/connection.hpp>
#include <events/signal_handler/synchronized_signal_handler.hpp>


// NOLINTBEGIN(cppcoreguidelines-prefer-member-initializer,hicpp-noexcept-move,performance-noexcept-move-constructor)

namespace events {
namespace detail {

template<typename EventT = void, typename AllocatorT = std::allocator<void>>
class synchronized_discrete_event_dispatcher;


template<typename AllocatorT>
class [[nodiscard]] synchronized_discrete_event_dispatcher<void, AllocatorT> {
public:
	synchronized_discrete_event_dispatcher() = default;
	synchronized_discrete_event_dispatcher(synchronized_discrete_event_dispatcher const&) = delete;
	synchronized_discrete_event_dispatcher(synchronized_discrete_event_dispatcher&&) noexcept = default;

	virtual ~synchronized_discrete_event_dispatcher() = default;

	auto operator=(synchronized_discrete_event_dispatcher const&) -> synchronized_discrete_event_dispatcher& = delete;
	auto operator=(synchronized_discrete_event_dispatcher&&) noexcept
	    -> synchronized_discrete_event_dispatcher& = default;

	virtual auto dispatch() -> void = 0;
	virtual auto clear() -> void = 0;
	virtual auto size() -> size_t = 0;
};


template<typename EventT, typename AllocatorT>
class [[nodiscard]] synchronized_discrete_event_dispatcher final : public synchronized_discrete_event_dispatcher<void, AllocatorT> {
	using event_allocator_type = typename std::allocator_traits<AllocatorT>::template rebind_alloc<EventT>;
	using event_container_type = std::vector<EventT, event_allocator_type>;

public:
	synchronized_discrete_event_dispatcher() = default;

	explicit synchronized_discrete_event_dispatcher(AllocatorT const& alloc) : handler(alloc), events(alloc) {
	}

	synchronized_discrete_event_dispatcher(synchronized_discrete_event_dispatcher const&) = delete;

	synchronized_discrete_event_dispatcher(synchronized_discrete_event_dispatcher&& other) {
		auto lock = std::scoped_lock{other.events_mut};
		handler = std::move(other.handler);
		events = std::move(other.events);
	}

	synchronized_discrete_event_dispatcher(synchronized_discrete_event_dispatcher&& other, AllocatorT const& alloc) {
		auto lock = std::scoped_lock{other.events_mut};
		handler = decltype(handler){std::move(other.handler), alloc};
		events = event_container_type{std::move(other.events), alloc};
	}

	~synchronized_discrete_event_dispatcher() override = default;

	auto operator=(synchronized_discrete_event_dispatcher const&) -> synchronized_discrete_event_dispatcher& = delete;

	auto operator=(synchronized_discrete_event_dispatcher&& other) -> synchronized_discrete_event_dispatcher& {
		auto lock = std::scoped_lock{events_mut, other.events_mut};
		handler = std::move(other.handler);
		events = std::move(other.events);
		return *this;
	}

	template<std::invocable<EventT const&> FunctionT>
	auto connect(FunctionT&& callback) -> connection {
		return handler.connect(std::forward<FunctionT>(callback));
	}

	auto dispatch() -> void override {
		// Moving the vector and iterating over a local one allows events to be enqueued during iteration
		auto lock = std::unique_lock{events_mut};
		auto to_publish = std::move(events);
		events.clear();
		lock.unlock();

		for (auto const& event : to_publish) {
			handler.publish(event);
		}
	}

	auto send(EventT const& event) -> void {
		handler.publish(event);
	}

	template<std::ranges::range RangeT>
	requires std::convertible_to<std::ranges::range_value_t<RangeT>, EventT>
	auto send(RangeT&& range) -> void {
		for (auto&& event : range) {
			handler.publish(event);
		}
	}

	template<typename... ArgsT>
	requires std::constructible_from<EventT, ArgsT...>
	auto enqueue(ArgsT&&... args) -> void {
		auto lock = std::scoped_lock{events_mut};
		events.emplace_back(std::forward<ArgsT>(args)...);
	}

	template<std::ranges::range RangeT>
	requires std::convertible_to<std::ranges::range_value_t<RangeT>, EventT>
	auto enqueue(RangeT&& range) -> void {
		auto lock = std::scoped_lock{events_mut};
		events.insert(events.end(), std::ranges::begin(range), std::ranges::end(range));
	}

	auto clear() -> void override {
		auto lock = std::scoped_lock{events_mut};
		events.clear();
	}

	auto size() -> size_t override {
		auto lock = std::scoped_lock{events_mut};
		return events.size();
	}

private:
	synchronized_signal_handler<void(EventT const&), AllocatorT> handler;

	event_container_type events;
	std::mutex events_mut;
};

}  //namespace detail


/**
 * @brief A thread-safe @ref event_dispatcher
 */
template<typename AllocatorT = std::allocator<void>>
class [[nodiscard]] basic_synchronized_event_dispatcher {
	using alloc_traits = std::allocator_traits<AllocatorT>;

	using generic_dispatcher = detail::synchronized_discrete_event_dispatcher<void, AllocatorT>;
	using generic_dispatcher_pointer = std::shared_ptr<generic_dispatcher>;

	using dispatcher_map_element_type = std::pair<const std::type_index, generic_dispatcher_pointer>;
	using dispatcher_allocator_type = typename alloc_traits::template rebind_alloc<dispatcher_map_element_type>;
	using dispatcher_map_type = std::map<std::type_index, generic_dispatcher_pointer, std::less<>, dispatcher_allocator_type>;

public:
	using allocator_type = AllocatorT;

	basic_synchronized_event_dispatcher() = default;

	explicit basic_synchronized_event_dispatcher(AllocatorT const& alloc) : allocator(alloc) {
	}

	basic_synchronized_event_dispatcher(basic_synchronized_event_dispatcher const&) = delete;

	/**
	 * @brief Construct a new synchronized_event_dispatcher that will take ownership of another's signal handlers and
	 *        enqueued events.
	 *
	 * @details Existing connection objects from the other event dispatcher are NOT invalidated.
	 */
	basic_synchronized_event_dispatcher(basic_synchronized_event_dispatcher&& other) {
		auto lock = std::scoped_lock{other.dispatcher_mut};

		if constexpr (alloc_traits::propagate_on_container_move_assignment::value) {
			allocator = std::move(other.allocator);
		}

		dispatchers = std::move(other.dispatchers);
	}

	/**
	 * @brief Construct a new basic_synchronized_event_dispatcher that will take ownership of another's signal handlers and
	 *        enqueued events.
	 *
	 * @details Existing connection objects from the other event dispatcher are NOT invalidated.
	 */
	basic_synchronized_event_dispatcher(basic_synchronized_event_dispatcher&& other, AllocatorT const& alloc) : allocator(alloc) {
		auto lock = std::scoped_lock{other.dispatcher_mut};

		if constexpr (alloc_traits::propagate_on_container_move_assignment::value) {
			allocator = std::move(other.allocator);
		}

		dispatchers = dispatcher_map_type{std::move(other.dispatchers), allocator};
	}

	~basic_synchronized_event_dispatcher() = default;

	auto operator=(basic_synchronized_event_dispatcher const&) -> basic_synchronized_event_dispatcher& = delete;

	/**
	 * @brief Move a the signal handlers and enqueued events from a basic_synchronized_event_dispatcher into this one
	 *
	 * @details Existing connection objects from this event dispatcher are invalidated. Existing connection objects
	 *          from the other event dispatcher are NOT invalidated, and will now refer to this event dispatcher.
	 */
	auto operator=(basic_synchronized_event_dispatcher&& other) -> basic_synchronized_event_dispatcher& {
		if (&other == this) {
			return *this;
		}

		auto locks = std::scoped_lock{dispatcher_mut, other.dispatcher_mut};

		if constexpr (alloc_traits::propagate_on_container_move_assignment::value) {
			allocator = std::move(other.allocator);
		}

		dispatchers = std::move(other.dispatchers);

		return *this;
	}

	[[nodiscard]]
	constexpr auto get_allocator() const noexcept -> allocator_type {
		return allocator;
	}

	/**
	 * @brief Register a callback function that will be invoked when an event of the specified type is published
	 *
	 * @tparam EventT  The type of event this callback handles
	 * @tparam FunctionT
	 *
	 * @param callback  A function which accepts one argument of type EventT
	 *
	 * @return A connection handle that can be used to disconnect the function from this event dispatcher
	 */
	template<typename EventT, std::invocable<EventT const&> FunctionT>
	auto connect(FunctionT&& callback) -> connection {
		return get_or_create_dispatcher<EventT>().connect(std::forward<FunctionT>(callback));
	}


	/**
	 * @brief Enqueue an event to be dispatched later
	 *
	 * @tparam EventT  The type of event to enqueue
	 *
	 * @param event  An instance of the event to enqueue
	 */
	template<typename EventT>
	auto enqueue(EventT&& event) -> void {
		using event_type = std::remove_cvref_t<EventT>;
		get_or_create_dispatcher<event_type>().enqueue(std::forward<EventT>(event));
	}

	/**
	 * @brief Enqueue an event to be dispatched later
	 *
	 * @tparam EventT  The type of event to enqueue
	 * @tparam ArgsT
	 *
	 * @param args The arguments requires to construct an instance of this event
	 */
	template<typename EventT, typename... ArgsT>
	requires std::constructible_from<EventT, ArgsT...>
	auto enqueue(ArgsT&&... args) -> void {
		get_or_create_dispatcher<EventT>().enqueue(std::forward<ArgsT>(args)...);
	}

	/**
	 * @brief Enqueue a range of events to be dispatched later
	 *
	 * @tparam EventT  The type of event to enqueue
	 * @tparam RangeT
	 *
	 * @param args The range of events to enqueue
	 */
	template<typename EventT, std::ranges::range RangeT>
	requires std::convertible_to<std::ranges::range_value_t<RangeT>, EventT>
	auto enqueue(RangeT&& range) -> void {
		get_or_create_dispatcher<EventT>().enqueue(std::forward<RangeT>(range));
	}

	/**
	 * @brief Send an event immediately
	 *
	 * @tparam EventT  The type of event to send
	 *
	 * @param args  An instance of the event to send
	 */
	template<typename EventT>
	auto send(EventT&& event) -> void {
		using event_type = std::remove_cvref_t<EventT>;
		get_or_create_dispatcher<event_type>().send(std::forward<EventT>(event));
	}

	/**
	 * @brief Send an event immediately
	 *
	 * @tparam EventT  The type of event to send
	 * @tparam ArgsT
	 *
	 * @param args  The arguments requires to construct an instance of this event
	 */
	template<typename EventT, typename... ArgsT>
	requires std::constructible_from<EventT, ArgsT...>
	auto send(ArgsT&&... args) -> void {
		get_or_create_dispatcher<EventT>().send(EventT{std::forward<ArgsT>(args)...});
	}

	/**
	 * @brief Send a range of events immediately
	 *
	 * @tparam EventT  The type of event to send
	 * @tparam RangeT
	 *
	 * @param args The range of events to send
	 */
	template<typename EventT, std::ranges::range RangeT>
	requires std::convertible_to<std::ranges::range_value_t<RangeT>, EventT>
	auto send(RangeT&& range) -> void {
		get_or_create_dispatcher<EventT>().send(std::forward<RangeT>(range));
	}

	/// Dispatch all events in the queue
	auto dispatch() -> void {
		// Take a snapshot of the current dispatchers to avoid holding dispatcher_mut while invoking user callbacks. A
		// callback may enqueue/send a new event type, which requires upgrading to an exclusive lock to create a new
		// dispatcher. Holding a shared lock here would deadlock (publisher thread waits for callback; callback waits
		// for unique_lock).
		using snapshot_alloc_type = typename alloc_traits::template rebind_alloc<generic_dispatcher_pointer>;
		auto snapshot = std::vector<generic_dispatcher_pointer, snapshot_alloc_type>{snapshot_alloc_type{allocator}};
		{
			auto lock = std::shared_lock{dispatcher_mut};
			snapshot.reserve(dispatchers.size());
			for (auto const& [type, dispatcher] : dispatchers) {
				snapshot.push_back(dispatcher);
			}
		}

		for (auto const& dispatcher : snapshot) {
			dispatcher->dispatch();
		}
	}

	/**
	 * @brief Get the number of enqueued events for a specific event type or for all events
	 *
	 * @tparam EventT  The type of event to get the count of. Leave default (void) to obtain the total number of
	 *                 enqueued events.
	 *
	 * @return The number of enqueued events
	 */
	template<typename EventT = void>
	[[nodiscard]]
	auto queue_size() const -> size_t {
		auto lock = std::shared_lock{dispatcher_mut};

		if constexpr (std::same_as<void, EventT>) {
			auto sizes = std::views::values(dispatchers) | std::views::transform([](auto const& ptr) { return ptr->size(); });
			return std::accumulate(std::ranges::begin(sizes), std::ranges::end(sizes), 0ull);
		}

		auto const key = std::type_index{typeid(EventT)};

		if (auto it = dispatchers.find(key); it != dispatchers.end()) {
			return it->second->size();
		}

		return 0;
	}

private:
	template<typename EventT>
	auto get_or_create_dispatcher() -> detail::synchronized_discrete_event_dispatcher<EventT, AllocatorT>& {
		using derived_dispatcher_type = detail::synchronized_discrete_event_dispatcher<EventT, AllocatorT>;

		auto const key = std::type_index{typeid(EventT)};

		// Attempt to find an existing dispatcher
		{
			auto lock = std::shared_lock{dispatcher_mut};

			if (auto it = dispatchers.find(key); it != dispatchers.end()) {
				return static_cast<derived_dispatcher_type&>(*(it->second));
			}
		}

		// If the dispatcher didn't exist, then acquire an exclusive lock and create it.
		auto lock = std::unique_lock{dispatcher_mut};

		auto const [iter, inserted] = dispatchers.try_emplace(key);

		// Check if it actually was created since two threads could get to the point where they try
		// to acquire an exclusive lock.
		if (inserted) {
			iter->second = std::allocate_shared<derived_dispatcher_type>(allocator, allocator);
		}

		return static_cast<derived_dispatcher_type&>(*(iter->second));
	}

	AllocatorT allocator;
	dispatcher_map_type dispatchers{allocator};
	mutable std::shared_mutex dispatcher_mut;
};


/// Type alias for a basic_synchronized_event_dispatcher with the default template arguments
using synchronized_event_dispatcher = basic_synchronized_event_dispatcher<>;

}  //namespace events

// NOLINTEND(cppcoreguidelines-prefer-member-initializer,hicpp-noexcept-move,performance-noexcept-move-constructor)
