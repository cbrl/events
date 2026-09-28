#pragma once

#include <atomic>
#include <concepts>
#include <cstddef>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <shared_mutex>
#include <typeinfo>
#include <typeindex>
#include <utility>
#include <vector>

#include <events/connection.hpp>
#include <events/signal_handler/synchronized_signal_handler.hpp>


// NOLINTBEGIN(hicpp-noexcept-move,performance-noexcept-move-constructor)

namespace events {
namespace detail {

template<typename EventT = void, typename AllocatorT = std::allocator<void>>
class synchronized_discrete_event_dispatcher;


template<typename AllocatorT>
class [[nodiscard]] synchronized_discrete_event_dispatcher<void, AllocatorT> {
public:
	synchronized_discrete_event_dispatcher() = default;
	synchronized_discrete_event_dispatcher(synchronized_discrete_event_dispatcher const&) = delete;
	synchronized_discrete_event_dispatcher(synchronized_discrete_event_dispatcher&&) = delete;

	virtual ~synchronized_discrete_event_dispatcher() = default;

	auto operator=(synchronized_discrete_event_dispatcher const&) -> synchronized_discrete_event_dispatcher& = delete;
	auto operator=(synchronized_discrete_event_dispatcher&&) -> synchronized_discrete_event_dispatcher& = delete;

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
class [[nodiscard]] synchronized_discrete_event_dispatcher final : public synchronized_discrete_event_dispatcher<void, AllocatorT> {
	using event_allocator_type = typename std::allocator_traits<AllocatorT>::template rebind_alloc<EventT>;
	using event_container_type = std::vector<EventT, event_allocator_type>;
	using difference_type = typename event_container_type::difference_type;

public:
	explicit synchronized_discrete_event_dispatcher(AllocatorT const& alloc) :
		handler(alloc),
		events(alloc),
		staged(alloc) {
	}

	template<std::invocable<EventT const&> FunctionT>
	auto connect(FunctionT&& callback) -> connection {
		return handler.connect(std::forward<FunctionT>(callback));
	}

	auto stage() -> void override {
		auto lock = std::scoped_lock{events_mut};

		if (staged.empty()) {
			// The queue takes over the staging buffer, so its capacity is reused instead of reallocated
			staged.swap(events);
		}
		else {
			// Left over from an interrupted, nested or concurrent dispatch. Those events are older, so they stay in front.
			staged.insert(staged.end(), std::make_move_iterator(events.begin()), std::make_move_iterator(events.end()));
			events.clear();
		}
	}

	auto dispatch_staged() -> void override {
		// Publish from a local batch without holding the lock, so that callbacks may enqueue events or dispatch again
		auto lock = std::unique_lock{events_mut};
		if (staged.empty()) {
			return;
		}

		auto batch = std::move(staged);
		staged.clear();
		auto const current_generation = generation.load(std::memory_order_relaxed);
		lock.unlock();

		auto const* const first = batch.data();  // callbacks can't reach the local batch, so it doesn't change
		auto const count = batch.size();
		auto index = size_t{0};

		try {
			for (; (index < count) && (generation.load(std::memory_order_relaxed) == current_generation); ++index) {
				handler.publish(first[index]);
			}
		}
		catch (...) {
			lock.lock();
			if (generation.load(std::memory_order_relaxed) == current_generation) {
				requeue(batch, index + 1);  // The event whose callback threw isn't delivered again
			}
			lock.unlock();
			throw;  // The rest of the batch (if any) is destroyed outside the lock
		}

		// Hand the buffer back so that its capacity is reused by the next stage()
		batch.clear();
		lock.lock();
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
		auto lock = std::scoped_lock{events_mut};
		events.emplace_back(std::forward<ArgsT>(args)...);
	}

	template<std::ranges::input_range RangeT>
	requires std::convertible_to<std::ranges::range_reference_t<RangeT>, EventT>
	auto enqueue(RangeT&& range) -> void {
		// vector::insert(pos, first, last) requires a common range with C++17-style iterators, so append manually
		auto lock = std::scoped_lock{events_mut};
		for (auto&& event : range) {
			events.emplace_back(std::forward<decltype(event)>(event));
		}
	}

	auto clear() -> void override {
		discard_events();
	}

	auto close() noexcept -> void override {
		handler.disconnect_all();
		discard_events();
	}

	[[nodiscard]] auto size() const -> size_t override {
		auto lock = std::scoped_lock{events_mut};
		return events.size() + staged.size();
	}

private:
	// Put the undelivered part of a batch back in front of anything staged since, so that the next dispatch delivers
	// it. Requires events_mut to be locked.
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
		// The events are destroyed after unlocking, in case an event's destructor enqueues an event
		auto discarded_events = event_container_type{events.get_allocator()};
		auto discarded_staged = event_container_type{staged.get_allocator()};

		auto lock = std::scoped_lock{events_mut};
		generation.fetch_add(1, std::memory_order_relaxed);
		discarded_events.swap(events);
		discarded_staged.swap(staged);
	}

	synchronized_signal_handler<void(EventT const&), AllocatorT> handler;

	event_container_type events;  ///< Enqueued events
	event_container_type staged;  ///< Events taken from the queue by a dispatch, and not published yet
	mutable std::mutex events_mut;

	/// Incremented (while holding events_mut) by clear() and close() to stop a dispatch_staged() in progress
	std::atomic<size_t> generation{0};
};

}  //namespace detail


/**
 * @brief A thread-safe @ref event_dispatcher
 *
 * @details Events are delivered in the same order as by @ref event_dispatcher, with these additions for concurrent use:
 *          - Events of one type are delivered in the order in which they were added to the queue.
 *          - An event that another thread enqueues while dispatch() is starting may be delivered either by that
 *            dispatch() or by the next one. Events enqueued after a dispatch() has started invoking callbacks are
 *            delivered by the next one.
 *          - If multiple threads call dispatch() at the same time, each event is delivered once, by one of them.
 *            dispatch() may then return before all of the events it picked up were delivered by the other threads.
 *          - Callbacks follow the rules of @ref synchronized_signal_handler. In particular, disconnecting doesn't wait
 *            for invocations that are already running on other threads.
 *          - Callbacks are never invoked while the dispatcher holds a lock, so they may use the dispatcher freely.
 */
template<typename AllocatorT = std::allocator<void>>
class [[nodiscard]] basic_synchronized_event_dispatcher {
	using alloc_traits = std::allocator_traits<AllocatorT>;

	using generic_dispatcher = detail::synchronized_discrete_event_dispatcher<void, AllocatorT>;
	using generic_dispatcher_pointer = std::shared_ptr<generic_dispatcher>;

	template<typename EventT>
	using derived_dispatcher = detail::synchronized_discrete_event_dispatcher<std::remove_cvref_t<EventT>, AllocatorT>;

	using dispatcher_map_element_type = std::pair<const std::type_index, generic_dispatcher_pointer>;
	using dispatcher_allocator_type = typename alloc_traits::template rebind_alloc<dispatcher_map_element_type>;
	using dispatcher_map_type = std::map<std::type_index, generic_dispatcher_pointer, std::less<>, dispatcher_allocator_type>;

	// The dispatchers in the order they were created. The list is replaced (never modified) when a dispatcher is added,
	// so that dispatch() can use it without holding a lock.
	using dispatcher_list_allocator_type = typename alloc_traits::template rebind_alloc<generic_dispatcher_pointer>;
	using dispatcher_list_type = std::vector<generic_dispatcher_pointer, dispatcher_list_allocator_type>;
	using dispatcher_list_pointer = std::shared_ptr<dispatcher_list_type const>;

	using lock_type = std::unique_lock<std::shared_mutex>;

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
	 * @details Existing connection objects from the other event dispatcher are NOT disconnected, and will now refer to
	 *          this event dispatcher. The other event dispatcher is left empty.
	 */
	basic_synchronized_event_dispatcher(basic_synchronized_event_dispatcher&& other) :
		basic_synchronized_event_dispatcher(std::move(other), lock_type{other.dispatcher_mut}) {
	}

	/**
	 * @brief Construct a new basic_synchronized_event_dispatcher that will take ownership of another's signal handlers and
	 *        enqueued events.
	 *
	 * @details Existing connection objects from the other event dispatcher are NOT disconnected, and will now refer to
	 *          this event dispatcher. The other event dispatcher is left empty.
	 */
	basic_synchronized_event_dispatcher(basic_synchronized_event_dispatcher&& other, AllocatorT const& alloc) :
		basic_synchronized_event_dispatcher(std::move(other), alloc, lock_type{other.dispatcher_mut}) {
	}

	~basic_synchronized_event_dispatcher() {
		close_all(dispatcher_list);
	}

	auto operator=(basic_synchronized_event_dispatcher const&) -> basic_synchronized_event_dispatcher& = delete;

	/**
	 * @brief Move the signal handlers and enqueued events from a basic_synchronized_event_dispatcher into this one
	 *
	 * @details Existing connection objects from this event dispatcher are disconnected, and its enqueued events are
	 *          discarded. Existing connection objects from the other event dispatcher are NOT disconnected, and will now
	 *          refer to this event dispatcher. The other event dispatcher is left empty.
	 */
	auto operator=(basic_synchronized_event_dispatcher&& other) -> basic_synchronized_event_dispatcher& {
		if (&other == this) {
			return *this;
		}

		// Closed and destroyed after the locks are released, since destroying callbacks may run code that uses this
		// dispatcher
		auto previous_map = std::optional<dispatcher_map_type>{};
		auto previous_list = dispatcher_list_pointer{};

		{
			auto locks = std::scoped_lock{dispatcher_mut, other.dispatcher_mut};

			previous_map.emplace(std::move(dispatchers));
			previous_list = std::move(dispatcher_list);

			if constexpr (alloc_traits::propagate_on_container_move_assignment::value) {
				allocator = std::move(other.allocator);
			}

			dispatchers = std::move(other.dispatchers);
			dispatcher_list = std::move(other.dispatcher_list);
			other.dispatchers.clear();
		}

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
		return get_dispatcher<EventT>()->connect(std::forward<FunctionT>(callback));
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
		get_dispatcher<EventT>()->enqueue(std::forward<EventT>(event));
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
		get_dispatcher<EventT>()->enqueue(std::forward<ArgsT>(args)...);
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
		get_dispatcher<EventT>()->enqueue(std::forward<RangeT>(range));
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
		get_dispatcher<EventT>()->send(std::forward<EventT>(event));
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
		get_dispatcher<EventT>()->send(EventT(std::forward<ArgsT>(args)...));
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
		get_dispatcher<EventT>()->send(std::forward<RangeT>(range));
	}

	/**
	 * @brief Dispatch all events in the queue
	 *
	 * @details See the class description for the order in which events are delivered.
	 */
	auto dispatch() -> void {
		// No lock is held while invoking callbacks, since a callback may use this dispatcher (e.g. enqueue an event of
		// a new type, which requires an exclusive lock). The list keeps its dispatchers alive.
		auto const list = acquire_list();
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
	 * @details Events that a dispatch() in progress hasn't delivered yet are discarded too.
	 *
	 * @tparam EventT  The type of event to discard. Leave default (void) to discard all enqueued events.
	 */
	template<typename EventT = void>
	auto clear() -> void {
		if constexpr (std::same_as<void, EventT>) {
			if (auto const list = acquire_list()) {
				for (auto const& dispatcher : *list) {
					dispatcher->clear();
				}
			}
		}
		else if (auto const dispatcher = find_dispatcher<EventT>()) {
			dispatcher->clear();
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
			if (auto const list = acquire_list()) {
				for (auto const& dispatcher : *list) {
					total += dispatcher->size();
				}
			}
			return total;
		}
		else if (auto const dispatcher = find_dispatcher<EventT>()) {
			return dispatcher->size();
		}
		else {
			return 0;
		}
	}

private:
	// Delegation targets for the move constructors, so that the source stays locked while members are initialized.
	// The allocator can't be assigned in the constructor body (e.g. std::pmr::polymorphic_allocator isn't assignable).
	basic_synchronized_event_dispatcher(basic_synchronized_event_dispatcher&& other, lock_type /*lock*/) :
		allocator(other.allocator),
		dispatchers(std::move(other.dispatchers)),
		dispatcher_list(std::move(other.dispatcher_list)) {
		other.dispatchers.clear();
	}

	basic_synchronized_event_dispatcher(basic_synchronized_event_dispatcher&& other, AllocatorT const& alloc, lock_type /*lock*/) :
		allocator(alloc),
		dispatchers(std::move(other.dispatchers), allocator),
		dispatcher_list(std::move(other.dispatcher_list)) {
		other.dispatchers.clear();  // With unequal allocators, the elements were moved individually
	}

	static auto close_all(dispatcher_list_pointer const& list) noexcept -> void {
		if (list) {
			for (auto const& dispatcher : *list) {
				dispatcher->close();
			}
		}
	}

	[[nodiscard]]
	auto acquire_list() const -> dispatcher_list_pointer {
		auto lock = std::shared_lock{dispatcher_mut};
		return dispatcher_list;
	}

	template<typename EventT>
	[[nodiscard]]
	auto find_dispatcher() const -> generic_dispatcher_pointer {
		auto lock = std::shared_lock{dispatcher_mut};

		if (auto const it = dispatchers.find(std::type_index{typeid(std::remove_cvref_t<EventT>)}); it != dispatchers.end()) {
			return it->second;
		}

		return nullptr;
	}

	// Returns a shared pointer, since another thread may assign to this object while the dispatcher is being used
	template<typename EventT>
	auto get_dispatcher() -> std::shared_ptr<derived_dispatcher<EventT>> {
		if (auto existing = find_dispatcher<EventT>()) {
			return std::static_pointer_cast<derived_dispatcher<EventT>>(std::move(existing));
		}

		// If the dispatcher didn't exist, then acquire an exclusive lock and create it. Another thread may have created
		// it in the meantime.
		auto lock = std::unique_lock{dispatcher_mut};

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

		return std::static_pointer_cast<derived_dispatcher<EventT>>(iter->second);
	}

	// Requires dispatcher_mut to be locked exclusively
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
	mutable std::shared_mutex dispatcher_mut;
};


/// Type alias for a basic_synchronized_event_dispatcher with the default template arguments
using synchronized_event_dispatcher = basic_synchronized_event_dispatcher<>;

}  //namespace events

// NOLINTEND(hicpp-noexcept-move,performance-noexcept-move-constructor)
