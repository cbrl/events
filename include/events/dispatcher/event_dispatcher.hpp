#pragma once

#include <algorithm>
#include <concepts>
#include <map>
#include <memory>
#include <numeric>
#include <ranges>
#include <typeinfo>
#include <typeindex>
#include <vector>

#include <events/connection.hpp>
#include <events/signal_handler/signal_handler.hpp>


namespace events {
namespace detail {

template<typename EventT = void, typename AllocatorT = std::allocator<void>>
class discrete_event_dispatcher;


template<typename AllocatorT>
class [[nodiscard]] discrete_event_dispatcher<void, AllocatorT> {
public:
	discrete_event_dispatcher() = default;
	discrete_event_dispatcher(discrete_event_dispatcher const&) = delete;
	discrete_event_dispatcher(discrete_event_dispatcher&&) noexcept = default;

	virtual ~discrete_event_dispatcher() = default;

	auto operator=(discrete_event_dispatcher const&) -> discrete_event_dispatcher& = delete;
	auto operator=(discrete_event_dispatcher&&) noexcept -> discrete_event_dispatcher& = default;

	virtual auto dispatch() -> void = 0;
	virtual auto clear() -> void = 0;
	virtual auto size() -> size_t = 0;
};


template<typename EventT, typename AllocatorT>
class [[nodiscard]] discrete_event_dispatcher final : public discrete_event_dispatcher<void, AllocatorT> {
	using event_allocator_type = typename std::allocator_traits<AllocatorT>::template rebind_alloc<EventT>;
	using event_container_type = std::vector<EventT, event_allocator_type>;

public:
	discrete_event_dispatcher() = default;

	explicit discrete_event_dispatcher(AllocatorT const& allocator) : handler(allocator), events(allocator) {
	}

	discrete_event_dispatcher(discrete_event_dispatcher const&) = delete;

	discrete_event_dispatcher(discrete_event_dispatcher&&) noexcept = default;

	discrete_event_dispatcher(discrete_event_dispatcher&& other, AllocatorT const& allocator) :
		handler(std::move(other.handler), allocator),
		events(std::move(other.events), allocator) {
	}

	~discrete_event_dispatcher() override = default;

	auto operator=(discrete_event_dispatcher const&) -> discrete_event_dispatcher& = delete;
	auto operator=(discrete_event_dispatcher&&) noexcept -> discrete_event_dispatcher& = default;

	template<std::invocable<EventT const&> FunctionT>
	auto connect(FunctionT&& callback) -> connection {
		return handler.connect(std::forward<FunctionT>(callback));
	}

	auto dispatch() -> void override {
		// Moving the vector and iterating over a local one allows events to be enqueued during iteration
		auto to_publish = std::move(events);
		events.clear();

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
		events.emplace_back(std::forward<ArgsT>(args)...);
	}

	template<std::ranges::range RangeT>
	requires std::convertible_to<std::ranges::range_value_t<RangeT>, EventT>
	auto enqueue(RangeT&& range) -> void {
		events.insert(events.end(), std::ranges::begin(range), std::ranges::end(range));
	}

	auto clear() -> void override {
		events.clear();
	}

	auto size() -> size_t override {
		return events.size();
	}

private:
	signal_handler<void(EventT const&), AllocatorT> handler;
	event_container_type events;
};

}  //namespace detail


/**
 * @brief Stores callback functions that will be invoked when an event is published. Events may be
 *        immediately dispatched or enqueued for future bulk dispatch.
 */
template<typename AllocatorT = std::allocator<void>>
class [[nodiscard]] event_dispatcher {
	using alloc_traits = std::allocator_traits<AllocatorT>;

	using generic_dispatcher = detail::discrete_event_dispatcher<void, AllocatorT>;
	using generic_dispatcher_pointer = std::shared_ptr<generic_dispatcher>;

	using dispatcher_map_element_type = std::pair<const std::type_index, generic_dispatcher_pointer>;
	using dispatcher_allocator_type = typename alloc_traits::template rebind_alloc<dispatcher_map_element_type>;
	using dispatcher_map_type = std::map<std::type_index, generic_dispatcher_pointer, std::less<>, dispatcher_allocator_type>;

public:
	using allocator_type = AllocatorT;

	event_dispatcher() = default;

	explicit event_dispatcher(AllocatorT const& alloc) : allocator(alloc) {
	}

	event_dispatcher(event_dispatcher const&) = delete;

	/**
	 * @brief Construct a new event_dispatcher that will take ownership of another's signal handlers and enqueued
	 *        events.
	 *
	 * @details Existing connection objects from the other event dispatcher are NOT invalidated.
	 */
	event_dispatcher(event_dispatcher&& other) noexcept {
		if constexpr (alloc_traits::propagate_on_container_move_assignment::value) {
			allocator = std::move(other.allocator);
		}

		dispatchers = std::move(other.dispatchers);
	}

	/**
	 * @brief Construct a new event_dispatcher that will take ownership of another's signal handlers and enqueued
	 *        events.
	 *
	 * @details Existing connection objects from the other event dispatcher are NOT invalidated, and will now refer to
	 *          this event dispatcher.
	 */
	event_dispatcher(event_dispatcher&& other, AllocatorT const& alloc) noexcept :
		allocator(alloc),
		dispatchers(std::move(other.dispatchers), allocator) {
	}

	~event_dispatcher() = default;

	auto operator=(event_dispatcher const&) -> event_dispatcher& = delete;

	/**
	 * @brief Move a the signal handlers and enqueued events from an event_dispatcher into this one
	 *
	 * @details Existing connection objects from this event dispatcher are invalidated. Existing connection objects
	 *          from the other event dispatcher are NOT invalidated, and will now refer to this event dispatcher.
	 */
	auto operator=(event_dispatcher&& other) noexcept -> event_dispatcher& {
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
		for (auto& [type, dispatcher] : dispatchers) {
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
	auto get_or_create_dispatcher() -> detail::discrete_event_dispatcher<EventT, AllocatorT>& {
		using derived_type = detail::discrete_event_dispatcher<EventT, AllocatorT>;

		auto const [iter, inserted] = dispatchers.try_emplace(std::type_index{typeid(EventT)});

		if (inserted) {
			iter->second = std::allocate_shared<derived_type>(allocator, allocator);
		}

		return static_cast<derived_type&>(*(iter->second));
	}

	AllocatorT allocator;
	dispatcher_map_type dispatchers{allocator};
};

}  //namespace events
