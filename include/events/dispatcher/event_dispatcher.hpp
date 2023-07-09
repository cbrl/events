#pragma once

#include <concepts>
#include <map>
#include <memory>
#include <typeinfo>
#include <typeindex>
#include <vector>

#include <events/connection.hpp>
#include <events/signal_handler/signal_handler.hpp>


namespace events {
namespace detail {

template<typename EventT = void>
class discrete_event_dispatcher;


template<>
class [[nodiscard]] discrete_event_dispatcher<void> {
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


template<typename EventT>
class [[nodiscard]] discrete_event_dispatcher final : public discrete_event_dispatcher<void> {
public:
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
	signal_handler<void(EventT const&)> handler;
	std::vector<EventT> events;
};

}  //namespace detail


/**
 * @brief Stores callback functions that will be invoked when an event is published. Events may be
 *        immediately dispatched or enqueued for future bulk dispatch.
 */
class [[nodiscard]] event_dispatcher {
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
	 * @brief Get the number of enqueued events for a specific event type
	 *
	 * @tparam EventT  The type of event to get the count of
	 *
	 * @return The number of events of this type that are in the queue
	 */
	template<typename EventT>
	[[nodiscard]]
	auto queue_size() const -> size_t {
		auto const key = std::type_index{typeid(EventT)};

		if (auto it = dispatchers.find(key); it != dispatchers.end()) {
			return it->second->size();
		}

		return 0;
	}

private:
	template<typename EventT>
	auto get_or_create_dispatcher() -> detail::discrete_event_dispatcher<EventT>& {
		auto const [iter, inserted] = dispatchers.try_emplace(std::type_index{typeid(EventT)});

		if (inserted) {
			iter->second = std::make_unique<detail::discrete_event_dispatcher<EventT>>();
		}

		return static_cast<detail::discrete_event_dispatcher<EventT>&>(*(iter->second));
	}

	std::map<std::type_index, std::unique_ptr<detail::discrete_event_dispatcher<>>> dispatchers;
};

}  //namespace events
