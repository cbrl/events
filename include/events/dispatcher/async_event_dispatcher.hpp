#pragma once

#include <algorithm>
#include <concepts>
#include <map>
#include <memory>
#include <shared_mutex>
#include <typeinfo>
#include <typeindex>
#include <type_traits>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/experimental/parallel_group.hpp>

#include <events/connection.hpp>
#include <events/signal_handler/async_signal_handler.hpp>


namespace events {
namespace detail {

template<typename EventT, typename CallbackPolicyT>
class async_discrete_event_dispatcher;


template<typename CallbackPolicyT>
class [[nodiscard]] async_discrete_event_dispatcher<void, CallbackPolicyT> {
public:
	async_discrete_event_dispatcher() = default;
	async_discrete_event_dispatcher(async_discrete_event_dispatcher const&) = delete;
	async_discrete_event_dispatcher(async_discrete_event_dispatcher&&) noexcept = default;

	virtual ~async_discrete_event_dispatcher() = default;

	auto operator=(async_discrete_event_dispatcher const&) -> async_discrete_event_dispatcher& = delete;
	auto operator=(async_discrete_event_dispatcher&&) noexcept -> async_discrete_event_dispatcher& = default;

	virtual auto dispatch() -> void = 0;
	virtual auto async_dispatch() -> void = 0;
	virtual auto async_dispatch(boost::asio::any_completion_handler<void()> handler) -> void = 0;
	virtual auto clear() -> void = 0;
	virtual auto size() -> size_t = 0;
};


template<typename EventT, typename CallbackPolicyT>
class [[nodiscard]] async_discrete_event_dispatcher final
    : public async_discrete_event_dispatcher<void, CallbackPolicyT> {
	using signal_handler_type = async_signal_handler<void(EventT const&), CallbackPolicyT>;

public:
	explicit async_discrete_event_dispatcher(boost::asio::any_io_executor const& exec) :
	    handler(signal_handler_type::create(exec)) {
	}

	async_discrete_event_dispatcher(async_discrete_event_dispatcher const&) = delete;
	async_discrete_event_dispatcher(async_discrete_event_dispatcher&&) noexcept = default;

	~async_discrete_event_dispatcher() override = default;

	auto operator=(async_discrete_event_dispatcher const&) -> async_discrete_event_dispatcher& = delete;
	auto operator=(async_discrete_event_dispatcher&&) noexcept -> async_discrete_event_dispatcher& = default;

	template<std::invocable<EventT const&> FunctionT>
	auto connect(FunctionT&& callback) -> connection {
		return handler->connect(std::forward<FunctionT>(callback));
	}

	auto dispatch() -> void override {
		auto lock = std::unique_lock{events_mut};
		auto to_publish = std::move(events);
		events.clear();
		lock.unlock();

		for (auto const& event : to_publish) {
			handler->publish(event);
		}
	}

	auto async_dispatch() -> void override {
		// Moving the vector and iterating over a local one allows events to be enqueued during iteration. Storing the
		// vector in a shared_ptr allows the vector to be kept alive until all of the callbacks have completed.
		auto lock = std::unique_lock{events_mut};
		auto to_publish = std::make_shared<std::vector<EventT>>(std::move(events));
		events.clear();
		lock.unlock();

		for (auto const& event : *to_publish) {
			handler->async_publish(event, boost::asio::consign(boost::asio::detached, to_publish));
		}
	}

	auto async_dispatch(boost::asio::any_completion_handler<void()> completion) -> void override {
		auto lock = std::unique_lock{events_mut};
		auto to_publish = std::make_shared<std::vector<EventT>>(std::move(events));
		events.clear();
		lock.unlock();

		return parallel_publish(
		    std::span{*to_publish},
		    [user_completion = std::move(completion), to_publish](auto&&...) mutable { std::move(user_completion)(); }
		);
	}

	auto send(EventT const& event) -> void {
		handler->publish(event);
	}

	template<std::ranges::range RangeT>
	requires std::convertible_to<std::ranges::range_value_t<RangeT>, EventT>
	auto send(RangeT const& range) -> void {
		for (auto&& event : range) {
			handler->publish(event);
		}
	}

	auto async_send(EventT const& event) -> void {
		handler->async_publish(event, boost::asio::detached);
	}

	template<boost::asio::completion_token_for<void()> CompletionToken>
	auto async_send(EventT const& event, CompletionToken&& completion) {
		return handler->async_publish(event, std::forward<CompletionToken>(completion));
	}

	template<std::ranges::range RangeT, boost::asio::completion_token_for<void()> CompletionToken>
	requires std::convertible_to<std::ranges::range_value_t<RangeT>, EventT>
	auto async_send(RangeT const& range, CompletionToken&& completion) {
		return parallel_publish(std::span{range}, std::forward<CompletionToken>(completion));
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
	// Publish a set of events using a parallel_group with a single completion that is invoked when all callbacks finish
	template<std::convertible_to<EventT> U, boost::asio::completion_token_for<void()> CompletionToken>
	auto parallel_publish(std::span<U> to_publish, CompletionToken&& completion) {
		using op_type = decltype(handler->async_publish(std::declval<EventT>(), boost::asio::deferred));

		auto operations = std::vector<op_type>{};
		operations.reserve(to_publish.size());

		for (auto&& event : to_publish) {
			operations.emplace_back(handler->async_publish(event, boost::asio::deferred));
		}

		auto default_exec = handler->get_executor();
		return detail::parallel_publish<void>(
		    default_exec, std::move(operations), std::forward<CompletionToken>(completion)
		);
	}


	std::shared_ptr<signal_handler_type> handler;

	std::vector<EventT> events;
	std::mutex events_mut;
};

}  //namespace detail


/**
 * @brief An @ref event_dispatcher that invokes callbacks asynchronously
 */
template<typename CallbackPolicyT = callback_policy::concurrent>
class [[nodiscard]] async_event_dispatcher {
	template<typename T>
	using dispatcher_type = detail::async_discrete_event_dispatcher<T, CallbackPolicyT>;

public:
	explicit async_event_dispatcher(boost::asio::any_io_executor exec) : executor(std::move(exec)) {
	}

	template<typename ExecutionContext>
	explicit async_event_dispatcher(ExecutionContext& exec) : executor(exec.get_executor()) {
	}

	async_event_dispatcher(async_event_dispatcher const&) = delete;
	async_event_dispatcher(async_event_dispatcher&&) noexcept = default;

	~async_event_dispatcher() = default;

	auto operator=(async_event_dispatcher const&) -> async_event_dispatcher& = delete;
	auto operator=(async_event_dispatcher&&) noexcept -> async_event_dispatcher& = default;

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
	 * @param range  The range of events to enqueue
	 */
	template<typename EventT, std::ranges::range RangeT>
	requires std::convertible_to<std::ranges::range_value_t<RangeT>, EventT>
	auto enqueue(RangeT&& range) -> void {
		get_or_create_dispatcher<EventT>().enqueue(std::forward<RangeT>(range));
	}

	/**
	 * @brief Synchronously send an event immediately
	 *
	 * @details The user is responsible for ensuring that the event's lifetime does not end before all callbacks have
	 *          finished executing.
	 *
	 * @tparam EventT  The type of event to send
	 *
	 * @param event  An instance of the event to send
	 */
	template<typename EventT>
	auto send(EventT const& event) -> void {
		get_or_create_dispatcher<EventT>().send(event);
	}

	/**
	 * @brief Synchronously send a range of events immediately
	 *
	 * @details The user is responsible for ensuring that the event's lifetime does not end before all callbacks have
	 *          finished executing.
	 *
	 * @tparam EventT  The type of event to send
	 * @tparam RangeT
	 *
	 * @param range  The range of events to send
	 */
	template<typename EventT, std::ranges::range RangeT>
	requires std::convertible_to<std::ranges::range_value_t<RangeT>, EventT>
	auto send(RangeT const& range) -> void {
		get_or_create_dispatcher<EventT>().send(range);
	}

	/**
	 * @brief Asynchronously send an event immediately
	 *
	 * @details The user is responsible for ensuring that the event's lifetime does not end before all callbacks have
	 *          finished executing.
	 *
	 * @tparam EventT  The type of event to send
	 * @tparam CompletionToken
	 *
	 * @param event  An instance of the event to send
	 * @param completion  The completion token that will be invoked when all callbacks have completed
	 */
	template<typename EventT, boost::asio::completion_token_for<void()> CompletionToken>
	auto async_send(EventT const& event, CompletionToken&& completion) {
		return boost::asio::async_initiate<CompletionToken, void()>(
		    [&](auto handler) mutable { get_or_create_dispatcher<EventT>().async_send(event, std::move(handler)); },
		    completion
		);
	}

	/**
	 * @brief Asynchronously send a range of events immediately
	 *
	 * @details The user is responsible for ensuring that the event lifetimes do not end before all callbacks have
	 *          finished executing.
	 *
	 * @tparam EventT  The type of event to send
	 * @tparam RangeT
	 * @tparam CompletionToken
	 *
	 * @param range  The range of events to send
	 * @param completion  The completion token that will be invoked when all callbacks have completed
	 */
	template<typename EventT, std::ranges::range RangeT, typename CompletionToken>
	requires std::convertible_to<std::ranges::range_value_t<RangeT>, EventT>
	    && boost::asio::completion_token_for<CompletionToken, void()>
	auto async_send(RangeT const& range, CompletionToken&& completion) -> void {
		return boost::asio::async_initiate<CompletionToken, void()>(
		    [&](auto handler) mutable { get_or_create_dispatcher<EventT>().async_send(range, std::move(handler)); },
		    completion
		);
	}

	/// Dispatch all events in the queue synchronously
	auto dispatch() -> void {
		auto lock = std::shared_lock{dispatcher_mut};
		for (auto& [type, dispatcher] : dispatchers) {
			dispatcher->dispatch();
		}
	}

	/// Dispatch all events in the queue asynchronously
	auto async_dispatch() -> void {
		auto lock = std::shared_lock{dispatcher_mut};
		for (auto& [type, dispatcher] : dispatchers) {
			dispatcher->async_dispatch();
		}
	}

	/**
	 * @brief Dispatch all events in the queue asynchronously
	 *
	 * @details This overload of async_dispatch taking a completion token defers all callback invocations so they can
	 *          be launched as a parallel_group. This will necessarily require allocating memory upfront to store all
	 *          the data required to invoke a callback, requiring O(events * callbacks) memory for each type of event.
	 *          This can be quite large depending on the number of events enqueued, reaching 10s-100s of MB on the
	 *          order of hundreds of thousands of events.
	 *
	 * @tparam CompletionToken
	 *
	 * @param completion  The completion token that will be invoked when all callbacks have completed
	 */
	template<boost::asio::completion_token_for<void()> CompletionToken>
	auto async_dispatch(CompletionToken&& completion) {
		auto lock = std::shared_lock{dispatcher_mut};

		auto initiate = [](dispatcher_type<void>& dispatcher) {
			return boost::asio::async_initiate<decltype(boost::asio::deferred), void()>(
			    [&dispatcher](auto handler) mutable { dispatcher.async_dispatch(std::move(handler)); },
			    boost::asio::deferred
			);
		};

		using op_type = decltype(initiate(*(dispatchers.begin()->second)));
		auto operations = std::vector<op_type>{};
		operations.reserve(dispatchers.size());

		for (auto& [type, dispatcher] : dispatchers) {
			operations.emplace_back(initiate(*dispatcher));
		}

		return detail::parallel_publish<void>(
		    executor, std::move(operations), std::forward<CompletionToken>(completion)
		);
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
	auto get_or_create_dispatcher() -> dispatcher_type<EventT>& {
		auto const key = std::type_index{typeid(EventT)};

		// Attempt to find an existing dispatcher
		{
			auto lock = std::shared_lock{dispatcher_mut};

			if (auto it = dispatchers.find(key); it != dispatchers.end()) {
				return static_cast<dispatcher_type<EventT>&>(*(it->second));
			}
		}

		// If the dispatcher didn't exist, then acquire an exclusive lock and create it.
		auto lock = std::unique_lock{dispatcher_mut};

		auto const [iter, inserted] = dispatchers.try_emplace(key);

		// Check if it actually was created since two threads could get to the point where they try
		// to acquire an exclusive lock.
		if (inserted) {
			iter->second = std::make_unique<dispatcher_type<EventT>>(executor);
		}

		return static_cast<dispatcher_type<EventT>&>(*(iter->second));
	}

	boost::asio::any_io_executor executor;

	std::map<std::type_index, std::unique_ptr<dispatcher_type<void>>> dispatchers;
	std::shared_mutex dispatcher_mut;
};

}  //namespace events
