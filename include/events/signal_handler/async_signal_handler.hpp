#pragma once

#include <algorithm>
#include <cassert>
#include <concepts>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <plf_colony.h>

#include <boost/asio.hpp>
#include <boost/asio/experimental/parallel_group.hpp>

#include <events/connection.hpp>
#include <events/detail/parallel_publish.hpp>


// NOLINTBEGIN(cppcoreguidelines-prefer-member-initializer,hicpp-noexcept-move,performance-noexcept-move-constructor)

namespace events {


template<typename FunctionT, typename ExecutorT, typename AllocatorT>
class async_signal_handler;


/**
 * @brief An asynchronous variant of @ref signal_handler. Callback invocations will happen on an ASIO executor.
 *        Callbacks that don't finish before a new signal is published will still be invoked. An ASIO completion token
 *        may optionally be provided when publishing a signal, which will be invoked once all callbacks have completed.
 *        If the signal returns values, these will be passed to the completion token.
 */
template<typename ReturnT, typename... ArgsT, typename ExecutorT, typename AllocatorT>
class [[nodiscard]] async_signal_handler<ReturnT(ArgsT...), ExecutorT, AllocatorT> {
public:
	using allocator_type = AllocatorT;
	using executor_type = ExecutorT;
	using function_type = ReturnT(ArgsT...);
	using completion_type = std::conditional_t<std::is_same_v<void, ReturnT>, void(), void(std::vector<ReturnT>)>;

private:
	using alloc_traits = std::allocator_traits<AllocatorT>;

	using element_type = std::shared_ptr<std::function<function_type>>;
	using container_allocator_type = typename alloc_traits::template rebind_alloc<element_type>;
	using container_type = plf::colony<element_type, container_allocator_type>;

public:
	async_signal_handler(ExecutorT const& exec) : executor(exec) {
	}

	template<typename ExecutionContext>
	requires std::convertible_to<ExecutionContext&, boost::asio::execution_context&>
	async_signal_handler(ExecutionContext& context) :
		async_signal_handler(context.get_executor()) {
	}

	async_signal_handler(ExecutorT const& exec, AllocatorT const& alloc) :
		allocator(alloc),
		executor(exec) {
	}

	template<typename ExecutionContext>
	requires std::convertible_to<ExecutionContext&, boost::asio::execution_context&>
	async_signal_handler(ExecutionContext& context, AllocatorT const& alloc) :
		async_signal_handler(context.get_executor(), alloc) {
	}

	/// Construct a new async_signal_handler that holds the same callbacks as another
	async_signal_handler(async_signal_handler const& other) {
		auto lock = std::shared_lock{other.callback_mut};

		if constexpr (alloc_traits::propagate_on_container_copy_assignment::value) {
			allocator = other.allocator;
		}

		executor = other.executor;
		callbacks = other.callbacks;
	}

	/// Construct a new async_signal_handler that holds the same callbacks as another
	async_signal_handler(async_signal_handler const& other, AllocatorT const& alloc) :
		allocator(alloc) {
		auto lock = std::shared_lock{other.callback_mut};
		executor = other.executor;
		callbacks = other.callbacks;
	}

	/**
	 * @brief Construct a new async_signal_handler that will take ownership of another's callbacks
	 * @details Moving from a handler with running callbacks is undefined behavior.
	 */
	async_signal_handler(async_signal_handler&& other) {
		auto lock = std::scoped_lock{other.callback_mut};

		if constexpr (alloc_traits::propagate_on_container_move_assignment::value) {
			allocator = std::move(other.allocator);
		}

		executor = std::move(other.executor);
		callbacks = std::move(other.callbacks);
	}

	/**
	 * @brief Construct a new async_signal_handler that will take ownership of another's callbacks
	 * @details Moving from a handler with running callbacks is undefined behavior.
	 */
	async_signal_handler(async_signal_handler&& other, AllocatorT const& alloc) :
		allocator(alloc) {
		auto lock = std::scoped_lock{other.callback_mut};
		executor = std::move(other.executor);
		callbacks = container_type{std::move(other.callbacks), allocator};
	}

	~async_signal_handler() = default;

	/**
	 * @brief Copy the valid callbacks from an async_signal_handler to this one
	 * @details Assigning to an async_signal_handler that has running callbacks is undefined behavior
	 */
	auto operator=(async_signal_handler const& other) -> async_signal_handler& {
		if (&other == this) {
			return *this;
		}

		auto lock1 = std::scoped_lock{callback_mut};
		auto lock2 = std::shared_lock{other.callback_mut};

		if constexpr (alloc_traits::propagate_on_container_copy_assignment::value) {
			allocator = other.allocator;
		}

		executor = other.executor;
		callbacks = other.callbacks;

		return *this;
	}

	/**
	 * @brief Move the valid callbacks from an async_signal_handler to this one
	 *
	 * @details Assigning to an async_signal_handler that has running callbacks is undefined behavior, and moving from
	 *          one that has running callbacks is undefined behavior.
	 */
	auto operator=(async_signal_handler&& other) -> async_signal_handler& {
		if (&other == this) {
			return *this;
		}

		auto lock1 = std::scoped_lock{callback_mut};
		auto lock2 = std::scoped_lock{other.callback_mut};

		if constexpr (alloc_traits::propagate_on_container_move_assignment::value) {
			allocator = std::move(other.allocator);
		}

		executor = std::move(other.executor);
		callbacks = std::move(other.callbacks);

		return *this;
	}

	[[nodiscard]]
	constexpr auto get_allocator() const noexcept -> allocator_type {
		return allocator;
	}

	/// Get the executor associated with this object
	[[nodiscard]]
	auto get_executor() const -> executor_type {
		return executor;
	}

	/// Get the number of callbacks registered with this signal handler
	[[nodiscard]]
	auto size() const -> size_t {
		auto lock = std::scoped_lock{callback_mut};
		return callbacks.size();
	}

	/// Disconnect all callbacks
	auto disconnect_all() -> void {
		auto lock = std::scoped_lock{callback_mut};
		callbacks.clear();
	}

	/**
	 * @brief Register a callback function that will be invoked when the signal is fired
	 *
	 * @tparam FunctionT
	 *
	 * @param callback  A function that is compatible with the signal handler's function signature
	 *
	 * @return A connection handle that can be used to disconnect the function from this signal handler
	 */
	template<typename FunctionT>
	auto connect(FunctionT&& func) -> connection {
		auto callback_ptr = std::allocate_shared<std::function<function_type>>(allocator, std::forward<FunctionT>(func));

		auto lock = std::unique_lock{callback_mut};
		auto const it = callbacks.insert(callback_ptr);
		lock.unlock();

		return connection{[this, ptr = &(*it)] {
			this->disconnect(ptr);
		}};
	}

	/**
	 * @brief Fire the signal synchronously
	 *
	 * @param args The signal arguments
	 */
	auto publish(ArgsT... args) -> void requires std::same_as<void, ReturnT>
	{
		auto lock = std::shared_lock{callback_mut};

		for (auto& callback_ptr : callbacks) {
			(*callback_ptr)(args...);
		}
	}

	/**
	 * @brief Fire the signal synchronously
	 *
	 * @param args The signal arguments
	 *
	 * @return The callback results
	 */
	auto publish(ArgsT... args) -> std::vector<ReturnT> requires(!std::same_as<void, ReturnT>)
	{
		auto lock = std::shared_lock{callback_mut};

		auto results = std::vector<ReturnT>{};
		results.reserve(callbacks.size());

		for (auto& callback_ptr : callbacks) {
			results.emplace_back((*callback_ptr)(args...));
		}

		return results;
	}

	/**
	 * @brief Fire the signal asynchronously
	 *
	 * @param args The signal arguments
	 */
	auto async_publish(ArgsT... args) -> void {
		auto args_payload = std::allocate_shared<const std::tuple<ArgsT...>>(
			allocator,
			std::forward<ArgsT>(args)...
		);

		auto lock = std::shared_lock{callback_mut};

		for (auto& callback_ptr : callbacks) {
			boost::asio::post(executor, [callback_ptr, args_payload]() {
				(void)std::apply(*callback_ptr, *args_payload);
			});
		}
	}

	/**
	 * @brief Fire the signal asynchronously and invoke a completion token when finished
	 *
	 * @param args The signal arguments
	 * @param completion A completion token that will be called when the operation completes
	 */
	template<typename CompletionToken>
	auto async_publish(ArgsT... args, CompletionToken&& completion) {
		// Store the arguments in a tuple, which will be referenced by each async operation.
		auto args_payload = std::allocate_shared<std::tuple<ArgsT...> const>(
			allocator,
			std::forward<ArgsT>(args)...
		);

		// This lambda will post a task to the executor which will invoke the callback. The actual operation is
		// deferred to be later executed as part of a parallel_group.
		auto post_op = [this, &args_payload](typename container_type::reference callback_ptr) {
			auto execute = [callback_ptr, args_payload]() {
				if constexpr (std::same_as<void, ReturnT>) {
					std::apply(*callback_ptr, *args_payload);
					return boost::asio::deferred_t::values(std::monostate{});  //needs to return a value
				}
				else {
					auto result = std::apply(*callback_ptr, *args_payload);
					return boost::asio::deferred_t::values(std::move(result));
				}
			};

			return boost::asio::post(executor, boost::asio::deferred(std::move(execute)));
		};

		using post_op_type = decltype(post_op(std::declval<typename container_type::reference>()));
		auto operations = std::vector<post_op_type>{};

		auto lock = std::shared_lock{callback_mut};
		operations.reserve(callbacks.size());

		// Create a deferred callback invocation for each callback
		for (auto& ptr : callbacks) {
			operations.emplace_back(post_op(ptr));
		}

		// Initiate the callbacks as a parallel_group, with a completion that takes either nothing if ReturnT is void,
		// or a vector of the callback results otherwise.
		return detail::parallel_publish<completion_type>(
			executor,
			std::move(operations),
			std::forward<CompletionToken>(completion),
			allocator
		);
	}

private:
	auto disconnect(typename container_type::const_pointer pointer) -> void {
		auto lock = std::scoped_lock{callback_mut};
		auto const it = callbacks.get_iterator(pointer);
		if (it != callbacks.end() && *it == *pointer) {
			callbacks.erase(callbacks.get_iterator(pointer));
		}
	}

	AllocatorT allocator;

	ExecutorT executor;

	container_type callbacks{allocator};
	std::shared_mutex callback_mut;
};

}  //namespace events

// NOLINTEND(cppcoreguidelines-prefer-member-initializer,hicpp-noexcept-move,performance-noexcept-move-constructor)
