#pragma once

#include <concepts>
#include <cstddef>
#include <iterator>
#include <map>
#include <memory>
#include <ranges>
#include <typeinfo>
#include <typeindex>
#include <utility>
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
	discrete_event_dispatcher(discrete_event_dispatcher&&) = delete;

	virtual ~discrete_event_dispatcher() = default;

	auto operator=(discrete_event_dispatcher const&) -> discrete_event_dispatcher& = delete;
	auto operator=(discrete_event_dispatcher&&) -> discrete_event_dispatcher& = delete;

	/// Move the queued events to the staging area. They're published by the next call to dispatch_staged().
	virtual auto stage() -> void = 0;

	/// Publish the staged events
	virtual auto dispatch_staged() -> void = 0;

	/// Discard the queued and staged events, and stop a dispatch_staged() that is in progress
	virtual auto clear() -> void = 0;

	/// Disconnect all callbacks, discard all events, and stop a dispatch_staged() that is in progress
	virtual auto close() noexcept -> void = 0;

	/// Get the number of events that haven't been published yet
	[[nodiscard]] virtual auto size() const -> size_t = 0;
};


template<typename EventT, typename AllocatorT>
class [[nodiscard]] discrete_event_dispatcher final : public discrete_event_dispatcher<void, AllocatorT> {
	using event_allocator_type = typename std::allocator_traits<AllocatorT>::template rebind_alloc<EventT>;
	using event_container_type = std::vector<EventT, event_allocator_type>;
	using difference_type = typename event_container_type::difference_type;

public:
	explicit discrete_event_dispatcher(AllocatorT const& allocator) :
		handler(allocator),
		events(allocator),
		staged(allocator) {
	}

	template<std::invocable<EventT const&> FunctionT>
	auto connect(FunctionT&& callback) -> connection {
		return handler.connect(std::forward<FunctionT>(callback));
	}

	auto stage() -> void override {
		if (staged.empty()) {
			// The queue takes over the staging buffer, so its capacity is reused instead of reallocated
			staged.swap(events);
		}
		else {
			// Left over from an interrupted or nested dispatch. Those events are older, so they stay in front.
			staged.insert(staged.end(), std::make_move_iterator(events.begin()), std::make_move_iterator(events.end()));
			events.clear();
		}
	}

	auto dispatch_staged() -> void override {
		if (staged.empty()) {
			return;
		}

		// Publish from a local batch, so that callbacks may enqueue events or dispatch again
		auto batch = std::move(staged);
		staged.clear();

		auto const current_generation = generation;
		auto const* const first = batch.data();  // callbacks can't reach the local batch, so it doesn't change
		auto const count = batch.size();
		auto index = size_t{0};

		try {
			for (; (index < count) && (generation == current_generation); ++index) {
				handler.publish(first[index]);
			}
		}
		catch (...) {
			if (generation == current_generation) {
				requeue(batch, index + 1);  // The event whose callback threw isn't delivered again
			}
			throw;
		}

		// Hand the buffer back so that its capacity is reused by the next stage()
		batch.clear();
		if (staged.empty() && (staged.capacity() < batch.capacity())) {
			staged.swap(batch);
		}
	}

	auto send(EventT const& event) -> void {
		handler.publish(event);
	}

	template<std::ranges::input_range RangeT>
	requires std::convertible_to<std::ranges::range_reference_t<RangeT>, EventT>
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

	template<std::ranges::input_range RangeT>
	requires std::convertible_to<std::ranges::range_reference_t<RangeT>, EventT>
	auto enqueue(RangeT&& range) -> void {
		// vector::insert(pos, first, last) requires a common range with C++17-style iterators, so append manually
		for (auto&& event : range) {
			events.emplace_back(std::forward<decltype(event)>(event));
		}
	}

	auto clear() -> void override {
		++generation;
		discard_events();
	}

	auto close() noexcept -> void override {
		++generation;
		handler.disconnect_all();
		discard_events();
	}

	[[nodiscard]] auto size() const -> size_t override {
		return events.size() + staged.size();
	}

private:
	// Put the undelivered part of a batch back in front of anything staged since, so that the next dispatch delivers it
	auto requeue(event_container_type& batch, size_t first) noexcept -> void {
		try {
			batch.erase(batch.begin(), batch.begin() + static_cast<difference_type>(first));
			batch.insert(batch.end(), std::make_move_iterator(staged.begin()), std::make_move_iterator(staged.end()));
			staged.swap(batch);
		}
		catch (...) {
			// Out of memory. The undelivered events are lost, but the callback's exception is still propagated.
		}
	}

	auto discard_events() noexcept -> void {
		// The events are destroyed after the containers are reset, in case an event's destructor enqueues an event
		[[maybe_unused]] auto const discarded_events = std::exchange(events, event_container_type{events.get_allocator()});
		[[maybe_unused]] auto const discarded_staged = std::exchange(staged, event_container_type{staged.get_allocator()});
	}

	signal_handler<void(EventT const&), AllocatorT> handler;

	event_container_type events;  ///< Enqueued events
	event_container_type staged;  ///< Events taken from the queue by a dispatch, and not published yet
	size_t generation = 0;        ///< Incremented by clear() and close() to stop a dispatch_staged() in progress
};

}  //namespace detail


/**
 * @brief Stores callback functions that will be invoked when an event is published. Events may be
 *        immediately dispatched or enqueued for future bulk dispatch.
 *
 * @details Each event type has its own set of callbacks and its own queue. The callbacks for an event type follow the
 *          rules of @ref signal_handler (e.g. they're invoked in the order they were connected).
 *
 *          dispatch() delivers the events that were enqueued before it was called:
 *          - Events of one type are delivered in the order they were enqueued.
 *          - Events are grouped by type: all events of one type are delivered before those of the next type. Event
 *            types are processed in the order in which they were first used with this dispatcher (by connect,
 *            enqueue or send). The relative order of events of different types is not preserved.
 *          - Events enqueued by callbacks while dispatch() is running are delivered by the next call to dispatch(),
 *            whatever their type.
 *          - If a callback throws, the exception propagates out of dispatch(). The remaining callbacks aren't invoked
 *            for the event being delivered, and that event is discarded. The events that weren't delivered yet stay
 *            queued, and are delivered by the next call to dispatch().
 *          - A callback may call dispatch(). The nested call also delivers the events that the outer call hasn't
 *            reached yet, so events may then be delivered out of order.
 *          - A callback may call clear(), or destroy or assign to the dispatcher. The events that weren't delivered
 *            yet are discarded (all of them, or only those of the cleared type).
 */
template<typename AllocatorT = std::allocator<void>>
class [[nodiscard]] basic_event_dispatcher {
	using alloc_traits = std::allocator_traits<AllocatorT>;

	using generic_dispatcher = detail::discrete_event_dispatcher<void, AllocatorT>;
	using generic_dispatcher_pointer = std::shared_ptr<generic_dispatcher>;

	template<typename EventT>
	using derived_dispatcher = detail::discrete_event_dispatcher<std::remove_cvref_t<EventT>, AllocatorT>;

	using dispatcher_map_element_type = std::pair<const std::type_index, generic_dispatcher_pointer>;
	using dispatcher_allocator_type = typename alloc_traits::template rebind_alloc<dispatcher_map_element_type>;
	using dispatcher_map_type = std::map<std::type_index, generic_dispatcher_pointer, std::less<>, dispatcher_allocator_type>;

	// The dispatchers in the order they were created. The list is replaced (never modified) when a dispatcher is added,
	// so that dispatch() can keep using the list it started with.
	using dispatcher_list_allocator_type = typename alloc_traits::template rebind_alloc<generic_dispatcher_pointer>;
	using dispatcher_list_type = std::vector<generic_dispatcher_pointer, dispatcher_list_allocator_type>;
	using dispatcher_list_pointer = std::shared_ptr<dispatcher_list_type const>;

public:
	using allocator_type = AllocatorT;

	basic_event_dispatcher() = default;

	explicit basic_event_dispatcher(AllocatorT const& alloc) : allocator(alloc) {
	}

	basic_event_dispatcher(basic_event_dispatcher const&) = delete;

	/**
	 * @brief Construct a new basic_event_dispatcher that will take ownership of another's signal handlers and enqueued
	 *        events.
	 *
	 * @details Existing connection objects from the other event dispatcher are NOT disconnected, and will now refer to
	 *          this event dispatcher. The other event dispatcher is left empty.
	 */
	basic_event_dispatcher(basic_event_dispatcher&& other) noexcept :
		allocator(other.allocator),
		dispatchers(std::move(other.dispatchers)),
		dispatcher_list(std::move(other.dispatcher_list)) {
		other.dispatchers.clear();
	}

	/**
	 * @brief Construct a new basic_event_dispatcher that will take ownership of another's signal handlers and enqueued
	 *        events.
	 *
	 * @details Existing connection objects from the other event dispatcher are NOT disconnected, and will now refer to
	 *          this event dispatcher. The other event dispatcher is left empty.
	 */
	basic_event_dispatcher(basic_event_dispatcher&& other, AllocatorT const& alloc) :
		allocator(alloc),
		dispatchers(std::move(other.dispatchers), allocator),
		dispatcher_list(std::move(other.dispatcher_list)) {
		other.dispatchers.clear();  // With unequal allocators, the elements were moved individually
	}

	~basic_event_dispatcher() {
		close_all(dispatcher_list);
	}

	auto operator=(basic_event_dispatcher const&) -> basic_event_dispatcher& = delete;

	/**
	 * @brief Move the signal handlers and enqueued events from a basic_event_dispatcher into this one
	 *
	 * @details Existing connection objects from this event dispatcher are disconnected, and its enqueued events are
	 *          discarded. Existing connection objects from the other event dispatcher are NOT disconnected, and will now
	 *          refer to this event dispatcher. The other event dispatcher is left empty.
	 */
	auto operator=(basic_event_dispatcher&& other
	) noexcept(alloc_traits::propagate_on_container_move_assignment::value || alloc_traits::is_always_equal::value)
	    -> basic_event_dispatcher& {
		if (&other == this) {
			return *this;
		}

		// Close the previous dispatchers last, since destroying callbacks may run code that uses this dispatcher
		auto const previous_map = std::move(dispatchers);
		auto const previous_list = std::move(dispatcher_list);

		if constexpr (alloc_traits::propagate_on_container_move_assignment::value) {
			allocator = std::move(other.allocator);
		}

		dispatchers = std::move(other.dispatchers);
		dispatcher_list = std::move(other.dispatcher_list);
		other.dispatchers.clear();

		close_all(previous_list);

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
		return get_dispatcher<EventT>().connect(std::forward<FunctionT>(callback));
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
		get_dispatcher<EventT>().enqueue(std::forward<EventT>(event));
	}

	/**
	 * @brief Enqueue an event to be dispatched later
	 *
	 * @tparam EventT  The type of event to enqueue
	 * @tparam ArgsT
	 *
	 * @param args The arguments required to construct an instance of this event
	 */
	template<typename EventT, typename... ArgsT>
	requires std::constructible_from<EventT, ArgsT...>
	auto enqueue(ArgsT&&... args) -> void {
		get_dispatcher<EventT>().enqueue(std::forward<ArgsT>(args)...);
	}

	/**
	 * @brief Enqueue a range of events to be dispatched later
	 *
	 * @tparam EventT  The type of event to enqueue
	 * @tparam RangeT
	 *
	 * @param range The range of events to enqueue
	 */
	template<typename EventT, std::ranges::input_range RangeT>
	requires std::convertible_to<std::ranges::range_reference_t<RangeT>, EventT>
	auto enqueue(RangeT&& range) -> void {
		get_dispatcher<EventT>().enqueue(std::forward<RangeT>(range));
	}

	/**
	 * @brief Send an event immediately
	 *
	 * @tparam EventT  The type of event to send
	 *
	 * @param event  An instance of the event to send
	 */
	template<typename EventT>
	auto send(EventT&& event) -> void {
		get_dispatcher<EventT>().send(std::forward<EventT>(event));
	}

	/**
	 * @brief Send an event immediately
	 *
	 * @tparam EventT  The type of event to send
	 * @tparam ArgsT
	 *
	 * @param args  The arguments required to construct an instance of this event
	 */
	template<typename EventT, typename... ArgsT>
	requires std::constructible_from<EventT, ArgsT...>
	auto send(ArgsT&&... args) -> void {
		// Parentheses (not braces) to match the constructible_from constraint and enqueue()'s emplace_back
		get_dispatcher<EventT>().send(EventT(std::forward<ArgsT>(args)...));
	}

	/**
	 * @brief Send a range of events immediately
	 *
	 * @tparam EventT  The type of event to send
	 * @tparam RangeT
	 *
	 * @param range The range of events to send
	 */
	template<typename EventT, std::ranges::input_range RangeT>
	requires std::convertible_to<std::ranges::range_reference_t<RangeT>, EventT>
	auto send(RangeT&& range) -> void {
		// Keep the dispatcher alive between events, in case a callback destroys or assigns to this object
		auto const target = std::static_pointer_cast<derived_dispatcher<EventT>>(get_or_create_dispatcher<EventT>());
		target->send(std::forward<RangeT>(range));
	}

	/**
	 * @brief Dispatch all events in the queue
	 *
	 * @details See the class description for the order in which events are delivered.
	 */
	auto dispatch() -> void {
		// Keep the list and its dispatchers alive, in case a callback destroys or assigns to this object. Dispatchers
		// created by callbacks aren't in this list, but any events they have were enqueued during this dispatch.
		auto const list = dispatcher_list;
		if (!list) {
			return;
		}

		for (auto const& dispatcher : *list) {
			dispatcher->stage();
		}

		for (auto const& dispatcher : *list) {
			dispatcher->dispatch_staged();
		}
	}

	/**
	 * @brief Discard the enqueued events of a specific event type or of all event types
	 *
	 * @details If called by a callback during dispatch(), the events that haven't been delivered yet are discarded.
	 *
	 * @tparam EventT  The type of event to discard. Leave default (void) to discard all enqueued events.
	 */
	template<typename EventT = void>
	auto clear() -> void {
		if constexpr (std::same_as<void, EventT>) {
			// Iterate over a copy, since destroying an event may enqueue another event of a new type
			if (auto const list = dispatcher_list) {
				for (auto const& dispatcher : *list) {
					dispatcher->clear();
				}
			}
		}
		else if (auto const it = dispatchers.find(std::type_index{typeid(std::remove_cvref_t<EventT>)}); it != dispatchers.end()) {
			it->second->clear();
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
			auto total = size_t{0};
			if (dispatcher_list) {
				for (auto const& dispatcher : *dispatcher_list) {
					total += dispatcher->size();
				}
			}
			return total;
		}
		else {
			auto const key = std::type_index{typeid(std::remove_cvref_t<EventT>)};

			if (auto it = dispatchers.find(key); it != dispatchers.end()) {
				return it->second->size();
			}

			return 0;
		}
	}

private:
	static auto close_all(dispatcher_list_pointer const& list) noexcept -> void {
		if (list) {
			for (auto const& dispatcher : *list) {
				dispatcher->close();
			}
		}
	}

	template<typename EventT>
	auto get_dispatcher() -> derived_dispatcher<EventT>& {
		return static_cast<derived_dispatcher<EventT>&>(*get_or_create_dispatcher<EventT>());
	}

	template<typename EventT>
	auto get_or_create_dispatcher() -> generic_dispatcher_pointer const& {
		auto const [iter, inserted] = dispatchers.try_emplace(std::type_index{typeid(std::remove_cvref_t<EventT>)});

		if (inserted) {
			try {
				iter->second = std::allocate_shared<derived_dispatcher<EventT>>(allocator, allocator);
				dispatcher_list = make_list_with(iter->second);
			}
			catch (...) {
				dispatchers.erase(iter);  // don't leave a null dispatcher behind
				throw;
			}
		}

		return iter->second;
	}

	[[nodiscard]]
	auto make_list_with(generic_dispatcher_pointer const& dispatcher) const -> dispatcher_list_pointer {
		auto list = dispatcher_list_type{dispatcher_list_allocator_type{allocator}};
		list.reserve((dispatcher_list ? dispatcher_list->size() : 0) + 1);
		if (dispatcher_list) {
			list.insert(list.end(), dispatcher_list->begin(), dispatcher_list->end());
		}
		list.push_back(dispatcher);

		return std::allocate_shared<dispatcher_list_type>(allocator, std::move(list));
	}

	AllocatorT allocator;
	dispatcher_map_type dispatchers{allocator};
	dispatcher_list_pointer dispatcher_list;
};


/// Type alias for a basic_event_dispatcher with the default template arguments
using event_dispatcher = basic_event_dispatcher<>;

}  //namespace events
