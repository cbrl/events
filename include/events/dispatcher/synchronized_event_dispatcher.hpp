#pragma once

#include <concepts>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <typeinfo>
#include <typeindex>
#include <vector>

#include <events/connection.hpp>
#include <events/signal_handler/synchronized_signal_handler.hpp>


namespace events {
namespace detail {

template<typename EventT = void>
class synchronized_discrete_event_dispatcher;


template<>
class [[nodiscard]] synchronized_discrete_event_dispatcher<void> {
public:
	virtual ~synchronized_discrete_event_dispatcher() = default;
	virtual auto dispatch() -> void = 0;
	virtual auto clear() -> void = 0;
	virtual auto size() -> size_t = 0;
};


template<typename EventT>
class [[nodiscard]] synchronized_discrete_event_dispatcher final : public synchronized_discrete_event_dispatcher<void> {
public:
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
		return events.size();
	}

private:
	synchronized_signal_handler<void(EventT const&)> handler;

	std::vector<EventT> events;
	std::mutex events_mut;
};

} //namespace detail


/**
 * @brief A thread-safe @ref event_dispatcher 
 */
class [[nodiscard]] synchronized_event_dispatcher {
public:
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
		get_or_create_dispatcher<EventT>().enqueue(std::forward<EventT>(event));
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
		get_or_create_dispatcher<EventT>().send(std::forward<EventT>(event));
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
		auto lock = std::shared_lock{dispatcher_mut};
		for (auto& [type, dispatcher] : dispatchers) {
			dispatcher->dispatch();
		}
	}

	/**
	 * @brief Get the number of enqueued events for a specific event type
	 *
	 * @tparam EventT  The type of event to get the count of
	 *
	 * @return The number of events of this type that are in the queue
	 */
	template<typename EventT>
	auto queue_size() const -> size_t {
		auto const key = std::type_index{typeid(EventT)};

		if (auto it = dispatchers.find(key); it != dispatchers.end()) {
			return it->second->size();
		}

		return 0;
	}

private:
	template<typename EventT>
	auto get_or_create_dispatcher() -> detail::synchronized_discrete_event_dispatcher<EventT>& {
		auto const key = std::type_index{typeid(EventT)};

		// Attempt to find an existing dispatcher
		{
			auto lock = std::shared_lock{dispatcher_mut};

			if (auto it = dispatchers.find(key); it != dispatchers.end()) {
				return static_cast<detail::synchronized_discrete_event_dispatcher<EventT>&>(*(it->second));
			}

		}

		// If the dispatcher didn't exist, then acquire an exclusive lock and create it.
		auto lock = std::unique_lock{dispatcher_mut};

		auto const [iter, inserted] = dispatchers.try_emplace(key);

		// Check if it actually was created since two threads could get to the point where they try
		// to acquire an exclusive lock.
		if (inserted) {
			iter->second = std::make_unique<detail::synchronized_discrete_event_dispatcher<EventT>>();
		}

		return static_cast<detail::synchronized_discrete_event_dispatcher<EventT>&>(*(iter->second));
	}

	std::map<std::type_index, std::unique_ptr<detail::synchronized_discrete_event_dispatcher<>>> dispatchers;
	std::shared_mutex dispatcher_mut;
};

} //namespace events
